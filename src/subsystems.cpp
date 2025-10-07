#include "spectre/subsystems.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spectre::detail {
    namespace {
        constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
        constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

        std::uint64_t HashString(std::string_view value) {
            std::uint64_t hash = kFnvOffset;
            for (unsigned char ch: value) {
                hash ^= ch;
                hash *= kFnvPrime;
            }
            return hash;
        }

        std::uint64_t HashBytes(const std::vector<std::uint8_t> &bytes) {
            std::uint64_t hash = kFnvOffset;
            for (auto b: bytes) {
                hash ^= static_cast<std::uint64_t>(b);
                hash *= kFnvPrime;
            }
            return hash;
        }

        std::string ToHex(std::uint64_t value) {
            std::array < char, 17 > buffer{};
            for (int i = 0; i < 16; ++i) {
                auto shift = static_cast<unsigned>((15 - i) * 4);
                auto digit = static_cast<unsigned>((value >> shift) & 0xFULL);
                buffer[i] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
            }
            buffer[16] = '\0';
            return std::string(buffer.data());
        }

        enum class OpCode : std::uint8_t {
            PushNumber = 0,
            PushString = 1,
            PushBoolean = 2,
            PushNull = 3,
            PushUndefined = 4,
            Add = 5,
            Sub = 6,
            Mul = 7,
            Div = 8,
            Negate = 9,
            Return = 10
        };

        struct Instruction {
            OpCode opcode;
            std::uint32_t operand;
            bool hasOperand;
        };

        class Lexer {
        public:
            explicit Lexer(std::string_view source) : m_Source(source), m_Pos(0) {
            }

            bool Tokenize(std::vector<ParsedToken> &tokens, std::vector<std::string> &literals,
                          std::string &diagnostics) {
                tokens.clear();
                literals.clear();
                diagnostics.clear();

                while (m_Pos < m_Source.size()) {
                    char ch = m_Source[m_Pos];
                    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
                        ++m_Pos;
                        continue;
                    }

                    if (ch == '/' && m_Pos + 1 < m_Source.size() && m_Source[m_Pos + 1] == '/') {
                        SkipLineComment();
                        continue;
                    }

                    if (std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '$') {
                        if (!ParseIdentifier(tokens, diagnostics)) {
                            return false;
                        }
                        continue;
                    }

                    if (std::isdigit(static_cast<unsigned char>(ch)) != 0 || (
                            ch == '.' && m_Pos + 1 < m_Source.size() && std::isdigit(
                                static_cast<unsigned char>(m_Source[m_Pos + 1])) != 0)) {
                        if (!ParseNumber(tokens, diagnostics)) {
                            return false;
                        }
                        continue;
                    }

                    if (ch == '\'' || ch == '"') {
                        if (!ParseStringLiteral(tokens, literals, diagnostics)) {
                            return false;
                        }
                        continue;
                    }

                    if (!ParsePunctuation(tokens, diagnostics)) {
                        return false;
                    }
                }

                ParsedToken endToken{};
                endToken.kind = TokenKind::End;
                endToken.begin = static_cast<std::uint32_t>(m_Source.size());
                endToken.end = endToken.begin;
                endToken.literalIndex = -1;
                endToken.numericValue = 0.0;
                tokens.push_back(endToken);
                return true;
            }

        private:
            void SkipLineComment() {
                while (m_Pos < m_Source.size() && m_Source[m_Pos] != '\n') {
                    ++m_Pos;
                }
            }

            bool ParseIdentifier(std::vector<ParsedToken> &tokens, std::string &diagnostics) {
                std::size_t start = m_Pos;
                while (m_Pos < m_Source.size()) {
                    char ch = m_Source[m_Pos];
                    if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '_' && ch != '$') {
                        break;
                    }
                    ++m_Pos;
                }
                std::string_view lexeme(m_Source.data() + start, m_Pos - start);
                ParsedToken token{};
                token.begin = static_cast<std::uint32_t>(start);
                token.end = static_cast<std::uint32_t>(m_Pos);
                token.literalIndex = -1;
                token.numericValue = 0.0;

                if (lexeme == "return") {
                    token.kind = TokenKind::Return;
                } else if (lexeme == "true") {
                    token.kind = TokenKind::TrueLiteral;
                } else if (lexeme == "false") {
                    token.kind = TokenKind::FalseLiteral;
                } else if (lexeme == "null") {
                    token.kind = TokenKind::NullLiteral;
                } else if (lexeme == "undefined") {
                    token.kind = TokenKind::UndefinedLiteral;
                } else {
                    diagnostics = "Unexpected identifier '" + std::string(lexeme) + "'";
                    return false;
                }

                tokens.push_back(token);
                return true;
            }

            bool ParseNumber(std::vector<ParsedToken> &tokens, std::string &diagnostics) {
                std::size_t start = m_Pos;
                if (m_Source[m_Pos] == '+' || m_Source[m_Pos] == '-') {
                    ++m_Pos;
                }

                bool hasDigits = false;
                while (m_Pos < m_Source.size() && std::isdigit(static_cast<unsigned char>(m_Source[m_Pos])) != 0) {
                    ++m_Pos;
                    hasDigits = true;
                }

                if (m_Pos < m_Source.size() && m_Source[m_Pos] == '.') {
                    ++m_Pos;
                    while (m_Pos < m_Source.size() && std::isdigit(static_cast<unsigned char>(m_Source[m_Pos])) != 0) {
                        ++m_Pos;
                        hasDigits = true;
                    }
                }

                if (m_Pos < m_Source.size() && (m_Source[m_Pos] == 'e' || m_Source[m_Pos] == 'E')) {
                    ++m_Pos;
                    if (m_Pos < m_Source.size() && (m_Source[m_Pos] == '+' || m_Source[m_Pos] == '-')) {
                        ++m_Pos;
                    }
                    bool expDigits = false;
                    while (m_Pos < m_Source.size() && std::isdigit(static_cast<unsigned char>(m_Source[m_Pos])) != 0) {
                        ++m_Pos;
                        expDigits = true;
                    }
                    if (!expDigits) {
                        diagnostics = "Malformed exponent in numeric literal";
                        return false;
                    }
                }

                if (!hasDigits) {
                    diagnostics = "Malformed numeric literal";
                    return false;
                }

                std::string_view lexeme(m_Source.data() + start, m_Pos - start);
                double value = 0.0;
                auto result = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value,
                                              std::chars_format::general);
                if (result.ec != std::errc()) {
                    diagnostics = "Failed to parse numeric literal";
                    return false;
                }

                ParsedToken token{};
                token.kind = TokenKind::Number;
                token.begin = static_cast<std::uint32_t>(start);
                token.end = static_cast<std::uint32_t>(m_Pos);
                token.literalIndex = -1;
                token.numericValue = value;
                tokens.push_back(token);
                return true;
            }

            bool ParseStringLiteral(std::vector<ParsedToken> &tokens, std::vector<std::string> &literals,
                                    std::string &diagnostics) {
                char quote = m_Source[m_Pos];
                std::size_t start = m_Pos;
                ++m_Pos;
                std::string value;
                value.reserve(16);
                while (m_Pos < m_Source.size()) {
                    char ch = m_Source[m_Pos];
                    if (ch == quote) {
                        ++m_Pos;
                        ParsedToken token{};
                        token.kind = TokenKind::String;
                        token.begin = static_cast<std::uint32_t>(start);
                        token.end = static_cast<std::uint32_t>(m_Pos);
                        token.numericValue = 0.0;
                        token.literalIndex = static_cast<std::int32_t>(literals.size());
                        literals.push_back(std::move(value));
                        tokens.push_back(token);
                        return true;
                    }
                    if (ch == '\\') {
                        if (m_Pos + 1 >= m_Source.size()) {
                            diagnostics = "Unterminated string literal";
                            return false;
                        }
                        char escape = m_Source[m_Pos + 1];
                        switch (escape) {
                            case '\\': value.push_back('\\');
                                break;
                            case '\"': value.push_back('\"');
                                break;
                            case '\'': value.push_back('\'');
                                break;
                            case 'n': value.push_back('\n');
                                break;
                            case 'r': value.push_back('\r');
                                break;
                            case 't': value.push_back('\t');
                                break;
                            default:
                                diagnostics = "Unsupported escape sequence";
                                return false;
                        }
                        m_Pos += 2;
                    } else {
                        value.push_back(ch);
                        ++m_Pos;
                    }
                }
                diagnostics = "Unterminated string literal";
                return false;
            }

            bool ParsePunctuation(std::vector<ParsedToken> &tokens, std::string &diagnostics) {
                static const std::unordered_map<char, TokenKind> kMap{
                    {';', TokenKind::Semicolon},
                    {'+', TokenKind::Plus},
                    {'-', TokenKind::Minus},
                    {'*', TokenKind::Star},
                    {'/', TokenKind::Slash},
                    {'(', TokenKind::LeftParen},
                    {')', TokenKind::RightParen}
                };

                char ch = m_Source[m_Pos];
                auto it = kMap.find(ch);
                if (it == kMap.end()) {
                    diagnostics = std::string("Unexpected character '") + ch + "'";
                    return false;
                }
                ParsedToken token{};
                token.kind = it->second;
                token.begin = static_cast<std::uint32_t>(m_Pos);
                token.end = static_cast<std::uint32_t>(m_Pos + 1);
                token.literalIndex = -1;
                token.numericValue = 0.0;
                tokens.push_back(token);
                ++m_Pos;
                return true;
            }

            std::string_view m_Source;
            std::size_t m_Pos;
        };

        class CompilationContext {
        public:
            explicit CompilationContext(const ModuleArtifact &artifact)
                : m_Artifact(artifact), m_Index(0) {
            }

            bool Compile(ExecutableProgram &outProgram) {
                if (m_Artifact.tokens.empty()) {
                    m_Diagnostics = "Module has no tokens";
                    return false;
                }

                if (!Expect(TokenKind::Return, "Module must start with return statement")) {
                    return false;
                }
                Advance();
                if (!ParseExpression()) {
                    return false;
                }
                if (!Expect(TokenKind::Semicolon, "Missing ';' after return expression")) {
                    return false;
                }
                Advance();
                if (!Expect(TokenKind::End, "Unexpected tokens after return statement")) {
                    return false;
                }

                Instruction ret{};
                ret.opcode = OpCode::Return;
                ret.hasOperand = false;
                ret.operand = 0;
                m_Instructions.push_back(ret);

                Encode(outProgram);
                return true;
            }

            const std::string &Diagnostics() const {
                return m_Diagnostics;
            }

        private:
            void Encode(ExecutableProgram &outProgram) {
                outProgram.code.clear();
                outProgram.code.reserve(m_Instructions.size() * 5);
                for (const auto &inst: m_Instructions) {
                    outProgram.code.push_back(static_cast<std::uint8_t>(inst.opcode));
                    if (inst.hasOperand) {
                        std::uint32_t value = inst.operand;
                        outProgram.code.push_back(static_cast<std::uint8_t>(value & 0xFFU));
                        outProgram.code.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
                        outProgram.code.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
                        outProgram.code.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
                    }
                }
                outProgram.numberConstants = m_NumberConstants;
                outProgram.stringConstants = m_StringConstants;
            }

            bool ParseExpression() {
                return ParseAdditive();
            }

            bool ParseAdditive() {
                if (!ParseMultiplicative()) {
                    return false;
                }
                while (Match(TokenKind::Plus) || Match(TokenKind::Minus)) {
                    TokenKind op = Previous();
                    if (!ParseMultiplicative()) {
                        return false;
                    }
                    EmitBinary(op);
                }
                return true;
            }

            bool ParseMultiplicative() {
                if (!ParseUnary()) {
                    return false;
                }
                while (Match(TokenKind::Star) || Match(TokenKind::Slash)) {
                    TokenKind op = Previous();
                    if (!ParseUnary()) {
                        return false;
                    }
                    EmitBinary(op);
                }
                return true;
            }

            bool ParseUnary() {
                if (Match(TokenKind::Minus)) {
                    if (!ParseUnary()) {
                        return false;
                    }
                    Instruction inst{};
                    inst.opcode = OpCode::Negate;
                    inst.hasOperand = false;
                    inst.operand = 0;
                    m_Instructions.push_back(inst);
                    return true;
                }
                if (Match(TokenKind::Plus)) {
                    return ParseUnary();
                }
                return ParsePrimary();
            }

            bool ParsePrimary() {
                const auto &token = Current();
                switch (token.kind) {
                    case TokenKind::Number: {
                        auto index = AddNumberConstant(token.numericValue);
                        Emit(OpCode::PushNumber, index);
                        Advance();
                        return true;
                    }
                    case TokenKind::String: {
                        if (token.literalIndex < 0 || static_cast<std::size_t>(token.literalIndex) >= m_Artifact.
                            literals.size()) {
                            m_Diagnostics = "Invalid string literal index";
                            return false;
                        }
                        auto index = AddStringConstant(
                            m_Artifact.literals[static_cast<std::size_t>(token.literalIndex)]);
                        Emit(OpCode::PushString, index);
                        Advance();
                        return true;
                    }
                    case TokenKind::TrueLiteral: {
                        Emit(OpCode::PushBoolean, 1U);
                        Advance();
                        return true;
                    }
                    case TokenKind::FalseLiteral: {
                        Emit(OpCode::PushBoolean, 0U);
                        Advance();
                        return true;
                    }
                    case TokenKind::NullLiteral: {
                        Emit(OpCode::PushNull, 0U, false);
                        Advance();
                        return true;
                    }
                    case TokenKind::UndefinedLiteral: {
                        Emit(OpCode::PushUndefined, 0U, false);
                        Advance();
                        return true;
                    }
                    case TokenKind::LeftParen: {
                        Advance();
                        if (!ParseExpression()) {
                            return false;
                        }
                        if (!Expect(TokenKind::RightParen, "Missing ')' in expression")) {
                            return false;
                        }
                        Advance();
                        return true;
                    }
                    default:
                        m_Diagnostics = "Unexpected token in expression";
                        return false;
                }
            }

            void Emit(OpCode opcode, std::uint32_t operand, bool hasOperand = true) {
                Instruction inst{};
                inst.opcode = opcode;
                inst.operand = operand;
                inst.hasOperand = hasOperand;
                m_Instructions.push_back(inst);
            }

            void EmitBinary(TokenKind operation) {
                Instruction inst{};
                inst.hasOperand = false;
                inst.operand = 0;
                if (operation == TokenKind::Plus) {
                    inst.opcode = OpCode::Add;
                } else if (operation == TokenKind::Minus) {
                    inst.opcode = OpCode::Sub;
                } else if (operation == TokenKind::Star) {
                    inst.opcode = OpCode::Mul;
                } else {
                    inst.opcode = OpCode::Div;
                }
                m_Instructions.push_back(inst);
            }

            bool Match(TokenKind kind) {
                if (Current().kind == kind) {
                    Advance();
                    return true;
                }
                return false;
            }

            bool Expect(TokenKind kind, const char *message) {
                if (Current().kind != kind) {
                    m_Diagnostics = message;
                    return false;
                }
                return true;
            }

            void Advance() {
                if (m_Index < m_Artifact.tokens.size()) {
                    ++m_Index;
                }
            }

            TokenKind Previous() const {
                if (m_Index == 0) {
                    return TokenKind::Invalid;
                }
                return m_Artifact.tokens[m_Index - 1].kind;
            }

            const ParsedToken &Current() const {
                if (m_Index >= m_Artifact.tokens.size()) {
                    return m_Artifact.tokens.back();
                }
                return m_Artifact.tokens[m_Index];
            }

            std::uint32_t AddNumberConstant(double value) {
                std::uint64_t bits;
                static_assert(sizeof(bits) == sizeof(value));
                std::memcpy(&bits, &value, sizeof(value));
                auto it = m_NumberLookup.find(bits);
                if (it != m_NumberLookup.end()) {
                    return it->second;
                }
                std::uint32_t index = static_cast<std::uint32_t>(m_NumberConstants.size());
                m_NumberConstants.push_back(value);
                m_NumberLookup.emplace(bits, index);
                return index;
            }

            std::uint32_t AddStringConstant(const std::string &value) {
                auto it = m_StringLookup.find(value);
                if (it != m_StringLookup.end()) {
                    return it->second;
                }
                std::uint32_t index = static_cast<std::uint32_t>(m_StringConstants.size());
                m_StringConstants.push_back(value);
                m_StringLookup.emplace(m_StringConstants.back(), index);
                return index;
            }

            const ModuleArtifact &m_Artifact;
            std::size_t m_Index;
            std::vector<Instruction> m_Instructions;
            std::vector<double> m_NumberConstants;
            std::vector<std::string> m_StringConstants;
            std::unordered_map<std::uint64_t, std::uint32_t> m_NumberLookup;
            std::unordered_map<std::string, std::uint32_t> m_StringLookup;
            std::string m_Diagnostics;
        };

        class CpuParser final : public ParserFrontend {
        public:
            StatusCode ParseModule(const ScriptUnit &unit, ModuleArtifact &outArtifact) override {
                outArtifact = ModuleArtifact{};
                outArtifact.name = unit.name;
                outArtifact.payload.assign(unit.source.begin(), unit.source.end());
                outArtifact.diagnostics.clear();

                Lexer lexer(unit.source);
                if (!lexer.Tokenize(outArtifact.tokens, outArtifact.literals, outArtifact.diagnostics)) {
                    return StatusCode::InvalidArgument;
                }
                bool hasReturn = false;
                for (const auto &token: outArtifact.tokens) {
                    if (token.kind == TokenKind::Return) {
                        hasReturn = true;
                        break;
                    }
                }
                if (!hasReturn) {
                    outArtifact.diagnostics = "Module must contain a return statement";
                    return StatusCode::InvalidArgument;
                }
                outArtifact.fingerprint = ToHex(HashString(unit.source));
                return StatusCode::Ok;
            }
        };

        class CpuBytecodePipeline final : public BytecodePipeline {
        public:
            StatusCode LowerModule(const ModuleArtifact &artifact, ExecutableProgram &outProgram) override {
                outProgram = ExecutableProgram{};
                outProgram.name = artifact.name;
                CompilationContext context(artifact);
                if (!context.Compile(outProgram)) {
                    outProgram.diagnostics = context.Diagnostics();
                    return StatusCode::InvalidArgument;
                }
                outProgram.version = ++m_Version;
                return StatusCode::Ok;
            }

        private:
            std::uint64_t m_Version{0};
        };

        class BaselineExecutionEngine final : public ExecutionEngine {
        public:
            ExecutionResponse Execute(const ExecutionRequest &request) override {
                ExecutionResponse response{};
                if (request.program == nullptr) {
                    response.status = StatusCode::InvalidArgument;
                    response.diagnostics = "Program pointer is null";
                    return response;
                }
                const auto &program = *request.program;
                std::vector<Value> stack;
                stack.reserve(8);
                std::size_t ip = 0;
                const auto &code = program.code;
                while (ip < code.size()) {
                    auto opcode = static_cast<OpCode>(code[ip++]);
                    switch (opcode) {
                        case OpCode::PushNumber: {
                            auto index = ReadOperand(code, ip);
                            if (index >= program.numberConstants.size()) {
                                return Fail(response, "Numeric constant out of range");
                            }
                            stack.push_back(Value::FromNumber(program.numberConstants[index]));
                            break;
                        }
                        case OpCode::PushString: {
                            auto index = ReadOperand(code, ip);
                            if (index >= program.stringConstants.size()) {
                                return Fail(response, "String constant out of range");
                            }
                            stack.push_back(Value::FromStringIndex(index));
                            break;
                        }
                        case OpCode::PushBoolean: {
                            auto value = ReadOperand(code, ip);
                            stack.push_back(Value::FromBoolean(value != 0));
                            break;
                        }
                        case OpCode::PushNull: {
                            stack.push_back(Value::Null());
                            break;
                        }
                        case OpCode::PushUndefined: {
                            stack.push_back(Value::Undefined());
                            break;
                        }
                        case OpCode::Add:
                        case OpCode::Sub:
                        case OpCode::Mul:
                        case OpCode::Div: {
                            if (stack.size() < 2) {
                                return Fail(response, "Stack underflow");
                            }
                            auto rhs = stack.back();
                            stack.pop_back();
                            auto lhs = stack.back();
                            stack.pop_back();
                            if (lhs.type != ValueType::Number || rhs.type != ValueType::Number) {
                                return Fail(response, "Arithmetic operators require numeric operands");
                            }
                            double result = 0.0;
                            switch (opcode) {
                                case OpCode::Add: result = lhs.number + rhs.number;
                                    break;
                                case OpCode::Sub: result = lhs.number - rhs.number;
                                    break;
                                case OpCode::Mul: result = lhs.number * rhs.number;
                                    break;
                                case OpCode::Div:
                                    if (std::fabs(rhs.number) <= std::numeric_limits<double>::epsilon()) {
                                        return Fail(response, "Division by zero");
                                    }
                                    result = lhs.number / rhs.number;
                                    break;
                                default: break;
                            }
                            stack.push_back(Value::FromNumber(result));
                            break;
                        }
                        case OpCode::Negate: {
                            if (stack.empty()) {
                                return Fail(response, "Stack underflow");
                            }
                            auto &value = stack.back();
                            if (value.type != ValueType::Number) {
                                return Fail(response, "Negation requires numeric operand");
                            }
                            value.number = -value.number;
                            break;
                        }
                        case OpCode::Return: {
                            Value result = stack.empty() ? Value::Undefined() : stack.back();
                            response.status = StatusCode::Ok;
                            response.value = RenderValue(result, program);
                            response.diagnostics = "ok";
                            return response;
                        }
                    }
                }
                return Fail(response, "Program terminated without return");
            }

        private:
            enum class ValueType : std::uint8_t {
                Number,
                Boolean,
                Null,
                Undefined,
                String
            };

            struct Value {
                ValueType type;
                double number;
                bool boolean;
                std::uint32_t stringIndex;
                std::string ownedString;

                static Value FromNumber(double value) {
                    Value v{};
                    v.type = ValueType::Number;
                    v.number = value;
                    v.boolean = false;
                    v.stringIndex = 0;
                    return v;
                }

                static Value FromBoolean(bool value) {
                    Value v{};
                    v.type = ValueType::Boolean;
                    v.number = 0.0;
                    v.boolean = value;
                    v.stringIndex = 0;
                    return v;
                }

                static Value FromStringIndex(std::uint32_t index) {
                    Value v{};
                    v.type = ValueType::String;
                    v.number = 0.0;
                    v.boolean = false;
                    v.stringIndex = index;
                    return v;
                }

                static Value Null() {
                    Value v{};
                    v.type = ValueType::Null;
                    v.number = 0.0;
                    v.boolean = false;
                    v.stringIndex = 0;
                    return v;
                }

                static Value Undefined() {
                    Value v{};
                    v.type = ValueType::Undefined;
                    v.number = 0.0;
                    v.boolean = false;
                    v.stringIndex = 0;
                    return v;
                }
            };

            static std::uint32_t ReadOperand(const std::vector<std::uint8_t> &code, std::size_t &ip) {
                if (ip + 4 > code.size()) {
                    return std::numeric_limits<std::uint32_t>::max();
                }
                std::uint32_t value = 0;
                value |= static_cast<std::uint32_t>(code[ip]);
                value |= static_cast<std::uint32_t>(code[ip + 1]) << 8U;
                value |= static_cast<std::uint32_t>(code[ip + 2]) << 16U;
                value |= static_cast<std::uint32_t>(code[ip + 3]) << 24U;
                ip += 4;
                return value;
            }

            static ExecutionResponse Fail(ExecutionResponse &response, const char *message) {
                response.status = StatusCode::InvalidArgument;
                response.value.clear();
                response.diagnostics = message;
                return response;
            }

            static std::string RenderValue(const Value &value, const ExecutableProgram &program) {
                switch (value.type) {
                    case ValueType::Number: {
                        char buffer[64];
                        auto result = std::to_chars(std::begin(buffer), std::end(buffer), value.number,
                                                    std::chars_format::general, 15);
                        if (result.ec != std::errc()) {
                            return std::to_string(value.number);
                        }
                        return std::string(buffer, result.ptr);
                    }
                    case ValueType::Boolean:
                        return value.boolean ? "true" : "false";
                    case ValueType::Null:
                        return "null";
                    case ValueType::Undefined:
                        return "undefined";
                    case ValueType::String:
                        if (!value.ownedString.empty()) {
                            return value.ownedString;
                        }
                        if (value.stringIndex < program.stringConstants.size()) {
                            return program.stringConstants[value.stringIndex];
                        }
                        return {};
                }
                return {};
            }
        };

        class CpuGarbageCollector final : public GarbageCollector {
        public:
            StatusCode Collect(GcSnapshot &snapshot) override {
                snapshot.generation = ++m_Generation;
                snapshot.reclaimedBytes = snapshot.generation * 1024;
                return StatusCode::Ok;
            }

        private:
            std::uint64_t m_Generation{0};
        };

        class CpuMemorySystem final : public MemorySystem {
        public:
            explicit CpuMemorySystem(MemoryBudget budget) : m_Budget(budget) {
            }

            StatusCode ApplyPlan(const MemoryBudgetPlan &plan) override {
                m_Budget = plan.target;
                m_LastWaste = plan.arenaWaste;
                return StatusCode::Ok;
            }

            const MemoryBudget &Budget() const {
                return m_Budget;
            }

            std::uint64_t LastWaste() const {
                return m_LastWaste;
            }

        private:
            MemoryBudget m_Budget;
            std::uint64_t m_LastWaste{0};
        };

        class CpuTelemetryHub final : public TelemetryHub {
        public:
            explicit CpuTelemetryHub(std::size_t capacity) {
                m_Buffer.reserve(capacity);
            }

            void PushSample(const TelemetrySample &sample) override {
                m_Buffer.push_back(sample);
            }

            std::vector<TelemetrySample> Drain() override {
                auto data = m_Buffer;
                m_Buffer.clear();
                return data;
            }

        private:
            std::vector<TelemetrySample> m_Buffer;
        };

        class CpuScheduler final : public Scheduler {
        public:
            StatusCode PlanFrame(const SchedulerFramePlan &plan) override {
                if (plan.cpuBudget <= 0.0 || plan.gpuBudget < 0.0) {
                    return StatusCode::InvalidArgument;
                }
                m_LastPlan = plan;
                return StatusCode::Ok;
            }

            const SchedulerFramePlan &LastPlan() const {
                return m_LastPlan;
            }

        private:
            SchedulerFramePlan m_LastPlan{0, 0.0, 0.0};
        };

        class CpuInteropBridge final : public InteropBridge {
        public:
            StatusCode Register(const InteropBinding &binding) override {
                auto it = m_Bindings.find(binding.symbol);
                if (it != m_Bindings.end()) {
                    return StatusCode::AlreadyExists;
                }
                m_Bindings[binding.symbol] = binding.handle;
                return StatusCode::Ok;
            }

            std::size_t BindingCount() const {
                return m_Bindings.size();
            }

        private:
            std::unordered_map<std::string, void *> m_Bindings;
        };
    }

    SubsystemSuite CreateCpuSubsystemSuite(const RuntimeConfig &config) {
        SubsystemSuite suite;
        suite.parser = std::make_unique<CpuParser>();
        suite.bytecode = std::make_unique<CpuBytecodePipeline>();
        suite.execution = std::make_unique<BaselineExecutionEngine>();
        suite.gc = std::make_unique<CpuGarbageCollector>();
        suite.memory = std::make_unique<CpuMemorySystem>(config.memory);
        suite.telemetry = std::make_unique<CpuTelemetryHub>(config.telemetry.historySize);
        suite.scheduler = std::make_unique<CpuScheduler>();
        suite.interop = std::make_unique<CpuInteropBridge>();
        suite.manifest.parserBackend = "cpu.parser.v1";
        suite.manifest.bytecodeBackend = "cpu.bytecode.v1";
        suite.manifest.executionBackend = "cpu.execution.baseline.v1";
        suite.manifest.gcBackend = "cpu.gc.linear.v1";
        suite.manifest.memoryBackend = "cpu.memory.arena.v1";
        suite.manifest.telemetryBackend = "cpu.telemetry.ring.v1";
        suite.manifest.schedulerBackend = "cpu.scheduler.frame.v1";
        suite.manifest.interopBackend = "cpu.interop.table.v1";
        return suite;
    }
}