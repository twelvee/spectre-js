#pragma once

#include <cstdint>
#include <vector>
#include <string_view>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"
#include "spectre/es2025/modules/object_module.h"

namespace spectre::es2025 {
    class WeakRefModule final : public Module {
    public:
        using Handle = std::uint64_t;

        struct Metrics {
            std::uint64_t liveRefs;
            std::uint64_t totalAllocations;
            std::uint64_t totalReleases;
            std::uint64_t derefOps;
            std::uint64_t refreshOps;
            std::uint64_t clearedRefs;
            std::uint64_t resurrectedRefs;
            std::uint64_t failedOps;
            std::uint64_t lastFrameTouched;
            bool gpuOptimized;
        };

        WeakRefModule();

        ~WeakRefModule() override;

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(ObjectModule::Handle target, Handle &outHandle);
        StatusCode Destroy(Handle handle);
        StatusCode Refresh(Handle handle, ObjectModule::Handle target);
        StatusCode Deref(Handle handle, ObjectModule::Handle &outTarget, bool &outAlive);
        bool Alive(Handle handle) const;
        std::uint32_t LiveCount() const noexcept;
        StatusCode Compact();

        const Metrics &GetMetrics() const noexcept;

    private:
        struct Reference {
            Reference();
            ObjectModule::Handle target;
            std::uint64_t version;
            std::uint64_t lastAliveFrame;
        };

        struct SlotRecord {
            SlotRecord();
            bool inUse;
            std::uint32_t generation;
            Reference record;
        };

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        ObjectModule *m_ObjectModule;
        Metrics m_Metrics;
        std::vector<SlotRecord> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        std::uint32_t m_PruneCursor;

        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;
        static std::uint32_t DecodeSlot(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;

        SlotRecord *FindMutableSlot(Handle handle) noexcept;
        const SlotRecord *FindSlot(Handle handle) const noexcept;

        Reference *FindMutable(Handle handle) noexcept;
        const Reference *Find(Handle handle) const noexcept;

        void TouchMetrics() noexcept;
        void PruneInvalid(std::uint32_t budget) noexcept;
    };
}
