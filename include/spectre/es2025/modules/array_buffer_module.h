#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class ArrayBufferModule final : public Module {
    public:
        using Handle = std::uint64_t;

        struct Metrics {
            std::uint64_t allocations;
            std::uint64_t deallocations;
            std::uint64_t resizes;
            std::uint64_t detaches;
            std::uint64_t copiesIn;
            std::uint64_t copiesOut;
            std::uint64_t copyBetweenBuffers;
            std::uint64_t fills;
            std::uint64_t poolReuses;
            std::uint64_t poolReturns;
            std::uint64_t bytesAllocated;
            std::uint64_t bytesRecycled;
            std::uint64_t bytesInUse;
            std::uint64_t peakBytesInUse;
            std::uint64_t pooledBytes;
            std::uint64_t activeBuffers;
            std::uint64_t lastFrameTouched;
            std::uint64_t hotBuffers;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        ArrayBufferModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(std::string_view label, std::size_t byteLength, Handle &outHandle);
        StatusCode Clone(Handle handle, std::string_view label, Handle &outHandle);
        StatusCode Slice(Handle handle,
                         std::size_t begin,
                         std::size_t end,
                         std::string_view label,
                         Handle &outHandle);
        StatusCode Resize(Handle handle, std::size_t newByteLength, bool preserveData = true);
        StatusCode Detach(Handle handle);
        StatusCode Destroy(Handle handle);

        StatusCode Fill(Handle handle, std::uint8_t value) noexcept;
        StatusCode CopyIn(Handle handle,
                          std::size_t offset,
                          const std::uint8_t *data,
                          std::size_t size) noexcept;
        StatusCode CopyOut(Handle handle,
                          std::size_t offset,
                          std::uint8_t *data,
                          std::size_t size) noexcept;
        StatusCode CopyToBuffer(Handle source,
                                Handle target,
                                std::size_t sourceOffset,
                                std::size_t targetOffset,
                                std::size_t size) noexcept;

        bool Has(Handle handle) const noexcept;
        bool Detached(Handle handle) const noexcept;
        std::size_t ByteLength(Handle handle) const noexcept;
        const Metrics &GetMetrics() const noexcept;
        bool GpuEnabled() const noexcept;

    private:
        struct Block {
            std::unique_ptr<std::uint8_t[]> data;
            std::size_t capacity;

            Block() noexcept;
            Block(std::unique_ptr<std::uint8_t[]> ptr, std::size_t cap) noexcept;
            Block(Block &&other) noexcept;
            Block &operator=(Block &&other) noexcept;
            Block(const Block &) = delete;
            Block &operator=(const Block &) = delete;
        };

        struct BufferRecord {
            Handle handle;
            std::uint32_t slot;
            std::uint32_t generation;
            std::string label;
            std::unique_ptr<std::uint8_t[]> data;
            std::size_t byteLength;
            std::size_t capacity;
            std::uint64_t version;
            std::uint64_t lastTouchFrame;
            bool detachable;
            bool detached;
            bool hot;

            BufferRecord() noexcept;
        };

        struct Slot {
            BufferRecord record;
            std::uint32_t generation;
            bool inUse;

            Slot() noexcept;
        };

        static constexpr std::size_t kPoolBuckets = 32;
        static constexpr std::size_t kMinCapacity = 64;
        static constexpr std::size_t kAlignment = 64;
        static constexpr std::uint64_t kHotFrameWindow = 12;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        std::array<std::vector<Block>, kPoolBuckets> m_Pool;
        std::uint64_t m_TotalPooledBytes;
        Metrics m_Metrics;

        BufferRecord *FindMutable(Handle handle) noexcept;
        const BufferRecord *Find(Handle handle) const noexcept;

        static std::uint32_t DecodeSlot(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;
        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;

        static std::size_t AlignCapacity(std::size_t size) noexcept;
        static std::size_t BucketIndex(std::size_t capacity) noexcept;

        void Reset();
        std::uint32_t AcquireSlot();
        void ReleaseSlot(std::uint32_t slotIndex);

        Block AcquireBlock(std::size_t capacity);
        void ReturnBlock(Block &&block) noexcept;
        void ReturnBlock(BufferRecord &record) noexcept;

        void Touch(BufferRecord &record) noexcept;
        void RecomputeHotMetrics() noexcept;
    };
}




