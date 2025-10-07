#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "spectre/runtime.h"
#include "spectre/subsystems.h"
#include "spectre/status.h"

namespace spectre::detail {
    std::string HashBytes(const std::vector<std::uint8_t> &bytes);

    std::uint64_t HashString(const std::string &value);

    StatusCode CompileScript(ParserFrontend &parser,
                             BytecodePipeline &bytecode,
                             const ScriptSource &source,
                             ExecutableProgram &outProgram,
                             std::string &diagnostics);

    std::vector<std::uint8_t> SerializeProgram(const ExecutableProgram &program);

    StatusCode DeserializeProgram(const std::vector<std::uint8_t> &data,
                                  ExecutableProgram &outProgram,
                                  std::string &diagnostics);
}
