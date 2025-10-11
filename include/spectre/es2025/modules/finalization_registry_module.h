#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/es2025/module.h"
#include "spectre/es2025/value.h"
#include "spectre/es2025/modules/object_module.h"
#include "spectre/status.h"

namespace spectre::es2025 {
    class FinalizationRegistryModule final : public Module {
    public:
        using Handle = std::uint64_t;

        using CleanupCallback = void (*)(const Value &holdings, void *userData);

        struct CreateOptions {
            std::string_view label;
            CleanupCallback defaultCleanup;
            void *defaultUserData;
            std::uint32_t initialCapacity;
            std::uint32_t autoCleanupBatch;
            bool autoCleanup;

            CreateOptions() noexcept;
        };

        struct Metrics {
            std::uint64_t registryCount;
            std::uint64_t liveCells;
            std::uint64_t pendingCells;
            std::uint64_t reclaimedCells;
            std::uint64_t registrations;
            std::uint64_t unregistrations;
            std::uint64_t processedHoldings;
            std::uint64_t defaultCallbacks;
            std::uint64_t manualCallbacks;
            std::uint64_t failedRegistrations;
            std::uint64_t lastFrameTouched;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        FinalizationRegistryModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(const CreateOptions &options, Handle &outHandle);
        StatusCode Destroy(Handle handle);

        StatusCode Register(Handle handle,
                            ObjectModule::Handle target,
                            const Value &holdings,
                            ObjectModule::Handle unregisterToken);

        StatusCode Unregister(Handle handle,
                              ObjectModule::Handle unregisterToken,
                              bool &outRemoved);

        StatusCode CleanupSome(Handle handle,
                               CleanupCallback callback,
                               void *userData,
                               std::uint32_t limit,
                               std::uint32_t &outProcessed);

        std::uint32_t LiveCellCount(Handle handle) const noexcept;
        std::uint32_t PendingCount(Handle handle) const noexcept;

        const Metrics &GetMetrics() const noexcept;

    private:
        struct Cell {
            ObjectModule::Handle target;
            ObjectModule::Handle unregisterToken;
            Value holdings;
            std::uint32_t tokenNext;
            std::uint64_t version;
            std::uint64_t lastAliveFrame;
            bool inUse;
            bool pending;

            Cell() noexcept;
        };

        struct RegistryRecord {
            Handle handle;
            std::string label;
            CleanupCallback defaultCleanup;
            void *defaultUserData;
            std::uint32_t autoCleanupBatch;
            bool autoCleanup;
            std::vector<Cell> cells;
            std::vector<std::uint32_t> freeCells;
            std::vector<std::uint32_t> pendingQueue;
            std::vector<std::uint32_t> tokenBuckets;
            std::uint32_t scanCursor;
            std::uint32_t liveCells;
            std::uint32_t pendingCells;
            std::uint64_t lastCleanupFrame;

            RegistryRecord() noexcept;
        };

        struct SlotRecord {
            bool inUse;
            std::uint32_t generation;
            RegistryRecord record;

            SlotRecord() noexcept;
        };

        static constexpr std::uint32_t kInvalidIndex = 0xffffffffu;
        static constexpr std::uint32_t kDefaultTokenBucketCount = 8;
        static constexpr std::uint32_t kScanBudgetPerRegistry = 32;

        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;
        static std::uint32_t DecodeSlot(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;

        SlotRecord *FindMutableSlot(Handle handle) noexcept;
        const SlotRecord *FindSlot(Handle handle) const noexcept;

        RegistryRecord *FindRegistry(Handle handle) noexcept;
        const RegistryRecord *FindRegistry(Handle handle) const noexcept;

        std::uint32_t AcquireCell(RegistryRecord &registry);
        void ReleaseCell(RegistryRecord &registry, std::uint32_t index, bool processed) noexcept;

        void EnsureTokenCapacity(RegistryRecord &registry);
        void InsertToken(RegistryRecord &registry, std::uint32_t cellIndex) noexcept;
        void RemoveToken(RegistryRecord &registry, std::uint32_t cellIndex) noexcept;

        void QueueCell(RegistryRecord &registry, std::uint32_t index) noexcept;
        bool DequeueCell(RegistryRecord &registry, std::uint32_t &outIndex) noexcept;
        void RunAutoCleanup(RegistryRecord &registry) noexcept;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        ObjectModule *m_ObjectModule;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        Metrics m_Metrics;
        std::vector<SlotRecord> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
    };
}

