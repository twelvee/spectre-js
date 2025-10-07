
#include "spectre/es2025/modules/proxy_module.h"

#include <utility>

#include "spectre/es2025/environment.h"
#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Proxy";
        constexpr std::string_view kSummary = "Proxy traps and meta-object protocol wiring.";
        constexpr std::string_view kReference = "ECMA-262 Section 28.1";
    }

    struct ProxyModule::ProxyRecord {
        ProxyRecord()
            : handle(0),
              slot(0),
              generation(0),
              target(0),
              traps{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
              revoked(false),
              version(0),
              lastTouchFrame(0) {}
        Handle handle;
        std::uint32_t slot;
        std::uint32_t generation;
        ObjectModule::Handle target;
        TrapTable traps;
        bool revoked;
        std::uint64_t version;
        std::uint64_t lastTouchFrame;
    };

    struct ProxyModule::SlotRecord {
        SlotRecord() : inUse(false), generation(0), record() {}
        bool inUse;
        std::uint32_t generation;
        ProxyRecord record;
    };
    ProxyModule::~ProxyModule() = default;

    ProxyModule::ProxyModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_ObjectModule(nullptr),
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Metrics{},
          m_Slots(),
          m_FreeList() {}

    std::string_view ProxyModule::Name() const noexcept {
        return kName;
    }

    std::string_view ProxyModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view ProxyModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void ProxyModule::Initialize(const ModuleInitContext &context) {
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
        auto &environment = context.runtime.EsEnvironment();
        auto *module = environment.FindModule("Object");
        m_ObjectModule = dynamic_cast<ObjectModule *>(module);
    }

    void ProxyModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void ProxyModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void ProxyModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }
    StatusCode ProxyModule::Create(ObjectModule::Handle target, const TrapTable &traps, Handle &outHandle) {
        outHandle = 0;
        if (!m_ObjectModule) {
            return StatusCode::NotFound;
        }
        if (!m_ObjectModule->IsValid(target)) {
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
        auto &record = slot.record;
        record = ProxyRecord();
        record.slot = slotIndex;
        record.generation = slot.generation;
        record.handle = EncodeHandle(slotIndex, slot.generation);
        record.target = target;
        record.traps = traps;
        record.revoked = false;
        record.version = 0;
        record.lastTouchFrame = m_CurrentFrame;
        Touch(record);
        TouchMetrics();
        outHandle = record.handle;
        m_Metrics.liveProxies += 1;
        m_Metrics.allocations += 1;
        return StatusCode::Ok;
    }

    StatusCode ProxyModule::Destroy(Handle handle) {
        auto *slot = FindMutableSlot(handle);
        if (!slot) {
            return StatusCode::NotFound;
        }
        slot->inUse = false;
        auto index = slot->record.slot;
        slot->record = ProxyRecord();
        m_FreeList.push_back(index);
        if (m_Metrics.liveProxies > 0) {
            m_Metrics.liveProxies -= 1;
        }
        m_Metrics.releases += 1;
        TouchMetrics();
        return StatusCode::Ok;
    }

    StatusCode ProxyModule::Revoke(Handle handle) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (record->revoked) {
            return StatusCode::Ok;
        }
        record->revoked = true;
        record->traps = TrapTable{};
        Touch(*record);
        m_Metrics.revocations += 1;
        return StatusCode::Ok;
    }
    StatusCode ProxyModule::Get(Handle handle, std::string_view key, ObjectModule::Value &outValue) {
        outValue.Reset();
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (record->revoked) {
            return StatusCode::InvalidArgument;
        }
        auto status = EnsureTarget(*record);
        if (status != StatusCode::Ok) {
            m_Metrics.misses += 1;
            return status;
        }
        Touch(*record);
        if (record->traps.get) {
            status = record->traps.get(*m_ObjectModule, record->target, key, outValue, record->traps.userdata);
            if (status == StatusCode::Ok) {
                m_Metrics.trapHits += 1;
            } else if (status == StatusCode::NotFound) {
                m_Metrics.misses += 1;
            }
            return status;
        }
        status = m_ObjectModule->Get(record->target, key, outValue);
        if (status == StatusCode::Ok) {
            m_Metrics.fallbackHits += 1;
        } else if (status == StatusCode::NotFound) {
            m_Metrics.misses += 1;
        }
        return status;
    }

    StatusCode ProxyModule::Set(Handle handle, std::string_view key, const ObjectModule::Value &value) {
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (record->revoked) {
            return StatusCode::InvalidArgument;
        }
        auto status = EnsureTarget(*record);
        if (status != StatusCode::Ok) {
            m_Metrics.misses += 1;
            return status;
        }
        Touch(*record);
        if (record->traps.set) {
            status = record->traps.set(*m_ObjectModule, record->target, key, value, record->traps.userdata);
            if (status == StatusCode::Ok) {
                m_Metrics.trapHits += 1;
            } else if (status == StatusCode::NotFound) {
                m_Metrics.misses += 1;
            }
            return status;
        }
        status = m_ObjectModule->Set(record->target, key, value);
        if (status == StatusCode::Ok) {
            m_Metrics.fallbackHits += 1;
        } else if (status == StatusCode::NotFound) {
            m_Metrics.misses += 1;
        }
        return status;
    }

    StatusCode ProxyModule::Has(Handle handle, std::string_view key, bool &outHas) {
        outHas = false;
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (record->revoked) {
            return StatusCode::InvalidArgument;
        }
        auto status = EnsureTarget(*record);
        if (status != StatusCode::Ok) {
            m_Metrics.misses += 1;
            return status;
        }
        Touch(*record);
        if (record->traps.has) {
            status = record->traps.has(*m_ObjectModule, record->target, key, outHas, record->traps.userdata);
            if (status == StatusCode::Ok) {
                m_Metrics.trapHits += 1;
            } else if (status == StatusCode::NotFound) {
                m_Metrics.misses += 1;
            }
            return status;
        }
        outHas = m_ObjectModule->Has(record->target, key);
        m_Metrics.fallbackHits += 1;
        return StatusCode::Ok;
    }

    StatusCode ProxyModule::Delete(Handle handle, std::string_view key, bool &outDeleted) {
        outDeleted = false;
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (record->revoked) {
            return StatusCode::InvalidArgument;
        }
        auto status = EnsureTarget(*record);
        if (status != StatusCode::Ok) {
            m_Metrics.misses += 1;
            return status;
        }
        Touch(*record);
        if (record->traps.drop) {
            status = record->traps.drop(*m_ObjectModule, record->target, key, outDeleted, record->traps.userdata);
            if (status == StatusCode::Ok) {
                m_Metrics.trapHits += 1;
            } else if (status == StatusCode::NotFound) {
                m_Metrics.misses += 1;
            }
            return status;
        }
        status = m_ObjectModule->Delete(record->target, key, outDeleted);
        if (status == StatusCode::Ok) {
            m_Metrics.fallbackHits += 1;
        } else if (status == StatusCode::NotFound) {
            m_Metrics.misses += 1;
        }
        return status;
    }

    StatusCode ProxyModule::OwnKeys(Handle handle, std::vector<std::string> &keys) {
        keys.clear();
        auto *record = FindMutable(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (record->revoked) {
            return StatusCode::InvalidArgument;
        }
        auto status = EnsureTarget(*record);
        if (status != StatusCode::Ok) {
            m_Metrics.misses += 1;
            return status;
        }
        Touch(*record);
        if (record->traps.keys) {
            status = record->traps.keys(*m_ObjectModule, record->target, keys, record->traps.userdata);
            if (status == StatusCode::Ok) {
                m_Metrics.trapHits += 1;
            } else if (status == StatusCode::NotFound) {
                m_Metrics.misses += 1;
            }
            return status;
        }
        status = m_ObjectModule->OwnKeys(record->target, keys);
        if (status == StatusCode::Ok) {
            m_Metrics.fallbackHits += 1;
        } else if (status == StatusCode::NotFound) {
            m_Metrics.misses += 1;
        }
        return status;
    }

    const ProxyModule::Metrics &ProxyModule::GetMetrics() const noexcept {
        return m_Metrics;
    }
    ProxyModule::SlotRecord *ProxyModule::FindMutableSlot(Handle handle) noexcept {
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

    const ProxyModule::SlotRecord *ProxyModule::FindSlot(Handle handle) const noexcept {
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

    ProxyModule::ProxyRecord *ProxyModule::FindMutable(Handle handle) noexcept {
        auto *slot = FindMutableSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    const ProxyModule::ProxyRecord *ProxyModule::Find(Handle handle) const noexcept {
        const auto *slot = FindSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    void ProxyModule::Touch(ProxyRecord &record) noexcept {
        record.version += 1;
        record.lastTouchFrame = m_CurrentFrame;
        TouchMetrics();
    }

    void ProxyModule::TouchMetrics() noexcept {
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    StatusCode ProxyModule::EnsureTarget(ProxyRecord &record) const {
        if (!m_ObjectModule) {
            return StatusCode::NotFound;
        }
        return m_ObjectModule->IsValid(record.target) ? StatusCode::Ok : StatusCode::NotFound;
    }

    ProxyModule::Handle ProxyModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    std::uint32_t ProxyModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t ProxyModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }
}


