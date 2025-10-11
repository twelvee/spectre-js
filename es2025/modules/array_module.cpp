#include "spectre/es2025/modules/array_module.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <numeric>
#include <sstream>
#include <utility>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Array";
        constexpr std::string_view kSummary = "Array constructor, dense and sparse storage strategies, and algorithms.";
        constexpr std::string_view kReference = "ECMA-262 Section 23.1";
        constexpr std::size_t kDefaultDenseCapacity = 16;
        constexpr double kSparsePromotionRatio = 0.75;
        constexpr double kDenseCompactionRatio = 0.30;
    }

    ArrayModule::ArrayModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Slots(),
          m_FreeSlots(),
          m_Metrics() {
    }

    std::string_view ArrayModule::Name() const noexcept {
        return kName;
    }

    std::string_view ArrayModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view ArrayModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void ArrayModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Initialized = true;
        m_CurrentFrame = 0;
        m_Slots.clear();
        m_FreeSlots.clear();
        m_Metrics = Metrics();
    }

    void ArrayModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        for (auto &slot: m_Slots) {
            if (!slot.inUse) {
                continue;
            }
            auto &record = slot.record;
            if (record.pendingCompaction) {
                if (record.kind == StorageKind::Dense) {
                    CompactDense(record);
                } else {
                    CompactSparse(record);
                }
                record.pendingCompaction = false;
                m_Metrics.compactions += 1;
            } else {
                if (record.kind == StorageKind::Dense) {
                    MaybeDemote(record);
                } else {
                    MaybePromote(record);
                }
            }
            record.hot = false;
        }
    }

    void ArrayModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
    }

    void ArrayModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
    }

    StatusCode ArrayModule::CreateDense(std::string_view label, std::size_t capacityHint, Handle &outHandle) {
        return CreateInternal(label, StorageKind::Dense, capacityHint, outHandle);
    }

    StatusCode ArrayModule::CreateSparse(std::string_view label, Handle &outHandle) {
        return CreateInternal(label, StorageKind::Sparse, 0, outHandle);
    }

    StatusCode ArrayModule::Clone(Handle handle, std::string_view label, Handle &outHandle) {
        const auto *source = Find(handle);
        if (source == nullptr) {
            outHandle = 0;
            return StatusCode::NotFound;
        }
        StatusCode status = CreateInternal(label,
                                           source->kind,
                                           source->kind == StorageKind::Dense
                                               ? source->dense.items.capacity()
                                               : source->sparse.entries.size(),
                                           outHandle);
        if (status != StatusCode::Ok) {
            return status;
        }
        source = Find(handle);
        if (source == nullptr) {
            return StatusCode::InternalError;
        }
        auto *target = FindMutable(outHandle);
        if (target == nullptr) {
            return StatusCode::InternalError;
        }
        if (source->kind == StorageKind::Dense) {
            auto previousCapacity = target->dense.items.capacity();
            target->dense.items = source->dense.items;
            UpdateMetricsOnResizeDense(previousCapacity, target->dense.items.capacity());
            UpdateLength(*target, target->dense.items.size());
        } else {
            auto previous = target->sparse.entries.size();
            target->sparse.entries = source->sparse.entries;
            target->sparse.maxIndex = source->sparse.maxIndex;
            UpdateSparseEntryCount(*target, previous, target->sparse.entries.size());
            UpdateLength(*target, target->sparse.entries.empty() ? 0 : target->sparse.maxIndex + 1);
        }
        target->version = source->version;
        target->lastTouchFrame = m_CurrentFrame;
        target->hot = true;
        m_Metrics.clones += 1;
        m_Metrics.lastMutationFrame = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Destroy(Handle handle) {
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return StatusCode::NotFound;
        }
        auto generation = DecodeGeneration(handle);
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse || slot.generation != generation) {
            return StatusCode::NotFound;
        }
        UpdateMetricsOnDestroy(slot.record);
        slot.inUse = false;
        slot.generation += 1;
        m_FreeSlots.push_back(slotIndex);
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Push(Handle handle, const Value &value) {
        auto *record = FindMutable(handle);
        if (record == nullptr) {
            return StatusCode::NotFound;
        }
        if (record->kind == StorageKind::Dense) {
            auto &items = record->dense.items;
            if (items.capacity() == 0) {
                auto desired = AlignCapacity(std::max<std::size_t>(kDefaultDenseCapacity, items.size() + 1));
                items.reserve(desired);
                UpdateMetricsOnResizeDense(0, items.capacity());
            } else if (items.size() == items.capacity()) {
                auto previousCapacity = items.capacity();
                auto desired = AlignCapacity(previousCapacity * 2);
                std::vector<Value> expanded;
                expanded.reserve(desired);
                expanded.insert(expanded.end(), items.begin(), items.end());
                items.swap(expanded);
                UpdateMetricsOnResizeDense(previousCapacity, items.capacity());
            }
            items.push_back(value);
            UpdateLength(*record, items.size());
        } else {
            auto previous = record->sparse.entries.size();
            record->sparse.entries.push_back({record->length, value});
            std::sort(record->sparse.entries.begin(), record->sparse.entries.end(),
                      [](const SparseEntry &lhs, const SparseEntry &rhs) {
                          return lhs.index < rhs.index;
                      });
            auto current = record->sparse.entries.size();
            UpdateSparseEntryCount(*record, previous, current);
            record->sparse.maxIndex = record->sparse.entries.empty() ? 0 : record->sparse.entries.back().index;
            UpdateLength(*record, record->sparse.entries.empty() ? 0 : record->sparse.maxIndex + 1);
        }
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::PushNumber(Handle handle, double value) {
        return Push(handle, Value(value));
    }

    StatusCode ArrayModule::PushBoolean(Handle handle, bool value) {
        return Push(handle, Value(value));
    }

    StatusCode ArrayModule::PushString(Handle handle, std::string_view value) {
        return Push(handle, Value(value));
    }

    StatusCode ArrayModule::Pop(Handle handle, Value &outValue) {
        auto *record = FindMutable(handle);
        if (record == nullptr) {
            outValue = Value::Undefined();
            return StatusCode::NotFound;
        }
        if (record->kind == StorageKind::Dense) {
            auto &items = record->dense.items;
            if (items.empty()) {
                outValue = Value::Undefined();
                return StatusCode::NotFound;
            }
            outValue = items.back();
            items.pop_back();
            UpdateLength(*record, items.size());
            Touch(*record);
            EnsureCompaction(*record);
            return StatusCode::Ok;
        }
        auto &entries = record->sparse.entries;
        if (entries.empty()) {
            outValue = Value::Undefined();
            return StatusCode::NotFound;
        }
        std::sort(entries.begin(), entries.end(), [](const SparseEntry &lhs, const SparseEntry &rhs) {
            return lhs.index < rhs.index;
        });
        auto previous = entries.size();
        outValue = entries.back().value;
        entries.pop_back();
        UpdateSparseEntryCount(*record, previous, entries.size());
        record->sparse.maxIndex = entries.empty() ? 0 : entries.back().index;
        UpdateLength(*record, entries.empty() ? 0 : record->sparse.maxIndex + 1);
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Shift(Handle handle, Value &outValue) {
        auto *record = FindMutable(handle);
        if (record == nullptr) {
            outValue = Value::Undefined();
            return StatusCode::NotFound;
        }
        if (record->kind == StorageKind::Dense) {
            auto &items = record->dense.items;
            if (items.empty()) {
                outValue = Value::Undefined();
                return StatusCode::NotFound;
            }
            outValue = items.front();
            std::move(items.begin() + 1, items.end(), items.begin());
            items.pop_back();
            UpdateLength(*record, items.size());
            Touch(*record);
            EnsureCompaction(*record);
            return StatusCode::Ok;
        }
        auto &entries = record->sparse.entries;
        if (entries.empty()) {
            outValue = Value::Undefined();
            return StatusCode::NotFound;
        }
        std::sort(entries.begin(), entries.end(), [](const SparseEntry &lhs, const SparseEntry &rhs) {
            return lhs.index < rhs.index;
        });
        auto previous = entries.size();
        outValue = entries.front().value;
        entries.erase(entries.begin());
        for (auto &entry: entries) {
            entry.index -= 1;
        }
        UpdateSparseEntryCount(*record, previous, entries.size());
        record->sparse.maxIndex = entries.empty() ? 0 : entries.back().index;
        UpdateLength(*record, entries.empty() ? 0 : record->sparse.maxIndex + 1);
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Unshift(Handle handle, const Value &value) {
        auto *record = FindMutable(handle);
        if (record == nullptr) {
            return StatusCode::NotFound;
        }
        if (record->kind == StorageKind::Dense) {
            auto &items = record->dense.items;
            if (items.capacity() == 0) {
                items.reserve(AlignCapacity(kDefaultDenseCapacity));
                UpdateMetricsOnResizeDense(0, items.capacity());
            } else if (items.size() == items.capacity()) {
                auto previousCapacity = items.capacity();
                std::vector<Value> expanded;
                expanded.reserve(AlignCapacity(previousCapacity * 2));
                expanded.insert(expanded.end(), items.begin(), items.end());
                items.swap(expanded);
                UpdateMetricsOnResizeDense(previousCapacity, items.capacity());
            }
            items.insert(items.begin(), value);
            UpdateLength(*record, items.size());
        } else {
            auto &entries = record->sparse.entries;
            auto previous = entries.size();
            for (auto &entry: entries) {
                entry.index += 1;
            }
            entries.insert(entries.begin(), SparseEntry{0, value});
            std::sort(entries.begin(), entries.end(), [](const SparseEntry &lhs, const SparseEntry &rhs) {
                return lhs.index < rhs.index;
            });
            UpdateSparseEntryCount(*record, previous, entries.size());
            record->sparse.maxIndex = entries.empty() ? 0 : entries.back().index;
            UpdateLength(*record, entries.empty() ? 0 : record->sparse.maxIndex + 1);
        }
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Get(Handle handle, std::size_t index, Value &outValue) const {
        auto *record = Find(handle);
        if (record == nullptr) {
            outValue = Value::Undefined();
            return StatusCode::NotFound;
        }
        if (record->kind == StorageKind::Dense) {
            if (index >= record->dense.items.size()) {
                outValue = Value::Undefined();
                return StatusCode::NotFound;
            }
            outValue = record->dense.items[index];
            return StatusCode::Ok;
        }
        const auto &entries = record->sparse.entries;
        auto it = std::lower_bound(entries.begin(), entries.end(), index,
                                   [](const SparseEntry &entry, std::size_t value) {
                                       return entry.index < value;
                                   });
        if (it == entries.end() || it->index != index) {
            outValue = Value::Undefined();
            return StatusCode::NotFound;
        }
        outValue = it->value;
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Set(Handle handle, std::size_t index, const Value &value) {
        auto *record = FindMutable(handle);
        if (record == nullptr) {
            return StatusCode::NotFound;
        }
        if (record->kind == StorageKind::Dense) {
            auto &items = record->dense.items;
            if (index > items.size()) {
                ConvertToSparse(*record);
                return Set(handle, index, value);
            }
            if (index == items.size()) {
                return Push(handle, value);
            }
            items[index] = value;
            Touch(*record);
            return StatusCode::Ok;
        }
        auto &entries = record->sparse.entries;
        auto previous = entries.size();
        auto it = std::lower_bound(entries.begin(), entries.end(), index,
                                   [](const SparseEntry &entry, std::size_t value) {
                                       return entry.index < value;
                                   });
        if (it != entries.end() && it->index == index) {
            it->value = value;
        } else {
            entries.insert(it, SparseEntry{index, value});
        }
        UpdateSparseEntryCount(*record, previous, entries.size());
        record->sparse.maxIndex = entries.empty() ? 0 : entries.back().index;
        UpdateLength(*record, entries.empty() ? 0 : record->sparse.maxIndex + 1);
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Erase(Handle handle, std::size_t index) {
        auto *record = FindMutable(handle);
        if (record == nullptr) {
            return StatusCode::NotFound;
        }
        if (record->kind == StorageKind::Dense) {
            auto &items = record->dense.items;
            if (index >= items.size()) {
                return StatusCode::NotFound;
            }
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
            UpdateLength(*record, items.size());
            Touch(*record);
            EnsureCompaction(*record);
            return StatusCode::Ok;
        }
        auto &entries = record->sparse.entries;
        if (entries.empty()) {
            return StatusCode::NotFound;
        }
        auto previous = entries.size();
        auto it = std::lower_bound(entries.begin(), entries.end(), index,
                                   [](const SparseEntry &entry, std::size_t value) {
                                       return entry.index < value;
                                   });
        if (it == entries.end() || it->index != index) {
            return StatusCode::NotFound;
        }
        entries.erase(it);
        for (auto &entry: entries) {
            if (entry.index > index) {
                entry.index -= 1;
            }
        }
        UpdateSparseEntryCount(*record, previous, entries.size());
        record->sparse.maxIndex = entries.empty() ? 0 : entries.back().index;
        UpdateLength(*record, entries.empty() ? 0 : record->sparse.maxIndex + 1);
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Fill(Handle handle, const Value &value) {
        auto *record = FindMutable(handle);
        if (record == nullptr) {
            return StatusCode::NotFound;
        }
        if (record->kind == StorageKind::Sparse) {
            ConvertToDense(*record);
        }
        auto &items = record->dense.items;
        for (auto &entry: items) {
            entry = value;
        }
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Reserve(Handle handle, std::size_t capacity) {
        auto *record = FindMutable(handle);
        if (record == nullptr) {
            return StatusCode::NotFound;
        }
        if (record->kind == StorageKind::Dense) {
            auto &items = record->dense.items;
            auto previousCapacity = items.capacity();
            auto desired = AlignCapacity(std::max(capacity, items.size()));
            if (desired <= previousCapacity) {
                return StatusCode::Ok;
            }
            items.reserve(desired);
            UpdateMetricsOnResizeDense(previousCapacity, items.capacity());
        } else {
            record->sparse.entries.reserve(capacity);
        }
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Trim(Handle handle) {
        auto *record = FindMutable(handle);
        if (record == nullptr) {
            return StatusCode::NotFound;
        }
        record->pendingCompaction = true;
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Clear(Handle handle) {
        auto *record = FindMutable(handle);
        if (record == nullptr) {
            return StatusCode::NotFound;
        }
        if (record->kind == StorageKind::Dense) {
            auto &items = record->dense.items;
            auto previousCapacity = items.capacity();
            items.clear();
            UpdateLength(*record, 0);
            Touch(*record);
            if (previousCapacity > AlignCapacity(kDefaultDenseCapacity)) {
                record->pendingCompaction = true;
            }
        } else {
            auto previous = record->sparse.entries.size();
            record->sparse.entries.clear();
            UpdateSparseEntryCount(*record, previous, 0);
            record->sparse.maxIndex = 0;
            UpdateLength(*record, 0);
            Touch(*record);
        }
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Concat(Handle destination, Handle source) {
        auto *dst = FindMutable(destination);
        const auto *src = Find(source);
        if (dst == nullptr || src == nullptr) {
            return StatusCode::NotFound;
        }
        std::vector<Value> buffer;
        buffer.reserve(src->length);
        Value value;
        for (std::size_t i = 0; i < src->length; ++i) {
            if (Get(source, i, value) != StatusCode::Ok) {
                value = Value::Undefined();
            }
            buffer.push_back(value);
        }
        for (const auto &entry: buffer) {
            Push(destination, entry);
        }
        Touch(*dst);
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::Slice(Handle handle, std::size_t begin, std::size_t end,
                                  std::vector<Value> &outValues) const {
        auto *record = Find(handle);
        if (record == nullptr) {
            outValues.clear();
            return StatusCode::NotFound;
        }
        if (begin > end || end > record->length) {
            outValues.clear();
            return StatusCode::InvalidArgument;
        }
        outValues.clear();
        outValues.reserve(end - begin);
        Value value;
        for (std::size_t i = begin; i < end; ++i) {
            if (Get(handle, i, value) != StatusCode::Ok) {
                value = Value::Undefined();
            }
            outValues.push_back(value);
        }
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::SortNumeric(Handle handle, bool ascending) {
        auto *record = FindMutable(handle);
        if (record == nullptr) {
            return StatusCode::NotFound;
        }
        if (record->kind == StorageKind::Sparse) {
            ConvertToDense(*record);
        }
        auto &items = record->dense.items;
        auto comparator = [ascending](const Value &lhs, const Value &rhs) {
            int cmp = NumericCompare(lhs, rhs);
            if (ascending) {
                return cmp < 0;
            }
            return cmp > 0;
        };
        std::sort(items.begin(), items.end(), comparator);
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::SortLexicographic(Handle handle, bool ascending) {
        auto *record = FindMutable(handle);
        if (record == nullptr) {
            return StatusCode::NotFound;
        }
        if (record->kind == StorageKind::Sparse) {
            ConvertToDense(*record);
        }
        auto &items = record->dense.items;
        auto comparator = [ascending](const Value &lhs, const Value &rhs) {
            int cmp = LexicographicCompare(lhs, rhs);
            if (ascending) {
                return cmp < 0;
            }
            return cmp > 0;
        };
        std::sort(items.begin(), items.end(), comparator);
        Touch(*record);
        return StatusCode::Ok;
    }

    StatusCode ArrayModule::BinarySearch(Handle handle, const Value &needle, bool numeric,
                                         std::size_t &outIndex) const {
        auto *record = Find(handle);
        outIndex = std::numeric_limits<std::size_t>::max();
        if (record == nullptr) {
            return StatusCode::NotFound;
        }
        if (record->kind != StorageKind::Dense) {
            return StatusCode::InvalidArgument;
        }
        const auto &items = record->dense.items;
        if (items.empty()) {
            return StatusCode::NotFound;
        }
        std::size_t left = 0;
        std::size_t right = items.size();
        while (left < right) {
            std::size_t mid = left + ((right - left) >> 1);
            int cmp = numeric ? NumericCompare(items[mid], needle) : LexicographicCompare(items[mid], needle);
            if (cmp == 0) {
                outIndex = mid;
                return StatusCode::Ok;
            }
            if (cmp < 0) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
        return StatusCode::NotFound;
    }

    std::size_t ArrayModule::Length(Handle handle) const {
        auto *record = Find(handle);
        if (record == nullptr) {
            return 0;
        }
        if (record->kind == StorageKind::Dense) {
            return record->dense.items.size();
        }
        return record->length;
    }

    bool ArrayModule::Has(Handle handle) const noexcept {
        return Find(handle) != nullptr;
    }

    ArrayModule::StorageKind ArrayModule::KindOf(Handle handle) const noexcept {
        auto *record = Find(handle);
        if (record == nullptr) {
            return StorageKind::Dense;
        }
        return record->kind;
    }

    const ArrayModule::Metrics &ArrayModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool ArrayModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    ArrayModule::ArrayRecord *ArrayModule::FindMutable(Handle handle) noexcept {
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        auto generation = DecodeGeneration(handle);
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse || slot.generation != generation) {
            return nullptr;
        }
        return &slot.record;
    }

    const ArrayModule::ArrayRecord *ArrayModule::Find(Handle handle) const noexcept {
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        auto generation = DecodeGeneration(handle);
        const auto &slot = m_Slots[slotIndex];
        if (!slot.inUse || slot.generation != generation) {
            return nullptr;
        }
        return &slot.record;
    }

    std::uint32_t ArrayModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t ArrayModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }

    ArrayModule::Handle ArrayModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    StatusCode ArrayModule::CreateInternal(std::string_view label, StorageKind kind, std::size_t capacityHint,
                                           Handle &outHandle) {
        std::uint32_t slotIndex;
        if (!m_FreeSlots.empty()) {
            slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            auto &slot = m_Slots[slotIndex];
            slot.inUse = true;
            slot.generation += 1;
            outHandle = EncodeHandle(slotIndex, slot.generation);
            ResetRecord(slot.record, kind, label, capacityHint, outHandle, slotIndex, slot.generation);
            UpdateMetricsOnCreate(slot.record);
            return StatusCode::Ok;
        }
        slotIndex = static_cast<std::uint32_t>(m_Slots.size());
        Slot slot{};
        slot.inUse = true;
        slot.generation = 1;
        m_Slots.push_back(slot);
        outHandle = EncodeHandle(slotIndex, m_Slots[slotIndex].generation);
        ResetRecord(m_Slots[slotIndex].record, kind, label, capacityHint, outHandle, slotIndex,
                    m_Slots[slotIndex].generation);
        UpdateMetricsOnCreate(m_Slots[slotIndex].record);
        return StatusCode::Ok;
    }

    void ArrayModule::ResetRecord(ArrayRecord &record, StorageKind kind, std::string_view label,
                                  std::size_t capacityHint, Handle handle, std::uint32_t slot,
                                  std::uint32_t generation) {
        record.handle = handle;
        record.slot = slot;
        record.generation = generation;
        record.kind = kind;
        record.label.assign(label);
        record.dense.items.clear();
        record.sparse.entries.clear();
        record.sparse.maxIndex = 0;
        record.length = 0;
        record.version = 0;
        record.lastTouchFrame = m_CurrentFrame;
        record.pendingCompaction = false;
        record.hot = true;
        if (kind == StorageKind::Dense) {
            auto desired = AlignCapacity(std::max<std::size_t>(capacityHint, kDefaultDenseCapacity));
            record.dense.items.reserve(desired);
            UpdateMetricsOnResizeDense(0, record.dense.items.capacity());
        }
    }

    void ArrayModule::Touch(ArrayRecord &record) noexcept {
        record.version += 1;
        record.lastTouchFrame = m_CurrentFrame;
        record.hot = true;
        m_Metrics.lastMutationFrame = m_CurrentFrame;
    }

    void ArrayModule::UpdateMetricsOnCreate(const ArrayRecord &record) {
        if (record.kind == StorageKind::Dense) {
            m_Metrics.denseCount += 1;
            m_Metrics.denseCapacity += record.dense.items.capacity();
        } else {
            m_Metrics.sparseCount += 1;
        }
        m_Metrics.lastMutationFrame = m_CurrentFrame;
    }

    void ArrayModule::UpdateMetricsOnDestroy(const ArrayRecord &record) {
        if (record.kind == StorageKind::Dense) {
            if (m_Metrics.denseCount > 0) {
                m_Metrics.denseCount -= 1;
            }
            if (m_Metrics.denseCapacity >= record.dense.items.capacity()) {
                m_Metrics.denseCapacity -= record.dense.items.capacity();
            } else {
                m_Metrics.denseCapacity = 0;
            }
            if (m_Metrics.denseLength >= record.length) {
                m_Metrics.denseLength -= record.length;
            } else {
                m_Metrics.denseLength = 0;
            }
        } else {
            if (m_Metrics.sparseCount > 0) {
                m_Metrics.sparseCount -= 1;
            }
            if (m_Metrics.sparseEntries >= record.sparse.entries.size()) {
                m_Metrics.sparseEntries -= record.sparse.entries.size();
            } else {
                m_Metrics.sparseEntries = 0;
            }
        }
    }

    void ArrayModule::UpdateMetricsOnResizeDense(std::size_t previousCapacity, std::size_t newCapacity) {
        if (newCapacity >= previousCapacity) {
            m_Metrics.denseCapacity += newCapacity - previousCapacity;
        } else {
            m_Metrics.denseCapacity -= previousCapacity - newCapacity;
        }
    }

    void ArrayModule::UpdateKindMetrics(StorageKind fromKind, StorageKind toKind) {
        if (fromKind == toKind) {
            return;
        }
        if (fromKind == StorageKind::Dense) {
            if (m_Metrics.denseCount > 0) {
                m_Metrics.denseCount -= 1;
            }
        } else {
            if (m_Metrics.sparseCount > 0) {
                m_Metrics.sparseCount -= 1;
            }
        }
        if (toKind == StorageKind::Dense) {
            m_Metrics.denseCount += 1;
        } else {
            m_Metrics.sparseCount += 1;
        }
    }

    void ArrayModule::UpdateLength(ArrayRecord &record, std::size_t newLength) {
        auto previous = record.length;
        if (record.kind == StorageKind::Dense) {
            if (newLength >= previous) {
                m_Metrics.denseLength += newLength - previous;
            } else {
                m_Metrics.denseLength -= previous - newLength;
            }
        }
        record.length = newLength;
    }

    void ArrayModule::UpdateSparseEntryCount(ArrayRecord &record, std::size_t previousCount, std::size_t newCount) {
        if (record.kind != StorageKind::Sparse) {
            return;
        }
        if (newCount >= previousCount) {
            m_Metrics.sparseEntries += newCount - previousCount;
        } else {
            m_Metrics.sparseEntries -= previousCount - newCount;
        }
    }

    void ArrayModule::ConvertToSparse(ArrayRecord &record) {
        if (record.kind == StorageKind::Sparse) {
            return;
        }
        auto previousCapacity = record.dense.items.capacity();
        auto previousLength = record.dense.items.size();
        UpdateKindMetrics(StorageKind::Dense, StorageKind::Sparse);
        if (m_Metrics.denseCapacity >= previousCapacity) {
            m_Metrics.denseCapacity -= previousCapacity;
        } else {
            m_Metrics.denseCapacity = 0;
        }
        if (m_Metrics.denseLength >= previousLength) {
            m_Metrics.denseLength -= previousLength;
        } else {
            m_Metrics.denseLength = 0;
        }
        std::vector<SparseEntry> entries;
        entries.reserve(previousLength);
        for (std::size_t i = 0; i < record.dense.items.size(); ++i) {
            if (!record.dense.items[i].Empty()) {
                entries.push_back({i, record.dense.items[i]});
            }
        }
        record.dense.items.clear();
        record.kind = StorageKind::Sparse;
        auto previous = record.sparse.entries.size();
        record.sparse.entries = std::move(entries);
        std::sort(record.sparse.entries.begin(), record.sparse.entries.end(),
                  [](const SparseEntry &lhs, const SparseEntry &rhs) {
                      return lhs.index < rhs.index;
                  });
        UpdateSparseEntryCount(record, previous, record.sparse.entries.size());
        record.sparse.maxIndex = record.sparse.entries.empty() ? 0 : record.sparse.entries.back().index;
        UpdateLength(record, record.sparse.entries.empty() ? 0 : record.sparse.maxIndex + 1);
        m_Metrics.transitionsToSparse += 1;
    }

    void ArrayModule::ConvertToDense(ArrayRecord &record) {
        if (record.kind == StorageKind::Dense) {
            return;
        }
        auto previousCount = record.sparse.entries.size();
        UpdateKindMetrics(StorageKind::Sparse, StorageKind::Dense);
        UpdateSparseEntryCount(record, previousCount, 0);
        std::size_t targetLength = record.sparse.entries.empty() ? 0 : record.sparse.maxIndex + 1;
        std::size_t desiredCapacity = AlignCapacity(std::max<std::size_t>(targetLength, kDefaultDenseCapacity));
        std::vector<Value> dense;
        dense.reserve(desiredCapacity);
        dense.resize(targetLength, Value::Undefined());
        for (const auto &entry: record.sparse.entries) {
            if (entry.index < dense.size()) {
                dense[entry.index] = entry.value;
            }
        }
        record.sparse.entries.clear();
        record.sparse.maxIndex = 0;
        record.kind = StorageKind::Dense;
        auto previousCapacity = record.dense.items.capacity();
        record.dense.items = std::move(dense);
        UpdateMetricsOnResizeDense(previousCapacity, record.dense.items.capacity());
        UpdateLength(record, record.dense.items.size());
        m_Metrics.transitionsToDense += 1;
    }

    void ArrayModule::EnsureCompaction(ArrayRecord &record) {
        if (record.kind == StorageKind::Dense) {
            auto capacity = record.dense.items.capacity();
            auto size = record.dense.items.size();
            if (capacity > 0 && size * 2 < capacity) {
                record.pendingCompaction = true;
            }
        } else {
            record.pendingCompaction = true;
        }
    }

    void ArrayModule::CompactDense(ArrayRecord &record) {
        auto previousCapacity = record.dense.items.capacity();
        auto desiredCapacity = AlignCapacity(std::max<std::size_t>(record.dense.items.size(), kDefaultDenseCapacity));
        if (desiredCapacity == previousCapacity) {
            return;
        }
        std::vector<Value> compacted;
        compacted.reserve(desiredCapacity);
        compacted.insert(compacted.end(), record.dense.items.begin(), record.dense.items.end());
        record.dense.items.swap(compacted);
        UpdateMetricsOnResizeDense(previousCapacity, record.dense.items.capacity());
    }

    void ArrayModule::CompactSparse(ArrayRecord &record) {
        if (record.sparse.entries.empty()) {
            record.sparse.maxIndex = 0;
            UpdateLength(record, 0);
            return;
        }
        std::sort(record.sparse.entries.begin(), record.sparse.entries.end(),
                  [](const SparseEntry &lhs, const SparseEntry &rhs) {
                      return lhs.index < rhs.index;
                  });
        auto previous = record.sparse.entries.size();
        auto endIt = std::unique(record.sparse.entries.begin(), record.sparse.entries.end(),
                                 [](const SparseEntry &lhs, const SparseEntry &rhs) {
                                     return lhs.index == rhs.index;
                                 });
        record.sparse.entries.erase(endIt, record.sparse.entries.end());
        UpdateSparseEntryCount(record, previous, record.sparse.entries.size());
        record.sparse.maxIndex = record.sparse.entries.empty() ? 0 : record.sparse.entries.back().index;
        UpdateLength(record, record.sparse.entries.empty() ? 0 : record.sparse.maxIndex + 1);
    }

    bool ArrayModule::MaybePromote(ArrayRecord &record) {
        if (record.kind != StorageKind::Sparse) {
            return false;
        }
        if (record.sparse.entries.empty()) {
            ConvertToDense(record);
            return true;
        }
        if (record.length == 0) {
            ConvertToDense(record);
            return true;
        }
        double density = static_cast<double>(record.sparse.entries.size()) / static_cast<double>(record.length);
        if (density >= kSparsePromotionRatio) {
            ConvertToDense(record);
            return true;
        }
        return false;
    }

    bool ArrayModule::MaybeDemote(ArrayRecord &record) {
        if (record.kind != StorageKind::Dense) {
            return false;
        }
        auto capacity = record.dense.items.capacity();
        if (capacity == 0) {
            return false;
        }
        auto size = record.dense.items.size();
        if (size == 0 && capacity > kDefaultDenseCapacity) {
            record.pendingCompaction = true;
            return true;
        }
        double fill = static_cast<double>(size) / static_cast<double>(capacity);
        if (fill < kDenseCompactionRatio && capacity > kDefaultDenseCapacity) {
            record.pendingCompaction = true;
            return true;
        }
        return false;
    }

    std::size_t ArrayModule::AlignCapacity(std::size_t capacity) noexcept {
        if (capacity < kDefaultDenseCapacity) {
            return kDefaultDenseCapacity;
        }
        constexpr std::size_t kMask = 15;
        return (capacity + kMask) & ~kMask;
    }

    int ArrayModule::LexicographicCompare(const Value &a, const Value &b) noexcept {
        return a.ToString().compare(b.ToString());
    }

    int ArrayModule::NumericCompare(const Value &a, const Value &b) noexcept {
        double lhs = ResolveNumeric(a, 0.0);
        double rhs = ResolveNumeric(b, 0.0);
        if (lhs < rhs) {
            return -1;
        }
        if (lhs > rhs) {
            return 1;
        }
        return 0;
    }

    bool ArrayModule::IsNumeric(const Value &value) noexcept {
        if (value.IsNumeric() || value.IsBoolean()) {
            return true;
        }
        if (!value.IsString()) {
            return false;
        }
        auto textView = value.AsString();
        if (textView.empty()) {
            return false;
        }
        std::string buffer(textView);
        std::istringstream stream(buffer);
        double parsed{};
        stream >> parsed;
        return stream && stream.peek() == std::char_traits<char>::eof();
    }

    double ArrayModule::ResolveNumeric(const Value &value, double fallback) noexcept {
        if (value.IsNumeric() || value.IsBoolean()) {
            return value.AsNumber(fallback);
        }
        if (value.IsString()) {
            auto textView = value.AsString();
            if (!textView.empty()) {
                std::string buffer(textView);
                std::istringstream stream(buffer);
                double parsed{};
                stream >> parsed;
                if (stream && stream.peek() == std::char_traits<char>::eof()) {
                    return parsed;
                }
            }
        }
        return fallback;
    }

}
