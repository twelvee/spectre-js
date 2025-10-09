#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "spectre/es2025/module.h"
#include "spectre/es2025/value.h"
#include "spectre/es2025/modules/array_buffer_module.h"
#include "spectre/es2025/modules/shared_array_buffer_module.h"
#include "spectre/es2025/modules/typed_array_module.h"

namespace spectre::es2025 {
    class StructuredCloneModule final : public Module {
    public:
        struct Node {
            enum class Kind : std::uint8_t {
                Undefined,
                Boolean,
                Number,
                String,
                Array,
                Object,
                Map,
                Set,
                ArrayBuffer,
                SharedArrayBuffer,
                TypedArray
            };

            struct TypedArrayInfo {
                TypedArrayModule::Handle handle;
                TypedArrayModule::ElementType elementType;
                std::size_t length;
                std::size_t byteOffset;
                bool copyBuffer;
                std::string label;

                TypedArrayInfo() noexcept;
            };

            Kind kind;
            Value primitive;
            std::vector<Node> arrayItems;
            std::vector<std::pair<std::string, Node> > objectProperties;
            std::vector<std::pair<Node, Node> > mapEntries;
            std::vector<Node> setEntries;
            ArrayBufferModule::Handle arrayBuffer;
            SharedArrayBufferModule::Handle sharedBuffer;
            TypedArrayInfo typedArray;
            std::string label;
            bool transfer;

            Node() noexcept;

            static Node MakeUndefined() noexcept;

            static Node FromBoolean(bool value) noexcept;

            static Node FromNumber(double value) noexcept;

            static Node FromString(std::string_view text);

            static Node FromValue(const Value &value);
        };

        struct CloneOptions {
            bool enableTransfer;
            bool shareSharedBuffers;
            bool copyTypedArrayBuffer;
            std::vector<ArrayBufferModule::Handle> transferList;

            CloneOptions() noexcept;
        };

        struct Metrics {
            std::uint64_t cloneCalls;
            std::uint64_t valueClones;
            std::uint64_t objectCopies;
            std::uint64_t arrayCopies;
            std::uint64_t mapCopies;
            std::uint64_t setCopies;
            std::uint64_t bufferCopies;
            std::uint64_t sharedShares;
            std::uint64_t typedArrayCopies;
            std::uint64_t serializedBytes;
            std::uint64_t deserializedBytes;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        StructuredCloneModule();

        std::string_view Name() const noexcept override;

        std::string_view Summary() const noexcept override;

        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;

        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;

        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;

        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Clone(const Node &input, Node &outClone, const CloneOptions &options = {});

        StatusCode CloneValue(const Value &input, Value &outValue);

        StatusCode Serialize(const Node &input, std::vector<std::uint8_t> &outBytes);

        StatusCode Deserialize(const std::uint8_t *data, std::size_t size, Node &outNode);

        const Metrics &GetMetrics() const noexcept;

        bool GpuEnabled() const noexcept;

    private:
        struct CloneContext {
            const CloneOptions &options;
            std::unordered_map<const Node *, Node *> memo;
        };

        struct BinaryCursor {
            const std::uint8_t *data;
            std::size_t size;
            std::size_t offset;

            BinaryCursor(const std::uint8_t *ptr, std::size_t length) noexcept;

            bool Read(std::uint8_t &value) noexcept;

            bool Read(std::uint32_t &value) noexcept;

            bool Read(std::uint64_t &value) noexcept;

            bool Read(double &value) noexcept;

            bool Read(std::string &value) noexcept;

            bool ReadBytes(std::vector<std::uint8_t> &buffer, std::size_t length) noexcept;
        };

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        ArrayBufferModule *m_ArrayBufferModule;
        SharedArrayBufferModule *m_SharedArrayBufferModule;
        TypedArrayModule *m_TypedArrayModule;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        Metrics m_Metrics;
        mutable std::vector<std::uint8_t> m_ByteScratch;
        mutable std::vector<Node *> m_NodeStack;
        mutable std::vector<std::uint8_t> m_Serialized;

        StatusCode CloneNode(const Node &input, Node &outClone, CloneContext &context);

        StatusCode CloneObject(const Node &input, Node &outClone, CloneContext &context);

        StatusCode CloneArray(const Node &input, Node &outClone, CloneContext &context);

        StatusCode CloneMap(const Node &input, Node &outClone, CloneContext &context);

        StatusCode CloneSet(const Node &input, Node &outClone, CloneContext &context);

        StatusCode CloneArrayBufferHandle(ArrayBufferModule::Handle handle,
                                          bool transfer,
                                          std::string_view label,
                                          ArrayBufferModule::Handle &outHandle,
                                          const CloneOptions &options);

        StatusCode CloneSharedArrayBufferHandle(SharedArrayBufferModule::Handle handle,
                                                std::string_view label,
                                                SharedArrayBufferModule::Handle &outHandle,
                                                const CloneOptions &options);

        StatusCode CloneTypedArrayHandle(const Node &input,
                                         Node &outClone,
                                         const CloneOptions &options);

        void ResetScratch() const;

        void WriteHeader();

        StatusCode WriteNodeBinary(const Node &node);

        void WritePrimitive(const Node &node);

        void WriteString(std::string_view value);

        void WriteUint8(std::uint8_t value);

        void WriteUint32(std::uint32_t value);

        void WriteUint64(std::uint64_t value);

        void WriteDouble(double value);

        StatusCode ReadNode(BinaryCursor &cursor, Node &outNode);

        StatusCode DeserializeArray(BinaryCursor &cursor, Node &outNode);

        StatusCode DeserializeObject(BinaryCursor &cursor, Node &outNode);

        StatusCode DeserializeMap(BinaryCursor &cursor, Node &outNode);

        StatusCode DeserializeSet(BinaryCursor &cursor, Node &outNode);

        StatusCode DeserializeArrayBuffer(BinaryCursor &cursor, Node &outNode);

        StatusCode DeserializeSharedArrayBuffer(BinaryCursor &cursor, Node &outNode);

        StatusCode DeserializeTypedArray(BinaryCursor &cursor, Node &outNode);

        bool EnsureDependencies();
    };
}