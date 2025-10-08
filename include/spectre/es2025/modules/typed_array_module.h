#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"
#include "spectre/es2025/modules/array_buffer_module.h"

namespace spectre::es2025 {
    class TypedArrayModule final : public Module {
    public:
        using Handle = std::uint64_t;

        enum class ElementType : std::uint8_t {
            Int8,
            Uint8,
            Uint8Clamped,
            Int16,
            Uint16,
            Int32,
            Uint32,
            Float32,
            Float64,
            BigInt64,
            BigUint64
        };

        struct Metrics {
            std::uint64_t createdViews;
            std::uint64_t destroyedViews;
            std::uint64_t attachedBuffers;
            std::uint64_t detachedBuffers;
            std::uint64_t readOps;
            std::uint64_t writeOps;
            std::uint64_t fillOps;
            std::uint64_t copyOps;
            std::uint64_t subarrayOps;
            std::uint64_t clampOps;
            std::uint64_t activeViews;
            std::uint64_t hotViews;
            std::uint64_t lastFrameTouched;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        struct Snapshot {
            ElementType type;
            std::size_t length;
            std::size_t byteOffset;
            std::size_t byteLength;
            std::size_t elementSize;
            bool ownsBuffer;
        };

        TypedArrayModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(ElementType type, std::size_t length, std::string_view label, Handle &outHandle);
        StatusCode FromBuffer(ArrayBufferModule::Handle buffer,
                              ElementType type,
                              std::size_t byteOffset,
                              std::size_t length,
                              std::string_view label,
                              Handle &outHandle);
        StatusCode Clone(Handle source, std::string_view label, Handle &outHandle);
        StatusCode Destroy(Handle handle);
        StatusCode Detach(Handle handle);

        std::size_t Length(Handle handle) const noexcept;
        std::size_t ByteOffset(Handle handle) const noexcept;
        std::size_t ByteLength(Handle handle) const noexcept;
        std::size_t ElementSize(Handle handle) const noexcept;
        ElementType TypeOf(Handle handle) const noexcept;
        StatusCode Describe(Handle handle, Snapshot &outSnapshot) const noexcept;

        StatusCode Fill(Handle handle, double value) noexcept;
        StatusCode FillBigInt(Handle handle, std::int64_t value) noexcept;
        StatusCode Set(Handle handle, std::size_t index, double value, bool clamp = true) noexcept;
        StatusCode SetBigInt(Handle handle, std::size_t index, std::int64_t value) noexcept;
        StatusCode Get(Handle handle, std::size_t index, double &outValue) const noexcept;
        StatusCode GetBigInt(Handle handle, std::size_t index, std::int64_t &outValue) const noexcept;
        StatusCode CopyWithin(Handle handle, std::size_t target, std::size_t begin, std::size_t end) noexcept;
        StatusCode Subarray(Handle handle, std::size_t begin, std::size_t end, std::string_view label,
                            Handle &outHandle);
        StatusCode ToVector(Handle handle, std::vector<double> &outValues) const;
        StatusCode ToBigIntVector(Handle handle, std::vector<std::int64_t> &outValues) const;

        const Metrics &GetMetrics() const noexcept;
        bool GpuEnabled() const noexcept;

    private:
        struct ViewRecord {
            ViewRecord() noexcept
                : handle(0),
                  slot(0),
                  generation(0),
                  bufferHandle(0),
                  type(ElementType::Int8),
                  length(0),
                  byteOffset(0),
                  elementSize(1),
                  ownsBuffer(false),
                  bufferVersion(0),
                  lastTouchFrame(0),
                  hot(false),
                  label() {}

            Handle handle;
            std::uint32_t slot;
            std::uint32_t generation;
            ArrayBufferModule::Handle bufferHandle;
            ElementType type;
            std::uint32_t length;
            std::uint32_t byteOffset;
            std::uint32_t elementSize;
            bool ownsBuffer;
            std::uint64_t bufferVersion;
            std::uint64_t lastTouchFrame;
            bool hot;
            std::string label;
        };

        struct SlotRecord {
            SlotRecord() noexcept
                : inUse(false),
                  generation(0),
                  record() {}

            bool inUse;
            std::uint32_t generation;
            ViewRecord record;
        };

        struct ElementTraits {
            std::uint8_t size;
            bool isFloat;
            bool isSigned;
            bool isClamp;
            bool isBigInt;
        };

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

        static ElementTraits Traits(ElementType type) noexcept;
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

        std::uint8_t *ResolveMutablePointer(ViewRecord &record) noexcept;
        const std::uint8_t *ResolvePointer(const ViewRecord &record) const noexcept;
        bool ValidateBounds(const ViewRecord &record, std::size_t index) const noexcept;
        bool CheckBigInt(ElementType type) const noexcept;
        StatusCode ResolveBuffer(ViewRecord &record, ArrayBufferModule::BufferRecord *&outRecord) noexcept;
        StatusCode ResolveBuffer(const ViewRecord &record, const ArrayBufferModule::BufferRecord *&outRecord) const noexcept;
    };
}

