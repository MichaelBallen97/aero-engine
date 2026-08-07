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

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using engine::formatGuid;
using engine::Guid;
using engine::GuidGenerator;
using engine::parseGuid;
using engine::editor::AssetMetaState;
using engine::editor::assetNameForMeta;
using engine::editor::AssetPlanEntry;
using engine::editor::AssetPlanResult;
using engine::editor::AssetRecord;
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

// The fixture's pinned GUID, as text -- shared by AG3/AG4 so there is exactly one spelling of it.
constexpr std::string_view FIXTURE_GUID_TEXT = "a3f1c07e5b8d42198e6f0c3d7a2b4b92";

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
    // Exact bytes, never a substring test: a name that merely CONTAINS "Thumbs.db" is scannable.
    CHECK(isScannableAssetName("MyThumbs.dbFile.png"));
    CHECK(isScannableAssetName("Thumbs.db.bak"));
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
    for (const std::string_view name : NAMES) {
        CAPTURE(name);
        CHECK(isWatchableAssetName(name) == (isScannableAssetName(name) || isMetaFileName(name)));
    }
}
