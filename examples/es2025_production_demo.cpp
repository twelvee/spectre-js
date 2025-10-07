#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

#include "spectre/config.h"
#include "spectre/runtime.h"
#include "spectre/status.h"
#include "spectre/es2025/environment.h"
#include "spectre/es2025/modules/global_module.h"
#include "spectre/es2025/modules/error_module.h"
#include "spectre/es2025/modules/function_module.h"
#include "spectre/es2025/modules/array_module.h"

namespace {
    spectre::StatusCode DemoSumCallback(const std::vector<std::string> &args, std::string &outResult, void *) {
        long total = 0;
        for (const auto &value: args) {
            try {
                total += std::stol(value);
            } catch (...) {
                outResult.clear();
                return spectre::StatusCode::InvalidArgument;
            }
        }
        outResult = std::to_string(total);
        return spectre::StatusCode::Ok;
    }

    spectre::StatusCode DemoUpperCallback(const std::vector<std::string> &args, std::string &outResult, void *) {
        if (args.empty()) {
            outResult.clear();
            return spectre::StatusCode::InvalidArgument;
        }
        outResult = args.front();
        std::transform(outResult.begin(), outResult.end(), outResult.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return spectre::StatusCode::Ok;
    }

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

    void DemonstrateArrayModule(spectre::es2025::ArrayModule &arrayModule) {
        using Value = spectre::es2025::ArrayModule::Value;
        spectre::es2025::ArrayModule::Handle metricsHandle = 0;
        if (arrayModule.CreateDense("demo.metrics", 8, metricsHandle) != spectre::StatusCode::Ok) {
            std::cout << "Array module demo unavailable" << std::endl;
            return;
        }
        arrayModule.PushNumber(metricsHandle, 18.0);
        arrayModule.PushNumber(metricsHandle, 7.0);
        arrayModule.PushNumber(metricsHandle, 32.0);
        arrayModule.PushNumber(metricsHandle, 12.0);
        arrayModule.SortNumeric(metricsHandle, true);
        std::vector<Value> metricValues;
        arrayModule.Slice(metricsHandle, 0, arrayModule.Length(metricsHandle), metricValues);
        std::cout << "\nArray metrics" << std::endl;
        for (std::size_t i = 0; i < metricValues.size(); ++i) {
            std::cout << "  metrics[" << i << "] = " << metricValues[i].ToString() << std::endl;
        }
        std::size_t foundIndex = 0;
        if (arrayModule.BinarySearch(metricsHandle, Value(12.0), true, foundIndex) == spectre::StatusCode::Ok) {
            std::cout << "  search 12 => index " << foundIndex << std::endl;
        }

        spectre::es2025::ArrayModule::Handle traceHandle = 0;
        if (arrayModule.CreateDense("demo.trace", 2, traceHandle) != spectre::StatusCode::Ok) {
            return;
        }
        arrayModule.Set(traceHandle, 0, Value("frame-0"));
        arrayModule.Set(traceHandle, 9, Value("frame-9"));
        arrayModule.PushString(traceHandle, "frame-10");
        arrayModule.SortLexicographic(traceHandle, true);
        std::vector<Value> traceValues;
        arrayModule.Slice(traceHandle, 0, arrayModule.Length(traceHandle), traceValues);
        std::cout << "Trace labels" << std::endl;
        for (std::size_t i = 0; i < traceValues.size(); ++i) {
            std::cout << "  trace[" << i << "] = " << traceValues[i].ToString() << std::endl;
        }
        const auto &metrics = arrayModule.GetMetrics();
        std::cout << "Array metrics summary: dense=" << metrics.denseCount
                  << " sparse=" << metrics.sparseCount
                  << " d2s=" << metrics.transitionsToSparse
                  << " s2d=" << metrics.transitionsToDense << std::endl;
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

    auto *errorModulePtr = environment.FindModule("Error");
    auto *errorModule = dynamic_cast<es2025::ErrorModule *>(errorModulePtr);
    if (!errorModule) {
        std::cerr << "Error module unavailable" << std::endl;
        return 1;
    }

    auto *functionModulePtr = environment.FindModule("Function");
    auto *functionModule = dynamic_cast<es2025::FunctionModule *>(functionModulePtr);
    if (!functionModule) {
        std::cerr << "Function module unavailable" << std::endl;
        return 1;
    }

    auto *arrayModulePtr = environment.FindModule("Array");
    auto *arrayModule = dynamic_cast<es2025::ArrayModule *>(arrayModulePtr);
    if (!arrayModule) {
        std::cerr << "Array module unavailable" << std::endl;
        return 1;
    }

    functionModule->RegisterHostFunction("demo.sum", DemoSumCallback);
    functionModule->RegisterHostFunction("demo.upper", DemoUpperCallback);

    std::string hostResult;
    std::string hostDiagnostics;
    auto hostStatus = functionModule->InvokeHostFunction("demo.sum", std::vector<std::string>{"10", "20", "5"},
                                                         hostResult, hostDiagnostics);
    if (hostStatus == StatusCode::Ok) {
        std::cout << "Host demo.sum => " << hostResult << std::endl;
    }
    hostStatus = functionModule->InvokeHostFunction("demo.upper", std::vector<std::string>{"spectre"}, hostResult,
                                                    hostDiagnostics);
    if (hostStatus == StatusCode::Ok) {
        std::cout << "Host demo.upper => " << hostResult << std::endl;
    }

    EvaluateSampleScripts(*globalModule);

    DemonstrateArrayModule(*arrayModule);

    std::cout << "\nDemonstrating error capture" << std::endl;
    std::string failingValue;
    std::string failingDiagnostics;
    auto errorStatus = globalModule->EvaluateScript("let a = 1;", failingValue, failingDiagnostics, "invalid-script");
    if (errorStatus != StatusCode::Ok) {
        std::string formatted;
        errorModule->RaiseError("SyntaxError",
                                failingDiagnostics.empty() ? "Script parsing failed" : failingDiagnostics,
                                globalModule->DefaultContext(), "invalid-script", failingDiagnostics, formatted, nullptr);
        std::cout << "  captured => " << formatted << std::endl;
    }

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

