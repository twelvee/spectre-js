#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "spectre/config.h"
#include "spectre/runtime.h"
#include "spectre/status.h"
#include "spectre/es2025/environment.h"
#include "spectre/es2025/modules/global_module.h"

namespace {
    std::string LoadBootstrapScript() {
        const char *candidates[] = {
            "examples/scripts/global_bootstrap.js",
            "../examples/scripts/global_bootstrap.js",
            "../../examples/scripts/global_bootstrap.js"
        };
        for (const auto *path: candidates) {
            std::ifstream stream(path);
            if (stream) {
                std::ostringstream buffer;
                buffer << stream.rdbuf();
                return buffer.str();
            }
        }
        return "return 'spectre-js::global-bootstrap-fallback';";
    }

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

    void EvaluateSampleScripts(spectre::es2025::GlobalModule &globalModule) {
        std::cout << "\nExecuting global scripts" << std::endl;
        std::string value;
        std::string diagnostics;

        auto fileScript = LoadBootstrapScript();
        auto status = globalModule.EvaluateScript(fileScript, value, diagnostics, "bootstrap-from-file");
        if (status != spectre::StatusCode::Ok) {
            std::cerr << "  bootstrap file failed: " << diagnostics << std::endl;
        } else {
            std::cout << "  bootstrap file => " << value << " (" << diagnostics << ")" << std::endl;
        }

        const char *inlineScript = "return 'spectre-es2025-inline';";
        status = globalModule.EvaluateScript(inlineScript, value, diagnostics, "inline");
        if (status != spectre::StatusCode::Ok) {
            std::cerr << "  inline failed: " << diagnostics << std::endl;
            return;
        }
        std::cout << "  inline => " << value << " (" << diagnostics << ")" << std::endl;

        const char *numeric = "return (12 + 8) / 5;";
        status = globalModule.EvaluateScript(numeric, value, diagnostics, "numeric");
        if (status != spectre::StatusCode::Ok) {
            std::cerr << "  numeric failed: " << diagnostics << std::endl;
            return;
        }
        std::cout << "  numeric => " << value << " (" << diagnostics << ")" << std::endl;
    }

    void DriveHostTicks(spectre::SpectreRuntime &runtime, std::uint32_t frames) {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            spectre::TickInfo tick{0.016, frame};
            runtime.Tick(tick);
        }
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

    auto *globalModulePtr = environment.FindModule("Global");
    auto *globalModule = dynamic_cast<es2025::GlobalModule *>(globalModulePtr);
    if (!globalModule) {
        std::cerr << "Global module unavailable" << std::endl;
        return 1;
    }

    EvaluateSampleScripts(*globalModule);

    std::cout << "\nSimulating production tick loop" << std::endl;
    DriveHostTicks(*runtime, 3);

    auto gpuConfig = runtime->Config();
    gpuConfig.enableGpuAcceleration = true;
    if (runtime->Reconfigure(gpuConfig) != StatusCode::Ok) {
        std::cerr << "Failed to enable GPU acceleration" << std::endl;
        return 1;
    }
    environment.OptimizeGpu(true);
    std::cout << "GPU acceleration flag propagated" << std::endl;

    DriveHostTicks(*runtime, 2);

    std::string value;
    std::string diagnostics;
    auto status = globalModule->EvaluateScript("return 'final-pass';", value, diagnostics, "final");
    if (status != StatusCode::Ok) {
        std::cerr << "Final evaluation failed: " << diagnostics << std::endl;
        return 1;
    }
    std::cout << "Final script => " << value << " (" << diagnostics << ")" << std::endl;

    std::cout << "\nES2025 production scaffold ready" << std::endl;
    return 0;
}
