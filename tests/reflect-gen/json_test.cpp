// tests/reflect-gen/json_test.cpp — task 1.2.1: the runtime proof that generated JSON serializers
// produce exact, ordered, round-trippable JSON (AC-10/AC-11). Standalone single-TU doctest (own
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN, no shared test_main.cpp), sibling of aero_reflect_meta_test but
// WITHOUT EnTT — serialization is meta-independent (D4).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <aero/reflect/json_writer.hpp>
#include <aero/reflect/serialize.hpp>

#include "component_codegen.hpp"
#include "component_multi.hpp"
#include "component_wiring.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

// GENERATED serializers (aero_reflect_generate_json over the three headers; aeroWriteJson in the component's
// namespace, D7). camelBack => no NOLINT.
void aeroWriteJson(engine::JsonWriter&, const ReflectSample&);
void aeroWriteJson(engine::JsonWriter&, const ReflectWiring&);
namespace engine::demo {
void aeroWriteJson(engine::JsonWriter&, const Light&);
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

TEST_CASE("JsonWriter units: escaping, nesting, arrays, bool/int/null, non-finite, compact (AC-11)") {
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
}
