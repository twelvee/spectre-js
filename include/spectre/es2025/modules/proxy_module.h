#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "spectre/es2025/module.h"
#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/modules/object_module.h"
#include "spectre/es2025/value.h"

namespace spectre::es2025 {
    class ProxyModule final : public Module {
    public:
        using Handle = std::uint64_t;

        struct TrapTable {
            using GetTrap = StatusCode (*)(ObjectModule &, ObjectModule::Handle, std::string_view, Value &, void *);
            using SetTrap = StatusCode (*)(ObjectModule &, ObjectModule::Handle, std::string_view, const Value &, void *);
            using HasTrap = StatusCode (*)(ObjectModule &, ObjectModule::Handle, std::string_view, bool &, void *);
            using DeleteTrap = StatusCode (*)(ObjectModule &, ObjectModule::Handle, std::string_view, bool &, void *);
            using KeysTrap = StatusCode (*)(ObjectModule &, ObjectModule::Handle, std::vector<std::string> &, void *);

            GetTrap get;
            SetTrap set;
            HasTrap has;
            DeleteTrap drop;
            KeysTrap keys;
            void *userdata;
        };

        struct Metrics {
            std::uint64_t liveProxies;
            std::uint64_t allocations;
            std::uint64_t releases;
            std::uint64_t trapHits;
            std::uint64_t fallbackHits;
            std::uint64_t revocations;
            std::uint64_t misses;
            std::uint64_t lastFrameTouched;
            bool gpuOptimized;
        };

        ProxyModule();
        ~ProxyModule() override;

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(ObjectModule::Handle target, const TrapTable &traps, Handle &outHandle);
        StatusCode Destroy(Handle handle);
        StatusCode Revoke(Handle handle);

        StatusCode Get(Handle handle, std::string_view key, Value &outValue);
        StatusCode Set(Handle handle, std::string_view key, const Value &value);
        StatusCode Has(Handle handle, std::string_view key, bool &outHas);
        StatusCode Delete(Handle handle, std::string_view key, bool &outDeleted);
        StatusCode OwnKeys(Handle handle, std::vector<std::string> &keys);

        const Metrics &GetMetrics() const noexcept;

    private:
        struct ProxyRecord;
        struct SlotRecord;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        ObjectModule *m_ObjectModule;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        Metrics m_Metrics;
        std::vector<SlotRecord> m_Slots;
        std::vector<std::uint32_t> m_FreeList;

        SlotRecord *FindMutableSlot(Handle handle) noexcept;
        const SlotRecord *FindSlot(Handle handle) const noexcept;

        ProxyRecord *FindMutable(Handle handle) noexcept;
        const ProxyRecord *Find(Handle handle) const noexcept;

        void Touch(ProxyRecord &record) noexcept;
        void TouchMetrics() noexcept;

        StatusCode EnsureTarget(ProxyRecord &record) const;

        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;
        static std::uint32_t DecodeSlot(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;
    };
}


