#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"
#include "spectre/es2025/modules/object_module.h"

namespace spectre::es2025 {
    class ObjectModule;

    class WeakSetModule final : public Module {
    public:
        using Handle = std::uint64_t;

        struct Metrics {
            std::uint64_t liveSets;
            std::uint64_t totalAllocations;
            std::uint64_t totalReleases;
            std::uint64_t addOps;
            std::uint64_t deleteOps;
            std::uint64_t hasOps;
            std::uint64_t hits;
            std::uint64_t misses;
            std::uint64_t compactions;
            std::uint64_t clears;
            std::uint64_t rehashes;
            std::uint64_t collisions;
            std::uint64_t lastFrameTouched;
            bool gpuOptimized;
        };

        WeakSetModule();

        ~WeakSetModule() override;

        std::string_view Name() const noexcept override;

        std::string_view Summary() const noexcept override;

        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;

        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;

        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;

        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(std::string_view label, Handle &outHandle);

        StatusCode Destroy(Handle handle);

        StatusCode Clear(Handle handle);

        StatusCode Add(Handle handle, ObjectModule::Handle value);

        bool Has(Handle handle, ObjectModule::Handle value) const;

        StatusCode Delete(Handle handle, ObjectModule::Handle value, bool &outDeleted);

        StatusCode Compact(Handle handle);

        std::uint32_t Size(Handle handle) const;

        const Metrics &GetMetrics() const noexcept;

    private:
        struct Entry;
        struct SetRecord;
        struct SlotRecord;

        static constexpr std::uint32_t kInvalidIndex = 0xffffffffu;
        static constexpr std::uint32_t kDeletedIndex = 0xfffffffeu;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        Metrics m_Metrics;
        std::vector<SlotRecord> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        ObjectModule *m_ObjectModule;

        SlotRecord *FindMutableSlot(Handle handle) noexcept;

        const SlotRecord *FindSlot(Handle handle) const noexcept;

        SetRecord *FindMutable(Handle handle) noexcept;

        const SetRecord *Find(Handle handle) const noexcept;

        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;

        static std::uint32_t DecodeSlot(Handle handle) noexcept;

        static std::uint32_t DecodeGeneration(Handle handle) noexcept;

        static std::uint64_t HashKey(ObjectModule::Handle value) noexcept;

        void Touch(SetRecord &record) noexcept;

        void TouchMetrics() noexcept;

        void EnsureCapacity(SetRecord &record);

        void Rehash(SetRecord &record, std::uint32_t newBucketCount);

        std::uint32_t Locate(const SetRecord &record, ObjectModule::Handle value, std::uint64_t hash,
                             std::uint32_t &bucket, bool &collision) const;

        std::uint32_t AllocateEntry(SetRecord &record);

        StatusCode Insert(SetRecord &record, ObjectModule::Handle value, std::uint64_t hash);

        StatusCode Remove(SetRecord &record, ObjectModule::Handle value, std::uint64_t hash, bool &outDeleted);

        void PruneInvalid(SetRecord &record);
    };
}
