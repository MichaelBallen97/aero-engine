// tests/material_format_test.cpp — task 3.4.1: the material asset v1 proof (docs/09-file-formats.md
// §11). Tier 0: no GPU, no reflect-gen, no files, no randomness. Covers the strict envelope, the
// version gate, the glTF default table, every REJECT arm, the WARN-and-ignore tolerance, the
// canonical form and both §1 round-trip guarantees.
//
// House rules this TU inherits. <ostream> is included PREVENTIVELY (the four-time MSVC trap: a CHECK
// comparing std::string_view instantiates MS STL's operator<< against an incomplete std::ostream and
// fails the Windows lane alone, inside the STL headers). The byte goldens are hoisted into named
// namespace-scope locals rather than passed as raw literals to a doctest macro. Every case-local
// table pins a LITERAL row count, never TABLE.size(), so deleting a row reddens instead of quietly
// testing less. The material enums deliberately have no toString (the ADL trap), so comparisons go
// through the material*Label functions -- whose injectivity MT30 proves, which is what makes a label
// comparison a faithful proxy for an enum comparison.
#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>
#include <aero/reflect/material_format.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

// The fully-defaulted material: what {"version": 1} means, spelled out. Every scalar is present even
// at its default (docs/09 §11.3 -- "a material file reads as its own documentation"); `name` and
// `textures` are the omitted ones. Byte-pinned INCLUDING the single trailing newline.
constexpr std::string_view MINIMAL_CANONICAL = R"({
  "version": 1,
  "baseColorFactor": [
    1,
    1,
    1,
    1
  ],
  "metallicFactor": 1,
  "roughnessFactor": 1,
  "emissiveFactor": [
    0,
    0,
    0
  ],
  "normalScale": 1,
  "occlusionStrength": 1,
  "alphaMode": "opaque",
  "alphaCutoff": 0.5,
  "doubleSided": false
}
)";

// Every key the format has, at a non-default value, with all five slots bound. baseColor deliberately
// carries wrapU != wrapV and minFilter != magFilter so a swapped parse target cannot survive; the
// emissive factor's 2.5 pins the "unbounded above" row; metallicRoughness pins uvSet != 0 and the
// "none" mip token.
constexpr std::string_view FULL_CANONICAL = R"({
  "version": 1,
  "name": "brushed steel",
  "baseColorFactor": [
    0.8,
    0.75,
    0.7,
    1
  ],
  "metallicFactor": 0.9,
  "roughnessFactor": 0.35,
  "emissiveFactor": [
    0,
    0,
    2.5
  ],
  "normalScale": 1.5,
  "occlusionStrength": 0.8,
  "alphaMode": "mask",
  "alphaCutoff": 0.25,
  "doubleSided": true,
  "textures": {
    "baseColor": {
      "guid": "0123456789abcdef0123456789abcdef",
      "uvSet": 0,
      "wrapU": "repeat",
      "wrapV": "clamp",
      "minFilter": "linear",
      "magFilter": "nearest",
      "mipFilter": "linear"
    },
    "metallicRoughness": {
      "guid": "fedcba9876543210fedcba9876543210",
      "uvSet": 1,
      "wrapU": "mirror",
      "wrapV": "mirror",
      "minFilter": "nearest",
      "magFilter": "nearest",
      "mipFilter": "none"
    },
    "normal": {
      "guid": "00000000000000000000000000000001",
      "uvSet": 0,
      "wrapU": "repeat",
      "wrapV": "repeat",
      "minFilter": "linear",
      "magFilter": "linear",
      "mipFilter": "linear"
    },
    "occlusion": {
      "guid": "11111111111111112222222222222222",
      "uvSet": 0,
      "wrapU": "repeat",
      "wrapV": "repeat",
      "minFilter": "linear",
      "magFilter": "linear",
      "mipFilter": "linear"
    },
    "emissive": {
      "guid": "aaaaaaaaaaaaaaaabbbbbbbbbbbbbbbb",
      "uvSet": 0,
      "wrapU": "repeat",
      "wrapV": "repeat",
      "minFilter": "linear",
      "magFilter": "linear",
      "mipFilter": "linear"
    }
  }
}
)";

engine::MaterialDocument parseOk(std::string_view text) {
    const engine::MaterialParseResult result = engine::parseMaterial(text);
    REQUIRE(result.ok());
    return *result.document;
}

engine::MaterialError parseFail(std::string_view text) {
    const engine::MaterialParseResult result = engine::parseMaterial(text);
    REQUIRE_FALSE(result.ok());
    CHECK_FALSE(result.document.has_value());  // nothing partial is ever returned
    return result.error;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// A minimal, version-1 material carrying one extra raw `"key": value` fragment.
std::string materialWith(std::string_view fragment) {
    return std::string(R"({"version": 1, )") + std::string(fragment) + "}";
}

// A minimal, version-1 material whose baseColor slot is the given raw JSON object literal.
std::string materialWithBaseColorSlot(std::string_view slotLiteral) {
    return std::string(R"({"version": 1, "textures": {"baseColor": )") + std::string(slotLiteral) + "}}";
}

// A baseColor slot with a valid guid plus one extra raw `"key": value` fragment.
std::string materialWithSlotField(std::string_view fragment) {
    return materialWithBaseColorSlot(std::string(R"({"guid": "0123456789abcdef0123456789abcdef", )") +
                                     std::string(fragment) + "}");
}

engine::Guid guidOf(std::string_view text) {
    const std::optional<engine::Guid> parsed = engine::parseGuid(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

// The exact document FULL_CANONICAL describes, hand-built -- so MT2 compares the parse against an
// independently-authored value rather than against the parser's own output.
engine::MaterialDocument expectedFullDocument() {
    engine::MaterialDocument doc;
    doc.name = "brushed steel";
    doc.baseColorFactor = engine::Vec4{.x = 0.8F, .y = 0.75F, .z = 0.7F, .w = 1.0F};
    doc.metallicFactor = 0.9F;
    doc.roughnessFactor = 0.35F;
    doc.emissiveFactor = engine::Vec3{.x = 0.0F, .y = 0.0F, .z = 2.5F};
    doc.normalScale = 1.5F;
    doc.occlusionStrength = 0.8F;
    doc.alphaMode = engine::MaterialAlphaMode::Mask;
    doc.alphaCutoff = 0.25F;
    doc.doubleSided = true;
    doc.baseColor = engine::MaterialTextureSlot{.guid = guidOf("0123456789abcdef0123456789abcdef"),
                                                .uvSet = 0,
                                                .wrapU = engine::MaterialWrap::Repeat,
                                                .wrapV = engine::MaterialWrap::Clamp,
                                                .minFilter = engine::MaterialFilter::Linear,
                                                .magFilter = engine::MaterialFilter::Nearest,
                                                .mipFilter = engine::MaterialMipFilter::Linear};
    doc.metallicRoughness = engine::MaterialTextureSlot{.guid = guidOf("fedcba9876543210fedcba9876543210"),
                                                        .uvSet = 1,
                                                        .wrapU = engine::MaterialWrap::Mirror,
                                                        .wrapV = engine::MaterialWrap::Mirror,
                                                        .minFilter = engine::MaterialFilter::Nearest,
                                                        .magFilter = engine::MaterialFilter::Nearest,
                                                        .mipFilter = engine::MaterialMipFilter::None};
    doc.normal = engine::MaterialTextureSlot{.guid = guidOf("00000000000000000000000000000001")};
    doc.occlusion = engine::MaterialTextureSlot{.guid = guidOf("11111111111111112222222222222222")};
    doc.emissive = engine::MaterialTextureSlot{.guid = guidOf("aaaaaaaaaaaaaaaabbbbbbbbbbbbbbbb")};
    return doc;
}

struct KeyedFragment {
    std::string_view path;      // the path the message must name
    std::string_view fragment;  // the raw `"key": value` JSON
};

}  // namespace

TEST_CASE("material: {\"version\": 1} is a legal, fully-defaulted material (MT1)") {
    static_assert(engine::MATERIAL_FORMAT_VERSION == 1);
    static_assert(engine::MATERIAL_MAX_UV_SETS == 4);
    static_assert(std::is_aggregate_v<engine::MaterialDocument>);
    static_assert(std::is_aggregate_v<engine::MaterialTextureSlot>);
    static_assert(std::is_aggregate_v<engine::MaterialError>);

    const engine::MaterialDocument parsed = parseOk(R"({"version": 1})");
    const engine::MaterialDocument defaults;
    CHECK((parsed == defaults));

    // The glTF 2.0 default table, restated literally so a changed default reddens HERE and not only
    // through the canonical golden.
    CHECK(parsed.name.empty());
    CHECK((parsed.baseColorFactor == engine::Vec4::one()));
    CHECK(parsed.metallicFactor == 1.0F);
    CHECK(parsed.roughnessFactor == 1.0F);
    CHECK((parsed.emissiveFactor == engine::Vec3{}));
    CHECK(parsed.normalScale == 1.0F);
    CHECK(parsed.occlusionStrength == 1.0F);
    CHECK(engine::materialAlphaModeLabel(parsed.alphaMode) == "opaque");
    CHECK(parsed.alphaCutoff == 0.5F);
    CHECK_FALSE(parsed.doubleSided);
    CHECK_FALSE(parsed.baseColor.has_value());
    CHECK_FALSE(parsed.metallicRoughness.has_value());
    CHECK_FALSE(parsed.normal.has_value());
    CHECK_FALSE(parsed.occlusion.has_value());
    CHECK_FALSE(parsed.emissive.has_value());
}

TEST_CASE("material: the full fixture parses field for field, all five slots (MT2)") {
    const engine::MaterialDocument parsed = parseOk(FULL_CANONICAL);
    CHECK((parsed == expectedFullDocument()));

    // Spelled out again for the fields a whole-struct compare would report as one opaque failure --
    // in particular the per-axis wrap and the per-direction filter, which exist here so a swapped
    // parse target cannot survive.
    REQUIRE(parsed.baseColor.has_value());
    CHECK(engine::materialWrapLabel(parsed.baseColor->wrapU) == "repeat");
    CHECK(engine::materialWrapLabel(parsed.baseColor->wrapV) == "clamp");
    CHECK(engine::materialFilterLabel(parsed.baseColor->minFilter) == "linear");
    CHECK(engine::materialFilterLabel(parsed.baseColor->magFilter) == "nearest");
    CHECK(engine::materialMipFilterLabel(parsed.baseColor->mipFilter) == "linear");
    REQUIRE(parsed.metallicRoughness.has_value());
    CHECK(parsed.metallicRoughness->uvSet == 1);
    CHECK(engine::materialMipFilterLabel(parsed.metallicRoughness->mipFilter) == "none");
    CHECK(parsed.emissiveFactor.z == 2.5F);  // the HDR-legal, above-1 row
}

TEST_CASE("material: a JSON-stage failure carries line/column/offset; a material-stage one does not (MT3)") {
    const engine::MaterialParseResult result = engine::parseMaterial("{\n  \"version\": 1,\n}");
    REQUIRE_FALSE(result.ok());
    CHECK(result.error.line > 0);
    CHECK(result.error.column > 0);
    CHECK(result.error.offset > 0);
    CHECK_FALSE(result.error.message.empty());

    const engine::MaterialError material = parseFail(R"({"version": 2})");
    CHECK(material.line == 0);
    CHECK(material.column == 0);
    CHECK(material.offset == 0);
}

TEST_CASE("material: a missing version REJECTs (MT4)") {
    CHECK(contains(parseFail(R"({"metallicFactor": 0.5})").message, R"(missing required key "version")"));
    CHECK(contains(parseFail("{}").message, R"(missing required key "version")"));
}

TEST_CASE("material: version 2 REJECTs, and reports the version even when the rest is garbage (MT5)") {
    CHECK(contains(parseFail(R"({"version": 2})").message, "unsupported material format version 2"));
    // Version is checked FIRST: this document is wrong in three other ways and still reports the
    // version, which is what a future-format file needs to hear.
    const std::string garbage = R"({"version": 7, "metallicFactor": "x", "alphaMode": 3, "textures": 9})";
    CHECK(contains(parseFail(garbage).message, "unsupported material format version 7"));
    CHECK(contains(parseFail(R"({"version": 0})").message, "unsupported material format version 0"));
}

TEST_CASE("material: a non-integral version REJECTs as a kind error, not a value error (MT6)") {
    constexpr std::array VERSION_LEXEMES{std::string_view("1.0"),  std::string_view("\"1\""), std::string_view("true"),
                                         std::string_view("null"), std::string_view("[1]"),   std::string_view("1e0")};
    CHECK(VERSION_LEXEMES.size() == 6);
    for (const std::string_view lexeme : VERSION_LEXEMES) {
        const std::string text = std::string(R"({"version": )") + std::string(lexeme) + "}";
        const engine::MaterialError error = parseFail(text);
        INFO("version lexeme: ", lexeme);
        CHECK(contains(error.message, R"("version" must be an integer)"));
    }
}

TEST_CASE("material: a root that is not an object REJECTs (MT7)") {
    constexpr std::array ROOTS{std::string_view("[]"), std::string_view("1"), std::string_view("\"x\""),
                               std::string_view("null"), std::string_view("true")};
    CHECK(ROOTS.size() == 5);
    for (const std::string_view root : ROOTS) {
        INFO("root: ", root);
        CHECK(contains(parseFail(root).message, "material root must be a JSON object"));
    }
}

TEST_CASE("material: every root key REJECTs the wrong kind, naming itself (MT8)") {
    constexpr std::array WRONG_KIND_ROWS{
        KeyedFragment{.path = "name", .fragment = R"("name": 5)"},
        KeyedFragment{.path = "baseColorFactor", .fragment = R"("baseColorFactor": 1)"},
        KeyedFragment{.path = "metallicFactor", .fragment = R"("metallicFactor": "x")"},
        KeyedFragment{.path = "roughnessFactor", .fragment = R"("roughnessFactor": true)"},
        KeyedFragment{.path = "emissiveFactor", .fragment = R"("emissiveFactor": {})"},
        KeyedFragment{.path = "normalScale", .fragment = R"("normalScale": [])"},
        KeyedFragment{.path = "occlusionStrength", .fragment = R"("occlusionStrength": null)"},
        KeyedFragment{.path = "alphaMode", .fragment = R"("alphaMode": 1)"},
        KeyedFragment{.path = "alphaCutoff", .fragment = R"("alphaCutoff": "0.5")"},
        KeyedFragment{.path = "doubleSided", .fragment = R"("doubleSided": 1)"},
        KeyedFragment{.path = "textures", .fragment = R"("textures": [])"},
    };
    CHECK(WRONG_KIND_ROWS.size() == 11);  // one per root key that has a kind rule; version is MT6's
    for (const KeyedFragment& row : WRONG_KIND_ROWS) {
        INFO("fragment: ", row.fragment);
        const engine::MaterialError error = parseFail(materialWith(row.fragment));
        CHECK(contains(error.message, row.path));
        CHECK(contains(error.message, "must be"));
    }
    // The two array-shaped factors also REJECT the wrong LENGTH, which is not a kind error.
    CHECK(contains(parseFail(materialWith(R"("baseColorFactor": [1, 1, 1])")).message,
                   R"("baseColorFactor" must be an array of 4 numbers (found 3))"));
    CHECK(contains(parseFail(materialWith(R"("emissiveFactor": [0, 0, 0, 0])")).message,
                   R"("emissiveFactor" must be an array of 3 numbers (found 4))"));
}

TEST_CASE("material: an unknown alphaMode token REJECTs rather than falling back (MT9)") {
    const engine::MaterialError error = parseFail(materialWith(R"("alphaMode": "shiny")"));
    CHECK(contains(error.message, R"("alphaMode": unknown token "shiny")"));
    CHECK(contains(error.message, R"("opaque", "mask", "blend")"));
    // Case matters: the token vocabulary is exact-match, never case-folded.
    CHECK(contains(parseFail(materialWith(R"("alphaMode": "OPAQUE")")).message, "unknown token"));
    CHECK(contains(parseFail(materialWith(R"("alphaMode": "")")).message, "unknown token"));
}

TEST_CASE("material: an unknown wrap token REJECTs, on either axis (MT10)") {
    CHECK(contains(parseFail(materialWithSlotField(R"("wrapU": "wrap")")).message,
                   R"("textures.baseColor.wrapU": unknown token "wrap")"));
    CHECK(contains(parseFail(materialWithSlotField(R"("wrapV": "border")")).message,
                   R"("textures.baseColor.wrapV": unknown token "border")"));
    CHECK(contains(parseFail(materialWithSlotField(R"("wrapU": "clampToEdge")")).message,
                   R"("repeat", "clamp", "mirror")"));
}

TEST_CASE("material: an unknown min/mag filter token REJECTs (MT11)") {
    CHECK(contains(parseFail(materialWithSlotField(R"("minFilter": "trilinear")")).message,
                   R"("textures.baseColor.minFilter": unknown token "trilinear")"));
    CHECK(contains(parseFail(materialWithSlotField(R"("magFilter": "none")")).message,
                   R"("textures.baseColor.magFilter": unknown token "none")"));
    CHECK(contains(parseFail(materialWithSlotField(R"("minFilter": 0)")).message,
                   R"("textures.baseColor.minFilter" must be a string)"));
}

TEST_CASE("material: an unknown mipFilter token REJECTs; \"none\" is legal here alone (MT12)") {
    CHECK(contains(parseFail(materialWithSlotField(R"("mipFilter": "point")")).message,
                   R"("textures.baseColor.mipFilter": unknown token "point")"));
    CHECK(contains(parseFail(materialWithSlotField(R"("mipFilter": "point")")).message,
                   R"("none", "nearest", "linear")"));
    const engine::MaterialDocument ok = parseOk(materialWithSlotField(R"("mipFilter": "none")"));
    REQUIRE(ok.baseColor.has_value());
    CHECK(engine::materialMipFilterLabel(ok.baseColor->mipFilter) == "none");
}

TEST_CASE("material: a present slot without a guid REJECTs (MT13)") {
    CHECK(contains(parseFail(materialWithBaseColorSlot("{}")).message,
                   R"("textures.baseColor": missing required key "guid")"));
    CHECK(contains(parseFail(materialWithBaseColorSlot(R"({"uvSet": 1})")).message, R"(missing required key "guid")"));
    // A slot that is not an object at all is a kind error, one level up.
    CHECK(contains(parseFail(materialWithBaseColorSlot("7")).message, R"("textures.baseColor" must be an object)"));
}

TEST_CASE("material: a malformed guid REJECTs; an uppercase one reads and re-emits lowercase (MT14)") {
    constexpr std::array MALFORMED{
        std::string_view("0123456789abcdef0123456789abcde"),       // 31 digits
        std::string_view("0123456789abcdef0123456789abcdef0"),     // 33 digits
        std::string_view("01234567-89ab-cdef-0123-456789abcdef"),  // dashed
        std::string_view("{0123456789abcdef0123456789abcdef}"),    // braced
        std::string_view("0x123456789abcdef0123456789abcdef"),     // 0x prefix
        std::string_view("0123456789abcdefg123456789abcdef"),      // non-hex digit
        std::string_view(""),                                      // empty
    };
    CHECK(MALFORMED.size() == 7);
    for (const std::string_view text : MALFORMED) {
        INFO("guid: ", text);
        const std::string doc = materialWithBaseColorSlot(std::string(R"({"guid": ")") + std::string(text) + R"("})");
        CHECK(contains(parseFail(doc).message, R"("textures.baseColor.guid" must be exactly 32 hex digits)"));
    }
    // A non-string guid is a kind error, not a codec error.
    CHECK(contains(parseFail(materialWithBaseColorSlot(R"({"guid": 1})")).message,
                   R"("textures.baseColor.guid" must be a string)"));

    // TOLERANT READ / CANONICAL WRITE (docs/09 §5, shared by every GUID-bearing format here):
    // parseGuid accepts either case, and the canonical writer always emits lowercase. This is NOT a
    // laxity this format invented -- rejecting uppercase would make .aeromat the only format in the
    // document that does.
    const engine::MaterialDocument upper =
        parseOk(materialWithBaseColorSlot(R"({"guid": "0123456789ABCDEF0123456789ABCDEF"})"));
    REQUIRE(upper.baseColor.has_value());
    CHECK((upper.baseColor->guid == guidOf("0123456789abcdef0123456789abcdef")));
    CHECK(contains(engine::writeMaterialText(upper), R"("guid": "0123456789abcdef0123456789abcdef")"));
    CHECK_FALSE(contains(engine::writeMaterialText(upper), "ABCDEF"));
}

TEST_CASE("material: a nil guid in a present slot REJECTs -- absence is spelled by omission (MT15)") {
    const std::string doc = materialWithBaseColorSlot(R"({"guid": "00000000000000000000000000000000"})");
    const engine::MaterialError error = parseFail(doc);
    CHECK(contains(error.message, R"("textures.baseColor.guid" must not be nil)"));
    CHECK(contains(error.message, R"(omitting "textures.baseColor")"));
}

TEST_CASE("material: uvSet is capped below MATERIAL_MAX_UV_SETS (MT16)") {
    constexpr std::array BAD_UV_SETS{std::string_view("4"),   std::string_view("9"),     std::string_view("-1"),
                                     std::string_view("1.5"), std::string_view("\"0\""), std::string_view("true"),
                                     std::string_view("null")};
    CHECK(BAD_UV_SETS.size() == 7);
    for (const std::string_view lexeme : BAD_UV_SETS) {
        INFO("uvSet: ", lexeme);
        const std::string doc = materialWithSlotField(std::string(R"("uvSet": )") + std::string(lexeme));
        CHECK(contains(parseFail(doc).message, R"("textures.baseColor.uvSet" must be an integer in [0, 3])"));
    }
    // The three legal non-zero values parse and survive the round trip.
    for (std::uint32_t value = 0; value < engine::MATERIAL_MAX_UV_SETS; ++value) {
        const std::string doc = materialWithSlotField(std::format(R"("uvSet": {})", value));
        const engine::MaterialDocument parsed = parseOk(doc);
        REQUIRE(parsed.baseColor.has_value());
        CHECK(parsed.baseColor->uvSet == value);
    }
}

TEST_CASE("material: baseColorFactor components REJECT outside [0, 1], on either side (MT17)") {
    constexpr std::array ROWS{std::string_view("[-0.1, 1, 1, 1]"), std::string_view("[1, 1.1, 1, 1]"),
                              std::string_view("[1, 1, -1, 1]"), std::string_view("[1, 1, 1, 2]")};
    CHECK(ROWS.size() == 4);  // one per component, so a dropped component reddens
    for (std::size_t i = 0; i < ROWS.size(); ++i) {
        INFO("baseColorFactor: ", ROWS[i]);
        const engine::MaterialError error =
            parseFail(materialWith(std::string(R"("baseColorFactor": )") + std::string(ROWS[i])));
        CHECK(contains(error.message, std::format("baseColorFactor[{}]", i)));
        CHECK(contains(error.message, "must be in [0, 1]"));
    }
    CHECK(parseOk(materialWith(R"("baseColorFactor": [0, 0, 0, 0])")).baseColorFactor.x == 0.0F);
    CHECK(parseOk(materialWith(R"("baseColorFactor": [1, 1, 1, 1])")).baseColorFactor.w == 1.0F);
}

TEST_CASE("material: metallicFactor REJECTs outside [0, 1], on either side (MT18)") {
    CHECK(contains(parseFail(materialWith(R"("metallicFactor": -0.1)")).message,
                   R"("metallicFactor" must be in [0, 1] (found -0.1))"));
    CHECK(
        contains(parseFail(materialWith(R"("metallicFactor": 1.1)")).message, R"("metallicFactor" must be in [0, 1])"));
    CHECK(parseOk(materialWith(R"("metallicFactor": 0)")).metallicFactor == 0.0F);
    CHECK(parseOk(materialWith(R"("metallicFactor": 1)")).metallicFactor == 1.0F);
}

TEST_CASE("material: roughnessFactor REJECTs outside [0, 1], on either side (MT19)") {
    CHECK(contains(parseFail(materialWith(R"("roughnessFactor": -0.5)")).message,
                   R"("roughnessFactor" must be in [0, 1])"));
    CHECK(contains(parseFail(materialWith(R"("roughnessFactor": 100)")).message,
                   R"("roughnessFactor" must be in [0, 1])"));
    CHECK(parseOk(materialWith(R"("roughnessFactor": 0)")).roughnessFactor == 0.0F);
}

TEST_CASE("material: occlusionStrength REJECTs outside [0, 1], on either side (MT20)") {
    CHECK(contains(parseFail(materialWith(R"("occlusionStrength": -0.001)")).message,
                   R"("occlusionStrength" must be in [0, 1])"));
    CHECK(contains(parseFail(materialWith(R"("occlusionStrength": 1.5)")).message,
                   R"("occlusionStrength" must be in [0, 1])"));
    CHECK(parseOk(materialWith(R"("occlusionStrength": 0.25)")).occlusionStrength == 0.25F);
}

TEST_CASE("material: alphaCutoff REJECTs outside [0, 1], on either side (MT21)") {
    CHECK(contains(parseFail(materialWith(R"("alphaCutoff": -1)")).message, R"("alphaCutoff" must be in [0, 1])"));
    CHECK(contains(parseFail(materialWith(R"("alphaCutoff": 1.0001)")).message, R"("alphaCutoff" must be in [0, 1])"));
    // Stored regardless of alphaMode: the file is the record, the consumer decides when it matters.
    CHECK(parseOk(materialWith(R"("alphaCutoff": 0)")).alphaCutoff == 0.0F);
    CHECK(parseOk(materialWith(R"("alphaCutoff": 1)")).alphaCutoff == 1.0F);
}

TEST_CASE("material: the two >= 0 rows are unbounded ABOVE and REJECT below zero (MT22)") {
    CHECK(contains(parseFail(materialWith(R"("normalScale": -1)")).message, R"("normalScale" must be >= 0)"));
    CHECK(contains(parseFail(materialWith(R"("emissiveFactor": [-1, 0, 0])")).message,
                   R"("emissiveFactor[0]" must be >= 0)"));
    CHECK(contains(parseFail(materialWith(R"("emissiveFactor": [0, 0, -0.001])")).message,
                   R"("emissiveFactor[2]" must be >= 0)"));
    // Above 1 is LEGAL for both -- the HDR row, matching the lights' unclamped precedent. A [0,1]
    // clamp sneaked onto either of these reddens here.
    CHECK(parseOk(materialWith(R"("normalScale": 8)")).normalScale == 8.0F);
    CHECK(parseOk(materialWith(R"("emissiveFactor": [12, 0, 3.5])")).emissiveFactor.x == 12.0F);
}

TEST_CASE("material: null where a number belongs REJECTs, and so does an unrepresentable one (MT23)") {
    // Deliberately stricter than the scene reader's null -> NaN payload tolerance (docs/09 §11.1).
    constexpr std::array NULL_ROWS{
        std::string_view(R"("metallicFactor": null)"), std::string_view(R"("alphaCutoff": null)"),
        std::string_view(R"("normalScale": null)"), std::string_view(R"("baseColorFactor": [null, 1, 1, 1])"),
        std::string_view(R"("emissiveFactor": [0, null, 0])")};
    CHECK(NULL_ROWS.size() == 5);
    for (const std::string_view fragment : NULL_ROWS) {
        INFO("fragment: ", fragment);
        CHECK(contains(parseFail(materialWith(fragment)).message, "must be a number (found null)"));
    }
    // docs/09 §2.4's `1e999` corner: the JSON layer keeps the lexeme VERBATIM, so the overflow
    // surfaces at the typed read here rather than being pre-collapsed to null.
    CHECK(contains(parseFail(materialWith(R"("metallicFactor": 1e999)")).message,
                   R"("metallicFactor" is not representable as a 32-bit float (found "1e999"))"));
    CHECK(contains(parseFail(materialWith(R"("emissiveFactor": [1e999, 0, 0])")).message,
                   R"("emissiveFactor[0]" is not representable as a 32-bit float)"));
}

TEST_CASE("material: unknown keys at all three levels are tolerated and stripped on save (MT24)") {
    const std::string text = R"({
      "version": 1,
      "futureRootKey": {"anything": [1, 2, 3]},
      "metallicFactor": 0.25,
      "textures": {
        "futureSlotName": {"guid": "0123456789abcdef0123456789abcdef"},
        "baseColor": {"guid": "0123456789abcdef0123456789abcdef", "futureSlotKey": 7}
      }
    })";
    const engine::MaterialDocument parsed = parseOk(text);
    CHECK(parsed.metallicFactor == 0.25F);
    REQUIRE(parsed.baseColor.has_value());
    CHECK((parsed.baseColor->guid == guidOf("0123456789abcdef0123456789abcdef")));

    const std::string written = engine::writeMaterialText(parsed);
    CHECK_FALSE(contains(written, "futureRootKey"));
    CHECK_FALSE(contains(written, "futureSlotName"));
    CHECK_FALSE(contains(written, "futureSlotKey"));
    CHECK(contains(written, R"("metallicFactor": 0.25)"));
}

TEST_CASE("material: a REJECTed document is unaffected by the unknown keys it also carries (MT25)") {
    // The WARN sweep runs ONLY on the success path -- parseMaterial calls warnUnknownMaterialKeys
    // after extract() has returned no error, so a rejected document emits exactly one error and ZERO
    // warnings by construction. No log sink exists in this tree to observe a warning directly (the
    // 0.2.4 deferral), so the observable half asserted here is that unknown keys change NOTHING about
    // the failure: same rejection, same message, with and without them.
    const std::string clean = R"({"version": 1, "metallicFactor": 5})";
    const std::string noisy =
        R"({"version": 1, "junkA": 1, "metallicFactor": 5, "textures": {"junkB": 2}, "junkC": [null]})";
    const engine::MaterialError cleanError = parseFail(clean);
    const engine::MaterialError noisyError = parseFail(noisy);
    CHECK(cleanError.message == noisyError.message);
    CHECK(contains(cleanError.message, R"("metallicFactor" must be in [0, 1])"));
}

TEST_CASE("material: the canonical golden is byte-exact -- key order and always-emitted scalars (MT26)") {
    CHECK(engine::writeMaterialText(engine::MaterialDocument{}) == MINIMAL_CANONICAL);
    CHECK(engine::writeMaterialText(expectedFullDocument()) == FULL_CANONICAL);
    // Key ORDER, asserted independently of the golden: each root key appears after the previous one.
    constexpr std::array ROOT_ORDER{
        std::string_view(R"("version")"),         std::string_view(R"("name")"),
        std::string_view(R"("baseColorFactor")"), std::string_view(R"("metallicFactor")"),
        std::string_view(R"("roughnessFactor")"), std::string_view(R"("emissiveFactor")"),
        std::string_view(R"("normalScale")"),     std::string_view(R"("occlusionStrength")"),
        std::string_view(R"("alphaMode")"),       std::string_view(R"("alphaCutoff")"),
        std::string_view(R"("doubleSided")"),     std::string_view(R"("textures")")};
    CHECK(ROOT_ORDER.size() == 12);
    const std::string full = engine::writeMaterialText(expectedFullDocument());
    std::size_t cursor = 0;
    for (const std::string_view key : ROOT_ORDER) {
        INFO("root key: ", key);
        const std::size_t at = full.find(key, cursor);
        REQUIRE(at != std::string::npos);
        cursor = at;
    }
    constexpr std::array SLOT_ORDER{std::string_view(R"("baseColor")"), std::string_view(R"("metallicRoughness")"),
                                    std::string_view(R"("normal")"), std::string_view(R"("occlusion")"),
                                    std::string_view(R"("emissive")")};
    CHECK(SLOT_ORDER.size() == 5);
    cursor = full.find(R"("textures")");
    REQUIRE(cursor != std::string::npos);
    for (const std::string_view slot : SLOT_ORDER) {
        INFO("slot: ", slot);
        const std::size_t at = full.find(slot, cursor);
        REQUIRE(at != std::string::npos);
        cursor = at;
    }
}

TEST_CASE("material: the four omission rules -- name, each slot, textures, and never a slot sub-key (MT27)") {
    engine::MaterialDocument doc;
    CHECK_FALSE(contains(engine::writeMaterialText(doc), R"("name")"));
    CHECK_FALSE(contains(engine::writeMaterialText(doc), R"("textures")"));

    doc.name = "steel";
    CHECK(contains(engine::writeMaterialText(doc), R"("name": "steel")"));
    doc.name.clear();
    CHECK_FALSE(contains(engine::writeMaterialText(doc), R"("name")"));

    doc.normal = engine::MaterialTextureSlot{.guid = guidOf("00000000000000000000000000000001")};
    const std::string oneSlot = engine::writeMaterialText(doc);
    CHECK(contains(oneSlot, R"("textures")"));
    CHECK(contains(oneSlot, R"("normal")"));
    CHECK_FALSE(contains(oneSlot, R"("baseColor")"));
    CHECK_FALSE(contains(oneSlot, R"("metallicRoughness")"));
    CHECK_FALSE(contains(oneSlot, R"("occlusion")"));
    CHECK_FALSE(contains(oneSlot, R"("emissive")"));

    // A BOUND slot spells its sampler state in full, even when every value is the default.
    constexpr std::array SLOT_KEYS{std::string_view(R"("guid")"),      std::string_view(R"("uvSet")"),
                                   std::string_view(R"("wrapU")"),     std::string_view(R"("wrapV")"),
                                   std::string_view(R"("minFilter")"), std::string_view(R"("magFilter")"),
                                   std::string_view(R"("mipFilter")")};
    CHECK(SLOT_KEYS.size() == 7);
    for (const std::string_view key : SLOT_KEYS) {
        INFO("slot key: ", key);
        CHECK(contains(oneSlot, key));
    }
}

TEST_CASE("material: guarantee 1 -- canonical text is a byte-stable fixpoint (MT28)") {
    CHECK(engine::writeMaterialText(parseOk(MINIMAL_CANONICAL)) == MINIMAL_CANONICAL);
    CHECK(engine::writeMaterialText(parseOk(FULL_CANONICAL)) == FULL_CANONICAL);
    // And once more through a second full cycle, so a fixpoint that only holds on the first pass
    // cannot pass.
    CHECK(engine::writeMaterialText(parseOk(engine::writeMaterialText(parseOk(FULL_CANONICAL)))) == FULL_CANONICAL);
}

TEST_CASE("material: guarantee 2 -- write o parse o write is idempotent for any parsed input (MT29)") {
    constexpr std::array NON_CANONICAL{
        std::string_view(R"({"version":1})"),                                          // compact
        std::string_view(R"({"metallicFactor":0.5,"version":1,"name":"reordered"})"),  // key order
        std::string_view(R"({  "version"  :  1  ,  "doubleSided"  :  true  })"),       // whitespace
        std::string_view(R"({"version": 1, "metallicFactor": 1.0})"),                  // 1.0 -> 1
        std::string_view(R"({"version": 1, "unknown": [1, {"deep": null}]})"),         // unknown keys
        std::string_view(R"({"version": 1, "textures": {}})"),                         // empty textures
        std::string_view(R"({"version": 1, "textures": {"emissive": {"guid": "0123456789ABCDEF0123456789ABCDEF"}}})"),
        std::string_view(R"({"version": 1, "emissiveFactor": [1e1, 0, 0]})"),  // 1e1 -> 10
    };
    CHECK(NON_CANONICAL.size() == 8);
    for (const std::string_view input : NON_CANONICAL) {
        INFO("input: ", input);
        const std::string first = engine::writeMaterialText(parseOk(input));
        const std::string second = engine::writeMaterialText(parseOk(first));
        CHECK(first == second);
        CHECK(first.back() == '\n');
        CHECK(first.find("\n\n") == std::string::npos);  // exactly ONE trailing newline
    }
}

TEST_CASE("material: the four label functions are total and injective (MT30)") {
    constexpr std::array ALPHA_MODES{engine::MaterialAlphaMode::Opaque, engine::MaterialAlphaMode::Mask,
                                     engine::MaterialAlphaMode::Blend};
    constexpr std::array WRAPS{engine::MaterialWrap::Repeat, engine::MaterialWrap::Clamp, engine::MaterialWrap::Mirror};
    constexpr std::array FILTERS{engine::MaterialFilter::Nearest, engine::MaterialFilter::Linear};
    constexpr std::array MIP_FILTERS{engine::MaterialMipFilter::None, engine::MaterialMipFilter::Nearest,
                                     engine::MaterialMipFilter::Linear};
    CHECK(ALPHA_MODES.size() == 3);
    CHECK(WRAPS.size() == 3);
    CHECK(FILTERS.size() == 2);
    CHECK(MIP_FILTERS.size() == 3);

    // The exact spellings, pinned: they are the FILE's vocabulary, so an edit here is a format change.
    CHECK(engine::materialAlphaModeLabel(engine::MaterialAlphaMode::Opaque) == "opaque");
    CHECK(engine::materialAlphaModeLabel(engine::MaterialAlphaMode::Mask) == "mask");
    CHECK(engine::materialAlphaModeLabel(engine::MaterialAlphaMode::Blend) == "blend");
    CHECK(engine::materialWrapLabel(engine::MaterialWrap::Repeat) == "repeat");
    CHECK(engine::materialWrapLabel(engine::MaterialWrap::Clamp) == "clamp");
    CHECK(engine::materialWrapLabel(engine::MaterialWrap::Mirror) == "mirror");
    CHECK(engine::materialFilterLabel(engine::MaterialFilter::Nearest) == "nearest");
    CHECK(engine::materialFilterLabel(engine::MaterialFilter::Linear) == "linear");
    CHECK(engine::materialMipFilterLabel(engine::MaterialMipFilter::None) == "none");
    CHECK(engine::materialMipFilterLabel(engine::MaterialMipFilter::Nearest) == "nearest");
    CHECK(engine::materialMipFilterLabel(engine::MaterialMipFilter::Linear) == "linear");

    // Injectivity per enum (what makes a label comparison a faithful proxy for an enum comparison).
    for (std::size_t i = 0; i < ALPHA_MODES.size(); ++i) {
        CHECK_FALSE(engine::materialAlphaModeLabel(ALPHA_MODES[i]).empty());
        for (std::size_t j = i + 1; j < ALPHA_MODES.size(); ++j) {
            CHECK(engine::materialAlphaModeLabel(ALPHA_MODES[i]) != engine::materialAlphaModeLabel(ALPHA_MODES[j]));
        }
    }
    for (std::size_t i = 0; i < WRAPS.size(); ++i) {
        for (std::size_t j = i + 1; j < WRAPS.size(); ++j) {
            CHECK(engine::materialWrapLabel(WRAPS[i]) != engine::materialWrapLabel(WRAPS[j]));
        }
    }
    CHECK(engine::materialFilterLabel(FILTERS[0]) != engine::materialFilterLabel(FILTERS[1]));
    for (std::size_t i = 0; i < MIP_FILTERS.size(); ++i) {
        for (std::size_t j = i + 1; j < MIP_FILTERS.size(); ++j) {
            CHECK(engine::materialMipFilterLabel(MIP_FILTERS[i]) != engine::materialMipFilterLabel(MIP_FILTERS[j]));
        }
    }

    // Totality end to end: every enumerator's label is a token the parser accepts back.
    for (const engine::MaterialAlphaMode mode : ALPHA_MODES) {
        const std::string text = std::format(R"("alphaMode": "{}")", engine::materialAlphaModeLabel(mode));
        CHECK(engine::materialAlphaModeLabel(parseOk(materialWith(text)).alphaMode) ==
              engine::materialAlphaModeLabel(mode));
    }
    for (const engine::MaterialWrap wrap : WRAPS) {
        const std::string text = std::format(R"("wrapU": "{}")", engine::materialWrapLabel(wrap));
        const engine::MaterialDocument parsed = parseOk(materialWithSlotField(text));
        REQUIRE(parsed.baseColor.has_value());
        CHECK(engine::materialWrapLabel(parsed.baseColor->wrapU) == engine::materialWrapLabel(wrap));
    }
    for (const engine::MaterialFilter filter : FILTERS) {
        const std::string text = std::format(R"("magFilter": "{}")", engine::materialFilterLabel(filter));
        const engine::MaterialDocument parsed = parseOk(materialWithSlotField(text));
        REQUIRE(parsed.baseColor.has_value());
        CHECK(engine::materialFilterLabel(parsed.baseColor->magFilter) == engine::materialFilterLabel(filter));
    }
    for (const engine::MaterialMipFilter filter : MIP_FILTERS) {
        const std::string text = std::format(R"("mipFilter": "{}")", engine::materialMipFilterLabel(filter));
        const engine::MaterialDocument parsed = parseOk(materialWithSlotField(text));
        REQUIRE(parsed.baseColor.has_value());
        CHECK(engine::materialMipFilterLabel(parsed.baseColor->mipFilter) == engine::materialMipFilterLabel(filter));
    }
}

TEST_CASE("material: equality is per-field over every slot member -- nothing is lossy (MT31)") {
    const engine::MaterialDocument base = expectedFullDocument();
    CHECK((base == expectedFullDocument()));
    CHECK((parseOk(FULL_CANONICAL) == base));

    // One mutation per slot member: each must break equality, so a field the round trip drops cannot
    // hide behind a defaulted comparison.
    {
        engine::MaterialDocument other = base;
        other.baseColor->guid = guidOf("fedcba9876543210fedcba9876543210");
        CHECK((other != base));
    }
    {
        engine::MaterialDocument other = base;
        other.baseColor->uvSet = 2;
        CHECK((other != base));
    }
    {
        engine::MaterialDocument other = base;
        other.baseColor->wrapU = engine::MaterialWrap::Mirror;
        CHECK((other != base));
    }
    {
        engine::MaterialDocument other = base;
        other.baseColor->wrapV = engine::MaterialWrap::Repeat;
        CHECK((other != base));
    }
    {
        engine::MaterialDocument other = base;
        other.baseColor->minFilter = engine::MaterialFilter::Nearest;
        CHECK((other != base));
    }
    {
        engine::MaterialDocument other = base;
        other.baseColor->magFilter = engine::MaterialFilter::Linear;
        CHECK((other != base));
    }
    {
        engine::MaterialDocument other = base;
        other.baseColor->mipFilter = engine::MaterialMipFilter::None;
        CHECK((other != base));
    }
    {
        engine::MaterialDocument other = base;
        other.occlusion.reset();
        CHECK((other != base));
    }
    {
        engine::MaterialDocument other = base;
        other.emissiveFactor.z = 2.4F;
        CHECK((other != base));
    }
}

TEST_CASE("material: validateMaterial reaches the arms text cannot spell (MT32)") {
    CHECK_FALSE(engine::validateMaterial(engine::MaterialDocument{}).has_value());
    CHECK_FALSE(engine::validateMaterial(expectedFullDocument()).has_value());

    constexpr float NOT_A_NUMBER = std::numeric_limits<float>::quiet_NaN();
    constexpr float INFINITE = std::numeric_limits<float>::infinity();

    {  // NaN is unspellable in JSON but trivially assignable in C++; the range test is written to
       // REJECT it (every comparison against NaN is false).
        engine::MaterialDocument doc;
        doc.metallicFactor = NOT_A_NUMBER;
        const std::optional<engine::MaterialError> error = engine::validateMaterial(doc);
        REQUIRE(error.has_value());
        CHECK(contains(error->message, "metallicFactor"));
    }
    {
        engine::MaterialDocument doc;
        doc.baseColorFactor.y = NOT_A_NUMBER;
        const std::optional<engine::MaterialError> error = engine::validateMaterial(doc);
        REQUIRE(error.has_value());
        CHECK(contains(error->message, "baseColorFactor[1]"));
    }
    {  // "unbounded above" means every FINITE float, not an infinity.
        engine::MaterialDocument doc;
        doc.emissiveFactor.x = INFINITE;
        const std::optional<engine::MaterialError> error = engine::validateMaterial(doc);
        REQUIRE(error.has_value());
        CHECK(contains(error->message, "emissiveFactor[0]"));
    }
    {
        engine::MaterialDocument doc;
        doc.normalScale = -1.0F;
        REQUIRE(engine::validateMaterial(doc).has_value());
        doc.normalScale = 0.0F;
        doc.occlusionStrength = 1.5F;
        REQUIRE(engine::validateMaterial(doc).has_value());
        doc.occlusionStrength = 1.0F;
        doc.alphaCutoff = 2.0F;
        REQUIRE(engine::validateMaterial(doc).has_value());
        doc.alphaCutoff = 0.5F;
        doc.roughnessFactor = -0.0001F;
        REQUIRE(engine::validateMaterial(doc).has_value());
    }
    {  // A present slot must carry a real guid and an in-range uvSet, exactly as parse requires.
        engine::MaterialDocument doc;
        doc.normal = engine::MaterialTextureSlot{};  // nil guid
        const std::optional<engine::MaterialError> nilError = engine::validateMaterial(doc);
        REQUIRE(nilError.has_value());
        CHECK(contains(nilError->message, "textures.normal.guid"));

        doc.normal->guid = guidOf("00000000000000000000000000000001");
        CHECK_FALSE(engine::validateMaterial(doc).has_value());
        doc.normal->uvSet = 4;
        const std::optional<engine::MaterialError> uvError = engine::validateMaterial(doc);
        REQUIRE(uvError.has_value());
        CHECK(contains(uvError->message, "textures.normal.uvSet"));
    }
    {  // Why validateMaterial exists at all: a non-finite factor writes as `null`, which the parser
       // then REJECTs -- so a caller that skips validation can produce a file it cannot read back.
        engine::MaterialDocument doc;
        doc.metallicFactor = NOT_A_NUMBER;
        const std::string written = engine::writeMaterialText(doc);
        CHECK(contains(written, R"("metallicFactor": null)"));
        CHECK(contains(parseFail(written).message, "must be a number (found null)"));
    }
}

TEST_CASE("material: duplicate JSON keys collapse last-wins at the JSON layer (MT33)") {
    // Inherited parser tolerance (docs/09 §2.3's scene precedent), documented rather than fought: the
    // JSON layer overwrites in place, so the material layer never sees two members with one key.
    const engine::MaterialDocument parsed = parseOk(R"({"version": 1, "metallicFactor": 0.1, "metallicFactor": 0.9})");
    CHECK(parsed.metallicFactor == 0.9F);
    // Including for the version gate itself: the LAST version is the one that governs.
    CHECK(contains(parseFail(R"({"version": 1, "version": 3})").message, "unsupported material format version 3"));
    CHECK(parseOk(R"({"version": 3, "version": 1})").metallicFactor == 1.0F);
}

TEST_CASE("material: an empty textures object is legal and is omitted on rewrite (MT34)") {
    const engine::MaterialDocument parsed = parseOk(R"({"version": 1, "textures": {}})");
    CHECK((parsed == engine::MaterialDocument{}));
    CHECK(engine::writeMaterialText(parsed) == MINIMAL_CANONICAL);
    // The DOM overload agrees with the text overload -- it is the primitive, and the text one is a
    // thin wrapper over it.
    const engine::JsonParseResult json = engine::parseJson(R"({"version": 1, "textures": {}})");
    REQUIRE(json.value.has_value());
    const engine::MaterialParseResult fromDom = engine::parseMaterial(*json.value);
    REQUIRE(fromDom.ok());
    CHECK((*fromDom.document == parsed));
}
