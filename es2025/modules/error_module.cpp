#include "spectre/es2025/modules/error_module.h"

#include <array>
#include <sstream>

#include "spectre/runtime.h"

namespace spectre::es2025 {
    namespace {
        constexpr std::string_view kName = "Error";
        constexpr std::string_view kSummary = "Error constructors, stacks, and aggregate error handling.";
        constexpr std::string_view kReference = "ECMA-262 Section 19.5";

        struct BuiltinErrorSpec {
            std::string_view type;
            std::string_view message;
        };

        constexpr std::array<BuiltinErrorSpec, 8> kBuiltinErrors{ {
            {"Error", "Unknown error"},
            {"EvalError", "Error during evaluation"},
            {"RangeError", "Argument out of range"},
            {"ReferenceError", "Invalid reference"},
            {"SyntaxError", "Syntax error"},
            {"TypeError", "Invalid operand type"},
            {"URIError", "Malformed URI component"},
            {"AggregateError", "Multiple errors occurred"}
        } };
    }

    ErrorModule::ErrorModule()
        : m_Runtime(nullptr),
          m_Subsystems(nullptr),
          m_Config{},
          m_GpuEnabled(false),
          m_Initialized(false),
          m_ErrorTypes{},
          m_History{},
          m_CurrentFrame(0) {
    }

    std::string_view ErrorModule::Name() const noexcept {
        return kName;
    }

    std::string_view ErrorModule::Summary() const noexcept {
        return kSummary;
    }

    std::string_view ErrorModule::SpecificationReference() const noexcept {
        return kReference;
    }

    void ErrorModule::Initialize(const ModuleInitContext &context) {
        m_Runtime = &context.runtime;
        m_Subsystems = &context.subsystems;
        m_Config = context.config;
        m_GpuEnabled = context.config.enableGpuAcceleration;
        m_CurrentFrame = 0;
        RegisterBuiltins();
        m_Initialized = true;
    }

    void ErrorModule::Tick(const TickInfo &info, const ModuleTickContext &) noexcept {
        m_CurrentFrame = info.frameIndex;
    }

    void ErrorModule::OptimizeGpu(const ModuleGpuContext &context) noexcept {
        m_GpuEnabled = context.enableAcceleration;
        (void) context;
    }

    void ErrorModule::Reconfigure(const RuntimeConfig &config) {
        m_Config = config;
        m_GpuEnabled = config.enableGpuAcceleration;
        RegisterBuiltins();
    }

    StatusCode ErrorModule::RegisterErrorType(std::string_view type, std::string_view defaultMessage) {
        if (type.empty()) {
            return StatusCode::InvalidArgument;
        }
        std::string key(type);
        auto it = m_ErrorTypes.find(key);
        if (it == m_ErrorTypes.end()) {
            ErrorDescriptor descriptor{key, std::string(defaultMessage)};
            m_ErrorTypes.emplace(std::move(key), std::move(descriptor));
        } else {
            it->second.defaultMessage = std::string(defaultMessage);
        }
        return StatusCode::Ok;
    }

    bool ErrorModule::HasErrorType(std::string_view type) const noexcept {
        if (type.empty()) {
            return false;
        }
        return m_ErrorTypes.find(std::string(type)) != m_ErrorTypes.end();
    }

    StatusCode ErrorModule::RaiseError(std::string_view type,
                                       std::string_view message,
                                       std::string_view contextName,
                                       std::string_view scriptName,
                                       std::string_view diagnostics,
                                       std::string &outFormatted,
                                       ErrorRecord *outRecord) {
        if (type.empty()) {
            outFormatted.clear();
            return StatusCode::InvalidArgument;
        }

        std::string key(type);
        auto it = m_ErrorTypes.find(key);
        if (it == m_ErrorTypes.end()) {
            outFormatted.clear();
            return StatusCode::NotFound;
        }

        const auto &descriptor = it->second;
        std::string resolvedMessage = message.empty() ? descriptor.defaultMessage : std::string(message);

        std::ostringstream stream;
        stream << descriptor.type << ": " << resolvedMessage;
        if (!contextName.empty() || !scriptName.empty()) {
            stream << " [";
            if (!contextName.empty()) {
                stream << contextName;
            }
            if (!contextName.empty() && !scriptName.empty()) {
                stream << "::";
            }
            if (!scriptName.empty()) {
                stream << scriptName;
            }
            stream << "]";
        }
        if (!diagnostics.empty()) {
            stream << " - " << diagnostics;
        }

        outFormatted = stream.str();

        ErrorRecord record{};
        record.type = descriptor.type;
        record.message = resolvedMessage;
        record.contextName = std::string(contextName);
        record.scriptName = std::string(scriptName);
        record.diagnostics = std::string(diagnostics);
        record.frameIndex = m_CurrentFrame;

        m_History.push_back(record);
        if (outRecord != nullptr) {
            *outRecord = record;
        }

        return StatusCode::Ok;
    }

    const std::vector<ErrorRecord> &ErrorModule::History() const noexcept {
        return m_History;
    }

    std::vector<ErrorRecord> ErrorModule::DrainHistory() {
        auto data = m_History;
        m_History.clear();
        return data;
    }

    void ErrorModule::ClearHistory() noexcept {
        m_History.clear();
    }

    const ErrorRecord *ErrorModule::LastError() const noexcept {
        if (m_History.empty()) {
            return nullptr;
        }
        return &m_History.back();
    }

    bool ErrorModule::GpuEnabled() const noexcept {
        return m_GpuEnabled;
    }

    void ErrorModule::RegisterBuiltins() {
        for (const auto &spec : kBuiltinErrors) {
            RegisterErrorType(spec.type, spec.message);
        }
    }
}

