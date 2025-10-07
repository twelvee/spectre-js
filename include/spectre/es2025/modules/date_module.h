#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class DateModule final : public Module {
    public:
        using Handle = std::uint64_t;

        struct Metrics {
            std::uint64_t allocations;
            std::uint64_t releases;
            std::uint64_t canonicalHits;
            std::uint64_t canonicalMisses;
            std::uint64_t epochConstructs;
            std::uint64_t componentConstructs;
            std::uint64_t nowCalls;
            std::uint64_t componentConversions;
            std::uint64_t isoFormats;
            std::uint64_t isoParses;
            std::uint64_t arithmeticOps;
            std::uint64_t lastFrameTouched;
            std::uint64_t hotDates;
            bool gpuOptimized;
            Metrics() noexcept;
        };

        struct Components {
            int year;
            int month;
            int day;
            int hour;
            int minute;
            int second;
            int millisecond;
            int dayOfWeek;
            int dayOfYear;
        };

        DateModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode CreateFromEpochMilliseconds(std::string_view label, std::int64_t epochMs, Handle &outHandle);
        StatusCode CreateFromComponents(std::string_view label,
                                        int year,
                                        int month,
                                        int day,
                                        int hour,
                                        int minute,
                                        int second,
                                        int millisecond,
                                        Handle &outHandle);
        StatusCode Now(std::string_view label, Handle &outHandle);
        StatusCode Clone(Handle handle, std::string_view label, Handle &outHandle);
        StatusCode Destroy(Handle handle);

        StatusCode SetEpochMilliseconds(Handle handle, std::int64_t epochMs) noexcept;
        StatusCode AddMilliseconds(Handle handle, std::int64_t delta) noexcept;
        StatusCode AddDays(Handle handle, std::int32_t days) noexcept;

        StatusCode ToComponents(Handle handle, Components &outComponents) const;
        StatusCode FormatIso8601(Handle handle, std::string &outText) const;
        StatusCode ParseIso8601(std::string_view text, std::string_view label, Handle &outHandle);
        StatusCode DifferenceMilliseconds(Handle left, Handle right, std::int64_t &outDelta) const noexcept;

        bool Has(Handle handle) const noexcept;
        std::int64_t EpochMilliseconds(Handle handle, std::int64_t fallback = 0) const noexcept;

        Handle CanonicalEpoch() noexcept;

        const Metrics &GetMetrics() const noexcept;
        bool GpuEnabled() const noexcept;

    private:
        static constexpr std::int64_t kMillisPerSecond = 1000;
        static constexpr std::int64_t kMillisPerMinute = kMillisPerSecond * 60;
        static constexpr std::int64_t kMillisPerHour = kMillisPerMinute * 60;
        static constexpr std::int64_t kMillisPerDay = kMillisPerHour * 24;
        static constexpr std::size_t kHotFrameWindow = 10;

        struct Entry {
            Handle handle;
            std::uint32_t slot;
            std::uint32_t generation;
            std::int64_t epochMilliseconds;
            Components components;
            std::uint64_t version;
            std::uint64_t lastTouchFrame;
            std::string label;
            bool componentsDirty;
            bool hot;
            bool pinned;
        };

        struct Slot {
            Entry entry;
            std::uint32_t generation;
            bool inUse;
        };

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        Handle m_CanonicalEpoch;
        mutable Metrics m_Metrics;

        Entry *FindMutable(Handle handle) noexcept;
        const Entry *Find(Handle handle) const noexcept;

        static std::uint32_t DecodeSlot(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;
        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;

        StatusCode CreateInternal(std::string_view label, std::int64_t epochMs, bool pinned, Handle &outHandle);
        void Reset();
        void Touch(Entry &entry) noexcept;
        void RecomputeHotMetrics() noexcept;
        void EnsureComponents(Entry &entry) const;
        static bool ValidateComponents(int year,
                                       int month,
                                       int day,
                                       int hour,
                                       int minute,
                                       int second,
                                       int millisecond) noexcept;
        static std::int64_t CivilToEpochMilliseconds(int year,
                                                     int month,
                                                     int day,
                                                     int hour,
                                                     int minute,
                                                     int second,
                                                     int millisecond) noexcept;
        static void EpochMillisecondsToCivil(std::int64_t epochMs, Components &outComponents) noexcept;
        static bool ParseIso8601Internal(std::string_view text, Components &outComponents) noexcept;
    };
}

