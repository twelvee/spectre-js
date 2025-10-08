#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class SetModule final : public Module {
    public:
        using Handle = std::uint64_t;

        struct Value {
            friend class SetModule;

            enum class Kind : std::uint8_t { Undefined, Boolean, Int64, Double, String, Handle };

            Value();

            Value(const Value &other);

            Value(Value &&other) noexcept;

            Value &operator=(const Value &other);

            Value &operator=(Value &&other) noexcept;

            ~Value();

            static Value Undefined();

            static Value FromBoolean(bool v);

            static Value FromInt(std::int64_t v);

            static Value FromDouble(double v);

            static Value FromHandle(Handle v);

            static Value FromString(std::string_view text);

            bool IsUndefined() const noexcept;

            bool IsBoolean() const noexcept;

            bool IsInt() const noexcept;

            bool IsDouble() const noexcept;

            bool IsString() const noexcept;

            bool IsHandle() const noexcept;

            bool Boolean() const noexcept;

            std::int64_t Int() const noexcept;

            double Double() const noexcept;

            Handle HandleValue() const noexcept;

            std::string_view String() const noexcept;

            void Assign(const Value &other);

            void Assign(Value &&other) noexcept;

            void Reset() noexcept;

            bool Equals(const Value &other) const noexcept;

        private:
            union Scalar {
                bool booleanValue;
                std::int64_t intValue;
                double doubleValue;
                Handle handleValue;

                Scalar() : handleValue(0) {
                }
            } m_Scalar;

            Kind m_Kind;
            std::string m_String;
        };

        struct Metrics {
            std::uint64_t liveSets;
            std::uint64_t totalAllocations;
            std::uint64_t totalReleases;
            std::uint64_t addOps;
            std::uint64_t hasOps;
            std::uint64_t deleteOps;
            std::uint64_t hits;
            std::uint64_t misses;
            std::uint64_t rehashes;
            std::uint64_t collisions;
            std::uint64_t iterations;
            std::uint64_t clears;
            std::uint64_t lastFrameTouched;
            bool gpuOptimized;
        };

        SetModule();

        ~SetModule() override;

        std::string_view Name() const noexcept override;

        std::string_view Summary() const noexcept override;

        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;

        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;

        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;

        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(std::string_view label, Handle &outHandle);

        StatusCode Destroy(Handle handle);

        StatusCode Clear(Handle handle);

        StatusCode Add(Handle handle, const Value &value);

        bool Has(Handle handle, const Value &value) const;

        StatusCode Delete(Handle handle, const Value &value, bool &outDeleted);

        std::uint32_t Size(Handle handle) const;

        StatusCode Values(Handle handle, std::vector<Value> &values) const;

        StatusCode Entries(Handle handle, std::vector<std::pair<Value, Value> > &entries) const;

        const Metrics &GetMetrics() const noexcept;

    private:
        struct Entry;
        struct SetRecord;
        struct SlotRecord;

        static constexpr std::uint32_t kInvalidIndex = 0xffffffffu;
        static constexpr std::uint32_t kDeletedIndex = 0xfffffffeu;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        Metrics m_Metrics;
        std::vector<SlotRecord> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;

        SlotRecord *FindMutableSlot(Handle handle) noexcept;

        const SlotRecord *FindSlot(Handle handle) const noexcept;

        SetRecord *FindMutable(Handle handle) noexcept;

        const SetRecord *Find(Handle handle) const noexcept;

        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;

        static std::uint32_t DecodeSlot(Handle handle) noexcept;

        static std::uint32_t DecodeGeneration(Handle handle) noexcept;

        static std::uint64_t HashValue(const Value &value) noexcept;

        void Touch(SetRecord &record) noexcept;

        void TouchMetrics() noexcept;

        void EnsureCapacity(SetRecord &record);

        void Rehash(SetRecord &record, std::uint32_t newBucketCount);

        std::uint32_t Locate(const SetRecord &record, const Value &value, std::uint64_t hash,
                             std::uint32_t &bucket, bool &collision) const;

        std::uint32_t AllocateEntry(SetRecord &record);

        StatusCode Insert(SetRecord &record, const Value &value, std::uint64_t hash);

        StatusCode Remove(SetRecord &record, const Value &value, std::uint64_t hash, bool &outDeleted);

        void LinkTail(SetRecord &record, std::uint32_t entryIndex);

        void Unlink(SetRecord &record, std::uint32_t entryIndex);
    };
}
