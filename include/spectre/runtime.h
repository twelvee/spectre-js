#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/subsystems.h"

namespace spectre::es2025 {
    class Environment;
}


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
        static std::unique_ptr<SpectreRuntime> Create(const RuntimeConfig &config);

        ~SpectreRuntime();

        StatusCode CreateContext(const ContextConfig &config, SpectreContext **outContext);

        StatusCode DestroyContext(const std::string &name);

        EvaluationResult LoadScript(const std::string &contextName, const ScriptSource &script);

        EvaluationResult LoadBytecode(const std::string &contextName, const BytecodeArtifact &artifact);

        EvaluationResult EvaluateSync(const std::string &contextName, const std::string &entryPoint);

        void Tick(const TickInfo &info);

        StatusCode Reconfigure(const RuntimeConfig &config);

        const RuntimeConfig &Config() const;

        detail::SubsystemSuite &Subsystems();

        const detail::SubsystemSuite &Subsystems() const;

        const detail::SubsystemManifest &Manifest() const;

        TickInfo LastTick() const;

        StatusCode GetContext(const std::string &name, const SpectreContext **outContext) const;

        es2025::Environment &EsEnvironment();

        const es2025::Environment &EsEnvironment() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;

        explicit SpectreRuntime(std::unique_ptr<Impl> impl);

        void InitializeEnvironment(const RuntimeConfig &config);
    };
}