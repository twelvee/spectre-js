#include "spectre/es2025/modules/weak_set_module.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "spectre/runtime.h"
#include "spectre/es2025/environment.h"
#include "spectre/es2025/modules/object_module.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "WeakSet";
        constexpr std::string_view kSummary = "WeakSet membership collection with garbage collected entries.";
        constexpr std::string_view kReference = "ECMA-262 Section 24.4";
        constexpr std::uint32_t kInitialBucketCount = 8;

        std::uint64_t HashBytes(const void *data, std::size_t size) noexcept {
            const auto *bytes = static_cast<const std::uint8_t *>(data);
            std::uint64_t hash = 1469598103934665603ull;
            for (std::size_t i = 0; i < size; ++i) {
                hash ^= static_cast<std::uint64_t>(bytes[i]);
                hash *= 1099511628211ull;
            }
            if (hash == 0) {
                hash = 1469598103934665603ull;
            }
            return hash;
        }
    }

    struct WeakSetModule::Entry {
        Entry() : hash(0), active(false), value(0) {
        }

        std::uint64_t hash;
        bool active;
        ObjectModule::Handle value;
    };

    struct WeakSetModule::SetRecord {
        SetRecord()
            : handle(0),
              slot(0),
              generation(0),
              label(),
              size(0),
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
        std::uint64_t version;
        std::uint64_t lastTouchFrame;
        std::vector<Entry> entries;
        std::vector<std::uint32_t> buckets;
        std::vector<std::uint32_t> freeEntries;
    };

    struct WeakSetModule::SlotRecord {
        SlotRecord() : inUse(false), generation(0), record() {
        }

        bool inUse;
        std::uint32_t generation;
        SetRecord record;
    };

    WeakSetModule::WeakSetModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_Metrics{},
          m_Slots(),
          m_FreeSlots(),
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_ObjectModule(nullptr) {
    }

    WeakSetModule::~WeakSetModule() = default;

    std::string_view WeakSetModule::Name() const noexcept {
        return kName;
    }

    std::string_view WeakSetModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view WeakSetModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void WeakSetModule::Initialize(const ModuleInitContext &context) {
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
        auto &environment = context.runtime.EsEnvironment();
        auto *module = environment.FindModule("Object");
        m_ObjectModule = dynamic_cast<ObjectModule *>(module);
    }

    void WeakSetModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void WeakSetModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void WeakSetModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode WeakSetModule::Create(std::string_view label, Handle &outHandle) {
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
        record = SetRecord();
        record.slot = slotIndex;
        record.generation = slot.generation;
        record.handle = EncodeHandle(slotIndex, slot.generation);
        record.label.assign(label.begin(), label.end());
        record.size = 0;
        record.version = 0;
        record.lastTouchFrame = m_CurrentFrame;
        record.entries.clear();
        record.buckets.clear();
        record.freeEntries.clear();
        EnsureCapacity(record);
        Touch(record);
        TouchMetrics();
        outHandle = record.handle;
        m_Metrics.liveSets += 1;
        m_Metrics.totalAllocations += 1;
        return StatusCode::Ok;
    }

    StatusCode WeakSetModule::Destroy(Handle handle) {
        auto *slot = FindMutableSlot(handle);
        if (!slot) {
            return StatusCode::NotFound;
        }
        slot->inUse = false;
        auto index = slot->record.slot;
        slot->record = SetRecord();
        m_FreeSlots.push_back(index);
        if (m_Metrics.liveSets > 0) {
            m_Metrics.liveSets -= 1;
        }
        m_Metrics.totalReleases += 1;
        TouchMetrics();
        return StatusCode::Ok;
    }

    StatusCode WeakSetModule::Clear(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        for (std::uint32_t index = 0; index < record->entries.size(); ++index) {
            auto &entry = record->entries[index];
            if (!entry.active) {
                continue;
            }
            entry.active = false;
            entry.hash = 0;
            entry.value = 0;
            record->freeEntries.push_back(index);
        }
        record->size = 0;
        std::fill(record->buckets.begin(), record->buckets.end(), kInvalidIndex);
        Touch(*record);
        TouchMetrics();
        m_Metrics.clears += 1;
        return StatusCode::Ok;
    }

    StatusCode WeakSetModule::Add(Handle handle, ObjectModule::Handle value) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!m_ObjectModule || !m_ObjectModule->IsValid(value)) {
            return StatusCode::InvalidArgument;
        }
        m_Metrics.addOps += 1;
        EnsureCapacity(*record);
        auto hash = HashKey(value);
        std::uint32_t bucket = 0;
        bool collision = false;
        auto existing = Locate(*record, value, hash, bucket, collision);
        if (existing != kInvalidIndex) {
            if (collision) {
                m_Metrics.collisions += 1;
            }
            m_Metrics.hits += 1;
            TouchMetrics();
            return StatusCode::Ok;
        }
        auto status = Insert(*record, value, hash);
        if (collision && status == StatusCode::Ok) {
            m_Metrics.collisions += 1;
        }
        if (status == StatusCode::Ok) {
            m_Metrics.hits += 1;
        }
        return status;
    }

    bool WeakSetModule::Has(Handle handle, ObjectModule::Handle value) const {
        auto *self = const_cast<WeakSetModule *>(this);
        self->m_Metrics.hasOps += 1;
        const auto *record = Find(handle);
        if (!record) {
            self->m_Metrics.misses += 1;
            return false;
        }
        if (!self->m_ObjectModule || !self->m_ObjectModule->IsValid(value)) {
            self->m_Metrics.misses += 1;
            return false;
        }
        auto hash = HashKey(value);
        std::uint32_t bucket = 0;
        bool collision = false;
        auto index = Locate(*record, value, hash, bucket, collision);
        if (index == kInvalidIndex) {
            if (collision) {
                self->m_Metrics.collisions += 1;
            }
            self->m_Metrics.misses += 1;
            return false;
        }
        if (collision) {
            self->m_Metrics.collisions += 1;
        }
        self->m_Metrics.hits += 1;
        self->TouchMetrics();
        return true;
    }

    StatusCode WeakSetModule::Delete(Handle handle, ObjectModule::Handle value, bool &outDeleted) {
        outDeleted = false;
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!m_ObjectModule || !m_ObjectModule->IsValid(value)) {
            return StatusCode::InvalidArgument;
        }
        m_Metrics.deleteOps += 1;
        auto hash = HashKey(value);
        bool deleted = false;
        auto status = Remove(*record, value, hash, deleted);
        if (deleted) {
            outDeleted = true;
            m_Metrics.hits += 1;
        } else {
            m_Metrics.misses += 1;
        }
        return status;
    }

    StatusCode WeakSetModule::Compact(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        PruneInvalid(*record);
        Touch(*record);
        TouchMetrics();
        m_Metrics.compactions += 1;
        return StatusCode::Ok;
    }

    std::uint32_t WeakSetModule::Size(Handle handle) const {
        const auto *record = Find(handle);
        return record ? record->size : 0;
    }

    const WeakSetModule::Metrics &WeakSetModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    WeakSetModule::SlotRecord *WeakSetModule::FindMutableSlot(Handle handle) noexcept {
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

    const WeakSetModule::SlotRecord *WeakSetModule::FindSlot(Handle handle) const noexcept {
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

    WeakSetModule::SetRecord *WeakSetModule::FindMutable(Handle handle) noexcept {
        auto *slot = FindMutableSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    const WeakSetModule::SetRecord *WeakSetModule::Find(Handle handle) const noexcept {
        const auto *slot = FindSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    WeakSetModule::Handle WeakSetModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::uint32_t WeakSetModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t WeakSetModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }

    std::uint64_t WeakSetModule::HashKey(ObjectModule::Handle value) noexcept {
        return HashBytes(&value, sizeof(value));
    }

    void WeakSetModule::Touch(SetRecord &record) noexcept {
        record.version += 1;
        record.lastTouchFrame = m_CurrentFrame;
        TouchMetrics();
    }

    void WeakSetModule::TouchMetrics() noexcept {
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void WeakSetModule::EnsureCapacity(SetRecord &record) {
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

    void WeakSetModule::Rehash(SetRecord &record, std::uint32_t newBucketCount) {
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

    std::uint32_t WeakSetModule::Locate(const SetRecord &record, ObjectModule::Handle value, std::uint64_t hash,
                                        std::uint32_t &bucket, bool &collision) const {
        if (record.buckets.empty()) {
            bucket = 0;
            collision = false;
            return kInvalidIndex;
        }
        auto mask = static_cast<std::uint32_t>(record.buckets.size() - 1);
        auto index = static_cast<std::uint32_t>(hash) & mask;
        auto firstDeleted = kInvalidIndex;
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
                if (entry.active && entry.hash == hash && entry.value == value) {
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

    std::uint32_t WeakSetModule::AllocateEntry(SetRecord &record) {
        if (!record.freeEntries.empty()) {
            auto index = record.freeEntries.back();
            record.freeEntries.pop_back();
            auto &entry = record.entries[index];
            entry.active = true;
            entry.hash = 0;
            entry.value = 0;
            return index;
        }
        Entry entry;
        entry.active = true;
        record.entries.push_back(entry);
        return static_cast<std::uint32_t>(record.entries.size() - 1);
    }

    StatusCode WeakSetModule::Insert(SetRecord &record, ObjectModule::Handle value, std::uint64_t hash) {
        std::uint32_t bucket = 0;
        bool collision = false;
        auto existing = Locate(record, value, hash, bucket, collision);
        if (existing != kInvalidIndex) {
            if (collision) {
                m_Metrics.collisions += 1;
            }
            return StatusCode::AlreadyExists;
        }
        auto entryIndex = AllocateEntry(record);
        auto &entry = record.entries[entryIndex];
        entry.hash = hash;
        entry.value = value;
        if (record.buckets.empty()) {
            record.buckets.resize(kInitialBucketCount, kInvalidIndex);
        }
        record.buckets[bucket] = entryIndex;
        record.size += 1;
        Touch(record);
        if (collision) {
            m_Metrics.collisions += 1;
        }
        return StatusCode::Ok;
    }

    StatusCode WeakSetModule::Remove(SetRecord &record, ObjectModule::Handle value, std::uint64_t hash,
                                     bool &outDeleted) {
        outDeleted = false;
        if (record.buckets.empty()) {
            return StatusCode::Ok;
        }
        std::uint32_t bucket = 0;
        bool collision = false;
        auto index = Locate(record, value, hash, bucket, collision);
        if (index == kInvalidIndex) {
            if (collision) {
                m_Metrics.collisions += 1;
            }
            return StatusCode::Ok;
        }
        auto &entry = record.entries[index];
        if (!entry.active) {
            return StatusCode::Ok;
        }
        entry.active = false;
        entry.hash = 0;
        entry.value = 0;
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

    void WeakSetModule::PruneInvalid(SetRecord &record) {
        if (!m_ObjectModule) {
            return;
        }
        for (std::uint32_t index = 0; index < record.entries.size(); ++index) {
            auto &entry = record.entries[index];
            if (!entry.active) {
                continue;
            }
            if (!m_ObjectModule->IsValid(entry.value)) {
                bool deleted = false;
                Remove(record, entry.value, entry.hash, deleted);
            }
        }
    }
}
