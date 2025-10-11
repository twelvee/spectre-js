#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cctype>
#include <array>
#include <chrono>
#include <cstdint>

#include <numeric>
#include "spectre/config.h"
#include "spectre/runtime.h"
#include "spectre/status.h"
#include "spectre/es2025/environment.h"
#include "spectre/es2025/modules/global_module.h"
#include "spectre/es2025/modules/object_module.h"
#include "spectre/es2025/modules/proxy_module.h"
#include "spectre/es2025/modules/reflect_module.h"
#include "spectre/es2025/modules/error_module.h"
#include "spectre/es2025/modules/function_module.h"
#include "spectre/es2025/modules/async_function_module.h"
#include "spectre/es2025/modules/async_iterator_module.h"
#include "spectre/es2025/modules/promise_module.h"
#include "spectre/es2025/modules/atomics_module.h"
#include "spectre/es2025/modules/boolean_module.h"
#include "spectre/es2025/modules/array_module.h"
#include "spectre/es2025/modules/array_buffer_module.h"
#include "spectre/es2025/modules/data_view_module.h"
#include "spectre/es2025/modules/shared_array_buffer_module.h"
#include "spectre/es2025/modules/iterator_module.h"
#include "spectre/es2025/modules/generator_module.h"
#include "spectre/es2025/modules/string_module.h"
#include "spectre/es2025/modules/regexp_module.h"
#include "spectre/es2025/modules/typed_array_module.h"
#include "spectre/es2025/modules/structured_clone_module.h"
#include "spectre/es2025/modules/json_module.h"
#include "spectre/es2025/modules/module_loader_module.h"
#include "spectre/es2025/modules/symbol_module.h"
#include "spectre/es2025/modules/math_module.h"
#include "spectre/es2025/modules/number_module.h"
#include "spectre/es2025/modules/bigint_module.h"
#include "spectre/es2025/modules/date_module.h"
#include "spectre/es2025/modules/map_module.h"
#include "spectre/es2025/modules/set_module.h"
#include "spectre/es2025/modules/weak_set_module.h"
#include "spectre/es2025/modules/weak_map_module.h"
#include "spectre/es2025/modules/weak_ref_module.h"
#include "spectre/es2025/modules/shadow_realm_module.h"
#include "spectre/es2025/modules/temporal_module.h"
#include "spectre/es2025/value.h"
#include "spectre/es2025/modules/intl_module.h"

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

    struct DemoAsyncPayload {
        bool *flag;
        int base;
        int delta;
    };

    spectre::StatusCode DemoAsyncValueCallback(void *userData,
                                               spectre::es2025::Value &outValue,
                                               std::string &outDiagnostics) {
        auto *payload = static_cast<DemoAsyncPayload *>(userData);
        if (payload != nullptr && payload->flag != nullptr) {
            *payload->flag = true;
        }
        const int value = payload != nullptr ? (payload->base + payload->delta) : 0;
        outValue = spectre::es2025::Value::Number(static_cast<double>(value));
        outDiagnostics = "ok";
        return spectre::StatusCode::Ok;
    }

    spectre::StatusCode DemoAsyncMessageCallback(void *userData,
                                                 spectre::es2025::Value &outValue,
                                                 std::string &outDiagnostics) {
        auto *message = static_cast<const char *>(userData);
        outValue = spectre::es2025::Value::String(message != nullptr ? std::string_view(message) : "async-demo");
        outDiagnostics = "resolved";
        return spectre::StatusCode::Ok;
    }

    struct DemoPromisePayload {
        int factor;
        bool invoked;
    };

    spectre::StatusCode DemoPromiseFulfillCallback(void *userData,
                                                   const spectre::es2025::Value &input,
                                                   spectre::es2025::Value &outValue,
                                                   std::string &outDiagnostics) {
        auto *payload = static_cast<DemoPromisePayload *>(userData);
        if (payload) {
            payload->invoked = true;
            auto value = input.AsNumber(0.0) * static_cast<double>(payload->factor);
            outValue = spectre::es2025::Value::Number(value);
        } else {
            outValue = input;
        }
        outDiagnostics = "promise-scaled";
        return spectre::StatusCode::Ok;
    }

    spectre::StatusCode DemoPromiseRejectCallback(void *userData,
                                                  const spectre::es2025::Value &input,
                                                  spectre::es2025::Value &outValue,
                                                  std::string &outDiagnostics) {
        auto *payload = static_cast<DemoPromisePayload *>(userData);
        if (payload) {
            payload->invoked = true;
        }
        (void) input;
        outValue = spectre::es2025::Value::String("promise-recovered");
        outDiagnostics = "promise-handled";
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

    void DemonstrateAsyncFunctionModule(spectre::es2025::AsyncFunctionModule &asyncModule,
                                        spectre::SpectreRuntime &runtime) {
        std::cout << "\nAsyncFunction job queue" << std::endl;
        if (asyncModule.Configure(16, 8) != spectre::StatusCode::Ok) {
            std::cout << "  async queue unavailable" << std::endl;
            return;
        }

        bool fastResolved = false;
        bool delayedResolved = false;
        DemoAsyncPayload fastPayload{&fastResolved, 48, 12};
        DemoAsyncPayload delayedPayload{&delayedResolved, 128, -28};
        const char *messagePayload = "demo.message";

        spectre::es2025::AsyncFunctionModule::DispatchOptions fastOptions;
        fastOptions.label = "demo.fast";
        spectre::es2025::AsyncFunctionModule::DispatchOptions delayedOptions;
        delayedOptions.delayFrames = 2;
        delayedOptions.delaySeconds = 0.030;
        delayedOptions.label = "demo.delayed";
        spectre::es2025::AsyncFunctionModule::DispatchOptions messageOptions;
        messageOptions.delayFrames = 1;
        messageOptions.label = "demo.message";

        spectre::es2025::AsyncFunctionModule::Handle fastHandle = 0;
        spectre::es2025::AsyncFunctionModule::Handle delayedHandle = 0;
        spectre::es2025::AsyncFunctionModule::Handle messageHandle = 0;

        if (asyncModule.Enqueue(DemoAsyncValueCallback, &fastPayload, fastOptions, fastHandle) != spectre::StatusCode::Ok ||
            asyncModule.Enqueue(DemoAsyncValueCallback, &delayedPayload, delayedOptions, delayedHandle) != spectre::StatusCode::Ok ||
            asyncModule.Enqueue(DemoAsyncMessageCallback, const_cast<char *>(messagePayload), messageOptions,
                                messageHandle) != spectre::StatusCode::Ok) {
            std::cout << "  failed to enqueue demo jobs" << std::endl;
            return;
        }

        std::cout << "  handles => " << fastHandle << ", " << delayedHandle << ", " << messageHandle << std::endl;
        std::cout << "  pending jobs: " << asyncModule.PendingCount() << std::endl;

        std::vector<spectre::es2025::AsyncFunctionModule::Result> results;
        auto processTick = [&](double delta, std::uint64_t frame) {
            runtime.Tick({delta, frame});
            asyncModule.DrainCompleted(results);
            for (const auto &entry: results) {
                std::ostringstream timing;
                timing << std::fixed << std::setprecision(2) << entry.executionMicros;
                std::cout << "    [frame " << entry.completedFrame << "] " << entry.label.data()
                        << " status=" << static_cast<int>(entry.status)
                        << " value=" << entry.value.ToString()
                        << " diag=" << entry.diagnostics
                        << " time=" << timing.str() << "us" << std::endl;
            }
            results.clear();
        };

        processTick(0.016, 0);
        processTick(0.016, 1);
        processTick(0.016, 2);

        const auto &metrics = asyncModule.GetMetrics();
        std::ostringstream average;
        average << std::fixed << std::setprecision(2) << metrics.AverageExecutionMicros();
        std::cout << "  metrics enqueued=" << metrics.enqueued
                << " executed=" << metrics.executed
                << " cancelled=" << metrics.cancelled
                << " failed=" << metrics.failed
                << " avg=" << average.str() << "us" << std::endl;
        std::cout << "  pending after ticks: " << asyncModule.PendingCount() << std::endl;
        std::cout << "  fast resolved=" << (fastResolved ? "yes" : "no")
                << " delayed resolved=" << (delayedResolved ? "yes" : "no") << std::endl;
    }

    void DemonstrateAsyncIteratorModule(spectre::es2025::AsyncIteratorModule &iteratorModule,
                                        spectre::SpectreRuntime &runtime) {
        std::cout << "\nAsyncIterator host streams" << std::endl;
        spectre::es2025::AsyncIteratorModule::StreamConfig config{};
        config.queueCapacity = 6;
        config.waiterCapacity = 4;
        config.label = "demo.iter";
        spectre::es2025::AsyncIteratorModule::Handle handle = 0;
        if (iteratorModule.CreateStream(config, handle) != spectre::StatusCode::Ok) {
            std::cout << "  stream unavailable" << std::endl;
            return;
        }
        std::cout << "  stream handle => " << handle << std::endl;
        std::cout << "  active streams => " << iteratorModule.ActiveStreams() << std::endl;

        auto dumpResults = [](const std::vector<spectre::es2025::AsyncIteratorModule::Result> &results) {
            for (const auto &entry: results) {
                std::cout << "    ticket " << entry.ticket
                        << " state=" << static_cast<int>(entry.streamState)
                        << " status=" << static_cast<int>(entry.status)
                        << " value=" << (entry.hasValue ? entry.value.ToString() : "<void>")
                        << " done=" << (entry.done ? "true" : "false")
                        << " label=" << entry.requestLabel.data()
                        << " frame=" << entry.satisfiedFrame
                        << " diag=" << entry.diagnostics << std::endl;
            }
        };

        std::vector<spectre::es2025::AsyncIteratorModule::Result> drained;

        spectre::es2025::AsyncIteratorModule::EnqueueOptions warmup{};
        warmup.value = spectre::es2025::Value::String("warmup");
        warmup.hasValue = true;
        warmup.done = false;
        warmup.diagnostics = "preloaded";
        if (iteratorModule.Enqueue(handle, warmup) != spectre::StatusCode::Ok) {
            std::cout << "  failed to preload warmup" << std::endl;
        }

        spectre::es2025::AsyncIteratorModule::Request immediate;
        if (iteratorModule.RequestNext(handle, immediate) == spectre::StatusCode::Ok && immediate.immediate) {
            std::cout << "  immediate => value=" << immediate.result.value.ToString()
                    << " diagnostics=" << immediate.result.diagnostics << std::endl;
        }

        iteratorModule.DrainSettled(drained);
        dumpResults(drained);
        drained.clear();

        spectre::es2025::AsyncIteratorModule::Request pending;
        if (iteratorModule.RequestNext(handle, pending) == spectre::StatusCode::Ok) {
            std::cout << "  pending ticket => " << pending.ticket << std::endl;
        } else {
            std::cout << "  failed to enqueue waiter" << std::endl;
        }

        auto lastTick = runtime.LastTick();
        runtime.Tick({0.008, lastTick.frameIndex + 1});

        spectre::es2025::AsyncIteratorModule::EnqueueOptions payload{};
        payload.value = spectre::es2025::Value::Number(7.5);
        payload.hasValue = true;
        payload.done = false;
        payload.diagnostics = "payload";
        if (iteratorModule.Enqueue(handle, payload) != spectre::StatusCode::Ok) {
            std::cout << "  failed to enqueue payload" << std::endl;
        }

        iteratorModule.DrainSettled(drained);
        dumpResults(drained);
        drained.clear();

        if (iteratorModule.SignalComplete(handle, "completed") != spectre::StatusCode::Ok) {
            std::cout << "  completion signal failed" << std::endl;
        }
        spectre::es2025::AsyncIteratorModule::Request completion;
        if (iteratorModule.RequestNext(handle, completion) == spectre::StatusCode::Ok && completion.immediate) {
            std::cout << "  completion => done=" << (completion.result.done ? "true" : "false")
                    << " diagnostics=" << completion.result.diagnostics << std::endl;
        }

        iteratorModule.DrainSettled(drained);
        dumpResults(drained);
        drained.clear();

        spectre::es2025::AsyncIteratorModule::Request postComplete;
        if (iteratorModule.RequestNext(handle, postComplete) == spectre::StatusCode::Ok && postComplete.immediate) {
            std::cout << "  subsequent => done=" << (postComplete.result.done ? "true" : "false") << std::endl;
        }

        const auto &metrics = iteratorModule.GetMetrics();
        std::cout << "  metrics queued=" << metrics.valuesQueued
                << " delivered=" << metrics.valuesDelivered
                << " completed=" << metrics.completionsDelivered
                << " waiters=" << metrics.waitersEnqueued
                << " served=" << metrics.waitersServed << std::endl;

        if (!iteratorModule.DestroyStream(handle)) {
            std::cout << "  failed to destroy stream" << std::endl;
        } else {
            std::cout << "  active streams => " << iteratorModule.ActiveStreams() << std::endl;
        }
    }

    void DemonstratePromiseModule(spectre::es2025::PromiseModule &promiseModule,
                                  spectre::SpectreRuntime &runtime) {
        std::cout << "\nPromise microtask demo" << std::endl;
        if (promiseModule.Configure(32, 64) != spectre::StatusCode::Ok) {
            std::cout << "  promise module unavailable" << std::endl;
            return;
        }

        auto stateName = [](spectre::es2025::PromiseModule::State state) {
            switch (state) {
                case spectre::es2025::PromiseModule::State::Pending:
                    return "pending";
                case spectre::es2025::PromiseModule::State::Fulfilled:
                    return "fulfilled";
                case spectre::es2025::PromiseModule::State::Rejected:
                    return "rejected";
                case spectre::es2025::PromiseModule::State::Cancelled:
                    return "cancelled";
            }
            return "unknown";
        };

        spectre::es2025::PromiseModule::Handle resolveRoot = 0;
        spectre::es2025::PromiseModule::Handle resolveScaled = 0;
        DemoPromisePayload fulfillPayload{4, false};

        if (promiseModule.CreatePromise(resolveRoot, {"promise.resolve"}) == spectre::StatusCode::Ok) {
            spectre::es2025::PromiseModule::ReactionOptions options{};
            options.onFulfilled = DemoPromiseFulfillCallback;
            options.userData = &fulfillPayload;
            options.label = "promise.scale";
            if (promiseModule.Then(resolveRoot, resolveScaled, options) == spectre::StatusCode::Ok) {
                promiseModule.Resolve(resolveRoot, spectre::es2025::Value::Number(2.5), "ready");
            }
        }

        runtime.Tick({0.0, 0});

        std::vector<spectre::es2025::PromiseModule::SettledPromise> settled;
        promiseModule.DrainSettled(settled);
        for (const auto &entry: settled) {
            std::cout << "  [" << entry.label.data() << "] state=" << stateName(entry.state)
                    << " value=" << entry.value.ToString()
                    << " diag=" << entry.diagnostics << std::endl;
        }
        settled.clear();

        spectre::es2025::PromiseModule::Handle rejectRoot = 0;
        spectre::es2025::PromiseModule::Handle recovered = 0;
        DemoPromisePayload rejectPayload{1, false};
        if (promiseModule.CreatePromise(rejectRoot, {"promise.reject"}) == spectre::StatusCode::Ok) {
            spectre::es2025::PromiseModule::ReactionOptions rejectOptions{};
            rejectOptions.onRejected = DemoPromiseRejectCallback;
            rejectOptions.userData = &rejectPayload;
            rejectOptions.label = "promise.recover";
            if (promiseModule.Then(rejectRoot, recovered, rejectOptions) == spectre::StatusCode::Ok) {
                promiseModule.Reject(rejectRoot, "demo-error", spectre::es2025::Value::String("payload"));
            }
        }

        runtime.Tick({0.0, 1});
        promiseModule.DrainSettled(settled);
        for (const auto &entry: settled) {
            std::cout << "  [" << entry.label.data() << "] state=" << stateName(entry.state)
                    << " value=" << entry.value.ToString()
                    << " diag=" << entry.diagnostics << std::endl;
        }
        settled.clear();

        spectre::es2025::PromiseModule::Handle cancelled = 0;
        if (promiseModule.CreatePromise(cancelled, {"promise.cancel"}) == spectre::StatusCode::Ok) {
            promiseModule.Cancel(cancelled);
        }

        runtime.Tick({0.0, 2});
        promiseModule.DrainSettled(settled);
        for (const auto &entry: settled) {
            std::cout << "  [" << entry.label.data() << "] state=" << stateName(entry.state)
                    << " value=" << entry.value.ToString()
                    << " diag=" << entry.diagnostics << std::endl;
        }

        const auto &metrics = promiseModule.GetMetrics();
        std::cout << "  metrics created=" << metrics.created
                << " resolved=" << metrics.resolved
                << " rejected=" << metrics.rejected
                << " cancelled=" << metrics.cancelled
                << " reactions=" << metrics.executedReactions << std::endl;

        if (resolveRoot != spectre::es2025::PromiseModule::kInvalidHandle) {
            promiseModule.Release(resolveRoot);
        }
        if (resolveScaled != spectre::es2025::PromiseModule::kInvalidHandle) {
            promiseModule.Release(resolveScaled);
        }
        if (rejectRoot != spectre::es2025::PromiseModule::kInvalidHandle) {
            promiseModule.Release(rejectRoot);
        }
        if (recovered != spectre::es2025::PromiseModule::kInvalidHandle) {
            promiseModule.Release(recovered);
        }
        if (cancelled != spectre::es2025::PromiseModule::kInvalidHandle) {
            promiseModule.Release(cancelled);
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

    void DemonstrateDataViewModule(spectre::es2025::DataViewModule &dataViewModule,
                                   spectre::es2025::ArrayBufferModule &bufferModule) {
        std::cout << "\nDataView precision demo" << std::endl;
        spectre::es2025::ArrayBufferModule::Handle backing = 0;
        if (bufferModule.Create("dataview.backing", 64, backing) != spectre::StatusCode::Ok) {
            std::cout << "  backing allocation failed" << std::endl;
            return;
        }
        auto cleanup = [&]() {
            if (backing != 0) {
                bufferModule.Destroy(backing);
                backing = 0;
            }
        };

        spectre::es2025::DataViewModule::Handle view = 0;
        if (dataViewModule.Create(backing, 16, 32, "dataview.payload", view) != spectre::StatusCode::Ok) {
            std::cout << "  view creation failed" << std::endl;
            cleanup();
            return;
        }

        dataViewModule.SetUint32(view, 0, 0x11223344u, true);
        dataViewModule.SetUint32(view, 4, 0x8899aabbu, false);
        dataViewModule.SetFloat64(view, 8, 42.75, true);
        dataViewModule.SetBigUint64(view, 16, 0x0102030405060708ull, false);
        dataViewModule.SetInt8(view, 24, -12);

        std::uint32_t little = 0;
        std::uint32_t big = 0;
        double energy = 0.0;
        std::uint64_t signature = 0;
        std::int8_t bias = 0;
        dataViewModule.GetUint32(view, 0, true, little);
        dataViewModule.GetUint32(view, 4, false, big);
        dataViewModule.GetFloat64(view, 8, true, energy);
        dataViewModule.GetBigUint64(view, 16, false, signature);
        dataViewModule.GetInt8(view, 24, bias);

        std::ostringstream scalarLine;
        scalarLine << std::hex << std::setfill('0');
        scalarLine << "  little=0x" << std::setw(8) << little
                   << " big=0x" << std::setw(8) << big
                   << " signature=0x" << std::setw(16) << signature;
        std::cout << scalarLine.str() << std::endl;

        std::ostringstream floatLine;
        floatLine.setf(std::ios::fixed);
        floatLine << std::setprecision(2);
        floatLine << "  energy=" << energy << " bias=" << static_cast<int>(bias);
        std::cout << floatLine.str() << std::endl;

        std::array<std::uint8_t, 16> bytes{};
        if (bufferModule.CopyOut(backing, 16, bytes.data(), bytes.size()) == spectre::StatusCode::Ok) {
            std::ostringstream raw;
            raw << std::hex << std::setfill('0') << "  raw:";
            for (auto byte: bytes) {
                raw << ' ' << std::setw(2) << static_cast<int>(byte);
            }
            std::cout << raw.str() << std::endl;
        }

        spectre::es2025::DataViewModule::Snapshot snapshot{};
        if (dataViewModule.Describe(view, snapshot) == spectre::StatusCode::Ok) {
            std::cout << "  snapshot offset=" << snapshot.byteOffset
                      << " length=" << snapshot.byteLength
                      << " attached=" << (snapshot.attached ? "true" : "false") << std::endl;
        }

        dataViewModule.Detach(view);
        std::uint8_t guard = 0;
        auto status = dataViewModule.GetUint8(view, 0, guard);
        std::cout << "  post-detach read => "
                  << (status == spectre::StatusCode::Ok ? "ok" : "blocked") << std::endl;

        dataViewModule.Destroy(view);
        const auto &metrics = dataViewModule.GetMetrics();
        std::cout << "  DataView metrics => reads=" << metrics.readOps
                  << " writes=" << metrics.writeOps
                  << " hot=" << metrics.hotViews << std::endl;
        cleanup();
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

void DemonstrateRegExpModule(spectre::es2025::RegExpModule &regexpModule) {
    std::cout << "\nRegExp pipeline demo" << std::endl;
    spectre::es2025::RegExpModule::Handle pattern = 0;
    if (regexpModule.Compile("([A-Z]+)[0-9]+", "gi", pattern) != spectre::StatusCode::Ok) {
        std::cout << "  failed to compile pattern" << std::endl;
        return;
    }
    spectre::es2025::RegExpModule::MatchResult result;
    regexpModule.Exec(pattern, "NODE32 CORE91 XR77", std::numeric_limits<std::size_t>::max(), result);
    if (result.matched) {
        std::cout << "  match => span [" << result.index << ", " << (result.index + result.length) << "]" << std::endl;
        if (result.groups.size() > 1 && result.groups[1].begin != std::numeric_limits<std::uint32_t>::max()) {
            std::cout << "  capture[1] => span [" << result.groups[1].begin
                      << ", " << result.groups[1].end << "]" << std::endl;
        }
    } else {
        std::cout << "  no match" << std::endl;
    }
    bool hasNext = false;
    regexpModule.Test(pattern, "NODE32 CORE91 XR77", hasNext);
    std::cout << "  test() => " << (hasNext ? "next" : "none")
              << " lastIndex=" << regexpModule.LastIndex(pattern) << std::endl;
    std::string replaced;
    regexpModule.Replace(pattern, "NODE32 CORE91 XR77", "X", replaced, false);
    std::cout << "  replace => \"" << replaced << "\"" << std::endl;

    spectre::es2025::RegExpModule::Handle split = 0;
    if (regexpModule.Compile("\\s+", "", split) == spectre::StatusCode::Ok) {
        std::vector<std::string> tokens;
        regexpModule.Split(split, "alpha beta   gamma", 0, tokens);
        std::cout << "  split => ";
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            std::cout << (i == 0 ? "" : " | ") << tokens[i];
        }
        std::cout << std::endl;
        regexpModule.Destroy(split);
    }
    regexpModule.Destroy(pattern);
}

void DemonstrateTypedArrayModule(spectre::es2025::TypedArrayModule &typedArrayModule) {
    using Module = spectre::es2025::TypedArrayModule;
    std::cout << "\nTypedArray staging demo" << std::endl;
    Module::Handle buffer = 0;
    if (typedArrayModule.Create(Module::ElementType::Float32, 16, "audio.frame", buffer) != spectre::StatusCode::Ok) {
        std::cout << "  float32 buffer unavailable" << std::endl;
        return;
    }
    typedArrayModule.Fill(buffer, 0.0);
    typedArrayModule.Set(buffer, 0, 0.25);
    typedArrayModule.Set(buffer, 1, 0.5);
    typedArrayModule.CopyWithin(buffer, 8, 0, 8);
    std::vector<double> samples;
    typedArrayModule.ToVector(buffer, samples);
    std::cout << "  samples =>";
    for (std::size_t i = 0; i < samples.size() && i < 8; ++i) {
        std::cout << ' ' << samples[i];
    }
    std::cout << std::endl;

    Module::Handle view = 0;
    if (typedArrayModule.Subarray(buffer, 8, 12, "audio.view", view) == spectre::StatusCode::Ok) {
        typedArrayModule.Set(view, 2, -0.5);
        double value = 0.0;
        typedArrayModule.Get(buffer, 10, value);
        std::cout << "  shared view value => " << value << std::endl;
        typedArrayModule.Destroy(view);
    }

    Module::Handle big = 0;
    if (typedArrayModule.Create(Module::ElementType::BigInt64, 4, "id.buffer", big) == spectre::StatusCode::Ok) {
        typedArrayModule.FillBigInt(big, 42);
        std::vector<std::int64_t> ids;
        typedArrayModule.ToBigIntVector(big, ids);
        std::cout << "  bigint ids =>";
        for (auto id : ids) {
            std::cout << ' ' << id;
        }
        std::cout << std::endl;
        typedArrayModule.Destroy(big);
    }

    const auto &metrics = typedArrayModule.GetMetrics();
    std::cout << "  views=" << metrics.activeViews
              << " reads=" << metrics.readOps
              << " writes=" << metrics.writeOps
              << " subarrays=" << metrics.subarrayOps << std::endl;
    typedArrayModule.Destroy(buffer);
}

void DemonstrateStructuredClone(spectre::es2025::StructuredCloneModule &cloneModule,
                                spectre::es2025::ArrayBufferModule &arrayModule,
                                spectre::es2025::SharedArrayBufferModule &sharedModule,
                                spectre::es2025::TypedArrayModule &typedArrayModule) {
    std::cout << "\nStructured clone showcase" << std::endl;

    spectre::es2025::TypedArrayModule::Handle typedHandle = 0;
    if (typedArrayModule.Create(spectre::es2025::TypedArrayModule::ElementType::Float32,
                                4, "clone.view", typedHandle) != spectre::StatusCode::Ok) {
        std::cout << "  typed array unavailable" << std::endl;
        return;
    }
    typedArrayModule.Set(typedHandle, 0, 2.5);
    typedArrayModule.Set(typedHandle, 1, -6.0);
    typedArrayModule.Set(typedHandle, 2, 9.25);
    typedArrayModule.Set(typedHandle, 3, 12.5);

    spectre::es2025::SharedArrayBufferModule::Handle sharedHandle = 0;
    if (sharedModule.CreateResizable("clone.shared", 96, 160, sharedHandle) != spectre::StatusCode::Ok) {
        std::cout << "  shared buffer unavailable" << std::endl;
        typedArrayModule.Destroy(typedHandle);
        return;
    }
    std::vector<std::uint8_t> sharedBytes(96);
    for (std::size_t i = 0; i < sharedBytes.size(); ++i) {
        sharedBytes[i] = static_cast<std::uint8_t>((i * 7) & 0xffu);
    }
    sharedModule.CopyIn(sharedHandle, 0, sharedBytes.data(), sharedBytes.size());

    spectre::es2025::ArrayBufferModule::Handle bufferHandle = 0;
    if (arrayModule.Create("clone.buffer", 48, bufferHandle) != spectre::StatusCode::Ok) {
        std::cout << "  array buffer unavailable" << std::endl;
        sharedModule.Destroy(sharedHandle);
        typedArrayModule.Destroy(typedHandle);
        return;
    }
    std::vector<std::uint8_t> bufferBytes(48);
    for (std::size_t i = 0; i < bufferBytes.size(); ++i) {
        bufferBytes[i] = static_cast<std::uint8_t>(0x40u + i);
    }
    arrayModule.CopyIn(bufferHandle, 0, bufferBytes.data(), bufferBytes.size());

    spectre::es2025::StructuredCloneModule::Node root;
    root.kind = spectre::es2025::StructuredCloneModule::Node::Kind::Object;
    root.objectProperties.emplace_back("kind", spectre::es2025::StructuredCloneModule::Node::FromString("demo"));

    spectre::es2025::StructuredCloneModule::Node bufferNode;
    bufferNode.kind = spectre::es2025::StructuredCloneModule::Node::Kind::ArrayBuffer;
    bufferNode.arrayBuffer = bufferHandle;
    bufferNode.transfer = true;
    bufferNode.label = "clone.buffer";
    root.objectProperties.emplace_back("buffer", bufferNode);

    spectre::es2025::StructuredCloneModule::Node sharedNode;
    sharedNode.kind = spectre::es2025::StructuredCloneModule::Node::Kind::SharedArrayBuffer;
    sharedNode.sharedBuffer = sharedHandle;
    sharedNode.label = "clone.shared";
    root.objectProperties.emplace_back("shared", sharedNode);

    spectre::es2025::StructuredCloneModule::Node typedNode;
    typedNode.kind = spectre::es2025::StructuredCloneModule::Node::Kind::TypedArray;
    typedNode.typedArray.handle = typedHandle;
    typedNode.typedArray.elementType = spectre::es2025::TypedArrayModule::ElementType::Float32;
    typedNode.typedArray.length = 4;
    typedNode.typedArray.byteOffset = 0;
    typedNode.typedArray.copyBuffer = true;
    typedNode.typedArray.label = "clone.view";
    root.objectProperties.emplace_back("vector", typedNode);

    spectre::es2025::StructuredCloneModule::CloneOptions options;
    options.enableTransfer = true;
    options.shareSharedBuffers = true;
    options.copyTypedArrayBuffer = true;
    options.transferList.push_back(bufferHandle);

    spectre::es2025::StructuredCloneModule::Node cloned;
    if (cloneModule.Clone(root, cloned, options) != spectre::StatusCode::Ok) {
        std::cout << "  clone failed" << std::endl;
        typedArrayModule.Destroy(typedHandle);
        sharedModule.Destroy(sharedHandle);
        arrayModule.Destroy(bufferHandle);
        return;
    }

    auto findProperty = [](const spectre::es2025::StructuredCloneModule::Node &node,
                           const std::string &key) -> const spectre::es2025::StructuredCloneModule::Node * {
        for (const auto &prop: node.objectProperties) {
            if (prop.first == key) {
                return &prop.second;
            }
        }
        return nullptr;
    };

    if (const auto *cloneBuffer = findProperty(cloned, "buffer")) {
        std::vector<std::uint8_t> verify(bufferBytes.size());
        arrayModule.CopyOut(cloneBuffer->arrayBuffer, 0, verify.data(), verify.size());
        std::cout << "  cloned buffer checksum => "
                << std::accumulate(verify.begin(), verify.end(), 0u) << std::endl;
        std::cout << "  transfer detached => " << (arrayModule.Detached(bufferHandle) ? "true" : "false") << std::endl;
        arrayModule.Destroy(cloneBuffer->arrayBuffer);
    }

    if (const auto *cloneShared = findProperty(cloned, "shared")) {
        auto originalCount = sharedModule.RefCount(sharedHandle);
        auto cloneCount = sharedModule.RefCount(cloneShared->sharedBuffer);
        std::cout << "  shared refcount => " << originalCount << " (clone=" << cloneCount << ")" << std::endl;
        sharedModule.Destroy(cloneShared->sharedBuffer);
    }

    if (const auto *cloneTyped = findProperty(cloned, "vector")) {
        std::vector<double> values;
        typedArrayModule.ToVector(cloneTyped->typedArray.handle, values);
        if (!values.empty()) {
            std::cout << "  cloned typed first => " << values.front() << std::endl;
        }
        typedArrayModule.Destroy(cloneTyped->typedArray.handle);
    }

    std::vector<std::uint8_t> blob;
    auto serializeStatus = cloneModule.Serialize(cloned, blob);
    if (serializeStatus == spectre::StatusCode::Ok) {
        std::cout << "  serialized payload => " << blob.size() << " bytes" << std::endl;
    }

    spectre::es2025::StructuredCloneModule::Node restored;
    if (serializeStatus == spectre::StatusCode::Ok &&
        cloneModule.Deserialize(blob.data(), blob.size(), restored) == spectre::StatusCode::Ok) {
        if (const auto *restoredBuffer = findProperty(restored, "buffer")) {
            arrayModule.Destroy(restoredBuffer->arrayBuffer);
        }
        if (const auto *restoredShared = findProperty(restored, "shared")) {
            sharedModule.Destroy(restoredShared->sharedBuffer);
        }
        if (const auto *restoredTyped = findProperty(restored, "vector")) {
            typedArrayModule.Destroy(restoredTyped->typedArray.handle);
        }
    }

    const auto &metrics = cloneModule.GetMetrics();
    std::cout << "  metrics cloneCalls=" << metrics.cloneCalls
            << " buffers=" << metrics.bufferCopies
            << " shared=" << metrics.sharedShares
            << " typed=" << metrics.typedArrayCopies
            << " serialized=" << metrics.serializedBytes
            << " deserialized=" << metrics.deserializedBytes << std::endl;

    typedArrayModule.Destroy(typedHandle);
    sharedModule.Destroy(sharedHandle);
    arrayModule.Destroy(bufferHandle);
}

void DemonstrateIntl(spectre::es2025::IntlModule &intl) {
    std::cout << "Intl formatting demo" << std::endl;

    spectre::es2025::IntlModule::NumberFormatOptions usdOptions;
    usdOptions.style = spectre::es2025::IntlModule::NumberStyle::Currency;
    usdOptions.currency = "USD";
    usdOptions.minimumFractionDigits = 2;
    usdOptions.maximumFractionDigits = 2;

    auto usd = intl.FormatNumber("en-US", 9876543.21, usdOptions);
    if (usd.status == spectre::StatusCode::Ok) {
        std::cout << "  en-US currency => " << usd.value << std::endl;
    } else {
        std::cout << "  en-US currency failed => " << usd.diagnostics << std::endl;
    }

    spectre::es2025::IntlModule::LocaleHandle ruHandle = spectre::es2025::IntlModule::kInvalidLocale;
    auto ensureStatus = intl.EnsureLocale("ru-RU", ruHandle);
    if (ensureStatus != spectre::StatusCode::Ok) {
        std::cout << "  ru-RU ensure failed" << std::endl;
        return;
    }

    spectre::es2025::IntlModule::NumberFormatOptions rubOptions;
    rubOptions.style = spectre::es2025::IntlModule::NumberStyle::Currency;
    rubOptions.currency = "RUB";
    rubOptions.minimumFractionDigits = 2;
    rubOptions.maximumFractionDigits = 2;
    auto rub = intl.FormatNumber(ruHandle, -54321.75, rubOptions);
    if (rub.status == spectre::StatusCode::Ok) {
        std::cout << "  ru-RU currency => " << rub.value << std::endl;
    } else {
        std::cout << "  ru-RU currency failed => " << rub.diagnostics << std::endl;
    }

    spectre::es2025::IntlModule::NumberFormatOptions percentOptions;
    percentOptions.style = spectre::es2025::IntlModule::NumberStyle::Percent;
    percentOptions.minimumFractionDigits = 1;
    percentOptions.maximumFractionDigits = 1;
    auto percent = intl.FormatNumber(ruHandle, 0.423, percentOptions);
    if (percent.status == spectre::StatusCode::Ok) {
        std::cout << "  ru-RU percent => " << percent.value << std::endl;
    }

    spectre::es2025::IntlModule::DateTimeFormatOptions dtOptions;
    dtOptions.dateStyle = spectre::es2025::IntlModule::DateStyle::Long;
    dtOptions.timeStyle = spectre::es2025::IntlModule::TimeStyle::Medium;
    dtOptions.timeZone = spectre::es2025::IntlModule::TimeZone::Utc;
    auto sample = std::chrono::system_clock::from_time_t(1717171717);
    auto dateTime = intl.FormatDateTime(ruHandle, sample, dtOptions);
    if (dateTime.status == spectre::StatusCode::Ok) {
        std::cout << "  ru-RU datetime => " << dateTime.value << std::endl;
    }

    spectre::es2025::IntlModule::ListFormatOptions ruListOptions;
    ruListOptions.type = spectre::es2025::IntlModule::ListType::Conjunction;
    ruListOptions.style = spectre::es2025::IntlModule::ListStyle::Long;
    auto ruList = intl.FormatList(ruHandle, {"yabloko", "banan", "malina"}, ruListOptions);
    if (ruList.status == spectre::StatusCode::Ok) {
        std::cout << "  ru-RU list => " << ruList.value << std::endl;
    }

    spectre::es2025::IntlModule::ListFormatOptions ukListOptions;
    ukListOptions.type = spectre::es2025::IntlModule::ListType::Disjunction;
    ukListOptions.style = spectre::es2025::IntlModule::ListStyle::Short;
    auto ukList = intl.FormatList("en-GB", {"alpha", "beta", "gamma"}, ukListOptions);
    if (ukList.status == spectre::StatusCode::Ok) {
        std::cout << "  en-GB options => " << ukList.value << std::endl;
    }

    const auto &metrics = intl.GetMetrics();
    std::cout << "  metrics locales=" << metrics.localesRegistered
              << " numberHits=" << metrics.numberFormatterHits
              << " dateHits=" << metrics.dateFormatterHits
              << " listHits=" << metrics.listFormatterHits
              << " cacheEvictions=" << metrics.cacheEvictions << std::endl;
}

void DemonstrateModuleLoader(spectre::es2025::ModuleLoaderModule &loader,
                             spectre::es2025::GlobalModule &globalModule) {
    std::cout << "\nModule loader graph demo" << std::endl;
    using Loader = spectre::es2025::ModuleLoaderModule;

    Loader::RegisterOptions options;
    options.overrideDependencies = true;
    options.dependencies.reserve(3);

    Loader::Handle configHandle = Loader::kInvalidHandle;
    auto status = loader.RegisterModule("demo.module.config", "return 'cfg';", configHandle, options);
    if (status != spectre::StatusCode::Ok) {
        std::cout << "  failed to register config" << std::endl;
        return;
    }

    Loader::Handle assetsHandle = Loader::kInvalidHandle;
    options.dependencies.clear();
    options.dependencies.push_back("demo.module.config");
    status = loader.RegisterModule("demo.module.assets", "return 'assets-v1';", assetsHandle, options);
    if (status != spectre::StatusCode::Ok) {
        std::cout << "  failed to register assets" << std::endl;
        return;
    }

    Loader::Handle entryHandle = Loader::kInvalidHandle;
    options.dependencies.clear();
    options.dependencies.push_back("demo.module.config");
    options.dependencies.push_back("demo.module.assets");
    status = loader.RegisterModule("demo.module.entry", "return 'entry';", entryHandle, options);
    if (status != spectre::StatusCode::Ok) {
        std::cout << "  failed to register entry" << std::endl;
        return;
    }

    auto evaluateAndLog = [&](Loader::Handle handle, std::string_view label) {
        auto evaluation = loader.Evaluate(handle);
        if (evaluation.status == spectre::StatusCode::Ok) {
            std::cout << "  " << label << " => " << evaluation.value << std::endl;
        } else {
            std::cout << "  " << label << " failed => " << evaluation.diagnostics << std::endl;
        }
    };

    evaluateAndLog(entryHandle, "entry initial");
    evaluateAndLog(entryHandle, "entry cached");

    auto snapshotEntry = loader.Snapshot(entryHandle);
    std::cout << "  entry version=" << snapshotEntry.version
              << " context=" << globalModule.DefaultContext() << std::endl;

    options.dependencies.clear();
    options.dependencies.push_back("demo.module.config");
    status = loader.RegisterModule("demo.module.assets", "return 'assets-v2';", assetsHandle, options);
    if (status == spectre::StatusCode::Ok) {
        evaluateAndLog(entryHandle, "entry updated");
    }

    const auto snapConfig = loader.Snapshot(configHandle);
    const auto snapAssets = loader.Snapshot(assetsHandle);
    snapshotEntry = loader.Snapshot(entryHandle);
    std::cout << "  versions config=" << snapConfig.version
              << " assets=" << snapAssets.version
              << " entry=" << snapshotEntry.version << std::endl;

    const auto &metrics = loader.GetMetrics();
    std::cout << "  metrics evals=" << metrics.evaluations
              << " cacheHits=" << metrics.cacheHits
              << " cacheMisses=" << metrics.cacheMisses
              << " resolver=" << metrics.resolverHits << "/" << metrics.resolverRequests
              << " graphDepth=" << metrics.maxGraphDepth << std::endl;
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

void DemonstrateReflectModule(spectre::es2025::ObjectModule &objectModule,
                              spectre::es2025::ReflectModule &reflectModule) {
    std::cout << "\nDemonstrating Reflect module" << std::endl;
    spectre::es2025::ObjectModule::Handle model = 0;
    if (objectModule.Create("demo.reflect.model", 0, model) != spectre::StatusCode::Ok) {
        std::cout << "  object allocation failed" << std::endl;
        return;
    }
    spectre::es2025::ObjectModule::PropertyDescriptor descriptor{};
    descriptor.value = spectre::es2025::ObjectModule::Value::FromInt(120);
    descriptor.enumerable = true;
    descriptor.configurable = false;
    descriptor.writable = true;
    if (reflectModule.DefineProperty(model, "hp", descriptor) != spectre::StatusCode::Ok) {
        std::cout << "  define property failed" << std::endl;
        objectModule.Destroy(model);
        return;
    }
    reflectModule.Set(model, "hp", spectre::es2025::ObjectModule::Value::FromInt(95));
    spectre::es2025::ObjectModule::Value hpValue;
    if (reflectModule.Get(model, "hp", hpValue) == spectre::StatusCode::Ok && hpValue.IsInt()) {
        std::cout << "  hp => " << hpValue.Int() << std::endl;
    }
    spectre::es2025::ObjectModule::PropertyDescriptor readBack{};
    if (reflectModule.GetOwnPropertyDescriptor(model, "hp", readBack) == spectre::StatusCode::Ok) {
        std::cout << "  descriptor => enumerable:" << (readBack.enumerable ? "true" : "false")
                  << " configurable:" << (readBack.configurable ? "true" : "false")
                  << " writable:" << (readBack.writable ? "true" : "false") << std::endl;
    }
    std::vector<std::string> keys;
    if (reflectModule.OwnKeys(model, keys) == spectre::StatusCode::Ok) {
        std::cout << "  keys:";
        for (const auto &key: keys) {
            std::cout << " " << key;
        }
        std::cout << std::endl;
    }
    bool deleted = false;
    auto deleteStatus = reflectModule.DeleteProperty(model, "hp", deleted);
    std::cout << "  delete hp => status:" << static_cast<int>(deleteStatus)
              << " removed:" << (deleted ? "true" : "false") << std::endl;
    auto preventStatus = reflectModule.PreventExtensions(model);
    std::cout << "  prevent extensions => status:" << static_cast<int>(preventStatus)
              << " extensible:" << (reflectModule.IsExtensible(model) ? "true" : "false") << std::endl;
    spectre::es2025::ObjectModule::PropertyDescriptor armorDescriptor{};
    armorDescriptor.value = spectre::es2025::ObjectModule::Value::FromInt(50);
    armorDescriptor.enumerable = true;
    armorDescriptor.configurable = true;
    armorDescriptor.writable = true;
    auto armorStatus = reflectModule.DefineProperty(model, "armor", armorDescriptor);
    std::cout << "  define armor status => " << static_cast<int>(armorStatus) << std::endl;
    auto prototype = reflectModule.GetPrototypeOf(model);
    std::cout << "  prototype handle => " << prototype << std::endl;
    objectModule.Destroy(model);
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



void DemonstrateSetModule(spectre::es2025::SetModule &setModule) {
    std::cout << "\nDemonstrating Set module" << std::endl;
    spectre::es2025::SetModule::Handle handle = 0;
    if (setModule.Create("demo.set", handle) != spectre::StatusCode::Ok) {
        std::cout << "  set unavailable" << std::endl;
        return;
    }
    using Value = spectre::es2025::SetModule::Value;
    setModule.Add(handle, Value::FromString("spectre-set"));
    setModule.Add(handle, Value::FromInt(21));
    setModule.Add(handle, Value::FromDouble(4.2));
    setModule.Add(handle, Value::FromInt(21));
    std::vector<Value> values;
    if (setModule.Values(handle, values) == spectre::StatusCode::Ok) {
        std::cout << "  values:";
        for (const auto &value: values) {
            if (value.IsString()) {
                std::cout << " " << value.String();
            } else if (value.IsInt()) {
                std::cout << " " << value.Int();
            } else if (value.IsDouble()) {
                std::cout << " " << value.Double();
            }
        }
        std::cout << std::endl;
    }
    bool removed = false;
    setModule.Delete(handle, Value::FromDouble(4.2), removed);
    std::cout << "  delete 4.2 => " << (removed ? "true" : "false") << std::endl;
    std::cout << "  size => " << setModule.Size(handle) << std::endl;
    setModule.Clear(handle);
    setModule.Destroy(handle);
}

void DemonstrateWeakSetModule(spectre::es2025::ObjectModule &objectModule,
                              spectre::es2025::WeakSetModule &weakSetModule) {
    std::cout << "\nDemonstrating WeakSet module" << std::endl;
    spectre::es2025::WeakSetModule::Handle handle = 0;
    if (weakSetModule.Create("demo.weakset", handle) != spectre::StatusCode::Ok) {
        std::cout << "  weak set unavailable" << std::endl;
        return;
    }
    spectre::es2025::ObjectModule::Handle keyA = 0;
    spectre::es2025::ObjectModule::Handle keyB = 0;
    if (objectModule.Create("demo.weakset.keyA", 0, keyA) != spectre::StatusCode::Ok ||
        objectModule.Create("demo.weakset.keyB", 0, keyB) != spectre::StatusCode::Ok) {
        std::cout << "  object handles unavailable" << std::endl;
        if (keyA != 0) {
            objectModule.Destroy(keyA);
        }
        if (keyB != 0) {
            objectModule.Destroy(keyB);
        }
        weakSetModule.Destroy(handle);
        return;
    }
    weakSetModule.Add(handle, keyA);
    weakSetModule.Add(handle, keyB);
    std::cout << "  size => " << weakSetModule.Size(handle) << std::endl;
    objectModule.Destroy(keyA);
    weakSetModule.Compact(handle);
    std::cout << "  size after compact => " << weakSetModule.Size(handle) << std::endl;
    bool removed = false;
    weakSetModule.Delete(handle, keyB, removed);
    std::cout << "  delete keyB => " << (removed ? "true" : "false") << std::endl;
    weakSetModule.Destroy(handle);
    objectModule.Destroy(keyB);
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

void DemonstrateWeakRefModule(spectre::es2025::ObjectModule &objectModule,
                              spectre::es2025::WeakRefModule &weakRefModule) {
    std::cout << "\nDemonstrating WeakRef module" << std::endl;
    spectre::es2025::ObjectModule::Handle hero = 0;
    spectre::es2025::ObjectModule::Handle loot = 0;
    if (objectModule.Create("demo.weakref.hero", 0, hero) != spectre::StatusCode::Ok ||
        objectModule.Create("demo.weakref.loot", 0, loot) != spectre::StatusCode::Ok) {
        std::cout << "  object allocation failed" << std::endl;
        if (hero != 0) {
            objectModule.Destroy(hero);
        }
        if (loot != 0) {
            objectModule.Destroy(loot);
        }
        return;
    }
    spectre::es2025::WeakRefModule::Handle heroRef = 0;
    spectre::es2025::WeakRefModule::Handle lootRef = 0;
    if (weakRefModule.Create(hero, heroRef) != spectre::StatusCode::Ok ||
        weakRefModule.Create(loot, lootRef) != spectre::StatusCode::Ok) {
        std::cout << "  weak refs unavailable" << std::endl;
        if (heroRef != 0) {
            weakRefModule.Destroy(heroRef);
        }
        if (lootRef != 0) {
            weakRefModule.Destroy(lootRef);
        }
        objectModule.Destroy(hero);
        objectModule.Destroy(loot);
        return;
    }
    std::cout << "  handles => hero:" << heroRef << " loot:" << lootRef << std::endl;
    spectre::es2025::ObjectModule::Handle derefHandle = 0;
    bool alive = false;
    if (weakRefModule.Deref(heroRef, derefHandle, alive) == spectre::StatusCode::Ok) {
        std::cout << "  hero alive => " << (alive ? "true" : "false") << " handle:" << derefHandle << std::endl;
    }
    objectModule.Destroy(loot);
    if (weakRefModule.Deref(lootRef, derefHandle, alive) == spectre::StatusCode::Ok) {
        std::cout << "  loot alive after destroy => " << (alive ? "true" : "false") << std::endl;
    }
    weakRefModule.Refresh(lootRef, hero);
    if (weakRefModule.Deref(lootRef, derefHandle, alive) == spectre::StatusCode::Ok) {
        std::cout << "  loot ref rebound => " << (alive ? "true" : "false") << " handle:" << derefHandle << std::endl;
    }
    weakRefModule.Destroy(lootRef);
    weakRefModule.Destroy(heroRef);
    weakRefModule.Compact();
    const auto &metrics = weakRefModule.GetMetrics();
    std::cout << "  metrics => live:" << weakRefModule.LiveCount()
              << " derefOps:" << metrics.derefOps
              << " cleared:" << metrics.clearedRefs << std::endl;
    objectModule.Destroy(hero);
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
void DemonstrateTemporalModule(spectre::es2025::TemporalModule &temporalModule) {
    std::cout << "\nTemporal module timeline" << std::endl;
    const auto canonical = temporalModule.CanonicalEpoch();
    std::cout << "  canonical epoch ns " << temporalModule.EpochNanoseconds(canonical, -1) << std::endl;

    spectre::es2025::TemporalModule::PlainDateTime meetup{};
    meetup.year = 2025;
    meetup.month = 3;
    meetup.day = 9;
    meetup.hour = 18;
    meetup.minute = 45;
    meetup.second = 0;
    meetup.millisecond = 0;
    meetup.microsecond = 0;
    meetup.nanosecond = 0;

    spectre::es2025::TemporalModule::Handle meetupHandle = 0;
    if (temporalModule.CreateInstant(meetup, 60, "meetup", meetupHandle) != spectre::StatusCode::Ok) {
        std::cout << "  failed to create meetup instant" << std::endl;
        return;
    }

    spectre::es2025::TemporalModule::PlainDateTime meetupLocal{};
    temporalModule.ToPlainDateTime(meetupHandle, 60, meetupLocal);
    std::cout << "  meetup local time "
              << meetupLocal.year << "-" << meetupLocal.month << "-" << meetupLocal.day << " "
              << meetupLocal.hour << ":" << meetupLocal.minute << std::endl;

    auto reminderDuration = spectre::es2025::TemporalModule::Duration::FromComponents(0, -1, -15, 0, 0, 0, 0);
    spectre::es2025::TemporalModule::Handle reminderHandle = 0;
    if (temporalModule.AddDuration(meetupHandle, reminderDuration, "meetup.reminder", reminderHandle)
        == spectre::StatusCode::Ok) {
        spectre::es2025::TemporalModule::PlainDateTime reminder{};
        temporalModule.ToPlainDateTime(reminderHandle, 60, reminder);
        std::cout << "  reminder time " << reminder.hour << ":" << reminder.minute << std::endl;
        temporalModule.Destroy(reminderHandle);
    }

    auto fineTune = spectre::es2025::TemporalModule::Duration::FromComponents(0, 0, 5, 0, 0, 0, 0);
    temporalModule.AddDurationInPlace(meetupHandle, fineTune);
    spectre::es2025::TemporalModule::Handle roundedHandle = 0;
    if (temporalModule.Round(meetupHandle,
                              15,
                              spectre::es2025::TemporalModule::Unit::Minute,
                              spectre::es2025::TemporalModule::RoundingMode::HalfExpand,
                              "meetup.rounded",
                              roundedHandle) == spectre::StatusCode::Ok) {
        spectre::es2025::TemporalModule::PlainDateTime rounded{};
        temporalModule.ToPlainDateTime(roundedHandle, 60, rounded);
        std::cout << "  rounded start " << rounded.hour << ":" << rounded.minute << std::endl;
        temporalModule.Destroy(roundedHandle);
    }

    temporalModule.Destroy(meetupHandle);
}

void DemonstrateShadowRealmModule(spectre::es2025::ShadowRealmModule &shadowModule,
                                  spectre::es2025::GlobalModule &globalModule) {
    (void) globalModule;
    std::cout << "\nShadowRealm isolation demo" << std::endl;
    spectre::es2025::ShadowRealmModule::Handle hostRealm = 0;
    spectre::es2025::ShadowRealmModule::Handle toolsRealm = 0;
    if (shadowModule.Create("shadow.host", hostRealm) != spectre::StatusCode::Ok ||
        shadowModule.Create("shadow.tools", toolsRealm) != spectre::StatusCode::Ok) {
        std::cout << "  failed to create shadow realms" << std::endl;
        return;
    }

    std::string value;
    std::string diagnostics;
    if (shadowModule.Evaluate(hostRealm,
                              "return globalThis ? \"shadow-host\" : \"shadow\";",
                              value,
                              diagnostics,
                              "shadow.host.init") == spectre::StatusCode::Ok) {
        std::cout << "  host realm => " << value << std::endl;
    }

    shadowModule.ExportValue(hostRealm, "apiVersion", spectre::es2025::Value::String("v1"));
    spectre::es2025::Value imported;
    if (shadowModule.ImportValue(toolsRealm, hostRealm, "apiVersion", imported) == spectre::StatusCode::Ok) {
        std::cout << "  tools realm imports apiVersion=" << imported.ToString() << std::endl;
    }
    shadowModule.Destroy(toolsRealm);
    shadowModule.Destroy(hostRealm);
}

void DemonstrateJsonModule(spectre::es2025::JsonModule &jsonModule) {
    std::cout << "\nJSON module showcase" << std::endl;

    spectre::es2025::JsonModule::Document doc;
    std::string diagnostics;
    const std::string payload =
        R"({"level":7,"label":"spectre","enabled":true,"tags":["engine","runtime","json"],"limits":{"cpu":0.5,"gpu":0.25}})";
    auto status = jsonModule.Parse(payload, doc, diagnostics);
    if (status != spectre::StatusCode::Ok) {
        std::cout << "  parse failed => " << diagnostics << std::endl;
        return;
    }
    const auto &root = doc.nodes[doc.root];
    auto summarize = [&](std::uint32_t index) -> std::string {
        const auto &node = doc.nodes[index];
        switch (node.kind) {
            case spectre::es2025::JsonModule::NodeKind::Null:
                return "null";
            case spectre::es2025::JsonModule::NodeKind::Boolean:
                return node.boolValue ? "true" : "false";
            case spectre::es2025::JsonModule::NodeKind::Number: {
                std::ostringstream stream;
                stream.setf(std::ios::fixed);
                stream.precision(3);
                stream << node.numberValue;
                return stream.str();
            }
            case spectre::es2025::JsonModule::NodeKind::String:
                return std::string(doc.GetString(node.stringRef));
            case spectre::es2025::JsonModule::NodeKind::Array: {
                std::ostringstream stream;
                stream << "array[" << node.span.count << "]";
                return stream.str();
            }
            case spectre::es2025::JsonModule::NodeKind::Object: {
                std::ostringstream stream;
                stream << "object{" << node.span.count << "}";
                return stream.str();
            }
        }
        return {};
    };

    if (root.kind == spectre::es2025::JsonModule::NodeKind::Object) {
        for (std::uint32_t i = 0; i < root.span.count; ++i) {
            const auto &prop = doc.properties[root.span.offset + i];
            auto key = doc.GetString(prop.key);
            auto summary = summarize(prop.valueIndex);
            std::cout << "  " << key << " => " << summary << std::endl;
        }
    }

    spectre::es2025::JsonModule::StringifyOptions pretty;
    pretty.pretty = true;
    pretty.indentWidth = 2;
    std::string prettyText;
    if (jsonModule.Stringify(doc, prettyText, &pretty) == spectre::StatusCode::Ok) {
        std::cout << "  pretty =>\n" << prettyText << std::endl;
    }

    spectre::es2025::JsonModule::ParseOptions permissive;
    permissive.allowComments = true;
    permissive.allowTrailingCommas = true;
    permissive.maxDepth = 32;
    permissive.maxNodes = 256;
    spectre::es2025::JsonModule::Document annotated;
    const std::string annotatedPayload = "{/*cache*/\"title\":\"caf\\u00e9\",\"size\":128,}";
    status = jsonModule.Parse(annotatedPayload, annotated, diagnostics, &permissive);
    if (status == spectre::StatusCode::Ok) {
        spectre::es2025::JsonModule::StringifyOptions ascii;
        ascii.asciiOnly = true;
        std::string asciiText;
        if (jsonModule.Stringify(annotated, asciiText, &ascii) == spectre::StatusCode::Ok) {
            std::cout << "  ascii => " << asciiText << std::endl;
        }
    } else {
        std::cout << "  permissive parse failed => " << diagnostics << std::endl;
    }

    const auto &metrics = jsonModule.GetMetrics();
    std::cout << "  metrics parseCalls=" << metrics.parseCalls
              << " stringifyCalls=" << metrics.stringifyCalls
              << " peakNodes=" << metrics.peakNodeCount
              << " peakStringBytes=" << metrics.peakStringArena << std::endl;
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

    auto *reflectModulePtr = environment.FindModule("Reflect");
    auto *reflectModule = dynamic_cast<es2025::ReflectModule *>(reflectModulePtr);
    if (!reflectModule) {
        std::cerr << "Reflect module unavailable" << std::endl;
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

    auto *setModulePtr = environment.FindModule("Set");
    auto *setModule = dynamic_cast<es2025::SetModule *>(setModulePtr);
    if (!setModule) {
        std::cerr << "Set module unavailable" << std::endl;
        return 1;
    }

    auto *weakMapModulePtr = environment.FindModule("WeakMap");
    auto *weakMapModule = dynamic_cast<es2025::WeakMapModule *>(weakMapModulePtr);
    if (!weakMapModule) {
        std::cerr << "WeakMap module unavailable" << std::endl;
        return 1;
    }

    auto *weakRefModulePtr = environment.FindModule("WeakRef");
    auto *weakRefModule = dynamic_cast<es2025::WeakRefModule *>(weakRefModulePtr);
    if (!weakRefModule) {
        std::cerr << "WeakRef module unavailable" << std::endl;
        return 1;
    }

    auto *weakSetModulePtr = environment.FindModule("WeakSet");
    auto *weakSetModule = dynamic_cast<es2025::WeakSetModule *>(weakSetModulePtr);
    if (!weakSetModule) {
        std::cerr << "WeakSet module unavailable" << std::endl;
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

    auto *moduleLoaderPtr = environment.FindModule("ModuleLoader");
    auto *moduleLoader = dynamic_cast<es2025::ModuleLoaderModule *>(moduleLoaderPtr);
    if (!moduleLoader) {
        std::cerr << "ModuleLoader module unavailable" << std::endl;
        return 1;
    }

    auto *asyncFunctionModulePtr = environment.FindModule("AsyncFunction");
    auto *asyncFunctionModule = dynamic_cast<es2025::AsyncFunctionModule *>(asyncFunctionModulePtr);
    if (!asyncFunctionModule) {
        std::cerr << "AsyncFunction module unavailable" << std::endl;
        return 1;
    }

    auto *asyncIteratorModulePtr = environment.FindModule("AsyncIterator");
    auto *asyncIteratorModule = dynamic_cast<es2025::AsyncIteratorModule *>(asyncIteratorModulePtr);
    if (!asyncIteratorModule) {
        std::cerr << "AsyncIterator module unavailable" << std::endl;
        return 1;
    }

    auto *promiseModulePtr = environment.FindModule("Promise");
    auto *promiseModule = dynamic_cast<es2025::PromiseModule *>(promiseModulePtr);
    if (!promiseModule) {
        std::cerr << "Promise module unavailable" << std::endl;
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
    auto *dataViewModulePtr = environment.FindModule("DataView");
    auto *dataViewModule = dynamic_cast<es2025::DataViewModule *>(dataViewModulePtr);
    if (!dataViewModule) {
        std::cerr << "DataView module unavailable" << std::endl;
        return 1;
    }
    auto *sharedArrayModulePtr = environment.FindModule("SharedArrayBuffer");
    auto *sharedArrayModule = dynamic_cast<es2025::SharedArrayBufferModule *>(sharedArrayModulePtr);
    if (!sharedArrayModule) {
        std::cerr << "SharedArrayBuffer module unavailable" << std::endl;
        return 1;
    }

    auto *structuredCloneModulePtr = environment.FindModule("StructuredClone");
    auto *structuredCloneModule = dynamic_cast<es2025::StructuredCloneModule *>(structuredCloneModulePtr);
    if (!structuredCloneModule) {
        std::cerr << "StructuredClone module unavailable" << std::endl;
        return 1;
    }

    auto *jsonModulePtr = environment.FindModule("JSON");
    auto *jsonModule = dynamic_cast<es2025::JsonModule *>(jsonModulePtr);
    if (!jsonModule) {
        std::cerr << "JSON module unavailable" << std::endl;
        return 1;
    }
    auto *intlModulePtr = environment.FindModule("Intl");
    auto *intlModule = dynamic_cast<es2025::IntlModule *>(intlModulePtr);
    if (!intlModule) {
        std::cerr << "Intl module unavailable" << std::endl;
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

    auto *temporalModulePtr = environment.FindModule("Temporal");
    auto *temporalModule = dynamic_cast<es2025::TemporalModule *>(temporalModulePtr);
    if (!temporalModule) {
        std::cerr << "Temporal module unavailable" << std::endl;
        return 1;
    }

    auto *shadowModulePtr = environment.FindModule("ShadowRealm");
    auto *shadowModule = dynamic_cast<es2025::ShadowRealmModule *>(shadowModulePtr);
    if (!shadowModule) {
        std::cerr << "ShadowRealm module unavailable" << std::endl;
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

    auto *regexpModulePtr = environment.FindModule("RegExp");
    auto *regexpModule = dynamic_cast<es2025::RegExpModule *>(regexpModulePtr);
    if (!regexpModule) {
        std::cerr << "RegExp module unavailable" << std::endl;
        return 1;
    }

    auto *typedArrayModulePtr = environment.FindModule("TypedArray");
    auto *typedArrayModule = dynamic_cast<es2025::TypedArrayModule *>(typedArrayModulePtr);
    if (!typedArrayModule) {
        std::cerr << "TypedArray module unavailable" << std::endl;
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
    DemonstrateModuleLoader(*moduleLoader, *globalModule);

    ShowcaseSymbols(*symbolModule);
    DemonstrateIteratorModule(*iteratorModule);
    DemonstrateGeneratorModule(*generatorModule, *iteratorModule);
    DemonstrateAsyncFunctionModule(*asyncFunctionModule, *runtime);
    DemonstrateAsyncIteratorModule(*asyncIteratorModule, *runtime);
    DemonstratePromiseModule(*promiseModule, *runtime);
    DemonstrateArrayModule(*arrayModule);
    DemonstrateArrayBufferModule(*arrayBufferModule);
    DemonstrateDataViewModule(*dataViewModule, *arrayBufferModule);
    DemonstrateTypedArrayModule(*typedArrayModule);
    DemonstrateStructuredClone(*structuredCloneModule, *arrayBufferModule, *sharedArrayModule, *typedArrayModule);
    DemonstrateJsonModule(*jsonModule);
    DemonstrateIntl(*intlModule);
    DemonstrateAtomicsModule(*atomicsModule);
    DemonstrateBooleanModule(*booleanModule);
    DemonstrateStringModule(*stringModule);
    DemonstrateRegExpModule(*regexpModule);
    DemonstrateMathModule(*mathModule);
    DemonstrateTemporalModule(*temporalModule);
    DemonstrateDateModule(*dateModule);
    DemonstrateNumberModule(*numberModule);
    DemonstrateBigIntModule(*bigintModule);
    DemonstrateObjectModule(*objectModule);
    DemonstrateReflectModule(*objectModule, *reflectModule);
    DemonstrateProxyModule(*objectModule, *proxyModule);
    DemonstrateShadowRealmModule(*shadowModule, *globalModule);
    DemonstrateMapModule(*mapModule);
    DemonstrateSetModule(*setModule);
    DemonstrateWeakMapModule(*objectModule, *weakMapModule);
    DemonstrateWeakRefModule(*objectModule, *weakRefModule);
    DemonstrateWeakSetModule(*objectModule, *weakSetModule);

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























