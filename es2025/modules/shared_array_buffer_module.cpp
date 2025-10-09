#include "spectre/es2025/modules/shared_array_buffer_module.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "SharedArrayBuffer";
        constexpr std::string_view kSummary =
                "SharedArrayBuffer lifetime, sharing, slicing, and resizing semantics.";
        constexpr std::string_view kReference = "ECMA-262 Section 25.2";
        constexpr std::uint64_t kHandleSlotMask = 0xffffffffull;
        constexpr std::uint64_t kHotFrameWindow = 12;
    }

    SharedArrayBufferModule::Metrics::Metrics() noexcept
        : allocations(0),
          releases(0),
          shares(0),
          slices(0),
          grows(0),
          bytesAllocated(0),
          bytesInUse(0),
          bytesPooled(0),
          lastFrameTouched(0),
          hotBuffers(0),
          gpuOptimized(false) {
    }

    SharedArrayBufferModule::Storage::Storage() noexcept
        : data(),
          byteLength(0),
          maxByteLength(0),
          refCount(0),
          version(0),
          lastTouchFrame(0),
          resizable(false),
          hot(false) {
    }

    SharedArrayBufferModule::BufferRecord::BufferRecord() noexcept
        : handle(0),
          slot(0),
          generation(0),
          storageIndex(0),
          storageGeneration(0),
          label(),
          lastTouchFrame(0),
          hot(false) {
    }

    SharedArrayBufferModule::Slot::Slot() noexcept : record(), generation(0), inUse(false) {
    }

    SharedArrayBufferModule::StorageSlot::StorageSlot() noexcept : storage(), generation(0), inUse(false) {
    }

    SharedArrayBufferModule::SharedArrayBufferModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Slots(),
          m_FreeSlots(),
          m_Storages(),
          m_FreeStorages(),
          m_Metrics() {
    }

    std::string_view SharedArrayBufferModule::Name() const noexcept {
        return kName;
    }

    std::string_view SharedArrayBufferModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view SharedArrayBufferModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void SharedArrayBufferModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_CurrentFrame = 0;
        m_Slots.clear();
        m_FreeSlots.clear();
        m_Storages.clear();
        m_FreeStorages.clear();
        m_Metrics = Metrics();
    }

    void SharedArrayBufferModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        RecomputeHotMetrics();
    }

    void SharedArrayBufferModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void SharedArrayBufferModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode SharedArrayBufferModule::Create(std::string_view label,
                                               std::size_t byteLength,
                                               Handle &outHandle) {
        outHandle = 0;
        return CreateResizable(label, byteLength, byteLength, outHandle);
    }

    StatusCode SharedArrayBufferModule::CreateResizable(std::string_view label,
                                                        std::size_t initialByteLength,
                                                        std::size_t maxByteLength,
                                                        Handle &outHandle) {
        outHandle = 0;
        if (initialByteLength > maxByteLength) {
            return StatusCode::InvalidArgument;
        }
        auto storageIndex = AcquireStorageSlot();
        auto &storageSlot = m_Storages[storageIndex];
        storageSlot.inUse = true;
        storageSlot.storage = Storage();
        storageSlot.storage.data.resize(maxByteLength);
        storageSlot.storage.byteLength = initialByteLength;
        storageSlot.storage.maxByteLength = maxByteLength;
        storageSlot.storage.refCount = 1;
        storageSlot.storage.resizable = (initialByteLength != maxByteLength);
        storageSlot.storage.version = 1;
        storageSlot.storage.lastTouchFrame = m_CurrentFrame;
        storageSlot.storage.hot = true;
        m_Metrics.allocations += 1;
        m_Metrics.bytesAllocated += maxByteLength;
        m_Metrics.bytesInUse += initialByteLength;

        auto slotIndex = AcquireSlot();
        auto &slot = m_Slots[slotIndex];
        slot.inUse = true;
        slot.record = BufferRecord();
        slot.record.slot = slotIndex;
        slot.record.generation = slot.generation;
        slot.record.storageIndex = storageIndex;
        slot.record.storageGeneration = storageSlot.generation;
        slot.record.label = label.empty()
                                ? std::string("shared.buffer.").append(std::to_string(slotIndex))
                                : std::string(label);
        slot.record.lastTouchFrame = m_CurrentFrame;
        slot.record.hot = true;
        slot.record.handle = EncodeHandle(slotIndex, slot.generation);
        m_Metrics.hotBuffers += 1;
        m_Metrics.lastFrameTouched = m_CurrentFrame;

        outHandle = slot.record.handle;
        return StatusCode::Ok;
    }

    StatusCode SharedArrayBufferModule::Share(Handle handle,
                                              std::string_view label,
                                              Handle &outHandle) {
        outHandle = 0;
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto *storage = ResolveStorage(*record);
        if (!storage) {
            return StatusCode::InvalidArgument;
        }
        storage->refCount += 1;
        auto slotIndex = AcquireSlot();
        auto &slot = m_Slots[slotIndex];
        slot.inUse = true;
        slot.record = BufferRecord();
        slot.record.slot = slotIndex;
        slot.record.generation = slot.generation;
        slot.record.storageIndex = record->storageIndex;
        slot.record.storageGeneration = record->storageGeneration;
        slot.record.label = label.empty()
                                ? std::string("shared.share.").append(std::to_string(slotIndex))
                                : std::string(label);
        slot.record.lastTouchFrame = m_CurrentFrame;
        slot.record.hot = true;
        storage->hot = true;
        storage->lastTouchFrame = m_CurrentFrame;
        slot.record.handle = EncodeHandle(slotIndex, slot.generation);
        outHandle = slot.record.handle;
        m_Metrics.shares += 1;
        return StatusCode::Ok;
    }

    StatusCode SharedArrayBufferModule::Slice(Handle handle,
                                              std::size_t begin,
                                              std::size_t end,
                                              std::string_view label,
                                              Handle &outHandle) {
        outHandle = 0;
        auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto *storage = ResolveStorage(*record);
        if (!storage || begin > end || end > storage->byteLength) {
            return StatusCode::InvalidArgument;
        }
        auto length = end - begin;
        Handle sliceHandle = 0;
        auto status = Create(label, length, sliceHandle);
        if (status != StatusCode::Ok) {
            return status;
        }
        auto *sliceRecord = FindMutable(sliceHandle);
        auto *sliceStorage = sliceRecord ? ResolveStorage(*sliceRecord) : nullptr;
        if (!sliceRecord || !sliceStorage) {
            Destroy(sliceHandle);
            return StatusCode::InternalError;
        }
        if (length > 0) {
            std::memcpy(sliceStorage->data.data(), storage->data.data() + begin, length);
        }
        sliceStorage->byteLength = length;
        outHandle = sliceHandle;
        m_Metrics.slices += 1;
        return StatusCode::Ok;
    }

    StatusCode SharedArrayBufferModule::Grow(Handle handle, std::size_t newByteLength, bool /*preserveData*/) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto *storage = ResolveStorage(*record);
        if (!storage || !storage->resizable) {
            return StatusCode::InvalidArgument;
        }
        if (newByteLength > storage->maxByteLength) {
            return StatusCode::CapacityExceeded;
        }
        storage->byteLength = newByteLength;
        storage->lastTouchFrame = m_CurrentFrame;
        storage->hot = true;
        record->lastTouchFrame = m_CurrentFrame;
        record->hot = true;
        m_Metrics.grows += 1;
        return StatusCode::Ok;
    }

    StatusCode SharedArrayBufferModule::Destroy(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto storageIndex = record->storageIndex;
        auto *storageSlot = (storageIndex < m_Storages.size()) ? &m_Storages[storageIndex] : nullptr;
        if (storageSlot && storageSlot->inUse && storageSlot->generation == record->storageGeneration) {
            if (storageSlot->storage.refCount > 0) {
                storageSlot->storage.refCount -= 1;
            }
            if (storageSlot->storage.refCount == 0) {
                m_Metrics.releases += 1;
                if (storageSlot->storage.byteLength <= m_Metrics.bytesInUse) {
                    m_Metrics.bytesInUse -= storageSlot->storage.byteLength;
                }
                storageSlot->storage.data.clear();
                storageSlot->storage.byteLength = 0;
                storageSlot->storage.hot = false;
                ReleaseStorageSlot(storageIndex);
            }
        }
        ReleaseSlot(record->slot);
        return StatusCode::Ok;
    }

    StatusCode SharedArrayBufferModule::CopyIn(Handle handle,
                                               std::size_t offset,
                                               const std::uint8_t *data,
                                               std::size_t size) {
        auto *record = FindMutable(handle);
        if (!record || !data) {
            return StatusCode::InvalidArgument;
        }
        auto *storage = ResolveStorage(*record);
        if (!storage || offset > storage->byteLength || (offset + size) > storage->byteLength) {
            return StatusCode::InvalidArgument;
        }
        if (size > 0) {
            std::memcpy(storage->data.data() + offset, data, size);
        }
        Touch(*record, *storage);
        return StatusCode::Ok;
    }

    StatusCode SharedArrayBufferModule::CopyOut(Handle handle,
                                                std::size_t offset,
                                                std::uint8_t *data,
                                                std::size_t size) const {
        auto *record = Find(handle);
        if (!record || !data) {
            return StatusCode::InvalidArgument;
        }
        auto *storage = ResolveStorage(*record);
        if (!storage || offset > storage->byteLength || (offset + size) > storage->byteLength) {
            return StatusCode::InvalidArgument;
        }
        if (size > 0) {
            std::memcpy(data, storage->data.data() + offset, size);
        }
        return StatusCode::Ok;
    }

    StatusCode SharedArrayBufferModule::Snapshot(Handle handle, std::vector<std::uint8_t> &outBytes) const {
        auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto *storage = ResolveStorage(*record);
        if (!storage) {
            return StatusCode::InvalidArgument;
        }
        outBytes.assign(storage->data.begin(), storage->data.begin() + storage->byteLength);
        return StatusCode::Ok;
    }

    bool SharedArrayBufferModule::Has(Handle handle) const noexcept {
        return Find(handle) != nullptr;
    }

    std::size_t SharedArrayBufferModule::ByteLength(Handle handle) const noexcept {
        auto *record = Find(handle);
        auto *storage = record ? ResolveStorage(*record) : nullptr;
        return storage ? storage->byteLength : 0;
    }

    std::size_t SharedArrayBufferModule::MaxByteLength(Handle handle) const noexcept {
        auto *record = Find(handle);
        auto *storage = record ? ResolveStorage(*record) : nullptr;
        return storage ? storage->maxByteLength : 0;
    }

    bool SharedArrayBufferModule::Resizable(Handle handle) const noexcept {
        auto *record = Find(handle);
        auto *storage = record ? ResolveStorage(*record) : nullptr;
        return storage ? storage->resizable : false;
    }

    std::uint32_t SharedArrayBufferModule::RefCount(Handle handle) const noexcept {
        auto *record = Find(handle);
        auto *storage = record ? ResolveStorage(*record) : nullptr;
        return storage ? storage->refCount : 0;
    }

    const SharedArrayBufferModule::Metrics &SharedArrayBufferModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool SharedArrayBufferModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    SharedArrayBufferModule::BufferRecord *SharedArrayBufferModule::FindMutable(Handle handle) noexcept {
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

    const SharedArrayBufferModule::BufferRecord *SharedArrayBufferModule::Find(Handle handle) const noexcept {
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

    SharedArrayBufferModule::Storage *SharedArrayBufferModule::ResolveStorage(BufferRecord &record) noexcept {
        if (record.storageIndex >= m_Storages.size()) {
            return nullptr;
        }
        auto &slot = m_Storages[record.storageIndex];
        if (!slot.inUse || slot.generation != record.storageGeneration) {
            return nullptr;
        }
        return &slot.storage;
    }

    const SharedArrayBufferModule::Storage *SharedArrayBufferModule::ResolveStorage(
        const BufferRecord &record) const noexcept {
        if (record.storageIndex >= m_Storages.size()) {
            return nullptr;
        }
        const auto &slot = m_Storages[record.storageIndex];
        if (!slot.inUse || slot.generation != record.storageGeneration) {
            return nullptr;
        }
        return &slot.storage;
    }

    std::uint32_t SharedArrayBufferModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & kHandleSlotMask);
    }

    std::uint32_t SharedArrayBufferModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32u) & kHandleSlotMask);
    }

    SharedArrayBufferModule::Handle SharedArrayBufferModule::EncodeHandle(
        std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<std::uint64_t>(generation) << 32u) | slot;
    }

    void SharedArrayBufferModule::Touch(BufferRecord &record, Storage &storage) noexcept {
        record.lastTouchFrame = m_CurrentFrame;
        record.hot = true;
        storage.lastTouchFrame = m_CurrentFrame;
        if (!storage.hot) {
            storage.hot = true;
            m_Metrics.hotBuffers += 1;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void SharedArrayBufferModule::RecomputeHotMetrics() noexcept {
        std::uint64_t hotCount = 0;
        for (auto &storageSlot: m_Storages) {
            if (!storageSlot.inUse) {
                continue;
            }
            auto &storage = storageSlot.storage;
            if (m_CurrentFrame >= storage.lastTouchFrame &&
                (m_CurrentFrame - storage.lastTouchFrame) <= kHotFrameWindow) {
                storage.hot = true;
                hotCount += 1;
            } else {
                storage.hot = false;
            }
        }
        m_Metrics.hotBuffers = hotCount;
    }

    std::uint32_t SharedArrayBufferModule::AcquireSlot() {
        if (!m_FreeSlots.empty()) {
            auto index = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            auto &slot = m_Slots[index];
            slot.inUse = true;
            slot.generation += 1;
            return index;
        }
        auto index = static_cast<std::uint32_t>(m_Slots.size());
        m_Slots.emplace_back();
        auto &slot = m_Slots.back();
        slot.inUse = true;
        slot.generation = 1;
        return index;
    }

    void SharedArrayBufferModule::ReleaseSlot(std::uint32_t slotIndex) {
        if (slotIndex >= m_Slots.size()) {
            return;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse) {
            return;
        }
        slot.inUse = false;
        slot.generation += 1;
        slot.record = BufferRecord();
        m_FreeSlots.push_back(slotIndex);
    }

    std::uint32_t SharedArrayBufferModule::AcquireStorageSlot() {
        if (!m_FreeStorages.empty()) {
            auto index = m_FreeStorages.back();
            m_FreeStorages.pop_back();
            auto &slot = m_Storages[index];
            slot.inUse = true;
            slot.generation += 1;
            return index;
        }
        auto index = static_cast<std::uint32_t>(m_Storages.size());
        m_Storages.emplace_back();
        auto &slot = m_Storages.back();
        slot.inUse = true;
        slot.generation = 1;
        return index;
    }

    void SharedArrayBufferModule::ReleaseStorageSlot(std::uint32_t storageIndex) {
        if (storageIndex >= m_Storages.size()) {
            return;
        }
        auto &slot = m_Storages[storageIndex];
        if (!slot.inUse) {
            return;
        }
        slot.inUse = false;
        slot.generation += 1;
        slot.storage = Storage();
        m_FreeStorages.push_back(storageIndex);
    }
}
