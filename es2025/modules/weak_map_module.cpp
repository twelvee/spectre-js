#include "spectre/es2025/modules/weak_map_module.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "spectre/runtime.h"
#include "spectre/es2025/environment.h"
#include "spectre/es2025/modules/object_module.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "WeakMap";
        constexpr std::string_view kSummary = "WeakMap keyed collection with garbage collected keys.";
        constexpr std::string_view kReference = "ECMA-262 Section 24.3";
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

        std::uint64_t HashString(std::string_view text) noexcept {
            return HashBytes(text.data(), text.size());
        }

        std::uint64_t HashDouble(double value) noexcept {
            std::uint64_t bits;
            std::memcpy(&bits, &value, sizeof(double));
            return HashBytes(&bits, sizeof(bits));
        }
    }

    struct WeakMapModule::Entry {
        Entry() : hash(0), active(false), key(0), value() {
        }

        std::uint64_t hash;
        bool active;
        ObjectModule::Handle key;
        Value value;
    };

    struct WeakMapModule::MapRecord {
        MapRecord()
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

    struct WeakMapModule::SlotRecord {
        SlotRecord() : inUse(false), generation(0), record() {
        }

        bool inUse;
        std::uint32_t generation;
        MapRecord record;
    };

    WeakMapModule::Value::Value() : m_Scalar(), m_Kind(Kind::Undefined), m_String() {
    }

    WeakMapModule::Value::Value(const Value &other) : m_Scalar(), m_Kind(Kind::Undefined), m_String() {
        Assign(other);
    }

    WeakMapModule::Value::Value(Value &&other) noexcept : m_Scalar(), m_Kind(Kind::Undefined), m_String() {
        Assign(std::move(other));
    }

    WeakMapModule::Value &WeakMapModule::Value::operator=(const Value &other) {
        if (this != &other) {
            Assign(other);
        }
        return *this;
    }

    WeakMapModule::Value &WeakMapModule::Value::operator=(Value &&other) noexcept {
        if (this != &other) {
            Assign(std::move(other));
        }
        return *this;
    }

    WeakMapModule::Value::~Value() = default;

    WeakMapModule::Value WeakMapModule::Value::Undefined() {
        Value v;
        v.Reset();
        return v;
    }

    WeakMapModule::Value WeakMapModule::Value::FromBoolean(bool v) {
        Value value;
        value.m_Kind = Kind::Boolean;
        value.m_Scalar.booleanValue = v;
        value.m_String.clear();
        return value;
    }

    WeakMapModule::Value WeakMapModule::Value::FromInt(std::int64_t v) {
        Value value;
        value.m_Kind = Kind::Int64;
        value.m_Scalar.intValue = v;
        value.m_String.clear();
        return value;
    }

    WeakMapModule::Value WeakMapModule::Value::FromDouble(double v) {
        Value value;
        value.m_Kind = Kind::Double;
        value.m_Scalar.doubleValue = v;
        value.m_String.clear();
        return value;
    }

    WeakMapModule::Value WeakMapModule::Value::FromHandle(Handle v) {
        Value value;
        value.m_Kind = Kind::Handle;
        value.m_Scalar.handleValue = v;
        value.m_String.clear();
        return value;
    }

    WeakMapModule::Value WeakMapModule::Value::FromString(std::string_view text) {
        Value value;
        value.m_Kind = Kind::String;
        value.m_Scalar.handleValue = 0;
        value.m_String.assign(text.data(), text.size());
        return value;
    }

    bool WeakMapModule::Value::IsUndefined() const noexcept {
        return m_Kind == Kind::Undefined;
    }

    bool WeakMapModule::Value::IsBoolean() const noexcept {
        return m_Kind == Kind::Boolean;
    }

    bool WeakMapModule::Value::IsInt() const noexcept {
        return m_Kind == Kind::Int64;
    }

    bool WeakMapModule::Value::IsDouble() const noexcept {
        return m_Kind == Kind::Double;
    }

    bool WeakMapModule::Value::IsString() const noexcept {
        return m_Kind == Kind::String;
    }

    bool WeakMapModule::Value::IsHandle() const noexcept {
        return m_Kind == Kind::Handle;
    }

    bool WeakMapModule::Value::Boolean() const noexcept {
        return m_Scalar.booleanValue;
    }

    std::int64_t WeakMapModule::Value::Int() const noexcept {
        return m_Scalar.intValue;
    }

    double WeakMapModule::Value::Double() const noexcept {
        return m_Scalar.doubleValue;
    }

    WeakMapModule::Handle WeakMapModule::Value::HandleValue() const noexcept {
        return m_Scalar.handleValue;
    }

    std::string_view WeakMapModule::Value::String() const noexcept {
        return m_String;
    }

    void WeakMapModule::Value::Assign(const Value &other) {
        m_Kind = other.m_Kind;
        switch (other.m_Kind) {
            case Kind::Undefined:
                m_Scalar.handleValue = 0;
                m_String.clear();
                break;
            case Kind::Boolean:
                m_Scalar.booleanValue = other.m_Scalar.booleanValue;
                m_String.clear();
                break;
            case Kind::Int64:
                m_Scalar.intValue = other.m_Scalar.intValue;
                m_String.clear();
                break;
            case Kind::Double:
                m_Scalar.doubleValue = other.m_Scalar.doubleValue;
                m_String.clear();
                break;
            case Kind::Handle:
                m_Scalar.handleValue = other.m_Scalar.handleValue;
                m_String.clear();
                break;
            case Kind::String:
                m_Scalar.handleValue = 0;
                m_String = other.m_String;
                break;
        }
    }

    void WeakMapModule::Value::Assign(Value &&other) noexcept {
        m_Kind = other.m_Kind;
        switch (other.m_Kind) {
            case Kind::Undefined:
                m_Scalar.handleValue = 0;
                m_String.clear();
                break;
            case Kind::Boolean:
                m_Scalar.booleanValue = other.m_Scalar.booleanValue;
                m_String.clear();
                break;
            case Kind::Int64:
                m_Scalar.intValue = other.m_Scalar.intValue;
                m_String.clear();
                break;
            case Kind::Double:
                m_Scalar.doubleValue = other.m_Scalar.doubleValue;
                m_String.clear();
                break;
            case Kind::Handle:
                m_Scalar.handleValue = other.m_Scalar.handleValue;
                m_String.clear();
                break;
            case Kind::String:
                m_Scalar.handleValue = 0;
                m_String = std::move(other.m_String);
                break;
        }
        other.m_Kind = Kind::Undefined;
        other.m_Scalar.handleValue = 0;
        other.m_String.clear();
    }

    void WeakMapModule::Value::Reset() noexcept {
        m_Kind = Kind::Undefined;
        m_Scalar.handleValue = 0;
        m_String.clear();
    }

    bool WeakMapModule::Value::Equals(const Value &other) const noexcept {
        if (m_Kind != other.m_Kind) {
            return false;
        }
        switch (m_Kind) {
            case Kind::Undefined:
                return true;
            case Kind::Boolean:
                return m_Scalar.booleanValue == other.m_Scalar.booleanValue;
            case Kind::Int64:
                return m_Scalar.intValue == other.m_Scalar.intValue;
            case Kind::Double: {
                std::uint64_t a;
                std::uint64_t b;
                std::memcpy(&a, &m_Scalar.doubleValue, sizeof(double));
                std::memcpy(&b, &other.m_Scalar.doubleValue, sizeof(double));
                return a == b;
            }
            case Kind::Handle:
                return m_Scalar.handleValue == other.m_Scalar.handleValue;
            case Kind::String:
                return m_String == other.m_String;
        }
        return false;
    }

    WeakMapModule::WeakMapModule()
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

    WeakMapModule::~WeakMapModule() = default;

    std::string_view WeakMapModule::Name() const noexcept {
        return kName;
    }

    std::string_view WeakMapModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view WeakMapModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void WeakMapModule::Initialize(const ModuleInitContext &context) {
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

    void WeakMapModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void WeakMapModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void WeakMapModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode WeakMapModule::Create(std::string_view label, Handle &outHandle) {
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

    StatusCode WeakMapModule::Destroy(Handle handle) {
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

    StatusCode WeakMapModule::Clear(Handle handle) {
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
            entry.key = 0;
            entry.value.Reset();
            record->freeEntries.push_back(static_cast<std::uint32_t>(&entry - record->entries.data()));
        }
        record->size = 0;
        std::fill(record->buckets.begin(), record->buckets.end(), kInvalidIndex);
        Touch(*record);
        TouchMetrics();
        m_Metrics.clears += 1;
        return StatusCode::Ok;
    }

    StatusCode WeakMapModule::Set(Handle handle, ObjectModule::Handle key, const Value &value) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!m_ObjectModule || !m_ObjectModule->IsValid(key)) {
            return StatusCode::InvalidArgument;
        }
        m_Metrics.setOps += 1;
        EnsureCapacity(*record);
        auto hash = HashKey(key);
        std::uint32_t bucket = 0;
        bool collision = false;
        auto existing = Locate(*record, key, hash, bucket, collision);
        if (existing != kInvalidIndex) {
            record->entries[existing].value.Assign(value);
            Touch(*record);
            if (collision) {
                m_Metrics.collisions += 1;
            }
            return StatusCode::Ok;
        }
        auto status = Insert(*record, key, value, hash, true);
        if (collision && status == StatusCode::Ok) {
            m_Metrics.collisions += 1;
        }
        return status;
    }

    StatusCode WeakMapModule::Get(Handle handle, ObjectModule::Handle key, Value &outValue) const {
        outValue.Reset();
        const auto *record = Find(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (!m_ObjectModule || !m_ObjectModule->IsValid(key)) {
            const_cast<WeakMapModule *>(this)->m_Metrics.misses += 1;
            return StatusCode::NotFound;
        }
        const_cast<WeakMapModule *>(this)->m_Metrics.getOps += 1;
        auto hash = HashKey(key);
        std::uint32_t bucket = 0;
        bool collision = false;
        auto index = Locate(*record, key, hash, bucket, collision);
        if (index == kInvalidIndex) {
            const_cast<WeakMapModule *>(this)->m_Metrics.misses += 1;
            return StatusCode::NotFound;
        }
        outValue = record->entries[index].value;
        const_cast<WeakMapModule *>(this)->m_Metrics.hits += 1;
        const_cast<WeakMapModule *>(this)->TouchMetrics();
        return StatusCode::Ok;
    }

    bool WeakMapModule::Has(Handle handle, ObjectModule::Handle key) const {
        Value value;
        return Get(handle, key, value) == StatusCode::Ok;
    }

    StatusCode WeakMapModule::Delete(Handle handle, ObjectModule::Handle key, bool &outDeleted) {
        outDeleted = false;
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        m_Metrics.deleteOps += 1;
        auto hash = HashKey(key);
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

    StatusCode WeakMapModule::Compact(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        PruneInvalid(*record);
        m_Metrics.compactions += 1;
        Touch(*record);
        TouchMetrics();
        return StatusCode::Ok;
    }

    std::uint32_t WeakMapModule::Size(Handle handle) const {
        const auto *record = Find(handle);
        return record ? record->size : 0;
    }

    const WeakMapModule::Metrics &WeakMapModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    WeakMapModule::SlotRecord *WeakMapModule::FindMutableSlot(Handle handle) noexcept {
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

    const WeakMapModule::SlotRecord *WeakMapModule::FindSlot(Handle handle) const noexcept {
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

    WeakMapModule::MapRecord *WeakMapModule::FindMutable(Handle handle) noexcept {
        auto *slot = FindMutableSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    const WeakMapModule::MapRecord *WeakMapModule::Find(Handle handle) const noexcept {
        const auto *slot = FindSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    WeakMapModule::Handle WeakMapModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::uint32_t WeakMapModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t WeakMapModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }

    std::uint64_t WeakMapModule::HashKey(ObjectModule::Handle key) noexcept {
        return HashBytes(&key, sizeof(ObjectModule::Handle));
    }

    void WeakMapModule::Touch(MapRecord &record) noexcept {
        record.version += 1;
        record.lastTouchFrame = m_CurrentFrame;
        TouchMetrics();
    }

    void WeakMapModule::TouchMetrics() noexcept {
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void WeakMapModule::EnsureCapacity(MapRecord &record) {
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

    void WeakMapModule::Rehash(MapRecord &record, std::uint32_t newBucketCount) {
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
    }

    std::uint32_t WeakMapModule::Locate(const MapRecord &record, ObjectModule::Handle key, std::uint64_t hash,
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
                if (entry.active && entry.hash == hash && entry.key == key) {
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

    std::uint32_t WeakMapModule::AllocateEntry(MapRecord &record) {
        if (!record.freeEntries.empty()) {
            auto index = record.freeEntries.back();
            record.freeEntries.pop_back();
            auto &entry = record.entries[index];
            entry.active = true;
            entry.hash = 0;
            entry.key = 0;
            entry.value.Reset();
            return index;
        }
        Entry entry;
        entry.active = true;
        record.entries.push_back(entry);
        return static_cast<std::uint32_t>(record.entries.size() - 1);
    }

    StatusCode WeakMapModule::Insert(MapRecord &record, ObjectModule::Handle key, const Value &value,
                                     std::uint64_t hash, bool allowReplace) {
        std::uint32_t bucket = 0;
        bool collision = false;
        auto existing = Locate(record, key, hash, bucket, collision);
        if (existing != kInvalidIndex) {
            if (!allowReplace) {
                return StatusCode::AlreadyExists;
            }
            record.entries[existing].value.Assign(value);
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
        entry.value.Assign(value);
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

    StatusCode WeakMapModule::Remove(MapRecord &record, ObjectModule::Handle key, std::uint64_t hash,
                                     bool &outDeleted) {
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
        entry.key = 0;
        entry.value.Reset();
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

    void WeakMapModule::PruneInvalid(MapRecord &record) {
        if (!m_ObjectModule) {
            return;
        }
        for (std::uint32_t index = 0; index < record.entries.size(); ++index) {
            auto &entry = record.entries[index];
            if (!entry.active) {
                continue;
            }
            if (!m_ObjectModule->IsValid(entry.key)) {
                bool deleted = false;
                Remove(record, entry.key, entry.hash, deleted);
            }
        }
    }
}