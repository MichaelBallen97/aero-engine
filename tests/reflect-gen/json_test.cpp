// tests/reflect-gen/json_test.cpp — task 1.2.1/1.2.2: the runtime proof that generated JSON
// serializers produce exact, ordered, round-trippable JSON (AC-10/AC-11) AND that the hand-rolled
// parser/DOM/read-runtime (task 1.2.2) satisfy the full grammar/policy/round-trip contract (AC-7/8/9).
// Standalone single-TU doctest (own DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN, no shared test_main.cpp),
// sibling of aero_reflect_meta_test but WITHOUT EnTT — serialization is meta-independent (D4).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <aero/core/math.hpp>
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>
#include <aero/reflect/serialize.hpp>

#include "component_codegen.hpp"
#include "component_limits.hpp"
#include "component_multi.hpp"
#include "component_tag.hpp"
#include "component_wiring.hpp"

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// GENERATED serializers (aero_reflect_generate_json over the five headers; aeroWriteJson/aeroReadJson in
// the component's namespace, D7). camelBack => no NOLINT.
void aeroWriteJson(engine::JsonWriter&, const ReflectSample&);
void aeroWriteJson(engine::JsonWriter&, const ReflectWiring&);
void aeroWriteJson(engine::JsonWriter&, const ReflectLimits&);
void aeroWriteJson(engine::JsonWriter&, const Tag&);
void aeroWriteJson(engine::JsonWriter&, const Player&);
namespace engine::demo {
void aeroWriteJson(engine::JsonWriter&, const Light&);
}

bool aeroReadJson(const engine::JsonValue&, ReflectSample&);
bool aeroReadJson(const engine::JsonValue&, ReflectWiring&);
bool aeroReadJson(const engine::JsonValue&, ReflectLimits&);
bool aeroReadJson(const engine::JsonValue&, Tag&);
bool aeroReadJson(const engine::JsonValue&, Player&);
namespace engine::demo {
bool aeroReadJson(const engine::JsonValue&, Light&);
}

TEST_CASE("generated serializer emits exact, ordered JSON (AC-10)") {
    ReflectSample s{};
    s.position = {1.0f, 2.0f, 3.0f};
    s.rotation = engine::Quat::identity();  // {0,0,0,1}
    s.mass = 2.5f;
    s.hitPoints = 42;
    s.active = true;
    // velocity left default (Vec4, unsupported -> must be absent)

    engine::JsonWriter w;
    aeroWriteJson(w, s);

    const std::string expected =
        "{\n"
        "  \"position\": {\n"
        "    \"x\": 1,\n"
        "    \"y\": 2,\n"
        "    \"z\": 3\n"
        "  },\n"
        "  \"rotation\": {\n"
        "    \"x\": 0,\n"
        "    \"y\": 0,\n"
        "    \"z\": 0,\n"
        "    \"w\": 1\n"
        "  },\n"
        "  \"mass\": 2.5,\n"
        "  \"hitPoints\": 42,\n"
        "  \"active\": true\n"
        "}";  // no trailing newline
    CHECK(w.str() == expected);
    CHECK(w.str().find("velocity") == std::string::npos);        // unsupported -> absent
    CHECK(w.str().find("SCHEMA_VERSION") == std::string::npos);  // static -> absent
}

TEST_CASE("namespaced (double) + unsigned serialize correctly (AC-10)") {
    engine::demo::Light light{};
    light.color = {0.5f, 0.5f, 0.5f};
    light.intensity = 4.0;  // double
    engine::JsonWriter wl;
    aeroWriteJson(wl, light);  // resolves engine::demo::aeroWriteJson (fwd-declared)
    CHECK(wl.str().find("\"color\"") != std::string::npos);
    CHECK(wl.str().find("\"intensity\": 4") != std::string::npos);

    ReflectWiring rw{};
    rw.target = {0.0f, 0.0f, 0.0f};
    rw.speed = 1.5f;
    rw.gear = 7U;
    rw.engaged = false;
    engine::JsonWriter ww;
    aeroWriteJson(ww, rw);
    CHECK(ww.str().find("\"gear\": 7") != std::string::npos);  // uint32 -> unsigned long long
    CHECK(ww.str().find("\"engaged\": false") != std::string::npos);
}

namespace {

bool isJsonNumberChar(char c) {
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

// Finds `"key": <number>` inside `text` and returns the number's lexeme. Scoping the search to a sub-block
// (e.g. the "position" object's own text) disambiguates field names that repeat across nested objects (both
// position and rotation have an "x").
std::string extractNumber(const std::string& text, const std::string& key) {
    const std::string marker = "\"" + key + "\": ";
    const std::size_t at = text.find(marker);
    REQUIRE(at != std::string::npos);
    const std::size_t start = at + marker.size();
    std::size_t end = start;
    while (end < text.size() && isJsonNumberChar(text[end])) {
        ++end;
    }
    return text.substr(start, end - start);
}

// Builds the JSON text  "\uXXXX..."  (real quotes, real backslashes) from 4-hex-digit code units,
// plus an optional trailing suffix inside the quotes.
//
// This CANNOT be written as a source string literal, because the three lanes disagree irreconcilably:
//   - a raw literal   R"("\ud83d")"    -- MSVC forms universal-character-names inside raw string
//     literals and rejects a lone surrogate: "C3850: a universal-character-name specifies an invalid
//     character". Clang/GCC accept it, so a raw literal reds ONLY Windows.
//   - an escaped one  "\"\\ud83d\""    -- clang-tidy's modernize-raw-string-literal (live under this
//     repo's .clang-tidy) rewrites it back into the raw form, so it reds ONLY the Linux lint lane.
// Char literals are invisible to both checks, so assembling the token is the one portable option.
std::string jsonUnicodeToken(std::initializer_list<std::string_view> codeUnits, std::string_view suffix = {}) {
    std::string token(1, '"');
    for (const std::string_view unit : codeUnits) {
        token += '\\';
        token += 'u';
        token += unit;
    }
    token += suffix;
    token += '"';
    return token;
}

// std::strtof (NOT std::from_chars<float>) -- floating-point std::from_chars is unavailable in the pinned
// macosx15.4 SDK's libc++ (verified: <charconv> there includes only from_chars_integral.h, no floating-point
// overload exists to call). strtof is portable across all three toolchains and is only used HERE, in the
// test's own round-trip verification -- the writer itself (json_writer.cpp) uses std::to_chars exclusively.
float parseFloatLexeme(const std::string& lexeme) { return std::strtof(lexeme.c_str(), nullptr); }

}  // namespace

TEST_CASE("every emitted float/double lexeme round-trips bit-exactly (writer-side, AC-10)") {
    // Serialize s, scan each numeric lexeme by key, strtof it back, compare bit-exactly. Proves the writer is
    // round-trippable without shipping the 1.2.2 reader (F2).
    ReflectSample s{};
    s.position = {1.5f, -2.25f, 0.1f};
    s.rotation = engine::Quat{0.0f, 0.70710678f, 0.0f, 0.70710678f};
    s.mass = 123.456f;
    s.hitPoints = -7;
    s.active = false;

    engine::JsonWriter w;
    aeroWriteJson(w, s);
    const std::string& text = w.str();

    const std::size_t positionBlockStart = text.find("\"position\": {");
    REQUIRE(positionBlockStart != std::string::npos);
    const std::size_t positionBlockEnd = text.find('}', positionBlockStart);
    const std::string positionBlock = text.substr(positionBlockStart, positionBlockEnd - positionBlockStart);

    const std::size_t rotationBlockStart = text.find("\"rotation\": {");
    REQUIRE(rotationBlockStart != std::string::npos);
    const std::size_t rotationBlockEnd = text.find('}', rotationBlockStart);
    const std::string rotationBlock = text.substr(rotationBlockStart, rotationBlockEnd - rotationBlockStart);

    const auto parsedPositionX = parseFloatLexeme(extractNumber(positionBlock, "x"));
    const auto parsedPositionY = parseFloatLexeme(extractNumber(positionBlock, "y"));
    const auto parsedPositionZ = parseFloatLexeme(extractNumber(positionBlock, "z"));
    const auto parsedRotationX = parseFloatLexeme(extractNumber(rotationBlock, "x"));
    const auto parsedRotationY = parseFloatLexeme(extractNumber(rotationBlock, "y"));
    const auto parsedRotationZ = parseFloatLexeme(extractNumber(rotationBlock, "z"));
    const auto parsedRotationW = parseFloatLexeme(extractNumber(rotationBlock, "w"));
    const auto parsedMass = parseFloatLexeme(extractNumber(text, "mass"));

    CHECK(parsedPositionX == s.position.x);
    CHECK(parsedPositionY == s.position.y);
    CHECK(parsedPositionZ == s.position.z);
    CHECK(parsedRotationX == s.rotation.x);
    CHECK(parsedRotationY == s.rotation.y);
    CHECK(parsedRotationZ == s.rotation.z);
    CHECK(parsedRotationW == s.rotation.w);
    CHECK(parsedMass == s.mass);
}

TEST_CASE("JsonWriter units: escaping, nesting, arrays, bool/int/null, non-finite, compact, indent widths (AC-11)") {
    using engine::JsonWriter;
    using engine::JsonWriterConfig;
    // escaping
    {
        JsonWriter w;
        w.beginObject();
        w.key("q\"\\\n\t\x01");
        w.value(true);
        w.endObject();
        CHECK(w.str().find("\\\"") != std::string::npos);
        CHECK(w.str().find("\\u0001") != std::string::npos);
    }
    // arrays + ints + null
    {
        JsonWriter w;
        w.beginArray();
        w.value(1LL);
        w.value(2ULL);
        w.valueNull();
        w.endArray();
        CHECK(w.str().find('1') != std::string::npos);
        CHECK(w.str().find('2') != std::string::npos);
        CHECK(w.str().find("null") != std::string::npos);
    }
    // non-finite -> null
    {
        JsonWriter w;
        w.value(std::nanf(""));
        CHECK(w.str() == "null");
    }
    {
        JsonWriter w;
        w.value(std::numeric_limits<double>::infinity());
        CHECK(w.str() == "null");
    }
    // compact vs pretty
    {
        JsonWriter w{JsonWriterConfig{.pretty = false}};
        w.beginObject();
        w.key("a");
        w.value(1LL);
        w.endObject();
        CHECK(w.str() == "{\"a\":1}");
    }
    // empty object
    {
        JsonWriter w;
        w.beginObject();
        w.endObject();
        CHECK(w.str() == "{}");
    }
    // non-default indent widths; a negative width clamps to 0 instead of wrapping the size_t
    // multiply in writeIndent() into a throwing append (1.2 audit follow-up)
    {
        JsonWriter w{JsonWriterConfig{.indentWidth = 4}};
        w.beginObject();
        w.key("a");
        w.value(1LL);
        w.endObject();
        CHECK(w.str() == "{\n    \"a\": 1\n}");
    }
    {
        JsonWriter w{JsonWriterConfig{.indentWidth = 0}};
        w.beginObject();
        w.key("a");
        w.value(1LL);
        w.endObject();
        CHECK(w.str() == "{\n\"a\": 1\n}");
    }
    {
        JsonWriter w{JsonWriterConfig{.indentWidth = -3}};
        w.beginObject();
        w.key("a");
        w.value(1LL);
        w.endObject();
        CHECK(w.str() == "{\n\"a\": 1\n}");  // identical to indentWidth 0
    }
}

// ---- task 1.2.2: engine::parseJson / engine::JsonValue (the reader half) ----------------------------

TEST_CASE(
    "parser: accept battery -- scalars, nesting, escapes, surrogate pairs, compact/pretty, CRLF, BOM, "
    "dup keys (AC-7)") {
    // every scalar kind at the root (REQUIRE-then-deref: a regression fails cleanly instead of
    // dereferencing an empty optional -- 1.2 audit D4 pass; the tests-local .clang-tidy disables
    // bugprone-unchecked-optional-access, so the guard has to be a runtime one)
    {
        const engine::JsonParseResult r = engine::parseJson("null");
        REQUIRE(r.ok());
        CHECK(r.value->isNull());
    }
    {
        const engine::JsonParseResult r = engine::parseJson("true");
        REQUIRE(r.ok());
        const auto b = r.value->asBool();
        REQUIRE(b.has_value());
        CHECK(*b);
    }
    {
        const engine::JsonParseResult r = engine::parseJson("false");
        REQUIRE(r.ok());
        const auto b = r.value->asBool();
        REQUIRE(b.has_value());
        CHECK_FALSE(*b);
    }
    {
        const engine::JsonParseResult r = engine::parseJson("42");
        REQUIRE(r.ok());
        CHECK(r.value->asI64() == 42);
    }
    {
        const engine::JsonParseResult r = engine::parseJson("-3.5");
        REQUIRE(r.ok());
        CHECK(r.value->asF64() == -3.5);
    }
    {
        const engine::JsonParseResult r = engine::parseJson("\"hi\"");
        REQUIRE(r.ok());
        const auto s = r.value->asString();
        REQUIRE(s.has_value());
        CHECK(*s == "hi");
    }

    // nested objects/arrays (pointer chain bound and REQUIREd link by link -- D4 pass)
    {
        const engine::JsonParseResult r = engine::parseJson(R"({"a":[1,{"b":2}]})");
        REQUIRE(r.ok());
        const engine::JsonValue* a = r.value->find("a");
        REQUIRE(a != nullptr);
        const engine::JsonValue* first = a->at(0);
        REQUIRE(first != nullptr);
        CHECK(first->asI64() == 1);
        const engine::JsonValue* second = a->at(1);
        REQUIRE(second != nullptr);
        const engine::JsonValue* b = second->find("b");
        REQUIRE(b != nullptr);
        CHECK(b->asI64() == 2);
    }

    // full escape set incl. \uXXXX
    {
        const engine::JsonParseResult r = engine::parseJson(R"("\"\\\/\b\f\n\r\tA")");
        REQUIRE(r.ok());
        const auto s = r.value->asString();
        REQUIRE(s.has_value());
        CHECK(*s == "\"\\/\b\f\n\r\tA");
    }
    // REAL \uXXXX escapes (literal backslash-u text the parser must decode) exercising appendUtf8's
    // 2-byte, 3-byte, and (via a genuine surrogate PAIR) 4-byte branches -- lexHex4 + the
    // high/low-surrogate recombination path in lexString, none of which a raw-byte literal reaches.
    // Tokens come from jsonUnicodeToken() because NEITHER source literal form is portable here
    // (see that helper's comment).
    {
        const engine::JsonParseResult r2 = engine::parseJson(jsonUnicodeToken({"00e9"}));  // U+00E9, 2-byte
        REQUIRE(r2.ok());
        const auto s = r2.value->asString();
        REQUIRE(s.has_value());
        CHECK(*s == "\xC3\xA9");
    }
    {
        const engine::JsonParseResult r3 = engine::parseJson(jsonUnicodeToken({"20ac"}));  // U+20AC, 3-byte
        REQUIRE(r3.ok());
        const auto s = r3.value->asString();
        REQUIRE(s.has_value());
        CHECK(*s == "\xE2\x82\xAC");
    }
    {
        // U+1F600 "grinning face": no single \uXXXX can name it (UTF-16 has no codepoint that high),
        // so a genuine high+low surrogate PAIR is the only way JSON can express it.
        const engine::JsonParseResult r4 = engine::parseJson(jsonUnicodeToken({"d83d", "de00"}));
        REQUIRE(r4.ok());
        const auto s = r4.value->asString();
        REQUIRE(s.has_value());
        CHECK(*s == "\xF0\x9F\x98\x80");
    }
    // malformed \u escapes: an invalid low surrogate, a high surrogate with no following \u escape,
    // and a bad hex digit -- all rejected (AC-7)
    CHECK_FALSE(engine::parseJson(jsonUnicodeToken({"d83d"}, "A")).ok());  // invalid low surrogate
    CHECK_FALSE(engine::parseJson(jsonUnicodeToken({"d83d"}, "x")).ok());  // no second \u escape
    CHECK_FALSE(engine::parseJson(jsonUnicodeToken({"00zz"})).ok());       // 'z' is not a hex digit

    // raw SOURCE-EMBEDDED UTF-8 (D7 tolerance 3: non-key string content is not UTF-8-validated, so a
    // multi-byte sequence the parser never decoded as an escape passes straight through byte-for-
    // byte) -- a genuinely different code path from every \uXXXX case above (appendUtf8 is never
    // called here at all).
    {
        const engine::JsonParseResult r5 = engine::parseJson(R"("é")");  // 2-byte, passthrough
        REQUIRE(r5.ok());
        const auto s = r5.value->asString();
        REQUIRE(s.has_value());
        CHECK(*s == "\xC3\xA9");
    }
    {
        const engine::JsonParseResult r6 = engine::parseJson(R"("€")");  // 3-byte, passthrough
        REQUIRE(r6.ok());
        const auto s = r6.value->asString();
        REQUIRE(s.has_value());
        CHECK(*s == "\xE2\x82\xAC");
    }
    {
        const engine::JsonParseResult r7 = engine::parseJson(R"("😀")");  // 4-byte, passthrough
        REQUIRE(r7.ok());
        const auto s = r7.value->asString();
        REQUIRE(s.has_value());
        CHECK(*s == "\xF0\x9F\x98\x80");
    }

    // writer-escaped control chars recovered byte-for-byte through the REAL writer
    {
        std::string original;
        original += 'a';
        original += static_cast<char>(0x01);
        original += static_cast<char>(0x1F);
        original += 'b';
        engine::JsonWriter w;
        w.value(std::string_view(original));
        const engine::JsonParseResult r = engine::parseJson(w.str());
        REQUIRE(r.ok());
        const auto s = r.value->asString();
        REQUIRE(s.has_value());
        CHECK(*s == original);
    }

    // compact and pretty input; CRLF; a single leading UTF-8 BOM
    CHECK(engine::parseJson(R"({"a":1,"b":2})").ok());
    CHECK(engine::parseJson("{\n  \"a\": 1,\n  \"b\": 2\n}").ok());
    CHECK(engine::parseJson("{\r\n  \"a\": 1\r\n}").ok());
    CHECK(engine::parseJson("\xEF\xBB\xBF{\"a\":1}").ok());

    // root scalars accepted (already exercised above); duplicate keys, last-wins, one entry
    {
        const engine::JsonParseResult r = engine::parseJson(R"({"a":1,"a":2})");
        REQUIRE(r.ok());
        CHECK(r.value->size() == 1);
        const engine::JsonValue* a = r.value->find("a");
        REQUIRE(a != nullptr);
        CHECK(a->asI64() == 2);
    }
}

TEST_CASE("parser: reject battery -- malformed input, positions pinned for a few cases (AC-7)") {
    CHECK_FALSE(engine::parseJson("").ok());
    CHECK_FALSE(engine::parseJson("   ").ok());
    CHECK_FALSE(engine::parseJson("{} garbage").ok());
    CHECK_FALSE(engine::parseJson("\"unterminated").ok());
    CHECK_FALSE(engine::parseJson(R"("\z")").ok());                   // bad escape
    CHECK_FALSE(engine::parseJson(jsonUnicodeToken({"d800"})).ok());  // lone high surrogate
    CHECK_FALSE(engine::parseJson(jsonUnicodeToken({"dc00"})).ok());  // lone low surrogate
    {
        std::string raw;
        raw += '"';
        raw += static_cast<char>(0x01);
        raw += '"';
        CHECK_FALSE(engine::parseJson(raw).ok());  // raw control char inside a string
    }
    for (const char* lexeme : {"01", "+1", ".5", "5.", "1e", "1e+", "-", "0x1", "NaN", "Infinity"}) {
        CAPTURE(lexeme);
        CHECK_FALSE(engine::parseJson(lexeme).ok());
    }
    CHECK_FALSE(engine::parseJson("[1,2").ok());              // unbalanced (missing ']')
    CHECK_FALSE(engine::parseJson("[1,2]]").ok());            // mismatched / trailing garbage
    CHECK_FALSE(engine::parseJson("[1,2}").ok());             // mismatched close bracket ('}' for '[')
    CHECK_FALSE(engine::parseJson("[1 2]").ok());             // missing ',' between array elements
    CHECK_FALSE(engine::parseJson(R"({"a" 1})").ok());        // missing ':'
    CHECK_FALSE(engine::parseJson(R"({"a":1 "b":2})").ok());  // missing ','
    CHECK_FALSE(engine::parseJson("{'a':1}").ok());           // single-quoted
    CHECK_FALSE(engine::parseJson("[1,]").ok());              // trailing comma (array)
    CHECK_FALSE(engine::parseJson(R"({"a":1,})").ok());       // trailing comma (object)
    CHECK_FALSE(engine::parseJson("{\"a\":1 // c\n}").ok());  // comment

    // exact position pins: 1-based line, 1-based byte column, 0-based offset (the json_reader.hpp
    // contract; values derived from the input bytes -- 1.2 audit D8 pass, previously only `line`
    // was ever pinned)
    {
        const engine::JsonParseResult r = engine::parseJson("{\"a\":1,\n  \"b\" 2}");
        REQUIRE_FALSE(r.ok());
        CHECK(r.error.message == "expected ':' after object key");
        CHECK(r.error.line == 2);
        CHECK(r.error.column == 7);   // the '2' is byte 7 of line 2 ("  \"b\" 2}")
        CHECK(r.error.offset == 14);  // and byte 14 (0-based) of the whole input
    }
    {
        const engine::JsonParseResult r = engine::parseJson(R"({"a" 1})");
        REQUIRE_FALSE(r.ok());
        CHECK(r.error.message == "expected ':' after object key");
        CHECK(r.error.line == 1);
        CHECK(r.error.column == 6);
        CHECK(r.error.offset == 5);
    }
    {
        const engine::JsonParseResult r = engine::parseJson("[\n1,\n x]");
        REQUIRE_FALSE(r.ok());
        CHECK(r.error.message == "unexpected character at start of value");
        CHECK(r.error.line == 3);
        CHECK(r.error.column == 2);
        CHECK(r.error.offset == 6);
    }
    {
        const engine::JsonParseResult r = engine::parseJson("01");
        REQUIRE_FALSE(r.ok());
        CHECK(r.error.message.find("leading zero") != std::string::npos);
    }
    {
        const engine::JsonParseResult r = engine::parseJson("");
        REQUIRE_FALSE(r.ok());
        CHECK(r.error.message.find("unexpected end of input") != std::string::npos);
    }

    // depth cap: 301 '['s errors naming maxDepth; 256 (the default cap) parses fine
    {
        const std::string tooDeep(301, '[');
        const engine::JsonParseResult r = engine::parseJson(tooDeep);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error.message.find("maxDepth") != std::string::npos);
    }
    {
        std::string atCap(256, '[');
        atCap += std::string(256, ']');
        CHECK(engine::parseJson(atCap).ok());
    }
}

TEST_CASE("parser: truncation sweep -- every strict prefix of a valid document errors, none crashes (AC-7)") {
    const std::string doc = R"({"position":{"x":1,"y":2,"z":3},"rotation":{"x":0,"y":0,"z":0,"w":1},)"
                            R"("mass":2.5,"hitPoints":42,"active":true})";
    for (std::size_t i = 0; i < doc.size(); ++i) {
        const engine::JsonParseResult result = engine::parseJson(std::string_view(doc).substr(0, i));
        CHECK_FALSE(result.ok());
    }
    // sanity: the full document itself must parse cleanly (else the sweep above is vacuous)
    CHECK(engine::parseJson(doc).ok());
}

TEST_CASE("JsonValue accessors: kind checks + asI64/asU64 exactness + asF32/asF64 special values (AC-8)") {
    CHECK(engine::JsonValue::null().kind() == engine::JsonKind::Null);
    CHECK(engine::JsonValue::boolean(true).kind() == engine::JsonKind::Bool);

    {
        const engine::JsonParseResult r = engine::parseJson("-9223372036854775808");  // INT64_MIN
        REQUIRE(r.ok());
        CHECK(r.value->asI64() == std::numeric_limits<std::int64_t>::min());
    }
    {
        const engine::JsonParseResult r = engine::parseJson("9223372036854775807");  // INT64_MAX
        REQUIRE(r.ok());
        CHECK(r.value->asI64() == std::numeric_limits<std::int64_t>::max());
    }
    {
        const engine::JsonParseResult r = engine::parseJson("18446744073709551615");  // UINT64_MAX
        REQUIRE(r.ok());
        CHECK(r.value->asU64() == std::numeric_limits<std::uint64_t>::max());
        CHECK_FALSE(r.value->asI64().has_value());  // correctly rejected as i64 (E-u64)
    }
    {
        const engine::JsonParseResult r = engine::parseJson("-1");
        REQUIRE(r.ok());
        CHECK_FALSE(r.value->asU64().has_value());  // negative rejected as u64
    }
    {
        const engine::JsonParseResult r = engine::parseJson("2.5");
        REQUIRE(r.ok());
        CHECK_FALSE(r.value->asI64().has_value());  // not integral-form (D5)
    }
    {
        const engine::JsonParseResult r = engine::parseJson("1e2");
        REQUIRE(r.ok());
        CHECK_FALSE(r.value->asI64().has_value());
    }
    // the F4 regression pin: 7.038531e-26 must read back bit-equal to strtof's own value
    // (inner optionals bound + REQUIREd before every deref from here on -- 1.2 audit D4 pass)
    {
        const engine::JsonParseResult r = engine::parseJson("7.038531e-26");
        REQUIRE(r.ok());
        const auto viaDom = r.value->asF32();
        REQUIRE(viaDom.has_value());
        const float viaStrtof = std::strtof("7.038531e-26", nullptr);
        CHECK(std::bit_cast<std::uint32_t>(*viaDom) == std::bit_cast<std::uint32_t>(viaStrtof));
    }
    // -0 sign preservation
    {
        const engine::JsonParseResult r = engine::parseJson("-0");
        REQUIRE(r.ok());
        const auto negZero = r.value->asF32();
        REQUIRE(negZero.has_value());
        CHECK(std::signbit(*negZero));
    }
    // subnormal bit-exactness (float and double)
    {
        const engine::JsonParseResult r = engine::parseJson("1e-45");
        REQUIRE(r.ok());
        const auto viaDom = r.value->asF32();
        REQUIRE(viaDom.has_value());
        CHECK(std::bit_cast<std::uint32_t>(*viaDom) == std::bit_cast<std::uint32_t>(std::strtof("1e-45", nullptr)));
    }
    {
        const engine::JsonParseResult r = engine::parseJson("5e-324");
        REQUIRE(r.ok());
        const auto viaDom = r.value->asF64();
        REQUIRE(viaDom.has_value());
        CHECK(std::bit_cast<std::uint64_t>(*viaDom) == std::bit_cast<std::uint64_t>(std::strtod("5e-324", nullptr)));
    }
    // overflow -> nullopt; underflow -> accepted as 0.0
    {
        const engine::JsonParseResult r = engine::parseJson("1e999");
        REQUIRE(r.ok());
        CHECK_FALSE(r.value->asF32().has_value());
        CHECK_FALSE(r.value->asF64().has_value());
    }
    {
        const engine::JsonParseResult r = engine::parseJson("1e-999");
        REQUIRE(r.ok());
        CHECK(r.value->asF64() == 0.0);
    }
    // integer-lexeme floats
    {
        const engine::JsonParseResult r = engine::parseJson("1");
        REQUIRE(r.ok());
        CHECK(r.value->asF32() == 1.0F);
    }
}

TEST_CASE("read leaves + readField: null->NaN, strictness, range checks, Vec3/Quat policy (AC-9)") {
    // null -> quiet NaN for float/double
    {
        const engine::JsonParseResult r = engine::parseJson("null");
        REQUIRE(r.ok());
        float f = 0.0F;
        CHECK(engine::reflect::readJson(*r.value, f));
        CHECK(std::isnan(f));
        double d = 0.0;
        CHECK(engine::reflect::readJson(*r.value, d));
        CHECK(std::isnan(d));
    }
    // null -> false for bool/int/Vec3-root
    {
        const engine::JsonParseResult r = engine::parseJson("null");
        REQUIRE(r.ok());
        bool b = true;
        CHECK_FALSE(engine::reflect::readJson(*r.value, b));
        int i = 0;
        CHECK_FALSE(engine::reflect::readJson(*r.value, i));
        engine::Vec3 v{};
        CHECK_FALSE(engine::reflect::readJson(*r.value, v));
    }
    // null inside a Vec3 sub-key -> NaN for that component only (mirrors the writer, F7)
    {
        const engine::JsonParseResult r = engine::parseJson(R"({"x":null,"y":1,"z":2})");
        REQUIRE(r.ok());
        engine::Vec3 v{};
        CHECK(engine::reflect::readJson(*r.value, v));
        CHECK(std::isnan(v.x));
        CHECK(v.y == 1.0F);
        CHECK(v.z == 2.0F);
    }
    // bool strictness
    {
        const engine::JsonParseResult r = engine::parseJson("1");
        REQUIRE(r.ok());
        bool b = false;
        CHECK_FALSE(engine::reflect::readJson(*r.value, b));
    }
    // integral range/form checks
    {
        const engine::JsonParseResult r = engine::parseJson("300");
        REQUIRE(r.ok());
        std::uint8_t u8 = 5;
        CHECK_FALSE(engine::reflect::readJson(*r.value, u8));
        CHECK(u8 == 5);  // untouched on failure
    }
    {
        const engine::JsonParseResult r = engine::parseJson("-1");
        REQUIRE(r.ok());
        std::uint32_t u32 = 7;
        CHECK_FALSE(engine::reflect::readJson(*r.value, u32));
    }
    {
        const engine::JsonParseResult r = engine::parseJson("2.5");
        REQUIRE(r.ok());
        int i = 9;
        CHECK_FALSE(engine::reflect::readJson(*r.value, i));
    }
    // Vec3: all keys required, extras ignored, atomic failure on a bad sub-key
    {
        const engine::JsonParseResult r = engine::parseJson(R"({"x":1,"y":2})");  // missing z
        REQUIRE(r.ok());
        engine::Vec3 v{};
        CHECK_FALSE(engine::reflect::readJson(*r.value, v));
    }
    {
        const engine::JsonParseResult r = engine::parseJson(R"({"x":1,"y":2,"z":3,"extra":99})");
        REQUIRE(r.ok());
        engine::Vec3 v{};
        CHECK(engine::reflect::readJson(*r.value, v));
        CHECK(v.x == 1.0F);
    }
    {
        const engine::JsonParseResult r = engine::parseJson(R"({"x":1,"y":"bad","z":3})");
        REQUIRE(r.ok());
        engine::Vec3 v{1.0F, 2.0F, 3.0F};
        CHECK_FALSE(engine::reflect::readJson(*r.value, v));
        CHECK(v.x == 1.0F);  // untouched -- half-good input never partially commits (atomic, §3.8)
        CHECK(v.y == 2.0F);
        CHECK(v.z == 3.0F);
    }
    // Quat requires all FOUR keys
    {
        const engine::JsonParseResult r = engine::parseJson(R"({"x":0,"y":0,"z":0})");  // missing w
        REQUIRE(r.ok());
        engine::Quat q{};
        CHECK_FALSE(engine::reflect::readJson(*r.value, q));
    }
    // readField: a missing key leaves the field untouched and returns true (schema evolution, D9)
    {
        const engine::JsonParseResult r = engine::parseJson("{}");
        REQUIRE(r.ok());
        int i = 42;
        CHECK(engine::reflect::readField(*r.value, "T", "field", i));
        CHECK(i == 42);
    }
    // an unknown key does not fail the overall read (warned, not fatal) -- proven through GENERATED code
    {
        const engine::JsonParseResult r =
            engine::parseJson(R"({"target":{"x":1,"y":2,"z":3},"speed":1.5,"gear":3,"engaged":true,"bogus":123})");
        REQUIRE(r.ok());
        ReflectWiring restored{};
        CHECK(aeroReadJson(*r.value, restored));
        CHECK(restored.gear == 3);
    }
    // wrong-kind component root -> expectObject fails
    {
        const engine::JsonParseResult r = engine::parseJson("[1,2,3]");
        REQUIRE(r.ok());
        CHECK_FALSE(engine::reflect::expectObject(*r.value, "T"));
    }
}

namespace {

// The D13 round-trip helper: T -> aeroWriteJson -> text -> parseJson -> aeroReadJson -> T2, THEN
// aeroWriteJson(T2) must reproduce the SAME text (the fixpoint property, (b) of D13). Callers add their
// own per-field BIT-equality assertions on `restored` afterward -- (a) of D13, which this helper cannot
// do generically since each component type has different fields.
template <class T>
bool roundTrip(const T& original, T& restored) {
    engine::JsonWriter w1;
    aeroWriteJson(w1, original);
    const std::string s1 = w1.str();

    const engine::JsonParseResult parsed = engine::parseJson(s1);
    REQUIRE(parsed.ok());

    const bool ok = aeroReadJson(*parsed.value, restored);

    engine::JsonWriter w2;
    aeroWriteJson(w2, restored);
    CHECK(w2.str() == s1);
    return ok;
}

bool bitEqual(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
bool bitEqual(double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }

}  // namespace

TEST_CASE("round-trip: ReflectSample component -> JSON -> component, bit-equal + byte-equal (AC-10, the DoD)") {
    ReflectSample original{};
    original.position = {1.5F, -2.25F, 0.1F};
    original.rotation = engine::Quat{0.0F, 0.70710678F, 0.0F, 0.70710678F};
    original.mass = 123.456F;
    original.hitPoints = -7;
    original.active = true;

    ReflectSample restored{};
    CHECK(roundTrip(original, restored));

    CHECK(bitEqual(restored.position.x, original.position.x));
    CHECK(bitEqual(restored.position.y, original.position.y));
    CHECK(bitEqual(restored.position.z, original.position.z));
    CHECK(bitEqual(restored.rotation.x, original.rotation.x));
    CHECK(bitEqual(restored.rotation.y, original.rotation.y));
    CHECK(bitEqual(restored.rotation.z, original.rotation.z));
    CHECK(bitEqual(restored.rotation.w, original.rotation.w));
    CHECK(bitEqual(restored.mass, original.mass));
    CHECK(restored.hitPoints == original.hitPoints);
    CHECK(restored.active == original.active);
}

TEST_CASE("round-trip: the gnarly float table through ReflectSample.mass (AC-10, incl. the F4 value)") {
    const std::vector<float> values = {
        0.1F,
        1.0F / 3.0F,
        std::numeric_limits<float>::min(),
        std::numeric_limits<float>::denorm_min(),  // FLT_TRUE_MIN
        std::numeric_limits<float>::max(),
        -0.0F,
        1e30F,
        engine::PI,
        std::nextafter(1.0F, 2.0F),
        7.038531e-26F,  // the F4 regression value
    };
    for (const float value : values) {
        CAPTURE(value);
        ReflectSample original{};
        original.mass = value;
        ReflectSample restored{};
        CHECK(roundTrip(original, restored));
        CHECK(bitEqual(restored.mass, original.mass));
    }
}

TEST_CASE("round-trip: the gnarly double table through ReflectLimits.precise (AC-10)") {
    const std::vector<double> values = {
        0.1,
        1.0 / 3.0,
        std::numeric_limits<double>::min(),
        std::numeric_limits<double>::denorm_min(),  // DBL_TRUE_MIN
        std::numeric_limits<double>::max(),
        -0.0,
        1e300,
        std::nextafter(1.0, 2.0),
    };
    for (const double value : values) {
        CAPTURE(value);
        ReflectLimits original{};
        original.precise = value;
        ReflectLimits restored{};
        CHECK(roundTrip(original, restored));
        CHECK(bitEqual(restored.precise, original.precise));
    }
}

TEST_CASE("round-trip: ReflectLimits integer extremes -- INT64_MIN/MAX, UINT64_MAX, int16/uint8 bounds (AC-10)") {
    ReflectLimits low{};
    low.big = std::numeric_limits<std::int64_t>::min();
    low.huge = std::numeric_limits<std::uint64_t>::max();
    low.small = std::numeric_limits<std::int16_t>::min();
    low.tiny = 0;
    low.precise = -1e300;
    ReflectLimits restoredLow{};
    CHECK(roundTrip(low, restoredLow));
    CHECK(restoredLow.big == low.big);
    CHECK(restoredLow.huge == low.huge);
    CHECK(restoredLow.small == low.small);
    CHECK(restoredLow.tiny == low.tiny);
    CHECK(bitEqual(restoredLow.precise, low.precise));

    ReflectLimits high{};
    high.big = std::numeric_limits<std::int64_t>::max();
    high.huge = 0;
    high.small = std::numeric_limits<std::int16_t>::max();
    high.tiny = std::numeric_limits<std::uint8_t>::max();
    ReflectLimits restoredHigh{};
    CHECK(roundTrip(high, restoredHigh));
    CHECK(restoredHigh.big == high.big);
    CHECK(restoredHigh.small == high.small);
    CHECK(restoredHigh.tiny == high.tiny);
}

TEST_CASE("round-trip: namespaced engine::demo::Light (ADL) + ReflectWiring (unsigned) (AC-10)") {
    engine::demo::Light original{};
    original.color = {0.25F, 0.5F, 0.75F};
    original.intensity = 4.0;
    engine::demo::Light restored{};
    CHECK(roundTrip(original, restored));
    CHECK(bitEqual(restored.color.x, original.color.x));
    CHECK(bitEqual(restored.color.y, original.color.y));
    CHECK(bitEqual(restored.color.z, original.color.z));
    CHECK(bitEqual(restored.intensity, original.intensity));

    ReflectWiring wireOriginal{};
    wireOriginal.target = {1.0F, 2.0F, 3.0F};
    wireOriginal.speed = 9.5F;
    wireOriginal.gear = 7U;
    wireOriginal.engaged = true;
    ReflectWiring wireRestored{};
    CHECK(roundTrip(wireOriginal, wireRestored));
    CHECK(bitEqual(wireRestored.target.x, wireOriginal.target.x));
    CHECK(wireRestored.gear == wireOriginal.gear);
    CHECK(wireRestored.engaged == wireOriginal.engaged);
}

TEST_CASE("round-trip: global-namespace Player from the multi-component header (AC-10)") {
    // Before the 1.2 audit (D7), Player's generated pair was compile-proven only -- Light exercised
    // the namespaced half of component_multi.hpp, but nothing round-tripped the global-namespace
    // half at runtime.
    Player original{};
    original.position = {4.0F, -8.5F, 12.25F};
    original.score = -1234;
    Player restored{};
    CHECK(roundTrip(original, restored));
    CHECK(bitEqual(restored.position.x, original.position.x));
    CHECK(bitEqual(restored.position.y, original.position.y));
    CHECK(bitEqual(restored.position.z, original.position.z));
    CHECK(restored.score == original.score);
}

// component_tag.hpp is the fifth aero_reflect_json_test HEADERS entry (review fix): a REAL generated
// zero-field aeroWriteJson/aeroReadJson pair for Tag, proven to compile, link, and round-trip -- not
// just a DOM-level "{}" proxy. The generated shape itself (expectObject + an empty warnUnknownKeys,
// no readField( line) is separately pinned process-boundary-side by json_tag/json_reader_tag.
TEST_CASE("round-trip: Tag{} -- a REAL generated zero-field component pair compiles and round-trips (AC-10)") {
    const Tag original{};
    Tag restored{};
    CHECK(roundTrip(original, restored));

    engine::JsonWriter w;
    aeroWriteJson(w, original);
    CHECK(w.str() == "{}");
}

TEST_CASE("round-trip: NaN field reads back as NaN (isnan, not bits) and re-serializes byte-identically (AC-10)") {
    ReflectSample original{};
    original.mass = std::numeric_limits<float>::quiet_NaN();

    engine::JsonWriter w1;
    aeroWriteJson(w1, original);
    const std::string s1 = w1.str();
    CHECK(s1.find("\"mass\": null") != std::string::npos);

    const engine::JsonParseResult r = engine::parseJson(s1);
    REQUIRE(r.ok());
    ReflectSample restored{};
    CHECK(aeroReadJson(*r.value, restored));
    CHECK(std::isnan(restored.mass));

    engine::JsonWriter w2;
    aeroWriteJson(w2, restored);
    CHECK(w2.str() == s1);
}

TEST_CASE(
    "best-effort application: one bad + one good field applies the good one and returns false; "
    "missing-all leaves defaults; key-permutation is order-independent (AC-11)") {
    ReflectSample original{};
    original.position = {1.0F, 2.0F, 3.0F};
    original.rotation = engine::Quat::identity();
    original.mass = 2.5F;
    original.hitPoints = 42;
    original.active = true;

    engine::JsonWriter w;
    aeroWriteJson(w, original);
    const std::string canonical = w.str();

    // one bad field (mass -> a string) + the rest good: good fields still apply, overall result false
    {
        std::string doc = canonical;
        const std::string good = "\"mass\": 2.5";
        const std::size_t at = doc.find(good);
        REQUIRE(at != std::string::npos);
        doc.replace(at, good.size(), R"("mass": "broken")");
        const engine::JsonParseResult r = engine::parseJson(doc);
        REQUIRE(r.ok());
        ReflectSample restored{};
        CHECK_FALSE(aeroReadJson(*r.value, restored));
        CHECK(restored.hitPoints == 42);  // a LATER field still applied (no short-circuit, AC-11)
        CHECK(restored.active);
        CHECK(restored.mass == 0.0F);  // the bad field left at its default (untouched)
    }

    // every key missing -> true, defaults intact
    {
        const engine::JsonParseResult r = engine::parseJson("{}");
        REQUIRE(r.ok());
        ReflectSample restored{};
        restored.hitPoints = 7;  // a sentinel distinct from ReflectSample{}'s zero-init
        CHECK(aeroReadJson(*r.value, restored));
        CHECK(restored.hitPoints == 7);  // untouched
    }

    // key-permutation round-trips to the same component (order-independence)
    {
        const std::size_t massAt = canonical.find("\"mass\":");
        const std::size_t hitAt = canonical.find("\"hitPoints\":");
        REQUIRE(massAt != std::string::npos);
        REQUIRE(hitAt != std::string::npos);
        REQUIRE(massAt < hitAt);
        const std::size_t massEnd = canonical.find(',', massAt);
        const std::size_t hitEnd = canonical.find(',', hitAt);
        REQUIRE(massEnd != std::string::npos);
        REQUIRE(hitEnd != std::string::npos);
        const std::string massSpan = canonical.substr(massAt, massEnd - massAt);
        const std::string hitSpan = canonical.substr(hitAt, hitEnd - hitAt);

        std::string permuted = canonical;
        permuted.replace(hitAt, hitEnd - hitAt, massSpan);    // apply the LATER edit first (positions
        permuted.replace(massAt, massEnd - massAt, hitSpan);  // computed from the ORIGINAL string stay valid)

        const engine::JsonParseResult r = engine::parseJson(permuted);
        REQUIRE(r.ok());
        ReflectSample restored{};
        CHECK(aeroReadJson(*r.value, restored));
        CHECK(restored.mass == original.mass);
        CHECK(restored.hitPoints == original.hitPoints);
    }
}
