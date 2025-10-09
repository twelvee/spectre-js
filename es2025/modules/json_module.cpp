#include "spectre/es2025/modules/json_module.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <system_error>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "JSON";
        constexpr std::string_view kSummary = "JSON parsing, stringification, and structured data utilities.";
        constexpr std::string_view kReference = "ECMA-262 Section 30";
        constexpr std::size_t kDefaultMaxDepth = 1024;
        constexpr std::size_t kDefaultMaxNodes = 262144;
        constexpr std::size_t kAbsoluteMaxNodes = static_cast<std::size_t>(
            std::numeric_limits<std::uint32_t>::max() - 1);
        constexpr std::size_t kStringLimit = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
        constexpr std::uint8_t kDefaultIndent = 2;
        constexpr std::uint8_t kMaxIndent = 10;

        bool ParseUnsigned(std::string_view text, std::size_t &outValue) {
            if (text.empty()) {
                return false;
            }
            unsigned long long value = 0;
            auto begin = text.data();
            auto end = begin + text.size();
            auto result = std::from_chars(begin, end, value);
            if (result.ec != std::errc() || result.ptr != end) {
                return false;
            }
            outValue = static_cast<std::size_t>(value);
            return true;
        }
    }

    JsonModule::Node JsonModule::Node::MakeNull() noexcept {
        return Node();
    }

    JsonModule::Node JsonModule::Node::MakeBoolean(bool value) noexcept {
        Node node;
        node.kind = NodeKind::Boolean;
        node.boolValue = value;
        return node;
    }

    JsonModule::Node JsonModule::Node::MakeNumber(double value) noexcept {
        Node node;
        node.kind = NodeKind::Number;
        node.numberValue = value;
        return node;
    }

    JsonModule::Node JsonModule::Node::MakeString(StringRef ref) noexcept {
        Node node;
        node.kind = NodeKind::String;
        node.stringRef = ref;
        return node;
    }

    JsonModule::Node JsonModule::Node::MakeArray(NodeSpan span) noexcept {
        Node node;
        node.kind = NodeKind::Array;
        node.span = span;
        return node;
    }

    JsonModule::Node JsonModule::Node::MakeObject(NodeSpan span) noexcept {
        Node node;
        node.kind = NodeKind::Object;
        node.span = span;
        return node;
    }

    JsonModule::Document::Document() noexcept
        : nodes(),
          elements(),
          properties(),
          stringArena(),
          root(kInvalidIndex),
          version(0) {
    }

    void JsonModule::Document::Reset() noexcept {
        nodes.clear();
        elements.clear();
        properties.clear();
        stringArena.clear();
        root = kInvalidIndex;
        version += 1;
    }

    std::string_view JsonModule::Document::GetString(const StringRef &ref) const noexcept {
        if (ref.length == 0 || ref.offset >= stringArena.size()) {
            return {};
        }
        auto begin = static_cast<std::size_t>(ref.offset);
        auto end = std::min(stringArena.size(), begin + static_cast<std::size_t>(ref.length));
        if (end <= begin) {
            return {};
        }
        return std::string_view(stringArena.data() + begin, end - begin);
    }

    bool JsonModule::Document::Empty() const noexcept {
        return root == kInvalidIndex;
    }

    JsonModule::ParseOptions::ParseOptions() noexcept
        : allowComments(false),
          allowTrailingCommas(false),
          maxDepth(kDefaultMaxDepth),
          maxNodes(kDefaultMaxNodes) {
    }

    JsonModule::StringifyOptions::StringifyOptions() noexcept
        : pretty(false),
          asciiOnly(false),
          trailingNewline(false),
          indentWidth{kDefaultIndent} {
    }

    JsonModule::Metrics::Metrics() noexcept
        : parseCalls(0),
          stringifyCalls(0),
          parsedNodes(0),
          stringifiedNodes(0),
          reusedDocuments(0),
          peakStringArena(0),
          peakNodeCount(0),
          failures(0),
          lastParseFrame(0),
          gpuOptimized(false) {
    }

    JsonModule::JsonModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_DefaultParseOptions(),
          m_DefaultStringifyOptions(),
          m_GpuEnabled(false),
          m_Initialized(false),
          m_CurrentFrame(0),
          m_Metrics() {
    }

    std::string_view JsonModule::Name() const noexcept {
        return kName;
    }

    std::string_view JsonModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view JsonModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void JsonModule::ApplyConfig(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        m_DefaultParseOptions = ParseOptions();
        m_DefaultStringifyOptions = StringifyOptions();

        if (config.memory.heapBytes > 0) {
            auto estimated = std::max<std::size_t>(kDefaultMaxNodes, config.memory.heapBytes / 16);
            m_DefaultParseOptions.maxNodes = std::min<std::size_t>(estimated, kAbsoluteMaxNodes);
        }

        for (const auto &flag: config.featureFlags) {
            std::string_view view(flag);
            if (view == "json.allowComments") {
                m_DefaultParseOptions.allowComments = true;
            } else if (view == "json.allowTrailingCommas") {
                m_DefaultParseOptions.allowTrailingCommas = true;
            } else if (view == "json.stringify.pretty") {
                m_DefaultStringifyOptions.pretty = true;
            } else if (view == "json.stringify.asciiOnly") {
                m_DefaultStringifyOptions.asciiOnly = true;
            } else if (view == "json.stringify.trailingNewline") {
                m_DefaultStringifyOptions.trailingNewline = true;
            } else if (view.rfind("json.maxDepth=", 0) == 0) {
                std::size_t depth = 0;
                if (ParseUnsigned(view.substr(13), depth) && depth > 0) {
                    m_DefaultParseOptions.maxDepth = depth;
                }
            } else if (view.rfind("json.maxNodes=", 0) == 0) {
                std::size_t nodes = 0;
                if (ParseUnsigned(view.substr(13), nodes) && nodes > 0) {
                    m_DefaultParseOptions.maxNodes = std::min(nodes, kAbsoluteMaxNodes);
                }
            } else if (view.rfind("json.stringify.indent=", 0) == 0) {
                std::size_t indent = 0;
                if (ParseUnsigned(view.substr(22), indent)) {
                    auto bounded = static_cast<std::uint8_t>(std::min<std::size_t>(indent, kMaxIndent));
                    m_DefaultStringifyOptions.indentWidth = bounded;
                    if (bounded > 0) {
                        m_DefaultStringifyOptions.pretty = true;
                    }
                }
            }
        }

        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void JsonModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        ApplyConfig(context.config);
        m_Initialized = true;
        m_Metrics = Metrics();
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void JsonModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
    }

    void JsonModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        m_Metrics.gpuOptimized = m_GpuEnabled;
    }

    void JsonModule::Reconfigure(const RuntimeConfig &config) {
        ApplyConfig(config);
    }

    class JsonModule::Parser {
    public:
        Parser(std::string_view json,
               Document &document,
               const ParseOptions &options,
               std::string &diagnostics)
            : m_Current(json.data()),
              m_End(json.data() + json.size()),
              m_Document(document),
              m_Options(options),
              m_Diagnostics(diagnostics),
              m_Line(1),
              m_Column(1),
              m_Status(StatusCode::Ok) {
            m_Diagnostics.clear();
            m_Document.root = Document::kInvalidIndex;
        }

        StatusCode Run() {
            SkipWhitespaceAndComments();
            if (AtEnd()) {
                Fail("JSON payload is empty");
                return m_Status;
            }
            std::uint32_t rootIndex = Document::kInvalidIndex;
            if (!ParseValue(rootIndex, 1)) {
                return m_Status;
            }
            SkipWhitespaceAndComments();
            if (!AtEnd()) {
                Fail("Trailing content after JSON value");
                return m_Status;
            }
            m_Document.root = rootIndex;
            return m_Status;
        }

    private:
        const char *m_Current;
        const char *m_End;
        Document &m_Document;
        const ParseOptions &m_Options;
        std::string &m_Diagnostics;
        std::uint64_t m_Line;
        std::uint64_t m_Column;
        StatusCode m_Status;

        bool AtEnd() const {
            return m_Current >= m_End;
        }

        char Peek() const {
            return AtEnd() ? '\0' : *m_Current;
        }

        char Advance() {
            char value = *m_Current++;
            if (value == '\n') {
                ++m_Line;
                m_Column = 1;
            } else {
                ++m_Column;
            }
            return value;
        }

        void SkipWhitespaceAndComments() {
            while (!AtEnd()) {
                char c = Peek();
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    Advance();
                    continue;
                }
                if (!m_Options.allowComments || c != '/') {
                    break;
                }
                if ((m_Current + 1) >= m_End) {
                    break;
                }
                char next = *(m_Current + 1);
                if (next == '/') {
                    Advance();
                    Advance();
                    while (!AtEnd()) {
                        char lineChar = Advance();
                        if (lineChar == '\n') {
                            break;
                        }
                    }
                    continue;
                }
                if (next == '*') {
                    Advance();
                    Advance();
                    bool closed = false;
                    while (!AtEnd()) {
                        char ch = Advance();
                        if (ch == '*' && !AtEnd() && Peek() == '/') {
                            Advance();
                            closed = true;
                            break;
                        }
                    }
                    if (!closed) {
                        Fail("Unterminated block comment");
                        return;
                    }
                    continue;
                }
                break;
            }
        }

        bool ParseValue(std::uint32_t &outIndex, std::size_t depth) {
            if (depth > m_Options.maxDepth) {
                return Fail("Maximum JSON nesting depth exceeded");
            }
            SkipWhitespaceAndComments();
            if (AtEnd()) {
                return Fail("Unexpected end of JSON input");
            }
            char c = Peek();
            if (c == '"') {
                return ParseStringNode(outIndex);
            }
            if (c == '{') {
                return ParseObject(outIndex, depth + 1);
            }
            if (c == '[') {
                return ParseArray(outIndex, depth + 1);
            }
            if (c == 't') {
                return ParseTrue(outIndex);
            }
            if (c == 'f') {
                return ParseFalse(outIndex);
            }
            if (c == 'n') {
                return ParseNull(outIndex);
            }
            if (c == '-' || (c >= '0' && c <= '9')) {
                return ParseNumber(outIndex);
            }
            return Fail("Invalid JSON value");
        }

        bool ParseNull(std::uint32_t &outIndex) {
            if (!ExpectLiteral("null")) {
                return false;
            }
            outIndex = AppendNode(Node::MakeNull());
            return m_Status == StatusCode::Ok;
        }

        bool ParseTrue(std::uint32_t &outIndex) {
            if (!ExpectLiteral("true")) {
                return false;
            }
            outIndex = AppendNode(Node::MakeBoolean(true));
            return m_Status == StatusCode::Ok;
        }

        bool ParseFalse(std::uint32_t &outIndex) {
            if (!ExpectLiteral("false")) {
                return false;
            }
            outIndex = AppendNode(Node::MakeBoolean(false));
            return m_Status == StatusCode::Ok;
        }

        bool ParseNumber(std::uint32_t &outIndex) {
            const char *start = m_Current;
            const char *p = m_Current;
            if (p < m_End && *p == '-') {
                ++p;
            }
            if (p >= m_End) {
                return Fail("Incomplete number literal");
            }
            if (*p == '0') {
                ++p;
                if (p < m_End && std::isdigit(static_cast<unsigned char>(*p))) {
                    return Fail("Leading zeros are not allowed");
                }
            } else if (*p >= '1' && *p <= '9') {
                while (p < m_End && std::isdigit(static_cast<unsigned char>(*p))) {
                    ++p;
                }
            } else {
                return Fail("Invalid number literal");
            }
            if (p < m_End && *p == '.') {
                ++p;
                if (p >= m_End || !std::isdigit(static_cast<unsigned char>(*p))) {
                    return Fail("Missing fractional digits in number literal");
                }
                while (p < m_End && std::isdigit(static_cast<unsigned char>(*p))) {
                    ++p;
                }
            }
            if (p < m_End && (*p == 'e' || *p == 'E')) {
                ++p;
                if (p < m_End && (*p == '+' || *p == '-')) {
                    ++p;
                }
                if (p >= m_End || !std::isdigit(static_cast<unsigned char>(*p))) {
                    return Fail("Missing exponent digits in number literal");
                }
                while (p < m_End && std::isdigit(static_cast<unsigned char>(*p))) {
                    ++p;
                }
            }
            double value = 0.0;
            auto result = std::from_chars(start, p, value, std::chars_format::general);
            if (result.ec != std::errc()) {
                std::array<char, 128> buffer{};
                auto length = static_cast<std::size_t>(p - start);
                if (length >= buffer.size()) {
                    return Fail("Number literal too long");
                }
                std::memcpy(buffer.data(), start, length);
                buffer[length] = '\0';
                value = std::strtod(buffer.data(), nullptr);
            }
            auto consumed = static_cast<std::size_t>(p - m_Current);
            m_Current = p;
            m_Column += consumed;
            outIndex = AppendNode(Node::MakeNumber(value));
            return m_Status == StatusCode::Ok;
        }

        bool ParseStringNode(std::uint32_t &outIndex) {
            StringRef ref;
            if (!ParseStringLiteral(ref)) {
                return false;
            }
            outIndex = AppendNode(Node::MakeString(ref));
            return m_Status == StatusCode::Ok;
        }

        bool ParseStringLiteral(StringRef &outRef) {
            if (Peek() != '"') {
                return Fail("Expected string literal");
            }
            Advance();
            auto startOffset = m_Document.stringArena.size();
            if (startOffset >= kStringLimit) {
                FailCapacity("String storage exhausted");
                return false;
            }
            while (!AtEnd()) {
                char c = Advance();
                if (c == '"') {
                    auto endOffset = m_Document.stringArena.size();
                    auto length = endOffset - startOffset;
                    if (length > kStringLimit) {
                        FailCapacity("String literal too large");
                        return false;
                    }
                    outRef.offset = static_cast<std::uint32_t>(startOffset);
                    outRef.length = static_cast<std::uint32_t>(length);
                    return true;
                }
                if (static_cast<unsigned char>(c) <= 0x1F) {
                    return Fail("Unescaped control character in string literal");
                }
                if (c == '\\') {
                    if (AtEnd()) {
                        return Fail("Unterminated escape sequence");
                    }
                    char esc = Advance();
                    switch (esc) {
                        case '"':
                            m_Document.stringArena.push_back('"');
                            break;
                        case '\\':
                            m_Document.stringArena.push_back('\\');
                            break;
                        case '/':
                            m_Document.stringArena.push_back('/');
                            break;
                        case 'b':
                            m_Document.stringArena.push_back('\b');
                            break;
                        case 'f':
                            m_Document.stringArena.push_back('\f');
                            break;
                        case 'n':
                            m_Document.stringArena.push_back('\n');
                            break;
                        case 'r':
                            m_Document.stringArena.push_back('\r');
                            break;
                        case 't':
                            m_Document.stringArena.push_back('\t');
                            break;
                        case 'u': {
                            std::uint32_t codepoint = 0;
                            if (!ParseUnicodeEscape(codepoint)) {
                                return false;
                            }
                            AppendCodepointUtf8(codepoint, m_Document.stringArena);
                            break;
                        }
                        default:
                            return Fail("Unknown escape sequence in string literal");
                    }
                } else {
                    m_Document.stringArena.push_back(c);
                }
                if (m_Document.stringArena.size() >= kStringLimit) {
                    FailCapacity("String storage exhausted");
                    return false;
                }
            }
            return Fail("Unterminated string literal");
        }

        bool ParseArray(std::uint32_t &outIndex, std::size_t depth) {
            Advance();
            SkipWhitespaceAndComments();
            auto start = m_Document.elements.size();
            if (start >= kAbsoluteMaxNodes) {
                FailCapacity("Array element budget exhausted");
                return false;
            }
            std::uint32_t count = 0;
            if (Peek() == ']') {
                Advance();
                outIndex = AppendNode(Node::MakeArray(NodeSpan{static_cast<std::uint32_t>(start), 0}));
                return m_Status == StatusCode::Ok;
            }
            while (true) {
                std::uint32_t elementIndex = 0;
                if (!ParseValue(elementIndex, depth)) {
                    return false;
                }
                if (m_Document.elements.size() >= kAbsoluteMaxNodes) {
                    FailCapacity("Array element budget exhausted");
                    return false;
                }
                m_Document.elements.push_back(elementIndex);
                ++count;
                SkipWhitespaceAndComments();
                if (AtEnd()) {
                    return Fail("Unterminated array literal");
                }
                char c = Peek();
                if (c == ',') {
                    Advance();
                    SkipWhitespaceAndComments();
                    if (!AtEnd() && Peek() == ']') {
                        if (!m_Options.allowTrailingCommas) {
                            return Fail("Trailing comma in array literal");
                        }
                        Advance();
                        break;
                    }
                    continue;
                }
                if (c == ']') {
                    Advance();
                    break;
                }
                return Fail("Expected ',' or ']' inside array literal");
            }
            outIndex = AppendNode(Node::MakeArray(NodeSpan{static_cast<std::uint32_t>(start), count}));
            return m_Status == StatusCode::Ok;
        }

        bool ParseObject(std::uint32_t &outIndex, std::size_t depth) {
            Advance();
            SkipWhitespaceAndComments();
            auto start = m_Document.properties.size();
            if (start >= kAbsoluteMaxNodes) {
                FailCapacity("Object property budget exhausted");
                return false;
            }
            std::uint32_t count = 0;
            if (Peek() == '}') {
                Advance();
                outIndex = AppendNode(Node::MakeObject(NodeSpan{static_cast<std::uint32_t>(start), 0}));
                return m_Status == StatusCode::Ok;
            }
            while (true) {
                SkipWhitespaceAndComments();
                if (AtEnd()) {
                    return Fail("Unterminated object literal");
                }
                if (Peek() != '"') {
                    return Fail("Object keys must be strings");
                }
                StringRef key;
                if (!ParseStringLiteral(key)) {
                    return false;
                }
                SkipWhitespaceAndComments();
                if (!ExpectChar(':')) {
                    return false;
                }
                SkipWhitespaceAndComments();
                std::uint32_t valueIndex = 0;
                if (!ParseValue(valueIndex, depth)) {
                    return false;
                }
                if (m_Document.properties.size() >= kAbsoluteMaxNodes) {
                    FailCapacity("Object property budget exhausted");
                    return false;
                }
                m_Document.properties.emplace_back(key, valueIndex);
                ++count;
                SkipWhitespaceAndComments();
                if (AtEnd()) {
                    return Fail("Unterminated object literal");
                }
                char c = Peek();
                if (c == ',') {
                    Advance();
                    SkipWhitespaceAndComments();
                    if (!AtEnd() && Peek() == '}') {
                        if (!m_Options.allowTrailingCommas) {
                            return Fail("Trailing comma in object literal");
                        }
                        Advance();
                        break;
                    }
                    continue;
                }
                if (c == '}') {
                    Advance();
                    break;
                }
                return Fail("Expected ',' or '}' inside object literal");
            }
            outIndex = AppendNode(Node::MakeObject(NodeSpan{static_cast<std::uint32_t>(start), count}));
            return m_Status == StatusCode::Ok;
        }

        bool ExpectLiteral(const char *literal) {
            const char *ptr = literal;
            while (*ptr != '\0') {
                if (AtEnd() || Peek() != *ptr) {
                    return Fail("Malformed literal");
                }
                Advance();
                ++ptr;
            }
            return true;
        }

        bool ExpectChar(char expected) {
            if (AtEnd() || Peek() != expected) {
                std::string message = "Expected '";
                message.push_back(expected);
                message.push_back('\'');
                return Fail(message);
            }
            Advance();
            return true;
        }

        std::uint32_t AppendNode(const Node &node) {
            if (m_Document.nodes.size() >= std::min(m_Options.maxNodes, kAbsoluteMaxNodes)) {
                FailCapacity("JSON node budget exhausted");
                return Document::kInvalidIndex;
            }
            m_Document.nodes.push_back(node);
            return static_cast<std::uint32_t>(m_Document.nodes.size() - 1);
        }

        static std::uint16_t ParseHexValue(char ch) {
            if (ch >= '0' && ch <= '9') {
                return static_cast<std::uint16_t>(ch - '0');
            }
            if (ch >= 'a' && ch <= 'f') {
                return static_cast<std::uint16_t>(10 + (ch - 'a'));
            }
            if (ch >= 'A' && ch <= 'F') {
                return static_cast<std::uint16_t>(10 + (ch - 'A'));
            }
            return std::numeric_limits<std::uint16_t>::max();
        }

        std::uint16_t ParseHexQuad() {
            std::uint16_t value = 0;
            for (int i = 0; i < 4; ++i) {
                if (AtEnd()) {
                    Fail("Incomplete unicode escape sequence");
                    return 0;
                }
                char ch = Advance();
                auto digit = ParseHexValue(ch);
                if (digit == std::numeric_limits<std::uint16_t>::max()) {
                    Fail("Invalid hexadecimal digit in unicode escape");
                    return 0;
                }
                value = static_cast<std::uint16_t>((value << 4) | digit);
            }
            return value;
        }

        bool ParseUnicodeEscape(std::uint32_t &outCodepoint) {
            auto high = ParseHexQuad();
            if (m_Status != StatusCode::Ok) {
                return false;
            }
            if (high >= 0xD800 && high <= 0xDBFF) {
                if (AtEnd() || Peek() != '\\') {
                    return Fail("Missing low surrogate in unicode escape");
                }
                Advance();
                if (!ExpectChar('u')) {
                    return false;
                }
                auto low = ParseHexQuad();
                if (m_Status != StatusCode::Ok) {
                    return false;
                }
                if (low < 0xDC00 || low > 0xDFFF) {
                    return Fail("Invalid low surrogate in unicode escape");
                }
                outCodepoint = 0x10000 + (((static_cast<std::uint32_t>(high) - 0xD800) << 10) |
                                          (static_cast<std::uint32_t>(low) - 0xDC00));
                return true;
            }
            if (high >= 0xDC00 && high <= 0xDFFF) {
                return Fail("Unexpected low surrogate in unicode escape");
            }
            outCodepoint = high;
            return true;
        }

        void CaptureError(const char *message, StatusCode code) {
            if (m_Status != StatusCode::Ok) {
                return;
            }
            m_Status = code;
            m_Diagnostics.assign(message);
            m_Diagnostics.append(" at line ");
            m_Diagnostics.append(std::to_string(m_Line));
            m_Diagnostics.append(", column ");
            m_Diagnostics.append(std::to_string(m_Column));
        }

        bool Fail(const char *message) {
            CaptureError(message, StatusCode::InvalidArgument);
            return false;
        }

        bool Fail(const std::string &message) {
            return Fail(message.c_str());
        }

        void FailCapacity(const char *message) {
            if (m_Status == StatusCode::Ok) {
                CaptureError(message, StatusCode::CapacityExceeded);
            }
        }
    };

    StatusCode JsonModule::Parse(std::string_view json,
                                 Document &outDocument,
                                 std::string &outDiagnostics,
                                 const ParseOptions *options) {
        if (!m_Initialized) {
            outDiagnostics = "JsonModule is not initialized";
            m_Metrics.failures += 1;
            return StatusCode::InternalError;
        }
        m_Metrics.parseCalls += 1;
        const bool hadHistory = !outDocument.nodes.empty() ||
                                !outDocument.elements.empty() ||
                                !outDocument.properties.empty() ||
                                !outDocument.stringArena.empty();
        outDocument.Reset();
        ParseOptions effective = m_DefaultParseOptions;
        if (options) {
            effective = *options;
        }
        if (effective.maxDepth == 0) {
            effective.maxDepth = 1;
        }
        if (effective.maxNodes == 0 || effective.maxNodes > kAbsoluteMaxNodes) {
            effective.maxNodes = std::min<std::size_t>(kAbsoluteMaxNodes, m_DefaultParseOptions.maxNodes);
        }
        if (json.size() > outDocument.stringArena.capacity()) {
            outDocument.stringArena.reserve(json.size());
        }
        Parser parser(json, outDocument, effective, outDiagnostics);
        auto status = parser.Run();
        if (status == StatusCode::Ok) {
            m_Metrics.parsedNodes += static_cast<std::uint64_t>(outDocument.nodes.size());
            m_Metrics.peakNodeCount = std::max(m_Metrics.peakNodeCount,
                                               static_cast<std::uint64_t>(outDocument.nodes.size()));
            m_Metrics.peakStringArena = std::max(m_Metrics.peakStringArena,
                                                 static_cast<std::uint64_t>(outDocument.stringArena.size()));
            if (hadHistory) {
                m_Metrics.reusedDocuments += 1;
            }
            m_Metrics.lastParseFrame = m_CurrentFrame;
        } else {
            m_Metrics.failures += 1;
        }
        return status;
    }

    StatusCode JsonModule::Stringify(const Document &document,
                                     std::string &outJson,
                                     const StringifyOptions *options) const {
        if (!m_Initialized) {
            return StatusCode::InternalError;
        }
        StringifyOptions effective = m_DefaultStringifyOptions;
        if (options) {
            effective = *options;
        }
        if (effective.indentWidth > kMaxIndent) {
            effective.indentWidth = kMaxIndent;
        }
        outJson.clear();
        m_Metrics.stringifyCalls += 1;
        if (document.Empty()) {
            outJson.assign("null");
            if (effective.trailingNewline) {
                outJson.push_back('\n');
            }
            return StatusCode::Ok;
        }
        outJson.reserve(document.stringArena.size() + document.nodes.size() * 2);
        auto status = WriteNode(document, document.root, outJson, effective, 0);
        if (status == StatusCode::Ok) {
            if (effective.trailingNewline) {
                outJson.push_back('\n');
            }
            m_Metrics.stringifiedNodes += static_cast<std::uint64_t>(document.nodes.size());
        } else {
            m_Metrics.failures += 1;
        }
        return status;
    }

    const JsonModule::Metrics &JsonModule::GetMetrics() const noexcept {
        return m_Metrics;
    }

    bool JsonModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    void JsonModule::AppendIndent(std::string &out, std::uint32_t depth, std::uint8_t indentWidth) {
        if (indentWidth == 0 || depth == 0) {
            return;
        }
        out.append(static_cast<std::size_t>(depth) * indentWidth, ' ');
    }

    void JsonModule::AppendUnicodeEscape(std::uint32_t codepoint, std::string &out) {
        auto appendQuad = [&out](std::uint32_t value) {
            char buffer[6];
            buffer[0] = '\\';
            buffer[1] = 'u';
            static const char kHex[] = "0123456789ABCDEF";
            buffer[2] = kHex[(value >> 12) & 0x0F];
            buffer[3] = kHex[(value >> 8) & 0x0F];
            buffer[4] = kHex[(value >> 4) & 0x0F];
            buffer[5] = kHex[value & 0x0F];
            out.append(buffer, buffer + 6);
        };
        if (codepoint <= 0xFFFF) {
            appendQuad(codepoint);
            return;
        }
        auto base = codepoint - 0x10000;
        auto high = static_cast<std::uint32_t>(0xD800 + (base >> 10));
        auto low = static_cast<std::uint32_t>(0xDC00 + (base & 0x3FF));
        appendQuad(high);
        appendQuad(low);
    }

    void JsonModule::AppendCodepointUtf8(std::uint32_t codepoint, std::string &target) {
        if (codepoint <= 0x7F) {
            target.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            target.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
            target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            target.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
            target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            target.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
            target.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    void JsonModule::AppendEscapedString(std::string_view text, bool asciiOnly, std::string &out) {
        out.push_back('"');
        const char *ptr = text.data();
        const char *end = ptr + text.size();
        while (ptr < end) {
            unsigned char ch = static_cast<unsigned char>(*ptr);
            switch (ch) {
                case '"':
                    out.append("\\\"");
                    ++ptr;
                    continue;
                case '\\':
                    out.append("\\\\");
                    ++ptr;
                    continue;
                case '\b':
                    out.append("\\b");
                    ++ptr;
                    continue;
                case '\f':
                    out.append("\\f");
                    ++ptr;
                    continue;
                case '\n':
                    out.append("\\n");
                    ++ptr;
                    continue;
                case '\r':
                    out.append("\\r");
                    ++ptr;
                    continue;
                case '\t':
                    out.append("\\t");
                    ++ptr;
                    continue;
                default:
                    break;
            }
            if (ch <= 0x1F) {
                AppendUnicodeEscape(ch, out);
                ++ptr;
                continue;
            }
            if (!asciiOnly || ch < 0x80) {
                out.push_back(static_cast<char>(ch));
                ++ptr;
                continue;
            }
            std::uint32_t codepoint = 0xFFFD;
            std::size_t consumed = 1;
            if ((ch & 0xE0) == 0xC0 && (ptr + 1) < end) {
                unsigned char b1 = static_cast<unsigned char>(ptr[1]);
                if ((b1 & 0xC0) == 0x80) {
                    std::uint32_t cp = ((ch & 0x1F) << 6) | (b1 & 0x3F);
                    if (cp >= 0x80) {
                        codepoint = cp;
                        consumed = 2;
                    }
                }
            } else if ((ch & 0xF0) == 0xE0 && (ptr + 2) < end) {
                unsigned char b1 = static_cast<unsigned char>(ptr[1]);
                unsigned char b2 = static_cast<unsigned char>(ptr[2]);
                if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
                    std::uint32_t cp = ((ch & 0x0F) << 12) |
                                       ((b1 & 0x3F) << 6) |
                                       (b2 & 0x3F);
                    if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) {
                        codepoint = cp;
                        consumed = 3;
                    }
                }
            } else if ((ch & 0xF8) == 0xF0 && (ptr + 3) < end) {
                unsigned char b1 = static_cast<unsigned char>(ptr[1]);
                unsigned char b2 = static_cast<unsigned char>(ptr[2]);
                unsigned char b3 = static_cast<unsigned char>(ptr[3]);
                if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
                    std::uint32_t cp = ((ch & 0x07) << 18) |
                                       ((b1 & 0x3F) << 12) |
                                       ((b2 & 0x3F) << 6) |
                                       (b3 & 0x3F);
                    if (cp >= 0x10000 && cp <= 0x10FFFF) {
                        codepoint = cp;
                        consumed = 4;
                    }
                }
            }
            AppendUnicodeEscape(codepoint, out);
            ptr += consumed;
        }
        out.push_back('"');
    }

    void JsonModule::AppendNumber(double value, std::string &out) {
        char buffer[64];
        auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 17);
        if (result.ec == std::errc()) {
            out.append(buffer, result.ptr);
            return;
        }
        auto written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
        if (written > 0) {
            out.append(buffer, buffer + std::min<int>(written, sizeof(buffer)));
        }
    }

    StatusCode JsonModule::WriteNode(const Document &document,
                                     std::uint32_t nodeIndex,
                                     std::string &outJson,
                                     const StringifyOptions &options,
                                     std::uint32_t depth) const {
        if (nodeIndex >= document.nodes.size()) {
            return StatusCode::InvalidArgument;
        }
        const auto &node = document.nodes[nodeIndex];
        switch (node.kind) {
            case NodeKind::Null:
                outJson.append("null");
                return StatusCode::Ok;
            case NodeKind::Boolean:
                outJson.append(node.boolValue ? "true" : "false");
                return StatusCode::Ok;
            case NodeKind::Number:
                AppendNumber(node.numberValue, outJson);
                return StatusCode::Ok;
            case NodeKind::String: {
                auto value = document.GetString(node.stringRef);
                AppendEscapedString(value, options.asciiOnly, outJson);
                return StatusCode::Ok;
            }
            case NodeKind::Array: {
                auto span = node.span;
                if ((static_cast<std::size_t>(span.offset) + static_cast<std::size_t>(span.count)) > document.elements.
                    size()) {
                    return StatusCode::InvalidArgument;
                }
                outJson.push_back('[');
                if (span.count == 0) {
                    outJson.push_back(']');
                    return StatusCode::Ok;
                }
                if (options.pretty) {
                    outJson.push_back('\n');
                }
                for (std::uint32_t i = 0; i < span.count; ++i) {
                    if (i > 0) {
                        outJson.push_back(',');
                        if (options.pretty) {
                            outJson.push_back('\n');
                        }
                    }
                    if (options.pretty) {
                        AppendIndent(outJson, depth + 1, options.indentWidth);
                    }
                    auto child = document.elements[span.offset + i];
                    auto status = WriteNode(document, child, outJson, options, depth + 1);
                    if (status != StatusCode::Ok) {
                        return status;
                    }
                }
                if (options.pretty) {
                    outJson.push_back('\n');
                    AppendIndent(outJson, depth, options.indentWidth);
                }
                outJson.push_back(']');
                return StatusCode::Ok;
            }
            case NodeKind::Object: {
                auto span = node.span;
                if ((static_cast<std::size_t>(span.offset) + static_cast<std::size_t>(span.count)) > document.properties
                    .size()) {
                    return StatusCode::InvalidArgument;
                }
                outJson.push_back('{');
                if (span.count == 0) {
                    outJson.push_back('}');
                    return StatusCode::Ok;
                }
                if (options.pretty) {
                    outJson.push_back('\n');
                }
                for (std::uint32_t i = 0; i < span.count; ++i) {
                    if (i > 0) {
                        outJson.push_back(',');
                        if (options.pretty) {
                            outJson.push_back('\n');
                        }
                    }
                    if (options.pretty) {
                        AppendIndent(outJson, depth + 1, options.indentWidth);
                    }
                    const auto &property = document.properties[span.offset + i];
                    AppendEscapedString(document.GetString(property.key), options.asciiOnly, outJson);
                    outJson.push_back(':');
                    if (options.pretty) {
                        outJson.push_back(' ');
                    }
                    auto status = WriteNode(document, property.valueIndex, outJson, options, depth + 1);
                    if (status != StatusCode::Ok) {
                        return status;
                    }
                }
                if (options.pretty) {
                    outJson.push_back('\n');
                    AppendIndent(outJson, depth, options.indentWidth);
                }
                outJson.push_back('}');
                return StatusCode::Ok;
            }
        }
        return StatusCode::InvalidArgument;
    }
}
