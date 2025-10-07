#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "spectre/status.h"

namespace spectre {
    struct ScriptRecord {
        std::string source;
        std::string bytecodeHash;
    };

    class SpectreContext {
    public:
        SpectreContext(std::string name, std::uint32_t stackSize);

        const std::string &Name() const;

        std::uint32_t StackSize() const;

        StatusCode StoreScript(const std::string &scriptName, ScriptRecord record);

        bool HasScript(const std::string &scriptName) const;

        StatusCode GetScript(const std::string &scriptName, const ScriptRecord **outRecord) const;

        std::vector<std::string> ScriptNames() const;

    private:
        std::string m_Name;
        std::uint32_t m_StackSize;
        std::unordered_map<std::string, ScriptRecord> m_Scripts;
    };
}
