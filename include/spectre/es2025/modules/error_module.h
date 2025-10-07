#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre {
    class SpectreRuntime;
}

namespace spectre::es2025 {
    struct ErrorRecord {
        std::string type;
        std::string message;
        std::string contextName;
        std::string scriptName;
        std::string diagnostics;
        std::uint64_t frameIndex;
    };

    class ErrorModule final : public Module {
    public:
        ErrorModule();

        std::string_view Name() const noexcept override;

        std::string_view Summary() const noexcept override;

        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;

        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;

        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;

        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode RegisterErrorType(std::string_view type, std::string_view defaultMessage);

        bool HasErrorType(std::string_view type) const noexcept;

        StatusCode RaiseError(std::string_view type,
                              std::string_view message,
                              std::string_view contextName,
                              std::string_view scriptName,
                              std::string_view diagnostics,
                              std::string &outFormatted,
                              ErrorRecord *outRecord = nullptr);

        const std::vector<ErrorRecord> &History() const noexcept;

        std::vector<ErrorRecord> DrainHistory();

        void ClearHistory() noexcept;

        const ErrorRecord *LastError() const noexcept;

        bool GpuEnabled() const noexcept;

    private:
        struct ErrorDescriptor {
            std::string type;
            std::string defaultMessage;
        };

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::unordered_map<std::string, ErrorDescriptor> m_ErrorTypes;
        std::vector<ErrorRecord> m_History;
        std::uint64_t m_CurrentFrame;

        void RegisterBuiltins();
    };
}