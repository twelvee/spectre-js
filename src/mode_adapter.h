#pragma once

#include <memory>
#include <string>

#include "spectre/context.h"
#include "spectre/runtime.h"
#include "spectre/status.h"

namespace spectre::detail {
    class ModeAdapter {
    public:
        virtual ~ModeAdapter() = default;

        virtual StatusCode CreateContext(const ContextConfig &config, SpectreContext **outContext) = 0;

        virtual StatusCode DestroyContext(const std::string &name) = 0;

        virtual EvaluationResult LoadScript(const std::string &contextName, const ScriptSource &script) = 0;

        virtual EvaluationResult LoadBytecode(const std::string &contextName, const BytecodeArtifact &artifact) = 0;

        virtual EvaluationResult EvaluateSync(const std::string &contextName, const std::string &entryPoint) = 0;

        virtual void Tick(const TickInfo &info) = 0;

        virtual StatusCode Reconfigure(const RuntimeConfig &config) = 0;

        virtual const RuntimeConfig &Config() const = 0;

        virtual StatusCode GetContext(const std::string &name, const SpectreContext **outContext) const = 0;
    };

    std::unique_ptr<ModeAdapter> MakeModeAdapter(const RuntimeConfig &config);
}