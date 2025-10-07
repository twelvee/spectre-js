
#include "mode_adapter.h"
#include "mode_helpers.h"

#include "spectre/subsystems.h"

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spectre::detail {
    namespace {
        constexpr std::uint32_t kProgramFormatVersion = 1;

        struct ProgramHeader {
            char magic[4];
            std::uint32_t formatVersion;
            std::uint64_t programVersion;
            std::uint32_t codeSize;
            std::uint32_t numberCount;
            std::uint32_t stringCount;
        };

        std::vector<std::uint8_t> SerializeProgram(const ExecutableProgram &program) {
            ProgramHeader header{};
            header.magic[0] = 'S';
            header.magic[1] = 'J';
            header.magic[2] = 'S';
            header.magic[3] = 'B';
            header.formatVersion = kProgramFormatVersion;
            header.programVersion = program.version;
            header.codeSize = static_cast<std::uint32_t>(program.code.size());
            header.numberCount = static_cast<std::uint32_t>(program.numberConstants.size());
            header.stringCount = static_cast<std::uint32_t>(program.stringConstants.size());

            std::vector<std::uint8_t> data;
            data.reserve(sizeof(header) + header.codeSize + header.numberCount * sizeof(double));
            const auto *headerBytes = reinterpret_cast<const std::uint8_t *>(&header);
            data.insert(data.end(), headerBytes, headerBytes + sizeof(header));
            data.insert(data.end(), program.code.begin(), program.code.end());

            for (double value: program.numberConstants) {
                std::uint64_t bits{};
                static_assert(sizeof(bits) == sizeof(value));
                std::memcpy(&bits, &value, sizeof(value));
                for (int i = 0; i < 8; ++i) {
                    data.push_back(static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFFULL));
                }
            }

            for (const auto &str: program.stringConstants) {
                auto length = static_cast<std::uint32_t>(str.size());
                for (int i = 0; i < 4; ++i) {
                    data.push_back(static_cast<std::uint8_t>((length >> (i * 8)) & 0xFFU));
                }
                data.insert(data.end(), str.begin(), str.end());
            }

            return data;
        }

        StatusCode DeserializeProgram(const std::vector<std::uint8_t> &data, ExecutableProgram &outProgram,
                                      std::string &diagnostics) {
            if (data.size() < sizeof(ProgramHeader)) {
                diagnostics = "Bytecode payload too small";
                return StatusCode::InvalidArgument;
            }
            ProgramHeader header{};
            std::memcpy(&header, data.data(), sizeof(header));
            if (!(header.magic[0] == 'S' && header.magic[1] == 'J' && header.magic[2] == 'S' && header.magic[3] ==
                  'B')) {
                diagnostics = "Invalid bytecode signature";
                return StatusCode::InvalidArgument;
            }
            if (header.formatVersion != kProgramFormatVersion) {
                diagnostics = "Unsupported bytecode format";
                return StatusCode::InvalidArgument;
            }

            std::size_t offset = sizeof(header);
            if (offset + header.codeSize > data.size()) {
                diagnostics = "Corrupted bytecode payload";
                return StatusCode::InvalidArgument;
            }
            outProgram.code.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                   data.begin() + static_cast<std::ptrdiff_t>(offset + header.codeSize));
            offset += header.codeSize;

            outProgram.numberConstants.clear();
            outProgram.numberConstants.reserve(header.numberCount);
            for (std::uint32_t i = 0; i < header.numberCount; ++i) {
                if (offset + sizeof(std::uint64_t) > data.size()) {
                    diagnostics = "Corrupted numeric section";
                    return StatusCode::InvalidArgument;
                }
                std::uint64_t bits = 0;
                for (int b = 0; b < 8; ++b) {
                    bits |= static_cast<std::uint64_t>(data[offset + b]) << (b * 8);
                }
                double value;
                std::memcpy(&value, &bits, sizeof(value));
                outProgram.numberConstants.push_back(value);
                offset += 8;
            }

            outProgram.stringConstants.clear();
            outProgram.stringConstants.reserve(header.stringCount);
            for (std::uint32_t i = 0; i < header.stringCount; ++i) {
                if (offset + 4 > data.size()) {
                    diagnostics = "Corrupted string section";
                    return StatusCode::InvalidArgument;
                }
                std::uint32_t length = 0;
                for (int b = 0; b < 4; ++b) {
                    length |= static_cast<std::uint32_t>(data[offset + b]) << (b * 8);
                }
                offset += 4;
                if (offset + length > data.size()) {
                    diagnostics = "Corrupted string payload";
                    return StatusCode::InvalidArgument;
                }
                outProgram.stringConstants.emplace_back(reinterpret_cast<const char *>(data.data() + offset), length);
                offset += length;
            }

            outProgram.version = header.programVersion;
            outProgram.diagnostics.clear();
            return StatusCode::Ok;
        }
    }

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

            ModuleArtifact artifact{};
            ScriptUnit unit{script.name, script.source};
            auto parseStatus = m_Parser->ParseModule(unit, artifact);
            if (parseStatus != StatusCode::Ok) {
                result.status = parseStatus;
                result.value.clear();
                result.diagnostics = artifact.diagnostics.empty() ? "Parser error" : artifact.diagnostics;
                return result;
            }

            ExecutableProgram program{};
            auto lowerStatus = m_Bytecode->LowerModule(artifact, program);
            if (lowerStatus != StatusCode::Ok) {
                result.status = lowerStatus;
                result.value.clear();
                result.diagnostics = program.diagnostics.empty() ? "Bytecode generation failed" : program.diagnostics;
                return result;
            }
            program.name = script.name;

            auto serialized = SerializeProgram(program);
            ScriptRecord record;
            record.source = script.source;
            record.bytecode = serialized;
            record.bytecodeHash = HashBytes(record.bytecode);
            record.isBytecode = false;

            auto status = state->context.StoreScript(script.name, std::move(record));
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
            std::string diagnostics;
            auto status = DeserializeProgram(artifact.data, program, diagnostics);
            if (status != StatusCode::Ok) {
                result.status = status;
                result.value.clear();
                result.diagnostics = diagnostics;
                return result;
            }
            program.name = artifact.name;

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
