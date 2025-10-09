#include "spectre/es2025/modules/structured_clone_module.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "spectre/es2025/environment.h"
#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "StructuredClone";
        constexpr std::string_view kSummary = "Structured cloning algorithms for host messaging and persistence.";
        constexpr std::string_view kReference = "WHATWG HTML Section 2.7";
        constexpr std::uint32_t kHeaderMagic = 0x4e4c4353u; // "SCLN"
        constexpr std::uint32_t kBinaryVersion = 1;
        constexpr std::string_view kArrayBufferCloneLabel = "structured.clone.buffer";
        constexpr std::string_view kSharedBufferCloneLabel = "structured.clone.shared";
        constexpr std::string_view kTypedArrayCloneLabel = "structured.clone.view";

        bool HandleAllowedForTransfer(ArrayBufferModule::Handle handle,
                                      const StructuredCloneModule::CloneOptions &options) {
            if (options.transferList.empty()) {
                return true;
            }
            return std::find(options.transferList.begin(), options.transferList.end(), handle) != options.transferList.
                   end();
        }

        std::size_t SafeCastToSize(std::uint64_t value) {
            if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                return std::numeric_limits<std::size_t>::max();
            }
            return static_cast<std::size_t>(value);
        }
    }

    StructuredCloneModule::Node::TypedArrayInfo::TypedArrayInfo() noexcept
        : handle(0),
          elementType(TypedArrayModule::ElementType::Int8),
          length(0),
          byteOffset(0),
          copyBuffer(true),
          label() {
    }

    StructuredCloneModule::Node::Node() noexcept
        : kind(Kind::Undefined),
          primitive(Value::Undefined()),
          arrayItems(),
          objectProperties(),
          mapEntries(),
          setEntries(),
          arrayBuffer(0),
          sharedBuffer(0),
          typedArray(),
          label(),
          transfer(false) {
    }

    StructuredCloneModule::Node StructuredCloneModule::Node::MakeUndefined() noexcept {
        return Node();
    }

    StructuredCloneModule::Node StructuredCloneModule::Node::FromBoolean(bool value) noexcept {
        Node node;
        node.kind = Kind::Boolean;
        node.primitive = Value::Boolean(value);
        return node;
    }

    StructuredCloneModule::Node StructuredCloneModule::Node::FromNumber(double value) noexcept {
        Node node;
        node.kind = Kind::Number;
        node.primitive = Value::Number(value);
        return node;
    }

    StructuredCloneModule::Node StructuredCloneModule::Node::FromString(std::string_view text) {
        Node node;
        node.kind = Kind::String;
        node.primitive = Value::String(text);
        return node;
    }

    StructuredCloneModule::Node StructuredCloneModule::Node::FromValue(const Value &value) {
        Node node;
        switch (value.kind) {
            case Value::Kind::Undefined:
                node.kind = Kind::Undefined;
                node.primitive = Value::Undefined();
                break;
            case Value::Kind::Number:
                node.kind = Kind::Number;
                node.primitive = Value::Number(value.number);
                break;
            case Value::Kind::Boolean:
                node.kind = Kind::Boolean;
                node.primitive = Value::Boolean(value.booleanValue);
                break;
            case Value::Kind::String:
                node.kind = Kind::String;
                node.primitive = Value::String(value.text);
                break;
        }
        return node;
    }

    StructuredCloneModule::CloneOptions::CloneOptions() noexcept
        : enableTransfer(false),
          shareSharedBuffers(true),
          copyTypedArrayBuffer(false),
          transferList() {
    }

    StructuredCloneModule::Metrics::Metrics() noexcept
        : cloneCalls(0),
          valueClones(0),
          objectCopies(0),
          arrayCopies(0),
          mapCopies(0),
          setCopies(0),
          bufferCopies(0),
          sharedShares(0),
          typedArrayCopies(0),
          serializedBytes(0),
          deserializedBytes(0),
          gpuOptimized(false) {
    }

    StructuredCloneModule::BinaryCursor::BinaryCursor(const std::uint8_t *ptr, std::size_t length) noexcept
        : data(ptr),
          size(length),
          offset(0) {
    }

    bool StructuredCloneModule::BinaryCursor::Read(std::uint8_t &value) noexcept {
        if (offset >= size) {
            return false;
        }
        value = data[offset];
        offset += 1;
        return true;
    }

    bool StructuredCloneModule::BinaryCursor::Read(std::uint32_t &value) noexcept {
        if ((size - offset) < sizeof(std::uint32_t)) {
            return false;
        }
        std::memcpy(&value, data + offset, sizeof(std::uint32_t));
        offset += sizeof(std::uint32_t);
        return true;
    }

    bool StructuredCloneModule::BinaryCursor::Read(std::uint64_t &value) noexcept {
        if ((size - offset) < sizeof(std::uint64_t)) {
            return false;
        }
        std::memcpy(&value, data + offset, sizeof(std::uint64_t));
        offset += sizeof(std::uint64_t);
        return true;
    }

    bool StructuredCloneModule::BinaryCursor::Read(double &value) noexcept {
        std::uint64_t bits = 0;
        if (!Read(bits)) {
            return false;
        }
        std::memcpy(&value, &bits, sizeof(double));
        return true;
    }

    bool StructuredCloneModule::BinaryCursor::Read(std::string &value) noexcept {
        std::uint32_t length = 0;
        if (!Read(length)) {
            return false;
        }
        if ((size - offset) < length) {
            return false;
        }
        if (length == 0) {
            value.clear();
            return true;
        }
        value.assign(reinterpret_cast<const char *>(data + offset), length);
        offset += length;
        return true;
    }

    bool StructuredCloneModule::BinaryCursor::ReadBytes(std::vector<std::uint8_t> &buffer,
                                                        std::size_t length) noexcept {
        if ((size - offset) < length) {
            return false;
        }
        buffer.resize(length);
        if (length > 0) {
            std::memcpy(buffer.data(), data + offset, length);
        }
        offset += length;
        return true;
    }

    StructuredCloneModule::StructuredCloneModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_ArrayBufferModule(nullptr),
          m_SharedArrayBufferModule(nullptr),
          m_TypedArrayModule(nullptr),
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Metrics(),
          m_ByteScratch(),
          m_NodeStack(),
          m_Serialized() {
    }

    std::string_view StructuredCloneModule::Name() const noexcept {
        return kName;
    }

    std::string_view StructuredCloneModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view StructuredCloneModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void StructuredCloneModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
        m_CurrentFrame = 0;
        m_Initialized = true;
        ResetScratch();
        EnsureDependencies();
    }

    void StructuredCloneModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
    }

    void StructuredCloneModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void StructuredCloneModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    StatusCode StructuredCloneModule::Clone(const Node &input, Node &outClone, const CloneOptions &options) {
        outClone = Node();
        if (!EnsureDependencies()) {
            return StatusCode::NotFound;
        }
        m_Metrics.cloneCalls += 1;
        CloneContext context{options, {}};
        auto status = CloneNode(input, outClone, context);
        return status;
    }

    StatusCode StructuredCloneModule::CloneValue(const Value &input, Value &outValue) {
        outValue = input;
        m_Metrics.valueClones += 1;
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::Serialize(const Node &input, std::vector<std::uint8_t> &outBytes) {
        if (!EnsureDependencies()) {
            return StatusCode::NotFound;
        }
        ResetScratch();
        WriteHeader();
        auto status = WriteNodeBinary(input);
        if (status != StatusCode::Ok) {
            m_Serialized.clear();
            return status;
        }
        outBytes = m_Serialized;
        m_Metrics.serializedBytes += static_cast<std::uint64_t>(outBytes.size());
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::Deserialize(const std::uint8_t *data, std::size_t size, Node &outNode) {
        outNode = Node();
        if (!EnsureDependencies()) {
            return StatusCode::NotFound;
        }
        if (!data || size < sizeof(std::uint32_t) * 2) {
            return StatusCode::InvalidArgument;
        }
        BinaryCursor cursor(data, size);
        std::uint32_t magic = 0;
        if (!cursor.Read(magic) || magic != kHeaderMagic) {
            return StatusCode::InvalidArgument;
        }
        std::uint32_t version = 0;
        if (!cursor.Read(version) || version != kBinaryVersion) {
            return StatusCode::InvalidArgument;
        }
        auto status = ReadNode(cursor, outNode);
        if (status != StatusCode::Ok) {
            outNode = Node();
            return status;
        }
        m_Metrics.deserializedBytes += static_cast<std::uint64_t>(cursor.offset);
        return StatusCode::Ok;
    }

    const StructuredCloneModule::Metrics &StructuredCloneModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool StructuredCloneModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    StatusCode StructuredCloneModule::CloneNode(const Node &input, Node &outClone, CloneContext &context) {
        auto existing = context.memo.find(&input);
        if (existing != context.memo.end() && existing->second) {
            outClone = *(existing->second);
            return StatusCode::Ok;
        }

        outClone = Node();
        outClone.kind = input.kind;
        outClone.label = input.label;
        outClone.transfer = input.transfer;
        outClone.typedArray = input.typedArray;
        outClone.primitive = input.primitive;
        context.memo[&input] = &outClone;

        switch (input.kind) {
            case Node::Kind::Undefined:
            case Node::Kind::Boolean:
            case Node::Kind::Number:
            case Node::Kind::String:
                return StatusCode::Ok;
            case Node::Kind::Array:
                return CloneArray(input, outClone, context);
            case Node::Kind::Object:
                return CloneObject(input, outClone, context);
            case Node::Kind::Map:
                return CloneMap(input, outClone, context);
            case Node::Kind::Set:
                return CloneSet(input, outClone, context);
            case Node::Kind::ArrayBuffer: {
                auto status = CloneArrayBufferHandle(input.arrayBuffer,
                                                     input.transfer,
                                                     input.label,
                                                     outClone.arrayBuffer,
                                                     context.options);
                return status;
            }
            case Node::Kind::SharedArrayBuffer: {
                auto status = CloneSharedArrayBufferHandle(input.sharedBuffer,
                                                           input.label,
                                                           outClone.sharedBuffer,
                                                           context.options);
                return status;
            }
            case Node::Kind::TypedArray:
                outClone.typedArray.label = input.typedArray.label;
                return CloneTypedArrayHandle(input, outClone, context.options);
        }
        return StatusCode::InvalidArgument;
    }

    StatusCode StructuredCloneModule::CloneObject(const Node &input, Node &outClone, CloneContext &context) {
        outClone.kind = Node::Kind::Object;
        outClone.objectProperties.clear();
        outClone.objectProperties.reserve(input.objectProperties.size());
        for (const auto &property: input.objectProperties) {
            outClone.objectProperties.emplace_back(property.first, Node());
            auto &target = outClone.objectProperties.back().second;
            auto status = CloneNode(property.second, target, context);
            if (status != StatusCode::Ok) {
                return status;
            }
        }
        m_Metrics.objectCopies += 1;
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::CloneArray(const Node &input, Node &outClone, CloneContext &context) {
        outClone.kind = Node::Kind::Array;
        outClone.arrayItems.clear();
        outClone.arrayItems.reserve(input.arrayItems.size());
        for (const auto &item: input.arrayItems) {
            outClone.arrayItems.emplace_back();
            auto &target = outClone.arrayItems.back();
            auto status = CloneNode(item, target, context);
            if (status != StatusCode::Ok) {
                return status;
            }
        }
        m_Metrics.arrayCopies += 1;
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::CloneMap(const Node &input, Node &outClone, CloneContext &context) {
        outClone.kind = Node::Kind::Map;
        outClone.mapEntries.clear();
        outClone.mapEntries.reserve(input.mapEntries.size());
        for (const auto &entry: input.mapEntries) {
            outClone.mapEntries.emplace_back();
            auto &target = outClone.mapEntries.back();
            auto status = CloneNode(entry.first, target.first, context);
            if (status != StatusCode::Ok) {
                return status;
            }
            status = CloneNode(entry.second, target.second, context);
            if (status != StatusCode::Ok) {
                return status;
            }
        }
        m_Metrics.mapCopies += 1;
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::CloneSet(const Node &input, Node &outClone, CloneContext &context) {
        outClone.kind = Node::Kind::Set;
        outClone.setEntries.clear();
        outClone.setEntries.resize(input.setEntries.size());
        for (std::size_t i = 0; i < input.setEntries.size(); ++i) {
            auto status = CloneNode(input.setEntries[i], outClone.setEntries[i], context);
            if (status != StatusCode::Ok) {
                return status;
            }
        }
        m_Metrics.setCopies += 1;
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::CloneArrayBufferHandle(ArrayBufferModule::Handle handle,
                                                             bool transfer,
                                                             std::string_view label,
                                                             ArrayBufferModule::Handle &outHandle,
                                                             const CloneOptions &options) {
        outHandle = 0;
        if (handle == 0) {
            return StatusCode::Ok;
        }
        if (!m_ArrayBufferModule) {
            return StatusCode::NotFound;
        }
        if (!m_ArrayBufferModule->Has(handle)) {
            return StatusCode::InvalidArgument;
        }
        if (transfer && (!options.enableTransfer || !HandleAllowedForTransfer(handle, options))) {
            return StatusCode::InvalidArgument;
        }
        std::string_view cloneLabel = label.empty() ? kArrayBufferCloneLabel : label;
        auto status = m_ArrayBufferModule->Clone(handle, cloneLabel, outHandle);
        if (status != StatusCode::Ok) {
            return status;
        }
        if (transfer) {
            m_ArrayBufferModule->Detach(handle);
        }
        m_Metrics.bufferCopies += 1;
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::CloneSharedArrayBufferHandle(SharedArrayBufferModule::Handle handle,
                                                                   std::string_view label,
                                                                   SharedArrayBufferModule::Handle &outHandle,
                                                                   const CloneOptions &options) {
        outHandle = 0;
        if (handle == 0) {
            return StatusCode::Ok;
        }
        if (!m_SharedArrayBufferModule) {
            return StatusCode::NotFound;
        }
        if (!m_SharedArrayBufferModule->Has(handle)) {
            return StatusCode::InvalidArgument;
        }
        std::string_view cloneLabel = label.empty() ? kSharedBufferCloneLabel : label;
        if (options.shareSharedBuffers) {
            auto status = m_SharedArrayBufferModule->Share(handle, cloneLabel, outHandle);
            if (status != StatusCode::Ok) {
                return status;
            }
            m_Metrics.sharedShares += 1;
            return StatusCode::Ok;
        }

        std::size_t byteLength = m_SharedArrayBufferModule->ByteLength(handle);
        std::size_t maxByteLength = m_SharedArrayBufferModule->MaxByteLength(handle);
        bool resizable = m_SharedArrayBufferModule->Resizable(handle);

        SharedArrayBufferModule::Handle cloneHandle = 0;
        StatusCode status;
        if (resizable) {
            status = m_SharedArrayBufferModule->CreateResizable(cloneLabel, byteLength, maxByteLength, cloneHandle);
        } else {
            status = m_SharedArrayBufferModule->Create(cloneLabel, byteLength, cloneHandle);
        }
        if (status != StatusCode::Ok) {
            return status;
        }

        if (byteLength > 0) {
            status = m_SharedArrayBufferModule->Snapshot(handle, m_ByteScratch);
            if (status != StatusCode::Ok) {
                m_SharedArrayBufferModule->Destroy(cloneHandle);
                return status;
            }
            status = m_SharedArrayBufferModule->CopyIn(cloneHandle, 0, m_ByteScratch.data(), m_ByteScratch.size());
            if (status != StatusCode::Ok) {
                m_SharedArrayBufferModule->Destroy(cloneHandle);
                return status;
            }
        }
        outHandle = cloneHandle;
        m_Metrics.bufferCopies += 1;
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::CloneTypedArrayHandle(const Node &input,
                                                            Node &outClone,
                                                            const CloneOptions &options) {
        (void) options;
        outClone.kind = Node::Kind::TypedArray;
        outClone.typedArray = input.typedArray;
        outClone.typedArray.handle = 0;

        if (!m_TypedArrayModule) {
            return StatusCode::NotFound;
        }
        if (input.typedArray.handle == 0) {
            return StatusCode::Ok;
        }

        TypedArrayModule::Snapshot snapshot{};
        auto describeStatus = m_TypedArrayModule->Describe(input.typedArray.handle, snapshot);
        if (describeStatus != StatusCode::Ok) {
            return describeStatus;
        }

        std::string_view cloneLabel = input.typedArray.label.empty() ? kTypedArrayCloneLabel : input.typedArray.label;
        TypedArrayModule::Handle cloneHandle = 0;
        auto status = m_TypedArrayModule->Create(snapshot.type, snapshot.length, cloneLabel, cloneHandle);
        if (status != StatusCode::Ok) {
            return status;
        }

        const bool isBigInt = snapshot.type == TypedArrayModule::ElementType::BigInt64 ||
                              snapshot.type == TypedArrayModule::ElementType::BigUint64;
        if (isBigInt) {
            std::vector<std::int64_t> values;
            status = m_TypedArrayModule->ToBigIntVector(input.typedArray.handle, values);
            if (status != StatusCode::Ok) {
                m_TypedArrayModule->Destroy(cloneHandle);
                return status;
            }
            for (std::size_t i = 0; i < snapshot.length && i < values.size(); ++i) {
                status = m_TypedArrayModule->SetBigInt(cloneHandle, i, values[i]);
                if (status != StatusCode::Ok) {
                    m_TypedArrayModule->Destroy(cloneHandle);
                    return status;
                }
            }
        } else {
            std::vector<double> values;
            status = m_TypedArrayModule->ToVector(input.typedArray.handle, values);
            if (status != StatusCode::Ok) {
                m_TypedArrayModule->Destroy(cloneHandle);
                return status;
            }
            for (std::size_t i = 0; i < snapshot.length && i < values.size(); ++i) {
                status = m_TypedArrayModule->Set(cloneHandle, i, values[i], false);
                if (status != StatusCode::Ok) {
                    m_TypedArrayModule->Destroy(cloneHandle);
                    return status;
                }
            }
        }

        outClone.typedArray.handle = cloneHandle;
        outClone.typedArray.elementType = snapshot.type;
        outClone.typedArray.length = snapshot.length;
        outClone.typedArray.byteOffset = 0;
        outClone.typedArray.copyBuffer = true;
        m_Metrics.typedArrayCopies += 1;
        return StatusCode::Ok;
    }


    void StructuredCloneModule::ResetScratch() const {
        m_ByteScratch.clear();
        m_NodeStack.clear();
        m_Serialized.clear();
    }

    void StructuredCloneModule::WriteHeader() {
        m_Serialized.reserve(m_Serialized.size() + 8);
        WriteUint32(kHeaderMagic);
        WriteUint32(kBinaryVersion);
    }

    StatusCode StructuredCloneModule::WriteNodeBinary(const Node &node) {
        WriteUint8(static_cast<std::uint8_t>(node.kind));
        WriteString(node.label);
        WriteUint8(node.transfer ? 1u : 0u);

        switch (node.kind) {
            case Node::Kind::Undefined:
                return StatusCode::Ok;
            case Node::Kind::Boolean:
            case Node::Kind::Number:
            case Node::Kind::String:
                WritePrimitive(node);
                return StatusCode::Ok;
            case Node::Kind::Array: {
                WriteUint32(static_cast<std::uint32_t>(node.arrayItems.size()));
                for (const auto &item: node.arrayItems) {
                    auto status = WriteNodeBinary(item);
                    if (status != StatusCode::Ok) {
                        return status;
                    }
                }
                return StatusCode::Ok;
            }
            case Node::Kind::Object: {
                WriteUint32(static_cast<std::uint32_t>(node.objectProperties.size()));
                for (const auto &property: node.objectProperties) {
                    WriteString(property.first);
                    auto status = WriteNodeBinary(property.second);
                    if (status != StatusCode::Ok) {
                        return status;
                    }
                }
                return StatusCode::Ok;
            }
            case Node::Kind::Map: {
                WriteUint32(static_cast<std::uint32_t>(node.mapEntries.size()));
                for (const auto &entry: node.mapEntries) {
                    auto status = WriteNodeBinary(entry.first);
                    if (status != StatusCode::Ok) {
                        return status;
                    }
                    status = WriteNodeBinary(entry.second);
                    if (status != StatusCode::Ok) {
                        return status;
                    }
                }
                return StatusCode::Ok;
            }
            case Node::Kind::Set: {
                WriteUint32(static_cast<std::uint32_t>(node.setEntries.size()));
                for (const auto &entry: node.setEntries) {
                    auto status = WriteNodeBinary(entry);
                    if (status != StatusCode::Ok) {
                        return status;
                    }
                }
                return StatusCode::Ok;
            }
            case Node::Kind::ArrayBuffer: {
                WriteUint8(node.arrayBuffer != 0 ? 1u : 0u);
                if (node.arrayBuffer == 0) {
                    return StatusCode::Ok;
                }
                if (!m_ArrayBufferModule || !m_ArrayBufferModule->Has(node.arrayBuffer)) {
                    return StatusCode::InvalidArgument;
                }
                auto length = m_ArrayBufferModule->ByteLength(node.arrayBuffer);
                WriteUint64(static_cast<std::uint64_t>(length));
                if (length > 0) {
                    m_ByteScratch.resize(length);
                    auto status = m_ArrayBufferModule->CopyOut(node.arrayBuffer, 0, m_ByteScratch.data(), length);
                    if (status != StatusCode::Ok) {
                        return status;
                    }
                    m_Serialized.insert(m_Serialized.end(), m_ByteScratch.begin(), m_ByteScratch.end());
                }
                return StatusCode::Ok;
            }
            case Node::Kind::SharedArrayBuffer: {
                WriteUint8(node.sharedBuffer != 0 ? 1u : 0u);
                if (node.sharedBuffer == 0) {
                    return StatusCode::Ok;
                }
                if (!m_SharedArrayBufferModule || !m_SharedArrayBufferModule->Has(node.sharedBuffer)) {
                    return StatusCode::InvalidArgument;
                }
                auto byteLength = m_SharedArrayBufferModule->ByteLength(node.sharedBuffer);
                auto maxByteLength = m_SharedArrayBufferModule->MaxByteLength(node.sharedBuffer);
                auto resizable = m_SharedArrayBufferModule->Resizable(node.sharedBuffer) ? 1u : 0u;
                WriteUint64(static_cast<std::uint64_t>(byteLength));
                WriteUint64(static_cast<std::uint64_t>(maxByteLength));
                WriteUint8(resizable);
                if (byteLength > 0) {
                    auto status = m_SharedArrayBufferModule->Snapshot(node.sharedBuffer, m_ByteScratch);
                    if (status != StatusCode::Ok) {
                        return status;
                    }
                    m_Serialized.insert(m_Serialized.end(), m_ByteScratch.begin(), m_ByteScratch.end());
                }
                return StatusCode::Ok;
            }
            case Node::Kind::TypedArray: {
                WriteUint8(node.typedArray.handle != 0 ? 1u : 0u);
                if (node.typedArray.handle == 0) {
                    return StatusCode::Ok;
                }
                if (!m_TypedArrayModule) {
                    return StatusCode::NotFound;
                }
                TypedArrayModule::Snapshot snapshot{};
                auto status = m_TypedArrayModule->Describe(node.typedArray.handle, snapshot);
                if (status != StatusCode::Ok) {
                    return status;
                }
                const bool isBigInt = snapshot.type == TypedArrayModule::ElementType::BigInt64 ||
                                      snapshot.type == TypedArrayModule::ElementType::BigUint64;
                WriteUint8(static_cast<std::uint8_t>(snapshot.type));
                WriteUint32(static_cast<std::uint32_t>(snapshot.length));
                WriteUint8(isBigInt ? 1u : 0u);
                if (isBigInt) {
                    std::vector<std::int64_t> values;
                    status = m_TypedArrayModule->ToBigIntVector(node.typedArray.handle, values);
                    if (status != StatusCode::Ok) {
                        return status;
                    }
                    WriteUint32(static_cast<std::uint32_t>(values.size()));
                    for (auto value: values) {
                        WriteUint64(static_cast<std::uint64_t>(value));
                    }
                } else {
                    std::vector<double> values;
                    status = m_TypedArrayModule->ToVector(node.typedArray.handle, values);
                    if (status != StatusCode::Ok) {
                        return status;
                    }
                    WriteUint32(static_cast<std::uint32_t>(values.size()));
                    for (auto value: values) {
                        WriteDouble(value);
                    }
                }
                return StatusCode::Ok;
            }
        }
        return StatusCode::InvalidArgument;
    }

    void StructuredCloneModule::WritePrimitive(const Node &node) {
        switch (node.kind) {
            case Node::Kind::Boolean:
                WriteUint8(node.primitive.AsBoolean() ? 1u : 0u);
                break;
            case Node::Kind::Number:
                WriteDouble(node.primitive.AsNumber());
                break;
            case Node::Kind::String:
                WriteString(node.primitive.AsString());
                break;
            default:
                break;
        }
    }

    void StructuredCloneModule::WriteString(std::string_view value) {
        WriteUint32(static_cast<std::uint32_t>(value.size()));
        if (!value.empty()) {
            m_Serialized.insert(m_Serialized.end(), value.begin(), value.end());
        }
    }

    void StructuredCloneModule::WriteUint8(std::uint8_t value) {
        m_Serialized.push_back(value);
    }

    void StructuredCloneModule::WriteUint32(std::uint32_t value) {
        auto offset = m_Serialized.size();
        m_Serialized.resize(offset + sizeof(std::uint32_t));
        std::memcpy(m_Serialized.data() + offset, &value, sizeof(std::uint32_t));
    }

    void StructuredCloneModule::WriteUint64(std::uint64_t value) {
        auto offset = m_Serialized.size();
        m_Serialized.resize(offset + sizeof(std::uint64_t));
        std::memcpy(m_Serialized.data() + offset, &value, sizeof(std::uint64_t));
    }

    void StructuredCloneModule::WriteDouble(double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(double));
        WriteUint64(bits);
    }

    StatusCode StructuredCloneModule::ReadNode(BinaryCursor &cursor, Node &outNode) {
        std::uint8_t kindByte = 0;
        if (!cursor.Read(kindByte)) {
            return StatusCode::InvalidArgument;
        }
        outNode = Node();
        outNode.kind = static_cast<Node::Kind>(kindByte);
        if (!cursor.Read(outNode.label)) {
            return StatusCode::InvalidArgument;
        }
        std::uint8_t transferFlag = 0;
        if (!cursor.Read(transferFlag)) {
            return StatusCode::InvalidArgument;
        }
        outNode.transfer = transferFlag != 0;

        switch (outNode.kind) {
            case Node::Kind::Undefined:
                return StatusCode::Ok;
            case Node::Kind::Boolean: {
                std::uint8_t booleanByte = 0;
                if (!cursor.Read(booleanByte)) {
                    return StatusCode::InvalidArgument;
                }
                outNode.primitive = Value::Boolean(booleanByte != 0);
                return StatusCode::Ok;
            }
            case Node::Kind::Number: {
                double numberValue = 0.0;
                if (!cursor.Read(numberValue)) {
                    return StatusCode::InvalidArgument;
                }
                outNode.primitive = Value::Number(numberValue);
                return StatusCode::Ok;
            }
            case Node::Kind::String: {
                std::string text;
                if (!cursor.Read(text)) {
                    return StatusCode::InvalidArgument;
                }
                outNode.primitive = Value(text);
                return StatusCode::Ok;
            }
            case Node::Kind::Array:
                return DeserializeArray(cursor, outNode);
            case Node::Kind::Object:
                return DeserializeObject(cursor, outNode);
            case Node::Kind::Map:
                return DeserializeMap(cursor, outNode);
            case Node::Kind::Set:
                return DeserializeSet(cursor, outNode);
            case Node::Kind::ArrayBuffer:
                return DeserializeArrayBuffer(cursor, outNode);
            case Node::Kind::SharedArrayBuffer:
                return DeserializeSharedArrayBuffer(cursor, outNode);
            case Node::Kind::TypedArray:
                return DeserializeTypedArray(cursor, outNode);
        }
        return StatusCode::InvalidArgument;
    }

    StatusCode StructuredCloneModule::DeserializeArray(BinaryCursor &cursor, Node &outNode) {
        std::uint32_t count = 0;
        if (!cursor.Read(count)) {
            return StatusCode::InvalidArgument;
        }
        outNode.arrayItems.clear();
        outNode.arrayItems.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            auto status = ReadNode(cursor, outNode.arrayItems[i]);
            if (status != StatusCode::Ok) {
                return status;
            }
        }
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::DeserializeObject(BinaryCursor &cursor, Node &outNode) {
        std::uint32_t count = 0;
        if (!cursor.Read(count)) {
            return StatusCode::InvalidArgument;
        }
        outNode.objectProperties.clear();
        outNode.objectProperties.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::string key;
            if (!cursor.Read(key)) {
                return StatusCode::InvalidArgument;
            }
            Node value;
            auto status = ReadNode(cursor, value);
            if (status != StatusCode::Ok) {
                return status;
            }
            outNode.objectProperties.emplace_back(std::move(key), std::move(value));
        }
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::DeserializeMap(BinaryCursor &cursor, Node &outNode) {
        std::uint32_t count = 0;
        if (!cursor.Read(count)) {
            return StatusCode::InvalidArgument;
        }
        outNode.mapEntries.clear();
        outNode.mapEntries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            Node key;
            Node value;
            auto status = ReadNode(cursor, key);
            if (status != StatusCode::Ok) {
                return status;
            }
            status = ReadNode(cursor, value);
            if (status != StatusCode::Ok) {
                return status;
            }
            outNode.mapEntries.emplace_back(std::move(key), std::move(value));
        }
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::DeserializeSet(BinaryCursor &cursor, Node &outNode) {
        std::uint32_t count = 0;
        if (!cursor.Read(count)) {
            return StatusCode::InvalidArgument;
        }
        outNode.setEntries.clear();
        outNode.setEntries.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            auto status = ReadNode(cursor, outNode.setEntries[i]);
            if (status != StatusCode::Ok) {
                return status;
            }
        }
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::DeserializeArrayBuffer(BinaryCursor &cursor, Node &outNode) {
        std::uint8_t hasHandle = 0;
        if (!cursor.Read(hasHandle)) {
            return StatusCode::InvalidArgument;
        }
        if (!hasHandle) {
            outNode.arrayBuffer = 0;
            return StatusCode::Ok;
        }
        std::uint64_t length64 = 0;
        if (!cursor.Read(length64)) {
            return StatusCode::InvalidArgument;
        }
        std::size_t length = SafeCastToSize(length64);
        ArrayBufferModule::Handle handle = 0;
        if (!m_ArrayBufferModule) {
            return StatusCode::NotFound;
        }
        auto status = m_ArrayBufferModule->Create(outNode.label.empty() ? kArrayBufferCloneLabel : outNode.label,
                                                  length,
                                                  handle);
        if (status != StatusCode::Ok) {
            return status;
        }
        if (length > 0) {
            if (!cursor.ReadBytes(m_ByteScratch, length)) {
                m_ArrayBufferModule->Destroy(handle);
                return StatusCode::InvalidArgument;
            }
            status = m_ArrayBufferModule->CopyIn(handle, 0, m_ByteScratch.data(), length);
            if (status != StatusCode::Ok) {
                m_ArrayBufferModule->Destroy(handle);
                return status;
            }
        }
        outNode.arrayBuffer = handle;
        m_Metrics.bufferCopies += 1;
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::DeserializeSharedArrayBuffer(BinaryCursor &cursor, Node &outNode) {
        std::uint8_t hasHandle = 0;
        if (!cursor.Read(hasHandle)) {
            return StatusCode::InvalidArgument;
        }
        if (!hasHandle) {
            outNode.sharedBuffer = 0;
            return StatusCode::Ok;
        }
        std::uint64_t length64 = 0;
        std::uint64_t maxLength64 = 0;
        if (!cursor.Read(length64) || !cursor.Read(maxLength64)) {
            return StatusCode::InvalidArgument;
        }
        std::uint8_t resizableFlag = 0;
        if (!cursor.Read(resizableFlag)) {
            return StatusCode::InvalidArgument;
        }
        std::size_t length = SafeCastToSize(length64);
        std::size_t maxLength = SafeCastToSize(maxLength64);

        if (!cursor.ReadBytes(m_ByteScratch, length)) {
            return StatusCode::InvalidArgument;
        }

        if (!m_SharedArrayBufferModule) {
            return StatusCode::NotFound;
        }
        SharedArrayBufferModule::Handle handle = 0;
        StatusCode status;
        if (resizableFlag != 0) {
            status = m_SharedArrayBufferModule->CreateResizable(
                outNode.label.empty() ? kSharedBufferCloneLabel : outNode.label,
                length,
                maxLength,
                handle);
        } else {
            status = m_SharedArrayBufferModule->Create(outNode.label.empty() ? kSharedBufferCloneLabel : outNode.label,
                                                       length,
                                                       handle);
        }
        if (status != StatusCode::Ok) {
            return status;
        }
        if (length > 0) {
            status = m_SharedArrayBufferModule->CopyIn(handle, 0, m_ByteScratch.data(), length);
            if (status != StatusCode::Ok) {
                m_SharedArrayBufferModule->Destroy(handle);
                return status;
            }
        }
        outNode.sharedBuffer = handle;
        m_Metrics.bufferCopies += 1;
        return StatusCode::Ok;
    }

    StatusCode StructuredCloneModule::DeserializeTypedArray(BinaryCursor &cursor, Node &outNode) {
        std::uint8_t hasHandle = 0;
        if (!cursor.Read(hasHandle)) {
            return StatusCode::InvalidArgument;
        }
        if (!hasHandle) {
            outNode.typedArray.handle = 0;
            return StatusCode::Ok;
        }
        std::uint8_t elementTypeByte = 0;
        if (!cursor.Read(elementTypeByte)) {
            return StatusCode::InvalidArgument;
        }
        auto elementType = static_cast<TypedArrayModule::ElementType>(elementTypeByte);
        std::uint32_t length32 = 0;
        if (!cursor.Read(length32)) {
            return StatusCode::InvalidArgument;
        }
        std::uint8_t bigIntFlag = 0;
        if (!cursor.Read(bigIntFlag)) {
            return StatusCode::InvalidArgument;
        }
        std::uint32_t valueCount = 0;
        if (!cursor.Read(valueCount)) {
            return StatusCode::InvalidArgument;
        }

        if (!m_TypedArrayModule) {
            return StatusCode::NotFound;
        }
        TypedArrayModule::Handle handle = 0;
        auto status = m_TypedArrayModule->Create(elementType,
                                                 length32,
                                                 outNode.label.empty() ? kTypedArrayCloneLabel : outNode.label,
                                                 handle);
        if (status != StatusCode::Ok) {
            return status;
        }

        const bool isBigInt = bigIntFlag != 0;
        if (isBigInt) {
            for (std::uint32_t i = 0; i < valueCount && i < length32; ++i) {
                std::uint64_t raw = 0;
                if (!cursor.Read(raw)) {
                    m_TypedArrayModule->Destroy(handle);
                    return StatusCode::InvalidArgument;
                }
                status = m_TypedArrayModule->SetBigInt(handle, i, static_cast<std::int64_t>(raw));
                if (status != StatusCode::Ok) {
                    m_TypedArrayModule->Destroy(handle);
                    return status;
                }
            }
        } else {
            for (std::uint32_t i = 0; i < valueCount && i < length32; ++i) {
                double value = 0.0;
                if (!cursor.Read(value)) {
                    m_TypedArrayModule->Destroy(handle);
                    return StatusCode::InvalidArgument;
                }
                status = m_TypedArrayModule->Set(handle, i, value, false);
                if (status != StatusCode::Ok) {
                    m_TypedArrayModule->Destroy(handle);
                    return status;
                }
            }
        }

        outNode.typedArray.handle = handle;
        outNode.typedArray.elementType = elementType;
        outNode.typedArray.length = length32;
        outNode.typedArray.byteOffset = 0;
        outNode.typedArray.copyBuffer = true;
        outNode.typedArray.label = outNode.label;
        m_Metrics.typedArrayCopies += 1;
        return StatusCode::Ok;
    }

    bool StructuredCloneModule::EnsureDependencies() {
        if (m_ArrayBufferModule && m_SharedArrayBufferModule && m_TypedArrayModule) {
            return true;
        }
        if (!m_Runtime) {
            return false;
        }
        auto &environment = m_Runtime->EsEnvironment();
        if (!m_ArrayBufferModule) {
            auto *module = environment.FindModule("ArrayBuffer");
            m_ArrayBufferModule = dynamic_cast<ArrayBufferModule *>(module);
        }
        if (!m_SharedArrayBufferModule) {
            auto *module = environment.FindModule("SharedArrayBuffer");
            m_SharedArrayBufferModule = dynamic_cast<SharedArrayBufferModule *>(module);
        }
        if (!m_TypedArrayModule) {
            auto *module = environment.FindModule("TypedArray");
            m_TypedArrayModule = dynamic_cast<TypedArrayModule *>(module);
        }
        return m_ArrayBufferModule != nullptr && m_SharedArrayBufferModule != nullptr && m_TypedArrayModule != nullptr;
    }
}
