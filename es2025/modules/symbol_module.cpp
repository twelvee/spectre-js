#include "spectre/es2025/modules/symbol_module.h"

#include <algorithm>
#include <array>
#include <utility>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Symbol";
        constexpr std::string_view kSummary = "Symbol primitives, registries, and well-known symbol plumbing.";
        constexpr std::string_view kReference = "ECMA-262 Section 19.4";
        constexpr std::uint32_t kDefaultBuckets = 16;
        constexpr std::uint64_t kHashSeed = 1469598103934665603ull;
        constexpr std::uint64_t kHashPrime = 1099511628211ull;
        constexpr std::uint64_t kHashMixerA = 0xff51afd7ed558ccdull;
        constexpr std::uint64_t kHashMixerB = 0xc4ceb9fe1a85ec53ull;
        constexpr std::array<std::string_view, static_cast<std::size_t>(SymbolModule::WellKnown::Count)> kWellKnownNames = {
            "Symbol.asyncIterator",
            "Symbol.hasInstance",
            "Symbol.isConcatSpreadable",
            "Symbol.iterator",
            "Symbol.match",
            "Symbol.matchAll",
            "Symbol.replace",
            "Symbol.search",
            "Symbol.species",
            "Symbol.split",
            "Symbol.toPrimitive",
            "Symbol.toStringTag",
            "Symbol.unscopables",
            "Symbol.dispose",
            "Symbol.asyncDispose",
            "Symbol.metadata"
        };
    }

    SymbolModule::Metrics::Metrics() noexcept
        : totalSymbols(0),
          liveSymbols(0),
          globalSymbols(0),
          localSymbols(0),
          wellKnownSymbols(0),
          recycledSlots(0),
          registryLookups(0),
          registryHits(0),
          registryMisses(0),
          collisions(0),
          lastFrameTouched(0),
          gpuOptimized(false) {
    }

    SymbolModule::SymbolModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_NextSequence(1),
          m_Slots(),
          m_FreeSlots(),
          m_GlobalBuckets(),
          m_WellKnown(),
          m_Metrics(),
          m_GlobalCount(0),
          m_LocalCount(0) {
        m_WellKnown.fill(0);
    }

    std::string_view SymbolModule::Name() const noexcept {
        return kName;
    }

    std::string_view SymbolModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view SymbolModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void SymbolModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        Reset();
        RehashGlobals(kDefaultBuckets);
        for (std::size_t i = 0; i < kWellKnownNames.size(); ++i) {
            Handle handle = 0;
            std::uint32_t slot = kInvalidIndex;
            auto status = CreateInternal(kWellKnownNames[i], {}, false, true, HashKey(kWellKnownNames[i]), handle, slot);
            if (status != StatusCode::Ok) {
                continue;
            }
            m_WellKnown[i] = handle;
        }
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_Initialized = true;
    }

    void SymbolModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void SymbolModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void SymbolModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode SymbolModule::Create(std::string_view description, Handle &outHandle) {
        outHandle = 0;
        if (!m_Initialized) {
            return StatusCode::InternalError;
        }
        std::uint32_t slot = kInvalidIndex;
        return CreateInternal(description, {}, false, false, HashKey(description), outHandle, slot);
    }

    StatusCode SymbolModule::CreateUnique(Handle &outHandle) {
        return Create({}, outHandle);
    }

    StatusCode SymbolModule::CreateGlobal(std::string_view key, Handle &outHandle) {
        outHandle = 0;
        if (!m_Initialized) {
            return StatusCode::InternalError;
        }
        EnsureGlobalCapacity();
        std::uint64_t hash = HashKey(key);
        std::uint32_t bucket = 0;
        bool collision = false;
        auto existing = LocateGlobal(key, hash, bucket, collision);
        m_Metrics.registryLookups += 1;
        if (existing != kInvalidIndex) {
            auto &slot = m_Slots[existing];
            Touch(slot);
            outHandle = slot.entry.handle;
            m_Metrics.registryHits += 1;
            ObserveGlobal(collision);
            return StatusCode::Ok;
        }
        m_Metrics.registryMisses += 1;
        Handle handle = 0;
        std::uint32_t slot = kInvalidIndex;
        auto status = CreateInternal(key, key, true, false, hash, handle, slot);
        if (status != StatusCode::Ok) {
            return status;
        }
        if (m_GlobalBuckets.empty()) {
            RehashGlobals(static_cast<std::uint32_t>(m_GlobalCount + 1));
        }
        auto mask = static_cast<std::uint32_t>(m_GlobalBuckets.size() - 1);
        auto index = static_cast<std::uint32_t>(hash) & mask;
        bool insertCollision = false;
        for (std::uint32_t probes = 0; probes < m_GlobalBuckets.size(); ++probes) {
            if (m_GlobalBuckets[index] == kInvalidIndex) {
                m_GlobalBuckets[index] = slot;
                break;
            }
            index = (index + 1) & mask;
            insertCollision = true;
        }
        ObserveGlobal(insertCollision);
        outHandle = handle;
        return StatusCode::Ok;
    }

    StatusCode SymbolModule::KeyFor(Handle handle, std::string &outKey) const {
        outKey.clear();
        const auto *slot = Find(handle);
        if (!slot || !slot->entry.global) {
            return StatusCode::NotFound;
        }
        outKey.assign(slot->entry.key);
        return StatusCode::Ok;
    }

    std::string_view SymbolModule::Description(Handle handle) const noexcept {
        const auto *slot = Find(handle);
        if (!slot) {
            return {};
        }
        return slot->entry.description;
    }

    bool SymbolModule::IsGlobal(Handle handle) const noexcept {
        const auto *slot = Find(handle);
        return slot && slot->entry.global;
    }

    bool SymbolModule::IsPinned(Handle handle) const noexcept {
        const auto *slot = Find(handle);
        return slot && slot->entry.pinned;
    }

    bool SymbolModule::IsValid(Handle handle) const noexcept {
        return Find(handle) != nullptr;
    }

    SymbolModule::Handle SymbolModule::WellKnownHandle(WellKnown kind) const noexcept {
        auto index = static_cast<std::size_t>(kind);
        if (index >= m_WellKnown.size()) {
            return 0;
        }
        return m_WellKnown[index];
    }

    const SymbolModule::Metrics &SymbolModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool SymbolModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    StatusCode SymbolModule::CreateInternal(std::string_view description,
                                            std::string_view key,
                                            bool global,
                                            bool pinned,
                                            std::uint64_t hash,
                                            Handle &outHandle,
                                            std::uint32_t &outSlot) {
        outHandle = 0;
        outSlot = kInvalidIndex;
        std::uint32_t slotIndex = kInvalidIndex;
        Slot *slot = nullptr;
        if (!m_FreeSlots.empty()) {
            slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            slot = &m_Slots[slotIndex];
            slot->inUse = true;
            slot->generation += 1;
            m_Metrics.recycledSlots += 1;
        } else {
            slotIndex = static_cast<std::uint32_t>(m_Slots.size());
            Slot fresh{};
            fresh.inUse = true;
            fresh.generation = 1;
            m_Slots.push_back(std::move(fresh));
            slot = &m_Slots.back();
            slotIndex = static_cast<std::uint32_t>(m_Slots.size() - 1);
        }
        slot->entry.handle = EncodeHandle(slotIndex, slot->generation);
        slot->entry.hash = hash;
        slot->entry.sequence = m_NextSequence++;
        slot->entry.version = 0;
        slot->entry.lastTouchFrame = 0;
        slot->entry.description.assign(description.data(), description.size());
        if (global) {
            slot->entry.key.assign(key.data(), key.size());
        } else {
            slot->entry.key.clear();
        }
        slot->entry.global = global;
        slot->entry.pinned = pinned;
        outHandle = slot->entry.handle;
        outSlot = slotIndex;
        if (global) {
            m_GlobalCount += 1;
        } else {
            m_LocalCount += 1;
        }
        m_Metrics.totalSymbols += 1;
        m_Metrics.globalSymbols = m_GlobalCount;
        m_Metrics.localSymbols = m_LocalCount;
        m_Metrics.liveSymbols = m_GlobalCount + m_LocalCount;
        if (pinned) {
            m_Metrics.wellKnownSymbols += 1;
        }
        Touch(*slot);
        return StatusCode::Ok;
    }

    void SymbolModule::Reset() {
        m_Slots.clear();
        m_FreeSlots.clear();
        m_GlobalBuckets.clear();
        m_WellKnown.fill(0);
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_GlobalCount = 0;
        m_LocalCount = 0;
        m_CurrentFrame = 0;
        m_NextSequence = 1;
        m_Initialized = false;
    }

    void SymbolModule::Touch(Slot &slot) noexcept {
        slot.entry.version += 1;
        slot.entry.lastTouchFrame = m_CurrentFrame;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void SymbolModule::ObserveGlobal(bool collision) noexcept {
        if (collision) {
            m_Metrics.collisions += 1;
        }
    }

    void SymbolModule::EnsureGlobalCapacity() {
        auto needed = static_cast<std::uint32_t>(m_GlobalCount + 1);
        if (m_GlobalBuckets.empty()) {
            RehashGlobals(std::max(kDefaultBuckets, needed));
            return;
        }
        auto capacity = static_cast<std::uint32_t>(m_GlobalBuckets.size());
        auto limit = (capacity * 3u) / 4u;
        if (limit < 1) {
            limit = 1;
        }
        if (needed > limit) {
            RehashGlobals(capacity * 2u);
        }
    }

    void SymbolModule::RehashGlobals(std::uint32_t request) {
        auto capacity = NormalizeBuckets(std::max(kDefaultBuckets, request));
        if (capacity == 0) {
            m_GlobalBuckets.clear();
            return;
        }
        std::vector<std::uint32_t> buckets(capacity, kInvalidIndex);
        auto mask = capacity - 1;
        for (std::uint32_t index = 0; index < m_Slots.size(); ++index) {
            const auto &slot = m_Slots[index];
            if (!slot.inUse || !slot.entry.global) {
                continue;
            }
            auto bucket = static_cast<std::uint32_t>(slot.entry.hash) & mask;
            while (buckets[bucket] != kInvalidIndex) {
                bucket = (bucket + 1) & mask;
            }
            buckets[bucket] = index;
        }
        m_GlobalBuckets.swap(buckets);
    }

    std::uint32_t SymbolModule::LocateGlobal(std::string_view key, std::uint64_t hash, std::uint32_t &bucket, bool &collision) const {
        bucket = 0;
        collision = false;
        if (m_GlobalBuckets.empty()) {
            return kInvalidIndex;
        }
        auto capacity = static_cast<std::uint32_t>(m_GlobalBuckets.size());
        auto mask = capacity - 1;
        auto index = static_cast<std::uint32_t>(hash) & mask;
        for (std::uint32_t probes = 0; probes < capacity; ++probes) {
            auto candidate = m_GlobalBuckets[index];
            if (candidate == kInvalidIndex) {
                bucket = index;
                collision = probes != 0;
                return kInvalidIndex;
            }
            const auto &slot = m_Slots[candidate];
            if (slot.inUse && slot.entry.global && slot.entry.hash == hash && slot.entry.key == key) {
                bucket = index;
                collision = probes != 0;
                return candidate;
            }
            index = (index + 1) & mask;
        }
        bucket = index;
        collision = true;
        return kInvalidIndex;
    }

    SymbolModule::Slot *SymbolModule::FindMutable(Handle handle) noexcept {
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse) {
            return nullptr;
        }
        if (DecodeGeneration(handle) != slot.generation) {
            return nullptr;
        }
        if (slot.entry.handle != handle) {
            return nullptr;
        }
        return &slot;
    }

    const SymbolModule::Slot *SymbolModule::Find(Handle handle) const noexcept {
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        const auto &slot = m_Slots[slotIndex];
        if (!slot.inUse) {
            return nullptr;
        }
        if (DecodeGeneration(handle) != slot.generation) {
            return nullptr;
        }
        if (slot.entry.handle != handle) {
            return nullptr;
        }
        return &slot;
    }

    std::uint64_t SymbolModule::HashKey(std::string_view key) noexcept {
        std::uint64_t hash = kHashSeed;
        for (unsigned char c: key) {
            hash ^= static_cast<std::uint64_t>(c);
            hash *= kHashPrime;
        }
        hash ^= hash >> 33;
        hash *= kHashMixerA;
        hash ^= hash >> 33;
        hash *= kHashMixerB;
        hash ^= hash >> 33;
        if (hash == 0) {
            hash = kHashPrime;
        }
        return hash;
    }

    std::uint32_t SymbolModule::NormalizeBuckets(std::uint32_t request) noexcept {
        if (request < 1) {
            request = 1;
        }
        if ((request & (request - 1)) != 0) {
            std::uint32_t value = 1;
            while (value < request) {
                value <<= 1;
            }
            request = value;
        }
        if (request < kDefaultBuckets) {
            request = kDefaultBuckets;
        }
        return request;
    }

    SymbolModule::Handle SymbolModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::uint32_t SymbolModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t SymbolModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }
}
