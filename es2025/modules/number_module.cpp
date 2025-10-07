#include "spectre/es2025/modules/number_module.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Number";
        constexpr std::string_view kSummary = "Number wrapper operations and IEEE-754 conversions.";
        constexpr std::string_view kReference = "ECMA-262 Section 20.1";
        constexpr std::size_t kHotFrameWindow = 8;
        constexpr double kMinInitial = std::numeric_limits<double>::infinity();
        constexpr double kMaxInitial = -std::numeric_limits<double>::infinity();
    }

    NumberModule::Metrics::Metrics() noexcept
        : canonicalHits(0),
          canonicalMisses(0),
          allocations(0),
          releases(0),
          mutations(0),
          normalizations(0),
          accumulations(0),
          saturations(0),
          hotNumbers(0),
          lastFrameTouched(0),
          minObserved(kMinInitial),
          maxObserved(kMaxInitial),
          gpuOptimized(false) {
    }

    NumberModule::NumberModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Slots{},
          m_FreeSlots{},
          m_CanonicalZero(0),
          m_CanonicalOne(0),
          m_CanonicalNaN(0),
          m_CanonicalInfinity(0),
          m_Metrics() {
    }

    std::string_view NumberModule::Name() const noexcept {
        return kName;
    }

    std::string_view NumberModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view NumberModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void NumberModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        Reset();
        m_Initialized = true;
    }

    void NumberModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        RecomputeHotMetrics();
    }

    void NumberModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void NumberModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode NumberModule::Create(std::string_view label, double value, Handle &outHandle) {
        return CreateInternal(label, value, false, outHandle);
    }

    StatusCode NumberModule::Clone(Handle handle, std::string_view label, Handle &outHandle) {
        const auto *entry = Find(handle);
        if (!entry) {
            outHandle = 0;
            return StatusCode::NotFound;
        }
        return CreateInternal(label, entry->value, false, outHandle);
    }

    StatusCode NumberModule::Destroy(Handle handle) {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        auto slotIndex = entry->slot;
        m_Slots[slotIndex].inUse = false;
        m_Slots[slotIndex].entry = Entry{};
        m_Slots[slotIndex].entry.slot = slotIndex;
        m_FreeSlots.push_back(slotIndex);
        m_Metrics.releases += 1;
        return StatusCode::Ok;
    }

    StatusCode NumberModule::Set(Handle handle, double value) noexcept {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        entry->value = value;
        entry->filtered = value;
        Touch(*entry);
        Observe(value);
        m_Metrics.mutations += 1;
        return StatusCode::Ok;
    }

    StatusCode NumberModule::Add(Handle handle, double delta) noexcept {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        entry->value += delta;
        entry->filtered = entry->filtered * 0.875 + entry->value * 0.125;
        Touch(*entry);
        Observe(entry->value);
        m_Metrics.mutations += 1;
        return StatusCode::Ok;
    }

    StatusCode NumberModule::Multiply(Handle handle, double factor) noexcept {
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        if (entry->pinned) {
            return StatusCode::InvalidArgument;
        }
        entry->value *= factor;
        entry->filtered = entry->filtered * factor;
        Touch(*entry);
        Observe(entry->value);
        m_Metrics.mutations += 1;
        return StatusCode::Ok;
    }

    StatusCode NumberModule::Saturate(Handle handle, double minValue, double maxValue) noexcept {
        if (minValue > maxValue) {
            std::swap(minValue, maxValue);
        }
        auto *entry = FindMutable(handle);
        if (!entry) {
            return StatusCode::NotFound;
        }
        auto clamped = std::min(std::max(entry->value, minValue), maxValue);
        if (clamped != entry->value) {
            entry->value = clamped;
            entry->filtered = clamped;
            Touch(*entry);
            Observe(clamped);
        }
        m_Metrics.saturations += 1;
        return StatusCode::Ok;
    }

    double NumberModule::ValueOf(Handle handle, double fallback) const noexcept {
        const auto *entry = Find(handle);
        return entry ? entry->value : fallback;
    }

    bool NumberModule::Has(Handle handle) const noexcept {
        return Find(handle) != nullptr;
    }

    StatusCode NumberModule::Accumulate(const double *values, std::size_t count, double &outSum) const noexcept {
        if (!values && count != 0) {
            outSum = 0.0;
            return StatusCode::InvalidArgument;
        }
        constexpr std::size_t kUnroll = 4;
        double sum0 = 0.0;
        double sum1 = 0.0;
        double sum2 = 0.0;
        double sum3 = 0.0;
        std::size_t i = 0;
        for (; i + kUnroll <= count; i += kUnroll) {
            sum0 += values[i];
            sum1 += values[i + 1];
            sum2 += values[i + 2];
            sum3 += values[i + 3];
        }
        double tail = 0.0;
        for (; i < count; ++i) {
            tail += values[i];
        }
        outSum = ((sum0 + sum1) + (sum2 + sum3)) + tail;
        m_Metrics.accumulations += 1;
        Observe(outSum);
        return StatusCode::Ok;
    }

    StatusCode NumberModule::Normalize(double *values,
                                       std::size_t count,
                                       double targetMin,
                                       double targetMax) const noexcept {
        if (!values && count != 0) {
            return StatusCode::InvalidArgument;
        }
        if (targetMin > targetMax) {
            std::swap(targetMin, targetMax);
        }
        if (count == 0) {
            return StatusCode::Ok;
        }
        double minValue = values[0];
        double maxValue = values[0];
        for (std::size_t i = 1; i < count; ++i) {
            minValue = std::min(minValue, values[i]);
            maxValue = std::max(maxValue, values[i]);
        }
        if (maxValue - minValue < std::numeric_limits<double>::epsilon()) {
            double mid = (targetMin + targetMax) * 0.5;
            for (std::size_t i = 0; i < count; ++i) {
                values[i] = mid;
            }
            m_Metrics.normalizations += 1;
            return StatusCode::Ok;
        }
        double inv = 1.0 / (maxValue - minValue);
        double range = targetMax - targetMin;
        for (std::size_t i = 0; i < count; ++i) {
            values[i] = targetMin + (values[i] - minValue) * inv * range;
        }
        m_Metrics.normalizations += 1;
        return StatusCode::Ok;
    }

    StatusCode NumberModule::BuildStatistics(const double *values,
                                             std::size_t count,
                                             Statistics &outStats) const noexcept {
        if (!values && count != 0) {
            return StatusCode::InvalidArgument;
        }
        if (count == 0) {
            outStats = {0.0, 0.0, 0.0, 0.0};
            return StatusCode::Ok;
        }
        double mean = 0.0;
        double m2 = 0.0;
        double minValue = values[0];
        double maxValue = values[0];
        double invN = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            double x = values[i];
            invN = 1.0 / static_cast<double>(i + 1);
            double delta = x - mean;
            mean += delta * invN;
            double delta2 = x - mean;
            m2 += delta * delta2;
            minValue = std::min(minValue, x);
            maxValue = std::max(maxValue, x);
        }
        outStats.mean = mean;
        outStats.variance = count > 1 ? m2 / static_cast<double>(count - 1) : 0.0;
        outStats.minValue = minValue;
        outStats.maxValue = maxValue;
        return StatusCode::Ok;
    }

    NumberModule::Handle NumberModule::Canonical(double value) noexcept {
        if (std::isnan(value)) {
            m_Metrics.canonicalHits += 1;
            return m_CanonicalNaN;
        }
        if (value == 0.0) {
            auto handle = std::signbit(value) ? m_CanonicalZero : m_CanonicalZero;
            m_Metrics.canonicalHits += 1;
            return handle;
        }
        if (value == 1.0) {
            m_Metrics.canonicalHits += 1;
            return m_CanonicalOne;
        }
        if (!std::isfinite(value) && std::isinf(value)) {
            m_Metrics.canonicalHits += 1;
            return m_CanonicalInfinity;
        }
        m_Metrics.canonicalMisses += 1;
        Handle handle = 0;
        CreateInternal("number.fast", value, false, handle);
        return handle;
    }

    const NumberModule::Metrics &NumberModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool NumberModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    NumberModule::Entry *NumberModule::FindMutable(Handle handle) noexcept {
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
        return &slot.entry;
    }

    const NumberModule::Entry *NumberModule::Find(Handle handle) const noexcept {
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
        return &slot.entry;
    }

    std::uint32_t NumberModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffull);
    }

    std::uint32_t NumberModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32) & 0xffffffffull);
    }

    NumberModule::Handle NumberModule::EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32) | static_cast<Handle>(slot);
    }

    StatusCode NumberModule::CreateInternal(std::string_view label, double value, bool pinned, Handle &outHandle) {
        outHandle = 0;
        std::uint32_t slotIndex = 0;
        if (!m_FreeSlots.empty()) {
            slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            auto &slot = m_Slots[slotIndex];
            slot.inUse = true;
            slot.generation += 1;
        } else {
            slotIndex = static_cast<std::uint32_t>(m_Slots.size());
            Slot slot{};
            slot.inUse = true;
            slot.generation = 1;
            m_Slots.push_back(slot);
        }
        auto &slot = m_Slots[slotIndex];
        slot.entry.handle = EncodeHandle(slotIndex, slot.generation);
        slot.entry.slot = slotIndex;
        slot.entry.generation = slot.generation;
        slot.entry.value = value;
        slot.entry.filtered = value;
        slot.entry.version = 0;
        slot.entry.lastTouchFrame = m_CurrentFrame;
        slot.entry.label.assign(label);
        slot.entry.hot = true;
        slot.entry.pinned = pinned;
        outHandle = slot.entry.handle;
        Touch(slot.entry);
        Observe(value);
        if (!pinned) {
            m_Metrics.allocations += 1;
        }
        return StatusCode::Ok;
    }

    void NumberModule::Reset() {
        m_Slots.clear();
        m_FreeSlots.clear();
        m_CurrentFrame = 0;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_CanonicalZero = 0;
        m_CanonicalOne = 0;
        m_CanonicalNaN = 0;
        m_CanonicalInfinity = 0;
        Handle zero = 0;
        Handle one = 0;
        Handle nan = 0;
        Handle infinity = 0;
        CreateInternal("number.zero", 0.0, true, zero);
        CreateInternal("number.one", 1.0, true, one);
        CreateInternal("number.nan", std::numeric_limits<double>::quiet_NaN(), true, nan);
        CreateInternal("number.inf", std::numeric_limits<double>::infinity(), true, infinity);
        m_CanonicalZero = zero;
        m_CanonicalOne = one;
        m_CanonicalNaN = nan;
        m_CanonicalInfinity = infinity;
    }

    void NumberModule::Touch(Entry &entry) noexcept {
        entry.version += 1;
        entry.lastTouchFrame = m_CurrentFrame;
        entry.hot = true;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void NumberModule::RecomputeHotMetrics() noexcept {
        std::uint64_t hot = 0;
        for (auto &slot: m_Slots) {
            if (!slot.inUse) {
                continue;
            }
            auto &entry = slot.entry;
            entry.hot = (m_CurrentFrame - entry.lastTouchFrame) <= kHotFrameWindow;
            if (entry.hot) {
                ++hot;
            }
        }
        m_Metrics.hotNumbers = hot;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
    }

    void NumberModule::Observe(double value) const noexcept {
        if (!std::isfinite(value)) {
            return;
        }
        m_Metrics.minObserved = std::min(m_Metrics.minObserved, value);
        m_Metrics.maxObserved = std::max(m_Metrics.maxObserved, value);
    }
}