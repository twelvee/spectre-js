#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"
#include "spectre/es2025/modules/object_module.h"
#include "spectre/es2025/value.h"

namespace spectre::es2025 {
    class ReflectModule final : public Module {
    public:
        struct Metrics {
            std::uint64_t getOps;
            std::uint64_t setOps;
            std::uint64_t defineOps;
            std::uint64_t deleteOps;
            std::uint64_t hasOps;
            std::uint64_t ownKeysOps;
            std::uint64_t prototypeGets;
            std::uint64_t prototypeSets;
            std::uint64_t extensibilityQueries;
            std::uint64_t preventExtensionsOps;
            std::uint64_t descriptorQueries;
            std::uint64_t failedOps;
            std::uint64_t lastFrameTouched;
            bool gpuOptimized;
        };

        ReflectModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode DefineProperty(ObjectModule::Handle target,
                                  std::string_view key,
                                  const ObjectModule::PropertyDescriptor &descriptor);

        StatusCode Set(ObjectModule::Handle target,
                       std::string_view key,
                       const Value &value);

        StatusCode Get(ObjectModule::Handle target,
                       std::string_view key,
                       Value &outValue);

        StatusCode DeleteProperty(ObjectModule::Handle target,
                                  std::string_view key,
                                  bool &outDeleted);

        StatusCode OwnKeys(ObjectModule::Handle target,
                           std::vector<std::string> &keys);

        bool Has(ObjectModule::Handle target, std::string_view key);

        StatusCode GetOwnPropertyDescriptor(ObjectModule::Handle target,
                                            std::string_view key,
                                            ObjectModule::PropertyDescriptor &outDescriptor);

        ObjectModule::Handle GetPrototypeOf(ObjectModule::Handle target);

        StatusCode SetPrototypeOf(ObjectModule::Handle target, ObjectModule::Handle prototype);

        bool IsExtensible(ObjectModule::Handle target);

        StatusCode PreventExtensions(ObjectModule::Handle target);

        const Metrics &GetMetrics() const noexcept;

    private:
        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        ObjectModule *m_ObjectModule;
        RuntimeConfig m_Config;
        Metrics m_Metrics;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;

        void TouchMetrics() noexcept;
        StatusCode EnsureObjectModule() const noexcept;
    };
}
