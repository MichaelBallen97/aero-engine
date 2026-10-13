// tests/editor/asset_view_test.cpp -- task 3.1.3, Step 1: the asset browser's presentation model
// (KINDS, icons, filter, whole-project search, grid column math). A TU of aero_editor_shell_test,
// which supplies main() from shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED (D4/AC-17/INV-P5, the asset_meta_test.cpp precedent): asset_view.hpp depends on nothing but
// asset_meta.hpp and project_files.hpp (code-review BLOCKING-2 added the latter, for
// filterEntriesByKind's FileEntry parameter) -- neither needs reflection, so every case here must be
// PRESENT and PASSING in all three build configurations -- prove it with --list-test-cases. Tier-0: no
// GPU, no window, no ImGui context, no entropy source, and no disk I/O except BV9's read of the editor's
// own source text through AERO_EDITOR_SRC_DIR (task E.4.4; the DR18 precedent in asset_drag_test.cpp).
#include <aero/editor/asset_browser_chrome.hpp>  // task E.6.3 (AV66): the contents pane's group
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/asset_view.hpp>
#include <aero/editor/project_files.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using engine::editor::ASSET_KIND_FILTER_OPTIONS;
using engine::editor::AssetFilter;
using engine::editor::AssetKind;
using engine::editor::assetKindLabel;
using engine::editor::AssetRecord;
using engine::editor::classifyAssetKind;
using engine::editor::FileEntry;
using engine::editor::filterEntriesByKind;
using engine::editor::gridColumnsFor;
using engine::editor::iconColorFor;
using engine::editor::iconLabelFor;
using engine::editor::IGNORED_ASSET_NAME_SUFFIXES;
using engine::editor::IGNORED_ASSET_NAMES;
using engine::editor::isBrowserVisibleName;
using engine::editor::isIgnoredAssetName;
using engine::editor::isMetaFileName;
using engine::editor::isScannableAssetName;
using engine::editor::isThumbnailDecodable;
using engine::editor::matchesFilter;
using engine::editor::searchAssets;
using engine::editor::SearchResult;
using engine::editor::tileEdgeFontMultiple;
using engine::editor::TileSize;

TEST_CASE("asset view: every Texture extension classifies as Texture (AV1)") {
    constexpr std::array<std::string_view, 10> EXTS{"png", "jpg", "jpeg", "tga",  "bmp",
                                                    "gif", "hdr", "psd",  "ktx2", "dds"};
    for (const std::string_view ext : EXTS) {
        const std::string name = std::string("x.") + std::string(ext);
        INFO("ext: ", std::string(ext));
        CHECK(classifyAssetKind(name, false) == AssetKind::Texture);
    }
}

TEST_CASE("asset view: every Model extension classifies as Model (AV2)") {
    // The extension table itself is 8 entries (gltf glb fbx obj blend dae ply stl); the plan's own
    // table comment lists all 8 verbatim even though its AV2 summary line says "(7)" -- a counting
    // slip in the plan text, not in the extension list. All 8 are proven here.
    constexpr std::array<std::string_view, 8> EXTS{"gltf", "glb", "fbx", "obj", "blend", "dae", "ply", "stl"};
    for (const std::string_view ext : EXTS) {
        const std::string name = std::string("x.") + std::string(ext);
        INFO("ext: ", std::string(ext));
        CHECK(classifyAssetKind(name, false) == AssetKind::Model);
    }
}

TEST_CASE("asset view: every Audio extension classifies as Audio (AV3)") {
    constexpr std::array<std::string_view, 4> EXTS{"wav", "mp3", "ogg", "flac"};
    for (const std::string_view ext : EXTS) {
        const std::string name = std::string("x.") + std::string(ext);
        CHECK(classifyAssetKind(name, false) == AssetKind::Audio);
    }
}

TEST_CASE("asset view: every Text extension classifies as Text (AV4)") {
    constexpr std::array<std::string_view, 7> EXTS{"json", "txt", "md", "hlsl", "glsl", "ts", "js"};
    for (const std::string_view ext : EXTS) {
        const std::string name = std::string("x.") + std::string(ext);
        CHECK(classifyAssetKind(name, false) == AssetKind::Text);
    }
}

TEST_CASE("asset view: no extension is Unknown (AV5)") {
    CHECK(classifyAssetKind("README", false) == AssetKind::Unknown);
}

TEST_CASE("asset view: a trailing dot is Unknown (AV6)") {
    CHECK(classifyAssetKind("x.", false) == AssetKind::Unknown);
}

TEST_CASE("asset view: only the LAST extension is used, and an unknown last extension wins (AV7)") {
    CHECK(classifyAssetKind("archive.tar.gz", false) == AssetKind::Unknown);
}

TEST_CASE("asset view: the last extension wins the other way too (AV8)") {
    CHECK(classifyAssetKind("scene.tar.gltf", false) == AssetKind::Model);
}

TEST_CASE("asset view: a directory named like a texture is still Folder (AV9)") {
    CHECK(classifyAssetKind("textures.png", true) == AssetKind::Folder);
}

TEST_CASE("asset view: a directory with no extension is Folder (AV10)") {
    CHECK(classifyAssetKind("textures", true) == AssetKind::Folder);
}

TEST_CASE("asset view: extension classification is ASCII-case-insensitive (AV11, seed S17)") {
    CHECK(classifyAssetKind("x.PNG", false) == AssetKind::Texture);
    CHECK(classifyAssetKind("x.PnG", false) == AssetKind::Texture);
    CHECK(classifyAssetKind("x.pNg", false) == AssetKind::Texture);
}

TEST_CASE("asset view: a non-ASCII extension is Unknown, no byte mangling (AV12)") {
    CHECK(classifyAssetKind("x.p\xC3\xB1g", false) == AssetKind::Unknown);  // "pñg"
}

TEST_CASE("asset view: an empty name is Unknown, or Folder when isDirectory (AV13)") {
    CHECK(classifyAssetKind("", false) == AssetKind::Unknown);
    CHECK(classifyAssetKind("", true) == AssetKind::Folder);
}

TEST_CASE("asset view: classifyAssetKind is total (AV14)") {
    constexpr std::array<std::string_view, 6> UNKNOWNS{"a.zip", "a.exe", "a.rar", "a.7z", "a.iso", "a.bin"};
    for (const std::string_view name : UNKNOWNS) {
        const AssetKind kind = classifyAssetKind(name, false);
        CHECK((kind == AssetKind::Folder || kind == AssetKind::Texture || kind == AssetKind::Model ||
               kind == AssetKind::Audio || kind == AssetKind::Text || kind == AssetKind::Material ||
               kind == AssetKind::Unknown));
    }
}

TEST_CASE("asset view: isThumbnailDecodable true for the stb-readable subset (AV15)") {
    constexpr std::array<std::string_view, 8> EXTS{"png", "jpg", "jpeg", "tga", "bmp", "gif", "hdr", "psd"};
    for (const std::string_view ext : EXTS) {
        const std::string name = std::string("x.") + std::string(ext);
        CHECK(isThumbnailDecodable(name));
    }
}

TEST_CASE("asset view: isThumbnailDecodable is FALSE for .ktx2 and .dds (AV16, D7, seed S32)") {
    CHECK_FALSE(isThumbnailDecodable("x.ktx2"));
    CHECK_FALSE(isThumbnailDecodable("x.dds"));
    // Still classified as Texture -- a strict subset, not a redefinition of the kind.
    CHECK(classifyAssetKind("x.ktx2", false) == AssetKind::Texture);
    CHECK(classifyAssetKind("x.dds", false) == AssetKind::Texture);
}

TEST_CASE("asset view: isThumbnailDecodable false for every non-Texture extension (AV17)") {
    CHECK_FALSE(isThumbnailDecodable("x.gltf"));
    CHECK_FALSE(isThumbnailDecodable("x.wav"));
    CHECK_FALSE(isThumbnailDecodable("x.txt"));
    CHECK_FALSE(isThumbnailDecodable("x.zip"));
}

TEST_CASE("asset view: isThumbnailDecodable is ASCII-case-insensitive (AV18)") { CHECK(isThumbnailDecodable("x.PNG")); }

TEST_CASE("asset view: iconLabelFor(\"wood.png\") == \"PNG\" (AV19)") { CHECK(iconLabelFor("wood.png") == "PNG"); }

TEST_CASE("asset view: iconLabelFor(\"scene.gltf\") == \"GLTF\" (AV20)") {
    CHECK(iconLabelFor("scene.gltf") == "GLTF");
}

TEST_CASE("asset view: iconLabelFor(\"model.blend1\") == \"BLE1\" -- truncated to 4 (AV21, seed S18)") {
    CHECK(iconLabelFor("model.blend1") == "BLE1");
}

TEST_CASE("asset view: iconLabelFor falls back to FILE (AV22)") {
    CHECK(iconLabelFor("README") == "FILE");
    CHECK(iconLabelFor("x.") == "FILE");
}

TEST_CASE("asset view: iconLabelFor on a non-ASCII extension is FILE (AV23)") {
    CHECK(iconLabelFor("x.p\xC3\xB1g") == "FILE");
}

TEST_CASE("asset view: iconLabelFor is always <= 4 chars and ASCII uppercase (AV24)") {
    constexpr std::array<std::string_view, 20> NAMES{
        "a.png",  "a.jpeg", "a.gltf", "a.blend", "a.blend2", "a.wav", "a.mp3", "a.hlsl", "a.glsl", "a.md",
        "a.json", "a.dds",  "a.ktx2", "a.tga",   "a.bmp",    "a.gif", "a.hdr", "a.psd",  "a.fbx",  "a.stl"};
    for (const std::string_view name : NAMES) {
        const std::string label = iconLabelFor(name);
        INFO("name: ", std::string(name), " label: ", label);
        CHECK(label.size() <= 4);
        for (const char c : label) {
            const auto uc = static_cast<unsigned char>(c);
            CHECK(((uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9')));
        }
    }
}

TEST_CASE("asset view: iconColorFor is total and stable across calls (AV25)") {
    constexpr std::array<AssetKind, 7> KINDS{AssetKind::Folder, AssetKind::Texture, AssetKind::Model,
                                             AssetKind::Audio,  AssetKind::Text,    AssetKind::Material,
                                             AssetKind::Unknown};
    for (const AssetKind kind : KINDS) {
        const auto c1 = iconColorFor(kind);
        const auto c2 = iconColorFor(kind);
        CHECK(c1.r == c2.r);
        CHECK(c1.g == c2.g);
        CHECK(c1.b == c2.b);
        CHECK(c1.a == c2.a);
    }
}

TEST_CASE("asset view: iconColorFor gives distinct colours to every kind (AV26)") {
    constexpr std::array<AssetKind, 7> KINDS{AssetKind::Folder, AssetKind::Texture, AssetKind::Model,
                                             AssetKind::Audio,  AssetKind::Text,    AssetKind::Material,
                                             AssetKind::Unknown};
    for (std::size_t i = 0; i < KINDS.size(); ++i) {
        for (std::size_t j = i + 1; j < KINDS.size(); ++j) {
            const auto a = iconColorFor(KINDS[i]);
            const auto b = iconColorFor(KINDS[j]);
            const bool distinct = a.r != b.r || a.g != b.g || a.b != b.b;
            INFO("i: ", i, " j: ", j);
            CHECK(distinct);
        }
    }
}

TEST_CASE("asset view: assetKindLabel is total and non-empty (AV27)") {
    constexpr std::array<AssetKind, 7> KINDS{AssetKind::Folder, AssetKind::Texture, AssetKind::Model,
                                             AssetKind::Audio,  AssetKind::Text,    AssetKind::Material,
                                             AssetKind::Unknown};
    for (const AssetKind kind : KINDS) {
        CHECK_FALSE(assetKindLabel(kind).empty());
    }
}

TEST_CASE("asset view: tileEdgeFontMultiple is strictly increasing and positive (AV28)") {
    const float small = tileEdgeFontMultiple(TileSize::Small);
    const float medium = tileEdgeFontMultiple(TileSize::Medium);
    const float large = tileEdgeFontMultiple(TileSize::Large);
    CHECK(small > 0.0F);
    CHECK(medium > small);
    CHECK(large > medium);
}

TEST_CASE("asset view: gridColumnsFor(0, 64, 8) == 1 (AV29, seed S16)") {
    CHECK(gridColumnsFor(0.0F, 64.0F, 8.0F) == 1);
}

TEST_CASE("asset view: gridColumnsFor(-100, 64, 8) == 1 (AV30, seed S16)") {
    CHECK(gridColumnsFor(-100.0F, 64.0F, 8.0F) == 1);
}

TEST_CASE("asset view: gridColumnsFor is NaN/inf-safe (AV31, seed S16)") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(gridColumnsFor(nan, 64.0F, 8.0F) == 1);
    CHECK(gridColumnsFor(inf, 64.0F, 8.0F) >= 1);
}

TEST_CASE("asset view: gridColumnsFor is division-safe (AV32, seed S16)") {
    CHECK(gridColumnsFor(500.0F, 0.0F, 0.0F) == 1);
}

TEST_CASE("asset view: gridColumnsFor boundary widths (AV33)") {
    // one tile wide exactly: availWidth == tileWidth -> 1 column.
    CHECK(gridColumnsFor(64.0F, 64.0F, 8.0F) == 1);
    // exactly two tiles wide (tileWidth*2 + spacing): 2 columns.
    CHECK(gridColumnsFor((64.0F * 2.0F) + 8.0F, 64.0F, 8.0F) == 2);
    // one pixel short of two tiles: still 1 column.
    CHECK(gridColumnsFor((64.0F * 2.0F) + 8.0F - 1.0F, 64.0F, 8.0F) == 1);
}

TEST_CASE("asset view: matchesFilter is ASCII-case-insensitive on the query (AV34)") {
    AssetFilter filter;
    filter.query = "WOOD";
    CHECK(matchesFilter("wood.png", false, filter));
}

TEST_CASE("asset view: an empty query matches everything with anyKind (AV35)") {
    const AssetFilter filter;
    CHECK(matchesFilter("anything.xyz", false, filter));
    CHECK(matchesFilter("folder", true, filter));
}

TEST_CASE("asset view: matchesFilter over the LEAF only, never the parent path (AV36, D5, seed S14)") {
    AssetFilter filter;
    filter.query = "wood";
    CHECK_FALSE(matchesFilter("plank.png", false, filter));
}

TEST_CASE("asset view: every kind arm filters correctly; a directory matches only Folder (AV37)") {
    AssetFilter filter;
    filter.anyKind = false;
    filter.kind = AssetKind::Texture;
    CHECK(matchesFilter("x.png", false, filter));
    CHECK_FALSE(matchesFilter("x.wav", false, filter));
    CHECK_FALSE(matchesFilter("x.png", true, filter));  // it's a DIRECTORY -- Folder, not Texture

    filter.kind = AssetKind::Folder;
    CHECK(matchesFilter("anything", true, filter));
    CHECK_FALSE(matchesFilter("anything.png", false, filter));
}

TEST_CASE("asset view: anyKind = true ignores kind entirely (AV38)") {
    AssetFilter filter;
    filter.anyKind = true;
    filter.kind = AssetKind::Audio;  // deliberately mismatched -- must be ignored
    CHECK(matchesFilter("x.png", false, filter));
}

TEST_CASE("asset view: searchAssets preserves order and fills recordIndex (AV39)") {
    std::vector<AssetRecord> records(3);
    records[0].relativePath = "a/one.png";
    records[1].relativePath = "b/two.png";
    records[2].relativePath = "c/three.png";
    AssetFilter filter;
    filter.query = "png";
    const SearchResult result = searchAssets(std::span<const AssetRecord>(records), filter);
    REQUIRE(result.hits.size() == 3);
    CHECK(result.hits[0].relativePath == "a/one.png");
    CHECK(result.hits[0].recordIndex == 0);
    CHECK(result.hits[1].relativePath == "b/two.png");
    CHECK(result.hits[1].recordIndex == 1);
    CHECK(result.hits[2].relativePath == "c/three.png");
    CHECK(result.hits[2].recordIndex == 2);
}

TEST_CASE("asset view: searchAssets matches the LEAF, never a parent directory segment (AV39b, D5, seed S14)") {
    // AV36 proves matchesFilter itself never fuzzy-matches a query against unrelated bytes -- it does
    // NOT exercise searchAssets' own leaf-extraction call site, since it calls matchesFilter directly
    // with an already-bare leaf. This case closes that gap: "wood" is a real PARENT DIRECTORY segment
    // of the one record below, and must NOT match, because searchAssets is documented (D5) to search
    // leaves only. Found by direct sabotage (S14: passing record.relativePath instead of the leaf to
    // matchesFilter) -- the existing suite stayed 739/739 green against that seed before this case was
    // added, which is exactly the coverage gap this closes.
    std::vector<AssetRecord> records(1);
    records[0].relativePath = "wood/plank.png";
    AssetFilter filter;
    filter.query = "wood";
    const SearchResult result = searchAssets(std::span<const AssetRecord>(records), filter);
    CHECK(result.hits.empty());
    CHECK(result.total == 0);
}

TEST_CASE("asset view: total is UNCAPPED while hits is capped (AV40, AC-14, seed S15)") {
    std::vector<AssetRecord> records(5);
    for (std::size_t i = 0; i < records.size(); ++i) {
        records[i].relativePath = "dir/" + std::to_string(i) + ".png";
    }
    AssetFilter filter;
    filter.query = "png";
    const SearchResult result = searchAssets(std::span<const AssetRecord>(records), filter, /*cap=*/2);
    CHECK(result.hits.size() == 2);
    CHECK(result.total == 5);
    CHECK(result.truncated);
}

TEST_CASE("asset view: an empty span is zero hits, zero total, not truncated (AV41)") {
    const std::vector<AssetRecord> records;
    const AssetFilter filter;
    const SearchResult result = searchAssets(std::span<const AssetRecord>(records), filter);
    CHECK(result.hits.empty());
    CHECK(result.total == 0);
    CHECK_FALSE(result.truncated);
}

TEST_CASE("asset view: cap == 0 gives zero hits but the full total, and truncated (AV42, AC-14)") {
    std::vector<AssetRecord> records(3);
    for (std::size_t i = 0; i < records.size(); ++i) {
        records[i].relativePath = std::to_string(i) + ".txt";
    }
    const AssetFilter filter;
    const SearchResult result = searchAssets(std::span<const AssetRecord>(records), filter, /*cap=*/0);
    CHECK(result.hits.empty());
    CHECK(result.total == 3);
    CHECK(result.truncated);
}

// ---- filterEntriesByKind: code-review BLOCKING-2 (AC-13's first clause) -------------------------

TEST_CASE("asset view: filterEntriesByKind with anyKind returns every index, in order (AV43)") {
    std::vector<FileEntry> entries(3);
    entries[0].name = "wood.png";
    entries[1].name = "tex";
    entries[1].isDirectory = true;
    entries[2].name = "readme.txt";
    const AssetFilter filter;  // anyKind == true (the default)
    const std::vector<std::size_t> indices = filterEntriesByKind(std::span<const FileEntry>(entries), filter);
    REQUIRE(indices.size() == 3);
    CHECK(indices[0] == 0);
    CHECK(indices[1] == 1);
    CHECK(indices[2] == 2);
}

TEST_CASE("asset view: filterEntriesByKind(Texture) keeps only Texture files, and NO directory (AV44, AC-13)") {
    std::vector<FileEntry> entries(4);
    entries[0].name = "wood.png";
    entries[1].name = "tex";
    entries[1].isDirectory = true;  // classifies Folder, never Texture -- matchesFilter's own rule
    entries[2].name = "readme.txt";
    entries[3].name = "stone.jpg";
    AssetFilter filter;
    filter.anyKind = false;
    filter.kind = AssetKind::Texture;
    const std::vector<std::size_t> indices = filterEntriesByKind(std::span<const FileEntry>(entries), filter);
    REQUIRE(indices.size() == 2);
    CHECK(indices[0] == 0);  // wood.png
    CHECK(indices[1] == 3);  // stone.jpg
}

TEST_CASE("asset view: filterEntriesByKind(Folder) keeps ONLY directories (AV45)") {
    std::vector<FileEntry> entries(3);
    entries[0].name = "wood.png";
    entries[1].name = "tex";
    entries[1].isDirectory = true;
    entries[2].name = "sfx";
    entries[2].isDirectory = true;
    AssetFilter filter;
    filter.anyKind = false;
    filter.kind = AssetKind::Folder;
    const std::vector<std::size_t> indices = filterEntriesByKind(std::span<const FileEntry>(entries), filter);
    REQUIRE(indices.size() == 2);
    CHECK(indices[0] == 1);
    CHECK(indices[1] == 2);
}

TEST_CASE("asset view: filterEntriesByKind over an empty span is an empty result (AV46)") {
    const std::vector<FileEntry> entries;
    AssetFilter filter;
    filter.anyKind = false;
    filter.kind = AssetKind::Audio;
    CHECK(filterEntriesByKind(std::span<const FileEntry>(entries), filter).empty());
}

TEST_CASE(
    "asset view: filterEntriesByKind(Texture) with NO texture present is an empty result, not a crash "
    "(AV47)") {
    std::vector<FileEntry> entries(2);
    entries[0].name = "readme.txt";
    entries[1].name = "notes.md";
    AssetFilter filter;
    filter.anyKind = false;
    filter.kind = AssetKind::Texture;
    CHECK(filterEntriesByKind(std::span<const FileEntry>(entries), filter).empty());
}

// ---- task 3.4.2: .aeromat becomes a first-class kind (AV48-AV52, AC-1/AC-3) ----------------------

TEST_CASE("asset view: .aeromat classifies Material, in any ASCII case (AV48, AC-1, seed S1)") {
    CHECK((classifyAssetKind("wood.aeromat", false) == AssetKind::Material));
    CHECK((classifyAssetKind("WOOD.AEROMAT", false) == AssetKind::Material));
    CHECK((classifyAssetKind("Wood.AeroMat", false) == AssetKind::Material));
    // A DIRECTORY called "materials.aeromat" is still a Folder -- the isDirectory arm wins first.
    CHECK((classifyAssetKind("materials.aeromat", true) == AssetKind::Folder));
}

TEST_CASE("asset view: .mtl STAYS Unknown -- the 3.2.3 fact, re-pinned (AV49)") {
    // Recorded at task 3.2.3: a Wavefront material library is a CLAIMED IMPORTABLE FILE but not a
    // browser kind, so it is invisible while the kind filter is set to Model. This task touches the
    // same table, so the fact is pinned here rather than left to be re-derived.
    CHECK((classifyAssetKind("chair.mtl", false) == AssetKind::Unknown));
    CHECK((classifyAssetKind("chair.aeromat", false) == AssetKind::Material));
}

TEST_CASE("asset view: assetKindLabel answers \"Material\" (AV50, AC-1)") {
    CHECK(assetKindLabel(AssetKind::Material) == std::string_view("Material"));
    CHECK(assetKindLabel(AssetKind::Unknown) == std::string_view("Unknown"));
}

TEST_CASE("asset view: the icon LABEL for .aeromat is unchanged at \"AERT\" (AV51)") {
    // iconLabelFor is extension-derived and kind-INDEPENDENT: seven characters take the >4 branch, so
    // "aeromat" becomes ext[0..2] + ext.back(). AC-1 moves the COLOUR, never the label -- pinned so a
    // future label change is visible rather than surprising.
    CHECK(iconLabelFor("x.aeromat") == std::string("AERT"));
}

TEST_CASE("asset view: .aeromat is NOT thumbnail-decodable (AV52, AC-3)") {
    // isThumbnailDecodable is deliberately untouched by this task: the two tables are separate by
    // recorded design, and a rendered material-ball thumbnail is a named deferral with no owner.
    CHECK_FALSE(isThumbnailDecodable("x.aeromat"));
    CHECK_FALSE(isThumbnailDecodable("X.AEROMAT"));
}

TEST_CASE("asset view: the kind-filter option list is EVERY kind, in enum order (AV53, seed S2)") {
    // THE CLOSURE FOR A MEASURED GAP. This list used to be a `constexpr std::array` local inside
    // AssetBrowserPanel::drawHeader, where no tier in this tree could reach it: seed S2 dropped
    // AssetKind::Material from it -- making materials unfilterable in the only UI that offers the
    // filter -- and all 1570 shell cases and all 119 GPU cases stayed green. No runtime tier can read
    // a combo's contents, so the fix was to delete the second copy rather than to test the panel: the
    // list now lives beside the enum and this case is its pin.
    constexpr std::array<AssetKind, 7> EXPECTED{AssetKind::Folder, AssetKind::Texture, AssetKind::Model,
                                                AssetKind::Audio,  AssetKind::Text,    AssetKind::Material,
                                                AssetKind::Unknown};
    REQUIRE(ASSET_KIND_FILTER_OPTIONS.size() == EXPECTED.size());
    for (std::size_t i = 0; i < EXPECTED.size(); ++i) {
        CAPTURE(i);
        CHECK((ASSET_KIND_FILTER_OPTIONS[i] == EXPECTED[i]));
    }
    // ENUM ORDER is load-bearing, not cosmetic: the filter action encodes the kind as
    // static_cast<int>(kind) and decodes it as path[0] - '0', so a list whose order disagrees with the
    // enum would still round-trip -- it is the COMBO that would show the wrong name beside each row.
    for (std::size_t i = 0; i < ASSET_KIND_FILTER_OPTIONS.size(); ++i) {
        CAPTURE(i);
        CHECK(static_cast<std::size_t>(ASSET_KIND_FILTER_OPTIONS[i]) == i);
        CHECK_FALSE(assetKindLabel(ASSET_KIND_FILTER_OPTIONS[i]).empty());
    }
}

TEST_CASE("asset view: the Material kind FILTERS, both predicate and wiring (AV54, task 3.4.2 AC-2)") {
    // AC-2 was covered only TRANSITIVELY until the code-review round: AV53 pins that Material is in the
    // combo's option list and the classification cases pin that a .aeromat classifies Material, but
    // nothing drove the two functions that actually decide what a chosen kind SHOWS. Those are the
    // filter's whole implementation, and both take AssetKind by value -- so a new enumerator is exactly
    // the input neither had ever been given.
    CHECK(matchesFilter("brass.aeromat", false, AssetFilter{.kind = AssetKind::Material, .anyKind = false}));
    CHECK_FALSE(matchesFilter("wood.png", false, AssetFilter{.kind = AssetKind::Material, .anyKind = false}));
    // A DIRECTORY only ever matches Folder, whatever it is called -- matchesFilter's own rule, restated
    // for the new kind because "a folder named like a material" is the case that would break it.
    CHECK_FALSE(matchesFilter("brass.aeromat", true, AssetFilter{.kind = AssetKind::Material, .anyKind = false}));
    // And a .aeromat is NOT swept up by any other specific kind -- Unknown included, which is where it
    // used to land and where a reverted classification arm would put it back.
    CHECK_FALSE(matchesFilter("brass.aeromat", false, AssetFilter{.kind = AssetKind::Unknown, .anyKind = false}));
    CHECK_FALSE(matchesFilter("brass.aeromat", false, AssetFilter{.kind = AssetKind::Texture, .anyKind = false}));
    // The query half still composes: the kind narrows, the substring narrows further, and both apply.
    CHECK(matchesFilter("brass.aeromat", false,
                        AssetFilter{.query = "bra", .kind = AssetKind::Material, .anyKind = false}));
    CHECK_FALSE(matchesFilter("brass.aeromat", false,
                              AssetFilter{.query = "steel", .kind = AssetKind::Material, .anyKind = false}));

    // The WIRING (the code-review BLOCKING-2 that AV43-AV46 exist for), driven with the new kind: the
    // panel's non-search path filters a listing through this, in the listing's own order.
    std::vector<FileEntry> entries(4);
    entries[0].name = "brass.aeromat";
    entries[1].name = "materials";
    entries[1].isDirectory = true;  // classifies Folder, never Material
    entries[2].name = "wood.png";
    entries[3].name = "steel.AEROMAT";  // case-folded, like every other extension table
    const AssetFilter filter{.kind = AssetKind::Material, .anyKind = false};
    const std::vector<std::size_t> indices = filterEntriesByKind(std::span<const FileEntry>(entries), filter);
    REQUIRE(indices.size() == 2);
    CHECK(indices[0] == 0);
    CHECK(indices[1] == 3);
}

// ---- task E.4.4: isBrowserVisibleName (BV1-BV9) ---------------------------------------------------
//
// What the Asset Browser's directory listing KEEPS. The predicate is pure, so a case here that passes
// isDirectory = true asserts the right thing BY CONSTRUCTION -- which is why I228 drives a real
// roster-named directory through the real panel. Every case name ENDS with its id and ")".

namespace {

// One name per scannable kind, the Blender ASSET itself, and a name with no extension.
constexpr std::array<std::string_view, 12> ORDINARY_ASSETS = {
    "wood.png",  "hero.glb", "scene.gltf", "a.fbx",    "model.blend", "clip.wav",
    "m.aeromat", "t.ktx2",   "x.hlsl",     "notes.md", "README",      "level.scene.json",
};

// The roster's FILE forms, built from the arrays so a deleted entry shrinks the corpus.
[[nodiscard]] std::vector<std::string> rosterFileNames() {
    std::vector<std::string> names;
    names.reserve(IGNORED_ASSET_NAMES.size() + IGNORED_ASSET_NAME_SUFFIXES.size() + 2U);
    for (const std::string_view entry : IGNORED_ASSET_NAMES) {
        names.emplace_back(entry);
    }
    for (const std::string_view suffix : IGNORED_ASSET_NAME_SUFFIXES) {
        names.push_back("wood.png" + std::string(suffix));
    }
    names.emplace_back("scene.blend1");
    names.emplace_back("scene.blend32");
    return names;
}

// One line with its `//` comment removed -- the editorSourceCodeLines rule (imgui_layer_test.cpp): a
// citation in PROSE must never satisfy or break a gate about CODE.
[[nodiscard]] std::string withoutLineComment(std::string line) {
    if (const std::size_t comment = line.find("//"); comment != std::string::npos) {
        line.erase(comment);
    }
    return line;
}

[[nodiscard]] bool namesARosterPredicate(const std::string& codeLine) {
    return codeLine.find("isIgnoredAssetName") != std::string::npos ||
           codeLine.find("isBrowserVisibleName") != std::string::npos;
}

}  // namespace

TEST_CASE("asset view: a DIRECTORY is always browser-visible, whatever it is called (AC-9, seed S6, BV1)") {
    std::vector<std::string> names = rosterFileNames();
    names.emplace_back("wood.png.meta");  // a folder NAMED like a sidecar is still a folder
    names.emplace_back("old~");
    names.emplace_back("backup.bak");
    for (const std::string_view suffix : IGNORED_ASSET_NAME_SUFFIXES) {
        names.emplace_back(suffix);  // a folder named exactly ".bak" or "~"
    }
    REQUIRE(names.size() > 15U);  // anti-vacuity: the roster really contributed
    for (const std::string& name : names) {
        CAPTURE(name);
        // A REQUIRE: every other browser claim is moot if the editor can lose a folder the user made.
        REQUIRE(isBrowserVisibleName(name, true));
    }
}

TEST_CASE("asset view: a FILE on the roster is not browser-visible (AC-11, BV2)") {
    const std::vector<std::string> names = rosterFileNames();
    REQUIRE(names.size() > IGNORED_ASSET_NAME_SUFFIXES.size());
    for (const std::string& name : names) {
        CAPTURE(name);
        CHECK_FALSE(isBrowserVisibleName(name, false));
    }
}

TEST_CASE("asset view: a sidecar FILE is not browser-visible, in any case (BV3)") {
    CHECK_FALSE(isBrowserVisibleName("wood.png.meta", false));
    CHECK_FALSE(isBrowserVisibleName("wood.png.META", false));
    CHECK_FALSE(isBrowserVisibleName("scene.blend1.meta", false));  // the sidecar OF an ignored file
}

TEST_CASE("asset view: an ordinary asset file is browser-visible (BV4)") {
    for (const std::string_view name : ORDINARY_ASSETS) {
        CAPTURE(name);
        CHECK(isBrowserVisibleName(name, false));
    }
}

TEST_CASE("asset view: a HIDDEN file is browser-visible -- listDirectory owns that rule (AC-10, seed S7, BV5)") {
    // Restating the hidden rule here would make the panel's `Show hidden` checkbox do nothing at all:
    // listDirectory(root, rel, showHidden) already drops dotfiles unless it is ticked.
    CHECK(isBrowserVisibleName(".DS_Store", false));
    CHECK(isBrowserVisibleName(".hidden.png", false));
    CHECK(isBrowserVisibleName(".gitignore", false));
}

TEST_CASE("asset view: file visibility is exactly not-a-sidecar and not-ignored (BV6)") {
    std::vector<std::string> names = rosterFileNames();
    names.insert(names.end(), ORDINARY_ASSETS.begin(), ORDINARY_ASSETS.end());
    for (const std::string_view extra :
         {std::string_view("wood.png.meta"), std::string_view(".DS_Store"), std::string_view(".meta"),
          std::string_view("a.blend1x"), std::string_view("MyThumbs.dbFile.png"), std::string_view("")}) {
        names.emplace_back(extra);
    }
    std::size_t visible = 0;
    std::size_t dropped = 0;
    for (const std::string& name : names) {
        CAPTURE(name);
        const bool expected = !isMetaFileName(name) && !isIgnoredAssetName(name);
        CHECK(isBrowserVisibleName(name, false) == expected);
        if (expected) {
            ++visible;
        } else {
            ++dropped;
        }
    }
    CHECK(visible >= ORDINARY_ASSETS.size());  // anti-vacuity: the corpus straddles the predicate
    CHECK(dropped > IGNORED_ASSET_NAME_SUFFIXES.size());
}

TEST_CASE("asset view: the empty name has a DEFINED answer, though no listing yields it (BV7)") {
    // listDirectory skips an empty leaf and counts it (project_files.cpp:307-308), so this is unreachable
    // from the panel; a defined answer still beats an undefined one.
    CHECK(isBrowserVisibleName("", false));
    CHECK(isBrowserVisibleName("", true));
}

TEST_CASE("asset view: every scannable name is visible as a file, and not the converse (BV8)") {
    std::vector<std::string> names = rosterFileNames();
    names.insert(names.end(), ORDINARY_ASSETS.begin(), ORDINARY_ASSETS.end());
    std::size_t scannable = 0;
    for (const std::string& name : names) {
        if (isScannableAssetName(name)) {
            ++scannable;
            CAPTURE(name);
            CHECK(isBrowserVisibleName(name, false));
        }
    }
    CHECK(scannable >= ORDINARY_ASSETS.size());  // anti-vacuity: the implication was exercised
    // The CONVERSE is false, and asserted rather than noted: a dotfile is visible (when Show hidden lets
    // listDirectory return it) yet never scannable.
    CHECK(isBrowserVisibleName(".DS_Store", false));
    CHECK_FALSE(isScannableAssetName(".DS_Store"));
}

TEST_CASE("asset view: only six editor files name the roster predicates (AC-18, seed S16, BV9)") {
    // THE NAME-FREEDOM PIN (D9). EditorApp's New Material path lists a folder UNFILTERED -- "a hidden file
    // still owns its name" (editor_app.cpp) -- and an IGNORED file owns its name exactly as hard. A filter
    // added there would let uniqueMaterialFileName return a name that already exists, and
    // writeTextFileAtomic would rename the default document over a user's file with no warning and no
    // undo. No runtime witness can exist: no roster name can collide with NewMaterial<N>.aeromat. So the
    // pin is a SET of the files allowed to name either predicate, and its ABSENCE of editor_app.cpp is the
    // claim. A set, never a `<=` bound (E.2.4 measured that a bound hides the seed in both directions).
    //
    // Its limit, stated: a name-freedom filter spelled with isScannableAssetName or isMetaFileName is not
    // caught here -- those predicates predate this task and are named in many files.
    //
    // (a) The reader's own self-test: a code token counts; the same token in a comment does not.
    CHECK(namesARosterPredicate(withoutLineComment("    return isIgnoredAssetName(name);")));
    CHECK_FALSE(namesARosterPredicate(withoutLineComment("    // never isBrowserVisibleName here")));
    CHECK_FALSE(namesARosterPredicate(withoutLineComment("    return isMetaFileName(name);")));

    // (b) The sweep: BOTH editor roots, .cpp and .hpp, non-recursive (neither root has subdirectories).
    const std::filesystem::path src{AERO_EDITOR_SRC_DIR};
    const std::filesystem::path include = src.parent_path() / "include" / "aero" / "editor";
    std::vector<std::string> naming;
    std::size_t srcScanned = 0;
    std::size_t includeScanned = 0;
    for (const std::filesystem::path& root : {src, include}) {
        std::error_code ec;
        REQUIRE(std::filesystem::is_directory(root, ec));
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root, ec)) {
            const std::string extension = entry.path().extension().string();
            if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".hpp")) {
                continue;
            }
            if (root == src) {
                ++srcScanned;
            } else {
                ++includeScanned;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            REQUIRE(in.good());
            std::string line;
            while (std::getline(in, line)) {
                if (namesARosterPredicate(withoutLineComment(line))) {
                    naming.push_back(entry.path().filename().string());
                    break;
                }
            }
        }
        REQUIRE_FALSE(ec);
    }
    std::sort(naming.begin(), naming.end());
    naming.erase(std::unique(naming.begin(), naming.end()), naming.end());
    std::string joined;
    for (const std::string& name : naming) {
        joined += name + " ";
    }
    INFO("files naming a roster predicate in code: ", joined);
    // asset_actions.cpp joined at task E.4.4's Commit 3a: validateAssetName refuses an ignored TARGET name,
    // which is the opposite question from name freedom and the one legitimate reason to name the roster there.
    const std::vector<std::string> expected{"asset_actions.cpp", "asset_browser_panel.cpp", "asset_meta.cpp",
                                            "asset_meta.hpp",    "asset_view.cpp",          "asset_view.hpp"};
    CHECK(naming == expected);
    CHECK(srcScanned > 80U);      // anti-vacuity: 115 files under editor/src at the branch point
    CHECK(includeScanned > 40U);  // anti-vacuity: 64 headers under editor/include/aero/editor
}

// ---- AV55-AV59: task E.4.4's validation finding 2 -- the Asset Browser's vertical budget -------------
//
// assetBrowserLayout is the whole of the arithmetic; AssetBrowserPanel::onDraw only reads the metrics
// and passes the answer on. I231 (imgui_layer_test.cpp) is the ImGui-tier witness that the panel really
// does fit; these cases pin the contract the panel relies on, at the two metric sets this editor runs at.

namespace {

using engine::editor::ASSET_ISSUES_BODY_MAX_FRACTION;
using engine::editor::ASSET_PANES_MIN_FONT;
using engine::editor::AssetBrowserLayout;
using engine::editor::assetBrowserLayout;
using engine::editor::AssetBrowserLayoutMetrics;

// 1.92.8's defaults at 1x (a 13-point font, FramePadding.y 3, ItemSpacing.y 4) and on a Retina display,
// where ScaleAllSizes doubles the STYLE and not the font (imgui_layer.cpp:87-89, E.3.4's measurement).
constexpr AssetBrowserLayoutMetrics METRICS_1X{
    .availHeight = 0.0F, .fontSize = 13.0F, .frameHeight = 19.0F, .textLineHeight = 13.0F, .itemSpacingY = 4.0F};
constexpr AssetBrowserLayoutMetrics METRICS_2X{
    .availHeight = 0.0F, .fontSize = 13.0F, .frameHeight = 25.0F, .textLineHeight = 13.0F, .itemSpacingY = 8.0F};
// The smallest panel the budget fits with the body open: footer (spacing + frame) + header (frame +
// spacing) + one text row + the spacing after it + a four-point pane -- ImGui's smallest child window
// (IMGUI_WINDOW_HARD_MIN_SIZE) since task E.6.3's code-review round, a one-point pane (64 and 88) before, which
// ImGui drew 4 tall. Spelled as LITERALS so this is the header's stated contract, never the implementation's
// arithmetic re-run.
constexpr float FIT_BOUND_1X = 67.0F;  // 23 + 23 + 13 + 4 + 4
constexpr float FIT_BOUND_2X = 91.0F;  // 33 + 33 + 13 + 8 + 4

[[nodiscard]] AssetBrowserLayoutMetrics withState(AssetBrowserLayoutMetrics metrics, float avail, bool shown, bool open,
                                                  float content) {
    metrics.availHeight = avail;
    metrics.issuesShown = shown;
    metrics.issuesOpen = open;
    metrics.issuesContentHeight = content;
    return metrics;
}

[[nodiscard]] float budgetSum(const AssetBrowserLayout& layout) {
    return layout.paneHeight + layout.issuesHeight + layout.footerHeight;
}

// Exact on purpose: a sanitised metric must behave EXACTLY like 0, not approximately -- the mode included since task
// E.6.3's code-review round folds the crumb row (crumbRowShown, AV67).
[[nodiscard]] bool sameLayout(const AssetBrowserLayout& a, const AssetBrowserLayout& b) {
    return a.paneHeight == b.paneHeight && a.issuesHeight == b.issuesHeight &&
           a.issuesBodyHeight == b.issuesBodyHeight && a.footerHeight == b.footerHeight &&
           a.crumbRowShown == b.crumbRowShown;
}

}  // namespace

TEST_CASE("asset view: panes + Issues + footer fill the panel exactly whenever the floors allow (AV55)") {
    // Whole and dyadic-fraction heights keep every sum here exact, so the claim is ==, not <=: the panes
    // absorb the remainder, so nothing below them is pushed past the bottom and nothing is left unused.
    std::vector<float> avails;
    for (float avail = 0.0F; avail <= 600.0F; avail += 1.0F) {
        avails.push_back(avail);
    }
    for (const float fractional : {195.25F, 333.5F, 1000.75F, 4096.0F}) {  // a dock split is rarely whole
        avails.push_back(fractional);
    }
    struct Case {
        const char* name;
        AssetBrowserLayoutMetrics metrics;
        float bound;
    };
    for (const Case& c : {Case{"1x", METRICS_1X, FIT_BOUND_1X}, Case{"2x", METRICS_2X, FIT_BOUND_2X}}) {
        for (const bool shown : {false, true}) {
            for (const bool open : {false, true}) {
                for (const float content : {0.0F, 13.0F, 47.0F, 170.0F, 5000.0F}) {
                    CAPTURE(c.name);
                    CAPTURE(shown);
                    CAPTURE(open);
                    CAPTURE(content);
                    std::size_t checked = 0;
                    std::size_t exact = 0;
                    float firstMiss = -1.0F;
                    for (const float avail : avails) {
                        if (avail < c.bound) {
                            continue;
                        }
                        ++checked;
                        const AssetBrowserLayout layout =
                            assetBrowserLayout(withState(c.metrics, avail, shown, open, content));
                        if (budgetSum(layout) == avail && layout.paneHeight >= 1.0F) {
                            ++exact;
                        } else if (firstMiss < 0.0F) {
                            firstMiss = avail;
                        }
                    }
                    CAPTURE(firstMiss);
                    CHECK(checked > 500U);  // anti-vacuity: the sweep really ran above the bound
                    CHECK(exact == checked);
                }
            }
        }
    }
}

TEST_CASE("asset view: the floors -- the body yields first, never below one row; the panes never below 4 (AV56)") {
    constexpr float CONTENT = 5000.0F;  // 40 orphans' worth and more: the body always WANTS its cap
    // (a) The panes keep ASSET_PANES_MIN_FONT font heights while the body can yield: at 150 the cap
    // (floor(0.4 * 150) = 60) would leave the panes 40, so the body gives up 12 and the panes keep 52.
    const AssetBrowserLayout yielding = assetBrowserLayout(withState(METRICS_1X, 150.0F, true, true, CONTENT));
    CHECK(ASSET_PANES_MIN_FONT * METRICS_1X.fontSize == 52.0F);
    CHECK(yielding.paneHeight == 52.0F);
    CHECK(yielding.issuesBodyHeight == 48.0F);
    CHECK(budgetSum(yielding) == 150.0F);
    // (b) ... but never below one text row: at 100 the body is 13 and the PANES go below their minimum.
    const AssetBrowserLayout oneRow = assetBrowserLayout(withState(METRICS_1X, 100.0F, true, true, CONTENT));
    CHECK(oneRow.issuesBodyHeight == 13.0F);
    CHECK(oneRow.paneHeight == 37.0F);
    CHECK(budgetSum(oneRow) == 100.0F);
    // (c) Below the fit bound nothing can fit, and the floors hold: the panes are 4 (BeginChild reads 0 as
    // "fill the window", and ImGui draws no child shorter than 4 -- 1 until task E.6.3's code-review round, which
    // ImGui drew 4 tall), the body one row, and only here does the budget exceed the panel.
    for (const float avail : {0.0F, 1.0F, 10.0F, FIT_BOUND_1X - 1.0F}) {
        CAPTURE(avail);
        const AssetBrowserLayout tiny = assetBrowserLayout(withState(METRICS_1X, avail, true, true, CONTENT));
        CHECK(tiny.paneHeight == 4.0F);
        CHECK(tiny.issuesBodyHeight == 13.0F);
        CHECK(tiny.issuesHeight == 23.0F + 13.0F + 4.0F);
        CHECK(tiny.footerHeight == 23.0F);
        CHECK(budgetSum(tiny) > avail);
    }
    // (d) A zero text-line height still yields a body of at least 1 -- a 0 would fill the window.
    AssetBrowserLayoutMetrics noLine = withState(METRICS_1X, 300.0F, true, true, 0.0F);
    noLine.textLineHeight = 0.0F;
    CHECK(assetBrowserLayout(noLine).issuesBodyHeight == 1.0F);
}

TEST_CASE("asset view: the Issues body is capped, and otherwise exactly as tall as its content (AV57)") {
    // The cap is floor(ASSET_ISSUES_BODY_MAX_FRACTION * avail): whole points.
    CHECK(ASSET_ISSUES_BODY_MAX_FRACTION == 0.4F);
    const AssetBrowserLayout capped = assetBrowserLayout(withState(METRICS_1X, 600.0F, true, true, 5000.0F));
    CHECK(capped.issuesBodyHeight == 240.0F);
    CHECK(capped.issuesBodyHeight < 5000.0F);  // anti-vacuity: the content really is taller than the cap
    CHECK(assetBrowserLayout(withState(METRICS_1X, 601.0F, true, true, 5000.0F)).issuesBodyHeight == 240.0F);
    CHECK(assetBrowserLayout(withState(METRICS_2X, 600.0F, true, true, 5000.0F)).issuesBodyHeight == 240.0F);
    // Under the cap the body is its content: three rows are 3 * 13 + 2 * 4 = 47 points.
    CHECK(assetBrowserLayout(withState(METRICS_1X, 600.0F, true, true, 47.0F)).issuesBodyHeight == 47.0F);
    // Before the first measurement (0), and for anything shorter than a row, it is one row.
    CHECK(assetBrowserLayout(withState(METRICS_1X, 600.0F, true, true, 0.0F)).issuesBodyHeight == 13.0F);
    CHECK(assetBrowserLayout(withState(METRICS_1X, 600.0F, true, true, 5.0F)).issuesBodyHeight == 13.0F);
    // A taller panel shows more rows: the cap scales with the panel, not with the font.
    CHECK(assetBrowserLayout(withState(METRICS_1X, 1000.0F, true, true, 5000.0F)).issuesBodyHeight == 400.0F);
}

TEST_CASE("asset view: a NaN, negative or infinite metric counts as zero (AV58)") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const AssetBrowserLayoutMetrics base = withState(METRICS_1X, 600.0F, true, true, 170.0F);
    // Each field, replaced one at a time: the answer must be EXACTLY the one for a 0 in that field. Seven since task
    // E.6.3's code-review round recorded paneChromeHeight (AV66 drives it at short heights, where its floor binds).
    using Field = float AssetBrowserLayoutMetrics::*;
    constexpr std::array<Field, 7> FIELDS{
        &AssetBrowserLayoutMetrics::availHeight,         &AssetBrowserLayoutMetrics::fontSize,
        &AssetBrowserLayoutMetrics::frameHeight,         &AssetBrowserLayoutMetrics::textLineHeight,
        &AssetBrowserLayoutMetrics::itemSpacingY,        &AssetBrowserLayoutMetrics::paneChromeHeight,
        &AssetBrowserLayoutMetrics::issuesContentHeight,
    };
    std::size_t checked = 0;
    for (std::size_t f = 0; f < FIELDS.size(); ++f) {
        AssetBrowserLayoutMetrics zeroed = base;
        zeroed.*FIELDS[f] = 0.0F;
        const AssetBrowserLayout expected = assetBrowserLayout(zeroed);
        for (const float bad : {nan, -nan, -5.0F, -inf, inf}) {
            CAPTURE(f);
            CAPTURE(bad);
            AssetBrowserLayoutMetrics broken = base;
            broken.*FIELDS[f] = bad;
            const AssetBrowserLayout layout = assetBrowserLayout(broken);
            CHECK(std::isfinite(layout.paneHeight));
            CHECK(std::isfinite(layout.issuesHeight));
            CHECK(std::isfinite(layout.issuesBodyHeight));
            CHECK(std::isfinite(layout.footerHeight));
            CHECK(layout.paneHeight >= 1.0F);
            CHECK(layout.issuesBodyHeight >= 1.0F);  // open, so drawn -- never the 0 that means "fill"
            CHECK(sameLayout(layout, expected));
            ++checked;
        }
    }
    CHECK(checked == 35U);  // 7 fields x 5 bad values
    // Anti-vacuity: the zeroed answers really differ from the healthy one, so "same as zero" is a claim.
    AssetBrowserLayoutMetrics zeroAvail = base;
    zeroAvail.availHeight = 0.0F;
    CHECK_FALSE(sameLayout(assetBrowserLayout(zeroAvail), assetBrowserLayout(base)));
    CHECK(assetBrowserLayout(zeroAvail).paneHeight == 4.0F);  // the floor: ImGui's smallest child (AV56)
}

TEST_CASE("asset view: no Issues means no reservation, and a closed header costs one line (AV59)") {
    // Not shown: today's reservation exactly -- one frame-height line plus spacing for the footer, and the
    // panes take the rest. A stale open flag with nothing to show changes nothing.
    const AssetBrowserLayout none = assetBrowserLayout(withState(METRICS_1X, 300.0F, false, false, 0.0F));
    CHECK(none.footerHeight == 19.0F + 4.0F);  // GetFrameHeightWithSpacing(), the pre-fix reservation
    CHECK(none.issuesHeight == 0.0F);
    CHECK(none.issuesBodyHeight == 0.0F);
    CHECK(none.paneHeight == 300.0F - 23.0F);
    CHECK(sameLayout(assetBrowserLayout(withState(METRICS_1X, 300.0F, false, true, 170.0F)), none));
    // Shown and closed: the header's own line, and no body.
    const AssetBrowserLayout closed = assetBrowserLayout(withState(METRICS_1X, 300.0F, true, false, 170.0F));
    CHECK(closed.issuesHeight == 19.0F + 4.0F);
    CHECK(closed.issuesBodyHeight == 0.0F);
    CHECK(closed.paneHeight == 300.0F - 23.0F - 23.0F);
    // Open: header + body + the spacing after the body.
    const AssetBrowserLayout open = assetBrowserLayout(withState(METRICS_1X, 300.0F, true, true, 47.0F));
    CHECK(open.issuesHeight == 23.0F + 47.0F + 4.0F);
    CHECK(open.paneHeight == 300.0F - 23.0F - 74.0F);
}

// ---- AV66: task E.6.3's code-review round (findings 7 and 19) -- the contents pane's chrome is RESERVED -----------
//
// E.6.3 moved the breadcrumb into the contents pane: ONE group beside the tree -- the crumb row, its rule, one
// ItemSpacing.y, then the ##contents child, which ImGui never draws shorter than 4 points (IMGUI_WINDOW_HARD_MIN_SIZE)
// -- so the group is never shorter than that chrome + 4. The budget used to floor the panes at 1 with no term for it,
// so a pane below 41 points (1x) pushed the Issues region and the footer past the bottom of the panel while the sum
// still claimed a fit -- the #111 defect again. The panel now RECORDS the chrome and the budget reserves it, with the
// child's 4: a floor of chrome + 1 still left a pane in [38, 41) up to 3 points short of its group (I231's 640
// subcase drew tree 38, group 41). The chrome is spelled as a literal here (37 = the crumb row's 24 + 2 x 3, a 1-point
// rule, 6) and tied to assetContentsPaneChromeHeight once per set; the child's minimum is a literal too.
//
// ROUND 2: a pane under the chrome + 4 no longer overflows -- the row FOLDS there (AV67), so every claim below about
// the panes' chrome floor is the SHOWN mode's, and each arm the fold changed says what it read before.
namespace {

constexpr float CHILD_MIN = 4.0F;  // IMGUI_WINDOW_HARD_MIN_SIZE (imgui_internal.h): ImGui's shortest child window

using engine::editor::assetContentsPaneChromeHeight;
using engine::editor::AssetContentsPaneLayout;
using engine::editor::assetContentsPaneLayout;

struct ChromeCase {
    std::string_view name;
    AssetBrowserLayoutMetrics metrics;
    float frame;  // assetContentsPaneLayout's inputs, as drawContentsPane passes them
    float rule;
    float spacing;
    float scale;
};

// Plex at 1x and 2x (E.6.1's metrics: body 16, frame 24, ItemSpacing.y 6, every one doubled at 2), and one set where
// the chrome + 4 is taller than ASSET_PANES_MIN_FONT font heights, so the body's yield must honour the chrome too.
constexpr AssetBrowserLayoutMetrics PLEX_1X{.availHeight = 0.0F,
                                            .fontSize = 16.0F,
                                            .frameHeight = 24.0F,
                                            .textLineHeight = 16.0F,
                                            .itemSpacingY = 6.0F,
                                            .paneChromeHeight = 37.0F};
constexpr AssetBrowserLayoutMetrics PLEX_2X{.availHeight = 0.0F,
                                            .fontSize = 32.0F,
                                            .frameHeight = 48.0F,
                                            .textLineHeight = 32.0F,
                                            .itemSpacingY = 12.0F,
                                            .paneChromeHeight = 74.0F};
constexpr AssetBrowserLayoutMetrics SMALL_FONT{.availHeight = 0.0F,
                                               .fontSize = 5.0F,  // 4 x 5 = 20 < 37 + 4
                                               .frameHeight = 24.0F,
                                               .textLineHeight = 16.0F,
                                               .itemSpacingY = 6.0F,
                                               .paneChromeHeight = 37.0F};

[[nodiscard]] AssetBrowserLayoutMetrics withChrome(AssetBrowserLayoutMetrics metrics, float chrome) {
    metrics.paneChromeHeight = chrome;
    return metrics;
}

}  // namespace

TEST_CASE("asset view: the panes hold the crumb row's chrome; the budget overflows exactly when it must (AV66)") {
    std::vector<float> avails;
    for (float avail = 0.0F; avail <= 600.0F; avail += 1.0F) {
        avails.push_back(avail);
    }
    for (const float fractional : {195.25F, 333.5F, 1000.75F, 4096.0F}) {
        avails.push_back(fractional);
    }
    struct State {
        bool shown;
        bool open;
    };
    constexpr std::array<State, 4> STATES{{{false, false}, {false, true}, {true, false}, {true, true}}};
    const std::array<ChromeCase, 3> cases{ChromeCase{"plex 1x", PLEX_1X, 24.0F, 1.0F, 6.0F, 1.0F},
                                          ChromeCase{"plex 2x", PLEX_2X, 48.0F, 2.0F, 12.0F, 2.0F},
                                          ChromeCase{"small font", SMALL_FONT, 24.0F, 1.0F, 6.0F, 1.0F}};
    for (const ChromeCase& c : cases) {
        CAPTURE(c.name);
        const float chrome = c.metrics.paneChromeHeight;
        // The recorded metric is the group's own chrome: what the panel records is what the pane subtracts.
        CHECK(assetContentsPaneChromeHeight(c.frame, c.rule, c.spacing, c.scale) == chrome);
        const float bodyFloor = c.metrics.textLineHeight;
        std::size_t checked = 0;
        std::size_t shown = 0;
        std::size_t folded = 0;
        std::size_t yielded = 0;  // open, with a body taller than its floor
        std::size_t misses = 0;
        float firstMiss = -1.0F;
        for (const State& s : STATES) {
            for (const float content : {0.0F, 16.0F, 47.0F, 170.0F, 5000.0F}) {
                for (const float avail : avails) {
                    const AssetBrowserLayout layout =
                        assetBrowserLayout(withState(c.metrics, avail, s.shown, s.open, content));
                    ++checked;
                    const bool rowFits = avail - layout.footerHeight - layout.issuesHeight >= chrome + CHILD_MIN;
                    bool ok = layout.crumbRowShown == rowFits;
                    if (rowFits) {
                        // SHOWN: the panes hold the chrome + 4 and absorb the remainder exactly.
                        ok = ok && layout.paneHeight >= chrome + CHILD_MIN && budgetSum(layout) == avail;
                        ++shown;
                    } else {
                        // FOLDED, under the row's floor (AV67). Until round 2 this arm read "the panes sit AT the
                        // chrome + 4 and the sum exceeds the panel" -- the scroll the fold removes.
                        ok = ok && layout.paneHeight < chrome + CHILD_MIN;
                        ++folded;
                    }
                    if (s.shown && s.open && layout.issuesBodyHeight > bodyFloor) {
                        ok = ok && rowFits;  // the body YIELDS before the row can fold
                        ++yielded;
                    }
                    // The group IS the pane the budget handed out: crumb row + rule + spacing + the child when the
                    // row shows, the child alone when it folds (AH18).
                    const AssetContentsPaneLayout group = assetContentsPaneLayout(
                        layout.paneHeight, c.frame, c.rule, c.spacing, c.scale, layout.crumbRowShown);
                    const float rowChrome = layout.crumbRowShown ? group.crumbRowHeight + c.rule + c.spacing : 0.0F;
                    ok = ok && rowChrome + group.contentsHeight == layout.paneHeight;
                    if (!ok) {
                        ++misses;
                        if (firstMiss < 0.0F) {
                            firstMiss = avail;
                        }
                    }
                }
            }
        }
        CAPTURE(firstMiss);
        CHECK(misses == 0U);
        CHECK(checked == 4U * 5U * 605U);
        CHECK(shown > 0U);    // anti-vacuity: both modes were visited ...
        CHECK(folded > 0U);   // ... including panes under the row's floor, where the group once overflowed
        CHECK(yielded > 0U);  // ... and an open body above its floor
    }

    // THE ROW'S BOUNDS at 1x, as literals -- the header's stated contract, never the arithmetic re-run. At the bound
    // the row shows over panes of exactly the chrome + 4 and the sum is the panel; one point below, the row FOLDS over
    // panes one point shorter and the sum is still the panel (until round 2 the panes stayed 41 there, one point over).
    struct Bound {
        bool shown;
        bool open;
        float avail;
    };
    for (const Bound b : {Bound{false, false, 71.0F},    // footer 30 + 37 + 4
                          Bound{true, false, 101.0F},    // + the closed header 30
                          Bound{true, true, 123.0F}}) {  // + one row 16 + spacing 6
        CAPTURE(b.shown);
        CAPTURE(b.open);
        const AssetBrowserLayout at = assetBrowserLayout(withState(PLEX_1X, b.avail, b.shown, b.open, 5000.0F));
        const float under = b.avail - 1.0F;
        const AssetBrowserLayout below = assetBrowserLayout(withState(PLEX_1X, under, b.shown, b.open, 5000.0F));
        CHECK(at.crumbRowShown);
        CHECK(at.paneHeight == 41.0F);
        CHECK(budgetSum(at) == b.avail);
        CHECK_FALSE(below.crumbRowShown);
        CHECK(below.paneHeight == 40.0F);
        CHECK(budgetSum(below) == under);  // the panel, one point shorter -- no longer one point over it
    }
    // The heights the findings named, every one FOLDED and fitting since round 2: CI's likely avail 104 with Issues
    // open (the panes were 22 before the reservation, then 41 with the sum 19 over), I231's 640-point window (avail 89,
    // closed: 29, then 41 with 12 over), and its 768-point one (avail 121, open: 41 with 2 over, which moved that
    // subcase to 824 until the fold brought it back).
    const AssetBrowserLayout ci = assetBrowserLayout(withState(PLEX_1X, 104.0F, true, true, 5000.0F));
    CHECK_FALSE(ci.crumbRowShown);
    CHECK(ci.paneHeight == 22.0F);
    CHECK(budgetSum(ci) == 104.0F);
    const AssetBrowserLayout short640 = assetBrowserLayout(withState(PLEX_1X, 89.0F, true, false, 0.0F));
    CHECK_FALSE(short640.crumbRowShown);
    CHECK(short640.paneHeight == 29.0F);
    CHECK(budgetSum(short640) == 89.0F);
    const AssetBrowserLayout short768 = assetBrowserLayout(withState(PLEX_1X, 121.0F, true, true, 5000.0F));
    CHECK_FALSE(short768.crumbRowShown);
    CHECK(short768.paneHeight == 39.0F);
    CHECK(budgetSum(short768) == 121.0F);
    // The 824-point window the short subcase ran at in between: the row shows there, over the one-row body.
    const AssetBrowserLayout short824 = assetBrowserLayout(withState(PLEX_1X, 135.0F, true, true, 5000.0F));
    CHECK(short824.crumbRowShown);
    CHECK(short824.issuesBodyHeight == 16.0F);
    CHECK(short824.paneHeight == 53.0F);
    CHECK(budgetSum(short824) == 135.0F);
    // THE COST the reservation had, and the fold removed: the editor's own 1280 x 720 launch (editor/src/main.cpp)
    // gives the default layout's panel avail 109 (measured through I231's fixture). Closed it fits with the row shown;
    // with the Issues list open the sum was 14 over and the panel scrolled (by 6, measured) where main's fit. Now the
    // row folds there and the panes take the 27 points left (AV67 pins both arms beside its fit bounds).
    const AssetBrowserLayout launchClosed = assetBrowserLayout(withState(PLEX_1X, 109.0F, true, false, 0.0F));
    CHECK(launchClosed.crumbRowShown);
    CHECK(launchClosed.paneHeight == 49.0F);
    CHECK(budgetSum(launchClosed) == 109.0F);
    const AssetBrowserLayout launchOpen = assetBrowserLayout(withState(PLEX_1X, 109.0F, true, true, 5000.0F));
    CHECK_FALSE(launchOpen.crumbRowShown);
    CHECK(launchOpen.paneHeight == 27.0F);
    CHECK(budgetSum(launchOpen) == 109.0F);
    // The chrome term in the panes' minimum: at 130 the body yields to 23, leaving the panes exactly 41 with the row
    // shown -- without the term it would take 44 and leave 20, and fold the row the body could have made room for.
    const AssetBrowserLayout yielding = assetBrowserLayout(withState(SMALL_FONT, 130.0F, true, true, 5000.0F));
    CHECK(yielding.crumbRowShown);
    CHECK(yielding.issuesBodyHeight == 23.0F);
    CHECK(yielding.paneHeight == 41.0F);
    CHECK(budgetSum(yielding) == 130.0F);

    // A TALL panel is unchanged: the chrome is drawn INSIDE the panes, never subtracted from them.
    for (const State& s : STATES) {
        CAPTURE(s.shown);
        CAPTURE(s.open);
        const AssetBrowserLayoutMetrics tall = withState(PLEX_1X, 600.0F, s.shown, s.open, 170.0F);
        CHECK(sameLayout(assetBrowserLayout(tall), assetBrowserLayout(withChrome(tall, 0.0F))));
    }

    // A NaN, negative or infinite chrome counts as 0 -- the row shows from a 4-point remainder -- at heights where the
    // mode turns on it: 32 (round 2) leaves a 2-point remainder with no Issues, folded at 0 and shown under a -5 read
    // raw.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    std::size_t badChecked = 0;
    for (const float bad : {nan, -nan, -5.0F, inf, -inf}) {
        for (const float avail : {0.0F, 10.0F, 32.0F, 50.0F, 89.0F, 104.0F, 600.0F}) {
            for (const State& s : STATES) {
                CAPTURE(bad);
                CAPTURE(avail);
                CAPTURE(s.shown);
                CAPTURE(s.open);
                const AssetBrowserLayoutMetrics m = withState(PLEX_1X, avail, s.shown, s.open, 170.0F);
                const AssetBrowserLayout layout = assetBrowserLayout(withChrome(m, bad));
                CHECK(std::isfinite(layout.paneHeight));
                CHECK(sameLayout(layout, assetBrowserLayout(withChrome(m, 0.0F))));
                ++badChecked;
            }
        }
    }
    CHECK(badChecked == 5U * 7U * 4U);
    // Anti-vacuity: the chrome really moves the MODE -- at 50 points with no Issues (a 20-point remainder) the row
    // shows without it and folds with it, over the same 20-point pane. Until round 2 it moved the floor instead: at 10
    // points the panes were 4 without it and 41 with it; folded, both are ImGui's 4.
    const AssetBrowserLayoutMetrics fifty = withState(PLEX_1X, 50.0F, false, false, 0.0F);
    CHECK(assetBrowserLayout(withChrome(fifty, 0.0F)).crumbRowShown);
    CHECK_FALSE(assetBrowserLayout(fifty).crumbRowShown);
    CHECK(assetBrowserLayout(fifty).paneHeight == 20.0F);
    const AssetBrowserLayoutMetrics tiny = withState(PLEX_1X, 10.0F, false, false, 0.0F);
    CHECK(assetBrowserLayout(withChrome(tiny, 0.0F)).paneHeight == CHILD_MIN);
    CHECK(assetBrowserLayout(tiny).paneHeight == CHILD_MIN);
}

// ---- AV67: task E.6.3's code-review round, round 2 -- the crumb row FOLDS in a pane too short for it --------------
//
// AV66's reservation made the budget honest and left the editor's own 1280 x 720 launch (avail 109) scrolling with
// the Issues list open where main fit, and no fit at all in 640-776-point windows with the list open. Below the crumb
// row's chrome + 4 the row now FOLDS: the ##contents child takes the whole pane and the tree still navigates
// (asset_view.hpp's THE FOLD). With R = avail - footer - issues, in both modes
//     crumbRowShown == (R >= chrome + 4)     and     paneHeight == max(R, 4),
// so the pane is continuous in avail across the fold, and the sum is the panel down to the FOLDED fit bound,
// footer + header [+ one row + spacing] + ImGui's 4 -- main's 1-point-floor bounds + 3. Those bounds are LITERALS,
// per metric set and Issues state (content-independent: with the Issues list open the fold fires only with the body
// at its one-row floor; closed, or with no Issues, there is no body). The ##contents child is NOT continuous: it
// steps by the chrome at the threshold, and closing the list shrinks it in a band at 1x (the last block).
namespace {

struct FoldedBounds {
    float none;    // footer + 4
    float closed;  // + the header
    float open;    // + one body row + its spacing
};

[[nodiscard]] float foldedBoundFor(const FoldedBounds& b, bool shown, bool open) {
    if (!shown) {
        return b.none;  // a stale open flag with nothing to show reserves nothing (AV59)
    }
    return open ? b.open : b.closed;
}

}  // namespace

TEST_CASE("asset view: a pane under the crumb row's floor folds the row, continuously, and fits (AV67)") {
    std::vector<float> avails;
    for (float avail = 0.0F; avail <= 600.0F; avail += 1.0F) {
        avails.push_back(avail);
    }
    for (const float fractional : {195.25F, 333.5F, 1000.75F, 4096.0F}) {
        avails.push_back(fractional);
    }
    std::sort(avails.begin(), avails.end());  // ADJACENT points, for the continuity arm
    struct State {
        bool shown;
        bool open;
    };
    constexpr std::array<State, 4> STATES{{{false, false}, {false, true}, {true, false}, {true, true}}};
    struct FoldCase {
        std::string_view name;
        AssetBrowserLayoutMetrics metrics;
        FoldedBounds bounds;
    };
    // 1x and the small font: footer 6 + 24 = 30, header 30, row 16, spacing 6. 2x: every one doubled.
    const std::array<FoldCase, 3> cases{FoldCase{"plex 1x", PLEX_1X, {34.0F, 64.0F, 86.0F}},
                                        FoldCase{"plex 2x", PLEX_2X, {64.0F, 124.0F, 168.0F}},
                                        FoldCase{"small font", SMALL_FONT, {34.0F, 64.0F, 86.0F}}};
    for (const FoldCase& c : cases) {
        CAPTURE(c.name);
        const float rowFloor = c.metrics.paneChromeHeight + CHILD_MIN;
        std::size_t checked = 0;
        std::size_t folded = 0;
        std::size_t foldedFits = 0;  // folded AND fitting: the band the reservation used to scroll in
        std::size_t crossings = 0;   // adjacent points on either side of the fold
        std::size_t misses = 0;
        std::size_t jumps = 0;
        float firstMiss = -1.0F;
        float firstJump = -1.0F;
        for (const State& s : STATES) {
            for (const float content : {0.0F, 16.0F, 47.0F, 170.0F, 5000.0F}) {
                const float bound = foldedBoundFor(c.bounds, s.shown, s.open);
                bool havePrevious = false;
                AssetBrowserLayout previous;
                float previousAvail = 0.0F;
                for (const float avail : avails) {
                    const AssetBrowserLayout layout =
                        assetBrowserLayout(withState(c.metrics, avail, s.shown, s.open, content));
                    ++checked;
                    const float remainder = avail - layout.footerHeight - layout.issuesHeight;
                    // The mode is decided by the remainder alone, and the pane is max(remainder, 4) in both.
                    bool ok = layout.crumbRowShown == (remainder >= rowFloor);
                    ok = ok && layout.paneHeight == std::max(remainder, CHILD_MIN);
                    if (!layout.crumbRowShown) {
                        ok = ok && layout.paneHeight >= CHILD_MIN && layout.paneHeight < rowFloor;
                        ++folded;
                    }
                    // THE FIT: the sum is the panel from the folded bound up; below it the panes are ImGui's 4 and
                    // only there does the sum exceed the panel.
                    if (avail >= bound) {
                        ok = ok && budgetSum(layout) == avail;
                        foldedFits += layout.crumbRowShown ? 0U : 1U;
                    } else {
                        ok = ok && budgetSum(layout) > avail && layout.paneHeight == CHILD_MIN;
                    }
                    if (!ok) {
                        ++misses;
                        if (firstMiss < 0.0F) {
                            firstMiss = avail;
                        }
                    }
                    // CONTINUITY: one point of panel moves the pane by at most one point -- across the fold included.
                    if (havePrevious) {
                        const float step = std::abs(layout.paneHeight - previous.paneHeight);
                        if (step > std::abs(avail - previousAvail) + 1e-3F) {
                            ++jumps;
                            if (firstJump < 0.0F) {
                                firstJump = avail;
                            }
                        }
                        crossings += layout.crumbRowShown != previous.crumbRowShown ? 1U : 0U;
                    }
                    havePrevious = true;
                    previous = layout;
                    previousAvail = avail;
                }
            }
        }
        CAPTURE(firstMiss);
        CAPTURE(firstJump);
        CHECK(misses == 0U);
        CHECK(jumps == 0U);
        CHECK(checked == 4U * 5U * 605U);
        CHECK(folded > 0U);      // anti-vacuity: the fold was reached ...
        CHECK(foldedFits > 0U);  // ... with the panel fitting folded ...
        CHECK(crossings > 0U);   // ... and the continuity arm stepped across it
    }

    // The 1x literals. The row shows from 71 with no Issues, 101 closed and 123 open, and folds one point below --
    // where the pane is one point shorter, not 21 (the open body keeps its one-row floor across the fold).
    const auto at = [](const AssetBrowserLayoutMetrics& metrics, float avail, bool shown, bool open) {
        return assetBrowserLayout(withState(metrics, avail, shown, open, 5000.0F));
    };
    CHECK(at(PLEX_1X, 71.0F, false, false).crumbRowShown);
    CHECK_FALSE(at(PLEX_1X, 70.0F, false, false).crumbRowShown);
    CHECK(at(PLEX_1X, 101.0F, true, false).crumbRowShown);
    CHECK_FALSE(at(PLEX_1X, 100.0F, true, false).crumbRowShown);
    CHECK(at(PLEX_1X, 123.0F, true, true).crumbRowShown);
    CHECK(at(PLEX_1X, 123.0F, true, true).paneHeight == 41.0F);
    CHECK_FALSE(at(PLEX_1X, 122.0F, true, true).crumbRowShown);
    CHECK(at(PLEX_1X, 122.0F, true, true).paneHeight == 40.0F);
    // ... the same at the small font, where 4 font heights (20) are under the chrome + 4 (41): a body that grew on the
    // fold, yielding only to 20, would leave a 20-point pane at 122 -- the jump the continuity arm refuses.
    CHECK(at(SMALL_FONT, 123.0F, true, true).paneHeight == 41.0F);
    CHECK(at(SMALL_FONT, 122.0F, true, true).paneHeight == 40.0F);
    CHECK(at(SMALL_FONT, 122.0F, true, true).issuesBodyHeight == 16.0F);
    // The folded fit bounds at 1x: the sum is the panel AT them with ImGui's 4-point panes, one point over below them.
    for (const float bound : {34.0F, 64.0F, 86.0F}) {
        CAPTURE(bound);
        const bool shown = bound > 34.0F;
        const bool open = bound > 64.0F;
        const AssetBrowserLayout fits = at(PLEX_1X, bound, shown, open);
        CHECK_FALSE(fits.crumbRowShown);
        CHECK(fits.paneHeight == CHILD_MIN);
        CHECK(budgetSum(fits) == bound);
        const AssetBrowserLayout over = at(PLEX_1X, bound - 1.0F, shown, open);
        CHECK(over.paneHeight == CHILD_MIN);
        CHECK(budgetSum(over) == bound);
    }
    // THE LAUNCH: the editor's 1280 x 720 window, avail 109 (I231's fixture, measured). Open, the row folds and the
    // panel fits; closed, the row shows and the panel fits.
    const AssetBrowserLayout launchOpen = at(PLEX_1X, 109.0F, true, true);
    CHECK_FALSE(launchOpen.crumbRowShown);
    CHECK(launchOpen.paneHeight == 27.0F);
    CHECK(budgetSum(launchOpen) == 109.0F);
    const AssetBrowserLayout launchClosed = at(PLEX_1X, 109.0F, true, false);
    CHECK(launchClosed.crumbRowShown);
    CHECK(launchClosed.paneHeight == 49.0F);
    CHECK(budgetSum(launchClosed) == 109.0F);

    // ONLY THE PANE IS CONTINUOUS (asset_view.hpp): the ##contents child is the pane less the chrome while the row
    // shows and the whole pane once it folds, so it steps by the chrome at the threshold -- 4 points at 123 with the
    // list open, 40 at 122. Closing the list SHRINKS it exactly where the row shows closed and folds open, [101, 123)
    // at 1x: one body row (16) + one spacing (6) wide. The launch's 109 is inside: 27 points open, 12 closed.
    const auto contentsOf = [](const AssetBrowserLayout& layout) {
        const AssetContentsPaneLayout group =
            assetContentsPaneLayout(layout.paneHeight, 24.0F, 1.0F, 6.0F, 1.0F, layout.crumbRowShown);
        return group.contentsHeight;
    };
    CHECK(contentsOf(at(PLEX_1X, 123.0F, true, true)) == 4.0F);
    CHECK(contentsOf(at(PLEX_1X, 122.0F, true, true)) == 40.0F);
    CHECK(contentsOf(launchOpen) == 27.0F);
    CHECK(contentsOf(launchClosed) == 12.0F);
    std::size_t shrinks = 0;
    std::size_t bandMisses = 0;
    float firstBandMiss = -1.0F;
    for (float avail = 0.0F; avail <= 600.0F; avail += 1.0F) {
        const float open = contentsOf(at(PLEX_1X, avail, true, true));
        const bool closingShrinks = contentsOf(at(PLEX_1X, avail, true, false)) < open;
        if (closingShrinks != (avail >= 101.0F && avail < 123.0F)) {
            ++bandMisses;
            firstBandMiss = firstBandMiss < 0.0F ? avail : firstBandMiss;
        }
        shrinks += closingShrinks ? 1U : 0U;
    }
    CAPTURE(firstBandMiss);
    CHECK(bandMisses == 0U);
    CHECK(shrinks == 22U);  // anti-vacuity: the band was walked, every whole point of it
}

// ---- task E.4.5 (the code-review round): a SUBTITLED tile's first line keeps the FILE NAME ------------------
// A tile carrying a document name gives its file name ONE line, and a search hit's caption source is
// "parent/leaf" -- so today's right-elision kept the folder and dropped the very name the tile exists to show.
// Both rules are pure over an injected measurer, so every case here is a table over a code-point budget.
namespace {

using engine::editor::CaptionLineFits;
using engine::editor::elideCaptionRight;
using engine::editor::subtitledTileCaptionLine;

constexpr std::string_view ELLIPSIS = "\xE2\x80\xA6";  // U+2026 -- the caption's own ellipsis, one glyph
constexpr std::string_view E_ACUTE = "\xC3\xA9";       // U+00E9 -- two bytes, one glyph

[[nodiscard]] bool isContinuationByte(char byte) noexcept {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

// A line holds `width` CODE POINTS, and the ellipsis is one. A stray continuation byte costs NOTHING here, so a
// suffix search that ignored UTF-8 boundaries would PREFER one -- which is what lets AV63 see that it did.
[[nodiscard]] CaptionLineFits fitsCodePoints(std::size_t width) {
    return [width](std::string_view text) {
        const auto points =
            std::count_if(text.begin(), text.end(), [](char byte) { return !isContinuationByte(byte); });
        return static_cast<std::size_t>(points) <= width;
    };
}

// A line holds `width` BYTES. A prefix that ends inside a sequence is SHORTER in bytes than the whole code point,
// so a prefix search that ignored UTF-8 boundaries would prefer the split -- AV64's UTF-8 arm.
[[nodiscard]] CaptionLineFits fitsBytes(std::size_t width) {
    return [width](std::string_view text) { return text.size() <= width; };
}

[[nodiscard]] std::string withEllipsis(std::string_view head, std::string_view tail) {
    std::string out(head);
    out += tail;
    return out;
}

}  // namespace

TEST_CASE("asset view: a subtitled tile's first line is its caption source whenever that fits (AV60)") {
    const CaptionLineFits fits = fitsCodePoints(20U);
    CHECK(subtitledTileCaptionLine("mats/gold.aeromat", "gold.aeromat", fits) == "mats/gold.aeromat");
    CHECK(subtitledTileCaptionLine("gold.aeromat", "gold.aeromat", fits) == "gold.aeromat");
    // EXACTLY the width fits (the measurer is <=): unchanged, and no ellipsis is spent.
    CHECK(subtitledTileCaptionLine("abcdefg/gold.aeromat", "gold.aeromat", fits) == "abcdefg/gold.aeromat");
}

TEST_CASE("asset view: a narrow search hit keeps its file name -- an ellipsis, then the longest suffix (AV61)") {
    const CaptionLineFits fits = fitsCodePoints(20U);
    const std::string source = "materials/metals/gold.aeromat";  // 29 code points
    const std::string line = subtitledTileCaptionLine(source, "gold.aeromat", fits);
    CHECK(line == withEllipsis(ELLIPSIS, "metals/gold.aeromat"));  // 1 + 19: the LONGEST suffix that fits
    CHECK(line.starts_with(ELLIPSIS));
    CHECK(line.ends_with("gold.aeromat"));
    CHECK(fits(line));
    // ANTI-VACUITY: today's rule over the SAME source and width keeps the folder and loses the name -- the
    // defect this line fixes. If this arm stopped holding, the arms above would prove nothing.
    const std::string rightElided = elideCaptionRight(source, fits);
    CHECK(rightElided == withEllipsis("materials/metals/go", ELLIPSIS));
    CHECK_FALSE(rightElided.ends_with("gold.aeromat"));
    // Room for the name alone: the ellipsis and the whole leaf, nothing of the folder.
    CHECK(subtitledTileCaptionLine(source, "gold.aeromat", fitsCodePoints(13U)) ==
          withEllipsis(ELLIPSIS, "gold.aeromat"));
}

TEST_CASE("asset view: a file name too long for its line is right-elided -- the leaf, never the folder (AV62)") {
    const CaptionLineFits fits = fitsCodePoints(12U);
    const std::string leaf = "an_extremely_long_material.aeromat";
    const std::string expected = withEllipsis("an_extremel", ELLIPSIS);  // 11 + 1
    const std::string hit = "mats/" + leaf;
    CHECK(subtitledTileCaptionLine(hit, leaf, fits) == expected);
    // A caption source that IS the leaf -- every tile that is not a search hit -- keeps today's rule exactly.
    CHECK(subtitledTileCaptionLine(leaf, leaf, fits) == expected);
    CHECK(elideCaptionRight(leaf, fits) == expected);
    // A leaf that is not the source's suffix is a caller's mistake: the line is then today's rule over the source.
    const std::string source = "materials/metals/gold.aeromat";
    const CaptionLineFits twenty = fitsCodePoints(20U);
    CHECK(subtitledTileCaptionLine(source, "silver.aeromat", twenty) == elideCaptionRight(source, twenty));
    CHECK(subtitledTileCaptionLine(source, "", twenty) == elideCaptionRight(source, twenty));
}

TEST_CASE("asset view: the kept suffix starts on a UTF-8 boundary, never inside a sequence (AV63)") {
    std::string parent;
    for (int i = 0; i < 5; ++i) {
        parent += E_ACUTE;  // five two-byte code points
    }
    const std::string source = parent + "/or.aeromat";  // 16 code points in 21 bytes
    const std::string line = subtitledTileCaptionLine(source, "or.aeromat", fitsCodePoints(14U));
    std::string expected(ELLIPSIS);
    expected += E_ACUTE;
    expected += E_ACUTE;
    expected += "/or.aeromat";  // 1 + 13 code points: two whole accents, never a half of a third
    CHECK(line == expected);
    REQUIRE(line.size() > ELLIPSIS.size());
    CHECK_FALSE(isContinuationByte(line[ELLIPSIS.size()]));
}

TEST_CASE("asset view: elideCaptionRight keeps a whole caption, else its longest prefix and an ellipsis (AV64)") {
    CHECK(elideCaptionRight("gold.aeromat", fitsCodePoints(12U)) == "gold.aeromat");
    CHECK(elideCaptionRight("gold.aeromat", fitsCodePoints(6U)) == withEllipsis("gold.", ELLIPSIS));
    // Never inside a sequence: "caf" + e-acute is five bytes, and seven bytes hold "caf" + a SPLIT accent + the
    // three-byte ellipsis exactly -- so the prefix steps back to the accent's lead byte and keeps "caf" alone.
    const std::string accented = "caf" + std::string(E_ACUTE) + "_noir.aeromat";
    CHECK(elideCaptionRight(accented, fitsBytes(7U)) == withEllipsis("caf", ELLIPSIS));
    CHECK(elideCaptionRight(accented, fitsBytes(8U)) == withEllipsis("caf" + std::string(E_ACUTE), ELLIPSIS));
    // Nothing but the ellipsis fits: the ellipsis alone, as elideForCaption has always answered.
    CHECK(elideCaptionRight("gold.aeromat", fitsCodePoints(1U)) == ELLIPSIS);
    CHECK(elideCaptionRight("gold.aeromat", fitsCodePoints(0U)) == ELLIPSIS);
}

TEST_CASE("asset view: a first code point wider than one byte is kept when it fits with the ellipsis (AV65)") {
    // The second code-review round: bisecting over BYTES, a probe that fell inside the FIRST code point stepped
    // back to byte 0 and gave up, answering the ellipsis alone although the whole first code point fit beside it.
    // U+1F600 is four bytes; the font draws it as '?', but the rule is about bytes and boundaries, not glyphs.
    const std::string grin = "\xF0\x9F\x98\x80";
    const std::string text = grin + "abcdef";  // seven code points in ten bytes
    CHECK(elideCaptionRight(text, fitsCodePoints(2U)) == withEllipsis(grin, ELLIPSIS));
    CHECK(elideCaptionRight(text, fitsBytes(7U)) == withEllipsis(grin, ELLIPSIS));  // 4 + 3 bytes exactly
    CHECK(elideCaptionRight(text, fitsCodePoints(3U)) == withEllipsis(grin + "a", ELLIPSIS));
    CHECK(elideCaptionRight(text, fitsCodePoints(7U)) == text);  // fits whole: unchanged
    // Nothing but the ellipsis fits: the ellipsis alone, and never a split first code point.
    CHECK(elideCaptionRight(text, fitsCodePoints(1U)) == ELLIPSIS);
    CHECK(elideCaptionRight(text, fitsBytes(6U)) == ELLIPSIS);
    // The subtitled line inherits the rule where it falls back to eliding the leaf.
    CHECK(subtitledTileCaptionLine(text, text, fitsCodePoints(2U)) == withEllipsis(grin, ELLIPSIS));
}
