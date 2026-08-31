#include "minimal_json.h"

#include <cctype>

namespace {

class JsonParser {
 public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    std::optional<JsonValue> parse() {
        skipWhitespace();
        auto value = parseValue();
        if (!value) {
            return std::nullopt;
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            return std::nullopt;  // trailing garbage after the document
        }
        return value;
    }

 private:
    const std::string& text_;
    std::size_t pos_ = 0;

    void skipWhitespace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool atEnd() const { return pos_ >= text_.size(); }
    char peek() const { return atEnd() ? '\0' : text_[pos_]; }

    bool consume(char expected) {
        if (peek() != expected) {
            return false;
        }
        ++pos_;
        return true;
    }

    std::optional<JsonValue> parseValue() {
        skipWhitespace();
        if (atEnd()) {
            return std::nullopt;
        }
        const char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parseNumber();
        return std::nullopt;
    }

    std::optional<JsonValue> parseObject() {
        if (!consume('{')) {
            return std::nullopt;
        }
        JsonValue value;
        value.type = JsonType::Object;
        skipWhitespace();
        if (consume('}')) {
            return value;
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                return std::nullopt;
            }
            auto key = parseRawString();
            if (!key) {
                return std::nullopt;
            }
            skipWhitespace();
            if (!consume(':')) {
                return std::nullopt;
            }
            auto val = parseValue();
            if (!val) {
                return std::nullopt;
            }
            value.objectValue[*key] = std::move(*val);
            skipWhitespace();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                break;
            }
            return std::nullopt;
        }
        return value;
    }

    std::optional<JsonValue> parseArray() {
        if (!consume('[')) {
            return std::nullopt;
        }
        JsonValue value;
        value.type = JsonType::Array;
        skipWhitespace();
        if (consume(']')) {
            return value;
        }
        while (true) {
            auto val = parseValue();
            if (!val) {
                return std::nullopt;
            }
            value.arrayValue.push_back(std::move(*val));
            skipWhitespace();
            if (consume(',')) {
                continue;
            }
            if (consume(']')) {
                break;
            }
            return std::nullopt;
        }
        return value;
    }

    std::optional<std::string> parseRawString() {
        if (!consume('"')) {
            return std::nullopt;
        }
        std::string result;
        while (true) {
            if (atEnd()) {
                return std::nullopt;
            }
            const char c = text_[pos_++];
            if (c == '"') {
                break;
            }
            if (c == '\\') {
                if (atEnd()) {
                    return std::nullopt;
                }
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"':  result += '"';  break;
                    case '\\': result += '\\'; break;
                    case '/':  result += '/';  break;
                    case 'n':  result += '\n'; break;
                    case 't':  result += '\t'; break;
                    case 'r':  result += '\r'; break;
                    case 'b':  result += '\b'; break;
                    case 'f':  result += '\f'; break;
                    default:   return std::nullopt;  // includes unsupported \uXXXX
                }
            } else {
                result += c;
            }
        }
        return result;
    }

    std::optional<JsonValue> parseString() {
        auto raw = parseRawString();
        if (!raw) {
            return std::nullopt;
        }
        JsonValue value;
        value.type = JsonType::String;
        value.stringValue = *raw;
        return value;
    }

    std::optional<JsonValue> parseBool() {
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            JsonValue value;
            value.type = JsonType::Boolean;
            value.boolValue = true;
            return value;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            JsonValue value;
            value.type = JsonType::Boolean;
            value.boolValue = false;
            return value;
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parseNull() {
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return JsonValue{};
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parseNumber() {
        const std::size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        if (atEnd() || !std::isdigit(static_cast<unsigned char>(peek()))) {
            return std::nullopt;
        }
        while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
            ++pos_;
        }
        if (!atEnd() && peek() == '.') {
            ++pos_;
            if (atEnd() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                return std::nullopt;
            }
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
                ++pos_;
            }
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!atEnd() && (peek() == '+' || peek() == '-')) {
                ++pos_;
            }
            if (atEnd() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                return std::nullopt;
            }
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
                ++pos_;
            }
        }
        const std::string numText = text_.substr(start, pos_ - start);
        JsonValue value;
        value.type = JsonType::Number;
        try {
            value.numberValue = std::stod(numText);
        } catch (...) {
            return std::nullopt;
        }
        return value;
    }
};

}  // namespace

std::optional<JsonValue> parseJson(const std::string& text) {
    JsonParser parser(text);
    return parser.parse();
}

const JsonValue* JsonValue::find(const std::string& key) const {
    if (type != JsonType::Object) {
        return nullptr;
    }
    auto it = objectValue.find(key);
    if (it == objectValue.end()) {
        return nullptr;
    }
    return &it->second;
}

std::string JsonValue::asString(const std::string& fallback) const {
    if (type != JsonType::String) {
        return fallback;
    }
    return stringValue;
}

bool JsonValue::isString() const { return type == JsonType::String; }
bool JsonValue::isArray() const { return type == JsonType::Array; }
bool JsonValue::isObject() const { return type == JsonType::Object; }
