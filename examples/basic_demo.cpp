#include <iostream>

#include "spectre/config.h"
#include "spectre/runtime.h"
#include "spectre/status.h"

int main() {
    auto config = spectre::MakeDefaultConfig();
    auto runtime = spectre::SpectreRuntime::Create(config);

    spectre::ContextConfig contextConfig{"demo", 1u << 16};
    spectre::SpectreContext *context = nullptr;
    auto status = runtime->CreateContext(contextConfig, &context);
    if (status != spectre::StatusCode::Ok && status != spectre::StatusCode::AlreadyExists) {
        std::cout << "Context creation failed" << std::endl;
        return 1;
    }

    spectre::ScriptSource script{"boot", "return 'spectre';"};
    auto loadResult = runtime->LoadScript("demo", script);
    if (loadResult.status != spectre::StatusCode::Ok) {
        std::cout << "Script load failed" << std::endl;
        return 1;
    }

    auto evalResult = runtime->EvaluateSync("demo", "boot");
    if (evalResult.status != spectre::StatusCode::Ok) {
        std::cout << "Evaluation failed" << std::endl;
        return 1;
    }

    std::cout << "Result: " << evalResult.value << " diagnostics: " << evalResult.diagnostics << std::endl;
    return 0;
}
