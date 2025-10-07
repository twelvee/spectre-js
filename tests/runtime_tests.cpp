#include <iostream>
#include <string>
#include <vector>

#include "spectre/config.h"
#include "spectre/context.h"
#include "spectre/runtime.h"
#include "spectre/status.h"
#include "spectre/subsystems.h"

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
        auto bytecodeMissing = runtime->LoadBytecode("ghost", {"bundle", {}});
        ok &= ExpectStatus(bytecodeMissing.status, StatusCode::NotFound, "LoadBytecode missing context");
        return ok;
    }

    bool ScriptStorageAndLookup() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        runtime->CreateContext({"logic", 65536}, nullptr);
        spectre::ScriptSource script{"init", "return 42;"};
        auto loadResult = runtime->LoadScript("logic", script);
        bool ok = ExpectStatus(loadResult.status, StatusCode::Ok, "LoadScript status");
        ok &= ExpectTrue(loadResult.value == "init", "LoadScript returns script name");
        auto evalResult = runtime->EvaluateSync("logic", "init");
        ok &= ExpectStatus(evalResult.status, StatusCode::Ok, "EvaluateSync status");
        ok &= ExpectTrue(evalResult.value == "42", "EvaluateSync literal");
        ok &= ExpectTrue(evalResult.diagnostics == "Literal",
                         std::string("Literal diagnostics actual: ") + evalResult.diagnostics);
        const SpectreContext *logicContext = nullptr;
        auto ctxStatus = runtime->GetContext("logic", &logicContext);
        ok &= ExpectStatus(ctxStatus, StatusCode::Ok, "GetContext status");
        if (logicContext != nullptr) {
            ok &= ExpectTrue(logicContext->ScriptVersion("init") == 1, "Script version increments");
            ok &= ExpectTrue(!logicContext->ScriptNames().empty(), "Script names populated");
        }
        spectre::ScriptSource update{"init", "return 7;"};
        auto secondLoad = runtime->LoadScript("logic", update);
        ok &= ExpectStatus(secondLoad.status, StatusCode::Ok, "Reload script status");
        runtime->GetContext("logic", &logicContext);
        if (logicContext != nullptr) {
            ok &= ExpectTrue(logicContext->ScriptVersion("init") == 2, "Script version updated");
        }
        return ok;
    }

    bool LiteralCoverage() {
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
            ok &= ExpectTrue(eval.diagnostics == "Literal",
                             std::string("Literal eval diagnostics actual: ") + eval.diagnostics);
        }
        auto hashRes = runtime->LoadScript("literals", {"hash", "let x = 1;"});
        ok &= ExpectStatus(hashRes.status, StatusCode::Ok, "Hash load status");
        auto hashEval = runtime->EvaluateSync("literals", "hash");
        ok &= ExpectStatus(hashEval.status, StatusCode::Ok, "Hash eval status");
        ok &= ExpectTrue(hashEval.diagnostics == std::string("Baseline hash"),
                         std::string("Hash diagnostics actual: ") + hashEval.diagnostics);
        ok &= ExpectTrue(!hashEval.value.empty(), "Hash value populated");
        return ok;
    }

    bool BytecodeRoundtripReturnsHash() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::MultiThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime allocation");
        if (!ok) {
            return false;
        }
        ok &= ExpectTrue(runtime->Config().mode == RuntimeMode::MultiThread, "Configured multi thread");
        runtime->CreateContext({"render", 8192}, nullptr);
        spectre::BytecodeArtifact artifact{"bundle", {1, 2, 3, 4}};
        auto loadResult = runtime->LoadBytecode("render", artifact);
        ok &= ExpectStatus(loadResult.status, StatusCode::Ok, "LoadBytecode status");
        auto evalResult = runtime->EvaluateSync("render", "bundle");
        ok &= ExpectStatus(evalResult.status, StatusCode::Ok, "EvaluateSync bytecode status");
        ok &= ExpectTrue(!evalResult.value.empty(), "Bytecode hash present");
        ok &= ExpectTrue(evalResult.diagnostics == "Bytecode hash",
                         std::string("Bytecode diagnostics actual: ") + evalResult.diagnostics);
        spectre::BytecodeArtifact fallback{"empty", {}};
        auto loadFallback = runtime->LoadBytecode("render", fallback);
        ok &= ExpectStatus(loadFallback.status, StatusCode::Ok, "Load empty bytecode status");
        auto evalFallback = runtime->EvaluateSync("render", "empty");
        ok &= ExpectStatus(evalFallback.status, StatusCode::Ok, "Eval empty bytecode status");
        ok &= ExpectTrue(!evalFallback.value.empty(), "Eval empty bytecode hash");
        ok &= ExpectTrue(evalFallback.diagnostics == "Bytecode hash",
                         std::string("Eval empty bytecode diagnostics actual: ") + evalFallback.diagnostics);
        return ok;
    }

    bool DestroyContextRejectsOperations() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        runtime->CreateContext({"ai", 16384}, nullptr);
        auto status = runtime->DestroyContext("ai");
        bool ok = ExpectStatus(status, StatusCode::Ok, "DestroyContext status");
        auto loadResult = runtime->LoadScript("ai", {"noop", "return 'x';"});
        ok &= ExpectStatus(loadResult.status, StatusCode::NotFound, "LoadScript should fail after destroy");
        const SpectreContext *out = nullptr;
        auto getStatus = runtime->GetContext("ai", &out);
        ok &= ExpectStatus(getStatus, StatusCode::NotFound, "GetContext after destroy");
        return ok;
    }

    bool TickAndReconfigureUpdatesState() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        spectre::TickInfo tickInfo{0.008, 12};
        runtime->Tick(tickInfo);
        auto last = runtime->LastTick();
        bool ok = ExpectTrue(last.frameIndex == 12, "Tick frame index");
        ok &= ExpectTrue(last.deltaSeconds == 0.008, "Tick delta seconds");
        auto updated = runtime->Config();
        updated.memory.heapBytes += 4096;
        updated.enableGpuAcceleration = true;
        auto sameModeStatus = runtime->Reconfigure(updated);
        ok &= ExpectStatus(sameModeStatus, StatusCode::Ok, "Reconfigure same mode");
        auto reloaded = runtime->Config();
        ok &= ExpectTrue(reloaded.memory.heapBytes == updated.memory.heapBytes, "Config heap updated");
        ok &= ExpectTrue(reloaded.enableGpuAcceleration, "GPU flag applied");
        updated.mode = RuntimeMode::MultiThread;
        auto modeChangeStatus = runtime->Reconfigure(updated);
        ok &= ExpectStatus(modeChangeStatus, StatusCode::InvalidArgument, "Reconfigure mode change rejected");
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

    bool SubsystemScaffoldingProvidesStubs() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        bool ok = ExpectTrue(runtime != nullptr, "Runtime init");
        if (!ok) {
            return false;
        }
        auto &manifest = runtime->Manifest();
        ok &= ExpectTrue(manifest.parserBackend == "stub.parser.v0", "Parser backend name");
        ok &= ExpectTrue(manifest.bytecodeBackend == "stub.bytecode.v0", "Bytecode backend name");
        ok &= ExpectTrue(manifest.executionBackend == "stub.execution.v0", "Execution backend name");
        ok &= ExpectTrue(manifest.gcBackend == "stub.gc.v0", "GC backend name");
        ok &= ExpectTrue(manifest.memoryBackend == "stub.memory.v0", "Memory backend name");
        ok &= ExpectTrue(manifest.telemetryBackend == "stub.telemetry.v0", "Telemetry backend name");
        ok &= ExpectTrue(manifest.schedulerBackend == "stub.scheduler.v0", "Scheduler backend name");
        ok &= ExpectTrue(manifest.interopBackend == "stub.interop.v0", "Interop backend name");
        auto &suite = runtime->Subsystems();
        ok &= ExpectTrue(suite.parser != nullptr, "Parser stub present");
        ok &= ExpectTrue(suite.bytecode != nullptr, "Bytecode stub present");
        ok &= ExpectTrue(suite.execution != nullptr, "Execution stub present");
        ok &= ExpectTrue(suite.gc != nullptr, "GC stub present");
        ok &= ExpectTrue(suite.memory != nullptr, "Memory stub present");
        ok &= ExpectTrue(suite.telemetry != nullptr, "Telemetry stub present");
        ok &= ExpectTrue(suite.scheduler != nullptr, "Scheduler stub present");
        ok &= ExpectTrue(suite.interop != nullptr, "Interop stub present");
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
        ok &= ExpectStatus(execResponse.status, StatusCode::Ok, "Execution stub status");
        ok &= ExpectTrue(!execResponse.value.empty(), "Execution value populated");
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
        {"ScriptStorageAndLookup", ScriptStorageAndLookup},
        {"LiteralCoverage", LiteralCoverage},
        {"BytecodeRoundtripReturnsHash", BytecodeRoundtripReturnsHash},
        {"DestroyContextRejectsOperations", DestroyContextRejectsOperations},
        {"MultiThreadLifecycle", MultiThreadLifecycle},
        {"SubsystemScaffoldingProvidesStubs", SubsystemScaffoldingProvidesStubs},
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








