#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class StringModule final : public Module {
    public:
        using Handle = std::uint64_t;

        struct Metrics {
            std::uint64_t internHits;
            std::uint64_t internMisses;
            std::uint64_t reuseHits;
            std::uint64_t allocations;
            std::uint64_t releases;
            std::uint64_t slices;
            std::uint64_t transforms;
            std::uint64_t lastFrameTouched;
            std::uint64_t hotStrings;
            std::uint64_t activeStrings;
            std::uint64_t bytesInUse;
            std::uint64_t bytesReserved;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        StringModule();

        std::string_view Name() const noexcept override;

        std::string_view Summary() const noexcept override;

        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;

        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;

        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;

        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(std::string_view label, std::string_view value, Handle &outHandle);

        StatusCode Clone(Handle handle, std::string_view label, Handle &outHandle);

        StatusCode Intern(std::string_view value, Handle &outHandle);

        StatusCode Release(Handle handle);

        StatusCode Concat(Handle left, Handle right, std::string_view label, Handle &outHandle);

        StatusCode Slice(Handle handle, std::size_t begin, std::size_t length, std::string_view label,
                         Handle &outHandle);

        StatusCode Append(Handle handle, std::string_view suffix);

        StatusCode ToUpperAscii(Handle handle) noexcept;

        StatusCode ToLowerAscii(Handle handle) noexcept;

        StatusCode TrimAscii(Handle handle) noexcept;

        bool Has(Handle handle) const noexcept;

        std::string_view View(Handle handle) const noexcept;

        std::uint64_t Hash(Handle handle) const noexcept;

        const Metrics &GetMetrics() const noexcept;

        bool GpuEnabled() const noexcept;

    private:
        struct Entry {
            Handle handle;
            std::uint32_t slot;
            std::uint32_t generation;
            std::uint32_t offset;
            std::uint32_t length;
            std::uint32_t capacity;
            std::uint64_t hash;
            std::uint32_t refCount;
            std::string label;
            std::uint64_t version;
            std::uint64_t lastTouchFrame;
            bool hot;
            bool pinned;
        };

        struct Slot {
            Entry entry;
            std::uint32_t generation;
            bool inUse;
        };

        enum class InternState : std::uint8_t { Empty, Occupied, Tombstone };

        struct InternSlot {
            std::uint64_t hash;
            std::uint32_t slot;
            InternState state;
        };

        struct FreeBlock {
            std::uint32_t offset;
            std::uint32_t length;
        };

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        std::vector<char> m_TextStorage;
        std::vector<InternSlot> m_InternTable;
        std::size_t m_InternCount;
        std::vector<FreeBlock> m_FreeBlocks;
        Metrics m_Metrics;

        Entry *FindMutable(Handle handle) noexcept;

        const Entry *Find(Handle handle) const noexcept;

        static std::uint32_t DecodeSlot(Handle handle) noexcept;

        static std::uint32_t DecodeGeneration(Handle handle) noexcept;

        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;

        static std::uint32_t AlignSize(std::size_t size) noexcept;

        static bool IsAsciiWhitespace(unsigned char c) noexcept;

        static std::size_t NextPowerOfTwo(std::size_t value) noexcept;

        void Touch(Entry &entry) noexcept;

        void Reset();

        void RecomputeHotMetrics() noexcept;

        void EnsureTextCapacity(std::size_t requiredAdditionalBytes);

        bool AllocateText(std::size_t length, std::uint32_t &offset, std::uint32_t &capacity);

        void FreeText(std::uint32_t offset, std::uint32_t capacity);

        void WriteText(std::uint32_t offset, std::string_view value) noexcept;

        std::string_view ViewInternal(const Entry &entry) const noexcept;

        std::uint64_t HashValue(std::string_view value) const noexcept;

        StatusCode CreateInternal(std::string_view label, std::string_view value, bool pinned, std::uint64_t hash,
                                  Handle &outHandle);

        void EnsureInternCapacity();

        bool LookupIntern(std::string_view value, std::uint64_t hash, std::uint32_t &outSlot) const noexcept;

        void InsertIntern(std::uint32_t slotIndex, std::uint64_t hash);

        void EraseIntern(std::uint32_t slotIndex, std::uint64_t hash);
    };
}