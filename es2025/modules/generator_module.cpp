#include "spectre/es2025/modules/generator_module.h"

#include <utility>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Generator";
        constexpr std::string_view kSummary = "Generator functions, iterator integration, and resume mechanics.";
        constexpr std::string_view kReference = "ECMA-262 Section 27.4";
    }

    GeneratorModule::GeneratorModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Slots(),
          m_FreeSlots(),
          m_Bridges(),
          m_FreeBridges(),
          m_Active(0),
          m_ActiveBridges(0) {
    }

    std::string_view GeneratorModule::Name() const noexcept {
        return kName;
    }

    std::string_view GeneratorModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view GeneratorModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void GeneratorModule::Initialize(const ModuleInitContext &context) {
        for (auto &slot : m_Slots) {
            if (slot.active) {
                CloseSlot(slot);
                slot.active = false;
            }
        }
        for (auto &bridgePtr : m_Bridges) {
            if (bridgePtr && bridgePtr->active) {
                bridgePtr->active = false;
                bridgePtr->module = nullptr;
            }
        }

        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Initialized = true;
        m_CurrentFrame = 0;
        m_Slots.clear();
        m_FreeSlots.clear();
        m_Bridges.clear();
        m_FreeBridges.clear();
        m_Active = 0;
        m_ActiveBridges = 0;
    }

    void GeneratorModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
    }

    void GeneratorModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
    }

    void GeneratorModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
    }

    StatusCode GeneratorModule::Register(const Descriptor &descriptor, Handle &outHandle) {
        outHandle = 0;
        if (descriptor.stepper == nullptr) {
            return StatusCode::InvalidArgument;
        }
        auto [slot, index] = AllocateSlot();
        slot->stepper = descriptor.stepper;
        slot->state = descriptor.state;
        slot->reset = descriptor.reset;
        slot->destroy = descriptor.destroy;
        slot->name = descriptor.name;
        slot->yieldValue = Value::Undefined();
        slot->hasValue = false;
        slot->done = false;
        slot->running = false;
        slot->awaitingInput = false;
        slot->resumePoint = descriptor.resumePoint;
        slot->initialResumePoint = descriptor.resumePoint;
        slot->resumeCount = 0;
        slot->lastFrame = m_CurrentFrame;
        if (slot->reset != nullptr) {
            slot->reset(slot->state);
        }
        outHandle = MakeHandle(index, slot->generation);
        return StatusCode::Ok;
    }

    bool GeneratorModule::Destroy(Handle handle) {
        auto index = DecodeIndex(handle);
        if (index >= m_Slots.size()) {
            return false;
        }
        auto generation = DecodeGeneration(handle);
        auto &slot = m_Slots[index];
        if (!slot.active || slot.generation != generation) {
            return false;
        }
        ReleaseSlot(index);
        return true;
    }

    GeneratorModule::StepResult GeneratorModule::Resume(Handle handle, std::string_view input) {
        auto *slot = GetSlot(handle);
        if (slot == nullptr) {
            return StepResult();
        }
        return ResumeInternal(*slot, input);
    }

    void GeneratorModule::Reset(Handle handle) {
        auto *slot = GetSlot(handle);
        if (slot == nullptr) {
            return;
        }
        if (slot->reset != nullptr) {
            slot->reset(slot->state);
        }
        slot->resumePoint = slot->initialResumePoint;
        slot->done = false;
        slot->hasValue = false;
        slot->running = false;
        slot->awaitingInput = false;
        slot->yieldValue = Value::Undefined();
        slot->lastFrame = m_CurrentFrame;
    }

    bool GeneratorModule::Completed(Handle handle) const noexcept {
        auto *slot = GetSlot(handle);
        if (slot == nullptr) {
            return true;
        }
        return slot->done;
    }

    bool GeneratorModule::Valid(Handle handle) const noexcept {
        return GetSlot(handle) != nullptr;
    }

    std::size_t GeneratorModule::ActiveGenerators() const noexcept {
        return m_Active;
    }

    std::uint64_t GeneratorModule::ResumeCount(Handle handle) const noexcept {
        auto *slot = GetSlot(handle);
        if (slot == nullptr) {
            return 0;
        }
        return slot->resumeCount;
    }

    std::uint64_t GeneratorModule::LastFrameTouched(Handle handle) const noexcept {
        auto *slot = GetSlot(handle);
        if (slot == nullptr) {
            return 0;
        }
        return slot->lastFrame;
    }

    StatusCode GeneratorModule::CreateIteratorBridge(Handle handle,
                                                     IteratorModule &iteratorModule,
                                                     std::uint32_t &outIteratorHandle) {
        outIteratorHandle = 0;
        auto *slot = GetSlot(handle);
        if (slot == nullptr) {
            return StatusCode::InvalidArgument;
        }
        auto *bridge = AllocateBridge(handle, slot->generation);
        if (bridge == nullptr) {
            return StatusCode::InternalError;
        }
        bridge->module = this;
        bridge->closed = false;
        IteratorModule::CustomConfig config{};
        config.next = BridgeNextThunk;
        config.reset = BridgeResetThunk;
        config.close = BridgeCloseThunk;
        config.destroy = BridgeDestroyThunk;
        config.state = bridge;
        auto status = iteratorModule.CreateCustom(config, outIteratorHandle);
        if (status != StatusCode::Ok) {
            ReleaseBridge(bridge->index);
            return status;
        }
        return StatusCode::Ok;
    }

    GeneratorModule::Handle GeneratorModule::MakeHandle(std::uint32_t index, std::uint32_t generation) noexcept {
        return ((generation & kGenerationMask) << kIndexBits) | (index & kIndexMask);
    }

    std::uint32_t GeneratorModule::DecodeIndex(Handle handle) noexcept {
        return handle & kIndexMask;
    }

    std::uint32_t GeneratorModule::DecodeGeneration(Handle handle) noexcept {
        return (handle >> kIndexBits) & kGenerationMask;
    }

    GeneratorModule::Slot *GeneratorModule::GetSlot(Handle handle) noexcept {
        auto index = DecodeIndex(handle);
        if (index >= m_Slots.size()) {
            return nullptr;
        }
        auto generation = DecodeGeneration(handle);
        auto &slot = m_Slots[index];
        if (!slot.active) {
            return nullptr;
        }
        if (slot.generation != generation) {
            return nullptr;
        }
        return &slot;
    }

    const GeneratorModule::Slot *GeneratorModule::GetSlot(Handle handle) const noexcept {
        return const_cast<GeneratorModule *>(this)->GetSlot(handle);
    }

    std::pair<GeneratorModule::Slot *, std::uint32_t> GeneratorModule::AllocateSlot() {
        Slot *slot;
        std::uint32_t index;
        if (!m_FreeSlots.empty()) {
            index = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            slot = &m_Slots[index];
        } else {
            index = static_cast<std::uint32_t>(m_Slots.size());
            m_Slots.emplace_back();
            slot = &m_Slots.back();
        }
        slot->active = true;
        slot->generation = (slot->generation + 1) & kGenerationMask;
        if (slot->generation == 0) {
            slot->generation = 1;
        }
        slot->resumeCount = 0;
        slot->lastFrame = m_CurrentFrame;
        slot->yieldValue = Value::Undefined();
        slot->hasValue = false;
        slot->done = false;
        slot->running = false;
        slot->awaitingInput = false;
        m_Active += 1;
        return {slot, index};
    }

    void GeneratorModule::ReleaseSlot(std::uint32_t index) {
        if (index >= m_Slots.size()) {
            return;
        }
        auto &slot = m_Slots[index];
        if (!slot.active) {
            return;
        }
        CloseSlot(slot);
        slot.active = false;
        slot.generation = (slot.generation + 1) & kGenerationMask;
        if (slot.generation == 0) {
            slot.generation = 1;
        }
        slot.stepper = nullptr;
        slot.state = nullptr;
        slot.reset = nullptr;
        slot.destroy = nullptr;
        slot.name.clear();
        slot.yieldValue = Value::Undefined();
        slot.hasValue = false;
        slot.done = true;
        slot.running = false;
        slot.awaitingInput = false;
        slot.resumePoint = 0;
        slot.initialResumePoint = 0;
        slot.resumeCount = 0;
        slot.lastFrame = m_CurrentFrame;
        if (m_Active > 0) {
            m_Active -= 1;
        }
        m_FreeSlots.push_back(index);
    }

    GeneratorModule::BridgeState *GeneratorModule::AllocateBridge(Handle handle, std::uint32_t generation) {
        BridgeState *bridge = nullptr;
        std::uint32_t index = 0;
        if (!m_FreeBridges.empty()) {
            index = m_FreeBridges.back();
            m_FreeBridges.pop_back();
            if (index < m_Bridges.size() && m_Bridges[index]) {
                bridge = m_Bridges[index].get();
            } else {
                if (index >= m_Bridges.size()) {
                    m_Bridges.resize(index + 1);
                }
                m_Bridges[index] = std::make_unique<BridgeState>();
                bridge = m_Bridges[index].get();
            }
        } else {
            index = static_cast<std::uint32_t>(m_Bridges.size());
            m_Bridges.emplace_back(std::make_unique<BridgeState>());
            bridge = m_Bridges.back().get();
        }
        *bridge = BridgeState();
        bridge->handle = handle;
        bridge->generation = generation;
        bridge->module = this;
        bridge->closed = false;
        bridge->index = index;
        bridge->active = true;
        m_ActiveBridges += 1;
        return bridge;
    }

    void GeneratorModule::ReleaseBridge(std::uint32_t index) {
        if (index >= m_Bridges.size()) {
            return;
        }
        auto &bridgePtr = m_Bridges[index];
        if (!bridgePtr || !bridgePtr->active) {
            return;
        }
        auto &bridge = *bridgePtr;
        bridge.active = false;
        bridge.closed = true;
        bridge.module = nullptr;
        bridge.handle = 0;
        bridge.generation = 0;
        if (m_ActiveBridges > 0) {
            m_ActiveBridges -= 1;
        }
        m_FreeBridges.push_back(index);
    }




    GeneratorModule::StepResult GeneratorModule::ResumeInternal(Slot &slot, std::string_view input) {
        StepResult result;
        result.value = Value::Undefined();
        result.done = slot.done;
        result.hasValue = false;
        result.awaitingInput = false;
        if (slot.done || slot.stepper == nullptr || slot.running) {
            return result;
        }
        slot.yieldValue = Value::Undefined();
        ExecutionContext context{input, slot.yieldValue, false, false, slot.resumePoint, slot.resumePoint, false};
        slot.running = true;
        slot.stepper(slot.state, context);
        slot.running = false;
        slot.resumePoint = context.nextResumePoint;
        slot.awaitingInput = context.requestingInput;
        slot.done = context.done;
        slot.hasValue = context.hasValue && !slot.done;
        slot.resumeCount += 1;
        slot.lastFrame = m_CurrentFrame;
        if (context.hasValue) {
            result.value = context.yieldValue;
            result.hasValue = true;
        } else {
            result.value = Value::Undefined();
            result.hasValue = false;
        }
        result.done = slot.done;
        result.awaitingInput = slot.awaitingInput && !slot.done;
        if (slot.done) {
            slot.yieldValue = Value::Undefined();
            slot.hasValue = false;
        }
        return result;
    }

    IteratorModule::Result GeneratorModule::BridgeNext(BridgeState &bridge) {
        IteratorModule::Result result;
        if (!bridge.active || bridge.module == nullptr || bridge.closed) {
            return result;
        }
        auto *slot = GetSlot(bridge.handle);
        if (slot == nullptr || slot->generation != bridge.generation) {
            bridge.closed = true;
            return result;
        }
        auto step = ResumeInternal(*slot, {});
        result.value = step.value;
        result.done = step.done;
        result.hasValue = step.hasValue;
        if (step.done) {
            bridge.closed = true;
        }
        return result;
    }

    void GeneratorModule::BridgeReset(BridgeState &bridge) {
        if (!bridge.active || bridge.module == nullptr || bridge.closed) {
            return;
        }
        auto *slot = GetSlot(bridge.handle);
        if (slot == nullptr || slot->generation != bridge.generation) {
            bridge.closed = true;
            return;
        }
        Reset(bridge.handle);
    }

    void GeneratorModule::BridgeClose(BridgeState &bridge) {
        if (!bridge.active || bridge.module == nullptr || bridge.closed) {
            return;
        }
        bridge.closed = true;
    }

    void GeneratorModule::BridgeDestroy(BridgeState &bridge) {
        if (!bridge.active) {
            return;
        }
        ReleaseBridge(bridge.index);
    }

    IteratorModule::Result GeneratorModule::BridgeNextThunk(void *state) {
        if (state == nullptr) {
            return IteratorModule::Result();
        }
        auto *bridge = static_cast<BridgeState *>(state);
        return bridge->module != nullptr ? bridge->module->BridgeNext(*bridge) : IteratorModule::Result();
    }

    void GeneratorModule::BridgeResetThunk(void *state) {
        if (state == nullptr) {
            return;
        }
        auto *bridge = static_cast<BridgeState *>(state);
        if (bridge->module != nullptr) {
            bridge->module->BridgeReset(*bridge);
        }
    }

    void GeneratorModule::BridgeCloseThunk(void *state) {
        if (state == nullptr) {
            return;
        }
        auto *bridge = static_cast<BridgeState *>(state);
        if (bridge->module != nullptr) {
            bridge->module->BridgeClose(*bridge);
        }
    }

    void GeneratorModule::BridgeDestroyThunk(void *state) {
        if (state == nullptr) {
            return;
        }
        auto *bridge = static_cast<BridgeState *>(state);
        if (bridge->module != nullptr) {
            bridge->module->BridgeDestroy(*bridge);
        } else if (bridge->active) {
            bridge->active = false;
        }
    }

    void GeneratorModule::CloseSlot(Slot &slot) {
        if (slot.destroy != nullptr && slot.state != nullptr) {
            slot.destroy(slot.state);
        }
        slot.state = nullptr;
    }
}









