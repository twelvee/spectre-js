#include "spectre/config.h"

namespace spectre {

RuntimeConfig MakeDefaultConfig() {
    RuntimeConfig config{};
    config.mode = RuntimeMode::SingleThread;
    config.memory = MemoryBudget{256 * 1024 * 1024ULL, 128 * 1024 * 1024ULL, 512 * 1024 * 1024ULL};
    config.telemetry = TelemetryConfig{true, false, 256};
    return config;
}

}
