#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <array>
#include <cstdint>

#include "spectre/config.h"
#include "spectre/runtime.h"
#include "spectre/status.h"
#include "spectre/es2025/environment.h"
#include "spectre/es2025/modules/global_module.h"
#include "spectre/es2025/modules/object_module.h"
#include "spectre/es2025/modules/proxy_module.h"
#include "spectre/es2025/modules/error_module.h"
#include "spectre/es2025/modules/function_module.h"
#include "spectre/es2025/modules/atomics_module.h"
#include "spectre/es2025/modules/boolean_module.h"
#include "spectre/es2025/modules/array_module.h"
#include "spectre/es2025/modules/array_buffer_module.h"
#include "spectre/es2025/modules/iterator_module.h"
#include "spectre/es2025/modules/generator_module.h"
#include "spectre/es2025/modules/string_module.h"
#include "spectre/es2025/modules/symbol_module.h"
#include "spectre/es2025/modules/math_module.h"
#include "spectre/es2025/modules/number_module.h"
#include "spectre/es2025/modules/bigint_module.h"
#include "spectre/es2025/modules/date_module.h"
#include "spectre/es2025/modules/map_module.h"
#include "spectre/es2025/modules/weak_map_module.h"
#include "spectre/es2025/value.h"

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

    void DemonstrateArrayBufferModule(spectre::es2025::ArrayBufferModule &bufferModule) {
        std::cout << "\nArrayBuffer streaming demo" << std::endl;
        spectre::es2025::ArrayBufferModule::Handle streamHandle = 0;
        spectre::es2025::ArrayBufferModule::Handle stagingHandle = 0;
        auto release = [&](spectre::es2025::ArrayBufferModule::Handle &handle) {
            if (handle != 0) {
                bufferModule.Destroy(handle);
                handle = 0;
            }
        };

        if (bufferModule.Create("demo.stream", 1024, streamHandle) != spectre::StatusCode::Ok) {
            std::cout << "  stream allocation failed" << std::endl;
            return;
        }
        if (bufferModule.Create("demo.stage", 1024, stagingHandle) != spectre::StatusCode::Ok) {
            std::cout << "  staging allocation failed" << std::endl;
            release(streamHandle);
            return;
        }

        std::array<std::uint8_t, 128> header{};
        for (std::size_t i = 0; i < header.size(); ++i) {
            header[i] = static_cast<std::uint8_t>(0x20 + (i & 0x0F));
        }
        if (bufferModule.CopyIn(streamHandle, 0, header.data(), header.size()) != spectre::StatusCode::Ok) {
            std::cout << "  header upload failed" << std::endl;
            release(stagingHandle);
            release(streamHandle);
            return;
        }

        std::array<std::uint8_t, 512> payload{};
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::uint8_t>((i * 11) & 0xFF);
        }
        if (bufferModule.CopyIn(streamHandle, header.size(), payload.data(), payload.size()) != spectre::StatusCode::Ok) {
            std::cout << "  payload upload failed" << std::endl;
            release(stagingHandle);
            release(streamHandle);
            return;
        }

        const auto totalBytes = header.size() + payload.size();
        if (bufferModule.CopyToBuffer(streamHandle, stagingHandle, 0, 0, totalBytes) != spectre::StatusCode::Ok) {
            std::cout << "  staging copy failed" << std::endl;
            release(stagingHandle);
            release(streamHandle);
            return;
        }

        std::vector<std::uint8_t> preview(24, 0);
        if (bufferModule.CopyOut(stagingHandle, 0, preview.data(), preview.size()) == spectre::StatusCode::Ok) {
            std::cout << "  staging preview:";
            auto flags = std::cout.flags();
            auto fill = std::cout.fill('0');
            std::cout << std::hex;
            for (auto byte: preview) {
                std::cout << ' ' << std::setw(2) << static_cast<int>(byte);
            }
            std::cout.flags(flags);
            std::cout.fill(fill);
            std::cout << std::endl;
        }

        if (bufferModule.Resize(streamHandle, 4096, true) == spectre::StatusCode::Ok) {
            bufferModule.Fill(streamHandle, 0);
            bufferModule.CopyToBuffer(stagingHandle, streamHandle, 0, 0, totalBytes);
        }

        bufferModule.Detach(stagingHandle);
        release(stagingHandle);
        release(streamHandle);

        const auto &metrics = bufferModule.GetMetrics();
        std::cout << "  ArrayBuffer metrics => allocations=" << metrics.allocations
                << " pool-reuses=" << metrics.poolReuses
                << " detaches=" << metrics.detaches
                << " peak-bytes=" << metrics.peakBytesInUse << std::endl;
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
        std::cout << " YES => " << booleanModule.ToBoolean("  yes  ") << std::endl;
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

void DemonstrateStringModule(spectre::es2025::StringModule &stringModule) {
    std::cout << "\nDemonstrating string module" << std::endl;
    spectre::es2025::StringModule::Handle title = 0;
    if (stringModule.Create("demo.title", "  spectre-js  ", title) != spectre::StatusCode::Ok) {
        std::cout << "  unable to allocate base string" << std::endl;
        return;
    }

    stringModule.TrimAscii(title);
    stringModule.ToUpperAscii(title);
    stringModule.Append(title, " ENGINE");

    auto titleView = stringModule.View(title);
    std::cout << "  title => " << titleView << std::endl;

    spectre::es2025::StringModule::Handle slice = 0;
    if (stringModule.Slice(title, 11, 6, "demo.slice", slice) == spectre::StatusCode::Ok) {
        std::cout << "  slice => " << stringModule.View(slice) << std::endl;
    }

    spectre::es2025::StringModule::Handle intern = 0;
    if (stringModule.Intern("spectre.hero", intern) == spectre::StatusCode::Ok) {
        std::cout << "  intern => " << stringModule.View(intern) << std::endl;
    }

    const auto &metrics = stringModule.GetMetrics();
    std::cout << "  metrics: allocations=" << metrics.allocations
            << " transforms=" << metrics.transforms
            << " internHits=" << metrics.internHits
            << " internMisses=" << metrics.internMisses << std::endl;

    if (slice != 0) {
        stringModule.Release(slice);
    }
    if (intern != 0) {
        stringModule.Release(intern);
    }
    stringModule.Release(title);
}

void ShowcaseSymbols(spectre::es2025::SymbolModule &symbolModule) {
    std::cout << "\nSymbol registry demo" << std::endl;
    auto iteratorHandle = symbolModule.WellKnownHandle(spectre::es2025::SymbolModule::WellKnown::Iterator);
    auto toStringTagHandle = symbolModule.WellKnownHandle(spectre::es2025::SymbolModule::WellKnown::ToStringTag);
    std::cout << "  iterator => " << symbolModule.Description(iteratorHandle) << std::endl;
    std::cout << "  toStringTag => " << symbolModule.Description(toStringTagHandle) << std::endl;

    spectre::es2025::SymbolModule::Handle localHandle = 0;
    if (symbolModule.Create("demo.local", localHandle) == spectre::StatusCode::Ok) {
        auto localDesc = symbolModule.Description(localHandle);
        std::cout << "  local => " << localDesc << " global=" << (symbolModule.IsGlobal(localHandle) ? "true" : "false") << std::endl;
    } else {
        std::cout << "  local symbol creation failed" << std::endl;
    }

    spectre::es2025::SymbolModule::Handle uniqueHandle = 0;
    if (symbolModule.CreateUnique(uniqueHandle) == spectre::StatusCode::Ok) {
        auto uniqueDesc = symbolModule.Description(uniqueHandle);
        std::cout << "  unique => handle=" << uniqueHandle << " description=";
        if (uniqueDesc.empty()) {
            std::cout << "<empty>";
        } else {
            std::cout << uniqueDesc;
        }
        std::cout << std::endl;
    }

    std::string registryKey;
    spectre::es2025::SymbolModule::Handle globalHandle = 0;
    if (symbolModule.CreateGlobal("demo.registry.key", globalHandle) == spectre::StatusCode::Ok) {
        if (symbolModule.KeyFor(globalHandle, registryKey) == spectre::StatusCode::Ok) {
            std::cout << "  global => " << registryKey << " handle=" << globalHandle << std::endl;
        }
        spectre::es2025::SymbolModule::Handle reuseHandle = 0;
        if (symbolModule.CreateGlobal("demo.registry.key", reuseHandle) == spectre::StatusCode::Ok) {
            std::cout << "  global reuse stable=" << (reuseHandle == globalHandle ? "true" : "false") << std::endl;
        }
    } else {
        std::cout << "  global symbol creation failed" << std::endl;
    }

    const auto &metrics = symbolModule.GetMetrics();
    std::cout << "  metrics => live=" << metrics.liveSymbols
            << " local=" << metrics.localSymbols
            << " global=" << metrics.globalSymbols
            << " hits=" << metrics.registryHits
            << " misses=" << metrics.registryMisses << std::endl;
}
void DemonstrateMathModule(spectre::es2025::MathModule &mathModule) {
    std::cout << "\nDemonstrating math module" << std::endl;
    auto originalFlags = std::cout.flags();
    auto originalPrecision = std::cout.precision();

    constexpr double angle = 0.78539816339;
    double sinFast = mathModule.FastSin(angle);
    double cosFast = mathModule.FastCos(angle);
    double tanFast = mathModule.FastTan(angle);
    float invSqrt = mathModule.FastInverseSqrt(256.0f);
    double reduced = mathModule.ReduceAngle(10.0);
    double lerpValue = mathModule.Lerp(-20.0, 20.0, 0.35);

    double lhs3[3] = {1.0, 2.0, 3.0};
    double rhs3[3] = {4.0, 5.0, 6.0};
    double dot3 = mathModule.Dot3(lhs3, rhs3);

    double fmaA[4] = {1.0, 2.0, 3.0, 4.0};
    double fmaB[4] = {0.5, -0.25, 0.75, -1.0};
    double fmaC[4] = {0.0, 0.5, -0.5, 1.0};
    double fmaOut[4] = {0.0, 0.0, 0.0, 0.0};
    mathModule.BatchedFma(fmaA, fmaB, fmaC, fmaOut, 4);

    std::cout << std::fixed << std::setprecision(5);
    std::cout << "  angle=" << angle
            << " sin=" << sinFast
            << " cos=" << cosFast
            << " tan=" << tanFast << std::endl;
    std::cout << "  invsqrt(256)=" << invSqrt
            << " reduced=" << reduced
            << " lerp=" << lerpValue << std::endl;
    std::cout << "  dot3 => " << dot3 << std::endl;
    std::cout << "  fma lanes => ["
            << fmaOut[0] << ", "
            << fmaOut[1] << ", "
            << fmaOut[2] << ", "
            << fmaOut[3] << "]" << std::endl;

    const auto &metrics = mathModule.GetMetrics();
    std::cout << "  metrics: sinCalls=" << metrics.fastSinCalls
            << " cosCalls=" << metrics.fastCosCalls
            << " tanCalls=" << metrics.fastTanCalls
            << " fmaOps=" << metrics.batchedFmaOps << std::endl;

    std::cout.flags(originalFlags);
    std::cout.precision(originalPrecision);
}

void DemonstrateDateModule(spectre::es2025::DateModule &dateModule) {
    std::cout << "\nDemonstrating date module" << std::endl;
    spectre::es2025::DateModule::Handle nowHandle = 0;
    if (dateModule.Now("demo.now", nowHandle) != spectre::StatusCode::Ok) {
        std::cout << "  unable to create current date" << std::endl;
        return;
    }

    std::string iso;
    dateModule.FormatIso8601(nowHandle, iso);
    spectre::es2025::DateModule::Components components{};
    dateModule.ToComponents(nowHandle, components);

    spectre::es2025::DateModule::Handle launchHandle = 0;
    dateModule.ParseIso8601("2025-10-07T12:34:56.789Z", "demo.launch", launchHandle);

    std::int64_t deltaMs = 0;
    dateModule.DifferenceMilliseconds(launchHandle, nowHandle, deltaMs);

    dateModule.AddDays(launchHandle, 3);
    std::string shiftedIso;
    dateModule.FormatIso8601(launchHandle, shiftedIso);

    auto preservedFlags = std::cout.flags();
    auto preservedPrecision = std::cout.precision();
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  now => " << iso << std::endl;
    std::cout << "  now components => year=" << components.year
            << " day=" << components.day
            << " weekday=" << components.dayOfWeek << std::endl;
    std::cout << "  launch => " << shiftedIso << " (delta " << deltaMs << " ms)" << std::endl;
    std::cout.flags(preservedFlags);
    std::cout.precision(preservedPrecision);

    dateModule.Destroy(launchHandle);
    dateModule.Destroy(nowHandle);
}

void DemonstrateNumberModule(spectre::es2025::NumberModule &numberModule) {
    std::cout << "\nDemonstrating number module" << std::endl;
    auto preservedFlags = std::cout.flags();
    auto preservedPrecision = std::cout.precision();
    double telemetry[6] = {12.0, 14.5, 18.25, 21.75, 16.5, 13.0};
    spectre::es2025::NumberModule::Handle peak = 0;
    if (numberModule.Create("metrics.peak", telemetry[2], peak) != spectre::StatusCode::Ok) {
        std::cout << "  unable to allocate number" << std::endl;
        return;
    }
    numberModule.Add(peak, 9.5);
    numberModule.Saturate(peak, 0.0, 32.0);
    double sum = 0.0;
    numberModule.Accumulate(telemetry, 6, sum);
    numberModule.Normalize(telemetry, 6, 0.0, 1.0);
    spectre::es2025::NumberModule::Statistics summary{};
    numberModule.BuildStatistics(telemetry, 6, summary);
    auto canonicalZero = numberModule.Canonical(0.0);
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  peak => " << numberModule.ValueOf(peak)
            << " normalized mean=" << summary.mean
            << " variance=" << summary.variance << std::endl;
    const auto &metrics = numberModule.GetMetrics();
    std::cout << "  metrics: allocations=" << metrics.allocations
            << " mutations=" << metrics.mutations
            << " accumulations=" << metrics.accumulations << std::endl;
    std::cout << "  canonical zero handle=" << canonicalZero << std::endl;
    numberModule.Destroy(peak);
    std::cout.flags(preservedFlags);
    std::cout.precision(preservedPrecision);
}

void DemonstrateBigIntModule(spectre::es2025::BigIntModule &bigintModule) {
    std::cout << "\nDemonstrating bigint module" << std::endl;
    spectre::es2025::BigIntModule::Handle base = 0;
    if (bigintModule.CreateFromDecimal("ledger.base", "12345678901234567890", base) != spectre::StatusCode::Ok) {
        std::cout << "  unable to allocate base bigint" << std::endl;
        return;
    }
    spectre::es2025::BigIntModule::Handle delta = 0;
    bigintModule.Create("ledger.delta", 1024, delta);
    bigintModule.Add(base, delta);
    bigintModule.ShiftLeft(base, 12);
    bigintModule.MultiplySmall(base, 5);
    std::string decimal;
    bigintModule.ToDecimalString(base, decimal);
    auto comparison = bigintModule.Compare(base, delta);
    std::cout << "  base => " << decimal << std::endl;
    std::cout << "  compare sign=" << comparison.sign << " digits=" << comparison.digits << std::endl;
    const auto &metrics = bigintModule.GetMetrics();
    std::cout << "  metrics: allocations=" << metrics.allocations
            << " additions=" << metrics.additions
            << " multiplications=" << metrics.multiplications << std::endl;
    bigintModule.Destroy(delta);
    bigintModule.Destroy(base);
}

void DemonstrateObjectModule(spectre::es2025::ObjectModule &objectModule) {
    std::cout << "\nDemonstrating Object module" << std::endl;
    spectre::es2025::ObjectModule::Handle prototype = 0;
    if (objectModule.Create("demo.prototype", 0, prototype) != spectre::StatusCode::Ok) {
        std::cout << "  object prototype unavailable" << std::endl;
        return;
    }
    spectre::es2025::ObjectModule::PropertyDescriptor descriptor;
    descriptor.value = spectre::es2025::ObjectModule::Value::FromString("entity");
    descriptor.enumerable = true;
    descriptor.configurable = true;
    descriptor.writable = true;
    objectModule.Define(prototype, "type", descriptor);
    spectre::es2025::ObjectModule::Handle instance = 0;
    if (objectModule.Create("demo.instance", prototype, instance) != spectre::StatusCode::Ok) {
        objectModule.Destroy(prototype);
        std::cout << "  object instance unavailable" << std::endl;
        return;
    }
    objectModule.Set(instance, "hp", spectre::es2025::ObjectModule::Value::FromInt(240));
    objectModule.Set(instance, "name", spectre::es2025::ObjectModule::Value::FromString("spectre-npc"));
    std::vector<std::string> keys;
    if (objectModule.OwnKeys(instance, keys) == spectre::StatusCode::Ok) {
        std::cout << "  own keys:";
        for (const auto &key: keys) {
            std::cout << " " << key;
        }
        std::cout << std::endl;
    }
    spectre::es2025::ObjectModule::Value protoValue;
    if (objectModule.Get(instance, "type", protoValue) == spectre::StatusCode::Ok && protoValue.IsString()) {
        std::cout << "  prototype type => " << protoValue.String() << std::endl;
    }
    objectModule.Destroy(instance);
    objectModule.Destroy(prototype);
}

void DemonstrateProxyModule(spectre::es2025::ObjectModule &objectModule, spectre::es2025::ProxyModule &proxyModule) {
    std::cout << "\nDemonstrating Proxy module" << std::endl;
    spectre::es2025::ObjectModule::Handle target = 0;
    if (objectModule.Create("demo.proxy.target", 0, target) != spectre::StatusCode::Ok) {
        std::cout << "  proxy target unavailable" << std::endl;
        return;
    }
    objectModule.Set(target, "count", spectre::es2025::ObjectModule::Value::FromInt(3));
    struct ProxyState {
        int gets;
        int sets;
        int deletes;
    } state{0, 0, 0};
    spectre::es2025::ProxyModule::TrapTable traps{};
    traps.get = [](spectre::es2025::ObjectModule &objects, spectre::es2025::ObjectModule::Handle handle,
                   std::string_view key, spectre::es2025::ObjectModule::Value &outValue,
                   void *userdata) -> spectre::StatusCode {
        auto *stats = static_cast<ProxyState *>(userdata);
        stats->gets += 1;
        return objects.Get(handle, key, outValue);
    };
    traps.set = [](spectre::es2025::ObjectModule &objects, spectre::es2025::ObjectModule::Handle handle,
                   std::string_view key, const spectre::es2025::ObjectModule::Value &value,
                   void *userdata) -> spectre::StatusCode {
        auto *stats = static_cast<ProxyState *>(userdata);
        stats->sets += 1;
        return objects.Set(handle, key, value);
    };
    traps.drop = [](spectre::es2025::ObjectModule &objects, spectre::es2025::ObjectModule::Handle handle,
                    std::string_view key, bool &removed, void *userdata) -> spectre::StatusCode {
        auto *stats = static_cast<ProxyState *>(userdata);
        stats->deletes += 1;
        return objects.Delete(handle, key, removed);
    };
    traps.userdata = &state;
    spectre::es2025::ProxyModule::Handle proxy = 0;
    if (proxyModule.Create(target, traps, proxy) != spectre::StatusCode::Ok) {
        objectModule.Destroy(target);
        std::cout << "  proxy instance unavailable" << std::endl;
        return;
    }
    spectre::es2025::ObjectModule::Value value;
    if (proxyModule.Get(proxy, "count", value) == spectre::StatusCode::Ok && value.IsInt()) {
        std::cout << "  initial count => " << value.Int() << std::endl;
    }
    proxyModule.Set(proxy, "count", spectre::es2025::ObjectModule::Value::FromInt(7));
    bool hasCount = false;
    proxyModule.Has(proxy, "count", hasCount);
    std::cout << "  has count => " << (hasCount ? "true" : "false") << std::endl;
    bool removed = false;
    proxyModule.Delete(proxy, "count", removed);
    std::cout << "  delete count => " << (removed ? "true" : "false") << std::endl;
    std::vector<std::string> keys;
    proxyModule.OwnKeys(proxy, keys);
    std::cout << "  keys:";
    for (const auto &key: keys) {
        std::cout << " " << key;
    }
    std::cout << std::endl;
    proxyModule.Revoke(proxy);
    proxyModule.Destroy(proxy);
    objectModule.Destroy(target);
    std::cout << "  traps counts => get:" << state.gets << " set:" << state.sets << " delete:" << state.deletes <<
            std::endl;
}

void DemonstrateMapModule(spectre::es2025::MapModule &mapModule) {
    std::cout << "\nDemonstrating Map module" << std::endl;
    spectre::es2025::MapModule::Handle handle = 0;
    if (mapModule.Create("demo.map", handle) != spectre::StatusCode::Ok) {
        std::cout << "  map unavailable" << std::endl;
        return;
    }
    mapModule.Set(handle, spectre::es2025::MapModule::Value::FromString("name"),
                  spectre::es2025::MapModule::Value::FromString("spectre-map"));
    mapModule.Set(handle, spectre::es2025::MapModule::Value::FromInt(7),
                  spectre::es2025::MapModule::Value::FromDouble(42.5));
    spectre::es2025::MapModule::Value value;
    if (mapModule.Get(handle, spectre::es2025::MapModule::Value::FromString("name"), value) == spectre::StatusCode::Ok
        && value.IsString()) {
        std::cout << "  name => " << value.String() << std::endl;
    }
    std::vector<spectre::es2025::MapModule::Value> keys;
    if (mapModule.Keys(handle, keys) == spectre::StatusCode::Ok) {
        std::cout << "  keys:";
        for (const auto &k: keys) {
            if (k.IsString()) {
                std::cout << " " << k.String();
            } else if (k.IsInt()) {
                std::cout << " " << k.Int();
            }
        }
        std::cout << std::endl;
    }
    std::vector<spectre::es2025::MapModule::Value> values;
    if (mapModule.Values(handle, values) == spectre::StatusCode::Ok) {
        std::cout << "  values:";
        for (const auto &v: values) {
            if (v.IsString()) {
                std::cout << " " << v.String();
            } else if (v.IsDouble()) {
                std::cout << " " << v.Double();
            }
        }
        std::cout << std::endl;
    }
    bool removed = false;
    mapModule.Delete(handle, spectre::es2025::MapModule::Value::FromInt(7), removed);
    std::cout << "  delete numeric key => " << (removed ? "true" : "false") << std::endl;
    mapModule.Clear(handle);
    mapModule.Destroy(handle);
}

void DemonstrateWeakMapModule(spectre::es2025::ObjectModule &objectModule,
                              spectre::es2025::WeakMapModule &weakMapModule) {
    std::cout << "\nDemonstrating WeakMap module" << std::endl;
    spectre::es2025::WeakMapModule::Handle handle = 0;
    if (weakMapModule.Create("demo.weakmap", handle) != spectre::StatusCode::Ok) {
        std::cout << "  weak map unavailable" << std::endl;
        return;
    }
    spectre::es2025::ObjectModule::Handle keyA = 0;
    spectre::es2025::ObjectModule::Handle keyB = 0;
    if (objectModule.Create("demo.weakmap.keyA", 0, keyA) != spectre::StatusCode::Ok || objectModule.
        Create("demo.weakmap.keyB", 0, keyB) != spectre::StatusCode::Ok) {
        std::cout << "  object handles unavailable" << std::endl;
        if (keyA != 0) {
            objectModule.Destroy(keyA);
        }
        weakMapModule.Destroy(handle);
        return;
    }
    weakMapModule.Set(handle, keyA, spectre::es2025::WeakMapModule::Value::FromInt(12));
    weakMapModule.Set(handle, keyB, spectre::es2025::WeakMapModule::Value::FromInt(44));
    spectre::es2025::WeakMapModule::Value value;
    if (weakMapModule.Get(handle, keyA, value) == spectre::StatusCode::Ok && value.IsInt()) {
        std::cout << "  keyA => " << value.Int() << std::endl;
    }
    objectModule.Destroy(keyA);
    weakMapModule.Compact(handle);
    std::cout << "  size after destroy => " << weakMapModule.Size(handle) << std::endl;
    weakMapModule.Destroy(handle);
    objectModule.Destroy(keyB);
}

void DemonstrateIteratorModule(spectre::es2025::IteratorModule &iteratorModule) {
    using Value = spectre::es2025::Value;
    spectre::es2025::IteratorModule::Handle rangeHandle = 0;
    spectre::es2025::IteratorModule::RangeConfig rangeConfig{0, 6, 1, false};
    if (iteratorModule.CreateRange(rangeConfig, rangeHandle) == spectre::StatusCode::Ok) {
        std::array<spectre::es2025::IteratorModule::Result, 12> buffer{};
        auto produced = iteratorModule.Drain(rangeHandle, buffer);
        std::cout << "\nIterator range demo (" << produced << ")" << std::endl;
        for (std::size_t i = 0; i < produced; ++i) {
            const auto &entry = buffer[i];
            std::cout << "  value=" << entry.value.ToString() << " done=" << (entry.done ? "true" : "false") << std::endl;
        }
        iteratorModule.Destroy(rangeHandle);
    }

    std::vector<Value> waypoints;
    waypoints.emplace_back(Value::String("alpha"));
    waypoints.emplace_back(Value::String("beta"));
    waypoints.emplace_back(Value::String("gamma"));
    spectre::es2025::IteratorModule::Handle listHandle = 0;
    if (iteratorModule.CreateList(std::move(waypoints), listHandle) == spectre::StatusCode::Ok) {
        std::cout << "Iterator list demo" << std::endl;
        auto item = iteratorModule.Next(listHandle);
        while (!item.done) {
            std::cout << "  item=" << item.value.ToString() << std::endl;
            item = iteratorModule.Next(listHandle);
        }
        iteratorModule.Destroy(listHandle);
    }

    struct CustomState {
        double current;
        double step;
        int closes;
        int destroys;
    } custom{1.0, 0.25, 0, 0};

    auto nextFn = [](void *state) -> spectre::es2025::IteratorModule::Result {
        auto *ptr = static_cast<CustomState *>(state);
        spectre::es2025::IteratorModule::Result result;
        if (ptr == nullptr) {
            result.done = true;
            result.hasValue = false;
            return result;
        }
        if (ptr->current > 2.0) {
            result.done = true;
            result.hasValue = false;
            return result;
        }
        result.done = false;
        result.hasValue = true;
        result.value = spectre::es2025::Value::Number(ptr->current);
        ptr->current += ptr->step;
        return result;
    };

    auto resetFn = [](void *state) {
        auto *ptr = static_cast<CustomState *>(state);
        if (ptr == nullptr) {
            return;
        }
        ptr->current = 1.0;
    };

    auto closeFn = [](void *state) {
        auto *ptr = static_cast<CustomState *>(state);
        if (ptr == nullptr) {
            return;
        }
        ptr->closes += 1;
    };

    auto destroyFn = [](void *state) {
        auto *ptr = static_cast<CustomState *>(state);
        if (ptr == nullptr) {
            return;
        }
        ptr->destroys += 1;
    };

    spectre::es2025::IteratorModule::CustomConfig customConfig{};
    customConfig.next = nextFn;
    customConfig.reset = resetFn;
    customConfig.close = closeFn;
    customConfig.destroy = destroyFn;
    customConfig.state = &custom;
    spectre::es2025::IteratorModule::Handle customHandle = 0;
    if (iteratorModule.CreateCustom(customConfig, customHandle) == spectre::StatusCode::Ok) {
        std::cout << "Iterator custom demo" << std::endl;
        iteratorModule.Reset(customHandle);
        auto value = iteratorModule.Next(customHandle);
        while (!value.done) {
            std::cout << "  value=" << value.value.ToString() << std::endl;
            value = iteratorModule.Next(customHandle);
        }
        iteratorModule.Close(customHandle);
        iteratorModule.Destroy(customHandle);
        std::cout << "  closes=" << custom.closes << " destroys=" << custom.destroys << std::endl;
    }
}

void DemonstrateGeneratorModule(spectre::es2025::GeneratorModule &generatorModule,
                                spectre::es2025::IteratorModule &iteratorModule) {
    struct GeneratorState {
        int index;
        int limit;
        bool finalValue;
    } state{0, 3, false};

    auto resetFn = [](void *ptr) {
        auto *statePtr = static_cast<GeneratorState *>(ptr);
        if (statePtr == nullptr) {
            return;
        }
        statePtr->index = 0;
        statePtr->finalValue = false;
    };

    auto stepFn = [](void *ptr, spectre::es2025::GeneratorModule::ExecutionContext &context) {
        auto *statePtr = static_cast<GeneratorState *>(ptr);
        if (statePtr == nullptr) {
            context.done = true;
            context.hasValue = false;
            return;
        }
        context.requestingInput = false;
        context.nextResumePoint = 0;
        if (statePtr->index < statePtr->limit) {
            context.yieldValue = spectre::es2025::Value::Number(static_cast<double>(statePtr->index));
            context.hasValue = true;
            context.done = false;
            statePtr->index += 1;
            return;
        }
        if (!statePtr->finalValue) {
            context.yieldValue = spectre::es2025::Value::String("generator-final");
            context.hasValue = true;
            context.done = true;
            statePtr->finalValue = true;
            return;
        }
        context.done = true;
        context.hasValue = false;
    };

    spectre::es2025::GeneratorModule::Descriptor descriptor{};
    descriptor.stepper = stepFn;
    descriptor.state = &state;
    descriptor.reset = resetFn;
    descriptor.destroy = nullptr;
    descriptor.name = "generator-demo";
    descriptor.resumePoint = 0;

    spectre::es2025::GeneratorModule::Handle handle = 0;
    if (generatorModule.Register(descriptor, handle) != spectre::StatusCode::Ok) {
        std::cout << "\nGenerator demo unavailable" << std::endl;
        return;
    }

    std::cout << "\nGenerator resume demo" << std::endl;
    auto step = generatorModule.Resume(handle);
    while (!step.done || step.hasValue) {
        std::cout << "  value=" << step.value.ToString() << " done=" << (step.done ? "true" : "false") << std::endl;
        if (step.done) {
            break;
        }
        step = generatorModule.Resume(handle);
    }

    generatorModule.Reset(handle);
    std::uint32_t iteratorHandle = 0;
    if (generatorModule.CreateIteratorBridge(handle, iteratorModule, iteratorHandle) == spectre::StatusCode::Ok) {
        std::array<spectre::es2025::IteratorModule::Result, 8> bridge{};
        auto produced = iteratorModule.Drain(iteratorHandle, bridge);
        std::cout << "Generator bridge demo" << std::endl;
        for (std::size_t i = 0; i < produced; ++i) {
            const auto &entry = bridge[i];
            std::cout << "  bridge=" << entry.value.ToString() << " done=" << (entry.done ? "true" : "false") << std::endl;
        }
        iteratorModule.Destroy(iteratorHandle);
    }

    generatorModule.Destroy(handle);
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

    auto *objectModulePtr = environment.FindModule("Object");
    auto *objectModule = dynamic_cast<es2025::ObjectModule *>(objectModulePtr);
    if (!objectModule) {
        std::cerr << "Object module unavailable" << std::endl;
        return 1;
    }

    auto *proxyModulePtr = environment.FindModule("Proxy");
    auto *proxyModule = dynamic_cast<es2025::ProxyModule *>(proxyModulePtr);
    if (!proxyModule) {
        std::cerr << "Proxy module unavailable" << std::endl;
        return 1;
    }

    auto *mapModulePtr = environment.FindModule("Map");
    auto *mapModule = dynamic_cast<es2025::MapModule *>(mapModulePtr);
    if (!mapModule) {
        std::cerr << "Map module unavailable" << std::endl;
        return 1;
    }

    auto *weakMapModulePtr = environment.FindModule("WeakMap");
    auto *weakMapModule = dynamic_cast<es2025::WeakMapModule *>(weakMapModulePtr);
    if (!weakMapModule) {
        std::cerr << "WeakMap module unavailable" << std::endl;
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

    auto *iteratorModulePtr = environment.FindModule("Iterator");
    auto *iteratorModule = dynamic_cast<es2025::IteratorModule *>(iteratorModulePtr);
    if (!iteratorModule) {
        std::cerr << "Iterator module unavailable" << std::endl;
        return 1;
    }

    auto *generatorModulePtr = environment.FindModule("Generator");
    auto *generatorModule = dynamic_cast<es2025::GeneratorModule *>(generatorModulePtr);
    if (!generatorModule) {
        std::cerr << "Generator module unavailable" << std::endl;
        return 1;
    }

    auto *arrayBufferModulePtr = environment.FindModule("ArrayBuffer");
    auto *arrayBufferModule = dynamic_cast<es2025::ArrayBufferModule *>(arrayBufferModulePtr);
    if (!arrayBufferModule) {
        std::cerr << "ArrayBuffer module unavailable" << std::endl;
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

    auto *dateModulePtr = environment.FindModule("Date");
    auto *dateModule = dynamic_cast<es2025::DateModule *>(dateModulePtr);
    if (!dateModule) {
        std::cerr << "Date module unavailable" << std::endl;
        return 1;
    }

    auto *numberModulePtr = environment.FindModule("Number");
    auto *numberModule = dynamic_cast<es2025::NumberModule *>(numberModulePtr);
    if (!numberModule) {
        std::cerr << "Number module unavailable" << std::endl;
        return 1;
    }

    auto *bigintModulePtr = environment.FindModule("BigInt");
    auto *bigintModule = dynamic_cast<es2025::BigIntModule *>(bigintModulePtr);
    if (!bigintModule) {
        std::cerr << "BigInt module unavailable" << std::endl;
        return 1;
    }

    auto *stringModulePtr = environment.FindModule("String");
    auto *stringModule = dynamic_cast<es2025::StringModule *>(stringModulePtr);
    if (!stringModule) {
        std::cerr << "String module unavailable" << std::endl;
        return 1;
    }

    auto *symbolModulePtr = environment.FindModule("Symbol");
    auto *symbolModule = dynamic_cast<es2025::SymbolModule *>(symbolModulePtr);
    if (!symbolModule) {
        std::cerr << "Symbol module unavailable" << std::endl;
        return 1;
    }

    auto *mathModulePtr = environment.FindModule("Math");
    auto *mathModule = dynamic_cast<es2025::MathModule *>(mathModulePtr);
    if (!mathModule) {
        std::cerr << "Math module unavailable" << std::endl;
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

    ShowcaseSymbols(*symbolModule);
    DemonstrateIteratorModule(*iteratorModule);
    DemonstrateGeneratorModule(*generatorModule, *iteratorModule);
    DemonstrateArrayModule(*arrayModule);
    DemonstrateArrayBufferModule(*arrayBufferModule);
    DemonstrateAtomicsModule(*atomicsModule);
    DemonstrateBooleanModule(*booleanModule);
    DemonstrateStringModule(*stringModule);
    DemonstrateMathModule(*mathModule);
    DemonstrateDateModule(*dateModule);
    DemonstrateNumberModule(*numberModule);
    DemonstrateBigIntModule(*bigintModule);
    DemonstrateObjectModule(*objectModule);
    DemonstrateProxyModule(*objectModule, *proxyModule);
    DemonstrateMapModule(*mapModule);
    DemonstrateWeakMapModule(*objectModule, *weakMapModule);

    std::cout << "\nDemonstrating error capture" << std::endl;
    std::string failingValue;
    std::string failingDiagnostics;
    auto errorStatus = globalModule->EvaluateScript("let a = 1;", failingValue, failingDiagnostics, "invalid-script");
    if (errorStatus != StatusCode::Ok) {
        std::string formatted;
        errorModule->RaiseError("SyntaxError",
                                failingDiagnostics.empty() ? "Script parsing failed" : failingDiagnostics,
                                globalModule->DefaultContext(), "invalid-script", failingDiagnostics, formatted,
                                nullptr);
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
