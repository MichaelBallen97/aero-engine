// tests/editor/asset_view_test.cpp -- task 3.1.3, Step 1: the asset browser's presentation model
// (KINDS, icons, filter, whole-project search, grid column math). A TU of aero_editor_shell_test,
// which supplies main() from shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED (D4/AC-17/INV-P5, the asset_meta_test.cpp precedent): asset_view.hpp depends on nothing but
// asset_meta.hpp, which itself depends on nothing but guid.hpp -- neither needs reflection, so every
// case here must be PRESENT and PASSING in all three build configurations -- prove it with
// --list-test-cases. Tier-0: no GPU, no window, no ImGui context, no disk I/O, no entropy source.
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/asset_view.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using engine::editor::AssetFilter;
using engine::editor::AssetKind;
using engine::editor::assetKindLabel;
using engine::editor::AssetRecord;
using engine::editor::classifyAssetKind;
using engine::editor::gridColumnsFor;
using engine::editor::iconColorFor;
using engine::editor::iconLabelFor;
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
               kind == AssetKind::Audio || kind == AssetKind::Text || kind == AssetKind::Unknown));
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
    constexpr std::array<AssetKind, 6> KINDS{AssetKind::Folder, AssetKind::Texture, AssetKind::Model,
                                             AssetKind::Audio,  AssetKind::Text,    AssetKind::Unknown};
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
    constexpr std::array<AssetKind, 6> KINDS{AssetKind::Folder, AssetKind::Texture, AssetKind::Model,
                                             AssetKind::Audio,  AssetKind::Text,    AssetKind::Unknown};
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
    constexpr std::array<AssetKind, 6> KINDS{AssetKind::Folder, AssetKind::Texture, AssetKind::Model,
                                             AssetKind::Audio,  AssetKind::Text,    AssetKind::Unknown};
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
