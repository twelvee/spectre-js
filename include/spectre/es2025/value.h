#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace spectre::es2025 {
    struct Value {
        enum class Kind : std::uint8_t { Undefined, Number, Boolean, String };
        Kind kind;
        double number;
        bool booleanValue;
        std::string text;

        Value() noexcept : kind(Kind::Undefined), number(0.0), booleanValue(false), text() {}
        explicit Value(double v) noexcept : kind(Kind::Number), number(v), booleanValue(false), text() {}
        explicit Value(bool v) noexcept : kind(Kind::Boolean), number(0.0), booleanValue(v), text() {}
        Value(std::string_view v) : kind(Kind::String), number(0.0), booleanValue(false), text(v) {}
        Value(const Value &other) = default;
        Value(Value &&other) noexcept = default;
        Value &operator=(const Value &other) = default;
        Value &operator=(Value &&other) noexcept = default;
        ~Value() = default;

        static Value Undefined() noexcept { return Value(); }
        static Value Number(double v) noexcept { return Value(v); }
        static Value Boolean(bool v) noexcept { return Value(v); }
        static Value String(std::string_view v) { return Value(v); }

        bool IsUndefined() const noexcept { return kind == Kind::Undefined; }
        bool IsNumber() const noexcept { return kind == Kind::Number; }
        bool IsBoolean() const noexcept { return kind == Kind::Boolean; }
        bool IsString() const noexcept { return kind == Kind::String; }

        double AsNumber(double fallback = 0.0) const noexcept { return kind == Kind::Number ? number : fallback; }
        bool AsBoolean(bool fallback = false) const noexcept { return kind == Kind::Boolean ? booleanValue : fallback; }
        std::string_view AsString() const noexcept { return kind == Kind::String ? std::string_view(text) : std::string_view(); }

        std::string ToString() const {
            switch (kind) {
                case Kind::Undefined:
                    return "undefined";
                case Kind::Number: {
                    std::ostringstream stream;
                    stream.setf(std::ios::fmtflags(0), std::ios::floatfield);
                    stream.precision(17);
                    stream << number;
                    return stream.str();
                }
                case Kind::Boolean:
                    return booleanValue ? "true" : "false";
                case Kind::String:
                    return text;
            }
            return {};
        }

        bool operator==(const Value &other) const noexcept {
            if (kind != other.kind) {
                return false;
            }
            switch (kind) {
                case Kind::Undefined:
                    return true;
                case Kind::Number:
                    return number == other.number;
                case Kind::Boolean:
                    return booleanValue == other.booleanValue;
                case Kind::String:
                    return text == other.text;
            }
            return false;
        }

        bool operator!=(const Value &other) const noexcept { return !(*this == other); }
    };
}
