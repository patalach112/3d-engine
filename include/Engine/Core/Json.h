#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace engine::json {

// ---------------------------------------------------------------------------
// Minimal JSON value model, parser and writer (Phase 19).
//
// Just enough RFC 8259 for engine data files (scene serialization,
// future config/metadata): objects preserve insertion order, numbers
// are doubles, strings are UTF-8 with full escape handling. The
// writer emits the shortest representation that round-trips: values
// exactly representable as float print in float form (0.1, not
// 0.10000000149011612), which keeps scene files human-readable while
// remaining bit-exact for float fields.
// ---------------------------------------------------------------------------
class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;

    static Value null() { return Value{}; }
    static Value boolean(bool value) {
        Value result;
        result.m_type = Type::Bool;
        result.m_bool = value;
        return result;
    }
    static Value number(double value) {
        Value result;
        result.m_type = Type::Number;
        result.m_number = value;
        return result;
    }
    static Value string(std::string value) {
        Value result;
        result.m_type = Type::String;
        result.m_string = std::move(value);
        return result;
    }
    static Value array() {
        Value result;
        result.m_type = Type::Array;
        return result;
    }
    static Value object() {
        Value result;
        result.m_type = Type::Object;
        return result;
    }

    void push(Value value) { m_items.push_back(std::move(value)); }
    void set(std::string key, Value value) {
        for (auto& [existingKey, existingValue] : m_members) {
            if (existingKey == key) {
                existingValue = std::move(value);
                return;
            }
        }
        m_members.emplace_back(std::move(key), std::move(value));
    }

    [[nodiscard]] Type type() const { return m_type; }
    [[nodiscard]] bool isNull() const { return m_type == Type::Null; }
    [[nodiscard]] bool asBool(bool fallback = false) const {
        return m_type == Type::Bool ? m_bool : fallback;
    }
    [[nodiscard]] double asNumber(double fallback = 0.0) const {
        return m_type == Type::Number ? m_number : fallback;
    }
    [[nodiscard]] const std::string& asString() const {
        static const std::string kEmpty;
        return m_type == Type::String ? m_string : kEmpty;
    }
    [[nodiscard]] const std::vector<Value>& items() const { return m_items; }
    [[nodiscard]] const std::vector<std::pair<std::string, Value>>& members() const { return m_members; }
    [[nodiscard]] const Value* find(std::string_view key) const {
        if (m_type != Type::Object) {
            return nullptr;
        }
        for (const auto& [memberKey, memberValue] : m_members) {
            if (memberKey == key) {
                return &memberValue;
            }
        }
        return nullptr;
    }

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    std::vector<Value> m_items;
    std::vector<std::pair<std::string, Value>> m_members;
};

namespace detail {

inline void appendUtf8(std::string& out, uint32_t codePoint) {
    if (codePoint < 0x80u) {
        out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800u) {
        out += static_cast<char>(0xC0u | (codePoint >> 6u));
        out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
    } else if (codePoint < 0x10000u) {
        out += static_cast<char>(0xE0u | (codePoint >> 12u));
        out += static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu));
        out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (codePoint >> 18u));
        out += static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3Fu));
        out += static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu));
        out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
    }
}

class Parser {
public:
    Parser(std::string_view text, std::string* error) : m_text(text), m_error(error) {}

    bool parse(Value& out) {
        skipWhitespace();
        if (!parseValue(out)) {
            return false;
        }
        skipWhitespace();
        if (m_pos != m_text.size()) {
            return fail("trailing characters after the top-level value");
        }
        return true;
    }

private:
    bool fail(const char* message) {
        if (m_error != nullptr) {
            *m_error = std::string{message} + " at byte " + std::to_string(m_pos);
        }
        return false;
    }

    void skipWhitespace() {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++m_pos;
        }
    }

    bool consume(char expected) {
        if (m_pos < m_text.size() && m_text[m_pos] == expected) {
            ++m_pos;
            return true;
        }
        return false;
    }

    bool parseValue(Value& out) {
        if (m_pos >= m_text.size()) {
            return fail("unexpected end of input");
        }
        switch (m_text[m_pos]) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': {
                std::string text;
                if (!parseString(text)) {
                    return false;
                }
                out = Value::string(std::move(text));
                return true;
            }
            case 't': return parseLiteral("true", Value::boolean(true), out);
            case 'f': return parseLiteral("false", Value::boolean(false), out);
            case 'n': return parseLiteral("null", Value::null(), out);
            default: return parseNumber(out);
        }
    }

    bool parseLiteral(std::string_view literal, Value value, Value& out) {
        if (m_text.substr(m_pos, literal.size()) == literal) {
            m_pos += literal.size();
            out = std::move(value);
            return true;
        }
        return fail("invalid literal");
    }

    bool parseNumber(Value& out) {
        const size_t start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-') {
            ++m_pos;
        }
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++m_pos;
            } else {
                break;
            }
        }
        if (m_pos == start) {
            return fail("expected a value");
        }
        double number = 0.0;
        const char* first = m_text.data() + start;
        const char* last = m_text.data() + m_pos;
        const std::from_chars_result result = std::from_chars(first, last, number);
        if (result.ec != std::errc{} || result.ptr != last) {
            return fail("invalid number");
        }
        out = Value::number(number);
        return true;
    }

    bool parseHex4(uint32_t& out) {
        if (m_pos + 4 > m_text.size()) {
            return fail("truncated \\u escape");
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            value <<= 4u;
            if (c >= '0' && c <= '9') {
                value |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return fail("invalid hex digit in \\u escape");
            }
        }
        out = value;
        return true;
    }

    bool parseString(std::string& out) {
        if (!consume('"')) {
            return fail("expected a string");
        }
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_pos >= m_text.size()) {
                break;
            }
            const char escape = m_text[m_pos++];
            switch (escape) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t codePoint = 0;
                    if (!parseHex4(codePoint)) {
                        return false;
                    }
                    if (codePoint >= 0xD800u && codePoint <= 0xDBFFu && m_pos + 1 < m_text.size() &&
                        m_text[m_pos] == '\\' && m_text[m_pos + 1] == 'u') {
                        m_pos += 2;
                        uint32_t low = 0;
                        if (!parseHex4(low)) {
                            return false;
                        }
                        if (low >= 0xDC00u && low <= 0xDFFFu) {
                            codePoint = 0x10000u + ((codePoint - 0xD800u) << 10u) + (low - 0xDC00u);
                        }
                    }
                    appendUtf8(out, codePoint);
                    break;
                }
                default: return fail("invalid escape sequence");
            }
        }
        return fail("unterminated string");
    }

    bool parseArray(Value& out) {
        ++m_pos; // consume '['
        Value result = Value::array();
        skipWhitespace();
        if (consume(']')) {
            out = std::move(result);
            return true;
        }
        while (true) {
            skipWhitespace();
            Value item;
            if (!parseValue(item)) {
                return false;
            }
            result.push(std::move(item));
            skipWhitespace();
            if (consume(']')) {
                break;
            }
            if (!consume(',')) {
                return fail("expected ',' or ']' in array");
            }
        }
        out = std::move(result);
        return true;
    }

    bool parseObject(Value& out) {
        ++m_pos; // consume '{'
        Value result = Value::object();
        skipWhitespace();
        if (consume('}')) {
            out = std::move(result);
            return true;
        }
        while (true) {
            skipWhitespace();
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipWhitespace();
            if (!consume(':')) {
                return fail("expected ':' in object");
            }
            skipWhitespace();
            Value member;
            if (!parseValue(member)) {
                return false;
            }
            result.set(std::move(key), std::move(member));
            skipWhitespace();
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return fail("expected ',' or '}' in object");
            }
        }
        out = std::move(result);
        return true;
    }

    std::string_view m_text;
    std::string* m_error = nullptr;
    size_t m_pos = 0;
};

inline void writeEscaped(std::string& out, const std::string& text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    out += "\\u00";
                    const char* digits = "0123456789abcdef";
                    out += digits[(static_cast<unsigned char>(c) >> 4u) & 0xFu];
                    out += digits[static_cast<unsigned char>(c) & 0xFu];
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

inline void writeNumber(std::string& out, double number) {
    char buffer[64];
    // Shortest round-trip form: prefer the float rendering when the
    // double is exactly a float (the common case for engine data).
    const float asFloat = static_cast<float>(number);
    std::to_chars_result result;
    if (static_cast<double>(asFloat) == number) {
        result = std::to_chars(buffer, buffer + sizeof(buffer), asFloat);
    } else {
        result = std::to_chars(buffer, buffer + sizeof(buffer), number);
    }
    if (result.ec == std::errc{}) {
        out.append(buffer, result.ptr);
    } else {
        out += '0';
    }
    // Keep integers visibly integral ("1" is fine as-is; nothing to do).
}

inline void writeValue(std::string& out, const Value& value, int indent) {
    const std::string pad(static_cast<size_t>(indent) * 2, ' ');
    const std::string padInner(static_cast<size_t>(indent + 1) * 2, ' ');
    switch (value.type()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += value.asBool() ? "true" : "false"; break;
        case Value::Type::Number: writeNumber(out, value.asNumber()); break;
        case Value::Type::String: writeEscaped(out, value.asString()); break;
        case Value::Type::Array: {
            if (value.items().empty()) {
                out += "[]";
                break;
            }
            out += "[\n";
            for (size_t i = 0; i < value.items().size(); ++i) {
                out += padInner;
                writeValue(out, value.items()[i], indent + 1);
                out += (i + 1 < value.items().size() ? ",\n" : "\n");
            }
            out += pad + "]";
            break;
        }
        case Value::Type::Object: {
            if (value.members().empty()) {
                out += "{}";
                break;
            }
            out += "{\n";
            for (size_t i = 0; i < value.members().size(); ++i) {
                out += padInner;
                writeEscaped(out, value.members()[i].first);
                out += ": ";
                writeValue(out, value.members()[i].second, indent + 1);
                out += (i + 1 < value.members().size() ? ",\n" : "\n");
            }
            out += pad + "}";
            break;
        }
    }
}

} // namespace detail

// Parses JSON text. On failure returns false and (when given) fills
// `error` with a message carrying the byte offset.
inline bool parse(std::string_view text, Value& out, std::string* error = nullptr) {
    detail::Parser parser{text, error};
    return parser.parse(out);
}

// Serializes a value as pretty-printed JSON (2-space indent).
inline std::string stringify(const Value& value) {
    std::string out;
    detail::writeValue(out, value, 0);
    out += '\n';
    return out;
}

} // namespace engine::json
