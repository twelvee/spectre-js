#include "spectre/es2025/modules/array_buffer_module.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "spectre/runtime.h"
namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "ArrayBuffer";
        constexpr std::string_view kSummary = "ArrayBuffer allocation, detachment, slicing, and pooling semantics.";
        constexpr std::string_view kReference = "ECMA-262 Section 25.1";

        constexpr std::uint64_t kHandleSlotMask = 0xffffffffull;
    }

    ArrayBufferModule::Metrics::Metrics() noexcept
        : allocations(0),
          deallocations(0),
          resizes(0),
          detaches(0),
          copiesIn(0),
          copiesOut(0),
          copyBetweenBuffers(0),
          fills(0),
          poolReuses(0),
          poolReturns(0),
          bytesAllocated(0),
          bytesRecycled(0),
          bytesInUse(0),
          peakBytesInUse(0),
          pooledBytes(0),
          activeBuffers(0),
          lastFrameTouched(0),
          hotBuffers(0),
          gpuOptimized(false) {
    }

    ArrayBufferModule::Block::Block() noexcept : data(), capacity(0) {
    }

    ArrayBufferModule::Block::Block(std::unique_ptr<std::uint8_t[]> ptr, std::size_t cap) noexcept
        : data(std::move(ptr)), capacity(cap) {
    }

    ArrayBufferModule::Block::Block(Block &&other) noexcept
        : data(std::move(other.data)), capacity(other.capacity) {
        other.capacity = 0;
    }

    ArrayBufferModule::Block &ArrayBufferModule::Block::operator=(Block &&other) noexcept {
        if (this != &other) {
            data = std::move(other.data);
            capacity = other.capacity;
            other.capacity = 0;
        }
        return *this;
    }

    ArrayBufferModule::BufferRecord::BufferRecord() noexcept
        : handle(0),
          slot(0),
          generation(0),
          label(),
          data(),
          byteLength(0),
          capacity(0),
          version(0),
          lastTouchFrame(0),
          detachable(true),
          detached(false),
          hot(false) {
    }

    ArrayBufferModule::Slot::Slot() noexcept : record(), generation(0), inUse(false) {
    }

    ArrayBufferModule::ArrayBufferModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Slots(),
          m_FreeSlots(),
          m_Pool(),
          m_TotalPooledBytes(0),
          m_Metrics() {
    }

    std::string_view ArrayBufferModule::Name() const noexcept {
        return kName;
    }

    std::string_view ArrayBufferModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view ArrayBufferModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void ArrayBufferModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        Reset();
        m_Initialized = true;
    }

    void ArrayBufferModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        RecomputeHotMetrics();
    }

    void ArrayBufferModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void ArrayBufferModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode ArrayBufferModule::Create(std::string_view label, std::size_t byteLength, Handle &outHandle) {
        outHandle = 0;
        if (byteLength > m_Config.memory.heapBytes) {
            return StatusCode::CapacityExceeded;
        }
        if (m_Metrics.bytesInUse + byteLength > m_Config.memory.heapBytes) {
            return StatusCode::CapacityExceeded;
        }
        auto alignedCapacity = AlignCapacity(byteLength);
        Block block = AcquireBlock(alignedCapacity);
        if (byteLength > 0 && alignedCapacity > 0 && !block.data) {
            return StatusCode::InternalError;
        }
        if (byteLength > 0 && block.data) {
            std::memset(block.data.get(), 0, byteLength);
        }

        auto slotIndex = AcquireSlot();
        auto &slot = m_Slots[slotIndex];
        auto generation = slot.generation;
        auto handle = EncodeHandle(slotIndex, generation);

        slot.record = BufferRecord();
        slot.record.handle = handle;
        slot.record.slot = slotIndex;
        slot.record.generation = generation;
        if (label.empty()) {
            slot.record.label = "arraybuffer." + std::to_string(slotIndex);
        } else {
            slot.record.label.assign(label);
        }
        slot.record.byteLength = byteLength;
        slot.record.capacity = block.capacity;
        slot.record.detached = false;
        slot.record.data = std::move(block.data);
        slot.record.lastTouchFrame = m_CurrentFrame;
        slot.record.hot = true;

        slot.record.version = 1;

        slot.record.handle = handle;

        slot.record.detachable = true;

        outHandle = handle;

        slot.inUse = true;

        m_Metrics.allocations += 1;
        m_Metrics.activeBuffers += 1;
        m_Metrics.bytesInUse += byteLength;
        m_Metrics.peakBytesInUse = std::max(m_Metrics.peakBytesInUse, m_Metrics.bytesInUse);
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        if (slot.record.hot) {
            m_Metrics.hotBuffers += 1;
        }

        return StatusCode::Ok;
    }

        StatusCode ArrayBufferModule::Clone(Handle handle, std::string_view label, Handle &outHandle) {
        outHandle = 0;
        const BufferRecord *source = Find(handle);
        if (!source) {
            return StatusCode::NotFound;
        }
        if (source->detached) {
            return StatusCode::InvalidArgument;
        }
        auto cloneLabel = label.empty() ? std::string(source->label).append(".clone") : std::string(label);
        auto status = Create(cloneLabel, source->byteLength, outHandle);
        if (status != StatusCode::Ok) {
            return status;
        }
        source = Find(handle);
        if (!source) {
            return StatusCode::InternalError;
        }
        auto *target = FindMutable(outHandle);
        if (!target) {
            return StatusCode::InternalError;
        }
        if (source->byteLength > 0 && target->data && source->data) {
            std::memcpy(target->data.get(), source->data.get(), source->byteLength);
        }
        target->detachable = source->detachable;
        Touch(*target);
        return StatusCode::Ok;
    }

    StatusCode ArrayBufferModule::Resize(Handle handle, std::size_t newByteLength, bool preserveData) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (record->detached) {
            return StatusCode::InvalidArgument;
        }
        if (newByteLength == record->byteLength) {
            return StatusCode::Ok;
        }
        if (newByteLength > m_Config.memory.heapBytes) {
            return StatusCode::CapacityExceeded;
        }
        auto projectedBytes = m_Metrics.bytesInUse - std::min<std::uint64_t>(m_Metrics.bytesInUse, record->byteLength);
        projectedBytes += newByteLength;
        if (projectedBytes > m_Config.memory.heapBytes) {
            return StatusCode::CapacityExceeded;
        }
        auto oldLength = record->byteLength;
        auto oldCapacity = record->capacity;
        auto desiredCapacity = AlignCapacity(newByteLength);
        if (desiredCapacity == 0) {
            Block oldBlock(std::move(record->data), record->capacity);
            record->capacity = 0;
            ReturnBlock(std::move(oldBlock));
        } else if (desiredCapacity > oldCapacity || (desiredCapacity < oldCapacity / 2 && desiredCapacity > 0)) {
            Block newBlock = AcquireBlock(desiredCapacity);
            if (newByteLength > 0 && !newBlock.data) {
                return StatusCode::InternalError;
            }
            if (newByteLength > 0) {
                if (preserveData && record->data) {
                    auto copyBytes = std::min(oldLength, newByteLength);
                    if (copyBytes > 0) {
                        std::memcpy(newBlock.data.get(), record->data.get(), copyBytes);
                    }
                    if (newByteLength > copyBytes) {
                        std::memset(newBlock.data.get() + copyBytes, 0, newByteLength - copyBytes);
                    }
                } else {
                    std::memset(newBlock.data.get(), 0, newByteLength);
                }
            }
            Block oldBlock(std::move(record->data), record->capacity);
            record->data = std::move(newBlock.data);
            record->capacity = newBlock.capacity;
            ReturnBlock(std::move(oldBlock));
        } else {
            if (record->data) {
                if (!preserveData && newByteLength > 0) {
                    std::memset(record->data.get(), 0, newByteLength);
                } else if (preserveData && newByteLength > oldLength) {
                    std::memset(record->data.get() + oldLength, 0, newByteLength - oldLength);
                }
            }
            if (newByteLength == 0) {
                Block oldBlock(std::move(record->data), record->capacity);
                record->capacity = 0;
                ReturnBlock(std::move(oldBlock));
            }
        }
        record->byteLength = newByteLength;
        m_Metrics.bytesInUse = projectedBytes;
        m_Metrics.peakBytesInUse = std::max(m_Metrics.peakBytesInUse, m_Metrics.bytesInUse);
        m_Metrics.resizes += 1;
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayBufferModule::Slice(Handle handle,
                                        std::size_t begin,
                                        std::size_t end,
                                        std::string_view label,
                                        Handle &outHandle) {
        outHandle = 0;
        const BufferRecord *source = Find(handle);
        if (!source) {
            return StatusCode::NotFound;
        }
        if (source->detached) {
            return StatusCode::InvalidArgument;
        }
        if (begin > end) {
            return StatusCode::InvalidArgument;
        }
        if (begin > source->byteLength) {
            return StatusCode::InvalidArgument;
        }
        auto clampedEnd = std::min(end, source->byteLength);
        auto sliceLength = clampedEnd > begin ? clampedEnd - begin : 0;
        auto sliceLabel = label.empty() ? std::string(source->label).append(".slice") : std::string(label);
        auto status = Create(sliceLabel, sliceLength, outHandle);
        if (status != StatusCode::Ok) {
            return status;
        }
        source = Find(handle);
        if (!source) {
            return StatusCode::InternalError;
        }
        auto *target = FindMutable(outHandle);
        if (!target) {
            return StatusCode::InternalError;
        }
        if (sliceLength > 0 && target->data && source->data) {
            std::memcpy(target->data.get(), source->data.get() + begin, sliceLength);
        }
        Touch(*target);
        return StatusCode::Ok;
    }


    StatusCode ArrayBufferModule::Detach(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (record->detached) {
            return StatusCode::Ok;
        }
        auto releasedBytes = record->byteLength;
        Block oldBlock(std::move(record->data), record->capacity);
        record->capacity = 0;
        record->byteLength = 0;
        record->detached = true;
        ReturnBlock(std::move(oldBlock));
        m_Metrics.bytesInUse = (releasedBytes > m_Metrics.bytesInUse) ? 0 : (m_Metrics.bytesInUse - releasedBytes);
        m_Metrics.detaches += 1;
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayBufferModule::Destroy(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto slotIndex = record->slot;
        auto reclaimedBytes = record->byteLength;
        Block oldBlock(std::move(record->data), record->capacity);
        record->capacity = 0;
        record->byteLength = 0;
        ReturnBlock(std::move(oldBlock));
        m_Metrics.bytesInUse = (reclaimedBytes > m_Metrics.bytesInUse) ? 0 : (m_Metrics.bytesInUse - reclaimedBytes);
        if (m_Metrics.activeBuffers > 0) {
            m_Metrics.activeBuffers -= 1;
        }
        m_Metrics.deallocations += 1;
        ReleaseSlot(slotIndex);
        RecomputeHotMetrics();
        return StatusCode::Ok;
    }

    StatusCode ArrayBufferModule::Fill(Handle handle, std::uint8_t value) noexcept {
        auto *record = FindMutable(handle);
        if (!record || record->detached) {
            return record ? StatusCode::InvalidArgument : StatusCode::NotFound;
        }
        if (!record->data || record->byteLength == 0) {
            return StatusCode::Ok;
        }
        std::memset(record->data.get(), value, record->byteLength);
        m_Metrics.fills += 1;
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayBufferModule::CopyIn(Handle handle,
                                         std::size_t offset,
                                         const std::uint8_t *data,
                                         std::size_t size) noexcept {
        auto *record = FindMutable(handle);
        if (!record || record->detached) {
            return record ? StatusCode::InvalidArgument : StatusCode::NotFound;
        }
        if ((size > 0 && !data) || offset > record->byteLength) {
            return StatusCode::InvalidArgument;
        }
        if (offset + size > record->byteLength) {
            return StatusCode::InvalidArgument;
        }
        if (size == 0) {
            return StatusCode::Ok;
        }
        std::memcpy(record->data.get() + offset, data, size);
        m_Metrics.copiesIn += 1;
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayBufferModule::CopyOut(Handle handle,
                                          std::size_t offset,
                                          std::uint8_t *data,
                                          std::size_t size) noexcept {
        const auto *record = Find(handle);
        if (!record || record->detached) {
            return record ? StatusCode::InvalidArgument : StatusCode::NotFound;
        }
        if ((size > 0 && !data) || offset > record->byteLength) {
            return StatusCode::InvalidArgument;
        }
        if (offset + size > record->byteLength) {
            return StatusCode::InvalidArgument;
        }
        if (size == 0) {
            return StatusCode::Ok;
        }
        std::memcpy(data, record->data.get() + offset, size);
        m_Metrics.copiesOut += 1;
        return StatusCode::Ok;
    }

    StatusCode ArrayBufferModule::CopyToBuffer(Handle source,
                                               Handle target,
                                               std::size_t sourceOffset,
                                               std::size_t targetOffset,
                                               std::size_t size) noexcept {
        if (size == 0) {
            return StatusCode::Ok;
        }
        auto *sourceRecord = FindMutable(source);
        auto *targetRecord = FindMutable(target);
        if (!sourceRecord || !targetRecord) {
            return StatusCode::NotFound;
        }
        if (sourceRecord->detached || targetRecord->detached) {
            return StatusCode::InvalidArgument;
        }
        if (sourceOffset + size > sourceRecord->byteLength ||
            targetOffset + size > targetRecord->byteLength) {
            return StatusCode::InvalidArgument;
        }
        std::memmove(targetRecord->data.get() + targetOffset,
                     sourceRecord->data.get() + sourceOffset,
                     size);
        m_Metrics.copyBetweenBuffers += 1;
        Touch(*targetRecord);
        return StatusCode::Ok;
    }

    bool ArrayBufferModule::Has(Handle handle) const noexcept {
        return Find(handle) != nullptr;
    }

    bool ArrayBufferModule::Detached(Handle handle) const noexcept {
        const auto *record = Find(handle);
        return record ? record->detached : false;
    }

    std::size_t ArrayBufferModule::ByteLength(Handle handle) const noexcept {
        const auto *record = Find(handle);
        if (!record || record->detached) {
            return 0;
        }
        return record->byteLength;
    }

    const ArrayBufferModule::Metrics &ArrayBufferModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool ArrayBufferModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    ArrayBufferModule::BufferRecord *ArrayBufferModule::FindMutable(Handle handle) noexcept {
        if (handle == 0) {
            return nullptr;
        }
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse) {
            return nullptr;
        }
        if (slot.generation != DecodeGeneration(handle)) {
            return nullptr;
        }
        return &slot.record;
    }

    const ArrayBufferModule::BufferRecord *ArrayBufferModule::Find(Handle handle) const noexcept {
        if (handle == 0) {
            return nullptr;
        }
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        const auto &slot = m_Slots[slotIndex];
        if (!slot.inUse) {
            return nullptr;
        }
        if (slot.generation != DecodeGeneration(handle)) {
            return nullptr;
        }
        return &slot.record;
    }

    std::uint32_t ArrayBufferModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & kHandleSlotMask);
    }

    std::uint32_t ArrayBufferModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & kHandleSlotMask);
    }

    ArrayBufferModule::Handle ArrayBufferModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::size_t ArrayBufferModule::AlignCapacity(std::size_t size) noexcept {
        if (size == 0) {
            return 0;
        }
        auto aligned = (size + (kAlignment - 1)) & ~(kAlignment - 1);
        std::size_t capacity = kMinCapacity;
        while (capacity < aligned && capacity < (std::numeric_limits<std::size_t>::max() / 2)) {
            capacity <<= 1;
        }
        if (capacity < aligned) {
            capacity = aligned;
        }
        return capacity;
    }

    std::size_t ArrayBufferModule::BucketIndex(std::size_t capacity) noexcept {
        if (capacity <= kMinCapacity) {
            return 0;
        }
        capacity /= kMinCapacity;
        std::size_t index = 0;
        while (capacity > 1 && index + 1 < kPoolBuckets) {
            capacity >>= 1;
            index += 1;
        }
        return index;
    }

    void ArrayBufferModule::Reset() {
        m_Slots.clear();
        m_FreeSlots.clear();
        for (auto &bucket: m_Pool) {
            bucket.clear();
        }
        m_TotalPooledBytes = 0;
        m_CurrentFrame = 0;
        m_Metrics.bytesInUse = 0;
        m_Metrics.peakBytesInUse = 0;
        m_Metrics.pooledBytes = 0;
        m_Metrics.activeBuffers = 0;
        m_Metrics.hotBuffers = 0;
        m_Metrics.lastFrameTouched = 0;
    }

    std::uint32_t ArrayBufferModule::AcquireSlot() {
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
        auto &slot = m_Slots[slotIndex];
        slot.inUse = true;
        slot.generation = 1;
        return slotIndex;
    }

    void ArrayBufferModule::ReleaseSlot(std::uint32_t slotIndex) {
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

    ArrayBufferModule::Block ArrayBufferModule::AcquireBlock(std::size_t capacity) {
        if (capacity == 0) {
            return Block();
        }
        auto bucket = BucketIndex(capacity);
        auto &list = m_Pool[bucket];
        if (!list.empty()) {
            Block block = std::move(list.back());
            list.pop_back();
            auto cap = block.capacity;
            if (m_TotalPooledBytes >= cap) {
                m_TotalPooledBytes -= cap;
            } else {
                m_TotalPooledBytes = 0;
            }
            m_Metrics.poolReuses += 1;
            m_Metrics.pooledBytes = m_TotalPooledBytes;
            return block;
        }
        auto ptr = std::make_unique<std::uint8_t[]>(capacity);
        m_Metrics.bytesAllocated += capacity;
        return Block(std::move(ptr), capacity);
    }

    void ArrayBufferModule::ReturnBlock(Block &&block) noexcept {
        if (!block.data || block.capacity == 0) {
            return;
        }
        auto bucket = BucketIndex(block.capacity);
        auto cap = block.capacity;
        m_TotalPooledBytes += cap;
        m_Metrics.poolReturns += 1;
        m_Metrics.bytesRecycled += cap;
        m_Metrics.pooledBytes = m_TotalPooledBytes;
        m_Pool[bucket].push_back(std::move(block));
    }

    void ArrayBufferModule::ReturnBlock(BufferRecord &record) noexcept {
        if (!record.data || record.capacity == 0) {
            record.data.reset();
            record.capacity = 0;
            return;
        }
        Block block(std::move(record.data), record.capacity);
        record.capacity = 0;
        ReturnBlock(std::move(block));
    }

    void ArrayBufferModule::Touch(BufferRecord &record) noexcept {
        record.version += 1;
        record.lastTouchFrame = m_CurrentFrame;
        if (!record.hot) {
            record.hot = true;
            m_Metrics.hotBuffers += 1;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void ArrayBufferModule::RecomputeHotMetrics() noexcept {
        std::uint64_t hotCount = 0;
        for (auto &slot: m_Slots) {
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
        m_Metrics.hotBuffers = hotCount;
    }
}














