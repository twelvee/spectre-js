#include "spectre/es2025/modules/boolean_module.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Boolean";
        constexpr std::string_view kSummary = "Boolean wrapper caches, canonical boxing, and conversion helpers.";
        constexpr std::string_view kReference = "ECMA-262 Section 19.3";

        constexpr std::size_t kHotFrameWindow = 6;

        std::string_view TrimAscii(std::string_view text) noexcept {
            std::size_t begin = 0;
            std::size_t end = text.size();
            while (begin < end && static_cast<unsigned char>(text[begin]) <= 0x20) {
                ++begin;
            }
            while (end > begin && static_cast<unsigned char>(text[end - 1]) <= 0x20) {
                --end;
            }
            return text.substr(begin, end - begin);
        }

        bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
            if (a.size() != b.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i) {
                auto ca = static_cast<unsigned char>(a[i]);
                auto cb = static_cast<unsigned char>(b[i]);
                if (std::tolower(ca) != std::tolower(cb)) {
                    return false;
                }
            }
            return true;
        }
    }

    BooleanModule::CacheMetrics::CacheMetrics() noexcept
        : conversions(0),
          canonicalHits(0),
          allocations(0),
          activeBoxes(0),
          toggles(0),
          lastFrameTouched(0),
          hotBoxes(0),
          gpuOptimized(false) {
    }

    BooleanModule::BooleanModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_CanonicalTrue(0),
          m_CanonicalFalse(0),
          m_Slots{},
          m_FreeSlots{},
          m_Metrics{} {
    }

    std::string_view BooleanModule::Name() const noexcept {
        return kName;
    }

    std::string_view BooleanModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view BooleanModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void BooleanModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Metrics = CacheMetrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        Reset();
        m_Initialized = true;
    }

    void BooleanModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        RecomputeHotMetrics();
    }

    void BooleanModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void BooleanModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    bool BooleanModule::ToBoolean(double value) const noexcept {
        return !std::isnan(value) && value != 0.0;
    }

    bool BooleanModule::ToBoolean(std::int64_t value) const noexcept {
        return value != 0;
    }

    bool BooleanModule::ToBoolean(std::string_view text) const noexcept {
        auto trimmed = TrimAscii(text);
        if (trimmed.empty()) {
            return false;
        }
        if (EqualsIgnoreCase(trimmed, "false") ||
            EqualsIgnoreCase(trimmed, "0") ||
            EqualsIgnoreCase(trimmed, "nan") ||
            EqualsIgnoreCase(trimmed, "null") ||
            EqualsIgnoreCase(trimmed, "undefined") ||
            EqualsIgnoreCase(trimmed, "off") ||
            EqualsIgnoreCase(trimmed, "no")) {
            return false;
        }
        return true;
    }

    BooleanModule::Handle BooleanModule::Box(bool value) noexcept {
        m_Metrics.conversions += 1;
        m_Metrics.canonicalHits += 1;
        auto handle = value ? m_CanonicalTrue : m_CanonicalFalse;
        if (auto *entry = FindMutable(handle)) {
            Touch(*entry);
        }
        return handle;
    }

    StatusCode BooleanModule::Create(std::string_view label, bool value, Handle &outHandle) {
        auto assignedLabel = label.empty() ? std::string_view("bool.box") : label;
        auto status = CreateInternal(assignedLabel, value, false, outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.conversions += 1;
        }
        return status;
    }

    StatusCode BooleanModule::Destroy(Handle handle) {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        auto slotIndex = entry->slot;
        auto &slot = m_Slots[slotIndex];
        slot.inUse = false;
        slot.generation += 1;
        slot.entry = Entry{};
        m_FreeSlots.push_back(slotIndex);
        if (m_Metrics.activeBoxes > 0) {
            m_Metrics.activeBoxes -= 1;
        }
        RecomputeHotMetrics();
        return StatusCode::Ok;
    }

    StatusCode BooleanModule::Set(Handle handle, bool value) noexcept {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        entry->value = value;
        Touch(*entry);
        return StatusCode::Ok;
    }

    StatusCode BooleanModule::Toggle(Handle handle) noexcept {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        entry->value = !entry->value;
        m_Metrics.toggles += 1;
        Touch(*entry);
        return StatusCode::Ok;
    }

    StatusCode BooleanModule::ValueOf(Handle handle, bool &outValue) const noexcept {
        const auto *entry = Find(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        outValue = entry->value;
        return StatusCode::Ok;
    }

    bool BooleanModule::Has(Handle handle) const noexcept {
        return Find(handle) != nullptr;
    }

    const BooleanModule::CacheMetrics &BooleanModule::Metrics() const noexcept {
        return m_Metrics;
    }

    bool BooleanModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    BooleanModule::Entry *BooleanModule::FindMutable(Handle handle) noexcept {
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
        return &slot.entry;
    }

    const BooleanModule::Entry *BooleanModule::Find(Handle handle) const noexcept {
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
        return &slot.entry;
    }

    std::uint32_t BooleanModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t BooleanModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }

    BooleanModule::Handle BooleanModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    void BooleanModule::Reset() {
        m_Slots.clear();
        m_FreeSlots.clear();
        m_CurrentFrame = 0;
        m_CanonicalTrue = 0;
        m_CanonicalFalse = 0;
        Handle trueHandle = 0;
        Handle falseHandle = 0;
        CreateInternal("bool.true", true, true, trueHandle);
        CreateInternal("bool.false", false, true, falseHandle);
        m_CanonicalTrue = trueHandle;
        m_CanonicalFalse = falseHandle;
        RecomputeHotMetrics();
    }

    StatusCode BooleanModule::CreateInternal(std::string_view label,
                                             bool value,
                                             bool pinned,
                                             Handle &outHandle) {
        outHandle = 0;
        std::uint32_t slotIndex;
        if (!m_FreeSlots.empty()) {
            slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            auto &slot = m_Slots[slotIndex];
            slot.inUse = true;
            slot.generation += 1;
            outHandle = EncodeHandle(slotIndex, slot.generation);
            slot.entry.handle = outHandle;
            slot.entry.slot = slotIndex;
            slot.entry.generation = slot.generation;
            slot.entry.label.assign(label);
            slot.entry.value = value;
            slot.entry.pinned = pinned;
            slot.entry.version = 0;
            slot.entry.lastTouchFrame = m_CurrentFrame;
            slot.entry.hot = true;
        } else {
            slotIndex = static_cast<std::uint32_t>(m_Slots.size());
            Slot slot{};
            slot.inUse = true;
            slot.generation = 1;
            slot.entry.handle = 0;
            m_Slots.push_back(slot);
            auto &stored = m_Slots[slotIndex];
            stored.inUse = true;
            stored.generation = 1;
            stored.entry.handle = EncodeHandle(slotIndex, stored.generation);
            stored.entry.slot = slotIndex;
            stored.entry.generation = stored.generation;
            stored.entry.label.assign(label);
            stored.entry.value = value;
            stored.entry.pinned = pinned;
            stored.entry.version = 0;
            stored.entry.lastTouchFrame = m_CurrentFrame;
            stored.entry.hot = true;
            outHandle = stored.entry.handle;
        }
        if (!pinned) {
            m_Metrics.allocations += 1;
            m_Metrics.activeBoxes += 1;
        }
        Touch(*FindMutable(outHandle));
        RecomputeHotMetrics();
        return StatusCode::Ok;
    }

    void BooleanModule::Touch(Entry &entry) noexcept {
        entry.version += 1;
        entry.lastTouchFrame = m_CurrentFrame;
        entry.hot = true;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void BooleanModule::RecomputeHotMetrics() noexcept {
        std::uint64_t hot = 0;
        for (auto &slot : m_Slots) {
            if (!slot.inUse) {
                continue;
            }
            auto &entry = slot.entry;
            entry.hot = (m_CurrentFrame - entry.lastTouchFrame) <= kHotFrameWindow;
            if (entry.hot) {
                ++hot;
            }
        }
        m_Metrics.hotBoxes = hot;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }
}
