// tests/editor/asset_meta_test.cpp -- task 3.1.1: the .meta v1 format (naming, classification,
// parse, write) and planAssetMetas, the pure asset-identity lifecycle planner. A TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, and that is the point (D4/AC-17/INV-P5, the project_test.cpp precedent): asset_meta.hpp
// depends on nothing but <aero/core/guid.hpp>, so every case in this file must be PRESENT and
// PASSING in all three build configurations -- prove it with --list-test-cases, never with a skip.
// Tier-0: no GPU, no window, no ImGui context, no disk I/O at all -- planAssetMetas and parseMeta
// touch no filesystem; only the two golden-fixture reads below touch disk, through
// scene_golden::readBytes.
#include <aero/core/guid.hpp>
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/import_settings.hpp>
#include <aero/editor/project_files.hpp>  // task E.4.4: isHiddenName (IX15, IX18, AM28)
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <ostream>  // MSVC alone needs the complete type to stringify a string_view inside a CHECK
#include <string>
#include <string_view>
#include <vector>

using engine::formatGuid;
using engine::Guid;
using engine::GuidGenerator;
using engine::JsonMember;
using engine::JsonParseResult;
using engine::JsonValue;
using engine::parseGuid;
using engine::parseJson;
using engine::editor::AssetMetaState;
using engine::editor::assetNameForMeta;
using engine::editor::AssetPlanEntry;
using engine::editor::AssetPlanResult;
using engine::editor::AssetRecord;
using engine::editor::ATOMIC_TEMP_SUFFIX;
using engine::editor::BLEND_BACKUP_STEM;
using engine::editor::IGNORED_ASSET_NAME_SUFFIXES;
using engine::editor::IGNORED_ASSET_NAMES;
using engine::editor::ImportSettings;
using engine::editor::isHiddenName;
using engine::editor::isIgnoredAssetName;
using engine::editor::isMetaFileName;
using engine::editor::isScannableAssetName;
using engine::editor::isWatchableAssetName;
using engine::editor::MetaError;
using engine::editor::metaFileNameFor;
using engine::editor::MetaParseResult;
using engine::editor::parseMeta;
using engine::editor::planAssetMetas;
using engine::editor::writeMetaText;

namespace {

constexpr std::string_view MINIMAL_FIXTURE = AERO_ASSET_FIXTURES_DIR "/minimal.meta";
constexpr std::string_view UNKNOWN_KEYS_FIXTURE = AERO_ASSET_FIXTURES_DIR "/unknown-keys.meta";
constexpr std::string_view IMPORTER_SETTINGS_FIXTURE = AERO_ASSET_FIXTURES_DIR "/importer-settings.meta";
// task 3.2.2 (D7/AC-17): the FBX counterpart to IMPORTER_SETTINGS_FIXTURE above -- same shape, "fbx" in
// place of "gltf", proving the second registered name did not disturb the first.
constexpr std::string_view IMPORTER_FBX_FIXTURE = AERO_ASSET_FIXTURES_DIR "/importer-fbx.meta";

// The fixture's pinned GUID, as text -- shared by AG3/AG4 so there is exactly one spelling of it.
constexpr std::string_view FIXTURE_GUID_TEXT = "a3f1c07e5b8d42198e6f0c3d7a2b4b92";
// importer-fbx.meta's own pinned GUID, DISTINCT from FIXTURE_GUID_TEXT so the two fixtures are never
// confused for one another by a case that reads both.
constexpr std::string_view FBX_FIXTURE_GUID_TEXT = "b4e2d18f6c9e53209f7a1d4e8b3c5ca3";

}  // namespace

// ---- naming ---------------------------------------------------------------------------------------

TEST_CASE("asset_meta: metaFileNameFor appends to the FULL name (AM1, AC-18)") {
    CHECK(metaFileNameFor("wood.png") == "wood.png.meta");
    CHECK(metaFileNameFor("wood.jpg") == "wood.jpg.meta");
    // Distinct sidecar names -- wood.png and wood.jpg never collide (AC-18).
    CHECK(metaFileNameFor("wood.png") != metaFileNameFor("wood.jpg"));
    CHECK(metaFileNameFor("noext") == "noext.meta");
}

TEST_CASE("asset_meta: isMetaFileName is case-insensitive on the suffix only (AM2, AC-19)") {
    CHECK(isMetaFileName("x.meta"));
    CHECK(isMetaFileName("x.META"));
    CHECK(isMetaFileName("x.MeTa"));
    CHECK(isMetaFileName("wood.png.meta"));
    CHECK_FALSE(isMetaFileName(".meta"));  // 5 bytes, not > 5 -- not a sidecar of anything
    CHECK_FALSE(isMetaFileName("meta"));   // no dot
    CHECK_FALSE(isMetaFileName("x.metal"));
    CHECK_FALSE(isMetaFileName(""));
}

TEST_CASE("asset_meta: assetNameForMeta round-trips (AM3, AC-20)") {
    CHECK(assetNameForMeta("wood.png.meta") == "wood.png");
    CHECK(assetNameForMeta(metaFileNameFor("scene.gltf")) == "scene.gltf");
    CHECK(assetNameForMeta("wood.png").empty());  // not a sidecar name at all
    CHECK(assetNameForMeta(".meta").empty());
}

TEST_CASE("asset_meta: isScannableAssetName rejects hidden names (AM4, AC-21)") {
    CHECK_FALSE(isScannableAssetName(".git"));
    CHECK_FALSE(isScannableAssetName(".DS_Store"));
    CHECK_FALSE(isScannableAssetName("."));
}

TEST_CASE("asset_meta: isScannableAssetName rejects sidecars (AM5, AC-21)") {
    CHECK_FALSE(isScannableAssetName("wood.png.meta"));
    CHECK_FALSE(isScannableAssetName("wood.png.META"));
}

TEST_CASE("asset_meta: isScannableAssetName rejects *.aero-tmp by SUFFIX (AM6, AC-21, E19)") {
    CHECK_FALSE(isScannableAssetName("wood.png.aero-tmp"));
    CHECK_FALSE(isScannableAssetName("wood.png.meta.aero-tmp"));  // E19: a temp of a sidecar
}

TEST_CASE("asset_meta: isScannableAssetName rejects the two exact OS-noise names (AM7, AC-21)") {
    CHECK_FALSE(isScannableAssetName("Thumbs.db"));
    CHECK_FALSE(isScannableAssetName("desktop.ini"));
}

TEST_CASE("asset_meta: isScannableAssetName accepts everything else (AM8, AC-21, E28)") {
    CHECK(isScannableAssetName("notes.metal"));           // ends "metal", not ".meta"
    CHECK(isScannableAssetName("a.aero-tmp.png"));        // the suffix is a MIDDLE segment, not the tail
    CHECK(isScannableAssetName("\xF0\x9F\x9A\x80.png"));  // an emoji leaf name (E28)
    // Equality, never a substring test: a name that merely CONTAINS "Thumbs.db" is scannable. (Task
    // E.4.4 made the equality ASCII-case-folded and put ".bak" on the roster -- so the vehicle below is
    // ".dbx", which proves the same thing and is not an ignored name.)
    CHECK(isScannableAssetName("MyThumbs.dbFile.png"));
    CHECK(isScannableAssetName("Thumbs.dbx"));
}

// ---- parseMeta success ------------------------------------------------------------------------

TEST_CASE("asset_meta: parseMeta succeeds on canonical text (AM9)") {
    const MetaParseResult result =
        parseMeta("{\n  \"version\": 1,\n  \"guid\": \"" + std::string(FIXTURE_GUID_TEXT) + "\"\n}\n");
    REQUIRE(result.guid.has_value());
    CHECK(result.error == MetaError::None);
    CHECK(result.message.empty());
    CHECK(result.unknownKeys.empty());
    CHECK(formatGuid(*result.guid) == FIXTURE_GUID_TEXT);
}

TEST_CASE("asset_meta: parseMeta tolerates a BOM (AM10)") {
    const std::string text =
        "\xEF\xBB\xBF{\n  \"version\": 1,\n  \"guid\": \"" + std::string(FIXTURE_GUID_TEXT) + "\"\n}\n";
    const MetaParseResult result = parseMeta(text);
    REQUIRE(result.guid.has_value());
}

TEST_CASE("asset_meta: parseMeta tolerates CRLF (AM11)") {
    const std::string text =
        "{\r\n  \"version\": 1,\r\n  \"guid\": \"" + std::string(FIXTURE_GUID_TEXT) + "\"\r\n}\r\n";
    const MetaParseResult result = parseMeta(text);
    REQUIRE(result.guid.has_value());
}

TEST_CASE("asset_meta: parseMeta tolerates a missing trailing newline (AM12)") {
    const std::string text = "{\n  \"version\": 1,\n  \"guid\": \"" + std::string(FIXTURE_GUID_TEXT) + "\"\n}";
    const MetaParseResult result = parseMeta(text);
    REQUIRE(result.guid.has_value());
}

// ---- parseMeta errors, message text asserted VERBATIM (docs/09 §5.4) -----------------------------

TEST_CASE("asset_meta: parseMeta rejects a non-object root (AM13)") {
    const MetaParseResult result = parseMeta("[]");
    CHECK(result.error == MetaError::NotAnObject);
    CHECK(result.message == "asset meta root must be a JSON object (found array)");
}

TEST_CASE("asset_meta: parseMeta rejects a missing version (AM14)") {
    const MetaParseResult result = parseMeta(R"({"guid": ")" + std::string(FIXTURE_GUID_TEXT) + "\"}");
    CHECK(result.error == MetaError::BadVersion);
    CHECK(result.message == "missing required key \"version\"");
}

TEST_CASE("asset_meta: parseMeta rejects a non-integer version (AM15)") {
    const MetaParseResult result = parseMeta(R"({"version": "1", "guid": ")" + std::string(FIXTURE_GUID_TEXT) + "\"}");
    CHECK(result.error == MetaError::BadVersion);
    CHECK(result.message == "\"version\" must be an integer (found string)");

    const MetaParseResult formResult = parseMeta(R"({"version": 1.5, "guid": "x"})");
    CHECK(formResult.error == MetaError::BadVersion);
    CHECK(formResult.message == "\"version\" must be an integer (found \"1.5\")");
}

TEST_CASE("asset_meta: parseMeta rejects an unsupported version (AM16)") {
    const MetaParseResult result = parseMeta(R"({"version": 2, "guid": ")" + std::string(FIXTURE_GUID_TEXT) + "\"}");
    CHECK(result.error == MetaError::UnsupportedVersion);
    CHECK(result.message == "unsupported asset meta format version 2 (this build reads version 1)");
}

TEST_CASE("asset_meta: parseMeta rejects a missing guid (AM17)") {
    const MetaParseResult result = parseMeta(R"({"version": 1})");
    CHECK(result.error == MetaError::MissingGuid);
    CHECK(result.message == "missing required key \"guid\"");
}

TEST_CASE("asset_meta: parseMeta rejects a non-string guid (AM18)") {
    const MetaParseResult result = parseMeta(R"({"version": 1, "guid": 5})");
    CHECK(result.error == MetaError::BadGuidKind);
    CHECK(result.message == "\"guid\" must be a string (found number)");
}

TEST_CASE("asset_meta: parseMeta rejects a badly-shaped guid text (AM19)") {
    const MetaParseResult result = parseMeta(R"({"version": 1, "guid": "not-a-guid"})");
    CHECK(result.error == MetaError::BadGuidText);
    CHECK(result.message == "\"guid\" must be 32 hexadecimal digits (found \"not-a-guid\")");
}

TEST_CASE("asset_meta: parseMeta rejects the nil guid (AM20)") {
    const MetaParseResult result = parseMeta(R"({"version": 1, "guid": ")" + std::string(32, '0') + "\"}");
    CHECK(result.error == MetaError::NilGuid);
    CHECK(result.message == "\"guid\" must not be the nil GUID");
}

TEST_CASE("asset_meta: version is validated BEFORE guid (AM21, AC-13, seed S9)") {
    // Missing BOTH keys must still report the VERSION error, not the guid one.
    const MetaParseResult result = parseMeta("{}");
    CHECK(result.error == MetaError::BadVersion);
    CHECK(result.message == "missing required key \"version\"");
}

TEST_CASE("asset_meta: an unsupported version wins even when guid is ALSO missing (AM22, AC-13)") {
    const MetaParseResult result = parseMeta("{\"version\": 2}");
    CHECK(result.error == MetaError::UnsupportedVersion);
}

TEST_CASE("asset_meta: unknown keys are collected in document order (AM23, AC-14)") {
    const MetaParseResult result = parseMeta(R"({"version": 1, "importer": "texture", "guid": ")" +
                                             std::string(FIXTURE_GUID_TEXT) + R"(", "userData": {"note": "x"}})");
    REQUIRE(result.guid.has_value());
    REQUIRE(result.unknownKeys.size() == 2);
    CHECK(result.unknownKeys[0] == "importer");
    CHECK(result.unknownKeys[1] == "userData");
}

TEST_CASE("asset_meta: a nested key under an unknown key is NOT separately listed (AM24)") {
    const MetaParseResult result = parseMeta(R"({"version": 1, "guid": ")" + std::string(FIXTURE_GUID_TEXT) +
                                             R"(", "userData": {"note": "x", "nested": {"deep": 1}}})");
    REQUIRE(result.guid.has_value());
    REQUIRE(result.unknownKeys.size() == 1);
    CHECK(result.unknownKeys[0] == "userData");  // "note"/"nested"/"deep" never appear
}

// ---- writeMetaText and the two round-trip guarantees (docs/09 §1) --------------------------------

TEST_CASE("asset_meta: writeMetaText is byte-exact (AM25, AC-16)") {
    const std::optional<Guid> guid = parseGuid(FIXTURE_GUID_TEXT);
    REQUIRE(guid.has_value());
    CHECK(writeMetaText(*guid) == "{\n  \"version\": 1,\n  \"guid\": \"" + std::string(FIXTURE_GUID_TEXT) + "\"\n}\n");
}

TEST_CASE("asset_meta: canonical text is byte-stable through parse -> write (AM26, docs/09 §1 guarantee 1)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MINIMAL_FIXTURE);
    REQUIRE(fixture.ok);
    const MetaParseResult parsed = parseMeta(fixture.text);
    REQUIRE(parsed.guid.has_value());
    const std::string written = writeMetaText(*parsed.guid);
    INFO(scene_golden::describeMismatch(fixture.text, written));
    CHECK(written == fixture.text);
}

TEST_CASE("asset_meta: writing is idempotent for non-canonical input (AM27, docs/09 §1 guarantee 2)") {
    // No trailing newline and CRLF line endings -- NOT canonical, but a legally parsed document.
    const std::string nonCanonical = "{\r\n\"version\": 1,\r\n\"guid\": \"" + std::string(FIXTURE_GUID_TEXT) + "\"}";
    const MetaParseResult firstParse = parseMeta(nonCanonical);
    REQUIRE(firstParse.guid.has_value());
    const std::string firstWrite = writeMetaText(*firstParse.guid);

    const MetaParseResult secondParse = parseMeta(firstWrite);
    REQUIRE(secondParse.guid.has_value());
    const std::string secondWrite = writeMetaText(*secondParse.guid);

    CHECK(firstWrite == secondWrite);
}

// ---- planAssetMetas, all PURE, all with a FIXED seed (D5-D9) --------------------------------------

namespace {
AssetPlanEntry entryOk(std::string_view path, Guid guid) { return AssetPlanEntry{std::string(path), guid, true}; }
AssetPlanEntry entryMissing(std::string_view path) { return AssetPlanEntry{std::string(path), std::nullopt, false}; }
AssetPlanEntry entryInvalid(std::string_view path) { return AssetPlanEntry{std::string(path), std::nullopt, true}; }
// task 3.1.2 (D13/A14) -- designated initializers, since `reattachedGuid` is the FOURTH field and the
// three helpers above stay on the original 3-positional-argument form (AssetPlanEntry::reattachedGuid
// is APPENDED, never inserted, so every 3.1.1 literal keeps its exact original meaning; AP20 pins it).
AssetPlanEntry entryReattached(std::string_view path, Guid reattachedGuid) {
    return AssetPlanEntry{.relativePath = std::string(path),
                          .guid = std::nullopt,
                          .metaPresent = false,
                          .reattachedGuid = reattachedGuid};
}
}  // namespace

TEST_CASE("asset_meta: planAssetMetas on empty input (AP1)") {
    GuidGenerator gen(1);
    const AssetPlanResult result = planAssetMetas({}, gen);
    CHECK(result.records.empty());
    CHECK(result.writeIndices.empty());
    CHECK(result.created == 0);
    CHECK(result.repaired == 0);
    CHECK(result.invalid == 0);
}

TEST_CASE("asset_meta: planAssetMetas create-only (AP2, AC-23)") {
    GuidGenerator gen(2);
    std::vector<AssetPlanEntry> entries{entryMissing("b.png"), entryMissing("a.png")};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 2);
    CHECK(result.created == 2);
    CHECK(result.repaired == 0);
    CHECK(result.invalid == 0);
    CHECK(result.writeIndices.size() == 2);
    for (const AssetRecord& r : result.records) {
        CHECK(r.state == AssetMetaState::Created);
        CHECK(r.guid.valid());
    }
    CHECK(result.records[0].guid != result.records[1].guid);
}

TEST_CASE("asset_meta: an all-valid tree writes NOTHING (AP3, D6, AC-24, seed S13)") {
    GuidGenerator gen(3);
    const Guid a{1, 1};
    const Guid b{2, 2};
    std::vector<AssetPlanEntry> entries{entryOk("b.png", b), entryOk("a.png", a)};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 2);
    CHECK(result.created == 0);
    CHECK(result.repaired == 0);
    CHECK(result.invalid == 0);
    CHECK(result.writeIndices.empty());  // THE most important property in this file
    CHECK(result.records[0].relativePath == "a.png");
    CHECK(result.records[0].guid == a);
    CHECK(result.records[0].state == AssetMetaState::Ok);
    CHECK(result.records[1].guid == b);
}

TEST_CASE("asset_meta: an invalid sidecar is never given an identity or written (AP4, AC-25/27, seed S14)") {
    GuidGenerator gen(4);
    std::vector<AssetPlanEntry> entries{entryInvalid("broken.png")};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 1);
    CHECK(result.records[0].state == AssetMetaState::Invalid);
    CHECK(result.records[0].guid == Guid{});  // nil
    CHECK(result.invalid == 1);
    CHECK(result.writeIndices.empty());
}

TEST_CASE("asset_meta: a duplicate GUID repairs the byte-lexicographically LATER path (AP5, D9, AC-26, seed S16)") {
    GuidGenerator gen(5);
    const Guid shared{7, 7};
    std::vector<AssetPlanEntry> entries{entryOk("z.png", shared), entryOk("a.png", shared)};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 2);
    // Sorted first: "a.png" precedes "z.png" -- the KEEPER is the byte-lexicographically first.
    CHECK(result.records[0].relativePath == "a.png");
    CHECK(result.records[0].guid == shared);
    CHECK(result.records[0].state == AssetMetaState::Ok);
    CHECK(result.records[1].relativePath == "z.png");
    CHECK(result.records[1].guid != shared);
    CHECK(result.records[1].state == AssetMetaState::Repaired);
    CHECK(result.repaired == 1);
    REQUIRE(result.writeIndices.size() == 1);
    CHECK(result.writeIndices[0] == 1);  // the loser only -- the keeper is NOT written
}

TEST_CASE("asset_meta: a three-way duplicate repairs two, mutually distinct (AP6, D9)") {
    GuidGenerator gen(6);
    const Guid shared{9, 9};
    std::vector<AssetPlanEntry> entries{entryOk("c.png", shared), entryOk("a.png", shared), entryOk("b.png", shared)};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 3);
    CHECK(result.records[0].relativePath == "a.png");
    CHECK(result.records[0].guid == shared);
    CHECK(result.records[0].state == AssetMetaState::Ok);
    CHECK(result.records[1].state == AssetMetaState::Repaired);
    CHECK(result.records[2].state == AssetMetaState::Repaired);
    CHECK(result.records[1].guid != result.records[2].guid);
    CHECK(result.records[1].guid != shared);
    CHECK(result.records[2].guid != shared);
    CHECK(result.repaired == 2);
    CHECK(result.writeIndices.size() == 2);
}

TEST_CASE("asset_meta: a duplicate where the keeper is CREATED and the loser is Ok (AP7, D9)") {
    GuidGenerator gen(7);
    // "a.png" has no sidecar (Created, minted fresh); "z.png" already claims the SAME fresh value --
    // impossible with a real generator, so build it by minting first, then reusing that value.
    GuidGenerator peek(7);
    const Guid minted = peek.next();  // exactly what gen will mint for "a.png" below
    std::vector<AssetPlanEntry> entries{entryOk("z.png", minted), entryMissing("a.png")};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 2);
    CHECK(result.records[0].relativePath == "a.png");
    CHECK(result.records[0].state == AssetMetaState::Created);
    CHECK(result.records[0].guid == minted);
    CHECK(result.records[1].relativePath == "z.png");
    CHECK(result.records[1].state == AssetMetaState::Repaired);
    CHECK(result.records[1].guid != minted);
}

TEST_CASE("asset_meta: a duplicate where the keeper is Ok and the loser is CREATED (AP8, D9)") {
    GuidGenerator gen(8);
    GuidGenerator peek(8);
    const Guid minted = peek.next();  // what gen mints for "z.png", the second (losing) entry
    std::vector<AssetPlanEntry> entries{entryOk("a.png", minted), entryMissing("z.png")};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 2);
    CHECK(result.records[0].relativePath == "a.png");
    CHECK(result.records[0].state == AssetMetaState::Ok);
    CHECK(result.records[0].guid == minted);
    CHECK(result.records[1].relativePath == "z.png");
    CHECK(result.records[1].state == AssetMetaState::Repaired);
    CHECK(result.records[1].guid != minted);
}

TEST_CASE("asset_meta: shuffled input gives an identical result (AP9, AC-22, seed S15)") {
    const Guid gA{1, 1};
    const Guid gB{2, 2};
    const std::vector<AssetPlanEntry> order1{entryOk("a.png", gA), entryOk("b.png", gB), entryMissing("c.png")};
    const std::vector<AssetPlanEntry> order2{entryMissing("c.png"), entryOk("a.png", gA), entryOk("b.png", gB)};
    const std::vector<AssetPlanEntry> order3{entryOk("b.png", gB), entryMissing("c.png"), entryOk("a.png", gA)};

    GuidGenerator gen1(9);
    GuidGenerator gen2(9);
    GuidGenerator gen3(9);
    const AssetPlanResult r1 = planAssetMetas(order1, gen1);
    const AssetPlanResult r2 = planAssetMetas(order2, gen2);
    const AssetPlanResult r3 = planAssetMetas(order3, gen3);

    REQUIRE(r1.records.size() == 3);
    REQUIRE(r2.records.size() == 3);
    REQUIRE(r3.records.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(r1.records[i].relativePath == r2.records[i].relativePath);
        CHECK(r1.records[i].relativePath == r3.records[i].relativePath);
        CHECK(r1.records[i].guid == r2.records[i].guid);
        CHECK(r1.records[i].guid == r3.records[i].guid);
        CHECK(r1.records[i].state == r2.records[i].state);
        CHECK(r1.records[i].state == r3.records[i].state);
    }
}

TEST_CASE("asset_meta: the sort is BYTE order, not case-folded -- 'Z.png' precedes 'a.png' (AP10, seed S15)") {
    GuidGenerator gen(10);
    std::vector<AssetPlanEntry> entries{entryOk("a.png", Guid{1, 1}), entryOk("Z.png", Guid{2, 2})};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 2);
    CHECK(result.records[0].relativePath == "Z.png");  // 'Z' (0x5A) < 'a' (0x61) in byte order
    CHECK(result.records[1].relativePath == "a.png");
}

TEST_CASE("asset_meta: an Invalid record is never a duplicate keeper or repair target (AP11, INV-A7)") {
    GuidGenerator gen(11);
    // Two invalid entries share the same (nil) "guid" conceptually -- neither may claim or be
    // repaired; both stay Invalid, and NEITHER is written.
    std::vector<AssetPlanEntry> entries{entryInvalid("a.png"), entryInvalid("b.png")};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 2);
    CHECK(result.records[0].state == AssetMetaState::Invalid);
    CHECK(result.records[1].state == AssetMetaState::Invalid);
    CHECK(result.invalid == 2);
    CHECK(result.repaired == 0);
    CHECK(result.writeIndices.empty());
}

TEST_CASE("asset_meta: counts are consistent with writeIndices in a mixed tree (AP12, AC-24)") {
    GuidGenerator gen(12);
    const Guid shared{5, 5};
    std::vector<AssetPlanEntry> entries{
        entryOk("a.png", Guid{1, 1}),  // Ok, no write
        entryInvalid("b.png"),         // Invalid, no write
        entryMissing("c.png"),         // Created, write
        entryOk("d.png", shared),      // Ok (first claimant), no write
        entryOk("e.png", shared),      // Repaired (second claimant), write
    };
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 5);
    // task 3.1.2 (A14): the invariant gained a third term, `reattached` -- updated here, the only
    // place this task changes an existing 3.1.1 assertion (D-10's documented caveat).
    CHECK(result.created + result.repaired + result.reattached == result.writeIndices.size());
    CHECK(result.created == 1);
    CHECK(result.repaired == 1);
    CHECK(result.invalid == 1);
    CHECK(result.reattached == 0);
}

TEST_CASE("asset_meta: records stay sorted and every writeIndex is in range and unique (AP13)") {
    GuidGenerator gen(13);
    std::vector<AssetPlanEntry> entries{entryMissing("z.png"), entryMissing("m.png"), entryMissing("a.png"),
                                        entryOk("d.png", Guid{4, 4})};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(std::is_sorted(result.records.begin(), result.records.end(),
                           [](const AssetRecord& a, const AssetRecord& b) { return a.relativePath < b.relativePath; }));
    std::vector<std::size_t> seen;
    for (const std::size_t index : result.writeIndices) {
        CHECK(index < result.records.size());
        CHECK(std::find(seen.begin(), seen.end(), index) == seen.end());
        seen.push_back(index);
    }
}

TEST_CASE("asset_meta: planAssetMetas is deterministic for a fixed seed (AP14, D2)") {
    const std::vector<AssetPlanEntry> entries{entryMissing("z.png"), entryMissing("a.png"),
                                              entryOk("m.png", Guid{3, 3})};
    GuidGenerator genA(14);
    GuidGenerator genB(14);
    const AssetPlanResult resultA = planAssetMetas(entries, genA);
    const AssetPlanResult resultB = planAssetMetas(entries, genB);
    REQUIRE(resultA.records.size() == resultB.records.size());
    for (std::size_t i = 0; i < resultA.records.size(); ++i) {
        CHECK(resultA.records[i].relativePath == resultB.records[i].relativePath);
        CHECK(resultA.records[i].guid == resultB.records[i].guid);
        CHECK(resultA.records[i].state == resultB.records[i].state);
    }
    CHECK(resultA.writeIndices == resultB.writeIndices);
}

// ---- the reattachedGuid arm, task 3.1.2 (D13/A14) --------------------------------------------------

TEST_CASE(
    "asset_meta: a reattachedGuid entry becomes Reattached, keeps that exact GUID, is written, and consumes "
    "NO GUID from the generator (AP15, D13, A14)") {
    const Guid reattached{7, 7};
    GuidGenerator gen(15);
    std::vector<AssetPlanEntry> entries{entryReattached("a.png", reattached)};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 1);
    CHECK(result.records[0].guid == reattached);
    CHECK(result.records[0].state == AssetMetaState::Reattached);
    REQUIRE(result.writeIndices.size() == 1);
    CHECK(result.writeIndices[0] == 0);

    // generator.next() was NOT consumed: a FRESH generator with the SAME seed draws the SAME value
    // `gen` would draw next -- if planAssetMetas had called next() even once, the two would diverge.
    GuidGenerator freshGen(15);
    CHECK(gen.next() == freshGen.next());
}

TEST_CASE("asset_meta: a Reattached GUID colliding with an Ok record's is repaired like any other (AP16, D13)") {
    const Guid shared{8, 8};
    GuidGenerator gen(16);
    // 'a.png' sorts first -> claims `shared` first, as Ok. 'z.png' sorts second -> the Reattached
    // arm settles it to `shared` in step 2, then step 3's claim map finds it already taken and
    // repairs it -- exactly like any other second claimant (D-10's documented arm).
    std::vector<AssetPlanEntry> entries{entryOk("a.png", shared), entryReattached("z.png", shared)};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 2);
    CHECK(result.records[0].relativePath == "a.png");
    CHECK(result.records[0].state == AssetMetaState::Ok);
    CHECK(result.records[0].guid == shared);
    CHECK(result.records[1].relativePath == "z.png");
    CHECK(result.records[1].state == AssetMetaState::Repaired);
    CHECK(result.records[1].guid != shared);
}

TEST_CASE("asset_meta: created + repaired + reattached == writeIndices.size() in a mixed tree (AP17, A14)") {
    GuidGenerator gen(17);
    const Guid shared{9, 9};
    std::vector<AssetPlanEntry> entries{
        entryOk("a.png", Guid{1, 1}),          // Ok, no write
        entryInvalid("b.png"),                 // Invalid, no write
        entryMissing("c.png"),                 // Created, write
        entryReattached("d.png", Guid{5, 5}),  // Reattached, write
        entryOk("e.png", shared),              // Ok (first claimant), no write
        entryOk("f.png", shared),              // Repaired (second claimant), write
    };
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 6);
    CHECK(result.created + result.repaired + result.reattached == result.writeIndices.size());
    CHECK(result.created == 1);
    CHECK(result.repaired == 1);
    CHECK(result.reattached == 1);
    CHECK(result.invalid == 1);
}

TEST_CASE("asset_meta: reattached is counted separately from created (AP18, A14)") {
    GuidGenerator gen(18);
    std::vector<AssetPlanEntry> entries{entryMissing("a.png"), entryReattached("b.png", Guid{3, 3})};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 2);
    CHECK(result.created == 1);
    CHECK(result.reattached == 1);
    CHECK(result.repaired == 0);
    REQUIRE(result.writeIndices.size() == 2);
}

TEST_CASE(
    "asset_meta: metaPresent == true wins over a set reattachedGuid -- the sidecar's own identity is kept "
    "(AP19, A14)") {
    GuidGenerator gen(19);
    const Guid sidecarGuid{4, 4};
    const Guid wouldBeReattached{6, 6};
    // The caller never actually does this (asset_database.cpp's phase 5 only ever sets reattachedGuid
    // for a METALESS entry) -- but the arm order in planAssetMetas makes the combination decidable
    // rather than undefined: metaPresent's arm is checked FIRST for the Invalid case, and the
    // reattachedGuid arm is gated on `!metaPresent`, so a present sidecar always wins.
    std::vector<AssetPlanEntry> entries{AssetPlanEntry{
        .relativePath = "a.png", .guid = sidecarGuid, .metaPresent = true, .reattachedGuid = wouldBeReattached}};
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 1);
    CHECK(result.records[0].state == AssetMetaState::Ok);
    CHECK(result.records[0].guid == sidecarGuid);
    CHECK(result.records[0].guid != wouldBeReattached);
    CHECK(result.writeIndices.empty());  // Ok is never written (D6)
}

TEST_CASE(
    "asset_meta: every 3.1.1-shaped entry (reattachedGuid == nullopt by default) reproduces PRE-3.1.2 "
    "behavior exactly (AP20, A14)") {
    // entryOk/entryMissing/entryInvalid all still construct via the ORIGINAL 3-positional-argument
    // form, unchanged since 3.1.1 -- possible only because AssetPlanEntry::reattachedGuid is APPENDED,
    // never inserted. This is the one place this task's asset_meta change is verified in isolation
    // from any Reattached record at all; every pre-existing AP1-AP14 case is re-run untouched as
    // further proof (task report).
    GuidGenerator gen(20);
    std::vector<AssetPlanEntry> entries{entryOk("a.png", Guid{1, 1}), entryMissing("b.png"), entryInvalid("c.png")};
    for (const AssetPlanEntry& entry : entries) {
        CHECK_FALSE(entry.reattachedGuid.has_value());
    }
    const AssetPlanResult result = planAssetMetas(std::move(entries), gen);
    REQUIRE(result.records.size() == 3);
    CHECK(result.records[0].state == AssetMetaState::Ok);
    CHECK(result.records[1].state == AssetMetaState::Created);
    CHECK(result.records[2].state == AssetMetaState::Invalid);
    CHECK(result.reattached == 0);
}

// ---- the golden battery (docs/09 §5.7) -------------------------------------------------------------
// scene_golden::readBytes / hygieneComplaint / describeMismatch, NO dumpActual -- nothing here writes
// anywhere, so ctest -j has no collision surface.

TEST_CASE("asset_meta: minimal.meta is a fixpoint under parse -> write (AG1, AC-12)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MINIMAL_FIXTURE);
    REQUIRE(fixture.ok);
    CHECK(scene_golden::hygieneComplaint(fixture.text).empty());
    const MetaParseResult parsed = parseMeta(fixture.text);
    REQUIRE(parsed.guid.has_value());
    const std::string written = writeMetaText(*parsed.guid);
    INFO(scene_golden::describeMismatch(fixture.text, written));
    CHECK(written == fixture.text);
}

TEST_CASE("asset_meta: a second parse -> write cycle is byte-identical (AG2, AC-12)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MINIMAL_FIXTURE);
    REQUIRE(fixture.ok);
    const MetaParseResult firstParse = parseMeta(fixture.text);
    REQUIRE(firstParse.guid.has_value());
    const std::string firstWrite = writeMetaText(*firstParse.guid);

    const MetaParseResult secondParse = parseMeta(firstWrite);
    REQUIRE(secondParse.guid.has_value());
    const std::string secondWrite = writeMetaText(*secondParse.guid);

    INFO(scene_golden::describeMismatch(firstWrite, secondWrite));
    CHECK(firstWrite == secondWrite);
}

TEST_CASE("asset_meta: unknown-keys.meta parses, yields the right GUID and reports both unknowns (AG3)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(UNKNOWN_KEYS_FIXTURE);
    REQUIRE(fixture.ok);
    const MetaParseResult parsed = parseMeta(fixture.text);
    REQUIRE(parsed.guid.has_value());
    CHECK(formatGuid(*parsed.guid) == FIXTURE_GUID_TEXT);
    REQUIRE(parsed.unknownKeys.size() == 2);
    CHECK(parsed.unknownKeys[0] == "importer");
    CHECK(parsed.unknownKeys[1] == "userData");
}

TEST_CASE("asset_meta: minimal.meta's GUID equals a hardcoded literal (AG4, semantic)") {
    // Deliberately independent of AG1/AG2's byte comparison -- 2.5.2's S12 / 2.6.1's S9 lesson: a
    // parse/write pair that both stopped handling a key agrees WITH ITSELF, and every byte-only case
    // passes the moment the fixture is regenerated from the buggy build. This reads the fixture and
    // compares to a LITERAL, never to another product output.
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MINIMAL_FIXTURE);
    REQUIRE(fixture.ok);
    const MetaParseResult parsed = parseMeta(fixture.text);
    REQUIRE(parsed.guid.has_value());
    const std::optional<Guid> literal = parseGuid(FIXTURE_GUID_TEXT);
    REQUIRE(literal.has_value());
    CHECK(*parsed.guid == *literal);
}

TEST_CASE("asset_meta: minimal.meta's raw bytes name version 1 and a 32-lowercase-hex guid (AG5, semantic)") {
    // Read the RAW bytes and check them with plain string search -- never through parseMeta/formatGuid,
    // so this case cannot be fooled by a bug shared between the reader and the writer.
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MINIMAL_FIXTURE);
    REQUIRE(fixture.ok);
    CHECK(fixture.text.find("\"version\": 1,") != std::string::npos);

    const std::size_t guidKeyPos = fixture.text.find(R"("guid": ")");
    REQUIRE(guidKeyPos != std::string::npos);
    const std::size_t valueStart = guidKeyPos + std::string(R"("guid": ")").size();
    const std::size_t valueEnd = fixture.text.find('"', valueStart);
    REQUIRE(valueEnd != std::string::npos);
    const std::string_view guidText(fixture.text.data() + valueStart, valueEnd - valueStart);
    REQUIRE(guidText.size() == 32);
    for (const char c : guidText) {
        const bool isLowerHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(isLowerHex);
    }
    CHECK(guidText == FIXTURE_GUID_TEXT);
}

// AG6 -- a freshly created sidecar is byte-identical to minimal.meta MODULO its GUID -- needs the
// real AssetDatabase and is written in Step 3's tests/editor/asset_database_test.cpp.

// ---- isWatchableAssetName (task 3.1.4, D4) --------------------------------------------------------

TEST_CASE("asset_meta: isWatchableAssetName accepts an ordinary asset name (AM-w1, AC-15)") {
    CHECK(isWatchableAssetName("wood.png"));
}

TEST_CASE("asset_meta: isWatchableAssetName accepts a .meta sidecar (AM-w2, AC-15)") {
    CHECK(isWatchableAssetName("wood.png.meta"));
}

TEST_CASE("asset_meta: isWatchableAssetName rejects a hidden dotfile (AM-w3, AC-13)") {
    CHECK_FALSE(isWatchableAssetName(".DS_Store"));
}

TEST_CASE("asset_meta: isWatchableAssetName rejects a hidden name (AM-w4, AC-13)") {
    CHECK_FALSE(isWatchableAssetName(".hidden"));
}

TEST_CASE("asset_meta: isWatchableAssetName rejects an .aero-tmp file (AM-w5, AC-14)") {
    CHECK_FALSE(isWatchableAssetName("wood.png.aero-tmp"));
}

TEST_CASE("asset_meta: isWatchableAssetName rejects .meta.aero-tmp -- a suffix test, not equality (AM-w6, E19)") {
    CHECK_FALSE(isWatchableAssetName("wood.png.meta.aero-tmp"));
}

TEST_CASE("asset_meta: isWatchableAssetName rejects the empty name (AM-w7)") { CHECK_FALSE(isWatchableAssetName("")); }

TEST_CASE("asset_meta: isWatchableAssetName rejects the two OS-noise names (AM-w8)") {
    CHECK_FALSE(isWatchableAssetName("Thumbs.db"));
    CHECK_FALSE(isWatchableAssetName("desktop.ini"));
}

TEST_CASE("asset_meta: isWatchableAssetName rejects \".meta\" alone -- not a sidecar, and hidden (AM-w9)") {
    CHECK_FALSE(isWatchableAssetName(".meta"));
}

TEST_CASE("asset_meta: isWatchableAssetName folds the sidecar suffix's case (AM-w10)") {
    CHECK(isWatchableAssetName("wood.png.META"));
}

TEST_CASE("asset_meta: isWatchableAssetName is EXACTLY the composition of the two predicates (AM-w11, D4)") {
    constexpr std::array<std::string_view, 10> NAMES = {
        "wood.png", "wood.png.meta", ".DS_Store", ".hidden",       "wood.png.aero-tmp", "wood.png.meta.aero-tmp",
        "",         "Thumbs.db",     ".meta",     "wood.png.META",
    };
    // task E.4.4 WIDENED this corpus rather than adding a case -- INV-W3's equality re-proved over the
    // names the recomposition changed: the Blender forms, a folded OS name, a sidecar OF a backup, a
    // HIDDEN sidecar, and every roster entry alone, after a real name and after a sidecar name.
    constexpr std::array<std::string_view, 8> ROSTER_FORMS = {
        "THUMBS.DB", "scene.blend",       "scene.blend1", "scene.blend32",
        ".blend7",   "scene.blend1.meta", ".x.png.meta",  "a.blend1x",
    };
    std::vector<std::string> names(NAMES.begin(), NAMES.end());
    names.insert(names.end(), ROSTER_FORMS.begin(), ROSTER_FORMS.end());
    for (const std::string_view entry : IGNORED_ASSET_NAMES) {
        names.emplace_back(entry);
    }
    for (const std::string_view suffix : IGNORED_ASSET_NAME_SUFFIXES) {
        names.emplace_back(suffix);
        names.push_back("wood.png" + std::string(suffix));
        names.push_back("wood.png.meta" + std::string(suffix));
    }
    REQUIRE(names.size() > 30U);  // anti-vacuity: the roster really contributed (41 today)
    for (const std::string& name : names) {
        CAPTURE(name);
        CHECK(isWatchableAssetName(name) == (isScannableAssetName(name) || isMetaFileName(name)));
    }
}

// ---- the ignore roster (task E.4.4, IX1-IX18) ----------------------------------------------------
//
// EVERY case that ranges over the roster ITERATES THE ARRAYS, so deleting an entry shrinks a corpus
// rather than deleting an assertion (D1, the ASSET_KIND_FILTER_OPTIONS lesson). Every array-driven loop
// is preceded by a REQUIRE that the array is non-empty, so an emptied array cannot pass vacuously.
// Each case name ENDS with its id and ")" so a -tc='*IX7)' filter selects exactly one case.

namespace {

// ASCII-only case mappers -- NEVER std::toupper(char): a UTF-8 continuation byte is negative as char.
[[nodiscard]] std::string asciiUpper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - ('a' - 'A'));
        }
    }
    return out;
}

[[nodiscard]] std::string asciiLower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return out;
}

// One name per scannable kind this editor classifies, plus the Blender ASSET itself and a name with no
// extension. Shared by IX16 and AM28.
constexpr std::array<std::string_view, 14> REAL_ASSET_NAMES = {
    "wood.png",  "hero.glb", "scene.gltf", "a.fbx", "a.obj",    "model.blend", "clip.wav",
    "m.aeromat", "t.ktx2",   "x.hlsl",     "x.ts",  "notes.md", "README",      "level.scene.json",
};

constexpr std::string_view ASSET_META_HEADER = AERO_EDITOR_SRC_DIR "/../include/aero/editor/asset_meta.hpp";

// The comment-stripped code lines of a text -- the editorSourceCodeLines rule (imgui_layer_test.cpp): a
// citation in PROSE must never satisfy or break a gate about CODE.
[[nodiscard]] std::vector<std::string> codeLinesOf(std::string_view text) {
    std::vector<std::string> lines;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        if (const std::size_t comment = line.find("//"); comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }
        lines.emplace_back(line);
        if (newline == std::string_view::npos) {
            break;
        }
        text.remove_prefix(newline + 1);
    }
    return lines;
}

}  // namespace

TEST_CASE("asset_meta: every exact roster name is ignored, in any ASCII case (AC-4, IX1)") {
    REQUIRE_FALSE(IGNORED_ASSET_NAMES.empty());
    for (const std::string_view name : IGNORED_ASSET_NAMES) {
        CAPTURE(name);
        CHECK(isIgnoredAssetName(name));
        CHECK(isIgnoredAssetName(asciiUpper(name)));  // a widening: 3.1.1 compared these byte for byte
        CHECK(isIgnoredAssetName(asciiLower(name)));
    }
}

TEST_CASE("asset_meta: a roster suffix after a real name is ignored; the name alone is not (AC-2, IX2)") {
    constexpr std::string_view STEM = "wood.png";
    // The negative arm is what makes the loop a STATEMENT: a predicate that ignored everything would pass
    // the loop and fail here.
    REQUIRE_FALSE(isIgnoredAssetName(STEM));
    REQUIRE_FALSE(IGNORED_ASSET_NAME_SUFFIXES.empty());
    for (const std::string_view suffix : IGNORED_ASSET_NAME_SUFFIXES) {
        const std::string name = std::string(STEM) + std::string(suffix);
        CAPTURE(name);
        CHECK(isIgnoredAssetName(name));
    }
}

TEST_CASE("asset_meta: the roster is exactly the documented entries, none empty or shadowed (IX3)") {
    // The roster's CONTENTS, pinned. A universal over the arrays cannot see an entry DELETED from them --
    // the corpus simply shrinks -- so this is the one place, with docs/09 section 5.10, where the entries
    // are restated on purpose. Changing the roster means changing this pin and 5.10 in the same commit.
    const std::vector<std::string_view> names(IGNORED_ASSET_NAMES.begin(), IGNORED_ASSET_NAMES.end());
    const std::vector<std::string_view> expectedNames{"Thumbs.db", "desktop.ini"};
    CHECK(names == expectedNames);
    const auto& roster = IGNORED_ASSET_NAME_SUFFIXES;
    const std::vector<std::string_view> suffixes(roster.begin(), roster.end());
    const std::vector<std::string_view> expectedSuffixes{
        ".aero-tmp", ".blend@", ".bak", ".tmp", ".orig", ".rej", "~",
    };
    CHECK(suffixes == expectedSuffixes);
    CHECK(BLEND_BACKUP_STEM == ".blend");
    // ...and the well-formedness claims, which any future entry must also satisfy.
    CHECK_FALSE(IGNORED_ASSET_NAMES.empty());
    CHECK_FALSE(IGNORED_ASSET_NAME_SUFFIXES.empty());
    CHECK_FALSE(BLEND_BACKUP_STEM.empty());
    std::vector<std::string> folded;
    for (const std::string_view entry : IGNORED_ASSET_NAMES) {
        CAPTURE(entry);
        CHECK_FALSE(entry.empty());
        folded.push_back(asciiLower(entry));
    }
    for (const std::string_view entry : IGNORED_ASSET_NAME_SUFFIXES) {
        CAPTURE(entry);
        CHECK_FALSE(entry.empty());  // an EMPTY suffix would match every name in every project
        folded.push_back(asciiLower(entry));
    }
    std::vector<std::string> unique = folded;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    CHECK(unique.size() == folded.size());  // no entry is listed twice, in any case
    // No EXACT name is also matched by a SUFFIX -- that would make the exact-name loop partly dead code.
    // True today. If a future entry legitimately breaks it, delete THIS loop, with a comment naming the
    // exact entry the suffix loop now shadows, rather than working around it.
    for (const std::string_view name : IGNORED_ASSET_NAMES) {
        for (const std::string_view suffix : IGNORED_ASSET_NAME_SUFFIXES) {
            CAPTURE(name);
            CAPTURE(suffix);
            CHECK_FALSE(asciiLower(name).ends_with(asciiLower(suffix)));
        }
    }
}

TEST_CASE("asset_meta: an exact roster name is EQUALITY, never a substring (AC-4, seed S13, IX4)") {
    // Anti-vacuity FIRST: both exact names really are ignored, so the negatives below are about the SHAPE
    // of the match rather than about a roster that matches nothing.
    REQUIRE(isIgnoredAssetName("Thumbs.db"));
    REQUIRE(isIgnoredAssetName("desktop.ini"));
    constexpr std::array<std::string_view, 6> NAMES = {
        "MyThumbs.dbFile.png", "Thumbs.dbx", "xdesktop.ini", "my-desktop.ini", "Thumbs.db.png", "desktop.ini.txt",
    };
    for (const std::string_view name : NAMES) {
        CAPTURE(name);
        CHECK_FALSE(isIgnoredAssetName(name));
    }
}

TEST_CASE("asset_meta: a roster suffix in the MIDDLE of a name is not a match (IX5)") {
    REQUIRE(isIgnoredAssetName("a.bak"));  // anti-vacuity: the same suffix at the TAIL is a match
    constexpr std::array<std::string_view, 7> NAMES = {
        "a.bak.png", "a.aero-tmp.png", "a~.png", "a.tmp.fbx", "a.orig.txt", "a.rej.md", "a.blend@.glb",
    };
    for (const std::string_view name : NAMES) {
        CAPTURE(name);
        CHECK_FALSE(isIgnoredAssetName(name));
    }
}

TEST_CASE("asset_meta: a roster suffix matches in any ASCII case (AC-4, IX6)") {
    CHECK(isIgnoredAssetName("wood.png.BAK"));
    CHECK(isIgnoredAssetName("wood.png.Bak"));
    CHECK(isIgnoredAssetName("x.AERO-TMP"));  // a widening: 3.1.1 compared .aero-tmp case-SENSITIVELY
    CHECK(isIgnoredAssetName("x.Orig"));
    CHECK(isIgnoredAssetName("x.BLEND@"));
    REQUIRE_FALSE(IGNORED_ASSET_NAME_SUFFIXES.empty());
    for (const std::string_view suffix : IGNORED_ASSET_NAME_SUFFIXES) {
        const std::string upper = "x" + asciiUpper(suffix);
        CAPTURE(upper);
        CHECK(isIgnoredAssetName(upper));
    }
}

TEST_CASE("asset_meta: a name EQUAL to a roster suffix matches it, unlike .meta (seed S2, IX7)") {
    CHECK(isIgnoredAssetName("~"));
    CHECK(isIgnoredAssetName(".bak"));
    CHECK(isIgnoredAssetName(".aero-tmp"));
    REQUIRE_FALSE(IGNORED_ASSET_NAME_SUFFIXES.empty());
    for (const std::string_view suffix : IGNORED_ASSET_NAME_SUFFIXES) {
        CAPTURE(suffix);
        CHECK(isIgnoredAssetName(suffix));
    }
    // The asymmetry, stated where it can be read: ".meta" alone is NOT a sidecar (the `size > 5` guard).
    CHECK_FALSE(isMetaFileName(".meta"));
}

TEST_CASE("asset_meta: the empty name is invalid, not ignored (AC-1, IX8)") {
    CHECK_FALSE(isIgnoredAssetName(""));
    CHECK_FALSE(isScannableAssetName(""));  // refused on its OWN term
}

TEST_CASE("asset_meta: Blender's rolling backups are ignored, in range and beyond it (AC-5, IX9)") {
    // Save Versions' property range is 0..32 (rna_userdef.cc); 0 writes no backup at all.
    for (int n = 1; n <= 32; ++n) {
        const std::string name = "scene.blend" + std::to_string(n);
        CAPTURE(name);
        CHECK(isIgnoredAssetName(name));
    }
    // UNBOUNDED by decision (D3): past the maximum, zero-padded, a lone zero and a year all match.
    constexpr std::array<std::string_view, 5> BEYOND = {
        "model.blend33", "model.blend01", "model.blend0", "model.blend99", "model.blend2024",
    };
    for (const std::string_view name : BEYOND) {
        CAPTURE(name);
        CHECK(isIgnoredAssetName(name));
    }
}

TEST_CASE("asset_meta: the Blender ASSET itself is never ignored (AC-5, seed S3, IX10)") {
    // A REQUIRE, because every other Blender arm is meaningless if the asset itself is eaten -- a rule that
    // ate it would make every Blender project in this engine unusable.
    REQUIRE_FALSE(isIgnoredAssetName("model.blend"));
    REQUIRE(isScannableAssetName("model.blend"));
    CHECK_FALSE(isIgnoredAssetName("MODEL.BLEND"));
    CHECK_FALSE(isIgnoredAssetName("a.tar.blend"));
    CHECK_FALSE(isIgnoredAssetName(".blend"));  // hidden, and a backup of nothing
}

TEST_CASE("asset_meta: the Blender digit run must reach the TAIL (AC-6, IX11)") {
    REQUIRE(isIgnoredAssetName("a.blend1"));  // anti-vacuity: the tail form IS a match
    constexpr std::array<std::string_view, 3> NAMES = {"a.blend1x", "a.blend1.png", "a.blend 1"};
    for (const std::string_view name : NAMES) {
        CAPTURE(name);
        CHECK_FALSE(isIgnoredAssetName(name));
    }
}

TEST_CASE("asset_meta: what precedes the Blender digits must END with .blend (IX12)") {
    REQUIRE(isIgnoredAssetName("a.blend1"));
    constexpr std::array<std::string_view, 5> NAMES = {"blend1", "xblend1", "a.blen1", "a.blendx1", "a.blend.1"};
    for (const std::string_view name : NAMES) {
        CAPTURE(name);
        CHECK_FALSE(isIgnoredAssetName(name));
    }
}

TEST_CASE("asset_meta: an all-digit name strips to nothing and is not a backup (seed S4, IX13)") {
    constexpr std::array<std::string_view, 4> DIGITS = {"123", "0", "00", "2024"};
    for (const std::string_view name : DIGITS) {
        CAPTURE(name);
        CHECK_FALSE(isIgnoredAssetName(name));
        CHECK(isScannableAssetName(name));
    }
}

TEST_CASE("asset_meta: the Blender rule folds ASCII case (AC-4, IX14)") {
    CHECK(isIgnoredAssetName("x.BLEND2"));
    CHECK(isIgnoredAssetName("x.Blend2"));
    CHECK(isIgnoredAssetName("x.bLeNd7"));
}

TEST_CASE("asset_meta: a dot-prefixed backup is ignored AND hidden, independently (IX15)") {
    CHECK(isIgnoredAssetName(".blend7"));  // the remainder EQUALS the stem -- the edge seed S2 breaks
    CHECK(isHiddenName(".blend7"));
    CHECK_FALSE(isIgnoredAssetName(".hidden.png"));  // hidden but NOT ignored
    CHECK(isHiddenName(".hidden.png"));
}

TEST_CASE("asset_meta: no real asset name is ignored (IX16)") {
    REQUIRE_FALSE(REAL_ASSET_NAMES.empty());
    for (const std::string_view name : REAL_ASSET_NAMES) {
        CAPTURE(name);
        CHECK_FALSE(isIgnoredAssetName(name));
    }
}

TEST_CASE("asset_meta: ATOMIC_TEMP_SUFFIX is in the roster BY NAME (AC-3, seed S12, IX17)") {
    // (a) The VALUE: exactly one entry equals the constant.
    std::size_t copies = 0;
    for (const std::string_view suffix : IGNORED_ASSET_NAME_SUFFIXES) {
        copies += suffix == ATOMIC_TEMP_SUFFIX ? 1U : 0U;
    }
    CHECK(copies == 1U);
    // (b) The SOURCE. A re-spelled ".aero-tmp" literal has the same VALUE, so no runtime assertion can see
    // it -- only the header's own text can (E.1.4's sabotage row 20, a restated colour, is the same shape).
    const scene_golden::FileBytes header = scene_golden::readBytes(ASSET_META_HEADER);
    REQUIRE(header.ok);
    const std::vector<std::string> code = codeLinesOf(header.text);
    REQUIRE(code.size() > 100U);  // anti-vacuity: the reader really read the header
    std::size_t literalLines = 0;
    for (const std::string& line : code) {
        if (line.find("\".aero-tmp\"") != std::string::npos) {
            ++literalLines;
        }
    }
    CHECK(literalLines == 1U);  // ATOMIC_TEMP_SUFFIX's own definition, and nothing else
    std::string initializer;
    bool inside = false;
    for (const std::string& line : code) {
        inside = inside || line.find("IGNORED_ASSET_NAME_SUFFIXES{") != std::string::npos;
        if (inside) {
            initializer += line;
            initializer += '\n';
            if (line.find("};") != std::string::npos) {
                break;
            }
        }
    }
    INFO("initializer text: ", initializer);
    REQUIRE_FALSE(initializer.empty());  // anti-vacuity: the reader found the array at all
    CHECK(initializer.find("ATOMIC_TEMP_SUFFIX") != std::string::npos);
    CHECK(initializer.find("aero-tmp") == std::string::npos);
}

TEST_CASE("asset_meta: hidden, sidecar and ignored are three DISJOINT questions (IX18)") {
    CHECK_FALSE(isIgnoredAssetName("wood.png.meta"));  // a sidecar is not ignored ...
    CHECK_FALSE(isMetaFileName("wood.png.bak"));       // ... and an ignored name is not a sidecar
    CHECK_FALSE(isIgnoredAssetName(".DS_Store"));      // a hidden name is not ignored
    CHECK_FALSE(isIgnoredAssetName(".hidden"));
    // THE ORPHAN PATH DEPENDS ON THIS (D7): a sidecar OF an ignored file is still a SIDECAR, so the scan
    // still buckets it, consumes it with nothing, and reports it -- rather than dropping it silently.
    CHECK(isMetaFileName("scene.blend1.meta"));
    CHECK_FALSE(isIgnoredAssetName("scene.blend1.meta"));
    // ... and a backup OF a sidecar is ignored, not a sidecar: the TAIL decides, once.
    CHECK(isIgnoredAssetName("wood.png.meta.bak"));
    CHECK_FALSE(isMetaFileName("wood.png.meta.bak"));
    REQUIRE_FALSE(IGNORED_ASSET_NAME_SUFFIXES.empty());
    for (const std::string_view suffix : IGNORED_ASSET_NAME_SUFFIXES) {
        const std::string name = "wood.png" + std::string(suffix);
        CAPTURE(name);
        CHECK_FALSE(isMetaFileName(name));
        CHECK_FALSE(isHiddenName(name));
    }
}

TEST_CASE("asset_meta: isScannableAssetName is exactly four disjoint refusals (AC-7, seed S5, AM28)") {
    // AM-w11's shape, one layer down: a PROPERTY over a corpus, never a list of examples. isHiddenName is
    // taken from project_files.hpp HERE, in the test, and that is what proves the production code's INLINE
    // hidden term (asset_meta.cpp keeps `front() == '.'`) equal to the one-source rule.
    constexpr std::array<std::string_view, 13> EDGES = {
        "",
        ".DS_Store",
        ".hidden.png",
        "wood.png.meta",
        "wood.png.META",
        ".meta",
        "scene.blend1.meta",
        "model.blend1",
        "x.BLEND32",
        ".blend7",
        "a.blend1x",
        "123",
        "THUMBS.DB",
    };
    std::vector<std::string> names(EDGES.begin(), EDGES.end());
    names.insert(names.end(), REAL_ASSET_NAMES.begin(), REAL_ASSET_NAMES.end());
    for (const std::string_view entry : IGNORED_ASSET_NAMES) {
        names.emplace_back(entry);
        names.push_back(asciiUpper(entry));
    }
    for (const std::string_view suffix : IGNORED_ASSET_NAME_SUFFIXES) {
        names.emplace_back(suffix);
        names.push_back("wood.png" + std::string(suffix));
        names.push_back(".wood.png" + std::string(suffix));  // hidden AND ignored: two terms refuse it
    }
    std::size_t accepted = 0;
    std::size_t refused = 0;
    for (const std::string& name : names) {
        CAPTURE(name);
        const std::string_view n = name;
        const bool expected = !n.empty() && !isHiddenName(n) && !isMetaFileName(n) && !isIgnoredAssetName(n);
        CHECK(isScannableAssetName(n) == expected);
        if (expected) {
            ++accepted;
        } else {
            ++refused;
        }
    }
    // Anti-vacuity: the corpus straddles the predicate. Every real asset is accepted, and the roster alone
    // contributes more refusals than it has suffixes.
    CHECK(accepted >= REAL_ASSET_NAMES.size());
    CHECK(refused > IGNORED_ASSET_NAME_SUFFIXES.size());
}

// ---- the optional importer block (task 3.2.1, D6) --------------------------------------------------

TEST_CASE(
    "asset_meta: writeMetaText with DEFAULT settings is byte-identical to the one-arg overload (AM-i1, AC-9, "
    "INV-M10)") {
    const std::optional<Guid> guid = parseGuid(FIXTURE_GUID_TEXT);
    REQUIRE(guid.has_value());
    CHECK(writeMetaText(*guid, ImportSettings{}) == writeMetaText(*guid));
}

TEST_CASE(
    "asset_meta: writeMetaText with DEFAULT settings still matches minimal.meta's 65-byte fixpoint (AM-i2, AC-9)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MINIMAL_FIXTURE);
    REQUIRE(fixture.ok);
    const std::optional<Guid> guid = parseGuid(FIXTURE_GUID_TEXT);
    REQUIRE(guid.has_value());
    const std::string written = writeMetaText(*guid, ImportSettings{});
    CHECK(written.size() == 65);
    INFO(scene_golden::describeMismatch(fixture.text, written));
    CHECK(written == fixture.text);
}

TEST_CASE(
    "asset_meta: writeMetaText with NON-DEFAULT settings matches importer-settings.meta byte for byte (AM-i3, AC-10)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(IMPORTER_SETTINGS_FIXTURE);
    REQUIRE(fixture.ok);
    const std::optional<Guid> guid = parseGuid(FIXTURE_GUID_TEXT);
    REQUIRE(guid.has_value());
    const ImportSettings settings{0.01F, false, true, false};
    const std::string written = writeMetaText(*guid, settings);
    INFO(scene_golden::describeMismatch(fixture.text, written));
    CHECK(written == fixture.text);
    REQUIRE(written.size() >= 2);
    CHECK(written.back() == '\n');
    CHECK(written[written.size() - 2] != '\n');  // exactly ONE trailing newline
}

TEST_CASE("asset_meta: importer-settings.meta round-trips its settings exactly (AM-i4)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(IMPORTER_SETTINGS_FIXTURE);
    REQUIRE(fixture.ok);
    const MetaParseResult parsed = parseMeta(fixture.text);
    REQUIRE(parsed.guid.has_value());
    REQUIRE(parsed.importer.has_value());
    CHECK(parsed.importer->settings == ImportSettings{0.01F, false, true, false});
}

TEST_CASE(
    "asset_meta: parseMeta on a VALID importer block returns guid, importer and empty unknownKeys (AM-i5, AC-11)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(IMPORTER_SETTINGS_FIXTURE);
    REQUIRE(fixture.ok);
    const MetaParseResult parsed = parseMeta(fixture.text);
    REQUIRE(parsed.guid.has_value());
    REQUIRE(parsed.importer.has_value());
    CHECK(parsed.importer->name == "gltf");
    CHECK(parsed.importer->version == 2);  // task 3.5.1: GLTF_IMPORTER_VERSION 1 -> 2 (the importSkins gate)
    CHECK(parsed.importer->settings == ImportSettings{0.01F, false, true, false});
    CHECK(parsed.unknownKeys.empty());
    CHECK(parsed.importerMessage.empty());
}

TEST_CASE("asset_meta: parseMeta with NO importer key leaves it disengaged, with no message (AM-i6, AC-13)") {
    const MetaParseResult result = parseMeta(R"({"version": 1, "guid": ")" + std::string(FIXTURE_GUID_TEXT) + "\"}");
    REQUIRE(result.guid.has_value());
    CHECK_FALSE(result.importer.has_value());
    CHECK(result.importerMessage.empty());
}

TEST_CASE("asset_meta: a non-object importer block never invalidates the identity (AM-i7, AC-12)") {
    const MetaParseResult result =
        parseMeta(R"({"version": 1, "guid": ")" + std::string(FIXTURE_GUID_TEXT) + R"(", "importer": 5})");
    REQUIRE(result.guid.has_value());
    CHECK(result.error == MetaError::None);
    CHECK_FALSE(result.importer.has_value());
    CHECK_FALSE(result.importerMessage.empty());
}

TEST_CASE("asset_meta: an importer block missing \"name\" never invalidates the identity (AM-i8, AC-12)") {
    const MetaParseResult result =
        parseMeta(R"({"version": 1, "guid": ")" + std::string(FIXTURE_GUID_TEXT) + R"(", "importer": {}})");
    REQUIRE(result.guid.has_value());
    CHECK(result.error == MetaError::None);
    CHECK_FALSE(result.importer.has_value());
    CHECK_FALSE(result.importerMessage.empty());
}

TEST_CASE("asset_meta: a wrong-typed \"scale\" never invalidates the identity (AM-i9, AC-12)") {
    const std::string text = R"({"version": 1, "guid": ")" + std::string(FIXTURE_GUID_TEXT) +
                             R"(", "importer": {"name": "gltf", "version": 1, "settings": )"
                             R"({"scale": "big", "importMaterials": true, "importAnimations": true, )"
                             R"("importSkins": true}}})";
    const MetaParseResult result = parseMeta(text);
    REQUIRE(result.guid.has_value());
    CHECK(result.error == MetaError::None);
    CHECK_FALSE(result.importer.has_value());
    CHECK_FALSE(result.importerMessage.empty());
}

TEST_CASE("asset_meta: a non-finite \"scale\" never invalidates the identity (AM-i10, AC-12, E14)") {
    const std::string text = R"({"version": 1, "guid": ")" + std::string(FIXTURE_GUID_TEXT) +
                             R"(", "importer": {"name": "gltf", "version": 1, "settings": )"
                             R"({"scale": 1e400, "importMaterials": true, "importAnimations": true, )"
                             R"("importSkins": true}}})";
    const MetaParseResult result = parseMeta(text);
    REQUIRE(result.guid.has_value());
    CHECK(result.error == MetaError::None);
    CHECK_FALSE(result.importer.has_value());
    CHECK_FALSE(result.importerMessage.empty());
}

TEST_CASE("asset_meta: an unknown key inside settings is a DOTTED PATH, and the block still engages (AM-i11, AC-14)") {
    const std::string text = R"({"version": 1, "guid": ")" + std::string(FIXTURE_GUID_TEXT) +
                             R"(", "importer": {"name": "gltf", "version": 1, "settings": )"
                             R"({"scale": 1, "importMaterials": true, "importAnimations": true, )"
                             R"("importSkins": true, "foo": 1}}})";
    const MetaParseResult result = parseMeta(text);
    REQUIRE(result.guid.has_value());
    REQUIRE(result.importer.has_value());
    REQUIRE(result.unknownKeys.size() == 1);
    CHECK(result.unknownKeys[0] == "importer.settings.foo");
}

TEST_CASE(
    "asset_meta: an unknown key at the root, alongside a VALID importer block, is reported alone (AM-i12, AC-14)") {
    const std::string text = R"({"version": 1, "guid": ")" + std::string(FIXTURE_GUID_TEXT) +
                             R"(", "importer": {"name": "gltf", "version": 1, "settings": )"
                             R"({"scale": 1, "importMaterials": true, "importAnimations": true, )"
                             R"("importSkins": true}}, "userData": 1})";
    const MetaParseResult result = parseMeta(text);
    REQUIRE(result.guid.has_value());
    REQUIRE(result.importer.has_value());
    REQUIRE(result.unknownKeys.size() == 1);
    CHECK(result.unknownKeys[0] == "userData");
}

TEST_CASE("asset_meta: the EXISTING unknown-keys.meta fixture's behaviour is unchanged by this task (AM-i13)") {
    // unknown-keys.meta's "importer" value is a bare STRING ("texture"), not an object -- MALFORMED,
    // not valid -- so, per AC-11's own scope ("a VALID importer block"), it is reported exactly as it
    // was before this task: as an ordinary unrecognised root key. This is the same assertion AG3
    // already makes; it is repeated here under its own id as the direct proof that landing the
    // importer block did not regress a fixture this task did not touch.
    const scene_golden::FileBytes fixture = scene_golden::readBytes(UNKNOWN_KEYS_FIXTURE);
    REQUIRE(fixture.ok);
    const MetaParseResult parsed = parseMeta(fixture.text);
    REQUIRE(parsed.guid.has_value());
    CHECK_FALSE(parsed.importer.has_value());
    CHECK_FALSE(parsed.importerMessage.empty());
    REQUIRE(parsed.unknownKeys.size() == 2);
    CHECK(parsed.unknownKeys[0] == "importer");
    CHECK(parsed.unknownKeys[1] == "userData");
}

TEST_CASE("asset_meta: importer-settings.meta is forward-compat SHAPED for an older build (AM-i14, AC-15, R6)") {
    // AC-15's real claim -- that a binary built at dc4064a accepts this file -- cannot be proven by
    // rebuilding an old binary inside this test (R6, stated honestly rather than guessed). What IS
    // provable: the fixture's own JSON SHAPE is exactly what an older parseMeta (one with no notion of
    // "importer" at all) reads the GUID from correctly and reports as ONE unknown-key warning.
    const scene_golden::FileBytes fixture = scene_golden::readBytes(IMPORTER_SETTINGS_FIXTURE);
    REQUIRE(fixture.ok);
    const JsonParseResult parsed = parseJson(fixture.text);
    REQUIRE(parsed.value.has_value());
    const JsonValue& root = *parsed.value;
    REQUIRE(root.isObject());
    const JsonValue* const version = root.find("version");
    REQUIRE(version != nullptr);
    CHECK(version->asU64() == 1);
    std::vector<std::string> otherKeys;
    for (const JsonMember& member : root.members()) {
        if (member.key != "version" && member.key != "guid") {
            otherKeys.push_back(member.key);
        }
    }
    REQUIRE(otherKeys.size() == 1);
    CHECK(otherKeys[0] == "importer");
}

// =====================================================================================================
// task 3.2.2: writeMetaText's identity becomes CALLER-SUPPLIED (a third hard-coded-GLTF_IMPORTER_NAME
// site the plan itself did not name, found while implementing Step 10 -- ModelImportSession::
// applySettings() would otherwise have written "name": "gltf" into an .fbx's own sidecar). The two new
// TRAILING, DEFAULTED parameters keep every EXISTING two-argument call in THIS file (AM-i1 through
// AM-i14, all unedited above) byte-identical -- proof that the fix is additive, not a rewrite.
// =====================================================================================================

TEST_CASE(
    "asset_meta: writeMetaText with the FBX identity emits \"name\": \"fbx\", the SAME four settings "
    "keys in the SAME order as a glTF sidecar, and round-trips (AM-i15, AC-17)") {
    const std::optional<Guid> guid = parseGuid(FBX_FIXTURE_GUID_TEXT);
    REQUIRE(guid.has_value());
    const ImportSettings settings{0.01F, true, false, true};
    const std::string fbxText = writeMetaText(*guid, settings, "fbx", 1);
    const std::string gltfText = writeMetaText(*guid, settings);  // the identity DEFAULTS to glTF

    const JsonParseResult fbxParsed = parseJson(fbxText);
    REQUIRE(fbxParsed.value.has_value());
    const JsonValue* const fbxImporter = fbxParsed.value->find("importer");
    REQUIRE(fbxImporter != nullptr);
    const JsonValue* const fbxName = fbxImporter->find("name");
    REQUIRE(fbxName != nullptr);
    CHECK(fbxName->asString() == "fbx");
    const JsonValue* const fbxVersion = fbxImporter->find("version");
    REQUIRE(fbxVersion != nullptr);
    CHECK(fbxVersion->asU64() == 1);

    const JsonParseResult gltfParsed = parseJson(gltfText);
    REQUIRE(gltfParsed.value.has_value());
    const JsonValue* const gltfImporter = gltfParsed.value->find("importer");
    REQUIRE(gltfImporter != nullptr);
    const JsonValue* const gltfSettings = gltfImporter->find("settings");
    REQUIRE(gltfSettings != nullptr);
    const JsonValue* const fbxSettings = fbxImporter->find("settings");
    REQUIRE(fbxSettings != nullptr);
    // The SAME four keys, in the SAME order, whichever identity wrote the block -- the writer's SHAPE
    // does not depend on which importer produced it, only its NAME/VERSION do.
    std::vector<std::string> fbxKeys;
    std::vector<std::string> gltfKeys;
    for (const JsonMember& m : fbxSettings->members()) {
        fbxKeys.push_back(m.key);
    }
    for (const JsonMember& m : gltfSettings->members()) {
        gltfKeys.push_back(m.key);
    }
    CHECK(fbxKeys == gltfKeys);

    // Re-parsing yields the SAME settings the caller wrote, regardless of the identity attached.
    const MetaParseResult reparsed = parseMeta(fbxText);
    REQUIRE(reparsed.guid.has_value());
    CHECK(*reparsed.guid == *guid);
    REQUIRE(reparsed.importer.has_value());
    CHECK(reparsed.importer->name == "fbx");
    CHECK(reparsed.importer->version == 1);
    CHECK(reparsed.importer->settings == settings);
}

TEST_CASE("asset_meta: tests/fixtures/assets/importer-fbx.meta parses to exactly its settings (AM-i16)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(IMPORTER_FBX_FIXTURE);
    REQUIRE(fixture.ok);
    const MetaParseResult parsed = parseMeta(fixture.text);
    REQUIRE(parsed.guid.has_value());
    CHECK(formatGuid(*parsed.guid) == FBX_FIXTURE_GUID_TEXT);
    REQUIRE(parsed.importer.has_value());
    CHECK(parsed.importer->name == "fbx");
    CHECK(parsed.importer->version == 1);
    CHECK(parsed.importer->settings == ImportSettings{0.01F, true, false, true});
    CHECK(parsed.unknownKeys.empty());
    CHECK(parsed.importerMessage.empty());
}

TEST_CASE(
    "asset_meta: writeMetaText(guid, ImportSettings{}) is STILL byte-identical to the one-argument "
    "overload and to minimal.meta's 65-byte fixpoint, with the identity DEFAULTED (AM-i17, AC-16)") {
    // D7's omit-when-default branch never reaches the "name"/"version" write at all for
    // ImportSettings{} -- so an EXPLICIT non-default identity, passed alongside DEFAULT settings, must
    // still produce the identical 65 bytes AM-i2 already pins. This is the evidence the new parameters
    // are genuinely inert on the path every existing project's minimal.meta already depends on.
    const std::optional<Guid> guid = parseGuid(FIXTURE_GUID_TEXT);
    REQUIRE(guid.has_value());
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MINIMAL_FIXTURE);
    REQUIRE(fixture.ok);
    const std::string viaOneArg = writeMetaText(*guid);
    const std::string viaDefaultIdentity = writeMetaText(*guid, ImportSettings{});
    const std::string viaExplicitFbxIdentity = writeMetaText(*guid, ImportSettings{}, "fbx", 99);
    CHECK(viaOneArg.size() == 65);
    CHECK(viaOneArg == fixture.text);
    CHECK(viaDefaultIdentity == viaOneArg);
    CHECK(viaExplicitFbxIdentity == viaOneArg);  // the identity is UNOBSERVABLE when settings are default
}

TEST_CASE(
    "asset_meta: importer-settings.meta (name \"gltf\") parses UNCHANGED -- the second registered name "
    "did not disturb the first (AM-i18, AC-18)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(IMPORTER_SETTINGS_FIXTURE);
    REQUIRE(fixture.ok);
    const MetaParseResult parsed = parseMeta(fixture.text);
    REQUIRE(parsed.guid.has_value());
    REQUIRE(parsed.importer.has_value());
    CHECK(parsed.importer->name == "gltf");
    CHECK(parsed.importer->version == 2);  // task 3.5.1: GLTF_IMPORTER_VERSION 1 -> 2 (the importSkins gate)
    CHECK(parsed.importer->settings == ImportSettings{0.01F, false, true, false});
    CHECK(parsed.unknownKeys.empty());
    CHECK(parsed.importerMessage.empty());
}

TEST_CASE(
    "asset_meta: an unrecognised importer.name still returns the GUID and the settings, with no error "
    "-- the name is recorded information, never a validated enum (AM-i19, AC-19)") {
    const std::string text = R"({"version": 1, "guid": ")" + std::string(FIXTURE_GUID_TEXT) +
                             R"(", "importer": {"name": "wavefront-obj-3.2.3", "version": 7, )"
                             R"("settings": {"scale": 2.0, "importMaterials": true, "importAnimations": true, )"
                             R"("importSkins": true}}})";
    const MetaParseResult result = parseMeta(text);
    REQUIRE(result.guid.has_value());
    CHECK(result.error == MetaError::None);
    REQUIRE(result.importer.has_value());
    CHECK(result.importer->name == "wavefront-obj-3.2.3");
    CHECK(result.importer->version == 7);
    CHECK(result.importer->settings.scale == 2.0F);
    CHECK(result.importerMessage.empty());
}

TEST_CASE(
    "asset_meta: an EMPTY importer.name is the degenerate case of AM-i19 and parses the identical way "
    "(AM-i20)") {
    const std::string text = R"({"version": 1, "guid": ")" + std::string(FIXTURE_GUID_TEXT) +
                             R"(", "importer": {"name": "", "version": 1, )"
                             R"("settings": {"scale": 1.0, "importMaterials": true, "importAnimations": true, )"
                             R"("importSkins": true}}})";
    const MetaParseResult result = parseMeta(text);
    REQUIRE(result.guid.has_value());
    CHECK(result.error == MetaError::None);
    REQUIRE(result.importer.has_value());
    CHECK(result.importer->name.empty());
    CHECK(result.importerMessage.empty());
}

TEST_CASE(
    "asset_meta: writeMetaText with the OBJ identity records \"obj\", and minimal.meta's 65-byte "
    "fixpoint re-runs UNEDITED (AM-i21, task 3.2.3)") {
    // AM-i17's own shape, one format over: proves .obj's identity write path is genuinely exercised and
    // that it disturbs NOTHING on the DEFAULT-settings path every existing project's minimal.meta
    // already depends on (D7's omit-when-default branch never reaches "name"/"version" for
    // ImportSettings{}, regardless of which identity is passed alongside it).
    const std::optional<Guid> guid = parseGuid(FIXTURE_GUID_TEXT);
    REQUIRE(guid.has_value());
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MINIMAL_FIXTURE);
    REQUIRE(fixture.ok);
    const std::string viaOneArg = writeMetaText(*guid);
    const std::string viaExplicitObjIdentity = writeMetaText(*guid, ImportSettings{}, "obj", 1);
    CHECK(viaOneArg.size() == 65);
    CHECK(viaOneArg == fixture.text);            // minimal.meta's fixpoint, RE-RUN, UNEDITED
    CHECK(viaExplicitObjIdentity == viaOneArg);  // the identity is UNOBSERVABLE when settings are default

    // With NON-default settings, "obj"/1 IS observed, and round-trips through parseMeta.
    const std::string withScale = writeMetaText(*guid, ImportSettings{2.0F, true, true, true}, "obj", 1);
    const MetaParseResult parsed = parseMeta(withScale);
    REQUIRE(parsed.guid.has_value());
    REQUIRE(parsed.importer.has_value());
    CHECK(parsed.importer->name == "obj");
    CHECK(parsed.importer->version == 1);
    CHECK(parsed.importer->settings.scale == 2.0F);
}
