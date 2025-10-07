#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class AtomicsModule final : public Module {
    public:
        using Handle = std::uint64_t;

        enum class MemoryOrder : std::uint8_t {
            Relaxed,
            Acquire,
            Release,
            AcquireRelease,
            SequentiallyConsistent
        };

        struct BufferMetrics {
            std::uint64_t allocations;
            std::uint64_t deallocations;
            std::uint64_t totalWords;
            std::uint64_t maxWords;
            std::uint64_t loadOps;
            std::uint64_t storeOps;
            std::uint64_t rmwOps;
            std::uint64_t compareExchangeHits;
            std::uint64_t compareExchangeMisses;
            std::uint64_t lastFrameTouched;
            std::uint64_t hotBuffers;
            bool gpuOptimized;

            BufferMetrics() noexcept;
        };

        AtomicsModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode CreateBuffer(std::string_view label, std::size_t wordCount, Handle &outHandle);
        StatusCode DestroyBuffer(Handle handle);

        StatusCode Fill(Handle handle,
                        std::int64_t value,
                        MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept;

        StatusCode Load(Handle handle,
                        std::size_t index,
                        std::int64_t &outValue,
                        MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept;

        StatusCode Store(Handle handle,
                         std::size_t index,
                         std::int64_t value,
                         MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept;

        StatusCode Exchange(Handle handle,
                            std::size_t index,
                            std::int64_t value,
                            std::int64_t &outPrevious,
                            MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept;

        StatusCode CompareExchange(Handle handle,
                                   std::size_t index,
                                   std::int64_t expected,
                                   std::int64_t desired,
                                   std::int64_t &outPrevious,
                                   MemoryOrder successOrder = MemoryOrder::SequentiallyConsistent,
                                   MemoryOrder failureOrder = MemoryOrder::Acquire) noexcept;

        StatusCode Add(Handle handle,
                       std::size_t index,
                       std::int64_t value,
                       std::int64_t &outPrevious,
                       MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept;

        StatusCode Sub(Handle handle,
                       std::size_t index,
                       std::int64_t value,
                       std::int64_t &outPrevious,
                       MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept;

        StatusCode And(Handle handle,
                       std::size_t index,
                       std::int64_t value,
                       std::int64_t &outPrevious,
                       MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept;

        StatusCode Or(Handle handle,
                      std::size_t index,
                      std::int64_t value,
                      std::int64_t &outPrevious,
                      MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept;

        StatusCode Xor(Handle handle,
                       std::size_t index,
                       std::int64_t value,
                       std::int64_t &outPrevious,
                       MemoryOrder order = MemoryOrder::SequentiallyConsistent) noexcept;

        StatusCode Snapshot(Handle handle, std::vector<std::int64_t> &outValues) const;

        bool Has(Handle handle) const noexcept;
        std::size_t Capacity(Handle handle) const noexcept;

        const BufferMetrics &Metrics() const noexcept;
        bool GpuEnabled() const noexcept;

    private:
        struct BufferRecord {
            Handle handle;
            std::uint32_t slot;
            std::uint32_t generation;
            std::string label;
            std::vector<std::int64_t> words;
            std::size_t logicalLength;
            std::uint64_t version;
            std::uint64_t lastTouchFrame;
            bool hot;
        };

        struct Slot {
            BufferRecord record;
            std::uint32_t generation;
            bool inUse;
        };

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        BufferMetrics m_Metrics;

        BufferRecord *FindMutable(Handle handle) noexcept;
        const BufferRecord *Find(Handle handle) const noexcept;

        static std::uint32_t DecodeSlot(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;
        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;
        static std::memory_order ToStdOrder(MemoryOrder order) noexcept;

        void ResetRecord(BufferRecord &record,
                         std::string_view label,
                         std::size_t wordCount,
                         Handle handle,
                         std::uint32_t slot,
                         std::uint32_t generation);

        void Touch(BufferRecord &record) noexcept;
        void UpdateMetricsOnCreate(const BufferRecord &record);
        void UpdateMetricsOnDestroy(const BufferRecord &record);
        void RecomputeHotMetrics() noexcept;

        static std::size_t AlignWordCount(std::size_t count) noexcept;
    };
}
