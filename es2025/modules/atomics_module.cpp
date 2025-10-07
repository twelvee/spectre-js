#include "spectre/es2025/modules/atomics_module.h"

#include <algorithm>
#include <limits>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Atomics";
        constexpr std::string_view kSummary = "High-throughput atomic memory primitives over shared backing stores.";
        constexpr std::string_view kReference = "ECMA-262 Section 26.2";

        constexpr std::size_t kHotFrameWindow = 8;
        constexpr std::size_t kAlignmentWords = 16;
        constexpr std::size_t kMaxLinearWords = static_cast<std::size_t>(1) << 26; // 64M bytes of 64-bit lanes.

        std::memory_order NormalizeFailureOrder(std::memory_order order) noexcept {
            switch (order) {
                case std::memory_order_release:
                    return std::memory_order_relaxed;
                case std::memory_order_acq_rel:
                    return std::memory_order_acquire;
                default:
                    return order;
            }
        }
    }

    AtomicsModule::BufferMetrics::BufferMetrics() noexcept
        : allocations(0),
          deallocations(0),
          totalWords(0),
          maxWords(0),
          loadOps(0),
          storeOps(0),
          rmwOps(0),
          compareExchangeHits(0),
          compareExchangeMisses(0),
          lastFrameTouched(0),
          hotBuffers(0),
          gpuOptimized(false) {
    }

    AtomicsModule::AtomicsModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Slots{},
          m_FreeSlots{},
          m_Metrics{} {
    }

    std::string_view AtomicsModule::Name() const noexcept {
        return kName;
    }

    std::string_view AtomicsModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view AtomicsModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void AtomicsModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Metrics = BufferMetrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_Slots.clear();
        m_FreeSlots.clear();
        m_CurrentFrame = 0;
        m_Initialized = true;
    }

    void AtomicsModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        RecomputeHotMetrics();
    }

    void AtomicsModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void AtomicsModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode AtomicsModule::CreateBuffer(std::string_view label, std::size_t wordCount, Handle &outHandle) {
        outHandle = 0;
        if (wordCount == 0 || wordCount > kMaxLinearWords) {
            return StatusCode::InvalidArgument;
        }
        std::uint32_t slotIndex;
        if (!m_FreeSlots.empty()) {
            slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            auto &slot = m_Slots[slotIndex];
            slot.inUse = true;
            slot.generation += 1;
            outHandle = EncodeHandle(slotIndex, slot.generation);
            ResetRecord(slot.record, label, wordCount, outHandle, slotIndex, slot.generation);
            UpdateMetricsOnCreate(slot.record);
            Touch(slot.record);
            RecomputeHotMetrics();
            return StatusCode::Ok;
        }
        slotIndex = static_cast<std::uint32_t>(m_Slots.size());
        Slot slot{};
        slot.inUse = true;
        slot.generation = 1;
        m_Slots.push_back(slot);
        outHandle = EncodeHandle(slotIndex, slot.generation);
        ResetRecord(m_Slots[slotIndex].record, label, wordCount, outHandle, slotIndex, slot.generation);
        UpdateMetricsOnCreate(m_Slots[slotIndex].record);
        Touch(m_Slots[slotIndex].record);
        RecomputeHotMetrics();
        return StatusCode::Ok;
    }

    StatusCode AtomicsModule::DestroyBuffer(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        UpdateMetricsOnDestroy(*record);
        auto slotIndex = record->slot;
        auto &slot = m_Slots[slotIndex];
        slot.inUse = false;
        slot.generation += 1;
        slot.record = BufferRecord{};
        m_FreeSlots.push_back(slotIndex);
        RecomputeHotMetrics();
        return StatusCode::Ok;
    }

    StatusCode AtomicsModule::Fill(Handle handle, std::int64_t value, MemoryOrder order) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto memOrder = ToStdOrder(order);
        for (std::size_t i = 0; i < record->logicalLength; ++i) {
            std::atomic_ref<std::int64_t> ref(record->words[i]);
            ref.store(value, memOrder);
        }
        Touch(*record);
        m_Metrics.storeOps += record->logicalLength;
        return StatusCode::Ok;
    }

    StatusCode AtomicsModule::Load(Handle handle,
                                   std::size_t index,
                                   std::int64_t &outValue,
                                   MemoryOrder order) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (index >= record->logicalLength) {
            return StatusCode::InvalidArgument;
        }
        std::atomic_ref<std::int64_t> ref(record->words[index]);
        outValue = ref.load(ToStdOrder(order));
        Touch(*record);
        m_Metrics.loadOps += 1;
        return StatusCode::Ok;
    }

    StatusCode AtomicsModule::Store(Handle handle,
                                    std::size_t index,
                                    std::int64_t value,
                                    MemoryOrder order) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (index >= record->logicalLength) {
            return StatusCode::InvalidArgument;
        }
        std::atomic_ref<std::int64_t> ref(record->words[index]);
        ref.store(value, ToStdOrder(order));
        Touch(*record);
        m_Metrics.storeOps += 1;
        return StatusCode::Ok;
    }

    StatusCode AtomicsModule::Exchange(Handle handle,
                                       std::size_t index,
                                       std::int64_t value,
                                       std::int64_t &outPrevious,
                                       MemoryOrder order) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (index >= record->logicalLength) {
            return StatusCode::InvalidArgument;
        }
        std::atomic_ref<std::int64_t> ref(record->words[index]);
        outPrevious = ref.exchange(value, ToStdOrder(order));
        Touch(*record);
        m_Metrics.rmwOps += 1;
        return StatusCode::Ok;
    }

    StatusCode AtomicsModule::CompareExchange(Handle handle,
                                              std::size_t index,
                                              std::int64_t expected,
                                              std::int64_t desired,
                                              std::int64_t &outPrevious,
                                              MemoryOrder successOrder,
                                              MemoryOrder failureOrder) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (index >= record->logicalLength) {
            return StatusCode::InvalidArgument;
        }
        std::atomic_ref<std::int64_t> ref(record->words[index]);
        auto expectedLocal = expected;
        auto success = ref.compare_exchange_strong(expectedLocal,
                                                   desired,
                                                   ToStdOrder(successOrder),
                                                   NormalizeFailureOrder(ToStdOrder(failureOrder)));
        outPrevious = success ? expected : expectedLocal;
        Touch(*record);
        m_Metrics.rmwOps += 1;
        if (success) {
            m_Metrics.compareExchangeHits += 1;
        } else {
            m_Metrics.compareExchangeMisses += 1;
        }
        return StatusCode::Ok;
    }

    StatusCode AtomicsModule::Add(Handle handle,
                                  std::size_t index,
                                  std::int64_t value,
                                  std::int64_t &outPrevious,
                                  MemoryOrder order) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (index >= record->logicalLength) {
            return StatusCode::InvalidArgument;
        }
        std::atomic_ref<std::int64_t> ref(record->words[index]);
        outPrevious = ref.fetch_add(value, ToStdOrder(order));
        Touch(*record);
        m_Metrics.rmwOps += 1;
        return StatusCode::Ok;
    }

    StatusCode AtomicsModule::Sub(Handle handle,
                                  std::size_t index,
                                  std::int64_t value,
                                  std::int64_t &outPrevious,
                                  MemoryOrder order) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (index >= record->logicalLength) {
            return StatusCode::InvalidArgument;
        }
        std::atomic_ref<std::int64_t> ref(record->words[index]);
        outPrevious = ref.fetch_sub(value, ToStdOrder(order));
        Touch(*record);
        m_Metrics.rmwOps += 1;
        return StatusCode::Ok;
    }

    StatusCode AtomicsModule::And(Handle handle,
                                  std::size_t index,
                                  std::int64_t value,
                                  std::int64_t &outPrevious,
                                  MemoryOrder order) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (index >= record->logicalLength) {
            return StatusCode::InvalidArgument;
        }
        std::atomic_ref<std::int64_t> ref(record->words[index]);
        outPrevious = ref.fetch_and(value, ToStdOrder(order));
        Touch(*record);
        m_Metrics.rmwOps += 1;
        return StatusCode::Ok;
    }

    StatusCode AtomicsModule::Or(Handle handle,
                                 std::size_t index,
                                 std::int64_t value,
                                 std::int64_t &outPrevious,
                                 MemoryOrder order) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (index >= record->logicalLength) {
            return StatusCode::InvalidArgument;
        }
        std::atomic_ref<std::int64_t> ref(record->words[index]);
        outPrevious = ref.fetch_or(value, ToStdOrder(order));
        Touch(*record);
        m_Metrics.rmwOps += 1;
        return StatusCode::Ok;
    }

    StatusCode AtomicsModule::Xor(Handle handle,
                                  std::size_t index,
                                  std::int64_t value,
                                  std::int64_t &outPrevious,
                                  MemoryOrder order) noexcept {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (index >= record->logicalLength) {
            return StatusCode::InvalidArgument;
        }
        std::atomic_ref<std::int64_t> ref(record->words[index]);
        outPrevious = ref.fetch_xor(value, ToStdOrder(order));
        Touch(*record);
        m_Metrics.rmwOps += 1;
        return StatusCode::Ok;
    }

    StatusCode AtomicsModule::Snapshot(Handle handle, std::vector<std::int64_t> &outValues) const {
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        outValues.assign(record->words.begin(), record->words.begin() + static_cast<std::ptrdiff_t>(record->logicalLength));
        return StatusCode::Ok;
    }

    bool AtomicsModule::Has(Handle handle) const noexcept {
        return Find(handle) != nullptr;
    }

    std::size_t AtomicsModule::Capacity(Handle handle) const noexcept {
        const auto *record = Find(handle);
        return record ? record->logicalLength : 0;
    }

    const AtomicsModule::BufferMetrics &AtomicsModule::Metrics() const noexcept {
        return m_Metrics;
    }

    bool AtomicsModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    AtomicsModule::BufferRecord *AtomicsModule::FindMutable(Handle handle) noexcept {
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

    const AtomicsModule::BufferRecord *AtomicsModule::Find(Handle handle) const noexcept {
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

    std::uint32_t AtomicsModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t AtomicsModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }

    AtomicsModule::Handle AtomicsModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::memory_order AtomicsModule::ToStdOrder(MemoryOrder order) noexcept {
        switch (order) {
            case MemoryOrder::Relaxed:
                return std::memory_order_relaxed;
            case MemoryOrder::Acquire:
                return std::memory_order_acquire;
            case MemoryOrder::Release:
                return std::memory_order_release;
            case MemoryOrder::AcquireRelease:
                return std::memory_order_acq_rel;
            case MemoryOrder::SequentiallyConsistent:
            default:
                return std::memory_order_seq_cst;
        }
    }

    void AtomicsModule::ResetRecord(BufferRecord &record,
                                    std::string_view label,
                                    std::size_t wordCount,
                                    Handle handle,
                                    std::uint32_t slot,
                                    std::uint32_t generation) {
        record.handle = handle;
        record.slot = slot;
        record.generation = generation;
        record.label.assign(label);
        record.logicalLength = wordCount;
        record.words.assign(AlignWordCount(wordCount), 0);
        record.version = 0;
        record.lastTouchFrame = m_CurrentFrame;
        record.hot = true;
    }

    void AtomicsModule::Touch(BufferRecord &record) noexcept {
        record.version += 1;
        record.lastTouchFrame = m_CurrentFrame;
        record.hot = true;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void AtomicsModule::UpdateMetricsOnCreate(const BufferRecord &record) {
        m_Metrics.allocations += 1;
        m_Metrics.totalWords += record.logicalLength;
        m_Metrics.maxWords = std::max(m_Metrics.maxWords, static_cast<std::uint64_t>(record.logicalLength));
    }

    void AtomicsModule::UpdateMetricsOnDestroy(const BufferRecord &record) {
        m_Metrics.deallocations += 1;
        if (m_Metrics.totalWords >= record.logicalLength) {
            m_Metrics.totalWords -= record.logicalLength;
        } else {
            m_Metrics.totalWords = 0;
        }
    }

    void AtomicsModule::RecomputeHotMetrics() noexcept {
        std::uint64_t hot = 0;
        for (auto &slot : m_Slots) {
            if (!slot.inUse) {
                continue;
            }
            auto &record = slot.record;
            record.hot = (m_CurrentFrame - record.lastTouchFrame) <= kHotFrameWindow;
            if (record.hot) {
                ++hot;
            }
        }
        m_Metrics.hotBuffers = hot;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    std::size_t AtomicsModule::AlignWordCount(std::size_t count) noexcept {
        if (count == 0) {
            return 0;
        }
        auto blocks = (count + (kAlignmentWords - 1)) / kAlignmentWords;
        return blocks * kAlignmentWords;
    }
}
