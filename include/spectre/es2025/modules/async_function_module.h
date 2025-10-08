#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/es2025/module.h"
#include "spectre/es2025/value.h"
#include "spectre/status.h"

namespace spectre::es2025 {
    class AsyncFunctionModule final : public Module {
    public:
        static constexpr std::size_t kMaxLabelLength = 31;

        using Handle = std::uint32_t;
        static constexpr Handle kInvalidHandle = 0;

        using Callback = StatusCode (*)(void *userData,
                                        Value &outValue,
                                        std::string &outDiagnostics);

        struct DispatchOptions {
            std::uint32_t delayFrames = 0;
            double delaySeconds = 0.0;
            std::string_view label;
        };

        struct Result {
            Handle handle = kInvalidHandle;
            StatusCode status = StatusCode::Ok;
            Value value;
            std::string diagnostics;
            std::uint64_t completedFrame = 0;
            double executionMicros = 0.0;
            std::array<char, kMaxLabelLength + 1> label{};
        };

        struct Metrics {
            std::uint64_t enqueued = 0;
            std::uint64_t executed = 0;
            std::uint64_t cancelled = 0;
            std::uint64_t failed = 0;
            std::uint64_t overflow = 0;
            std::uint64_t fastPath = 0;
            std::size_t maxQueueDepth = 0;
            double totalExecutionMicros = 0.0;
            double lastExecutionMicros = 0.0;

            [[nodiscard]] double AverageExecutionMicros() const noexcept {
                return executed == 0 ? 0.0 : totalExecutionMicros / static_cast<double>(executed);
            }
        };

        AsyncFunctionModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Configure(std::size_t queueCapacity, std::size_t completionCapacity = 0);

        StatusCode Enqueue(Callback callback,
                           void *userData,
                           const DispatchOptions &options,
                           Handle &outHandle);

        bool Cancel(Handle handle) noexcept;

        void DrainCompleted(std::vector<Result> &outResults);

        [[nodiscard]] const Metrics &GetMetrics() const noexcept;

        [[nodiscard]] std::size_t PendingCount() const noexcept;

        [[nodiscard]] std::size_t Capacity() const noexcept;

        [[nodiscard]] bool GpuEnabled() const noexcept;

    private:
        struct Job {
            Callback callback;
            void *userData;
            Handle handle;
            std::uint64_t frameDeadline;
            double secondsDeadline;
            std::uint64_t sequence;
            bool inUse;
            bool fastPath;
            std::array<char, kMaxLabelLength + 1> label;

            Job() noexcept;
            void Reset() noexcept;
        };

        struct Slot {
            Job job;
            std::uint16_t generation;

            Slot() noexcept;
        };

        static constexpr std::size_t kHandleIndexBits = 16;
        static constexpr std::size_t kHandleIndexMask = (1u << kHandleIndexBits) - 1;
        static constexpr std::size_t kMaxSlots = kHandleIndexMask;

        static Handle MakeHandle(std::size_t index, std::uint16_t generation) noexcept;
        static std::size_t ExtractIndex(Handle handle) noexcept;
        static std::uint16_t ExtractGeneration(Handle handle) noexcept;
        static std::uint16_t NextGeneration(std::uint16_t generation) noexcept;
        static void CopyLabel(std::string_view label,
                              std::array<char, kMaxLabelLength + 1> &dest) noexcept;

        [[nodiscard]] Slot *ResolveSlot(Handle handle) noexcept;
        [[nodiscard]] const Slot *ResolveSlot(Handle handle) const noexcept;
        [[nodiscard]] bool ReadyForExecution(const Job &job) const noexcept;

        std::size_t AcquireSlot() noexcept;
        void ReleaseSlot(std::size_t index) noexcept;
        void Execute(Handle handle) noexcept;
        void RemoveFromQueue(std::vector<Handle> &queue, Handle handle) noexcept;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        double m_TotalSeconds;
        std::uint64_t m_SequenceCounter;
        std::size_t m_Capacity;

        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeList;
        std::vector<Handle> m_WaitingQueue;
        std::vector<Handle> m_ReadyQueue;
        std::vector<Result> m_Completed;
        Metrics m_Metrics;
    };
}

