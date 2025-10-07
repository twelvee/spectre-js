#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/es2025/module.h"
#include "spectre/status.h"

namespace spectre::es2025 {
    class SymbolModule final : public Module {
    public:
        using Handle = std::uint64_t;

        enum class WellKnown : std::uint8_t {
            AsyncIterator,
            HasInstance,
            IsConcatSpreadable,
            Iterator,
            Match,
            MatchAll,
            Replace,
            Search,
            Species,
            Split,
            ToPrimitive,
            ToStringTag,
            Unscopables,
            Dispose,
            AsyncDispose,
            Metadata,
            Count
        };

        struct Metrics {
            std::uint64_t totalSymbols;
            std::uint64_t liveSymbols;
            std::uint64_t globalSymbols;
            std::uint64_t localSymbols;
            std::uint64_t wellKnownSymbols;
            std::uint64_t recycledSlots;
            std::uint64_t registryLookups;
            std::uint64_t registryHits;
            std::uint64_t registryMisses;
            std::uint64_t collisions;
            std::uint64_t lastFrameTouched;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        SymbolModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(std::string_view description, Handle &outHandle);
        StatusCode CreateUnique(Handle &outHandle);
        StatusCode CreateGlobal(std::string_view key, Handle &outHandle);
        StatusCode KeyFor(Handle handle, std::string &outKey) const;
        std::string_view Description(Handle handle) const noexcept;
        bool IsGlobal(Handle handle) const noexcept;
        bool IsPinned(Handle handle) const noexcept;
        bool IsValid(Handle handle) const noexcept;
        Handle WellKnownHandle(WellKnown kind) const noexcept;
        const Metrics &GetMetrics() const noexcept;
        bool GpuEnabled() const noexcept;

    private:
        struct alignas(64) Entry {
            Handle handle;
            std::uint64_t hash;
            std::uint64_t sequence;
            std::uint64_t version;
            std::uint64_t lastTouchFrame;
            std::string description;
            std::string key;
            bool global;
            bool pinned;
        };

        struct Slot {
            Entry entry;
            std::uint32_t generation;
            bool inUse;
        };

        static constexpr std::uint32_t kInvalidIndex = 0xffffffffu;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        std::uint64_t m_NextSequence;
        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        std::vector<std::uint32_t> m_GlobalBuckets;
        std::array<Handle, static_cast<std::size_t>(WellKnown::Count)> m_WellKnown;
        mutable Metrics m_Metrics;
        std::uint64_t m_GlobalCount;
        std::uint64_t m_LocalCount;

        StatusCode CreateInternal(std::string_view description,
                                  std::string_view key,
                                  bool global,
                                  bool pinned,
                                  std::uint64_t hash,
                                  Handle &outHandle,
                                  std::uint32_t &outSlot);

        void Reset();
        void Touch(Slot &slot) noexcept;
        void ObserveGlobal(bool collision) noexcept;
        void EnsureGlobalCapacity();
        void RehashGlobals(std::uint32_t request);
        std::uint32_t LocateGlobal(std::string_view key, std::uint64_t hash, std::uint32_t &bucket, bool &collision) const;
        Slot *FindMutable(Handle handle) noexcept;
        const Slot *Find(Handle handle) const noexcept;
        static std::uint64_t HashKey(std::string_view key) noexcept;
        static std::uint32_t NormalizeBuckets(std::uint32_t request) noexcept;
        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;
        static std::uint32_t DecodeSlot(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;
    };
}
