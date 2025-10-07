#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"

namespace spectre::detail {
    struct ScriptUnit {
        std::string name;
        std::string source;
    };

    struct ModuleArtifact {
        std::string name;
        std::string fingerprint;
        std::vector<std::uint8_t> payload;
    };

    struct ExecutableProgram {
        std::string name;
        std::vector<std::uint8_t> code;
        std::uint64_t version;
    };

    struct ExecutionRequest {
        std::string contextName;
        std::string entryPoint;
        const ExecutableProgram *program;
    };

    struct ExecutionResponse {
        StatusCode status;
        std::string value;
        std::string diagnostics;
    };

    struct GcSnapshot {
        std::uint64_t generation;
        std::uint64_t reclaimedBytes;
    };

    struct MemoryBudgetPlan {
        MemoryBudget target;
        std::uint64_t arenaWaste;
    };

    struct TelemetrySample {
        std::string channel;
        double value;
        std::uint64_t frameIndex;
    };

    struct SchedulerFramePlan {
        std::uint64_t frameIndex;
        double cpuBudget;
        double gpuBudget;
    };

    struct InteropBinding {
        std::string symbol;
        void *handle;
    };

    class ParserFrontend {
    public:
        virtual ~ParserFrontend() = default;
        virtual StatusCode ParseModule(const ScriptUnit &unit, ModuleArtifact &outArtifact) = 0;
    };

    class BytecodePipeline {
    public:
        virtual ~BytecodePipeline() = default;
        virtual StatusCode LowerModule(const ModuleArtifact &artifact, ExecutableProgram &outProgram) = 0;
    };

    class ExecutionEngine {
    public:
        virtual ~ExecutionEngine() = default;
        virtual ExecutionResponse Execute(const ExecutionRequest &request) = 0;
    };

    class GarbageCollector {
    public:
        virtual ~GarbageCollector() = default;
        virtual StatusCode Collect(GcSnapshot &snapshot) = 0;
    };

    class MemorySystem {
    public:
        virtual ~MemorySystem() = default;
        virtual StatusCode ApplyPlan(const MemoryBudgetPlan &plan) = 0;
    };

    class TelemetryHub {
    public:
        virtual ~TelemetryHub() = default;
        virtual void PushSample(const TelemetrySample &sample) = 0;
        virtual std::vector<TelemetrySample> Drain() = 0;
    };

    class Scheduler {
    public:
        virtual ~Scheduler() = default;
        virtual StatusCode PlanFrame(const SchedulerFramePlan &plan) = 0;
    };

    class InteropBridge {
    public:
        virtual ~InteropBridge() = default;
        virtual StatusCode Register(const InteropBinding &binding) = 0;
    };

    struct SubsystemManifest {
        std::string parserBackend;
        std::string bytecodeBackend;
        std::string executionBackend;
        std::string gcBackend;
        std::string memoryBackend;
        std::string telemetryBackend;
        std::string schedulerBackend;
        std::string interopBackend;
    };

    struct SubsystemSuite {
        std::unique_ptr<ParserFrontend> parser;
        std::unique_ptr<BytecodePipeline> bytecode;
        std::unique_ptr<ExecutionEngine> execution;
        std::unique_ptr<GarbageCollector> gc;
        std::unique_ptr<MemorySystem> memory;
        std::unique_ptr<TelemetryHub> telemetry;
        std::unique_ptr<Scheduler> scheduler;
        std::unique_ptr<InteropBridge> interop;
        SubsystemManifest manifest;
    };

    SubsystemSuite CreateStubSubsystemSuite(const RuntimeConfig &config);
}
