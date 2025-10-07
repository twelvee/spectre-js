#include "mode_adapter.h"

#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spectre::detail {
    namespace {
        std::vector<std::uint8_t> BuildBaseline(const std::string &source) {
            std::vector<std::uint8_t> data;
            data.reserve(source.size() + 8);
            std::uint64_t hash = 1469598103934665603ULL;
            for (unsigned char ch: source) {
                hash ^= ch;
                hash *= 1099511628211ULL;
                data.push_back(static_cast<std::uint8_t>(hash & 0xFFULL));
            }
            for (int i = 0; i < 8; ++i) {
                hash ^= static_cast<std::uint64_t>(i + 1);
                hash *= 1099511628211ULL;
                data.push_back(static_cast<std::uint8_t>((hash >> ((i & 7) * 8)) & 0xFFULL));
            }
            return data;
        }

        std::string HashBytes(const std::vector<std::uint8_t> &bytes) {
            std::uint64_t hash = 1469598103934665603ULL;
            for (auto b: bytes) {
                hash ^= static_cast<std::uint64_t>(b);
                hash *= 1099511628211ULL;
            }
            char buffer[17];
            for (int i = 0; i < 16; ++i) {
                auto shift = static_cast<unsigned>(60 - i * 4);
                auto digit = static_cast<unsigned>((hash >> shift) & 0xFULL);
                buffer[i] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
            }
            buffer[16] = '\0';
            return std::string(buffer);
        }

        void TrimLeft(const std::string &source, std::size_t &pos) {
            while (pos < source.size() && std::isspace(static_cast<unsigned char>(source[pos])) != 0) {
                ++pos;
            }
        }

        bool MatchKeyword(const std::string &source, std::size_t pos, std::string_view keyword) {
            if (pos + keyword.size() > source.size()) {
                return false;
            }
            return source.compare(pos, keyword.size(), keyword) == 0;
        }

        bool TryParseStringLiteral(const std::string &source, std::size_t &pos, std::string &value) {
            if (pos >= source.size()) {
                return false;
            }
            char quote = source[pos];
            if (quote != '\'' && quote != '"') {
                return false;
            }
            ++pos;
            std::string result;
            while (pos < source.size()) {
                char ch = source[pos];
                if (ch == quote) {
                    ++pos;
                    value = std::move(result);
                    return true;
                }
                if (ch == '\\') {
                    if (pos + 1 >= source.size()) {
                        return false;
                    }
                    result.push_back(source[pos + 1]);
                    pos += 2;
                } else {
                    result.push_back(ch);
                    ++pos;
                }
            }
            return false;
        }

        bool TryParseNumberLiteral(const std::string &source, std::size_t &pos, std::string &value) {
            std::size_t start = pos;
            if (pos < source.size() && (source[pos] == '+' || source[pos] == '-')) {
                ++pos;
            }
            while (pos < source.size() && std::isdigit(static_cast<unsigned char>(source[pos])) != 0) {
                ++pos;
            }
            if (pos < source.size() && source[pos] == '.') {
                ++pos;
                while (pos < source.size() && std::isdigit(static_cast<unsigned char>(source[pos])) != 0) {
                    ++pos;
                }
            }
            if (pos < source.size() && (source[pos] == 'e' || source[pos] == 'E')) {
                ++pos;
                if (pos < source.size() && (source[pos] == '+' || source[pos] == '-')) {
                    ++pos;
                }
                while (pos < source.size() && std::isdigit(static_cast<unsigned char>(source[pos])) != 0) {
                    ++pos;
                }
            }
            if (pos == start) {
                return false;
            }
            std::string_view token(source.data() + start, pos - start);
            value.assign(token.begin(), token.end());
            return true;
        }

        bool TryInterpretLiteral(const std::string &source, std::string &value) {
            auto pos = source.rfind("return");
            if (pos == std::string::npos) {
                return false;
            }
            pos += 6;
            TrimLeft(source, pos);
            if (TryParseStringLiteral(source, pos, value)) {
                return true;
            }
            if (TryParseNumberLiteral(source, pos, value)) {
                return true;
            }
            if (MatchKeyword(source, pos, "true")) {
                value = "true";
                return true;
            }
            if (MatchKeyword(source, pos, "false")) {
                value = "false";
                return true;
            }
            if (MatchKeyword(source, pos, "null")) {
                value = "null";
                return true;
            }
            if (MatchKeyword(source, pos, "undefined")) {
                value = "undefined";
                return true;
            }
            return false;
        }
    }

    class SingleThreadAdapter final : public ModeAdapter {
    public:
        explicit SingleThreadAdapter(const RuntimeConfig &config) : m_Config(config) {
            m_Config.mode = RuntimeMode::SingleThread;
        }

        StatusCode CreateContext(const ContextConfig &config, SpectreContext **outContext) override {
            auto *existing = FindContext(config.name);
            if (existing != nullptr) {
                if (outContext != nullptr) {
                    *outContext = &existing->context;
                }
                return StatusCode::AlreadyExists;
            }
            auto state = std::make_unique<ContextState>(config);
            auto *contextPtr = &state->context;
            m_Contexts.emplace(config.name, std::move(state));
            if (outContext != nullptr) {
                *outContext = contextPtr;
            }
            return StatusCode::Ok;
        }

        StatusCode DestroyContext(const std::string &name) override {
            auto erased = m_Contexts.erase(name);
            return erased > 0 ? StatusCode::Ok : StatusCode::NotFound;
        }

        EvaluationResult LoadScript(const std::string &contextName, const ScriptSource &script) override {
            EvaluationResult result{StatusCode::Ok, script.name, {}};
            auto *context = FindContext(contextName);
            if (context == nullptr) {
                result.status = StatusCode::NotFound;
                result.value.clear();
                result.diagnostics = "Context not found";
                return result;
            }
            ScriptRecord record;
            record.source = script.source;
            record.bytecode = BuildBaseline(script.source);
            record.bytecodeHash = HashBytes(record.bytecode);
            record.isBytecode = false;
            result.status = context->context.StoreScript(script.name, std::move(record));
            if (result.status == StatusCode::Ok) {
                result.diagnostics = "Script cached";
            } else {
                result.value.clear();
                result.diagnostics = "Store failed";
            }
            return result;
        }

        EvaluationResult LoadBytecode(const std::string &contextName, const BytecodeArtifact &artifact) override {
            EvaluationResult result{StatusCode::Ok, artifact.name, {}};
            auto *context = FindContext(contextName);
            if (context == nullptr) {
                result.status = StatusCode::NotFound;
                result.value.clear();
                result.diagnostics = "Context not found";
                return result;
            }
            ScriptRecord record;
            record.source.clear();
            record.bytecode = artifact.data;
            if (record.bytecode.empty()) {
                record.bytecode = BuildBaseline(artifact.name);
            }
            record.bytecodeHash = HashBytes(record.bytecode);
            record.isBytecode = true;
            result.status = context->context.StoreScript(artifact.name, std::move(record));
            if (result.status == StatusCode::Ok) {
                result.diagnostics = "Bytecode cached";
            } else {
                result.value.clear();
                result.diagnostics = "Store failed";
            }
            return result;
        }

        EvaluationResult EvaluateSync(const std::string &contextName, const std::string &entryPoint) override {
            EvaluationResult result{StatusCode::Ok, {}, {}};
            auto *context = FindContext(contextName);
            if (context == nullptr) {
                result.status = StatusCode::NotFound;
                result.diagnostics = "Context not found";
                return result;
            }
            const ScriptRecord *record = nullptr;
            auto status = context->context.GetScript(entryPoint, &record);
            if (status != StatusCode::Ok || record == nullptr) {
                result.status = StatusCode::NotFound;
                result.diagnostics = "Script not found";
                return result;
            }
            if (record->isBytecode) {
                result.value = record->bytecodeHash;
                result.diagnostics = "Bytecode hash";
                return result;
            }
            std::string literal;
            if (TryInterpretLiteral(record->source, literal)) {
                result.value = std::move(literal);
                result.diagnostics = "Literal";
            } else {
                result.value = record->bytecodeHash;
                result.diagnostics = "Baseline hash";
            }
            return result;
        }

        void Tick(const TickInfo &info) override {
            m_LastTick = info;
        }

        StatusCode Reconfigure(const RuntimeConfig &config) override {
            if (config.mode != RuntimeMode::SingleThread && config.mode != m_Config.mode) {
                return StatusCode::InvalidArgument;
            }
            m_Config = config;
            m_Config.mode = RuntimeMode::SingleThread;
            return StatusCode::Ok;
        }

        const RuntimeConfig &Config() const override {
            return m_Config;
        }

        StatusCode GetContext(const std::string &name, const SpectreContext **outContext) const override {
            auto *context = FindContextConst(name);
            if (context == nullptr) {
                return StatusCode::NotFound;
            }
            if (outContext != nullptr) {
                *outContext = &context->context;
            }
            return StatusCode::Ok;
        }

    private:
        struct ContextState {
            explicit ContextState(const ContextConfig &config) : context(config.name, config.initialStackSize) {
            }

            SpectreContext context;
        };

        ContextState *FindContext(const std::string &name) {
            auto it = m_Contexts.find(name);
            if (it == m_Contexts.end()) {
                return nullptr;
            }
            return it->second.get();
        }

        const ContextState *FindContextConst(const std::string &name) const {
            auto it = m_Contexts.find(name);
            if (it == m_Contexts.end()) {
                return nullptr;
            }
            return it->second.get();
        }

        RuntimeConfig m_Config;
        std::unordered_map<std::string, std::unique_ptr<ContextState> > m_Contexts;
        TickInfo m_LastTick{0.0, 0};
    };

    std::unique_ptr<ModeAdapter> MakeModeAdapter(const RuntimeConfig &config) {
        if (config.mode == RuntimeMode::SingleThread) {
            return std::make_unique<SingleThreadAdapter>(config);
        }
        return nullptr;
    }
}
