#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spectre::detail {
    std::vector<std::uint8_t> BuildBaseline(const std::string &source);

    std::string HashBytes(const std::vector<std::uint8_t> &bytes);

    std::uint64_t HashString(const std::string &value);

    void TrimLeft(const std::string &source, std::size_t &pos);

    bool MatchKeyword(const std::string &source, std::size_t pos, std::string_view keyword);

    bool TryParseStringLiteral(const std::string &source, std::size_t &pos, std::string &value);

    bool TryParseNumberLiteral(const std::string &source, std::size_t &pos, std::string &value);

    bool TryInterpretLiteral(const std::string &source, std::string &value);
}
