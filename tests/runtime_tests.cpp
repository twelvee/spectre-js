#include <iostream>
#include <string>
#include <vector>

#include "spectre/config.h"
#include "spectre/context.h"
#include "spectre/runtime.h"
#include "spectre/status.h"

namespace {
    using spectre::RuntimeConfig;
    using spectre::RuntimeMode;
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
        spectre::SpectreContext *context = nullptr;
        auto status = runtime->CreateContext({"ui", 32768}, &context);
        bool ok = ExpectStatus(status, StatusCode::Ok, "CreateContext should succeed");
        ok &= ExpectTrue(context != nullptr, "Context pointer valid");
        if (context != nullptr) {
            ok &= ExpectTrue(context->Name() == "ui", "Context name");
            ok &= ExpectTrue(context->StackSize() == 32768, "Context stack size");
            ok &= ExpectTrue(context->ScriptNames().empty(), "Script list empty");
        }
        return ok;
    }

    bool ScriptStorageAndLookup() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        runtime->CreateContext({"logic", 65536}, nullptr);
        spectre::ScriptSource script{"init", "let x = 1;"};
        auto loadResult = runtime->LoadScript("logic", script);
        bool ok = ExpectStatus(loadResult.status, StatusCode::Ok, "LoadScript status");
        ok &= ExpectTrue(loadResult.value == "init", "LoadScript returns script name");
        auto evalResult = runtime->EvaluateSync("logic", "init");
        ok &= ExpectStatus(evalResult.status, StatusCode::Ok, "EvaluateSync status");
        ok &= ExpectTrue(evalResult.value == script.source, "EvaluateSync returns source");

        spectre::SpectreContext *uiContext = nullptr;
        runtime->CreateContext({"ui", 4096}, &uiContext);
        if (uiContext != nullptr) {
            spectre::ScriptRecord record;
            record.source = "function tick() {}";
            record.bytecodeHash = "hash";
            auto storeStatus = uiContext->StoreScript("tick", record);
            ok &= ExpectStatus(storeStatus, StatusCode::Ok, "StoreScript status");
            ok &= ExpectTrue(uiContext->HasScript("tick"), "HasScript should find entry");
            const spectre::ScriptRecord *fetched = nullptr;
            auto fetchStatus = uiContext->GetScript("tick", &fetched);
            ok &= ExpectStatus(fetchStatus, StatusCode::Ok, "GetScript status");
            ok &= ExpectTrue(fetched != nullptr && fetched->source == "function tick() {}", "Fetched script source");
        } else {
            ok = false;
        }
        return ok;
    }

    bool BytecodeRoundtripReturnsHash() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::MultiThread));
        runtime->CreateContext({"render", 8192}, nullptr);
        spectre::BytecodeArtifact artifact{"bundle", {1, 2, 3, 4}};
        auto loadResult = runtime->LoadBytecode("render", artifact);
        bool ok = ExpectStatus(loadResult.status, StatusCode::Ok, "LoadBytecode status");
        auto evalResult = runtime->EvaluateSync("render", "bundle");
        ok &= ExpectStatus(evalResult.status, StatusCode::Ok, "EvaluateSync bytecode status");
        ok &= ExpectTrue(!evalResult.value.empty(), "Bytecode hash present");
        ok &= ExpectTrue(evalResult.diagnostics == "Bytecode echo", "Bytecode diagnostics");
        return ok;
    }

    bool DestroyContextRejectsOperations() {
        auto runtime = SpectreRuntime::Create(MakeConfig(RuntimeMode::SingleThread));
        runtime->CreateContext({"ai", 16384}, nullptr);
        auto status = runtime->DestroyContext("ai");
        bool ok = ExpectStatus(status, StatusCode::Ok, "DestroyContext status");
        auto loadResult = runtime->LoadScript("ai", {"noop", ""});
        ok &= ExpectStatus(loadResult.status, StatusCode::NotFound, "LoadScript should fail after destroy");
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
        updated.mode = RuntimeMode::GpuAccelerated;
        auto status = runtime->Reconfigure(updated);
        ok &= ExpectStatus(status, StatusCode::Ok, "Reconfigure status");
        ok &= ExpectTrue(runtime->Config().mode == RuntimeMode::GpuAccelerated, "Config mode updated");
        return ok;
    }

    struct TestCase {
        const char *name;

        bool (*fn)();
    };
} // namespace

int main() {
    std::vector<TestCase> tests{
        {"DefaultConfigPopulatesDefaults", DefaultConfigPopulatesDefaults},
        {"ContextLifecycle", ContextLifecycle},
        {"ScriptStorageAndLookup", ScriptStorageAndLookup},
        {"BytecodeRoundtripReturnsHash", BytecodeRoundtripReturnsHash},
        {"DestroyContextRejectsOperations", DestroyContextRejectsOperations},
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