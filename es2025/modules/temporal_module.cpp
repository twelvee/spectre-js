#include "spectre/es2025/modules/temporal_module.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Temporal";
        constexpr std::string_view kSummary =
                "Temporal instants, high precision duration math, and calendar conversions.";
        constexpr std::string_view kReference = "TC39 Temporal Stage 3";

        constexpr std::int64_t kNanosPerMicrosecond = 1000;
        constexpr std::int64_t kNanosPerMillisecond = kNanosPerMicrosecond * 1000;
        constexpr std::int64_t kNanosPerSecond = kNanosPerMillisecond * 1000;
        constexpr std::int64_t kNanosPerMinute = kNanosPerSecond * 60;
        constexpr std::int64_t kNanosPerHour = kNanosPerMinute * 60;
        constexpr std::int64_t kNanosPerDay = kNanosPerHour * 24;

        constexpr int kMonthOffsets[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        constexpr int kMonthOffsetsLeap[12] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
        constexpr std::size_t kTemporalSlotLimit = (1u << 16) - 1;

        inline bool IsLeap(int year) noexcept {
            return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        }

        std::int64_t DaysFromCivil(int year, int month, int day) noexcept {
            year -= month <= 2;
            const int era = (year >= 0 ? year : year - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(year - era * 400);
            const unsigned doy = (153u * (static_cast<unsigned>(month + (month > 2 ? -3 : 9))) + 2u) / 5u
                                 + static_cast<unsigned>(day) - 1u;
            const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + yoe / 400u + doy;
            return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
        }

        void CivilFromDays(std::int64_t days, int &year, unsigned &month, unsigned &day) noexcept {
            days += 719468;
            const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
            const unsigned doe = static_cast<unsigned>(days - era * 146097);
            const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100 + yoe / 400);
            const unsigned mp = (5 * doy + 2) / 153;
            day = doy - (153 * mp + 2) / 5 + 1;
            month = mp + (mp < 10 ? 3 : -9);
            year = static_cast<int>(yoe) + static_cast<int>(era) * 400 + (month <= 2 ? 1 : 0);
        }

        std::int64_t FloorDiv(std::int64_t numerator, std::int64_t denominator) noexcept {
            if (denominator <= 0) {
                return 0;
            }
            auto quotient = numerator / denominator;
            auto remainder = numerator % denominator;
            if (remainder != 0 && ((remainder < 0) != (denominator < 0))) {
                quotient -= 1;
            }
            return quotient;
        }

        std::int64_t FloorMod(std::int64_t numerator, std::int64_t denominator) noexcept {
            if (denominator <= 0) {
                return 0;
            }
            auto remainder = numerator % denominator;
            if (remainder != 0 && ((remainder < 0) != (denominator < 0))) {
                remainder += denominator;
            }
            return remainder;
        }

        std::size_t RecommendInstantCapacity(const RuntimeConfig &config) noexcept {
            auto heap = config.memory.heapBytes;
            if (heap == 0) {
                return 32;
            }
            auto capacity = heap / 65536ULL;
            if (capacity < 32) {
                capacity = 32;
            }
            if (capacity > kTemporalSlotLimit) {
                capacity = kTemporalSlotLimit;
            }
            return static_cast<std::size_t>(capacity);
        }
    }

    TemporalModule::Duration::Duration() noexcept : totalNanoseconds(0) {
    }

    TemporalModule::Duration::Duration(std::int64_t nanos) noexcept : totalNanoseconds(nanos) {
    }

    TemporalModule::Duration TemporalModule::Duration::FromComponents(std::int64_t days,
                                                                      std::int64_t hours,
                                                                      std::int64_t minutes,
                                                                      std::int64_t seconds,
                                                                      std::int64_t milliseconds,
                                                                      std::int64_t microseconds,
                                                                      std::int64_t nanoseconds) noexcept {
        std::int64_t total = 0;
        total = TemporalModule::SaturatingAdd(
            total, TemporalModule::SaturatingMul(days, kNanosPerDay));
        total = TemporalModule::SaturatingAdd(
            total, TemporalModule::SaturatingMul(hours, kNanosPerHour));
        total = TemporalModule::SaturatingAdd(
            total, TemporalModule::SaturatingMul(minutes, kNanosPerMinute));
        total = TemporalModule::SaturatingAdd(
            total, TemporalModule::SaturatingMul(seconds, kNanosPerSecond));
        total = TemporalModule::SaturatingAdd(
            total, TemporalModule::SaturatingMul(milliseconds, kNanosPerMillisecond));
        total = TemporalModule::SaturatingAdd(
            total, TemporalModule::SaturatingMul(microseconds, kNanosPerMicrosecond));
        total = TemporalModule::SaturatingAdd(total, nanoseconds);
        return Duration(total);
    }

    std::int64_t TemporalModule::Duration::TotalNanoseconds() const noexcept {
        return totalNanoseconds;
    }

    TemporalModule::Metrics::Metrics() noexcept
        : instantAllocations(0),
          instantReleases(0),
          nowCalls(0),
          arithmeticOps(0),
          differenceOps(0),
          roundingOps(0),
          conversions(0),
          canonicalHits(0),
          canonicalMisses(0),
          lastFrameTouched(0),
          activeInstants(0),
          peakInstants(0),
          gpuOptimized(false) {
    }

    TemporalModule::TemporalModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_TotalSeconds(0.0),
          m_Slots(),
          m_FreeSlots(),
          m_Metrics(),
          m_CanonicalInstant(kInvalidHandle) {
    }

    std::string_view TemporalModule::Name() const noexcept {
        return kName;
    }

    std::string_view TemporalModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view TemporalModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void TemporalModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_CurrentFrame = 0;
        m_TotalSeconds = 0.0;
        ResetPools(RecommendInstantCapacity(context.config));
        Handle canonical = kInvalidHandle;
        (void) AllocateInstant("temporal.zero", 0, true, canonical);
        m_CanonicalInstant = canonical;
        m_Initialized = true;
    }

    void TemporalModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_TotalSeconds += info.deltaSeconds;
    }

    void TemporalModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
        (void) context;
    }

    void TemporalModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
        EnsureCapacity(RecommendInstantCapacity(config));
    }

    StatusCode TemporalModule::CreateInstant(std::string_view label,
                                             std::int64_t epochNanoseconds,
                                             Handle &outHandle,
                                             bool pinned) {
        return AllocateInstant(label, epochNanoseconds, pinned, outHandle);
    }

    StatusCode TemporalModule::CreateInstant(const PlainDateTime &dateTime,
                                             std::int32_t offsetMinutes,
                                             std::string_view label,
                                             Handle &outHandle) {
        std::int64_t days = DaysFromCivil(dateTime.year, dateTime.month, dateTime.day);
        std::int64_t timeOfDay = 0;
        timeOfDay = SaturatingAdd(timeOfDay, SaturatingMul(dateTime.hour, kNanosPerHour));
        timeOfDay = SaturatingAdd(timeOfDay, SaturatingMul(dateTime.minute, kNanosPerMinute));
        timeOfDay = SaturatingAdd(timeOfDay, SaturatingMul(dateTime.second, kNanosPerSecond));
        timeOfDay = SaturatingAdd(timeOfDay, SaturatingMul(dateTime.millisecond, kNanosPerMillisecond));
        timeOfDay = SaturatingAdd(timeOfDay, SaturatingMul(dateTime.microsecond, kNanosPerMicrosecond));
        timeOfDay = SaturatingAdd(timeOfDay, dateTime.nanosecond);
        auto epoch = SaturatingAdd(SaturatingMul(days, kNanosPerDay), timeOfDay);
        auto offset = SaturatingMul(offsetMinutes, kNanosPerMinute);
        epoch = SaturatingSub(epoch, offset);
        auto status = AllocateInstant(label, epoch, false, outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.conversions += 1;
            m_Metrics.lastFrameTouched = m_CurrentFrame;
        }
        return status;
    }

    StatusCode TemporalModule::Now(std::string_view label, Handle &outHandle) {
        auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        auto epoch = static_cast<std::int64_t>(now.time_since_epoch().count());
        auto status = AllocateInstant(label, epoch, false, outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.nowCalls += 1;
            m_Metrics.lastFrameTouched = m_CurrentFrame;
        }
        return status;
    }

    StatusCode TemporalModule::Clone(Handle source, std::string_view label, Handle &outHandle) {
        outHandle = kInvalidHandle;
        auto *record = Resolve(source);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto chosen = label.empty() ? std::string_view(record->label.data(), record->labelLength) : label;
        auto status = AllocateInstant(chosen, record->epochNanoseconds, false, outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.lastFrameTouched = m_CurrentFrame;
        }
        return status;
    }

    StatusCode TemporalModule::Destroy(Handle handle) {
        auto *record = Resolve(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (record->pinned) {
            return StatusCode::InvalidArgument;
        }
        ReleaseSlot(record->slot);
        m_Metrics.instantReleases += 1;
        if (m_Metrics.activeInstants > 0) {
            m_Metrics.activeInstants -= 1;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode TemporalModule::AddDuration(Handle handle,
                                           const Duration &duration,
                                           std::string_view label,
                                           Handle &outHandle) {
        outHandle = kInvalidHandle;
        auto *record = Resolve(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto epoch = SaturatingAdd(record->epochNanoseconds, duration.totalNanoseconds);
        auto chosen = label.empty() ? std::string_view("temporal.derived") : label;
        auto status = AllocateInstant(chosen, epoch, false, outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.arithmeticOps += 1;
            m_Metrics.lastFrameTouched = m_CurrentFrame;
        }
        return status;
    }

    StatusCode TemporalModule::AddDurationInPlace(Handle handle, const Duration &duration) {
        auto *record = Resolve(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        if (record->pinned) {
            return StatusCode::InvalidArgument;
        }
        record->epochNanoseconds = SaturatingAdd(record->epochNanoseconds, duration.totalNanoseconds);
        record->lastFrame = m_CurrentFrame;
        record->lastSeconds = m_TotalSeconds;
        m_Metrics.arithmeticOps += 1;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode TemporalModule::Difference(Handle start, Handle end, Duration &outDuration) noexcept {
        auto *startRecord = Resolve(start);
        auto *endRecord = Resolve(end);
        if (!startRecord || !endRecord) {
            outDuration = Duration();
            return StatusCode::NotFound;
        }
        auto diff = SaturatingSub(endRecord->epochNanoseconds, startRecord->epochNanoseconds);
        outDuration = Duration(diff);
        m_Metrics.differenceOps += 1;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode TemporalModule::Round(Handle handle,
                                     std::int64_t increment,
                                     Unit unit,
                                     RoundingMode mode,
                                     std::string_view label,
                                     Handle &outHandle) {
        outHandle = kInvalidHandle;
        if (increment <= 0) {
            return StatusCode::InvalidArgument;
        }
        auto *record = Resolve(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto unitSize = UnitToNanoseconds(unit);
        auto quantum = SaturatingMul(increment, unitSize);
        if (quantum <= 0) {
            return StatusCode::InvalidArgument;
        }
        auto rounded = RoundValue(record->epochNanoseconds, quantum, mode);
        auto chosen = label.empty() ? std::string_view("temporal.rounded") : label;
        auto status = AllocateInstant(chosen, rounded, false, outHandle);
        if (status == StatusCode::Ok) {
            m_Metrics.roundingOps += 1;
            m_Metrics.lastFrameTouched = m_CurrentFrame;
        }
        return status;
    }

    StatusCode TemporalModule::ToPlainDateTime(Handle handle,
                                               std::int32_t offsetMinutes,
                                               PlainDateTime &outDateTime) noexcept {
        auto *record = Resolve(handle);
        if (!record) {
            return StatusCode::NotFound;
        }
        auto offset = SaturatingMul(static_cast<std::int64_t>(offsetMinutes), kNanosPerMinute);
        auto adjusted = SaturatingAdd(record->epochNanoseconds, offset);
        auto days = FloorDiv(adjusted, kNanosPerDay);
        auto dayRemainder = FloorMod(adjusted, kNanosPerDay);
        int year = 1970;
        unsigned month = 1;
        unsigned day = 1;
        CivilFromDays(days, year, month, day);
        outDateTime.year = year;
        outDateTime.month = static_cast<int>(month);
        outDateTime.day = static_cast<int>(day);
        outDateTime.hour = static_cast<int>(dayRemainder / kNanosPerHour);
        dayRemainder %= kNanosPerHour;
        outDateTime.minute = static_cast<int>(dayRemainder / kNanosPerMinute);
        dayRemainder %= kNanosPerMinute;
        outDateTime.second = static_cast<int>(dayRemainder / kNanosPerSecond);
        dayRemainder %= kNanosPerSecond;
        outDateTime.millisecond = static_cast<int>(dayRemainder / kNanosPerMillisecond);
        dayRemainder %= kNanosPerMillisecond;
        outDateTime.microsecond = static_cast<int>(dayRemainder / kNanosPerMicrosecond);
        dayRemainder %= kNanosPerMicrosecond;
        outDateTime.nanosecond = static_cast<int>(dayRemainder);
        m_Metrics.conversions += 1;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode TemporalModule::FromPlainDateTime(const PlainDateTime &dateTime,
                                                 std::int32_t offsetMinutes,
                                                 std::string_view label,
                                                 Handle &outHandle) {
        return CreateInstant(dateTime, offsetMinutes, label, outHandle);
    }

    StatusCode TemporalModule::Normalize(Duration &duration) const noexcept {
        const auto minValue = std::numeric_limits<std::int64_t>::min();
        const auto maxValue = std::numeric_limits<std::int64_t>::max();
        if (duration.totalNanoseconds < minValue) {
            duration.totalNanoseconds = minValue;
        } else if (duration.totalNanoseconds > maxValue) {
            duration.totalNanoseconds = maxValue;
        }
        return StatusCode::Ok;
    }

    TemporalModule::DurationBreakdown TemporalModule::Breakdown(const Duration &duration) const noexcept {
        DurationBreakdown breakdown{};
        auto total = duration.totalNanoseconds;
        bool negative = total < 0;
        std::uint64_t absTotal = negative
                                     ? 0ULL - static_cast<std::uint64_t>(total)
                                     : static_cast<std::uint64_t>(total);
        breakdown.days = static_cast<std::int64_t>(absTotal / static_cast<std::uint64_t>(kNanosPerDay));
        absTotal %= static_cast<std::uint64_t>(kNanosPerDay);
        breakdown.hours = static_cast<int>(absTotal / static_cast<std::uint64_t>(kNanosPerHour));
        absTotal %= static_cast<std::uint64_t>(kNanosPerHour);
        breakdown.minutes = static_cast<int>(absTotal / static_cast<std::uint64_t>(kNanosPerMinute));
        absTotal %= static_cast<std::uint64_t>(kNanosPerMinute);
        breakdown.seconds = static_cast<int>(absTotal / static_cast<std::uint64_t>(kNanosPerSecond));
        absTotal %= static_cast<std::uint64_t>(kNanosPerSecond);
        breakdown.milliseconds = static_cast<int>(absTotal / static_cast<std::uint64_t>(kNanosPerMillisecond));
        absTotal %= static_cast<std::uint64_t>(kNanosPerMillisecond);
        breakdown.microseconds = static_cast<int>(absTotal / static_cast<std::uint64_t>(kNanosPerMicrosecond));
        absTotal %= static_cast<std::uint64_t>(kNanosPerMicrosecond);
        breakdown.nanoseconds = static_cast<int>(absTotal);
        if (negative) {
            breakdown.days = -breakdown.days;
            breakdown.hours = -breakdown.hours;
            breakdown.minutes = -breakdown.minutes;
            breakdown.seconds = -breakdown.seconds;
            breakdown.milliseconds = -breakdown.milliseconds;
            breakdown.microseconds = -breakdown.microseconds;
            breakdown.nanoseconds = -breakdown.nanoseconds;
        }
        return breakdown;
    }

    bool TemporalModule::Has(Handle handle) const noexcept {
        return const_cast<TemporalModule *>(this)->Resolve(handle) != nullptr;
    }

    std::int64_t TemporalModule::EpochNanoseconds(Handle handle, std::int64_t fallback) const noexcept {
        auto *record = const_cast<TemporalModule *>(this)->Resolve(handle);
        return record ? record->epochNanoseconds : fallback;
    }

    TemporalModule::Handle TemporalModule::CanonicalEpoch() noexcept {
        if (!Has(m_CanonicalInstant)) {
            Handle handle = kInvalidHandle;
            if (AllocateInstant("temporal.zero", 0, true, handle) == StatusCode::Ok) {
                m_CanonicalInstant = handle;
            }
        }
        if (Has(m_CanonicalInstant)) {
            m_Metrics.canonicalHits += 1;
        } else {
            m_Metrics.canonicalMisses += 1;
        }
        return m_CanonicalInstant;
    }

    const TemporalModule::Metrics &TemporalModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool TemporalModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    TemporalModule::Handle TemporalModule::MakeHandle(std::uint32_t slot, std::uint16_t generation) noexcept {
        auto encoded = static_cast<std::uint32_t>(generation) << kHandleIndexBits;
        encoded |= (slot & kHandleIndexMask);
        if (encoded == 0) {
            encoded = 1;
        }
        return static_cast<Handle>(encoded);
    }

    std::uint32_t TemporalModule::ExtractSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & kHandleIndexMask);
    }

    std::uint16_t TemporalModule::ExtractGeneration(Handle handle) noexcept {
        return static_cast<std::uint16_t>((handle >> kHandleIndexBits) & kHandleIndexMask);
    }

    void TemporalModule::ResetPools(std::size_t targetCapacity) {
        if (targetCapacity == 0) {
            targetCapacity = 32;
        }
        if (targetCapacity > kMaxSlots) {
            targetCapacity = kMaxSlots;
        }
        m_Slots.clear();
        m_FreeSlots.clear();
        m_Slots.resize(targetCapacity);
        m_FreeSlots.reserve(targetCapacity);
        for (std::size_t i = 0; i < targetCapacity; ++i) {
            auto slotIndex = static_cast<std::uint32_t>(targetCapacity - 1 - i);
            auto &slot = m_Slots[slotIndex];
            slot.generation = 1;
            slot.inUse = false;
            slot.record.handle = kInvalidHandle;
            slot.record.slot = slotIndex;
            slot.record.generation = slot.generation;
            slot.record.epochNanoseconds = 0;
            slot.record.lastFrame = 0;
            slot.record.lastSeconds = 0.0;
            slot.record.pinned = false;
            slot.record.active = false;
            slot.record.labelLength = 0;
            slot.record.label[0] = '\0';
            m_FreeSlots.push_back(slotIndex);
        }
        m_Metrics.activeInstants = 0;
        m_Metrics.peakInstants = 0;
        m_CanonicalInstant = kInvalidHandle;
    }

    void TemporalModule::EnsureCapacity(std::size_t desiredCapacity) {
        if (desiredCapacity <= m_Slots.size()) {
            return;
        }
        if (desiredCapacity > kMaxSlots) {
            desiredCapacity = kMaxSlots;
        }
        auto currentSize = m_Slots.size();
        m_Slots.resize(desiredCapacity);
        m_FreeSlots.reserve(desiredCapacity);
        for (std::size_t index = currentSize; index < desiredCapacity; ++index) {
            auto slotIndex = static_cast<std::uint32_t>(desiredCapacity - 1 - (index - currentSize));
            auto &slot = m_Slots[slotIndex];
            slot.generation = 1;
            slot.inUse = false;
            slot.record.handle = kInvalidHandle;
            slot.record.slot = slotIndex;
            slot.record.generation = slot.generation;
            slot.record.epochNanoseconds = 0;
            slot.record.lastFrame = 0;
            slot.record.lastSeconds = 0.0;
            slot.record.pinned = false;
            slot.record.active = false;
            slot.record.labelLength = 0;
            slot.record.label[0] = '\0';
            m_FreeSlots.push_back(slotIndex);
        }
    }

    void TemporalModule::ReleaseSlot(std::uint32_t slotIndex) noexcept {
        auto &slot = m_Slots[slotIndex];
        slot.inUse = false;
        slot.generation = static_cast<std::uint16_t>(slot.generation + 1);
        if (slot.generation == 0) {
            slot.generation = 1;
        }
        slot.record.handle = kInvalidHandle;
        slot.record.generation = slot.generation;
        slot.record.active = false;
        slot.record.pinned = false;
        slot.record.labelLength = 0;
        slot.record.label[0] = '\0';
        m_FreeSlots.push_back(slotIndex);
    }

    TemporalModule::InstantRecord *TemporalModule::Resolve(Handle handle) noexcept {
        if (handle == kInvalidHandle) {
            return nullptr;
        }
        auto slotIndex = ExtractSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse) {
            return nullptr;
        }
        if (slot.generation != ExtractGeneration(handle)) {
            return nullptr;
        }
        return &slot.record;
    }

    const TemporalModule::InstantRecord *TemporalModule::Resolve(Handle handle) const noexcept {
        return const_cast<TemporalModule *>(this)->Resolve(handle);
    }

    void TemporalModule::CopyLabel(std::string_view text,
                                   std::array<char, kMaxLabelLength + 1> &dest,
                                   std::uint8_t &outLength) noexcept {
        const auto count = static_cast<std::size_t>(std::min<std::size_t>(text.size(), kMaxLabelLength));
        if (count > 0) {
            std::memcpy(dest.data(), text.data(), count);
        }
        dest[count] = '\0';
        outLength = static_cast<std::uint8_t>(count);
    }

    StatusCode TemporalModule::AllocateInstant(std::string_view label,
                                               std::int64_t epochNanoseconds,
                                               bool pinned,
                                               Handle &outHandle) {
        outHandle = kInvalidHandle;
        if (m_FreeSlots.empty()) {
            EnsureCapacity(std::min<std::size_t>(m_Slots.size() * 2 + 1, static_cast<std::size_t>(kMaxSlots)));
            if (m_FreeSlots.empty()) {
                return StatusCode::CapacityExceeded;
            }
        }
        auto slotIndex = m_FreeSlots.back();
        m_FreeSlots.pop_back();
        auto &slot = m_Slots[slotIndex];
        if (slot.inUse) {
            return StatusCode::InternalError;
        }
        slot.inUse = true;
        slot.record.handle = MakeHandle(slotIndex, slot.generation);
        slot.record.slot = slotIndex;
        slot.record.generation = slot.generation;
        slot.record.epochNanoseconds = epochNanoseconds;
        slot.record.lastFrame = m_CurrentFrame;
        slot.record.lastSeconds = m_TotalSeconds;
        slot.record.pinned = pinned;
        slot.record.active = true;
        CopyLabel(label, slot.record.label, slot.record.labelLength);
        m_Metrics.instantAllocations += 1;
        m_Metrics.activeInstants += 1;
        if (m_Metrics.activeInstants > m_Metrics.peakInstants) {
            m_Metrics.peakInstants = m_Metrics.activeInstants;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        outHandle = slot.record.handle;
        return StatusCode::Ok;
    }

    std::int64_t TemporalModule::SaturatingAdd(std::int64_t a, std::int64_t b) noexcept {
        if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
            return std::numeric_limits<std::int64_t>::max();
        }
        if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return a + b;
    }

    std::int64_t TemporalModule::SaturatingSub(std::int64_t a, std::int64_t b) noexcept {
        if (b == std::numeric_limits<std::int64_t>::min()) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return SaturatingAdd(a, -b);
    }

    std::int64_t TemporalModule::SaturatingMul(std::int64_t a, std::int64_t b) noexcept {
        using Limits = std::numeric_limits<std::int64_t>;
        if (a == 0 || b == 0) {
            return 0;
        }
        bool negative = (a < 0) ^ (b < 0);
        auto absA = a < 0 ? 0ULL - static_cast<std::uint64_t>(a) : static_cast<std::uint64_t>(a);
        auto absB = b < 0 ? 0ULL - static_cast<std::uint64_t>(b) : static_cast<std::uint64_t>(b);
        const auto maxPositive = static_cast<std::uint64_t>(Limits::max());
        const auto maxNegative = maxPositive + 1ULL;
        if (!negative) {
            if (absA > 0 && absB > maxPositive / absA) {
                return Limits::max();
            }
            auto absResult = absA * absB;
            if (absResult > maxPositive) {
                return Limits::max();
            }
            return static_cast<std::int64_t>(absResult);
        }
        if (absA > 0 && absB > maxNegative / absA) {
            return Limits::min();
        }
        auto absResult = absA * absB;
        if (absResult > maxNegative) {
            return Limits::min();
        }
        if (absResult == maxNegative) {
            return Limits::min();
        }
        return -static_cast<std::int64_t>(absResult);
    }

    std::int64_t TemporalModule::SaturatingNegate(std::int64_t value) noexcept {
        if (value == std::numeric_limits<std::int64_t>::min()) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return -value;
    }

    std::int64_t TemporalModule::UnitToNanoseconds(Unit unit) noexcept {
        switch (unit) {
            case Unit::Nanosecond:
                return 1;
            case Unit::Microsecond:
                return kNanosPerMicrosecond;
            case Unit::Millisecond:
                return kNanosPerMillisecond;
            case Unit::Second:
                return kNanosPerSecond;
            case Unit::Minute:
                return kNanosPerMinute;
            case Unit::Hour:
                return kNanosPerHour;
            case Unit::Day:
                return kNanosPerDay;
        }
        return 1;
    }

    std::int64_t TemporalModule::RoundValue(std::int64_t value,
                                            std::int64_t quantum,
                                            RoundingMode mode) noexcept {
        if (quantum <= 0) {
            return value;
        }
        switch (mode) {
            case RoundingMode::Trunc: {
                return (value / quantum) * quantum;
            }
            case RoundingMode::Floor: {
                auto quotient = FloorDiv(value, quantum);
                return SaturatingMul(quotient, quantum);
            }
            case RoundingMode::Ceil: {
                auto quotient = FloorDiv(value, quantum);
                auto rounded = SaturatingMul(quotient, quantum);
                if (rounded != value) {
                    rounded = SaturatingAdd(rounded, quantum);
                }
                return rounded;
            }
            case RoundingMode::HalfExpand: {
                auto floorQ = FloorDiv(value, quantum);
                auto floorValue = SaturatingMul(floorQ, quantum);
                auto remainder = SaturatingSub(value, floorValue);
                auto twice = SaturatingMul(remainder, 2);
                if (twice > quantum || (twice == quantum && value >= 0)) {
                    floorValue = SaturatingAdd(floorValue, quantum);
                }
                if (twice < -quantum || (twice == -quantum && value < 0)) {
                    floorValue = SaturatingSub(floorValue, quantum);
                }
                return floorValue;
            }
        }
        return value;
    }
}
