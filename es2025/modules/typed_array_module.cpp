
#include "spectre/es2025/modules/typed_array_module.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "spectre/es2025/environment.h"
#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "TypedArray";
        constexpr std::string_view kSummary = "TypedArray views, element kinds, and buffer interop.";
        constexpr std::string_view kReference = "ECMA-262 Section 23.2";
        constexpr std::uint64_t kHandleSlotMask = 0xffffffffull;
        constexpr std::uint64_t kHotFrameWindow = 10;

        template <typename T>
        T ClampToRange(double value) noexcept {
            if (!std::isfinite(value)) {
                return static_cast<T>(0);
            }
            auto clipped = std::trunc(value);
            if (clipped > static_cast<double>(std::numeric_limits<T>::max())) {
                clipped = static_cast<double>(std::numeric_limits<T>::max());
            }
            if (clipped < static_cast<double>(std::numeric_limits<T>::min())) {
                clipped = static_cast<double>(std::numeric_limits<T>::min());
            }
            return static_cast<T>(clipped);
        }

        template <typename T>
        T ClampUnsigned(double value) noexcept {
            if (!std::isfinite(value) || value <= 0.0) {
                return static_cast<T>(0);
            }
            auto truncated = std::trunc(value);
            if (truncated >= static_cast<double>(std::numeric_limits<T>::max())) {
                return std::numeric_limits<T>::max();
            }
            if (truncated <= 0.0) {
                return static_cast<T>(0);
            }
            return static_cast<T>(truncated);
        }

        std::uint8_t ToUint8Clamped(double value) noexcept {
            if (!(value == value)) {
                return 0;
            }
            if (value <= 0.0) {
                return 0;
            }
            if (value >= 255.0) {
                return 255;
            }
            auto rounded = std::floor(value + 0.5);
            if (rounded > 255.0) {
                return 255;
            }
            if (rounded < 0.0) {
                return 0;
            }
            auto diff = rounded - value;
            if (diff == 0.5 && (static_cast<std::uint64_t>(rounded) % 2) == 1) {
                rounded -= 1.0;
            }
            return static_cast<std::uint8_t>(rounded);
        }
    }

    TypedArrayModule::Metrics::Metrics() noexcept
        : createdViews(0),
          destroyedViews(0),
          attachedBuffers(0),
          detachedBuffers(0),
          readOps(0),
          writeOps(0),
          fillOps(0),
          copyOps(0),
          subarrayOps(0),
          clampOps(0),
          activeViews(0),
          hotViews(0),
          lastFrameTouched(0),
          gpuOptimized(false) {
    }

    TypedArrayModule::TypedArrayModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_ArrayBufferModule(nullptr),
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Metrics{},
          m_Slots{},
          m_FreeSlots{} {
    }

    std::string_view TypedArrayModule::Name() const noexcept {
        return kName;
    }

    std::string_view TypedArrayModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view TypedArrayModule::SpecificationReference() const noexcept {
        return kReference;
    }
    void TypedArrayModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Initialized = true;
        m_CurrentFrame = 0;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_Slots.clear();
        m_FreeSlots.clear();

        auto &environment = context.runtime.EsEnvironment();
        auto *bufferModule = environment.FindModule("ArrayBuffer");
        m_ArrayBufferModule = dynamic_cast<ArrayBufferModule *>(bufferModule);
    }

    void TypedArrayModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        RecomputeHotMetrics();
    }

    void TypedArrayModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void TypedArrayModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode TypedArrayModule::Create(ElementType type, std::size_t length, std::string_view label, Handle &outHandle) {
        outHandle = 0;
        if (!m_ArrayBufferModule) {
            return StatusCode::NotFound;
        }
        auto traits = Traits(type);
        if (traits.size == 0) {
            return StatusCode::InvalidArgument;
        }
        if (length > (std::numeric_limits<std::size_t>::max() / traits.size)) {
            return StatusCode::CapacityExceeded;
        }
        auto byteLength = length * static_cast<std::size_t>(traits.size);
        ArrayBufferModule::Handle bufferHandle = 0;
        auto status = m_ArrayBufferModule->Create(label.empty() ? std::string_view("typedarray.buffer") : label,
                                                  byteLength,
                                                  bufferHandle);
        if (status != StatusCode::Ok) {
            return status;
        }
        auto *bufferRecord = m_ArrayBufferModule->FindMutable(bufferHandle);
        if (!bufferRecord) {
            m_ArrayBufferModule->Destroy(bufferHandle);
            return StatusCode::InternalError;
        }
        std::uint32_t slotIndex = AcquireSlot();
        auto &slot = m_Slots[slotIndex];
        slot.inUse = true;
        auto generation = slot.generation;
        auto handle = EncodeHandle(slotIndex, generation);
        slot.record = ViewRecord();
        slot.record.handle = handle;
        slot.record.slot = slotIndex;
        slot.record.generation = generation;
        slot.record.bufferHandle = bufferHandle;
        slot.record.type = type;
        slot.record.length = static_cast<std::uint32_t>(length);
        slot.record.byteOffset = 0;
        slot.record.elementSize = traits.size;
        slot.record.ownsBuffer = true;
        slot.record.bufferVersion = bufferRecord->version;
        slot.record.label = label.empty() ? std::string("typedarray.").append(std::to_string(slotIndex))
                                          : std::string(label);
        slot.record.hot = true;
        slot.record.lastTouchFrame = m_CurrentFrame;

        outHandle = handle;
        m_Metrics.createdViews += 1;
        m_Metrics.attachedBuffers += 1;
        m_Metrics.activeViews += 1;
        if (slot.record.hot) {
            m_Metrics.hotViews += 1;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode TypedArrayModule::FromBuffer(ArrayBufferModule::Handle buffer,
                                            ElementType type,
                                            std::size_t byteOffset,
                                            std::size_t length,
                                            std::string_view label,
                                            Handle &outHandle) {
        outHandle = 0;
        if (!m_ArrayBufferModule) {
            return StatusCode::NotFound;
        }
        if (buffer == 0) {
            return StatusCode::InvalidArgument;
        }
        auto *bufferRecord = m_ArrayBufferModule->FindMutable(buffer);
        if (!bufferRecord || bufferRecord->detached) {
            return StatusCode::InvalidArgument;
        }
        auto traits = Traits(type);
        if (traits.size == 0) {
            return StatusCode::InvalidArgument;
        }
        if (byteOffset % traits.size != 0) {
            return StatusCode::InvalidArgument;
        }
        auto safeOffset = std::min<std::size_t>(byteOffset, bufferRecord->byteLength);
        auto availableBytes = bufferRecord->byteLength - safeOffset;
        auto maxElements = availableBytes / traits.size;
        if (length > maxElements) {
            return StatusCode::InvalidArgument;
        }
        std::uint32_t slotIndex = AcquireSlot();
        auto &slot = m_Slots[slotIndex];
        slot.inUse = true;
        auto generation = slot.generation;
        auto handle = EncodeHandle(slotIndex, generation);

        slot.record = ViewRecord();
        slot.record.handle = handle;
        slot.record.slot = slotIndex;
        slot.record.generation = generation;
        slot.record.bufferHandle = buffer;
        slot.record.type = type;
        slot.record.length = static_cast<std::uint32_t>(length);
        slot.record.byteOffset = static_cast<std::uint32_t>(byteOffset);
        slot.record.elementSize = traits.size;
        slot.record.ownsBuffer = false;
        slot.record.bufferVersion = bufferRecord->version;
        slot.record.label = label.empty() ? std::string("typedarray.view.").append(std::to_string(slotIndex))
                                          : std::string(label);
        slot.record.hot = true;
        slot.record.lastTouchFrame = m_CurrentFrame;

        outHandle = handle;
        m_Metrics.createdViews += 1;
        m_Metrics.attachedBuffers += 1;
        m_Metrics.activeViews += 1;
        if (slot.record.hot) {
            m_Metrics.hotViews += 1;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode TypedArrayModule::Clone(Handle source, std::string_view label, Handle &outHandle) {
        outHandle = 0;
        auto *record = Find(source);
        if (!record) {
            return StatusCode::NotFound;
        }
        Handle cloneHandle = 0;
        auto status = Create(record->type, record->length, label, cloneHandle);
        if (status != StatusCode::Ok) {
            return status;
        }
        auto *cloneRecord = FindMutable(cloneHandle);
        if (!cloneRecord) {
            Destroy(cloneHandle);
            return StatusCode::InternalError;
        }
        auto *sourceBuffer = m_ArrayBufferModule ? m_ArrayBufferModule->FindMutable(record->bufferHandle) : nullptr;
        auto *targetBuffer = m_ArrayBufferModule ? m_ArrayBufferModule->FindMutable(cloneRecord->bufferHandle) : nullptr;
        if (!sourceBuffer || !targetBuffer) {
            Destroy(cloneHandle);
            return StatusCode::InvalidArgument;
        }
        auto byteLength = static_cast<std::size_t>(record->length) * record->elementSize;
        if (byteLength > 0 && sourceBuffer->data && targetBuffer->data) {
            std::memcpy(targetBuffer->data.get() + cloneRecord->byteOffset,
                        sourceBuffer->data.get() + record->byteOffset,
                        byteLength);
        }
        Touch(*cloneRecord);
        outHandle = cloneHandle;
        m_Metrics.copyOps += 1;
        return StatusCode::Ok;
    }
    StatusCode TypedArrayModule::Destroy(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (record->ownsBuffer && m_ArrayBufferModule) {
            m_ArrayBufferModule->Destroy(record->bufferHandle);
        }
        auto slotIndex = record->slot;
        if (record->hot && m_Metrics.hotViews > 0) {
            m_Metrics.hotViews -= 1;
        }
        if (m_Metrics.activeViews > 0) {
            m_Metrics.activeViews -= 1;
        }
        m_Metrics.destroyedViews += 1;
        ReleaseSlot(slotIndex);
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode TypedArrayModule::Detach(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!m_ArrayBufferModule || record->bufferHandle == 0) {
            return StatusCode::InvalidArgument;
        }
        auto status = m_ArrayBufferModule->Detach(record->bufferHandle);
        if (status != StatusCode::Ok) {
            return status;
        }
        record->bufferVersion = 0;
        record->length = 0;
        record->byteOffset = 0;
        record->bufferHandle = 0;
        record->ownsBuffer = false;
        m_Metrics.detachedBuffers += 1;
        Touch(*record);
        return StatusCode::Ok;
    }

    std::size_t TypedArrayModule::Length(Handle handle) const noexcept {
        const auto *record = Find(handle);
        return record ? record->length : 0;
    }

    std::size_t TypedArrayModule::ByteOffset(Handle handle) const noexcept {
        const auto *record = Find(handle);
        return record ? record->byteOffset : 0;
    }

    std::size_t TypedArrayModule::ByteLength(Handle handle) const noexcept {
        const auto *record = Find(handle);
        if (!record) {
            return 0;
        }
        return static_cast<std::size_t>(record->length) * record->elementSize;
    }

    std::size_t TypedArrayModule::ElementSize(Handle handle) const noexcept {
        const auto *record = Find(handle);
        return record ? record->elementSize : 0;
    }

    TypedArrayModule::ElementType TypedArrayModule::TypeOf(Handle handle) const noexcept {
        const auto *record = Find(handle);
        return record ? record->type : ElementType::Int8;
    }

    StatusCode TypedArrayModule::Describe(Handle handle, Snapshot &outSnapshot) const noexcept {
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        outSnapshot.type = record->type;
        outSnapshot.length = record->length;
        outSnapshot.byteOffset = record->byteOffset;
        outSnapshot.elementSize = record->elementSize;
        outSnapshot.byteLength = static_cast<std::size_t>(record->length) * record->elementSize;
        outSnapshot.ownsBuffer = record->ownsBuffer;
        return StatusCode::Ok;
    }
    StatusCode TypedArrayModule::Fill(Handle handle, double value) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (CheckBigInt(record->type)) {
            return StatusCode::InvalidArgument;
        }
        auto *bufferPtr = ResolveMutablePointer(*record);
        if (!bufferPtr) {
            return StatusCode::InvalidArgument;
        }
        auto *base = bufferPtr + record->byteOffset;
        for (std::uint32_t i = 0; i < record->length; ++i) {
            switch (record->type) {
                case ElementType::Int8:
                    reinterpret_cast<std::int8_t *>(base)[i] = ClampToRange<std::int8_t>(value);
                    break;
                case ElementType::Uint8:
                    reinterpret_cast<std::uint8_t *>(base)[i] = ClampUnsigned<std::uint8_t>(value);
                    break;
                case ElementType::Uint8Clamped:
                    reinterpret_cast<std::uint8_t *>(base)[i] = ToUint8Clamped(value);
                    m_Metrics.clampOps += 1;
                    break;
                case ElementType::Int16:
                    reinterpret_cast<std::int16_t *>(base)[i] = ClampToRange<std::int16_t>(value);
                    break;
                case ElementType::Uint16:
                    reinterpret_cast<std::uint16_t *>(base)[i] = ClampUnsigned<std::uint16_t>(value);
                    break;
                case ElementType::Int32:
                    reinterpret_cast<std::int32_t *>(base)[i] = ClampToRange<std::int32_t>(value);
                    break;
                case ElementType::Uint32:
                    reinterpret_cast<std::uint32_t *>(base)[i] = ClampUnsigned<std::uint32_t>(value);
                    break;
                case ElementType::Float32:
                    reinterpret_cast<float *>(base)[i] = static_cast<float>(value);
                    break;
                case ElementType::Float64:
                    reinterpret_cast<double *>(base)[i] = value;
                    break;
                case ElementType::BigInt64:
                case ElementType::BigUint64:
                    return StatusCode::InvalidArgument;
            }
        }
        Touch(*record);
        m_Metrics.fillOps += 1;
        m_Metrics.writeOps += record->length;
        return StatusCode::Ok;
    }

    StatusCode TypedArrayModule::FillBigInt(Handle handle, std::int64_t value) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!CheckBigInt(record->type)) {
            return StatusCode::InvalidArgument;
        }
        auto *bufferPtr = ResolveMutablePointer(*record);
        if (!bufferPtr) {
            return StatusCode::InvalidArgument;
        }
        auto *base = bufferPtr + record->byteOffset;
        if (record->type == ElementType::BigInt64) {
            auto *typed = reinterpret_cast<std::int64_t *>(base);
            for (std::uint32_t i = 0; i < record->length; ++i) {
                typed[i] = value;
            }
        } else {
            auto *typed = reinterpret_cast<std::uint64_t *>(base);
            auto unsignedValue = value < 0 ? 0ull : static_cast<std::uint64_t>(value);
            for (std::uint32_t i = 0; i < record->length; ++i) {
                typed[i] = unsignedValue;
            }
        }
        Touch(*record);
        m_Metrics.fillOps += 1;
        m_Metrics.writeOps += record->length;
        return StatusCode::Ok;
    }
    StatusCode TypedArrayModule::Set(Handle handle, std::size_t index, double value, bool clamp) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (CheckBigInt(record->type)) {
            return StatusCode::InvalidArgument;
        }
        if (!ValidateBounds(*record, index)) {
            return StatusCode::InvalidArgument;
        }
        auto *ptr = ResolveMutablePointer(*record);
        if (!ptr) {
            return StatusCode::InvalidArgument;
        }
        auto *base = ptr + record->byteOffset;
        switch (record->type) {
            case ElementType::Int8:
                reinterpret_cast<std::int8_t *>(base)[index] = ClampToRange<std::int8_t>(value);
                break;
            case ElementType::Uint8:
                reinterpret_cast<std::uint8_t *>(base)[index] = ClampUnsigned<std::uint8_t>(value);
                break;
            case ElementType::Uint8Clamped:
                reinterpret_cast<std::uint8_t *>(base)[index] = clamp ? ToUint8Clamped(value)
                                                                      : ClampUnsigned<std::uint8_t>(value);
                if (clamp) {
                    m_Metrics.clampOps += 1;
                }
                break;
            case ElementType::Int16:
                reinterpret_cast<std::int16_t *>(base)[index] = ClampToRange<std::int16_t>(value);
                break;
            case ElementType::Uint16:
                reinterpret_cast<std::uint16_t *>(base)[index] = ClampUnsigned<std::uint16_t>(value);
                break;
            case ElementType::Int32:
                reinterpret_cast<std::int32_t *>(base)[index] = ClampToRange<std::int32_t>(value);
                break;
            case ElementType::Uint32:
                reinterpret_cast<std::uint32_t *>(base)[index] = ClampUnsigned<std::uint32_t>(value);
                break;
            case ElementType::Float32:
                reinterpret_cast<float *>(base)[index] = static_cast<float>(value);
                break;
            case ElementType::Float64:
                reinterpret_cast<double *>(base)[index] = value;
                break;
            case ElementType::BigInt64:
            case ElementType::BigUint64:
                return StatusCode::InvalidArgument;
        }
        Touch(*record);
        m_Metrics.writeOps += 1;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode TypedArrayModule::SetBigInt(Handle handle, std::size_t index, std::int64_t value) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!CheckBigInt(record->type) || !ValidateBounds(*record, index)) {
            return StatusCode::InvalidArgument;
        }
        auto *ptr = ResolveMutablePointer(*record);
        if (!ptr) {
            return StatusCode::InvalidArgument;
        }
        auto *base = ptr + record->byteOffset;
        if (record->type == ElementType::BigInt64) {
            reinterpret_cast<std::int64_t *>(base)[index] = value;
        } else {
            reinterpret_cast<std::uint64_t *>(base)[index] = value < 0 ? 0ull : static_cast<std::uint64_t>(value);
        }
        Touch(*record);
        m_Metrics.writeOps += 1;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode TypedArrayModule::Get(Handle handle, std::size_t index, double &outValue) const noexcept {
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (CheckBigInt(record->type) || !ValidateBounds(*record, index)) {
            return StatusCode::InvalidArgument;
        }
        const auto *ptr = ResolvePointer(*record);
        if (!ptr) {
            return StatusCode::InvalidArgument;
        }
        const auto *base = ptr + record->byteOffset;
        switch (record->type) {
            case ElementType::Int8:
                outValue = static_cast<double>(reinterpret_cast<const std::int8_t *>(base)[index]);
                break;
            case ElementType::Uint8:
            case ElementType::Uint8Clamped:
                outValue = static_cast<double>(reinterpret_cast<const std::uint8_t *>(base)[index]);
                break;
            case ElementType::Int16:
                outValue = static_cast<double>(reinterpret_cast<const std::int16_t *>(base)[index]);
                break;
            case ElementType::Uint16:
                outValue = static_cast<double>(reinterpret_cast<const std::uint16_t *>(base)[index]);
                break;
            case ElementType::Int32:
                outValue = static_cast<double>(reinterpret_cast<const std::int32_t *>(base)[index]);
                break;
            case ElementType::Uint32:
                outValue = static_cast<double>(reinterpret_cast<const std::uint32_t *>(base)[index]);
                break;
            case ElementType::Float32:
                outValue = static_cast<double>(reinterpret_cast<const float *>(base)[index]);
                break;
            case ElementType::Float64:
                outValue = reinterpret_cast<const double *>(base)[index];
                break;
            case ElementType::BigInt64:
            case ElementType::BigUint64:
                return StatusCode::InvalidArgument;
        }
        m_Metrics.readOps += 1;
        return StatusCode::Ok;
    }

    StatusCode TypedArrayModule::GetBigInt(Handle handle, std::size_t index, std::int64_t &outValue) const noexcept {
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!CheckBigInt(record->type) || !ValidateBounds(*record, index)) {
            return StatusCode::InvalidArgument;
        }
        const auto *ptr = ResolvePointer(*record);
        if (!ptr) {
            return StatusCode::InvalidArgument;
        }
        const auto *base = ptr + record->byteOffset;
        if (record->type == ElementType::BigInt64) {
            outValue = reinterpret_cast<const std::int64_t *>(base)[index];
        } else {
            outValue = static_cast<std::int64_t>(reinterpret_cast<const std::uint64_t *>(base)[index]);
        }
        m_Metrics.readOps += 1;
        return StatusCode::Ok;
    }
    StatusCode TypedArrayModule::CopyWithin(Handle handle,
                                            std::size_t target,
                                            std::size_t begin,
                                            std::size_t end) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (target > record->length || begin > record->length || end > record->length || begin > end) {
            return StatusCode::InvalidArgument;
        }
        auto *ptr = ResolveMutablePointer(*record);
        if (!ptr) {
            return StatusCode::InvalidArgument;
        }
        auto count = std::min(end - begin, static_cast<std::size_t>(record->length) - target);
        auto elementBytes = record->elementSize;
        auto *base = ptr + record->byteOffset;
        std::memmove(base + target * elementBytes, base + begin * elementBytes, count * elementBytes);
        Touch(*record);
        m_Metrics.copyOps += 1;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode TypedArrayModule::Subarray(Handle handle,
                                          std::size_t begin,
                                          std::size_t end,
                                          std::string_view label,
                                          Handle &outHandle) {
        outHandle = 0;
        auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto length = record->length;
        if (begin > length) {
            begin = length;
        }
        if (end > length) {
            end = length;
        }
        if (begin > end) {
            std::swap(begin, end);
        }
        auto subLength = end > begin ? (end - begin) : 0;
        auto byteOffset = record->byteOffset + static_cast<std::uint32_t>(begin * record->elementSize);
        auto status = FromBuffer(record->bufferHandle, record->type, byteOffset, subLength, label, outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.subarrayOps += 1;
        }
        return status;
    }

    StatusCode TypedArrayModule::ToVector(Handle handle, std::vector<double> &outValues) const {
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (CheckBigInt(record->type)) {
            return StatusCode::InvalidArgument;
        }
        const auto *ptr = ResolvePointer(*record);
        if (!ptr) {
            return StatusCode::InvalidArgument;
        }
        outValues.resize(record->length);
        const auto *base = ptr + record->byteOffset;
        for (std::uint32_t i = 0; i < record->length; ++i) {
            switch (record->type) {
                case ElementType::Int8:
                    outValues[i] = static_cast<double>(reinterpret_cast<const std::int8_t *>(base)[i]);
                    break;
                case ElementType::Uint8:
                case ElementType::Uint8Clamped:
                    outValues[i] = static_cast<double>(reinterpret_cast<const std::uint8_t *>(base)[i]);
                    break;
                case ElementType::Int16:
                    outValues[i] = static_cast<double>(reinterpret_cast<const std::int16_t *>(base)[i]);
                    break;
                case ElementType::Uint16:
                    outValues[i] = static_cast<double>(reinterpret_cast<const std::uint16_t *>(base)[i]);
                    break;
                case ElementType::Int32:
                    outValues[i] = static_cast<double>(reinterpret_cast<const std::int32_t *>(base)[i]);
                    break;
                case ElementType::Uint32:
                    outValues[i] = static_cast<double>(reinterpret_cast<const std::uint32_t *>(base)[i]);
                    break;
                case ElementType::Float32:
                    outValues[i] = static_cast<double>(reinterpret_cast<const float *>(base)[i]);
                    break;
                case ElementType::Float64:
                    outValues[i] = reinterpret_cast<const double *>(base)[i];
                    break;
                case ElementType::BigInt64:
                case ElementType::BigUint64:
                    return StatusCode::InvalidArgument;
            }
        }
        m_Metrics.readOps += record->length;
        return StatusCode::Ok;
    }

    StatusCode TypedArrayModule::ToBigIntVector(Handle handle, std::vector<std::int64_t> &outValues) const {
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!CheckBigInt(record->type)) {
            return StatusCode::InvalidArgument;
        }
        const auto *ptr = ResolvePointer(*record);
        if (!ptr) {
            return StatusCode::InvalidArgument;
        }
        outValues.resize(record->length);
        const auto *base = ptr + record->byteOffset;
        if (record->type == ElementType::BigInt64) {
            for (std::uint32_t i = 0; i < record->length; ++i) {
                outValues[i] = reinterpret_cast<const std::int64_t *>(base)[i];
            }
        } else {
            for (std::uint32_t i = 0; i < record->length; ++i) {
                outValues[i] = static_cast<std::int64_t>(reinterpret_cast<const std::uint64_t *>(base)[i]);
            }
        }
        m_Metrics.readOps += record->length;
        return StatusCode::Ok;
    }
    const TypedArrayModule::Metrics &TypedArrayModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool TypedArrayModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    TypedArrayModule::ElementTraits TypedArrayModule::Traits(ElementType type) noexcept {
        switch (type) {
            case ElementType::Int8:
                return ElementTraits{1, false, true, false, false};
            case ElementType::Uint8:
                return ElementTraits{1, false, false, false, false};
            case ElementType::Uint8Clamped:
                return ElementTraits{1, false, false, true, false};
            case ElementType::Int16:
                return ElementTraits{2, false, true, false, false};
            case ElementType::Uint16:
                return ElementTraits{2, false, false, false, false};
            case ElementType::Int32:
                return ElementTraits{4, false, true, false, false};
            case ElementType::Uint32:
                return ElementTraits{4, false, false, false, false};
            case ElementType::Float32:
                return ElementTraits{4, true, true, false, false};
            case ElementType::Float64:
                return ElementTraits{8, true, true, false, false};
            case ElementType::BigInt64:
                return ElementTraits{8, false, true, false, true};
            case ElementType::BigUint64:
                return ElementTraits{8, false, false, false, true};
        }
        return ElementTraits{0, false, false, false, false};
    }

    TypedArrayModule::Handle TypedArrayModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::uint32_t TypedArrayModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & kHandleSlotMask);
    }

    std::uint32_t TypedArrayModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & kHandleSlotMask);
    }

    TypedArrayModule::ViewRecord *TypedArrayModule::FindMutable(Handle handle) noexcept {
        if (handle == 0) {
            return nullptr;
        }
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse || slot.generation != DecodeGeneration(handle)) {
            return nullptr;
        }
        return &slot.record;
    }

    const TypedArrayModule::ViewRecord *TypedArrayModule::Find(Handle handle) const noexcept {
        if (handle == 0) {
            return nullptr;
        }
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        const auto &slot = m_Slots[slotIndex];
        if (!slot.inUse || slot.generation != DecodeGeneration(handle)) {
            return nullptr;
        }
        return &slot.record;
    }

    std::uint32_t TypedArrayModule::AcquireSlot() {
        if (!m_FreeSlots.empty()) {
            auto slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            auto &slot = m_Slots[slotIndex];
            slot.inUse = true;
            slot.generation += 1;
            return slotIndex;
        }
        auto slotIndex = static_cast<std::uint32_t>(m_Slots.size());
        m_Slots.emplace_back();
        auto &slot = m_Slots.back();
        slot.inUse = true;
        slot.generation = 1;
        return slotIndex;
    }

    void TypedArrayModule::ReleaseSlot(std::uint32_t slotIndex) {
        if (slotIndex >= m_Slots.size()) {
            return;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse) {
            return;
        }
        slot.inUse = false;
        slot.generation += 1;
        slot.record = ViewRecord();
        m_FreeSlots.push_back(slotIndex);
    }

    void TypedArrayModule::Reset() {
        m_Slots.clear();
        m_FreeSlots.clear();
        m_CurrentFrame = 0;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void TypedArrayModule::Touch(ViewRecord &record) noexcept {
        record.lastTouchFrame = m_CurrentFrame;
        if (!record.hot) {
            record.hot = true;
            m_Metrics.hotViews += 1;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void TypedArrayModule::RecomputeHotMetrics() noexcept {
        std::uint64_t hotCount = 0;
        for (auto &slot : m_Slots) {
            if (!slot.inUse) {
                continue;
            }
            auto &record = slot.record;
            if (m_CurrentFrame >= record.lastTouchFrame &&
                (m_CurrentFrame - record.lastTouchFrame) <= kHotFrameWindow) {
                record.hot = true;
                hotCount += 1;
            } else {
                record.hot = false;
            }
        }
        m_Metrics.hotViews = hotCount;
    }
    std::uint8_t *TypedArrayModule::ResolveMutablePointer(ViewRecord &record) noexcept {
        ArrayBufferModule::BufferRecord *buffer = nullptr;
        if (ResolveBuffer(record, buffer) != StatusCode::Ok) {
            return nullptr;
        }
        return buffer && buffer->data ? buffer->data.get() : nullptr;
    }

    const std::uint8_t *TypedArrayModule::ResolvePointer(const ViewRecord &record) const noexcept {
        const ArrayBufferModule::BufferRecord *buffer = nullptr;
        if (ResolveBuffer(record, buffer) != StatusCode::Ok) {
            return nullptr;
        }
        return buffer && buffer->data ? buffer->data.get() : nullptr;
    }

    bool TypedArrayModule::ValidateBounds(const ViewRecord &record, std::size_t index) const noexcept {
        return index < record.length;
    }

    bool TypedArrayModule::CheckBigInt(ElementType type) const noexcept {
        return Traits(type).isBigInt;
    }

    StatusCode TypedArrayModule::ResolveBuffer(ViewRecord &record,
                                               ArrayBufferModule::BufferRecord *&outRecord) noexcept {
        outRecord = nullptr;
        if (!m_ArrayBufferModule || record.bufferHandle == 0) {
            return StatusCode::InvalidArgument;
        }
        auto *buffer = m_ArrayBufferModule->FindMutable(record.bufferHandle);
        if (!buffer || buffer->detached) {
            return StatusCode::InvalidArgument;
        }
        auto requiredBytes = static_cast<std::size_t>(record.byteOffset) +
                              static_cast<std::size_t>(record.length) * record.elementSize;
        if (requiredBytes > buffer->byteLength) {
            return StatusCode::InvalidArgument;
        }
        if (buffer->version != record.bufferVersion) {
            record.bufferVersion = buffer->version;
        }
        m_ArrayBufferModule->Touch(*buffer);
        outRecord = buffer;
        return StatusCode::Ok;
    }

    StatusCode TypedArrayModule::ResolveBuffer(const ViewRecord &record,
                                               const ArrayBufferModule::BufferRecord *&outRecord) const noexcept {
        outRecord = nullptr;
        if (!m_ArrayBufferModule || record.bufferHandle == 0) {
            return StatusCode::InvalidArgument;
        }
        auto *buffer = m_ArrayBufferModule->Find(record.bufferHandle);
        if (!buffer || buffer->detached) {
            return StatusCode::InvalidArgument;
        }
        auto requiredBytes = static_cast<std::size_t>(record.byteOffset) +
                              static_cast<std::size_t>(record.length) * record.elementSize;
        if (requiredBytes > buffer->byteLength) {
            return StatusCode::InvalidArgument;
        }
        outRecord = buffer;
        return StatusCode::Ok;
    }
}

