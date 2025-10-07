#include "mode_helpers.h"

#include <cctype>

namespace spectre::detail {
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

    std::uint64_t HashString(const std::string &value) {
        std::uint64_t hash = 1469598103934665603ULL;
        for (unsigned char ch: value) {
            hash ^= ch;
            hash *= 1099511628211ULL;
        }
        return hash;
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
