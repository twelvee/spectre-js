#include "spectre/es2025/modules/reflect_module.h"

#include "spectre/runtime.h"
#include "spectre/es2025/environment.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Reflect";
        constexpr std::string_view kSummary = "Reflect helpers for meta-operations and introspection.";
        constexpr std::string_view kReference = "ECMA-262 Section 28";
    }

    ReflectModule::ReflectModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_ObjectModule(nullptr),
          m_Config{},
          m_Metrics{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0) {
    }

    std::string_view ReflectModule::Name() const noexcept {
        return kName;
    }

    std::string_view ReflectModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view ReflectModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void ReflectModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Initialized = true;
        m_CurrentFrame = 0;
        m_Metrics = {};
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_Metrics.lastFrameTouched = 0;
        auto &environment = context.runtime.EsEnvironment();
        auto *module = environment.FindModule("Object");
        m_ObjectModule = dynamic_cast<ObjectModule *>(module);
    }

    void ReflectModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void ReflectModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void ReflectModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode ReflectModule::DefineProperty(ObjectModule::Handle target,
                                             std::string_view key,
                                             const ObjectModule::PropertyDescriptor &descriptor) {
        if (!m_ObjectModule) {
            m_Metrics.failedOps += 1;
            return StatusCode::InternalError;
        }
        auto status = m_ObjectModule->Define(target, key, descriptor);
        if (status == StatusCode::Ok) {
            m_Metrics.defineOps += 1;
            TouchMetrics();
        } else {
            m_Metrics.failedOps += 1;
        }
        return status;
    }

    StatusCode ReflectModule::Set(ObjectModule::Handle target,
                                  std::string_view key,
                                  const ObjectModule::Value &value) {
        if (!m_ObjectModule) {
            m_Metrics.failedOps += 1;
            return StatusCode::InternalError;
        }
        auto status = m_ObjectModule->Set(target, key, value);
        if (status == StatusCode::Ok) {
            m_Metrics.setOps += 1;
            TouchMetrics();
        } else {
            m_Metrics.failedOps += 1;
        }
        return status;
    }

    StatusCode ReflectModule::Get(ObjectModule::Handle target,
                                  std::string_view key,
                                  ObjectModule::Value &outValue) {
        if (!m_ObjectModule) {
            outValue.Reset();
            m_Metrics.failedOps += 1;
            return StatusCode::InternalError;
        }
        auto status = m_ObjectModule->Get(target, key, outValue);
        if (status == StatusCode::Ok) {
            m_Metrics.getOps += 1;
            TouchMetrics();
        } else {
            m_Metrics.failedOps += 1;
        }
        return status;
    }

    StatusCode ReflectModule::DeleteProperty(ObjectModule::Handle target,
                                             std::string_view key,
                                             bool &outDeleted) {
        if (!m_ObjectModule) {
            outDeleted = false;
            m_Metrics.failedOps += 1;
            return StatusCode::InternalError;
        }
        auto status = m_ObjectModule->Delete(target, key, outDeleted);
        if (status == StatusCode::Ok) {
            m_Metrics.deleteOps += 1;
            TouchMetrics();
        } else {
            m_Metrics.failedOps += 1;
        }
        return status;
    }

    StatusCode ReflectModule::OwnKeys(ObjectModule::Handle target,
                                      std::vector<std::string> &keys) {
        if (!m_ObjectModule) {
            keys.clear();
            m_Metrics.failedOps += 1;
            return StatusCode::InternalError;
        }
        auto status = m_ObjectModule->OwnKeys(target, keys);
        if (status == StatusCode::Ok) {
            m_Metrics.ownKeysOps += 1;
            TouchMetrics();
        } else {
            m_Metrics.failedOps += 1;
        }
        return status;
    }

    bool ReflectModule::Has(ObjectModule::Handle target, std::string_view key) {
        if (!m_ObjectModule) {
            m_Metrics.failedOps += 1;
            return false;
        }
        auto result = m_ObjectModule->Has(target, key);
        m_Metrics.hasOps += 1;
        TouchMetrics();
        return result;
    }

    StatusCode ReflectModule::GetOwnPropertyDescriptor(ObjectModule::Handle target,
                                                       std::string_view key,
                                                       ObjectModule::PropertyDescriptor &outDescriptor) {
        if (!m_ObjectModule) {
            outDescriptor.value.Reset();
            outDescriptor.enumerable = false;
            outDescriptor.configurable = false;
            outDescriptor.writable = false;
            m_Metrics.failedOps += 1;
            return StatusCode::InternalError;
        }
        auto status = m_ObjectModule->Describe(target, key, outDescriptor);
        if (status == StatusCode::Ok) {
            m_Metrics.descriptorQueries += 1;
            TouchMetrics();
        } else {
            m_Metrics.failedOps += 1;
        }
        return status;
    }

    ObjectModule::Handle ReflectModule::GetPrototypeOf(ObjectModule::Handle target) {
        if (!m_ObjectModule) {
            m_Metrics.failedOps += 1;
            return 0;
        }
        auto prototype = m_ObjectModule->Prototype(target);
        m_Metrics.prototypeGets += 1;
        TouchMetrics();
        return prototype;
    }

    StatusCode ReflectModule::SetPrototypeOf(ObjectModule::Handle target, ObjectModule::Handle prototype) {
        if (!m_ObjectModule) {
            m_Metrics.failedOps += 1;
            return StatusCode::InternalError;
        }
        auto status = m_ObjectModule->SetPrototype(target, prototype);
        if (status == StatusCode::Ok) {
            m_Metrics.prototypeSets += 1;
            TouchMetrics();
        } else {
            m_Metrics.failedOps += 1;
        }
        return status;
    }

    bool ReflectModule::IsExtensible(ObjectModule::Handle target) {
        if (!m_ObjectModule) {
            m_Metrics.failedOps += 1;
            return false;
        }
        auto extensible = m_ObjectModule->IsExtensible(target);
        m_Metrics.extensibilityQueries += 1;
        TouchMetrics();
        return extensible;
    }

    StatusCode ReflectModule::PreventExtensions(ObjectModule::Handle target) {
        if (!m_ObjectModule) {
            m_Metrics.failedOps += 1;
            return StatusCode::InternalError;
        }
        auto status = m_ObjectModule->SetExtensible(target, false);
        if (status == StatusCode::Ok) {
            m_Metrics.preventExtensionsOps += 1;
            TouchMetrics();
        } else {
            m_Metrics.failedOps += 1;
        }
        return status;
    }

    const ReflectModule::Metrics &ReflectModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    void ReflectModule::TouchMetrics() noexcept {
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    StatusCode ReflectModule::EnsureObjectModule() const noexcept {
        return m_ObjectModule ? StatusCode::Ok : StatusCode::InternalError;
    }
}