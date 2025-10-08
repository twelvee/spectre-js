#include "spectre/es2025/modules/async_function_module.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "AsyncFunction";
        constexpr std::string_view kSummary =
                "AsyncFunction constructor bridging asynchronous job queues to host ticks.";
        constexpr std::string_view kReference = "ECMA-262 Section 27.7";
        constexpr std::size_t kDefaultQueueCapacity = 128;
        constexpr std::size_t kDefaultCompletionCapacity = 64;
        constexpr double kSecondsEpsilon = 1e-9;
        constexpr std::size_t kInvalidIndex = std::numeric_limits<std::size_t>::max();
        constexpr std::size_t kHandleIndexBits = 16;
        constexpr std::size_t kMaxSlotEstimate = (1u << kHandleIndexBits) - 1;

        std::size_t RecommendCapacity(std::uint64_t heapBytes) noexcept {
            if (heapBytes == 0) {
                return kDefaultQueueCapacity;
            }
            auto scaled = heapBytes / 32768ULL;
            if (scaled < 32) {
                scaled = 32;
            }
            if (scaled > kMaxSlotEstimate) {
                scaled = kMaxSlotEstimate;
            }
            return static_cast<std::size_t>(scaled);
        }
    }

    AsyncFunctionModule::Job::Job() noexcept
        : callback(nullptr),
          userData(nullptr),
          handle(kInvalidHandle),
          frameDeadline(0),
          secondsDeadline(0.0),
          sequence(0),
          inUse(false),
          fastPath(false),
          label{} {
        label.fill('\0');
    }

    void AsyncFunctionModule::Job::Reset() noexcept {
        callback = nullptr;
        userData = nullptr;
        handle = kInvalidHandle;
        frameDeadline = 0;
        secondsDeadline = 0.0;
        sequence = 0;
        inUse = false;
        fastPath = false;
        label[0] = '\0';
    }

    AsyncFunctionModule::Slot::Slot() noexcept : job(), generation(1) {
    }

    AsyncFunctionModule::AsyncFunctionModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_TotalSeconds(0.0),
          m_SequenceCounter(0),
          m_Capacity(0),
          m_Slots(),
          m_FreeList(),
          m_WaitingQueue(),
          m_ReadyQueue(),
          m_Completed(),
          m_Metrics() {
    }

    std::string_view AsyncFunctionModule::Name() const noexcept {
        return kName;
    }

    std::string_view AsyncFunctionModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view AsyncFunctionModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void AsyncFunctionModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Initialized = true;
        m_CurrentFrame = 0;
        m_TotalSeconds = 0.0;
        m_SequenceCounter = 0;

        const auto recommended = RecommendCapacity(context.config.memory.heapBytes);
        const auto completion = std::max<std::size_t>(recommended / 2, kDefaultCompletionCapacity);
        Configure(recommended, completion);
    }

    void AsyncFunctionModule::Tick(const TickInfo &info, const ModuleTickContext &context) noexcept {
        (void) context;
        if (!m_Initialized) {
            return;
        }

        m_CurrentFrame = info.frameIndex;
        m_TotalSeconds += info.deltaSeconds;

        // Move ready jobs into the execution queue while preserving submission order.
        std::size_t writeIndex = 0;
        for (std::size_t readIndex = 0; readIndex < m_WaitingQueue.size(); ++readIndex) {
            const auto handle = m_WaitingQueue[readIndex];
            auto slot = ResolveSlot(handle);
            if (!slot) {
                continue;
            }
            if (ReadyForExecution(slot->job)) {
                m_ReadyQueue.push_back(handle);
            } else {
                m_WaitingQueue[writeIndex++] = handle;
            }
        }
        m_WaitingQueue.resize(writeIndex);

        for (const auto handle: m_ReadyQueue) {
            Execute(handle);
        }
        m_ReadyQueue.clear();
    }

    void AsyncFunctionModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
    }

    void AsyncFunctionModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
    }

    StatusCode AsyncFunctionModule::Configure(std::size_t queueCapacity, std::size_t completionCapacity) {
        if (queueCapacity == 0) {
            queueCapacity = kDefaultQueueCapacity;
        }
        if (queueCapacity > kMaxSlots) {
            queueCapacity = kMaxSlots;
        }

        m_Capacity = queueCapacity;
        m_Slots.clear();
        m_Slots.resize(m_Capacity);
        m_FreeList.clear();
        m_FreeList.reserve(m_Capacity);
        for (std::size_t i = 0; i < m_Capacity; ++i) {
            m_Slots[i].job.Reset();
            m_Slots[i].generation = 1;
            m_FreeList.push_back(static_cast<std::uint32_t>(m_Capacity - 1 - i));
        }

        m_WaitingQueue.clear();
        m_WaitingQueue.reserve(m_Capacity);
        m_ReadyQueue.clear();
        m_ReadyQueue.reserve(m_Capacity);
        m_Completed.clear();
        const auto completionReserve = completionCapacity == 0
                                        ? std::max<std::size_t>(m_Capacity / 2, kDefaultCompletionCapacity)
                                        : completionCapacity;
        m_Completed.reserve(completionReserve);
        m_Metrics = Metrics();
        m_SequenceCounter = 0;
        return StatusCode::Ok;
    }

    StatusCode AsyncFunctionModule::Enqueue(Callback callback,
                                            void *userData,
                                            const DispatchOptions &options,
                                            Handle &outHandle) {
        outHandle = kInvalidHandle;
        if (callback == nullptr) {
            return StatusCode::InvalidArgument;
        }
        if (!m_Initialized || m_Capacity == 0) {
            return StatusCode::InternalError;
        }

        const auto slotIndex = AcquireSlot();
        if (slotIndex == kInvalidIndex) {
            m_Metrics.overflow += 1;
            return StatusCode::CapacityExceeded;
        }

        auto &slot = m_Slots[slotIndex];
        slot.job.Reset();
        slot.job.callback = callback;
        slot.job.userData = userData;
        slot.job.inUse = true;
        slot.job.sequence = ++m_SequenceCounter;
        slot.job.frameDeadline = m_CurrentFrame + static_cast<std::uint64_t>(options.delayFrames);
        const double delaySeconds = options.delaySeconds > 0.0 ? options.delaySeconds : 0.0;
        slot.job.secondsDeadline = m_TotalSeconds + delaySeconds;
        slot.job.fastPath = (options.delayFrames == 0 && delaySeconds <= 0.0);
        CopyLabel(options.label, slot.job.label);
        slot.job.handle = MakeHandle(slotIndex, slot.generation);
        outHandle = slot.job.handle;

        if (slot.job.fastPath) {
            m_ReadyQueue.push_back(outHandle);
            m_Metrics.fastPath += 1;
        } else {
            m_WaitingQueue.push_back(outHandle);
        }

        m_Metrics.enqueued += 1;
        const auto queueDepth = m_WaitingQueue.size() + m_ReadyQueue.size();
        if (queueDepth > m_Metrics.maxQueueDepth) {
            m_Metrics.maxQueueDepth = queueDepth;
        }
        return StatusCode::Ok;
    }

    bool AsyncFunctionModule::Cancel(Handle handle) noexcept {
        auto slot = ResolveSlot(handle);
        if (!slot) {
            return false;
        }

        const auto index = ExtractIndex(handle);
        ReleaseSlot(index);
        RemoveFromQueue(m_WaitingQueue, handle);
        RemoveFromQueue(m_ReadyQueue, handle);
        m_Metrics.cancelled += 1;
        return true;
    }

    void AsyncFunctionModule::DrainCompleted(std::vector<Result> &outResults) {
        outResults.reserve(outResults.size() + m_Completed.size());
        for (auto &result: m_Completed) {
            outResults.push_back(std::move(result));
        }
        m_Completed.clear();
    }

    const AsyncFunctionModule::Metrics &AsyncFunctionModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    std::size_t AsyncFunctionModule::PendingCount() const noexcept {
        return m_WaitingQueue.size() + m_ReadyQueue.size();
    }

    std::size_t AsyncFunctionModule::Capacity() const noexcept {
        return m_Capacity;
    }

    bool AsyncFunctionModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    AsyncFunctionModule::Handle AsyncFunctionModule::MakeHandle(std::size_t index, std::uint16_t generation) noexcept {
        return static_cast<Handle>((static_cast<std::uint32_t>(generation) << kHandleIndexBits)
                                   | static_cast<std::uint32_t>(index + 1));
    }

    std::size_t AsyncFunctionModule::ExtractIndex(Handle handle) noexcept {
        const auto indexPart = handle & kHandleIndexMask;
        return indexPart == 0 ? kInvalidIndex : static_cast<std::size_t>(indexPart - 1);
    }

    std::uint16_t AsyncFunctionModule::ExtractGeneration(Handle handle) noexcept {
        return static_cast<std::uint16_t>(handle >> kHandleIndexBits);
    }

    std::uint16_t AsyncFunctionModule::NextGeneration(std::uint16_t generation) noexcept {
        ++generation;
        if (generation == 0) {
            generation = 1;
        }
        return generation;
    }

    void AsyncFunctionModule::CopyLabel(std::string_view label,
                                        std::array<char, kMaxLabelLength + 1> &dest) noexcept {
        dest.fill('\0');
        if (label.empty()) {
            return;
        }
        const auto count = std::min<std::size_t>(label.size(), kMaxLabelLength);
        std::memcpy(dest.data(), label.data(), count);
        dest[count] = '\0';
    }

    AsyncFunctionModule::Slot *AsyncFunctionModule::ResolveSlot(Handle handle) noexcept {
        const auto index = ExtractIndex(handle);
        if (index >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[index];
        if (!slot.job.inUse) {
            return nullptr;
        }
        if (slot.generation != ExtractGeneration(handle)) {
            return nullptr;
        }
        if (slot.job.handle != handle) {
            return nullptr;
        }
        return &slot;
    }

    const AsyncFunctionModule::Slot *AsyncFunctionModule::ResolveSlot(Handle handle) const noexcept {
        const auto index = ExtractIndex(handle);
        if (index >= m_Slots.size()) {
            return nullptr;
        }
        const auto &slot = m_Slots[index];
        if (!slot.job.inUse) {
            return nullptr;
        }
        if (slot.generation != ExtractGeneration(handle)) {
            return nullptr;
        }
        if (slot.job.handle != handle) {
            return nullptr;
        }
        return &slot;
    }

    bool AsyncFunctionModule::ReadyForExecution(const Job &job) const noexcept {
        if (m_CurrentFrame < job.frameDeadline) {
            return false;
        }
        if (m_TotalSeconds + kSecondsEpsilon < job.secondsDeadline) {
            return false;
        }
        return true;
    }

    std::size_t AsyncFunctionModule::AcquireSlot() noexcept {
        if (m_FreeList.empty()) {
            return kInvalidIndex;
        }
        const auto index = static_cast<std::size_t>(m_FreeList.back());
        m_FreeList.pop_back();
        return index;
    }

    void AsyncFunctionModule::ReleaseSlot(std::size_t index) noexcept {
        if (index >= m_Slots.size()) {
            return;
        }
        auto &slot = m_Slots[index];
        slot.job.Reset();
        slot.generation = NextGeneration(slot.generation);
        m_FreeList.push_back(static_cast<std::uint32_t>(index));
    }

    void AsyncFunctionModule::Execute(Handle handle) noexcept {
        auto slot = ResolveSlot(handle);
        if (!slot) {
            return;
        }

        Value value;
        std::string diagnostics;
        const auto start = std::chrono::steady_clock::now();
        const auto status = slot->job.callback(slot->job.userData, value, diagnostics);
        const auto end = std::chrono::steady_clock::now();
        const double micros = std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(end - start).count();

        Result result;
        result.handle = handle;
        result.status = status;
        result.value = std::move(value);
        result.diagnostics = std::move(diagnostics);
        result.completedFrame = m_CurrentFrame;
        result.executionMicros = micros;
        result.label = slot->job.label;
        m_Completed.push_back(std::move(result));

        m_Metrics.executed += 1;
        m_Metrics.lastExecutionMicros = micros;
        m_Metrics.totalExecutionMicros += micros;
        if (status != StatusCode::Ok) {
            m_Metrics.failed += 1;
        }

        const auto index = ExtractIndex(handle);
        ReleaseSlot(index);
    }

    void AsyncFunctionModule::RemoveFromQueue(std::vector<Handle> &queue, Handle handle) noexcept {
        const auto it = std::remove(queue.begin(), queue.end(), handle);
        queue.erase(it, queue.end());
    }
}
