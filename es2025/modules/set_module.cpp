#include "spectre/es2025/modules/set_module.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Set";
        constexpr std::string_view kSummary = "Set collection for unique membership tracking.";
        constexpr std::string_view kReference = "ECMA-262 Section 24.2";
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

    struct SetModule::Entry {
        Entry()
            : hash(0),
              active(false),
              value(),
              orderPrev(kInvalidIndex),
              orderNext(kInvalidIndex) {
        }

        std::uint64_t hash;
        bool active;
        Value value;
        std::uint32_t orderPrev;
        std::uint32_t orderNext;
    };

    struct SetModule::SetRecord {
        SetRecord()
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

    struct SetModule::SlotRecord {
        SlotRecord() : inUse(false), generation(0), record() {
        }

        bool inUse;
        std::uint32_t generation;
        SetRecord record;
    };

    SetModule::Value::Value() : m_Scalar(), m_Kind(Kind::Undefined), m_String() {
    }

    SetModule::Value::Value(const Value &other) : m_Scalar(), m_Kind(Kind::Undefined), m_String() {
        Assign(other);
    }

    SetModule::Value::Value(Value &&other) noexcept : m_Scalar(), m_Kind(Kind::Undefined), m_String() {
        Assign(std::move(other));
    }

    SetModule::Value &SetModule::Value::operator=(const Value &other) {
        if (this != &other) {
            Assign(other);
        }
        return *this;
    }

    SetModule::Value &SetModule::Value::operator=(Value &&other) noexcept {
        if (this != &other) {
            Assign(std::move(other));
        }
        return *this;
    }

    SetModule::Value::~Value() = default;

    SetModule::Value SetModule::Value::Undefined() {
        Value v;
        v.Reset();
        return v;
    }

    SetModule::Value SetModule::Value::FromBoolean(bool v) {
        Value value;
        value.m_Kind = Kind::Boolean;
        value.m_Scalar.booleanValue = v;
        value.m_String.clear();
        return value;
    }

    SetModule::Value SetModule::Value::FromInt(std::int64_t v) {
        Value value;
        value.m_Kind = Kind::Int64;
        value.m_Scalar.intValue = v;
        value.m_String.clear();
        return value;
    }

    SetModule::Value SetModule::Value::FromDouble(double v) {
        Value value;
        value.m_Kind = Kind::Double;
        value.m_Scalar.doubleValue = v;
        value.m_String.clear();
        return value;
    }

    SetModule::Value SetModule::Value::FromHandle(Handle v) {
        Value value;
        value.m_Kind = Kind::Handle;
        value.m_Scalar.handleValue = v;
        value.m_String.clear();
        return value;
    }

    SetModule::Value SetModule::Value::FromString(std::string_view text) {
        Value value;
        value.m_Kind = Kind::String;
        value.m_Scalar.handleValue = 0;
        value.m_String.assign(text.data(), text.size());
        return value;
    }

    bool SetModule::Value::IsUndefined() const noexcept {
        return m_Kind == Kind::Undefined;
    }

    bool SetModule::Value::IsBoolean() const noexcept {
        return m_Kind == Kind::Boolean;
    }

    bool SetModule::Value::IsInt() const noexcept {
        return m_Kind == Kind::Int64;
    }

    bool SetModule::Value::IsDouble() const noexcept {
        return m_Kind == Kind::Double;
    }

    bool SetModule::Value::IsString() const noexcept {
        return m_Kind == Kind::String;
    }

    bool SetModule::Value::IsHandle() const noexcept {
        return m_Kind == Kind::Handle;
    }

    bool SetModule::Value::Boolean() const noexcept {
        return m_Scalar.booleanValue;
    }

    std::int64_t SetModule::Value::Int() const noexcept {
        return m_Scalar.intValue;
    }

    double SetModule::Value::Double() const noexcept {
        return m_Scalar.doubleValue;
    }

    SetModule::Handle SetModule::Value::HandleValue() const noexcept {
        return m_Scalar.handleValue;
    }

    std::string_view SetModule::Value::String() const noexcept {
        return m_String;
    }

    void SetModule::Value::Assign(const Value &other) {
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

    void SetModule::Value::Assign(Value &&other) noexcept {
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

    void SetModule::Value::Reset() noexcept {
        m_Kind = Kind::Undefined;
        m_Scalar.handleValue = 0;
        m_String.clear();
    }

    bool SetModule::Value::Equals(const Value &other) const noexcept {
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

    SetModule::SetModule()
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

    SetModule::~SetModule() = default;

    std::string_view SetModule::Name() const noexcept {
        return kName;
    }

    std::string_view SetModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view SetModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void SetModule::Initialize(const ModuleInitContext &context) {
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

    void SetModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void SetModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void SetModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode SetModule::Create(std::string_view label, Handle &outHandle) {
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
        m_Metrics.liveSets += 1;
        m_Metrics.totalAllocations += 1;
        return StatusCode::Ok;
    }

    StatusCode SetModule::Destroy(Handle handle) {
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

    StatusCode SetModule::Clear(Handle handle) {
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
            entry.value.Reset();
            entry.orderPrev = kInvalidIndex;
            entry.orderNext = kInvalidIndex;
            record->freeEntries.push_back(index);
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

    StatusCode SetModule::Add(Handle handle, const Value &value) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        m_Metrics.addOps += 1;
        EnsureCapacity(*record);
        auto hash = HashValue(value);
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

    bool SetModule::Has(Handle handle, const Value &value) const {
        auto *self = const_cast<SetModule *>(this);
        self->m_Metrics.hasOps += 1;
        const auto *record = Find(handle);
        if (!record) {
            self->m_Metrics.misses += 1;
            return false;
        }
        auto hash = HashValue(value);
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

    StatusCode SetModule::Delete(Handle handle, const Value &value, bool &outDeleted) {
        outDeleted = false;
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        m_Metrics.deleteOps += 1;
        auto hash = HashValue(value);
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

    std::uint32_t SetModule::Size(Handle handle) const {
        const auto *record = Find(handle);
        return record ? record->size : 0;
    }

    StatusCode SetModule::Values(Handle handle, std::vector<Value> &values) const {
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
        const_cast<SetModule *>(this)->m_Metrics.iterations += 1;
        const_cast<SetModule *>(this)->TouchMetrics();
        return StatusCode::Ok;
    }

    StatusCode SetModule::Entries(Handle handle, std::vector<std::pair<Value, Value> > &entries) const {
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
                entries.emplace_back(entry.value, entry.value);
            }
            index = entry.orderNext;
        }
        const_cast<SetModule *>(this)->m_Metrics.iterations += 1;
        const_cast<SetModule *>(this)->TouchMetrics();
        return StatusCode::Ok;
    }

    const SetModule::Metrics &SetModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    SetModule::SlotRecord *SetModule::FindMutableSlot(Handle handle) noexcept {
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

    const SetModule::SlotRecord *SetModule::FindSlot(Handle handle) const noexcept {
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

    SetModule::SetRecord *SetModule::FindMutable(Handle handle) noexcept {
        auto *slot = FindMutableSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    const SetModule::SetRecord *SetModule::Find(Handle handle) const noexcept {
        const auto *slot = FindSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    SetModule::Handle SetModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::uint32_t SetModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t SetModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }

    std::uint64_t SetModule::HashValue(const Value &value) noexcept {
        switch (value.m_Kind) {
            case Value::Kind::Undefined:
                return 0x9e3779b97f4a7c15ull;
            case Value::Kind::Boolean:
                return value.m_Scalar.booleanValue ? 0x51ed270b27b4f3cfull : 0x7f4a7c159e3779b9ull;
            case Value::Kind::Int64:
                return HashBytes(&value.m_Scalar.intValue, sizeof(std::int64_t));
            case Value::Kind::Double:
                return HashDouble(value.m_Scalar.doubleValue);
            case Value::Kind::String:
                return HashString(value.m_String);
            case Value::Kind::Handle:
                return HashBytes(&value.m_Scalar.handleValue, sizeof(Handle));
        }
        return 0x9e3779b97f4a7c15ull;
    }

    void SetModule::Touch(SetRecord &record) noexcept {
        record.version += 1;
        record.lastTouchFrame = m_CurrentFrame;
        TouchMetrics();
    }

    void SetModule::TouchMetrics() noexcept {
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void SetModule::EnsureCapacity(SetRecord &record) {
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

    void SetModule::Rehash(SetRecord &record, std::uint32_t newBucketCount) {
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

    std::uint32_t SetModule::Locate(const SetRecord &record, const Value &value, std::uint64_t hash,
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
                if (entry.active && entry.hash == hash && entry.value.Equals(value)) {
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

    std::uint32_t SetModule::AllocateEntry(SetRecord &record) {
        if (!record.freeEntries.empty()) {
            auto index = record.freeEntries.back();
            record.freeEntries.pop_back();
            auto &entry = record.entries[index];
            entry.active = true;
            entry.hash = 0;
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

    StatusCode SetModule::Insert(SetRecord &record, const Value &value, std::uint64_t hash) {
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
        entry.value.Assign(value);
        entry.orderPrev = kInvalidIndex;
        entry.orderNext = kInvalidIndex;
        if (record.buckets.empty()) {
            record.buckets.resize(kInitialBucketCount, kInvalidIndex);
        }
        record.buckets[bucket] = entryIndex;
        record.size += 1;
        LinkTail(record, entryIndex);
        Touch(record);
        if (collision) {
            m_Metrics.collisions += 1;
        }
        return StatusCode::Ok;
    }

    StatusCode SetModule::Remove(SetRecord &record, const Value &value, std::uint64_t hash, bool &outDeleted) {
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
        entry.value.Reset();
        Unlink(record, index);
        entry.orderPrev = kInvalidIndex;
        entry.orderNext = kInvalidIndex;
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

    void SetModule::LinkTail(SetRecord &record, std::uint32_t entryIndex) {
        auto &entry = record.entries[entryIndex];
        entry.orderPrev = record.tail;
        entry.orderNext = kInvalidIndex;
        if (record.tail != kInvalidIndex) {
            record.entries[record.tail].orderNext = entryIndex;
        } else {
            record.head = entryIndex;
        }
        record.tail = entryIndex;
    }

    void SetModule::Unlink(SetRecord &record, std::uint32_t entryIndex) {
        auto &entry = record.entries[entryIndex];
        if (entry.orderPrev != kInvalidIndex) {
            record.entries[entry.orderPrev].orderNext = entry.orderNext;
        } else {
            record.head = entry.orderNext;
        }
        if (entry.orderNext != kInvalidIndex) {
            record.entries[entry.orderNext].orderPrev = entry.orderPrev;
        } else {
            record.tail = entry.orderPrev;
        }
    }
}
