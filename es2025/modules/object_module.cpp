
#include "spectre/es2025/modules/object_module.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Object";
        constexpr std::string_view kSummary = "Object constructor, prototypes, and property descriptor semantics.";
        constexpr std::string_view kReference = "ECMA-262 Section 19.1";
        constexpr std::uint32_t kInitialBucketCount = 8;
        constexpr std::uint8_t kAttrEnumerable = 1u << 0;
        constexpr std::uint8_t kAttrConfigurable = 1u << 1;
        constexpr std::uint8_t kAttrWritable = 1u << 2;

        std::uint8_t EncodeAttributes(bool enumerable, bool configurable, bool writable) noexcept {
            std::uint8_t flags = 0;
            if (enumerable) {
                flags |= kAttrEnumerable;
            }
            if (configurable) {
                flags |= kAttrConfigurable;
            }
            if (writable) {
                flags |= kAttrWritable;
            }
            return flags;
        }

        bool IsEnumerable(std::uint8_t attributes) noexcept {
            return (attributes & kAttrEnumerable) != 0;
        }

        bool IsConfigurable(std::uint8_t attributes) noexcept {
            return (attributes & kAttrConfigurable) != 0;
        }

        bool IsWritable(std::uint8_t attributes) noexcept {
            return (attributes & kAttrWritable) != 0;
        }
    }

    struct ObjectModule::PropertySlot {
        PropertySlot() : hash(0), attributes(0), active(false), value(), key() {}
        std::uint64_t hash;
        std::uint8_t attributes;
        bool active;
        Value value;
        std::string key;
    };

    struct ObjectModule::ObjectRecord {
        ObjectRecord()
            : handle(0),
              slot(0),
              generation(0),
              label(),
              prototype(0),
              extensible(true),
              sealed(false),
              frozen(false),
              version(0),
              lastTouchFrame(0),
              activeProperties(0),
              properties(),
              buckets(),
              freeProperties() {}
        Handle handle;
        std::uint32_t slot;
        std::uint32_t generation;
        std::string label;
        Handle prototype;
        bool extensible;
        bool sealed;
        bool frozen;
        std::uint64_t version;
        std::uint64_t lastTouchFrame;
        std::uint32_t activeProperties;
        std::vector<PropertySlot> properties;
        std::vector<std::uint32_t> buckets;
        std::vector<std::uint32_t> freeProperties;
    };

    struct ObjectModule::SlotRecord {
        SlotRecord() : inUse(false), generation(0), record() {}
        bool inUse;
        std::uint32_t generation;
        ObjectRecord record;
    };

    ObjectModule::~ObjectModule() = default;

    ObjectModule::ObjectModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_Metrics{},
          m_Slots(),
          m_FreeList(),
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0) {}

    std::string_view ObjectModule::Name() const noexcept {
        return kName;
    }

    std::string_view ObjectModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view ObjectModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void ObjectModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Metrics = {};
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_Metrics.lastFrameTouched = 0;
        m_Slots.clear();
        m_FreeList.clear();
        m_CurrentFrame = 0;
        m_Initialized = true;
    }

    void ObjectModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void ObjectModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void ObjectModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode ObjectModule::Create(std::string_view label, Handle prototype, Handle &outHandle) {
        outHandle = 0;
        if (prototype != 0 && !IsValid(prototype)) {
            return StatusCode::InvalidArgument;
        }
        std::uint32_t slotIndex;
        if (!m_FreeList.empty()) {
            slotIndex = m_FreeList.back();
            m_FreeList.pop_back();
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
        auto &object = slot.record;
        object = ObjectRecord();
        object.slot = slotIndex;
        object.generation = slot.generation;
        object.handle = EncodeHandle(slotIndex, slot.generation);
        object.label.assign(label.begin(), label.end());
        object.prototype = prototype;
        object.extensible = true;
        object.sealed = false;
        object.frozen = false;
        object.version = 0;
        object.lastTouchFrame = m_CurrentFrame;
        object.activeProperties = 0;
        object.properties.clear();
        object.buckets.clear();
        object.freeProperties.clear();
        EnsureCapacity(object);
        Touch(object);
        TouchMetrics();
        outHandle = object.handle;
        m_Metrics.liveObjects += 1;
        m_Metrics.totalAllocations += 1;
        return StatusCode::Ok;
    }

    StatusCode ObjectModule::Clone(Handle source, std::string_view label, Handle &outHandle) {
        outHandle = 0;
        const auto *original = Find(source);
        if (!original) {
            return StatusCode::NotFound;
        }
        auto status = Create(label, original->prototype, outHandle);
        if (status != StatusCode::Ok) {
            return status;
        }
        auto *clone = FindMutable(outHandle);
        if (!clone) {
            return StatusCode::InternalError;
        }
        clone->extensible = true;
        clone->sealed = false;
        clone->frozen = false;
        clone->properties.clear();
        clone->freeProperties.clear();
        clone->activeProperties = 0;
        clone->buckets.clear();
        EnsureCapacity(*clone);
        for (const auto &slot : original->properties) {
            if (!slot.active) {
                continue;
            }
            PropertyDescriptor descriptor;
            descriptor.value = slot.value;
            descriptor.enumerable = IsEnumerable(slot.attributes);
            descriptor.configurable = IsConfigurable(slot.attributes);
            descriptor.writable = IsWritable(slot.attributes);
            InsertOrUpdate(*clone, slot.key, descriptor, true);
        }
        clone->extensible = original->extensible;
        clone->sealed = original->sealed;
        clone->frozen = original->frozen;
        if (clone->sealed) {
            clone->extensible = false;
            for (auto &slot : clone->properties) {
                if (!slot.active) {
                    continue;
                }
                slot.attributes &= static_cast<std::uint8_t>(~kAttrConfigurable);
            }
        }
        if (clone->frozen) {
            clone->extensible = false;
            clone->sealed = true;
            for (auto &slot : clone->properties) {
                if (!slot.active) {
                    continue;
                }
                slot.attributes &= static_cast<std::uint8_t>(~kAttrConfigurable);
                slot.attributes &= static_cast<std::uint8_t>(~kAttrWritable);
            }
        }
        Touch(*clone);
        TouchMetrics();
        return StatusCode::Ok;
    }

    StatusCode ObjectModule::Destroy(Handle handle) {
        auto *slot = FindMutableSlot(handle);
        if (!slot) {
            return StatusCode::NotFound;
        }
        slot->inUse = false;
        auto index = slot->record.slot;
        slot->record = ObjectRecord();
        m_FreeList.push_back(index);
        if (m_Metrics.liveObjects > 0) {
            m_Metrics.liveObjects -= 1;
        }
        m_Metrics.totalReleases += 1;
        TouchMetrics();
        return StatusCode::Ok;
    }
    StatusCode ObjectModule::Define(Handle handle, std::string_view key, const PropertyDescriptor &descriptor) {
        auto *object = FindMutable(handle);
        if (!object) {
            return StatusCode::NotFound;
        }
        return InsertOrUpdate(*object, key, descriptor, true);
    }

    StatusCode ObjectModule::Set(Handle handle, std::string_view key, const Value &value) {
        auto *object = FindMutable(handle);
        if (!object) {
            return StatusCode::NotFound;
        }
        bool allowNew = object->extensible && !object->sealed && !object->frozen;
        return UpdateValue(*object, key, value, allowNew);
    }

    StatusCode ObjectModule::Get(Handle handle, std::string_view key, Value &outValue) const {
        outValue.Reset();
        const auto *object = Find(handle);
        if (!object) {
            return StatusCode::NotFound;
        }
        auto hash = HashKey(key);
        std::uint32_t bucket = 0;
        bool collision = false;
        auto index = Locate(*object, key, hash, bucket, collision);
        if (index != kInvalidIndex) {
            outValue = object->properties[index].value;
            const_cast<ObjectModule *>(this)->m_Metrics.fastPathHits += 1;
            const_cast<ObjectModule *>(this)->TouchMetrics();
            return StatusCode::Ok;
        }
        Handle current = object->prototype;
        std::size_t depth = 0;
        while (current != 0 && depth < m_Slots.size() + 1) {
            const auto *proto = Find(current);
            if (!proto) {
                break;
            }
            bucket = 0;
            collision = false;
            index = Locate(*proto, key, hash, bucket, collision);
            if (index != kInvalidIndex) {
                outValue = proto->properties[index].value;
                const_cast<ObjectModule *>(this)->m_Metrics.prototypeHits += 1;
                const_cast<ObjectModule *>(this)->TouchMetrics();
                return StatusCode::Ok;
            }
            if (proto->prototype == current) {
                break;
            }
            current = proto->prototype;
            ++depth;
        }
        const_cast<ObjectModule *>(this)->m_Metrics.misses += 1;
        return StatusCode::NotFound;
    }

    StatusCode ObjectModule::Describe(Handle handle, std::string_view key, PropertyDescriptor &outDescriptor) const {
        outDescriptor.value.Reset();
        outDescriptor.enumerable = false;
        outDescriptor.configurable = false;
        outDescriptor.writable = false;
        const auto *object = Find(handle);
        if (!object) {
            return StatusCode::NotFound;
        }
        auto hash = HashKey(key);
        std::uint32_t bucket = 0;
        bool collision = false;
        auto index = Locate(*object, key, hash, bucket, collision);
        if (index == kInvalidIndex) {
            const_cast<ObjectModule *>(this)->m_Metrics.misses += 1;
            return StatusCode::NotFound;
        }
        const auto &slot = object->properties[index];
        if (!slot.active) {
            const_cast<ObjectModule *>(this)->m_Metrics.misses += 1;
            return StatusCode::NotFound;
        }
        outDescriptor.value = slot.value;
        outDescriptor.enumerable = IsEnumerable(slot.attributes);
        outDescriptor.configurable = IsConfigurable(slot.attributes);
        outDescriptor.writable = IsWritable(slot.attributes);
        const_cast<ObjectModule *>(this)->m_Metrics.fastPathHits += 1;
        const_cast<ObjectModule *>(this)->TouchMetrics();
        return StatusCode::Ok;
    }

    bool ObjectModule::Has(Handle handle, std::string_view key) const {
        Value value;
        return Get(handle, key, value) == StatusCode::Ok;
    }

    StatusCode ObjectModule::Delete(Handle handle, std::string_view key, bool &outDeleted) {
        auto *object = FindMutable(handle);
        if (!object) {
            outDeleted = false;
            return StatusCode::NotFound;
        }
        return RemoveProperty(*object, key, outDeleted);
    }

    StatusCode ObjectModule::OwnKeys(Handle handle, std::vector<std::string> &keys) const {
        keys.clear();
        const auto *object = Find(handle);
        if (!object) {
            return StatusCode::NotFound;
        }
        keys.reserve(object->activeProperties);
        for (const auto &slot : object->properties) {
            if (!slot.active) {
                continue;
            }
            if (!IsEnumerable(slot.attributes)) {
                continue;
            }
            keys.push_back(slot.key);
        }
        const_cast<ObjectModule *>(this)->TouchMetrics();
        return StatusCode::Ok;
    }

    StatusCode ObjectModule::SetPrototype(Handle handle, Handle prototype) {
        auto *object = FindMutable(handle);
        if (!object) {
            return StatusCode::NotFound;
        }
        if (prototype != 0 && !IsValid(prototype)) {
            return StatusCode::InvalidArgument;
        }
        if (!object->extensible && prototype != object->prototype) {
            return StatusCode::InvalidArgument;
        }
        Handle current = prototype;
        std::size_t guard = 0;
        while (current != 0 && guard < m_Slots.size() + 1) {
            if (current == handle) {
                return StatusCode::InvalidArgument;
            }
            const auto *ancestor = Find(current);
            if (!ancestor || ancestor->prototype == current) {
                break;
            }
            current = ancestor->prototype;
            ++guard;
        }
        object->prototype = prototype;
        Touch(*object);
        return StatusCode::Ok;
    }

    ObjectModule::Handle ObjectModule::Prototype(Handle handle) const {
        const auto *object = Find(handle);
        return object ? object->prototype : 0;
    }

    StatusCode ObjectModule::SetExtensible(Handle handle, bool extensible) {
        auto *object = FindMutable(handle);
        if (!object) {
            return StatusCode::NotFound;
        }
        if (extensible && (object->sealed || object->frozen)) {
            return StatusCode::InvalidArgument;
        }
        object->extensible = extensible;
        Touch(*object);
        return StatusCode::Ok;
    }

    bool ObjectModule::IsExtensible(Handle handle) const {
        const auto *object = Find(handle);
        return object ? object->extensible : false;
    }

    StatusCode ObjectModule::Seal(Handle handle) {
        auto *object = FindMutable(handle);
        if (!object) {
            return StatusCode::NotFound;
        }
        if (object->sealed) {
            return StatusCode::Ok;
        }
        object->extensible = false;
        object->sealed = true;
        for (auto &slot : object->properties) {
            if (!slot.active) {
                continue;
            }
            slot.attributes &= static_cast<std::uint8_t>(~kAttrConfigurable);
        }
        Touch(*object);
        m_Metrics.seals += 1;
        return StatusCode::Ok;
    }

    StatusCode ObjectModule::Freeze(Handle handle) {
        auto *object = FindMutable(handle);
        if (!object) {
            return StatusCode::NotFound;
        }
        if (object->frozen) {
            return StatusCode::Ok;
        }
        object->extensible = false;
        object->sealed = true;
        object->frozen = true;
        for (auto &slot : object->properties) {
            if (!slot.active) {
                continue;
            }
            slot.attributes &= static_cast<std::uint8_t>(~kAttrConfigurable);
            slot.attributes &= static_cast<std::uint8_t>(~kAttrWritable);
        }
        Touch(*object);
        m_Metrics.freezes += 1;
        return StatusCode::Ok;
    }

    bool ObjectModule::IsSealed(Handle handle) const {
        const auto *object = Find(handle);
        return object ? object->sealed : false;
    }

    bool ObjectModule::IsFrozen(Handle handle) const {
        const auto *object = Find(handle);
        return object ? object->frozen : false;
    }

    bool ObjectModule::IsValid(Handle handle) const noexcept {
        return FindSlot(handle) != nullptr;
    }

    const ObjectModule::Metrics &ObjectModule::GetMetrics() const noexcept {
        return m_Metrics;
    }
    ObjectModule::SlotRecord *ObjectModule::FindMutableSlot(Handle handle) noexcept {
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

    const ObjectModule::SlotRecord *ObjectModule::FindSlot(Handle handle) const noexcept {
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

    ObjectModule::ObjectRecord *ObjectModule::FindMutable(Handle handle) noexcept {
        auto *slot = FindMutableSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    const ObjectModule::ObjectRecord *ObjectModule::Find(Handle handle) const noexcept {
        const auto *slot = FindSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    std::uint64_t ObjectModule::HashKey(std::string_view key) noexcept {
        const std::uint64_t fnvOffset = 1469598103934665603ull;
        const std::uint64_t fnvPrime = 1099511628211ull;
        std::uint64_t hash = fnvOffset;
        for (unsigned char c : key) {
            hash ^= static_cast<std::uint64_t>(c);
            hash *= fnvPrime;
        }
        if (hash == 0) {
            hash = fnvOffset;
        }
        return hash;
    }

    std::uint32_t ObjectModule::NormalizeBuckets(std::uint32_t requested) noexcept {
        if (requested < kInitialBucketCount) {
            requested = kInitialBucketCount;
        }
        if ((requested & (requested - 1)) != 0) {
            std::uint32_t power = 1;
            while (power < requested && power < (1u << 30)) {
                power <<= 1;
            }
            requested = power;
        }
        return requested < kInitialBucketCount ? kInitialBucketCount : requested;
    }

    StatusCode ObjectModule::InsertOrUpdate(ObjectRecord &object, std::string_view key, const PropertyDescriptor &descriptor, bool allowNew) {
        EnsureCapacity(object);
        auto hash = HashKey(key);
        std::uint32_t bucket = 0;
        bool collision = false;
        auto index = Locate(object, key, hash, bucket, collision);
        if (index != kInvalidIndex) {
            auto &slot = object.properties[index];
            if (object.frozen) {
                return StatusCode::InvalidArgument;
            }
            if (!IsWritable(slot.attributes) && !slot.value.SameValueZero(descriptor.value)) {
                return StatusCode::InvalidArgument;
            }
            if (!IsConfigurable(slot.attributes)) {
                if (IsEnumerable(slot.attributes) != descriptor.enumerable) {
                    return StatusCode::InvalidArgument;
                }
                if (IsWritable(slot.attributes) != descriptor.writable) {
                    return StatusCode::InvalidArgument;
                }
            }
            slot.value = descriptor.value;
            slot.attributes = EncodeAttributes(descriptor.enumerable, descriptor.configurable, descriptor.writable);
            Touch(object);
            if (collision) {
                m_Metrics.collisions += 1;
            }
            m_Metrics.propertyUpdates += 1;
            return StatusCode::Ok;
        }
        if (object.sealed || object.frozen) {
            return StatusCode::InvalidArgument;
        }
        if (!object.extensible || !allowNew) {
            return StatusCode::InvalidArgument;
        }
        auto propertyIndex = AllocateProperty(object);
        auto &slot = object.properties[propertyIndex];
        slot.hash = hash;
        slot.key.assign(key.begin(), key.end());
        slot.value = descriptor.value;
        slot.attributes = EncodeAttributes(descriptor.enumerable, descriptor.configurable, descriptor.writable);
        slot.active = true;
        object.buckets[bucket] = propertyIndex;
        object.activeProperties += 1;
        Touch(object);
        if (collision) {
            m_Metrics.collisions += 1;
        }
        m_Metrics.propertyAdds += 1;
        return StatusCode::Ok;
    }

    StatusCode ObjectModule::UpdateValue(ObjectRecord &object, std::string_view key, const Value &value, bool allowNew) {
        EnsureCapacity(object);
        auto hash = HashKey(key);
        std::uint32_t bucket = 0;
        bool collision = false;
        auto index = Locate(object, key, hash, bucket, collision);
        if (index != kInvalidIndex) {
            auto &slot = object.properties[index];
            if (object.frozen) {
                return StatusCode::InvalidArgument;
            }
            if (!IsWritable(slot.attributes) && !slot.value.SameValueZero(value)) {
                return StatusCode::InvalidArgument;
            }
            slot.value = value;
            Touch(object);
            if (collision) {
                m_Metrics.collisions += 1;
            }
            m_Metrics.propertyUpdates += 1;
            return StatusCode::Ok;
        }
        if (!allowNew || object.sealed || object.frozen || !object.extensible) {
            return StatusCode::InvalidArgument;
        }
        PropertyDescriptor descriptor;
        descriptor.value = value;
        descriptor.enumerable = true;
        descriptor.configurable = true;
        descriptor.writable = true;
        return InsertOrUpdate(object, key, descriptor, true);
    }

    StatusCode ObjectModule::RemoveProperty(ObjectRecord &object, std::string_view key, bool &outDeleted) {
        outDeleted = false;
        if (object.buckets.empty()) {
            return StatusCode::Ok;
        }
        auto hash = HashKey(key);
        std::uint32_t bucket = 0;
        bool collision = false;
        auto index = Locate(object, key, hash, bucket, collision);
        if (index == kInvalidIndex) {
            return StatusCode::Ok;
        }
        auto &slot = object.properties[index];
        if (!IsConfigurable(slot.attributes) || object.frozen) {
            return StatusCode::InvalidArgument;
        }
        object.buckets[bucket] = kDeletedIndex;
        slot.active = false;
        slot.hash = 0;
        slot.key.clear();
        slot.value.Reset();
        slot.attributes = 0;
        object.freeProperties.push_back(index);
        if (object.activeProperties > 0) {
            object.activeProperties -= 1;
        }
        outDeleted = true;
        Touch(object);
        if (collision) {
            m_Metrics.collisions += 1;
        }
        m_Metrics.propertyRemovals += 1;
        return StatusCode::Ok;
    }

    std::uint32_t ObjectModule::Locate(const ObjectRecord &object, std::string_view key, std::uint64_t hash, std::uint32_t &bucket, bool &collision) const {
        if (object.buckets.empty()) {
            bucket = 0;
            collision = false;
            return kInvalidIndex;
        }
        auto mask = static_cast<std::uint32_t>(object.buckets.size() - 1);
        auto index = static_cast<std::uint32_t>(hash) & mask;
        std::uint32_t firstDeleted = kInvalidIndex;
        std::uint32_t probes = 0;
        collision = false;
        while (true) {
            auto entry = object.buckets[index];
            if (entry == kInvalidIndex) {
                bucket = firstDeleted != kInvalidIndex ? firstDeleted : index;
                collision = probes != 0;
                return kInvalidIndex;
            }
            if (entry == kDeletedIndex) {
                if (firstDeleted == kInvalidIndex) {
                    firstDeleted = index;
                }
            } else {
                const auto &slot = object.properties[entry];
                if (slot.active && slot.hash == hash && slot.key == key) {
                    bucket = index;
                    collision = probes != 0;
                    return entry;
                }
            }
            index = (index + 1) & mask;
            ++probes;
            if (probes >= object.buckets.size()) {
                bucket = firstDeleted != kInvalidIndex ? firstDeleted : index;
                collision = true;
                return kInvalidIndex;
            }
        }
    }

    std::uint32_t ObjectModule::AllocateProperty(ObjectRecord &object) {
        if (!object.freeProperties.empty()) {
            auto index = object.freeProperties.back();
            object.freeProperties.pop_back();
            auto &slot = object.properties[index];
            slot.active = true;
            slot.hash = 0;
            slot.key.clear();
            slot.value.Reset();
            slot.attributes = 0;
            return index;
        }
        PropertySlot slot;
        slot.active = true;
        object.properties.push_back(slot);
        return static_cast<std::uint32_t>(object.properties.size() - 1);
    }
    void ObjectModule::EnsureCapacity(ObjectRecord &object) {
        if (object.buckets.empty()) {
            Rehash(object, kInitialBucketCount);
            return;
        }
        auto capacity = static_cast<std::uint32_t>(object.buckets.size());
        auto limit = (capacity * 3u) / 5u;
        if (limit < 1) {
            limit = 1;
        }
        if (object.activeProperties + 1 > limit) {
            Rehash(object, capacity * 2);
        }
    }

    void ObjectModule::Rehash(ObjectRecord &object, std::uint32_t newBucketCount) {
        auto normalized = NormalizeBuckets(newBucketCount);
        std::vector<std::uint32_t> buckets(normalized, kInvalidIndex);
        auto mask = normalized - 1;
        for (std::uint32_t index = 0; index < object.properties.size(); ++index) {
            auto &slot = object.properties[index];
            if (!slot.active) {
                continue;
            }
            auto bucket = static_cast<std::uint32_t>(slot.hash) & mask;
            while (true) {
                if (buckets[bucket] == kInvalidIndex) {
                    buckets[bucket] = index;
                    break;
                }
                bucket = (bucket + 1) & mask;
            }
        }
        object.buckets.swap(buckets);
        m_Metrics.rehashes += 1;
    }

    void ObjectModule::Touch(ObjectRecord &object) noexcept {
        object.version += 1;
        object.lastTouchFrame = m_CurrentFrame;
        TouchMetrics();
    }

    void ObjectModule::TouchMetrics() noexcept {
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    ObjectModule::Handle ObjectModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::uint32_t ObjectModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t ObjectModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }
}



