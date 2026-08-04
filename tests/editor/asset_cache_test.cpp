// tests/editor/asset_cache_test.cpp -- task 3.1.2: the asset import cache index v1 format
// (parseAssetCache/writeAssetCacheText/importChangeLabel/AssetCacheIndex::find). A TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, and that is the point (D4/AC-17/INV-P5, the project_test.cpp / asset_meta_test.cpp
// precedent): asset_cache.hpp depends on nothing but <aero/core/content_hash.hpp> and
// <aero/core/guid.hpp>, so every case in this file must be PRESENT and PASSING in all three build
// configurations -- prove it with --list-test-cases, never with a skip. Tier-0: no GPU, no window,
// no ImGui context, no disk I/O at all -- parseAssetCache/writeAssetCacheText touch no filesystem;
// only the three golden-fixture reads below touch disk, through scene_golden::readBytes.
//
// This step lands ONLY the format's coverage (IC1-IC33, IG1-IG5). planImports/commitImports/
// planReattachments are declared in asset_cache.hpp but defined in Steps 6/7 -- this file calls
// none of them.
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/asset_cache.hpp>

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using engine::ContentHash;
using engine::formatGuid;
using engine::Guid;
using engine::parseGuid;
using engine::editor::AssetCacheEntry;
using engine::editor::AssetCacheIndex;
using engine::editor::AssetCacheParseResult;
using engine::editor::CacheLoadOutcome;
using engine::editor::ImportChange;
using engine::editor::importChangeLabel;
using engine::editor::MAX_DEPENDENCIES_PER_ENTRY;
using engine::editor::parseAssetCache;
using engine::editor::writeAssetCacheText;

namespace {

constexpr std::string_view MINIMAL_FIXTURE = AERO_ASSET_FIXTURES_DIR "/cache-minimal.json";
constexpr std::string_view DEPENDENCIES_FIXTURE = AERO_ASSET_FIXTURES_DIR "/cache-dependencies.json";
constexpr std::string_view DAMAGED_FIXTURE = AERO_ASSET_FIXTURES_DIR "/cache-damaged.json";

// A single "otherwise fully valid" entry's GUID/hash literals, shared by every per-entry-drop case
// below so there is exactly one spelling of "a valid entry" to compare a malformed variant against.
constexpr std::string_view GOOD_GUID = "a3f1c07e5b8d42198e6f0c3d7a2b4b92";
constexpr std::string_view GOOD_CONTENT_HASH = "1111111111111111ffffffffffffffff";
constexpr std::string_view GOOD_META_HASH = "2222222222222222eeeeeeeeeeeeeeee";

}  // namespace

// =====================================================================================================
// IC -- the format
// =====================================================================================================

// ---- parse success ----------------------------------------------------------------------------------

TEST_CASE("asset_cache: parseAssetCache succeeds on all three committed fixtures (IC1)") {
    for (const std::string_view fixture : {MINIMAL_FIXTURE, DEPENDENCIES_FIXTURE, DAMAGED_FIXTURE}) {
        const scene_golden::FileBytes bytes = scene_golden::readBytes(fixture);
        REQUIRE(bytes.ok);
        const AssetCacheParseResult result = parseAssetCache(bytes.text);
        CHECK(result.outcome == CacheLoadOutcome::Ok);
        // The steady-state, every-scan-in-practice case: nowhere NEAR MAX_CACHE_ENTRIES, so
        // `truncated` stays false. The `true` branch is a documented, deliberate coverage gap --
        // see the comment where IC25 used to be, above the MAX_DEPENDENCIES_PER_ENTRY case.
        CHECK_FALSE(result.truncated);
    }
}

// ---- envelope discard reasons, message text asserted VERBATIM (docs/09 §6.9) -----------------------

TEST_CASE("asset_cache: parseAssetCache discards unparseable JSON (IC2)") {
    const AssetCacheParseResult result = parseAssetCache("{not json");
    CHECK(result.outcome == CacheLoadOutcome::Discarded);
    CHECK_FALSE(result.discardReason.empty());
    CHECK(result.index.entries.empty());
}

TEST_CASE("asset_cache: parseAssetCache discards a non-object root (IC3)") {
    const AssetCacheParseResult result = parseAssetCache(R"([1, 2, 3])");
    CHECK(result.outcome == CacheLoadOutcome::Discarded);
    CHECK(result.discardReason == "asset cache root must be a JSON object (found array)");
    CHECK(result.index.entries.empty());
}

TEST_CASE("asset_cache: parseAssetCache discards a missing version (IC4)") {
    const AssetCacheParseResult result = parseAssetCache(R"({"hashAlgorithm": "murmur3-x64-128", "entries": []})");
    CHECK(result.outcome == CacheLoadOutcome::Discarded);
    CHECK(result.discardReason == "missing required key \"version\"");
}

TEST_CASE("asset_cache: parseAssetCache discards a non-integral version (IC5)") {
    const AssetCacheParseResult stringForm =
        parseAssetCache(R"({"version": "1", "hashAlgorithm": "murmur3-x64-128", "entries": []})");
    CHECK(stringForm.outcome == CacheLoadOutcome::Discarded);
    CHECK(stringForm.discardReason == "\"version\" must be an integer (found string)");

    const AssetCacheParseResult floatForm =
        parseAssetCache(R"({"version": 1.5, "hashAlgorithm": "murmur3-x64-128", "entries": []})");
    CHECK(floatForm.outcome == CacheLoadOutcome::Discarded);
    CHECK(floatForm.discardReason == "\"version\" must be an integer (found \"1.5\")");
}

TEST_CASE("asset_cache: parseAssetCache discards an unsupported version (IC6)") {
    const AssetCacheParseResult result =
        parseAssetCache(R"({"version": 2, "hashAlgorithm": "murmur3-x64-128", "entries": []})");
    CHECK(result.outcome == CacheLoadOutcome::Discarded);
    CHECK(result.discardReason == "unsupported asset cache format version 2 (this build reads version 1)");
}

TEST_CASE("asset_cache: parseAssetCache discards a missing hashAlgorithm (IC7)") {
    const AssetCacheParseResult result = parseAssetCache(R"({"version": 1, "entries": []})");
    CHECK(result.outcome == CacheLoadOutcome::Discarded);
    CHECK(result.discardReason == "missing required key \"hashAlgorithm\"");
}

TEST_CASE("asset_cache: parseAssetCache discards a non-string hashAlgorithm (IC8)") {
    const AssetCacheParseResult result = parseAssetCache(R"({"version": 1, "hashAlgorithm": 5, "entries": []})");
    CHECK(result.outcome == CacheLoadOutcome::Discarded);
    CHECK(result.discardReason == "\"hashAlgorithm\" must be a string (found number)");
}

TEST_CASE("asset_cache: parseAssetCache discards a wrong hashAlgorithm (IC9)") {
    const AssetCacheParseResult result = parseAssetCache(R"({"version": 1, "hashAlgorithm": "sha256", "entries": []})");
    CHECK(result.outcome == CacheLoadOutcome::Discarded);
    CHECK(result.discardReason == "unsupported hash algorithm \"sha256\" (this build writes \"murmur3-x64-128\")");
}

TEST_CASE("asset_cache: parseAssetCache discards a missing entries (IC10)") {
    const AssetCacheParseResult result = parseAssetCache(R"({"version": 1, "hashAlgorithm": "murmur3-x64-128"})");
    CHECK(result.outcome == CacheLoadOutcome::Discarded);
    CHECK(result.discardReason == "missing required key \"entries\"");
}

TEST_CASE("asset_cache: parseAssetCache discards a non-array entries (IC11)") {
    const AssetCacheParseResult result =
        parseAssetCache(R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": {}})");
    CHECK(result.outcome == CacheLoadOutcome::Discarded);
    CHECK(result.discardReason == "\"entries\" must be an array (found object)");
}

TEST_CASE("asset_cache: version is validated BEFORE hashAlgorithm and entries (IC12, AC-14, seed S8)") {
    // Missing ALL THREE root keys must still report the VERSION error, not one of the other two.
    const AssetCacheParseResult result = parseAssetCache("{}");
    CHECK(result.outcome == CacheLoadOutcome::Discarded);
    CHECK(result.discardReason == "missing required key \"version\"");
}

TEST_CASE("asset_cache: an unsupported version AND a wrong hashAlgorithm together report the version error (IC13)") {
    const AssetCacheParseResult result = parseAssetCache(R"({"version": 2, "hashAlgorithm": "sha256", "entries": []})");
    CHECK(result.outcome == CacheLoadOutcome::Discarded);
    CHECK(result.discardReason == "unsupported asset cache format version 2 (this build reads version 1)");
}

// ---- per-entry drop: envelope survives (Ok), the malformed element is dropped and counted ----------

TEST_CASE("asset_cache: a non-object entry element is dropped (IC14)") {
    const AssetCacheParseResult result =
        parseAssetCache(R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": ["not-an-object"]})");
    CHECK(result.outcome == CacheLoadOutcome::Ok);
    CHECK(result.droppedEntries == 1);
    CHECK(result.index.entries.empty());
}

TEST_CASE("asset_cache: every malformed-guid shape drops the entry (IC15, AC-16)") {
    // missing / non-string / short / non-hex / nil -- one otherwise-fully-valid entry each.
    const std::array<std::string_view, 5> docs = {
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"path": "a.png", "size": 10, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}]})",
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": 123, "path": "a.png", "size": 10, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}]})",
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "abc", "path": "a.png", "size": 10, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}]})",
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "g3f1c07e5b8d42198e6f0c3d7a2b4b92", "path": "a.png", "size": 10, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}]})",
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "00000000000000000000000000000000", "path": "a.png", "size": 10, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}]})",
    };
    for (const std::string_view doc : docs) {
        const AssetCacheParseResult result = parseAssetCache(doc);
        CHECK(result.outcome == CacheLoadOutcome::Ok);
        CHECK(result.droppedEntries == 1);
        CHECK(result.index.entries.empty());
    }
}

TEST_CASE("asset_cache: a missing path drops the entry (IC16)") {
    const AssetCacheParseResult result = parseAssetCache(
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92", "size": 10, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}]})");
    CHECK(result.outcome == CacheLoadOutcome::Ok);
    CHECK(result.droppedEntries == 1);
    CHECK(result.index.entries.empty());
}

TEST_CASE("asset_cache: a non-integral size drops the entry (IC17)") {
    const AssetCacheParseResult result = parseAssetCache(
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92", "path": "a.png", "size": 1.5, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}]})");
    CHECK(result.outcome == CacheLoadOutcome::Ok);
    CHECK(result.droppedEntries == 1);
    CHECK(result.index.entries.empty());
}

TEST_CASE("asset_cache: a non-integral mtime drops the entry (IC18)") {
    const AssetCacheParseResult result = parseAssetCache(
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92", "path": "a.png", "size": 10, "mtime": "five", "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}]})");
    CHECK(result.outcome == CacheLoadOutcome::Ok);
    CHECK(result.droppedEntries == 1);
    CHECK(result.index.entries.empty());
}

TEST_CASE("asset_cache: a malformed contentHash drops the entry (IC19)") {
    const AssetCacheParseResult result = parseAssetCache(
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92", "path": "a.png", "size": 10, "mtime": 5, "contentHash": "zz11111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}]})");
    CHECK(result.outcome == CacheLoadOutcome::Ok);
    CHECK(result.droppedEntries == 1);
    CHECK(result.index.entries.empty());
}

TEST_CASE("asset_cache: a malformed metaHash drops the entry (IC20)") {
    const AssetCacheParseResult result = parseAssetCache(
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92", "path": "a.png", "size": 10, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "zz22222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}]})");
    CHECK(result.outcome == CacheLoadOutcome::Ok);
    CHECK(result.droppedEntries == 1);
    CHECK(result.index.entries.empty());
}

TEST_CASE("asset_cache: a dependencies element that is not a 32-hex string drops the entry (IC21)") {
    const AssetCacheParseResult badElement = parseAssetCache(
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92", "path": "a.png", "size": 10, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": ["not-a-guid"], "missing": 0}]})");
    CHECK(badElement.outcome == CacheLoadOutcome::Ok);
    CHECK(badElement.droppedEntries == 1);
    CHECK(badElement.index.entries.empty());

    // A `dependencies` of the wrong KIND (not an array at all) is the same class of failure.
    const AssetCacheParseResult wrongKind = parseAssetCache(
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92", "path": "a.png", "size": 10, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": "oops", "missing": 0}]})");
    CHECK(wrongKind.outcome == CacheLoadOutcome::Ok);
    CHECK(wrongKind.droppedEntries == 1);
    CHECK(wrongKind.index.entries.empty());
}

TEST_CASE("asset_cache: contentHash/metaHash of 32 zeros are ACCEPTED, never dropped (IC22, A4)") {
    const AssetCacheParseResult result = parseAssetCache(
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92", "path": "a.png", "size": 10, "mtime": 5, "contentHash": "00000000000000000000000000000000", "metaHash": "00000000000000000000000000000000", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}]})");
    CHECK(result.outcome == CacheLoadOutcome::Ok);
    CHECK(result.droppedEntries == 0);
    REQUIRE(result.index.entries.size() == 1);
    CHECK(result.index.entries[0].contentHash == ContentHash{});
    CHECK_FALSE(result.index.entries[0].contentHash.valid());
    CHECK(result.index.entries[0].metaHash == ContentHash{});
}

TEST_CASE("asset_cache: a duplicate guid keeps the FIRST in document order (IC23, AC-16, seed S9)") {
    const AssetCacheParseResult result = parseAssetCache(
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92", "path": "first.png", "size": 10, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}, {"guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92", "path": "second.png", "size": 10, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0}]})");
    CHECK(result.outcome == CacheLoadOutcome::Ok);
    CHECK(result.droppedEntries == 1);
    REQUIRE(result.index.entries.size() == 1);
    CHECK(result.index.entries[0].path == "first.png");
}

TEST_CASE("asset_cache: unknown keys at BOTH levels are ignored SILENTLY (IC24, AC-17)") {
    const AssetCacheParseResult result = parseAssetCache(
        R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92", "path": "a.png", "size": 10, "mtime": 5, "contentHash": "1111111111111111ffffffffffffffff", "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "importer": "", "importerVersion": 0, "dependencies": [], "missing": 0, "extra": "ignored"}], "unknownRoot": true})");
    CHECK(result.outcome == CacheLoadOutcome::Ok);
    CHECK(result.droppedEntries == 0);
    REQUIRE(result.index.entries.size() == 1);
    CHECK(formatGuid(result.index.entries[0].guid) == GOOD_GUID);
    // AssetCacheParseResult has no field that could even NAME an unknown key -- unlike asset_meta's
    // MetaParseResult::unknownKeys, this format's policy is inverted (D7/§6.3): no report at all.
}

// IC25 -- MAX_CACHE_ENTRIES truncation -- DELIBERATELY NOT EXERCISED AT THE REAL CONSTANT. Measured
// at implementation time (a standalone ASan/Debug harness, this machine, this compiler): parsing a
// document built from the smallest input that can reach MAX_CACHE_ENTRIES=200000 -- 200 001 tiny but
// syntactically complete, unique, valid entries -- took ~10.8s, five times the plan's ~2s ASan-lane
// budget. The cost is LINEAR in entry count, not quadratic (5000 -> 0.28s, 20000 -> 1.05s,
// 50000 -> 2.71s, 100000 -> 5.42s, 200001 -> 10.81s) -- this is not an algorithmic bug in
// parseAssetCache, it is the inherent per-node cost of this tree's JsonValue DOM (one
// std::variant<std::monostate,bool,JsonNumber,std::string,vector<JsonValue>,vector<JsonMember>> plus
// several heap allocations per entry) under ASan's redzones, multiplied by 200 001.
//
// Unlike 3.1.1's AD23 (which substituted a SMALLER, independent cap -- MAX_ENTRIES_PER_DIRECTORY --
// that sets the identical `truncated` bit through a completely different code path, `listDirectory`),
// this format has no second, cheaper truncation source to redirect through: MAX_CACHE_ENTRIES is the
// ONLY thing that can set `truncated` in parseAssetCache. There is no substitute mechanism.
//
// Per plan A9's documented fallback ("record the budget branch as a deliberate coverage gap ... keep
// every flag plumbed ... rather than lowering the constant"): MAX_CACHE_ENTRIES stays 200000 (a
// constant a test changes is a constant no test pins -- 3.1.1's AD23/AD9 rule, unchanged here), and
// the `truncated == true` branch is a GENUINE, OPEN COVERAGE GAP, not a passing test in disguise.
// Recorded here for docs/10-engineering-log.md to carry forward. The `truncated == false` branch (the
// steady-state, every-scan-in-practice case) IS covered -- see IC1's explicit assertion below.

TEST_CASE("asset_cache: MAX_DEPENDENCIES_PER_ENTRY excess is dropped, counted, and the entry SURVIVES (IC26, E25)") {
    std::string deps;
    const std::size_t total = MAX_DEPENDENCIES_PER_ENTRY + 5;
    for (std::size_t i = 0; i < total; ++i) {
        if (i != 0) {
            deps += ',';
        }
        const Guid dependency{static_cast<std::uint64_t>(i) + 1, 1};
        deps += '"';
        deps += formatGuid(dependency);
        deps += '"';
    }
    std::string doc = R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [{"guid": ")";
    doc += GOOD_GUID;
    doc += R"(", "path": "a.png", "size": 10, "mtime": 5, "contentHash": ")";
    doc += GOOD_CONTENT_HASH;
    doc += R"(", "metaHash": ")";
    doc += GOOD_META_HASH;
    doc += R"(", "importer": "", "importerVersion": 0, "dependencies": [)";
    doc += deps;
    doc += R"(], "missing": 0}]})";

    const AssetCacheParseResult result = parseAssetCache(doc);
    CHECK(result.outcome == CacheLoadOutcome::Ok);
    CHECK(result.droppedEntries == 0);
    REQUIRE(result.index.entries.size() == 1);
    CHECK(result.index.entries[0].dependencies.size() == MAX_DEPENDENCIES_PER_ENTRY);
    CHECK(result.droppedDependencies == 5);
}

// ---- writeAssetCacheText: byte-exact for a hand-built index, and both docs/09 §1 round trips -------

namespace {
// Computed by hand-applying JsonWriter's exact state machine (json_writer.cpp) to a single entry
// with a negative mtime, a nil metaHash and a two-element dependencies array -- so this one literal
// exercises every branch cache-minimal.json's single, empty-dependencies entry does not (a non-empty
// array, negative-number formatting, a nil hash written as 32 zeros).
constexpr std::string_view HAND_BUILT_CANONICAL_TEXT =
    "{\n"
    "  \"version\": 1,\n"
    "  \"hashAlgorithm\": \"murmur3-x64-128\",\n"
    "  \"entries\": [\n"
    "    {\n"
    "      \"guid\": \"fedcba98765432100123456789abcdef\",\n"
    "      \"path\": \"models/car.gltf\",\n"
    "      \"size\": 123456789,\n"
    "      \"mtime\": -42,\n"
    "      \"contentHash\": \"11111111222222223333333344444444\",\n"
    "      \"metaHash\": \"00000000000000000000000000000000\",\n"
    "      \"importer\": \"gltf\",\n"
    "      \"importerVersion\": 7,\n"
    "      \"dependencies\": [\n"
    "        \"aaaaaaaaaaaaaaaabbbbbbbbbbbbbbbb\",\n"
    "        \"ccccccccccccccccdddddddddddddddd\"\n"
    "      ],\n"
    "      \"missing\": 2\n"
    "    }\n"
    "  ]\n"
    "}\n";

AssetCacheIndex handBuiltIndex() {
    AssetCacheEntry entry;
    entry.guid = Guid{0xFEDCBA9876543210ULL, 0x0123456789ABCDEFULL};
    entry.path = "models/car.gltf";
    entry.size = 123456789ULL;
    entry.mtime = -42;
    entry.contentHash = ContentHash{0x1111111122222222ULL, 0x3333333344444444ULL};
    entry.metaHash = ContentHash{};  // nil -- writes as 32 zeros (A4)
    entry.importer = "gltf";
    entry.importerVersion = 7;
    entry.dependencies = {Guid{0xAAAAAAAAAAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL},
                          Guid{0xCCCCCCCCCCCCCCCCULL, 0xDDDDDDDDDDDDDDDDULL}};
    entry.missing = 2;
    AssetCacheIndex index;
    index.entries.push_back(std::move(entry));
    return index;
}
}  // namespace

TEST_CASE("asset_cache: writeAssetCacheText is byte-exact for a hand-built index (IC27)") {
    const std::string written = writeAssetCacheText(handBuiltIndex());
    INFO(scene_golden::describeMismatch(HAND_BUILT_CANONICAL_TEXT, written));
    CHECK(written == HAND_BUILT_CANONICAL_TEXT);
}

TEST_CASE(
    "asset_cache: round-trip guarantee 1 -- canonical text is byte-stable through parse -> write (IC28, docs/09 §1)") {
    const AssetCacheParseResult parsed = parseAssetCache(HAND_BUILT_CANONICAL_TEXT);
    CHECK(parsed.outcome == CacheLoadOutcome::Ok);
    const std::string written = writeAssetCacheText(parsed.index);
    INFO(scene_golden::describeMismatch(HAND_BUILT_CANONICAL_TEXT, written));
    CHECK(written == HAND_BUILT_CANONICAL_TEXT);
}

TEST_CASE(
    "asset_cache: round-trip guarantee 2 -- ANY successfully-parsed text writes idempotently (IC29, docs/09 §1)") {
    // Deliberately NON-canonical: compact, entry keys in a DIFFERENT order than the writer's fixed
    // one. Guarantee 2 does not claim the FIRST write reproduces this text -- only that a SECOND
    // write cycle reproduces the FIRST write's bytes exactly.
    constexpr std::string_view NON_CANONICAL =
        R"({"entries": [{"missing": 0, "dependencies": [], "guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92", "path": "a.png", "importerVersion": 0, "size": 10, "importer": "", "mtime": 5, "metaHash": "2222222222222222eeeeeeeeeeeeeeee", "contentHash": "1111111111111111ffffffffffffffff"}], "hashAlgorithm": "murmur3-x64-128", "version": 1})";

    const AssetCacheParseResult firstParse = parseAssetCache(NON_CANONICAL);
    REQUIRE(firstParse.outcome == CacheLoadOutcome::Ok);
    const std::string firstWrite = writeAssetCacheText(firstParse.index);

    const AssetCacheParseResult secondParse = parseAssetCache(firstWrite);
    REQUIRE(secondParse.outcome == CacheLoadOutcome::Ok);
    const std::string secondWrite = writeAssetCacheText(secondParse.index);

    INFO(scene_golden::describeMismatch(firstWrite, secondWrite));
    CHECK(firstWrite == secondWrite);
}

TEST_CASE("asset_cache: entries are sorted by GUID regardless of input order (IC30, AC-18, seed S11)") {
    AssetCacheIndex index;
    AssetCacheEntry high;
    high.guid = Guid{0x9999999999999999ULL, 0};
    high.path = "high.png";
    AssetCacheEntry low;
    low.guid = Guid{0x1111111111111111ULL, 0};
    low.path = "low.png";
    AssetCacheEntry mid;
    mid.guid = Guid{0x5555555555555555ULL, 0};
    mid.path = "mid.png";
    // Pushed HIGH, LOW, MID -- deliberately not sorted -- to prove the writer sorts regardless.
    index.entries = {high, low, mid};

    const std::string written = writeAssetCacheText(index);
    const std::size_t lowPos = written.find(formatGuid(low.guid));
    const std::size_t midPos = written.find(formatGuid(mid.guid));
    const std::size_t highPos = written.find(formatGuid(high.guid));
    REQUIRE(lowPos != std::string::npos);
    REQUIRE(midPos != std::string::npos);
    REQUIRE(highPos != std::string::npos);
    CHECK(lowPos < midPos);
    CHECK(midPos < highPos);
}

TEST_CASE("asset_cache: a 19-digit mtime and a UINT64_MAX size round-trip EXACTLY (IC31, AC-19/F1, seed S12)") {
    AssetCacheEntry entry;
    entry.guid = Guid{0x1234567890ABCDEFULL, 0xFEDCBA0987654321ULL};
    entry.path = "big.bin";
    entry.size = UINT64_MAX;
    entry.mtime = 1234567890123456789LL;  // 19 significant digits, > 2^53 (F1's whole point)
    entry.contentHash = ContentHash{1, 1};
    entry.metaHash = ContentHash{2, 2};
    AssetCacheIndex index;
    index.entries.push_back(entry);

    const std::string written = writeAssetCacheText(index);
    CHECK(written.find("\"size\": 18446744073709551615") != std::string::npos);
    CHECK(written.find("\"mtime\": 1234567890123456789") != std::string::npos);

    const AssetCacheParseResult parsed = parseAssetCache(written);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    REQUIRE(parsed.index.entries.size() == 1);
    CHECK(parsed.index.entries[0].size == UINT64_MAX);
    CHECK(parsed.index.entries[0].mtime == 1234567890123456789LL);
}

TEST_CASE("asset_cache: a negative mtime round-trips (IC32, AC-19)") {
    // Some filesystems and some epochs legitimately predate 1970 -- a negative mtime is not an error.
    AssetCacheEntry entry;
    entry.guid = Guid{1, 2};
    entry.path = "old.bin";
    entry.mtime = -86400;
    AssetCacheIndex index;
    index.entries.push_back(entry);

    const std::string written = writeAssetCacheText(index);
    CHECK(written.find("\"mtime\": -86400") != std::string::npos);

    const AssetCacheParseResult parsed = parseAssetCache(written);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    REQUIRE(parsed.index.entries.size() == 1);
    CHECK(parsed.index.entries[0].mtime == -86400);
}

// ---- importChangeLabel (A13) -------------------------------------------------------------------------

TEST_CASE("asset_cache: importChangeLabel returns all EIGHT exact strings (IC33, A13)") {
    CHECK(importChangeLabel(ImportChange::UpToDate) == "up to date");
    CHECK(importChangeLabel(ImportChange::New) == "new");
    CHECK(importChangeLabel(ImportChange::SourceChanged) == "changed");
    CHECK(importChangeLabel(ImportChange::MetaChanged) == "settings changed");
    CHECK(importChangeLabel(ImportChange::ImporterChanged) == "importer changed");
    CHECK(importChangeLabel(ImportChange::DependencyChanged) == "dependency changed");
    CHECK(importChangeLabel(ImportChange::Unhashable) == "unreadable");
    CHECK(importChangeLabel(ImportChange::NotHashed) == "not hashed");
}

// ---- AssetCacheIndex::find --------------------------------------------------------------------------

TEST_CASE("asset_cache: AssetCacheIndex::find hits, misses, and returns nullptr for a nil GUID (IC34)") {
    AssetCacheIndex index;
    AssetCacheEntry a;
    a.guid = Guid{1, 1};
    AssetCacheEntry b;
    b.guid = Guid{2, 2};
    index.entries = {a, b};  // already sorted -- find() does not sort, callers must (AC-18's contract)

    CHECK(index.find(Guid{1, 1}) == &index.entries[0]);
    CHECK(index.find(Guid{2, 2}) == &index.entries[1]);
    CHECK(index.find(Guid{3, 3}) == nullptr);  // a well-formed miss
    CHECK(index.find(Guid{}) == nullptr);      // the nil GUID: no entry is ever nil (INV-C1)
}

// =====================================================================================================
// IG -- the golden battery (fixtures under tests/fixtures/assets/, NO regeneration path whatsoever)
// =====================================================================================================

TEST_CASE("asset_cache: cache-minimal.json is a fixpoint under parse -> write (IG1, AC-18)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MINIMAL_FIXTURE);
    REQUIRE(fixture.ok);
    CHECK(scene_golden::hygieneComplaint(fixture.text).empty());
    const AssetCacheParseResult parsed = parseAssetCache(fixture.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    const std::string written = writeAssetCacheText(parsed.index);
    INFO(scene_golden::describeMismatch(fixture.text, written));
    CHECK(written == fixture.text);
}

TEST_CASE("asset_cache: a second parse -> write cycle on cache-minimal.json is byte-identical (IG2, AC-18)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MINIMAL_FIXTURE);
    REQUIRE(fixture.ok);
    const AssetCacheParseResult firstParse = parseAssetCache(fixture.text);
    REQUIRE(firstParse.outcome == CacheLoadOutcome::Ok);
    const std::string firstWrite = writeAssetCacheText(firstParse.index);

    const AssetCacheParseResult secondParse = parseAssetCache(firstWrite);
    REQUIRE(secondParse.outcome == CacheLoadOutcome::Ok);
    const std::string secondWrite = writeAssetCacheText(secondParse.index);

    INFO(scene_golden::describeMismatch(firstWrite, secondWrite));
    CHECK(firstWrite == secondWrite);
}

TEST_CASE("asset_cache: cache-damaged.json yields exactly one survivor, three drops, no warning of any kind (IG3)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(DAMAGED_FIXTURE);
    REQUIRE(fixture.ok);
    const AssetCacheParseResult parsed = parseAssetCache(fixture.text);
    CHECK(parsed.outcome == CacheLoadOutcome::Ok);
    CHECK(parsed.droppedEntries == 3);
    REQUIRE(parsed.index.entries.size() == 1);
    CHECK(formatGuid(parsed.index.entries[0].guid) == "a3f1c07e5b8d42198e6f0c3d7a2b4b92");
    CHECK(parsed.index.entries[0].path == "mesh.gltf");
    // AssetCacheParseResult carries no field ANY unknown key could be collected into -- there is
    // nothing left to assert "no warning" against beyond the type itself having no such member.
}

TEST_CASE("asset_cache: cache-dependencies.json's A->B->C edges and its dangling GUID (IG4, semantic)") {
    // Deliberately independent of IG1/IG2's byte comparison -- 2.5.2's S12 / 2.6.1's S9 / 3.1.1's AG4
    // lesson: a parse/write pair that both stopped handling a key agrees WITH ITSELF, and every
    // byte-only case passes the moment the fixture is regenerated from the buggy build. This reads
    // the fixture and compares to HARDCODED LITERALS, never to another product output.
    const scene_golden::FileBytes fixture = scene_golden::readBytes(DEPENDENCIES_FIXTURE);
    REQUIRE(fixture.ok);
    const AssetCacheParseResult parsed = parseAssetCache(fixture.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    REQUIRE(parsed.index.entries.size() == 3);

    const std::optional<Guid> material = parseGuid("a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1");  // material.mat
    const std::optional<Guid> albedo = parseGuid("b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2");    // albedo.png
    const std::optional<Guid> source = parseGuid("c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3");    // source.psd
    const std::optional<Guid> dangling = parseGuid("d4d4d4d4d4d4d4d4d4d4d4d4d4d4d4d4");  // no entry declares this
    REQUIRE(material.has_value());
    REQUIRE(albedo.has_value());
    REQUIRE(source.has_value());
    REQUIRE(dangling.has_value());

    const AssetCacheEntry* materialEntry = parsed.index.find(*material);
    const AssetCacheEntry* albedoEntry = parsed.index.find(*albedo);
    const AssetCacheEntry* sourceEntry = parsed.index.find(*source);
    REQUIRE(materialEntry != nullptr);
    REQUIRE(albedoEntry != nullptr);
    REQUIRE(sourceEntry != nullptr);

    CHECK(materialEntry->path == "material.mat");
    REQUIRE(materialEntry->dependencies.size() == 1);
    CHECK(materialEntry->dependencies[0] == *albedo);  // A -> B

    CHECK(albedoEntry->path == "albedo.png");
    REQUIRE(albedoEntry->dependencies.size() == 1);
    CHECK(albedoEntry->dependencies[0] == *source);  // B -> C

    CHECK(sourceEntry->path == "source.psd");
    REQUIRE(sourceEntry->dependencies.size() == 1);
    CHECK(sourceEntry->dependencies[0] == *dangling);  // C -> a GUID no entry declares
    CHECK(parsed.index.find(*dangling) == nullptr);    // confirming it really is dangling
}

TEST_CASE("asset_cache: cache-dependencies.json's raw bytes name version 1 and murmur3-x64-128 (IG5, semantic)") {
    // Read the RAW bytes and check them with plain string search -- never through parseAssetCache, so
    // this case cannot be fooled by a bug shared between the reader and the writer.
    const scene_golden::FileBytes fixture = scene_golden::readBytes(DEPENDENCIES_FIXTURE);
    REQUIRE(fixture.ok);
    CHECK(fixture.text.find("\"version\": 1,") != std::string::npos);
    CHECK(fixture.text.find("\"hashAlgorithm\": \"murmur3-x64-128\"") != std::string::npos);
}
