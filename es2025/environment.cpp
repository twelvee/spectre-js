#include "spectre/es2025/environment.h"

#include "spectre/es2025/modules/all_modules.h"

namespace spectre::es2025 {
    Environment::Environment() : m_Subsystems(nullptr), m_Config{}, m_ConfigValid(false) {
        Register(std::make_unique<GlobalModule>());
        Register(std::make_unique<ObjectModule>());
        Register(std::make_unique<FunctionModule>());
        Register(std::make_unique<BooleanModule>());
        Register(std::make_unique<SymbolModule>());
        Register(std::make_unique<ErrorModule>());
        Register(std::make_unique<NumberModule>());
        Register(std::make_unique<BigIntModule>());
        Register(std::make_unique<MathModule>());
        Register(std::make_unique<DateModule>());
        Register(std::make_unique<TemporalModule>());
        Register(std::make_unique<StringModule>());
        Register(std::make_unique<RegExpModule>());
        Register(std::make_unique<ArrayModule>());
        Register(std::make_unique<TypedArrayModule>());
        Register(std::make_unique<MapModule>());
        Register(std::make_unique<SetModule>());
        Register(std::make_unique<WeakMapModule>());
        Register(std::make_unique<WeakSetModule>());
        Register(std::make_unique<WeakRefModule>());
        Register(std::make_unique<FinalizationRegistryModule>());
        Register(std::make_unique<ArrayBufferModule>());
        Register(std::make_unique<SharedArrayBufferModule>());
        Register(std::make_unique<DataViewModule>());
        Register(std::make_unique<AtomicsModule>());
        Register(std::make_unique<IteratorModule>());
        Register(std::make_unique<GeneratorModule>());
        Register(std::make_unique<AsyncFunctionModule>());
        Register(std::make_unique<AsyncIteratorModule>());
        Register(std::make_unique<PromiseModule>());
        Register(std::make_unique<ModuleLoaderModule>());
        Register(std::make_unique<JsonModule>());
        Register(std::make_unique<ReflectModule>());
        Register(std::make_unique<ProxyModule>());
        Register(std::make_unique<IntlModule>());
        Register(std::make_unique<ShadowRealmModule>());
        Register(std::make_unique<StructuredCloneModule>());
        BuildIndex();
    }

    Environment::~Environment() = default;

    void Environment::Register(std::unique_ptr<Module> module) {
        if (!module) {
            return;
        }
        auto name = module->Name();
        if (name.empty() || m_Index.find(name) != m_Index.end()) {
            return;
        }
        m_Index.emplace(name, module.get());
        m_Modules.push_back(std::move(module));
    }

    void Environment::BuildIndex() {
        m_Index.clear();
        m_Index.reserve(m_Modules.size());
        for (const auto &module: m_Modules) {
            auto name = module->Name();
            if (name.empty()) {
                continue;
            }
            m_Index.emplace(name, module.get());
        }
    }

    void Environment::Initialize(const ModuleInitContext &context) {
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_ConfigValid = true;
        for (auto &module: m_Modules) {
            module->Initialize(context);
        }
        OptimizeGpu(context.config.enableGpuAcceleration);
    }

    void Environment::Tick(const TickInfo &info) {
        if (!m_Subsystems || !m_ConfigValid) {
            return;
        }
        ModuleTickContext tickContext{*m_Subsystems};
        for (auto &module: m_Modules) {
            module->Tick(info, tickContext);
        }
    }

    void Environment::OptimizeGpu(bool enableAcceleration) {
        if (!m_Subsystems || !m_ConfigValid) {
            return;
        }
        m_Config.enableGpuAcceleration = enableAcceleration;
        ModuleGpuContext gpuContext{*m_Subsystems, m_Config, enableAcceleration};
        for (auto &module: m_Modules) {
            module->OptimizeGpu(gpuContext);
        }
    }

    void Environment::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_ConfigValid = true;
        for (auto &module: m_Modules) {
            module->Reconfigure(config);
        }
        OptimizeGpu(config.enableGpuAcceleration);
    }

    Module *Environment::FindModule(std::string_view name) noexcept {
        auto it = m_Index.find(name);
        return it == m_Index.end() ? nullptr : it->second;
    }

    const Module *Environment::FindModule(std::string_view name) const noexcept {
        auto it = m_Index.find(name);
        return it == m_Index.end() ? nullptr : it->second;
    }

    const std::vector<std::unique_ptr<Module> > &Environment::Modules() const noexcept {
        return m_Modules;
    }
}
