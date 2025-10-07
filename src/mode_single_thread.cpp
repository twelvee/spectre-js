#include "mode_adapter.h"
#include "mode_helpers.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace spectre::detail {
    class SingleThreadAdapter final : public ModeAdapter {
    public:
        explicit SingleThreadAdapter(const RuntimeConfig &config) : m_Config(config) {
            m_Config.mode = RuntimeMode::SingleThread;
            auto suite = CreateCpuSubsystemSuite(m_Config);
            m_Parser = std::move(suite.parser);
            m_Bytecode = std::move(suite.bytecode);
            m_Execution = std::move(suite.execution);
        }

        StatusCode CreateContext(const ContextConfig &config, SpectreContext **outContext) override {
            auto it = m_Contexts.find(config.name);
            if (it != m_Contexts.end()) {
                if (outContext != nullptr) {
                    *outContext = &it->second.context;
                }
                return StatusCode::AlreadyExists;
            }
            ContextState state(config);
            auto insert = m_Contexts.emplace(config.name, std::move(state));
            if (outContext != nullptr) {
                *outContext = &insert.first->second.context;
            }
            return StatusCode::Ok;
        }

        StatusCode DestroyContext(const std::string &name) override {
            return m_Contexts.erase(name) > 0 ? StatusCode::Ok : StatusCode::NotFound;
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
        }

        StatusCode Reconfigure(const RuntimeConfig &config) override {
            if (config.mode != RuntimeMode::SingleThread && config.mode != m_Config.mode) {
                return StatusCode::InvalidArgument;
            }
            m_Config = config;
            m_Config.mode = RuntimeMode::SingleThread;
            return StatusCode::Ok;
        }

        const RuntimeConfig &Config() const override {
            return m_Config;
        }

        StatusCode GetContext(const std::string &name, const SpectreContext **outContext) const override {
            auto it = m_Contexts.find(name);
            if (it == m_Contexts.end()) {
                return StatusCode::NotFound;
            }
            if (outContext != nullptr) {
                *outContext = &it->second.context;
            }
            return StatusCode::Ok;
        }

    private:
        struct ContextState {
            explicit ContextState(const ContextConfig &config) : context(config.name, config.initialStackSize) {
            }

            SpectreContext context;
            std::unordered_map<std::string, ExecutableProgram> programs;
        };

        ContextState *FindContext(const std::string &name) {
            auto it = m_Contexts.find(name);
            return it == m_Contexts.end() ? nullptr : &it->second;
        }

        const ContextState *FindContext(const std::string &name) const {
            auto it = m_Contexts.find(name);
            return it == m_Contexts.end() ? nullptr : &it->second;
        }

        RuntimeConfig m_Config;
        std::unique_ptr<ParserFrontend> m_Parser;
        std::unique_ptr<BytecodePipeline> m_Bytecode;
        std::unique_ptr<ExecutionEngine> m_Execution;
        std::unordered_map<std::string, ContextState> m_Contexts;
        TickInfo m_LastTick{0.0, 0};
    };

    std::unique_ptr<ModeAdapter> CreateSingleThreadAdapter(const RuntimeConfig &config) {
        return std::make_unique<SingleThreadAdapter>(config);
    }
}