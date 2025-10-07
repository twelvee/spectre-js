#include "spectre/es2025/modules/iterator_module.h"

#include <algorithm>
#include <limits>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Iterator";
        constexpr std::string_view kSummary = "Iterator protocol wiring and helper algorithms.";
        constexpr std::string_view kReference = "ECMA-262 Section 27.1";

        bool InitialRangeFinished(std::int64_t start, std::int64_t end, std::int64_t step, bool inclusive) noexcept {
            if (step > 0) {
                return inclusive ? start > end : start >= end;
            }
            if (step < 0) {
                return inclusive ? start < end : start <= end;
            }
            return true;
        }

        bool RangeExceeded(std::int64_t next, std::int64_t end, std::int64_t step, bool inclusive) noexcept {
            if (step > 0) {
                return inclusive ? next > end : next >= end;
            }
            if (step < 0) {
                return inclusive ? next < end : next <= end;
            }
            return true;
        }

        std::int64_t SafeAdvance(std::int64_t current, std::int64_t step) noexcept {
            if (step == 0) {
                return current;
            }
            if (step > 0) {
                auto limit = std::numeric_limits<std::int64_t>::max() - step;
                if (current > limit) {
                    return std::numeric_limits<std::int64_t>::max();
                }
            } else {
                auto limit = std::numeric_limits<std::int64_t>::min() - step;
                if (current < limit) {
                    return std::numeric_limits<std::int64_t>::min();
                }
            }
            return current + step;
        }
    }

    IteratorModule::IteratorModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Slots(),
          m_FreeList(),
          m_Active(0) {
    }

    std::string_view IteratorModule::Name() const noexcept {
        return kName;
    }

    std::string_view IteratorModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view IteratorModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void IteratorModule::Initialize(const ModuleInitContext &context) {
        for (auto &slot : m_Slots) {
            if (slot.active) {
                FinalizeSlot(slot);
            }
        }
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Initialized = true;
        m_CurrentFrame = 0;
        m_Slots.clear();
        m_FreeList.clear();
        m_Active = 0;
    }

    void IteratorModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
    }

    void IteratorModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
    }

    void IteratorModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
    }

    StatusCode IteratorModule::CreateRange(const RangeConfig &config, Handle &outHandle) {
        outHandle = 0;
        if (config.step == 0) {
            return StatusCode::InvalidArgument;
        }
        auto [slot, index] = AllocateSlot();
        RangeData data{};
        data.start = config.start;
        data.current = config.start;
        data.end = config.end;
        data.step = config.step;
        data.inclusive = config.inclusive;
        data.finished = InitialRangeFinished(config.start, config.end, config.step, config.inclusive);
        slot->payload = data;
        slot->done = data.finished;
        slot->last = Result();
        slot->last.done = data.finished;
        slot->last.hasValue = false;
        outHandle = MakeHandle(index, slot->generation);
        return StatusCode::Ok;
    }

    StatusCode IteratorModule::CreateList(std::vector<Value> values, Handle &outHandle) {
        outHandle = 0;
        auto [slot, index] = AllocateSlot();
        ListData data{};
        data.values = std::move(values);
        data.index = 0;
        slot->payload = std::move(data);
        slot->done = std::get<ListData>(slot->payload).values.empty();
        slot->last = Result();
        slot->last.done = slot->done;
        slot->last.hasValue = false;
        outHandle = MakeHandle(index, slot->generation);
        return StatusCode::Ok;
    }

    StatusCode IteratorModule::CreateList(std::span<const Value> values, Handle &outHandle) {
        std::vector<Value> copy;
        copy.reserve(values.size());
        for (const auto &value : values) {
            copy.push_back(value);
        }
        return CreateList(std::move(copy), outHandle);
    }

    StatusCode IteratorModule::CreateCustom(const CustomConfig &config, Handle &outHandle) {
        outHandle = 0;
        if (config.next == nullptr) {
            return StatusCode::InvalidArgument;
        }
        auto [slot, index] = AllocateSlot();
        CustomData data{};
        data.config = config;
        data.finished = false;
        data.closed = false;
        slot->payload = data;
        slot->done = false;
        slot->last = Result();
        slot->last.done = false;
        slot->last.hasValue = false;
        outHandle = MakeHandle(index, slot->generation);
        return StatusCode::Ok;
    }

    IteratorModule::Result IteratorModule::Next(Handle handle) {
        auto *slot = GetSlot(handle);
        if (slot == nullptr) {
            return Result();
        }
        slot->lastFrame = m_CurrentFrame;
        if (std::holds_alternative<std::monostate>(slot->payload)) {
            slot->last = Result();
            return slot->last;
        }
        if (auto *range = std::get_if<RangeData>(&slot->payload)) {
            if (range->finished) {
                slot->last = Result();
                slot->last.done = true;
                slot->last.hasValue = false;
                slot->done = true;
                return slot->last;
            }
            auto current = range->current;
            slot->last.value = Value::Number(static_cast<double>(current));
            slot->last.done = false;
            slot->last.hasValue = true;
            slot->done = false;
            auto next = SafeAdvance(current, range->step);
            range->current = next;
            range->finished = RangeExceeded(next, range->end, range->step, range->inclusive);
            if (range->finished) {
                slot->done = true;
            }
            return slot->last;
        }
        if (auto *list = std::get_if<ListData>(&slot->payload)) {
            if (list->index >= list->values.size()) {
                slot->last = Result();
                slot->last.done = true;
                slot->last.hasValue = false;
                slot->done = true;
                return slot->last;
            }
            slot->last.value = list->values[list->index];
            list->index += 1;
            slot->last.hasValue = true;
            slot->last.done = false;
            slot->done = false;
            return slot->last;
        }
        auto *custom = std::get_if<CustomData>(&slot->payload);
        if (custom == nullptr) {
            slot->last = Result();
            return slot->last;
        }
        if (custom->finished) {
            slot->last = Result();
            slot->last.done = true;
            slot->last.hasValue = false;
            slot->done = true;
            return slot->last;
        }
        auto result = custom->config.next(custom->config.state);
        slot->last = result;
        slot->done = result.done;
        if (result.done && !custom->closed) {
            if (custom->config.close != nullptr) {
                custom->config.close(custom->config.state);
            }
            custom->closed = true;
            custom->finished = true;
        }
        if (result.done) {
            custom->finished = true;
        }
        return slot->last;
    }

    std::size_t IteratorModule::Drain(Handle handle, std::span<Result> buffer) {
        if (buffer.empty()) {
            return 0;
        }
        std::size_t produced = 0;
        while (produced < buffer.size()) {
            auto result = Next(handle);
            buffer[produced] = result;
            produced += 1;
            if (result.done) {
                break;
            }
        }
        return produced;
    }

    void IteratorModule::Reset(Handle handle) {
        auto *slot = GetSlot(handle);
        if (slot == nullptr) {
            return;
        }
        slot->last = Result();
        slot->done = false;
        if (auto *range = std::get_if<RangeData>(&slot->payload)) {
            range->current = range->start;
            range->finished = InitialRangeFinished(range->start, range->end, range->step, range->inclusive);
            slot->done = range->finished;
            slot->last.done = slot->done;
            return;
        }
        if (auto *list = std::get_if<ListData>(&slot->payload)) {
            list->index = 0;
            slot->done = list->values.empty();
            slot->last.done = slot->done;
            return;
        }
        auto *custom = std::get_if<CustomData>(&slot->payload);
        if (custom == nullptr) {
            slot->done = true;
            slot->last.done = true;
            return;
        }
        custom->finished = false;
        if (custom->config.reset != nullptr) {
            custom->config.reset(custom->config.state);
        }
        if (custom->config.close != nullptr) {
            custom->closed = false;
        } else {
            custom->closed = true;
        }
    }

    void IteratorModule::Close(Handle handle) {
        auto *slot = GetSlot(handle);
        if (slot == nullptr) {
            return;
        }
        if (auto *custom = std::get_if<CustomData>(&slot->payload)) {
            if (!custom->closed && custom->config.close != nullptr) {
                custom->config.close(custom->config.state);
                custom->closed = true;
            }
            custom->finished = true;
        }
        slot->done = true;
        slot->last = Result();
        slot->last.done = true;
    }

    bool IteratorModule::Destroy(Handle handle) {
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

    bool IteratorModule::Done(Handle handle) const noexcept {
        auto *slot = GetSlot(handle);
        if (slot == nullptr) {
            return true;
        }
        return slot->done;
    }

    bool IteratorModule::Valid(Handle handle) const noexcept {
        return GetSlot(handle) != nullptr;
    }

    const IteratorModule::Result &IteratorModule::LastResult(Handle handle) const {
        auto *slot = GetSlot(handle);
        if (slot == nullptr) {
            static Result empty;
            return empty;
        }
        return slot->last;
    }

    std::size_t IteratorModule::ActiveIterators() const noexcept {
        return m_Active;
    }

    IteratorModule::Handle IteratorModule::MakeHandle(std::uint32_t index, std::uint32_t generation) noexcept {
        return ((generation & kGenerationMask) << kIndexBits) | (index & kIndexMask);
    }

    std::uint32_t IteratorModule::DecodeIndex(Handle handle) noexcept {
        return handle & kIndexMask;
    }

    std::uint32_t IteratorModule::DecodeGeneration(Handle handle) noexcept {
        return (handle >> kIndexBits) & kGenerationMask;
    }

    IteratorModule::Slot *IteratorModule::GetSlot(Handle handle) noexcept {
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

    const IteratorModule::Slot *IteratorModule::GetSlot(Handle handle) const noexcept {
        return const_cast<IteratorModule *>(this)->GetSlot(handle);
    }

    std::pair<IteratorModule::Slot *, std::uint32_t> IteratorModule::AllocateSlot() {
        Slot *slot;
        std::uint32_t index;
        if (!m_FreeList.empty()) {
            index = m_FreeList.back();
            m_FreeList.pop_back();
            slot = &m_Slots[index];
        } else {
            index = static_cast<std::uint32_t>(m_Slots.size());
            m_Slots.emplace_back();
            slot = &m_Slots.back();
        }
        slot->payload = std::monostate{};
        slot->last = Result();
        slot->done = true;
        slot->active = true;
        slot->lastFrame = m_CurrentFrame;
        slot->generation = (slot->generation + 1) & kGenerationMask;
        if (slot->generation == 0) {
            slot->generation = 1;
        }
        m_Active += 1;
        return {slot, index};
    }

    void IteratorModule::ReleaseSlot(std::uint32_t index) {
        if (index >= m_Slots.size()) {
            return;
        }
        auto &slot = m_Slots[index];
        if (!slot.active) {
            return;
        }
        FinalizeSlot(slot);
        slot.active = false;
        slot.done = true;
        slot.payload = std::monostate{};
        slot.last = Result();
        slot.lastFrame = m_CurrentFrame;
        slot.generation = (slot.generation + 1) & kGenerationMask;
        if (slot.generation == 0) {
            slot.generation = 1;
        }
        if (m_Active > 0) {
            m_Active -= 1;
        }
        m_FreeList.push_back(index);
    }

    void IteratorModule::FinalizeSlot(Slot &slot) {
        if (auto *custom = std::get_if<CustomData>(&slot.payload)) {
            if (!custom->closed && custom->config.close != nullptr) {
                custom->config.close(custom->config.state);
                custom->closed = true;
            }
            if (custom->config.destroy != nullptr) {
                custom->config.destroy(custom->config.state);
            }
            custom->config = CustomConfig{};
        }
    }
}



