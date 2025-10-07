#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "spectre/es2025/module.h"
#include "spectre/es2025/value.h"
#include "spectre/config.h"
#include "spectre/status.h"

namespace spectre::es2025 {
    class IteratorModule final : public Module {
    public:
        using Handle = std::uint32_t;

        struct Result {
            Value value;
            bool done;
            bool hasValue;

            Result() noexcept : value(Value::Undefined()), done(true), hasValue(false) {}
        };

        struct RangeConfig {
            std::int64_t start;
            std::int64_t end;
            std::int64_t step;
            bool inclusive;
        };

        using NextCallback = Result (*)(void *state);
        using VoidCallback = void (*)(void *state);

        struct CustomConfig {
            NextCallback next;
            VoidCallback reset;
            VoidCallback close;
            VoidCallback destroy;
            void *state;
        };

        IteratorModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode CreateRange(const RangeConfig &config, Handle &outHandle);
        StatusCode CreateList(std::vector<Value> values, Handle &outHandle);
        StatusCode CreateList(std::span<const Value> values, Handle &outHandle);
        StatusCode CreateCustom(const CustomConfig &config, Handle &outHandle);

        Result Next(Handle handle);
        std::size_t Drain(Handle handle, std::span<Result> buffer);
        void Reset(Handle handle);
        void Close(Handle handle);
        bool Destroy(Handle handle);

        bool Done(Handle handle) const noexcept;
        bool Valid(Handle handle) const noexcept;
        const Result &LastResult(Handle handle) const;
        std::size_t ActiveIterators() const noexcept;

    private:
        struct RangeData {
            std::int64_t start;
            std::int64_t current;
            std::int64_t end;
            std::int64_t step;
            bool inclusive;
            bool finished;
        };

        struct ListData {
            std::vector<Value> values;
            std::size_t index;
        };

        struct CustomData {
            CustomConfig config;
            bool finished;
            bool closed;
        };

        using Payload = std::variant<std::monostate, RangeData, ListData, CustomData>;

        struct Slot {
            Payload payload;
            Result last;
            std::uint32_t generation;
            bool active;
            bool done;
            std::uint64_t lastFrame;

            Slot() noexcept : payload(std::monostate{}), last(), generation(0), active(false), done(true), lastFrame(0) {}
        };

        static constexpr std::uint32_t kIndexBits = 20;
        static constexpr std::uint32_t kGenerationBits = 12;
        static constexpr std::uint32_t kIndexMask = (1u << kIndexBits) - 1u;
        static constexpr std::uint32_t kGenerationMask = (1u << kGenerationBits) - 1u;

        static Handle MakeHandle(std::uint32_t index, std::uint32_t generation) noexcept;
        static std::uint32_t DecodeIndex(Handle handle) noexcept;
        static std::uint32_t DecodeGeneration(Handle handle) noexcept;

        Slot *GetSlot(Handle handle) noexcept;
        const Slot *GetSlot(Handle handle) const noexcept;
        std::pair<Slot *, std::uint32_t> AllocateSlot();
        void ReleaseSlot(std::uint32_t index);

        void FinalizeSlot(Slot &slot);

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeList;
        std::size_t m_Active;
    };
}

