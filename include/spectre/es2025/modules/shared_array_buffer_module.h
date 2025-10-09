#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class SharedArrayBufferModule final : public Module {
    public:
        using Handle = std::uint64_t;

        struct Metrics {
            std::uint64_t allocations;
            std::uint64_t releases;
            std::uint64_t shares;
            std::uint64_t slices;
            std::uint64_t grows;
            std::uint64_t bytesAllocated;
            std::uint64_t bytesInUse;
            std::uint64_t bytesPooled;
            std::uint64_t lastFrameTouched;
            std::uint64_t hotBuffers;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        SharedArrayBufferModule();

        std::string_view Name() const noexcept override;

        std::string_view Summary() const noexcept override;

        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;

        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;

        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;

        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(std::string_view label, std::size_t byteLength, Handle &outHandle);

        StatusCode CreateResizable(std::string_view label,
                                   std::size_t initialByteLength,
                                   std::size_t maxByteLength,
                                   Handle &outHandle);

        StatusCode Share(Handle handle, std::string_view label, Handle &outHandle);

        StatusCode Slice(Handle handle,
                         std::size_t begin,
                         std::size_t end,
                         std::string_view label,
                         Handle &outHandle);

        StatusCode Grow(Handle handle, std::size_t newByteLength, bool preserveData = true);

        StatusCode Destroy(Handle handle);

        StatusCode CopyIn(Handle handle,
                          std::size_t offset,
                          const std::uint8_t *data,
                          std::size_t size);

        StatusCode CopyOut(Handle handle,
                           std::size_t offset,
                           std::uint8_t *data,
                           std::size_t size) const;

        StatusCode Snapshot(Handle handle, std::vector<std::uint8_t> &outBytes) const;

        bool Has(Handle handle) const noexcept;

        std::size_t ByteLength(Handle handle) const noexcept;

        std::size_t MaxByteLength(Handle handle) const noexcept;

        bool Resizable(Handle handle) const noexcept;

        std::uint32_t RefCount(Handle handle) const noexcept;

        const Metrics &GetMetrics() const noexcept;

        bool GpuEnabled() const noexcept;

    private:
        struct Storage {
            std::vector<std::uint8_t> data;
            std::size_t byteLength;
            std::size_t maxByteLength;
            std::uint32_t refCount;
            std::uint64_t version;
            std::uint64_t lastTouchFrame;
            bool resizable;
            bool hot;

            Storage() noexcept;
        };

        struct BufferRecord {
            Handle handle;
            std::uint32_t slot;
            std::uint32_t generation;
            std::uint32_t storageIndex;
            std::uint32_t storageGeneration;
            std::string label;
            std::uint64_t lastTouchFrame;
            bool hot;

            BufferRecord() noexcept;
        };

        struct Slot {
            BufferRecord record;
            std::uint32_t generation;
            bool inUse;

            Slot() noexcept;
        };

        struct StorageSlot {
            Storage storage;
            std::uint32_t generation;
            bool inUse;

            StorageSlot() noexcept;
        };

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        std::vector<StorageSlot> m_Storages;
        std::vector<std::uint32_t> m_FreeStorages;
        Metrics m_Metrics;

        BufferRecord *FindMutable(Handle handle) noexcept;

        const BufferRecord *Find(Handle handle) const noexcept;

        Storage *ResolveStorage(BufferRecord &record) noexcept;

        const Storage *ResolveStorage(const BufferRecord &record) const noexcept;

        static std::uint32_t DecodeSlot(Handle handle) noexcept;

        static std::uint32_t DecodeGeneration(Handle handle) noexcept;

        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;

        void Touch(BufferRecord &record, Storage &storage) noexcept;

        void RecomputeHotMetrics() noexcept;

        std::uint32_t AcquireSlot();

        void ReleaseSlot(std::uint32_t slotIndex);

        std::uint32_t AcquireStorageSlot();

        void ReleaseStorageSlot(std::uint32_t storageIndex);
    };
}