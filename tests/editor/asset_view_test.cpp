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
