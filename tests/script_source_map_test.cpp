// Aero Engine -- source maps (task 4.3.4): the pure core and the VM's mapped errors. SR1-SR9 test the backend-free
// half (internal/source_map.hpp) through aero::script_internal, with no VM; SR10-SR17 drive the public API over a
// VirtualFileSystem and are added with the VM's use of the core. No timing is asserted anywhere: SR9 prints its two
// durations for the manual validation row that reads them.

#include <aero/script/internal/source_map.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace si = engine::script::internal;

// The c01 bundle of the spike: util.ts then c01.ts under Library/Scripts, a throw on util.ts line 2.
constexpr std::string_view C01_MAP = R"JSON({
  "version": 3,
  "sources": ["../../assets/scripts/util.ts", "../../assets/scripts/c01.ts"],
  "sourcesContent": ["export function fail(reason: string): never {\n  throw new Error(\"util failed: \" + reason);\n}\n", "import { fail } from \"./util\";\n\nexport default class Player {\n  hp: number = 3;\n  constructor() {\n    fail(\"from ctor\");\n  }\n}\n"],
  "mappings": ";AAAO,SAAS,KAAK,QAAuB;AAC1C,QAAM,IAAI,MAAM,kBAAkB,MAAM;AAC1C;;;ACAA,IAAqB,SAArB,MAA4B;AAAA,EAC1B,KAAa;AAAA,EACb,cAAc;AACZ,SAAK,WAAW;AAAA,EAClB;AACF;",
  "names": []
})JSON";

// One value in the map's variable-length quantity form: the inverse of the decoder's loop.
[[nodiscard]] std::string vlq(std::int64_t value) {
    constexpr std::string_view DIGITS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::uint64_t rest =
        value < 0 ? ((static_cast<std::uint64_t>(-value) << 1U) | 1U) : (static_cast<std::uint64_t>(value) << 1U);
    std::string out;
    do {
        std::uint64_t digit = rest & 31U;
        rest >>= 5U;
        if (rest != 0) {
            digit |= 32U;
        }
        out.push_back(DIGITS[digit]);
    } while (rest != 0);
    return out;
}

struct Seg {
    std::int64_t generatedColumn = 0;
    std::int64_t source = 0;
    std::int64_t originalLine = 0;
    std::int64_t originalColumn = 0;
};

// The `mappings` text of per-line segment lists, encoded relative as the format asks.
[[nodiscard]] std::string mappingsOf(const std::vector<std::vector<Seg>>& lines) {
    std::string out;
    std::int64_t source = 0;
    std::int64_t originalLine = 0;
    std::int64_t originalColumn = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out.push_back(';');
        }
        std::int64_t generatedColumn = 0;
        for (std::size_t j = 0; j < lines[i].size(); ++j) {
            const Seg& seg = lines[i][j];
            if (j != 0) {
                out.push_back(',');
            }
            out += vlq(seg.generatedColumn - generatedColumn) + vlq(seg.source - source) +
                   vlq(seg.originalLine - originalLine) + vlq(seg.originalColumn - originalColumn);
            generatedColumn = seg.generatedColumn;
            source = seg.source;
            originalLine = seg.originalLine;
            originalColumn = seg.originalColumn;
        }
    }
    return out;
}

[[nodiscard]] std::string mapJson(const std::string& sourcesArray, const std::string& mappings) {
    return R"({"version":3,"sources":)" + sourcesArray + R"(,"mappings":")" + mappings + R"("})";
}

// A decoded map's line, as (genCol, source, origLine0, origCol0) tuples.
using Tuple = std::array<std::uint32_t, 4>;
[[nodiscard]] std::vector<Tuple> lineOf(const si::DecodedSourceMap& map, std::size_t line) {
    std::vector<Tuple> out;
    for (std::uint32_t i = map.lineStarts[line]; i < map.lineStarts[line + 1]; ++i) {
        const si::MapSegment& s = map.segments[i];
        out.push_back({s.generatedColumn, s.source, s.originalLine, s.originalColumn});
    }
    return out;
}

[[nodiscard]] std::string refusalOf(std::string_view json) {
    const si::SourceMapDecode decoded = si::decodeSourceMap("res://m.js.map", json);
    CHECK_FALSE(decoded.map.has_value());
    return decoded.refusal;
}

[[nodiscard]] std::string mappingsRefusal(const std::string& mappings) {
    return refusalOf(mapJson(R"(["a.ts"])", mappings));
}

[[nodiscard]] si::DecodedSourceMap decodedOrDie(std::string_view mapPath, std::string_view json) {
    si::SourceMapDecode decoded = si::decodeSourceMap(mapPath, json);
    REQUIRE(decoded.map.has_value());
    return std::move(*decoded.map);
}

}  // namespace

TEST_CASE("source map: the URL is the text's last line, and only that (SR1)") {
    CHECK(si::sourceMappingUrl("x;\n//# sourceMappingURL=c01.js.map\n") ==
          std::optional<std::string_view>{"c01.js.map"});
    CHECK(si::sourceMappingUrl("x;\n//# sourceMappingURL=c01.js.map") == std::optional<std::string_view>{"c01.js.map"});
    CHECK(si::sourceMappingUrl("x;\r\n//# sourceMappingURL=c01.js.map\r\n") ==
          std::optional<std::string_view>{"c01.js.map"});
    // A fake URL line inside a template literal is not the last line.
    CHECK(si::sourceMappingUrl("var t = `\n//# sourceMappingURL=fake-template.js.map\n`;\n"
                               "//# sourceMappingURL=real.js.map\n") == std::optional<std::string_view>{"real.js.map"});
    // A URL line followed by another line is not a URL line.
    CHECK_FALSE(si::sourceMappingUrl("//# sourceMappingURL=a.map\nx;\n").has_value());
    CHECK_FALSE(si::sourceMappingUrl("x;\n//@ sourceMappingURL=a.map\n").has_value());
    CHECK_FALSE(si::sourceMappingUrl("x;\n //# sourceMappingURL=a.map\n").has_value());
    CHECK_FALSE(si::sourceMappingUrl("x;\n//# sourceMappingURL=\n").has_value());
    CHECK_FALSE(si::sourceMappingUrl("x;\n//# sourceMappingURL=a b.map\n").has_value());
    CHECK_FALSE(si::sourceMappingUrl("x;\n//# sourceMappingURL=a\tb.map\n").has_value());
    CHECK(si::sourceMappingUrl("x;\n//# sourceMappingURL=a.map  \t\r\n") == std::optional<std::string_view>{"a.map"});
    CHECK(si::sourceMappingUrl("//# sourceMappingURL=only.map") == std::optional<std::string_view>{"only.map"});
    CHECK_FALSE(si::sourceMappingUrl("").has_value());
    CHECK_FALSE(si::sourceMappingUrl("x;\n\n").has_value());
}

TEST_CASE("source map: a URL resolves against the module's directory, or is not followed (SR2)") {
    const auto resolve = [](std::string_view key, std::string_view url) { return si::resolveSourceMapPath(key, url); };
    const std::string scripts = "res://Library/Scripts/c01.js";
    CHECK(resolve(scripts, "c01.js.map") == std::optional<std::string>{"res://Library/Scripts/c01.js.map"});
    CHECK(resolve("res://c01.js", "c01.js.map") == std::optional<std::string>{"res://c01.js.map"});
    CHECK(resolve(scripts, "./x.map") == std::optional<std::string>{"res://Library/Scripts/x.map"});
    CHECK(resolve(scripts, "../maps/x.map") == std::optional<std::string>{"res://Library/maps/x.map"});
    CHECK_FALSE(resolve(scripts, "../../../x.map").has_value());
    CHECK_FALSE(resolve(scripts, "../..").has_value());
    CHECK(resolve(scripts, "res://other/x.map") == std::optional<std::string>{"res://other/x.map"});
    CHECK(resolve(scripts, "res://a/../b.map") == std::optional<std::string>{"res://b.map"});
    CHECK_FALSE(resolve(scripts, "data:application/json;base64,e30=").has_value());
    CHECK_FALSE(resolve(scripts, "file:///x.map").has_value());
    CHECK_FALSE(resolve(scripts, "https://h/x.map").has_value());
    CHECK_FALSE(resolve(scripts, "C:/x.map").has_value());
    CHECK_FALSE(resolve(scripts, "/abs/x.map").has_value());
    CHECK_FALSE(resolve(scripts, "x.map?v=1").has_value());
    CHECK_FALSE(resolve(scripts, "x.map#f").has_value());
    CHECK(resolve(scripts, "%C3%BC.js.map") == std::optional<std::string>{"res://Library/Scripts/\xC3\xBC.js.map"});
    CHECK(resolve(scripts, "100%25.map") == std::optional<std::string>{"res://Library/Scripts/100%.map"});
    CHECK_FALSE(resolve(scripts, "%zz.map").has_value());
    CHECK_FALSE(resolve(scripts, "x%2").has_value());
    // A canonical key holds raw bytes: its own '%' is not an escape.
    CHECK(resolve("res://100%/a.js", "a.js.map") == std::optional<std::string>{"res://100%/a.js.map"});
}

TEST_CASE("source map: a map decodes into per-line segments, the carried fields intact (SR3)") {
    const si::DecodedSourceMap c01 = decodedOrDie("res://Library/Scripts/c01.js.map", C01_MAP);
    REQUIRE(c01.sources.size() == 2);
    CHECK(c01.sources[0] == "res://assets/scripts/util.ts");
    CHECK(c01.sources[1] == "res://assets/scripts/c01.ts");
    CHECK(c01.lineStarts.size() == 14);
    CHECK(
        lineOf(c01, 2) ==
        std::vector<Tuple>{{0, 0, 1, 2}, {8, 0, 1, 8}, {12, 0, 1, 12}, {18, 0, 1, 18}, {36, 0, 1, 36}, {42, 0, 1, 42}});
    CHECK(lineOf(c01, 9) == std::vector<Tuple>{{0, 1, 5, 4}, {9, 1, 5, 9}, {20, 1, 5, 20}});

    const si::DecodedSourceMap odd = decodedOrDie(
        "res://m.js.map", R"({"version":3,"sources":["a.ts","b.ts"],"mappings":"A,EAAAA;;gBCGoB,LDDP;AAw+BA"})");
    REQUIRE(odd.lineStarts.size() == 5);
    CHECK(lineOf(odd, 0) == std::vector<Tuple>{{0, si::NO_SOURCE, 0, 0}, {2, 0, 0, 0}});
    CHECK(lineOf(odd, 1).empty());
    // Decoded out of order, stably sorted by generated column.
    CHECK(lineOf(odd, 2) == std::vector<Tuple>{{11, 0, 2, 13}, {16, 1, 3, 20}});
    // A three-digit value, and the source, line and column carried across the ';'.
    CHECK(lineOf(odd, 3) == std::vector<Tuple>{{0, 0, 1002, 13}});

    // 2^31 - 1, the largest position.
    const si::DecodedSourceMap largest = decodedOrDie("res://m.js.map", mapJson(R"(["a.ts"])", "+/////D"));
    REQUIRE(largest.segments.size() == 1);
    CHECK(largest.segments[0].generatedColumn == 0x7FFFFFFFU);
}

TEST_CASE("source map: every unusable map is refused whole, for its own reason (SR4)") {
    CHECK(refusalOf("{not json").starts_with("it is not JSON: "));
    CHECK(refusalOf("{not json").find("(line 1, column ") != std::string::npos);
    CHECK(refusalOf("[]") == "it is not a JSON object");
    CHECK(refusalOf(R"({"sources":[],"mappings":""})") == "its version is not 3");
    CHECK(refusalOf(R"({"version":2,"sources":[],"mappings":""})") == "its version is not 3");
    CHECK(refusalOf(R"({"version":"3","sources":[],"mappings":""})") == "its version is not 3");
    CHECK(refusalOf(R"({"version":3,"sections":[]})") == "it is an index map (sections), which the VM does not read");
    CHECK(refusalOf(R"({"version":3,"mappings":""})") == "its sources are not an array of strings");
    CHECK(refusalOf(R"({"version":3,"sources":"a.ts","mappings":""})") == "its sources are not an array of strings");
    CHECK(refusalOf(R"({"version":3,"sources":["a",1],"mappings":""})") == "its sources are not an array of strings");
    CHECK(refusalOf(R"({"version":3,"sources":[],"sourceRoot":7,"mappings":""})") == "its sourceRoot is not a string");
    CHECK(si::decodeSourceMap("res://m.js.map", R"({"version":3,"sources":[],"sourceRoot":null,"mappings":""})")
              .map.has_value());
    CHECK(refusalOf(R"({"version":3,"sources":[]})") == "its mappings are not a string");
    CHECK(refusalOf(R"({"version":3,"sources":[],"mappings":7})") == "its mappings are not a string");

    CHECK(mappingsRefusal("AA!A") == "its mappings do not decode: a character outside base64 at byte 0");
    CHECK(mappingsRefusal("AAAg") == "its mappings do not decode: a value that does not end at byte 0");
    CHECK(mappingsRefusal("ggggggE") == "its mappings do not decode: a value over 31 bits at byte 0");
    CHECK(mappingsRefusal("AAAA,,AAAA") == "its mappings do not decode: an empty segment at byte 5");
    CHECK(mappingsRefusal("AAAA,") == "its mappings do not decode: an empty segment at byte 5");
    CHECK(mappingsRefusal("AA") == "its mappings do not decode: a segment of 2 fields at byte 0");
    CHECK(mappingsRefusal("AAAAAA") == "its mappings do not decode: a segment of 6 fields at byte 0");
    CHECK(mappingsRefusal("ACAA") == "its mappings do not decode: a source index out of range at byte 0");
    CHECK(mappingsRefusal("D") == "its mappings do not decode: a position below zero at byte 0");
    CHECK(mappingsRefusal("+/////D,C") == "its mappings do not decode: a position over 31 bits at byte 8");
}

TEST_CASE("source map: a source shows as the engine's path for it, or as the map spells it (SR5)") {
    const auto name = [](std::string_view mapPath, std::string_view root, std::string_view source) {
        return si::sourceDisplayName(mapPath, root, source);
    };
    const std::string_view scripts = "res://Library/Scripts/x.js.map";
    CHECK(name(scripts, "", "../../assets/scripts/util.ts") == "res://assets/scripts/util.ts");
    // A map at the root has nothing above it: the spelling stays as written.
    CHECK(name("res://x.js.map", "", "../../assets/scripts/util.ts") == "../../assets/scripts/util.ts");
    const std::string_view q = "res://Library/Scripts/q.js.map";
    CHECK(name(q, "", "../../assets/my dir/sp ace.ts") == "res://assets/my dir/sp ace.ts");
    CHECK(name(q, "", "../../assets/%C3%A9%E2%82%AC/%C3%B1.ts") == "res://assets/\xC3\xA9\xE2\x82\xAC/\xC3\xB1.ts");
    CHECK(name(q, "", "../../assets/100%25.ts") == "res://assets/100%.ts");
    CHECK(name(q, "", "../../assets/a%23b.ts") == "res://assets/a#b.ts");
    CHECK(name(q, "", "../../assets/q%3Fx.ts") == "res://assets/q?x.ts");
    CHECK(name(scripts, "src", "a.ts") == "res://Library/Scripts/src/a.ts");
    CHECK(name(scripts, "src/", "a.ts") == "res://Library/Scripts/src/a.ts");
    CHECK(name(scripts, "res://lib", "a.ts") == "res://lib/a.ts");
    CHECK(name(scripts, "", "webpack:///a.ts") == "webpack:///a.ts");
    CHECK(name(scripts, "", "/abs/a.ts") == "/abs/a.ts");
    CHECK(name(scripts, "", "bad%zz.ts") == "bad%zz.ts");
    // The map's own directory is a canonical key, never decoded.
    CHECK(name("res://100%/x.js.map", "", "a.ts") == "res://100%/a.ts");
}

TEST_CASE("source map: the lookup is the last segment at or before the position, on its own line (SR6)") {
    si::DecodedSourceMap map;
    map.sources = {"res://a.ts"};
    // line 1: segments at 0 and 5; line 2: at 0 and 5; line 3: first segment at 4; line 4: none; line 5: a source-less
    // segment after a sourced one; line 6: two segments at one column.
    map.segments = {{0, 0, 10, 0}, {5, 0, 11, 1},             //
                    {0, 0, 20, 0}, {5, 0, 21, 1},             //
                    {4, 0, 30, 3},                            //
                    {0, 0, 40, 0}, {3, si::NO_SOURCE, 0, 0},  //
                    {2, 0, 50, 1}, {2, 0, 51, 2}};
    map.lineStarts = {0, 2, 4, 5, 5, 7, 9};

    const auto at = [&map](std::uint32_t line, std::uint32_t column) {
        return si::originalPositionFor(map, line, column);
    };
    // Line 1 prints a 0-based column.
    REQUIRE(at(1, 5).has_value());
    CHECK(at(1, 5)->line == 12);
    CHECK(at(1, 5)->column == 2);
    CHECK(at(1, 4)->line == 11);
    // Later lines print a 1-based column: 5 is byte 4, 6 is byte 5.
    CHECK(at(2, 5)->line == 21);
    CHECK(at(2, 6)->line == 22);
    CHECK(at(2, 6)->column == 2);
    CHECK(at(2, 6)->source == 0);
    // Before a line's first segment: nothing, and the previous line is never searched.
    CHECK_FALSE(at(3, 4).has_value());
    CHECK(at(3, 5)->line == 31);
    CHECK_FALSE(at(4, 9).has_value());
    CHECK_FALSE(at(0, 1).has_value());
    CHECK_FALSE(at(7, 1).has_value());
    // A one-field segment shadows the sourced one before it.
    CHECK(at(5, 3)->line == 41);
    CHECK_FALSE(at(5, 4).has_value());
    // Two segments at one column: the later.
    CHECK(at(6, 3)->line == 52);
}

TEST_CASE("source map: a stack's frames are rewritten by file, and only by a file the store holds (SR7)") {
    const std::string first = mapJson(R"(["a.ts"])", mappingsOf({{}, {{0, 0, 0, 0}, {4, 0, 4, 2}}}));
    const std::string second = mapJson(R"(["b.ts"])", mappingsOf({{}, {{0, 0, 0, 0}, {4, 0, 7, 1}}}));
    const std::string third = mapJson(R"(["u.ts"])", mappingsOf({{}, {{0, 0, 0, 0}, {4, 0, 2, 5}}}));
    si::SourceMapStore store;
    store.keepMap("res://s/a.js", "res://s/a.js.map", first);
    store.keepMap("res://s/a.js?gen=2", "res://s/a.js.map", second);
    store.keepMap("res://%C3%BC dir/a (b).js", "res://\xC3\xBC dir/a (b).js.map", third);

    const auto mapped = [&store](std::string_view stack) { return store.mapStack(stack); };
    CHECK(mapped("    at f (res://s/a.js:2:5)").stack == "    at f (res://s/a.ts:5:3)");
    CHECK(mapped("    at res://s/a.js:2:5").stack == "    at res://s/a.ts:5:3");
    CHECK(mapped("    at g (res://s/a.js?gen=2:2:5)").stack == "    at g (res://s/b.ts:8:2)");
    CHECK(mapped("    at a (b) (res://%C3%BC dir/a (b).js:2:5)").stack == "    at a (b) (res://\xC3\xBC dir/u.ts:3:6)");
    for (const std::string_view line :
         {"    at forged (res://s/a.js:2:5) (res://hand/x.js:3:3)", "    at h (res://s/other.js:2:5)",
          "    at h (<input>:2:5)", "    at map (native)", "    at h (res://s/a.js:99999999999:1)",
          "    at h (res://s/a.js:2:)"}) {
        INFO("line: " << std::string{line});
        const si::MappedStack out = mapped(line);
        CHECK(out.stack == line);
        CHECK_FALSE(out.site.has_value());
    }

    // The site is the first POSITIONED frame, when that frame mapped.
    const si::MappedStack native =
        mapped("    at map (native)\n    at f (res://s/a.js:2:5)\n    at g (res://s/a.js:2:1)");
    CHECK(native.stack == "    at map (native)\n    at f (res://s/a.ts:5:3)\n    at g (res://s/a.ts:1:1)");
    CHECK(native.site == std::optional<std::string>{"res://s/a.ts:5:3"});
    const si::MappedStack input = mapped("    at <input>:1:1\n    at f (res://s/a.js:2:5)");
    CHECK_FALSE(input.site.has_value());
    CHECK(mapped("    at f (<input>:3:4)\n    at map (native)").stack == "    at f (<input>:3:4)\n    at map (native)");
    CHECK_FALSE(mapped("    at map (native)").site.has_value());

    // At most 64 lines are read; the rest are copied.
    std::string seventy;
    std::string expected;
    for (int i = 0; i < 70; ++i) {
        seventy += "    at f (res://s/a.js:2:5)";
        expected += i < 64 ? "    at f (res://s/a.ts:5:3)" : "    at f (res://s/a.js:2:5)";
        if (i != 69) {
            seventy += '\n';
            expected += '\n';
        }
    }
    CHECK(mapped(seventy).stack == expected);

    si::SourceMapStore empty;
    const si::MappedStack untouched = empty.mapStack("    at f (res://s/a.js:2:5)");
    CHECK(untouched.stack == "    at f (res://s/a.js:2:5)");
    CHECK_FALSE(untouched.site.has_value());
    CHECK(untouched.warnings.empty());
}

TEST_CASE("source map: the store decodes at the first frame that names a slot, and warns once per map path (SR8)") {
    const std::string good = mapJson(R"(["a.ts"])", mappingsOf({{}, {{0, 0, 0, 0}}}));
    const std::string broken = R"({"version":2,"sources":[],"mappings":""})";
    const std::string frameA = "    at f (res://s/a.js:2:1)";
    si::SourceMapStore store;
    store.keepMap("res://s/a.js", "res://s/a.js.map", good);
    store.keepMap("res://s/idle.js", "res://s/idle.js.map", good);
    CHECK(store.slotState("res://s/a.js") == std::optional{si::SourceMapSlotState::Read});
    const si::MappedStack once = store.mapStack(frameA);
    CHECK(store.slotState("res://s/a.js") == std::optional{si::SourceMapSlotState::Decoded});
    CHECK(store.slotState("res://s/idle.js") == std::optional{si::SourceMapSlotState::Read});
    const si::MappedStack twice = store.mapStack(frameA);
    CHECK(once.stack == twice.stack);
    CHECK(once.site == twice.site);
    CHECK(once.stack == "    at f (res://s/a.ts:1:1)");

    store.keepMap("res://s/b.js", "res://s/a.js.map", broken);
    store.keepMap("res://s/c.js", "res://s/a.js.map", broken);
    store.keepMap("res://s/d.js", "res://s/d.js.map", broken);
    const si::MappedStack warned = store.mapStack("    at f (res://s/b.js:2:1)");
    CHECK(store.slotState("res://s/b.js") == std::optional{si::SourceMapSlotState::Unusable});
    CHECK(warned.stack == "    at f (res://s/b.js:2:1)");
    CHECK(warned.warnings ==
          std::vector<std::string>{"script: res://s/a.js.map: source map not used: its version is not 3; "
                                   "stack frames name the JavaScript"});
    CHECK(store.mapStack("    at f (res://s/b.js:2:1)").warnings.empty());
    // A second name sharing the broken map path owes nothing more; another path owes its own.
    CHECK(store.mapStack("    at f (res://s/c.js:2:1)").warnings.empty());
    const si::MappedStack other = store.mapStack("    at f (res://s/d.js:2:1)");
    REQUIRE(other.warnings.size() == 1);
    CHECK(other.warnings[0].starts_with("script: res://s/d.js.map: source map not used: its version is not 3"));

    store.keepUnusable("res://s/e.js", "res://s/e.js.map", std::string{si::MAP_UNREADABLE_REASON});
    CHECK(store.slotState("res://s/e.js") == std::optional{si::SourceMapSlotState::Unusable});
    const si::MappedStack unreadable = store.mapStack("    at f (res://s/e.js:2:1)");
    REQUIRE(unreadable.warnings.size() == 1);
    CHECK(unreadable.warnings[0] ==
          "script: res://s/e.js.map: source map not used: it could not be read through the VFS; "
          "stack frames name the JavaScript");
    CHECK(store.mapStack("    at f (res://s/e.js:2:1)").warnings.empty());

    // The latest read of a name replaces the slot, whatever it held.
    store.keepMap("res://s/e.js", "res://s/e.js.map", good);
    CHECK(store.slotState("res://s/e.js") == std::optional{si::SourceMapSlotState::Read});
    store.forget("res://s/e.js");
    CHECK_FALSE(store.slotState("res://s/e.js").has_value());
    CHECK(store.mapStack("    at f (res://s/e.js:2:1)").stack == "    at f (res://s/e.js:2:1)");
}

TEST_CASE("source map: a big map decodes once and a lookup does not walk it (SR9)") {
    constexpr std::int64_t LINES = 30000;
    constexpr std::int64_t SOURCES = 21;
    std::vector<std::vector<Seg>> lines;
    lines.reserve(LINES);
    for (std::int64_t g = 0; g < LINES; ++g) {
        lines.push_back(
            {{0, g % SOURCES, g, 0}, {10, g % SOURCES, g, 10}, {20, g % SOURCES, g, 20}, {30, g % SOURCES, g, 30}});
    }
    std::string sources = "[";
    for (std::int64_t i = 0; i < SOURCES; ++i) {
        sources += (i == 0 ? "\"s" : ",\"s") + std::to_string(i) + ".ts\"";
    }
    sources += "]";
    si::SourceMapStore store;
    store.keepMap("res://s/m.js", "res://s/m.js.map", mapJson(sources, mappingsOf(lines)));

    const std::string frame = "    at f (res://s/m.js:29999:25)";
    const std::string expected = "    at f (res://s/s" + std::to_string(29998 % SOURCES) + ".ts:29999:21)";
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t0 = Clock::now();
    const si::MappedStack first = store.mapStack(frame);
    const Clock::time_point t1 = Clock::now();
    const si::MappedStack second = store.mapStack(frame);
    const Clock::time_point t2 = Clock::now();
    CHECK(first.stack == expected);
    CHECK(second.stack == expected);
    CHECK(store.slotState("res://s/m.js") == std::optional{si::SourceMapSlotState::Decoded});
    MESSAGE("first mapStack (decode + lookup): "
            << std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() << " us, second (lookup only): "
            << std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() << " us");
}
