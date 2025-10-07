#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/es2025/module.h"
#include "spectre/config.h"
#include "spectre/status.h"

namespace spectre::es2025 {
    class ObjectModule final : public Module {
    public:
        using Handle = std::uint64_t;

        struct Value {
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
            bool IsHandle() const noexcept;
            bool IsString() const noexcept;

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
                Scalar() : handleValue(0) {}
            } m_Scalar;
            Kind m_Kind;
            std::string m_String;
        };

        struct PropertyDescriptor {
            Value value;
            bool writable;
            bool enumerable;
            bool configurable;
        };

        struct Metrics {
            std::uint64_t liveObjects;
            std::uint64_t totalAllocations;
            std::uint64_t totalReleases;
            std::uint64_t propertyAdds;
            std::uint64_t propertyUpdates;
            std::uint64_t propertyRemovals;
            std::uint64_t fastPathHits;
            std::uint64_t prototypeHits;
            std::uint64_t misses;
            std::uint64_t rehashes;
            std::uint64_t collisions;
            std::uint64_t seals;
            std::uint64_t freezes;
            std::uint64_t lastFrameTouched;
            bool gpuOptimized;
        };

        ObjectModule();
        ~ObjectModule() override;

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Create(std::string_view label, Handle prototype, Handle &outHandle);
        StatusCode Clone(Handle source, std::string_view label, Handle &outHandle);
        StatusCode Destroy(Handle handle);

        StatusCode Define(Handle handle, std::string_view key, const PropertyDescriptor &descriptor);
        StatusCode Set(Handle handle, std::string_view key, const Value &value);
        StatusCode Get(Handle handle, std::string_view key, Value &outValue) const;
        bool Has(Handle handle, std::string_view key) const;
        StatusCode Delete(Handle handle, std::string_view key, bool &outDeleted);
        StatusCode OwnKeys(Handle handle, std::vector<std::string> &keys) const;

        StatusCode SetPrototype(Handle handle, Handle prototype);
        Handle Prototype(Handle handle) const;
        StatusCode SetExtensible(Handle handle, bool extensible);
        bool IsExtensible(Handle handle) const;
        StatusCode Seal(Handle handle);
        StatusCode Freeze(Handle handle);
        bool IsSealed(Handle handle) const;
        bool IsFrozen(Handle handle) const;

        bool IsValid(Handle handle) const noexcept;

        const Metrics &GetMetrics() const noexcept;

    private:
        struct PropertySlot;
        struct ObjectRecord;
        struct SlotRecord;

        static constexpr std::uint32_t kInvalidIndex = 0xffffffffu;
        static constexpr std::uint32_t kDeletedIndex = 0xfffffffeu;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        Metrics m_Metrics;
        std::vector<SlotRecord> m_Slots;
        std::vector<std::uint32_t> m_FreeList;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;

        SlotRecord *FindMutableSlot(Handle handle) noexcept;
        const SlotRecord *FindSlot(Handle handle) const noexcept;

        ObjectRecord *FindMutable(Handle handle) noexcept;
        const ObjectRecord *Find(Handle handle) const noexcept;

        static std::uint64_t HashKey(std::string_view key) noexcept;
        static std::uint32_t NormalizeBuckets(std::uint32_t requested) noexcept;

        StatusCode InsertOrUpdate(ObjectRecord &object, std::string_view key, const PropertyDescriptor &descriptor, bool allowNew);
        StatusCode UpdateValue(ObjectRecord &object, std::string_view key, const Value &value, bool allowNew);
        StatusCode RemoveProperty(ObjectRecord &object, std::string_view key, bool &outDeleted);

        std::uint32_t Locate(const ObjectRecord &object, std::string_view key, std::uint64_t hash, std::uint32_t &bucket, bool &collision) const;
        std::uint32_t AllocateProperty(ObjectRecord &object);
        void EnsureCapacity(ObjectRecord &object);
        void Rehash(ObjectRecord &object, std::uint32_t newBucketCount);

        void Touch(ObjectRecord &object) noexcept;
        void TouchMetrics() noexcept;

        static Handle EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;
        static std::uint32_t DecodeSlot(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;
    };
}


