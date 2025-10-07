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
#include "spectre/es2025/modules/atomics_module.h"
#include "spectre/es2025/modules/boolean_module.h"
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

    void DemonstrateAtomicsModule(spectre::es2025::AtomicsModule &atomicsModule) {
        std::cout << "\nAtomics lane demo" << std::endl;
        spectre::es2025::AtomicsModule::Handle buffer = 0;
        if (atomicsModule.CreateBuffer("demo.lanes", 6, buffer) != spectre::StatusCode::Ok) {
            std::cout << "Atomics module demo unavailable" << std::endl;
            return;
        }
        auto cleanup = [&]() {
            if (buffer != 0) {
                atomicsModule.DestroyBuffer(buffer);
                buffer = 0;
            }
        };
        if (atomicsModule.Fill(buffer, 0) != spectre::StatusCode::Ok) {
            std::cout << "  fill failed" << std::endl;
            cleanup();
            return;
        }
        std::int64_t previous = 0;
        if (atomicsModule.Add(buffer, 0, 256, previous) != spectre::StatusCode::Ok) {
            std::cout << "  add failed" << std::endl;
            cleanup();
            return;
        }
        if (atomicsModule.Store(buffer, 1, 7) != spectre::StatusCode::Ok) {
            std::cout << "  store failed" << std::endl;
            cleanup();
            return;
        }
        atomicsModule.CompareExchange(buffer, 1, 0, 99, previous);
        atomicsModule.CompareExchange(buffer, 1, previous, 99, previous);
        atomicsModule.Add(buffer, 1, 3, previous);
        atomicsModule.Xor(buffer, 2, 0x5a, previous);
        atomicsModule.Or(buffer, 3, 0x0f, previous);
        std::int64_t lane = 0;
        if (atomicsModule.Load(buffer, 1, lane) == spectre::StatusCode::Ok) {
            std::cout << "  lane[1] => " << lane << std::endl;
        }
        std::vector<std::int64_t> snapshot;
        if (atomicsModule.Snapshot(buffer, snapshot) == spectre::StatusCode::Ok) {
            for (std::size_t i = 0; i < snapshot.size(); ++i) {
                std::cout << "  lane[" << i << "] = " << snapshot[i] << std::endl;
            }
        }
        const auto &metrics = atomicsModule.Metrics();
        auto activeBuffers = metrics.allocations >= metrics.deallocations
                               ? metrics.allocations - metrics.deallocations
                               : 0;
        std::cout << "Atomics metrics: loads=" << metrics.loadOps
                  << " stores=" << metrics.storeOps
                  << " rmw=" << metrics.rmwOps
                  << " buffers=" << activeBuffers
                  << " hot=" << metrics.hotBuffers << std::endl;
        cleanup();
    }

    void DemonstrateBooleanModule(spectre::es2025::BooleanModule &booleanModule) {
        std::cout << "\nBoolean cache demo" << std::endl;
        auto originalFlags = std::cout.flags();
        std::cout << std::boolalpha;
        auto trueHandle = booleanModule.Box(true);
        auto falseHandle = booleanModule.Box(false);
        bool value = false;
        if (booleanModule.ValueOf(trueHandle, value) == spectre::StatusCode::Ok) {
            std::cout << "  canonical true => " << value << std::endl;
        }
        if (booleanModule.ValueOf(falseHandle, value) == spectre::StatusCode::Ok) {
            std::cout << "  canonical false => " << value << std::endl;
        }
        std::cout << "  \"  yes  \" => " << booleanModule.ToBoolean("  yes  ") << std::endl;
        std::cout << "  0.0 => " << booleanModule.ToBoolean(0.0) << std::endl;
        spectre::es2025::BooleanModule::Handle flag = 0;
        if (booleanModule.Create("demo.flag", true, flag) == spectre::StatusCode::Ok) {
            booleanModule.Toggle(flag);
            if (booleanModule.ValueOf(flag, value) == spectre::StatusCode::Ok) {
                std::cout << "  flag toggled => " << value << std::endl;
            }
            booleanModule.Set(flag, false);
            if (booleanModule.ValueOf(flag, value) == spectre::StatusCode::Ok) {
                std::cout << "  flag reset => " << value << std::endl;
            }
            booleanModule.Destroy(flag);
        }
        const auto &metrics = booleanModule.Metrics();
        std::cout << "Boolean metrics: conversions=" << metrics.conversions
                  << " allocations=" << metrics.allocations
                  << " hot=" << metrics.hotBoxes << std::endl;
        std::cout.flags(originalFlags);
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

    auto *atomicsModulePtr = environment.FindModule("Atomics");
    auto *atomicsModule = dynamic_cast<es2025::AtomicsModule *>(atomicsModulePtr);
    if (!atomicsModule) {
        std::cerr << "Atomics module unavailable" << std::endl;
        return 1;
    }

    auto *booleanModulePtr = environment.FindModule("Boolean");
    auto *booleanModule = dynamic_cast<es2025::BooleanModule *>(booleanModulePtr);
    if (!booleanModule) {
        std::cerr << "Boolean module unavailable" << std::endl;
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
    DemonstrateAtomicsModule(*atomicsModule);
    DemonstrateBooleanModule(*booleanModule);

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

