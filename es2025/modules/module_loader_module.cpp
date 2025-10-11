#include "spectre/es2025/modules/module_loader_module.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "ModuleLoader";
        constexpr std::string_view kSummary =
                "ES module graph loading, linking, and evaluation pipeline.";
        constexpr std::string_view kReference = "ECMA-262 Section 29";
        constexpr std::string_view kDefaultContext = "modules.main";
        constexpr std::uint32_t kMinimumStackSize = 1u << 15;

        constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
        constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
        constexpr char kTemplateQuote = static_cast<char>(0x60);

        std::uint64_t HashString(std::string_view value) noexcept {
            std::uint64_t hash = kFnvOffset;
            for (unsigned char ch: value) {
                hash ^= static_cast<std::uint64_t>(ch);
                hash *= kFnvPrime;
            }
            return hash;
        }

        bool IsIdentifierChar(char ch) noexcept {
            return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '$';
        }

        std::size_t SkipWhitespace(std::string_view text, std::size_t index) noexcept {
            const auto size = text.size();
            while (index < size) {
                unsigned char ch = static_cast<unsigned char>(text[index]);
                if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f') {
                    break;
                }
                ++index;
            }
            return index;
        }
    }

    ModuleLoaderModule::ModuleRecord::ModuleRecord() noexcept
        : handle(kInvalidHandle),
          specifier(),
          source(),
          contextName(),
          explicitStackSize(0),
          dependencies(),
          dependents(),
          sourceHash(0),
          dependencyStamp(0),
          version(0),
          state(State::Unresolved),
          lastStatus(StatusCode::NotFound),
          diagnostics(),
          lastValue(),
          dirty(true),
          evaluating(false) {
    }

    void ModuleLoaderModule::ModuleRecord::Reset() noexcept {
        handle = kInvalidHandle;
        specifier.clear();
        source.clear();
        contextName.clear();
        explicitStackSize = 0;
        dependencies.clear();
        dependents.clear();
        sourceHash = 0;
        dependencyStamp = 0;
        version = 0;
        state = State::Unresolved;
        lastStatus = StatusCode::NotFound;
        diagnostics.clear();
        lastValue.clear();
        dirty = true;
        evaluating = false;
    }

    ModuleLoaderModule::Slot::Slot() noexcept : record(), generation(1), inUse(false) {
    }

    ModuleLoaderModule::WorkItem::WorkItem(Handle h, std::size_t index) noexcept
        : handle(h), nextDependency(index) {
    }

    ModuleLoaderModule::ModuleLoaderModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_TotalSeconds(0.0),
          m_DefaultContextName(kDefaultContext),
          m_DefaultStackSize(kMinimumStackSize),
          m_Resolver(nullptr),
          m_ResolverUserData(nullptr),
          m_Slots(),
          m_FreeList(),
          m_DfsMarks(),
          m_DirtyMarks(),
          m_WorkStack(),
          m_EvaluationOrder(),
          m_ScratchDependencies(),
          m_DirtyStack(),
          m_SpecifierLookup(),
          m_ContextLookup(),
          m_Metrics(),
          m_Mutex() {
        m_Slots.reserve(32);
        m_FreeList.reserve(32);
        m_DfsMarks.reserve(32);
        m_DirtyMarks.reserve(32);
        m_WorkStack.reserve(32);
        m_EvaluationOrder.reserve(32);
        m_ScratchDependencies.reserve(8);
        m_DirtyStack.reserve(16);
    }

    std::string_view ModuleLoaderModule::Name() const noexcept {
        return kName;
    }

    std::string_view ModuleLoaderModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view ModuleLoaderModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void ModuleLoaderModule::Initialize(const ModuleInitContext &context) {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_CurrentFrame = 0;
        m_TotalSeconds = 0.0;
        m_Initialized = true;
        m_DefaultContextName = std::string(kDefaultContext);
        auto heapKilobytes = static_cast<std::uint32_t>(context.config.memory.heapBytes / 1024ULL);
        if (heapKilobytes > 0) {
            std::uint32_t suggested = std::min<std::uint32_t>(heapKilobytes * 2u, 1u << 20);
            m_DefaultStackSize = std::max(kMinimumStackSize, suggested);
        } else {
            m_DefaultStackSize = kMinimumStackSize;
        }
        m_ContextLookup.clear();
        ResetMetrics();
        (void) EnsureContextUnlocked(m_DefaultContextName, m_DefaultStackSize, lock);
    }

    void ModuleLoaderModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Initialized) {
            return;
        }
        m_CurrentFrame = info.frameIndex;
        m_TotalSeconds += info.deltaSeconds;
    }

    void ModuleLoaderModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_GpuEnabled = context.enableAcceleration;
    }

    void ModuleLoaderModule::Reconfigure(const RuntimeConfig &config) {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        auto heapKilobytes = static_cast<std::uint32_t>(config.memory.heapBytes / 1024ULL);
        if (heapKilobytes > 0) {
            std::uint32_t suggested = std::min<std::uint32_t>(heapKilobytes * 2u, 1u << 20);
            m_DefaultStackSize = std::max(kMinimumStackSize, suggested);
        }
        (void) EnsureContextUnlocked(m_DefaultContextName, m_DefaultStackSize, lock);
    }

    void ModuleLoaderModule::SetHostResolver(ResolveCallback callback, void *userData) noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Resolver = callback;
        m_ResolverUserData = userData;
    }

    StatusCode ModuleLoaderModule::RegisterModule(std::string_view specifier,
                                                  std::string_view source,
                                                  Handle &outHandle,
                                                  const RegisterOptions &options) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (specifier.empty()) {
            outHandle = kInvalidHandle;
            return StatusCode::InvalidArgument;
        }

        m_ScratchDependencies.clear();
        if (options.overrideDependencies) {
            m_ScratchDependencies.reserve(options.dependencies.size());
            for (auto dep: options.dependencies) {
                m_ScratchDependencies.emplace_back(dep);
            }
        } else if (!options.dependencies.empty()) {
            m_ScratchDependencies.reserve(options.dependencies.size());
            for (auto dep: options.dependencies) {
                m_ScratchDependencies.emplace_back(dep);
            }
        } else {
            ExtractDependencies(source, m_ScratchDependencies);
        }

        auto status = EnsureModuleLocked(specifier, outHandle);
        if (status != StatusCode::Ok) {
            return status;
        }

        auto index = ExtractIndex(outHandle);
        if (index >= m_Slots.size()) {
            return StatusCode::InternalError;
        }
        auto &slot = m_Slots[index];
        if (!slot.inUse) {
            return StatusCode::InternalError;
        }

        auto prevHash = slot.record.sourceHash;
        auto applyStatus = UpdateModuleLocked(slot, source, options, m_ScratchDependencies);
        if (applyStatus != StatusCode::Ok) {
            return applyStatus;
        }

        if (slot.record.sourceHash != prevHash) {
            ++m_Metrics.updated;
        }
        return StatusCode::Ok;
    }

    StatusCode ModuleLoaderModule::RegisterModule(std::string_view specifier,
                                                  std::string_view source,
                                                  Handle &outHandle) {
        RegisterOptions defaults{};
        return RegisterModule(specifier, source, outHandle, defaults);
    }

    StatusCode ModuleLoaderModule::EnsureModule(std::string_view specifier, Handle &outHandle) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (specifier.empty()) {
            outHandle = kInvalidHandle;
            return StatusCode::InvalidArgument;
        }
        return EnsureModuleLocked(specifier, outHandle);
    }


    ModuleLoaderModule::EvaluationResult ModuleLoaderModule::Evaluate(Handle handle, bool forceReload) {
        EvaluationResult result{};
        std::unique_lock<std::mutex> lock(m_Mutex);
        if (handle == kInvalidHandle) {
            result.status = StatusCode::InvalidArgument;
            result.diagnostics = "Invalid module handle";
            return result;
        }

        auto index = ExtractIndex(handle);
        if (index >= m_Slots.size()) {
            result.status = StatusCode::NotFound;
            result.diagnostics = "Module handle out of range";
            return result;
        }

        auto *rootSlot = &m_Slots[index];
        if (!rootSlot->inUse) {
            result.status = StatusCode::NotFound;
            result.diagnostics = "Module slot unused";
            return result;
        }

        std::string diagnostics;
        auto status = BuildEvaluationOrder(handle, m_EvaluationOrder, diagnostics);
        if (status != StatusCode::Ok) {
            result.status = status;
            result.diagnostics = diagnostics;
            m_EvaluationOrder.clear();
            return result;
        }
        m_Metrics.graphBuilds += 1;

        result.status = rootSlot->record.lastStatus;
        result.value = rootSlot->record.lastValue;
        result.diagnostics = rootSlot->record.diagnostics;
        result.version = rootSlot->record.version;

        for (auto moduleHandle: m_EvaluationOrder) {
            auto *slot = ResolveSlot(moduleHandle);
            if (!slot) {
                continue;
            }

            auto &record = slot->record;
            bool isRoot = moduleHandle == handle;
            std::uint64_t dependencyStamp = MaxDependencyVersion(record);
            bool dependenciesChanged = dependencyStamp != record.dependencyStamp;
            bool needsEvaluation = (isRoot && forceReload) || record.dirty || dependenciesChanged
                                   || record.state != State::Evaluated
                                   || record.lastStatus != StatusCode::Ok;

            if (!needsEvaluation) {
                m_Metrics.cacheHits += 1;
                if (isRoot) {
                    result.status = record.lastStatus;
                    result.value = record.lastValue;
                    result.diagnostics = record.diagnostics;
                    result.version = record.version;
                }
                continue;
            }

            m_Metrics.cacheMisses += 1;
            if (!m_Runtime) {
                record.evaluating = false;
                record.state = State::Failed;
                record.lastStatus = StatusCode::InternalError;
                record.diagnostics = "Runtime unavailable";
                result.status = StatusCode::InternalError;
                result.diagnostics = record.diagnostics;
                m_EvaluationOrder.clear();
                return result;
            }

            record.evaluating = true;

            auto contextName = record.contextName.empty() ? m_DefaultContextName : record.contextName;
            auto stackSize = record.explicitStackSize != 0 ? record.explicitStackSize : m_DefaultStackSize;

            if (record.source.empty()) {
                if (!m_Resolver) {
                    record.evaluating = false;
                    record.state = State::Failed;
                    record.lastStatus = StatusCode::InvalidArgument;
                    record.diagnostics = "Module source unavailable";
                    result.status = record.lastStatus;
                    result.diagnostics = record.diagnostics;
                    m_EvaluationOrder.clear();
                    return result;
                }

                lock.unlock();
                std::string resolvedSource;
                std::vector<std::string> resolvedDependencies;
                m_Metrics.resolverRequests += 1;
                auto resolverStatus = m_Resolver(m_ResolverUserData,
                                                 record.specifier,
                                                 resolvedSource,
                                                 resolvedDependencies);
                lock.lock();

                slot = ResolveSlot(moduleHandle);
                if (!slot || !slot->inUse) {
                    continue;
                }

                auto &resolvedRecord = slot->record;
                if (resolverStatus != StatusCode::Ok) {
                    resolvedRecord.evaluating = false;
                    resolvedRecord.state = State::Failed;
                    resolvedRecord.lastStatus = resolverStatus;
                    resolvedRecord.diagnostics = "Resolver failed for module '" + resolvedRecord.specifier + "'";
                    result.status = resolverStatus;
                    result.diagnostics = resolvedRecord.diagnostics;
                    m_EvaluationOrder.clear();
                    return result;
                }

                m_ScratchDependencies.clear();
                m_ScratchDependencies.reserve(resolvedDependencies.size());
                for (const auto &dep: resolvedDependencies) {
                    m_ScratchDependencies.push_back(dep);
                }

                RegisterOptions resolverOptions;
                resolverOptions.contextName = contextName;
                resolverOptions.stackSize = stackSize;
                resolverOptions.overrideDependencies = true;
                auto updateStatus = UpdateModuleLocked(*slot,
                                                       resolvedSource,
                                                       resolverOptions,
                                                       m_ScratchDependencies);
                if (updateStatus != StatusCode::Ok) {
                    auto &failedRecord = slot->record;
                    failedRecord.evaluating = false;
                    failedRecord.state = State::Failed;
                    failedRecord.lastStatus = updateStatus;
                    failedRecord.diagnostics = "Failed to apply resolved module";
                    result.status = updateStatus;
                    result.diagnostics = failedRecord.diagnostics;
                    m_EvaluationOrder.clear();
                    return result;
                }

                m_Metrics.resolverHits += 1;
                dependencyStamp = MaxDependencyVersion(slot->record);
                contextName = slot->record.contextName.empty() ? m_DefaultContextName : slot->record.contextName;
                stackSize = slot->record.explicitStackSize != 0 ? slot->record.explicitStackSize : m_DefaultStackSize;
            }

            auto contextStatus = EnsureContextUnlocked(contextName, stackSize, lock);
            if (contextStatus != StatusCode::Ok) {
                slot = ResolveSlot(moduleHandle);
                if (slot && slot->inUse) {
                    slot->record.evaluating = false;
                    slot->record.state = State::Failed;
                    slot->record.lastStatus = contextStatus;
                    slot->record.diagnostics = "Failed to ensure module context";
                    result.status = contextStatus;
                    result.diagnostics = slot->record.diagnostics;
                }
                m_EvaluationOrder.clear();
                return result;
            }

            slot = ResolveSlot(moduleHandle);
            if (!slot || !slot->inUse) {
                continue;
            }

            spectre::ScriptSource script{};
            script.name = slot->record.specifier;
            script.source = slot->record.source;

            lock.unlock();
            auto loadResult = m_Runtime->LoadScript(contextName, script);
            lock.lock();

            slot = ResolveSlot(moduleHandle);
            if (!slot || !slot->inUse) {
                continue;
            }

            if (loadResult.status != StatusCode::Ok) {
                auto &loadRecord = slot->record;
                loadRecord.evaluating = false;
                loadRecord.state = State::Failed;
                loadRecord.lastStatus = loadResult.status;
                loadRecord.diagnostics = loadResult.diagnostics;
                result.status = loadResult.status;
                result.diagnostics = loadResult.diagnostics;
                m_EvaluationOrder.clear();
                return result;
            }

            lock.unlock();
            auto evalResult = m_Runtime->EvaluateSync(contextName, script.name);
            lock.lock();

            slot = ResolveSlot(moduleHandle);
            if (!slot || !slot->inUse) {
                continue;
            }

            auto &finalRecord = slot->record;
            finalRecord.lastStatus = evalResult.status;
            finalRecord.diagnostics = evalResult.diagnostics;
            finalRecord.lastValue = evalResult.value;
            finalRecord.evaluating = false;

            if (evalResult.status == StatusCode::Ok) {
                finalRecord.state = State::Evaluated;
                finalRecord.dirty = false;
                finalRecord.dependencyStamp = dependencyStamp;
                finalRecord.version += 1;
                m_Metrics.evaluations += 1;
                if (isRoot) {
                    result.status = finalRecord.lastStatus;
                    result.value = finalRecord.lastValue;
                    result.diagnostics = finalRecord.diagnostics;
                    result.version = finalRecord.version;
                }
            } else {
                finalRecord.state = State::Failed;
                finalRecord.dirty = true;
                m_Metrics.evaluationErrors += 1;
                result.status = evalResult.status;
                result.diagnostics = evalResult.diagnostics;
                result.value = evalResult.value;
                result.version = finalRecord.version;
                m_EvaluationOrder.clear();
                return result;
            }
        }

        m_EvaluationOrder.clear();
        rootSlot = ResolveSlot(handle);
        if (rootSlot && rootSlot->inUse && rootSlot->record.state == State::Evaluated) {
            result.status = rootSlot->record.lastStatus;
            result.value = rootSlot->record.lastValue;
            result.diagnostics = rootSlot->record.diagnostics;
            result.version = rootSlot->record.version;
        }
        return result;
    }

    ModuleLoaderModule::EvaluationResult ModuleLoaderModule::Evaluate(std::string_view specifier,
                                                                      bool forceReload) {
        Handle handle = kInvalidHandle;
        auto status = EnsureModule(specifier, handle);
        if (status != StatusCode::Ok) {
            EvaluationResult result{};
            result.status = status;
            result.diagnostics = "Failed to ensure module";
            return result;
        }
        return Evaluate(handle, forceReload);
    }

    StatusCode ModuleLoaderModule::Invalidate(Handle handle) noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (handle == kInvalidHandle) {
            return StatusCode::InvalidArgument;
        }
        auto index = ExtractIndex(handle);
        if (index >= m_Slots.size()) {
            return StatusCode::NotFound;
        }
        auto &slot = m_Slots[index];
        if (!slot.inUse) {
            return StatusCode::NotFound;
        }
        slot.record.dirty = true;
        MarkDependentsDirty(handle);
        return StatusCode::Ok;
    }

    bool ModuleLoaderModule::HasModule(std::string_view specifier) const noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_SpecifierLookup.find(specifier) != m_SpecifierLookup.end();
    }

    ModuleLoaderModule::ModuleInfo ModuleLoaderModule::Snapshot(Handle handle) const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        ModuleInfo info{};
        if (handle == kInvalidHandle) {
            return info;
        }
        auto index = ExtractIndex(handle);
        if (index >= m_Slots.size()) {
            return info;
        }
        const auto &slot = m_Slots[index];
        if (!slot.inUse) {
            return info;
        }
        const auto &record = slot.record;
        info.handle = record.handle;
        info.state = record.state;
        info.lastStatus = record.lastStatus;
        info.specifier = record.specifier;
        info.version = record.version;
        info.sourceHash = record.sourceHash;
        info.dependencyStamp = record.dependencyStamp;
        info.dirty = record.dirty;
        info.cached = record.state == State::Evaluated && record.lastStatus == StatusCode::Ok && !record.dirty;
        info.evaluating = record.evaluating;
        info.diagnostics = record.diagnostics;
        info.dependencies.reserve(record.dependencies.size());
        for (auto depHandle: record.dependencies) {
            auto depIndex = ExtractIndex(depHandle);
            if (depIndex < m_Slots.size()) {
                const auto &depSlot = m_Slots[depIndex];
                if (depSlot.inUse) {
                    info.dependencies.push_back(depSlot.record.specifier);
                }
            }
        }
        return info;
    }

    void ModuleLoaderModule::Clear(bool releaseCapacity) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (std::size_t i = 0; i < m_Slots.size(); ++i) {
            auto &slot = m_Slots[i];
            if (!slot.inUse) {
                continue;
            }
            DetachDependencies(slot.record);
            slot.record.Reset();
            slot.inUse = false;
            slot.generation = NextGeneration(slot.generation);
            m_FreeList.push_back(static_cast<std::uint32_t>(i));
        }
        m_SpecifierLookup.clear();
        m_ContextLookup.clear();
        m_Metrics = Metrics();
        if (releaseCapacity) {
            m_Slots.clear();
            m_FreeList.clear();
            m_DfsMarks.clear();
            m_DirtyMarks.clear();
            m_WorkStack.clear();
            m_EvaluationOrder.clear();
            m_ScratchDependencies.clear();
            m_DirtyStack.clear();
        }
    }

    const ModuleLoaderModule::Metrics &ModuleLoaderModule::GetMetrics() const noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Metrics;
    }

    ModuleLoaderModule::Handle ModuleLoaderModule::MakeHandle(std::size_t index, std::uint16_t generation) noexcept {
        return static_cast<Handle>((static_cast<std::uint32_t>(generation) << kHandleIndexBits)
                                   | static_cast<std::uint32_t>(index & kHandleIndexMask));
    }

    std::size_t ModuleLoaderModule::ExtractIndex(Handle handle) noexcept {
        return static_cast<std::size_t>(handle & kHandleIndexMask);
    }

    std::uint16_t ModuleLoaderModule::ExtractGeneration(Handle handle) noexcept {
        return static_cast<std::uint16_t>(handle >> kHandleIndexBits);
    }

    std::uint16_t ModuleLoaderModule::NextGeneration(std::uint16_t generation) noexcept {
        return generation == 0xffffu ? static_cast<std::uint16_t>(1u) : static_cast<std::uint16_t>(generation + 1u);
    }

    ModuleLoaderModule::Slot *ModuleLoaderModule::ResolveSlot(Handle handle) noexcept {
        if (handle == kInvalidHandle) {
            return nullptr;
        }
        auto index = ExtractIndex(handle);
        if (index >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[index];
        if (!slot.inUse) {
            return nullptr;
        }
        if (ExtractGeneration(handle) != slot.generation) {
            return nullptr;
        }
        return &slot;
    }

    const ModuleLoaderModule::Slot *ModuleLoaderModule::ResolveSlot(Handle handle) const noexcept {
        if (handle == kInvalidHandle) {
            return nullptr;
        }
        auto index = ExtractIndex(handle);
        if (index >= m_Slots.size()) {
            return nullptr;
        }
        const auto &slot = m_Slots[index];
        if (!slot.inUse) {
            return nullptr;
        }
        if (ExtractGeneration(handle) != slot.generation) {
            return nullptr;
        }
        return &slot;
    }

    StatusCode ModuleLoaderModule::EnsureModuleLocked(std::string_view specifier, Handle &outHandle) {
        auto lookupIt = m_SpecifierLookup.find(specifier);
        if (lookupIt != m_SpecifierLookup.end()) {
            outHandle = lookupIt->second;
            return StatusCode::Ok;
        }

        auto index = AcquireSlot();
        if (index >= m_Slots.size()) {
            return StatusCode::InternalError;
        }
        auto &slot = m_Slots[index];
        slot.inUse = true;
        slot.record.Reset();
        slot.record.handle = MakeHandle(index, slot.generation);
        slot.record.specifier.assign(specifier.begin(), specifier.end());
        slot.record.state = State::Unresolved;
        slot.record.dirty = true;
        m_SpecifierLookup.emplace(slot.record.specifier, slot.record.handle);
        outHandle = slot.record.handle;
        m_Metrics.registered += 1;
        return StatusCode::Ok;
    }

    StatusCode ModuleLoaderModule::UpdateModuleLocked(Slot &slot,
                                                      std::string_view source,
                                                      const RegisterOptions &options,
                                                      const std::vector<std::string> &dependencies) {
        auto &record = slot.record;
        auto newHash = HashString(source);
        bool contentChanged = record.sourceHash != newHash || record.source.size() != source.size()
                              || std::memcmp(record.source.data(), source.data(), source.size()) != 0;
        if (contentChanged) {
            record.source.assign(source.begin(), source.end());
            record.sourceHash = newHash;
            record.dirty = true;
            record.state = State::Registered;
        }

        std::vector<Handle> dependencyHandles;
        dependencyHandles.reserve(dependencies.size());
        for (const auto &depName: dependencies) {
            Handle depHandle = kInvalidHandle;
            auto status = EnsureModuleLocked(depName, depHandle);
            if (status != StatusCode::Ok) {
                return status;
            }
            dependencyHandles.push_back(depHandle);
        }

        if (!record.dependencies.empty()) {
            if (m_Metrics.dependencyEdges >= record.dependencies.size()) {
                m_Metrics.dependencyEdges -= record.dependencies.size();
            } else {
                m_Metrics.dependencyEdges = 0;
            }
        }
        DetachDependencies(record);
        AttachDependencies(record, dependencyHandles);

        if (!dependencyHandles.empty()) {
            m_Metrics.dependencyEdges += dependencyHandles.size();
        }

        if (!options.contextName.empty()) {
            record.contextName.assign(options.contextName.begin(), options.contextName.end());
        } else if (record.contextName.empty()) {
            record.contextName = m_DefaultContextName;
        }
        record.explicitStackSize = options.stackSize;
        record.lastStatus = StatusCode::Ok;
        record.diagnostics.clear();
        MarkDependentsDirty(record.handle);
        return StatusCode::Ok;
    }


    StatusCode ModuleLoaderModule::EnsureContextUnlocked(std::string_view contextName,
                                                         std::uint32_t stackSize,
                                                         std::unique_lock<std::mutex> &lock) {
        if (!m_Runtime) {
            return StatusCode::InternalError;
        }

        std::string key(contextName);
        if (m_ContextLookup.find(key) != m_ContextLookup.end()) {
            return StatusCode::Ok;
        }

        auto initialStack = stackSize != 0 ? stackSize : m_DefaultStackSize;

        lock.unlock();
        spectre::ContextConfig config{};
        config.name = key;
        config.initialStackSize = initialStack;
        auto status = m_Runtime->CreateContext(config, nullptr);
        lock.lock();

        if (status == StatusCode::AlreadyExists) {
            status = StatusCode::Ok;
        }
        if (status == StatusCode::Ok && m_ContextLookup.find(key) == m_ContextLookup.end()) {
            m_ContextLookup.emplace(std::move(key), initialStack);
        }
        return status;
    }

    void ModuleLoaderModule::DetachDependencies(ModuleRecord &record) {
        for (auto handle: record.dependencies) {
            auto *slot = ResolveSlot(handle);
            if (!slot) {
                continue;
            }
            auto &dependents = slot->record.dependents;
            for (std::size_t i = 0; i < dependents.size(); ++i) {
                if (dependents[i] == record.handle) {
                    dependents[i] = dependents.back();
                    dependents.pop_back();
                    break;
                }
            }
        }
        record.dependencies.clear();
    }

    void ModuleLoaderModule::AttachDependencies(ModuleRecord &record, const std::vector<Handle> &handles) {
        record.dependencies = handles;
        for (auto handle: handles) {
            auto *slot = ResolveSlot(handle);
            if (!slot) {
                continue;
            }
            auto &dependents = slot->record.dependents;
            if (std::find(dependents.begin(), dependents.end(), record.handle) == dependents.end()) {
                dependents.push_back(record.handle);
            }
        }
    }

    void ModuleLoaderModule::MarkDependentsDirty(Handle handle) {
        if (m_DirtyMarks.size() < m_Slots.size()) {
            m_DirtyMarks.resize(m_Slots.size(), 0);
        }
        m_DirtyStack.clear();
        m_DirtyStack.push_back(handle);
        while (!m_DirtyStack.empty()) {
            auto current = m_DirtyStack.back();
            m_DirtyStack.pop_back();
            auto index = ExtractIndex(current);
            if (index >= m_Slots.size()) {
                continue;
            }
            auto &slot = m_Slots[index];
            if (!slot.inUse) {
                continue;
            }
            for (auto dependent: slot.record.dependents) {
                auto depIndex = ExtractIndex(dependent);
                if (depIndex >= m_Slots.size()) {
                    continue;
                }
                auto &depSlot = m_Slots[depIndex];
                if (!depSlot.inUse) {
                    continue;
                }
                if (!depSlot.record.dirty) {
                    depSlot.record.dirty = true;
                }
                if (m_DirtyMarks[depIndex] == 0) {
                    m_DirtyMarks[depIndex] = 1;
                    m_DirtyStack.push_back(dependent);
                }
            }
        }
        std::fill(m_DirtyMarks.begin(), m_DirtyMarks.end(), 0);
    }

    std::uint64_t ModuleLoaderModule::MaxDependencyVersion(const ModuleRecord &record) const noexcept {
        std::uint64_t version = 0;
        for (auto handle: record.dependencies) {
            const auto *slot = ResolveSlot(handle);
            if (!slot) {
                continue;
            }
            version = std::max(version, slot->record.version);
        }
        return version;
    }

    StatusCode ModuleLoaderModule::BuildEvaluationOrder(Handle root,
                                                        std::vector<Handle> &outOrder,
                                                        std::string &outDiagnostics) {
        outOrder.clear();
        outDiagnostics.clear();
        auto rootIndex = ExtractIndex(root);
        if (rootIndex >= m_Slots.size()) {
            return StatusCode::NotFound;
        }
        if (m_DfsMarks.size() < m_Slots.size()) {
            m_DfsMarks.resize(m_Slots.size(), 0);
        } else {
            std::fill(m_DfsMarks.begin(), m_DfsMarks.end(), 0);
        }

        m_WorkStack.clear();
        m_WorkStack.emplace_back(root, 0);
        std::size_t peakDepth = m_WorkStack.size();
        while (!m_WorkStack.empty()) {
            auto &frame = m_WorkStack.back();
            auto index = ExtractIndex(frame.handle);
            if (index >= m_Slots.size()) {
                m_WorkStack.pop_back();
                continue;
            }
            auto &slot = m_Slots[index];
            if (!slot.inUse) {
                m_WorkStack.pop_back();
                continue;
            }
            if (m_DfsMarks[index] == 0) {
                m_DfsMarks[index] = 1;
            }
            if (frame.nextDependency < slot.record.dependencies.size()) {
                auto depHandle = slot.record.dependencies[frame.nextDependency++];
                auto depIndex = ExtractIndex(depHandle);
                if (depIndex >= m_Slots.size()) {
                    continue;
                }
                if (m_DfsMarks[depIndex] == 1) {
                    m_Metrics.cyclesDetected += 1;
                    outDiagnostics = "Circular dependency detected";
                    return StatusCode::InvalidArgument;
                }
                if (m_DfsMarks[depIndex] == 0) {
                    m_WorkStack.emplace_back(depHandle, 0);
                    if (m_WorkStack.size() > peakDepth) {
                        peakDepth = m_WorkStack.size();
                    }
                }
                continue;
            }
            m_WorkStack.pop_back();
            if (m_DfsMarks[index] != 2) {
                m_DfsMarks[index] = 2;
                outOrder.push_back(slot.record.handle);
            }
        }

        for (auto handleValue: outOrder) {
            auto idx = ExtractIndex(handleValue);
            if (idx < m_DfsMarks.size()) {
                m_DfsMarks[idx] = 0;
            }
        }
        if (peakDepth > m_Metrics.maxGraphDepth) {
            m_Metrics.maxGraphDepth = peakDepth;
        }
        return StatusCode::Ok;
    }

    void ModuleLoaderModule::ExtractDependencies(std::string_view source,
                                                 std::vector<std::string> &outDependencies) const {
        outDependencies.clear();
        bool inSingleLine = false;
        bool inMultiLine = false;
        char stringQuote = '\0';
        bool escaped = false;
        const char *data = source.data();
        const auto size = source.size();
        for (std::size_t i = 0; i < size; ++i) {
            char ch = data[i];
            if (inSingleLine) {
                if (ch == '\n') {
                    inSingleLine = false;
                }
                continue;
            }
            if (inMultiLine) {
                if (ch == '*' && i + 1 < size && data[i + 1] == '/') {
                    inMultiLine = false;
                    ++i;
                }
                continue;
            }
            if (stringQuote != '\0') {
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (ch == '\\') {
                    escaped = true;
                    continue;
                }
                if (ch == stringQuote) {
                    stringQuote = '\0';
                }
                continue;
            }
            if (ch == '/') {
                if (i + 1 < size) {
                    if (data[i + 1] == '/') {
                        inSingleLine = true;
                        ++i;
                        continue;
                    }
                    if (data[i + 1] == '*') {
                        inMultiLine = true;
                        ++i;
                        continue;
                    }
                }
            }
            if (ch == '\'' || ch == '"' || ch == kTemplateQuote) {
                stringQuote = ch;
                continue;
            }

            auto matchKeyword = [&](std::string_view keyword) -> bool {
                if (i + keyword.size() > size) {
                    return false;
                }
                if (i > 0 && IsIdentifierChar(data[i - 1])) {
                    return false;
                }
                if (!std::equal(keyword.begin(), keyword.end(), data + i)) {
                    return false;
                }
                if (i + keyword.size() < size && IsIdentifierChar(data[i + keyword.size()])) {
                    return false;
                }
                return true;
            };

            auto parseString = [&](std::size_t start) -> std::size_t {
                if (start >= size) {
                    return size;
                }
                char quote = data[start];
                if (quote != '\'' && quote != '"') {
                    return size;
                }
                std::size_t pos = start + 1;
                bool escape = false;
                while (pos < size) {
                    char current = data[pos];
                    if (escape) {
                        escape = false;
                        ++pos;
                        continue;
                    }
                    if (current == '\\') {
                        escape = true;
                        ++pos;
                        continue;
                    }
                    if (current == quote) {
                        auto literal = source.substr(start + 1, pos - (start + 1));
                        if (std::find(outDependencies.begin(), outDependencies.end(), literal) == outDependencies.
                            end()) {
                            outDependencies.emplace_back(literal);
                        }
                        return pos + 1;
                    }
                    ++pos;
                }
                return size;
            };

            if (matchKeyword("import")) {
                std::size_t pos = SkipWhitespace(source, i + 6);
                if (pos < size && data[pos] == '(') {
                    pos = SkipWhitespace(source, pos + 1);
                    pos = parseString(pos);
                    i = pos;
                    continue;
                }
                if (pos < size && (data[pos] == '\'' || data[pos] == '"')) {
                    pos = parseString(pos);
                    i = pos;
                    continue;
                }
                bool foundFrom = false;
                while (pos < size) {
                    if (data[pos] == '\'' || data[pos] == '"') {
                        break;
                    }
                    if (matchKeyword("from")) {
                        foundFrom = true;
                        pos = SkipWhitespace(source, pos + 4);
                        break;
                    }
                    if (data[pos] == ';' || data[pos] == '\n') {
                        break;
                    }
                    ++pos;
                }
                if (foundFrom) {
                    pos = parseString(pos);
                    i = pos;
                    continue;
                }
            } else if (matchKeyword("export")) {
                std::size_t pos = SkipWhitespace(source, i + 6);
                bool foundFrom = false;
                while (pos < size) {
                    if (data[pos] == '\'' || data[pos] == '"') {
                        break;
                    }
                    if (matchKeyword("from")) {
                        foundFrom = true;
                        pos = SkipWhitespace(source, pos + 4);
                        break;
                    }
                    if (data[pos] == ';' || data[pos] == '\n') {
                        break;
                    }
                    ++pos;
                }
                if (foundFrom) {
                    pos = parseString(pos);
                    i = pos;
                    continue;
                }
            }
        }
    }

    std::size_t ModuleLoaderModule::AcquireSlot() {
        if (!m_FreeList.empty()) {
            auto index = m_FreeList.back();
            m_FreeList.pop_back();
            return index;
        }
        m_Slots.emplace_back();
        return m_Slots.size() - 1;
    }

    void ModuleLoaderModule::ReleaseSlot(std::size_t index) noexcept {
        if (index >= m_Slots.size()) {
            return;
        }
        auto &slot = m_Slots[index];
        slot.inUse = false;
        slot.generation = NextGeneration(slot.generation);
        m_FreeList.push_back(static_cast<std::uint32_t>(index));
    }

    void ModuleLoaderModule::ResetMetrics() noexcept {
        m_Metrics = Metrics();
    }
}
