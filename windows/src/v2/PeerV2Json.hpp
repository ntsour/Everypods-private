#pragma once

// Strict JSON for EveryPods peer protocol v2, matching Android's
// PeerV2Message.JsonParser/JsonWriter: objects, arrays, strings, booleans,
// null and 64-bit integers only. Duplicate keys, fractions/exponents,
// leading zeros, control characters, invalid UTF-8, trailing bytes and
// nesting deeper than 32 all fail. Errors throw std::invalid_argument with
// the same short codes Android uses.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Propods::v2 {

struct Json;
using JsonArray = std::vector<Json>;
using JsonObject = std::vector<std::pair<std::string, Json>>;  // insertion order, unique keys

struct Json {
    std::variant<std::nullptr_t, bool, std::int64_t, std::string, JsonArray, JsonObject> value;

    Json() : value(nullptr) {}
    Json(std::nullptr_t) : value(nullptr) {}
    Json(bool b) : value(b) {}
    Json(std::int64_t n) : value(n) {}
    Json(int n) : value(static_cast<std::int64_t>(n)) {}
    Json(std::string s) : value(std::move(s)) {}
    Json(const char* s) : value(std::string(s)) {}
    Json(JsonArray a) : value(std::move(a)) {}
    Json(JsonObject o) : value(std::move(o)) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(value); }
    const std::string* string() const { return std::get_if<std::string>(&value); }
    const std::int64_t* integer() const { return std::get_if<std::int64_t>(&value); }
    const bool* boolean() const { return std::get_if<bool>(&value); }
    const JsonArray* array() const { return std::get_if<JsonArray>(&value); }
    const JsonObject* object() const { return std::get_if<JsonObject>(&value); }
};

// Object field lookup; nullptr when absent.
const Json* field(const JsonObject& object, std::string_view key);

Json parseJson(std::string_view utf8);
std::string writeJson(const Json& value);

}  // namespace Propods::v2
