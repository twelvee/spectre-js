#include "spectre/es2025/modules/async_iterator_module.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "AsyncIterator";
        constexpr std::string_view kSummary =
                "Async iterator coordination for microtask-aligned consumption.";
        constexpr std::string_view kReference = "ECMA-262 Section 27.5";

        constexpr std::size_t kMinStreamCapacity = 16;
        constexpr std::size_t kMinQueueCapacity = 32;
        constexpr std::size_t kMinWaiterCapacity = 32;
        constexpr std::size_t kMaxQueueCapacity = 4096;
        constexpr std::size_t kMaxWaiterCapacity = 4096;
        constexpr std::size_t kMaxStreamSlots = (1u << 16) - 1;

        std::size_t RecommendStreamCount(std::uint64_t heapBytes) noexcept {
            if (heapBytes == 0) {
                return kMinStreamCapacity;
            }
            auto estimate = heapBytes / 262144ULL; // ~256 KiB per stream budget
            if (estimate < kMinStreamCapacity) {
                estimate = kMinStreamCapacity;
            }
            if (estimate > kMaxStreamSlots) {
                estimate = kMaxStreamSlots;
            }
            return static_cast<std::size_t>(estimate);
        }

        std::size_t RecommendQueueCapacity(std::uint64_t heapBytes) noexcept {
            if (heapBytes == 0) {
                return kMinQueueCapacity;
            }
            auto estimate = heapBytes / 65536ULL; // ~64 KiB per queue slice
            if (estimate < kMinQueueCapacity) {
                estimate = kMinQueueCapacity;
            }
            if (estimate > kMaxQueueCapacity) {
                estimate = kMaxQueueCapacity;
            }
            return static_cast<std::size_t>(estimate);
        }

        std::size_t RecommendWaiterCapacity(std::uint64_t heapBytes) noexcept {
            if (heapBytes == 0) {
                return kMinWaiterCapacity;
            }
            auto estimate = heapBytes / 65536ULL;
            if (estimate < kMinWaiterCapacity) {
                estimate = kMinWaiterCapacity;
            }
            if (estimate > kMaxWaiterCapacity) {
                estimate = kMaxWaiterCapacity;
            }
            return static_cast<std::size_t>(estimate);
        }
    }

        AsyncIteratorModule::Entry::Entry() noexcept
        : value(Value::Undefined()), hasValue(false), done(false), status(StatusCode::Ok), diagnostics() {}

    void AsyncIteratorModule::Entry::Reset() noexcept {
        value = Value::Undefined();
        hasValue = false;
        done = false;
        status = StatusCode::Ok;
        diagnostics.clear();
    }

    void AsyncIteratorModule::Entry::Assign(const EnqueueOptions &options) {
        if (options.hasValue) {
            value = options.value;
            hasValue = true;
        } else {
            value = Value::Undefined();
            hasValue = false;
        }
        done = options.done;
        status = StatusCode::Ok;
        if (!options.diagnostics.empty()) {
            diagnostics.assign(options.diagnostics);
        } else {
            diagnostics.clear();
        }
    }

    AsyncIteratorModule::Waiter::Waiter() noexcept
        : ticket(kInvalidTicket), label{}, requestFrame(0), requestSeconds(0.0), active(false) {
        label[0] = '\0';
    }

    void AsyncIteratorModule::Waiter::Reset() noexcept {
        ticket = kInvalidTicket;
        label[0] = '\0';
        requestFrame = 0;
        requestSeconds = 0.0;
        active = false;
    }

    AsyncIteratorModule::Slot::Slot() noexcept
        : state(StreamState::Cancelled),
          queue(),
          waiters(),
          capacity(0),
          waiterCapacity(0),
          head(0),
          tail(0),
          count(0),
          waiterHead(0),
          waiterCount(0),
          inUse(false),
          closing(false),
          generation(1),
          ticketBase(1),
          label{},
          terminalStatus(StatusCode::Ok),
          terminalDiagnostics(),
          lastFrame(0),
          lastSeconds(0.0) {
        label[0] = '\0';
    }

    void AsyncIteratorModule::Slot::Reset() noexcept {
        capacity = queue.size();
        waiterCapacity = waiters.size();
        head = 0;
        tail = 0;
        count = 0;
        waiterHead = 0;
        waiterCount = 0;
        inUse = false;
        closing = false;
        ticketBase = 1;
        state = StreamState::Cancelled;
        terminalStatus = StatusCode::Ok;
        terminalDiagnostics.clear();
        lastFrame = 0;
        lastSeconds = 0.0;
        label[0] = '\0';
        for (auto &entry: queue) {
            entry.Reset();
        }
        for (auto &waiter: waiters) {
            waiter.Reset();
        }
    }

AsyncIteratorModule::AsyncIteratorModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_TotalSeconds(0.0),
          m_DefaultQueueCapacity(kMinQueueCapacity),
          m_DefaultWaiterCapacity(kMinWaiterCapacity),
          m_StreamHint(kMinStreamCapacity),
          m_ActiveStreams(0),
          m_Slots(),
          m_FreeSlots(),
          m_Settled(),
          m_Metrics() {
        m_Settled.reserve(64);
    }

    std::string_view AsyncIteratorModule::Name() const noexcept {
        return kName;
    }

    std::string_view AsyncIteratorModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view AsyncIteratorModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void AsyncIteratorModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Initialized = true;
        m_CurrentFrame = 0;
        m_TotalSeconds = 0.0;
        m_ActiveStreams = 0;
        m_Settled.clear();
        m_Metrics = Metrics();

        auto streamCap = RecommendStreamCount(context.config.memory.heapBytes);
        auto queueCap = RecommendQueueCapacity(context.config.memory.heapBytes);
        auto waiterCap = RecommendWaiterCapacity(context.config.memory.heapBytes);
        Configure(streamCap, queueCap, waiterCap);

        for (auto &slot: m_Slots) {
            ClearSlot(slot);
            slot.generation = NextGeneration(slot.generation);
        }

        m_FreeSlots.clear();
        m_FreeSlots.reserve(m_Slots.size());
        for (std::uint32_t i = 0; i < m_Slots.size(); ++i) {
            m_FreeSlots.push_back(static_cast<std::uint32_t>(m_Slots.size() - 1 - i));
        }
    }

    void AsyncIteratorModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_TotalSeconds += info.deltaSeconds;
    }

    void AsyncIteratorModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
    }

    void AsyncIteratorModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        auto streamCap = RecommendStreamCount(config.memory.heapBytes);
        auto queueCap = RecommendQueueCapacity(config.memory.heapBytes);
        auto waiterCap = RecommendWaiterCapacity(config.memory.heapBytes);
        Configure(streamCap, queueCap, waiterCap);
    }

    StatusCode AsyncIteratorModule::Configure(std::size_t streamCapacity,
                                              std::size_t queueCapacity,
                                              std::size_t waiterCapacity) {
        if (queueCapacity == 0 || waiterCapacity == 0) {
            return StatusCode::InvalidArgument;
        }
        if (streamCapacity == 0) {
            streamCapacity = kMinStreamCapacity;
        }
        if (streamCapacity > kMaxSlots) {
            streamCapacity = kMaxSlots;
        }
        if (queueCapacity > kMaxQueueCapacity) {
            queueCapacity = kMaxQueueCapacity;
        }
        if (waiterCapacity > kMaxWaiterCapacity) {
            waiterCapacity = kMaxWaiterCapacity;
        }

        m_StreamHint = std::max(streamCapacity, m_StreamHint);
        m_DefaultQueueCapacity = std::max(queueCapacity, m_DefaultQueueCapacity);
        m_DefaultWaiterCapacity = std::max(waiterCapacity, m_DefaultWaiterCapacity);

        if (m_Slots.capacity() < m_StreamHint) {
            m_Slots.reserve(m_StreamHint);
        }

        for (auto &slot: m_Slots) {
            if (slot.queue.size() < m_DefaultQueueCapacity) {
                slot.queue.resize(m_DefaultQueueCapacity);
            }
            if (slot.waiters.size() < m_DefaultWaiterCapacity) {
                slot.waiters.resize(m_DefaultWaiterCapacity);
            }
            slot.capacity = slot.queue.size();
            slot.waiterCapacity = slot.waiters.size();
        }

        if (m_FreeSlots.capacity() < m_StreamHint) {
            m_FreeSlots.reserve(m_StreamHint);
        }

        return StatusCode::Ok;
    }

    StatusCode AsyncIteratorModule::CreateStream(const StreamConfig &config, Handle &outHandle) {
        outHandle = kInvalidHandle;
        if (!m_Initialized) {
            return StatusCode::InternalError;
        }
        auto queueCapacity = config.queueCapacity == 0 ? m_DefaultQueueCapacity : config.queueCapacity;
        auto waiterCapacity = config.waiterCapacity == 0 ? m_DefaultWaiterCapacity : config.waiterCapacity;
        if (queueCapacity == 0 || waiterCapacity == 0) {
            return StatusCode::InvalidArgument;
        }
        if (queueCapacity > kMaxQueueCapacity) {
            queueCapacity = kMaxQueueCapacity;
        }
        if (waiterCapacity > kMaxWaiterCapacity) {
            waiterCapacity = kMaxWaiterCapacity;
        }

        Slot *slot = nullptr;
        std::size_t index = 0;
        if (!m_FreeSlots.empty()) {
            index = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            if (index >= m_Slots.size()) {
                return StatusCode::InternalError;
            }
            slot = &m_Slots[index];
        } else {
            if (m_Slots.size() >= kMaxSlots) {
                return StatusCode::CapacityExceeded;
            }
            index = m_Slots.size();
            m_Slots.emplace_back();
            slot = &m_Slots.back();
        }

        if (slot->queue.size() < queueCapacity) {
            slot->queue.resize(queueCapacity);
        }
        if (slot->waiters.size() < waiterCapacity) {
            slot->waiters.resize(waiterCapacity);
        }
        slot->Reset();
        slot->capacity = slot->queue.size();
        slot->waiterCapacity = slot->waiters.size();
        slot->state = StreamState::Active;
        slot->inUse = true;
        slot->closing = false;
        slot->lastFrame = m_CurrentFrame;
        slot->lastSeconds = m_TotalSeconds;
        slot->terminalStatus = StatusCode::Ok;
        slot->terminalDiagnostics.clear();
        CopyLabel(config.label, slot->label);

        outHandle = MakeHandle(index, slot->generation);
        m_ActiveStreams += 1;
        m_Metrics.streamsCreated += 1;
        m_Metrics.maxActiveStreams = std::max(m_Metrics.maxActiveStreams, m_ActiveStreams);
        return StatusCode::Ok;
    }

    bool AsyncIteratorModule::DestroyStream(Handle handle) noexcept {
        auto *slot = ResolveSlot(handle);
        if (!slot) {
            return false;
        }
        const auto index = ExtractIndex(handle);
        slot->state = StreamState::Cancelled;
        slot->terminalStatus = StatusCode::InvalidArgument;
        slot->terminalDiagnostics = "cancelled";
        Result dummy;
        while (slot->waiterCount > 0) {
            auto waiter = PopWaiter(*slot);
            if (!waiter.active) {
                continue;
            }
            EmitTerminalResult(*slot, handle, waiter.ticket, StatusCode::InvalidArgument,
                               "cancelled", &waiter, nullptr, false);
            m_Metrics.waitersCancelled += 1;
        }
        ClearSlot(*slot);
        slot->generation = NextGeneration(slot->generation);
        slot->inUse = false;
        m_FreeSlots.push_back(static_cast<std::uint32_t>(index));
        if (m_ActiveStreams > 0) {
            --m_ActiveStreams;
        }
        m_Metrics.streamsDestroyed += 1;
        return true;
    }

    AsyncIteratorModule::StreamState AsyncIteratorModule::GetState(Handle handle) const noexcept {
        auto *slot = ResolveSlot(handle);
        if (!slot) {
            return StreamState::Cancelled;
        }
        return slot->state;
    }

    StatusCode AsyncIteratorModule::Enqueue(Handle handle, const EnqueueOptions &options) {
        auto *slot = ResolveSlot(handle);
        if (!slot) {
            return StatusCode::NotFound;
        }
        if (!slot->inUse) {
            return StatusCode::NotFound;
        }
        if (slot->state == StreamState::Failed || slot->state == StreamState::Cancelled ||
            slot->state == StreamState::Completed) {
            return StatusCode::InvalidArgument;
        }

        Entry entry;
        entry.Assign(options);

        if (entry.hasValue) {
            m_Metrics.valuesQueued += 1;
        }

        slot->lastFrame = m_CurrentFrame;
        slot->lastSeconds = m_TotalSeconds;

        if (slot->waiterCount > 0) {
            auto waiter = PopWaiter(*slot);
            while (!waiter.active && slot->waiterCount > 0) {
                waiter = PopWaiter(*slot);
            }
            if (waiter.active) {
                if (entry.done) {
                    slot->state = StreamState::Completed;
                    slot->closing = false;
                    slot->terminalStatus = StatusCode::Ok;
                    slot->terminalDiagnostics = entry.diagnostics;
                } else if (slot->closing) {
                    slot->state = StreamState::Closing;
                } else {
                    slot->state = StreamState::Active;
                }
                EmitResult(*slot, handle, waiter.ticket, entry, &waiter, nullptr);
                return StatusCode::Ok;
            }
        }

        if (slot->capacity == 0) {
            return StatusCode::CapacityExceeded;
        }
        if (slot->count >= slot->capacity) {
            return StatusCode::CapacityExceeded;
        }

        std::size_t index;
        if (options.urgent) {
            index = (slot->head + slot->capacity - 1) % slot->capacity;
            slot->head = index;
        } else {
            index = slot->tail;
            slot->tail = (slot->tail + 1) % slot->capacity;
        }
        slot->queue[index] = entry;
        slot->count += 1;
        if (entry.done) {
            slot->closing = true;
            slot->state = StreamState::Closing;
            slot->terminalStatus = StatusCode::Ok;
            slot->terminalDiagnostics = entry.diagnostics;
        }
        m_Metrics.maxQueueDepth = std::max(m_Metrics.maxQueueDepth, slot->count);
        return StatusCode::Ok;
    }

    StatusCode AsyncIteratorModule::SignalComplete(Handle handle, std::string_view diagnostics) {
        EnqueueOptions options;
        options.hasValue = false;
        options.done = true;
        options.diagnostics = diagnostics;
        return Enqueue(handle, options);
    }

    StatusCode AsyncIteratorModule::Fail(Handle handle,
                                         std::string_view diagnostics,
                                         StatusCode status) {
        auto *slot = ResolveSlot(handle);
        if (!slot) {
            return StatusCode::NotFound;
        }
        if (!slot->inUse) {
            return StatusCode::NotFound;
        }
        slot->state = StreamState::Failed;
        slot->closing = false;
        slot->terminalStatus = status;
        slot->terminalDiagnostics.assign(diagnostics);

        while (slot->count > 0) {
            slot->queue[slot->head].Reset();
            slot->head = (slot->head + 1) % slot->capacity;
            slot->count -= 1;
        }
        slot->head = 0;
        slot->tail = 0;

        while (slot->waiterCount > 0) {
            auto waiter = PopWaiter(*slot);
            if (!waiter.active) {
                continue;
            }
            EmitTerminalResult(*slot, handle, waiter.ticket, status, diagnostics, &waiter, nullptr, false);
        }
        return StatusCode::Ok;
    }

    StatusCode AsyncIteratorModule::RequestNext(Handle handle,
                                                Request &outRequest,
                                                const RequestOptions &options) {
        outRequest = Request();
        outRequest.result.stream = handle;
        auto *slot = ResolveSlot(handle);
        if (!slot) {
            return StatusCode::NotFound;
        }
        if (!slot->inUse) {
            return StatusCode::NotFound;
        }

        const auto ticketIndex = NextTicketIndex(*slot);
        const auto ticket = MakeTicket(slot->generation, ticketIndex);
        outRequest.ticket = ticket;

        Waiter immediateWaiter;
        immediateWaiter.ticket = ticket;
        CopyLabel(options.label, immediateWaiter.label);
        immediateWaiter.requestFrame = m_CurrentFrame;
        immediateWaiter.requestSeconds = m_TotalSeconds;
        immediateWaiter.active = true;

        if (slot->count > 0) {
            auto &stored = slot->queue[slot->head];
            Entry entry = stored;
            stored.Reset();
            slot->head = (slot->head + 1) % slot->capacity;
            slot->count -= 1;
            if (entry.done) {
                slot->state = StreamState::Completed;
                slot->closing = false;
                slot->terminalStatus = StatusCode::Ok;
                slot->terminalDiagnostics = entry.diagnostics;
            } else if (slot->closing) {
                slot->state = StreamState::Closing;
            } else {
                slot->state = StreamState::Active;
            }
            EmitResult(*slot, handle, ticket, entry, &immediateWaiter, &outRequest.result);
            outRequest.immediate = true;
            return StatusCode::Ok;
        }

        if (slot->state == StreamState::Completed) {
            Entry entry;
            entry.Reset();
            entry.done = true;
            entry.status = StatusCode::Ok;
            entry.hasValue = false;
            if (!slot->terminalDiagnostics.empty()) {
                entry.diagnostics = slot->terminalDiagnostics;
            }
            EmitResult(*slot, handle, ticket, entry, &immediateWaiter, &outRequest.result);
            outRequest.immediate = true;
            return StatusCode::Ok;
        }

        if (slot->state == StreamState::Failed) {
            Entry entry;
            entry.Reset();
            entry.done = true;
            entry.status = slot->terminalStatus;
            entry.hasValue = false;
            entry.diagnostics = slot->terminalDiagnostics;
            EmitResult(*slot, handle, ticket, entry, &immediateWaiter, &outRequest.result);
            outRequest.immediate = true;
            return StatusCode::Ok;
        }

        if (slot->state == StreamState::Cancelled) {
            Entry entry;
            entry.Reset();
            entry.done = true;
            entry.status = StatusCode::InvalidArgument;
            entry.hasValue = false;
            entry.diagnostics = "cancelled";
            EmitResult(*slot, handle, ticket, entry, &immediateWaiter, &outRequest.result);
            outRequest.immediate = true;
            return StatusCode::Ok;
        }

        if (slot->waiterCapacity == 0 || slot->waiterCount >= slot->waiterCapacity) {
            return StatusCode::CapacityExceeded;
        }

        auto index = (slot->waiterHead + slot->waiterCount) % slot->waiterCapacity;
        auto &waiter = slot->waiters[index];
        waiter.ticket = ticket;
        CopyLabel(options.label, waiter.label);
        waiter.requestFrame = m_CurrentFrame;
        waiter.requestSeconds = m_TotalSeconds;
        waiter.active = true;
        slot->waiterCount += 1;
        m_Metrics.waitersEnqueued += 1;
        m_Metrics.maxWaiterDepth = std::max(m_Metrics.maxWaiterDepth, slot->waiterCount);
        CopyLabel(std::string_view(slot->label.data()), outRequest.result.streamLabel);
        outRequest.result.ticket = ticket;
        outRequest.result.requestFrame = waiter.requestFrame;
        outRequest.result.requestSeconds = waiter.requestSeconds;
        outRequest.immediate = false;
        return StatusCode::Ok;
    }

    bool AsyncIteratorModule::CancelTicket(Handle handle, Ticket ticket) noexcept {
        auto *slot = ResolveSlot(handle);
        if (!slot) {
            return false;
        }
        if (slot->waiterCount == 0 || slot->waiterCapacity == 0) {
            return false;
        }
        std::size_t index = slot->waiterHead;
        for (std::size_t processed = 0; processed < slot->waiterCapacity; ++processed) {
            auto &waiter = slot->waiters[index];
            if (waiter.active && waiter.ticket == ticket) {
                waiter.active = false;
                waiter.ticket = kInvalidTicket;
                waiter.label[0] = '\0';
                waiter.requestFrame = 0;
                waiter.requestSeconds = 0.0;
                m_Metrics.waitersCancelled += 1;
                return true;
            }
            index = (index + 1) % slot->waiterCapacity;
        }
        return false;
    }

    void AsyncIteratorModule::DrainSettled(std::vector<Result> &outResults) {
        if (m_Settled.empty()) {
            outResults.clear();
            return;
        }
        outResults.clear();
        outResults.reserve(m_Settled.size());
        for (auto &result: m_Settled) {
            outResults.push_back(std::move(result));
        }
        m_Settled.clear();
    }

    const AsyncIteratorModule::Metrics &AsyncIteratorModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    std::size_t AsyncIteratorModule::ActiveStreams() const noexcept {
        return m_ActiveStreams;
    }

    bool AsyncIteratorModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    AsyncIteratorModule::Handle AsyncIteratorModule::MakeHandle(std::size_t index,
                                                                std::uint16_t generation) noexcept {
        return static_cast<Handle>((static_cast<std::uint64_t>(generation) << kHandleIndexBits) |
                                   (static_cast<std::uint64_t>(index) & kHandleIndexMask));
    }

    std::size_t AsyncIteratorModule::ExtractIndex(Handle handle) noexcept {
        return static_cast<std::size_t>(handle & kHandleIndexMask);
    }

    std::uint16_t AsyncIteratorModule::ExtractGeneration(Handle handle) noexcept {
        return static_cast<std::uint16_t>(handle >> kHandleIndexBits);
    }

    std::uint16_t AsyncIteratorModule::NextGeneration(std::uint16_t generation) noexcept {
        ++generation;
        if (generation == 0) {
            generation = 1;
        }
        return generation;
    }

    AsyncIteratorModule::Ticket AsyncIteratorModule::MakeTicket(std::uint16_t generation,
                                                                std::uint32_t local) noexcept {
        return static_cast<Ticket>((static_cast<std::uint64_t>(generation) << kTicketIndexBits) |
                                   (static_cast<std::uint64_t>(local) & kTicketIndexMask));
    }

    std::uint32_t AsyncIteratorModule::ExtractTicketLocal(Ticket ticket) noexcept {
        return static_cast<std::uint32_t>(ticket & kTicketIndexMask);
    }

    std::uint16_t AsyncIteratorModule::ExtractTicketGeneration(Ticket ticket) noexcept {
        return static_cast<std::uint16_t>(ticket >> kTicketIndexBits);
    }

    void AsyncIteratorModule::CopyLabel(std::string_view label,
                                        std::array<char, kMaxLabelLength + 1> &dest) noexcept {
        dest.fill('\0');
        if (label.empty()) {
            return;
        }
        const auto count = std::min<std::size_t>(label.size(), kMaxLabelLength);
        std::memcpy(dest.data(), label.data(), count);
        dest[count] = '\0';
    }

    AsyncIteratorModule::Slot *AsyncIteratorModule::ResolveSlot(Handle handle) noexcept {
        if (handle == kInvalidHandle) {
            return nullptr;
        }
        const auto index = ExtractIndex(handle);
        if (index >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[index];
        if (!slot.inUse) {
            return nullptr;
        }
        if (slot.generation != ExtractGeneration(handle)) {
            return nullptr;
        }
        return &slot;
    }

    const AsyncIteratorModule::Slot *AsyncIteratorModule::ResolveSlot(Handle handle) const noexcept {
        if (handle == kInvalidHandle) {
            return nullptr;
        }
        const auto index = ExtractIndex(handle);
        if (index >= m_Slots.size()) {
            return nullptr;
        }
        const auto &slot = m_Slots[index];
        if (!slot.inUse) {
            return nullptr;
        }
        if (slot.generation != ExtractGeneration(handle)) {
            return nullptr;
        }
        return &slot;
    }

    std::uint32_t AsyncIteratorModule::NextTicketIndex(Slot &slot) noexcept {
        auto index = slot.ticketBase & kTicketIndexMask;
        slot.ticketBase += 1;
        if (slot.ticketBase == 0 || slot.ticketBase > kTicketIndexMask) {
            slot.ticketBase = 1;
        }
        if (index == 0) {
            index = 1;
        }
        return index;
    }

    void AsyncIteratorModule::EmitResult(Slot &slot,
                                         Handle streamHandle,
                                         Ticket ticket,
                                         const Entry &entry,
                                         const Waiter *waiter,
                                         Result *outImmediate) {
        Result result;
        result.stream = streamHandle;
        result.ticket = ticket;
        result.status = entry.status;
        result.value = entry.value;
        result.hasValue = entry.hasValue;
        result.done = entry.done;
        result.streamState = slot.state;
        if (waiter != nullptr) {
            CopyLabel(std::string_view(waiter->label.data()), result.requestLabel);
            result.requestFrame = waiter->requestFrame;
            result.requestSeconds = waiter->requestSeconds;
        } else {
            result.requestFrame = m_CurrentFrame;
            result.requestSeconds = m_TotalSeconds;
        }
        result.satisfiedFrame = m_CurrentFrame;
        result.satisfiedSeconds = m_TotalSeconds;
        CopyLabel(std::string_view(slot.label.data()), result.streamLabel);
        result.diagnostics = entry.diagnostics;

        if (waiter != nullptr && waiter->active) {
            m_Metrics.waitersServed += 1;
        }
        if (entry.status == StatusCode::Ok) {
            if (entry.hasValue) {
                m_Metrics.valuesDelivered += 1;
            }
            if (entry.done) {
                m_Metrics.completionsDelivered += 1;
            }
        } else {
            m_Metrics.failuresDelivered += 1;
        }

        if (outImmediate != nullptr) {
            *outImmediate = result;
        }
        m_Settled.push_back(std::move(result));
    }

    void AsyncIteratorModule::EmitTerminalResult(Slot &slot,
                                                 Handle streamHandle,
                                                 Ticket ticket,
                                                 StatusCode status,
                                                 std::string_view diagnostics,
                                                 const Waiter *waiter,
                                                 Result *outImmediate,
                                                 bool completion) {
        Entry entry;
        entry.Reset();
        entry.status = status;
        entry.done = true;
        entry.hasValue = false;
        if (!diagnostics.empty()) {
            entry.diagnostics.assign(diagnostics);
        }
        if (completion) {
            slot.state = StreamState::Completed;
            slot.terminalStatus = StatusCode::Ok;
            slot.terminalDiagnostics = entry.diagnostics;
        } else {
            slot.terminalStatus = status;
            slot.terminalDiagnostics = entry.diagnostics;
            if (status != StatusCode::Ok && slot.state != StreamState::Cancelled) {
                slot.state = StreamState::Failed;
            }
        }
        EmitResult(slot, streamHandle, ticket, entry, waiter, outImmediate);
    }

    AsyncIteratorModule::Waiter AsyncIteratorModule::PopWaiter(Slot &slot) noexcept {
        Waiter waiter;
        if (slot.waiterCount == 0 || slot.waiterCapacity == 0) {
            return waiter;
        }
        while (slot.waiterCount > 0) {
            auto &current = slot.waiters[slot.waiterHead];
            Waiter temp = current;
            current.Reset();
            slot.waiterHead = (slot.waiterHead + 1) % slot.waiterCapacity;
            slot.waiterCount -= 1;
            if (temp.active) {
                return temp;
            }
        }
        return waiter;
    }

    void AsyncIteratorModule::ClearSlot(Slot &slot) noexcept {
        if (slot.queue.size() < m_DefaultQueueCapacity) {
            slot.queue.resize(m_DefaultQueueCapacity);
        }
        if (slot.waiters.size() < m_DefaultWaiterCapacity) {
            slot.waiters.resize(m_DefaultWaiterCapacity);
        }
        slot.Reset();
        slot.capacity = slot.queue.size();
        slot.waiterCapacity = slot.waiters.size();
    }
}
