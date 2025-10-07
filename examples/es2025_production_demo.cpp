#include <iomanip>
#include <iostream>
#include <string>

#include "spectre/config.h"
#include "spectre/runtime.h"
#include "spectre/status.h"
#include "spectre/es2025/environment.h"

namespace {
    void PrintModuleCatalog(const spectre::es2025::Environment &environment) {
        const auto &modules = environment.Modules();
        std::cout << "ES2025 module catalog (" << modules.size() << ")" << std::endl;
        for (const auto &modulePtr: modules) {
            if (!modulePtr) {
                continue;
            }
            std::cout << "  - " << std::setw(24) << std::left << modulePtr->Name()
                    << " : " << modulePtr->Summary() << std::endl;
            std::cout << "      spec: " << modulePtr->SpecificationReference() << std::endl;
        }
    }

    bool DriveHostTicks(spectre::SpectreRuntime &runtime, std::uint32_t frames) {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            spectre::TickInfo tick{0.016, frame};
            runtime.Tick(tick);
        }
        return true;
    }
}

int main() {
    using namespace spectre;

    auto config = MakeDefaultConfig();
    config.enableGpuAcceleration = false;

    auto runtime = SpectreRuntime::Create(config);
    if (!runtime) {
        std::cerr << "Failed to create Spectre runtime" << std::endl;
        return 1;
    }

    auto &environment = runtime->EsEnvironment();

    PrintModuleCatalog(environment);

    // Simulate production tick loop (3 frames here for brevity).
    if (!DriveHostTicks(*runtime, 3)) {
        std::cerr << "Tick loop failed" << std::endl;
        return 1;
    }

    // Toggle GPU acceleration to ensure OptimizeGpu hooks are exercised.
    auto gpuConfig = runtime->Config();
    gpuConfig.enableGpuAcceleration = true;
    if (runtime->Reconfigure(gpuConfig) != StatusCode::Ok) {
        std::cerr << "Failed to enable GPU acceleration" << std::endl;
        return 1;
    }
    environment.OptimizeGpu(true);

    // Perform a final tick pass with GPU acceleration enabled.
    if (!DriveHostTicks(*runtime, 2)) {
        std::cerr << "Tick loop failed under GPU configuration" << std::endl;
        return 1;
    }

    // Query a few individual modules through the index to mimic production lookups.
    constexpr const char *kSmokeTests[] = {
        "Global",
        "Array",
        "Promise",
        "Intl",
        "Temporal"
    };

    for (const auto *name: kSmokeTests) {
        const auto *module = environment.FindModule(name);
        if (!module) {
            std::cerr << "Missing ES2025 module: " << name << std::endl;
            return 1;
        }
        std::cout << "Verified module: " << module->Name() << std::endl;
    }

    std::cout << "ES2025 production scaffold ready" << std::endl;
    return 0;
}
