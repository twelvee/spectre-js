#include "spectre/es2025/modules/promise_module.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Promise";
        constexpr std::string_view kSummary =
                "Promise state machine, reactions, and realtime-friendly job queue integration.";
        constexpr std::string_view kReference = "ECMA-262 Section 27.2";
        constexpr std::size_t kDefaultPromiseCapacity = 256;
        constexpr std::size_t kDefaultReactionCapacity = 512;
        constexpr std::size_t kHandleIndexBits = 16;
        constexpr std::size_t kMaxSlotsEstimate = (1u << kHandleIndexBits) - 1;

        std::size_t RecommendPromiseCapacity(std::uint64_t heapBytes) noexcept {
            if (heapBytes == 0) {
                return kDefaultPromiseCapacity;
            }
            auto scaled = heapBytes / 16384ULL;
            if (scaled < 128) {
                scaled = 128;
            }
            if (scaled > kMaxSlotsEstimate) {
                scaled = kMaxSlotsEstimate;
            }
            return static_cast<std::size_t>(scaled);
        }

        std::size_t RecommendReactionCapacity(std::uint64_t heapBytes) noexcept {
            if (heapBytes == 0) {
                return kDefaultReactionCapacity;
            }
            auto scaled = heapBytes / 12288ULL;
            if (scaled < 256) {
                scaled = 256;
            }
            if (scaled > kMaxSlotsEstimate) {
                scaled = kMaxSlotsEstimate;
            }
            return static_cast<std::size_t>(scaled);
        }
    }

    PromiseModule::PromiseRecord::PromiseRecord() noexcept
        : state(State::Pending),
          value(Value::Undefined()),
          diagnostics(),
          handle(kInvalidHandle),
          reactionHead(kInvalidReactionIndex),
          settledFrame(0),
          settledSeconds(0.0),
          label{} {
        label.fill('\0');
    }

    void PromiseModule::PromiseRecord::Reset() noexcept {
        state = State::Pending;
        value = Value::Undefined();
        diagnostics.clear();
        handle = kInvalidHandle;
        reactionHead = kInvalidReactionIndex;
        settledFrame = 0;
        settledSeconds = 0.0;
        label[0] = '\0';
    }

    PromiseModule::PromiseSlot::PromiseSlot() noexcept : record(), generation(1) {
    }

    PromiseModule::ReactionSlot::ReactionSlot() noexcept : record{}, inUse(false), generation(1) {
        record.source = kInvalidHandle;
        record.derived = kInvalidHandle;
        record.onFulfilled = nullptr;
        record.onRejected = nullptr;
        record.userData = nullptr;
        record.next = kInvalidReactionIndex;
        record.label.fill('\0');
        record.sourceState = State::Pending;
        record.sourceValue = Value::Undefined();
        record.sourceDiagnostics.clear();
    }

    PromiseModule::PromiseModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_TotalSeconds(0.0),
          m_Promises(),
          m_FreePromises(),
          m_Settled(),
          m_Reactions(),
          m_FreeReactions(),
          m_MicrotaskQueue(),
          m_Metrics() {
    }

    std::string_view PromiseModule::Name() const noexcept {
        return kName;
    }

    std::string_view PromiseModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view PromiseModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void PromiseModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_CurrentFrame = 0;
        m_TotalSeconds = 0.0;
        m_Initialized = true;

        const auto promiseCapacity = RecommendPromiseCapacity(context.config.memory.heapBytes);
        const auto reactionCapacity = RecommendReactionCapacity(context.config.memory.heapBytes);
        Configure(promiseCapacity, reactionCapacity);
    }

    void PromiseModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        if (!m_Initialized) {
            return;
        }
        m_CurrentFrame = info.frameIndex;
        m_TotalSeconds += info.deltaSeconds;
        ProcessMicrotasks(m_MicrotaskQueue.size());
    }

    void PromiseModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
    }

    void PromiseModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
    }

    StatusCode PromiseModule::Configure(std::size_t promiseCapacity, std::size_t reactionCapacity) {
        if (promiseCapacity == 0) {
            promiseCapacity = kDefaultPromiseCapacity;
        }
        if (promiseCapacity > kMaxSlots) {
            promiseCapacity = kMaxSlots;
        }
        if (reactionCapacity == 0) {
            reactionCapacity = kDefaultReactionCapacity;
        }
        if (reactionCapacity > kMaxSlots) {
            reactionCapacity = kMaxSlots;
        }

        m_Promises.clear();
        m_Promises.resize(promiseCapacity);
        m_FreePromises.clear();
        m_FreePromises.reserve(promiseCapacity);
        for (std::size_t i = 0; i < promiseCapacity; ++i) {
            auto index = static_cast<std::uint32_t>(promiseCapacity - 1 - i);
            m_Promises[index].record.Reset();
            m_Promises[index].generation = 1;
            m_FreePromises.push_back(index);
        }

        m_Settled.clear();
        m_Settled.reserve(promiseCapacity);

        m_Reactions.clear();
        m_Reactions.resize(reactionCapacity);
        m_FreeReactions.clear();
        m_FreeReactions.reserve(reactionCapacity);
        for (std::size_t i = 0; i < reactionCapacity; ++i) {
            auto index = static_cast<std::uint32_t>(reactionCapacity - 1 - i);
            m_Reactions[index] = ReactionSlot();
            m_FreeReactions.push_back(index);
        }

        m_MicrotaskQueue.clear();
        m_MicrotaskQueue.reserve(reactionCapacity);
        m_Metrics = Metrics();
        return StatusCode::Ok;
    }

    StatusCode PromiseModule::CreatePromise(Handle &outHandle, const CreateOptions &options) {
        outHandle = kInvalidHandle;
        if (!m_Initialized) {
            return StatusCode::InternalError;
        }
        if (m_FreePromises.empty()) {
            m_Metrics.overflowPromises += 1;
            return StatusCode::CapacityExceeded;
        }
        auto index = m_FreePromises.back();
        m_FreePromises.pop_back();
        auto &slot = m_Promises[index];
        slot.record.Reset();
        slot.record.handle = MakeHandle(index, slot.generation);
        CopyLabel(options.label, slot.record.label);
        outHandle = slot.record.handle;
        m_Metrics.created += 1;
        const auto active = m_Promises.size() - m_FreePromises.size();
        if (active > m_Metrics.maxPromiseCount) {
            m_Metrics.maxPromiseCount = active;
        }
        return StatusCode::Ok;
    }

    StatusCode PromiseModule::Resolve(Handle handle, const Value &value, std::string_view diagnostics) {
        auto slot = ResolveSlot(handle);
        if (!slot) {
            return StatusCode::NotFound;
        }
        auto &record = slot->record;
        if (record.state != State::Pending) {
            return StatusCode::AlreadyExists;
        }
        RecordSettlement(record, State::Fulfilled, value, diagnostics);
        m_Metrics.resolved += 1;
        EnqueueReactions(record);
        return StatusCode::Ok;
    }

    StatusCode PromiseModule::Reject(Handle handle, std::string_view diagnostics, const Value &value) {
        auto slot = ResolveSlot(handle);
        if (!slot) {
            return StatusCode::NotFound;
        }
        auto &record = slot->record;
        if (record.state != State::Pending) {
            return StatusCode::AlreadyExists;
        }
        RecordSettlement(record, State::Rejected, value, diagnostics);
        m_Metrics.rejected += 1;
        EnqueueReactions(record);
        return StatusCode::Ok;
    }

    bool PromiseModule::Cancel(Handle handle) noexcept {
        auto slot = ResolveSlot(handle);
        if (!slot) {
            return false;
        }
        auto &record = slot->record;
        if (record.state != State::Pending) {
            return false;
        }
        RecordSettlement(record, State::Cancelled, Value::Undefined(), "cancelled");
        m_Metrics.cancelled += 1;
        EnqueueReactions(record);
        return true;
    }

    StatusCode PromiseModule::Release(Handle handle) noexcept {
        auto slot = ResolveSlot(handle);
        if (!slot) {
            return StatusCode::NotFound;
        }
        if (slot->record.reactionHead != kInvalidReactionIndex) {
            return StatusCode::AlreadyExists;
        }
        if (slot->record.state == State::Pending) {
            return StatusCode::InvalidArgument;
        }
        const auto index = ExtractIndex(handle);
        ReleasePromiseSlot(index);
        return StatusCode::Ok;
    }

    StatusCode PromiseModule::Then(Handle source,
                                   Handle &outDerived) {
        return Then(source, outDerived, ReactionOptions{});
    }

    StatusCode PromiseModule::Then(Handle source,
                                   Handle &outDerived,
                                   const ReactionOptions &options) {
        outDerived = kInvalidHandle;
        auto sourceSlot = ResolveSlot(source);
        if (!sourceSlot) {
            return StatusCode::NotFound;
        }
        if (m_FreeReactions.empty()) {
            m_Metrics.overflowReactions += 1;
            return StatusCode::CapacityExceeded;
        }
        auto status = CreatePromise(outDerived, {options.label});
        if (status != StatusCode::Ok) {
            return status;
        }

        auto reactionIndex = AcquireReactionSlot();
        auto &reactionSlot = m_Reactions[reactionIndex];
        reactionSlot.inUse = true;
        reactionSlot.record.source = sourceSlot->record.handle;
        reactionSlot.record.derived = outDerived;
        reactionSlot.record.onFulfilled = options.onFulfilled;
        reactionSlot.record.onRejected = options.onRejected;
        reactionSlot.record.userData = options.userData;
        CopyLabel(options.label, reactionSlot.record.label);
        reactionSlot.record.next = sourceSlot->record.reactionHead;
        sourceSlot->record.reactionHead = reactionIndex;

        if (sourceSlot->record.state != State::Pending) {
            EnqueueReactions(sourceSlot->record);
            m_Metrics.fastProcessed += 1;
        }

        m_Metrics.chained += 1;
        return StatusCode::Ok;
    }

    void PromiseModule::DrainSettled(std::vector<SettledPromise> &outPromises) {
        outPromises.reserve(outPromises.size() + m_Settled.size());
        for (auto &entry: m_Settled) {
            outPromises.push_back(std::move(entry));
        }
        m_Settled.clear();
    }

    PromiseModule::State PromiseModule::GetState(Handle handle) const noexcept {
        auto slot = ResolveSlot(handle);
        if (!slot) {
            return State::Cancelled;
        }
        return slot->record.state;
    }

    const PromiseModule::Metrics &PromiseModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    std::size_t PromiseModule::PendingMicrotasks() const noexcept {
        return m_MicrotaskQueue.size();
    }

    std::size_t PromiseModule::PromiseCount() const noexcept {
        return m_Promises.size() - m_FreePromises.size();
    }

    std::size_t PromiseModule::ReactionCapacity() const noexcept {
        return m_Reactions.size();
    }

    bool PromiseModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    PromiseModule::Handle PromiseModule::MakeHandle(std::size_t index, std::uint16_t generation) noexcept {
        return static_cast<Handle>((static_cast<std::uint32_t>(generation) << kHandleIndexBits)
                                   | static_cast<std::uint32_t>(index + 1));
    }

    std::size_t PromiseModule::ExtractIndex(Handle handle) noexcept {
        const auto part = handle & kHandleIndexMask;
        return part == 0 ? static_cast<std::size_t>(-1) : static_cast<std::size_t>(part - 1);
    }

    std::uint16_t PromiseModule::ExtractGeneration(Handle handle) noexcept {
        return static_cast<std::uint16_t>(handle >> kHandleIndexBits);
    }

    std::uint16_t PromiseModule::NextGeneration(std::uint16_t generation) noexcept {
        ++generation;
        if (generation == 0) {
            generation = 1;
        }
        return generation;
    }

    void PromiseModule::CopyLabel(std::string_view label,
                                  std::array<char, kMaxLabelLength + 1> &dest) noexcept {
        dest.fill('\0');
        if (label.empty()) {
            return;
        }
        const auto count = std::min<std::size_t>(label.size(), kMaxLabelLength);
        std::memcpy(dest.data(), label.data(), count);
        dest[count] = '\0';
    }

    PromiseModule::PromiseSlot *PromiseModule::ResolveSlot(Handle handle) noexcept {
        const auto index = ExtractIndex(handle);
        if (index >= m_Promises.size()) {
            return nullptr;
        }
        auto &slot = m_Promises[index];
        if (slot.record.handle != handle) {
            return nullptr;
        }
        if (slot.generation != ExtractGeneration(handle)) {
            return nullptr;
        }
        return &slot;
    }

    const PromiseModule::PromiseSlot *PromiseModule::ResolveSlot(Handle handle) const noexcept {
        const auto index = ExtractIndex(handle);
        if (index >= m_Promises.size()) {
            return nullptr;
        }
        const auto &slot = m_Promises[index];
        if (slot.record.handle != handle) {
            return nullptr;
        }
        if (slot.generation != ExtractGeneration(handle)) {
            return nullptr;
        }
        return &slot;
    }

    void PromiseModule::ReleasePromiseSlot(std::size_t index) noexcept {
        if (index >= m_Promises.size()) {
            return;
        }
        auto &slot = m_Promises[index];
        slot.record.Reset();
        slot.generation = NextGeneration(slot.generation);
        m_FreePromises.push_back(static_cast<std::uint32_t>(index));
    }

    std::uint32_t PromiseModule::AcquireReactionSlot() {
        if (m_FreeReactions.empty()) {
            return kInvalidReactionIndex;
        }
        auto index = m_FreeReactions.back();
        m_FreeReactions.pop_back();
        return index;
    }

    void PromiseModule::ReleaseReactionSlot(std::uint32_t index) noexcept {
        if (index >= m_Reactions.size()) {
            return;
        }
        auto &slot = m_Reactions[index];
        slot.inUse = false;
        slot.record = ReactionRecord{};
        slot.record.source = kInvalidHandle;
        slot.record.derived = kInvalidHandle;
        slot.record.onFulfilled = nullptr;
        slot.record.onRejected = nullptr;
        slot.record.userData = nullptr;
        slot.record.next = kInvalidReactionIndex;
        slot.record.label.fill('\0');
        slot.record.sourceState = State::Pending;
        slot.record.sourceValue = Value::Undefined();
        slot.record.sourceDiagnostics.clear();
        slot.generation = NextGeneration(slot.generation);
        m_FreeReactions.push_back(index);
    }

    void PromiseModule::EnqueueReactions(PromiseRecord &record) {
        auto index = record.reactionHead;
        record.reactionHead = kInvalidReactionIndex;
        while (index != kInvalidReactionIndex) {
            if (index >= m_Reactions.size()) {
                break;
            }
            auto &slot = m_Reactions[index];
            const auto next = slot.record.next;
            if (slot.inUse) {
                slot.record.sourceState = record.state;
                slot.record.sourceValue = record.value;
                slot.record.sourceDiagnostics = record.diagnostics;
                slot.record.next = kInvalidReactionIndex;
                m_MicrotaskQueue.push_back(index);
            }
            index = next;
        }
        if (m_MicrotaskQueue.size() > m_Metrics.maxReactionQueue) {
            m_Metrics.maxReactionQueue = m_MicrotaskQueue.size();
        }
    }

    void PromiseModule::ProcessMicrotasks(std::size_t budget) noexcept {
        std::size_t processed = 0;
        std::size_t limit = std::max<std::size_t>(budget, m_Reactions.empty() ? 1 : m_Reactions.size());
        const std::size_t hardCap = (m_Reactions.empty() ? 256u : m_Reactions.size() * 4u);
        while (!m_MicrotaskQueue.empty() && processed < limit) {
            auto index = m_MicrotaskQueue.back();
            m_MicrotaskQueue.pop_back();
            RunReaction(index);
            ++processed;
            if (!m_MicrotaskQueue.empty() && processed == limit) {
                const auto pending = m_MicrotaskQueue.size();
                limit += pending;
                if (limit > hardCap) {
                    limit = hardCap;
                }
                if (processed >= hardCap) {
                    break;
                }
            }
        }
        if (processed >= (m_Reactions.empty() ? 0 : hardCap) && !m_MicrotaskQueue.empty()) {
            m_MicrotaskQueue.clear();
        }
    }

    void PromiseModule::RunReaction(std::uint32_t reactionIndex) noexcept {
        if (reactionIndex >= m_Reactions.size()) {
            return;
        }
        auto &slot = m_Reactions[reactionIndex];
        if (!slot.inUse) {
            return;
        }
        if (!ResolveSlot(slot.record.derived)) {
            ReleaseReactionSlot(reactionIndex);
            return;
        }

        const auto state = slot.record.sourceState;
        const bool fulfilled = state == State::Fulfilled;
        const auto &sourceValue = slot.record.sourceValue;
        const auto &sourceDiagnostics = slot.record.sourceDiagnostics;

        ReactionCallback callback = fulfilled ? slot.record.onFulfilled : slot.record.onRejected;
        Value resultValue;
        std::string diagnostics;

        if (callback) {
            const auto status = callback(slot.record.userData,
                                         sourceValue,
                                         resultValue,
                                         diagnostics);
            if (status == StatusCode::Ok) {
                const auto &outValue = resultValue;
                const auto diagView = diagnostics.empty() ? std::string_view(sourceDiagnostics) : std::string_view(diagnostics);
                FulfillDerived(slot.record.derived, outValue, diagView);
            } else {
                if (diagnostics.empty()) {
                    diagnostics = "reaction failed";
                }
                RejectDerived(slot.record.derived,
                              diagnostics,
                              resultValue.IsUndefined() ? sourceValue : resultValue);
                m_Metrics.failedReactions += 1;
            }
        } else {
            if (fulfilled) {
                FulfillDerived(slot.record.derived, sourceValue, sourceDiagnostics);
            } else {
                RejectDerived(slot.record.derived, sourceDiagnostics, sourceValue);
            }
        }

        m_Metrics.executedReactions += 1;
        ReleaseReactionSlot(reactionIndex);
    }

    void PromiseModule::FulfillDerived(Handle handle,
                                       const Value &value,
                                       std::string_view diagnostics) {
        auto slot = ResolveSlot(handle);
        if (!slot) {
            return;
        }
        if (slot->record.state != State::Pending) {
            return;
        }
        RecordSettlement(slot->record, State::Fulfilled, value, diagnostics);
        m_Metrics.resolved += 1;
        EnqueueReactions(slot->record);
    }

    void PromiseModule::RejectDerived(Handle handle,
                                      std::string_view diagnostics,
                                      const Value &value) {
        auto slot = ResolveSlot(handle);
        if (!slot) {
            return;
        }
        if (slot->record.state != State::Pending) {
            return;
        }
        RecordSettlement(slot->record, State::Rejected, value, diagnostics);
        m_Metrics.rejected += 1;
        EnqueueReactions(slot->record);
    }

    void PromiseModule::RecordSettlement(PromiseRecord &record,
                                         State state,
                                         const Value &value,
                                         std::string_view diagnostics) {
        record.state = state;
        record.value = value;
        record.diagnostics.assign(diagnostics.begin(), diagnostics.end());
        record.settledFrame = m_CurrentFrame;
        record.settledSeconds = m_TotalSeconds;

        SettledPromise settled;
        settled.handle = record.handle;
        settled.state = state;
        settled.value = record.value;
        settled.diagnostics = record.diagnostics;
        settled.settledFrame = record.settledFrame;
        settled.settledSeconds = record.settledSeconds;
        settled.label = record.label;
        m_Settled.push_back(std::move(settled));

    }
}
