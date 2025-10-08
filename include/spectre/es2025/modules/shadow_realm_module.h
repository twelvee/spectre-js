#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"
#include "spectre/es2025/value.h"

namespace spectre::es2025 {
    class ShadowRealmModule final : public Module {
    public:
        using Handle = std::uint32_t;
        static constexpr Handle kInvalidHandle = 0;
        static constexpr std::size_t kMaxLabelLength = 47;
        static constexpr std::size_t kMaxExportNameLength = 31;
        static constexpr std::size_t kMaxExportsPerRealm = 32;

        struct Metrics {
            std::uint64_t created;
            std::uint64_t destroyed;
            std::uint64_t evaluations;
            std::uint64_t exports;
            std::uint64_t imports;
            std::uint64_t failedImports;
            std::uint64_t contextAllocs;
            std::uint64_t contextFailures;
            std::uint64_t reuseHits;
            std::uint64_t reuseMisses;
            std::uint64_t lastFrameTouched;
            std::size_t activeRealms;
            std::size_t peakRealms;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        ShadowRealmModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(std::string_view label, Handle &outHandle, std::uint32_t stackSize = 0);
        StatusCode Destroy(Handle handle);

        StatusCode Evaluate(Handle handle,
                            std::string_view source,
                            std::string &outValue,
                            std::string &outDiagnostics,
                            std::string_view scriptName = {}) noexcept;

        StatusCode ExportValue(Handle handle,
                               std::string_view exportName,
                               const Value &value) noexcept;

        StatusCode ImportValue(Handle targetRealm,
                               Handle sourceRealm,
                               std::string_view exportName,
                               Value &outValue) noexcept;

        StatusCode ClearExports(Handle handle) noexcept;

        StatusCode Describe(Handle handle,
                            std::string &outLabel,
                            std::string &outContextName,
                            std::uint64_t &outEvaluations) const;

        bool Has(Handle handle) const noexcept;
        std::size_t ActiveRealms() const noexcept;
        const Metrics &GetMetrics() const noexcept;
        bool GpuEnabled() const noexcept;

    private:
        struct ExportEntry {
            std::array<char, kMaxExportNameLength + 1> name;
            Value value;
            std::uint8_t length;
            bool inUse;
        };

        struct RealmRecord {
            Handle handle;
            std::uint32_t slot;
            std::uint16_t generation;
            std::uint32_t stackSize;
            std::uint64_t createdFrame;
            double createdSeconds;
            std::uint64_t evalCount;
            std::uint64_t importCount;
            std::uint64_t exportCount;
            bool pinned;
            bool active;
            std::array<char, kMaxLabelLength + 1> label;
            std::uint8_t labelLength;
            std::string contextName;
            std::string inlineScriptName;
            std::array<ExportEntry, kMaxExportsPerRealm> exports;
        };

        struct Slot {
            RealmRecord record;
            std::uint16_t generation;
            bool inUse;
        };

        static constexpr std::uint32_t kHandleIndexBits = 16;
        static constexpr std::uint32_t kHandleIndexMask = (1u << kHandleIndexBits) - 1;
        static constexpr std::uint32_t kMaxSlots = kHandleIndexMask;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        double m_TotalSeconds;
        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        Metrics m_Metrics;
        std::uint32_t m_DefaultStackSize;

        static Handle MakeHandle(std::uint32_t slot, std::uint16_t generation) noexcept;
        static std::uint32_t ExtractSlot(Handle handle) noexcept;
        static std::uint16_t ExtractGeneration(Handle handle) noexcept;

        void ResetAllocationPools(std::size_t targetCapacity);
        void EnsureCapacity(std::size_t desiredCapacity);
        void ReleaseSlot(std::uint32_t slotIndex) noexcept;
        RealmRecord *Resolve(Handle handle) noexcept;
        const RealmRecord *Resolve(Handle handle) const noexcept;
        static void CopyString(std::string_view text,
                               std::array<char, kMaxLabelLength + 1> &dest,
                               std::uint8_t &outLength) noexcept;
        static void CopyExportName(std::string_view text,
                                   std::array<char, kMaxExportNameLength + 1> &dest,
                                   std::uint8_t &outLength) noexcept;
        ExportEntry *FindExport(RealmRecord &realm, std::string_view exportName) noexcept;
        const ExportEntry *FindExport(const RealmRecord &realm, std::string_view exportName) const noexcept;
    };
}
