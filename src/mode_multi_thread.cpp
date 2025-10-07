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
            auto suite = CreateCpuSubsystemSuite(m_Config);
            m_Parser = std::move(suite.parser);
            m_Bytecode = std::move(suite.bytecode);
            m_Execution = std::move(suite.execution);
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
            auto *state = FindContext(contextName);
            if (state == nullptr) {
                result.status = StatusCode::NotFound;
                result.value.clear();
                result.diagnostics = "Context not found";
                return result;
            }
            if (!m_Parser || !m_Bytecode) {
                result.status = StatusCode::InternalError;
                result.value.clear();
                result.diagnostics = "Subsystems unavailable";
                return result;
            }

            ExecutableProgram program{};
            program.name = script.name;
            std::string diagnostics;
            auto status = CompileScript(*m_Parser, *m_Bytecode, script, program, diagnostics);
            if (status != StatusCode::Ok) {
                result.status = status;
                result.value.clear();
                result.diagnostics = diagnostics.empty() ? "Compilation failed" : diagnostics;
                return result;
            }

            auto serialized = SerializeProgram(program);
            ScriptRecord record;
            record.source = script.source;
            record.bytecode = std::move(serialized);
            record.bytecodeHash = HashBytes(record.bytecode);
            record.isBytecode = false;

            status = state->context.StoreScript(script.name, std::move(record));
            if (status != StatusCode::Ok) {
                result.status = status;
                result.value.clear();
                result.diagnostics = "Failed to store script";
                return result;
            }

            state->programs[script.name] = std::move(program);
            state->fiberVersion++;
            TouchReady(contextName);
            result.diagnostics = "Script compiled";
            return result;
        }

        EvaluationResult LoadBytecode(const std::string &contextName, const BytecodeArtifact &artifact) override {
            EvaluationResult result{StatusCode::Ok, artifact.name, {}};
            auto *state = FindContext(contextName);
            if (state == nullptr) {
                result.status = StatusCode::NotFound;
                result.value.clear();
                result.diagnostics = "Context not found";
                return result;
            }
            ExecutableProgram program{};
            program.name = artifact.name;
            std::string diagnostics;
            auto status = DeserializeProgram(artifact.data, program, diagnostics);
            if (status != StatusCode::Ok) {
                result.status = status;
                result.value.clear();
                result.diagnostics = diagnostics;
                return result;
            }

            ScriptRecord record;
            record.source.clear();
            record.bytecode = artifact.data;
            record.bytecodeHash = HashBytes(record.bytecode);
            record.isBytecode = true;

            status = state->context.StoreScript(artifact.name, std::move(record));
            if (status != StatusCode::Ok) {
                result.status = status;
                result.value.clear();
                result.diagnostics = "Failed to store bytecode";
                return result;
            }

            state->programs[artifact.name] = std::move(program);
            state->fiberVersion++;
            TouchReady(contextName);
            result.diagnostics = "Bytecode loaded";
            return result;
        }

        EvaluationResult EvaluateSync(const std::string &contextName, const std::string &entryPoint) override {
            EvaluationResult result{StatusCode::Ok, {}, {}};
            auto *state = FindContext(contextName);
            if (state == nullptr) {
                result.status = StatusCode::NotFound;
                result.diagnostics = "Context not found";
                return result;
            }
            auto programIt = state->programs.find(entryPoint);
            if (programIt == state->programs.end()) {
                result.status = StatusCode::NotFound;
                result.diagnostics = "Program not compiled";
                return result;
            }
            state->fiberVersion++;
            m_DispatchSeed += state->fiberVersion + static_cast<std::uint64_t>(m_WorkerCount);

            ExecutionRequest request{contextName, entryPoint, &programIt->second};
            auto response = m_Execution
                                ? m_Execution->Execute(request)
                                : ExecutionResponse{StatusCode::InternalError, {}, "Execution backend missing"};
            result.status = response.status;
            result.value = std::move(response.value);
            result.diagnostics = std::move(response.diagnostics);
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
            explicit ContextState(const ContextConfig &config) : context(config.name, config.initialStackSize) {
            }

            SpectreContext context;
            std::unordered_map<std::string, ExecutableProgram> programs;
            std::uint64_t fiberVersion{0};
        };

        ContextState *FindContext(const std::string &name) {
            auto it = m_Contexts.find(name);
            return it == m_Contexts.end() ? nullptr : it->second.get();
        }

        const ContextState *FindContextConst(const std::string &name) const {
            auto it = m_Contexts.find(name);
            return it == m_Contexts.end() ? nullptr : it->second.get();
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
        std::unique_ptr<ParserFrontend> m_Parser;
        std::unique_ptr<BytecodePipeline> m_Bytecode;
        std::unique_ptr<ExecutionEngine> m_Execution;
        std::unordered_map<std::string, std::unique_ptr<ContextState> > m_Contexts;
        std::vector<std::string> m_Ready;
        TickInfo m_LastTick{0.0, 0};
        std::uint64_t m_DispatchSeed{0};
        std::uint32_t m_WorkerCount;
    };

    std::unique_ptr<ModeAdapter> CreateMultiThreadAdapter(const RuntimeConfig &config) {
        return std::make_unique<MultiThreadAdapter>(config);
    }
}