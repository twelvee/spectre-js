#include <iostream>
#include <string>
#include <vector>

#include "spectre/config.h"
#include "spectre/context.h"
#include "spectre/runtime.h"
#include "spectre/status.h"
#include "spectre/subsystems.h"
#include "spectre/es2025/environment.h"
#include "spectre/es2025/modules/global_module.h"
#include "spectre/es2025/modules/error_module.h"
#include "spectre/es2025/modules/function_module.h"
#include "spectre/es2025/modules/atomics_module.h"
#include "spectre/es2025/modules/boolean_module.h"
#include "spectre/es2025/modules/array_module.h"

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
        ok &= ExpectTrue(arrayModule->KindOf(handle) == spectre::es2025::ArrayModule::StorageKind::Sparse, "Converted to sparse");
        spectre::es2025::ArrayModule::Value sparseValue;
        ok &= ExpectTrue(arrayModule->Length(handle) >= 65, "Sparse length expanded");
        status = arrayModule->SortLexicographic(handle, true);
        ok &= ExpectStatus(status, StatusCode::Ok, "Sort lexicographic");
        ok &= ExpectTrue(arrayModule->KindOf(handle) == spectre::es2025::ArrayModule::StorageKind::Dense, "Promoted to dense");
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
        ok &= ExpectStatus(arrayModule->BinarySearch(a, spectre::es2025::ArrayModule::Value(4.0), true, index), StatusCode::Ok, "Binary search value");
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
        ok &= ExpectTrue(arrayModule->KindOf(original) == spectre::es2025::ArrayModule::StorageKind::Dense, "Source dense kind");
        spectre::es2025::ArrayModule::Handle clone = 0;
        ok &= ExpectStatus(arrayModule->Clone(original, "clone-copy", clone), StatusCode::Ok, "Clone array");
        ok &= ExpectTrue(clone != 0, "Clone handle assigned");
        ok &= ExpectTrue(arrayModule->Has(clone), "Clone registered");
        ok &= ExpectTrue(arrayModule->KindOf(clone) == spectre::es2025::ArrayModule::StorageKind::Dense, "Clone dense kind");
        ok &= ExpectTrue(arrayModule->Length(clone) == arrayModule->Length(original), "Clone length");
        spectre::es2025::ArrayModule::Value value;
        std::vector<spectre::es2025::ArrayModule::Value> cloneSlice;
        ok &= ExpectStatus(arrayModule->Slice(clone, 0, arrayModule->Length(original), cloneSlice), StatusCode::Ok, "Clone slice status");
        ok &= ExpectTrue(cloneSlice.size() == arrayModule->Length(original), "Clone slice size");
        bool cloneHasPayload = false;
        for (const auto &entry : cloneSlice) {
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
        ok &= ExpectStatus(booleanModule->Destroy(canonicalTrue), StatusCode::InvalidArgument, "Canonical box protected");
        return ok;
    }

    struct TestCase {
        const char *name;

        bool (*fn)();
    };
}

int main() {
    std::vector<TestCase> tests{
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
        {"FunctionModuleRegistersAndInvokes", FunctionModuleRegistersAndInvokes},
        {"FunctionModuleHandlesDuplicatesAndRemoval", FunctionModuleHandlesDuplicatesAndRemoval},
        {"FunctionModuleGpuToggle", FunctionModuleGpuToggle},
        {"ArrayModuleCreatesDenseAndTracksMetrics", ArrayModuleCreatesDenseAndTracksMetrics},
        {"ArrayModuleSupportsSparseConversions", ArrayModuleSupportsSparseConversions},
        {"ArrayModuleConcatSliceAndBinarySearch", ArrayModuleConcatSliceAndBinarySearch},
        {"ArrayModuleCloneAndClear", ArrayModuleCloneAndClear},
        {"AtomicsModuleAllocatesAndAtomicallyUpdates", AtomicsModuleAllocatesAndAtomicallyUpdates},
        {"BooleanModuleCastsAndBoxes", BooleanModuleCastsAndBoxes},
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
