#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <memory>

#include "spectre/es2025/module.h"
#include "spectre/es2025/value.h"
#include "spectre/es2025/modules/iterator_module.h"
#include "spectre/config.h"
#include "spectre/status.h"

namespace spectre::es2025 {
    class IteratorModule;

    class GeneratorModule final : public Module {
    public:
        using Handle = std::uint32_t;

        struct StepResult {
            Value value;
            bool done;
            bool hasValue;
            bool awaitingInput;

            StepResult() noexcept : value(Value::Undefined()), done(true), hasValue(false), awaitingInput(false) {}
        };

        struct ExecutionContext {
            std::string_view input;
            Value &yieldValue;
            bool hasValue;
            bool done;
            std::uint32_t resumePoint;
            std::uint32_t nextResumePoint;
            bool requestingInput;
        };

        using Stepper = void (*)(void *state, ExecutionContext &context);
        using Cleanup = void (*)(void *state);

        struct Descriptor {
            Stepper stepper;
            void *state;
            Cleanup reset;
            Cleanup destroy;
            std::string name;
            std::uint32_t resumePoint;
        };

        GeneratorModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Register(const Descriptor &descriptor, Handle &outHandle);
        bool Destroy(Handle handle);
        StepResult Resume(Handle handle, std::string_view input = {});
        void Reset(Handle handle);
        bool Completed(Handle handle) const noexcept;
        bool Valid(Handle handle) const noexcept;
        std::size_t ActiveGenerators() const noexcept;
        std::uint64_t ResumeCount(Handle handle) const noexcept;
        std::uint64_t LastFrameTouched(Handle handle) const noexcept;
        StatusCode CreateIteratorBridge(Handle handle, IteratorModule &iteratorModule, std::uint32_t &outIteratorHandle);

    private:
        struct Slot {
            Stepper stepper;
            void *state;
            Cleanup reset;
            Cleanup destroy;
            std::string name;
            Value yieldValue;
            bool hasValue;
            bool done;
            bool running;
            bool awaitingInput;
            std::uint32_t generation;
            std::uint32_t resumePoint;
            std::uint32_t initialResumePoint;
            std::uint64_t resumeCount;
            std::uint64_t lastFrame;
            bool active;

            Slot()
                : stepper(nullptr),
                  state(nullptr),
                  reset(nullptr),
                  destroy(nullptr),
                  name(),
                  yieldValue(Value::Undefined()),
                  hasValue(false),
                  done(true),
                  running(false),
                  awaitingInput(false),
                  generation(0),
                  resumePoint(0),
                  initialResumePoint(0),
                  resumeCount(0),
                  lastFrame(0),
                  active(false) {}
        };

        struct BridgeState {
            Handle handle;
            std::uint32_t generation;
            GeneratorModule *module;
            bool closed;
            std::uint32_t index;
            bool active;

            BridgeState() : handle(0), generation(0), module(nullptr), closed(true), index(0), active(false) {}
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

        BridgeState *AllocateBridge(Handle handle, std::uint32_t generation);
        void ReleaseBridge(std::uint32_t index);
        StepResult ResumeInternal(Slot &slot, std::string_view input);
        IteratorModule::Result BridgeNext(BridgeState &bridge);
        void BridgeReset(BridgeState &bridge);
        void BridgeClose(BridgeState &bridge);
        void BridgeDestroy(BridgeState &bridge);
        static IteratorModule::Result BridgeNextThunk(void *state);
        static void BridgeResetThunk(void *state);
        static void BridgeCloseThunk(void *state);
        static void BridgeDestroyThunk(void *state);
        void CloseSlot(Slot &slot);

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        std::vector<Slot> m_Slots;
        std::vector<std::uint32_t> m_FreeSlots;
        std::vector<std::unique_ptr<BridgeState>> m_Bridges;
        std::vector<std::uint32_t> m_FreeBridges;
        std::size_t m_Active;
        std::size_t m_ActiveBridges;
    };
}
