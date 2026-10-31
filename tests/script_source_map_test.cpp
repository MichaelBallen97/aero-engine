// Aero Engine -- source maps (task 4.3.4): the pure core and the VM's mapped errors. SR1-SR9 test the backend-free
// half (internal/source_map.hpp) through aero::script_internal, with no VM; SR10-SR21 drive the public API over a
// VirtualFileSystem. No timing is asserted anywhere: SR9 prints its two
// durations for the manual validation row that reads them.

#include <aero/assets/cooked_script.hpp>
#include <aero/core/log.hpp>
#include <aero/core/vfs.hpp>
#include <aero/script/bytecode.hpp>
#include <aero/script/internal/source_map.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <set>
#include <string>
#include <string_view>
#include <utility>
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

// ---- SR10-SR18: the VM maps every stack it describes ------------------------------------------------------------

namespace {

using engine::LogLevel;
using engine::script::EvalResult;
using engine::script::evalStatusLabel;
using engine::script::ScriptInstanceResult;
using engine::script::ScriptLoadResult;
using engine::script::scriptStatusLabel;
using engine::script::Vm;
using engine::script::VmConfig;
using script_test::expectCleanShutdown;
using script_test::LogCapture;

// esbuild 0.28.2's own bundle of the spike's c01.ts (util.ts then c01.ts), byte for byte: 16 lines, a throw on util.ts
// line 2, the URL line last.
constexpr std::string_view C01_JS = R"JS(// assets/scripts/util.ts
function fail(reason) {
  throw new Error("util failed: " + reason);
}

// assets/scripts/c01.ts
var Player = class {
  hp = 3;
  constructor() {
    fail("from ctor");
  }
};
export {
  Player as default
};
//# sourceMappingURL=c01.js.map
)JS";

constexpr std::string_view C01_PATH = "res://Library/Scripts/c01.js";
constexpr std::string_view C01_MAP_FILE = "Library/Scripts/c01.js.map";
constexpr std::string_view C01_MAPPED_STACK =
    "    at fail (res://assets/scripts/util.ts:2:37)\n    at Player (res://assets/scripts/c01.ts:6:5)";
constexpr std::string_view C01_UNMAPPED_STACK =
    "    at fail (res://Library/Scripts/c01.js:3:37)\n    at Player (res://Library/Scripts/c01.js:10:5)";
constexpr std::string_view C01_MAPPED_HEAD =
    "script: res://assets/scripts/util.ts:2:37: uncaught Error: util failed: from ctor\n";
constexpr std::string_view C01_UNMAPPED_HEAD =
    "script: res://Library/Scripts/c01.js: uncaught Error: util failed: from ctor\n";

// A module source in memory, with per-path read counts.
class MapBackend final : public engine::FileSystemBackend {
public:
    void set(std::string relPath, std::string text) { files[std::move(relPath)] = std::move(text); }

    void remove(std::string_view relPath) {
        if (const auto found = files.find(relPath); found != files.end()) {
            files.erase(found);
        }
    }

    [[nodiscard]] bool exists(std::string_view relPath) const override { return files.contains(relPath); }

    // fileSize answers `size` for relPath whatever the file holds: nullopt for a file that exists, or a size it
    // outgrew before it is read.
    void claimSize(std::string relPath, std::optional<std::uint64_t> size) { claimedSizes[std::move(relPath)] = size; }

    // readFile refuses relPath although it exists; the refused read is still counted.
    void refuseRead(std::string relPath) { refusedReads.insert(std::move(relPath)); }

    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view relPath) const override {
        if (const auto claimed = claimedSizes.find(relPath); claimed != claimedSizes.end()) {
            return claimed->second;
        }
        const auto found = files.find(relPath);
        if (found == files.end()) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(found->second.size());
    }

    [[nodiscard]] std::optional<engine::ByteBuffer> readFile(std::string_view relPath) const override {
        ++readCounts[std::string{relPath}];
        const auto found = files.find(relPath);
        if (found == files.end() || refusedReads.contains(relPath)) {
            return std::nullopt;
        }
        engine::ByteBuffer bytes;
        bytes.reserve(found->second.size());
        for (const char c : found->second) {
            bytes.push_back(static_cast<std::byte>(c));
        }
        return bytes;
    }

    [[nodiscard]] std::size_t reads(std::string_view relPath) const {
        const auto found = readCounts.find(relPath);
        return found == readCounts.end() ? 0U : found->second;
    }

private:
    std::map<std::string, std::string, std::less<>> files;
    std::map<std::string, std::optional<std::uint64_t>, std::less<>> claimedSizes;
    std::set<std::string, std::less<>> refusedReads;
    mutable std::map<std::string, std::size_t, std::less<>> readCounts;
};

// A VM over a VFS whose root is one MapBackend. Non-movable: the Vm borrows the VFS's address.
class VmRig {
public:
    explicit VmRig(const VmConfig& config = script_test::patient()) {
        auto backend = std::make_unique<MapBackend>();
        files = backend.get();
        vfs.mount(std::move(backend));
        engine::script::VmCreateResult created = Vm::create(config, vfs);
        REQUIRE(created.vm.valid());
        vm = std::move(created.vm);
    }
    VmRig(const VmRig&) = delete;
    VmRig& operator=(const VmRig&) = delete;
    VmRig(VmRig&&) = delete;
    VmRig& operator=(VmRig&&) = delete;
    ~VmRig() = default;

    engine::VirtualFileSystem vfs;
    MapBackend* files = nullptr;
    Vm vm;
};

// `text` with its first `from` replaced by `to`; the case fails when `from` is absent.
[[nodiscard]] std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

// A map whose generated line g (1-based, of `lines`) has one segment at column 0, on `source` line firstOriginal + g
// (1-based), column 1.
[[nodiscard]] std::string lineMap(std::size_t lines, std::int64_t firstOriginal, std::string_view source) {
    std::string mappings = "AA" + vlq(firstOriginal) + "A";
    for (std::size_t i = 1; i < lines; ++i) {
        mappings += ";AACA";
    }
    return R"({"version":3,"sources":[")" + std::string{source} + R"("],"mappings":")" + mappings + R"("})";
}

// Every record at Warn or above, which a case expects to be exactly one ERROR; its text.
[[nodiscard]] std::string theOneError(const LogCapture& log) {
    const std::vector<script_test::CapturedRecord> loud = log.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 1U);
    CHECK(loud[0].level == LogLevel::Error);
    return loud[0].message;
}

[[nodiscard]] std::string statusOf(const ScriptInstanceResult& result) {
    return std::string{scriptStatusLabel(result.status)};
}

[[nodiscard]] std::string statusOf(const ScriptLoadResult& result) {
    return std::string{scriptStatusLabel(result.status)};
}

[[nodiscard]] std::string statusOf(const EvalResult& result) { return std::string{evalStatusLabel(result.status)}; }

[[nodiscard]] std::string cookedContainerOf(std::string_view js) {
    const engine::script::ScriptCookResult cooked = engine::script::cookScriptBytecode(js, "res://x.js");
    REQUIRE(cooked.status == engine::script::ScriptCookStatus::Ok);
    const engine::assets::CookedScriptWriteResult written =
        engine::assets::writeCookedScript(engine::assets::CookedScriptInput{
            engine::Guid{}, cooked.backendRelease, cooked.bytecodeVersion, cooked.stripFlags, cooked.payload});
    REQUIRE(written.status == engine::assets::CookedScriptStatus::Ok);
    return std::string{reinterpret_cast<const char*>(written.bytes.data()), written.bytes.size()};
}

}  // namespace

TEST_CASE("script: a thrown error's stack and record name the .ts file, line and column (SR10)") {
    const LogCapture log;
    VmRig rig;
    rig.files->set("Library/Scripts/c01.js", std::string{C01_JS});
    rig.files->set(std::string{C01_MAP_FILE}, std::string{C01_MAP});
    const ScriptLoadResult loaded = rig.vm.loadScript(C01_PATH);
    REQUIRE(statusOf(loaded) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);

    const ScriptInstanceResult made = rig.vm.createInstance(C01_PATH);
    CHECK(statusOf(made) == "Exception");
    CHECK(made.error.name == "Error");
    CHECK(made.error.message == "util failed: from ctor");
    CHECK(made.error.stack == C01_MAPPED_STACK);
    CHECK(theOneError(log) == std::string{C01_MAPPED_HEAD} + std::string{C01_MAPPED_STACK});
    expectCleanShutdown(rig.vm);
}

TEST_CASE("script: every stack shape is mapped, and the record's site follows its first positioned frame (SR11)") {
    SUBCASE("a compile error's unparenthesised frame") {
        const LogCapture log;
        VmRig rig;
        rig.files->set("Library/Scripts/c08.js",
                       replaced(replaced(std::string{C01_JS}, "  hp = 3;", "  hp = = 3;"), "c01.js.map", "c08.js.map"));
        rig.files->set("Library/Scripts/c08.js.map", std::string{C01_MAP});
        const EvalResult result = rig.vm.evaluateModule("res://Library/Scripts/c08.js");
        CHECK(statusOf(result) == "CompileError");
        CHECK(result.error.stack == "    at res://assets/scripts/c01.ts:4:16");
        CHECK(theOneError(log) ==
              "script: res://assets/scripts/c01.ts:4:16: compile error: SyntaxError: unexpected token in expression: "
              "'='\n    at res://assets/scripts/c01.ts:4:16");
        expectCleanShutdown(rig.vm);
    }
    SUBCASE("a module body that throws") {
        const LogCapture log;
        VmRig rig;
        rig.files->set("s/top.js",
                       "// top\n// top\nthrow new Error(\"at top level\");\n//# sourceMappingURL=top.js.map\n");
        rig.files->set("s/top.js.map", lineMap(3, 100, "top.ts"));
        const EvalResult result = rig.vm.evaluateModule("res://s/top.js");
        CHECK(statusOf(result) == "Exception");
        CHECK(theOneError(log) ==
              "script: res://s/top.ts:103:1: uncaught Error: at top level\n    at <anonymous> (res://s/top.ts:103:1)");
        expectCleanShutdown(rig.vm);
    }
    SUBCASE("an unhandled rejection") {
        const LogCapture log;
        VmRig rig;
        rig.files->set("s/rej.js",
                       "// rej\nasync function later() {\n  await 0;\n  throw new Error(\"async rejected\");\n}\n"
                       "export default class {\n  constructor() { later(); }\n}\n//# sourceMappingURL=rej.js.map\n");
        rig.files->set("s/rej.js.map", lineMap(8, 100, "rej.ts"));
        REQUIRE(statusOf(rig.vm.loadScript("res://s/rej.js")) == "Ok");
        CHECK(statusOf(rig.vm.createInstance("res://s/rej.js")) == "Ok");
        CHECK(theOneError(log) ==
              "script: res://s/rej.ts:104:1: unhandled promise rejection: Error: async rejected\n"
              "    at later (res://s/rej.ts:104:1)");
        expectCleanShutdown(rig.vm);
    }
    SUBCASE("an interrupt") {
        LogCapture log;
        VmRig rig{VmConfig{.callTimeBudget = std::chrono::milliseconds{50}}};
        rig.files->set("s/loop.js",
                       "export default class {\n  constructor() {\n    for (;;) {}\n  }\n}\n"
                       "//# sourceMappingURL=loop.js.map\n");
        rig.files->set("s/loop.js.map", lineMap(5, 0, "loop.ts"));
        REQUIRE(statusOf(rig.vm.loadScript("res://s/loop.js")) == "Ok");
        log.clear();
        const ScriptInstanceResult made = rig.vm.createInstance("res://s/loop.js");
        CHECK(statusOf(made) == "Interrupted");
        REQUIRE_FALSE(made.error.stack.empty());
        std::size_t begin = 0;
        std::string firstFrame;
        while (begin <= made.error.stack.size()) {
            std::size_t end = made.error.stack.find('\n', begin);
            if (end == std::string::npos) {
                end = made.error.stack.size();
            }
            const std::string line = made.error.stack.substr(begin, end - begin);
            CHECK(line.find("res://s/loop.ts:") != std::string::npos);
            if (firstFrame.empty()) {
                const std::size_t open = line.find('(');
                const std::size_t close = line.rfind(')');
                REQUIRE(open != std::string::npos);
                REQUIRE(close != std::string::npos);
                firstFrame = line.substr(open + 1, close - open - 1);
            }
            begin = end + 1;
        }
        CHECK(theOneError(log) ==
              "script: " + firstFrame + ": interrupted after exceeding the 50 ms time budget\n" + made.error.stack);
        expectCleanShutdown(rig.vm);
    }
    SUBCASE("a record with no stack keeps the call's file") {
        const LogCapture log;
        VmRig rig;
        rig.files->set("s/imp.js", "import \"./missing.js\";\n//# sourceMappingURL=imp.js.map\n");
        rig.files->set("s/imp.js.map", lineMap(1, 0, "imp.ts"));
        const EvalResult result = rig.vm.evaluateModule("res://s/imp.js");
        CHECK(statusOf(result) == "LoadError");
        CHECK(theOneError(log).starts_with("script: res://s/imp.js: load error: TypeError: "));
        expectCleanShutdown(rig.vm);
    }
}

TEST_CASE(
    "script: a missing map, no URL line or a URL the VM does not follow changes nothing and warns nothing (SR12)") {
    const std::string full{C01_JS};
    const std::string withoutUrl = full.substr(0, full.find("//# sourceMappingURL"));
    struct Arm {
        std::string label;
        std::string js;
        bool mapOnDisk = false;
    };
    const std::vector<Arm> arms{
        {"no map file", full, false},
        {"no URL line", withoutUrl, true},
        {"a data: URL", replaced(full, "c01.js.map", "data:application/json;base64,e30="), true},
        {"an https URL", replaced(full, "c01.js.map", "https://example.com/c01.js.map"), true},
    };
    for (const Arm& arm : arms) {
        INFO("arm: " << arm.label);
        const LogCapture log;
        VmRig rig;
        rig.files->set("Library/Scripts/c01.js", arm.js);
        if (arm.mapOnDisk) {
            rig.files->set(std::string{C01_MAP_FILE}, std::string{C01_MAP});
        }
        REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "Ok");
        const ScriptInstanceResult made = rig.vm.createInstance(C01_PATH);
        CHECK(statusOf(made) == "Exception");
        CHECK(made.error.stack == C01_UNMAPPED_STACK);
        CHECK(theOneError(log) == std::string{C01_UNMAPPED_HEAD} + std::string{C01_UNMAPPED_STACK});
        CHECK(rig.files->reads(C01_MAP_FILE) == 0U);
        expectCleanShutdown(rig.vm);
    }
}

TEST_CASE("script: a map that exists and cannot be used is one WARN, before the error it explains (SR13)") {
    const std::string tail = "; stack frames name the JavaScript";
    SUBCASE("not JSON") {
        const LogCapture log;
        VmRig rig;
        rig.files->set("Library/Scripts/c01.js", std::string{C01_JS});
        rig.files->set(std::string{C01_MAP_FILE}, "{not json");
        REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "Ok");
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);  // nothing is logged at load

        CHECK(statusOf(rig.vm.createInstance(C01_PATH)) == "Exception");
        const std::vector<script_test::CapturedRecord> first = log.atLeast(LogLevel::Warn);
        REQUIRE(first.size() == 2U);
        CHECK(first[0].level == LogLevel::Warn);
        CHECK(first[0].message.starts_with(
            "script: res://Library/Scripts/c01.js.map: source map not used: it is not JSON: "));
        CHECK(first[0].message.ends_with(tail));
        CHECK(first[1].level == LogLevel::Error);
        CHECK(first[1].message == std::string{C01_UNMAPPED_HEAD} + std::string{C01_UNMAPPED_STACK});

        CHECK(statusOf(rig.vm.createInstance(C01_PATH)) == "Exception");
        const std::vector<script_test::CapturedRecord> second = log.atLeast(LogLevel::Warn);
        REQUIRE(second.size() == 3U);
        CHECK(second[2].level == LogLevel::Error);
        expectCleanShutdown(rig.vm);
    }
    SUBCASE("over the heap limit") {
        const LogCapture log;
        VmRig rig{script_test::patient(VmConfig{.heapLimitBytes = engine::script::MIN_HEAP_LIMIT_BYTES})};
        std::string padded{C01_MAP};
        padded.resize(1048577U, ' ');
        rig.files->set("Library/Scripts/c01.js", std::string{C01_JS});
        rig.files->set(std::string{C01_MAP_FILE}, padded);
        REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "Ok");
        CHECK(statusOf(rig.vm.createInstance(C01_PATH)) == "Exception");
        const std::vector<script_test::CapturedRecord> loud = log.atLeast(LogLevel::Warn);
        REQUIRE(loud.size() == 2U);
        CHECK(loud[0].message ==
              "script: res://Library/Scripts/c01.js.map: source map not used: its 1048577 bytes are over the "
              "1048576-byte limit (the VM's heap limit); stack frames name the JavaScript");
        CHECK(rig.files->reads(C01_MAP_FILE) == 0U);  // refused on its size, never read
        expectCleanShutdown(rig.vm);
    }
    SUBCASE("a version that is not 3") {
        const LogCapture log;
        VmRig rig;
        rig.files->set("Library/Scripts/c01.js", std::string{C01_JS});
        rig.files->set(std::string{C01_MAP_FILE}, replaced(std::string{C01_MAP}, "\"version\": 3", "\"version\": 2"));
        REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "Ok");
        CHECK(statusOf(rig.vm.createInstance(C01_PATH)) == "Exception");
        const std::vector<script_test::CapturedRecord> loud = log.atLeast(LogLevel::Warn);
        REQUIRE(loud.size() == 2U);
        CHECK(loud[0].message ==
              "script: res://Library/Scripts/c01.js.map: source map not used: its version is not 3" + tail);
        expectCleanShutdown(rig.vm);
    }
}

TEST_CASE("script: each generation maps through the map read with its own JavaScript (SR14)") {
    const auto source = [](std::string_view message, std::string_view guard, std::string_view padding) {
        std::string text = "// p\nexport default class P {\n  constructor() {\n";
        text += padding;
        text += "    ";
        text += guard;
        text += "throw new Error(\"";
        text += message;
        text += "\");\n  }\n}\n//# sourceMappingURL=p.js.map\n";
        return text;
    };
    LogCapture log;
    VmRig rig;
    // v1: the throw is on generated line 4, and only when globalThis.boom is set.
    rig.files->set("s/p.js", source("v1", "if (globalThis.boom) ", ""));
    rig.files->set("s/p.js.map", lineMap(7, 100, "p.ts"));
    REQUIRE(statusOf(rig.vm.loadScript("res://s/p.js")) == "Ok");
    REQUIRE(statusOf(rig.vm.createInstance("res://s/p.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);

    // v2 always throws, on generated line 5: the rebuilt instance threw, so the reload is refused.
    rig.files->set("s/p.js", source("v2", "", "    // pad\n"));
    rig.files->set("s/p.js.map", lineMap(8, 200, "p.ts"));
    const ScriptLoadResult refused = rig.vm.loadScript("res://s/p.js");
    CHECK(statusOf(refused) == "Exception");
    CHECK(theOneError(log) == "script: res://s/p.ts:205:1: uncaught Error: v2\n    at P (res://s/p.ts:205:1)");

    // Generation 1's class is still the script's: its frames map through generation 1's map, not the one on disk.
    log.clear();
    REQUIRE(statusOf(rig.vm.evaluateScript("globalThis.boom = true", "set.js")) == "Ok");
    CHECK(statusOf(rig.vm.createInstance("res://s/p.js")) == "Exception");
    CHECK(theOneError(log) == "script: res://s/p.ts:104:1: uncaught Error: v1\n    at P (res://s/p.ts:104:1)");

    // v3 loads, and its own map is the one it reads.
    rig.files->set("s/p.js", source("v3", "if (globalThis.boom3) ", ""));
    rig.files->set("s/p.js.map", lineMap(7, 300, "p.ts"));
    log.clear();
    REQUIRE(statusOf(rig.vm.evaluateScript("globalThis.boom = false", "unset.js")) == "Ok");
    REQUIRE(statusOf(rig.vm.loadScript("res://s/p.js")) == "Ok");
    REQUIRE(statusOf(rig.vm.evaluateScript("globalThis.boom3 = true", "set3.js")) == "Ok");
    log.clear();
    CHECK(statusOf(rig.vm.createInstance("res://s/p.js")) == "Exception");
    CHECK(theOneError(log) == "script: res://s/p.ts:304:1: uncaught Error: v3\n    at P (res://s/p.ts:304:1)");
    expectCleanShutdown(rig.vm);
}

TEST_CASE("script: the map is read with its JavaScript, once, and only by a compile (SR15)") {
    const LogCapture log;
    VmRig rig;
    rig.files->set("Library/Scripts/c01.js", std::string{C01_JS});
    rig.files->set(std::string{C01_MAP_FILE}, std::string{C01_MAP});
    REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "Ok");
    CHECK(rig.files->reads(C01_MAP_FILE) == 1U);

    // The file is gone after the load: the kept map still answers.
    rig.files->remove(C01_MAP_FILE);
    const ScriptInstanceResult made = rig.vm.createInstance(C01_PATH);
    CHECK(statusOf(made) == "Exception");
    CHECK(made.error.stack == C01_MAPPED_STACK);
    CHECK(theOneError(log) == std::string{C01_MAPPED_HEAD} + std::string{C01_MAPPED_STACK});

    // The same bytes are Unchanged: the comparison reads no map.
    CHECK(statusOf(rig.vm.loadScript(C01_PATH)) == "Unchanged");
    CHECK(rig.files->reads(C01_MAP_FILE) == 1U);

    // A broken bundle at the same path: a new generation reads its own map, and its frame is mapped through it.
    rig.files->set(std::string{C01_MAP_FILE}, std::string{C01_MAP});
    const std::string broken = replaced(std::string{C01_JS}, "  hp = 3;", "  hp = = 3;");
    rig.files->set("Library/Scripts/c01.js", broken);
    const ScriptLoadResult refused = rig.vm.loadScript(C01_PATH);
    CHECK(statusOf(refused) == "CompileError");
    CHECK(refused.error.stack == "    at res://assets/scripts/c01.ts:4:16");
    CHECK(rig.files->reads(C01_MAP_FILE) == 2U);

    // The same refused bytes again: nothing is compiled, so nothing is read.
    CHECK(statusOf(rig.vm.loadScript(C01_PATH)) == "AlreadyRefused");
    CHECK(rig.files->reads(C01_MAP_FILE) == 2U);
    expectCleanShutdown(rig.vm);
}

TEST_CASE("script: a cooked script reads no map and its frames name the bundle's positions (SR16)") {
    const LogCapture log;
    VmRig rig;
    rig.files->set("Library/Scripts/c01.aeroscript", cookedContainerOf(C01_JS));
    rig.files->set(std::string{C01_MAP_FILE}, std::string{C01_MAP});
    rig.files->set("Library/Scripts/c01.aeroscript.map", std::string{C01_MAP});
    constexpr std::string_view COOKED = "res://Library/Scripts/c01.aeroscript";
    REQUIRE(statusOf(rig.vm.loadScript(COOKED)) == "Ok");
    const ScriptInstanceResult made = rig.vm.createInstance(COOKED);
    CHECK(statusOf(made) == "Exception");
    const std::string expectedStack =
        "    at fail (res://Library/Scripts/c01.aeroscript:3:37)\n"
        "    at Player (res://Library/Scripts/c01.aeroscript:10:5)";
    CHECK(made.error.stack == expectedStack);
    CHECK(theOneError(log) ==
          "script: res://Library/Scripts/c01.aeroscript: uncaught Error: util failed: from ctor\n" + expectedStack);
    CHECK(rig.files->reads(C01_MAP_FILE) == 0U);
    CHECK(rig.files->reads("Library/Scripts/c01.aeroscript.map") == 0U);
    expectCleanShutdown(rig.vm);
}

TEST_CASE("script: a cooked script read under a name a failed JavaScript read filled uses no map (SR18)") {
    LogCapture log;
    VmRig rig;
    // The JavaScript names a map and fails to compile: it is never created, so its key's generation does not move and
    // the cooked container read next compiles under the same name.
    rig.files->set("Library/Scripts/c01.js", replaced(std::string{C01_JS}, "  hp = 3;", "  hp = = 3;"));
    rig.files->set(std::string{C01_MAP_FILE}, std::string{C01_MAP});
    CHECK(statusOf(rig.vm.loadScript(C01_PATH)) == "CompileError");
    CHECK(rig.files->reads(C01_MAP_FILE) == 1U);
    log.clear();

    rig.files->set("Library/Scripts/c01.js", cookedContainerOf(C01_JS));
    REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "Ok");
    const ScriptInstanceResult made = rig.vm.createInstance(C01_PATH);
    CHECK(statusOf(made) == "Exception");
    CHECK(made.error.stack == C01_UNMAPPED_STACK);
    CHECK(theOneError(log) == std::string{C01_UNMAPPED_HEAD} + std::string{C01_UNMAPPED_STACK});
    CHECK(rig.files->reads(C01_MAP_FILE) == 1U);  // the cooked read reads no map
    expectCleanShutdown(rig.vm);
}

TEST_CASE("script: a JavaScript read after a failed compile under the same name clears the map it left (SR19)") {
    // The broken bundle names an existing map and fails to compile: it is never created, so its key's generation does
    // not move, and the next read compiles under the same name, over the slot the failed read filled.
    const std::string broken = replaced(std::string{C01_JS}, "  hp = 3;", "  hp = = 3;");
    const std::string full{C01_JS};
    SUBCASE("the next text has no URL line") {
        LogCapture log;
        VmRig rig;
        rig.files->set("Library/Scripts/c01.js", broken);
        rig.files->set(std::string{C01_MAP_FILE}, std::string{C01_MAP});
        REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "CompileError");
        REQUIRE(rig.files->reads(C01_MAP_FILE) == 1U);
        log.clear();

        rig.files->set("Library/Scripts/c01.js", full.substr(0, full.find("//# sourceMappingURL")));
        REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "Ok");
        const ScriptInstanceResult made = rig.vm.createInstance(C01_PATH);
        CHECK(statusOf(made) == "Exception");
        CHECK(made.error.stack == C01_UNMAPPED_STACK);
        CHECK(theOneError(log) == std::string{C01_UNMAPPED_HEAD} + std::string{C01_UNMAPPED_STACK});
        CHECK(rig.files->reads(C01_MAP_FILE) == 1U);
        expectCleanShutdown(rig.vm);
    }
    SUBCASE("the next text names a map that no longer exists") {
        LogCapture log;
        VmRig rig;
        rig.files->set("Library/Scripts/c01.js", broken);
        rig.files->set(std::string{C01_MAP_FILE}, std::string{C01_MAP});
        REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "CompileError");
        REQUIRE(rig.files->reads(C01_MAP_FILE) == 1U);
        log.clear();

        rig.files->remove(C01_MAP_FILE);
        rig.files->set("Library/Scripts/c01.js", full);
        REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "Ok");
        const ScriptInstanceResult made = rig.vm.createInstance(C01_PATH);
        CHECK(statusOf(made) == "Exception");
        CHECK(made.error.stack == C01_UNMAPPED_STACK);
        CHECK(theOneError(log) == std::string{C01_UNMAPPED_HEAD} + std::string{C01_UNMAPPED_STACK});
        CHECK(rig.files->reads(C01_MAP_FILE) == 1U);
        expectCleanShutdown(rig.vm);
    }
}

TEST_CASE("script: a statically imported and an import()ed module each read their own map (SR20)") {
    // Only the imported module names a map; the entry has none, so every mapped frame is the import's.
    constexpr std::string_view LIB =
        "// lib\nexport function boom() {\n  throw new Error(\"from lib\");\n}\n//# sourceMappingURL=lib.js.map\n";
    SUBCASE("a static import") {
        const LogCapture log;
        VmRig rig;
        rig.files->set("s/main.js", "import { boom } from \"./lib.js\";\nboom();\n");
        rig.files->set("s/lib.js", std::string{LIB});
        rig.files->set("s/lib.js.map", lineMap(5, 100, "lib.ts"));
        const EvalResult result = rig.vm.evaluateModule("res://s/main.js");
        CHECK(statusOf(result) == "Exception");
        const std::string stack = "    at boom (res://s/lib.ts:103:1)\n    at <anonymous> (res://s/main.js:2:1)";
        CHECK(result.error.stack == stack);
        CHECK(theOneError(log) == "script: res://s/lib.ts:103:1: uncaught Error: from lib\n" + stack);
        CHECK(rig.files->reads("s/lib.js.map") == 1U);
        expectCleanShutdown(rig.vm);
    }
    SUBCASE("an import()") {
        const LogCapture log;
        VmRig rig;
        rig.files->set("s/main.js", "import(\"./lib.js\").then((lib) => {\n  lib.boom();\n});\n");
        rig.files->set("s/lib.js", std::string{LIB});
        rig.files->set("s/lib.js.map", lineMap(5, 100, "lib.ts"));
        const EvalResult result = rig.vm.evaluateModule("res://s/main.js");
        CHECK(statusOf(result) == "Ok");
        CHECK(theOneError(log) ==
              "script: res://s/lib.ts:103:1: unhandled promise rejection: Error: from lib\n"
              "    at boom (res://s/lib.ts:103:1)\n    at <anonymous> (res://s/main.js:2:7)");
        CHECK(rig.files->reads("s/lib.js.map") == 1U);
        expectCleanShutdown(rig.vm);
    }
}

TEST_CASE("script: a map the VFS sizes or reads and then refuses is one WARN naming the refusal (SR21)") {
    const std::string unreadable =
        "script: res://Library/Scripts/c01.js.map: source map not used: it could not be read "
        "through the VFS; stack frames name the JavaScript";
    SUBCASE("its size is refused while it exists") {
        const LogCapture log;
        VmRig rig;
        rig.files->set("Library/Scripts/c01.js", std::string{C01_JS});
        rig.files->set(std::string{C01_MAP_FILE}, std::string{C01_MAP});
        rig.files->claimSize(std::string{C01_MAP_FILE}, std::nullopt);
        REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "Ok");
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
        const ScriptInstanceResult made = rig.vm.createInstance(C01_PATH);
        CHECK(statusOf(made) == "Exception");
        CHECK(made.error.stack == C01_UNMAPPED_STACK);
        const std::vector<script_test::CapturedRecord> loud = log.atLeast(LogLevel::Warn);
        REQUIRE(loud.size() == 2U);
        CHECK(loud[0].level == LogLevel::Warn);
        CHECK(loud[0].message == unreadable);
        CHECK(loud[1].level == LogLevel::Error);
        CHECK(loud[1].message == std::string{C01_UNMAPPED_HEAD} + std::string{C01_UNMAPPED_STACK});
        CHECK(rig.files->reads(C01_MAP_FILE) == 0U);  // refused on its size, never read
        expectCleanShutdown(rig.vm);
    }
    SUBCASE("its read is refused") {
        const LogCapture log;
        VmRig rig;
        rig.files->set("Library/Scripts/c01.js", std::string{C01_JS});
        rig.files->set(std::string{C01_MAP_FILE}, std::string{C01_MAP});
        rig.files->refuseRead(std::string{C01_MAP_FILE});
        REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "Ok");
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
        const ScriptInstanceResult made = rig.vm.createInstance(C01_PATH);
        CHECK(statusOf(made) == "Exception");
        CHECK(made.error.stack == C01_UNMAPPED_STACK);
        const std::vector<script_test::CapturedRecord> loud = log.atLeast(LogLevel::Warn);
        REQUIRE(loud.size() == 2U);
        CHECK(loud[0].level == LogLevel::Warn);
        CHECK(loud[0].message == unreadable);
        CHECK(loud[1].level == LogLevel::Error);
        CHECK(loud[1].message == std::string{C01_UNMAPPED_HEAD} + std::string{C01_UNMAPPED_STACK});
        CHECK(rig.files->reads(C01_MAP_FILE) == 1U);  // sized, then its one read refused
        expectCleanShutdown(rig.vm);
    }
    SUBCASE("it grew past the limit after its size was read") {
        const LogCapture log;
        VmRig rig{script_test::patient(VmConfig{.heapLimitBytes = engine::script::MIN_HEAP_LIMIT_BYTES})};
        std::string padded{C01_MAP};  // still a valid map: only the size refuses it
        padded.resize(1048577U, ' ');
        rig.files->set("Library/Scripts/c01.js", std::string{C01_JS});
        rig.files->set(std::string{C01_MAP_FILE}, padded);
        rig.files->claimSize(std::string{C01_MAP_FILE}, static_cast<std::uint64_t>(C01_MAP.size()));
        REQUIRE(statusOf(rig.vm.loadScript(C01_PATH)) == "Ok");
        const ScriptInstanceResult made = rig.vm.createInstance(C01_PATH);
        CHECK(statusOf(made) == "Exception");
        CHECK(made.error.stack == C01_UNMAPPED_STACK);
        const std::vector<script_test::CapturedRecord> loud = log.atLeast(LogLevel::Warn);
        REQUIRE(loud.size() == 2U);
        CHECK(loud[0].level == LogLevel::Warn);
        CHECK(loud[0].message ==
              "script: res://Library/Scripts/c01.js.map: source map not used: its 1048577 bytes are over the "
              "1048576-byte limit (the VM's heap limit); stack frames name the JavaScript");
        CHECK(loud[1].message == std::string{C01_UNMAPPED_HEAD} + std::string{C01_UNMAPPED_STACK});
        CHECK(rig.files->reads(C01_MAP_FILE) == 1U);  // read whole, then refused on the bytes it held
        expectCleanShutdown(rig.vm);
    }
}

TEST_CASE("script: the map read sits at the read's tail, in a frame of its own (SR17)") {
    namespace fs = std::filesystem;
    const auto strippedSource = [](const fs::path& path) {
        std::ifstream file{path};
        REQUIRE(file.is_open());
        std::string out;
        std::string line;
        while (std::getline(file, line)) {
            const std::size_t comment = line.find("//");
            if (comment != std::string::npos) {
                line.erase(comment);
            }
            out += line;
            out += '\n';
        }
        return out;
    };
    const auto occurrences = [](std::string_view text, std::string_view token) {
        std::size_t count = 0;
        for (std::size_t at = text.find(token); at != std::string_view::npos; at = text.find(token, at + 1)) {
            ++count;
        }
        return count;
    };
    const auto lineAt = [](std::string_view text, std::size_t at) {
        const std::size_t begin = text.rfind('\n', at) == std::string_view::npos ? 0 : text.rfind('\n', at) + 1;
        const std::size_t end = text.find('\n', at);
        return std::string{text.substr(begin, end - begin)};
    };

    const fs::path src{AERO_SCRIPT_SRC_DIR};
    const std::string modules = strippedSource(src / "vm_modules.cpp");
    CHECK(occurrences(modules, "keepSourceMap(") == 2U);  // the definition and one call

    const auto bodyBetween = [&modules](std::string_view from, std::string_view to) {
        const std::size_t begin = modules.find(from);
        const std::size_t end = modules.find(to);
        REQUIRE(begin != std::string::npos);
        REQUIRE(end != std::string::npos);
        REQUIRE(begin < end);
        return std::string_view{modules}.substr(begin, end - begin);
    };
    // The read sits at readModuleSource's tail: after its last check and before the text is returned, for every text.
    // The cooked judgement is keepSourceMap's, so a cooked read clears its name's slot and the reader holds none.
    const std::string_view reader = bodyBetween("std::optional<std::string> Vm::Impl::readModuleSource(",
                                                "std::optional<std::string> Vm::Impl::peekModuleSource(");
    const std::size_t keep = reader.find("keepSourceMap(");
    const std::size_t lastCap = reader.rfind("sourceWithinCap(");
    const std::size_t returned = reader.rfind("return text;");
    REQUIRE(keep != std::string_view::npos);
    REQUIRE(lastCap != std::string_view::npos);
    REQUIRE(returned != std::string_view::npos);
    CHECK(lastCap < keep);
    CHECK(keep < returned);
    CHECK(reader.find("isCookedScriptText(") == std::string_view::npos);
    // The call is a bare statement.
    const std::string call = lineAt(reader, keep);
    CHECK(call.find('=') == std::string::npos);
    CHECK(call.find("keepSourceMap(key, *text);") != std::string::npos);

    // keepSourceMap judges a cooked text first: before any URL scan or VFS call.
    const std::string_view keeper =
        bodyBetween("void Vm::Impl::keepSourceMap(", "JSModuleDef* Vm::Impl::compileModule(");
    const std::size_t judged = keeper.find("isCookedScriptText(");
    REQUIRE(judged != std::string_view::npos);
    for (const std::string_view later : {"sourceMappingUrl(", "fileSize(", "readText("}) {
        const std::size_t at = keeper.find(later);
        REQUIRE(at != std::string_view::npos);
        CHECK(judged < at);
    }

    // compileModule's frame is live across the nested compile at every import level (R33): it gains no call, and the
    // read happens before its deadline check.
    const std::string_view compiler =
        bodyBetween("JSModuleDef* Vm::Impl::compileModule(", "JSModuleDef* Vm::Impl::compileCookedModule(");
    CHECK(compiler.find("keepSourceMap(") == std::string_view::npos);
    const std::size_t reads = compiler.find("readModuleSource(");
    const std::size_t deadline = compiler.find("deadlinePassed()");
    REQUIRE(reads != std::string_view::npos);
    REQUIRE(deadline != std::string_view::npos);
    CHECK(reads < deadline);

    const std::string impl = strippedSource(src / "vm_impl.hpp");
    const std::size_t declared = impl.find("keepSourceMap(");
    REQUIRE(declared != std::string::npos);
    CHECK(occurrences(impl, "keepSourceMap(") == 1U);
    CHECK(lineAt(impl, declared).find("AERO_SCRIPT_NOINLINE") != std::string::npos);
}
