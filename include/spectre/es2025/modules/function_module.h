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
    struct TickInfo;
}

namespace spectre::es2025 {
    struct FunctionStats {
        std::uint64_t callCount;
        std::uint64_t lastFrameIndex;
        double lastDurationMicros;
    };

    using FunctionCallback = StatusCode (*)(const std::vector<std::string> &args,
                                            std::string &outResult,
                                            void *userData);

    class FunctionModule final : public Module {
    public:
        FunctionModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode RegisterHostFunction(std::string_view name,
                                        FunctionCallback callback,
                                        void *userData = nullptr,
                                        bool overwrite = false);

        bool HasHostFunction(std::string_view name) const noexcept;

        StatusCode InvokeHostFunction(std::string_view name,
                                      const std::vector<std::string> &args,
                                      std::string &outResult,
                                      std::string &outDiagnostics);

        StatusCode RemoveHostFunction(std::string_view name);

        const FunctionStats *GetStats(std::string_view name) const noexcept;

        const std::vector<std::string> &RegisteredNames() const noexcept;

        void Clear();

        bool GpuEnabled() const noexcept;

    private:
        struct Entry {
            std::string name;
            FunctionCallback callback;
            void *userData;
            FunctionStats stats;
            std::string lastDiagnostics;
        };

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::vector<Entry> m_Functions;
        std::vector<std::string> m_Names;
        std::unordered_map<std::string, std::size_t> m_Index;
        std::uint64_t m_CurrentFrame;

        Entry *FindMutable(std::string_view name) noexcept;

        const Entry *Find(std::string_view name) const noexcept;

        void RebuildNames();
    };
}

