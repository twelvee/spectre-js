#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <bit>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace spectre::es2025 {
    struct Value {
        enum class Kind : std::uint8_t {
            Undefined,
            Null,
            Boolean,
            Int32,
            Int64,
            Number,
            BigInt,
            String,
            Symbol,
            Handle,
            External
        };

        enum class HandleKind : std::uint8_t {
            Generic = 0,
            Object,
            Array,
            Map,
            Set,
            WeakMap,
            WeakSet,
            Iterator,
            Promise,
            Function,
            Module,
            Realm,
            ShadowRealm,
            FinalizationRegistry,
            TypedArray,
            DataView,
            ArrayBuffer,
            SharedArrayBuffer,
            Atomics,
            HostEntity
        };

        enum class ExternalKind : std::uint8_t {
            Pointer = 0,
            HostEntity,
            NativeResource
        };

        Value() noexcept;
        explicit Value(double v) noexcept;
        explicit Value(bool v) noexcept;
        explicit Value(std::int32_t v) noexcept;
        explicit Value(std::int64_t v) noexcept;
        explicit Value(std::string_view v);

        Value(const Value &other);
        Value(Value &&other) noexcept;
        Value &operator=(const Value &other);
        Value &operator=(Value &&other) noexcept;
        ~Value();

        static Value Undefined() noexcept;
        static Value Null() noexcept;
        static Value Boolean(bool v) noexcept;
        static Value Number(double v) noexcept;
        static Value Int32(std::int32_t v) noexcept;
        static Value Int64(std::int64_t v) noexcept;
        static Value String(std::string_view v);
        static Value Symbol(std::uint64_t handle) noexcept;
        static Value BigInt(std::uint64_t handle) noexcept;
        static Value Handle(std::uint64_t handle,
                            HandleKind kind = HandleKind::Generic) noexcept;
        static Value Object(std::uint64_t handle) noexcept;
        static Value Promise(std::uint64_t handle) noexcept;
        static Value External(void *pointer,
                              std::uintptr_t info = 0,
                              ExternalKind kind = ExternalKind::Pointer) noexcept;

        bool IsUndefined() const noexcept;
        bool IsNull() const noexcept;
        bool IsBoolean() const noexcept;
        bool IsInt32() const noexcept;
        bool IsInt64() const noexcept;
        bool IsNumber() const noexcept;
        bool IsNumeric() const noexcept;
        bool IsIntegral() const noexcept;
        bool IsString() const noexcept;
        bool IsSymbol() const noexcept;
        bool IsBigInt() const noexcept;
        bool IsHandle() const noexcept;
        bool IsPromise() const noexcept;
        bool IsObject() const noexcept;
        bool IsExternal() const noexcept;
        bool Empty() const noexcept;

        double AsNumber(double fallback = 0.0) const noexcept;
        std::int32_t AsInt32(std::int32_t fallback = 0) const noexcept;
        std::int64_t AsInt64(std::int64_t fallback = 0) const noexcept;
        bool AsBoolean(bool fallback = false) const noexcept;
        std::string_view AsString() const noexcept;
        std::uint64_t AsHandle() const noexcept;
        bool IsInt() const noexcept;
        bool IsDouble() const noexcept;
        std::int64_t Int() const noexcept;
        double Double() const noexcept;
        std::string_view String() const noexcept;

        std::uint64_t AsBigInt() const noexcept;
        std::uint64_t AsSymbol() const noexcept;
        HandleKind HandleTag() const noexcept;
        ExternalKind ExternalTag() const noexcept;
        void *AsExternalPointer() const noexcept;
        std::uintptr_t ExternalInfo() const noexcept;

        void Reset() noexcept;
        void Swap(Value &other) noexcept;

        bool operator==(const Value &other) const noexcept;
        bool operator!=(const Value &other) const noexcept;

        bool SameValueZero(const Value &other) const noexcept;

        std::uint64_t Hash() const noexcept;

        std::string ToString() const;

        Kind kind;

    private:
        struct ExternalPayload {
            void *pointer;
            std::uintptr_t info;
        };

        union Payload {
            bool booleanValue;
            std::int32_t int32Value;
            std::int64_t int64Value;
            double numberValue;
            std::uint64_t handleValue;
            void *pointerValue;
            ExternalPayload external;
            Payload() noexcept {
                Reset();
            }
            void Reset() noexcept {
                std::memset(this, 0, sizeof(Payload));
            }
        } m_Payload;

        std::uint8_t m_Tag;
        std::string m_String;

        static std::uint64_t HashBytes(const void *data, std::size_t size) noexcept;
        static std::uint64_t HashString(std::string_view text) noexcept;
        static bool IsFiniteIntegral(double value) noexcept;
        static bool FitsInInt32(double value) noexcept;
        static bool FitsInInt64(double value) noexcept;
        static bool NumberEqualsIntegral(double number, const Value &integral) noexcept;
        static std::uint64_t HashNormalizedDouble(double value) noexcept;
    };

    inline Value::Value() noexcept
        : kind(Kind::Undefined),
          m_Payload(),
          m_Tag(0),
          m_String() {
    }

    inline Value::Value(const Value &other)
        : kind(other.kind),
          m_Payload(other.m_Payload),
          m_Tag(other.m_Tag),
          m_String(other.m_String) {
    }

    inline Value::Value(Value &&other) noexcept
        : kind(other.kind),
          m_Payload(other.m_Payload),
          m_Tag(other.m_Tag),
          m_String(std::move(other.m_String)) {
        other.kind = Kind::Undefined;
        other.m_Payload.Reset();
        other.m_Tag = 0;
        other.m_String.clear();
    }

    inline Value::Value(double v) noexcept
        : Value() {
        kind = Kind::Number;
        m_Payload.numberValue = v;
    }

    inline Value::Value(bool v) noexcept
        : Value() {
        kind = Kind::Boolean;
        m_Payload.booleanValue = v;
    }

    inline Value::Value(std::int32_t v) noexcept
        : Value() {
        kind = Kind::Int32;
        m_Payload.int32Value = v;
    }

    inline Value::Value(std::int64_t v) noexcept
        : Value() {
        kind = Kind::Int64;
        m_Payload.int64Value = v;
    }

    inline Value::Value(std::string_view v)
        : Value() {
        kind = Kind::String;
        m_String.assign(v.begin(), v.end());
    }

    inline Value &Value::operator=(const Value &other) {
        if (this != &other) {
            kind = other.kind;
            m_Payload = other.m_Payload;
            m_Tag = other.m_Tag;
            m_String = other.m_String;
        }
        return *this;
    }

    inline Value &Value::operator=(Value &&other) noexcept {
        if (this != &other) {
            kind = other.kind;
            m_Payload = other.m_Payload;
            m_Tag = other.m_Tag;
            m_String = std::move(other.m_String);
            other.kind = Kind::Undefined;
            other.m_Payload.Reset();
            other.m_Tag = 0;
            other.m_String.clear();
        }
        return *this;
    }

    inline Value::~Value() = default;

    inline Value Value::Undefined() noexcept {
        return Value();
    }

    inline Value Value::Null() noexcept {
        Value value;
        value.kind = Kind::Null;
        return value;
    }

    inline Value Value::Boolean(bool v) noexcept {
        Value value;
        value.kind = Kind::Boolean;
        value.m_Payload.booleanValue = v;
        return value;
    }

    inline Value Value::Number(double v) noexcept {
        Value value;
        value.kind = Kind::Number;
        value.m_Payload.numberValue = v;
        return value;
    }

    inline Value Value::Int32(std::int32_t v) noexcept {
        Value value;
        value.kind = Kind::Int32;
        value.m_Payload.int32Value = v;
        return value;
    }

    inline Value Value::Int64(std::int64_t v) noexcept {
        Value value;
        value.kind = Kind::Int64;
        value.m_Payload.int64Value = v;
        return value;
    }

    inline Value Value::String(std::string_view v) {
        Value value;
        value.kind = Kind::String;
        value.m_String.assign(v.begin(), v.end());
        return value;
    }

    inline Value Value::Symbol(std::uint64_t handle) noexcept {
        Value value;
        value.kind = Kind::Symbol;
        value.m_Payload.handleValue = handle;
        return value;
    }

    inline Value Value::BigInt(std::uint64_t handle) noexcept {
        Value value;
        value.kind = Kind::BigInt;
        value.m_Payload.handleValue = handle;
        return value;
    }

    inline Value Value::Handle(std::uint64_t handle, HandleKind kindTag) noexcept {
        Value value;
        value.kind = Kind::Handle;
        value.m_Payload.handleValue = handle;
        value.m_Tag = static_cast<std::uint8_t>(kindTag);
        return value;
    }

    inline Value Value::Object(std::uint64_t handle) noexcept {
        return Handle(handle, HandleKind::Object);
    }

    inline Value Value::Promise(std::uint64_t handle) noexcept {
        return Handle(handle, HandleKind::Promise);
    }

    inline Value Value::External(void *pointer,
                                 std::uintptr_t info,
                                 ExternalKind kindTag) noexcept {
        Value value;
        value.kind = Kind::External;
        value.m_Payload.external.pointer = pointer;
        value.m_Payload.external.info = info;
        value.m_Tag = static_cast<std::uint8_t>(kindTag);
        return value;
    }

    inline bool Value::IsUndefined() const noexcept {
        return kind == Kind::Undefined;
    }

    inline bool Value::IsNull() const noexcept {
        return kind == Kind::Null;
    }

    inline bool Value::IsBoolean() const noexcept {
        return kind == Kind::Boolean;
    }

    inline bool Value::IsInt32() const noexcept {
        return kind == Kind::Int32;
    }

    inline bool Value::IsInt64() const noexcept {
        return kind == Kind::Int64;
    }

    inline bool Value::IsNumber() const noexcept {
        return kind == Kind::Number;
    }

    inline bool Value::IsNumeric() const noexcept {
        return kind == Kind::Number || kind == Kind::Int32 || kind == Kind::Int64;
    }

    inline bool Value::IsIntegral() const noexcept {
        return kind == Kind::Int32 || kind == Kind::Int64;
    }

    inline bool Value::IsString() const noexcept {
        return kind == Kind::String;
    }

    inline bool Value::IsSymbol() const noexcept {
        return kind == Kind::Symbol;
    }

    inline bool Value::IsBigInt() const noexcept {
        return kind == Kind::BigInt;
    }

    inline bool Value::IsHandle() const noexcept {
        return kind == Kind::Handle;
    }

    inline bool Value::IsPromise() const noexcept {
        return kind == Kind::Handle && m_Tag == static_cast<std::uint8_t>(HandleKind::Promise);
    }

    inline bool Value::IsObject() const noexcept {
        return kind == Kind::Handle && m_Tag == static_cast<std::uint8_t>(HandleKind::Object);
    }

    inline bool Value::IsExternal() const noexcept {
        return kind == Kind::External;
    }

    inline bool Value::Empty() const noexcept {
        return kind == Kind::Undefined;
    }

    inline double Value::AsNumber(double fallback) const noexcept {
        switch (kind) {
            case Kind::Number:
                return m_Payload.numberValue;
            case Kind::Int32:
                return static_cast<double>(m_Payload.int32Value);
            case Kind::Int64:
                return static_cast<double>(m_Payload.int64Value);
            case Kind::Boolean:
                return m_Payload.booleanValue ? 1.0 : 0.0;
            default:
                return fallback;
        }
    }

    inline std::int32_t Value::AsInt32(std::int32_t fallback) const noexcept {
        switch (kind) {
            case Kind::Int32:
                return m_Payload.int32Value;
            case Kind::Int64: {
                auto value = m_Payload.int64Value;
                if (value >= static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min())
                    && value <= static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
                    return static_cast<std::int32_t>(value);
                }
                return fallback;
            }
            case Kind::Number: {
                double value = m_Payload.numberValue;
                if (FitsInInt32(value)) {
                    return static_cast<std::int32_t>(value);
                }
                return fallback;
            }
            case Kind::Boolean:
                return m_Payload.booleanValue ? 1 : 0;
            default:
                return fallback;
        }
    }

    inline std::int64_t Value::AsInt64(std::int64_t fallback) const noexcept {
        switch (kind) {
            case Kind::Int64:
                return m_Payload.int64Value;
            case Kind::Int32:
                return static_cast<std::int64_t>(m_Payload.int32Value);
            case Kind::Number: {
                double value = m_Payload.numberValue;
                if (FitsInInt64(value)) {
                    return static_cast<std::int64_t>(value);
                }
                return fallback;
            }
            case Kind::Boolean:
                return m_Payload.booleanValue ? 1 : 0;
            default:
                return fallback;
        }
    }

    inline bool Value::AsBoolean(bool fallback) const noexcept {
        switch (kind) {
            case Kind::Boolean:
                return m_Payload.booleanValue;
            case Kind::Number: {
                double value = m_Payload.numberValue;
                if (std::isnan(value) || value == 0.0) {
                    return false;
                }
                return true;
            }
            case Kind::Int32:
                return m_Payload.int32Value != 0;
            case Kind::Int64:
                return m_Payload.int64Value != 0;
            case Kind::String:
                return !m_String.empty();
            case Kind::Null:
            case Kind::Undefined:
                return false;
            case Kind::BigInt:
            case Kind::Symbol:
            case Kind::Handle:
            case Kind::External:
                return true;
        }
        return fallback;
    }

    inline bool Value::IsInt() const noexcept {
        return kind == Kind::Int32 || kind == Kind::Int64;
    }

    inline bool Value::IsDouble() const noexcept {
        return kind == Kind::Number;
    }

    inline std::int64_t Value::Int() const noexcept {
        return AsInt64();
    }

    inline double Value::Double() const noexcept {
        return AsNumber();
    }

    inline std::string_view Value::String() const noexcept {
        return AsString();
    }

    inline std::string_view Value::AsString() const noexcept {
        if (kind == Kind::String) {
            return std::string_view(m_String);
        }
        return std::string_view();
    }

    inline std::uint64_t Value::AsHandle() const noexcept {
        if (kind == Kind::Handle) {
            return m_Payload.handleValue;
        }
        return 0;
    }

    inline std::uint64_t Value::AsBigInt() const noexcept {
        if (kind == Kind::BigInt) {
            return m_Payload.handleValue;
        }
        return 0;
    }

    inline std::uint64_t Value::AsSymbol() const noexcept {
        if (kind == Kind::Symbol) {
            return m_Payload.handleValue;
        }
        return 0;
    }

    inline Value::HandleKind Value::HandleTag() const noexcept {
        if (kind == Kind::Handle) {
            return static_cast<HandleKind>(m_Tag);
        }
        return HandleKind::Generic;
    }

    inline Value::ExternalKind Value::ExternalTag() const noexcept {
        if (kind == Kind::External) {
            return static_cast<ExternalKind>(m_Tag);
        }
        return ExternalKind::Pointer;
    }

    inline void *Value::AsExternalPointer() const noexcept {
        if (kind == Kind::External) {
            return m_Payload.external.pointer;
        }
        return nullptr;
    }

    inline std::uintptr_t Value::ExternalInfo() const noexcept {
        if (kind == Kind::External) {
            return m_Payload.external.info;
        }
        return 0;
    }

    inline void Value::Reset() noexcept {
        kind = Kind::Undefined;
        m_Payload.Reset();
        m_Tag = 0;
        m_String.clear();
    }

    inline void Value::Swap(Value &other) noexcept {
        using std::swap;
        swap(kind, other.kind);
        swap(m_Payload, other.m_Payload);
        swap(m_Tag, other.m_Tag);
        swap(m_String, other.m_String);
    }

    inline bool Value::operator==(const Value &other) const noexcept {
        if (kind != other.kind) {
            return false;
        }
        switch (kind) {
            case Kind::Undefined:
            case Kind::Null:
                return true;
            case Kind::Boolean:
                return m_Payload.booleanValue == other.m_Payload.booleanValue;
            case Kind::Int32:
                return m_Payload.int32Value == other.m_Payload.int32Value;
            case Kind::Int64:
                return m_Payload.int64Value == other.m_Payload.int64Value;
            case Kind::Number:
                return m_Payload.numberValue == other.m_Payload.numberValue;
            case Kind::BigInt:
            case Kind::Symbol:
            case Kind::Handle:
                return m_Payload.handleValue == other.m_Payload.handleValue
                       && m_Tag == other.m_Tag;
            case Kind::String:
                return m_String == other.m_String;
            case Kind::External:
                return m_Payload.external.pointer == other.m_Payload.external.pointer
                       && m_Payload.external.info == other.m_Payload.external.info
                       && m_Tag == other.m_Tag;
        }
        return false;
    }

    inline bool Value::operator!=(const Value &other) const noexcept {
        return !(*this == other);
    }

    inline bool Value::SameValueZero(const Value &other) const noexcept {
        if (kind == Kind::Number && other.kind == Kind::Number) {
            double lhs = m_Payload.numberValue;
            double rhs = other.m_Payload.numberValue;
            if (std::isnan(lhs) && std::isnan(rhs)) {
                return true;
            }
            if (lhs == 0.0 && rhs == 0.0) {
                return true;
            }
            return lhs == rhs;
        }
        if (kind == other.kind) {
            if (kind == Kind::Int32) {
                return m_Payload.int32Value == other.m_Payload.int32Value;
            }
            if (kind == Kind::Int64) {
                return m_Payload.int64Value == other.m_Payload.int64Value;
            }
            return *this == other;
        }
        if (IsIntegral() && other.IsIntegral()) {
            std::int64_t lhs = kind == Kind::Int64 ? m_Payload.int64Value
                                                   : static_cast<std::int64_t>(m_Payload.int32Value);
            std::int64_t rhs = other.kind == Kind::Int64 ? other.m_Payload.int64Value
                                                          : static_cast<std::int64_t>(other.m_Payload.int32Value);
            return lhs == rhs;
        }
        if (kind == Kind::Number && other.IsIntegral()) {
            return NumberEqualsIntegral(m_Payload.numberValue, other);
        }
        if (IsIntegral() && other.kind == Kind::Number) {
            return NumberEqualsIntegral(other.m_Payload.numberValue, *this);
        }
        return false;
    }

    inline std::uint64_t Value::Hash() const noexcept {
        switch (kind) {
            case Kind::Undefined:
                return 0x53a94d8f3f1b4c15ull;
            case Kind::Null:
                return 0x8c9f4a1be2d76143ull;
            case Kind::Boolean:
                return m_Payload.booleanValue ? 0x9b5f5b1a9bd0d3f5ull : 0x4f5a1c7d5c3e2b19ull;
            case Kind::Int32:
                return HashNormalizedDouble(static_cast<double>(m_Payload.int32Value));
            case Kind::Int64: {
                std::int64_t value = m_Payload.int64Value;
                double asDouble = static_cast<double>(value);
                if (static_cast<std::int64_t>(asDouble) == value) {
                    return HashNormalizedDouble(asDouble);
                }
                std::uint64_t buffer[2] = {std::bit_cast<std::uint64_t>(value), 0x9e3779b97f4a7c15ull};
                return HashBytes(buffer, sizeof(buffer));
            }
            case Kind::Number:
                return HashNormalizedDouble(m_Payload.numberValue);
            case Kind::String:
                return HashString(m_String);
            case Kind::BigInt:
            case Kind::Symbol:
            case Kind::Handle: {
                std::uint64_t buffer[2] = {m_Payload.handleValue,
                                           static_cast<std::uint64_t>(m_Tag)};
                return HashBytes(buffer, sizeof(buffer));
            }
            case Kind::External: {
                std::uint64_t buffer[3] = {
                    static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(m_Payload.external.pointer)),
                    static_cast<std::uint64_t>(m_Payload.external.info),
                    static_cast<std::uint64_t>(m_Tag)
                };
                return HashBytes(buffer, sizeof(buffer));
            }
        }
        return 0;
    }

    inline std::string Value::ToString() const {
        switch (kind) {
            case Kind::Undefined:
                return "undefined";
            case Kind::Null:
                return "null";
            case Kind::Boolean:
                return m_Payload.booleanValue ? "true" : "false";
            case Kind::Int32:
                return std::to_string(m_Payload.int32Value);
            case Kind::Int64:
                return std::to_string(m_Payload.int64Value);
            case Kind::Number: {
                std::ostringstream stream;
                stream.setf(std::ios::fmtflags(0), std::ios::floatfield);
                stream.precision(17);
                stream << m_Payload.numberValue;
                return stream.str();
            }
            case Kind::String:
                return m_String;
            case Kind::BigInt: {
                std::ostringstream stream;
                stream << "bigint(0x" << std::hex << m_Payload.handleValue << ")";
                return stream.str();
            }
            case Kind::Symbol: {
                std::ostringstream stream;
                stream << "symbol(0x" << std::hex << m_Payload.handleValue << ")";
                return stream.str();
            }
            case Kind::Handle: {
                std::ostringstream stream;
                stream << "handle(" << static_cast<int>(m_Tag) << ":0x"
                       << std::hex << m_Payload.handleValue << ")";
                return stream.str();
            }
            case Kind::External: {
                auto ptr = reinterpret_cast<std::uintptr_t>(m_Payload.external.pointer);
                std::ostringstream stream;
                stream << "external(0x" << std::hex << ptr << ",0x" << m_Payload.external.info << ")";
                return stream.str();
            }
        }
        return {};
    }

    inline std::uint64_t Value::HashBytes(const void *data, std::size_t size) noexcept {
        constexpr std::uint64_t kOffset = 1469598103934665603ull;
        constexpr std::uint64_t kPrime = 1099511628211ull;
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        std::uint64_t hash = kOffset;
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= static_cast<std::uint64_t>(bytes[i]);
            hash *= kPrime;
        }
        return hash == 0 ? kOffset : hash;
    }

    inline std::uint64_t Value::HashString(std::string_view text) noexcept {
        return HashBytes(text.data(), text.size());
    }

    inline bool Value::IsFiniteIntegral(double value) noexcept {
        return std::isfinite(value) && std::trunc(value) == value;
    }

    inline bool Value::FitsInInt32(double value) noexcept {
        constexpr double kMin = static_cast<double>(std::numeric_limits<std::int32_t>::min());
        constexpr double kMax = static_cast<double>(std::numeric_limits<std::int32_t>::max());
        return IsFiniteIntegral(value) && value >= kMin && value <= kMax;
    }

    inline bool Value::FitsInInt64(double value) noexcept {
        constexpr double kMin = static_cast<double>(std::numeric_limits<std::int64_t>::min());
        constexpr double kMax = static_cast<double>(std::numeric_limits<std::int64_t>::max());
        return IsFiniteIntegral(value) && value >= kMin && value <= kMax;
    }

    inline bool Value::NumberEqualsIntegral(double number, const Value &integral) noexcept {
        if (!integral.IsIntegral()) {
            return false;
        }
        if (integral.kind == Kind::Int32) {
            if (!FitsInInt32(number)) {
                return false;
            }
            return static_cast<std::int32_t>(number) == integral.m_Payload.int32Value;
        }
        if (integral.kind == Kind::Int64) {
            if (!FitsInInt64(number)) {
                return false;
            }
            return static_cast<std::int64_t>(number) == integral.m_Payload.int64Value;
        }
        return false;
    }

    inline std::uint64_t Value::HashNormalizedDouble(double value) noexcept {
        std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
        if (std::isnan(value)) {
            bits = 0x7ff8000000000000ull;
        } else if (bits == 0x8000000000000000ull) {
            bits = 0ull;
        }
        return HashBytes(&bits, sizeof(bits));
    }
}
