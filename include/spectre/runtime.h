#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"

namespace spectre {

struct TickInfo {
    double deltaSeconds;
    std::uint64_t frameIndex;
};

struct ContextConfig {
    std::string name;
    std::uint32_t initialStackSize;
};

struct ScriptSource {
    std::string name;
    std::string source;
};

struct BytecodeArtifact {
    std::string name;
    std::vector<std::uint8_t> data;
};

struct EvaluationResult {
    StatusCode status;
    std::string value;
    std::string diagnostics;
};

class SpectreContext;

class SpectreRuntime {
public:
    static std::unique_ptr<SpectreRuntime> Create(const RuntimeConfig& config);

    ~SpectreRuntime();

    StatusCode CreateContext(const ContextConfig& config, SpectreContext** outContext);
    StatusCode DestroyContext(const std::string& name);

    EvaluationResult LoadScript(const std::string& contextName, const ScriptSource& script);
    EvaluationResult LoadBytecode(const std::string& contextName, const BytecodeArtifact& artifact);

    EvaluationResult EvaluateSync(const std::string& contextName, const std::string& entryPoint);
    void Tick(const TickInfo& info);
    StatusCode Reconfigure(const RuntimeConfig& config);

    const RuntimeConfig& Config() const;
    TickInfo LastTick() const;
    StatusCode GetContext(const std::string& name, const SpectreContext** outContext) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;

    explicit SpectreRuntime(std::unique_ptr<Impl> impl);
};

}
