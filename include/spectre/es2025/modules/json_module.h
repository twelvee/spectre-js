#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "spectre/config.h"
#include "spectre/status.h"
#include "spectre/es2025/module.h"

namespace spectre::es2025 {
    class JsonModule final : public Module {
    public:
        struct StringRef {
            std::uint32_t offset;
            std::uint32_t length;

            constexpr StringRef() noexcept : offset(0), length(0) {}
            constexpr bool Empty() const noexcept { return length == 0; }
        };

        enum class NodeKind : std::uint8_t {
            Null,
            Boolean,
            Number,
            String,
            Array,
            Object
        };

        struct NodeSpan {
            std::uint32_t offset;
            std::uint32_t count;

            constexpr NodeSpan() noexcept : offset(0), count(0) {}
            constexpr NodeSpan(std::uint32_t offsetValue, std::uint32_t countValue) noexcept
                : offset(offsetValue), count(countValue) {}
        };

        struct Node {
            NodeKind kind;
            bool boolValue;
            double numberValue;
            StringRef stringRef;
            NodeSpan span;

            constexpr Node() noexcept
                : kind(NodeKind::Null),
                  boolValue(false),
                  numberValue(0.0),
                  stringRef(),
                  span() {}

            static Node MakeNull() noexcept;
            static Node MakeBoolean(bool value) noexcept;
            static Node MakeNumber(double value) noexcept;
            static Node MakeString(StringRef ref) noexcept;
            static Node MakeArray(NodeSpan span) noexcept;
            static Node MakeObject(NodeSpan span) noexcept;
        };

        struct Property {
            StringRef key;
            std::uint32_t valueIndex;

            constexpr Property() noexcept : key(), valueIndex(0) {}
            constexpr Property(StringRef keyRef, std::uint32_t index) noexcept : key(keyRef), valueIndex(index) {}
        };

        struct Document {
            static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

            std::vector<Node> nodes;
            std::vector<std::uint32_t> elements;
            std::vector<Property> properties;
            std::string stringArena;
            std::uint32_t root;
            std::uint64_t version;

            Document() noexcept;

            void Reset() noexcept;
            std::string_view GetString(const StringRef &ref) const noexcept;
            bool Empty() const noexcept;
        };

        struct ParseOptions {
            bool allowComments;
            bool allowTrailingCommas;
            std::size_t maxDepth;
            std::size_t maxNodes;

            ParseOptions() noexcept;
        };

        struct StringifyOptions {
            bool pretty;
            bool asciiOnly;
            bool trailingNewline;
            std::uint8_t indentWidth;

            StringifyOptions() noexcept;
        };

        struct Metrics {
            std::uint64_t parseCalls;
            std::uint64_t stringifyCalls;
            std::uint64_t parsedNodes;
            std::uint64_t stringifiedNodes;
            std::uint64_t reusedDocuments;
            std::uint64_t peakStringArena;
            std::uint64_t peakNodeCount;
            std::uint64_t failures;
            std::uint64_t lastParseFrame;
            bool gpuOptimized;

            Metrics() noexcept;
        };

        JsonModule();

        std::string_view Name() const noexcept override;
        std::string_view Summary() const noexcept override;
        std::string_view SpecificationReference() const noexcept override;

        void Initialize(const ModuleInitContext &context) override;
        void Tick(const TickInfo &info, const ModuleTickContext &context) noexcept override;
        void OptimizeGpu(const ModuleGpuContext &context) noexcept override;
        void Reconfigure(const RuntimeConfig &config) override;

        StatusCode Parse(std::string_view json,
                         Document &outDocument,
                         std::string &outDiagnostics,
                         const ParseOptions *options = nullptr);

        StatusCode Stringify(const Document &document,
                             std::string &outJson,
                             const StringifyOptions *options = nullptr) const;

        const Metrics &GetMetrics() const noexcept;
        bool GpuEnabled() const noexcept;

    private:
        struct Parser;

        SpectreRuntime *m_Runtime;
        detail::SubsystemSuite *m_Subsystems;
        RuntimeConfig m_Config;
        ParseOptions m_DefaultParseOptions;
        StringifyOptions m_DefaultStringifyOptions;
        bool m_GpuEnabled;
        bool m_Initialized;
        std::uint64_t m_CurrentFrame;
        mutable Metrics m_Metrics;

        void ApplyConfig(const RuntimeConfig &config);
        static void AppendIndent(std::string &out, std::uint32_t depth, std::uint8_t indentWidth);
        static void AppendEscapedString(std::string_view text, bool asciiOnly, std::string &out);
        static void AppendCodepointUtf8(std::uint32_t codepoint, std::string &target);
        static void AppendUnicodeEscape(std::uint32_t codepoint, std::string &out);
        static void AppendNumber(double value, std::string &out);
        StatusCode WriteNode(const Document &document,
                             std::uint32_t nodeIndex,
                             std::string &outJson,
                             const StringifyOptions &options,
                             std::uint32_t depth) const;
    };
}



