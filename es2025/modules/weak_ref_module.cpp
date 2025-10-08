#include "spectre/es2025/modules/weak_ref_module.h"

#include "spectre/runtime.h"
#include "spectre/es2025/environment.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "WeakRef";
        constexpr std::string_view kSummary = "WeakRef primitives and lifetime observation hooks.";
        constexpr std::string_view kReference = "ECMA-262 Section 25.4";
        constexpr std::uint32_t kPruneBudgetPerTick = 16;
    }

    WeakRefModule::Reference::Reference()
        : target(0),
          version(0),
          lastAliveFrame(0) {
    }

    WeakRefModule::SlotRecord::SlotRecord()
        : inUse(false),
          generation(0),
          record() {
    }

    WeakRefModule::WeakRefModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_ObjectModule(nullptr),
          m_Metrics{},
          m_Slots(),
          m_FreeSlots(),
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_PruneCursor(0) {
    }

    WeakRefModule::~WeakRefModule() = default;

    std::string_view WeakRefModule::Name() const noexcept {
        return kName;
    }

    std::string_view WeakRefModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view WeakRefModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void WeakRefModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Initialized = true;
        m_CurrentFrame = 0;
        m_PruneCursor = 0;
        m_Metrics = {};
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_Metrics.lastFrameTouched = 0;
        m_Slots.clear();
        m_FreeSlots.clear();
        m_Slots.reserve(64);
        m_FreeSlots.reserve(32);
        auto &environment = context.runtime.EsEnvironment();
        auto *module = environment.FindModule("Object");
        m_ObjectModule = dynamic_cast<ObjectModule *>(module);
    }

    void WeakRefModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        PruneInvalid(kPruneBudgetPerTick);
    }

    void WeakRefModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void WeakRefModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode WeakRefModule::Create(ObjectModule::Handle target, Handle &outHandle) {
        outHandle = 0;
        if (!m_ObjectModule) {
            m_Metrics.failedOps += 1;
            return StatusCode::InternalError;
        }
        if (target == 0 || !m_ObjectModule->IsValid(target)) {
            m_Metrics.failedOps += 1;
            return StatusCode::InvalidArgument;
        }
        std::uint32_t slotIndex;
        if (!m_FreeSlots.empty()) {
            slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            auto &slot = m_Slots[slotIndex];
            slot.inUse = true;
            slot.generation += 1;
            slot.record.version += 1;
            slot.record.target = target;
            slot.record.lastAliveFrame = m_CurrentFrame;
            outHandle = EncodeHandle(slotIndex, slot.generation);
        } else {
            SlotRecord slot;
            slot.inUse = true;
            slot.generation = 1;
            slot.record.version = 1;
            slot.record.target = target;
            slot.record.lastAliveFrame = m_CurrentFrame;
            slotIndex = static_cast<std::uint32_t>(m_Slots.size());
            m_Slots.push_back(slot);
            outHandle = EncodeHandle(slotIndex, slot.generation);
        }
        m_Metrics.liveRefs += 1;
        m_Metrics.totalAllocations += 1;
        TouchMetrics();
        return StatusCode::Ok;
    }

    StatusCode WeakRefModule::Destroy(Handle handle) {
        auto *slot = FindMutableSlot(handle);
        if (!slot) {
            m_Metrics.failedOps += 1;
            return StatusCode::NotFound;
        }
        if (m_Metrics.liveRefs > 0) {
            m_Metrics.liveRefs -= 1;
        }
        m_Metrics.totalReleases += 1;
        slot->inUse = false;
        slot->record.target = 0;
        slot->record.lastAliveFrame = m_CurrentFrame;
        slot->record.version += 1;
        m_FreeSlots.push_back(static_cast<std::uint32_t>(DecodeSlot(handle)));
        TouchMetrics();
        return StatusCode::Ok;
    }

    StatusCode WeakRefModule::Refresh(Handle handle, ObjectModule::Handle target) {
        if (!m_ObjectModule) {
            m_Metrics.failedOps += 1;
            return StatusCode::InternalError;
        }
        auto *reference = FindMutable(handle);
        if (!reference) {
            m_Metrics.failedOps += 1;
            return StatusCode::NotFound;
        }
        if (target != 0 && !m_ObjectModule->IsValid(target)) {
            m_Metrics.failedOps += 1;
            return StatusCode::InvalidArgument;
        }
        const bool wasEmpty = reference->target == 0;
        reference->target = target;
        reference->version += 1;
        reference->lastAliveFrame = m_CurrentFrame;
        if (target == 0) {
            if (!wasEmpty) {
                m_Metrics.clearedRefs += 1;
            }
        } else if (wasEmpty) {
            m_Metrics.resurrectedRefs += 1;
        }
        m_Metrics.refreshOps += 1;
        TouchMetrics();
        return StatusCode::Ok;
    }

    StatusCode WeakRefModule::Deref(Handle handle, ObjectModule::Handle &outTarget, bool &outAlive) {
        outTarget = 0;
        outAlive = false;
        if (!m_ObjectModule) {
            m_Metrics.failedOps += 1;
            return StatusCode::InternalError;
        }
        auto *reference = FindMutable(handle);
        if (!reference) {
            m_Metrics.failedOps += 1;
            return StatusCode::NotFound;
        }
        if (reference->target != 0) {
            if (m_ObjectModule->IsValid(reference->target)) {
                outTarget = reference->target;
                outAlive = true;
                reference->lastAliveFrame = m_CurrentFrame;
            } else {
                reference->target = 0;
                reference->version += 1;
                m_Metrics.clearedRefs += 1;
            }
        }
        m_Metrics.derefOps += 1;
        TouchMetrics();
        return StatusCode::Ok;
    }

    bool WeakRefModule::Alive(Handle handle) const {
        if (!m_ObjectModule) {
            return false;
        }
        const auto *reference = Find(handle);
        if (!reference || reference->target == 0) {
            return false;
        }
        return m_ObjectModule->IsValid(reference->target);
    }

    std::uint32_t WeakRefModule::LiveCount() const noexcept {
        return static_cast<std::uint32_t>(m_Metrics.liveRefs);
    }

    StatusCode WeakRefModule::Compact() {
        if (m_Slots.empty()) {
            return StatusCode::Ok;
        }
        std::uint32_t removed = 0;
        while (!m_Slots.empty()) {
            auto index = static_cast<std::uint32_t>(m_Slots.size() - 1);
            if (m_Slots[index].inUse) {
                break;
            }
            m_Slots.pop_back();
            for (std::size_t i = 0; i < m_FreeSlots.size(); ++i) {
                if (m_FreeSlots[i] == index) {
                    m_FreeSlots.erase(m_FreeSlots.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            removed += 1;
            if (m_PruneCursor > index) {
                m_PruneCursor = 0;
            }
            if (m_FreeSlots.empty()) {
                break;
            }
        }
        if (m_PruneCursor >= m_Slots.size()) {
            m_PruneCursor = 0;
        }
        return StatusCode::Ok;
    }

    const WeakRefModule::Metrics &WeakRefModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    WeakRefModule::Handle WeakRefModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<std::uint64_t>(generation) << 32) | static_cast<std::uint64_t>(slot);
    }

    std::uint32_t WeakRefModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffu);
    }

    std::uint32_t WeakRefModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffu);
    }

    WeakRefModule::SlotRecord *WeakRefModule::FindMutableSlot(Handle handle) noexcept {
        if (handle == 0) {
            return nullptr;
        }
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse || slot.generation != DecodeGeneration(handle)) {
            return nullptr;
        }
        return &slot;
    }

    const WeakRefModule::SlotRecord *WeakRefModule::FindSlot(Handle handle) const noexcept {
        if (handle == 0) {
            return nullptr;
        }
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        const auto &slot = m_Slots[slotIndex];
        if (!slot.inUse || slot.generation != DecodeGeneration(handle)) {
            return nullptr;
        }
        return &slot;
    }

    WeakRefModule::Reference *WeakRefModule::FindMutable(Handle handle) noexcept {
        auto *slot = FindMutableSlot(handle);
        if (!slot) {
            return nullptr;
        }
        return &slot->record;
    }

    const WeakRefModule::Reference *WeakRefModule::Find(Handle handle) const noexcept {
        auto *slot = FindSlot(handle);
        if (!slot) {
            return nullptr;
        }
        return &slot->record;
    }

    void WeakRefModule::TouchMetrics() noexcept {
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void WeakRefModule::PruneInvalid(std::uint32_t budget) noexcept {
        if (!m_ObjectModule || m_Slots.empty() || budget == 0) {
            return;
        }
        std::uint32_t processed = 0;
        std::uint32_t index = m_PruneCursor;
        const std::uint32_t size = static_cast<std::uint32_t>(m_Slots.size());
        while (processed < budget) {
            if (index >= size) {
                index = 0;
            }
            auto &slot = m_Slots[index];
            if (slot.inUse && slot.record.target != 0 && !m_ObjectModule->IsValid(slot.record.target)) {
                slot.record.target = 0;
                slot.record.version += 1;
                m_Metrics.clearedRefs += 1;
            }
            ++processed;
            ++index;
            if (processed >= size) {
                break;
            }
        }
        m_PruneCursor = index % (m_Slots.empty() ? 1u : static_cast<std::uint32_t>(m_Slots.size()));
    }
}