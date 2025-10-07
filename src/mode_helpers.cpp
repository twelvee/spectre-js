#include "mode_helpers.h"

#include <array>
#include <cstring>
#include <limits>

namespace spectre::detail {
    namespace {
        constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
        constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

        struct ProgramHeader {
            char magic[4];
            std::uint32_t formatVersion;
            std::uint64_t programVersion;
            std::uint32_t codeSize;
            std::uint32_t numberCount;
            std::uint32_t stringCount;
        };

        constexpr std::uint32_t kProgramFormatVersion = 1;
    }

    std::string HashBytes(const std::vector<std::uint8_t> &bytes) {
        std::uint64_t hash = kFnvOffset;
        for (auto b: bytes) {
            hash ^= static_cast<std::uint64_t>(b);
            hash *= kFnvPrime;
        }
        std::array < char, 17 > buffer{};
        for (int i = 0; i < 16; ++i) {
            auto shift = static_cast<unsigned>((15 - i) * 4);
            auto digit = static_cast<unsigned>((hash >> shift) & 0xFULL);
            buffer[i] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        }
        buffer[16] = '\0';
        return std::string(buffer.data());
    }

    std::uint64_t HashString(const std::string &value) {
        std::uint64_t hash = kFnvOffset;
        for (unsigned char ch: value) {
            hash ^= ch;
            hash *= kFnvPrime;
        }
        return hash;
    }

    StatusCode CompileScript(ParserFrontend &parser,
                             BytecodePipeline &bytecode,
                             const ScriptSource &source,
                             ExecutableProgram &outProgram,
                             std::string &diagnostics) {
        ScriptUnit unit{source.name, source.source};
        ModuleArtifact artifact{};
        auto parseStatus = parser.ParseModule(unit, artifact);
        if (parseStatus != StatusCode::Ok) {
            diagnostics = artifact.diagnostics;
            return parseStatus;
        }
        auto lowerStatus = bytecode.LowerModule(artifact, outProgram);
        if (lowerStatus != StatusCode::Ok) {
            diagnostics = outProgram.diagnostics;
            return lowerStatus;
        }
        diagnostics.clear();
        return StatusCode::Ok;
    }

    std::vector<std::uint8_t> SerializeProgram(const ExecutableProgram &program) {
        ProgramHeader header{};
        header.magic[0] = 'S';
        header.magic[1] = 'J';
        header.magic[2] = 'S';
        header.magic[3] = 'B';
        header.formatVersion = kProgramFormatVersion;
        header.programVersion = program.version;
        header.codeSize = static_cast<std::uint32_t>(program.code.size());
        header.numberCount = static_cast<std::uint32_t>(program.numberConstants.size());
        header.stringCount = static_cast<std::uint32_t>(program.stringConstants.size());

        std::vector<std::uint8_t> data;
        data.reserve(sizeof(header) + header.codeSize + header.numberCount * sizeof(double));
        const auto *headerBytes = reinterpret_cast<const std::uint8_t *>(&header);
        data.insert(data.end(), headerBytes, headerBytes + sizeof(header));
        data.insert(data.end(), program.code.begin(), program.code.end());

        for (double value: program.numberConstants) {
            std::uint64_t bits{};
            static_assert(sizeof(bits) == sizeof(value));
            std::memcpy(&bits, &value, sizeof(value));
            for (int i = 0; i < 8; ++i) {
                data.push_back(static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFFULL));
            }
        }

        for (const auto &str: program.stringConstants) {
            auto length = static_cast<std::uint32_t>(str.size());
            for (int i = 0; i < 4; ++i) {
                data.push_back(static_cast<std::uint8_t>((length >> (i * 8)) & 0xFFU));
            }
            data.insert(data.end(), str.begin(), str.end());
        }

        return data;
    }

    StatusCode DeserializeProgram(const std::vector<std::uint8_t> &data,
                                  ExecutableProgram &outProgram,
                                  std::string &diagnostics) {
        if (data.size() < sizeof(ProgramHeader)) {
            diagnostics = "Bytecode payload too small";
            return StatusCode::InvalidArgument;
        }
        ProgramHeader header{};
        std::memcpy(&header, data.data(), sizeof(header));
        if (!(header.magic[0] == 'S' && header.magic[1] == 'J' && header.magic[2] == 'S' && header.magic[3] == 'B')) {
            diagnostics = "Invalid bytecode signature";
            return StatusCode::InvalidArgument;
        }
        if (header.formatVersion != kProgramFormatVersion) {
            diagnostics = "Unsupported bytecode format";
            return StatusCode::InvalidArgument;
        }

        std::size_t offset = sizeof(header);
        if (offset + header.codeSize > data.size()) {
            diagnostics = "Corrupted bytecode payload";
            return StatusCode::InvalidArgument;
        }
        outProgram.code.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                               data.begin() + static_cast<std::ptrdiff_t>(offset + header.codeSize));
        offset += header.codeSize;

        outProgram.numberConstants.clear();
        outProgram.numberConstants.reserve(header.numberCount);
        for (std::uint32_t i = 0; i < header.numberCount; ++i) {
            if (offset + sizeof(std::uint64_t) > data.size()) {
                diagnostics = "Corrupted numeric section";
                return StatusCode::InvalidArgument;
            }
            std::uint64_t bits = 0;
            for (int b = 0; b < 8; ++b) {
                bits |= static_cast<std::uint64_t>(data[offset + b]) << (b * 8);
            }
            double value;
            std::memcpy(&value, &bits, sizeof(value));
            outProgram.numberConstants.push_back(value);
            offset += 8;
        }

        outProgram.stringConstants.clear();
        outProgram.stringConstants.reserve(header.stringCount);
        for (std::uint32_t i = 0; i < header.stringCount; ++i) {
            if (offset + 4 > data.size()) {
                diagnostics = "Corrupted string section";
                return StatusCode::InvalidArgument;
            }
            std::uint32_t length = 0;
            for (int b = 0; b < 4; ++b) {
                length |= static_cast<std::uint32_t>(data[offset + b]) << (b * 8);
            }
            offset += 4;
            if (offset + length > data.size()) {
                diagnostics = "Corrupted string payload";
                return StatusCode::InvalidArgument;
            }
            outProgram.stringConstants.emplace_back(reinterpret_cast<const char *>(data.data() + offset), length);
            offset += length;
        }

        outProgram.version = header.programVersion;
        outProgram.diagnostics.clear();
        diagnostics.clear();
        return StatusCode::Ok;
    }
}