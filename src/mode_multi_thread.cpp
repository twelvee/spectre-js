#include "mode_adapter.h"
#include "mode_helpers.h"

#include <algorithm>
#include <memory>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spectre::detail {
    class MultiThreadAdapter final : public ModeAdapter {
    public:
        explicit MultiThreadAdapter(const RuntimeConfig &config)
            : m_Config(config),
              m_WorkerCount(static_cast<std::uint32_t>(std::max(1u, std::thread::hardware_concurrency()))),
              m_DispatchSeed(HashString(config.enableGpuAcceleration ? "gpu" : "cpu")) {
            m_Config.mode = RuntimeMode::MultiThread;
        }

        StatusCode CreateContext(const ContextConfig &config, SpectreContext **outContext) override {
            auto it = m_Contexts.find(config.name);
            if (it != m_Contexts.end()) {
                if (outContext != nullptr) {
                    *outContext = &it->second->context;
                }
                return StatusCode::AlreadyExists;
            }
            auto state = std::make_unique<ContextState>(config);
            auto *ptr = &state->context;
            state->fiberVersion = ++m_DispatchSeed;
            TouchReady(config.name);
            m_Contexts.emplace(config.name, std::move(state));
            if (outContext != nullptr) {
                *outContext = ptr;
            }
            return StatusCode::Ok;
        }

        StatusCode DestroyContext(const std::string &name) override {
            auto it = m_Contexts.find(name);
            if (it == m_Contexts.end()) {
                return StatusCode::NotFound;
            }
            m_Contexts.erase(it);
            auto pos = std::find(m_Ready.begin(), m_Ready.end(), name);
            if (pos != m_Ready.end()) {
                m_Ready.erase(pos);
            }
            return StatusCode::Ok;
        }

        EvaluationResult LoadScript(const std::string &contextName, const ScriptSource &script) override {
            EvaluationResult result{StatusCode::Ok, script.name, {}};
            auto *context = FindContext(contextName);
            if (context == nullptr) {
                result.status = StatusCode::NotFound;
                result.value.clear();
                result.diagnostics = "Context not found";
                return result;
            }
            ScriptRecord record;
            record.source = script.source;
            record.bytecode = BuildBaseline(script.source);
            record.bytecodeHash = HashBytes(record.bytecode);
            record.isBytecode = false;
            result.status = context->context.StoreScript(script.name, std::move(record));
            if (result.status == StatusCode::Ok) {
                context->fiberVersion++;
                TouchReady(contextName);
                result.diagnostics = "Script cached";
            } else {
                result.value.clear();
                result.diagnostics = "Store failed";
            }
            return result;
        }

        EvaluationResult LoadBytecode(const std::string &contextName, const BytecodeArtifact &artifact) override {
            EvaluationResult result{StatusCode::Ok, artifact.name, {}};
            auto *context = FindContext(contextName);
            if (context == nullptr) {
                result.status = StatusCode::NotFound;
                result.value.clear();
                result.diagnostics = "Context not found";
                return result;
            }
            ScriptRecord record;
            record.source.clear();
            record.bytecode = artifact.data;
            if (record.bytecode.empty()) {
                record.bytecode = BuildBaseline(artifact.name);
            }
            record.bytecodeHash = HashBytes(record.bytecode);
            record.isBytecode = true;
            result.status = context->context.StoreScript(artifact.name, std::move(record));
            if (result.status == StatusCode::Ok) {
                context->fiberVersion++;
                TouchReady(contextName);
                result.diagnostics = "Bytecode cached";
            } else {
                result.value.clear();
                result.diagnostics = "Store failed";
            }
            return result;
        }

        EvaluationResult EvaluateSync(const std::string &contextName, const std::string &entryPoint) override {
            EvaluationResult result{StatusCode::Ok, {}, {}};
            auto *context = FindContext(contextName);
            if (context == nullptr) {
                result.status = StatusCode::NotFound;
                result.diagnostics = "Context not found";
                return result;
            }
            const ScriptRecord *record = nullptr;
            auto status = context->context.GetScript(entryPoint, &record);
            if (status != StatusCode::Ok || record == nullptr) {
                result.status = StatusCode::NotFound;
                result.diagnostics = "Script not found";
                return result;
            }
            context->fiberVersion++;
            m_DispatchSeed += context->fiberVersion + static_cast<std::uint64_t>(m_WorkerCount);
            if (record->isBytecode) {
                result.value = record->bytecodeHash;
                result.diagnostics = "Bytecode hash";
                return result;
            }
            std::string literal;
            if (TryInterpretLiteral(record->source, literal)) {
                result.value = std::move(literal);
                result.diagnostics = "Literal";
            } else {
                result.value = record->bytecodeHash;
                result.diagnostics = "Baseline hash";
            }
            return result;
        }

        void Tick(const TickInfo &info) override {
            m_LastTick = info;
            m_DispatchSeed ^= static_cast<std::uint64_t>(info.frameIndex + m_WorkerCount);
        }

        StatusCode Reconfigure(const RuntimeConfig &config) override {
            if (config.mode != RuntimeMode::MultiThread && config.mode != m_Config.mode) {
                return StatusCode::InvalidArgument;
            }
            m_Config = config;
            m_Config.mode = RuntimeMode::MultiThread;
            m_WorkerCount = static_cast<std::uint32_t>(std::max(1u, std::thread::hardware_concurrency()));
            return StatusCode::Ok;
        }

        const RuntimeConfig &Config() const override {
            return m_Config;
        }

        StatusCode GetContext(const std::string &name, const SpectreContext **outContext) const override {
            auto *context = FindContextConst(name);
            if (context == nullptr) {
                return StatusCode::NotFound;
            }
            if (outContext != nullptr) {
                *outContext = &context->context;
            }
            return StatusCode::Ok;
        }

    private:
        struct ContextState {
            explicit ContextState(const ContextConfig &config) : context(config.name, config.initialStackSize) {}
            SpectreContext context;
            std::uint64_t fiberVersion{0};
        };

        ContextState *FindContext(const std::string &name) {
            auto it = m_Contexts.find(name);
            if (it == m_Contexts.end()) {
                return nullptr;
            }
            return it->second.get();
        }

        const ContextState *FindContextConst(const std::string &name) const {
            auto it = m_Contexts.find(name);
            if (it == m_Contexts.end()) {
                return nullptr;
            }
            return it->second.get();
        }

        void TouchReady(const std::string &name) {
            auto it = std::find(m_Ready.begin(), m_Ready.end(), name);
            if (it == m_Ready.end()) {
                m_Ready.push_back(name);
            } else if (it + 1 != m_Ready.end()) {
                std::rotate(it, it + 1, m_Ready.end());
            }
        }

        RuntimeConfig m_Config;
        std::unordered_map<std::string, std::unique_ptr<ContextState>> m_Contexts;
        std::vector<std::string> m_Ready;
        TickInfo m_LastTick{0.0, 0};
        std::uint64_t m_DispatchSeed{0};
        std::uint32_t m_WorkerCount;
    };

    std::unique_ptr<ModeAdapter> CreateMultiThreadAdapter(const RuntimeConfig &config) {
        return std::make_unique<MultiThreadAdapter>(config);
    }
}
