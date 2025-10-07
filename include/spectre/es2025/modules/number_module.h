#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class NumberModule final : public Module {
    public:
        using Handle = std::uint64_t;

        struct Metrics {
            std::uint64_t canonicalHits;
            std::uint64_t canonicalMisses;
            std::uint64_t allocations;
            std::uint64_t releases;
            std::uint64_t mutations;
            std::uint64_t normalizations;
            std::uint64_t accumulations;
            std::uint64_t saturations;
            std::uint64_t hotNumbers;
            std::uint64_t lastFrameTouched;
            double minObserved;
            double maxObserved;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        struct Statistics {
            double mean;
            double variance;
            double minValue;
            double maxValue;
        };

        NumberModule();

        std::string_view Name() const noexcept override;

        std::string_view Summary() const noexcept override;

        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;

        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;

        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;

        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(std::string_view label, double value, Handle &outHandle);

        StatusCode Clone(Handle handle, std::string_view label, Handle &outHandle);

        StatusCode Destroy(Handle handle);

        StatusCode Set(Handle handle, double value) noexcept;

        StatusCode Add(Handle handle, double delta) noexcept;

        StatusCode Multiply(Handle handle, double factor) noexcept;

        StatusCode Saturate(Handle handle, double minValue, double maxValue) noexcept;

        double ValueOf(Handle handle, double fallback = 0.0) const noexcept;

        bool Has(Handle handle) const noexcept;

        StatusCode Accumulate(const double *values, std::size_t count, double &outSum) const noexcept;

        StatusCode Normalize(double *values, std::size_t count, double targetMin, double targetMax) const noexcept;

        StatusCode BuildStatistics(const double *values, std::size_t count, Statistics &outStats) const noexcept;

        Handle Canonical(double value) noexcept;

        const Metrics &GetMetrics() const noexcept;

        bool GpuEnabled() const noexcept;

    private:
        struct alignas(64) Entry {
            Handle handle;
            std::uint32_t slot;
            std::uint32_t generation;
            double value;
            double filtered;
            std::uint64_t version;
            std::uint64_t lastTouchFrame;
            std::string label;
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
        Handle m_CanonicalZero;
        Handle m_CanonicalOne;
        Handle m_CanonicalNaN;
        Handle m_CanonicalInfinity;
        mutable Metrics m_Metrics;

        Entry *FindMutable(Handle handle) noexcept;

        const Entry *Find(Handle handle) const noexcept;

        static std::uint32_t DecodeSlot(Handle handle) noexcept;

        static std::uint32_t DecodeGeneration(Handle handle) noexcept;

        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;

        StatusCode CreateInternal(std::string_view label, double value, bool pinned, Handle &outHandle);

        void Reset();

        void Touch(Entry &entry) noexcept;

        void RecomputeHotMetrics() noexcept;

        void Observe(double value) const noexcept;
    };
}