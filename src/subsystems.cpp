#include "spectre/subsystems.h"

#include <array>
#include <memory>
#include <unordered_map>

namespace spectre::detail {
    namespace {
        std::uint64_t HashString(const std::string &value) {
            std::uint64_t hash = 1469598103934665603ULL;
            for (unsigned char ch : value) {
                hash ^= ch;
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        std::uint64_t HashBytes(const std::vector<std::uint8_t> &bytes) {
            std::uint64_t hash = 1469598103934665603ULL;
            for (auto b : bytes) {
                hash ^= static_cast<std::uint64_t>(b);
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        std::string ToHex(std::uint64_t value) {
            std::array<char, 17> buffer{};
            for (int i = 0; i < 16; ++i) {
                auto shift = static_cast<unsigned>((15 - i) * 4);
                auto digit = static_cast<unsigned>((value >> shift) & 0xFULL);
                buffer[i] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
            }
            buffer[16] = '\0';
            return std::string(buffer.data());
        }

        class StubParser final : public ParserFrontend {
        public:
            StatusCode ParseModule(const ScriptUnit &unit, ModuleArtifact &outArtifact) override {
                outArtifact.name = unit.name;
                outArtifact.fingerprint = ToHex(HashString(unit.source));
                outArtifact.payload.clear();
                outArtifact.payload.reserve(unit.source.size());
                std::uint64_t rolling = HashString(unit.name);
                for (unsigned char ch : unit.source) {
                    rolling ^= ch;
                    rolling *= 1099511628211ULL;
                    outArtifact.payload.push_back(static_cast<std::uint8_t>(rolling & 0xFFULL));
                }
                return StatusCode::Ok;
            }
        };

        class StubBytecode final : public BytecodePipeline {
        public:
            StatusCode LowerModule(const ModuleArtifact &artifact, ExecutableProgram &outProgram) override {
                outProgram.name = artifact.name;
                outProgram.code = artifact.payload;
                outProgram.version = ++m_Version;
                if (outProgram.code.empty()) {
                    auto value = HashString(artifact.fingerprint);
                    outProgram.code.reserve(8);
                    for (int i = 0; i < 8; ++i) {
                        outProgram.code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFULL));
                    }
                }
                return StatusCode::Ok;
            }
        private:
            std::uint64_t m_Version{0};
        };

        class StubExecution final : public ExecutionEngine {
        public:
            ExecutionResponse Execute(const ExecutionRequest &request) override {
                ExecutionResponse response{};
                if (request.program == nullptr) {
                    response.status = StatusCode::InvalidArgument;
                    response.diagnostics = "missing program";
                    return response;
                }
                auto hash = HashBytes(request.program->code);
                hash ^= HashString(request.contextName);
                hash *= 1099511628211ULL;
                hash ^= HashString(request.entryPoint);
                hash *= 1099511628211ULL;
                response.status = StatusCode::Ok;
                response.value = ToHex(hash);
                response.diagnostics = "stub.execution";
                return response;
            }
        };

        class StubGc final : public GarbageCollector {
        public:
            StatusCode Collect(GcSnapshot &snapshot) override {
                ++m_Generation;
                snapshot.generation = m_Generation;
                snapshot.reclaimedBytes = m_Generation * 256;
                return StatusCode::Ok;
            }
        private:
            std::uint64_t m_Generation{0};
        };

        class StubMemory final : public MemorySystem {
        public:
            explicit StubMemory(MemoryBudget budget) : m_Budget(budget) {}

            StatusCode ApplyPlan(const MemoryBudgetPlan &plan) override {
                m_Budget = plan.target;
                m_LastWaste = plan.arenaWaste;
                return StatusCode::Ok;
            }

            const MemoryBudget &Budget() const {
                return m_Budget;
            }

            std::uint64_t LastWaste() const {
                return m_LastWaste;
            }
        private:
            MemoryBudget m_Budget;
            std::uint64_t m_LastWaste{0};
        };

        class StubTelemetry final : public TelemetryHub {
        public:
            explicit StubTelemetry(std::size_t capacity) {
                m_Buffer.reserve(capacity);
            }

            void PushSample(const TelemetrySample &sample) override {
                m_Buffer.push_back(sample);
            }

            std::vector<TelemetrySample> Drain() override {
                auto data = m_Buffer;
                m_Buffer.clear();
                return data;
            }
        private:
            std::vector<TelemetrySample> m_Buffer;
        };

        class StubScheduler final : public Scheduler {
        public:
            StatusCode PlanFrame(const SchedulerFramePlan &plan) override {
                if (plan.cpuBudget <= 0.0 || plan.gpuBudget < 0.0) {
                    return StatusCode::InvalidArgument;
                }
                m_LastPlan = plan;
                return StatusCode::Ok;
            }

            const SchedulerFramePlan &LastPlan() const {
                return m_LastPlan;
            }
        private:
            SchedulerFramePlan m_LastPlan{0, 0.0, 0.0};
        };

        class StubInterop final : public InteropBridge {
        public:
            StatusCode Register(const InteropBinding &binding) override {
                auto it = m_Bindings.find(binding.symbol);
                if (it != m_Bindings.end()) {
                    return StatusCode::AlreadyExists;
                }
                m_Bindings[binding.symbol] = binding.handle;
                return StatusCode::Ok;
            }

            std::size_t BindingCount() const {
                return m_Bindings.size();
            }
        private:
            std::unordered_map<std::string, void *> m_Bindings;
        };
    }

    SubsystemSuite CreateStubSubsystemSuite(const RuntimeConfig &config) {
        SubsystemSuite suite;
        suite.parser = std::make_unique<StubParser>();
        suite.bytecode = std::make_unique<StubBytecode>();
        suite.execution = std::make_unique<StubExecution>();
        suite.gc = std::make_unique<StubGc>();
        suite.memory = std::make_unique<StubMemory>(config.memory);
        suite.telemetry = std::make_unique<StubTelemetry>(config.telemetry.historySize);
        suite.scheduler = std::make_unique<StubScheduler>();
        suite.interop = std::make_unique<StubInterop>();
        suite.manifest.parserBackend = "stub.parser.v0";
        suite.manifest.bytecodeBackend = "stub.bytecode.v0";
        suite.manifest.executionBackend = "stub.execution.v0";
        suite.manifest.gcBackend = "stub.gc.v0";
        suite.manifest.memoryBackend = "stub.memory.v0";
        suite.manifest.telemetryBackend = "stub.telemetry.v0";
        suite.manifest.schedulerBackend = "stub.scheduler.v0";
        suite.manifest.interopBackend = "stub.interop.v0";
        return suite;
    }
}


