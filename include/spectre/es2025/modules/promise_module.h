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
    class PromiseModule final : public Module {
    public:
        enum class State : std::uint8_t {
            Pending,
            Fulfilled,
            Rejected,
            Cancelled
        };

        static constexpr std::size_t kMaxLabelLength = 31;

        using Handle = std::uint32_t;
        static constexpr Handle kInvalidHandle = 0;

        using ReactionCallback = StatusCode (*)(void *userData,
                                               const Value &input,
                                               Value &outValue,
                                               std::string &outDiagnostics);

        struct CreateOptions {
            std::string_view label;
        };

        struct ReactionOptions {
            ReactionCallback onFulfilled = nullptr;
            ReactionCallback onRejected = nullptr;
            void *userData = nullptr;
            std::string_view label;
        };

        struct SettledPromise {
            Handle handle = kInvalidHandle;
            State state = State::Pending;
            Value value;
            std::string diagnostics;
            std::uint64_t settledFrame = 0;
            double settledSeconds = 0.0;
            std::array<char, kMaxLabelLength + 1> label{};
        };

        struct Metrics {
            std::uint64_t created = 0;
            std::uint64_t resolved = 0;
            std::uint64_t rejected = 0;
            std::uint64_t cancelled = 0;
            std::uint64_t chained = 0;
            std::uint64_t executedReactions = 0;
            std::uint64_t failedReactions = 0;
            std::uint64_t overflowPromises = 0;
            std::uint64_t overflowReactions = 0;
            std::uint64_t fastProcessed = 0;
            std::size_t maxPromiseCount = 0;
            std::size_t maxReactionQueue = 0;
        };

        PromiseModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Configure(std::size_t promiseCapacity, std::size_t reactionCapacity);

        StatusCode CreatePromise(Handle &outHandle, const CreateOptions &options = {});

        StatusCode Resolve(Handle handle, const Value &value, std::string_view diagnostics = {});

        StatusCode Reject(Handle handle, std::string_view diagnostics, const Value &value = Value::Undefined());

        bool Cancel(Handle handle) noexcept;

        StatusCode Release(Handle handle) noexcept;

        StatusCode Then(Handle source,
                        Handle &outDerived);

        StatusCode Then(Handle source,
                        Handle &outDerived,
                        const ReactionOptions &options);

        void DrainSettled(std::vector<SettledPromise> &outPromises);

        [[nodiscard]] State GetState(Handle handle) const noexcept;

        [[nodiscard]] const Metrics &GetMetrics() const noexcept;

        [[nodiscard]] std::size_t PendingMicrotasks() const noexcept;

        [[nodiscard]] std::size_t PromiseCount() const noexcept;

        [[nodiscard]] std::size_t ReactionCapacity() const noexcept;

        [[nodiscard]] bool GpuEnabled() const noexcept;

    private:
        struct PromiseRecord {
            State state;
            Value value;
            std::string diagnostics;
            Handle handle;
            std::uint32_t reactionHead;
            std::uint64_t settledFrame;
            double settledSeconds;
            std::array<char, kMaxLabelLength + 1> label;

            PromiseRecord() noexcept;
            void Reset() noexcept;
        };

        struct PromiseSlot {
            PromiseRecord record;
            std::uint16_t generation;

            PromiseSlot() noexcept;
        };

        struct ReactionRecord {
            Handle source;
            Handle derived;
            ReactionCallback onFulfilled;
            ReactionCallback onRejected;
            void *userData;
            std::uint32_t next;
            std::array<char, kMaxLabelLength + 1> label;
            State sourceState;
            Value sourceValue;
            std::string sourceDiagnostics;
        };

        struct ReactionSlot {
            ReactionRecord record;
            bool inUse;
            std::uint16_t generation;

            ReactionSlot() noexcept;
        };

        static constexpr std::size_t kHandleIndexBits = 16;
        static constexpr std::size_t kHandleIndexMask = (1u << kHandleIndexBits) - 1;
        static constexpr std::size_t kMaxSlots = kHandleIndexMask;
        static constexpr std::uint32_t kInvalidReactionIndex = 0xffffffffu;

        static Handle MakeHandle(std::size_t index, std::uint16_t generation) noexcept;
        static std::size_t ExtractIndex(Handle handle) noexcept;
        static std::uint16_t ExtractGeneration(Handle handle) noexcept;
        static std::uint16_t NextGeneration(std::uint16_t generation) noexcept;
        static void CopyLabel(std::string_view label,
                              std::array<char, kMaxLabelLength + 1> &dest) noexcept;

        [[nodiscard]] PromiseSlot *ResolveSlot(Handle handle) noexcept;
        [[nodiscard]] const PromiseSlot *ResolveSlot(Handle handle) const noexcept;

        void ReleasePromiseSlot(std::size_t index) noexcept;

        std::uint32_t AcquireReactionSlot();
        void ReleaseReactionSlot(std::uint32_t index) noexcept;
        void TrimMicrotaskQueue() noexcept;

        void EnqueueReactions(PromiseRecord &record);
        void ProcessMicrotasks(std::size_t budget) noexcept;
        void RunReaction(std::uint32_t reactionIndex) noexcept;
        void FulfillDerived(Handle handle, const Value &value, std::string_view diagnostics);
        void RejectDerived(Handle handle, std::string_view diagnostics, const Value &value);
        void RecordSettlement(PromiseRecord &record,
                              State state,
                              const Value &value,
                              std::string_view diagnostics);

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        double m_TotalSeconds;

        std::vector<PromiseSlot> m_Promises;
        std::vector<std::uint32_t> m_FreePromises;
        std::vector<SettledPromise> m_Settled;

        std::vector<ReactionSlot> m_Reactions;
        std::vector<std::uint32_t> m_FreeReactions;
        std::vector<std::uint32_t> m_MicrotaskQueue;
        std::size_t m_MicrotaskHead;

        Metrics m_Metrics;
    };
}
