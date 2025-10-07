#pragma once

#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "spectre/config.h"
#include "spectre/es2025/module.h"

namespace spectre {
    struct TickInfo;
}

namespace spectre::es2025 {
    class Environment {
    public:
        Environment();
        ~Environment();

        void Initialize(const ModuleInitContext &context);

        void Tick(const TickInfo &info);

        void OptimizeGpu(bool enableAcceleration);

        void Reconfigure(const RuntimeConfig &config);

        Module *FindModule(std::string_view name) noexcept;

        const Module *FindModule(std::string_view name) const noexcept;

        const std::vector<std::unique_ptr<Module>> &Modules() const noexcept;

    private:
        std::vector<std::unique_ptr<Module>> m_Modules;
        std::unordered_map<std::string_view, Module *> m_Index;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_ConfigValid;

        void Register(std::unique_ptr<Module> module);

        void BuildIndex();
    };
}

