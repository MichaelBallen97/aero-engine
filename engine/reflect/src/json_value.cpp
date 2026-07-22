// engine/reflect/src/json_value.cpp — task 1.2.2: JsonValue accessors + the D6 number-conversion
// routing (spec §3.6, evidence F2-F5). Numbers are stored as a validated lexeme (json_reader.cpp's
// job) and converted here, at ACCESS time, at the caller's target precision — never pre-parsed to a
// double, which loses one ULP on some float lexemes (F4) and cannot represent the full uint64_t
// range (2^53).
#include <aero/reflect/json_value.hpp>

#include <cerrno>
#include <charconv>
#include <cmath>
#include <system_error>
#include <type_traits>
#include <utility>

// clocale/cstdlib (NOT locale.h/stdlib.h, modernize-deprecated-headers, live under this repo's
// .clang-tidy): newlocale/_create_locale/LC_ALL_MASK and strtof_l/_strtof_l are POSIX/UCRT
// EXTENSIONS, not ISO C, so this relies on libstdc++'s <clocale> transitively including <locale.h>
// (true in practice on glibc) and UCRT's <clocale>/<cstdlib> exposing the same underscore-prefixed
// extensions as <locale.h>/<stdlib.h> (also true in practice). If a future libc ever stops doing
// that, the fix is a per-host targeted include that still avoids the deprecated spelling — not a
// design change here.
#if defined(_WIN32)
    #include <clocale>  // _create_locale
    #include <cstdlib>  // _strtof_l, _strtod_l
#elif defined(__APPLE__)
    #include <xlocale.h>  // newlocale, strtof_l, strtod_l (not on the deprecated-headers list)
#else
    #include <clocale>  // newlocale (glibc)
    #include <cstdlib>  // strtof_l, strtod_l (glibc, _GNU_SOURCE — g++ predefines it, F3)
#endif

namespace engine {

namespace {

#if defined(_WIN32)
using CLocale = _locale_t;
#else
using CLocale = locale_t;
#endif

// A process-lifetime "C" locale, created once on first use and NEVER freed (D6): the fallback must
// be immune to a host application's setlocale(LC_NUMERIC, ...) call (E-locale), and there is nothing
// meaningful to release it into — the static holds it for the process's whole lifetime by design.
// May legitimately return a null handle on allocation failure; callers MUST check before use (POSIX
// leaves strtof_l/strtod_l undefined on an invalid locale_t, and UCRT silently substitutes the
// CURRENT locale for a null _locale_t — silently defeating the whole point of this fallback).
CLocale cLocale() {
#if defined(_WIN32)
    static const CLocale loc = _create_locale(LC_ALL, "C");
#else
    static const CLocale loc = newlocale(LC_ALL_MASK, "C", nullptr);
#endif
    return loc;
}

// The C-library fallback (compiled on every host; the ONLY path on Apple, F2). Rule (F5/D6):
// ERANGE + a +/-HUGE_VAL[F] result => overflow (nullopt); anything else — subnormal, zero, normal —
// is an accepted value. errno is cleared AFTER cLocale() so the one-time locale-creation call can
// never leave a stale errno in the window this function reads.
std::optional<float> fallbackParseF32(const std::string& lexeme) {
    const CLocale loc = cLocale();
    if (loc == nullptr) {
        return std::nullopt;  // allocation failure; never fall back to a wrong/current locale
    }
    errno = 0;
    char* end = nullptr;
#if defined(_WIN32)
    const float value = _strtof_l(lexeme.c_str(), &end, loc);
#else
    const float value = strtof_l(lexeme.c_str(), &end, loc);
#endif
    if (end != lexeme.c_str() + lexeme.size()) {
        return std::nullopt;  // defensive; unreachable on a validated lexeme
    }
    if (errno == ERANGE && std::isinf(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> fallbackParseF64(const std::string& lexeme) {
    const CLocale loc = cLocale();
    if (loc == nullptr) {
        return std::nullopt;
    }
    errno = 0;
    char* end = nullptr;
#if defined(_WIN32)
    const double value = _strtod_l(lexeme.c_str(), &end, loc);
#else
    const double value = strtod_l(lexeme.c_str(), &end, loc);
#endif
    if (end != lexeme.c_str() + lexeme.size()) {
        return std::nullopt;
    }
    if (errno == ERANGE && std::isinf(value)) {
        return std::nullopt;
    }
    return value;
}

// Correctly-rounded, locale-independent decimal -> FP at TARGET precision (D6). nullopt <=> the
// value rounds to +/-infinity.
std::optional<float> parseF32(const std::string& lexeme) {
#if defined(__cpp_lib_to_chars)
    float value = 0.0F;
    const char* begin = lexeme.data();
    const char* end = lexeme.data() + lexeme.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec == std::errc{}) {
        // Floating-point from_chars matches the LONGEST valid prefix, not necessarily the whole
        // input -- symmetric with the C fallback's own full-consumption check below, so a caller
        // constructing a JsonValue::number() outside the (grammar-validating) parser gets the same
        // answer on every lane (1.2.3's scene loader will do exactly that).
        if (result.ptr != end) {
            return std::nullopt;
        }
        return value;
    }
    if (result.ec != std::errc::result_out_of_range) {
        return std::nullopt;  // defensive; unreachable on a validated lexeme
    }
    // result_out_of_range: fall through to the C fallback (unifies underflow classification, F5).
#endif
    return fallbackParseF32(lexeme);
}

std::optional<double> parseF64(const std::string& lexeme) {
#if defined(__cpp_lib_to_chars)
    double value = 0.0;
    const char* begin = lexeme.data();
    const char* end = lexeme.data() + lexeme.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec == std::errc{}) {
        if (result.ptr != end) {
            return std::nullopt;
        }
        return value;
    }
    if (result.ec != std::errc::result_out_of_range) {
        return std::nullopt;
    }
#endif
    return fallbackParseF64(lexeme);
}

}  // namespace

std::string_view jsonKindName(JsonKind kind) {
    switch (kind) {
        case JsonKind::Null:
            return "null";
        case JsonKind::Bool:
            return "bool";
        case JsonKind::Number:
            return "number";
        case JsonKind::String:
            return "string";
        case JsonKind::Array:
            return "array";
        case JsonKind::Object:
            return "object";
    }
    return "unknown";  // unreachable; satisfies -Wreturn-type
}

JsonValue JsonValue::null() { return JsonValue(); }

JsonValue JsonValue::boolean(bool v) {
    JsonValue result;
    result.data = v;
    return result;
}

JsonValue JsonValue::number(std::string lexeme) {
    JsonValue result;
    result.data = JsonNumber{std::move(lexeme)};
    return result;
}

JsonValue JsonValue::string(std::string v) {
    JsonValue result;
    result.data = std::move(v);
    return result;
}

JsonValue JsonValue::array(std::vector<JsonValue> elements) {
    JsonValue result;
    result.data = std::move(elements);
    return result;
}

JsonValue JsonValue::object(std::vector<JsonMember> members) {
    JsonValue result;
    result.data = std::move(members);
    return result;
}

JsonKind JsonValue::kind() const {
    // Variant-index <-> JsonKind pin (D4): a reordering of either breaks the BUILD, not runtime
    // behavior. F6-verified NOT misc-no-recursion-flagged (the DOM shape, not this function).
    static_assert(std::is_same_v<std::variant_alternative_t<0, decltype(data)>, std::monostate>);
    static_assert(std::is_same_v<std::variant_alternative_t<1, decltype(data)>, bool>);
    static_assert(std::is_same_v<std::variant_alternative_t<2, decltype(data)>, JsonNumber>);
    static_assert(std::is_same_v<std::variant_alternative_t<3, decltype(data)>, std::string>);
    static_assert(std::is_same_v<std::variant_alternative_t<4, decltype(data)>, std::vector<JsonValue>>);
    static_assert(std::is_same_v<std::variant_alternative_t<5, decltype(data)>, std::vector<JsonMember>>);
    static_assert(static_cast<std::size_t>(JsonKind::Null) == 0);
    static_assert(static_cast<std::size_t>(JsonKind::Bool) == 1);
    static_assert(static_cast<std::size_t>(JsonKind::Number) == 2);
    static_assert(static_cast<std::size_t>(JsonKind::String) == 3);
    static_assert(static_cast<std::size_t>(JsonKind::Array) == 4);
    static_assert(static_cast<std::size_t>(JsonKind::Object) == 5);
    return static_cast<JsonKind>(data.index());
}

bool JsonValue::isNull() const { return kind() == JsonKind::Null; }
bool JsonValue::isBool() const { return kind() == JsonKind::Bool; }
bool JsonValue::isNumber() const { return kind() == JsonKind::Number; }
bool JsonValue::isString() const { return kind() == JsonKind::String; }
bool JsonValue::isArray() const { return kind() == JsonKind::Array; }
bool JsonValue::isObject() const { return kind() == JsonKind::Object; }

std::optional<bool> JsonValue::asBool() const {
    if (const bool* value = std::get_if<bool>(&data)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<float> JsonValue::asF32() const {
    if (kind() != JsonKind::Number) {
        return std::nullopt;
    }
    return parseF32(std::get<JsonNumber>(data).lexeme);
}

std::optional<double> JsonValue::asF64() const {
    if (kind() != JsonKind::Number) {
        return std::nullopt;
    }
    return parseF64(std::get<JsonNumber>(data).lexeme);
}

std::optional<std::int64_t> JsonValue::asI64() const {
    if (kind() != JsonKind::Number) {
        return std::nullopt;
    }
    const std::string& lexeme = std::get<JsonNumber>(data).lexeme;
    if (lexeme.find_first_of(".eE") != std::string::npos) {
        return std::nullopt;  // 2.5 / 1e2 are not integers (D5)
    }
    std::int64_t result = 0;
    const char* begin = lexeme.data();
    const char* end = lexeme.data() + lexeme.size();
    const auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::uint64_t> JsonValue::asU64() const {
    if (kind() != JsonKind::Number) {
        return std::nullopt;
    }
    const std::string& lexeme = std::get<JsonNumber>(data).lexeme;
    if (lexeme.find_first_of(".eE") != std::string::npos) {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    const char* begin = lexeme.data();
    const char* end = lexeme.data() + lexeme.size();
    const auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::string_view> JsonValue::asString() const {
    if (const std::string* value = std::get_if<std::string>(&data)) {
        return std::string_view(*value);
    }
    return std::nullopt;
}

std::string_view JsonValue::numberLexeme() const {
    if (const JsonNumber* value = std::get_if<JsonNumber>(&data)) {
        return value->lexeme;
    }
    return {};
}

std::size_t JsonValue::size() const {
    if (const auto* arr = std::get_if<std::vector<JsonValue>>(&data)) {
        return arr->size();
    }
    if (const auto* obj = std::get_if<std::vector<JsonMember>>(&data)) {
        return obj->size();
    }
    return 0;
}

const JsonValue* JsonValue::at(std::size_t index) const {
    const auto* arr = std::get_if<std::vector<JsonValue>>(&data);
    if (arr == nullptr || index >= arr->size()) {
        return nullptr;
    }
    return &(*arr)[index];
}

const JsonValue* JsonValue::find(std::string_view key) const {
    const auto* obj = std::get_if<std::vector<JsonMember>>(&data);
    if (obj == nullptr) {
        return nullptr;
    }
    for (const JsonMember& member : *obj) {
        if (member.key == key) {
            return &member.value;
        }
    }
    return nullptr;
}

const std::vector<JsonValue>& JsonValue::elements() const {
    if (const auto* arr = std::get_if<std::vector<JsonValue>>(&data)) {
        return *arr;
    }
    static const std::vector<JsonValue> empty;
    return empty;
}

const std::vector<JsonMember>& JsonValue::members() const {
    if (const auto* obj = std::get_if<std::vector<JsonMember>>(&data)) {
        return *obj;
    }
    static const std::vector<JsonMember> empty;
    return empty;
}

}  // namespace engine
