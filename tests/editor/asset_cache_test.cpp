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
// Coverage, measured from this file rather than carried forward from the plan: IC1-IC34 (the format),
// IG1-IG6 (the golden battery over the committed fixtures), IP1-IP26 and IP39-IP40 (planImports /
// commitImports), IP27-IP38 (planReattachments).
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
using engine::editor::commitImports;
using engine::editor::ImportChange;
using engine::editor::importChangeLabel;
using engine::editor::ImportInput;
using engine::editor::ImportPlanEntry;
using engine::editor::ImportPlanResult;
using engine::editor::MAX_CACHE_ENTRIES;
using engine::editor::MAX_DEPENDENCIES_PER_ENTRY;
using engine::editor::MISSING_SCAN_GRACE;
using engine::editor::OrphanMeta;
using engine::editor::parseAssetCache;
using engine::editor::planImports;
using engine::editor::planReattachments;
using engine::editor::ReattachCandidate;
using engine::editor::ReattachMatch;
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
        // `truncated` stays false. The `true` branch is covered by IC25 below, through the
        // `maxEntries` seam -- not by this case.
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

// IC25 -- the entry cap. The REAL constant is deliberately NOT reached: the smallest input that can
// reach MAX_CACHE_ENTRIES=200000 -- 200 001 tiny but syntactically complete, unique, valid entries --
// was measured at ~10.8s under ASan on this machine, five times the plan's ~2s ASan-lane budget. The
// cost is LINEAR in entry count, not quadratic (5000 -> 0.28s, 20000 -> 1.05s, 50000 -> 2.71s,
// 100000 -> 5.42s, 200001 -> 10.81s) -- not an algorithmic bug in parseAssetCache, but the inherent
// per-node cost of this tree's JsonValue DOM under ASan's redzones, multiplied by 200 001.
//
// The plan's R7 weighed exactly two options -- "document the gap" against "lower the constant" -- and
// considered no third. There is one: a SEAM. `maxEntries` is plan A9's defaulted parameter applied to
// this function (asset_cache.hpp states the rule at the declaration): MAX_CACHE_ENTRIES stays 200000,
// stays pinned at its own declaration and is what production exercises, while a test reaches the
// branch in microseconds. Nothing here rewrites a constant, so 3.1.1's AD23/AD9 rule ("a constant a
// test changes is a constant no test pins") is untouched.
//
// WHAT REMAINS AN OPEN GAP, and only this: `report.cacheTruncated = parsed.truncated` in
// asset_database.cpp's phase 3. `rescan` takes no entry cap and is not given one for a test's sake,
// so reaching that assignment still needs a real 200 001-entry file on disk -- the same ~10.8s plus
// the write. The flag's PRODUCER is covered below; its PROPAGATION into AssetScanReport is not, and
// no case in this tree asserts `AssetScanReport::cacheTruncated == true`.

namespace {
// N syntactically complete, unique, valid entries -- the cheapest input that can reach any cap.
std::string cacheDocWithEntries(std::size_t count) {
    std::string doc = R"({"version": 1, "hashAlgorithm": "murmur3-x64-128", "entries": [)";
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            doc += ',';
        }
        const Guid guid{static_cast<std::uint64_t>(i) + 1U, 7};
        doc += R"({"guid": ")";
        doc += formatGuid(guid);
        doc += R"(", "path": "a)";
        doc += std::to_string(i);
        doc += R"(.png", "size": 1, "mtime": 1, "contentHash": ")";
        doc += GOOD_CONTENT_HASH;
        doc += R"(", "metaHash": ")";
        doc += GOOD_META_HASH;
        doc += R"("})";
    }
    doc += "]}";
    return doc;
}
}  // namespace

TEST_CASE("asset_cache: exceeding the entry cap truncates; being exactly AT it does not (IC25, D10, AC-16)") {
    // Five entries, a cap of three: the first three are admitted, and the fourth trips the guard at
    // the TOP of the loop, which sets `truncated` and stops the walk.
    const AssetCacheParseResult over = parseAssetCache(cacheDocWithEntries(5), /*maxEntries=*/3);
    CHECK(over.outcome == CacheLoadOutcome::Ok);
    CHECK(over.truncated);
    CHECK(over.index.entries.size() == 3);
    CHECK(over.droppedEntries == 0);  // truncation is NOT a per-entry drop: a different counter entirely

    // Exactly at the cap: every entry is admitted and `truncated` stays false, because no further
    // element exists to reach the guard. The off-by-one this pins is the whole reason for the pair.
    const AssetCacheParseResult atLimit = parseAssetCache(cacheDocWithEntries(3), /*maxEntries=*/3);
    CHECK(atLimit.outcome == CacheLoadOutcome::Ok);
    CHECK_FALSE(atLimit.truncated);
    CHECK(atLimit.index.entries.size() == 3);

    // The DEFAULT overload admits all five -- proof the cap above came from the ARGUMENT and that
    // MAX_CACHE_ENTRIES itself is untouched and still what an unqualified call uses.
    const AssetCacheParseResult defaulted = parseAssetCache(cacheDocWithEntries(5));
    CHECK_FALSE(defaulted.truncated);
    CHECK(defaulted.index.entries.size() == 5);
    CHECK(MAX_CACHE_ENTRIES == 200000U);
}

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

TEST_CASE("asset_cache: cache-dependencies.json is a MULTI-ENTRY fixpoint, in GUID order (IG6, AC-18)") {
    // IG1/IG2 run over cache-minimal.json, which holds exactly ONE entry and is therefore a fixpoint
    // under ANY ordering whatsoever -- measured, not assumed: a writer sorting by `path` instead of by
    // `guid` leaves both of them green, and only IC30 catches it. This fixture holds THREE entries
    // whose path order (albedo.png, material.mat, source.psd) and GUID order (a1a1..., b2b2...,
    // c3c3...) DISAGREE, so AC-18's sort-by-GUID claim is carried by the golden tier too, against real
    // committed bytes rather than a hand-built index.
    const scene_golden::FileBytes fixture = scene_golden::readBytes(DEPENDENCIES_FIXTURE);
    REQUIRE(fixture.ok);
    CHECK(scene_golden::hygieneComplaint(fixture.text).empty());
    const AssetCacheParseResult parsed = parseAssetCache(fixture.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    REQUIRE(parsed.index.entries.size() == 3);
    const std::string written = writeAssetCacheText(parsed.index);
    INFO(scene_golden::describeMismatch(fixture.text, written));
    CHECK(written == fixture.text);
}

// =====================================================================================================
// IP -- planImports / commitImports, all PURE, from std::vector literals, no disk, no clock (task
// 3.1.2 Step 6)
// =====================================================================================================

namespace {

Guid guidOf(std::uint64_t n) { return Guid{n, n}; }
ContentHash hashOf(std::uint64_t n) { return ContentHash{n, n}; }

AssetCacheEntry cacheEntry(Guid guid, std::string path, ContentHash contentHash, ContentHash metaHash = ContentHash{},
                           std::string importer = "", std::uint32_t importerVersion = 0,
                           std::vector<Guid> dependencies = {}, std::uint32_t missing = 0) {
    AssetCacheEntry entry;
    entry.guid = guid;
    entry.path = std::move(path);
    entry.contentHash = contentHash;
    entry.metaHash = metaHash;
    entry.importer = std::move(importer);
    entry.importerVersion = importerVersion;
    entry.dependencies = std::move(dependencies);
    entry.missing = missing;
    return entry;
}

ImportInput importInput(Guid guid, std::string relativePath, std::optional<ContentHash> contentHash,
                        ContentHash metaHash = ContentHash{}, std::string importer = "",
                        std::uint32_t importerVersion = 0, bool hashSkippedByBudget = false) {
    ImportInput input;
    input.guid = guid;
    input.relativePath = std::move(relativePath);
    input.contentHash = contentHash;
    input.metaHash = metaHash;
    input.importer = std::move(importer);
    input.importerVersion = importerVersion;
    input.hashSkippedByBudget = hashSkippedByBudget;
    return input;
}

AssetCacheIndex indexOf(std::vector<AssetCacheEntry> entries) {
    AssetCacheIndex index;
    index.entries = std::move(entries);
    std::sort(index.entries.begin(), index.entries.end(),
              [](const AssetCacheEntry& a, const AssetCacheEntry& b) { return a.guid < b.guid; });
    return index;
}

const ImportPlanEntry* findEntry(const ImportPlanResult& result, Guid guid) {
    for (const ImportPlanEntry& entry : result.entries) {
        if (entry.guid == guid) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("asset_cache: planImports on empty input (IP1, AC-21)") {
    const AssetCacheIndex previous;
    const ImportPlanResult result = planImports({}, previous);
    CHECK(result.entries.empty());
    CHECK(result.jobIndices.empty());
    CHECK(result.upToDate == 0);
    CHECK(result.newAssets == 0);
    CHECK(result.changed == 0);
    CHECK(result.dependencyChanged == 0);
    CHECK(result.unhashable == 0);
    CHECK(result.notHashed == 0);
}

TEST_CASE("asset_cache: New for its own cause -- no entry for the GUID (IP2, AC-22)") {
    const AssetCacheIndex previous;  // empty: no entry can possibly match
    const ImportPlanResult result =
        planImports({importInput(guidOf(1), "a.png", hashOf(10), hashOf(20), "gltf", 3)}, previous);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].change == ImportChange::New);
    CHECK(result.newAssets == 1);
    CHECK(result.jobIndices == std::vector<std::size_t>{0});
}

TEST_CASE("asset_cache: SourceChanged for its own cause (IP3, AC-22, seed S13/S14)") {
    const AssetCacheIndex previous = indexOf({cacheEntry(guidOf(1), "a.png", hashOf(10), hashOf(20), "gltf", 3)});
    const ImportPlanResult result =
        planImports({importInput(guidOf(1), "a.png", hashOf(999), hashOf(20), "gltf", 3)}, previous);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].change == ImportChange::SourceChanged);
    CHECK(result.changed == 1);
}

TEST_CASE("asset_cache: MetaChanged for its own cause (IP4, AC-22)") {
    const AssetCacheIndex previous = indexOf({cacheEntry(guidOf(1), "a.png", hashOf(10), hashOf(20), "gltf", 3)});
    const ImportPlanResult result =
        planImports({importInput(guidOf(1), "a.png", hashOf(10), hashOf(999), "gltf", 3)}, previous);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].change == ImportChange::MetaChanged);
    CHECK(result.changed == 1);
}

TEST_CASE("asset_cache: ImporterChanged for its own cause (IP5, AC-22, D16)") {
    // A hand-built previous entry with importer "gltf"; the input names a different importer text.
    const AssetCacheIndex previous = indexOf({cacheEntry(guidOf(1), "a.png", hashOf(10), hashOf(20), "gltf", 3)});
    const ImportPlanResult result =
        planImports({importInput(guidOf(1), "a.png", hashOf(10), hashOf(20), "obj", 3)}, previous);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].change == ImportChange::ImporterChanged);
    CHECK(result.changed == 1);

    // The importer VERSION alone differing is the same cause.
    const ImportPlanResult versionResult =
        planImports({importInput(guidOf(1), "a.png", hashOf(10), hashOf(20), "gltf", 4)}, previous);
    REQUIRE(versionResult.entries.size() == 1);
    CHECK(versionResult.entries[0].change == ImportChange::ImporterChanged);
}

TEST_CASE("asset_cache: Unhashable -- contentHash nullopt, not budget-skipped (IP6, AC-22)") {
    const AssetCacheIndex previous;
    const ImportPlanResult result = planImports(
        {importInput(guidOf(1), "a.png", std::nullopt, hashOf(20), "", 0, /*hashSkippedByBudget=*/false)}, previous);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].change == ImportChange::Unhashable);
    CHECK(result.unhashable == 1);
}

TEST_CASE("asset_cache: NotHashed -- contentHash nullopt, budget-skipped (IP7, AC-22)") {
    const AssetCacheIndex previous;
    const ImportPlanResult result = planImports(
        {importInput(guidOf(1), "a.png", std::nullopt, hashOf(20), "", 0, /*hashSkippedByBudget=*/true)}, previous);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].change == ImportChange::NotHashed);
    CHECK(result.notHashed == 1);
}

TEST_CASE("asset_cache: UpToDate when nothing differs (IP8, AC-22, seed S13)") {
    const AssetCacheIndex previous = indexOf({cacheEntry(guidOf(1), "a.png", hashOf(10), hashOf(20), "gltf", 3)});
    const ImportPlanResult result =
        planImports({importInput(guidOf(1), "a.png", hashOf(10), hashOf(20), "gltf", 3)}, previous);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].change == ImportChange::UpToDate);
    CHECK(result.upToDate == 1);
    CHECK(result.jobIndices.empty());
}

TEST_CASE(
    "asset_cache: PRECEDENCE -- New wins when SourceChanged/MetaChanged/ImporterChanged also conceptually "
    "apply (IP9, AC-22)") {
    // No previous entry AT ALL: every other own-cause is vacuously "different from nothing", but New
    // is checked first in the chain and there is nothing to compare against for the rest.
    const AssetCacheIndex previous;
    const ImportPlanResult result =
        planImports({importInput(guidOf(1), "a.png", hashOf(10), hashOf(20), "gltf", 3)}, previous);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].change == ImportChange::New);
}

TEST_CASE("asset_cache: PRECEDENCE -- SourceChanged wins over MetaChanged and ImporterChanged (IP10, AC-22)") {
    const AssetCacheIndex previous = indexOf({cacheEntry(guidOf(1), "a.png", hashOf(10), hashOf(20), "gltf", 3)});
    // contentHash, metaHash AND importer all differ from the previous entry at once.
    const ImportPlanResult result =
        planImports({importInput(guidOf(1), "a.png", hashOf(999), hashOf(888), "obj", 7)}, previous);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].change == ImportChange::SourceChanged);
}

TEST_CASE(
    "asset_cache: PRECEDENCE -- MetaChanged wins over ImporterChanged; Unhashable beats every own-cause "
    "(IP11, AC-22)") {
    const AssetCacheIndex previous = indexOf({cacheEntry(guidOf(1), "a.png", hashOf(10), hashOf(20), "gltf", 3)});
    // Same contentHash (SourceChanged does not apply); metaHash AND importer both differ.
    const ImportPlanResult metaWins =
        planImports({importInput(guidOf(1), "a.png", hashOf(10), hashOf(888), "obj", 7)}, previous);
    REQUIRE(metaWins.entries.size() == 1);
    CHECK(metaWins.entries[0].change == ImportChange::MetaChanged);

    // Identical construction, but this scan could not read the bytes at all -- Unhashable wins over
    // every own-cause, including one that would otherwise have won (MetaChanged here).
    const ImportPlanResult unhashableWins = planImports(
        {importInput(guidOf(1), "a.png", std::nullopt, hashOf(888), "obj", 7, /*hashSkippedByBudget=*/false)},
        previous);
    REQUIRE(unhashableWins.entries.size() == 1);
    CHECK(unhashableWins.entries[0].change == ImportChange::Unhashable);
}

TEST_CASE("asset_cache: a shuffled input produces an identical result (IP12, AC-21, seed S11)") {
    const AssetCacheIndex previous = indexOf({
        cacheEntry(guidOf(1), "a.png", hashOf(1)),
        cacheEntry(guidOf(2), "b.png", hashOf(999)),  // will report SourceChanged
    });
    const std::vector<ImportInput> orderA = {
        importInput(guidOf(1), "a.png", hashOf(1)),
        importInput(guidOf(2), "b.png", hashOf(2)),
        importInput(guidOf(3), "c.png", hashOf(3)),
    };
    const std::vector<ImportInput> orderB = {orderA[2], orderA[0], orderA[1]};
    const std::vector<ImportInput> orderC = {orderA[1], orderA[2], orderA[0]};

    const ImportPlanResult resultA = planImports(orderA, previous);
    const ImportPlanResult resultB = planImports(orderB, previous);
    const ImportPlanResult resultC = planImports(orderC, previous);

    REQUIRE(resultA.entries.size() == 3);
    REQUIRE(resultB.entries.size() == 3);
    REQUIRE(resultC.entries.size() == 3);
    for (std::size_t i = 0; i < resultA.entries.size(); ++i) {
        CHECK(resultA.entries[i].guid == resultB.entries[i].guid);
        CHECK(resultA.entries[i].relativePath == resultB.entries[i].relativePath);
        CHECK(resultA.entries[i].change == resultB.entries[i].change);
        CHECK(resultA.entries[i].guid == resultC.entries[i].guid);
        CHECK(resultA.entries[i].relativePath == resultC.entries[i].relativePath);
        CHECK(resultA.entries[i].change == resultC.entries[i].change);
    }
    CHECK(resultA.jobIndices == resultB.jobIndices);
    CHECK(resultA.jobIndices == resultC.jobIndices);
}

TEST_CASE("asset_cache: entries are sorted byte-lexicographically -- 'Z.png' precedes 'a.png' (IP13, AC-21)") {
    const AssetCacheIndex previous;
    const ImportPlanResult result =
        planImports({importInput(guidOf(2), "a.png", hashOf(2)), importInput(guidOf(1), "Z.png", hashOf(1))}, previous);
    REQUIRE(result.entries.size() == 2);
    CHECK(result.entries[0].relativePath == "Z.png");
    CHECK(result.entries[1].relativePath == "a.png");
}

TEST_CASE(
    "asset_cache: transitive A -> B -> C, C changed marks both B and A DependencyChanged (IP14, AC-23, "
    "seed S18)") {
    const Guid a = guidOf(1);
    const Guid b = guidOf(2);
    const Guid c = guidOf(3);
    const AssetCacheIndex previous = indexOf({
        cacheEntry(a, "a.mat", hashOf(1), ContentHash{}, "", 0, {b}),
        cacheEntry(b, "b.png", hashOf(2), ContentHash{}, "", 0, {c}),
        cacheEntry(c, "c.psd", hashOf(3)),
    });
    const ImportPlanResult result = planImports(
        {
            importInput(a, "a.mat", hashOf(1)),    // unchanged own-cause
            importInput(b, "b.png", hashOf(2)),    // unchanged own-cause
            importInput(c, "c.psd", hashOf(999)),  // SourceChanged
        },
        previous);
    REQUIRE(result.entries.size() == 3);
    CHECK(findEntry(result, c)->change == ImportChange::SourceChanged);
    CHECK(findEntry(result, b)->change == ImportChange::DependencyChanged);
    CHECK(findEntry(result, a)->change == ImportChange::DependencyChanged);
}

TEST_CASE("asset_cache: a four-deep chain propagates all the way (IP15, AC-23)") {
    const Guid a = guidOf(1);
    const Guid b = guidOf(2);
    const Guid c = guidOf(3);
    const Guid d = guidOf(4);
    const AssetCacheIndex previous = indexOf({
        cacheEntry(a, "a", hashOf(1), ContentHash{}, "", 0, {b}),
        cacheEntry(b, "b", hashOf(2), ContentHash{}, "", 0, {c}),
        cacheEntry(c, "c", hashOf(3), ContentHash{}, "", 0, {d}),
        cacheEntry(d, "d", hashOf(4)),
    });
    const ImportPlanResult result = planImports(
        {
            importInput(a, "a", hashOf(1)), importInput(b, "b", hashOf(2)), importInput(c, "c", hashOf(3)),
            importInput(d, "d", hashOf(999)),  // SourceChanged
        },
        previous);
    REQUIRE(result.entries.size() == 4);
    CHECK(findEntry(result, d)->change == ImportChange::SourceChanged);
    CHECK(findEntry(result, c)->change == ImportChange::DependencyChanged);
    CHECK(findEntry(result, b)->change == ImportChange::DependencyChanged);
    CHECK(findEntry(result, a)->change == ImportChange::DependencyChanged);
}

TEST_CASE("asset_cache: a diamond marks each node ONCE -- no duplicates in jobIndices (IP16, AC-23)") {
    const Guid a = guidOf(1);
    const Guid b = guidOf(2);
    const Guid c = guidOf(3);
    const Guid d = guidOf(4);
    // D depends on B and C; both B and C depend on A.
    const AssetCacheIndex previous = indexOf({
        cacheEntry(a, "a", hashOf(1)),
        cacheEntry(b, "b", hashOf(2), ContentHash{}, "", 0, {a}),
        cacheEntry(c, "c", hashOf(3), ContentHash{}, "", 0, {a}),
        cacheEntry(d, "d", hashOf(4), ContentHash{}, "", 0, {b, c}),
    });
    const ImportPlanResult result = planImports(
        {
            importInput(a, "a", hashOf(999)),  // SourceChanged
            importInput(b, "b", hashOf(2)),
            importInput(c, "c", hashOf(3)),
            importInput(d, "d", hashOf(4)),
        },
        previous);
    REQUIRE(result.entries.size() == 4);
    CHECK(findEntry(result, a)->change == ImportChange::SourceChanged);
    CHECK(findEntry(result, b)->change == ImportChange::DependencyChanged);
    CHECK(findEntry(result, c)->change == ImportChange::DependencyChanged);
    CHECK(findEntry(result, d)->change == ImportChange::DependencyChanged);
    CHECK(result.jobIndices.size() == 4);
    std::vector<std::size_t> sortedJobIndices = result.jobIndices;
    std::sort(sortedJobIndices.begin(), sortedJobIndices.end());
    CHECK(std::adjacent_find(sortedJobIndices.begin(), sortedJobIndices.end()) == sortedJobIndices.end());
}

TEST_CASE("asset_cache: a dangling dependency marks its dependent DependencyChanged (IP17, AC-23, seed S20)") {
    const Guid onlyNode = guidOf(1);
    const Guid missing = guidOf(2);  // no entry, no input, this scan or ever
    const AssetCacheIndex previous =
        indexOf({cacheEntry(onlyNode, "only.mat", hashOf(1), ContentHash{}, "", 0, {missing})});
    const ImportPlanResult result = planImports({importInput(onlyNode, "only.mat", hashOf(1))}, previous);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].change == ImportChange::DependencyChanged);
}

TEST_CASE(
    "asset_cache: a cycle A -> B -> A with B's source changed terminates and marks both (IP18, AC-24, "
    "seed S19)") {
    // planImports's worklist pushes a node only on the clean->dirty transition (D-6 step 4), so this
    // call terminates in O(V+E) regardless of the cycle -- there is no visited set, no cycle detection
    // and no recursion to get wrong.
    //
    // READ THE FAILURE MODE PLAINLY: this case has NO hard bound of any kind. The plan's §B and §9
    // both claim it "fails an assertion rather than hanging"; that claim is WRONG. Measured against a
    // seeded enqueue-on-every-visit implementation, IP18 and IP20 each ran past a 20-second alarm --
    // a non-terminating cascade HANGS the test binary and is reported by ctest's timeout, never by a
    // failed CHECK. That is accepted deliberately: an iteration cap here would be a cycle detector in
    // disguise, and D12 forbids one. The CHECKs below are reached only if the call returns at all.
    const Guid a = guidOf(1);
    const Guid b = guidOf(2);
    const AssetCacheIndex previous = indexOf({
        cacheEntry(a, "a", hashOf(1), ContentHash{}, "", 0, {b}),
        cacheEntry(b, "b", hashOf(2), ContentHash{}, "", 0, {a}),
    });
    const ImportPlanResult result =
        planImports({importInput(a, "a", hashOf(1)), importInput(b, "b", hashOf(999))}, previous);
    REQUIRE(result.entries.size() == 2);
    CHECK(findEntry(result, b)->change == ImportChange::SourceChanged);
    CHECK(findEntry(result, a)->change == ImportChange::DependencyChanged);
    CHECK(result.jobIndices.size() == 2);
}

TEST_CASE("asset_cache: a clean cycle stays entirely UpToDate (IP19, AC-24, E27)") {
    const Guid a = guidOf(1);
    const Guid b = guidOf(2);
    const AssetCacheIndex previous = indexOf({
        cacheEntry(a, "a", hashOf(1), ContentHash{}, "", 0, {b}),
        cacheEntry(b, "b", hashOf(2), ContentHash{}, "", 0, {a}),
    });
    const ImportPlanResult result =
        planImports({importInput(a, "a", hashOf(1)), importInput(b, "b", hashOf(2))}, previous);
    REQUIRE(result.entries.size() == 2);
    CHECK(findEntry(result, a)->change == ImportChange::UpToDate);
    CHECK(findEntry(result, b)->change == ImportChange::UpToDate);
    CHECK(result.jobIndices.empty());
}

TEST_CASE("asset_cache: a self-loop (A depends on A), clean and dirty (IP20, AC-24)") {
    const Guid a = guidOf(1);
    const AssetCacheIndex cleanPrevious = indexOf({cacheEntry(a, "a", hashOf(1), ContentHash{}, "", 0, {a})});
    const ImportPlanResult cleanResult = planImports({importInput(a, "a", hashOf(1))}, cleanPrevious);
    REQUIRE(cleanResult.entries.size() == 1);
    CHECK(cleanResult.entries[0].change == ImportChange::UpToDate);

    const ImportPlanResult dirtyResult = planImports({importInput(a, "a", hashOf(999))}, cleanPrevious);
    REQUIRE(dirtyResult.entries.size() == 1);
    // A's own source changed is an OWN cause, not a self-inflicted DependencyChanged.
    CHECK(dirtyResult.entries[0].change == ImportChange::SourceChanged);
    CHECK(dirtyResult.jobIndices.size() == 1);
}

TEST_CASE(
    "asset_cache: commitImports writes an entry ONLY for an input with a resolved hash (IP21, AC-25, "
    "seed S15)") {
    const AssetCacheIndex previous;
    const std::vector<ImportInput> inputs = {
        importInput(guidOf(1), "a.png", hashOf(10)),                                 // hashed -> committed
        importInput(guidOf(2), "b.png", std::nullopt, ContentHash{}, "", 0, false),  // Unhashable, no previous
    };
    const ImportPlanResult plan = planImports(inputs, previous);
    const AssetCacheIndex next = commitImports(previous, inputs, plan);
    REQUIRE(next.entries.size() == 1);
    CHECK(next.entries[0].guid == guidOf(1));
    CHECK(next.find(guidOf(2)) == nullptr);
}

TEST_CASE(
    "asset_cache: an Unhashable input's previous entry is preserved byte-for-byte, and nothing is "
    "fabricated when there was none (IP22, AC-25, seed S16)") {
    const AssetCacheEntry richPrevious =
        cacheEntry(guidOf(1), "a.png", hashOf(10), hashOf(20), "gltf", 3, {guidOf(9)}, 1);
    const AssetCacheIndex previous = indexOf({richPrevious});
    const std::vector<ImportInput> inputs = {
        importInput(guidOf(1), "a.png", std::nullopt, ContentHash{}, "", 0, false),  // Unhashable
    };
    const ImportPlanResult plan = planImports(inputs, previous);
    const AssetCacheIndex next = commitImports(previous, inputs, plan);
    REQUIRE(next.entries.size() == 1);
    const AssetCacheEntry& preserved = next.entries[0];
    CHECK(preserved.guid == richPrevious.guid);
    CHECK(preserved.path == richPrevious.path);
    CHECK(preserved.size == richPrevious.size);
    CHECK(preserved.mtime == richPrevious.mtime);
    CHECK(preserved.contentHash == richPrevious.contentHash);
    CHECK(preserved.metaHash == richPrevious.metaHash);
    CHECK(preserved.importer == richPrevious.importer);
    CHECK(preserved.importerVersion == richPrevious.importerVersion);
    CHECK(preserved.dependencies == richPrevious.dependencies);
    CHECK(preserved.missing == richPrevious.missing);  // NOT reset -- verbatim means verbatim
}

TEST_CASE("asset_cache: a NotHashed input is not committed as up to date (IP23, AC-30, seed S15)") {
    const AssetCacheIndex previous = indexOf({cacheEntry(guidOf(1), "a.png", hashOf(10), hashOf(20))});
    const std::vector<ImportInput> inputs = {
        importInput(guidOf(1), "a.png", std::nullopt, hashOf(20), "", 0, /*hashSkippedByBudget=*/true)};
    const ImportPlanResult plan = planImports(inputs, previous);
    REQUIRE(plan.entries.size() == 1);
    CHECK(plan.entries[0].change == ImportChange::NotHashed);
    const AssetCacheIndex next = commitImports(previous, inputs, plan);
    REQUIRE(next.entries.size() == 1);
    CHECK(next.entries[0].contentHash == hashOf(10));  // the OLD hash, never fabricated as fresh

    // A re-run against the resulting index still reports it as needing work, not UpToDate.
    const ImportPlanResult replan = planImports(inputs, next);
    REQUIRE(replan.entries.size() == 1);
    CHECK(replan.entries[0].change == ImportChange::NotHashed);
}

// IP39/IP40 -- the shape IP22 and IP23 cannot see, and the reason "verbatim" has to be spelled out
// field by field. Both of those cases leave the cached entry AND the input at (size, mtime) == (0, 0),
// so a commit that carried the previous entry forward but REFRESHED its `size`/`mtime` to this scan's
// observed values would still compare equal to the fixture and both would stay green. That is not a
// hypothetical: it is the realistic form of the R-C2 regression, and it is the one outcome this task
// must never produce -- a refreshed (size, mtime) makes the NEXT scan's fast path vouch for bytes
// nobody ever hashed, i.e. a false UpToDate. The pair below is the only input shape where it shows.

TEST_CASE(
    "asset_cache: an Unhashable input's cached (size, mtime) survive a scan that OBSERVED different "
    "ones (IP39, AC-25, INV-C4, seed S15b)") {
    AssetCacheEntry cached = cacheEntry(guidOf(1), "a.png", hashOf(10), hashOf(20), "gltf", 3, {guidOf(9)}, 1);
    cached.size = 4096;
    cached.mtime = 1700000000;
    const AssetCacheIndex previous = indexOf({cached});

    ImportInput input = importInput(guidOf(1), "a.png", std::nullopt, ContentHash{}, "", 0, /*budget=*/false);
    input.size = 999999;       // what THIS scan stat()ed -- deliberately UNEQUAL to the cached value
    input.mtime = 1800000000;  // ditto: a real edit that could not be read back
    const std::vector<ImportInput> inputs = {input};

    const ImportPlanResult plan = planImports(inputs, previous);
    REQUIRE(plan.entries.size() == 1);
    REQUIRE(plan.entries[0].change == ImportChange::Unhashable);

    const AssetCacheIndex next = commitImports(previous, inputs, plan);
    REQUIRE(next.entries.size() == 1);
    CHECK(next.entries[0].size == 4096);         // the CACHED value, never this scan's observation
    CHECK(next.entries[0].mtime == 1700000000);  // ditto -- "verbatim" covers these two as well
    CHECK(next.entries[0].contentHash == hashOf(10));
    CHECK(next.entries[0].missing == 1);
}

TEST_CASE(
    "asset_cache: a NotHashed input's cached (size, mtime) survive a scan that OBSERVED different "
    "ones (IP40, AC-25, INV-C4, seed S15b)") {
    AssetCacheEntry cached = cacheEntry(guidOf(1), "big.bin", hashOf(10), hashOf(20), "gltf", 3, {}, 0);
    cached.size = 64;
    cached.mtime = -86400;  // a legitimately pre-1970 mtime, so a "refresh" cannot coincide with it
    const AssetCacheIndex previous = indexOf({cached});

    ImportInput input = importInput(guidOf(1), "big.bin", std::nullopt, hashOf(20), "gltf", 3, /*budget=*/true);
    input.size = 8ULL * 1024 * 1024 * 1024;  // the budget ran out precisely BECAUSE it is enormous
    input.mtime = 1800000000;
    const std::vector<ImportInput> inputs = {input};

    const ImportPlanResult plan = planImports(inputs, previous);
    REQUIRE(plan.entries.size() == 1);
    REQUIRE(plan.entries[0].change == ImportChange::NotHashed);

    const AssetCacheIndex next = commitImports(previous, inputs, plan);
    REQUIRE(next.entries.size() == 1);
    CHECK(next.entries[0].size == 64);
    CHECK(next.entries[0].mtime == -86400);
    CHECK(next.entries[0].contentHash == hashOf(10));
}

TEST_CASE(
    "asset_cache: the grace counter -- absent once retains at 1, absent 3x retains at 3, a 4th absence "
    "drops it (IP24, AC-26, seed S21)") {
    const AssetCacheEntry entry = cacheEntry(guidOf(1), "a.png", hashOf(10));
    const AssetCacheIndex freshIndex = indexOf({entry});
    const ImportPlanResult emptyPlan = planImports({}, freshIndex);

    const AssetCacheIndex afterOne = commitImports(freshIndex, {}, emptyPlan);
    REQUIRE(afterOne.entries.size() == 1);
    CHECK(afterOne.entries[0].missing == 1);

    // Simulate two more absent scans by feeding the previous result back in.
    const AssetCacheIndex afterTwo = commitImports(afterOne, {}, planImports({}, afterOne));
    REQUIRE(afterTwo.entries.size() == 1);
    CHECK(afterTwo.entries[0].missing == 2);

    const AssetCacheIndex afterThree = commitImports(afterTwo, {}, planImports({}, afterTwo));
    REQUIRE(afterThree.entries.size() == 1);
    CHECK(afterThree.entries[0].missing == MISSING_SCAN_GRACE);
    CHECK(MISSING_SCAN_GRACE == 3);

    // The scan that would make it 4 drops the entry instead.
    const AssetCacheIndex afterFour = commitImports(afterThree, {}, planImports({}, afterThree));
    CHECK(afterFour.entries.empty());
}

TEST_CASE("asset_cache: seeing the GUID again resets missing to 0 (IP25, AC-26)") {
    const AssetCacheIndex previous = indexOf({cacheEntry(guidOf(1), "a.png", hashOf(10), ContentHash{}, "", 0, {}, 2)});
    const std::vector<ImportInput> inputs = {importInput(guidOf(1), "a.png", hashOf(999))};
    const ImportPlanResult plan = planImports(inputs, previous);
    const AssetCacheIndex next = commitImports(previous, inputs, plan);
    REQUIRE(next.entries.size() == 1);
    CHECK(next.entries[0].missing == 0);
}

TEST_CASE(
    "asset_cache: a moved asset (same GUID, same hashes, different path) stays UpToDate and the entry's "
    "path is updated (IP26, D11, E8)") {
    const AssetCacheIndex previous = indexOf({cacheEntry(guidOf(1), "old/a.png", hashOf(10), hashOf(20))});
    const std::vector<ImportInput> inputs = {importInput(guidOf(1), "new/a.png", hashOf(10), hashOf(20))};
    const ImportPlanResult plan = planImports(inputs, previous);
    REQUIRE(plan.entries.size() == 1);
    CHECK(plan.entries[0].change == ImportChange::UpToDate);
    const AssetCacheIndex next = commitImports(previous, inputs, plan);
    REQUIRE(next.entries.size() == 1);
    CHECK(next.entries[0].path == "new/a.png");
}

// =====================================================================================================
// IP -- planReattachments, D13's orphan re-attachment. All PURE, from std::vector literals (task 3.1.2
// Step 7)
// =====================================================================================================

namespace {

ReattachCandidate candidateOf(std::string relativePath, ContentHash contentHash) {
    return ReattachCandidate{std::move(relativePath), contentHash};
}

OrphanMeta orphanOf(std::string relativePath, Guid guid) { return OrphanMeta{std::move(relativePath), guid}; }

}  // namespace

TEST_CASE(
    "asset_cache: the canonical re-attachment -- one candidate, one absent entry, one orphan, no live "
    "claimant (IP27, AC-27, D13)") {
    const Guid oldGuid = guidOf(1);
    const AssetCacheIndex previous = indexOf({cacheEntry(oldGuid, "old/wood.png", hashOf(50))});
    const std::vector<ReattachCandidate> candidates = {candidateOf("new/wood.png", hashOf(50))};
    const std::vector<OrphanMeta> orphans = {orphanOf("old/wood.png.meta", oldGuid)};
    const std::vector<ReattachMatch> matches =
        planReattachments(candidates, orphans, previous, /*liveGuids=*/{}, /*livePaths=*/{"new/wood.png"});
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].candidateIndex == 0);
    CHECK(matches[0].guid == oldGuid);
    CHECK(matches[0].fromMetaPath == "old/wood.png.meta");
    CHECK(matches[0].fromAssetPath == "old/wood.png");
}

TEST_CASE(
    "asset_cache: condition 3 fails -- two absent entries share the candidate's hash -> no match (IP28, "
    "AC-27, E10)") {
    const AssetCacheIndex previous = indexOf({
        cacheEntry(guidOf(1), "old1.png", hashOf(50)),
        cacheEntry(guidOf(2), "old2.png", hashOf(50)),
    });
    const std::vector<ReattachCandidate> candidates = {candidateOf("new.png", hashOf(50))};
    const std::vector<OrphanMeta> orphans = {orphanOf("old1.png.meta", guidOf(1)),
                                             orphanOf("old2.png.meta", guidOf(2))};
    const std::vector<ReattachMatch> matches = planReattachments(candidates, orphans, previous, {}, {"new.png"});
    CHECK(matches.empty());
}

TEST_CASE(
    "asset_cache: condition 4 fails -- a live asset already claims that GUID -> no match (IP29, AC-27, "
    "seed S23, E12)") {
    const Guid claimedGuid = guidOf(1);
    const AssetCacheIndex previous = indexOf({cacheEntry(claimedGuid, "old.png", hashOf(50))});
    const std::vector<ReattachCandidate> candidates = {candidateOf("new.png", hashOf(50))};
    const std::vector<OrphanMeta> orphans = {orphanOf("old.png.meta", claimedGuid)};
    const std::vector<ReattachMatch> matches =
        planReattachments(candidates, orphans, previous, /*liveGuids=*/{claimedGuid}, /*livePaths=*/{"new.png"});
    CHECK(matches.empty());
}

TEST_CASE(
    "asset_cache: condition 5 fails -- two orphans parse to that GUID -> no match (IP30, AC-27, seed S22, "
    "E11)") {
    const Guid targetGuid = guidOf(1);
    const AssetCacheIndex previous = indexOf({cacheEntry(targetGuid, "old.png", hashOf(50))});
    const std::vector<ReattachCandidate> candidates = {candidateOf("new.png", hashOf(50))};
    const std::vector<OrphanMeta> orphans = {orphanOf("a.meta", targetGuid), orphanOf("b.meta", targetGuid)};
    const std::vector<ReattachMatch> matches = planReattachments(candidates, orphans, previous, {}, {"new.png"});
    CHECK(matches.empty());
}

TEST_CASE("asset_cache: no orphan at all for the GUID -> no match (IP31, AC-27)") {
    const AssetCacheIndex previous = indexOf({cacheEntry(guidOf(1), "old.png", hashOf(50))});
    const std::vector<ReattachCandidate> candidates = {candidateOf("new.png", hashOf(50))};
    const std::vector<ReattachMatch> matches = planReattachments(candidates, /*orphans=*/{}, previous, {}, {"new.png"});
    CHECK(matches.empty());
}

TEST_CASE(
    "asset_cache: the matching entry's path is still live this scan -> not a candidate at all -> no match "
    "(IP32, AC-27)") {
    const Guid guid = guidOf(1);
    const AssetCacheIndex previous = indexOf({cacheEntry(guid, "still/here.png", hashOf(50))});
    const std::vector<ReattachCandidate> candidates = {candidateOf("new.png", hashOf(50))};
    const std::vector<OrphanMeta> orphans = {orphanOf("still/here.png.meta", guid)};
    // "still/here.png" is STILL live this scan -- the previous entry is filtered out of byHash before
    // it can ever be considered, regardless of the candidate's own hash.
    const std::vector<ReattachMatch> matches =
        planReattachments(candidates, orphans, previous, {}, {"new.png", "still/here.png"});
    CHECK(matches.empty());
}

TEST_CASE("asset_cache: an entry with missing == 2 is fully eligible for re-attachment (IP33, D14 + D13)") {
    const Guid oldGuid = guidOf(1);
    const AssetCacheIndex previous =
        indexOf({cacheEntry(oldGuid, "old.png", hashOf(50), ContentHash{}, "", 0, {}, /*missing=*/2)});
    const std::vector<ReattachCandidate> candidates = {candidateOf("new.png", hashOf(50))};
    const std::vector<OrphanMeta> orphans = {orphanOf("old.png.meta", oldGuid)};
    const std::vector<ReattachMatch> matches = planReattachments(candidates, orphans, previous, {}, {"new.png"});
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].guid == oldGuid);
}

TEST_CASE("asset_cache: two candidates, one GUID -- the first in sorted path order takes it (IP34, AC-27)") {
    const Guid guid = guidOf(1);
    const AssetCacheIndex previous = indexOf({cacheEntry(guid, "old.png", hashOf(50))});
    const std::vector<ReattachCandidate> candidates = {candidateOf("z-second.png", hashOf(50)),
                                                       candidateOf("a-first.png", hashOf(50))};
    const std::vector<OrphanMeta> orphans = {orphanOf("old.png.meta", guid)};
    const std::vector<ReattachMatch> matches =
        planReattachments(candidates, orphans, previous, {}, {"a-first.png", "z-second.png"});
    REQUIRE(matches.size() == 1);
    // candidateIndex 1 is "a-first.png" -- alphabetically first, regardless of vector position; it
    // does NOT silently steal the identity for "z-second.png" (vector index 0), which gets nothing.
    CHECK(matches[0].candidateIndex == 1);
    CHECK(matches[0].guid == guid);
}

TEST_CASE("asset_cache: two candidates with different hashes each match their own entry (IP35, AC-27)") {
    const Guid guidA = guidOf(1);
    const Guid guidB = guidOf(2);
    const AssetCacheIndex previous = indexOf({
        cacheEntry(guidA, "oldA.png", hashOf(50)),
        cacheEntry(guidB, "oldB.png", hashOf(60)),
    });
    const std::vector<ReattachCandidate> candidates = {candidateOf("newA.png", hashOf(50)),
                                                       candidateOf("newB.png", hashOf(60))};
    const std::vector<OrphanMeta> orphans = {orphanOf("oldA.png.meta", guidA), orphanOf("oldB.png.meta", guidB)};
    const std::vector<ReattachMatch> matches =
        planReattachments(candidates, orphans, previous, {}, {"newA.png", "newB.png"});
    REQUIRE(matches.size() == 2);
    CHECK(matches[0].candidateIndex == 0);  // "newA.png" sorts first
    CHECK(matches[0].guid == guidA);
    CHECK(matches[1].candidateIndex == 1);
    CHECK(matches[1].guid == guidB);
}

TEST_CASE(
    "asset_cache: two byte-identical LIVE files produce no re-attachment and keep two identities (IP36, "
    "seed S26, D9)") {
    const Guid guidX = guidOf(1);
    const Guid guidY = guidOf(2);
    // Both entries are STILL LIVE this scan (both paths appear in livePaths), and the OUTCOME this
    // pins is the one that matters: two byte-identical live files keep two identities and a third
    // copy steals neither. Content equality is never used for identity dedup (spec correction A12).
    //
    // This case cannot see the MECHANISM its earlier comment described. Removing A12's live-path
    // filter puts BOTH entries in byHash[H], and condition 3's `size() != 1` then rejects the
    // candidate anyway -- a different guard reaching the same answer, so this case stays green.
    // IP32 is the real discriminator for that filter: there, a single absent entry sails through
    // every remaining condition and produces a match the moment the filter is gone.
    const AssetCacheIndex previous = indexOf({
        cacheEntry(guidX, "copy1.png", hashOf(50)),
        cacheEntry(guidY, "copy2.png", hashOf(50)),
    });
    const std::vector<ReattachCandidate> candidates = {candidateOf("copy3.png", hashOf(50))};
    const std::vector<OrphanMeta> orphans = {orphanOf("orphan.meta", guidOf(99))};  // unrelated, irrelevant
    const std::vector<ReattachMatch> matches =
        planReattachments(candidates, orphans, previous, {}, {"copy1.png", "copy2.png", "copy3.png"});
    CHECK(matches.empty());
}

TEST_CASE("asset_cache: an empty (0-byte) candidate can match an empty absent entry (IP37, E13, A4)") {
    const Guid oldGuid = guidOf(1);
    const AssetCacheIndex previous = indexOf({cacheEntry(oldGuid, "old.bin", ContentHash{})});  // nil == empty file
    const std::vector<ReattachCandidate> candidates = {candidateOf("new.bin", ContentHash{})};
    const std::vector<OrphanMeta> orphans = {orphanOf("old.bin.meta", oldGuid)};
    const std::vector<ReattachMatch> matches = planReattachments(candidates, orphans, previous, {}, {"new.bin"});
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].guid == oldGuid);
}

TEST_CASE("asset_cache: the result is independent of candidate/orphan vector order (IP38, INV-C3)") {
    const Guid guidA = guidOf(1);
    const Guid guidB = guidOf(2);
    const AssetCacheIndex previous = indexOf({
        cacheEntry(guidA, "oldA.png", hashOf(50)),
        cacheEntry(guidB, "oldB.png", hashOf(60)),
    });
    const ReattachCandidate candA = candidateOf("newA.png", hashOf(50));
    const ReattachCandidate candB = candidateOf("newB.png", hashOf(60));
    const OrphanMeta orphanA = orphanOf("oldA.png.meta", guidA);
    const OrphanMeta orphanB = orphanOf("oldB.png.meta", guidB);
    const std::vector<std::string> livePaths = {"newA.png", "newB.png"};

    const std::vector<ReattachMatch> orderA =
        planReattachments({candA, candB}, {orphanA, orphanB}, previous, {}, livePaths);
    const std::vector<ReattachMatch> orderB =
        planReattachments({candB, candA}, {orphanB, orphanA}, previous, {}, livePaths);

    REQUIRE(orderA.size() == 2);
    REQUIRE(orderB.size() == 2);
    for (std::size_t i = 0; i < orderA.size(); ++i) {
        CHECK(orderA[i].guid == orderB[i].guid);
        CHECK(orderA[i].fromMetaPath == orderB[i].fromMetaPath);
        CHECK(orderA[i].fromAssetPath == orderB[i].fromAssetPath);
    }
}
