#include "spectre/runtime.h"

#include "spectre/context.h"

#include <functional>
#include <memory>
#include <unordered_map>

namespace spectre {

struct SpectreRuntime::Impl {
    RuntimeConfig m_Config;
    std::unordered_map<std::string, SpectreContext> m_Contexts;
    TickInfo m_LastTick{0.0, 0};

    SpectreContext* FindContext(const std::string& name) {
        auto it = m_Contexts.find(name);
        if (it == m_Contexts.end()) {
            return nullptr;
        }
        return &it->second;
    }

    const SpectreContext* FindContext(const std::string& name) const {
        auto it = m_Contexts.find(name);
        if (it == m_Contexts.end()) {
            return nullptr;
        }
        return &it->second;
    }
};

std::unique_ptr<SpectreRuntime> SpectreRuntime::Create(const RuntimeConfig& config) {
    auto impl = std::make_unique<Impl>();
    impl->m_Config = config;
    return std::unique_ptr<SpectreRuntime>(new SpectreRuntime(std::move(impl)));
}

SpectreRuntime::SpectreRuntime(std::unique_ptr<Impl> impl) : m_Impl(std::move(impl)) {}

SpectreRuntime::~SpectreRuntime() = default;

StatusCode SpectreRuntime::CreateContext(const ContextConfig& config, SpectreContext** outContext) {
    auto search = m_Impl->m_Contexts.find(config.name);
    if (search != m_Impl->m_Contexts.end()) {
        if (outContext != nullptr) {
            *outContext = &search->second;
        }
        return StatusCode::AlreadyExists;
    }
    auto insertResult = m_Impl->m_Contexts.emplace(config.name, SpectreContext(config.name, config.initialStackSize));
    if (!insertResult.second) {
        return StatusCode::InternalError;
    }
    if (outContext != nullptr) {
        *outContext = &insertResult.first->second;
    }
    return StatusCode::Ok;
}

StatusCode SpectreRuntime::DestroyContext(const std::string& name) {
    auto erased = m_Impl->m_Contexts.erase(name);
    return erased > 0 ? StatusCode::Ok : StatusCode::NotFound;
}

EvaluationResult SpectreRuntime::LoadScript(const std::string& contextName, const ScriptSource& script) {
    EvaluationResult result{StatusCode::Ok, script.name, "Script stored"};
    auto context = m_Impl->FindContext(contextName);
    if (context == nullptr) {
        result.status = StatusCode::NotFound;
        result.value.clear();
        result.diagnostics = "Context not found";
        return result;
    }
    ScriptRecord record;
    record.source = script.source;
    record.bytecodeHash = std::to_string(std::hash<std::string>{}(script.source));
    result.status = context->StoreScript(script.name, std::move(record));
    if (result.status != StatusCode::Ok) {
        result.value.clear();
        result.diagnostics = "Store script failed";
    }
    return result;
}

EvaluationResult SpectreRuntime::LoadBytecode(const std::string& contextName, const BytecodeArtifact& artifact) {
    EvaluationResult result{StatusCode::Ok, artifact.name, "Bytecode stored"};
    auto context = m_Impl->FindContext(contextName);
    if (context == nullptr) {
        result.status = StatusCode::NotFound;
        result.value.clear();
        result.diagnostics = "Context not found";
        return result;
    }
    ScriptRecord record;
    record.source = {};
    record.bytecodeHash = std::to_string(std::hash<std::string>{}(std::string(artifact.data.begin(), artifact.data.end())));
    result.status = context->StoreScript(artifact.name, std::move(record));
    if (result.status != StatusCode::Ok) {
        result.value.clear();
        result.diagnostics = "Store bytecode failed";
    }
    return result;
}

EvaluationResult SpectreRuntime::EvaluateSync(const std::string& contextName, const std::string& entryPoint) {
    EvaluationResult result{StatusCode::Ok, {}, "Success"};
    auto context = m_Impl->FindContext(contextName);
    if (context == nullptr) {
        result.status = StatusCode::NotFound;
        result.diagnostics = "Context not found";
        return result;
    }
    const ScriptRecord* record = nullptr;
    auto status = context->GetScript(entryPoint, &record);
    if (status != StatusCode::Ok || record == nullptr) {
        result.status = StatusCode::NotFound;
        result.diagnostics = "Script not found";
        return result;
    }
    if (!record->source.empty()) {
        result.value = record->source;
        result.diagnostics = "Source echo";
    } else {
        result.value = record->bytecodeHash;
        result.diagnostics = "Bytecode echo";
    }
    return result;
}

void SpectreRuntime::Tick(const TickInfo& info) {
    m_Impl->m_LastTick = info;
}

StatusCode SpectreRuntime::Reconfigure(const RuntimeConfig& config) {
    m_Impl->m_Config = config;
    return StatusCode::Ok;
}

const RuntimeConfig& SpectreRuntime::Config() const {
    return m_Impl->m_Config;
}

TickInfo SpectreRuntime::LastTick() const {
    return m_Impl->m_LastTick;
}

StatusCode SpectreRuntime::GetContext(const std::string& name, const SpectreContext** outContext) const {
    const Impl* implPtr = m_Impl.get();
    const auto* context = implPtr != nullptr ? implPtr->FindContext(name) : nullptr;
    if (context == nullptr) {
        return StatusCode::NotFound;
    }
    if (outContext != nullptr) {
        *outContext = context;
    }
    return StatusCode::Ok;
}

}
