#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre {
    struct ContextConfig;
    struct ScriptSource;
}

namespace spectre::es2025 {
    class GlobalModule final : public Module {
    public:
        GlobalModule();

        std::string_view Name() const noexcept override;

        std::string_view Summary() const noexcept override;

        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;

        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;

        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;

        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode EnsureContext(std::string_view name, std::uint32_t stackSize = 0);

        StatusCode EvaluateScript(std::string_view source,
                                  std::string &outValue,
                                  std::string &outDiagnostics,
                                  std::string_view scriptName = "main",
                                  std::string_view contextName = {});

        StatusCode EvaluateScript(const ScriptSource &source,
                                  std::string &outValue,
                                  std::string &outDiagnostics,
                                  std::string_view contextName = {});

        const std::string &DefaultContext() const noexcept;

        std::uint32_t DefaultStackSize() const noexcept;

        bool GpuEnabled() const noexcept;

    private:
        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        std::string m_DefaultContextName;
        std::uint32_t m_DefaultStackSize;
        bool m_GpuEnabled;
        bool m_Initialized;

        StatusCode EnsureDefaultContext();

        StatusCode EnsureContextInternal(const ContextConfig &config);
    };
}
