#include "spectre/runtime.h"

#include "mode_adapter.h"

#include <memory>
#include <utility>

namespace spectre {
    struct SpectreRuntime::Impl {
        std::unique_ptr<detail::ModeAdapter> mode;
        TickInfo lastTick{0.0, 0};
    };

    std::unique_ptr<SpectreRuntime> SpectreRuntime::Create(const RuntimeConfig &config) {
        auto impl = std::make_unique<Impl>();
        impl->mode = detail::MakeModeAdapter(config);
        if (!impl->mode) {
            RuntimeConfig fallback = config;
            fallback.mode = RuntimeMode::SingleThread;
            impl->mode = detail::MakeModeAdapter(fallback);
            if (!impl->mode) {
                return nullptr;
            }
        }
        return std::unique_ptr<SpectreRuntime>(new SpectreRuntime(std::move(impl)));
    }

    SpectreRuntime::SpectreRuntime(std::unique_ptr<Impl> impl) : m_Impl(std::move(impl)) {
    }

    SpectreRuntime::~SpectreRuntime() = default;

    StatusCode SpectreRuntime::CreateContext(const ContextConfig &config, SpectreContext **outContext) {
        return m_Impl->mode->CreateContext(config, outContext);
    }

    StatusCode SpectreRuntime::DestroyContext(const std::string &name) {
        return m_Impl->mode->DestroyContext(name);
    }

    EvaluationResult SpectreRuntime::LoadScript(const std::string &contextName, const ScriptSource &script) {
        return m_Impl->mode->LoadScript(contextName, script);
    }

    EvaluationResult SpectreRuntime::LoadBytecode(const std::string &contextName, const BytecodeArtifact &artifact) {
        return m_Impl->mode->LoadBytecode(contextName, artifact);
    }

    EvaluationResult SpectreRuntime::EvaluateSync(const std::string &contextName, const std::string &entryPoint) {
        return m_Impl->mode->EvaluateSync(contextName, entryPoint);
    }

    void SpectreRuntime::Tick(const TickInfo &info) {
        m_Impl->lastTick = info;
        m_Impl->mode->Tick(info);
    }

    StatusCode SpectreRuntime::Reconfigure(const RuntimeConfig &config) {
        const auto &current = m_Impl->mode->Config();
        if (config.mode != current.mode) {
            return StatusCode::InvalidArgument;
        }
        return m_Impl->mode->Reconfigure(config);
    }

    const RuntimeConfig &SpectreRuntime::Config() const {
        return m_Impl->mode->Config();
    }

    TickInfo SpectreRuntime::LastTick() const {
        return m_Impl->lastTick;
    }

    StatusCode SpectreRuntime::GetContext(const std::string &name, const SpectreContext **outContext) const {
        return m_Impl->mode->GetContext(name, outContext);
    }
}
