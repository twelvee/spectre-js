#include <iostream>
#include <string>
#include <vector>

#include "spectre/runtime.h"
#include "spectre/context.h"
#include "spectre/status.h"

using namespace spectre;

int main() {
    auto config = MakeDefaultConfig();
    config.mode = spectre::RuntimeMode::MultiThread;
    auto runtime = SpectreRuntime::Create(config);
    if (!runtime) {
        std::cerr << "Failed to create Spectre runtime" << std::endl;
        return 1;
    }

    auto status = runtime->CreateContext({"showcase", 1u << 16}, nullptr);
    if (status != StatusCode::Ok) {
        std::cerr << "Context creation failed" << std::endl;
        return 1;
    }

    struct ScriptCase {
        const char *name;
        const char *source;
    } cases[]{
                {"stringLiteral", "return 'Spectre';"},
                {"numbers", "return (12 + 8) / 5;"},
                {"precedence", "return 2 + 3 * 4;"},
                {"boolean", "return true;"},
                {"negation", "return -32 * 2;"},
                {"nullish", "return null;"},
                {"undefinedValue", "return undefined;"}
            };

    std::cout << "=== Spectre-JS Showcase ===" << std::endl;
    for (const auto &scriptCase: cases) {
        ScriptSource source{scriptCase.name, scriptCase.source};
        auto loadResult = runtime->LoadScript("showcase", source);
        if (loadResult.status != StatusCode::Ok) {
            std::cerr << "Compile failed for " << scriptCase.name << ": " << loadResult.diagnostics << std::endl;
            continue;
        }
        auto value = runtime->EvaluateSync("showcase", scriptCase.name);
        if (value.status != StatusCode::Ok) {
            std::cerr << "Execution failed for " << scriptCase.name << ": " << value.diagnostics << std::endl;
            continue;
        }
        std::cout << scriptCase.name << " => " << value.value << " (" << value.diagnostics << ")" << std::endl;
    }

    // Demonstrate bytecode reuse by cloning one script into a fresh context.
    const SpectreContext *context = nullptr;
    if (runtime->GetContext("showcase", &context) == StatusCode::Ok && context != nullptr) {
        const ScriptRecord *record = nullptr;
        if (context->GetScript("numbers", &record) == StatusCode::Ok && record != nullptr) {
            runtime->CreateContext({"bytecode", 1u << 16}, nullptr);
            BytecodeArtifact artifact{"numbers", record->bytecode};
            auto loadBytecode = runtime->LoadBytecode("bytecode", artifact);
            if (loadBytecode.status == StatusCode::Ok) {
                auto replay = runtime->EvaluateSync("bytecode", "numbers");
                if (replay.status == StatusCode::Ok) {
                    std::cout << "bytecode::numbers => " << replay.value << " (" << replay.diagnostics << ")" <<
                            std::endl;
                }
            }
        }
    }

    return 0;
}
