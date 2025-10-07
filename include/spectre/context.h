#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "spectre/status.h"

namespace spectre {
    struct ScriptRecord {
        std::string source;
        std::vector<std::uint8_t> bytecode;
        std::string bytecodeHash;
        bool isBytecode{false};
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

        std::uint64_t ScriptVersion(const std::string &scriptName) const;

    private:
        struct ScriptSlot {
            std::string name;
            ScriptRecord record;
            std::uint64_t version;
        };

        std::string m_Name;
        std::uint32_t m_StackSize;
        std::vector<ScriptSlot> m_Slots;
        std::unordered_map<std::string, std::size_t> m_Lookup;
    };
}
