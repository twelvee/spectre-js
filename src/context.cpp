#include "spectre/context.h"

namespace spectre {

SpectreContext::SpectreContext(std::string name, std::uint32_t stackSize)
    : m_Name(std::move(name)), m_StackSize(stackSize) {}

const std::string& SpectreContext::Name() const {
    return m_Name;
}

std::uint32_t SpectreContext::StackSize() const {
    return m_StackSize;
}

StatusCode SpectreContext::StoreScript(const std::string& scriptName, ScriptRecord record) {
    m_Scripts[scriptName] = std::move(record);
    return StatusCode::Ok;
}

bool SpectreContext::HasScript(const std::string& scriptName) const {
    return m_Scripts.find(scriptName) != m_Scripts.end();
}

StatusCode SpectreContext::GetScript(const std::string& scriptName, const ScriptRecord** outRecord) const {
    auto it = m_Scripts.find(scriptName);
    if (it == m_Scripts.end()) {
        return StatusCode::NotFound;
    }
    if (outRecord != nullptr) {
        *outRecord = &it->second;
    }
    return StatusCode::Ok;
}

std::vector<std::string> SpectreContext::ScriptNames() const {
    std::vector<std::string> names;
    names.reserve(m_Scripts.size());
    for (const auto& entry : m_Scripts) {
        names.push_back(entry.first);
    }
    return names;
}

}
