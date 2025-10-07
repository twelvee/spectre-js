#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class BooleanModule final : public Module {
    public:
        using Handle = std::uint64_t;

        struct CacheMetrics {
            std::uint64_t conversions;
            std::uint64_t canonicalHits;
            std::uint64_t allocations;
            std::uint64_t activeBoxes;
            std::uint64_t toggles;
            std::uint64_t lastFrameTouched;
            std::uint64_t hotBoxes;
            bool gpuOptimized;

            CacheMetrics() noexcept;
        };

        BooleanModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        bool ToBoolean(double value) const noexcept;
        bool ToBoolean(std::int64_t value) const noexcept;
        bool ToBoolean(std::string_view text) const noexcept;

        Handle Box(bool value) noexcept;
        StatusCode Create(std::string_view label, bool value, Handle &outHandle);
        StatusCode Destroy(Handle handle);

        StatusCode Set(Handle handle, bool value) noexcept;
        StatusCode Toggle(Handle handle) noexcept;
        StatusCode ValueOf(Handle handle, bool &outValue) const noexcept;

        bool Has(Handle handle) const noexcept;

        const CacheMetrics &Metrics() const noexcept;
        bool GpuEnabled() const noexcept;

    private:
        struct Entry {
            Handle handle;
            std::uint32_t slot;
            std::uint32_t generation;
            std::string label;
            bool value;
            bool pinned;
            std::uint64_t version;
            std::uint64_t lastTouchFrame;
            bool hot;
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
        Handle m_CanonicalTrue;
        Handle m_CanonicalFalse;
        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        CacheMetrics m_Metrics;

        Entry *FindMutable(Handle handle) noexcept;
        const Entry *Find(Handle handle) const noexcept;

        static std::uint32_t DecodeSlot(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;
        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;

        void Reset();
        StatusCode CreateInternal(std::string_view label, bool value, bool pinned, Handle &outHandle);

        void Touch(Entry &entry) noexcept;
        void RecomputeHotMetrics() noexcept;
    };
}
