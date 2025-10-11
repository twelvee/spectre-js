#include "spectre/es2025/modules/finalization_registry_module.h"

#include <algorithm>
#include <utility>
#include <cstring>

#include "spectre/runtime.h"
#include "spectre/es2025/environment.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "FinalizationRegistry";
        constexpr std::string_view kSummary = "FinalizationRegistry scheduling and cleanup callbacks.";
        constexpr std::string_view kReference = "ECMA-262 Section 25.4";
    }

    FinalizationRegistryModule::CreateOptions::CreateOptions() noexcept
        : label(),
          defaultCleanup(nullptr),
          defaultUserData(nullptr),
          initialCapacity(16),
          autoCleanupBatch(8),
          autoCleanup(true) {
    }

    FinalizationRegistryModule::Metrics::Metrics() noexcept
        : registryCount(0),
          liveCells(0),
          pendingCells(0),
          reclaimedCells(0),
          registrations(0),
          unregistrations(0),
          processedHoldings(0),
          defaultCallbacks(0),
          manualCallbacks(0),
          failedRegistrations(0),
          lastFrameTouched(0),
          gpuOptimized(false) {
    }

    FinalizationRegistryModule::Cell::Cell() noexcept
        : target(0),
          unregisterToken(0),
          holdings(Value::Undefined()),
          tokenNext(kInvalidIndex),
          version(0),
          lastAliveFrame(0),
          inUse(false),
          pending(false) {
    }

    FinalizationRegistryModule::RegistryRecord::RegistryRecord() noexcept
        : handle(0),
          label(),
          defaultCleanup(nullptr),
          defaultUserData(nullptr),
          autoCleanupBatch(0),
          autoCleanup(true),
          cells(),
          freeCells(),
          pendingQueue(),
          pendingHead(0),
          tokenBuckets(),
          scanCursor(0),
          liveCells(0),
          pendingCells(0),
          lastCleanupFrame(0) {
    }

    FinalizationRegistryModule::SlotRecord::SlotRecord() noexcept
        : inUse(false),
          generation(0),
          record() {
    }

    FinalizationRegistryModule::FinalizationRegistryModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_ObjectModule(nullptr),
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Metrics(),
          m_Slots(),
          m_FreeSlots() {
    }

    std::string_view FinalizationRegistryModule::Name() const noexcept {
        return kName;
    }

    std::string_view FinalizationRegistryModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view FinalizationRegistryModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void FinalizationRegistryModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Initialized = true;
        m_CurrentFrame = 0;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_Slots.clear();
        m_FreeSlots.clear();

        auto &environment = context.runtime.EsEnvironment();
        auto *objectModule = environment.FindModule("Object");
        m_ObjectModule = dynamic_cast<ObjectModule *>(objectModule);
    }

    void FinalizationRegistryModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_Metrics.lastFrameTouched = m_CurrentFrame;

        if (!m_ObjectModule) {
            return;
        }

        for (auto &slot: m_Slots) {
            if (!slot.inUse) {
                continue;
            }
            auto &registry = slot.record;
            const auto total = static_cast<std::uint32_t>(registry.cells.size());
            if (total == 0) {
                RunAutoCleanup(registry);
                continue;
            }
            std::uint32_t cursor = registry.scanCursor;
            const std::uint32_t budget = std::min<std::uint32_t>(kScanBudgetPerRegistry, total);
            std::uint32_t inspected = 0;
            while (inspected < budget) {
                if (cursor >= total) {
                    cursor = 0;
                }
                auto &cell = registry.cells[cursor];
                if (cell.inUse && !cell.pending) {
                    if (cell.target == 0 || !m_ObjectModule->IsValid(cell.target)) {
                        QueueCell(registry, cursor);
                        cell.target = 0;
                    } else {
                        cell.lastAliveFrame = m_CurrentFrame;
                    }
                }
                ++inspected;
                ++cursor;
                if (inspected >= total) {
                    break;
                }
            }
            registry.scanCursor = total == 0 ? 0 : (cursor % total);
            RunAutoCleanup(registry);
        }
    }

    void FinalizationRegistryModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void FinalizationRegistryModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode FinalizationRegistryModule::Create(const CreateOptions &options, Handle &outHandle) {
        outHandle = 0;
        if (!m_Initialized) {
            return StatusCode::InternalError;
        }

        std::uint32_t slotIndex;
        SlotRecord *slot;
        if (!m_FreeSlots.empty()) {
            slotIndex = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            slot = &m_Slots[slotIndex];
            slot->inUse = true;
            slot->generation += 1;
        } else {
            slotIndex = static_cast<std::uint32_t>(m_Slots.size());
            m_Slots.emplace_back();
            slot = &m_Slots.back();
            slot->inUse = true;
            slot->generation = 1;
        }

        auto &registry = slot->record;
        registry.handle = EncodeHandle(slotIndex, slot->generation);
        registry.label.assign(options.label.data(), options.label.size());
        registry.defaultCleanup = options.defaultCleanup;
        registry.defaultUserData = options.defaultUserData;
        registry.autoCleanupBatch = options.autoCleanupBatch;
        registry.autoCleanup = options.autoCleanup;
        registry.scanCursor = 0;
        registry.liveCells = 0;
        registry.pendingCells = 0;
        registry.lastCleanupFrame = m_CurrentFrame;

        registry.cells.clear();
        registry.freeCells.clear();
        registry.pendingQueue.clear();
        registry.pendingHead = 0;
        if (!registry.tokenBuckets.empty()) {
            std::fill(registry.tokenBuckets.begin(), registry.tokenBuckets.end(), kInvalidIndex);
        }

        const std::uint32_t reserveCount = std::max<std::uint32_t>(options.initialCapacity, 8);
        registry.cells.reserve(reserveCount);
        registry.pendingQueue.reserve(reserveCount);
        if (registry.tokenBuckets.empty()) {
            registry.tokenBuckets.assign(kDefaultTokenBucketCount, kInvalidIndex);
        }

        outHandle = registry.handle;
        m_Metrics.registryCount += 1;
        return StatusCode::Ok;
    }

    StatusCode FinalizationRegistryModule::Destroy(Handle handle) {
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return StatusCode::InvalidArgument;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse || slot.generation != DecodeGeneration(handle)) {
            return StatusCode::InvalidArgument;
        }

        auto &registry = slot.record;
        for (std::uint32_t index = 0; index < registry.cells.size(); ++index) {
            if (!registry.cells[index].inUse) {
                continue;
            }
            RemoveToken(registry, index);
            ReleaseCell(registry, index, false);
        }
        registry.cells.clear();
        registry.freeCells.clear();
        registry.pendingQueue.clear();
        registry.pendingHead = 0;
        if (!registry.tokenBuckets.empty()) {
            std::fill(registry.tokenBuckets.begin(), registry.tokenBuckets.end(), kInvalidIndex);
        }
        registry.liveCells = 0;
        registry.pendingCells = 0;
        registry.handle = 0;
        registry.scanCursor = 0;

        slot.inUse = false;
        slot.generation += 1;
        m_FreeSlots.push_back(slotIndex);
        if (m_Metrics.registryCount > 0) {
            m_Metrics.registryCount -= 1;
        }
        return StatusCode::Ok;
    }

    StatusCode FinalizationRegistryModule::Register(Handle handle,
                                                    ObjectModule::Handle target,
                                                    const Value &holdings,
                                                    ObjectModule::Handle unregisterToken) {
        auto *registry = FindRegistry(handle);
        if (!registry || !m_ObjectModule) {
            m_Metrics.failedRegistrations += 1;
            return StatusCode::InvalidArgument;
        }
        if (target == 0 || !m_ObjectModule->IsValid(target)) {
            m_Metrics.failedRegistrations += 1;
            return StatusCode::InvalidArgument;
        }

        const auto cellIndex = AcquireCell(*registry);
        auto &cell = registry->cells[cellIndex];
        cell.target = target;
        cell.unregisterToken = unregisterToken;
        cell.holdings = holdings;
        cell.pending = false;
        cell.tokenNext = kInvalidIndex;
        cell.lastAliveFrame = m_CurrentFrame;

        if (unregisterToken != 0) {
            EnsureTokenCapacity(*registry);
            InsertToken(*registry, cellIndex);
        }

        m_Metrics.registrations += 1;
        return StatusCode::Ok;
    }

    StatusCode FinalizationRegistryModule::Unregister(Handle handle,
                                                      ObjectModule::Handle unregisterToken,
                                                      bool &outRemoved) {
        outRemoved = false;
        auto *registry = FindRegistry(handle);
        if (!registry) {
            return StatusCode::InvalidArgument;
        }
        if (unregisterToken == 0 || registry->tokenBuckets.empty()) {
            return StatusCode::Ok;
        }

        const std::uint32_t mask = static_cast<std::uint32_t>(registry->tokenBuckets.size() - 1);
        auto bucketIndex = static_cast<std::uint32_t>(unregisterToken & mask);
        std::uint32_t prev = kInvalidIndex;
        std::uint32_t current = registry->tokenBuckets[bucketIndex];
        std::uint32_t removedCount = 0;

        while (current != kInvalidIndex) {
            auto &cell = registry->cells[current];
            auto next = cell.tokenNext;
            if (cell.inUse && cell.unregisterToken == unregisterToken) {
                if (prev == kInvalidIndex) {
                    registry->tokenBuckets[bucketIndex] = next;
                } else {
                    registry->cells[prev].tokenNext = next;
                }
                cell.tokenNext = kInvalidIndex;
                ReleaseCell(*registry, current, false);
                removedCount += 1;
                current = next;
                continue;
            }
            prev = current;
            current = next;
        }

        if (removedCount > 0) {
            outRemoved = true;
            m_Metrics.unregistrations += removedCount;
        }
        return StatusCode::Ok;
    }

    StatusCode FinalizationRegistryModule::CleanupSome(Handle handle,
                                                       CleanupCallback callback,
                                                       void *userData,
                                                       std::uint32_t limit,
                                                       std::uint32_t &outProcessed) {
        outProcessed = 0;
        auto *registry = FindRegistry(handle);
        if (!registry) {
            return StatusCode::InvalidArgument;
        }

        CleanupCallback effectiveCallback = callback ? callback : registry->defaultCleanup;
        void *effectiveUserData = callback ? userData : registry->defaultUserData;
        if (!effectiveCallback) {
            return StatusCode::InvalidArgument;
        }

        if (limit == 0) {
            limit = registry->pendingCells;
        }

        std::uint32_t processed = 0;
        while (processed < limit) {
            std::uint32_t cellIndex = 0;
            if (!DequeueCell(*registry, cellIndex)) {
                break;
            }
            if (cellIndex >= registry->cells.size()) {
                continue;
            }
            auto &cell = registry->cells[cellIndex];
            if (!cell.inUse || !cell.pending) {
                continue;
            }
            if (cell.unregisterToken != 0) {
                RemoveToken(*registry, cellIndex);
            }
            Value holdings(std::move(cell.holdings));
            ReleaseCell(*registry, cellIndex, true);
            effectiveCallback(holdings, effectiveUserData);
            processed += 1;
            m_Metrics.processedHoldings += 1;
            if (callback) {
                m_Metrics.manualCallbacks += 1;
            } else {
                m_Metrics.defaultCallbacks += 1;
            }
        }

        outProcessed = processed;
        registry->lastCleanupFrame = m_CurrentFrame;
        return StatusCode::Ok;
    }

    std::uint32_t FinalizationRegistryModule::LiveCellCount(Handle handle) const noexcept {
        const auto *registry = FindRegistry(handle);
        return registry ? registry->liveCells : 0;
    }

    std::uint32_t FinalizationRegistryModule::PendingCount(Handle handle) const noexcept {
        const auto *registry = FindRegistry(handle);
        return registry ? registry->pendingCells : 0;
    }

    const FinalizationRegistryModule::Metrics &FinalizationRegistryModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    FinalizationRegistryModule::Handle FinalizationRegistryModule::EncodeHandle(std::uint32_t slot,
                                                                                std::uint32_t generation) noexcept {
        return (static_cast<Handle>(generation) << 32u) | static_cast<Handle>(slot);
    }

    std::uint32_t FinalizationRegistryModule::DecodeSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & 0xffffffffu);
    }

    std::uint32_t FinalizationRegistryModule::DecodeGeneration(Handle handle) noexcept {
        return static_cast<std::uint32_t>((handle >> 32u) & 0xffffffffu);
    }

    FinalizationRegistryModule::SlotRecord *FinalizationRegistryModule::FindMutableSlot(Handle handle) noexcept {
        if (handle == 0) {
            return nullptr;
        }
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse || slot.generation != DecodeGeneration(handle)) {
            return nullptr;
        }
        return &slot;
    }

    const FinalizationRegistryModule::SlotRecord *FinalizationRegistryModule::FindSlot(Handle handle) const noexcept {
        if (handle == 0) {
            return nullptr;
        }
        auto slotIndex = DecodeSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        const auto &slot = m_Slots[slotIndex];
        if (!slot.inUse || slot.generation != DecodeGeneration(handle)) {
            return nullptr;
        }
        return &slot;
    }

    FinalizationRegistryModule::RegistryRecord *FinalizationRegistryModule::FindRegistry(Handle handle) noexcept {
        auto *slot = FindMutableSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    const FinalizationRegistryModule::RegistryRecord *FinalizationRegistryModule::FindRegistry(Handle handle) const noexcept {
        auto *slot = FindSlot(handle);
        return slot ? &slot->record : nullptr;
    }

    std::uint32_t FinalizationRegistryModule::AcquireCell(RegistryRecord &registry) {
        std::uint32_t index;
        if (!registry.freeCells.empty()) {
            index = registry.freeCells.back();
            registry.freeCells.pop_back();
            auto &cell = registry.cells[index];
            cell.inUse = true;
            cell.pending = false;
            cell.target = 0;
            cell.unregisterToken = 0;
            cell.holdings = Value::Undefined();
            cell.tokenNext = kInvalidIndex;
            cell.version += 1;
        } else {
            Cell cell;
            cell.inUse = true;
            cell.version = 1;
            registry.cells.push_back(cell);
            index = static_cast<std::uint32_t>(registry.cells.size() - 1);
        }
        registry.liveCells += 1;
        m_Metrics.liveCells += 1;
        return index;
    }

    void FinalizationRegistryModule::ReleaseCell(RegistryRecord &registry,
                                                 std::uint32_t index,
                                                 bool processed) noexcept {
        if (index >= registry.cells.size()) {
            return;
        }
        auto &cell = registry.cells[index];
        if (!cell.inUse) {
            cell.pending = false;
            cell.tokenNext = kInvalidIndex;
            return;
        }
        if (cell.pending) {
            if (registry.pendingCells > 0) {
                registry.pendingCells -= 1;
            }
            if (m_Metrics.pendingCells > 0) {
                m_Metrics.pendingCells -= 1;
            }
        }
        if (registry.liveCells > 0) {
            registry.liveCells -= 1;
        }
        if (m_Metrics.liveCells > 0) {
            m_Metrics.liveCells -= 1;
        }
        cell.inUse = false;
        cell.pending = false;
        cell.target = 0;
        cell.unregisterToken = 0;
        cell.holdings = Value::Undefined();
        cell.tokenNext = kInvalidIndex;
        registry.freeCells.push_back(index);
        if (processed) {
            m_Metrics.reclaimedCells += 1;
        }
    }

    void FinalizationRegistryModule::EnsureTokenCapacity(RegistryRecord &registry) {
        if (registry.tokenBuckets.empty()) {
            registry.tokenBuckets.assign(kDefaultTokenBucketCount, kInvalidIndex);
            return;
        }
        auto bucketCount = registry.tokenBuckets.size();
        if (bucketCount == 0) {
            registry.tokenBuckets.assign(kDefaultTokenBucketCount, kInvalidIndex);
            return;
        }
        if (registry.liveCells <= bucketCount * 2) {
            return;
        }
        std::size_t newCount = bucketCount;
        while (registry.liveCells > newCount * 2) {
            newCount *= 2;
        }
        std::vector<std::uint32_t> rehashed(newCount, kInvalidIndex);
        for (std::uint32_t index = 0; index < registry.cells.size(); ++index) {
            auto &cell = registry.cells[index];
            if (!cell.inUse || cell.unregisterToken == 0) {
                cell.tokenNext = kInvalidIndex;
                continue;
            }
            const auto mask = static_cast<std::uint32_t>(newCount - 1);
            const auto bucket = static_cast<std::uint32_t>(cell.unregisterToken & mask);
            cell.tokenNext = rehashed[bucket];
            rehashed[bucket] = index;
        }
        registry.tokenBuckets.swap(rehashed);
    }

    void FinalizationRegistryModule::InsertToken(RegistryRecord &registry, std::uint32_t cellIndex) noexcept {
        if (registry.tokenBuckets.empty() || cellIndex >= registry.cells.size()) {
            return;
        }
        auto &cell = registry.cells[cellIndex];
        if (cell.unregisterToken == 0) {
            cell.tokenNext = kInvalidIndex;
            return;
        }
        const auto mask = static_cast<std::uint32_t>(registry.tokenBuckets.size() - 1);
        const auto bucket = static_cast<std::uint32_t>(cell.unregisterToken & mask);
        cell.tokenNext = registry.tokenBuckets[bucket];
        registry.tokenBuckets[bucket] = cellIndex;
    }

    void FinalizationRegistryModule::RemoveToken(RegistryRecord &registry, std::uint32_t cellIndex) noexcept {
        if (registry.tokenBuckets.empty() || cellIndex >= registry.cells.size()) {
            return;
        }
        auto token = registry.cells[cellIndex].unregisterToken;
        if (token == 0) {
            return;
        }
        const auto mask = static_cast<std::uint32_t>(registry.tokenBuckets.size() - 1);
        const auto bucket = static_cast<std::uint32_t>(token & mask);
        auto &head = registry.tokenBuckets[bucket];
        std::uint32_t prev = kInvalidIndex;
        std::uint32_t current = head;
        while (current != kInvalidIndex) {
            if (current == cellIndex) {
                auto next = registry.cells[current].tokenNext;
                if (prev == kInvalidIndex) {
                    head = next;
                } else {
                    registry.cells[prev].tokenNext = next;
                }
                registry.cells[current].tokenNext = kInvalidIndex;
                break;
            }
            prev = current;
            current = registry.cells[current].tokenNext;
        }
    }

    void FinalizationRegistryModule::TrimPendingQueue(RegistryRecord &registry) noexcept {
        if (registry.pendingHead == 0) {
            return;
        }
        const auto queued = registry.pendingQueue.size();
        if (registry.pendingHead >= queued) {
            registry.pendingQueue.clear();
            registry.pendingHead = 0;
            return;
        }
        const auto pending = queued - registry.pendingHead;
        if (pending <= queued / 2) {
            std::memmove(registry.pendingQueue.data(),
                         registry.pendingQueue.data() + registry.pendingHead,
                         pending * sizeof(std::uint32_t));
            registry.pendingQueue.resize(pending);
            registry.pendingHead = 0;
        }
    }

    void FinalizationRegistryModule::QueueCell(RegistryRecord &registry, std::uint32_t index) noexcept {
        if (index >= registry.cells.size()) {
            return;
        }
        auto &cell = registry.cells[index];
        if (!cell.inUse || cell.pending) {
            return;
        }
        cell.pending = true;
        TrimPendingQueue(registry);
        if (registry.pendingQueue.size() == registry.pendingQueue.capacity()) {
            const std::size_t grow = registry.pendingQueue.capacity() == 0 ? 8 : registry.pendingQueue.capacity() * 2;
            registry.pendingQueue.reserve(grow);
        }
        registry.pendingQueue.push_back(index);
        registry.pendingCells += 1;
        m_Metrics.pendingCells += 1;
    }

    bool FinalizationRegistryModule::DequeueCell(RegistryRecord &registry, std::uint32_t &outIndex) noexcept {
        while (registry.pendingHead < registry.pendingQueue.size()) {
            outIndex = registry.pendingQueue[registry.pendingHead++];
            if (registry.pendingHead >= registry.pendingQueue.size()) {
                registry.pendingQueue.clear();
                registry.pendingHead = 0;
            } else if (registry.pendingHead >= registry.pendingQueue.size() / 2) {
                TrimPendingQueue(registry);
            }
            return true;
        }
        registry.pendingQueue.clear();
        registry.pendingHead = 0;
        return false;
    }

    void FinalizationRegistryModule::RunAutoCleanup(RegistryRecord &registry) noexcept {
        if (!registry.autoCleanup || registry.pendingCells == 0 || registry.defaultCleanup == nullptr) {
            return;
        }
        std::uint32_t limit = registry.autoCleanupBatch;
        if (limit == 0 || limit > registry.pendingCells) {
            limit = registry.pendingCells;
        }
        std::uint32_t processed = 0;
        while (processed < limit) {
            std::uint32_t cellIndex = 0;
            if (!DequeueCell(registry, cellIndex)) {
                break;
            }
            if (cellIndex >= registry.cells.size()) {
                continue;
            }
            auto &cell = registry.cells[cellIndex];
            if (!cell.inUse || !cell.pending) {
                continue;
            }
            if (cell.unregisterToken != 0) {
                RemoveToken(registry, cellIndex);
            }
            Value holdings(std::move(cell.holdings));
            ReleaseCell(registry, cellIndex, true);
            registry.defaultCleanup(holdings, registry.defaultUserData);
            processed += 1;
            m_Metrics.processedHoldings += 1;
            m_Metrics.defaultCallbacks += 1;
        }
        if (processed > 0) {
            registry.lastCleanupFrame = m_CurrentFrame;
        }
    }
}




