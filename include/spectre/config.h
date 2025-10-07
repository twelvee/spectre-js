#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace spectre {
    enum class RuntimeMode {
        SingleThread,
        MultiThread
    };

    struct MemoryBudget {
        std::uint64_t heapBytes;
        std::uint64_t arenaBytes;
        std::uint64_t gpuBytes;
    };

    struct TelemetryConfig {
        bool enableProfiling;
        bool enableTracing;
        std::uint32_t historySize;
    };

    struct RuntimeConfig {
        RuntimeMode mode;
        MemoryBudget memory;
        TelemetryConfig telemetry;
        bool enableGpuAcceleration;
        std::vector<std::string> featureFlags;
    };

    RuntimeConfig MakeDefaultConfig();
}


