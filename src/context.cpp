#include "spectre/context.h"

#include <algorithm>
#include <utility>

namespace spectre {
    SpectreContext::SpectreContext(std::string name, std::uint32_t stackSize)
        : m_Name(std::move(name)), m_StackSize(stackSize) {
        m_Slots.reserve(8);
        m_Lookup.reserve(8);
    }

    const std::string &SpectreContext::Name() const {
        return m_Name;
    }

    std::uint32_t SpectreContext::StackSize() const {
        return m_StackSize;
    }

    StatusCode SpectreContext::StoreScript(const std::string &scriptName, ScriptRecord record) {
        auto it = m_Lookup.find(scriptName);
        if (it == m_Lookup.end()) {
            ScriptSlot slot;
            slot.name = scriptName;
            slot.record = std::move(record);
            slot.version = 1;
            m_Lookup.emplace(slot.name, m_Slots.size());
            m_Slots.push_back(std::move(slot));
        } else {
            auto &slot = m_Slots[it->second];
            slot.record = std::move(record);
            ++slot.version;
        }
        return StatusCode::Ok;
    }

    bool SpectreContext::HasScript(const std::string &scriptName) const {
        return m_Lookup.find(scriptName) != m_Lookup.end();
    }

    StatusCode SpectreContext::GetScript(const std::string &scriptName, const ScriptRecord **outRecord) const {
        auto it = m_Lookup.find(scriptName);
        if (it == m_Lookup.end()) {
            return StatusCode::NotFound;
        }
        if (outRecord != nullptr) {
            *outRecord = &m_Slots[it->second].record;
        }
        return StatusCode::Ok;
    }

    std::vector<std::string> SpectreContext::ScriptNames() const {
        std::vector<std::string> names;
        names.reserve(m_Slots.size());
        for (const auto &slot: m_Slots) {
            names.push_back(slot.name);
        }
        return names;
    }

    std::uint64_t SpectreContext::ScriptVersion(const std::string &scriptName) const {
        auto it = m_Lookup.find(scriptName);
        if (it == m_Lookup.end()) {
            return 0;
        }
        return m_Slots[it->second].version;
    }
}

