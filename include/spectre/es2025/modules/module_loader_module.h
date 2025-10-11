#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "spectre/config.h"
#include "spectre/es2025/module.h"
#include "spectre/status.h"

namespace spectre::es2025 {
    class ModuleLoaderModule final : public Module {
    public:
        using Handle = std::uint32_t;
        static constexpr Handle kInvalidHandle = 0;

        enum class State : std::uint8_t {
            Unresolved,
            Registered,
            Compiled,
            Evaluated,
            Failed
        };

        using ResolveCallback = StatusCode (*)(void *userData,
                                               std::string_view specifier,
                                               std::string &outSource,
                                               std::vector<std::string> &outDependencies);

        struct RegisterOptions {
            std::string_view contextName{};
            std::uint32_t stackSize = 0;
            std::vector<std::string_view> dependencies{};
            bool overrideDependencies = false;
        };

        struct ModuleInfo {
            Handle handle = kInvalidHandle;
            State state = State::Unresolved;
            StatusCode lastStatus = StatusCode::NotFound;
            std::string specifier;
            std::vector<std::string> dependencies;
            std::uint64_t version = 0;
            std::uint64_t sourceHash = 0;
            std::uint64_t dependencyStamp = 0;
            bool dirty = true;
            bool cached = false;
            bool evaluating = false;
            std::string diagnostics;
        };

        struct EvaluationResult {
            StatusCode status = StatusCode::Ok;
            std::string value;
            std::string diagnostics;
            std::uint64_t version = 0;
        };

        struct Metrics {
            std::uint64_t registered = 0;
            std::uint64_t updated = 0;
            std::uint64_t evaluations = 0;
            std::uint64_t cacheHits = 0;
            std::uint64_t cacheMisses = 0;
            std::uint64_t resolverRequests = 0;
            std::uint64_t resolverHits = 0;
            std::uint64_t graphBuilds = 0;
            std::uint64_t cyclesDetected = 0;
            std::uint64_t evaluationErrors = 0;
            std::uint64_t dependencyEdges = 0;
            std::size_t maxGraphDepth = 0;
        };

        ModuleLoaderModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        void SetHostResolver(ResolveCallback callback, void *userData) noexcept;

        StatusCode RegisterModule(std::string_view specifier,
                                  std::string_view source,
                                  Handle &outHandle,
                                  const RegisterOptions &options);

        StatusCode RegisterModule(std::string_view specifier,
                                  std::string_view source,
                                  Handle &outHandle);

        StatusCode EnsureModule(std::string_view specifier, Handle &outHandle);

        EvaluationResult Evaluate(Handle handle, bool forceReload = false);
        EvaluationResult Evaluate(std::string_view specifier, bool forceReload = false);

        StatusCode Invalidate(Handle handle) noexcept;

        bool HasModule(std::string_view specifier) const noexcept;

        ModuleInfo Snapshot(Handle handle) const;

        void Clear(bool releaseCapacity = false);

        const Metrics &GetMetrics() const noexcept;

    private:
        struct ModuleRecord {
            Handle handle;
            std::string specifier;
            std::string source;
            std::string contextName;
            std::uint32_t explicitStackSize;
            std::vector<Handle> dependencies;
            std::vector<Handle> dependents;
            std::uint64_t sourceHash;
            std::uint64_t dependencyStamp;
            std::uint64_t version;
            State state;
            StatusCode lastStatus;
            std::string diagnostics;
            std::string lastValue;
            bool dirty;
            bool evaluating;

            ModuleRecord() noexcept;
            void Reset() noexcept;
        };

        struct Slot {
            ModuleRecord record;
            std::uint16_t generation;
            bool inUse;

            Slot() noexcept;
        };

        struct WorkItem {
            Handle handle;
            std::size_t nextDependency;

            WorkItem(Handle h, std::size_t index) noexcept;
        };

        struct TransparentStringHash {
            using is_transparent = void;

            std::size_t operator()(const std::string &value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }

            std::size_t operator()(std::string_view value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }

            std::size_t operator()(const char *value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }
        };

        struct TransparentStringEqual {
            using is_transparent = void;

            bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
                return lhs == rhs;
            }
        };

        static constexpr std::size_t kHandleIndexBits = 16;
        static constexpr std::size_t kHandleIndexMask = (1u << kHandleIndexBits) - 1;

        static Handle MakeHandle(std::size_t index, std::uint16_t generation) noexcept;
        static std::size_t ExtractIndex(Handle handle) noexcept;
        static std::uint16_t ExtractGeneration(Handle handle) noexcept;
        static std::uint16_t NextGeneration(std::uint16_t generation) noexcept;

        Slot *ResolveSlot(Handle handle) noexcept;
        const Slot *ResolveSlot(Handle handle) const noexcept;

        StatusCode EnsureModuleLocked(std::string_view specifier, Handle &outHandle);
        StatusCode UpdateModuleLocked(Slot &slot,
                                      std::string_view source,
                                      const RegisterOptions &options,
                                      const std::vector<std::string> &dependencies);

        StatusCode EnsureContextUnlocked(std::string_view contextName,
                                         std::uint32_t stackSize,
                                         std::unique_lock<std::mutex> &lock);

        void DetachDependencies(ModuleRecord &record);
        void AttachDependencies(ModuleRecord &record, const std::vector<Handle> &handles);
        void MarkDependentsDirty(Handle handle);
        std::uint64_t MaxDependencyVersion(const ModuleRecord &record) const noexcept;

        StatusCode BuildEvaluationOrder(Handle root,
                                        std::vector<Handle> &outOrder,
                                        std::string &outDiagnostics);

        void ExtractDependencies(std::string_view source, std::vector<std::string> &outDependencies) const;

        std::size_t AcquireSlot();
        void ReleaseSlot(std::size_t index) noexcept;

        void ResetMetrics() noexcept;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        double m_TotalSeconds;

        std::string m_DefaultContextName;
        std::uint32_t m_DefaultStackSize;

        ResolveCallback m_Resolver;
        void *m_ResolverUserData;

        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeList;
        std::vector<std::uint8_t> m_DfsMarks;
        std::vector<std::uint8_t> m_DirtyMarks;
        std::vector<WorkItem> m_WorkStack;
        std::vector<Handle> m_EvaluationOrder;
        std::vector<std::string> m_ScratchDependencies;
        std::vector<Handle> m_DirtyStack;

        std::unordered_map<std::string, Handle, TransparentStringHash, TransparentStringEqual> m_SpecifierLookup;
        std::unordered_map<std::string, std::uint32_t, TransparentStringHash, TransparentStringEqual> m_ContextLookup;

        Metrics m_Metrics;

        mutable std::mutex m_Mutex;
    };
}
