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
    class AsyncIteratorModule final : public Module {
    public:
        static constexpr std::size_t kMaxLabelLength = 31;

        using Handle = std::uint32_t;
        using Ticket = std::uint32_t;

        static constexpr Handle kInvalidHandle = 0;
        static constexpr Ticket kInvalidTicket = 0;

        enum class StreamState : std::uint8_t {
            Active,
            Closing,
            Completed,
            Failed,
            Cancelled
        };

        struct StreamConfig {
            std::size_t queueCapacity = 0;
            std::size_t waiterCapacity = 0;
            std::string_view label;
        };

        struct EnqueueOptions {
            Value value = Value::Undefined();
            bool hasValue = true;
            bool done = false;
            bool urgent = false;
            std::string_view diagnostics;
        };

        struct RequestOptions {
            std::string_view label;
        };

        struct Result {
            Handle stream = kInvalidHandle;
            Ticket ticket = kInvalidTicket;
            StatusCode status = StatusCode::Ok;
            Value value;
            bool hasValue = false;
            bool done = false;
            StreamState streamState = StreamState::Active;
            std::uint64_t requestFrame = 0;
            double requestSeconds = 0.0;
            std::uint64_t satisfiedFrame = 0;
            double satisfiedSeconds = 0.0;
            std::array<char, kMaxLabelLength + 1> requestLabel{};
            std::array<char, kMaxLabelLength + 1> streamLabel{};
            std::string diagnostics;
        };

        struct Request {
            Ticket ticket = kInvalidTicket;
            bool immediate = false;
            Result result;
        };

        struct Metrics {
            std::uint64_t streamsCreated = 0;
            std::uint64_t streamsDestroyed = 0;
            std::uint64_t valuesQueued = 0;
            std::uint64_t valuesDelivered = 0;
            std::uint64_t completionsDelivered = 0;
            std::uint64_t failuresDelivered = 0;
            std::uint64_t waitersEnqueued = 0;
            std::uint64_t waitersServed = 0;
            std::uint64_t waitersCancelled = 0;
            std::size_t maxActiveStreams = 0;
            std::size_t maxQueueDepth = 0;
            std::size_t maxWaiterDepth = 0;
        };

        AsyncIteratorModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Configure(std::size_t streamCapacity,
                             std::size_t queueCapacity,
                             std::size_t waiterCapacity);

        StatusCode CreateStream(const StreamConfig &config, Handle &outHandle);
        bool DestroyStream(Handle handle) noexcept;

        StreamState GetState(Handle handle) const noexcept;

        StatusCode Enqueue(Handle handle, const EnqueueOptions &options);
        StatusCode SignalComplete(Handle handle, std::string_view diagnostics = {});
        StatusCode Fail(Handle handle, std::string_view diagnostics,
                        StatusCode status = StatusCode::InvalidArgument);

        StatusCode RequestNext(Handle handle, Request &outRequest,
                               const RequestOptions &options = {});
        bool CancelTicket(Handle handle, Ticket ticket) noexcept;

        void DrainSettled(std::vector<Result> &outResults);

        [[nodiscard]] const Metrics &GetMetrics() const noexcept;
        [[nodiscard]] std::size_t ActiveStreams() const noexcept;
        [[nodiscard]] bool GpuEnabled() const noexcept;

    private:
        struct Entry {
            Value value;
            bool hasValue;
            bool done;
            StatusCode status;
            std::string diagnostics;

            Entry() noexcept;
            void Reset() noexcept;
            void Assign(const EnqueueOptions &options);
        };

        struct Waiter {
            Ticket ticket;
            std::array<char, kMaxLabelLength + 1> label;
            std::uint64_t requestFrame;
            double requestSeconds;
            bool active;

            Waiter() noexcept;
            void Reset() noexcept;
        };

        struct Slot {
            StreamState state;
            std::vector<Entry> queue;
            std::vector<Waiter> waiters;
            std::size_t capacity;
            std::size_t waiterCapacity;
            std::size_t head;
            std::size_t tail;
            std::size_t count;
            std::size_t waiterHead;
            std::size_t waiterCount;
            bool inUse;
            bool closing;
            std::uint16_t generation;
            std::uint32_t ticketBase;
            std::array<char, kMaxLabelLength + 1> label;
            StatusCode terminalStatus;
            std::string terminalDiagnostics;
            std::uint64_t lastFrame;
            double lastSeconds;

            Slot() noexcept;
            void Reset() noexcept;
        };

        static constexpr std::size_t kHandleIndexBits = 16;
        static constexpr std::size_t kHandleIndexMask = (1u << kHandleIndexBits) - 1;
        static constexpr std::size_t kMaxSlots = kHandleIndexMask;
        static constexpr std::size_t kTicketIndexBits = 20;
        static constexpr std::size_t kTicketIndexMask = (1u << kTicketIndexBits) - 1;

        static Handle MakeHandle(std::size_t index, std::uint16_t generation) noexcept;
        static std::size_t ExtractIndex(Handle handle) noexcept;
        static std::uint16_t ExtractGeneration(Handle handle) noexcept;
        static std::uint16_t NextGeneration(std::uint16_t generation) noexcept;
        static Ticket MakeTicket(std::uint16_t generation, std::uint32_t local) noexcept;
        static std::uint32_t ExtractTicketLocal(Ticket ticket) noexcept;
        static std::uint16_t ExtractTicketGeneration(Ticket ticket) noexcept;
        static void CopyLabel(std::string_view label,
                              std::array<char, kMaxLabelLength + 1> &dest) noexcept;

        Slot *ResolveSlot(Handle handle) noexcept;
        const Slot *ResolveSlot(Handle handle) const noexcept;

        std::uint32_t NextTicketIndex(Slot &slot) noexcept;
        void EmitResult(Slot &slot,
                        Handle streamHandle,
                        Ticket ticket,
                        const Entry &entry,
                        const Waiter *waiter,
                        Result *outImmediate);
        void EmitTerminalResult(Slot &slot,
                                Handle streamHandle,
                                Ticket ticket,
                                StatusCode status,
                                std::string_view diagnostics,
                                const Waiter *waiter,
                                Result *outImmediate,
                                bool completion);
        Waiter PopWaiter(Slot &slot) noexcept;
        void ClearSlot(Slot &slot) noexcept;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        double m_TotalSeconds;
        std::size_t m_DefaultQueueCapacity;
        std::size_t m_DefaultWaiterCapacity;
        std::size_t m_StreamHint;
        std::size_t m_ActiveStreams;

        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        std::vector<Result> m_Settled;
        Metrics m_Metrics;
    };
}
