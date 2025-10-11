#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/status.h"
#include "spectre/config.h"
#include "spectre/es2025/module.h"
#include "spectre/es2025/value.h"

namespace spectre::es2025 {
    class ArrayModule final : public Module {
    public:
        enum class StorageKind : std::uint8_t { Dense, Sparse };

        struct Metrics {
            std::size_t denseCount;
            std::size_t sparseCount;
            std::size_t denseCapacity;
            std::size_t denseLength;
            std::size_t sparseEntries;
            std::size_t transitionsToSparse;
            std::size_t transitionsToDense;
            std::size_t compactions;
            std::size_t clones;
            std::uint64_t lastMutationFrame;
            Metrics() noexcept
                : denseCount(0),
                  sparseCount(0),
                  denseCapacity(0),
                  denseLength(0),
                  sparseEntries(0),
                  transitionsToSparse(0),
                  transitionsToDense(0),
                  compactions(0),
                  clones(0),
                  lastMutationFrame(0) {}
        };

        using Handle = std::uint64_t;

        ArrayModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode CreateDense(std::string_view label, std::size_t capacityHint, Handle &outHandle);
        StatusCode CreateSparse(std::string_view label, Handle &outHandle);
        StatusCode Clone(Handle handle, std::string_view label, Handle &outHandle);
        StatusCode Destroy(Handle handle);

        StatusCode Push(Handle handle, const Value &value);
        StatusCode PushNumber(Handle handle, double value);
        StatusCode PushBoolean(Handle handle, bool value);
        StatusCode PushString(Handle handle, std::string_view value);

        StatusCode Pop(Handle handle, Value &outValue);
        StatusCode Shift(Handle handle, Value &outValue);
        StatusCode Unshift(Handle handle, const Value &value);

        StatusCode Get(Handle handle, std::size_t index, Value &outValue) const;
        StatusCode Set(Handle handle, std::size_t index, const Value &value);
        StatusCode Erase(Handle handle, std::size_t index);
        StatusCode Fill(Handle handle, const Value &value);

        StatusCode Reserve(Handle handle, std::size_t capacity);
        StatusCode Trim(Handle handle);
        StatusCode Clear(Handle handle);

        StatusCode Concat(Handle destination, Handle source);
        StatusCode Slice(Handle handle, std::size_t begin, std::size_t end, std::vector<Value> &outValues) const;

        StatusCode SortNumeric(Handle handle, bool ascending = true);
        StatusCode SortLexicographic(Handle handle, bool ascending = true);
        StatusCode BinarySearch(Handle handle, const Value &needle, bool numeric, std::size_t &outIndex) const;

        std::size_t Length(Handle handle) const;
        bool Has(Handle handle) const noexcept;
        StorageKind KindOf(Handle handle) const noexcept;
        const Metrics &GetMetrics() const noexcept;
        bool GpuEnabled() const noexcept;

    private:
        struct SparseEntry {
            std::size_t index;
            Value value;
        };

        struct DenseData {
            std::vector<Value> items;
        };

        struct SparseData {
            std::vector<SparseEntry> entries;
            std::size_t maxIndex;
        };

        struct ArrayRecord {
            Handle handle;
            std::uint32_t slot;
            std::uint32_t generation;
            StorageKind kind;
            std::string label;
            DenseData dense;
            SparseData sparse;
            std::size_t length;
            std::uint64_t version;
            std::uint64_t lastTouchFrame;
            bool pendingCompaction;
            bool hot;
        };

        struct Slot {
            ArrayRecord record;
            std::uint32_t generation;
            bool inUse;
        };

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        Metrics m_Metrics;

        ArrayRecord *FindMutable(Handle handle) noexcept;
        const ArrayRecord *Find(Handle handle) const noexcept;
        static std::uint32_t DecodeSlot(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;
        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;

        StatusCode CreateInternal(std::string_view label, StorageKind kind, std::size_t capacityHint, Handle &outHandle);
        void ResetRecord(ArrayRecord &record, StorageKind kind, std::string_view label, std::size_t capacityHint, Handle handle, std::uint32_t slot, std::uint32_t generation);
        void Touch(ArrayRecord &record) noexcept;
        void UpdateMetricsOnCreate(const ArrayRecord &record);
        void UpdateMetricsOnDestroy(const ArrayRecord &record);
        void UpdateMetricsOnResizeDense(std::size_t previousCapacity, std::size_t newCapacity);
        void UpdateKindMetrics(StorageKind fromKind, StorageKind toKind);
        void UpdateLength(ArrayRecord &record, std::size_t newLength);
        void UpdateSparseEntryCount(ArrayRecord &record, std::size_t previousCount, std::size_t newCount);
        void ConvertToSparse(ArrayRecord &record);
        void ConvertToDense(ArrayRecord &record);
        void EnsureCompaction(ArrayRecord &record);
        void CompactDense(ArrayRecord &record);
        void CompactSparse(ArrayRecord &record);
        bool MaybePromote(ArrayRecord &record);
        bool MaybeDemote(ArrayRecord &record);
        static std::size_t AlignCapacity(std::size_t capacity) noexcept;
        static int LexicographicCompare(const Value &a, const Value &b) noexcept;
        static int NumericCompare(const Value &a, const Value &b) noexcept;
        static bool IsNumeric(const Value &value) noexcept;
        static double ResolveNumeric(const Value &value, double fallback) noexcept;
    };
}
