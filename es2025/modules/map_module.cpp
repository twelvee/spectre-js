#include "spectre/es2025/modules/map_module.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Map";
        constexpr std::string_view kSummary = "Map keyed collection with ordered entries and iterator support.";
        constexpr std::string_view kReference = "ECMA-262 Section 24.1";
        constexpr std::uint32_t kInitialBucketCount = 8;


    }

    struct MapModule::Entry {
        Entry()
            : hash(0),
              active(false),
              key(),
              value(),
              orderPrev(kInvalidIndex),
              orderNext(kInvalidIndex) {
        }

        std::uint64_t hash;
        bool active;
        Value key;
        Value value;
        std::uint32_t orderPrev;
        std::uint32_t orderNext;
    };

    struct MapModule::MapRecord {
        MapRecord()
            : handle(0),
              slot(0),
              generation(0),
              label(),
              size(0),
              head(kInvalidIndex),
              tail(kInvalidIndex),
              version(0),
              lastTouchFrame(0),
              entries(),
              buckets(),
              freeEntries() {
        }

        Handle handle;
        std::uint32_t slot;
        std::uint32_t generation;
        std::string label;
        std::uint32_t size;
        std::uint32_t head;
        std::uint32_t tail;
        std::uint64_t version;
        std::uint64_t lastTouchFrame;
        std::vector<Entry> entries;
        std::vector<std::uint32_t> buckets;
        std::vector<std::uint32_t> freeEntries;
    };

    struct MapModule::SlotRecord {
        SlotRecord() : inUse(false), generation(0), record() {
        }

        bool inUse;
        std::uint32_t generation;
        MapRecord record;
    };

    MapModule::MapModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_Metrics{},
          m_Slots(),
          m_FreeSlots(),
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0) {
    }

    MapModule::~MapModule() = default;

    std::string_view MapModule::Name() const noexcept {
        return kName;
    }

    std::string_view MapModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view MapModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void MapModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Metrics = {};
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_Metrics.lastFrameTouched = 0;
        m_Slots.clear();
        m_FreeSlots.clear();
        m_CurrentFrame = 0;
        m_Initialized = true;
    }

    void MapModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void MapModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void MapModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode MapModule::Create(std::string_view label, Handle &outHandle) {
        outHandle = 0;
        std::uint32_t slotIndex;
        if (!m_FreeSlots.empty()) {
            slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            auto &slot = m_Slots[slotIndex];
            slot.inUse = true;
            slot.generation += 1;
        } else {
            slotIndex = static_cast<std::uint32_t>(m_Slots.size());
            SlotRecord slot;
            slot.inUse = true;
            slot.generation = 1;
            m_Slots.push_back(slot);
        }
        auto &slot = m_Slots[slotIndex];
        auto &record = slot.record;
        record = MapRecord();
        record.slot = slotIndex;
        record.generation = slot.generation;
        record.handle = EncodeHandle(slotIndex, slot.generation);
        record.label.assign(label.begin(), label.end());
        record.size = 0;
        record.head = kInvalidIndex;
        record.tail = kInvalidIndex;
        record.version = 0;
        record.lastTouchFrame = m_CurrentFrame;
        record.entries.clear();
        record.buckets.clear();
        record.freeEntries.clear();
        EnsureCapacity(record);
        Touch(record);
        TouchMetrics();
        outHandle = record.handle;
        m_Metrics.liveMaps += 1;
        m_Metrics.totalAllocations += 1;
        return StatusCode::Ok;
    }

    StatusCode MapModule::Destroy(Handle handle) {
        auto *slot = FindMutableSlot(handle);
        if (!slot) {
            return StatusCode::NotFound;
        }
        slot->inUse = false;
        auto index = slot->record.slot;
        slot->record = MapRecord();
        m_FreeSlots.push_back(index);
        if (m_Metrics.liveMaps > 0) {
            m_Metrics.liveMaps -= 1;
        }
        m_Metrics.totalReleases += 1;
        TouchMetrics();
        return StatusCode::Ok;
    }

    StatusCode MapModule::Clear(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        for (auto &entry: record->entries) {
            if (!entry.active) {
                continue;
            }
            entry.active = false;
            entry.hash = 0;
            entry.key.Reset();
            entry.value.Reset();
            entry.orderPrev = kInvalidIndex;
            entry.orderNext = kInvalidIndex;
            record->freeEntries.push_back(static_cast<std::uint32_t>(&entry - record->entries.data()));
        }
        record->size = 0;
        record->head = kInvalidIndex;
        record->tail = kInvalidIndex;
        std::fill(record->buckets.begin(), record->buckets.end(), kInvalidIndex);
        Touch(*record);
        TouchMetrics();
        m_Metrics.clears += 1;
        return StatusCode::Ok;
    }

    StatusCode MapModule::Set(Handle handle, const Value &key, const Value &value) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        m_Metrics.setOps += 1;
        EnsureCapacity(*record);
        auto hash = HashValue(key);
        bool collision = false;
        std::uint32_t bucket = 0;
        auto index = Locate(*record, key, hash, bucket, collision);
        if (index != kInvalidIndex) {
            record->entries[index].value = value;
            Touch(*record);
            if (collision) {
                m_Metrics.collisions += 1;
            }
            return StatusCode::Ok;
        }
        auto status = Insert(*record, key, value, hash, true);
        if (status == StatusCode::Ok) {
            if (collision) {
                m_Metrics.collisions += 1;
            }
        }
        return status;
    }

    StatusCode MapModule::Get(Handle handle, const Value &key, Value &outValue) const {
        outValue.Reset();
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        const_cast<MapModule *>(this)->m_Metrics.getOps += 1;
        auto hash = HashValue(key);
        std::uint32_t bucket = 0;
        bool collision = false;
        auto index = Locate(*record, key, hash, bucket, collision);
        if (index == kInvalidIndex) {
            const_cast<MapModule *>(this)->m_Metrics.misses += 1;
            return StatusCode::NotFound;
        }
        outValue = record->entries[index].value;
        const_cast<MapModule *>(this)->m_Metrics.hits += 1;
        const_cast<MapModule *>(this)->TouchMetrics();
        return StatusCode::Ok;
    }

    bool MapModule::Has(Handle handle, const Value &key) const {
        Value value;
        return Get(handle, key, value) == StatusCode::Ok;
    }

    StatusCode MapModule::Delete(Handle handle, const Value &key, bool &outDeleted) {
        outDeleted = false;
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        m_Metrics.deleteOps += 1;
        auto hash = HashValue(key);
        bool deleted = false;
        auto status = Remove(*record, key, hash, deleted);
        if (deleted) {
            outDeleted = true;
            m_Metrics.hits += 1;
        } else {
            m_Metrics.misses += 1;
        }
        return status;
    }

    std::uint32_t MapModule::Size(Handle handle) const {
        const auto *record = Find(handle);
        return record ? record->size : 0;
    }

    StatusCode MapModule::Keys(Handle handle, std::vector<Value> &keys) const {
        keys.clear();
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        keys.reserve(record->size);
        auto index = record->head;
        while (index != kInvalidIndex) {
            const auto &entry = record->entries[index];
            if (entry.active) {
                keys.push_back(entry.key);
            }
            index = entry.orderNext;
        }
        const_cast<MapModule *>(this)->m_Metrics.iterations += 1;
        const_cast<MapModule *>(this)->TouchMetrics();
        return StatusCode::Ok;
    }

    StatusCode MapModule::Values(Handle handle, std::vector<Value> &values) const {
        values.clear();
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        values.reserve(record->size);
        auto index = record->head;
        while (index != kInvalidIndex) {
            const auto &entry = record->entries[index];
            if (entry.active) {
                values.push_back(entry.value);
            }
            index = entry.orderNext;
        }
        const_cast<MapModule *>(this)->m_Metrics.iterations += 1;
        const_cast<MapModule *>(this)->TouchMetrics();
        return StatusCode::Ok;
    }

    StatusCode MapModule::Entries(Handle handle, std::vector<std::pair<Value, Value> > &entries) const {
        entries.clear();
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        entries.reserve(record->size);
        auto index = record->head;
        while (index != kInvalidIndex) {
            const auto &entry = record->entries[index];
            if (entry.active) {
                entries.emplace_back(entry.key, entry.value);
            }
            index = entry.orderNext;
        }
        const_cast<MapModule *>(this)->m_Metrics.iterations += 1;
        const_cast<MapModule *>(this)->TouchMetrics();
        return StatusCode::Ok;
    }

    const MapModule::Metrics &MapModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    MapModule::SlotRecord *MapModule::FindMutableSlot(Handle handle) noexcept {
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
        return &slot;
    }

    const MapModule::SlotRecord *MapModule::FindSlot(Handle handle) const noexcept {
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
        return &slot;
    }

    MapModule::MapRecord *MapModule::FindMutable(Handle handle) noexcept {
        auto *slot = FindMutableSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    const MapModule::MapRecord *MapModule::Find(Handle handle) const noexcept {
        const auto *slot = FindSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    MapModule::Handle MapModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::uint32_t MapModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t MapModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }

        std::uint64_t MapModule::HashValue(const Value &value) noexcept {
        auto hash = value.Hash();
        if (hash == 0) {
            hash = 0x9e3779b97f4a7c15ull;
        }
        return hash;
    }


    void MapModule::Touch(MapRecord &record) noexcept {
        record.version += 1;
        record.lastTouchFrame = m_CurrentFrame;
        TouchMetrics();
    }

    void MapModule::TouchMetrics() noexcept {
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void MapModule::EnsureCapacity(MapRecord &record) {
        if (record.buckets.empty()) {
            Rehash(record, kInitialBucketCount);
            return;
        }
        auto capacity = static_cast<std::uint32_t>(record.buckets.size());
        auto limit = (capacity * 3u) / 5u;
        if (limit < 1) {
            limit = 1;
        }
        if (record.size + 1 > limit) {
            Rehash(record, capacity * 2);
        }
    }

    void MapModule::Rehash(MapRecord &record, std::uint32_t newBucketCount) {
        if (newBucketCount < kInitialBucketCount) {
            newBucketCount = kInitialBucketCount;
        }
        if ((newBucketCount & (newBucketCount - 1)) != 0) {
            std::uint32_t power = 1;
            while (power < newBucketCount && power < (1u << 30)) {
                power <<= 1;
            }
            newBucketCount = power;
        }
        std::vector<std::uint32_t> buckets(newBucketCount, kInvalidIndex);
        auto mask = newBucketCount - 1;
        for (std::uint32_t index = 0; index < record.entries.size(); ++index) {
            auto &entry = record.entries[index];
            if (!entry.active) {
                continue;
            }
            auto bucket = static_cast<std::uint32_t>(entry.hash) & mask;
            while (true) {
                if (buckets[bucket] == kInvalidIndex) {
                    buckets[bucket] = index;
                    break;
                }
                bucket = (bucket + 1) & mask;
            }
        }
        record.buckets.swap(buckets);
        m_Metrics.rehashes += 1;
    }

    std::uint32_t MapModule::Locate(const MapRecord &record, const Value &key, std::uint64_t hash,
                                    std::uint32_t &bucket, bool &collision) const {
        if (record.buckets.empty()) {
            bucket = 0;
            collision = false;
            return kInvalidIndex;
        }
        auto mask = static_cast<std::uint32_t>(record.buckets.size() - 1);
        auto index = static_cast<std::uint32_t>(hash) & mask;
        std::uint32_t firstDeleted = kInvalidIndex;
        std::uint32_t probes = 0;
        collision = false;
        while (true) {
            auto entryIndex = record.buckets[index];
            if (entryIndex == kInvalidIndex) {
                bucket = firstDeleted != kInvalidIndex ? firstDeleted : index;
                collision = probes != 0;
                return kInvalidIndex;
            }
            if (entryIndex == kDeletedIndex) {
                if (firstDeleted == kInvalidIndex) {
                    firstDeleted = index;
                }
            } else {
                const auto &entry = record.entries[entryIndex];
                if (entry.active && entry.hash == hash && entry.key.SameValueZero(key)) {
                    bucket = index;
                    collision = probes != 0;
                    return entryIndex;
                }
            }
            index = (index + 1) & mask;
            ++probes;
            if (probes >= record.buckets.size()) {
                bucket = firstDeleted != kInvalidIndex ? firstDeleted : index;
                collision = true;
                return kInvalidIndex;
            }
        }
    }

    std::uint32_t MapModule::AllocateEntry(MapRecord &record) {
        if (!record.freeEntries.empty()) {
            auto index = record.freeEntries.back();
            record.freeEntries.pop_back();
            auto &entry = record.entries[index];
            entry.active = true;
            entry.hash = 0;
            entry.key.Reset();
            entry.value.Reset();
            entry.orderPrev = kInvalidIndex;
            entry.orderNext = kInvalidIndex;
            return index;
        }
        Entry entry;
        entry.active = true;
        record.entries.push_back(entry);
        return static_cast<std::uint32_t>(record.entries.size() - 1);
    }

    StatusCode MapModule::Insert(MapRecord &record, const Value &key, const Value &value, std::uint64_t hash,
                                 bool allowReplace) {
        std::uint32_t bucket = 0;
        bool collision = false;
        auto existing = Locate(record, key, hash, bucket, collision);
        if (existing != kInvalidIndex) {
            if (!allowReplace) {
                return StatusCode::AlreadyExists;
            }
            record.entries[existing].value = value;
            Touch(record);
            if (collision) {
                m_Metrics.collisions += 1;
            }
            return StatusCode::Ok;
        }
        auto entryIndex = AllocateEntry(record);
        auto &entry = record.entries[entryIndex];
        entry.hash = hash;
        entry.key = key;
        entry.value = value;
        entry.orderPrev = kInvalidIndex;
        entry.orderNext = kInvalidIndex;
        if (record.buckets.empty()) {
            record.buckets.resize(kInitialBucketCount, kInvalidIndex);
        }
        record.buckets[bucket] = entryIndex;
        LinkTail(record, entryIndex);
        record.size += 1;
        Touch(record);
        if (collision) {
            m_Metrics.collisions += 1;
        }
        return StatusCode::Ok;
    }

    StatusCode MapModule::Remove(MapRecord &record, const Value &key, std::uint64_t hash, bool &outDeleted) {
        outDeleted = false;
        if (record.buckets.empty()) {
            return StatusCode::Ok;
        }
        std::uint32_t bucket = 0;
        bool collision = false;
        auto index = Locate(record, key, hash, bucket, collision);
        if (index == kInvalidIndex) {
            return StatusCode::Ok;
        }
        auto &entry = record.entries[index];
        if (!entry.active) {
            return StatusCode::Ok;
        }
        entry.active = false;
        entry.hash = 0;
        entry.key.Reset();
        entry.value.Reset();
        Unlink(record, index);
        record.freeEntries.push_back(index);
        record.buckets[bucket] = kDeletedIndex;
        outDeleted = true;
        if (record.size > 0) {
            record.size -= 1;
        }
        Touch(record);
        if (collision) {
            m_Metrics.collisions += 1;
        }
        return StatusCode::Ok;
    }

    void MapModule::LinkTail(MapRecord &record, std::uint32_t entryIndex) {
        auto &entry = record.entries[entryIndex];
        if (record.tail == kInvalidIndex) {
            record.head = entryIndex;
            record.tail = entryIndex;
            entry.orderPrev = kInvalidIndex;
            entry.orderNext = kInvalidIndex;
            return;
        }
        auto &tailEntry = record.entries[record.tail];
        tailEntry.orderNext = entryIndex;
        entry.orderPrev = record.tail;
        entry.orderNext = kInvalidIndex;
        record.tail = entryIndex;
    }

    void MapModule::Unlink(MapRecord &record, std::uint32_t entryIndex) {
        auto &entry = record.entries[entryIndex];
        auto prev = entry.orderPrev;
        auto next = entry.orderNext;
        if (prev != kInvalidIndex) {
            record.entries[prev].orderNext = next;
        } else {
            record.head = next;
        }
        if (next != kInvalidIndex) {
            record.entries[next].orderPrev = prev;
        } else {
            record.tail = prev;
        }
        entry.orderPrev = kInvalidIndex;
        entry.orderNext = kInvalidIndex;
    }
}

