#include "spectre/es2025/modules/function_module.h"

#include <algorithm>
#include <chrono>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Function";
        constexpr std::string_view kSummary = "Function objects, closures, and callable behavior hooks.";
        constexpr std::string_view kReference = "ECMA-262 Section 19.2";

        constexpr FunctionStats kEmptyStats{0, 0, 0.0};
    }

    FunctionModule::FunctionModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_Functions{},
          m_Names{},
          m_Index{},
          m_CurrentFrame(0) {
    }

    std::string_view FunctionModule::Name() const noexcept {
        return kName;
    }

    std::string_view FunctionModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view FunctionModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void FunctionModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_CurrentFrame = 0;
        m_Functions.clear();
        m_Names.clear();
        m_Index.clear();
        m_Initialized = true;
    }

    void FunctionModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
    }

    void FunctionModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        (void) context;
    }

    void FunctionModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
    }

    StatusCode FunctionModule::RegisterHostFunction(std::string_view name,
                                                    FunctionCallback callback,
                                                    void *userData,
                                                    bool overwrite) {
        if (name.empty() || callback == nullptr) {
            return StatusCode::InvalidArgument;
        }
        std::string key(name);
        auto it = m_Index.find(key);
        if (it != m_Index.end()) {
            auto &entry = m_Functions[it->second];
            if (!overwrite) {
                return StatusCode::AlreadyExists;
            }
            entry.callback = callback;
            entry.userData = userData;
            entry.stats = kEmptyStats;
            entry.lastDiagnostics.clear();
            return StatusCode::Ok;
        }
        Entry entry{};
        entry.name = key;
        entry.callback = callback;
        entry.userData = userData;
        entry.stats = kEmptyStats;
        entry.lastDiagnostics.clear();
        auto index = m_Functions.size();
        m_Functions.push_back(std::move(entry));
        m_Index.emplace(key, index);
        m_Names.push_back(key);
        return StatusCode::Ok;
    }

    bool FunctionModule::HasHostFunction(std::string_view name) const noexcept {
        if (name.empty()) {
            return false;
        }
        return m_Index.find(std::string(name)) != m_Index.end();
    }

    StatusCode FunctionModule::InvokeHostFunction(std::string_view name,
                                                  const std::vector<std::string> &args,
                                                  std::string &outResult,
                                                  std::string &outDiagnostics) {
        outResult.clear();
        outDiagnostics.clear();
        auto *entry = FindMutable(name);
        if (entry == nullptr) {
            return StatusCode::NotFound;
        }
        auto start = std::chrono::steady_clock::now();
        auto status = entry->callback(args, outResult, entry->userData);
        auto end = std::chrono::steady_clock::now();
        auto micros = std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(end - start);
        entry->stats.callCount += 1;
        entry->stats.lastFrameIndex = m_CurrentFrame;
        entry->stats.lastDurationMicros = micros.count();
        if (status != StatusCode::Ok && outDiagnostics.empty()) {
            outDiagnostics = "Host function returned error";
        }
        entry->lastDiagnostics = outDiagnostics;
        return status;
    }

    StatusCode FunctionModule::RemoveHostFunction(std::string_view name) {
        if (name.empty()) {
            return StatusCode::InvalidArgument;
        }
        auto it = m_Index.find(std::string(name));
        if (it == m_Index.end()) {
            return StatusCode::NotFound;
        }
        auto idx = it->second;
        auto lastIdx = m_Functions.size() - 1;
        if (idx != lastIdx) {
            std::swap(m_Functions[idx], m_Functions[lastIdx]);
            m_Index[m_Functions[idx].name] = idx;
        }
        m_Functions.pop_back();
        m_Index.erase(it);
        RebuildNames();
        return StatusCode::Ok;
    }

    const FunctionStats *FunctionModule::GetStats(std::string_view name) const noexcept {
        auto *entry = Find(name);
        return entry == nullptr ? nullptr : &entry->stats;
    }

    const std::vector<std::string> &FunctionModule::RegisteredNames() const noexcept {
        return m_Names;
    }

    void FunctionModule::Clear() {
        m_Functions.clear();
        m_Names.clear();
        m_Index.clear();
    }

    bool FunctionModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    FunctionModule::Entry *FunctionModule::FindMutable(std::string_view name) noexcept {
        if (name.empty()) {
            return nullptr;
        }
        auto it = m_Index.find(std::string(name));
        if (it == m_Index.end()) {
            return nullptr;
        }
        return &m_Functions[it->second];
    }

    const FunctionModule::Entry *FunctionModule::Find(std::string_view name) const noexcept {
        if (name.empty()) {
            return nullptr;
        }
        auto it = m_Index.find(std::string(name));
        if (it == m_Index.end()) {
            return nullptr;
        }
        return &m_Functions[it->second];
    }

    void FunctionModule::RebuildNames() {
        m_Names.clear();
        m_Names.reserve(m_Functions.size());
        for (const auto &entry : m_Functions) {
            m_Names.push_back(entry.name);
        }
    }
}

