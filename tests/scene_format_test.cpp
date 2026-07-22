// tests/scene_format_test.cpp — task 1.2.3: the scene-file format v1 proof (docs/09-file-formats.md).
// Tier 0: no GPU, no reflect-gen, no files, no randomness. Covers the D2 schema shape, the D5 version
// gate, the D3 identity/hierarchy rules, the D11 error catalog, the D6 tolerance policy, the D7
// canonical form and both round-trip guarantees, the D9 DOM re-emitter, and D8's validateScene.
// Byte-pinned fixtures are the executable twins of docs/09's worked examples.
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>
#include <aero/reflect/scene_format.hpp>
#include <aero/reflect/serialize.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
// <ostream> is required by MSVC, not by libc++ (the 0.4.1 trap): doctest stringifies a failing
// CHECK(std::string == std::string_view) through operator<<, and MSVC's <__msvc_string_view.hpp>
// overload needs a COMPLETE std::ostream. Omitting it builds clean on macOS/Linux and fails only
// on the Windows lane with errors pointing inside the STL headers.
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

// The two canonical fixtures (spec section 3.2), byte-pinned INCLUDING the single trailing newline.
// constexpr std::string_view => UPPER_CASE per .clang-tidy's ConstexprVariableCase (N10).
constexpr std::string_view MINIMAL_CANONICAL = R"({
  "version": 1,
  "entities": []
}
)";

constexpr std::string_view FULL_CANONICAL = R"({
  "version": 1,
  "entities": [
    {
      "id": 1,
      "name": "camera",
      "components": {
        "engine::Transform": {
          "position": {
            "x": 0,
            "y": 2.5,
            "z": -10
          },
          "rotation": {
            "x": 0,
            "y": 0,
            "z": 0,
            "w": 1
          }
        },
        "engine::Camera": {
          "fovDegrees": 60,
          "nearPlane": 0.1,
          "farPlane": 100
        }
      }
    },
    {
      "id": 2,
      "name": "crate",
      "components": {
        "engine::Transform": {
          "position": {
            "x": 0,
            "y": 0,
            "z": 0
          }
        }
      }
    },
    {
      "id": 3,
      "name": "lamp",
      "parent": 2,
      "components": {
        "demo::Marker": {}
      }
    },
    {
      "id": 4
    }
  ]
}
)";

// Copied IDIOM (never the symbol) from tests/reflect-gen/json_test.cpp:134-144 -- a "\uXXXX" token
// cannot be written as a source literal portably: MSVC forms universal-character-names inside RAW
// string literals, and clang-tidy's modernize-raw-string-literal rewrites the escaped form back into
// a raw one. Assembling from char fragments is invisible to both checks.
std::string unicodeEscape(std::string_view fourHexDigits) {
    std::string token;
    token += '\\';
    token += 'u';
    token += fourHexDigits;
    return token;
}

// Parse a scene from text and REQUIRE success, returning the document. (bugprone-unchecked-optional-
// access is disabled for tests/ by tests/.clang-tidy, so the deref is fine HERE -- N8.)
engine::SceneDocument parseOk(std::string_view text) {
    const engine::SceneParseResult result = engine::parseScene(text);
    REQUIRE(result.ok());
    return *result.document;
}

// Parse a scene from text, REQUIRE failure, return the error.
engine::SceneError parseFail(std::string_view text) {
    const engine::SceneParseResult result = engine::parseScene(text);
    REQUIRE_FALSE(result.ok());
    return result.error;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// Wrap a single raw "version" JSON token in an otherwise-minimal scene envelope.
std::string sceneWithVersion(std::string_view versionLiteral) {
    return std::string(R"({"version": )") + std::string(versionLiteral) + R"(, "entities": []})";
}

// Wrap a single raw entity JSON object literal in an otherwise-minimal, version-1 scene envelope.
std::string sceneWithEntity(std::string_view entityLiteral) {
    return std::string(R"({"version": 1, "entities": [)") + std::string(entityLiteral) + "]}";
}

// Parse `src` as a bare JSON document and re-emit it through engine::reflect::writeJson under `config`.
std::string reemit(std::string_view src, engine::JsonWriterConfig config = {}) {
    const engine::JsonParseResult parsed = engine::parseJson(src);
    REQUIRE(parsed.value.has_value());
    engine::JsonWriter writer(config);
    engine::reflect::writeJson(writer, *parsed.value);
    return writer.str();
}

}  // namespace

TEST_CASE("scene: SCENE_FORMAT_VERSION and the D8 surface") {
    static_assert(engine::SCENE_FORMAT_VERSION == 1);
    static_assert(std::is_aggregate_v<engine::SceneEntityRecord>);
    static_assert(std::is_aggregate_v<engine::SceneComponentRecord>);
    static_assert(std::is_aggregate_v<engine::SceneDocument>);
    static_assert(std::is_aggregate_v<engine::SceneError>);

    const engine::SceneEntityRecord defaultEntity;
    CHECK(defaultEntity.id == 0);
    CHECK(defaultEntity.parent == 0);
    CHECK(defaultEntity.name.empty());
    CHECK(defaultEntity.components.empty());

    CHECK(engine::writeSceneText(engine::SceneDocument{}) == MINIMAL_CANONICAL);
}

TEST_CASE("scene: minimal fixture parses -- model fidelity") { CHECK(parseOk(MINIMAL_CANONICAL).entities.empty()); }

TEST_CASE("scene: full fixture parses -- order, name, parent, components, payload navigation") {
    const engine::SceneDocument doc = parseOk(FULL_CANONICAL);
    REQUIRE(doc.entities.size() == 4);
    CHECK(doc.entities[0].id == 1);
    CHECK(doc.entities[1].id == 2);
    CHECK(doc.entities[2].id == 3);
    CHECK(doc.entities[3].id == 4);

    CHECK(doc.entities[0].name == "camera");
    CHECK(doc.entities[3].name.empty());

    CHECK(doc.entities[0].parent == 0);
    CHECK(doc.entities[2].parent == 2);

    CHECK(doc.entities[3].components.empty());

    REQUIRE(doc.entities[0].components.size() == 2);
    CHECK(doc.entities[0].components[0].type == "engine::Transform");
    CHECK(doc.entities[0].components[1].type == "engine::Camera");

    REQUIRE(doc.entities[2].components.size() == 1);
    CHECK(doc.entities[2].components[0].type == "demo::Marker");
    CHECK(doc.entities[2].components[0].value.isObject());
    CHECK(doc.entities[2].components[0].value.size() == 0);

    const engine::JsonValue& transform = doc.entities[0].components[0].value;
    const engine::JsonValue* position = transform.find("position");
    REQUIRE(position != nullptr);
    const engine::JsonValue* y = position->find("y");
    const engine::JsonValue* z = position->find("z");
    REQUIRE(y != nullptr);
    REQUIRE(z != nullptr);

    const std::optional<double> yValue = y->asF64();
    REQUIRE(yValue.has_value());
    CHECK(*yValue == 2.5);

    const std::optional<std::int64_t> zValue = z->asI64();
    REQUIRE(zValue.has_value());
    CHECK(*zValue == -10);
}

TEST_CASE("scene: canonical write -- minimal and full byte pins") {
    CHECK(engine::writeSceneText(parseOk(MINIMAL_CANONICAL)) == MINIMAL_CANONICAL);
    CHECK(engine::writeSceneText(parseOk(FULL_CANONICAL)) == FULL_CANONICAL);
}

TEST_CASE("scene: a hand-built document emits the same bytes") {
    using engine::JsonMember;
    using engine::JsonValue;

    auto num = [](std::string_view lexeme) { return JsonValue::number(std::string(lexeme)); };
    auto vec3 = [&num](std::string_view x, std::string_view y, std::string_view z) {
        return JsonValue::object({
            JsonMember{.key = "x", .value = num(x)},
            JsonMember{.key = "y", .value = num(y)},
            JsonMember{.key = "z", .value = num(z)},
        });
    };

    engine::SceneDocument built;

    engine::SceneEntityRecord camera;
    camera.id = 1;
    camera.name = "camera";
    camera.components.push_back(
        engine::SceneComponentRecord{.type = "engine::Transform",
                                     .value = JsonValue::object({
                                         JsonMember{.key = "position", .value = vec3("0", "2.5", "-10")},
                                         JsonMember{.key = "rotation",
                                                    .value = JsonValue::object({
                                                        JsonMember{.key = "x", .value = num("0")},
                                                        JsonMember{.key = "y", .value = num("0")},
                                                        JsonMember{.key = "z", .value = num("0")},
                                                        JsonMember{.key = "w", .value = num("1")},
                                                    })},
                                     })});
    camera.components.push_back(engine::SceneComponentRecord{.type = "engine::Camera",
                                                             .value = JsonValue::object({
                                                                 JsonMember{.key = "fovDegrees", .value = num("60")},
                                                                 JsonMember{.key = "nearPlane", .value = num("0.1")},
                                                                 JsonMember{.key = "farPlane", .value = num("100")},
                                                             })});
    built.entities.push_back(std::move(camera));

    engine::SceneEntityRecord crate;
    crate.id = 2;
    crate.name = "crate";
    crate.components.push_back(engine::SceneComponentRecord{
        .type = "engine::Transform",
        .value = JsonValue::object({JsonMember{.key = "position", .value = vec3("0", "0", "0")}})});
    built.entities.push_back(std::move(crate));

    engine::SceneEntityRecord lamp;
    lamp.id = 3;
    lamp.name = "lamp";
    lamp.parent = 2;
    lamp.components.push_back(engine::SceneComponentRecord{.type = "demo::Marker", .value = JsonValue::object({})});
    built.entities.push_back(std::move(lamp));

    engine::SceneEntityRecord loose;
    loose.id = 4;
    built.entities.push_back(std::move(loose));

    CHECK(engine::writeSceneText(built) == FULL_CANONICAL);
}

TEST_CASE("scene: round-trip idempotence over non-canonical variants") {
    SUBCASE("permuted entity keys") {
        constexpr std::string_view PERMUTED = R"({
  "entities": [
    {
      "components": {
        "engine::Transform": {
          "position": { "x": 0, "y": 2.5, "z": -10 },
          "rotation": { "x": 0, "y": 0, "z": 0, "w": 1 }
        },
        "engine::Camera": { "fovDegrees": 60, "nearPlane": 0.1, "farPlane": 100 }
      },
      "name": "camera",
      "id": 1
    },
    {
      "components": {
        "engine::Transform": { "position": { "x": 0, "y": 0, "z": 0 } }
      },
      "name": "crate",
      "id": 2
    },
    {
      "components": { "demo::Marker": {} },
      "parent": 2,
      "name": "lamp",
      "id": 3
    },
    { "id": 4 }
  ],
  "version": 1
})";
        const std::string t1 = engine::writeSceneText(parseOk(PERMUTED));
        const std::string t2 = engine::writeSceneText(parseOk(t1));
        CHECK(t1 == t2);
        CHECK(t1 == FULL_CANONICAL);
    }

    SUBCASE("compact whitespace") {
        constexpr std::string_view COMPACT =
            R"({"version":1,"entities":[{"id":1,"name":"camera","components":{"engine::Transform":{"position":)"
            R"({"x":0,"y":2.5,"z":-10},"rotation":{"x":0,"y":0,"z":0,"w":1}},"engine::Camera":{"fovDegrees":60,)"
            R"("nearPlane":0.1,"farPlane":100}}},{"id":2,"name":"crate","components":{"engine::Transform":)"
            R"({"position":{"x":0,"y":0,"z":0}}}},{"id":3,"name":"lamp","parent":2,"components":)"
            R"({"demo::Marker":{}}},{"id":4}]})";
        const std::string t1 = engine::writeSceneText(parseOk(COMPACT));
        const std::string t2 = engine::writeSceneText(parseOk(t1));
        CHECK(t1 == t2);
        CHECK(t1 == FULL_CANONICAL);
    }

    SUBCASE("leading UTF-8 BOM + CRLF") {
        std::string bomCrlf = "\xEF\xBB\xBF";
        for (const char c : FULL_CANONICAL) {
            if (c == '\n') {
                bomCrlf += "\r\n";
            } else {
                bomCrlf += c;
            }
        }
        const std::string t1 = engine::writeSceneText(parseOk(bomCrlf));
        const std::string t2 = engine::writeSceneText(parseOk(t1));
        CHECK(t1 == t2);
        CHECK(t1 == FULL_CANONICAL);
    }

    SUBCASE("\\u-escaped name normalizes to raw UTF-8") {
        const std::string escapedNameVariant =
            std::string(R"({"version":1,"entities":[{"id":1,"name":"caf)") + unicodeEscape("00e9") + R"("}]})";
        const std::string t1 = engine::writeSceneText(parseOk(escapedNameVariant));
        const std::string t2 = engine::writeSceneText(parseOk(t1));
        CHECK(t1 == t2);
        CHECK(contains(t1, "\xC3\xA9"));
        CHECK_FALSE(contains(t1, unicodeEscape("00e9")));
    }

    SUBCASE("exotic-but-valid number lexemes") {
        constexpr std::string_view EXOTIC = R"({
  "version": 1,
  "entities": [
    {
      "id": 1,
      "components": {
        "test::Exotic": {
          "a": 1e2,
          "b": 100.0,
          "c": -0,
          "d": 1e-999,
          "e": 123456789012345678901
        }
      }
    }
  ]
}
)";
        const std::string t1 = engine::writeSceneText(parseOk(EXOTIC));
        const std::string t2 = engine::writeSceneText(parseOk(t1));
        CHECK(t1 == t2);
    }
}

TEST_CASE("scene: version gate battery") {
    CHECK(contains(parseFail(R"({"entities": []})").message, "missing required key \"version\""));

    const engine::SceneError stringVersion = parseFail(sceneWithVersion(R"("1")"));
    CHECK(contains(stringVersion.message, "\"version\" must be an integer"));
    CHECK(contains(stringVersion.message, "(found string)"));

    CHECK(contains(parseFail(sceneWithVersion("true")).message, "(found bool)"));
    CHECK(contains(parseFail(sceneWithVersion("null")).message, "(found null)"));
    CHECK(contains(parseFail(sceneWithVersion("1.0")).message, R"((found "1.0"))"));
    CHECK(contains(parseFail(sceneWithVersion("1e0")).message, R"((found "1e0"))"));
    CHECK(contains(parseFail(sceneWithVersion("-1")).message, R"((found "-1"))"));
    CHECK(contains(parseFail(sceneWithVersion("18446744073709551616")).message, R"((found "18446744073709551616"))"));
    CHECK(contains(parseFail(sceneWithVersion("0")).message,
                   "unsupported scene format version 0 (this build reads version 1)"));
    CHECK(contains(parseFail(sceneWithVersion("2")).message,
                   "unsupported scene format version 2 (this build reads version 1)"));

    const engine::SceneError priority = parseFail(R"({"version": 2})");
    CHECK(contains(priority.message, "unsupported scene format version 2"));
    CHECK_FALSE(contains(priority.message, "entities"));

    CHECK(contains(std::string(FULL_CANONICAL), "\"version\": 1"));
}

TEST_CASE("scene: id battery") {
    CHECK(contains(parseFail(sceneWithEntity(R"({"name": "x"})")).message, "entities[0]: missing required key \"id\""));
    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 0})")).message, "\"id\" must be an integer >= 1"));
    CHECK(contains(parseFail(sceneWithEntity(R"({"id": -1})")).message, "\"id\" must be an integer >= 1"));

    const engine::SceneError floatId = parseFail(sceneWithEntity(R"({"id": 1.5})"));
    CHECK(contains(floatId.message, "\"id\" must be an integer >= 1"));
    CHECK(contains(floatId.message, R"((found "1.5"))"));

    const engine::SceneError stringId = parseFail(sceneWithEntity(R"({"id": "7"})"));
    CHECK(contains(stringId.message, "\"id\" must be an integer >= 1"));
    CHECK(contains(stringId.message, "(found string)"));

    CHECK(contains(parseFail(sceneWithEntity(R"({"id": true})")).message, "\"id\" must be an integer >= 1"));
    CHECK(contains(parseFail(sceneWithEntity(R"({"id": null})")).message, "\"id\" must be an integer >= 1"));
    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 18446744073709551616})")).message,
                   "\"id\" must be an integer >= 1"));

    const engine::SceneDocument maxIdDoc = parseOk(sceneWithEntity(R"({"id": 18446744073709551615})"));
    REQUIRE(maxIdDoc.entities.size() == 1);
    CHECK(maxIdDoc.entities[0].id == 18446744073709551615ULL);
    CHECK(contains(engine::writeSceneText(maxIdDoc), "18446744073709551615"));

    CHECK(contains(parseFail(R"({"version": 1, "entities": [1]})").message,
                   "entities[0] must be an object (found number)"));
}

TEST_CASE("scene: duplicate id names both indices") {
    constexpr std::string_view TEXT = R"({"version": 1, "entities": [{"id": 7}, {"id": 9}, {"id": 7}]})";
    const engine::SceneError e1 = parseFail(TEXT);
    const engine::SceneError e2 = parseFail(TEXT);
    CHECK(e1.message == "entities[2]: duplicate entity id 7 (first used by entities[0])");
    CHECK(e1.message == e2.message);
}

TEST_CASE("scene: parent battery") {
    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 1, "parent": 99})")).message,
                   "parent 99 does not reference any entity id in this scene"));
    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 1, "parent": 0})")).message,
                   "\"parent\" must be an integer >= 1"));
    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 1, "parent": 1})")).message, "parent chain is cyclic"));

    const engine::SceneError twoCycle =
        parseFail(R"({"version": 1, "entities": [{"id": 1, "parent": 2}, {"id": 2, "parent": 1}]})");
    CHECK(contains(twoCycle.message, "parent chain is cyclic"));
    CHECK(contains(twoCycle.message, "entities[0] (id 1)"));

    const engine::SceneError threeCycle = parseFail(
        R"({"version": 1, "entities": [{"id": 1, "parent": 2}, {"id": 2, "parent": 3}, {"id": 3, "parent": 1}]})");
    CHECK(contains(threeCycle.message, "parent chain is cyclic"));
    CHECK(contains(threeCycle.message, "entities[0]"));

    // entities[0] (id 3) hangs OFF the 1<->2 cycle without being ON it: its own chain (3 -> 1 -> 2 ->
    // 1) revisits id 1 without ever revisiting id 3 itself -- so this is NOT the same shape as the
    // two-cycle case above, which asserts on an entity that IS part of its own cycle.
    const engine::SceneError hangingFirst = parseFail(
        R"({"version": 1, "entities": [{"id": 3, "parent": 1}, {"id": 1, "parent": 2}, {"id": 2, "parent": 1}]})");
    CHECK(hangingFirst.message == "entities[0] (id 3): parent chain is cyclic");

    // A cycle discovered at start > 0, AFTER an earlier walk has already memoized an acyclic chain
    // (entities[0]/[1] terminate at a root and get marked WalkState::Acyclic) -- the one arrangement
    // that actually exercises the memo rather than always starting a fresh walk at index 0.
    const engine::SceneError afterMemo = parseFail(R"({"version": 1, "entities": [{"id": 1}, {"id": 2, "parent": 1}, )"
                                                   R"({"id": 3, "parent": 4}, {"id": 4, "parent": 3}]})");
    CHECK(afterMemo.message == "entities[2] (id 3): parent chain is cyclic");

    const std::string forwardRefText = R"({"version": 1, "entities": [{"id": 1, "parent": 2}, {"id": 2}]})";
    const engine::SceneDocument forward = parseOk(forwardRefText);
    REQUIRE(forward.entities.size() == 2);
    CHECK(forward.entities[0].parent == 2);
    const std::string t1 = engine::writeSceneText(forward);
    const std::string t2 = engine::writeSceneText(parseOk(t1));
    CHECK(t1 == t2);
}

TEST_CASE("scene: 1000-entity linear chain") {
    std::string forwardChain = R"({"version": 1, "entities": [{"id": 1})";
    for (std::uint64_t k = 2; k <= 1000; ++k) {
        forwardChain += std::format(R"(, {{"id": {}, "parent": {}}})", k, k - 1);
    }
    forwardChain += "]}";
    const engine::SceneDocument doc = parseOk(forwardChain);
    CHECK(doc.entities.size() == 1000);
    CHECK(doc.entities.back().parent == 999);

    // Reverse order: entity 1000 declared FIRST, each child before its (still-undeclared) parent --
    // forward references at scale, under the same 1000-entity chain.
    std::string reverseChain = R"({"version": 1, "entities": [)";
    for (std::uint64_t k = 1000; k >= 2; --k) {
        reverseChain += std::format(R"({{"id": {}, "parent": {}}}, )", k, k - 1);
    }
    reverseChain += R"({"id": 1}]})";
    const engine::SceneDocument reverseDoc = parseOk(reverseChain);
    CHECK(reverseDoc.entities.size() == 1000);
}

TEST_CASE("scene: components battery") {
    CHECK(parseOk(sceneWithEntity(R"({"id": 1})")).entities[0].components.empty());
    CHECK(parseOk(sceneWithEntity(R"({"id": 1, "components": {}})")).entities[0].components.empty());

    const engine::SceneDocument tagDoc = parseOk(sceneWithEntity(R"({"id": 1, "components": {"demo::Tag": {}}})"));
    REQUIRE(tagDoc.entities[0].components.size() == 1);
    CHECK(tagDoc.entities[0].components[0].value.isObject());
    CHECK(tagDoc.entities[0].components[0].value.size() == 0);

    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 1, "components": []})")).message,
                   "\"components\" must be an object (found array)"));
    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 1, "components": null})")).message,
                   "\"components\" must be an object (found null)"));
    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 1, "components": 3})")).message,
                   "\"components\" must be an object (found number)"));

    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 1, "components": {"T": null}})")).message,
                   "payload must be an object (found null)"));
    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 1, "components": {"T": []}})")).message,
                   "payload must be an object (found array)"));
    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 1, "components": {"T": 3}})")).message,
                   "payload must be an object (found number)"));
    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 1, "components": {"T": "x"}})")).message,
                   "payload must be an object (found string)"));

    CHECK(contains(parseFail(sceneWithEntity(R"({"id": 1, "components": {"": {}}})")).message,
                   "component name must be a non-empty string"));

    // Duplicate component key in raw text -- last-wins at the JSON layer, at the FIRST position (F3).
    const engine::SceneDocument dupDoc = parseOk(
        sceneWithEntity(R"({"id": 1, "components": {"engine::Transform": {"a": 1}, "engine::Transform": {"a": 2}}})"));
    REQUIRE(dupDoc.entities[0].components.size() == 1);
    CHECK(dupDoc.entities[0].components[0].type == "engine::Transform");
    const engine::JsonValue* dupA = dupDoc.entities[0].components[0].value.find("a");
    REQUIRE(dupA != nullptr);
    CHECK(dupA->asI64() == 2);

    // Non-alphabetical member order preserved and re-emitted.
    const engine::SceneDocument orderedDoc =
        parseOk(sceneWithEntity(R"({"id": 1, "components": {"zzz::Last": {}, "aaa::First": {}, "mmm::Middle": {}}})"));
    REQUIRE(orderedDoc.entities[0].components.size() == 3);
    CHECK(orderedDoc.entities[0].components[0].type == "zzz::Last");
    CHECK(orderedDoc.entities[0].components[1].type == "aaa::First");
    CHECK(orderedDoc.entities[0].components[2].type == "mmm::Middle");
    const std::string t1 = engine::writeSceneText(orderedDoc);
    const std::string t2 = engine::writeSceneText(parseOk(t1));
    CHECK(t1 == t2);
}

TEST_CASE("scene: parseScene(const JsonValue&) called directly -- the DOM primitive (D8)") {
    // Every other case reaches parseScene through the text overload; D8 designates the DOM overload
    // "the primitive", so it needs its own direct call. This fixture doubles as D11 catalog coverage
    // for "scene root must be a JSON object (found <kind>)" (scene_format.cpp:76), the one line no
    // other test exercises.
    const engine::SceneParseResult result = engine::parseScene(engine::JsonValue::array({}));
    CHECK_FALSE(result.ok());
    CHECK(result.error.message == "scene root must be a JSON object (found array)");
    CHECK(result.error.line == 0);
    CHECK(result.error.column == 0);
    CHECK(result.error.offset == 0);
}

TEST_CASE("scene: D11 catalog coverage -- entities key/kind, name kind") {
    // Three more catalog lines (scene_format.cpp:96, :99, :134-135) with no prior direct coverage.
    CHECK(contains(parseFail(R"({"version": 1})").message, "missing required key \"entities\""));
    CHECK(contains(parseFail(R"({"version": 1, "entities": {}})").message,
                   "\"entities\" must be an array (found object)"));
    CHECK(parseFail(sceneWithEntity(R"({"id": 1, "name": 5})")).message ==
          "entities[0] (id 1): \"name\" must be a string (found number)");
}

TEST_CASE("scene: unknown keys tolerated and stripped") {
    const std::string text = R"({"version": 1, "comment": "hi", "entities": [{"id": 1, "tags": []}]})";
    const engine::SceneDocument doc = parseOk(text);
    const std::string canonical = engine::writeSceneText(doc);
    CHECK_FALSE(contains(canonical, "comment"));
    CHECK_FALSE(contains(canonical, "tags"));

    const engine::SceneParseResult rejected = engine::parseScene(sceneWithEntity(R"({"id": 0})"));
    CHECK_FALSE(rejected.ok());
    CHECK_FALSE(rejected.error.message.empty());
    CHECK_FALSE(rejected.document.has_value());
}

TEST_CASE("scene: error position contract -- JSON stage vs scene stage") {
    const engine::SceneError jsonStage = parseFail(R"({"version": 1,)");
    CHECK(jsonStage.line > 0);

    const engine::SceneError sceneStage = parseFail(R"({"version": 1, "entities": [{"id": 7}, {"id": 7}]})");
    CHECK(sceneStage.line == 0);
    CHECK(sceneStage.column == 0);
    CHECK(sceneStage.offset == 0);
    CHECK(contains(sceneStage.message, "entities["));

    const engine::SceneParseResult shallow =
        engine::parseScene(MINIMAL_CANONICAL, engine::JsonParseConfig{.maxDepth = 3});
    CHECK(shallow.ok());

    const engine::SceneParseResult tooDeep = engine::parseScene(FULL_CANONICAL, engine::JsonParseConfig{.maxDepth = 3});
    CHECK_FALSE(tooDeep.ok());
    CHECK(tooDeep.error.line > 0);
    CHECK(contains(tooDeep.error.message, "maxDepth"));
}

TEST_CASE("re-emitter: all six kinds, nesting, escapes") {
    CHECK(reemit("null") == "null");
    CHECK(reemit("true") == "true");
    CHECK(reemit("\"s\"") == "\"s\"");
    CHECK(reemit("1") == "1");
    CHECK(reemit("[]") == "[]");
    CHECK(reemit("{}") == "{}");

    constexpr std::string_view NESTED = R"({"a":[1,{"b":[true,null,"x"]}],"c":{}})";
    CHECK(reemit(NESTED, engine::JsonWriterConfig{.pretty = false}) == NESTED);

    const std::string prettyForm = reemit(NESTED);
    const std::string recompacted = reemit(prettyForm, engine::JsonWriterConfig{.pretty = false});
    CHECK(recompacted == NESTED);

    const std::string escapedE = std::string(R"("caf)") + unicodeEscape("00e9") + "\"";
    CHECK(reemit(escapedE) == "\"caf\xC3\xA9\"");

    const std::string weird = std::string(R"("a\"b\\c\nd)") + unicodeEscape("0001") + "e\"";
    const std::string t1 = reemit(weird);
    const std::string t2 = reemit(t1);
    CHECK(t1 == t2);
}

TEST_CASE("re-emitter: number canonicalization table") {
    struct Row {
        std::string_view input;
        std::string_view expected;
    };
    constexpr std::array<Row, 12> PINNED = {{
        {"1", "1"},
        {"0", "0"},
        {"-10", "-10"},
        {"2.5", "2.5"},
        {"0.5", "0.5"},
        {"-0", "-0"},
        {"0.1", "0.1"},
        {"9223372036854775807", "9223372036854775807"},
        {"-9223372036854775808", "-9223372036854775808"},
        {"18446744073709551615", "18446744073709551615"},
        {"1e999", "null"},
        {"1e-999", "0"},
    }};
    for (const Row& row : PINNED) {
        const std::string t1 = reemit(row.input, engine::JsonWriterConfig{.pretty = false});
        CHECK(t1 == row.expected);
        const std::string t2 = reemit(t1, engine::JsonWriterConfig{.pretty = false});
        CHECK(t1 == t2);
    }

    // Idempotence-only (spec 3.11 / AC-9): the exact std::to_chars bytes are not byte-pinned here.
    constexpr std::array<std::string_view, 6> IDEMPOTENT_ONLY = {"1e2",   "1e20",   "100.0",
                                                                 "1e-45", "5e-324", "123456789012345678901"};
    for (const std::string_view input : IDEMPOTENT_ONLY) {
        const std::string t1 = reemit(input, engine::JsonWriterConfig{.pretty = false});
        const std::string t2 = reemit(t1, engine::JsonWriterConfig{.pretty = false});
        CHECK(t1 == t2);
    }
}

TEST_CASE("re-emitter: deep DOM at the parse cap") {
    const std::string deep = std::string(200, '[') + std::string(200, ']');
    const std::string t1 = reemit(deep, engine::JsonWriterConfig{.pretty = false});
    CHECK(t1 == deep);
    const std::string t2 = reemit(t1, engine::JsonWriterConfig{.pretty = false});
    CHECK(t1 == t2);
}

TEST_CASE("validateScene battery") {
    CHECK_FALSE(engine::validateScene(parseOk(MINIMAL_CANONICAL)).has_value());
    CHECK_FALSE(engine::validateScene(parseOk(FULL_CANONICAL)).has_value());

    engine::SceneDocument zeroId;
    zeroId.entities.push_back(engine::SceneEntityRecord{.id = 0});
    const std::optional<engine::SceneError> zeroIdError = engine::validateScene(zeroId);
    REQUIRE(zeroIdError.has_value());
    CHECK(contains(zeroIdError->message, "\"id\" must be an integer >= 1"));
    // D12: the DOM path (foundDetail) quotes a Number's lexeme -- "(found \"0\")", never bare
    // "(found 0)". validateScene must render the identical catalog form, proven by an exact
    // cross-path comparison against the DOM path's own message for the equivalent input.
    CHECK(contains(zeroIdError->message, "(found \"0\")"));
    CHECK(zeroIdError->message == parseFail(sceneWithEntity(R"({"id": 0})")).message);

    engine::SceneDocument dupIds;
    dupIds.entities.push_back(engine::SceneEntityRecord{.id = 1});
    dupIds.entities.push_back(engine::SceneEntityRecord{.id = 1});
    const std::optional<engine::SceneError> dupIdError = engine::validateScene(dupIds);
    REQUIRE(dupIdError.has_value());
    CHECK(contains(dupIdError->message, "duplicate entity id"));

    engine::SceneDocument unknownParent;
    unknownParent.entities.push_back(engine::SceneEntityRecord{.id = 1, .parent = 99});
    const std::optional<engine::SceneError> unknownParentError = engine::validateScene(unknownParent);
    REQUIRE(unknownParentError.has_value());
    CHECK(contains(unknownParentError->message, "does not reference any entity id"));

    engine::SceneDocument selfParent;
    selfParent.entities.push_back(engine::SceneEntityRecord{.id = 1, .parent = 1});
    const std::optional<engine::SceneError> selfParentError = engine::validateScene(selfParent);
    REQUIRE(selfParentError.has_value());
    CHECK(contains(selfParentError->message, "parent chain is cyclic"));

    engine::SceneEntityRecord withEmptyType;
    withEmptyType.id = 1;
    withEmptyType.components.push_back(
        engine::SceneComponentRecord{.type = "", .value = engine::JsonValue::object({})});
    engine::SceneDocument emptyType;
    emptyType.entities.push_back(std::move(withEmptyType));
    const std::optional<engine::SceneError> emptyTypeError = engine::validateScene(emptyType);
    REQUIRE(emptyTypeError.has_value());
    CHECK(contains(emptyTypeError->message, "component name must be a non-empty string"));

    // Gap ruled by Michael: two records of the same component type on one entity validate clean
    // today but break the write<->parse fixpoint (JSON object keys collapse last-wins on reload) --
    // parseScene itself can never build this document (D4/F3), so only a hand-built one exercises it.
    engine::SceneEntityRecord withDuplicateComponentType;
    withDuplicateComponentType.id = 1;
    withDuplicateComponentType.components.push_back(
        engine::SceneComponentRecord{.type = "engine::Transform", .value = engine::JsonValue::object({})});
    withDuplicateComponentType.components.push_back(
        engine::SceneComponentRecord{.type = "engine::Transform", .value = engine::JsonValue::object({})});
    engine::SceneDocument duplicateComponentType;
    duplicateComponentType.entities.push_back(std::move(withDuplicateComponentType));
    const std::optional<engine::SceneError> dupTypeError = engine::validateScene(duplicateComponentType);
    REQUIRE(dupTypeError.has_value());
    CHECK(dupTypeError->message == R"(entities[0] (id 1): duplicate component type "engine::Transform")");

    engine::SceneEntityRecord withArrayPayload;
    withArrayPayload.id = 1;
    withArrayPayload.components.push_back(
        engine::SceneComponentRecord{.type = "demo::Bad", .value = engine::JsonValue::array({})});
    engine::SceneDocument arrayPayload;
    arrayPayload.entities.push_back(std::move(withArrayPayload));
    const std::optional<engine::SceneError> arrayPayloadError = engine::validateScene(arrayPayload);
    REQUIRE(arrayPayloadError.has_value());
    CHECK(contains(arrayPayloadError->message, "payload must be an object (found array)"));

    // Purity: writeScene emits a duplicate-id document without error; the emitted text then fails
    // parseScene with the SAME message -- D14's surfacing contract. NEVER feed writeScene a
    // non-object payload -- the Debug assert would abort the lane (spec risk #4).
    const std::string emittedDup = engine::writeSceneText(dupIds);
    const engine::SceneError reparsedDup = parseFail(emittedDup);
    CHECK(reparsedDup.message == dupIdError->message);
}

TEST_CASE("determinism: repeated parse/write and repeated error text") {
    const engine::SceneDocument a = parseOk(FULL_CANONICAL);
    const engine::SceneDocument b = parseOk(FULL_CANONICAL);
    CHECK(engine::writeSceneText(a) == engine::writeSceneText(b));

    std::string manyEntities = R"({"version": 1, "entities": [)";
    for (std::uint64_t k = 1; k <= 8; ++k) {
        if (k != 1) {
            manyEntities += ", ";
        }
        manyEntities += std::format(R"({{"id": {}, "parent": 999}})", k);
    }
    manyEntities += "]}";
    const engine::SceneError e1 = parseFail(manyEntities);
    const engine::SceneError e2 = parseFail(manyEntities);
    CHECK(e1.message == e2.message);

    const std::string manyUnknownKeys =
        R"({"version": 1, "k1": 1, "k2": 2, "k3": 3, "k4": 4, "k5": 5, "k6": 6, "entities": []})";
    const engine::SceneDocument c1 = parseOk(manyUnknownKeys);
    const engine::SceneDocument c2 = parseOk(manyUnknownKeys);
    CHECK(engine::writeSceneText(c1) == engine::writeSceneText(c2));
}
