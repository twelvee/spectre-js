#pragma once

#include <string_view>

namespace spectre {
    class SpectreRuntime;

    struct TickInfo;

    struct RuntimeConfig;

    namespace detail {
        struct SubsystemSuite;
    }
}

namespace spectre::es2025 {
    struct ModuleInitContext {
        SpectreRuntime &runtime;
        detail::SubsystemSuite &subsystems;
        const RuntimeConfig &config;
    };

    struct ModuleTickContext {
        detail::SubsystemSuite &subsystems;
    };

    struct ModuleGpuContext {
        detail::SubsystemSuite &subsystems;
        const RuntimeConfig &config;
        bool enableAcceleration;
    };

    class Module {
    public:
        virtual ~Module() = default;

        virtual std::string_view Name() const noexcept = 0;

        virtual std::string_view Summary() const noexcept {
            return {};
        }

        virtual std::string_view SpecificationReference() const noexcept {
            return {};
        }

        virtual void Initialize(const ModuleInitContext &context) {
            (void) context;
        }

        virtual void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept {
            (void) info;
            (void) context;
        }

        virtual void OptimizeGpu(const ModuleGpuContext &context) noexcept {
            (void) context;
        }

        virtual void Reconfigure(const RuntimeConfig &config) {
            (void) config;
        }
    };
}

