#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class TemporalModule final : public Module {
    public:
        using Handle = std::uint32_t;
        static constexpr Handle kInvalidHandle = 0;
        static constexpr std::size_t kMaxLabelLength = 47;

        struct Duration {
            std::int64_t totalNanoseconds;

            Duration() noexcept;
            explicit Duration(std::int64_t nanos) noexcept;

            static Duration FromComponents(std::int64_t days,
                                           std::int64_t hours,
                                           std::int64_t minutes,
                                           std::int64_t seconds,
                                           std::int64_t milliseconds,
                                           std::int64_t microseconds,
                                           std::int64_t nanoseconds) noexcept;

            std::int64_t TotalNanoseconds() const noexcept;
        };

        struct DurationBreakdown {
            std::int64_t days;
            int hours;
            int minutes;
            int seconds;
            int milliseconds;
            int microseconds;
            int nanoseconds;
        };

        enum class Unit : std::uint8_t {
            Nanosecond,
            Microsecond,
            Millisecond,
            Second,
            Minute,
            Hour,
            Day
        };

        enum class RoundingMode : std::uint8_t {
            Trunc,
            Floor,
            Ceil,
            HalfExpand
        };

        struct PlainDateTime {
            int year;
            int month;
            int day;
            int hour;
            int minute;
            int second;
            int millisecond;
            int microsecond;
            int nanosecond;
        };

        struct Metrics {
            std::uint64_t instantAllocations;
            std::uint64_t instantReleases;
            std::uint64_t nowCalls;
            std::uint64_t arithmeticOps;
            std::uint64_t differenceOps;
            std::uint64_t roundingOps;
            std::uint64_t conversions;
            std::uint64_t canonicalHits;
            std::uint64_t canonicalMisses;
            std::uint64_t lastFrameTouched;
            std::size_t activeInstants;
            std::size_t peakInstants;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        TemporalModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode CreateInstant(std::string_view label,
                                 std::int64_t epochNanoseconds,
                                 Handle &outHandle,
                                 bool pinned = false);

        StatusCode CreateInstant(const PlainDateTime &dateTime,
                                 std::int32_t offsetMinutes,
                                 std::string_view label,
                                 Handle &outHandle);

        StatusCode Now(std::string_view label, Handle &outHandle);

        StatusCode Clone(Handle source, std::string_view label, Handle &outHandle);

        StatusCode Destroy(Handle handle);

        StatusCode AddDuration(Handle handle,
                               const Duration &duration,
                               std::string_view label,
                               Handle &outHandle);

        StatusCode AddDurationInPlace(Handle handle, const Duration &duration);

        StatusCode Difference(Handle start, Handle end, Duration &outDuration) noexcept;

        StatusCode Round(Handle handle,
                         std::int64_t increment,
                         Unit unit,
                         RoundingMode mode,
                         std::string_view label,
                         Handle &outHandle);

        StatusCode ToPlainDateTime(Handle handle,
                                   std::int32_t offsetMinutes,
                                   PlainDateTime &outDateTime) noexcept;

        StatusCode FromPlainDateTime(const PlainDateTime &dateTime,
                                     std::int32_t offsetMinutes,
                                     std::string_view label,
                                     Handle &outHandle);

        StatusCode Normalize(Duration &duration) const noexcept;

        DurationBreakdown Breakdown(const Duration &duration) const noexcept;

        bool Has(Handle handle) const noexcept;
        std::int64_t EpochNanoseconds(Handle handle, std::int64_t fallback = 0) const noexcept;
        Handle CanonicalEpoch() noexcept;
        const Metrics &GetMetrics() const noexcept;
        bool GpuEnabled() const noexcept;

    private:
        struct InstantRecord {
            Handle handle;
            std::uint32_t slot;
            std::uint16_t generation;
            std::int64_t epochNanoseconds;
            std::uint64_t lastFrame;
            double lastSeconds;
            bool pinned;
            bool active;
            std::array<char, kMaxLabelLength + 1> label;
            std::uint8_t labelLength;
        };

        struct Slot {
            InstantRecord record;
            std::uint16_t generation;
            bool inUse;
        };

        static constexpr std::uint32_t kHandleIndexBits = 16;
        static constexpr std::uint32_t kHandleIndexMask = (1u << kHandleIndexBits) - 1;
        static constexpr std::uint32_t kMaxSlots = kHandleIndexMask;

        SpectreRuntime *m_Runtime;
        friend struct Duration;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        double m_TotalSeconds;
        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        Metrics m_Metrics;
        Handle m_CanonicalInstant;

        static Handle MakeHandle(std::uint32_t slot, std::uint16_t generation) noexcept;
        static std::uint32_t ExtractSlot(Handle handle) noexcept;
        static std::uint16_t ExtractGeneration(Handle handle) noexcept;

        void ResetPools(std::size_t targetCapacity);
        void EnsureCapacity(std::size_t desiredCapacity);
        void ReleaseSlot(std::uint32_t slotIndex) noexcept;
        InstantRecord *Resolve(Handle handle) noexcept;
        const InstantRecord *Resolve(Handle handle) const noexcept;
        static void CopyLabel(std::string_view text,
                              std::array<char, kMaxLabelLength + 1> &dest,
                              std::uint8_t &outLength) noexcept;

        StatusCode AllocateInstant(std::string_view label,
                                   std::int64_t epochNanoseconds,
                                   bool pinned,
                                   Handle &outHandle);

        static std::int64_t SaturatingAdd(std::int64_t a, std::int64_t b) noexcept;
        static std::int64_t SaturatingSub(std::int64_t a, std::int64_t b) noexcept;
        static std::int64_t SaturatingMul(std::int64_t a, std::int64_t b) noexcept;
        static std::int64_t SaturatingNegate(std::int64_t value) noexcept;
        static std::int64_t UnitToNanoseconds(Unit unit) noexcept;
        static std::int64_t RoundValue(std::int64_t value, std::int64_t quantum, RoundingMode mode) noexcept;
    };
}
