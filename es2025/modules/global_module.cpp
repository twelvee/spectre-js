#include "spectre/es2025/modules/global_module.h"

#include <algorithm>
#include <utility>

#include "spectre/context.h"
#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Global";
        constexpr std::string_view kSummary = "Global object bindings and host-defined intrinsics.";
        constexpr std::string_view kReference = "ECMA-262 Section 18";
        constexpr std::string_view kDefaultContext = "global.main";
        constexpr std::uint32_t kMinimumStackSize = 1u << 16;
    }

    GlobalModule::GlobalModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_DefaultContextName(std::string(kDefaultContext)),
          m_DefaultStackSize(kMinimumStackSize),
          m_GpuEnabled(false),
          m_Initialized(false) {
    }

    std::string_view GlobalModule::Name() const noexcept {
        return kName;
    }

    std::string_view GlobalModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view GlobalModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void GlobalModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        auto heapKilobytes = static_cast<std::uint32_t>(context.config.memory.heapBytes / 1024ULL);
        if (heapKilobytes > 0) {
            std::uint32_t suggested = std::min<std::uint32_t>(heapKilobytes * 2u, 1u << 20);
            m_DefaultStackSize = std::max(kMinimumStackSize, suggested);
        } else {
            m_DefaultStackSize = kMinimumStackSize;
        }
        (void) EnsureDefaultContext();
        m_Initialized = true;
    }

    void GlobalModule::Tick(const TickInfo &, const ModuleTickContext &) noexcept {
        // Job queues are advanced explicitly by host ticks; nothing to do yet.
    }

    void GlobalModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        (void) context;
    }

    void GlobalModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        auto heapKilobytes = static_cast<std::uint32_t>(config.memory.heapBytes / 1024ULL);
        if (heapKilobytes > 0) {
            std::uint32_t suggested = std::min<std::uint32_t>(heapKilobytes * 2u, 1u << 20);
            m_DefaultStackSize = std::max(kMinimumStackSize, suggested);
        }
        (void) EnsureDefaultContext();
    }

    StatusCode GlobalModule::EnsureContext(std::string_view name, std::uint32_t stackSize) {
        if (!m_Runtime) {
            return StatusCode::InternalError;
        }
        if (name.empty()) {
            name = m_DefaultContextName;
        }
        ContextConfig config{};
        config.name = std::string(name);
        config.initialStackSize = stackSize != 0 ? stackSize : m_DefaultStackSize;
        auto status = EnsureContextInternal(config);
        if (status == StatusCode::AlreadyExists) {
            status = StatusCode::Ok;
        }
        return status;
    }

    StatusCode GlobalModule::EvaluateScript(std::string_view source,
                                            std::string &outValue,
                                            std::string &outDiagnostics,
                                            std::string_view scriptName,
                                            std::string_view contextName) {
        ScriptSource script{};
        script.name = scriptName.empty() ? std::string("global::inline") : std::string(scriptName);
        script.source = std::string(source);
        return EvaluateScript(script, outValue, outDiagnostics, contextName);
    }

    StatusCode GlobalModule::EvaluateScript(const ScriptSource &source,
                                            std::string &outValue,
                                            std::string &outDiagnostics,
                                            std::string_view contextName) {
        outValue.clear();
        outDiagnostics.clear();

        if (!m_Runtime) {
            outDiagnostics = "Runtime unavailable";
            return StatusCode::InternalError;
        }

        auto status = EnsureContext(contextName);
        if (status != StatusCode::Ok) {
            outDiagnostics = "Failed to ensure context";
            return status;
        }

        const std::string targetContext = contextName.empty()
                                              ? m_DefaultContextName
                                              : std::string(contextName);

        auto loadResult = m_Runtime->LoadScript(targetContext, source);
        if (loadResult.status != StatusCode::Ok) {
            outDiagnostics = loadResult.diagnostics;
            return loadResult.status;
        }

        auto evalResult = m_Runtime->EvaluateSync(targetContext, source.name);
        outValue = evalResult.value;
        outDiagnostics = evalResult.diagnostics;
        return evalResult.status;
    }

    const std::string &GlobalModule::DefaultContext() const noexcept {
        return m_DefaultContextName;
    }

    std::uint32_t GlobalModule::DefaultStackSize() const noexcept {
        return m_DefaultStackSize;
    }

    bool GlobalModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    StatusCode GlobalModule::EnsureDefaultContext() {
        if (!m_Runtime) {
            return StatusCode::InternalError;
        }
        ContextConfig config{};
        config.name = m_DefaultContextName;
        config.initialStackSize = m_DefaultStackSize;
        return EnsureContextInternal(config);
    }

    StatusCode GlobalModule::EnsureContextInternal(const ContextConfig &config) {
        if (!m_Runtime) {
            return StatusCode::InternalError;
        }
        const SpectreContext *existing = nullptr;
        auto status = m_Runtime->GetContext(config.name, &existing);
        if (status == StatusCode::Ok) {
            return StatusCode::AlreadyExists;
        }
        return m_Runtime->CreateContext(config, nullptr);
    }
}
