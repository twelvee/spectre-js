#include "spectre/es2025/modules/data_view_module.h"

#include <bit>
#include <cstring>
#include <limits>
#include <type_traits>

#include "spectre/es2025/environment.h"
#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "DataView";
        constexpr std::string_view kSummary = "DataView typed buffer accessors and endian utilities.";
        constexpr std::string_view kReference = "ECMA-262 Section 25.3";
        constexpr bool kHostIsLittleEndian = (std::endian::native == std::endian::little);
        constexpr std::uint64_t kHotFrameWindow = 8;

        template <typename T>
        inline T ByteSwapValue(T value) noexcept {
            static_assert(std::is_trivially_copyable_v<T>, "ByteSwapValue requires trivially copyable type");
            if constexpr (sizeof(T) == 1) {
                return value;
            } else if constexpr (sizeof(T) == 2) {
                auto data = std::bit_cast<std::uint16_t>(value);
                data = static_cast<std::uint16_t>((data >> 8) | (data << 8));
                return std::bit_cast<T>(data);
            } else if constexpr (sizeof(T) == 4) {
                auto data = std::bit_cast<std::uint32_t>(value);
                data = (data >> 24) |
                       ((data & 0x00ff0000u) >> 8) |
                       ((data & 0x0000ff00u) << 8) |
                       (data << 24);
                return std::bit_cast<T>(data);
            } else if constexpr (sizeof(T) == 8) {
                auto data = std::bit_cast<std::uint64_t>(value);
                data = (data >> 56) |
                       ((data & 0x00ff000000000000ull) >> 40) |
                       ((data & 0x0000ff0000000000ull) >> 24) |
                       ((data & 0x000000ff00000000ull) >> 8) |
                       ((data & 0x00000000ff000000ull) << 8) |
                       ((data & 0x0000000000ff0000ull) << 24) |
                       ((data & 0x000000000000ff00ull) << 40) |
                       (data << 56);
                return std::bit_cast<T>(data);
            } else {
                static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                              "Unsupported type size for byte swapping");
                return value;
            }
        }

        template <typename T>
        inline T LoadScalar(const std::uint8_t *ptr, bool littleEndian) noexcept {
            T value;
            std::memcpy(&value, ptr, sizeof(T));
            if constexpr (sizeof(T) > 1) {
                if (littleEndian != kHostIsLittleEndian) {
                    value = ByteSwapValue(value);
                }
            }
            return value;
        }

        template <typename T>
        inline void StoreScalar(std::uint8_t *ptr, T value, bool littleEndian) noexcept {
            if constexpr (sizeof(T) > 1) {
                if (littleEndian != kHostIsLittleEndian) {
                    value = ByteSwapValue(value);
                }
            }
            std::memcpy(ptr, &value, sizeof(T));
        }
    }

    DataViewModule::Metrics::Metrics() noexcept
        : createdViews(0),
          destroyedViews(0),
          readOps(0),
          writeOps(0),
          rangeChecks(0),
          hotViews(0),
          activeViews(0),
          lastFrameTouched(0),
          gpuOptimized(false) {}

    DataViewModule::ViewRecord::ViewRecord() noexcept
        : handle(0),
          slot(0),
          generation(0),
          bufferHandle(0),
          byteOffset(0),
          byteLength(0),
          bufferVersion(0),
          lastTouchFrame(0),
          attached(false),
          hot(false),
          label() {}

    DataViewModule::SlotRecord::SlotRecord() noexcept
        : inUse(false),
          generation(0),
          record() {}

    DataViewModule::DataViewModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_ArrayBufferModule(nullptr),
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Metrics(),
          m_Slots(),
          m_FreeSlots() {}

    std::string_view DataViewModule::Name() const noexcept {
        return kName;
    }

    std::string_view DataViewModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view DataViewModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void DataViewModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;

        auto &environment = context.runtime.EsEnvironment();
        auto *bufferModule = environment.FindModule("ArrayBuffer");
        m_ArrayBufferModule = dynamic_cast<ArrayBufferModule *>(bufferModule);

        Reset();
        m_Initialized = true;
    }

    void DataViewModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        RecomputeHotMetrics();
    }

    void DataViewModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void DataViewModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode DataViewModule::Create(ArrayBufferModule::Handle buffer,
                                      std::size_t byteOffset,
                                      std::size_t byteLength,
                                      std::string_view label,
                                      Handle &outHandle) {
        outHandle = 0;
        if (!m_ArrayBufferModule || buffer == 0) {
            return StatusCode::InvalidArgument;
        }

        auto *bufferRecord = m_ArrayBufferModule->FindMutable(buffer);
        if (!bufferRecord || bufferRecord->detached) {
            return StatusCode::InvalidArgument;
        }

        if (byteOffset > bufferRecord->byteLength) {
            return StatusCode::InvalidArgument;
        }

        if (byteLength == kUseRemaining) {
            byteLength = bufferRecord->byteLength - byteOffset;
        }

        if (byteOffset > bufferRecord->byteLength || byteLength > bufferRecord->byteLength) {
            return StatusCode::InvalidArgument;
        }

        if (byteLength > (bufferRecord->byteLength - byteOffset)) {
            return StatusCode::InvalidArgument;
        }

        if (byteOffset > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
            byteLength > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            return StatusCode::CapacityExceeded;
        }

        auto slotIndex = AcquireSlot();
        auto &slot = m_Slots[slotIndex];
        slot.inUse = true;
        auto generation = slot.generation;
        auto handle = EncodeHandle(slotIndex, generation);

        slot.record = ViewRecord();
        slot.record.handle = handle;
        slot.record.slot = slotIndex;
        slot.record.generation = generation;
        slot.record.bufferHandle = buffer;
        slot.record.byteOffset = static_cast<std::uint32_t>(byteOffset);
        slot.record.byteLength = static_cast<std::uint32_t>(byteLength);
        slot.record.bufferVersion = bufferRecord->version;
        slot.record.lastTouchFrame = m_CurrentFrame;
        slot.record.attached = true;
        slot.record.hot = true;
        slot.record.label = label.empty() ? std::string("dataview.").append(std::to_string(slotIndex))
                                          : std::string(label);

        outHandle = handle;
        m_Metrics.createdViews += 1;
        m_Metrics.activeViews += 1;
        if (slot.record.hot) {
            m_Metrics.hotViews += 1;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode DataViewModule::Destroy(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (record->hot && m_Metrics.hotViews > 0) {
            m_Metrics.hotViews -= 1;
        }
        if (m_Metrics.activeViews > 0) {
            m_Metrics.activeViews -= 1;
        }
        m_Metrics.destroyedViews += 1;
        ReleaseSlot(record->slot);
        return StatusCode::Ok;
    }

    StatusCode DataViewModule::Detach(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        record->attached = false;
        record->bufferHandle = 0;
        record->bufferVersion = 0;
        return StatusCode::Ok;
    }

    StatusCode DataViewModule::Describe(Handle handle, Snapshot &outSnapshot) const noexcept {
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        outSnapshot.byteOffset = record->byteOffset;
        outSnapshot.byteLength = record->byteLength;
        outSnapshot.bufferHandle = record->bufferHandle;
        outSnapshot.attached = record->attached;
        return StatusCode::Ok;
    }

    StatusCode DataViewModule::GetInt8(Handle handle, std::size_t byteOffset, std::int8_t &outValue) const noexcept {
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!ValidateRange(*record, byteOffset, sizeof(std::int8_t))) {
            return StatusCode::InvalidArgument;
        }
        const ArrayBufferModule::BufferRecord *buffer = nullptr;
        if (ResolveBuffer(*record, buffer) != StatusCode::Ok) {
            return StatusCode::InvalidArgument;
        }
        if (!buffer->data) {
            return StatusCode::InvalidArgument;
        }
        auto base = static_cast<std::size_t>(record->byteOffset) + byteOffset;
        outValue = static_cast<std::int8_t>(buffer->data[base]);
        m_Metrics.readOps += 1;
        return StatusCode::Ok;
    }

    StatusCode DataViewModule::GetUint8(Handle handle, std::size_t byteOffset, std::uint8_t &outValue) const noexcept {
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!ValidateRange(*record, byteOffset, sizeof(std::uint8_t))) {
            return StatusCode::InvalidArgument;
        }
        const ArrayBufferModule::BufferRecord *buffer = nullptr;
        if (ResolveBuffer(*record, buffer) != StatusCode::Ok || !buffer->data) {
            return StatusCode::InvalidArgument;
        }
        auto base = static_cast<std::size_t>(record->byteOffset) + byteOffset;
        outValue = buffer->data[base];
        m_Metrics.readOps += 1;
        return StatusCode::Ok;
    }

    StatusCode DataViewModule::GetInt16(Handle handle,
                                        std::size_t byteOffset,
                                        bool littleEndian,
                                        std::int16_t &outValue) const noexcept {
        return GetScalar(handle, byteOffset, littleEndian, outValue);
    }

    StatusCode DataViewModule::GetUint16(Handle handle,
                                         std::size_t byteOffset,
                                         bool littleEndian,
                                         std::uint16_t &outValue) const noexcept {
        return GetScalar(handle, byteOffset, littleEndian, outValue);
    }

    StatusCode DataViewModule::GetInt32(Handle handle,
                                        std::size_t byteOffset,
                                        bool littleEndian,
                                        std::int32_t &outValue) const noexcept {
        return GetScalar(handle, byteOffset, littleEndian, outValue);
    }

    StatusCode DataViewModule::GetUint32(Handle handle,
                                         std::size_t byteOffset,
                                         bool littleEndian,
                                         std::uint32_t &outValue) const noexcept {
        return GetScalar(handle, byteOffset, littleEndian, outValue);
    }

    StatusCode DataViewModule::GetFloat32(Handle handle,
                                          std::size_t byteOffset,
                                          bool littleEndian,
                                          float &outValue) const noexcept {
        return GetScalar(handle, byteOffset, littleEndian, outValue);
    }

    StatusCode DataViewModule::GetFloat64(Handle handle,
                                          std::size_t byteOffset,
                                          bool littleEndian,
                                          double &outValue) const noexcept {
        return GetScalar(handle, byteOffset, littleEndian, outValue);
    }

    StatusCode DataViewModule::GetBigInt64(Handle handle,
                                           std::size_t byteOffset,
                                           bool littleEndian,
                                           std::int64_t &outValue) const noexcept {
        return GetScalar(handle, byteOffset, littleEndian, outValue);
    }

    StatusCode DataViewModule::GetBigUint64(Handle handle,
                                            std::size_t byteOffset,
                                            bool littleEndian,
                                            std::uint64_t &outValue) const noexcept {
        return GetScalar(handle, byteOffset, littleEndian, outValue);
    }

    StatusCode DataViewModule::SetInt8(Handle handle, std::size_t byteOffset, std::int8_t value) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!ValidateRange(*record, byteOffset, sizeof(std::int8_t))) {
            return StatusCode::InvalidArgument;
        }
        ArrayBufferModule::BufferRecord *buffer = nullptr;
        if (ResolveBuffer(*record, buffer) != StatusCode::Ok || !buffer->data) {
            return StatusCode::InvalidArgument;
        }
        auto base = static_cast<std::size_t>(record->byteOffset) + byteOffset;
        buffer->data[base] = static_cast<std::uint8_t>(value);
        Touch(*record);
        m_Metrics.writeOps += 1;
        return StatusCode::Ok;
    }

    StatusCode DataViewModule::SetUint8(Handle handle, std::size_t byteOffset, std::uint8_t value) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!ValidateRange(*record, byteOffset, sizeof(std::uint8_t))) {
            return StatusCode::InvalidArgument;
        }
        ArrayBufferModule::BufferRecord *buffer = nullptr;
        if (ResolveBuffer(*record, buffer) != StatusCode::Ok || !buffer->data) {
            return StatusCode::InvalidArgument;
        }
        auto base = static_cast<std::size_t>(record->byteOffset) + byteOffset;
        buffer->data[base] = value;
        Touch(*record);
        m_Metrics.writeOps += 1;
        return StatusCode::Ok;
    }

    StatusCode DataViewModule::SetInt16(Handle handle,
                                        std::size_t byteOffset,
                                        std::int16_t value,
                                        bool littleEndian) noexcept {
        return SetScalar(handle, byteOffset, value, littleEndian);
    }

    StatusCode DataViewModule::SetUint16(Handle handle,
                                         std::size_t byteOffset,
                                         std::uint16_t value,
                                         bool littleEndian) noexcept {
        return SetScalar(handle, byteOffset, value, littleEndian);
    }

    StatusCode DataViewModule::SetInt32(Handle handle,
                                        std::size_t byteOffset,
                                        std::int32_t value,
                                        bool littleEndian) noexcept {
        return SetScalar(handle, byteOffset, value, littleEndian);
    }

    StatusCode DataViewModule::SetUint32(Handle handle,
                                         std::size_t byteOffset,
                                         std::uint32_t value,
                                         bool littleEndian) noexcept {
        return SetScalar(handle, byteOffset, value, littleEndian);
    }

    StatusCode DataViewModule::SetFloat32(Handle handle,
                                          std::size_t byteOffset,
                                          float value,
                                          bool littleEndian) noexcept {
        return SetScalar(handle, byteOffset, value, littleEndian);
    }

    StatusCode DataViewModule::SetFloat64(Handle handle,
                                          std::size_t byteOffset,
                                          double value,
                                          bool littleEndian) noexcept {
        return SetScalar(handle, byteOffset, value, littleEndian);
    }

    StatusCode DataViewModule::SetBigInt64(Handle handle,
                                           std::size_t byteOffset,
                                           std::int64_t value,
                                           bool littleEndian) noexcept {
        return SetScalar(handle, byteOffset, value, littleEndian);
    }

    StatusCode DataViewModule::SetBigUint64(Handle handle,
                                            std::size_t byteOffset,
                                            std::uint64_t value,
                                            bool littleEndian) noexcept {
        return SetScalar(handle, byteOffset, value, littleEndian);
    }

    const DataViewModule::Metrics &DataViewModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool DataViewModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    DataViewModule::Handle DataViewModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::uint32_t DataViewModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & kHandleSlotMask);
    }

    std::uint32_t DataViewModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & kHandleSlotMask);
    }

    DataViewModule::ViewRecord *DataViewModule::FindMutable(Handle handle) noexcept {
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

    const DataViewModule::ViewRecord *DataViewModule::Find(Handle handle) const noexcept {
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

    std::uint32_t DataViewModule::AcquireSlot() {
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

    void DataViewModule::ReleaseSlot(std::uint32_t slotIndex) {
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

    void DataViewModule::Reset() {
        m_Slots.clear();
        m_FreeSlots.clear();
        m_CurrentFrame = 0;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void DataViewModule::Touch(ViewRecord &record) noexcept {
        record.lastTouchFrame = m_CurrentFrame;
        if (!record.hot) {
            record.hot = true;
            m_Metrics.hotViews += 1;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void DataViewModule::RecomputeHotMetrics() noexcept {
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

    bool DataViewModule::ValidateRange(const ViewRecord &record,
                                       std::size_t byteOffset,
                                       std::size_t width) const noexcept {
        m_Metrics.rangeChecks += 1;
        if (!record.attached) {
            return false;
        }
        if (byteOffset > record.byteLength) {
            return false;
        }
        if (width > record.byteLength) {
            return false;
        }
        if (byteOffset > static_cast<std::size_t>(record.byteLength) - width) {
            return false;
        }
        return true;
    }

    StatusCode DataViewModule::ResolveBuffer(ViewRecord &record,
                                             ArrayBufferModule::BufferRecord *&outRecord) noexcept {
        outRecord = nullptr;
        if (!m_ArrayBufferModule || !record.attached || record.bufferHandle == 0) {
            record.attached = false;
            return StatusCode::InvalidArgument;
        }
        auto *buffer = m_ArrayBufferModule->FindMutable(record.bufferHandle);
        if (!buffer || buffer->detached) {
            record.attached = false;
            return StatusCode::InvalidArgument;
        }
        auto required = static_cast<std::size_t>(record.byteOffset) + record.byteLength;
        if (required > buffer->byteLength) {
            record.attached = false;
            return StatusCode::InvalidArgument;
        }
        if (buffer->version != record.bufferVersion) {
            record.bufferVersion = buffer->version;
        }
        m_ArrayBufferModule->Touch(*buffer);
        outRecord = buffer;
        return StatusCode::Ok;
    }

    StatusCode DataViewModule::ResolveBuffer(const ViewRecord &record,
                                             const ArrayBufferModule::BufferRecord *&outRecord) const noexcept {
        outRecord = nullptr;
        if (!m_ArrayBufferModule || !record.attached || record.bufferHandle == 0) {
            return StatusCode::InvalidArgument;
        }
        auto *buffer = m_ArrayBufferModule->Find(record.bufferHandle);
        if (!buffer || buffer->detached) {
            return StatusCode::InvalidArgument;
        }
        auto required = static_cast<std::size_t>(record.byteOffset) + record.byteLength;
        if (required > buffer->byteLength) {
            return StatusCode::InvalidArgument;
        }
        outRecord = buffer;
        return StatusCode::Ok;
    }

    template <typename T>
    StatusCode DataViewModule::GetScalar(Handle handle,
                                         std::size_t byteOffset,
                                         bool littleEndian,
                                         T &outValue) const noexcept {
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!ValidateRange(*record, byteOffset, sizeof(T))) {
            return StatusCode::InvalidArgument;
        }
        const ArrayBufferModule::BufferRecord *buffer = nullptr;
        if (ResolveBuffer(*record, buffer) != StatusCode::Ok || !buffer->data) {
            return StatusCode::InvalidArgument;
        }
        auto base = static_cast<std::size_t>(record->byteOffset) + byteOffset;
        outValue = LoadScalar<T>(buffer->data.get() + base, littleEndian);
        m_Metrics.readOps += 1;
        return StatusCode::Ok;
    }

    template <typename T>
    StatusCode DataViewModule::SetScalar(Handle handle,
                                         std::size_t byteOffset,
                                         T value,
                                         bool littleEndian) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!ValidateRange(*record, byteOffset, sizeof(T))) {
            return StatusCode::InvalidArgument;
        }
        ArrayBufferModule::BufferRecord *buffer = nullptr;
        if (ResolveBuffer(*record, buffer) != StatusCode::Ok || !buffer->data) {
            return StatusCode::InvalidArgument;
        }
        auto base = static_cast<std::size_t>(record->byteOffset) + byteOffset;
        StoreScalar<T>(buffer->data.get() + base, value, littleEndian);
        Touch(*record);
        m_Metrics.writeOps += 1;
        return StatusCode::Ok;
    }
}
