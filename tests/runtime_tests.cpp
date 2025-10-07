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
