#include "spectre/es2025/modules/shadow_realm_module.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include "spectre/context.h"
#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "ShadowRealm";
        constexpr std::string_view kSummary =
                "ShadowRealm isolation, context pooling, and value bridges across host boundaries.";
        constexpr std::string_view kReference = "ECMA-262 Annex B + ShadowRealm Stage 3";
        constexpr std::uint32_t kMinimumStackSize = 1u << 15; // 32 KiB
        constexpr std::uint32_t kMaximumStackSize = 1u << 21; // 2 MiB
        constexpr std::size_t kDefaultRealmCapacity = 8;
        constexpr std::size_t kShadowRealmSlotLimit = (1u << 16) - 1;

        std::uint32_t RecommendStackSize(const RuntimeConfig &config) noexcept {
            auto heap = config.memory.heapBytes;
            if (heap == 0) {
                return std::max<std::uint32_t>(kMinimumStackSize, 1u << 16);
            }
            auto suggested = static_cast<std::uint64_t>(heap / 2048ULL);
            if (suggested < kMinimumStackSize) {
                suggested = kMinimumStackSize;
            }
            if (suggested > kMaximumStackSize) {
                suggested = kMaximumStackSize;
            }
            return static_cast<std::uint32_t>(suggested);
        }

        std::size_t RecommendRealmCapacity(const RuntimeConfig &config) noexcept {
            auto heap = config.memory.heapBytes;
            if (heap == 0) {
                return kDefaultRealmCapacity;
            }
            auto capacity = heap / 262144ULL; // ~256 KiB per isolated realm
            if (capacity < kDefaultRealmCapacity) {
                capacity = kDefaultRealmCapacity;
            }
            if (capacity > kShadowRealmSlotLimit) {
                capacity = kShadowRealmSlotLimit;
            }
            return static_cast<std::size_t>(capacity);
        }

        std::string ComposeContextName(std::uint32_t slot, std::uint16_t generation) {
            char buffer[48];
            std::snprintf(buffer, sizeof(buffer), "shadow.realm.%04u.%04u", slot,
                          static_cast<unsigned>(generation));
            return std::string(buffer);
        }

        std::string ComposeInlineScriptName(const std::string &contextName) {
            std::string name;
            name.reserve(contextName.size() + 8);
            name.append(contextName);
            name.append(".eval");
            return name;
        }
    }

    ShadowRealmModule::Metrics::Metrics() noexcept
        : created(0),
          destroyed(0),
          evaluations(0),
          exports(0),
          imports(0),
          failedImports(0),
          contextAllocs(0),
          contextFailures(0),
          reuseHits(0),
          reuseMisses(0),
          lastFrameTouched(0),
          activeRealms(0),
          peakRealms(0),
          gpuOptimized(false) {
    }

    ShadowRealmModule::ShadowRealmModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_TotalSeconds(0.0),
          m_Slots(),
          m_FreeSlots(),
          m_Metrics(),
          m_DefaultStackSize(kMinimumStackSize) {
    }

    std::string_view ShadowRealmModule::Name() const noexcept {
        return kName;
    }

    std::string_view ShadowRealmModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view ShadowRealmModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void ShadowRealmModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_CurrentFrame = 0;
        m_TotalSeconds = 0.0;
        m_DefaultStackSize = RecommendStackSize(context.config);
        ResetAllocationPools(RecommendRealmCapacity(context.config));
        m_Initialized = true;
    }

    void ShadowRealmModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
        m_TotalSeconds += info.deltaSeconds;
    }

    void ShadowRealmModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
        (void) context;
    }

    void ShadowRealmModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_DefaultStackSize = RecommendStackSize(config);
        EnsureCapacity(RecommendRealmCapacity(config));
    }

    StatusCode ShadowRealmModule::Create(std::string_view label, Handle &outHandle, std::uint32_t stackSize) {
        outHandle = kInvalidHandle;
        if (!m_Runtime) {
            return StatusCode::InternalError;
        }
        if (m_FreeSlots.empty()) {
            return StatusCode::CapacityExceeded;
        }

        auto slotIndex = m_FreeSlots.back();
        m_FreeSlots.pop_back();
        auto &slot = m_Slots[slotIndex];

        if (slot.inUse) {
            return StatusCode::InternalError;
        }

        if (slot.generation > 1) {
            m_Metrics.reuseHits += 1;
        } else {
            m_Metrics.reuseMisses += 1;
        }

        slot.inUse = true;
        slot.record.handle = MakeHandle(slotIndex, slot.generation);
        slot.record.slot = slotIndex;
        slot.record.generation = slot.generation;
        slot.record.stackSize = stackSize != 0 ? stackSize : m_DefaultStackSize;
        slot.record.createdFrame = m_CurrentFrame;
        slot.record.createdSeconds = m_TotalSeconds;
        slot.record.evalCount = 0;
        slot.record.importCount = 0;
        slot.record.exportCount = 0;
        slot.record.pinned = false;
        slot.record.active = true;
        slot.record.contextName = ComposeContextName(slotIndex, slot.generation);
        slot.record.inlineScriptName = ComposeInlineScriptName(slot.record.contextName);
        CopyString(label, slot.record.label, slot.record.labelLength);
        for (auto &entry: slot.record.exports) {
            entry.inUse = false;
            entry.length = 0;
            entry.name[0] = '\0';
            entry.value = Value::Undefined();
        }

        ContextConfig ctxConfig{};
        ctxConfig.name = slot.record.contextName;
        ctxConfig.initialStackSize = slot.record.stackSize;

        auto status = m_Runtime->CreateContext(ctxConfig, nullptr);
        if (status == StatusCode::AlreadyExists) {
            status = StatusCode::Ok;
        }
        if (status != StatusCode::Ok) {
            m_Metrics.contextFailures += 1;
            slot.inUse = false;
            slot.record.active = false;
            m_FreeSlots.push_back(slotIndex);
            return status;
        }

        m_Metrics.contextAllocs += 1;
        m_Metrics.created += 1;
        m_Metrics.activeRealms += 1;
        if (m_Metrics.activeRealms > m_Metrics.peakRealms) {
            m_Metrics.peakRealms = m_Metrics.activeRealms;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        outHandle = slot.record.handle;
        return StatusCode::Ok;
    }

    StatusCode ShadowRealmModule::Destroy(Handle handle) {
        auto *realm = Resolve(handle);
        if (!realm) {
            return StatusCode::NotFound;
        }
        if (m_Runtime) {
            (void) m_Runtime->DestroyContext(realm->contextName);
        }
        m_Metrics.destroyed += 1;
        if (m_Metrics.activeRealms > 0) {
            m_Metrics.activeRealms -= 1;
        }
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        ReleaseSlot(realm->slot);
        return StatusCode::Ok;
    }

    StatusCode ShadowRealmModule::Evaluate(Handle handle,
                                           std::string_view source,
                                           std::string &outValue,
                                           std::string &outDiagnostics,
                                           std::string_view scriptName) noexcept {
        outValue.clear();
        outDiagnostics.clear();
        auto *realm = Resolve(handle);
        if (!realm) {
            return StatusCode::NotFound;
        }
        if (!m_Runtime) {
            outDiagnostics = "Runtime unavailable";
            return StatusCode::InternalError;
        }

        ScriptSource script{};
        if (scriptName.empty()) {
            script.name = realm->inlineScriptName;
        } else {
            script.name = std::string(scriptName);
        }
        script.source = std::string(source);

        auto loadResult = m_Runtime->LoadScript(realm->contextName, script);
        if (loadResult.status != StatusCode::Ok) {
            outDiagnostics = loadResult.diagnostics;
            return loadResult.status;
        }

        auto evalResult = m_Runtime->EvaluateSync(realm->contextName, script.name);
        outValue = std::move(evalResult.value);
        outDiagnostics = std::move(evalResult.diagnostics);
        if (evalResult.status == StatusCode::Ok) {
            realm->evalCount += 1;
            m_Metrics.evaluations += 1;
            m_Metrics.lastFrameTouched = m_CurrentFrame;
        }
        return evalResult.status;
    }

    StatusCode ShadowRealmModule::ExportValue(Handle handle,
                                              std::string_view exportName,
                                              const Value &value) noexcept {
        if (exportName.empty()) {
            return StatusCode::InvalidArgument;
        }
        auto *realm = Resolve(handle);
        if (!realm) {
            return StatusCode::NotFound;
        }

        auto *entry = FindExport(*realm, exportName);
        if (!entry) {
            for (auto &candidate: realm->exports) {
                if (!candidate.inUse) {
                    entry = &candidate;
                    break;
                }
            }
        }
        if (!entry) {
            return StatusCode::CapacityExceeded;
        }

        CopyExportName(exportName, entry->name, entry->length);
        entry->value = value;
        entry->inUse = true;
        realm->exportCount += 1;
        m_Metrics.exports += 1;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode ShadowRealmModule::ImportValue(Handle targetRealm,
                                              Handle sourceRealm,
                                              std::string_view exportName,
                                              Value &outValue) noexcept {
        outValue = Value::Undefined();
        if (exportName.empty()) {
            return StatusCode::InvalidArgument;
        }
        auto *mutableThis = const_cast<ShadowRealmModule *>(this);
        auto *target = mutableThis->Resolve(targetRealm);
        auto *source = mutableThis->Resolve(sourceRealm);
        if (!target || !source) {
            return StatusCode::NotFound;
        }
        auto *entry = mutableThis->FindExport(*source, exportName);
        if (!entry) {
            m_Metrics.failedImports += 1;
            return StatusCode::NotFound;
        }
        outValue = entry->value;
        target->importCount += 1;
        m_Metrics.imports += 1;
        m_Metrics.lastFrameTouched = m_CurrentFrame;
        return StatusCode::Ok;
    }

    StatusCode ShadowRealmModule::ClearExports(Handle handle) noexcept {
        auto *realm = Resolve(handle);
        if (!realm) {
            return StatusCode::NotFound;
        }
        for (auto &entry: realm->exports) {
            entry.inUse = false;
            entry.length = 0;
            entry.name[0] = '\0';
            entry.value = Value::Undefined();
        }
        return StatusCode::Ok;
    }

    StatusCode ShadowRealmModule::Describe(Handle handle,
                                           std::string &outLabel,
                                           std::string &outContextName,
                                           std::uint64_t &outEvaluations) const {
        auto *realm = const_cast<ShadowRealmModule *>(this)->Resolve(handle);
        if (!realm) {
            outLabel.clear();
            outContextName.clear();
            outEvaluations = 0;
            return StatusCode::NotFound;
        }
        outLabel.assign(realm->label.data(), realm->labelLength);
        outContextName = realm->contextName;
        outEvaluations = realm->evalCount;
        return StatusCode::Ok;
    }

    bool ShadowRealmModule::Has(Handle handle) const noexcept {
        return const_cast<ShadowRealmModule *>(this)->Resolve(handle) != nullptr;
    }

    std::size_t ShadowRealmModule::ActiveRealms() const noexcept {
        return m_Metrics.activeRealms;
    }

    const ShadowRealmModule::Metrics &ShadowRealmModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool ShadowRealmModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    ShadowRealmModule::Handle ShadowRealmModule::MakeHandle(std::uint32_t slot,
                                                            std::uint16_t generation) noexcept {
        auto encoded = static_cast<std::uint32_t>(generation) << kHandleIndexBits;
        encoded |= (slot & kHandleIndexMask);
        if (encoded == 0) {
            encoded = 1;
        }
        return static_cast<Handle>(encoded);
    }

    std::uint32_t ShadowRealmModule::ExtractSlot(Handle handle) noexcept {
        return static_cast<std::uint32_t>(handle & kHandleIndexMask);
    }

    std::uint16_t ShadowRealmModule::ExtractGeneration(Handle handle) noexcept {
        return static_cast<std::uint16_t>((handle >> kHandleIndexBits) & kHandleIndexMask);
    }

    void ShadowRealmModule::ResetAllocationPools(std::size_t targetCapacity) {
        if (targetCapacity == 0) {
            targetCapacity = kDefaultRealmCapacity;
        }
        if (targetCapacity > kMaxSlots) {
            targetCapacity = kMaxSlots;
        }
        m_Slots.clear();
        m_FreeSlots.clear();
        m_Slots.resize(targetCapacity);
        m_FreeSlots.reserve(targetCapacity);
        for (std::size_t i = 0; i < targetCapacity; ++i) {
            auto slotIndex = static_cast<std::uint32_t>(targetCapacity - 1 - i);
            auto &slot = m_Slots[slotIndex];
            slot.generation = 1;
            slot.inUse = false;
            slot.record.handle = kInvalidHandle;
            slot.record.slot = slotIndex;
            slot.record.generation = slot.generation;
            slot.record.stackSize = m_DefaultStackSize;
            slot.record.createdFrame = 0;
            slot.record.createdSeconds = 0.0;
            slot.record.evalCount = 0;
            slot.record.importCount = 0;
            slot.record.exportCount = 0;
            slot.record.pinned = false;
            slot.record.active = false;
            slot.record.labelLength = 0;
            slot.record.label[0] = '\0';
            slot.record.contextName.clear();
            slot.record.inlineScriptName.clear();
            for (auto &entry: slot.record.exports) {
                entry.inUse = false;
                entry.length = 0;
                entry.name[0] = '\0';
                entry.value = Value::Undefined();
            }
            m_FreeSlots.push_back(slotIndex);
        }
        m_Metrics.activeRealms = 0;
        m_Metrics.peakRealms = 0;
    }

    void ShadowRealmModule::EnsureCapacity(std::size_t desiredCapacity) {
        if (desiredCapacity <= m_Slots.size()) {
            return;
        }
        if (desiredCapacity > kMaxSlots) {
            desiredCapacity = kMaxSlots;
        }
        auto currentSize = m_Slots.size();
        m_Slots.resize(desiredCapacity);
        m_FreeSlots.reserve(desiredCapacity);
        for (std::size_t index = currentSize; index < desiredCapacity; ++index) {
            auto slotIndex = static_cast<std::uint32_t>(desiredCapacity - 1 - (index - currentSize));
            auto &slot = m_Slots[slotIndex];
            slot.generation = 1;
            slot.inUse = false;
            slot.record.handle = kInvalidHandle;
            slot.record.slot = slotIndex;
            slot.record.generation = slot.generation;
            slot.record.stackSize = m_DefaultStackSize;
            slot.record.createdFrame = 0;
            slot.record.createdSeconds = 0.0;
            slot.record.evalCount = 0;
            slot.record.importCount = 0;
            slot.record.exportCount = 0;
            slot.record.pinned = false;
            slot.record.active = false;
            slot.record.labelLength = 0;
            slot.record.label[0] = '\0';
            slot.record.contextName.clear();
            slot.record.inlineScriptName.clear();
            for (auto &entry: slot.record.exports) {
                entry.inUse = false;
                entry.length = 0;
                entry.name[0] = '\0';
                entry.value = Value::Undefined();
            }
            m_FreeSlots.push_back(slotIndex);
        }
    }

    void ShadowRealmModule::ReleaseSlot(std::uint32_t slotIndex) noexcept {
        auto &slot = m_Slots[slotIndex];
        slot.inUse = false;
        slot.generation = static_cast<std::uint16_t>(slot.generation + 1);
        if (slot.generation == 0) {
            slot.generation = 1;
        }
        slot.record.handle = kInvalidHandle;
        slot.record.generation = slot.generation;
        slot.record.active = false;
        slot.record.labelLength = 0;
        slot.record.label[0] = '\0';
        slot.record.contextName.clear();
        slot.record.inlineScriptName.clear();
        slot.record.evalCount = 0;
        slot.record.importCount = 0;
        slot.record.exportCount = 0;
        for (auto &entry: slot.record.exports) {
            entry.inUse = false;
            entry.length = 0;
            entry.name[0] = '\0';
            entry.value = Value::Undefined();
        }
        m_FreeSlots.push_back(slotIndex);
    }

    ShadowRealmModule::RealmRecord *ShadowRealmModule::Resolve(Handle handle) noexcept {
        if (handle == kInvalidHandle) {
            return nullptr;
        }
        auto slotIndex = ExtractSlot(handle);
        if (slotIndex >= m_Slots.size()) {
            return nullptr;
        }
        auto &slot = m_Slots[slotIndex];
        if (!slot.inUse) {
            return nullptr;
        }
        if (slot.generation != ExtractGeneration(handle)) {
            return nullptr;
        }
        return &slot.record;
    }

    const ShadowRealmModule::RealmRecord *ShadowRealmModule::Resolve(Handle handle) const noexcept {
        return const_cast<ShadowRealmModule *>(this)->Resolve(handle);
    }

    void ShadowRealmModule::CopyString(std::string_view text,
                                       std::array<char, kMaxLabelLength + 1> &dest,
                                       std::uint8_t &outLength) noexcept {
        const auto count = static_cast<std::size_t>(std::min<std::size_t>(text.size(), kMaxLabelLength));
        if (count > 0) {
            std::memcpy(dest.data(), text.data(), count);
        }
        dest[count] = '\0';
        outLength = static_cast<std::uint8_t>(count);
    }

    void ShadowRealmModule::CopyExportName(std::string_view text,
                                           std::array<char, kMaxExportNameLength + 1> &dest,
                                           std::uint8_t &outLength) noexcept {
        const auto count = static_cast<std::size_t>(std::min<std::size_t>(text.size(), kMaxExportNameLength));
        if (count > 0) {
            std::memcpy(dest.data(), text.data(), count);
        }
        dest[count] = '\0';
        outLength = static_cast<std::uint8_t>(count);
    }

    ShadowRealmModule::ExportEntry *ShadowRealmModule::FindExport(RealmRecord &realm, std::string_view exportName) noexcept {
        for (auto &entry: realm.exports) {
            if (!entry.inUse) {
                continue;
            }
            if (entry.length != exportName.size()) {
                continue;
            }
            if (std::memcmp(entry.name.data(), exportName.data(), entry.length) == 0) {
                return &entry;
            }
        }
        return nullptr;
    }

    const ShadowRealmModule::ExportEntry *ShadowRealmModule::FindExport(const RealmRecord &realm,
                                                                        std::string_view exportName) const noexcept {
        return const_cast<ShadowRealmModule *>(this)->FindExport(const_cast<RealmRecord &>(realm), exportName);
    }
}



