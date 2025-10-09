#include <iostream>
#include <limits>
#include <string>
#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>
#include <cmath>

#include "spectre/config.h"
#include "spectre/context.h"
#include "spectre/runtime.h"
#include "spectre/status.h"
#include "spectre/subsystems.h"
#include "spectre/es2025/environment.h"
#include "spectre/es2025/modules/global_module.h"
#include "spectre/es2025/modules/object_module.h"
#include "spectre/es2025/modules/proxy_module.h"
#include "spectre/es2025/modules/map_module.h"
#include "spectre/es2025/modules/set_module.h"
#include "spectre/es2025/modules/weak_set_module.h"
#include "spectre/es2025/modules/weak_map_module.h"
#include "spectre/es2025/modules/error_module.h"
#include "spectre/es2025/modules/function_module.h"
#include "spectre/es2025/modules/async_function_module.h"
#include "spectre/es2025/modules/async_iterator_module.h"
#include "spectre/es2025/modules/atomics_module.h"
#include "spectre/es2025/modules/boolean_module.h"
#include "spectre/es2025/modules/array_module.h"
#include "spectre/es2025/modules/array_buffer_module.h"
#include "spectre/es2025/modules/shared_array_buffer_module.h"
#include "spectre/es2025/modules/iterator_module.h"
#include "spectre/es2025/modules/generator_module.h"
#include "spectre/es2025/modules/promise_module.h"
#include "spectre/es2025/modules/math_module.h"
#include "spectre/es2025/modules/number_module.h"
#include "spectre/es2025/modules/bigint_module.h"
#include "spectre/es2025/modules/date_module.h"
#include "spectre/es2025/modules/string_module.h"
#include "spectre/es2025/modules/regexp_module.h"
#include "spectre/es2025/modules/typed_array_module.h"
#include "spectre/es2025/modules/structured_clone_module.h"
#include "spectre/es2025/modules/symbol_module.h"
#include "spectre/es2025/modules/reflect_module.h"
#include "spectre/es2025/modules/weak_ref_module.h"
#include "spectre/es2025/modules/shadow_realm_module.h"
#include "spectre/es2025/modules/temporal_module.h"

namespace {
    using spectre::RuntimeConfig;
    using spectre::RuntimeMode;
    using spectre::SpectreContext;
    using spectre::SpectreRuntime;
    using spectre::StatusCode;

    bool ExpectTrue(bool condition, const std::string &message) {
        if (!condition) {
            std::cout << "    assertion failed: " << message << std::endl;
        }
        return condition;
    }

    bool ExpectStatus(StatusCode status, StatusCode expected, const std::string &message) {
        if (status != expected) {
            std::cout << "    status mismatch: " << message << std::endl;
            std::cout << "    expected " << static_cast<int>(expected)
                    << " got " << static_cast<int>(status) << std::endl;
            return false;
        }
        return true;
    }

    RuntimeConfig MakeConfig(RuntimeMode mode) {
        auto config = spectre::MakeDefaultConfig();
        config.mode = mode;
        return config;
    }

    StatusCode AddCallback(const std::vector<std::string> &args, std::string &outResult, void *) {
        long total = 0;
        for (const auto &value: args) {
            try {
                total += std::stol(value);
            } catch (...) {
                outResult.clear();
                return StatusCode::InvalidArgument;
            }
        }
        outResult = std::to_string(total);
        return StatusCode::Ok;
    }

    StatusCode EchoCallback(const std::vector<std::string> &args, std::string &outResult, void *) {
        if (args.empty()) {
            outResult.clear();
            return StatusCode::InvalidArgument;
        }
        outResult = args.front();
        return StatusCode::Ok;
    }

    struct AsyncNumberPayload {
        bool *flag;
        int value;
    };

    StatusCode AsyncNumberCallback(void *userData,
                                   spectre::es2025::Value &outValue,
                                   std::string &outDiagnostics) {
        auto *payload = static_cast<AsyncNumberPayload *>(userData);
        if (payload != nullptr && payload->flag != nullptr) {
            *payload->flag = true;
        }
        const int number = payload != nullptr ? payload->value : 0;
        outValue = spectre::es2025::Value::Number(static_cast<double>(number));
        outDiagnostics = "ok";
        return StatusCode::Ok;
    }

    StatusCode AsyncFailCallback(void *,
                                 spectre::es2025::Value &outValue,
                                 std::string &outDiagnostics) {
        outValue = spectre::es2025::Value::Undefined();
        outDiagnostics = "async failure";
        return StatusCode::InvalidArgument;
    }

    struct PromiseReactionPayload {
        bool invoked;
        int scale;
        std::string lastDiagnostics;
    };

    StatusCode PromiseFulfillCallback(void *userData,
                                      const spectre::es2025::Value &input,
                                      spectre::es2025::Value &outValue,
                                      std::string &outDiagnostics) {
        auto *payload = static_cast<PromiseReactionPayload *>(userData);
        if (payload) {
            payload->invoked = true;
            payload->lastDiagnostics = input.ToString();
            auto scaled = input.AsNumber(0.0) * static_cast<double>(payload->scale);
            outValue = spectre::es2025::Value::Number(scaled);
        } else {
            outValue = input;
        }
        outDiagnostics = "scaled";
        return StatusCode::Ok;
    }

    StatusCode PromiseRejectCallback(void *userData,
                                     const spectre::es2025::Value &input,
                                     spectre::es2025::Value &outValue,
                                     std::string &outDiagnostics) {
        auto *payload = static_cast<PromiseReactionPayload *>(userData);
        if (payload) {
            payload->invoked = true;
            payload->lastDiagnostics = input.ToString();
        }
        outValue = spectre::es2025::Value::String("recovered");
        outDiagnostics = "handled";
        return StatusCode::Ok;
    }

    bool DefaultConfigPopulatesDefaults() {
        auto config = spectre::MakeDefaultConfig();
        bool ok = true;
        ok &= ExpectTrue(config.mode == RuntimeMode::SingleThread, "Default mode should be single thread");
        ok &= ExpectTrue(config.memory.heapBytes > 0, "Heap budget positive");
        ok &= ExpectTrue(config.memory.arenaBytes > 0, "Arena budget positive");
        ok &= ExpectTrue(config.telemetry.historySize > 0, "Telemetry history positive");
        ok &= ExpectTrue(!config.enableGpuAcceleration, "GPU disabled by default");
        return ok;
    }

    bool ContextLifecycle() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        SpectreContext *context = nullptr;
        auto status = runtime->CreateContext({"ui", 32768}, &context);
        bool ok = ExpectStatus(status, StatusCode::Ok, "CreateContext should succeed");
        ok &= ExpectTrue(context != nullptr, "Context pointer valid");
        if (context != nullptr) {
            ok &= ExpectTrue(context->Name() == "ui", "Context name");
            ok &= ExpectTrue(context->StackSize() == 32768, "Context stack size");
            ok &= ExpectTrue(context->ScriptNames().empty(), "Script list empty");
            ok &= ExpectTrue(context->ScriptVersion("missing") == 0, "Missing script version zero");
        }
        auto duplicateStatus = runtime->CreateContext({"ui", 128}, &context);
        ok &= ExpectStatus(duplicateStatus, StatusCode::AlreadyExists, "Duplicate context status");
        ok &= ExpectTrue(context != nullptr, "Duplicate context pointer valid");
        auto destroyStatus = runtime->DestroyContext("ui");
        ok &= ExpectStatus(destroyStatus, StatusCode::Ok, "DestroyContext status");
        auto missingStatus = runtime->DestroyContext("ui");
        ok &= ExpectStatus(missingStatus, StatusCode::NotFound, "DestroyContext after removal");
        return ok;
    }

    bool LoadFailuresSurfaceStatuses() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        auto loadMissing = runtime->LoadScript("missing", {"boot", "return 1;"});
        bool ok = ExpectStatus(loadMissing.status, StatusCode::NotFound, "LoadScript missing context");
        runtime->CreateContext({"logic", 4096}, nullptr);
        auto evalMissing = runtime->EvaluateSync("logic", "boot");
        ok &= ExpectStatus(evalMissing.status, StatusCode::NotFound, "Evaluate missing script");
        auto invalidScript = runtime->LoadScript("logic", {"broken", "let a = 1;"});
        ok &= ExpectStatus(invalidScript.status, StatusCode::InvalidArgument, "Invalid script rejected");
        ok &= ExpectTrue(!invalidScript.diagnostics.empty(), "Invalid script diagnostics provided");
        auto bytecodeMissing = runtime->LoadBytecode("ghost", {"bundle", {}});
        ok &= ExpectStatus(bytecodeMissing.status, StatusCode::NotFound, "LoadBytecode missing context");
        return ok;
    }

    bool ScriptCompilationAndLookup() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        runtime->CreateContext({"logic", 65536}, nullptr);
        spectre::ScriptSource script{"init", "return 42;"};
        auto loadResult = runtime->LoadScript("logic", script);
        bool ok = ExpectStatus(loadResult.status, StatusCode::Ok, "LoadScript status");
        ok &= ExpectTrue(loadResult.value == "init", "LoadScript returns script name");
        ok &= ExpectTrue(loadResult.diagnostics == "Script compiled", "Compilation diagnostics");

        auto evalResult = runtime->EvaluateSync("logic", "init");
        ok &= ExpectStatus(evalResult.status, StatusCode::Ok, "EvaluateSync status");
        ok &= ExpectTrue(evalResult.value == "42", "EvaluateSync literal");
        ok &= ExpectTrue(evalResult.diagnostics == "ok", "Execution diagnostics");

        const SpectreContext *logicContext = nullptr;
        auto ctxStatus = runtime->GetContext("logic", &logicContext);
        ok &= ExpectStatus(ctxStatus, StatusCode::Ok, "GetContext status");
        const spectre::ScriptRecord *record = nullptr;
        if (logicContext != nullptr) {
            ok &= ExpectStatus(logicContext->GetScript("init", &record), StatusCode::Ok, "GetScript status");
            ok &= ExpectTrue(logicContext->ScriptVersion("init") == 1, "Script version increments");
            ok &= ExpectTrue(!logicContext->ScriptNames().empty(), "Script names populated");
        }
        ok &= ExpectTrue(record != nullptr, "Script record pointer valid");
        if (record != nullptr) {
            ok &= ExpectTrue(!record->bytecode.empty(), "Bytecode serialized");
            ok &= ExpectTrue(!record->bytecodeHash.empty(), "Bytecode hash populated");
        }

        spectre::ScriptSource update{"init", "return 64;"};
        auto updateResult = runtime->LoadScript("logic", update);
        ok &= ExpectStatus(updateResult.status, StatusCode::Ok, "Reload script status");
        ok &= ExpectTrue(updateResult.diagnostics == "Script compiled", "Reload diagnostics");
        if (logicContext != nullptr) {
            ok &= ExpectTrue(logicContext->ScriptVersion("init") == 2, "Script version updated");
        }
        auto evalUpdate = runtime->EvaluateSync("logic", "init");
        ok &= ExpectStatus(evalUpdate.status, StatusCode::Ok, "Evaluate updated script");
        ok &= ExpectTrue(evalUpdate.value == "64", "Updated script value");
        return ok;
    }

    bool LiteralExecutionCoverage() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        runtime->CreateContext({"literals", 16384}, nullptr);
        struct LiteralCase {
            const char *name;
            const char *source;
            const char *expected;
        } cases[]{
                    {"string", "return 'spectre';", "spectre"},
                    {"double", "return 3.1415;", "3.1415"},
                    {"int", "return 12;", "12"},
                    {"boolTrue", "return true;", "true"},
                    {"boolFalse", "return false;", "false"},
                    {"nullValue", "return null;", "null"},
                    {"undefinedValue", "return undefined;", "undefined"}
                };
        bool ok = true;
        for (const auto &c: cases) {
            auto res = runtime->LoadScript("literals", {c.name, c.source});
            ok &= ExpectStatus(res.status, StatusCode::Ok, "Literal load status");
            auto eval = runtime->EvaluateSync("literals", c.name);
            ok &= ExpectStatus(eval.status, StatusCode::Ok, "Literal eval status");
            ok &= ExpectTrue(eval.value == c.expected, std::string("Literal eval value actual: ") + eval.value);
            ok &= ExpectTrue(eval.diagnostics == "ok", "Literal diagnostics ok");
        }
        return ok;
    }

    bool ArithmeticExecution() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        runtime->CreateContext({"math", 16384}, nullptr);
        struct MathCase {
            const char *name;
            const char *source;
            const char *expected;
        } cases[]{
                    {"add", "return 1 + 2 + 3;", "6"},
                    {"precedence", "return 2 + 3 * 4;", "14"},
                    {"parentheses", "return (2 + 3) * 4;", "20"},
                    {"unary", "return -8 + 3;", "-5"},
                    {"division", "return 18 / 6;", "3"}
                };
        bool ok = true;
        for (const auto &c: cases) {
            auto res = runtime->LoadScript("math", {c.name, c.source});
            ok &= ExpectStatus(res.status, StatusCode::Ok, "Math load status");
            auto eval = runtime->EvaluateSync("math", c.name);
            ok &= ExpectStatus(eval.status, StatusCode::Ok, "Math eval status");
            ok &= ExpectTrue(eval.value == c.expected, std::string("Math eval value actual: ") + eval.value);
        }
        return ok;
    }

    bool DivisionByZeroReported() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        runtime->CreateContext({"math", 16384}, nullptr);
        auto load = runtime->LoadScript("math", {"explode", "return 4 / 0;"});
        bool ok = ExpectStatus(load.status, StatusCode::Ok, "Load division script");
        auto eval = runtime->EvaluateSync("math", "explode");
        ok &= ExpectStatus(eval.status, StatusCode::InvalidArgument, "Division by zero status");
        ok &= ExpectTrue(!eval.diagnostics.empty(), "Division diagnostics");
        return ok;
    }

    bool BytecodeRoundTrip() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        runtime->CreateContext({"compile", 16384}, nullptr);
        runtime->CreateContext({"playback", 16384}, nullptr);
        auto load = runtime->LoadScript("compile", {"entry", "return 100 - 1;"});
        bool ok = ExpectStatus(load.status, StatusCode::Ok, "Compile load status");
        const SpectreContext *compileContext = nullptr;
        ok &= ExpectStatus(runtime->GetContext("compile", &compileContext), StatusCode::Ok, "Fetch compile context");
        const spectre::ScriptRecord *record = nullptr;
        if (compileContext != nullptr) {
            ok &= ExpectStatus(compileContext->GetScript("entry", &record), StatusCode::Ok, "Fetch script record");
        }
        ok &= ExpectTrue(record != nullptr && !record->bytecode.empty(), "Serialized bytecode available");
        if (!ok || record == nullptr) {
            return false;
        }
        spectre::BytecodeArtifact artifact{"entry", record->bytecode};
        auto loadBytecode = runtime->LoadBytecode("playback", artifact);
        ok &= ExpectStatus(loadBytecode.status, StatusCode::Ok, "LoadBytecode status");
        auto eval = runtime->EvaluateSync("playback", "entry");
        ok &= ExpectStatus(eval.status, StatusCode::Ok, "Evaluate bytecode");
        ok &= ExpectTrue(eval.value == "99", "Bytecode result matches");
        return ok;
    }

    bool DestroyContextRejectsOperations() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        runtime->CreateContext({"temp", 4096}, nullptr);
        runtime->LoadScript("temp", {"value", "return 5;"});
        runtime->DestroyContext("temp");
        auto eval = runtime->EvaluateSync("temp", "value");
        bool ok = ExpectStatus(eval.status, StatusCode::NotFound, "Evaluate on destroyed context");
        auto ctxStatus = runtime->GetContext("temp", nullptr);
        ok &= ExpectStatus(ctxStatus, StatusCode::NotFound, "GetContext after destroy");
        return ok;
    }

    bool MultiThreadLifecycle() {
        auto config = MakeConfig(RuntimeMode::MultiThread);
        config.enableGpuAcceleration = true;
        auto runtime = SpectreRuntime::Create(config);
        bool ok = ExpectTrue(runtime != nullptr, "Runtime allocation");
        if (!ok) {
            return false;
        }
        ok &= ExpectTrue(runtime->Config().mode == RuntimeMode::MultiThread, "Runtime mode multi thread");
        SpectreContext *context = nullptr;
        auto status = runtime->CreateContext({"logic", 1u << 15}, &context);
        ok &= ExpectStatus(status, StatusCode::Ok, "CreateContext status");
        ok &= ExpectTrue(context != nullptr, "Context pointer valid");
        spectre::ScriptSource script{"main", "return 'multi';"};
        auto loadResult = runtime->LoadScript("logic", script);
        ok &= ExpectStatus(loadResult.status, StatusCode::Ok, "LoadScript status");
        auto evalResult = runtime->EvaluateSync("logic", "main");
        ok &= ExpectStatus(evalResult.status, StatusCode::Ok, "EvaluateSync status");
        ok &= ExpectTrue(evalResult.value == "multi", "EvaluateSync literal");
        spectre::TickInfo tickInfo{0.016, 3};
        runtime->Tick(tickInfo);
        auto lastTick = runtime->LastTick();
        ok &= ExpectTrue(lastTick.frameIndex == 3, "LastTick frame index");
        ok &= ExpectTrue(lastTick.deltaSeconds == 0.016, "LastTick delta seconds");
        status = runtime->DestroyContext("logic");
        ok &= ExpectStatus(status, StatusCode::Ok, "DestroyContext status");
        auto evalMissing = runtime->EvaluateSync("logic", "main");
        ok &= ExpectStatus(evalMissing.status, StatusCode::NotFound, "Evaluate missing context");
        return ok;
    }

    bool SubsystemSuiteProvidesCpuBackends() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime init");
        if (!ok) {
            return false;
        }
        auto &manifest = runtime->Manifest();
        ok &= ExpectTrue(manifest.parserBackend == "cpu.parser.v1", "Parser backend name");
        ok &= ExpectTrue(manifest.bytecodeBackend == "cpu.bytecode.v1", "Bytecode backend name");
        ok &= ExpectTrue(manifest.executionBackend == "cpu.execution.baseline.v1", "Execution backend name");
        ok &= ExpectTrue(manifest.gcBackend == "cpu.gc.linear.v1", "GC backend name");
        ok &= ExpectTrue(manifest.memoryBackend == "cpu.memory.arena.v1", "Memory backend name");
        ok &= ExpectTrue(manifest.telemetryBackend == "cpu.telemetry.ring.v1", "Telemetry backend name");
        ok &= ExpectTrue(manifest.schedulerBackend == "cpu.scheduler.frame.v1", "Scheduler backend name");
        ok &= ExpectTrue(manifest.interopBackend == "cpu.interop.table.v1", "Interop backend name");
        auto &suite = runtime->Subsystems();
        ok &= ExpectTrue(suite.parser != nullptr, "Parser present");
        ok &= ExpectTrue(suite.bytecode != nullptr, "Bytecode present");
        ok &= ExpectTrue(suite.execution != nullptr, "Execution present");
        ok &= ExpectTrue(suite.gc != nullptr, "GC present");
        ok &= ExpectTrue(suite.memory != nullptr, "Memory present");
        ok &= ExpectTrue(suite.telemetry != nullptr, "Telemetry present");
        ok &= ExpectTrue(suite.scheduler != nullptr, "Scheduler present");
        ok &= ExpectTrue(suite.interop != nullptr, "Interop present");
        if (!ok) {
            return false;
        }
        spectre::detail::ScriptUnit unit{"unit", "return 5;"};
        spectre::detail::ModuleArtifact artifact{};
        auto parseStatus = suite.parser->ParseModule(unit, artifact);
        ok &= ExpectStatus(parseStatus, StatusCode::Ok, "Parse module status");
        spectre::detail::ExecutableProgram program{};
        auto lowerStatus = suite.bytecode->LowerModule(artifact, program);
        ok &= ExpectStatus(lowerStatus, StatusCode::Ok, "Lower module status");
        spectre::detail::ExecutionRequest request{"ctx", "entry", &program};
        auto execResponse = suite.execution->Execute(request);
        ok &= ExpectStatus(execResponse.status, StatusCode::Ok, "Execution status");
        ok &= ExpectTrue(execResponse.value == "5", "Execution returns literal");
        spectre::detail::GcSnapshot snapshot{};
        auto gcStatus = suite.gc->Collect(snapshot);
        ok &= ExpectStatus(gcStatus, StatusCode::Ok, "GC collect status");
        ok &= ExpectTrue(snapshot.generation > 0, "GC generation incremented");
        spectre::detail::MemoryBudgetPlan plan{runtime->Config().memory, 64};
        auto memoryStatus = suite.memory->ApplyPlan(plan);
        ok &= ExpectStatus(memoryStatus, StatusCode::Ok, "Memory plan status");
        spectre::detail::TelemetrySample sample{"frame", 1.0, 1};
        suite.telemetry->PushSample(sample);
        auto drained = suite.telemetry->Drain();
        ok &= ExpectTrue(drained.size() == 1, "Telemetry drain count");
        spectre::detail::SchedulerFramePlan framePlan{1, 0.5, 0.0};
        auto scheduleStatus = suite.scheduler->PlanFrame(framePlan);
        ok &= ExpectStatus(scheduleStatus, StatusCode::Ok, "Scheduler plan status");
        spectre::detail::InteropBinding binding{"symbol", reinterpret_cast<void *>(0x1)};
        auto interopStatus = suite.interop->Register(binding);
        ok &= ExpectStatus(interopStatus, StatusCode::Ok, "Interop register status");
        auto dupStatus = suite.interop->Register(binding);
        ok &= ExpectStatus(dupStatus, StatusCode::AlreadyExists, "Interop duplicate status");
        return ok;
    }

    bool RegExpModuleCompilesAndMatches() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        auto &environment = runtime->EsEnvironment();
        auto *module = dynamic_cast<spectre::es2025::RegExpModule *>(environment.FindModule("RegExp"));
        bool ok = ExpectTrue(module != nullptr, "RegExp module available");
        if (!module) {
            return false;
        }

        spectre::es2025::RegExpModule::Handle globalHandle = 0;
        ok &= ExpectStatus(module->Compile("a+(b)", "gi", globalHandle), StatusCode::Ok,
                           "Compile global ignore-case pattern");

        spectre::es2025::RegExpModule::Handle cachedHandle = 0;
        ok &= ExpectStatus(module->Compile("a+(b)", "ig", cachedHandle), StatusCode::Ok,
                           "Compile with reordered flags hits cache");
        ok &= ExpectTrue(globalHandle == cachedHandle, "Compile reuses cached handle");

        spectre::es2025::RegExpModule::MatchResult firstMatch;
        ok &= ExpectStatus(module->Exec(globalHandle, "aaabAAAb", std::numeric_limits<std::size_t>::max(), firstMatch),
                           StatusCode::Ok, "Exec succeeds");
        ok &= ExpectTrue(firstMatch.matched, "Exec finds match");
        ok &= ExpectTrue(firstMatch.index == 0, "Match index");
        ok &= ExpectTrue(firstMatch.groups.size() >= 2, "Capture group count");
        if (firstMatch.groups.size() >= 2) {
            ok &= ExpectTrue(firstMatch.groups[1].begin == 3 && firstMatch.groups[1].end == 4,
                             "Capture span expected");
        }
        ok &= ExpectTrue(module->LastIndex(globalHandle) == firstMatch.nextIndex, "LastIndex updated after exec");

        bool matched = false;
        ok &= ExpectStatus(module->Test(globalHandle, "aaabAAAb", matched), StatusCode::Ok, "Test reuses last index");
        ok &= ExpectTrue(matched, "Test detects next hit");
        ok &= ExpectTrue(module->LastIndex(globalHandle) > firstMatch.nextIndex,
                         "Test advanced last index for global pattern");

        std::string replaced;
        ok &= ExpectStatus(module->Replace(globalHandle, "aaabAAAb", "x", replaced, true), StatusCode::Ok,
                           "Replace applies");
        ok &= ExpectTrue(replaced == "xx", "Replace output");
        ok &= ExpectTrue(module->LastIndex(globalHandle) == 0, "Global replace resets last index");

        spectre::es2025::RegExpModule::Handle splitHandle = 0;
        ok &= ExpectStatus(module->Compile("--", "", splitHandle), StatusCode::Ok, "Compile split pattern");
        std::vector<std::string> parts;
        ok &= ExpectStatus(module->Split(splitHandle, "foo--bar--baz--tail", 3, parts), StatusCode::Ok,
                           "Split segments");
        ok &= ExpectTrue(parts.size() == 3, "Split respects limit");
        if (parts.size() == 3) {
            ok &= ExpectTrue(parts[0] == "foo" && parts[1] == "bar" && parts[2] == "baz--tail",
                             "Split produces remainder");
        }

        spectre::es2025::RegExpModule::Handle stickyHandle = 0;
        ok &= ExpectStatus(module->Compile("\\d+", "y", stickyHandle), StatusCode::Ok,
                           "Compile sticky pattern");
        spectre::es2025::RegExpModule::MatchResult stickyMatch;
        ok &= ExpectStatus(module->Exec(stickyHandle, "123-456", 0, stickyMatch), StatusCode::Ok,
                           "Sticky exec matches prefix");
        ok &= ExpectTrue(stickyMatch.matched && stickyMatch.length == 3, "Sticky match length");
        spectre::es2025::RegExpModule::MatchResult stickyMiss;
        ok &= ExpectStatus(module->Exec(stickyHandle, "123-456", std::numeric_limits<std::size_t>::max(), stickyMiss),
                           StatusCode::Ok, "Sticky second exec");
        ok &= ExpectTrue(!stickyMiss.matched, "Sticky fails at non-adjacent position");

        return ok;
    }

    bool TypedArrayModuleCoversElementOps() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        auto &environment = runtime->EsEnvironment();
        auto *module = dynamic_cast<spectre::es2025::TypedArrayModule *>(environment.FindModule("TypedArray"));
        bool ok = ExpectTrue(module != nullptr, "TypedArray module available");
        if (!module) {
            return false;
        }

        spectre::es2025::TypedArrayModule::Handle handle = 0;
        ok &= ExpectStatus(module->Create(spectre::es2025::TypedArrayModule::ElementType::Uint8, 8, "buffer", handle),
                           StatusCode::Ok, "Create Uint8 array");
        ok &= ExpectTrue(module->Length(handle) == 8, "Length matches");
        ok &= ExpectStatus(module->Fill(handle, 255.0), StatusCode::Ok, "Fill entire array");
        ok &= ExpectStatus(module->Set(handle, 3, 127.0), StatusCode::Ok, "Set index");
        double value = 0.0;
        ok &= ExpectStatus(module->Get(handle, 3, value), StatusCode::Ok, "Get index");
        ok &= ExpectTrue(value == 127.0, "Value roundtrip");
        ok &= ExpectStatus(module->CopyWithin(handle, 4, 0, 4), StatusCode::Ok, "CopyWithin");

        std::vector<double> dense;
        ok &= ExpectStatus(module->ToVector(handle, dense), StatusCode::Ok, "ToVector succeeds");
        ok &= ExpectTrue(dense.size() == 8, "Vector size");

        spectre::es2025::TypedArrayModule::Handle sub = 0;
        ok &= ExpectStatus(module->Subarray(handle, 2, 6, "view", sub), StatusCode::Ok, "Create view");
        ok &= ExpectTrue(module->Length(sub) == 4, "View length");
        ok &= ExpectTrue(module->ByteOffset(sub) == 2, "View byte offset");
        ok &= ExpectStatus(module->Set(sub, 1, 64.0), StatusCode::Ok, "Mutate view");
        ok &= ExpectStatus(module->Get(handle, 3, value), StatusCode::Ok, "Shared buffer update visible");
        ok &= ExpectTrue(value == 64.0, "View mutation applied");

        spectre::es2025::TypedArrayModule::Handle big = 0;
        ok &= ExpectStatus(module->Create(spectre::es2025::TypedArrayModule::ElementType::BigInt64, 2, "big", big),
                           StatusCode::Ok, "Create BigInt array");
        ok &= ExpectStatus(module->FillBigInt(big, 42), StatusCode::Ok, "FillBigInt");
        std::int64_t bigValue = 0;
        ok &= ExpectStatus(module->GetBigInt(big, 1, bigValue), StatusCode::Ok, "GetBigInt");
        ok &= ExpectTrue(bigValue == 42, "BigInt value");
        ok &= ExpectStatus(module->SetBigInt(big, 0, -7), StatusCode::Ok, "SetBigInt negative");

        ok &= ExpectStatus(module->Destroy(sub), StatusCode::Ok, "Destroy subarray");
        ok &= ExpectStatus(module->Destroy(big), StatusCode::Ok, "Destroy BigInt array");
        ok &= ExpectStatus(module->Destroy(handle), StatusCode::Ok, "Destroy primary array");
        return ok;
    }

    bool TickAndReconfigureUpdatesState() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime init");
        if (!ok) {
            return false;
        }
        spectre::TickInfo info{0.016, 12};
        runtime->Tick(info);
        auto last = runtime->LastTick();
        ok &= ExpectTrue(last.frameIndex == 12, "Frame index recorded");
        ok &= ExpectTrue(last.deltaSeconds == 0.016, "Delta recorded");
        RuntimeConfig config = runtime->Config();
        config.memory.heapBytes += 1024;
        auto status = runtime->Reconfigure(config);
        ok &= ExpectStatus(status, StatusCode::Ok, "Reconfigure status");
        ok &= ExpectTrue(runtime->Config().memory.heapBytes == config.memory.heapBytes, "Config updated");
        return ok;
    }

    bool GlobalModuleInitializesDefaultContext() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!ok) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *module = environment.FindModule("Global");
        auto *globalModule = dynamic_cast<spectre::es2025::GlobalModule *>(module);
        ok &= ExpectTrue(globalModule != nullptr, "Global module available");
        if (!globalModule) {
            return false;
        }
        ok &= ExpectTrue(!globalModule->DefaultContext().empty(), "Default context name");
        const SpectreContext *context = nullptr;
        auto status = runtime->GetContext(globalModule->DefaultContext(), &context);
        ok &= ExpectStatus(status, StatusCode::Ok, "Default context created");
        ok &= ExpectTrue(context != nullptr, "Context pointer valid");
        if (context != nullptr) {
            ok &= ExpectTrue(context->Name() == globalModule->DefaultContext(), "Context name matches");
            ok &= ExpectTrue(context->StackSize() == globalModule->DefaultStackSize(), "Stack size matches");
        }
        return ok;
    }

    bool GlobalModuleEvaluatesScripts() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!ok) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *globalModule = dynamic_cast<spectre::es2025::GlobalModule *>(environment.FindModule("Global"));
        ok &= ExpectTrue(globalModule != nullptr, "Global module available");
        if (!globalModule) {
            return false;
        }
        std::string value;
        std::string diagnostics;
        auto status = globalModule->EvaluateScript("return 'spectre-global';", value, diagnostics, "boot");
        ok &= ExpectStatus(status, StatusCode::Ok, "Initial evaluation");
        ok &= ExpectTrue(value == "spectre-global", "Return value");
        ok &= ExpectTrue(diagnostics == "ok", "Diagnostics ok");
        const SpectreContext *context = nullptr;
        runtime->GetContext(globalModule->DefaultContext(), &context);
        const spectre::ScriptRecord *record = nullptr;
        if (context != nullptr) {
            auto scriptStatus = context->GetScript("boot", &record);
            ok &= ExpectStatus(scriptStatus, StatusCode::Ok, "Script stored");
            if (record != nullptr) {
                ok &= ExpectTrue(!record->bytecode.empty(), "Bytecode emitted");
                ok &= ExpectTrue(!record->bytecodeHash.empty(), "Bytecode hash");
            }
        }
        status = globalModule->EvaluateScript("return 'spectre-global-2';", value, diagnostics, "boot");
        ok &= ExpectStatus(status, StatusCode::Ok, "Reload evaluation");
        ok &= ExpectTrue(value == "spectre-global-2", "Reload result");
        if (context != nullptr) {
            ok &= ExpectTrue(context->ScriptVersion("boot") == 2, "Version increments");
        }
        return ok;
    }

    bool GlobalModuleReconfigureTogglesGpu() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!ok) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *globalModule = dynamic_cast<spectre::es2025::GlobalModule *>(environment.FindModule("Global"));
        ok &= ExpectTrue(globalModule != nullptr, "Global module available");
        if (!globalModule) {
            return false;
        }
        ok &= ExpectTrue(!globalModule->GpuEnabled(), "GPU disabled by default");
        auto config = runtime->Config();
        config.enableGpuAcceleration = true;
        auto status = runtime->Reconfigure(config);
        ok &= ExpectStatus(status, StatusCode::Ok, "Runtime reconfigure");
        environment.OptimizeGpu(true);
        ok &= ExpectTrue(globalModule->GpuEnabled(), "GPU enabled flag");
        std::string value;
        std::string diagnostics;
        status = globalModule->EvaluateScript("return 42;", value, diagnostics, "gpuCheck");
        ok &= ExpectStatus(status, StatusCode::Ok, "Evaluation under GPU");
        ok &= ExpectTrue(value == "42", "Numeric result");
        ok &= ExpectTrue(diagnostics == "ok", "Diagnostics ok");
        return ok;
    }

    bool ErrorModuleRegistersBuiltins() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *errorModule = dynamic_cast<spectre::es2025::ErrorModule *>(environment.FindModule("Error"));
        ok &= ExpectTrue(errorModule != nullptr, "Error module available");
        if (!errorModule) {
            return false;
        }
        const char *types[] = {
            "Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", "URIError",
            "AggregateError"
        };
        for (const auto *type: types) {
            ok &= ExpectTrue(errorModule->HasErrorType(type), std::string("Missing builtin error type ") + type);
        }
        ok &= ExpectTrue(errorModule->History().empty(), "History should start empty");
        return ok;
    }

    bool ErrorModuleRaisesAndTracksHistory() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *errorModule = dynamic_cast<spectre::es2025::ErrorModule *>(environment.FindModule("Error"));
        ok &= ExpectTrue(errorModule != nullptr, "Error module available");
        if (!errorModule) {
            return false;
        }
        spectre::TickInfo tick{0.016, 42};
        runtime->Tick(tick);

        std::string formatted;
        spectre::es2025::ErrorRecord record{};
        auto status = errorModule->RaiseError("TypeError", "Unsupported operand", "global.main", "demo",
                                              "operands must be numbers", formatted, &record);
        ok &= ExpectStatus(status, StatusCode::Ok, "RaiseError succeeds");
        ok &= ExpectTrue(!formatted.empty(), "Formatted error available");
        ok &= ExpectTrue(formatted.find("TypeError") != std::string::npos, "Formatted includes type");
        ok &= ExpectTrue(formatted.find("Unsupported operand") != std::string::npos, "Formatted includes message");
        ok &= ExpectTrue(record.frameIndex == 42, "Frame tracked");
        ok &= ExpectTrue(record.type == "TypeError", "Record type");
        ok &= ExpectTrue(record.message == "Unsupported operand", "Record message");
        ok &= ExpectTrue(record.contextName == "global.main", "Record context");
        ok &= ExpectTrue(record.scriptName == "demo", "Record script");
        ok &= ExpectTrue(record.diagnostics == "operands must be numbers", "Record diagnostics");
        ok &= ExpectTrue(!errorModule->History().empty(), "History populated");
        const auto *last = errorModule->LastError();
        ok &= ExpectTrue(last != nullptr, "LastError available");
        if (last != nullptr) {
            ok &= ExpectTrue(last->type == record.type, "LastError matches");
        }
        return ok;
    }

    bool ErrorModuleDrainsHistory() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *errorModule = dynamic_cast<spectre::es2025::ErrorModule *>(environment.FindModule("Error"));
        ok &= ExpectTrue(errorModule != nullptr, "Error module available");
        if (!errorModule) {
            return false;
        }
        std::string formatted;
        errorModule->RaiseError("Error", "First", "global", "a", "", formatted, nullptr);
        errorModule->RaiseError("Error", "Second", "global", "b", "", formatted, nullptr);

        auto drained = errorModule->DrainHistory();
        ok &= ExpectTrue(drained.size() == 2, "Drain size");
        ok &= ExpectTrue(errorModule->History().empty(), "History cleared");
        errorModule->RaiseError("Error", "Third", "global", "c", "", formatted, nullptr);
        errorModule->ClearHistory();
        ok &= ExpectTrue(errorModule->History().empty(), "History cleared explicit");
        return ok;
    }

    bool ErrorModuleSupportsCustomTypes() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *errorModule = dynamic_cast<spectre::es2025::ErrorModule *>(environment.FindModule("Error"));
        ok &= ExpectTrue(errorModule != nullptr, "Error module available");
        if (!errorModule) {
            return false;
        }
        auto status = errorModule->RegisterErrorType("ScriptError", "Script failure");
        ok &= ExpectStatus(status, StatusCode::Ok, "Register custom type");
        ok &= ExpectTrue(errorModule->HasErrorType("ScriptError"), "Custom type registered");
        std::string formatted;
        status = errorModule->RaiseError("ScriptError", "Fatal", "global.main", "script", "runtime", formatted,
                                         nullptr);
        ok &= ExpectStatus(status, StatusCode::Ok, "Raise custom error");
        ok &= ExpectTrue(formatted.find("ScriptError") != std::string::npos, "Formatted custom type");
        return ok;
    }

    bool AsyncFunctionModuleDispatchesJobs() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *module = dynamic_cast<spectre::es2025::AsyncFunctionModule *>(environment.FindModule("AsyncFunction"));
        ok &= ExpectTrue(module != nullptr, "AsyncFunction module available");
        if (!module) {
            return false;
        }
        ok &= ExpectStatus(module->Configure(8, 4), StatusCode::Ok, "Configure async module");

        bool fastInvoked = false;
        AsyncNumberPayload fastPayload{&fastInvoked, 7};
        spectre::es2025::AsyncFunctionModule::DispatchOptions fastOptions;
        fastOptions.label = "fast";
        spectre::es2025::AsyncFunctionModule::Handle fastHandle = 0;
        auto status = module->Enqueue(AsyncNumberCallback, &fastPayload, fastOptions, fastHandle);
        ok &= ExpectStatus(status, StatusCode::Ok, "Enqueue fast job");
        ok &= ExpectTrue(fastHandle != spectre::es2025::AsyncFunctionModule::kInvalidHandle,
                         "Fast handle valid");

        bool delayedInvoked = false;
        AsyncNumberPayload delayedPayload{&delayedInvoked, 42};
        spectre::es2025::AsyncFunctionModule::DispatchOptions delayedOptions;
        delayedOptions.delayFrames = 2;
        delayedOptions.label = "delayed";
        spectre::es2025::AsyncFunctionModule::Handle delayedHandle = 0;
        status = module->Enqueue(AsyncNumberCallback, &delayedPayload, delayedOptions, delayedHandle);
        ok &= ExpectStatus(status, StatusCode::Ok, "Enqueue delayed job");
        ok &= ExpectTrue(module->PendingCount() == 2, "Pending count after enqueue");

        std::vector<spectre::es2025::AsyncFunctionModule::Result> results;
        runtime->Tick({0.016, 0});
        module->DrainCompleted(results);
        ok &= ExpectTrue(results.size() == 1, "Fast path completes on first tick");
        if (!results.empty()) {
            ok &= ExpectTrue(results.front().handle == fastHandle, "Fast handle matches");
            ok &= ExpectTrue(results.front().status == StatusCode::Ok, "Fast status ok");
            ok &= ExpectTrue(results.front().value.AsNumber(-1.0) == 7.0, "Fast value matches");
            ok &= ExpectTrue(results.front().diagnostics == "ok", "Fast diagnostics propagated");
            ok &= ExpectTrue(std::string(results.front().label.data()) == "fast", "Fast label preserved");
        }
        results.clear();
        ok &= ExpectTrue(fastInvoked, "Fast callback invoked");
        ok &= ExpectTrue(!delayedInvoked, "Delayed callback pending");
        ok &= ExpectTrue(module->PendingCount() == 1, "Pending count after first tick");

        runtime->Tick({0.016, 1});
        module->DrainCompleted(results);
        ok &= ExpectTrue(results.empty(), "Delayed job still pending");
        ok &= ExpectTrue(module->PendingCount() == 1, "Pending count unchanged");

        runtime->Tick({0.016, 2});
        module->DrainCompleted(results);
        ok &= ExpectTrue(results.size() == 1, "Delayed job completes on schedule");
        if (!results.empty()) {
            ok &= ExpectTrue(results.front().handle == delayedHandle, "Delayed handle matches");
            ok &= ExpectTrue(results.front().status == StatusCode::Ok, "Delayed status ok");
            ok &= ExpectTrue(results.front().value.AsNumber(-1.0) == 42.0, "Delayed value matches");
            ok &= ExpectTrue(std::string(results.front().label.data()) == "delayed", "Delayed label preserved");
        }
        results.clear();
        ok &= ExpectTrue(delayedInvoked, "Delayed callback invoked");

        const auto &metrics = module->GetMetrics();
        ok &= ExpectTrue(metrics.enqueued == 2, "Metrics enqueued count");
        ok &= ExpectTrue(metrics.executed == 2, "Metrics executed count");
        ok &= ExpectTrue(metrics.failed == 0, "Metrics failure count");
        ok &= ExpectTrue(metrics.fastPath == 1, "Metrics fast path count");
        ok &= ExpectTrue(module->PendingCount() == 0, "Queue drained");
        return ok;
    }

    bool AsyncFunctionModuleHandlesDelaysAndCancellation() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *module = dynamic_cast<spectre::es2025::AsyncFunctionModule *>(environment.FindModule("AsyncFunction"));
        ok &= ExpectTrue(module != nullptr, "AsyncFunction module available");
        if (!module) {
            return false;
        }
        ok &= ExpectStatus(module->Configure(6, 6), StatusCode::Ok, "Configure async module");

        bool secondsInvoked = false;
        AsyncNumberPayload secondsPayload{&secondsInvoked, 11};
        spectre::es2025::AsyncFunctionModule::DispatchOptions secondsOptions;
        secondsOptions.delaySeconds = 0.05;
        secondsOptions.label = "seconds";
        spectre::es2025::AsyncFunctionModule::Handle secondsHandle = 0;
        auto status = module->Enqueue(AsyncNumberCallback, &secondsPayload, secondsOptions, secondsHandle);
        ok &= ExpectStatus(status, StatusCode::Ok, "Enqueue seconds-delayed job");

        spectre::es2025::AsyncFunctionModule::DispatchOptions failOptions;
        failOptions.label = "fail";
        spectre::es2025::AsyncFunctionModule::Handle failHandle = 0;
        status = module->Enqueue(AsyncFailCallback, nullptr, failOptions, failHandle);
        ok &= ExpectStatus(status, StatusCode::Ok, "Enqueue failing job");

        spectre::es2025::AsyncFunctionModule::DispatchOptions cancelOptions;
        cancelOptions.delayFrames = 5;
        cancelOptions.label = "cancel";
        spectre::es2025::AsyncFunctionModule::Handle cancelHandle = 0;
        status = module->Enqueue(AsyncNumberCallback, nullptr, cancelOptions, cancelHandle);
        ok &= ExpectStatus(status, StatusCode::Ok, "Enqueue cancellable job");
        ok &= ExpectTrue(module->Cancel(cancelHandle), "Cancel succeeds");
        ok &= ExpectTrue(module->PendingCount() == 2, "Pending count after cancel");

        std::vector<spectre::es2025::AsyncFunctionModule::Result> results;
        module->DrainCompleted(results);
        ok &= ExpectTrue(results.empty(), "No completions before tick");

        runtime->Tick({0.010, 0});
        module->DrainCompleted(results);
        ok &= ExpectTrue(results.size() == 1, "Failing job runs first");
        if (!results.empty()) {
            ok &= ExpectTrue(results.front().handle == failHandle, "Fail handle matches");
            ok &= ExpectTrue(results.front().status == StatusCode::InvalidArgument, "Fail status propagated");
            ok &= ExpectTrue(results.front().diagnostics == "async failure", "Fail diagnostics propagated");
            ok &= ExpectTrue(std::string(results.front().label.data()) == "fail", "Fail label preserved");
        }
        results.clear();
        ok &= ExpectTrue(!secondsInvoked, "Seconds job not yet executed");

        runtime->Tick({0.020, 1});
        module->DrainCompleted(results);
        ok &= ExpectTrue(results.empty(), "Seconds job still waiting");

        runtime->Tick({0.030, 2});
        module->DrainCompleted(results);
        ok &= ExpectTrue(results.size() == 1, "Seconds job completes after delay");
        if (!results.empty()) {
            ok &= ExpectTrue(results.front().handle == secondsHandle, "Seconds handle matches");
            ok &= ExpectTrue(results.front().status == StatusCode::Ok, "Seconds status ok");
            ok &= ExpectTrue(results.front().value.AsNumber(-1.0) == 11.0, "Seconds value matches");
            ok &= ExpectTrue(std::string(results.front().label.data()) == "seconds", "Seconds label preserved");
        }
        results.clear();
        ok &= ExpectTrue(secondsInvoked, "Seconds callback invoked");
        ok &= ExpectTrue(module->PendingCount() == 0, "Pending queue empty");

        const auto &metrics = module->GetMetrics();
        ok &= ExpectTrue(metrics.enqueued == 3, "Metrics enqueued total");
        ok &= ExpectTrue(metrics.executed == 2, "Metrics executed after cancel");
        ok &= ExpectTrue(metrics.cancelled == 1, "Metrics cancellation count");
        ok &= ExpectTrue(metrics.failed == 1, "Metrics failure count");
        ok &= ExpectTrue(metrics.overflow == 0, "Metrics overflow baseline");

        ok &= ExpectStatus(module->Configure(1, 1), StatusCode::Ok, "Reconfigure for overflow test");
        bool singleInvoked = false;
        AsyncNumberPayload singlePayload{&singleInvoked, 5};
        spectre::es2025::AsyncFunctionModule::DispatchOptions singleOptions;
        singleOptions.label = "single";
        spectre::es2025::AsyncFunctionModule::Handle singleHandle = 0;
        status = module->Enqueue(AsyncNumberCallback, &singlePayload, singleOptions, singleHandle);
        ok &= ExpectStatus(status, StatusCode::Ok, "Enqueue single job after reconfigure");
        ok &= ExpectTrue(module->PendingCount() == 1, "Pending count after single enqueue");

        spectre::es2025::AsyncFunctionModule::Handle overflowHandle = 0;
        status = module->Enqueue(AsyncNumberCallback, &singlePayload, singleOptions, overflowHandle);
        ok &= ExpectStatus(status, StatusCode::CapacityExceeded, "Overflow enqueue rejected");
        const auto &overflowMetrics = module->GetMetrics();
        ok &= ExpectTrue(overflowMetrics.overflow == 1, "Overflow metric increments");

        runtime->Tick({0.016, 3});
        module->DrainCompleted(results);
        ok &= ExpectTrue(results.size() == 1, "Single job executes after overflow");
        if (!results.empty()) {
            ok &= ExpectTrue(results.front().handle == singleHandle, "Single handle matches");
        }
        results.clear();
        ok &= ExpectTrue(singleInvoked, "Single callback invoked");
        ok &= ExpectTrue(module->PendingCount() == 0, "Pending queue drained after overflow test");

        return ok;
    }

    bool AsyncIteratorModuleCoordinatesValues() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *module = dynamic_cast<spectre::es2025::AsyncIteratorModule *>(environment.FindModule("AsyncIterator"));
        ok &= ExpectTrue(module != nullptr, "AsyncIterator module available");
        if (!module) {
            return false;
        }

        spectre::es2025::AsyncIteratorModule::StreamConfig config{};
        config.queueCapacity = 8;
        config.waiterCapacity = 8;
        config.label = "spectre.stream.primary";
        spectre::es2025::AsyncIteratorModule::Handle handle = 0;
        ok &= ExpectStatus(module->CreateStream(config, handle), StatusCode::Ok, "Create stream");
        ok &= ExpectTrue(handle != spectre::es2025::AsyncIteratorModule::kInvalidHandle, "Stream handle valid");

        spectre::es2025::AsyncIteratorModule::EnqueueOptions first{};
        first.value = spectre::es2025::Value::Number(42.0);
        first.hasValue = true;
        first.done = false;
        first.diagnostics = "payload";
        ok &= ExpectStatus(module->Enqueue(handle, first), StatusCode::Ok, "Enqueue first value");

        spectre::es2025::AsyncIteratorModule::Request immediate;
        ok &= ExpectStatus(module->RequestNext(handle, immediate), StatusCode::Ok, "Request next immediate");
        ok &= ExpectTrue(immediate.immediate, "Immediate request satisfied");
        ok &= ExpectTrue(immediate.result.hasValue, "Immediate result has value");
        ok &= ExpectTrue(!immediate.result.done, "Immediate result not done");
        ok &= ExpectTrue(immediate.result.value.AsNumber() == 42.0, "Immediate value matches");
        ok &= ExpectTrue(immediate.result.diagnostics == "payload", "Diagnostics propagated");

        std::vector<spectre::es2025::AsyncIteratorModule::Result> drained;
        module->DrainSettled(drained);
        ok &= ExpectTrue(!drained.empty(), "Drained immediate result");
        if (!drained.empty()) {
            ok &= ExpectTrue(drained.front().ticket == immediate.result.ticket, "Ticket preserved");
            ok &= ExpectTrue(drained.front().hasValue, "Drained retains value flag");
        }

        spectre::es2025::AsyncIteratorModule::Request pending;
        ok &= ExpectStatus(module->RequestNext(handle, pending), StatusCode::Ok, "Request pending ticket");
        ok &= ExpectTrue(!pending.immediate, "Pending request enqueued");
        ok &= ExpectTrue(pending.ticket != spectre::es2025::AsyncIteratorModule::kInvalidTicket,
                         "Pending ticket valid");

        spectre::es2025::AsyncIteratorModule::EnqueueOptions second{};
        second.value = spectre::es2025::Value::String("next");
        second.hasValue = true;
        second.done = false;
        ok &= ExpectStatus(module->Enqueue(handle, second), StatusCode::Ok, "Enqueue second value");

        module->DrainSettled(drained);
        ok &= ExpectTrue(drained.size() == 1, "Second drain produces single result");
        if (!drained.empty()) {
            ok &= ExpectTrue(drained.back().ticket == pending.ticket, "Pending ticket fulfilled");
            ok &= ExpectTrue(drained.back().value.AsString() == "next", "Second result payload");
            ok &= ExpectTrue(!drained.back().done, "Second result not done");
        }

        ok &= ExpectStatus(module->SignalComplete(handle, "closed"), StatusCode::Ok, "Signal completion");

        spectre::es2025::AsyncIteratorModule::Request finalRequest;
        ok &= ExpectStatus(module->RequestNext(handle, finalRequest), StatusCode::Ok, "Request final");
        ok &= ExpectTrue(finalRequest.immediate, "Final immediate completion");
        ok &= ExpectTrue(finalRequest.result.done, "Final result done");
        ok &= ExpectTrue(!finalRequest.result.hasValue, "Final result empty");
        ok &= ExpectTrue(
            finalRequest.result.streamState == spectre::es2025::AsyncIteratorModule::StreamState::Completed,
            "Stream state completed");

        module->DrainSettled(drained);
        ok &= ExpectTrue(!drained.empty(), "Completion drained");

        spectre::es2025::AsyncIteratorModule::Request postComplete;
        ok &= ExpectStatus(module->RequestNext(handle, postComplete), StatusCode::Ok, "Request after completion");
        ok &= ExpectTrue(postComplete.immediate, "Post completion immediate");
        ok &= ExpectTrue(postComplete.result.done, "Post completion done");
        ok &= ExpectTrue(!postComplete.result.hasValue, "Post completion empty");

        const auto &metrics = module->GetMetrics();
        ok &= ExpectTrue(metrics.valuesQueued >= 2, "Metrics queued values tracked");
        ok &= ExpectTrue(metrics.valuesDelivered >= 2, "Metrics delivered values tracked");
        ok &= ExpectTrue(metrics.completionsDelivered >= 1, "Metrics completions tracked");
        ok &= ExpectTrue(module->ActiveStreams() == 1, "Active stream count");

        ok &= ExpectTrue(module->DestroyStream(handle), "Destroy stream");
        ok &= ExpectTrue(module->ActiveStreams() == 0, "Active streams zero after destroy");

        return ok;
    }

    bool AsyncIteratorModuleHandlesFailuresAndCancellation() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *module = dynamic_cast<spectre::es2025::AsyncIteratorModule *>(environment.FindModule("AsyncIterator"));
        ok &= ExpectTrue(module != nullptr, "AsyncIterator module available");
        if (!module) {
            return false;
        }

        spectre::es2025::AsyncIteratorModule::StreamConfig config{};
        config.queueCapacity = 4;
        config.waiterCapacity = 4;
        config.label = "spectre.fail";
        spectre::es2025::AsyncIteratorModule::Handle handle = 0;
        ok &= ExpectStatus(module->CreateStream(config, handle), StatusCode::Ok, "Create fail stream");

        spectre::es2025::AsyncIteratorModule::Request firstWaiter;
        ok &= ExpectStatus(module->RequestNext(handle, firstWaiter), StatusCode::Ok, "First waiter");
        ok &= ExpectTrue(!firstWaiter.immediate, "First request pending");

        spectre::es2025::AsyncIteratorModule::Request secondWaiter;
        ok &= ExpectStatus(module->RequestNext(handle, secondWaiter), StatusCode::Ok, "Second waiter");
        ok &= ExpectTrue(!secondWaiter.immediate, "Second request pending");

        ok &= ExpectStatus(module->Fail(handle, "boom"), StatusCode::Ok, "Fail stream");

        std::vector<spectre::es2025::AsyncIteratorModule::Result> drained;
        module->DrainSettled(drained);
        ok &= ExpectTrue(drained.size() == 2, "Failure resolves waiters");
        for (const auto &result: drained) {
            ok &= ExpectTrue(result.status == StatusCode::InvalidArgument, "Failure status propagated");
            ok &= ExpectTrue(result.done, "Failure marks done");
            ok &= ExpectTrue(result.streamState == spectre::es2025::AsyncIteratorModule::StreamState::Failed,
                             "Stream state failed");
            ok &= ExpectTrue(result.diagnostics == "boom", "Failure diagnostics captured");
        }

        spectre::es2025::AsyncIteratorModule::Request postFailure;
        ok &= ExpectStatus(module->RequestNext(handle, postFailure), StatusCode::Ok, "Post failure request");
        ok &= ExpectTrue(postFailure.immediate, "Post failure immediate");
        ok &= ExpectTrue(postFailure.result.status == StatusCode::InvalidArgument, "Post failure status");
        ok &= ExpectTrue(postFailure.result.streamState == spectre::es2025::AsyncIteratorModule::StreamState::Failed,
                         "Post failure state failed");

        ok &= ExpectTrue(module->DestroyStream(handle), "Destroy failed stream");

        spectre::es2025::AsyncIteratorModule::StreamConfig cancelConfig{};
        cancelConfig.queueCapacity = 4;
        cancelConfig.waiterCapacity = 2;
        cancelConfig.label = "spectre.cancel";
        spectre::es2025::AsyncIteratorModule::Handle cancelHandle = 0;
        ok &= ExpectStatus(module->CreateStream(cancelConfig, cancelHandle), StatusCode::Ok, "Create cancel stream");

        spectre::es2025::AsyncIteratorModule::Request cancelWaiter;
        ok &= ExpectStatus(module->RequestNext(cancelHandle, cancelWaiter), StatusCode::Ok, "Pending cancel waiter");
        ok &= ExpectTrue(!cancelWaiter.immediate, "Cancel waiter pending");

        ok &= ExpectTrue(module->DestroyStream(cancelHandle), "Destroy cancel stream");

        module->DrainSettled(drained);
        ok &= ExpectTrue(!drained.empty(), "Cancellation produced result");
        bool sawCancelled = false;
        for (const auto &result: drained) {
            if (result.ticket == cancelWaiter.ticket) {
                sawCancelled = true;
                ok &= ExpectTrue(result.status == StatusCode::InvalidArgument, "Cancellation status invalid argument");
                ok &= ExpectTrue(result.streamState == spectre::es2025::AsyncIteratorModule::StreamState::Cancelled,
                                 "Cancellation stream state");
                ok &= ExpectTrue(result.diagnostics == "cancelled", "Cancellation diagnostics");
            }
        }
        ok &= ExpectTrue(sawCancelled, "Cancellation ticket found");

        return ok;
    }

    bool PromiseModuleResolvesAndChains() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *module = dynamic_cast<spectre::es2025::PromiseModule *>(environment.FindModule("Promise"));
        ok &= ExpectTrue(module != nullptr, "Promise module available");
        if (!module) {
            return false;
        }
        ok &= ExpectStatus(module->Configure(16, 32), StatusCode::Ok, "Configure promise module");

        spectre::es2025::PromiseModule::Handle root = 0;
        ok &= ExpectStatus(module->CreatePromise(root, {"root"}), StatusCode::Ok, "Create root promise");

        PromiseReactionPayload payload{false, 3, {}};
        spectre::es2025::PromiseModule::Handle derived = 0;
        spectre::es2025::PromiseModule::ReactionOptions reaction{};
        reaction.onFulfilled = PromiseFulfillCallback;
        reaction.userData = &payload;
        reaction.label = "derived";
        ok &= ExpectStatus(module->Then(root, derived, reaction), StatusCode::Ok, "Register then handler");

        ok &= ExpectStatus(module->Resolve(root, spectre::es2025::Value::Number(5.0), "initial"), StatusCode::Ok,
                           "Resolve root");
        ok &= ExpectTrue(module->PendingMicrotasks() == 1, "Microtask queued");

        runtime->Tick({0.0, 0});
        ok &= ExpectTrue(module->PendingMicrotasks() == 0, "Microtask drained");

        std::vector<spectre::es2025::PromiseModule::SettledPromise> settled;
        module->DrainSettled(settled);
        ok &= ExpectTrue(settled.size() == 2, "Two promises settled");
        bool rootVerified = false;
        bool derivedVerified = false;
        for (const auto &entry: settled) {
            if (entry.handle == root) {
                rootVerified = true;
                ok &= ExpectTrue(entry.state == spectre::es2025::PromiseModule::State::Fulfilled,
                                 "Root fulfilled state");
                ok &= ExpectTrue(entry.value.AsNumber(-1.0) == 5.0, "Root value stored");
            } else if (entry.handle == derived) {
                derivedVerified = true;
                ok &= ExpectTrue(entry.state == spectre::es2025::PromiseModule::State::Fulfilled,
                                 "Derived fulfilled state");
                ok &= ExpectTrue(entry.value.AsNumber(-1.0) == 15.0, "Derived value");
                ok &= ExpectTrue(entry.diagnostics == "scaled", "Derived diagnostics");
            }
        }
        ok &= ExpectTrue(rootVerified, "Root entry recorded");
        ok &= ExpectTrue(derivedVerified, "Derived entry recorded");
        ok &= ExpectTrue(payload.invoked, "Fulfill handler invoked");

        auto state = module->GetState(derived);
        ok &= ExpectTrue(state == spectre::es2025::PromiseModule::State::Fulfilled,
                         "Derived state API");

        const auto &metrics = module->GetMetrics();
        ok &= ExpectTrue(metrics.created >= 2, "Metrics created count");
        ok &= ExpectTrue(metrics.resolved >= 2, "Metrics resolved count");
        ok &= ExpectTrue(metrics.executedReactions >= 1, "Metrics executed reactions");

        ok &= ExpectStatus(module->Release(root), StatusCode::Ok, "Release root");
        ok &= ExpectStatus(module->Release(derived), StatusCode::Ok, "Release derived");
        return ok;
    }

    bool PromiseModuleHandlesRejectionFlow() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *module = dynamic_cast<spectre::es2025::PromiseModule *>(environment.FindModule("Promise"));
        ok &= ExpectTrue(module != nullptr, "Promise module available");
        if (!module) {
            return false;
        }
        ok &= ExpectStatus(module->Configure(16, 32), StatusCode::Ok, "Configure promise module");

        spectre::es2025::PromiseModule::Handle root = 0;
        ok &= ExpectStatus(module->CreatePromise(root, {"root"}), StatusCode::Ok, "Create root promise");

        spectre::es2025::PromiseModule::Handle propagated = 0;
        ok &= ExpectStatus(module->Then(root, propagated), StatusCode::Ok, "Chain without handler");

        PromiseReactionPayload rejectPayload{false, 1, {}};
        spectre::es2025::PromiseModule::Handle recovered = 0;
        spectre::es2025::PromiseModule::ReactionOptions rejectOptions{};
        rejectOptions.onRejected = PromiseRejectCallback;
        rejectOptions.userData = &rejectPayload;
        rejectOptions.label = "recover";
        ok &= ExpectStatus(module->Then(propagated, recovered, rejectOptions), StatusCode::Ok,
                           "Chain rejection handler");

        ok &= ExpectStatus(module->Reject(root, "boom", spectre::es2025::Value::String("root")), StatusCode::Ok,
                           "Reject root");

        runtime->Tick({0.0, 0});

        std::vector<spectre::es2025::PromiseModule::SettledPromise> settled;
        module->DrainSettled(settled);
        ok &= ExpectTrue(settled.size() == 3, "Three settlements recorded");

        auto stateRoot = module->GetState(root);
        auto statePropagated = module->GetState(propagated);
        auto stateRecovered = module->GetState(recovered);
        ok &= ExpectTrue(stateRoot == spectre::es2025::PromiseModule::State::Rejected, "Root rejected");
        ok &= ExpectTrue(statePropagated == spectre::es2025::PromiseModule::State::Rejected,
                         "Propagation rejected");
        ok &= ExpectTrue(stateRecovered == spectre::es2025::PromiseModule::State::Fulfilled,
                         "Recovery fulfilled");
        ok &= ExpectTrue(rejectPayload.invoked, "Rejection handler invoked");

        spectre::es2025::PromiseModule::Handle cancelled = 0;
        ok &= ExpectStatus(module->CreatePromise(cancelled, {"cancel"}), StatusCode::Ok, "Create cancellable");
        ok &= ExpectTrue(module->Cancel(cancelled), "Cancel promise");
        runtime->Tick({0.0, 1});
        std::vector<spectre::es2025::PromiseModule::SettledPromise> cancelDrain;
        module->DrainSettled(cancelDrain);
        ok &= ExpectTrue(cancelDrain.size() == 1, "Cancellation recorded");
        if (!cancelDrain.empty()) {
            ok &= ExpectTrue(cancelDrain.front().state == spectre::es2025::PromiseModule::State::Cancelled,
                             "Cancellation state");
        }

        const auto &metrics = module->GetMetrics();
        ok &= ExpectTrue(metrics.rejected >= 2, "Metrics rejected count");
        ok &= ExpectTrue(metrics.cancelled >= 1, "Metrics cancelled count");

        ok &= ExpectStatus(module->Release(root), StatusCode::Ok, "Release root");
        ok &= ExpectStatus(module->Release(propagated), StatusCode::Ok, "Release propagated");
        ok &= ExpectStatus(module->Release(recovered), StatusCode::Ok, "Release recovered");
        ok &= ExpectStatus(module->Release(cancelled), StatusCode::Ok, "Release cancelled");
        return ok;
    }

    bool FunctionModuleRegistersAndInvokes() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *functionModule = dynamic_cast<spectre::es2025::FunctionModule *>(environment.FindModule("Function"));
        ok &= ExpectTrue(functionModule != nullptr, "Function module available");
        if (!functionModule) {
            return false;
        }
        auto status = functionModule->RegisterHostFunction("sum", AddCallback);
        ok &= ExpectStatus(status, StatusCode::Ok, "Register sum");
        status = functionModule->RegisterHostFunction("echo", EchoCallback);
        ok &= ExpectStatus(status, StatusCode::Ok, "Register echo");
        std::string result;
        std::string diagnostics;
        status = functionModule->
                InvokeHostFunction("sum", std::vector<std::string>{"4", "5", "6"}, result, diagnostics);
        ok &= ExpectStatus(status, StatusCode::Ok, "Invoke sum");
        ok &= ExpectTrue(result == "15", "Sum result");
        status = functionModule->InvokeHostFunction("echo", std::vector<std::string>{"spectre"}, result, diagnostics);
        ok &= ExpectStatus(status, StatusCode::Ok, "Invoke echo");
        ok &= ExpectTrue(result == "spectre", "Echo result");
        const auto *stats = functionModule->GetStats("sum");
        ok &= ExpectTrue(stats != nullptr, "Stats available");
        if (stats != nullptr) {
            ok &= ExpectTrue(stats->callCount == 1, "Sum call count");
        }
        return ok;
    }

    bool FunctionModuleHandlesDuplicatesAndRemoval() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *functionModule = dynamic_cast<spectre::es2025::FunctionModule *>(environment.FindModule("Function"));
        ok &= ExpectTrue(functionModule != nullptr, "Function module available");
        if (!functionModule) {
            return false;
        }
        auto status = functionModule->RegisterHostFunction("sum", AddCallback);
        ok &= ExpectStatus(status, StatusCode::Ok, "Register sum");
        status = functionModule->RegisterHostFunction("sum", EchoCallback);
        ok &= ExpectStatus(status, StatusCode::AlreadyExists, "Duplicate rejected");
        status = functionModule->RegisterHostFunction("sum", EchoCallback, nullptr, true);
        ok &= ExpectStatus(status, StatusCode::Ok, "Overwrite allowed");
        status = functionModule->RemoveHostFunction("sum");
        ok &= ExpectStatus(status, StatusCode::Ok, "Remove sum");
        ok &= ExpectTrue(!functionModule->HasHostFunction("sum"), "Sum removed");
        std::string result;
        std::string diagnostics;
        status = functionModule->InvokeHostFunction("sum", std::vector<std::string>{}, result, diagnostics);
        ok &= ExpectStatus(status, StatusCode::NotFound, "Invoke removed function");
        functionModule->Clear();
        ok &= ExpectTrue(functionModule->RegisteredNames().empty(), "Registry cleared");
        return ok;
    }

    bool FunctionModuleGpuToggle() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *functionModule = dynamic_cast<spectre::es2025::FunctionModule *>(environment.FindModule("Function"));
        ok &= ExpectTrue(functionModule != nullptr, "Function module available");
        if (!functionModule) {
            return false;
        }
        ok &= ExpectTrue(!functionModule->GpuEnabled(), "GPU disabled");
        auto config = runtime->Config();
        config.enableGpuAcceleration = true;
        auto status = runtime->Reconfigure(config);
        ok &= ExpectStatus(status, StatusCode::Ok, "Runtime reconfigure");
        environment.OptimizeGpu(true);
        ok &= ExpectTrue(functionModule->GpuEnabled(), "GPU enabled");
        return ok;
    }


    bool IteratorModuleHandlesRangeListAndCustom() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *iteratorModule = dynamic_cast<spectre::es2025::IteratorModule *>(environment.FindModule("Iterator"));
        ok &= ExpectTrue(iteratorModule != nullptr, "Iterator module available");
        if (!iteratorModule) {
            return false;
        }

        spectre::es2025::IteratorModule::Handle rangeHandle = 0;
        spectre::es2025::IteratorModule::RangeConfig rangeConfig{0, 5, 1, false};
        ok &= ExpectStatus(iteratorModule->CreateRange(rangeConfig, rangeHandle), StatusCode::Ok,
                           "Create range iterator");
        std::array<spectre::es2025::IteratorModule::Result, 8> rangeBuffer{};
        auto produced = iteratorModule->Drain(rangeHandle, rangeBuffer);
        ok &= ExpectTrue(produced == 6, "Range drain produced values plus sentinel");
        for (std::size_t i = 0; i < 5 && i < produced; ++i) {
            ok &= ExpectTrue(rangeBuffer[i].hasValue, "Range produced value");
            ok &= ExpectTrue(!rangeBuffer[i].done, "Range entry not done");
            ok &= ExpectTrue(rangeBuffer[i].value.AsNumber(-1.0) == static_cast<double>(i), "Range sequence");
        }
        ok &= ExpectTrue(rangeBuffer[produced - 1].done, "Range sentinel done");
        ok &= ExpectTrue(!rangeBuffer[produced - 1].hasValue, "Range sentinel empty");
        ok &= ExpectTrue(iteratorModule->Destroy(rangeHandle), "Destroy range iterator");

        std::vector<spectre::es2025::Value> names;
        names.emplace_back(spectre::es2025::Value::String("alpha"));
        names.emplace_back(spectre::es2025::Value::String("beta"));
        names.emplace_back(spectre::es2025::Value::String("gamma"));
        spectre::es2025::IteratorModule::Handle listHandle = 0;
        ok &= ExpectStatus(iteratorModule->CreateList(names, listHandle), StatusCode::Ok, "Create list iterator");
        std::array<std::string, 3> expectedNames{"alpha", "beta", "gamma"};
        std::size_t listIndex = 0;
        auto listEntry = iteratorModule->Next(listHandle);
        while (!listEntry.done) {
            ok &= ExpectTrue(listIndex < expectedNames.size(), "List bounds");
            ok &= ExpectTrue(listEntry.hasValue, "List entry has value");
            if (listIndex < expectedNames.size()) {
                ok &= ExpectTrue(listEntry.value.AsString() == expectedNames[listIndex], "List preserves order");
            }
            listEntry = iteratorModule->Next(listHandle);
            ++listIndex;
        }
        ok &= ExpectTrue(listIndex == expectedNames.size(), "Consumed all list values");
        ok &= ExpectTrue(iteratorModule->Destroy(listHandle), "Destroy list iterator");

        struct CustomState {
            double current;
            double step;
            int resets;
            int closes;
            int destroys;
        } custom{0.0, 0.5, 0, 0, 0};

        auto nextFn = [](void *state) -> spectre::es2025::IteratorModule::Result {
            auto *ptr = static_cast<CustomState *>(state);
            spectre::es2025::IteratorModule::Result result;
            if (ptr == nullptr) {
                result.done = true;
                result.hasValue = false;
                return result;
            }
            if (ptr->current >= 1.5) {
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
            ptr->current = 0.0;
            ptr->resets += 1;
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
        ok &= ExpectStatus(iteratorModule->CreateCustom(customConfig, customHandle), StatusCode::Ok,
                           "Create custom iterator");
        iteratorModule->Reset(customHandle);
        auto customEntry = iteratorModule->Next(customHandle);
        int customCount = 0;
        while (!customEntry.done) {
            ok &= ExpectTrue(customEntry.hasValue, "Custom iterator yields value");
            customEntry = iteratorModule->Next(customHandle);
            customCount += 1;
        }
        iteratorModule->Close(customHandle);
        iteratorModule->Destroy(customHandle);
        ok &= ExpectTrue(custom.resets == 1, "Custom reset invoked");
        ok &= ExpectTrue(custom.closes == 1, "Custom close invoked");
        ok &= ExpectTrue(custom.destroys == 1, "Custom destroy invoked");
        ok &= ExpectTrue(customCount > 0, "Custom produced values");
        ok &= ExpectTrue(iteratorModule->ActiveIterators() == 0, "Iterators released");
        return ok;
    }

    bool GeneratorModuleRunsAndBridges() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *generatorModule = dynamic_cast<spectre::es2025::GeneratorModule *>(environment.FindModule("Generator"));
        ok &= ExpectTrue(generatorModule != nullptr, "Generator module available");
        auto *iteratorModule = dynamic_cast<spectre::es2025::IteratorModule *>(environment.FindModule("Iterator"));
        ok &= ExpectTrue(iteratorModule != nullptr, "Iterator module available");
        if (!generatorModule || !iteratorModule) {
            return false;
        }

        struct GeneratorState {
            std::size_t index;
            std::array<spectre::es2025::Value, 3> values;
        } state{
                    0,
                    {
                        spectre::es2025::Value::Number(1.0), spectre::es2025::Value::Number(2.0),
                        spectre::es2025::Value::String("done")
                    }
                };

        auto resetFn = [](void *raw) {
            auto *ptr = static_cast<GeneratorState *>(raw);
            if (ptr == nullptr) {
                return;
            }
            ptr->index = 0;
        };

        auto stepFn = [](void *raw, spectre::es2025::GeneratorModule::ExecutionContext &context) {
            auto *ptr = static_cast<GeneratorState *>(raw);
            if (ptr == nullptr) {
                context.done = true;
                context.hasValue = false;
                return;
            }
            context.requestingInput = false;
            context.nextResumePoint = 0;
            if (ptr->index < ptr->values.size() - 1) {
                context.yieldValue = ptr->values[ptr->index];
                context.hasValue = true;
                context.done = false;
                ptr->index += 1;
                return;
            }
            if (ptr->index == ptr->values.size() - 1) {
                context.yieldValue = ptr->values[ptr->index];
                context.hasValue = true;
                context.done = true;
                ptr->index += 1;
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
        descriptor.name = "runtime-generator";
        descriptor.resumePoint = 0;

        spectre::es2025::GeneratorModule::Handle handle = 0;
        ok &= ExpectStatus(generatorModule->Register(descriptor, handle), StatusCode::Ok, "Register generator");
        auto first = generatorModule->Resume(handle);
        ok &= ExpectTrue(first.hasValue, "First resume has value");
        ok &= ExpectTrue(!first.done, "First resume not done");
        ok &= ExpectTrue(first.value == state.values[0], "First value matches");
        auto second = generatorModule->Resume(handle);
        ok &= ExpectTrue(second.hasValue, "Second resume has value");
        ok &= ExpectTrue(!second.done, "Second resume not done");
        ok &= ExpectTrue(second.value == state.values[1], "Second value matches");
        auto final = generatorModule->Resume(handle);
        ok &= ExpectTrue(final.hasValue, "Final resume has value");
        ok &= ExpectTrue(final.done, "Final resume done");
        ok &= ExpectTrue(final.value == state.values[2], "Final value matches");
        auto exhausted = generatorModule->Resume(handle);
        ok &= ExpectTrue(exhausted.done, "Exhausted resume done");
        ok &= ExpectTrue(!exhausted.hasValue, "Exhausted resume empty");
        ok &= ExpectTrue(generatorModule->Completed(handle), "Generator reports completed");
        ok &= ExpectTrue(generatorModule->ResumeCount(handle) == 3, "Resume count tracked");

        generatorModule->Reset(handle);
        auto resetStep = generatorModule->Resume(handle);
        ok &= ExpectTrue(resetStep.hasValue, "Reset resume has value");
        ok &= ExpectTrue(resetStep.value == state.values[0], "Reset start value");
        generatorModule->Reset(handle);

        std::uint32_t iteratorHandle = 0;
        ok &= ExpectStatus(generatorModule->CreateIteratorBridge(handle, *iteratorModule, iteratorHandle),
                           StatusCode::Ok, "Create iterator bridge");
        std::array<spectre::es2025::IteratorModule::Result, 8> bridgeBuffer{};
        auto bridgeProduced = iteratorModule->Drain(iteratorHandle, bridgeBuffer);
        ok &= ExpectTrue(bridgeProduced == 3, "Bridge produced generator values");
        ok &= ExpectTrue(bridgeBuffer[0].value == state.values[0], "Bridge first value");
        ok &= ExpectTrue(!bridgeBuffer[0].done, "Bridge first not done");
        ok &= ExpectTrue(bridgeBuffer[1].value == state.values[1], "Bridge second value");
        ok &= ExpectTrue(!bridgeBuffer[1].done, "Bridge second not done");
        ok &= ExpectTrue(bridgeBuffer[2].value == state.values[2], "Bridge final value");
        ok &= ExpectTrue(bridgeBuffer[2].done, "Bridge final done");
        ok &= ExpectTrue(iteratorModule->Destroy(iteratorHandle), "Destroy bridge iterator");
        ok &= ExpectTrue(generatorModule->Destroy(handle), "Destroy generator");
        ok &= ExpectTrue(generatorModule->ActiveGenerators() == 0, "All generators released");
        return ok;
    }

    bool ArrayModuleCreatesDenseAndTracksMetrics() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *arrayModule = dynamic_cast<spectre::es2025::ArrayModule *>(environment.FindModule("Array"));
        ok &= ExpectTrue(arrayModule != nullptr, "Array module available");
        if (!arrayModule) {
            return false;
        }
        spectre::es2025::ArrayModule::Handle handle = 0;
        auto status = arrayModule->CreateDense("dense-primary", 8, handle);
        ok &= ExpectStatus(status, StatusCode::Ok, "Create dense array");
        ok &= ExpectTrue(handle != 0, "Handle assigned");
        status = arrayModule->PushNumber(handle, 42.0);
        ok &= ExpectStatus(status, StatusCode::Ok, "Push number");
        status = arrayModule->PushString(handle, "alpha");
        ok &= ExpectStatus(status, StatusCode::Ok, "Push string");
        spectre::es2025::ArrayModule::Value value;
        status = arrayModule->Get(handle, 0, value);
        ok &= ExpectStatus(status, StatusCode::Ok, "Get first element");
        ok &= ExpectTrue(value.kind == spectre::es2025::ArrayModule::Value::Kind::Number, "Value kind number");
        status = arrayModule->Get(handle, 1, value);
        ok &= ExpectStatus(status, StatusCode::Ok, "Get second element");
        ok &= ExpectTrue(value.kind == spectre::es2025::ArrayModule::Value::Kind::String, "Value kind string");
        ok &= ExpectTrue(arrayModule->Length(handle) == 2, "Length two");
        const auto &metrics = arrayModule->GetMetrics();
        ok &= ExpectTrue(metrics.denseCount >= 1, "Dense count tracked");
        ok &= ExpectTrue(metrics.denseLength >= 2, "Dense length tracked");
        return ok;
    }

    bool ArrayModuleSupportsSparseConversions() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *arrayModule = dynamic_cast<spectre::es2025::ArrayModule *>(environment.FindModule("Array"));
        ok &= ExpectTrue(arrayModule != nullptr, "Array module available");
        if (!arrayModule) {
            return false;
        }
        spectre::es2025::ArrayModule::Handle handle = 0;
        auto status = arrayModule->CreateDense("dense-to-sparse", 4, handle);
        ok &= ExpectStatus(status, StatusCode::Ok, "Create dense");
        status = arrayModule->Set(handle, 0, spectre::es2025::ArrayModule::Value(10.0));
        ok &= ExpectStatus(status, StatusCode::Ok, "Set base value");
        status = arrayModule->Set(handle, 64, spectre::es2025::ArrayModule::Value("tail"));
        ok &= ExpectStatus(status, StatusCode::Ok, "Set sparse index");
        ok &= ExpectTrue(arrayModule->KindOf(handle) == spectre::es2025::ArrayModule::StorageKind::Sparse,
                         "Converted to sparse");
        spectre::es2025::ArrayModule::Value sparseValue;
        ok &= ExpectTrue(arrayModule->Length(handle) >= 65, "Sparse length expanded");
        status = arrayModule->SortLexicographic(handle, true);
        ok &= ExpectStatus(status, StatusCode::Ok, "Sort lexicographic");
        ok &= ExpectTrue(arrayModule->KindOf(handle) == spectre::es2025::ArrayModule::StorageKind::Dense,
                         "Promoted to dense");
        const auto &metrics = arrayModule->GetMetrics();
        ok &= ExpectTrue(metrics.transitionsToSparse >= 1, "Sparse transition counted");
        ok &= ExpectTrue(metrics.transitionsToDense >= 1, "Dense transition counted");
        return ok;
    }

    bool ArrayModuleConcatSliceAndBinarySearch() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *arrayModule = dynamic_cast<spectre::es2025::ArrayModule *>(environment.FindModule("Array"));
        ok &= ExpectTrue(arrayModule != nullptr, "Array module available");
        if (!arrayModule) {
            return false;
        }
        spectre::es2025::ArrayModule::Handle a = 0;
        spectre::es2025::ArrayModule::Handle b = 0;
        ok &= ExpectStatus(arrayModule->CreateDense("concat-a", 4, a), StatusCode::Ok, "Create first array");
        ok &= ExpectStatus(arrayModule->CreateDense("concat-b", 4, b), StatusCode::Ok, "Create second array");
        ok &= ExpectStatus(arrayModule->PushNumber(a, 3.0), StatusCode::Ok, "Push into a");
        ok &= ExpectStatus(arrayModule->PushNumber(a, 1.0), StatusCode::Ok, "Push into a");
        ok &= ExpectStatus(arrayModule->PushNumber(a, 5.0), StatusCode::Ok, "Push into a");
        ok &= ExpectStatus(arrayModule->PushNumber(b, 4.0), StatusCode::Ok, "Push into b");
        ok &= ExpectStatus(arrayModule->PushNumber(b, 2.0), StatusCode::Ok, "Push into b");
        ok &= ExpectStatus(arrayModule->Concat(a, b), StatusCode::Ok, "Concat arrays");
        ok &= ExpectTrue(arrayModule->Length(a) == 5, "Concat length");
        std::vector<spectre::es2025::ArrayModule::Value> slice;
        ok &= ExpectStatus(arrayModule->Slice(a, 1, 4, slice), StatusCode::Ok, "Slice values");
        ok &= ExpectTrue(slice.size() == 3, "Slice size");
        ok &= ExpectStatus(arrayModule->SortNumeric(a, true), StatusCode::Ok, "Sort numeric");
        std::size_t index = 0;
        ok &= ExpectStatus(arrayModule->BinarySearch(a, spectre::es2025::ArrayModule::Value(4.0), true, index),
                           StatusCode::Ok, "Binary search value");
        ok &= ExpectTrue(index < arrayModule->Length(a), "Binary search index range");
        return ok;
    }

    bool ArrayModuleCloneAndClear() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *arrayModule = dynamic_cast<spectre::es2025::ArrayModule *>(environment.FindModule("Array"));
        ok &= ExpectTrue(arrayModule != nullptr, "Array module available");
        if (!arrayModule) {
            return false;
        }
        spectre::es2025::ArrayModule::Handle original = 0;
        ok &= ExpectStatus(arrayModule->CreateDense("clone-source", 4, original), StatusCode::Ok, "Create source");
        ok &= ExpectStatus(arrayModule->PushNumber(original, 7.0), StatusCode::Ok, "Push number");
        ok &= ExpectStatus(arrayModule->PushString(original, "node"), StatusCode::Ok, "Push string");
        ok &= ExpectTrue(arrayModule->Length(original) == 2, "Source length established");
        ok &= ExpectTrue(arrayModule->KindOf(original) == spectre::es2025::ArrayModule::StorageKind::Dense,
                         "Source dense kind");
        spectre::es2025::ArrayModule::Handle clone = 0;
        ok &= ExpectStatus(arrayModule->Clone(original, "clone-copy", clone), StatusCode::Ok, "Clone array");
        ok &= ExpectTrue(clone != 0, "Clone handle assigned");
        ok &= ExpectTrue(arrayModule->Has(clone), "Clone registered");
        ok &= ExpectTrue(arrayModule->KindOf(clone) == spectre::es2025::ArrayModule::StorageKind::Dense,
                         "Clone dense kind");
        ok &= ExpectTrue(arrayModule->Length(clone) == arrayModule->Length(original), "Clone length");
        spectre::es2025::ArrayModule::Value value;
        std::vector<spectre::es2025::ArrayModule::Value> cloneSlice;
        ok &= ExpectStatus(arrayModule->Slice(clone, 0, arrayModule->Length(original), cloneSlice), StatusCode::Ok,
                           "Clone slice status");
        ok &= ExpectTrue(cloneSlice.size() == arrayModule->Length(original), "Clone slice size");
        bool cloneHasPayload = false;
        for (const auto &entry: cloneSlice) {
            if (entry.ToString() == "node") {
                cloneHasPayload = true;
                break;
            }
        }
        ok &= ExpectTrue(cloneHasPayload, "Clone payload copied");
        ok &= ExpectStatus(arrayModule->Get(clone, 1, value), StatusCode::Ok, "Clone element");
        ok &= ExpectTrue(value.kind == spectre::es2025::ArrayModule::Value::Kind::String, "Clone value kind");
        ok &= ExpectStatus(arrayModule->Clear(original), StatusCode::Ok, "Clear source");
        ok &= ExpectTrue(arrayModule->Length(original) == 0, "Source cleared");
        const auto &metrics = arrayModule->GetMetrics();
        ok &= ExpectTrue(metrics.clones >= 1, "Clone metric updated");
        return ok;
    }

    bool AtomicsModuleAllocatesAndAtomicallyUpdates() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        auto &environment = runtime->EsEnvironment();
        auto *atomicsModule = dynamic_cast<spectre::es2025::AtomicsModule *>(environment.FindModule("Atomics"));
        bool ok = ExpectTrue(atomicsModule != nullptr, "Atomics module available");
        if (!atomicsModule) {
            return false;
        }
        spectre::es2025::AtomicsModule::Handle handle = 0;
        ok &= ExpectStatus(atomicsModule->CreateBuffer("test.atomic", 8, handle), StatusCode::Ok, "Create buffer");
        ok &= ExpectTrue(handle != 0, "Buffer handle assigned");
        ok &= ExpectTrue(atomicsModule->Capacity(handle) == 8, "Capacity matches request");
        ok &= ExpectStatus(atomicsModule->Fill(handle, 1), StatusCode::Ok, "Fill baseline");
        ok &= ExpectStatus(atomicsModule->Store(handle, 3, 42, spectre::es2025::AtomicsModule::MemoryOrder::Release),
                           StatusCode::Ok, "Store lane");
        std::int64_t value = 0;
        ok &= ExpectStatus(atomicsModule->Load(handle, 3, value, spectre::es2025::AtomicsModule::MemoryOrder::Acquire),
                           StatusCode::Ok, "Load lane");
        ok &= ExpectTrue(value == 42, "Load observed store");
        std::int64_t previous = 0;
        ok &= ExpectStatus(atomicsModule->Add(handle, 3, 5, previous), StatusCode::Ok, "Fetch add");
        ok &= ExpectTrue(previous == 42, "Fetch add previous value");
        ok &= ExpectStatus(atomicsModule->Sub(handle, 3, 2, previous), StatusCode::Ok, "Fetch sub");
        ok &= ExpectTrue(previous == 47, "Fetch sub previous value");
        ok &= ExpectStatus(atomicsModule->Or(handle, 5, 0xff, previous), StatusCode::Ok, "Fetch or");
        ok &= ExpectTrue(previous == 1, "Fetch or previous value");
        ok &= ExpectStatus(atomicsModule->Xor(handle, 5, 0x1, previous), StatusCode::Ok, "Fetch xor");
        ok &= ExpectTrue(previous == 0xff, "Fetch xor previous value");
        ok &= ExpectStatus(atomicsModule->And(handle, 5, 0x3, previous), StatusCode::Ok, "Fetch and");
        ok &= ExpectTrue(previous == 0xfe, "Fetch and previous value");
        ok &= ExpectStatus(atomicsModule->Exchange(handle, 2, 512, previous), StatusCode::Ok, "Exchange lane");
        ok &= ExpectTrue(previous == 1, "Exchange previous value");
        ok &= ExpectStatus(atomicsModule->CompareExchange(handle, 3, 11, 99, previous), StatusCode::Ok, "CAS miss");
        ok &= ExpectTrue(previous == 45, "CAS miss returns current");
        ok &= ExpectStatus(atomicsModule->CompareExchange(handle, 3, 45, 99, previous), StatusCode::Ok, "CAS hit");
        ok &= ExpectTrue(previous == 45, "CAS hit returns expected");
        std::vector<std::int64_t> snapshot;
        ok &= ExpectStatus(atomicsModule->Snapshot(handle, snapshot), StatusCode::Ok, "Snapshot buffer");
        ok &= ExpectTrue(snapshot.size() == 8, "Snapshot size matches length");
        ok &= ExpectTrue(snapshot[3] == 99, "Snapshot captured CAS result");
        const auto &metrics = atomicsModule->Metrics();
        ok &= ExpectTrue(metrics.allocations >= 1, "Allocation metric updated");
        ok &= ExpectTrue(metrics.loadOps >= 1, "Load metric updated");
        ok &= ExpectTrue(metrics.storeOps >= 9, "Store metric includes fill");
        ok &= ExpectTrue(metrics.rmwOps >= 7, "RMW metric updated");
        ok &= ExpectTrue(metrics.compareExchangeHits >= 1, "CAS hits tracked");
        ok &= ExpectTrue(metrics.compareExchangeMisses >= 1, "CAS misses tracked");
        ok &= ExpectStatus(atomicsModule->DestroyBuffer(handle), StatusCode::Ok, "Destroy buffer");
        ok &= ExpectTrue(!atomicsModule->Has(handle), "Handle removed after destroy");
        return ok;
    }


    bool ArrayBufferModuleAllocatesAndPools() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        auto &environment = runtime->EsEnvironment();
        auto *bufferModule = dynamic_cast<spectre::es2025::ArrayBufferModule *>(environment.FindModule("ArrayBuffer"));
        bool ok = ExpectTrue(bufferModule != nullptr, "ArrayBuffer module available");
        if (!bufferModule) {
            return false;
        }
        spectre::es2025::ArrayBufferModule::Handle bufferHandle = 0;
        ok &= ExpectStatus(bufferModule->Create("demo.buffer", 256, bufferHandle), StatusCode::Ok, "Create buffer");
        ok &= ExpectTrue(bufferModule->Has(bufferHandle), "Buffer registered");
        ok &= ExpectTrue(bufferModule->ByteLength(bufferHandle) == 256, "Byte length reported");
        std::vector<std::uint8_t> scratch(256, 0xCD);
        ok &= ExpectStatus(bufferModule->CopyOut(bufferHandle, 0, scratch.data(), scratch.size()), StatusCode::Ok,
                           "CopyOut fresh buffer");
        bool zeroed = std::all_of(scratch.begin(), scratch.end(), [](std::uint8_t value) {
            return value == 0;
        });
        ok &= ExpectTrue(zeroed, "Fresh buffer zero initialised");
        for (std::size_t i = 0; i < scratch.size(); ++i) {
            scratch[i] = static_cast<std::uint8_t>(i & 0xFF);
        }
        ok &= ExpectStatus(bufferModule->CopyIn(bufferHandle, 0, scratch.data(), scratch.size()), StatusCode::Ok,
                           "CopyIn pattern");
        spectre::es2025::ArrayBufferModule::Handle sliceHandle = 0;
        ok &= ExpectStatus(bufferModule->Slice(bufferHandle, 32, 64, "demo.slice", sliceHandle), StatusCode::Ok,
                           "Create slice");
        ok &= ExpectTrue(bufferModule->ByteLength(sliceHandle) == 32, "Slice byte length");
        std::array<std::uint8_t, 32> sliceData{};
        ok &= ExpectStatus(bufferModule->CopyOut(sliceHandle, 0, sliceData.data(), sliceData.size()), StatusCode::Ok,
                           "CopyOut slice");
        bool contiguous = true;
        for (std::size_t i = 0; i < sliceData.size(); ++i) {
            contiguous &= sliceData[i] == static_cast<std::uint8_t>((i + 32) & 0xFF);
        }
        ok &= ExpectTrue(contiguous, "Slice matches source window");
        spectre::es2025::ArrayBufferModule::Handle cloneHandle = 0;
        ok &= ExpectStatus(bufferModule->Clone(bufferHandle, "demo.clone", cloneHandle), StatusCode::Ok,
                           "Clone buffer");
        std::vector<std::uint8_t> cloneData(256, 0);
        ok &= ExpectStatus(bufferModule->CopyOut(cloneHandle, 0, cloneData.data(), cloneData.size()), StatusCode::Ok,
                           "CopyOut clone");
        bool cloneMatches = true;
        for (std::size_t i = 0; i < cloneData.size(); ++i) {
            cloneMatches &= cloneData[i] == static_cast<std::uint8_t>(i & 0xFF);
        }
        ok &= ExpectTrue(cloneMatches, "Clone preserves payload");
        ok &= ExpectStatus(bufferModule->Fill(bufferHandle, 0xAA), StatusCode::Ok, "Fill buffer");
        ok &= ExpectStatus(bufferModule->CopyOut(bufferHandle, 0, scratch.data(), scratch.size()), StatusCode::Ok,
                           "CopyOut filled buffer");
        bool filled = std::all_of(scratch.begin(), scratch.end(), [](std::uint8_t value) {
            return value == 0xAA;
        });
        ok &= ExpectTrue(filled, "Fill wrote expected value");
        ok &= ExpectStatus(bufferModule->CopyOut(cloneHandle, 0, cloneData.data(), cloneData.size()), StatusCode::Ok,
                           "Re-read clone");
        bool cloneUnaffected = true;
        for (std::size_t i = 0; i < cloneData.size(); ++i) {
            cloneUnaffected &= cloneData[i] == static_cast<std::uint8_t>(i & 0xFF);
        }
        ok &= ExpectTrue(cloneUnaffected, "Clone isolated from source fill");
        const auto metricsBeforeDestroy = bufferModule->GetMetrics();
        ok &= ExpectStatus(bufferModule->Destroy(sliceHandle), StatusCode::Ok, "Destroy slice");
        ok &= ExpectStatus(bufferModule->Destroy(bufferHandle), StatusCode::Ok, "Destroy primary buffer");
        spectre::es2025::ArrayBufferModule::Handle reuseHandle = 0;
        ok &= ExpectStatus(bufferModule->Create("demo.reuse", 256, reuseHandle), StatusCode::Ok, "Reuse allocation");
        const auto metricsAfterReuse = bufferModule->GetMetrics();
        ok &= ExpectTrue(metricsAfterReuse.poolReuses >= metricsBeforeDestroy.poolReuses + 1,
                         "Pool reuse accounted");
        ok &= ExpectTrue(metricsAfterReuse.bytesInUse >= 256, "Bytes in use tracked");
        ok &= ExpectStatus(bufferModule->Destroy(reuseHandle), StatusCode::Ok, "Destroy reused buffer");
        ok &= ExpectStatus(bufferModule->Destroy(cloneHandle), StatusCode::Ok, "Destroy clone");
        return ok;
    }

    bool ArrayBufferModuleResizesAndDetaches() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        auto &environment = runtime->EsEnvironment();
        auto *bufferModule = dynamic_cast<spectre::es2025::ArrayBufferModule *>(environment.FindModule("ArrayBuffer"));
        bool ok = ExpectTrue(bufferModule != nullptr, "ArrayBuffer module available for resize test");
        if (!bufferModule) {
            return false;
        }
        spectre::es2025::ArrayBufferModule::Handle sourceHandle = 0;
        ok &= ExpectStatus(bufferModule->Create("worker.source", 128, sourceHandle), StatusCode::Ok, "Create source");
        std::array<std::uint8_t, 128> basePattern{};
        for (std::size_t i = 0; i < basePattern.size(); ++i) {
            basePattern[i] = static_cast<std::uint8_t>((i * 3) & 0xFF);
        }
        ok &= ExpectStatus(bufferModule->CopyIn(sourceHandle, 0, basePattern.data(), basePattern.size()),
                           StatusCode::Ok,
                           "Seed pattern");
        ok &= ExpectStatus(bufferModule->Resize(sourceHandle, 512, true), StatusCode::Ok, "Grow preserving data");
        ok &= ExpectTrue(bufferModule->ByteLength(sourceHandle) == 512, "Growth length reported");
        std::vector<std::uint8_t> growCheck(512, 0);
        ok &= ExpectStatus(bufferModule->CopyOut(sourceHandle, 0, growCheck.data(), growCheck.size()), StatusCode::Ok,
                           "CopyOut grown buffer");
        bool prefixPreserved = true;
        for (std::size_t i = 0; i < basePattern.size(); ++i) {
            prefixPreserved &= growCheck[i] == basePattern[i];
        }
        bool tailZero = true;
        for (std::size_t i = basePattern.size(); i < growCheck.size(); ++i) {
            tailZero &= growCheck[i] == 0;
        }
        ok &= ExpectTrue(prefixPreserved && tailZero, "Resize preserved prefix and zeroed tail");
        ok &= ExpectStatus(bufferModule->Resize(sourceHandle, 64, false), StatusCode::Ok, "Shrink without preserve");
        ok &= ExpectTrue(bufferModule->ByteLength(sourceHandle) == 64, "Shrink length reported");
        std::array<std::uint8_t, 64> shrinkCheck{};
        ok &= ExpectStatus(bufferModule->CopyOut(sourceHandle, 0, shrinkCheck.data(), shrinkCheck.size()),
                           StatusCode::Ok,
                           "CopyOut shrunk buffer");
        bool shrinkCleared = std::all_of(shrinkCheck.begin(), shrinkCheck.end(), [](std::uint8_t value) {
            return value == 0;
        });
        ok &= ExpectTrue(shrinkCleared, "Shrink cleared contents when not preserving");
        spectre::es2025::ArrayBufferModule::Handle targetHandle = 0;
        ok &= ExpectStatus(bufferModule->Create("worker.target", 96, targetHandle), StatusCode::Ok,
                           "Create target buffer");
        std::array<std::uint8_t, 96> targetScratch{};
        targetScratch.fill(0);
        ok &= ExpectStatus(bufferModule->Fill(targetHandle, 0), StatusCode::Ok, "Zero target");
        ok &= ExpectStatus(bufferModule->CopyIn(sourceHandle, 0, basePattern.data(), 64), StatusCode::Ok,
                           "Overwrite source window");
        ok &= ExpectStatus(bufferModule->CopyToBuffer(sourceHandle, targetHandle, 8, 16, 40), StatusCode::Ok,
                           "Copy between buffers");
        ok &= ExpectStatus(bufferModule->CopyOut(targetHandle, 0, targetScratch.data(), targetScratch.size()),
                           StatusCode::Ok, "CopyOut target after blit");
        bool blitValid = true;
        for (std::size_t i = 0; i < 40; ++i) {
            auto expected = static_cast<std::uint8_t>(((i + 8) * 3) & 0xFF);
            blitValid &= targetScratch[i + 16] == expected;
        }
        ok &= ExpectTrue(blitValid, "CopyToBuffer populated expected region");
        ok &= ExpectStatus(bufferModule->Detach(sourceHandle), StatusCode::Ok, "Detach source");
        ok &= ExpectTrue(bufferModule->Detached(sourceHandle), "Detach flag set");
        ok &= ExpectTrue(bufferModule->ByteLength(sourceHandle) == 0, "Detached byteLength zero");
        auto detachStatus = bufferModule->CopyIn(sourceHandle, 0, basePattern.data(), 8);
        ok &= ExpectStatus(detachStatus, StatusCode::InvalidArgument, "Detached rejects writes");
        const auto &metrics = bufferModule->GetMetrics();
        ok &= ExpectTrue(metrics.resizes >= 2, "Resize metric tracked");
        ok &= ExpectTrue(metrics.detaches >= 1, "Detach metric tracked");
        ok &= ExpectTrue(metrics.copyBetweenBuffers >= 1, "CopyBetweenBuffers metric tracked");
        ok &= ExpectStatus(bufferModule->Destroy(sourceHandle), StatusCode::Ok, "Destroy detached source");
        ok &= ExpectStatus(bufferModule->Destroy(targetHandle), StatusCode::Ok, "Destroy target");
        return ok;
    }

    bool BooleanModuleCastsAndBoxes() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        auto &environment = runtime->EsEnvironment();
        auto *booleanModule = dynamic_cast<spectre::es2025::BooleanModule *>(environment.FindModule("Boolean"));
        bool ok = ExpectTrue(booleanModule != nullptr, "Boolean module available");
        if (!booleanModule) {
            return false;
        }
        ok &= ExpectTrue(booleanModule->ToBoolean(3.14), "Positive double is truthy");
        ok &= ExpectTrue(!booleanModule->ToBoolean(0.0), "Zero double is falsy");
        ok &= ExpectTrue(booleanModule->ToBoolean(static_cast<std::int64_t>(-42)), "Negative integer truthy");
        ok &= ExpectTrue(!booleanModule->ToBoolean(std::string_view("  false  ")), "False string is falsy");
        ok &= ExpectTrue(booleanModule->ToBoolean("yes"), "Yes string is truthy");
        auto canonicalTrue = booleanModule->Box(true);
        auto canonicalFalse = booleanModule->Box(false);
        ok &= ExpectTrue(booleanModule->Has(canonicalTrue), "Canonical true registered");
        bool boxedValue = false;
        ok &= ExpectStatus(booleanModule->ValueOf(canonicalTrue, boxedValue), StatusCode::Ok, "Canonical read");
        ok &= ExpectTrue(boxedValue, "Canonical true value");
        spectre::es2025::BooleanModule::Handle handle = 0;
        ok &= ExpectStatus(booleanModule->Create("temp.flag", false, handle), StatusCode::Ok, "Create boxed flag");
        ok &= ExpectTrue(booleanModule->Has(handle), "Handle registered");
        ok &= ExpectStatus(booleanModule->Set(handle, true), StatusCode::Ok, "Set flag");
        ok &= ExpectStatus(booleanModule->Toggle(handle), StatusCode::Ok, "Toggle flag");
        ok &= ExpectStatus(booleanModule->ValueOf(handle, boxedValue), StatusCode::Ok, "Read toggled flag");
        ok &= ExpectTrue(!boxedValue, "Flag toggled back to false");
        const auto &metrics = booleanModule->Metrics();
        ok &= ExpectTrue(metrics.conversions >= 3, "Conversions tracked");
        ok &= ExpectTrue(metrics.allocations >= 1, "Allocations tracked");
        ok &= ExpectTrue(metrics.canonicalHits >= 2, "Canonical hits tracked");
        ok &= ExpectStatus(booleanModule->Destroy(handle), StatusCode::Ok, "Destroy boxed flag");
        ok &= ExpectTrue(!booleanModule->Has(handle), "Box removed from cache");
        ok &= ExpectStatus(booleanModule->Destroy(canonicalTrue), StatusCode::InvalidArgument,
                           "Canonical box protected");
        return ok;
    }

    bool StringModuleHandlesInterningAndTransforms() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *modulePtr = environment.FindModule("String");
        auto *stringModule = dynamic_cast<spectre::es2025::StringModule *>(modulePtr);
        ok &= ExpectTrue(stringModule != nullptr, "String module available");
        if (!stringModule) {
            return false;
        }

        spectre::es2025::StringModule::Handle base = 0;
        ok &= ExpectStatus(stringModule->Create("demo.base", "  hello  ", base), StatusCode::Ok, "Create base string");
        auto baseView = stringModule->View(base);
        ok &= ExpectTrue(baseView == "  hello  ", "Base view matches input");

        ok &= ExpectStatus(stringModule->TrimAscii(base), StatusCode::Ok, "Trim whitespace");
        baseView = stringModule->View(base);
        ok &= ExpectTrue(baseView == "hello", "Trim applied");

        auto trimmedHash = stringModule->Hash(base);
        ok &= ExpectStatus(stringModule->ToUpperAscii(base), StatusCode::Ok, "Uppercase transform");
        baseView = stringModule->View(base);
        ok &= ExpectTrue(baseView == "HELLO", "Uppercase applied");
        ok &= ExpectTrue(stringModule->Hash(base) != trimmedHash, "Hash updated after uppercase");

        ok &= ExpectStatus(stringModule->Append(base, " WORLD"), StatusCode::Ok, "Append suffix");
        baseView = stringModule->View(base);
        ok &= ExpectTrue(baseView == "HELLO WORLD", "Append result correct");

        spectre::es2025::StringModule::Handle slice = 0;
        ok &= ExpectStatus(stringModule->Slice(base, 6, 5, "demo.slice", slice), StatusCode::Ok, "Slice substring");
        auto sliceView = stringModule->View(slice);
        ok &= ExpectTrue(sliceView == "WORLD", "Slice matches expectation");

        spectre::es2025::StringModule::Handle concat = 0;
        ok &= ExpectStatus(stringModule->Concat(base, slice, "demo.concat", concat), StatusCode::Ok, "Concat success");
        auto concatView = stringModule->View(concat);
        ok &= ExpectTrue(concatView == "HELLO WORLDWORLD", "Concat matches expectation");

        spectre::es2025::StringModule::Handle clone = 0;
        ok &= ExpectStatus(stringModule->Clone(slice, "demo.clone", clone), StatusCode::Ok, "Clone success");
        ok &= ExpectTrue(stringModule->View(clone) == "WORLD", "Clone view matches");

        spectre::es2025::StringModule::Handle internA = 0;
        ok &= ExpectStatus(stringModule->Intern("spectre.hero", internA), StatusCode::Ok, "Intern miss recorded");
        spectre::es2025::StringModule::Handle internB = 0;
        ok &= ExpectStatus(stringModule->Intern("spectre.hero", internB), StatusCode::Ok, "Intern hit recorded");
        ok &= ExpectTrue(internA == internB, "Intern returns canonical handle");

        ok &= ExpectStatus(stringModule->Release(clone), StatusCode::Ok, "Release clone");
        ok &= ExpectStatus(stringModule->Release(concat), StatusCode::Ok, "Release concat");
        ok &= ExpectStatus(stringModule->Release(slice), StatusCode::Ok, "Release slice");
        ok &= ExpectStatus(stringModule->Release(base), StatusCode::Ok, "Release base");
        ok &= ExpectTrue(!stringModule->Has(base), "Base handle removed");

        ok &= ExpectStatus(stringModule->Release(internA), StatusCode::Ok, "Release first intern reference");
        ok &= ExpectTrue(stringModule->Has(internB), "Intern handle held until final release");
        ok &= ExpectStatus(stringModule->Release(internB), StatusCode::Ok, "Release second intern reference");
        ok &= ExpectTrue(!stringModule->Has(internA), "Intern handle removed after final release");

        const auto &metrics = stringModule->GetMetrics();
        ok &= ExpectTrue(metrics.allocations >= 5, "String allocations counted");
        ok &= ExpectTrue(metrics.internHits >= 1, "Intern hits counted");
        ok &= ExpectTrue(metrics.internMisses >= 1, "Intern misses counted");
        ok &= ExpectTrue(metrics.transforms >= 3, "Transform operations counted");
        ok &= ExpectTrue(metrics.slices >= 2, "Slice operations counted");
        ok &= ExpectTrue(metrics.activeStrings == 0, "No active strings remaining");

        return ok;
    }

    bool MathModuleAcceleratesWorkloads() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *modulePtr = environment.FindModule("Math");
        auto *mathModule = dynamic_cast<spectre::es2025::MathModule *>(modulePtr);
        ok &= ExpectTrue(mathModule != nullptr, "Math module available");
        if (!mathModule) {
            return false;
        }

        double angle = 1.2345;
        auto sinFast = mathModule->FastSin(angle);
        auto sinStd = std::sin(angle);
        ok &= ExpectTrue(std::fabs(sinFast - sinStd) < 1e-4, "FastSin approximates std::sin");

        double cosFast = mathModule->FastCos(angle);
        double cosStd = std::cos(angle);
        ok &= ExpectTrue(std::fabs(cosFast - cosStd) < 1e-4, "FastCos approximates std::cos");

        auto reduced = mathModule->ReduceAngle(15.25);
        ok &= ExpectTrue(reduced >= -3.14159 && reduced <= 3.14159, "ReduceAngle clamps range");

        auto tanFast = mathModule->FastTan(0.5);
        ok &= ExpectTrue(std::fabs(tanFast - std::tan(0.5)) < 1e-3, "FastTan approximates std::tan");

        auto sqrtFast = mathModule->FastSqrt(144.0);
        ok &= ExpectTrue(std::fabs(sqrtFast - 12.0) < 1e-9, "FastSqrt accurate");

        auto invSqrtFast = mathModule->FastInverseSqrt(49.0f);
        ok &= ExpectTrue(std::fabs(static_cast<double>(invSqrtFast) - (1.0 / 7.0)) < 1e-3,
                         "FastInverseSqrt approximates reciprocal sqrt");

        double lhs3[3] = {1.0, 2.0, 3.0};
        double rhs3[3] = {4.0, 5.0, 6.0};
        ok &= ExpectTrue(std::fabs(mathModule->Dot3(lhs3, rhs3) - 32.0) < 1e-9, "Dot3 result correct");

        double lhs4[4] = {1.0, -2.0, 3.0, -4.0};
        double rhs4[4] = {5.0, 6.0, -7.0, 8.0};
        double expectedDot4 = 1.0 * 5.0 + (-2.0) * 6.0 + 3.0 * (-7.0) + (-4.0) * 8.0;
        ok &= ExpectTrue(std::fabs(mathModule->Dot4(lhs4, rhs4) - expectedDot4) < 1e-9, "Dot4 result correct");

        double fmaA[4] = {0.5, 1.5, 2.5, 3.5};
        double fmaB[4] = {2.0, -2.0, 3.0, -3.0};
        double fmaC[4] = {1.0, 1.0, 1.0, 1.0};
        double fmaOut[4] = {0.0, 0.0, 0.0, 0.0};
        mathModule->BatchedFma(fmaA, fmaB, fmaC, fmaOut, 4);
        ok &= ExpectTrue(std::fabs(fmaOut[0] - (0.5 * 2.0 + 1.0)) < 1e-9, "BatchedFma first lane");
        ok &= ExpectTrue(std::fabs(fmaOut[3] - (3.5 * -3.0 + 1.0)) < 1e-9, "BatchedFma last lane");

        double coeffs[4] = {2.0, -3.0, 0.5, 4.0};
        auto horner = mathModule->Horner(coeffs, 4, 1.25);
        double expectedHorner = ((2.0 * 1.25 - 3.0) * 1.25 + 0.5) * 1.25 + 4.0;
        ok &= ExpectTrue(std::fabs(horner - expectedHorner) < 1e-9, "Horner evaluation matches");

        auto lerp = mathModule->Lerp(-10.0, 10.0, 0.75);
        ok &= ExpectTrue(std::fabs(lerp - 5.0) < 1e-9, "Lerp result correct");

        auto clamp = mathModule->Clamp(42.0, -1.0, 5.0);
        ok &= ExpectTrue(clamp == 5.0, "Clamp upper bound");

        const auto &metrics = mathModule->GetMetrics();
        ok &= ExpectTrue(metrics.fastSinCalls >= 1, "fastSin counted");
        ok &= ExpectTrue(metrics.fastCosCalls >= 1, "fastCos counted");
        ok &= ExpectTrue(metrics.fastTanCalls >= 1, "fastTan counted");
        ok &= ExpectTrue(metrics.fastSqrtCalls >= 1, "fastSqrt counted");
        ok &= ExpectTrue(metrics.fastInvSqrtCalls >= 1, "fastInvSqrt counted");
        ok &= ExpectTrue(metrics.batchedFmaOps >= 4, "Batched FMA counted");
        ok &= ExpectTrue(metrics.hornerEvaluations >= 1, "Horner counted");
        ok &= ExpectTrue(metrics.dotProducts >= 2, "Dot product counted");
        ok &= ExpectTrue(metrics.tableSize >= 1024, "Lookup table populated");

        return ok;
    }

    bool NumberModuleHandlesAggregates() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *modulePtr = environment.FindModule("Number");
        auto *numberModule = dynamic_cast<spectre::es2025::NumberModule *>(modulePtr);
        ok &= ExpectTrue(numberModule != nullptr, "Number module available");
        if (!numberModule) {
            return false;
        }

        spectre::es2025::NumberModule::Handle value = 0;
        ok &= ExpectStatus(numberModule->Create("analytics.frame", 42.5, value), StatusCode::Ok, "Create number");
        ok &= ExpectTrue(numberModule->ValueOf(value) == 42.5, "Initial value matches");
        ok &= ExpectStatus(numberModule->Add(value, -12.5), StatusCode::Ok, "Add delta");
        ok &= ExpectTrue(std::fabs(numberModule->ValueOf(value) - 30.0) < 1e-9, "Addition applied");
        ok &= ExpectStatus(numberModule->Multiply(value, 2.0), StatusCode::Ok, "Multiply");
        ok &= ExpectTrue(std::fabs(numberModule->ValueOf(value) - 60.0) < 1e-9, "Multiplication applied");
        ok &= ExpectStatus(numberModule->Saturate(value, 0.0, 50.0), StatusCode::Ok, "Saturate clamp");
        ok &= ExpectTrue(std::fabs(numberModule->ValueOf(value) - 50.0) < 1e-9, "Saturation applied");

        double samples[6] = {5.0, 7.0, 11.0, 13.0, 17.0, 19.0};
        double sum = 0.0;
        ok &= ExpectStatus(numberModule->Accumulate(samples, 6, sum), StatusCode::Ok, "Accumulate vector");
        ok &= ExpectTrue(std::fabs(sum - 72.0) < 1e-9, "Accumulation result");

        ok &= ExpectStatus(numberModule->Normalize(samples, 6, -1.0, 1.0), StatusCode::Ok, "Normalize range");
        for (double sample: samples) {
            ok &= ExpectTrue(sample >= -1.0000001 && sample <= 1.0000001, "Normalized bounds");
        }

        spectre::es2025::NumberModule::Statistics summary{};
        ok &= ExpectStatus(numberModule->BuildStatistics(samples, 6, summary), StatusCode::Ok, "Summary build");
        ok &= ExpectTrue(summary.maxValue <= 1.0000001, "Summary max range");

        auto zeroHandle = numberModule->Canonical(0.0);
        ok &= ExpectTrue(numberModule->Has(zeroHandle), "Canonical zero registered");
        auto nanHandle = numberModule->Canonical(std::numeric_limits<double>::quiet_NaN());
        ok &= ExpectTrue(numberModule->Has(nanHandle), "Canonical NaN registered");
        auto transientHandle = numberModule->Canonical(1234.0);
        ok &= ExpectStatus(numberModule->Destroy(transientHandle), StatusCode::Ok, "Destroy transient canonical");
        ok &= ExpectStatus(numberModule->Destroy(value), StatusCode::Ok, "Destroy number handle");

        const auto &metrics = numberModule->GetMetrics();
        ok &= ExpectTrue(metrics.allocations >= 1, "Allocations tracked");
        ok &= ExpectTrue(metrics.mutations >= 2, "Mutations tracked");
        ok &= ExpectTrue(metrics.accumulations >= 1, "Accumulations tracked");
        ok &= ExpectTrue(metrics.normalizations >= 1, "Normalizations tracked");

        return ok;
    }

    bool DateModuleConstructsAndFormats() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *modulePtr = environment.FindModule("Date");
        auto *dateModule = dynamic_cast<spectre::es2025::DateModule *>(modulePtr);
        ok &= ExpectTrue(dateModule != nullptr, "Date module available");
        if (!dateModule) {
            return false;
        }

        auto canonicalEpoch = dateModule->CanonicalEpoch();
        ok &= ExpectTrue(dateModule->Has(canonicalEpoch), "Canonical epoch registered");

        spectre::es2025::DateModule::Handle launch = 0;
        ok &= ExpectStatus(dateModule->CreateFromComponents("mission.launch",
                                                            2025, 10, 7, 12, 34, 56, 789,
                                                            launch),
                           StatusCode::Ok, "Create from components");
        ok &= ExpectTrue(dateModule->Has(launch), "Launch handle registered");

        spectre::es2025::DateModule::Components components{};
        ok &= ExpectStatus(dateModule->ToComponents(launch, components), StatusCode::Ok, "Component extraction");
        ok &= ExpectTrue(components.year == 2025 && components.month == 10 && components.day == 7,
                         "Date fields correct");
        ok &= ExpectTrue(components.hour == 12 && components.minute == 34 && components.second == 56 &&
                         components.millisecond == 789, "Time fields correct");
        ok &= ExpectTrue(components.dayOfWeek == 2, "Tuesday day of week");
        ok &= ExpectTrue(components.dayOfYear == 280, "Day of year computed");

        std::string iso;
        ok &= ExpectStatus(dateModule->FormatIso8601(launch, iso), StatusCode::Ok, "ISO formatting");
        ok &= ExpectTrue(iso == "2025-10-07T12:34:56.789Z", "ISO string match");

        spectre::es2025::DateModule::Handle parsed = 0;
        ok &= ExpectStatus(dateModule->ParseIso8601("2024-02-29T00:00:00.000Z", "leap", parsed), StatusCode::Ok,
                           "Parse leap date");
        spectre::es2025::DateModule::Components leap{};
        ok &= ExpectStatus(dateModule->ToComponents(parsed, leap), StatusCode::Ok, "Extract leap components");
        ok &= ExpectTrue(leap.day == 29 && leap.month == 2, "Leap day components");

        ok &= ExpectStatus(dateModule->AddDays(launch, 10), StatusCode::Ok, "Add days");
        ok &= ExpectStatus(dateModule->AddMilliseconds(launch, 250), StatusCode::Ok, "Add milliseconds");
        auto shiftedEpoch = dateModule->EpochMilliseconds(launch, 0);
        auto baseEpoch = dateModule->EpochMilliseconds(canonicalEpoch, -1);
        ok &= ExpectTrue(shiftedEpoch > baseEpoch, "Epoch progressed");

        std::int64_t delta = 0;
        ok &= ExpectStatus(dateModule->DifferenceMilliseconds(launch, parsed, delta), StatusCode::Ok,
                           "Difference computed");
        ok &= ExpectTrue(delta != 0, "Difference non-zero");

        ok &= ExpectStatus(dateModule->Destroy(parsed), StatusCode::Ok, "Destroy parsed handle");
        ok &= ExpectStatus(dateModule->Destroy(launch), StatusCode::Ok, "Destroy launch handle");

        const auto &metrics = dateModule->GetMetrics();
        ok &= ExpectTrue(metrics.allocations >= 2, "Allocations tracked");
        ok &= ExpectTrue(metrics.componentConversions >= 2, "Component conversions tracked");
        ok &= ExpectTrue(metrics.isoFormats >= 1, "ISO formats tracked");
        ok &= ExpectTrue(metrics.isoParses >= 1, "ISO parses tracked");
        ok &= ExpectTrue(metrics.arithmeticOps >= 1, "Arithmetic ops tracked");

        return ok;
    }

    bool BigIntModulePerformsArithmetic() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *modulePtr = environment.FindModule("BigInt");
        auto *bigintModule = dynamic_cast<spectre::es2025::BigIntModule *>(modulePtr);
        ok &= ExpectTrue(bigintModule != nullptr, "BigInt module available");
        if (!bigintModule) {
            return false;
        }

        spectre::es2025::BigIntModule::Handle base = 0;
        ok &= ExpectStatus(bigintModule->Create("counter.base", 123456789, base), StatusCode::Ok, "Create base bigint");

        spectre::es2025::BigIntModule::Handle delta = 0;
        ok &= ExpectStatus(bigintModule->Create("counter.delta", 987654321, delta), StatusCode::Ok,
                           "Create delta bigint");
        ok &= ExpectStatus(bigintModule->Add(base, delta), StatusCode::Ok, "Add delta to base");

        ok &= ExpectStatus(bigintModule->AddSigned(base, -123456789), StatusCode::Ok, "Subtract via signed add");
        ok &= ExpectStatus(bigintModule->MultiplySmall(base, 8), StatusCode::Ok, "Multiply small");
        ok &= ExpectStatus(bigintModule->ShiftLeft(base, 5), StatusCode::Ok, "Shift left bits");

        std::string decimal;
        ok &= ExpectStatus(bigintModule->ToDecimalString(base, decimal), StatusCode::Ok, "Decimal conversion");
        ok &= ExpectTrue(!decimal.empty(), "Decimal string produced");

        std::uint64_t value64 = 0;
        auto toUint = bigintModule->ToUint64(base, value64);
        ok &= ExpectTrue(toUint == StatusCode::Ok || toUint == StatusCode::CapacityExceeded,
                         "Uint64 conversion status");

        spectre::es2025::BigIntModule::Handle parsed = 0;
        ok &= ExpectStatus(bigintModule->CreateFromDecimal("counter.parsed", "44321378655999887766", parsed),
                           StatusCode::Ok, "Parse decimal");

        auto comparison = bigintModule->Compare(parsed, base);
        ok &= ExpectTrue(comparison.digits >= 1, "Comparison digits populated");

        ok &= ExpectStatus(bigintModule->Destroy(delta), StatusCode::Ok, "Destroy delta");
        ok &= ExpectStatus(bigintModule->Destroy(parsed), StatusCode::Ok, "Destroy parsed");
        ok &= ExpectStatus(bigintModule->Destroy(base), StatusCode::Ok, "Destroy base");

        const auto &metrics = bigintModule->GetMetrics();
        ok &= ExpectTrue(metrics.allocations >= 3, "Allocations tracked");
        ok &= ExpectTrue(metrics.additions >= 2, "Additions tracked");
        ok &= ExpectTrue(metrics.multiplications >= 2, "Multiplications tracked");
        ok &= ExpectTrue(metrics.conversions >= 1, "Conversions tracked");

        return ok;
    }

    bool ObjectModuleHandlesPrototypes() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *modulePtr = environment.FindModule("Object");
        auto *objectModule = dynamic_cast<spectre::es2025::ObjectModule *>(modulePtr);
        ok &= ExpectTrue(objectModule != nullptr, "Object module available");
        if (!objectModule) {
            return false;
        }
        spectre::es2025::ObjectModule::Handle prototype = 0;
        ok &= ExpectStatus(objectModule->Create("test.prototype", 0, prototype), StatusCode::Ok, "Create prototype");
        spectre::es2025::ObjectModule::PropertyDescriptor descriptor;
        descriptor.value = spectre::es2025::ObjectModule::Value::FromString("base");
        descriptor.enumerable = true;
        descriptor.configurable = true;
        descriptor.writable = true;
        ok &= ExpectStatus(objectModule->Define(prototype, "type", descriptor), StatusCode::Ok,
                           "Define prototype property");
        spectre::es2025::ObjectModule::Handle instance = 0;
        ok &= ExpectStatus(objectModule->Create("test.instance", prototype, instance), StatusCode::Ok,
                           "Create instance");
        ok &= ExpectStatus(objectModule->Set(instance, "hp", spectre::es2025::ObjectModule::Value::FromInt(75)),
                           StatusCode::Ok, "Set instance property");
        spectre::es2025::ObjectModule::Value value;
        ok &= ExpectStatus(objectModule->Get(instance, "hp", value), StatusCode::Ok, "Get instance property");
        ok &= ExpectTrue(value.IsInt() && value.Int() == 75, "Instance value stored");
        spectre::es2025::ObjectModule::Value protoValue;
        ok &= ExpectStatus(objectModule->Get(instance, "type", protoValue), StatusCode::Ok, "Prototype lookup");
        ok &= ExpectTrue(protoValue.IsString() && protoValue.String() == "base", "Prototype value accessible");
        std::vector<std::string> keys;
        ok &= ExpectStatus(objectModule->OwnKeys(instance, keys), StatusCode::Ok, "Enumerate keys");
        ok &= ExpectTrue(std::find(keys.begin(), keys.end(), "hp") != keys.end(), "Own keys contain hp");
        bool deleted = false;
        ok &= ExpectStatus(objectModule->Delete(instance, "hp", deleted), StatusCode::Ok, "Delete property");
        ok &= ExpectTrue(deleted, "Delete flagged");
        ok &= ExpectStatus(objectModule->Seal(instance), StatusCode::Ok, "Seal instance");
        ok &= ExpectTrue(objectModule->IsSealed(instance), "Instance sealed");
        ok &= ExpectStatus(objectModule->Freeze(prototype), StatusCode::Ok, "Freeze prototype");
        ok &= ExpectTrue(objectModule->IsFrozen(prototype), "Prototype frozen");
        ok &= ExpectStatus(objectModule->Destroy(instance), StatusCode::Ok, "Destroy instance");
        ok &= ExpectStatus(objectModule->Destroy(prototype), StatusCode::Ok, "Destroy prototype");
        const auto &metrics = objectModule->GetMetrics();
        ok &= ExpectTrue(metrics.propertyAdds >= 2, "Property add metric");
        ok &= ExpectTrue(metrics.fastPathHits >= 1, "Fast hit metric");
        return ok;
    }

    bool ProxyModuleCoordinatesTraps() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *objectPtr = environment.FindModule("Object");
        auto *objectModule = dynamic_cast<spectre::es2025::ObjectModule *>(objectPtr);
        auto *proxyPtr = environment.FindModule("Proxy");
        auto *proxyModule = dynamic_cast<spectre::es2025::ProxyModule *>(proxyPtr);
        ok &= ExpectTrue(objectModule != nullptr, "Object module available");
        ok &= ExpectTrue(proxyModule != nullptr, "Proxy module available");
        if (!objectModule || !proxyModule) {
            return false;
        }
        spectre::es2025::ObjectModule::Handle target = 0;
        ok &= ExpectStatus(objectModule->Create("proxy.target", 0, target), StatusCode::Ok, "Create target");
        ok &= ExpectStatus(objectModule->Set(target, "count", spectre::es2025::ObjectModule::Value::FromInt(1)),
                           StatusCode::Ok, "Seed target count");
        struct TrapState {
            int gets;
            int sets;
            int deletes;
        } state{0, 0, 0};
        spectre::es2025::ProxyModule::TrapTable traps{};
        traps.get = [](spectre::es2025::ObjectModule &objects, spectre::es2025::ObjectModule::Handle handle,
                       std::string_view key, spectre::es2025::ObjectModule::Value &outValue,
                       void *userdata) -> StatusCode {
            auto *stats = static_cast<TrapState *>(userdata);
            stats->gets += 1;
            return objects.Get(handle, key, outValue);
        };
        traps.set = [](spectre::es2025::ObjectModule &objects, spectre::es2025::ObjectModule::Handle handle,
                       std::string_view key, const spectre::es2025::ObjectModule::Value &value,
                       void *userdata) -> StatusCode {
            auto *stats = static_cast<TrapState *>(userdata);
            stats->sets += 1;
            return objects.Set(handle, key, value);
        };
        traps.drop = [](spectre::es2025::ObjectModule &objects, spectre::es2025::ObjectModule::Handle handle,
                        std::string_view key, bool &removed, void *userdata) -> StatusCode {
            auto *stats = static_cast<TrapState *>(userdata);
            stats->deletes += 1;
            return objects.Delete(handle, key, removed);
        };
        traps.userdata = &state;
        spectre::es2025::ProxyModule::Handle proxy = 0;
        ok &= ExpectStatus(proxyModule->Create(target, traps, proxy), StatusCode::Ok, "Create proxy");
        spectre::es2025::ObjectModule::Value value;
        ok &= ExpectStatus(proxyModule->Get(proxy, "count", value), StatusCode::Ok, "Proxy get");
        ok &= ExpectTrue(value.IsInt() && value.Int() == 1, "Proxy get value");
        ok &= ExpectStatus(proxyModule->Set(proxy, "count", spectre::es2025::ObjectModule::Value::FromInt(5)),
                           StatusCode::Ok, "Proxy set");
        bool hasCount = false;
        ok &= ExpectStatus(proxyModule->Has(proxy, "count", hasCount), StatusCode::Ok, "Proxy has");
        ok &= ExpectTrue(hasCount, "Proxy reports presence");
        bool removed = false;
        ok &= ExpectStatus(proxyModule->Delete(proxy, "count", removed), StatusCode::Ok, "Proxy delete");
        ok &= ExpectTrue(removed, "Proxy delete flag");
        std::vector<std::string> keys;
        ok &= ExpectStatus(proxyModule->OwnKeys(proxy, keys), StatusCode::Ok, "Proxy keys");
        ok &= ExpectTrue(keys.empty(), "Proxy keys empty");
        ok &= ExpectStatus(proxyModule->Revoke(proxy), StatusCode::Ok, "Proxy revoke");
        spectre::es2025::ObjectModule::Value revoked;
        ok &= ExpectStatus(proxyModule->Get(proxy, "count", revoked), StatusCode::InvalidArgument, "Revoked proxy");
        ok &= ExpectStatus(proxyModule->Destroy(proxy), StatusCode::Ok, "Destroy proxy");
        ok &= ExpectStatus(objectModule->Destroy(target), StatusCode::Ok, "Destroy target");
        ok &= ExpectTrue(state.gets >= 1 && state.sets >= 1 && state.deletes >= 1, "Trap counters updated");
        const auto &metrics = proxyModule->GetMetrics();
        ok &= ExpectTrue(metrics.trapHits >= 3, "Trap hits metric");
        ok &= ExpectTrue(metrics.revocations >= 1, "Revocation metric");
        return ok;
    }

    bool MapModuleMaintainsOrder() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *mapPtr = environment.FindModule("Map");
        auto *mapModule = dynamic_cast<spectre::es2025::MapModule *>(mapPtr);
        ok &= ExpectTrue(mapModule != nullptr, "Map module available");
        if (!mapModule) {
            return false;
        }
        spectre::es2025::MapModule::Handle handle = 0;
        ok &= ExpectStatus(mapModule->Create("test.map", handle), StatusCode::Ok, "Create map");
        ok &= ExpectStatus(mapModule->Set(handle, spectre::es2025::MapModule::Value::FromString("alpha"),
                                          spectre::es2025::MapModule::Value::FromInt(10)), StatusCode::Ok, "Set alpha");
        ok &= ExpectStatus(mapModule->Set(handle, spectre::es2025::MapModule::Value::FromString("beta"),
                                          spectre::es2025::MapModule::Value::FromInt(20)), StatusCode::Ok, "Set beta");
        ok &= ExpectStatus(mapModule->Set(handle, spectre::es2025::MapModule::Value::FromString("gamma"),
                                          spectre::es2025::MapModule::Value::FromInt(30)), StatusCode::Ok, "Set gamma");
        ok &= ExpectTrue(mapModule->Size(handle) == 3, "Map size after inserts");
        spectre::es2025::MapModule::Value value;
        ok &= ExpectStatus(mapModule->Get(handle, spectre::es2025::MapModule::Value::FromString("beta"), value),
                           StatusCode::Ok, "Get beta");
        ok &= ExpectTrue(value.IsInt() && value.Int() == 20, "Beta value correct");
        std::vector<spectre::es2025::MapModule::Value> keys;
        ok &= ExpectStatus(mapModule->Keys(handle, keys), StatusCode::Ok, "Enumerate keys");
        ok &= ExpectTrue(keys.size() == 3, "Key count");
        ok &= ExpectTrue(keys[0].IsString() && keys[0].String() == "alpha", "First key order");
        ok &= ExpectTrue(keys[1].IsString() && keys[1].String() == "beta", "Second key order");
        bool removed = false;
        ok &= ExpectStatus(mapModule->Delete(handle, spectre::es2025::MapModule::Value::FromString("beta"), removed),
                           StatusCode::Ok, "Delete beta");
        ok &= ExpectTrue(removed, "Beta removed flag");
        ok &= ExpectTrue(mapModule->Size(handle) == 2, "Map size after delete");
        keys.clear();
        ok &= ExpectStatus(mapModule->Keys(handle, keys), StatusCode::Ok, "Keys after delete");
        ok &= ExpectTrue(keys.size() == 2, "Key count after delete");
        ok &= ExpectTrue(keys[0].IsString() && keys[0].String() == "alpha", "Order retained");
        ok &= ExpectStatus(mapModule->Clear(handle), StatusCode::Ok, "Clear map");
        ok &= ExpectTrue(mapModule->Size(handle) == 0, "Size after clear");
        ok &= ExpectStatus(mapModule->Destroy(handle), StatusCode::Ok, "Destroy map");
        const auto &metrics = mapModule->GetMetrics();
        ok &= ExpectTrue(metrics.setOps >= 3, "Set ops metric");
        ok &= ExpectTrue(metrics.iterations >= 2, "Iterations metric");
        return ok;
    }


    bool SetModuleMaintainsUniqueness() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *setPtr = environment.FindModule("Set");
        auto *setModule = dynamic_cast<spectre::es2025::SetModule *>(setPtr);
        ok &= ExpectTrue(setModule != nullptr, "Set module available");
        if (!setModule) {
            return false;
        }
        spectre::es2025::SetModule::Handle handle = 0;
        ok &= ExpectStatus(setModule->Create("test.set", handle), StatusCode::Ok, "Create set");
        using SetValue = spectre::es2025::SetModule::Value;
        ok &= ExpectStatus(setModule->Add(handle, SetValue::FromInt(42)), StatusCode::Ok, "Add int");
        ok &= ExpectStatus(setModule->Add(handle, SetValue::FromInt(42)), StatusCode::Ok, "Add duplicate");
        ok &= ExpectTrue(setModule->Size(handle) == 1, "Duplicate ignored");
        ok &= ExpectStatus(setModule->Add(handle, SetValue::FromString("alpha")), StatusCode::Ok, "Add string");
        ok &= ExpectStatus(setModule->Add(handle, SetValue::FromDouble(3.5)), StatusCode::Ok, "Add double");
        ok &= ExpectTrue(setModule->Size(handle) == 3, "Set size after additions");
        ok &= ExpectTrue(setModule->Has(handle, SetValue::FromString("alpha")), "Set contains string");
        ok &= ExpectTrue(!setModule->Has(handle, SetValue::FromBoolean(true)), "Missing value lookup");
        bool removed = false;
        ok &= ExpectStatus(setModule->Delete(handle, SetValue::FromInt(42), removed), StatusCode::Ok, "Delete value");
        ok &= ExpectTrue(removed, "Delete reported true");
        ok &= ExpectTrue(setModule->Size(handle) == 2, "Size after delete");
        std::vector<SetValue> values;
        ok &= ExpectStatus(setModule->Values(handle, values), StatusCode::Ok, "Collect values");
        ok &= ExpectTrue(values.size() == 2, "Values count");
        std::vector<std::pair<SetValue, SetValue> > entries;
        ok &= ExpectStatus(setModule->Entries(handle, entries), StatusCode::Ok, "Collect entries");
        ok &= ExpectTrue(entries.size() == 2, "Entries count");
        if (!entries.empty()) {
            ok &= ExpectTrue(entries.front().first.Equals(entries.front().second), "Entries mirror values");
        }
        ok &= ExpectStatus(setModule->Clear(handle), StatusCode::Ok, "Clear set");
        ok &= ExpectTrue(setModule->Size(handle) == 0, "Cleared size");
        ok &= ExpectStatus(setModule->Destroy(handle), StatusCode::Ok, "Destroy set");
        const auto &metrics = setModule->GetMetrics();
        ok &= ExpectTrue(metrics.addOps >= 3, "Add ops metric");
        ok &= ExpectTrue(metrics.iterations >= 2, "Iteration metric");
        return ok;
    }

    bool WeakSetModuleCompactsInvalidEntries() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *objectPtr = environment.FindModule("Object");
        auto *objectModule = dynamic_cast<spectre::es2025::ObjectModule *>(objectPtr);
        auto *weakPtr = environment.FindModule("WeakSet");
        auto *weakSetModule = dynamic_cast<spectre::es2025::WeakSetModule *>(weakPtr);
        ok &= ExpectTrue(objectModule != nullptr, "Object module available");
        ok &= ExpectTrue(weakSetModule != nullptr, "WeakSet module available");
        if (!objectModule || !weakSetModule) {
            return false;
        }
        spectre::es2025::WeakSetModule::Handle handle = 0;
        ok &= ExpectStatus(weakSetModule->Create("test.weakset", handle), StatusCode::Ok, "Create weak set");
        spectre::es2025::ObjectModule::Handle keyA = 0;
        spectre::es2025::ObjectModule::Handle keyB = 0;
        ok &= ExpectStatus(objectModule->Create("test.weakset.keyA", 0, keyA), StatusCode::Ok, "Create keyA");
        ok &= ExpectStatus(objectModule->Create("test.weakset.keyB", 0, keyB), StatusCode::Ok, "Create keyB");
        ok &= ExpectStatus(weakSetModule->Add(handle, keyA), StatusCode::Ok, "Add keyA");
        ok &= ExpectStatus(weakSetModule->Add(handle, keyB), StatusCode::Ok, "Add keyB");
        ok &= ExpectTrue(weakSetModule->Has(handle, keyA), "Weak set contains keyA");
        ok &= ExpectTrue(weakSetModule->Size(handle) == 2, "Weak set size");
        ok &= ExpectStatus(objectModule->Destroy(keyA), StatusCode::Ok, "Destroy keyA");
        ok &= ExpectStatus(weakSetModule->Compact(handle), StatusCode::Ok, "Compact weak set");
        ok &= ExpectTrue(weakSetModule->Size(handle) == 1, "Size after compact");
        bool removed = false;
        ok &= ExpectStatus(weakSetModule->Delete(handle, keyB, removed), StatusCode::Ok, "Delete keyB");
        ok &= ExpectTrue(removed, "Delete reported true");
        ok &= ExpectTrue(weakSetModule->Size(handle) == 0, "Empty after delete");
        ok &= ExpectStatus(weakSetModule->Destroy(handle), StatusCode::Ok, "Destroy weak set");
        ok &= ExpectStatus(objectModule->Destroy(keyB), StatusCode::Ok, "Destroy keyB");
        const auto &metrics = weakSetModule->GetMetrics();
        ok &= ExpectTrue(metrics.compactions >= 1, "Compaction metric");
        ok &= ExpectTrue(metrics.hits >= 1, "Hit metric");
        return ok;
    }

    bool ReflectModuleProvidesMetaOperations() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *objectPtr = environment.FindModule("Object");
        auto *reflectPtr = environment.FindModule("Reflect");
        auto *objectModule = dynamic_cast<spectre::es2025::ObjectModule *>(objectPtr);
        auto *reflectModule = dynamic_cast<spectre::es2025::ReflectModule *>(reflectPtr);
        ok &= ExpectTrue(objectModule != nullptr, "Object module available");
        ok &= ExpectTrue(reflectModule != nullptr, "Reflect module available");
        if (!objectModule || !reflectModule) {
            return false;
        }
        spectre::es2025::ObjectModule::Handle target = 0;
        ok &= ExpectStatus(objectModule->Create("test.reflect.target", 0, target), StatusCode::Ok, "Create target");
        spectre::es2025::ObjectModule::PropertyDescriptor descriptor{};
        descriptor.value = spectre::es2025::ObjectModule::Value::FromInt(42);
        descriptor.enumerable = true;
        descriptor.configurable = false;
        descriptor.writable = false;
        ok &= ExpectStatus(reflectModule->DefineProperty(target, "answer", descriptor), StatusCode::Ok,
                           "Define property");
        spectre::es2025::ObjectModule::Value value;
        ok &= ExpectStatus(reflectModule->Get(target, "answer", value), StatusCode::Ok, "Get property");
        ok &= ExpectTrue(value.IsInt() && value.Int() == 42, "Reflect.get returns expected value");
        ok &= ExpectTrue(reflectModule->Has(target, "answer"), "Reflect.has returns true");
        spectre::es2025::ObjectModule::PropertyDescriptor readBack{};
        ok &= ExpectStatus(reflectModule->GetOwnPropertyDescriptor(target, "answer", readBack), StatusCode::Ok,
                           "Descriptor fetch");
        ok &= ExpectTrue(readBack.enumerable && !readBack.configurable && !readBack.writable,
                         "Descriptor flags preserved");
        std::vector<std::string> keys;
        ok &= ExpectStatus(reflectModule->OwnKeys(target, keys), StatusCode::Ok, "Own keys");
        ok &= ExpectTrue(keys.size() == 1 && keys.front() == "answer", "Own keys contains property");
        auto prototype = reflectModule->GetPrototypeOf(target);
        ok &= ExpectTrue(prototype == 0, "Prototype initially null");
        spectre::es2025::ObjectModule::Handle protoHandle = 0;
        ok &= ExpectStatus(objectModule->Create("test.reflect.proto", 0, protoHandle), StatusCode::Ok,
                           "Create prototype");
        ok &= ExpectStatus(reflectModule->SetPrototypeOf(target, protoHandle), StatusCode::Ok, "Set prototype");
        ok &= ExpectTrue(reflectModule->GetPrototypeOf(target) == protoHandle, "Prototype updated");
        bool deleted = true;
        ok &= ExpectStatus(reflectModule->DeleteProperty(target, "answer", deleted), StatusCode::InvalidArgument,
                           "Delete property rejects non-configurable");
        ok &= ExpectTrue(!deleted, "Non-configurable property not removed");
        ok &= ExpectTrue(reflectModule->IsExtensible(target), "Still extensible before preventExtensions");
        ok &= ExpectStatus(reflectModule->PreventExtensions(target), StatusCode::Ok, "Prevent extensions");
        ok &= ExpectTrue(!reflectModule->IsExtensible(target), "Not extensible after preventExtensions");
        spectre::es2025::ObjectModule::PropertyDescriptor descriptor2{};
        descriptor2.value = spectre::es2025::ObjectModule::Value::FromInt(7);
        descriptor2.enumerable = true;
        descriptor2.configurable = true;
        descriptor2.writable = true;
        ok &= ExpectStatus(reflectModule->DefineProperty(target, "newProp", descriptor2), StatusCode::InvalidArgument,
                           "Cannot add property to non-extensible object");
        const auto &metrics = reflectModule->GetMetrics();
        ok &= ExpectTrue(metrics.getOps >= 1 && metrics.defineOps >= 1, "Reflect metrics updated");
        return ok;
    }

    bool WeakRefModuleTracksLifetime() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *objectPtr = environment.FindModule("Object");
        auto *weakPtr = environment.FindModule("WeakRef");
        auto *objectModule = dynamic_cast<spectre::es2025::ObjectModule *>(objectPtr);
        auto *weakRefModule = dynamic_cast<spectre::es2025::WeakRefModule *>(weakPtr);
        ok &= ExpectTrue(objectModule != nullptr, "Object module available");
        ok &= ExpectTrue(weakRefModule != nullptr, "WeakRef module available");
        if (!objectModule || !weakRefModule) {
            return false;
        }
        spectre::es2025::WeakRefModule::Handle invalidHandle = 0;
        ok &= ExpectStatus(weakRefModule->Create(0, invalidHandle), StatusCode::InvalidArgument, "Reject null target");
        spectre::es2025::ObjectModule::Handle targetA = 0;
        spectre::es2025::ObjectModule::Handle targetB = 0;
        ok &= ExpectStatus(objectModule->Create("test.weakref.A", 0, targetA), StatusCode::Ok, "Create targetA");
        ok &= ExpectStatus(objectModule->Create("test.weakref.B", 0, targetB), StatusCode::Ok, "Create targetB");
        spectre::es2025::WeakRefModule::Handle refA = 0;
        spectre::es2025::WeakRefModule::Handle refB = 0;
        ok &= ExpectStatus(weakRefModule->Create(targetA, refA), StatusCode::Ok, "Create refA");
        ok &= ExpectStatus(weakRefModule->Create(targetB, refB), StatusCode::Ok, "Create refB");
        ok &= ExpectTrue(weakRefModule->LiveCount() == 2, "Live count reflects allocations");
        spectre::es2025::ObjectModule::Handle derefHandle = 0;
        bool alive = false;
        ok &= ExpectStatus(weakRefModule->Deref(refA, derefHandle, alive), StatusCode::Ok, "Deref refA");
        ok &= ExpectTrue(alive && derefHandle == targetA, "refA alive");
        ok &= ExpectTrue(weakRefModule->Alive(refB), "Alive(refB) true");
        ok &= ExpectStatus(objectModule->Destroy(targetB), StatusCode::Ok, "Destroy targetB");
        derefHandle = 0;
        alive = true;
        ok &= ExpectStatus(weakRefModule->Deref(refB, derefHandle, alive), StatusCode::Ok, "Deref refB after destroy");
        ok &= ExpectTrue(!alive && derefHandle == 0, "refB cleared after GC");
        ok &= ExpectStatus(weakRefModule->Refresh(refB, targetA), StatusCode::Ok, "Rebind refB");
        ok &= ExpectTrue(weakRefModule->Alive(refB), "refB alive after refresh");
        ok &= ExpectStatus(weakRefModule->Refresh(refB, 0), StatusCode::Ok, "Explicit clear");
        ok &= ExpectTrue(!weakRefModule->Alive(refB), "refB not alive after clear");
        ok &= ExpectStatus(weakRefModule->Destroy(refB), StatusCode::Ok, "Destroy refB");
        ok &= ExpectTrue(weakRefModule->LiveCount() == 1, "Live count decremented");
        ok &= ExpectStatus(weakRefModule->Compact(), StatusCode::Ok, "Compact weak refs");
        runtime->Tick({1.0, 1});
        const auto &metrics = weakRefModule->GetMetrics();
        ok &= ExpectTrue(metrics.derefOps >= 2, "Deref metrics recorded");
        ok &= ExpectTrue(metrics.clearedRefs >= 1, "Cleared refs tracked");
        ok &= ExpectTrue(metrics.totalReleases >= 1, "Release metric updated");
        ok &= ExpectStatus(weakRefModule->Destroy(refA), StatusCode::Ok, "Destroy refA");
        return ok;
    }

    bool WeakMapModulePurgesInvalidKeys() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *objectPtr = environment.FindModule("Object");
        auto *objectModule = dynamic_cast<spectre::es2025::ObjectModule *>(objectPtr);
        auto *weakPtr = environment.FindModule("WeakMap");
        auto *weakMapModule = dynamic_cast<spectre::es2025::WeakMapModule *>(weakPtr);
        ok &= ExpectTrue(objectModule != nullptr, "Object module available");
        ok &= ExpectTrue(weakMapModule != nullptr, "WeakMap module available");
        if (!objectModule || !weakMapModule) {
            return false;
        }
        spectre::es2025::WeakMapModule::Handle handle = 0;
        ok &= ExpectStatus(weakMapModule->Create("test.weakmap", handle), StatusCode::Ok, "Create weak map");
        spectre::es2025::ObjectModule::Handle keyA = 0;
        spectre::es2025::ObjectModule::Handle keyB = 0;
        ok &= ExpectStatus(objectModule->Create("test.weakmap.keyA", 0, keyA), StatusCode::Ok, "Create keyA");
        ok &= ExpectStatus(objectModule->Create("test.weakmap.keyB", 0, keyB), StatusCode::Ok, "Create keyB");
        ok &= ExpectStatus(weakMapModule->Set(handle, keyA, spectre::es2025::WeakMapModule::Value::FromInt(5)),
                           StatusCode::Ok, "Set keyA");
        ok &= ExpectStatus(weakMapModule->Set(handle, keyB, spectre::es2025::WeakMapModule::Value::FromInt(9)),
                           StatusCode::Ok, "Set keyB");
        ok &= ExpectTrue(weakMapModule->Size(handle) == 2, "Weak map size");
        spectre::es2025::WeakMapModule::Value value;
        ok &= ExpectStatus(weakMapModule->Get(handle, keyB, value), StatusCode::Ok, "Get keyB");
        ok &= ExpectTrue(value.IsInt() && value.Int() == 9, "KeyB value");
        ok &= ExpectStatus(objectModule->Destroy(keyA), StatusCode::Ok, "Destroy keyA");
        ok &= ExpectStatus(weakMapModule->Compact(handle), StatusCode::Ok, "Compact weak map");
        ok &= ExpectTrue(weakMapModule->Size(handle) == 1, "Size after compact");
        bool removed = false;
        ok &= ExpectStatus(weakMapModule->Delete(handle, keyB, removed), StatusCode::Ok, "Delete keyB");
        ok &= ExpectTrue(removed, "KeyB removed flag");
        ok &= ExpectTrue(weakMapModule->Size(handle) == 0, "Size empty");
        ok &= ExpectStatus(weakMapModule->Destroy(handle), StatusCode::Ok, "Destroy weak map");
        const auto &metrics = weakMapModule->GetMetrics();
        ok &= ExpectTrue(metrics.compactions >= 1, "Compaction metric");
        ok &= ExpectTrue(metrics.hits >= 1, "Hit metric");
        return ok;
    }

    bool SymbolModuleManagesSymbols() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *modulePtr = environment.FindModule("Symbol");
        auto *symbolModule = dynamic_cast<spectre::es2025::SymbolModule *>(modulePtr);
        ok &= ExpectTrue(symbolModule != nullptr, "Symbol module available");
        if (!symbolModule) {
            return false;
        }
        std::array<std::pair<spectre::es2025::SymbolModule::WellKnown, const char *>, 16> wellKnown = {
            {
                {spectre::es2025::SymbolModule::WellKnown::AsyncIterator, "Symbol.asyncIterator"},
                {spectre::es2025::SymbolModule::WellKnown::HasInstance, "Symbol.hasInstance"},
                {spectre::es2025::SymbolModule::WellKnown::IsConcatSpreadable, "Symbol.isConcatSpreadable"},
                {spectre::es2025::SymbolModule::WellKnown::Iterator, "Symbol.iterator"},
                {spectre::es2025::SymbolModule::WellKnown::Match, "Symbol.match"},
                {spectre::es2025::SymbolModule::WellKnown::MatchAll, "Symbol.matchAll"},
                {spectre::es2025::SymbolModule::WellKnown::Replace, "Symbol.replace"},
                {spectre::es2025::SymbolModule::WellKnown::Search, "Symbol.search"},
                {spectre::es2025::SymbolModule::WellKnown::Species, "Symbol.species"},
                {spectre::es2025::SymbolModule::WellKnown::Split, "Symbol.split"},
                {spectre::es2025::SymbolModule::WellKnown::ToPrimitive, "Symbol.toPrimitive"},
                {spectre::es2025::SymbolModule::WellKnown::ToStringTag, "Symbol.toStringTag"},
                {spectre::es2025::SymbolModule::WellKnown::Unscopables, "Symbol.unscopables"},
                {spectre::es2025::SymbolModule::WellKnown::Dispose, "Symbol.dispose"},
                {spectre::es2025::SymbolModule::WellKnown::AsyncDispose, "Symbol.asyncDispose"},
                {spectre::es2025::SymbolModule::WellKnown::Metadata, "Symbol.metadata"}
            }
        };
        std::vector<spectre::es2025::SymbolModule::Handle> handles;
        handles.reserve(wellKnown.size());
        for (const auto &entry: wellKnown) {
            auto handle = symbolModule->WellKnownHandle(entry.first);
            ok &= ExpectTrue(handle != 0, "Well-known handle valid");
            ok &= ExpectTrue(symbolModule->IsPinned(handle), "Well-known pinned");
            ok &= ExpectTrue(symbolModule->Description(handle) == entry.second, "Well-known description");
            handles.push_back(handle);
        }
        auto sorted = handles;
        std::sort(sorted.begin(), sorted.end());
        ok &= ExpectTrue(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), "Well-known unique");
        spectre::es2025::SymbolModule::Handle localHandle = 0;
        ok &= ExpectStatus(symbolModule->Create("spectre.local", localHandle), StatusCode::Ok, "Create local symbol");
        ok &= ExpectTrue(localHandle != 0, "Local handle valid");
        ok &= ExpectTrue(!symbolModule->IsGlobal(localHandle), "Local symbol global flag");
        ok &= ExpectTrue(!symbolModule->IsPinned(localHandle), "Local symbol pinned flag");
        ok &= ExpectTrue(symbolModule->Description(localHandle) == "spectre.local", "Local description match");
        spectre::es2025::SymbolModule::Handle uniqueHandle = 0;
        ok &= ExpectStatus(symbolModule->CreateUnique(uniqueHandle), StatusCode::Ok, "Create unique symbol");
        ok &= ExpectTrue(uniqueHandle != 0, "Unique handle valid");
        ok &= ExpectTrue(symbolModule->Description(uniqueHandle).empty(), "Unique description empty");
        spectre::es2025::SymbolModule::Handle globalHandle = 0;
        ok &= ExpectStatus(symbolModule->CreateGlobal("spectre.global", globalHandle), StatusCode::Ok,
                           "Create global symbol");
        ok &= ExpectTrue(symbolModule->IsGlobal(globalHandle), "Global symbol flag");
        std::string registryKey;
        ok &= ExpectStatus(symbolModule->KeyFor(globalHandle, registryKey), StatusCode::Ok, "Registry lookup");
        ok &= ExpectTrue(registryKey == "spectre.global", "Registry key value");
        spectre::es2025::SymbolModule::Handle repeatHandle = 0;
        ok &= ExpectStatus(symbolModule->CreateGlobal("spectre.global", repeatHandle), StatusCode::Ok,
                           "Repeat global symbol");
        ok &= ExpectTrue(repeatHandle == globalHandle, "Global reuse");
        const auto &metrics = symbolModule->GetMetrics();
        ok &= ExpectTrue(metrics.globalSymbols >= 1, "Metrics global symbols");
        ok &= ExpectTrue(metrics.localSymbols >= static_cast<std::uint64_t>(handles.size() + 2),
                         "Metrics local symbols");
        ok &= ExpectTrue(metrics.registryHits >= 1, "Metrics registry hits");
        ok &= ExpectTrue(metrics.registryMisses >= 1, "Metrics registry misses");
        ok &= ExpectTrue(metrics.wellKnownSymbols == static_cast<std::uint64_t>(wellKnown.size()),
                         "Metrics well-known symbols");
        return ok;
    }

    bool ShadowRealmModuleCreatesIsolatedRealms() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *shadowPtr = environment.FindModule("ShadowRealm");
        auto *shadowModule = dynamic_cast<spectre::es2025::ShadowRealmModule *>(shadowPtr);
        auto *globalModule = dynamic_cast<spectre::es2025::GlobalModule *>(environment.FindModule("Global"));
        ok &= ExpectTrue(shadowModule != nullptr, "ShadowRealm module available");
        ok &= ExpectTrue(globalModule != nullptr, "Global module available");
        if (!shadowModule || !globalModule) {
            return false;
        }
        spectre::es2025::ShadowRealmModule::Handle realmA = 0;
        spectre::es2025::ShadowRealmModule::Handle realmB = 0;
        ok &= ExpectStatus(shadowModule->Create("shadow.realm.A", realmA), StatusCode::Ok, "Create realmA");
        ok &= ExpectStatus(shadowModule->Create("shadow.realm.B", realmB), StatusCode::Ok, "Create realmB");
        std::string value;
        std::string diagnostics;
        ok &= ExpectStatus(shadowModule->Evaluate(realmA, "return \"realm-a\";", value, diagnostics, "realmA.eval"),
                           StatusCode::Ok, "Eval realmA");
        ok &= ExpectTrue(value == "realm-a", "RealmA value");
        ok &= ExpectStatus(shadowModule->Evaluate(realmB, "return \"realm-b\";", value, diagnostics, "realmB.eval"),
                           StatusCode::Ok, "Eval realmB");
        ok &= ExpectTrue(value == "realm-b", "RealmB value");
        ok &= ExpectStatus(shadowModule->ExportValue(realmA, "answer", spectre::es2025::Value::Number(42.0)),
                           StatusCode::Ok, "Export value");
        spectre::es2025::Value imported;
        ok &= ExpectStatus(shadowModule->ImportValue(realmB, realmA, "answer", imported),
                           StatusCode::Ok, "Import from realmA");
        ok &= ExpectTrue(imported.IsNumber() && imported.AsNumber() == 42.0, "Imported content");
        ok &= ExpectStatus(shadowModule->ClearExports(realmA), StatusCode::Ok, "Clear exports");
        ok &= ExpectStatus(shadowModule->ImportValue(realmB, realmA, "answer", imported),
                           StatusCode::NotFound, "Import missing export");
        ok &= ExpectStatus(shadowModule->Destroy(realmA), StatusCode::Ok, "Destroy realmA");
        ok &= ExpectStatus(shadowModule->Destroy(realmB), StatusCode::Ok, "Destroy realmB");
        const auto &metrics = shadowModule->GetMetrics();
        ok &= ExpectTrue(metrics.created >= 2, "Metrics created");
        ok &= ExpectTrue(metrics.imports >= 1, "Metrics imports");
        ok &= ExpectTrue(metrics.failedImports >= 1, "Metrics failed imports");
        return ok;
    }

    bool TemporalModuleHandlesInstantsAndDurations() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime created");
        if (!runtime) {
            return false;
        }
        auto &environment = runtime->EsEnvironment();
        auto *temporalPtr = environment.FindModule("Temporal");
        auto *temporalModule = dynamic_cast<spectre::es2025::TemporalModule *>(temporalPtr);
        ok &= ExpectTrue(temporalModule != nullptr, "Temporal module available");
        if (!temporalModule) {
            return false;
        }
        auto zeroHandle = temporalModule->CanonicalEpoch();
        ok &= ExpectTrue(temporalModule->Has(zeroHandle), "Canonical handle registered");
        spectre::es2025::TemporalModule::Handle epochHandle = 0;
        ok &= ExpectStatus(temporalModule->CreateInstant("epoch+1s", 1000000000, epochHandle), StatusCode::Ok,
                           "Create instant from epoch");
        spectre::es2025::TemporalModule::PlainDateTime epochComponents{};
        ok &= ExpectStatus(temporalModule->ToPlainDateTime(epochHandle, 0, epochComponents), StatusCode::Ok,
                           "ToPlainDateTime");
        ok &= ExpectTrue(epochComponents.second == 1, "Epoch conversion");
        spectre::es2025::TemporalModule::PlainDateTime local{};
        local.year = 2024;
        local.month = 7;
        local.day = 21;
        local.hour = 19;
        local.minute = 5;
        local.second = 30;
        local.millisecond = 123;
        local.microsecond = 456;
        local.nanosecond = 789;
        spectre::es2025::TemporalModule::Handle localHandle = 0;
        ok &= ExpectStatus(temporalModule->CreateInstant(local, 120, "local.time", localHandle), StatusCode::Ok,
                           "Create from PlainDateTime");
        auto duration = spectre::es2025::TemporalModule::Duration::FromComponents(0, 1, 30, 0, 0, 0, 0);
        spectre::es2025::TemporalModule::Handle shiftedHandle = 0;
        ok &= ExpectStatus(temporalModule->AddDuration(localHandle, duration, "shifted", shiftedHandle),
                           StatusCode::Ok, "Add duration new");
        ok &= ExpectStatus(temporalModule->AddDurationInPlace(localHandle, duration),
                           StatusCode::Ok, "Add duration in place");
        spectre::es2025::TemporalModule::Duration diff{};
        ok &= ExpectStatus(temporalModule->Difference(localHandle, shiftedHandle, diff), StatusCode::Ok,
                           "Difference computation");
        ok &= ExpectTrue(diff.TotalNanoseconds() == 0, "Difference zero");
        auto breakdown = temporalModule->Breakdown(duration);
        ok &= ExpectTrue(breakdown.hours == 1 && breakdown.minutes == 30, "Duration breakdown");
        spectre::es2025::TemporalModule::Handle roundedHandle = 0;
        ok &= ExpectStatus(temporalModule->Round(shiftedHandle, 10,
                                                 spectre::es2025::TemporalModule::Unit::Minute,
                                                 spectre::es2025::TemporalModule::RoundingMode::HalfExpand,
                                                 "rounded", roundedHandle),
                           StatusCode::Ok, "Round instant");
        ok &= ExpectStatus(temporalModule->Destroy(roundedHandle), StatusCode::Ok, "Destroy rounded");
        ok &= ExpectStatus(temporalModule->Destroy(shiftedHandle), StatusCode::Ok, "Destroy shifted");
        ok &= ExpectStatus(temporalModule->Destroy(localHandle), StatusCode::Ok, "Destroy local");
        ok &= ExpectStatus(temporalModule->Destroy(epochHandle), StatusCode::Ok, "Destroy epoch");
        const auto &metrics = temporalModule->GetMetrics();
        ok &= ExpectTrue(metrics.instantAllocations >= 4, "Metrics allocations");
        ok &= ExpectTrue(metrics.arithmeticOps >= 2, "Metrics arithmetic");
        ok &= ExpectTrue(metrics.roundingOps >= 1, "Metrics rounding");
        return ok;
    }

    bool StructuredCloneModuleClonesComplexGraphs() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        auto &environment = runtime->EsEnvironment();
        auto *cloneModule = dynamic_cast<spectre::es2025::StructuredCloneModule *>(environment.FindModule(
            "StructuredClone"));
        auto *arrayModule = dynamic_cast<spectre::es2025::ArrayBufferModule *>(environment.FindModule("ArrayBuffer"));
        auto *sharedModule = dynamic_cast<spectre::es2025::SharedArrayBufferModule *>(environment.FindModule(
            "SharedArrayBuffer"));
        auto *typedModule = dynamic_cast<spectre::es2025::TypedArrayModule *>(environment.FindModule("TypedArray"));
        bool ok = ExpectTrue(cloneModule != nullptr, "StructuredClone module available");
        ok &= ExpectTrue(arrayModule != nullptr, "ArrayBuffer module required");
        ok &= ExpectTrue(sharedModule != nullptr, "SharedArrayBuffer module required");
        ok &= ExpectTrue(typedModule != nullptr, "TypedArray module required");
        if (!cloneModule || !arrayModule || !sharedModule || !typedModule) {
            return false;
        }

        spectre::es2025::ArrayBufferModule::Handle plainHandle = 0;
        ok &= ExpectStatus(arrayModule->Create("clone.source", 64, plainHandle), StatusCode::Ok, "Create array buffer");
        std::vector<std::uint8_t> pattern(64);
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            pattern[i] = static_cast<std::uint8_t>((i * 5) & 0xFFu);
        }
        ok &= ExpectStatus(arrayModule->CopyIn(plainHandle, 0, pattern.data(), pattern.size()), StatusCode::Ok,
                           "Populate buffer");

        spectre::es2025::SharedArrayBufferModule::Handle sharedHandle = 0;
        ok &= ExpectStatus(sharedModule->CreateResizable("clone.shared.source", 96, 160, sharedHandle), StatusCode::Ok,
                           "Create shared buffer");
        std::vector<std::uint8_t> sharedPattern(96);
        for (std::size_t i = 0; i < sharedPattern.size(); ++i) {
            sharedPattern[i] = static_cast<std::uint8_t>((i * 3) & 0xFFu);
        }
        ok &= ExpectStatus(sharedModule->CopyIn(sharedHandle, 0, sharedPattern.data(), sharedPattern.size()),
                           StatusCode::Ok,
                           "Populate shared buffer");

        spectre::es2025::TypedArrayModule::Handle typedHandle = 0;
        ok &= ExpectStatus(typedModule->Create(spectre::es2025::TypedArrayModule::ElementType::Float32,
                                               4,
                                               "clone.typed.source",
                                               typedHandle),
                           StatusCode::Ok,
                           "Create typed array");
        ok &= ExpectStatus(typedModule->Set(typedHandle, 0, 4.5), StatusCode::Ok, "Set typed value 0");
        ok &= ExpectStatus(typedModule->Set(typedHandle, 1, -7.25), StatusCode::Ok, "Set typed value 1");
        ok &= ExpectStatus(typedModule->Set(typedHandle, 2, 13.0), StatusCode::Ok, "Set typed value 2");
        ok &= ExpectStatus(typedModule->Set(typedHandle, 3, 2.0), StatusCode::Ok, "Set typed value 3");

        spectre::es2025::StructuredCloneModule::Node root;
        root.kind = spectre::es2025::StructuredCloneModule::Node::Kind::Object;
        root.label = "root";
        root.objectProperties.emplace_back(
            "title", spectre::es2025::StructuredCloneModule::Node::FromString("structured"));
        root.objectProperties.emplace_back("flag", spectre::es2025::StructuredCloneModule::Node::FromBoolean(true));

        spectre::es2025::StructuredCloneModule::Node list;
        list.kind = spectre::es2025::StructuredCloneModule::Node::Kind::Array;
        list.arrayItems.push_back(spectre::es2025::StructuredCloneModule::Node::FromNumber(42.0));
        list.arrayItems.push_back(spectre::es2025::StructuredCloneModule::Node::FromString("payload"));
        root.objectProperties.emplace_back("list", list);

        spectre::es2025::StructuredCloneModule::Node mapNode;
        mapNode.kind = spectre::es2025::StructuredCloneModule::Node::Kind::Map;
        mapNode.mapEntries.emplace_back(
            spectre::es2025::StructuredCloneModule::Node::FromString("alpha"),
            spectre::es2025::StructuredCloneModule::Node::FromNumber(1.5));
        mapNode.mapEntries.emplace_back(
            spectre::es2025::StructuredCloneModule::Node::FromString("beta"),
            spectre::es2025::StructuredCloneModule::Node::FromBoolean(false));
        root.objectProperties.emplace_back("lookup", mapNode);

        spectre::es2025::StructuredCloneModule::Node setNode;
        setNode.kind = spectre::es2025::StructuredCloneModule::Node::Kind::Set;
        setNode.setEntries.push_back(spectre::es2025::StructuredCloneModule::Node::FromNumber(3.14));
        setNode.setEntries.push_back(spectre::es2025::StructuredCloneModule::Node::FromString("omega"));
        root.objectProperties.emplace_back("bag", setNode);

        spectre::es2025::StructuredCloneModule::Node bufferNode;
        bufferNode.kind = spectre::es2025::StructuredCloneModule::Node::Kind::ArrayBuffer;
        bufferNode.arrayBuffer = plainHandle;
        bufferNode.label = "clone.buffer";
        bufferNode.transfer = true;
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
        typedNode.typedArray.label = "clone.typed";
        root.objectProperties.emplace_back("vector", typedNode);

        spectre::es2025::StructuredCloneModule::CloneOptions options;
        options.enableTransfer = true;
        options.shareSharedBuffers = true;
        options.copyTypedArrayBuffer = true;
        options.transferList.push_back(plainHandle);

        spectre::es2025::StructuredCloneModule::Node cloned;
        ok &= ExpectStatus(cloneModule->Clone(root, cloned, options), StatusCode::Ok, "Clone structured node");
        ok &= ExpectTrue(cloned.kind == spectre::es2025::StructuredCloneModule::Node::Kind::Object,
                         "Clone root object");

        auto findProperty = [](const spectre::es2025::StructuredCloneModule::Node &node,
                               const std::string &key) -> const spectre::es2025::StructuredCloneModule::Node * {
            for (const auto &prop: node.objectProperties) {
                if (prop.first == key) {
                    return &prop.second;
                }
            }
            return nullptr;
        };

        const auto *clonedBuffer = findProperty(cloned, "buffer");
        ok &= ExpectTrue(clonedBuffer != nullptr && clonedBuffer->arrayBuffer != 0, "Cloned buffer present");
        if (clonedBuffer && clonedBuffer->arrayBuffer != 0) {
            std::vector<std::uint8_t> verify(pattern.size());
            ok &= ExpectStatus(arrayModule->CopyOut(clonedBuffer->arrayBuffer, 0, verify.data(), verify.size()),
                               StatusCode::Ok,
                               "CopyOut cloned buffer");
            ok &= ExpectTrue(verify == pattern, "Cloned buffer bytes match");
            ok &= ExpectTrue(arrayModule->Detached(plainHandle), "Original buffer detached");
            arrayModule->Destroy(clonedBuffer->arrayBuffer);
        }

        const auto *clonedShared = findProperty(cloned, "shared");
        ok &= ExpectTrue(clonedShared != nullptr && clonedShared->sharedBuffer != 0, "Cloned shared buffer present");
        if (clonedShared && clonedShared->sharedBuffer != 0) {
            ok &= ExpectTrue(sharedModule->RefCount(sharedHandle) > 1, "Shared buffer refcount incremented");
            sharedModule->Destroy(clonedShared->sharedBuffer);
        }

        const auto *clonedTyped = findProperty(cloned, "vector");
        ok &= ExpectTrue(clonedTyped != nullptr && clonedTyped->typedArray.handle != 0, "Cloned typed array present");
        if (clonedTyped && clonedTyped->typedArray.handle != 0) {
            std::vector<double> values;
            ok &= ExpectStatus(typedModule->ToVector(clonedTyped->typedArray.handle, values), StatusCode::Ok,
                               "Read cloned typed values");
            ok &= ExpectTrue(values.size() == 4, "Cloned typed length");
            ok &= ExpectTrue(std::abs(values[1] + 7.25) < 1e-6, "Cloned typed data preserved");
            typedModule->Destroy(clonedTyped->typedArray.handle);
        }

        const auto &metrics = cloneModule->GetMetrics();
        ok &= ExpectTrue(metrics.cloneCalls == 1, "Clone metrics tracked");
        ok &= ExpectTrue(metrics.objectCopies == 1, "Object copy counted");
        ok &= ExpectTrue(metrics.arrayCopies == 1, "Array copy counted");
        ok &= ExpectTrue(metrics.mapCopies == 1, "Map copy counted");
        ok &= ExpectTrue(metrics.setCopies == 1, "Set copy counted");
        ok &= ExpectTrue(metrics.bufferCopies >= 1, "Buffer copy counted");
        ok &= ExpectTrue(metrics.sharedShares >= 1, "Shared buffer share counted");
        ok &= ExpectTrue(metrics.typedArrayCopies >= 1, "TypedArray copy counted");

        typedModule->Destroy(typedHandle);
        sharedModule->Destroy(sharedHandle);
        arrayModule->Destroy(plainHandle);
        return ok;
    }

    bool StructuredCloneModuleSerializesRoundTrips() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        auto &environment = runtime->EsEnvironment();
        auto *cloneModule = dynamic_cast<spectre::es2025::StructuredCloneModule *>(environment.FindModule(
            "StructuredClone"));
        auto *arrayModule = dynamic_cast<spectre::es2025::ArrayBufferModule *>(environment.FindModule("ArrayBuffer"));
        auto *sharedModule = dynamic_cast<spectre::es2025::SharedArrayBufferModule *>(environment.FindModule(
            "SharedArrayBuffer"));
        auto *typedModule = dynamic_cast<spectre::es2025::TypedArrayModule *>(environment.FindModule("TypedArray"));
        bool ok = ExpectTrue(cloneModule != nullptr, "StructuredClone module available");
        ok &= ExpectTrue(arrayModule != nullptr, "ArrayBuffer module present");
        ok &= ExpectTrue(sharedModule != nullptr, "SharedArrayBuffer module present");
        ok &= ExpectTrue(typedModule != nullptr, "TypedArray module present");
        if (!cloneModule || !arrayModule || !sharedModule || !typedModule) {
            return false;
        }

        spectre::es2025::ArrayBufferModule::Handle plainHandle = 0;
        ok &= ExpectStatus(arrayModule->Create("serialize.source", 32, plainHandle), StatusCode::Ok, "Create buffer");
        std::array<std::uint8_t, 32> seed{};
        for (std::size_t i = 0; i < seed.size(); ++i) {
            seed[i] = static_cast<std::uint8_t>((i * 11) & 0xFFu);
        }
        ok &= ExpectStatus(arrayModule->CopyIn(plainHandle, 0, seed.data(), seed.size()), StatusCode::Ok,
                           "Seed buffer");

        spectre::es2025::SharedArrayBufferModule::Handle sharedHandle = 0;
        ok &= ExpectStatus(sharedModule->Create("serialize.shared", 48, sharedHandle), StatusCode::Ok,
                           "Create shared buffer");
        std::array<std::uint8_t, 48> sharedSeed{};
        for (std::size_t i = 0; i < sharedSeed.size(); ++i) {
            sharedSeed[i] = static_cast<std::uint8_t>((i * 7) & 0xFFu);
        }
        ok &= ExpectStatus(sharedModule->CopyIn(sharedHandle, 0, sharedSeed.data(), sharedSeed.size()), StatusCode::Ok,
                           "Seed shared buffer");

        spectre::es2025::TypedArrayModule::Handle typedHandle = 0;
        ok &= ExpectStatus(typedModule->Create(spectre::es2025::TypedArrayModule::ElementType::Float32,
                                               3,
                                               "serialize.typed",
                                               typedHandle),
                           StatusCode::Ok,
                           "Create typed array");
        ok &= ExpectStatus(typedModule->Set(typedHandle, 0, 1.25), StatusCode::Ok, "Seed typed 0");
        ok &= ExpectStatus(typedModule->Set(typedHandle, 1, -2.5), StatusCode::Ok, "Seed typed 1");
        ok &= ExpectStatus(typedModule->Set(typedHandle, 2, 9.75), StatusCode::Ok, "Seed typed 2");

        spectre::es2025::StructuredCloneModule::Node root;
        root.kind = spectre::es2025::StructuredCloneModule::Node::Kind::Object;
        root.label = "root.serialize";
        root.objectProperties.emplace_back("kind", spectre::es2025::StructuredCloneModule::Node::FromString("sample"));

        spectre::es2025::StructuredCloneModule::Node bufferNode;
        bufferNode.kind = spectre::es2025::StructuredCloneModule::Node::Kind::ArrayBuffer;
        bufferNode.arrayBuffer = plainHandle;
        bufferNode.label = "serialize.buffer";
        root.objectProperties.emplace_back("buffer", bufferNode);

        spectre::es2025::StructuredCloneModule::Node sharedNode;
        sharedNode.kind = spectre::es2025::StructuredCloneModule::Node::Kind::SharedArrayBuffer;
        sharedNode.sharedBuffer = sharedHandle;
        sharedNode.label = "serialize.shared";
        root.objectProperties.emplace_back("shared", sharedNode);

        spectre::es2025::StructuredCloneModule::Node typedNode;
        typedNode.kind = spectre::es2025::StructuredCloneModule::Node::Kind::TypedArray;
        typedNode.typedArray.handle = typedHandle;
        typedNode.typedArray.elementType = spectre::es2025::TypedArrayModule::ElementType::Float32;
        typedNode.typedArray.length = 3;
        typedNode.typedArray.byteOffset = 0;
        typedNode.typedArray.copyBuffer = true;
        typedNode.typedArray.label = "serialize.typed";
        root.objectProperties.emplace_back("vector", typedNode);

        std::vector<std::uint8_t> blob;
        ok &= ExpectStatus(cloneModule->Serialize(root, blob), StatusCode::Ok, "Serialize structured node");
        ok &= ExpectTrue(!blob.empty(), "Serialized payload non-empty");

        spectre::es2025::StructuredCloneModule::Node restored;
        ok &= ExpectStatus(cloneModule->Deserialize(blob.data(), blob.size(), restored), StatusCode::Ok,
                           "Deserialize payload");

        auto findProperty = [](const spectre::es2025::StructuredCloneModule::Node &node,
                               const std::string &key) -> const spectre::es2025::StructuredCloneModule::Node * {
            for (const auto &prop: node.objectProperties) {
                if (prop.first == key) {
                    return &prop.second;
                }
            }
            return nullptr;
        };

        const auto *restoredBuffer = findProperty(restored, "buffer");
        ok &= ExpectTrue(restoredBuffer && restoredBuffer->arrayBuffer != 0, "Restored buffer present");
        if (restoredBuffer && restoredBuffer->arrayBuffer != 0) {
            std::array<std::uint8_t, 32> verify{};
            ok &= ExpectStatus(arrayModule->CopyOut(restoredBuffer->arrayBuffer, 0, verify.data(), verify.size()),
                               StatusCode::Ok,
                               "CopyOut restored buffer");
            ok &= ExpectTrue(std::equal(verify.begin(), verify.end(), seed.begin()), "Restored buffer bytes match");
            arrayModule->Destroy(restoredBuffer->arrayBuffer);
        }

        const auto *restoredShared = findProperty(restored, "shared");
        ok &= ExpectTrue(restoredShared && restoredShared->sharedBuffer != 0, "Restored shared buffer present");
        if (restoredShared && restoredShared->sharedBuffer != 0) {
            std::vector<std::uint8_t> restoredBytes(48);
            ok &= ExpectStatus(
                sharedModule->CopyOut(restoredShared->sharedBuffer, 0, restoredBytes.data(), restoredBytes.size()),
                StatusCode::Ok,
                "CopyOut restored shared buffer");
            ok &= ExpectTrue(std::equal(restoredBytes.begin(), restoredBytes.end(), sharedSeed.begin()),
                             "Restored shared bytes match");
            sharedModule->Destroy(restoredShared->sharedBuffer);
        }

        const auto *restoredTyped = findProperty(restored, "vector");
        ok &= ExpectTrue(restoredTyped && restoredTyped->typedArray.handle != 0, "Restored typed array present");
        if (restoredTyped && restoredTyped->typedArray.handle != 0) {
            std::vector<double> values;
            ok &= ExpectStatus(typedModule->ToVector(restoredTyped->typedArray.handle, values), StatusCode::Ok,
                               "CopyOut restored typed array");
            ok &= ExpectTrue(values.size() == 3, "Restored typed length");
            ok &= ExpectTrue(std::abs(values[2] - 9.75) < 1e-6, "Restored typed values correct");
            typedModule->Destroy(restoredTyped->typedArray.handle);
        }

        const auto &metrics = cloneModule->GetMetrics();
        ok &= ExpectTrue(metrics.serializedBytes == blob.size(), "Serialized bytes tracked");
        ok &= ExpectTrue(metrics.deserializedBytes >= blob.size(), "Deserialized bytes tracked");
        ok &= ExpectTrue(metrics.typedArrayCopies >= 1, "Typed clones in deserialize tracked");

        typedModule->Destroy(typedHandle);
        sharedModule->Destroy(sharedHandle);
        arrayModule->Destroy(plainHandle);
        return ok;
    }

    struct TestCase {
        const char *name;

        bool (*fn)();
    };
}

int main() {
    std::vector<TestCase> tests{
        {"ArrayBufferModuleAllocatesAndPools", ArrayBufferModuleAllocatesAndPools},
        {"ArrayBufferModuleResizesAndDetaches", ArrayBufferModuleResizesAndDetaches},
        {"DefaultConfigPopulatesDefaults", DefaultConfigPopulatesDefaults},
        {"ContextLifecycle", ContextLifecycle},
        {"LoadFailuresSurfaceStatuses", LoadFailuresSurfaceStatuses},
        {"ScriptCompilationAndLookup", ScriptCompilationAndLookup},
        {"LiteralExecutionCoverage", LiteralExecutionCoverage},
        {"ArithmeticExecution", ArithmeticExecution},
        {"DivisionByZeroReported", DivisionByZeroReported},
        {"BytecodeRoundTrip", BytecodeRoundTrip},
        {"DestroyContextRejectsOperations", DestroyContextRejectsOperations},
        {"MultiThreadLifecycle", MultiThreadLifecycle},
        {"SubsystemSuiteProvidesCpuBackends", SubsystemSuiteProvidesCpuBackends},
        {"GlobalModuleInitializesDefaultContext", GlobalModuleInitializesDefaultContext},
        {"GlobalModuleEvaluatesScripts", GlobalModuleEvaluatesScripts},
        {"GlobalModuleReconfigureTogglesGpu", GlobalModuleReconfigureTogglesGpu},
        {"ErrorModuleRegistersBuiltins", ErrorModuleRegistersBuiltins},
        {"ErrorModuleRaisesAndTracksHistory", ErrorModuleRaisesAndTracksHistory},
        {"ErrorModuleDrainsHistory", ErrorModuleDrainsHistory},
        {"ErrorModuleSupportsCustomTypes", ErrorModuleSupportsCustomTypes},
        {"AsyncFunctionModuleDispatchesJobs", AsyncFunctionModuleDispatchesJobs},
        {"AsyncFunctionModuleHandlesDelaysAndCancellation", AsyncFunctionModuleHandlesDelaysAndCancellation},
        {"AsyncIteratorModuleCoordinatesValues", AsyncIteratorModuleCoordinatesValues},
        {"AsyncIteratorModuleHandlesFailuresAndCancellation", AsyncIteratorModuleHandlesFailuresAndCancellation},
        {"PromiseModuleResolvesAndChains", PromiseModuleResolvesAndChains},
        {"PromiseModuleHandlesRejectionFlow", PromiseModuleHandlesRejectionFlow},
        {"FunctionModuleRegistersAndInvokes", FunctionModuleRegistersAndInvokes},
        {"FunctionModuleHandlesDuplicatesAndRemoval", FunctionModuleHandlesDuplicatesAndRemoval},
        {"FunctionModuleGpuToggle", FunctionModuleGpuToggle},
        {"IteratorModuleHandlesRangeListAndCustom", IteratorModuleHandlesRangeListAndCustom},
        {"GeneratorModuleRunsAndBridges", GeneratorModuleRunsAndBridges},
        {"ArrayModuleCreatesDenseAndTracksMetrics", ArrayModuleCreatesDenseAndTracksMetrics},
        {"ArrayModuleSupportsSparseConversions", ArrayModuleSupportsSparseConversions},
        {"ArrayModuleConcatSliceAndBinarySearch", ArrayModuleConcatSliceAndBinarySearch},
        {"ArrayModuleCloneAndClear", ArrayModuleCloneAndClear},
        {"AtomicsModuleAllocatesAndAtomicallyUpdates", AtomicsModuleAllocatesAndAtomicallyUpdates},
        {"BooleanModuleCastsAndBoxes", BooleanModuleCastsAndBoxes},
        {"StringModuleHandlesInterningAndTransforms", StringModuleHandlesInterningAndTransforms},
        {"DateModuleConstructsAndFormats", DateModuleConstructsAndFormats},
        {"NumberModuleHandlesAggregates", NumberModuleHandlesAggregates},
        {"BigIntModulePerformsArithmetic", BigIntModulePerformsArithmetic},
        {"ObjectModuleHandlesPrototypes", ObjectModuleHandlesPrototypes},
        {"ProxyModuleCoordinatesTraps", ProxyModuleCoordinatesTraps},
        {"SymbolModuleManagesSymbols", SymbolModuleManagesSymbols},
        {"RegExpModuleCompilesAndMatches", RegExpModuleCompilesAndMatches},
        {"TypedArrayModuleCoversElementOps", TypedArrayModuleCoversElementOps},
        {"MapModuleMaintainsOrder", MapModuleMaintainsOrder},
        {"SetModuleMaintainsUniqueness", SetModuleMaintainsUniqueness},
        {"WeakSetModuleCompactsInvalidEntries", WeakSetModuleCompactsInvalidEntries},
        {"ReflectModuleProvidesMetaOperations", ReflectModuleProvidesMetaOperations},
        {"WeakRefModuleTracksLifetime", WeakRefModuleTracksLifetime},
        {"WeakMapModulePurgesInvalidKeys", WeakMapModulePurgesInvalidKeys},
        {"MathModuleAcceleratesWorkloads", MathModuleAcceleratesWorkloads},
        {"ShadowRealmModuleCreatesIsolatedRealms", ShadowRealmModuleCreatesIsolatedRealms},
        {"TemporalModuleHandlesInstantsAndDurations", TemporalModuleHandlesInstantsAndDurations},
        {"StructuredCloneModuleClonesComplexGraphs", StructuredCloneModuleClonesComplexGraphs},
        {"StructuredCloneModuleSerializesRoundTrips", StructuredCloneModuleSerializesRoundTrips},
        {"TickAndReconfigureUpdatesState", TickAndReconfigureUpdatesState}
    };

    std::size_t passed = 0;
    for (const auto &test: tests) {
        std::cout << "Running " << test.name << std::endl;
        if (test.fn()) {
            ++passed;
            std::cout << "  [PASS]" << std::endl;
        } else {
            std::cout << "  [FAIL]" << std::endl;
            std::cout << "Executed " << passed << " / " << tests.size() << " tests" << std::endl;
            return 1;
        }
    }

    std::cout << "Executed " << passed << " / " << tests.size() << " tests" << std::endl;
    return 0;
}
