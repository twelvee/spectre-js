#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"
#include "spectre/es2025/modules/array_buffer_module.h"

namespace spectre::es2025 {
    class DataViewModule final : public Module {
    public:
        using Handle = std::uint64_t;
        static constexpr std::size_t kUseRemaining = std::numeric_limits<std::size_t>::max();

        struct Metrics {
            std::uint64_t createdViews;
            std::uint64_t destroyedViews;
            std::uint64_t readOps;
            std::uint64_t writeOps;
            std::uint64_t rangeChecks;
            std::uint64_t hotViews;
            std::uint64_t activeViews;
            std::uint64_t lastFrameTouched;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        struct Snapshot {
            std::size_t byteOffset;
            std::size_t byteLength;
            ArrayBufferModule::Handle bufferHandle;
            bool attached;
        };

        DataViewModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(ArrayBufferModule::Handle buffer,
                          std::size_t byteOffset,
                          std::size_t byteLength,
                          std::string_view label,
                          Handle &outHandle);
        StatusCode Destroy(Handle handle);
        StatusCode Detach(Handle handle);

        StatusCode Describe(Handle handle, Snapshot &outSnapshot) const noexcept;

        StatusCode GetInt8(Handle handle, std::size_t byteOffset, std::int8_t &outValue) const noexcept;
        StatusCode GetUint8(Handle handle, std::size_t byteOffset, std::uint8_t &outValue) const noexcept;
        StatusCode GetInt16(Handle handle, std::size_t byteOffset, bool littleEndian, std::int16_t &outValue) const noexcept;
        StatusCode GetUint16(Handle handle, std::size_t byteOffset, bool littleEndian, std::uint16_t &outValue) const noexcept;
        StatusCode GetInt32(Handle handle, std::size_t byteOffset, bool littleEndian, std::int32_t &outValue) const noexcept;
        StatusCode GetUint32(Handle handle, std::size_t byteOffset, bool littleEndian, std::uint32_t &outValue) const noexcept;
        StatusCode GetFloat32(Handle handle, std::size_t byteOffset, bool littleEndian, float &outValue) const noexcept;
        StatusCode GetFloat64(Handle handle, std::size_t byteOffset, bool littleEndian, double &outValue) const noexcept;
        StatusCode GetBigInt64(Handle handle, std::size_t byteOffset, bool littleEndian, std::int64_t &outValue) const noexcept;
        StatusCode GetBigUint64(Handle handle, std::size_t byteOffset, bool littleEndian, std::uint64_t &outValue) const noexcept;

        StatusCode SetInt8(Handle handle, std::size_t byteOffset, std::int8_t value) noexcept;
        StatusCode SetUint8(Handle handle, std::size_t byteOffset, std::uint8_t value) noexcept;
        StatusCode SetInt16(Handle handle, std::size_t byteOffset, std::int16_t value, bool littleEndian) noexcept;
        StatusCode SetUint16(Handle handle, std::size_t byteOffset, std::uint16_t value, bool littleEndian) noexcept;
        StatusCode SetInt32(Handle handle, std::size_t byteOffset, std::int32_t value, bool littleEndian) noexcept;
        StatusCode SetUint32(Handle handle, std::size_t byteOffset, std::uint32_t value, bool littleEndian) noexcept;
        StatusCode SetFloat32(Handle handle, std::size_t byteOffset, float value, bool littleEndian) noexcept;
        StatusCode SetFloat64(Handle handle, std::size_t byteOffset, double value, bool littleEndian) noexcept;
        StatusCode SetBigInt64(Handle handle, std::size_t byteOffset, std::int64_t value, bool littleEndian) noexcept;
        StatusCode SetBigUint64(Handle handle, std::size_t byteOffset, std::uint64_t value, bool littleEndian) noexcept;

        const Metrics &GetMetrics() const noexcept;
        bool GpuEnabled() const noexcept;

    private:
        struct ViewRecord {
            ViewRecord() noexcept;

            Handle handle;
            std::uint32_t slot;
            std::uint32_t generation;
            ArrayBufferModule::Handle bufferHandle;
            std::uint32_t byteOffset;
            std::uint32_t byteLength;
            std::uint64_t bufferVersion;
            std::uint64_t lastTouchFrame;
            bool attached;
            bool hot;
            std::string label;
        };

        struct SlotRecord {
            SlotRecord() noexcept;

            bool inUse;
            std::uint32_t generation;
            ViewRecord record;
        };

        static constexpr std::uint64_t kHandleSlotMask = 0xffffffffull;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        ArrayBufferModule *m_ArrayBufferModule;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        mutable Metrics m_Metrics;
        std::vector<SlotRecord> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;

        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;
        static std::uint32_t DecodeSlot(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;

        ViewRecord *FindMutable(Handle handle) noexcept;
        const ViewRecord *Find(Handle handle) const noexcept;

        std::uint32_t AcquireSlot();
        void ReleaseSlot(std::uint32_t slotIndex);
        void Reset();
        void Touch(ViewRecord &record) noexcept;
        void RecomputeHotMetrics() noexcept;

        bool ValidateRange(const ViewRecord &record, std::size_t byteOffset, std::size_t width) const noexcept;
        StatusCode ResolveBuffer(ViewRecord &record, ArrayBufferModule::BufferRecord *&outRecord) noexcept;
        StatusCode ResolveBuffer(const ViewRecord &record, const ArrayBufferModule::BufferRecord *&outRecord) const noexcept;

        template <typename T>
        StatusCode GetScalar(Handle handle,
                             std::size_t byteOffset,
                             bool littleEndian,
                             T &outValue) const noexcept;

        template <typename T>
        StatusCode SetScalar(Handle handle,
                             std::size_t byteOffset,
                             T value,
                             bool littleEndian) noexcept;
    };
}
