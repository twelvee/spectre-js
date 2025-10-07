#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class BigIntModule final : public Module {
    public:
        using Handle = std::uint64_t;

        struct Metrics {
            std::uint64_t canonicalHits;
            std::uint64_t canonicalMisses;
            std::uint64_t allocations;
            std::uint64_t releases;
            std::uint64_t additions;
            std::uint64_t multiplications;
            std::uint64_t conversions;
            std::uint64_t normalizedDigits;
            std::uint64_t hotIntegers;
            std::uint64_t lastFrameTouched;
            std::uint64_t maxDigits;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        struct ComparisonResult {
            int sign;
            std::size_t digits;
        };

        BigIntModule();

        std::string_view Name() const noexcept override;

        std::string_view Summary() const noexcept override;

        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;

        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;

        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;

        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(std::string_view label, std::int64_t value, Handle &outHandle);

        StatusCode CreateFromDecimal(std::string_view label, std::string_view text, Handle &outHandle);

        StatusCode Clone(Handle handle, std::string_view label, Handle &outHandle);

        StatusCode Destroy(Handle handle);

        StatusCode Add(Handle target, Handle addend);

        StatusCode AddSigned(Handle target, std::int64_t delta);

        StatusCode MultiplySmall(Handle handle, std::uint32_t factor);

        StatusCode ShiftLeft(Handle handle, std::size_t bits);

        ComparisonResult Compare(Handle left, Handle right) const noexcept;

        bool IsZero(Handle handle) const noexcept;

        StatusCode ToDecimalString(Handle handle, std::string &outText) const;

        StatusCode ToUint64(Handle handle, std::uint64_t &outValue) const noexcept;

        Handle Canonical(std::int64_t value);

        const Metrics &GetMetrics() const noexcept;

        bool GpuEnabled() const noexcept;

    private:
        static constexpr std::uint32_t kLimbBits = 32;
        static constexpr std::uint64_t kLimbBase = 1ull << kLimbBits;
        static constexpr std::size_t kHotFrameWindow = 12;

        struct Entry {
            Handle handle;
            std::uint32_t slot;
            std::uint32_t generation;
            bool negative;
            std::vector<std::uint32_t> limbs;
            std::string label;
            std::uint64_t version;
            std::uint64_t lastTouchFrame;
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
        Handle m_CanonicalMinusOne;
        mutable Metrics m_Metrics;

        Entry *FindMutable(Handle handle) noexcept;

        const Entry *Find(Handle handle) const noexcept;

        static std::uint32_t DecodeSlot(Handle handle) noexcept;

        static std::uint32_t DecodeGeneration(Handle handle) noexcept;

        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;

        StatusCode CreateInternal(std::string_view label, std::int64_t value, bool pinned, Handle &outHandle);

        StatusCode CreateFromLimbs(std::string_view label, const std::vector<std::uint32_t> &limbs, bool negative,
                                   bool pinned, Handle &outHandle);

        void Reset();

        void Normalize(Entry &entry) noexcept;

        void Touch(Entry &entry) noexcept;

        void RecomputeHotMetrics() noexcept;

        void AddLimbs(Entry &target, const Entry &source);

        void SubtractLimbs(Entry &target, const Entry &source);

        int CompareAbs(const Entry &left, const Entry &right) const noexcept;

        void AddSignedSmall(Entry &target, std::int64_t delta);

        void MultiplySmallInternal(Entry &entry, std::uint32_t factor);

        void ShiftLeftInternal(Entry &entry, std::size_t bits);

        int CompareInternal(const Entry &left, const Entry &right) const noexcept;

        void DecimalFromEntry(const Entry &entry, std::string &outText) const;

        void ObserveDigits(std::size_t digits) noexcept;
    };
}