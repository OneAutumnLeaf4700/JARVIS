#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

enum class JsonType { Null, Boolean, Number, String, Array, Object };

// A minimal JSON value — hand-rolled specifically to read plugin manifest.json files against
// the fixed schema in the plugin SDK design spec. Not a general-purpose JSON library: no
// \uXXXX unicode escapes, no streaming, no pretty-printing. See
// docs/superpowers/specs/2026-08-31-plugin-sdk-loader-design.md for why this is hand-rolled
// rather than a third-party dependency (INV-10).
class JsonValue {
 public:
    JsonType type = JsonType::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::map<std::string, JsonValue> objectValue;

    // Object lookup; nullptr if this isn't an object or the key is absent.
    const JsonValue* find(const std::string& key) const;

    // Returns stringValue if type is String, otherwise fallback — never throws.
    std::string asString(const std::string& fallback = "") const;

    bool isString() const;
    bool isArray() const;
    bool isObject() const;
};

// Parses text as a single JSON document. Returns std::nullopt on any malformed input
// (including trailing garbage after a valid document) — never throws, never crashes on
// untrusted input.
std::optional<JsonValue> parseJson(const std::string& text);
