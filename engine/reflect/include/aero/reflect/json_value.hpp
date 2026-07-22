#pragma once
// engine/reflect/include/aero/reflect/json_value.hpp — task 1.2.2: the immutable JSON DOM (spec
// §3.5, D4/D5). Numbers are stored as the validated JSON lexeme, verbatim, and converted at ACCESS
// time at the caller's target precision (D5/D6) — never pre-parsed to a double, which would break
// float exactness (F4) and 64-bit integer round-trips (2^53). Namespace is plain engine:: (matching
// JsonWriter), not engine::reflect:: — the leaf functions that consume this DOM live there instead.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace engine {

enum class JsonKind : std::uint8_t { Null, Bool, Number, String, Array, Object };

// "null"/"bool"/"number"/"string"/"array"/"object" — NEVER named toString: an ADL toString(Enum)
// breaks doctest CHECK compilation (the 0.4.1 trap, F11/D17).
[[nodiscard]] std::string_view jsonKindName(JsonKind kind);

// A distinct wrapper (not a bare std::string) so Number and String are different variant
// alternatives rather than two std::strings colliding on the same slot.
struct JsonNumber {
    std::string lexeme;  // the validated JSON number token, verbatim (D5)
};

struct JsonMember;  // { std::string key; JsonValue value; } — defined after JsonValue (recursion)

// Immutable after construction (D4): the parser builds children bottom-up, then wraps them via the
// array()/object() factories. Accessors are CHECKED, never asserting — untrusted input must never
// fire an assert (D16); a kind mismatch returns std::nullopt / nullptr / an empty static container.
//
// The NOLINT below covers the COMPILER-GENERATED special members only. JsonValue holds
// std::vector<JsonValue> / std::vector<JsonMember>, so its implicit copy/move/destroy are mutually
// recursive by construction — libstdc++ routes std::variant's copy through a visit chain, which
// misc-no-recursion reports (libc++ structures it differently, so this is invisible on macOS).
// That is inherent to a recursive tree type, not the hand-written recursion the check exists to
// police: the parser, the DOM re-emitter and the scene parent-chain walk are all deliberately
// iterative and remain fully checked. Task 1.2.3 was the first code to copy a JsonValue.
// NOLINTNEXTLINE(misc-no-recursion)
class JsonValue {
public:
    JsonValue() = default;  // Null

    static JsonValue null();
    static JsonValue boolean(bool v);
    static JsonValue number(std::string lexeme);  // caller supplies a JSON-grammar-valid lexeme
    static JsonValue string(std::string v);       // already-unescaped bytes
    static JsonValue array(std::vector<JsonValue> elements);
    static JsonValue object(std::vector<JsonMember> members);

    [[nodiscard]] JsonKind kind() const;
    [[nodiscard]] bool isNull() const;
    [[nodiscard]] bool isBool() const;
    [[nodiscard]] bool isNumber() const;
    [[nodiscard]] bool isString() const;
    [[nodiscard]] bool isArray() const;
    [[nodiscard]] bool isObject() const;

    [[nodiscard]] std::optional<bool> asBool() const;
    [[nodiscard]] std::optional<float> asF32() const;  // D6 routing; nullopt <=> rounds to +/-inf
    [[nodiscard]] std::optional<double> asF64() const;
    [[nodiscard]] std::optional<std::int64_t> asI64() const;  // integral-form lexemes only (D5)
    [[nodiscard]] std::optional<std::uint64_t> asU64() const;
    [[nodiscard]] std::optional<std::string_view> asString() const;
    [[nodiscard]] std::string_view numberLexeme() const;  // "" when not a Number

    [[nodiscard]] std::size_t size() const;                           // array/object element count; 0 otherwise
    [[nodiscard]] const JsonValue* at(std::size_t index) const;       // nullptr if OOB or not an array
    [[nodiscard]] const JsonValue* find(std::string_view key) const;  // nullptr if absent or not an object
    [[nodiscard]] const std::vector<JsonValue>& elements() const;     // a static empty vector when not an array
    [[nodiscard]] const std::vector<JsonMember>& members() const;     // a static empty vector when not an object

private:
    // Variant index <-> JsonKind order is pinned by a static_assert block in json_value.cpp.
    std::variant<std::monostate, bool, JsonNumber, std::string, std::vector<JsonValue>, std::vector<JsonMember>> data;
};

// NOLINTNEXTLINE(misc-no-recursion) — see the note on JsonValue: implicit special members only.
struct JsonMember {
    std::string key;
    JsonValue value;
};

}  // namespace engine
