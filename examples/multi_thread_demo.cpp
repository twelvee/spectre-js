#include <iostream>

#include "spectre/config.h"
#include "spectre/runtime.h"
#include "spectre/status.h"

int main() {
    auto config = spectre::MakeDefaultConfig();
    config.mode = spectre::RuntimeMode::MultiThread;
    config.enableGpuAcceleration = true;
    auto runtime = spectre::SpectreRuntime::Create(config);
    if (!runtime) {
        std::cout << "Failed to create runtime" << std::endl;
        return 1;
    }

    spectre::ContextConfig contextConfig{"multi", 1u << 17};
    if (runtime->CreateContext(contextConfig, nullptr) != spectre::StatusCode::Ok) {
        std::cout << "Context creation failed" << std::endl;
        return 1;
    }

    spectre::ScriptSource script{"entry", "return 'spectre-mt';"};
    auto loadResult = runtime->LoadScript("multi", script);
    if (loadResult.status != spectre::StatusCode::Ok) {
        std::cout << "Script load failed" << std::endl;
        return 1;
    }

    spectre::TickInfo tickInfo{0.016, 42};
    runtime->Tick(tickInfo);

    auto evalResult = runtime->EvaluateSync("multi", "entry");
    if (evalResult.status != spectre::StatusCode::Ok) {
        std::cout << "Evaluation failed" << std::endl;
        return 1;
    }

    auto manifest = runtime->Manifest();
    std::cout << "Result: " << evalResult.value << " diagnostics: " << evalResult.diagnostics << std::endl;
    std::cout << "Backend parser: " << manifest.parserBackend << std::endl;
    std::cout << "Backend execution: " << manifest.executionBackend << std::endl;

    runtime->DestroyContext("multi");
    return 0;
}
