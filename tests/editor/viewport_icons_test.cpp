// tests/editor/viewport_icons_test.cpp — task E.2.3: the icon vocabulary, the one predicate and the
// generated atlas. Joins aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0 and UNGATED: no GPU, no window, no ImGui
// context, and it must pass identically with AERO_REQUIRE_GPU unset and set. NO #if of any kind.
//
// EVERY CASE ASSERTS A RELATIONSHIP OR A STRUCTURAL PROPERTY, NEVER A TUNING MAGNITUDE (D14):
// VIEWPORT_ICON_SIZE_POINTS's VALUE is judged on the validation page, so VI11 asserts only its two
// inequalities and its half-relation. Retuning it, or any glyph's radius, reddens nothing here.
//
// The atlas buffer is 64 KiB, so it lives on the HEAP (std::vector) in every case -- that is above
// some default stack budgets and this file runs under ASan.
#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/picking.hpp>            // POINT_PICK_RADIUS_POINTS, for VI11
#include <aero/editor/selection_overlay.hpp>  // POINT_MARKER_HALF_POINTS, for VI11
#include <aero/editor/viewport_icons.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using engine::Camera;
using engine::DirectionalLight;
using engine::Entity;
using engine::Environment;
using engine::MeshRenderer;
using engine::PointLight;
using engine::SpotLight;
using engine::Transform;
using engine::World;
namespace ed = engine::editor;

constexpr std::string_view VIEWPORT_ICONS_SOURCE_PATH = AERO_EDITOR_SRC_DIR "/viewport_icons.cpp";

// COMMENT-STRIPPED, so a token that appears only in a comment cannot satisfy VI7 -- and this file's
// own subject names std::sin, std::cos and std::pow in the sentence saying they are absent, which is
// exactly the trap this strips away (render_debug_grid_test.cpp's GR22 precedent, copied).
[[nodiscard]] std::string strippedSourceAt(std::string_view absolutePath) {
    std::ifstream file{std::string{absolutePath}};
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
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// The four kinds in declaration order, spelled once so nine cases cannot drift apart.
constexpr std::array<ed::ViewportIconKind, ed::VIEWPORT_ICON_COUNT> ALL_KINDS{
    ed::ViewportIconKind::DirectionalLight, ed::ViewportIconKind::SpotLight, ed::ViewportIconKind::PointLight,
    ed::ViewportIconKind::Camera};

// A COMPARISON STRUCT IN THE FILE'S ANONYMOUS NAMESPACE, NOT INSIDE A TEST_CASE. [class.friend]/6
// forbids defining a friend in a local class, so a struct declared inside a case can carry no
// operator<< at all and every failure prints {?} == {?}.
struct TexelIndex {
    std::size_t value = 0;
    [[nodiscard]] bool operator==(const TexelIndex&) const = default;
};

std::ostream& operator<<(std::ostream& out, const TexelIndex& index) {
    out << "texel #" << index.value;
    return out;
}

// A freshly built atlas, on the heap. Every case that reads pixels goes through this, so a case can
// never accidentally read a buffer some earlier case left behind.
[[nodiscard]] std::vector<std::uint8_t> builtAtlas() {
    std::vector<std::uint8_t> atlas(ed::VIEWPORT_ICON_ATLAS_BYTES);
    REQUIRE(ed::buildViewportIconAtlas(atlas));
    return atlas;
}

[[nodiscard]] std::uint8_t alphaAt(const std::vector<std::uint8_t>& atlas, std::uint32_t x, std::uint32_t y) {
    const std::size_t texel = (static_cast<std::size_t>(y) * ed::VIEWPORT_ICON_ATLAS_WIDTH) + x;
    return atlas[(texel * 4U) + 3U];
}

// ONE CELL'S 64x64 ALPHA PLANE, row-major. A cell is a sub-rect of a 256-wide atlas, so a plain byte
// range would straddle all four glyphs -- this is what makes "are two glyphs the same picture?" a
// question that can be asked at all (VI12).
[[nodiscard]] std::vector<std::uint8_t> alphaPlaneOf(const std::vector<std::uint8_t>& atlas,
                                                     ed::ViewportIconKind kind) {
    const std::uint32_t base = ed::viewportIconCell(kind) * ed::VIEWPORT_ICON_CELL_TEXELS;
    std::vector<std::uint8_t> plane;
    plane.reserve(static_cast<std::size_t>(ed::VIEWPORT_ICON_CELL_TEXELS) * ed::VIEWPORT_ICON_CELL_TEXELS);
    for (std::uint32_t y = 0; y < ed::VIEWPORT_ICON_CELL_TEXELS; ++y) {
        for (std::uint32_t x = 0; x < ed::VIEWPORT_ICON_CELL_TEXELS; ++x) {
            plane.push_back(alphaAt(atlas, base + x, y));
        }
    }
    return plane;
}

// Counted rather than compared, so a failure prints HOW MANY texels differ instead of {?} == {?} --
// doctest cannot stringify a vector<uint8_t>, and 4096 bytes would be unreadable if it could.
[[nodiscard]] std::size_t differingTexels(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    REQUIRE(a.size() == b.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            ++differing;
        }
    }
    return differing;
}

struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};

}  // namespace

TEST_CASE("viewport icons: viewportIconUv tiles the atlas exactly and is total (VI1)") {
    SUBCASE("the four rects tile [0,1] in x, EXACTLY -- k/4 is exact in float") {
        CHECK(ed::viewportIconUv(ed::ViewportIconKind::DirectionalLight).uvMin.x == 0.0F);
        CHECK(ed::viewportIconUv(ed::ViewportIconKind::DirectionalLight).uvMax.x == 0.25F);
        CHECK(ed::viewportIconUv(ed::ViewportIconKind::SpotLight).uvMin.x == 0.25F);
        CHECK(ed::viewportIconUv(ed::ViewportIconKind::SpotLight).uvMax.x == 0.5F);
        CHECK(ed::viewportIconUv(ed::ViewportIconKind::PointLight).uvMin.x == 0.5F);
        CHECK(ed::viewportIconUv(ed::ViewportIconKind::PointLight).uvMax.x == 0.75F);
        CHECK(ed::viewportIconUv(ed::ViewportIconKind::Camera).uvMin.x == 0.75F);
        CHECK(ed::viewportIconUv(ed::ViewportIconKind::Camera).uvMax.x == 1.0F);
    }
    SUBCASE("each rect spans the full y range and is pairwise disjoint in x") {
        for (std::size_t i = 0; i < ed::VIEWPORT_ICON_COUNT; ++i) {
            const ed::ViewportIconUv uv = ed::viewportIconUv(ALL_KINDS[i]);
            CHECK(uv.uvMin.y == 0.0F);
            CHECK(uv.uvMax.y == 1.0F);
            CHECK(uv.uvMin.x < uv.uvMax.x);
            for (std::size_t j = i + 1; j < ed::VIEWPORT_ICON_COUNT; ++j) {
                const ed::ViewportIconUv other = ed::viewportIconUv(ALL_KINDS[j]);
                // Half-open by construction: cell i's max IS cell i+1's min, so disjointness is
                // "no overlap of interiors", which is what a sampler addressing [min, max) needs.
                CHECK((uv.uvMax.x <= other.uvMin.x || other.uvMax.x <= uv.uvMin.x));
            }
        }
    }
    SUBCASE("the cell index is the enumerator's ordinal, and the two agree") {
        for (std::size_t i = 0; i < ed::VIEWPORT_ICON_COUNT; ++i) {
            CHECK(ed::viewportIconCell(ALL_KINDS[i]) == static_cast<std::uint32_t>(i));
        }
    }
    SUBCASE("an out-of-range cast returns cell 0's rect rather than reading past anything") {
        const auto rogue = static_cast<ed::ViewportIconKind>(200);
        CHECK(ed::viewportIconCell(rogue) == 0U);
        CHECK(ed::viewportIconUv(rogue) == ed::viewportIconUv(ed::ViewportIconKind::DirectionalLight));
    }
}

TEST_CASE("viewport icons: a wrong-sized span is refused and NOTHING is written (VI2)") {
    CHECK(ed::VIEWPORT_ICON_ATLAS_BYTES == static_cast<std::size_t>(256) * 64U * 4U);
    CHECK(ed::VIEWPORT_ICON_ATLAS_WIDTH == 256U);
    CHECK(ed::VIEWPORT_ICON_ATLAS_HEIGHT == 64U);

    constexpr std::uint8_t CANARY = 0xABU;
    std::vector<std::uint8_t> buffer(ed::VIEWPORT_ICON_ATLAS_BYTES + 1U, CANARY);
    const std::span<std::uint8_t> whole{buffer};

    const auto allCanary = [&buffer]() {
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            if (buffer[i] != CANARY) {
                return TexelIndex{i};
            }
        }
        return TexelIndex{buffer.size()};
    };

    SUBCASE("one byte short") {
        CHECK_FALSE(ed::buildViewportIconAtlas(whole.first(ed::VIEWPORT_ICON_ATLAS_BYTES - 1U)));
        CHECK(allCanary() == TexelIndex{buffer.size()});
    }
    SUBCASE("one byte long") {
        CHECK_FALSE(ed::buildViewportIconAtlas(buffer));
        CHECK(allCanary() == TexelIndex{buffer.size()});
    }
    SUBCASE("empty") {
        CHECK_FALSE(ed::buildViewportIconAtlas(std::span<std::uint8_t>{}));
        CHECK(allCanary() == TexelIndex{buffer.size()});
    }
    SUBCASE("the exact size succeeds -- the anti-vacuity arm for the three refusals above") {
        CHECK(ed::buildViewportIconAtlas(whole.first(ed::VIEWPORT_ICON_ATLAS_BYTES)));
        CHECK(buffer.back() == CANARY);  // the one byte past the atlas is still untouched
    }
}

TEST_CASE("viewport icons: RGB is exactly 255 on every texel -- the glyph is alpha alone (VI3)") {
    const std::vector<std::uint8_t> atlas = builtAtlas();
    const std::size_t texels = ed::VIEWPORT_ICON_ATLAS_BYTES / 4U;
    CHECK(texels == 16384U);

    std::size_t nonWhite = 0;
    TexelIndex firstOffender{texels};
    for (std::size_t t = 0; t < texels; ++t) {
        if (atlas[t * 4U] != 255U || atlas[(t * 4U) + 1U] != 255U || atlas[(t * 4U) + 2U] != 255U) {
            ++nonWhite;
            if (firstOffender.value == texels) {
                firstOffender = TexelIndex{t};
            }
        }
    }
    CHECK(nonWhite == 0U);
    CHECK(firstOffender == TexelIndex{texels});
}

TEST_CASE("viewport icons: every cell carries a fully transparent gutter on all four sides (VI4)") {
    const std::vector<std::uint8_t> atlas = builtAtlas();
    const std::uint32_t gutter = ed::VIEWPORT_ICON_GUTTER_TEXELS;
    REQUIRE(gutter > 0U);

    std::size_t opaqueInGutter = 0;
    for (std::size_t k = 0; k < ed::VIEWPORT_ICON_COUNT; ++k) {
        const std::uint32_t base = ed::viewportIconCell(ALL_KINDS[k]) * ed::VIEWPORT_ICON_CELL_TEXELS;
        for (std::uint32_t y = 0; y < ed::VIEWPORT_ICON_CELL_TEXELS; ++y) {
            for (std::uint32_t x = 0; x < ed::VIEWPORT_ICON_CELL_TEXELS; ++x) {
                const std::uint32_t far = ed::VIEWPORT_ICON_CELL_TEXELS - gutter;
                const bool inGutter = x < gutter || x >= far || y < gutter || y >= far;
                if (inGutter && alphaAt(atlas, base + x, y) != 0U) {
                    ++opaqueInGutter;
                }
            }
        }
    }
    CHECK(opaqueInGutter == 0U);
}

TEST_CASE("viewport icons: every cell has ink, none is a filled square, none is a hairline (VI5)") {
    const std::vector<std::uint8_t> atlas = builtAtlas();
    const std::uint32_t cell = ed::VIEWPORT_ICON_CELL_TEXELS;
    const std::size_t cellArea = static_cast<std::size_t>(cell) * cell;

    for (std::size_t k = 0; k < ed::VIEWPORT_ICON_COUNT; ++k) {
        CAPTURE(k);
        const std::uint32_t base = ed::viewportIconCell(ALL_KINDS[k]) * cell;
        std::size_t opaque = 0;
        std::uint32_t longestRun = 0;
        for (std::uint32_t y = 0; y < cell; ++y) {
            std::uint32_t run = 0;
            for (std::uint32_t x = 0; x < cell; ++x) {
                if (alphaAt(atlas, base + x, y) == 255U) {
                    ++opaque;
                    ++run;
                    longestRun = run > longestRun ? run : longestRun;
                } else {
                    run = 0;
                }
            }
        }
        // BANDS, never magnitudes: a retune of any radius keeps all three true.
        CHECK(opaque > 0U);
        CHECK(opaque < cellArea);
        CHECK(longestRun >= ed::VIEWPORT_ICON_MIN_FEATURE_TEXELS);
    }
}

TEST_CASE("viewport icons: the atlas is bit-deterministic and destination-independent (VI6)") {
    std::vector<std::uint8_t> first(ed::VIEWPORT_ICON_ATLAS_BYTES);
    std::vector<std::uint8_t> second(ed::VIEWPORT_ICON_ATLAS_BYTES);
    REQUIRE(ed::buildViewportIconAtlas(first));
    REQUIRE(ed::buildViewportIconAtlas(second));
    CHECK(first == second);

    // A PRE-DIRTIED destination: the result must not depend on what was there, which is what makes
    // "writes every byte it owns" a statement rather than an assumption.
    std::vector<std::uint8_t> dirty(ed::VIEWPORT_ICON_ATLAS_BYTES, 0x5AU);
    REQUIRE(ed::buildViewportIconAtlas(dirty));
    CHECK(dirty == first);
}

TEST_CASE("viewport icons: the rasteriser reaches no approximating libm function and no each<> (VI7)") {
    const std::string source = strippedSourceAt(VIEWPORT_ICONS_SOURCE_PATH);
    REQUIRE_FALSE(source.empty());  // non-vacuity: a missing file would pass every check below

    // The positive control -- the file really is the one under test, comment-stripped.
    CHECK(contains(source, "buildViewportIconAtlas"));
    CHECK(contains(source, "std::sqrt"));
    // The negative probe: a token that is in no source file anywhere.
    CHECK_FALSE(contains(source, "does_not_exist_token"));

    CHECK_FALSE(contains(source, "std::sin"));
    CHECK_FALSE(contains(source, "std::cos"));
    CHECK_FALSE(contains(source, "std::pow"));
    CHECK_FALSE(contains(source, "std::atan"));
    CHECK_FALSE(contains(source, "std::asin"));
    CHECK_FALSE(contains(source, "std::acos"));
    CHECK_FALSE(contains(source, "std::exp"));
    CHECK_FALSE(contains(source, "std::log"));
    // World::each<T> logs one ERROR PER CALL for an unregistered type, straight into the Console.
    CHECK_FALSE(contains(source, "each<"));
}

TEST_CASE("viewport icons: viewportIconFor answers for each component and refuses everything else (VI8)") {
    World world;
    const Entity directional = world.create();
    world.add<Transform>(directional, Transform{});
    world.add<DirectionalLight>(directional, DirectionalLight{});
    const Entity spot = world.create();
    world.add<SpotLight>(spot, SpotLight{});
    const Entity point = world.create();
    world.add<PointLight>(point, PointLight{});
    const Entity camera = world.create();
    world.add<Camera>(camera, Camera{});

    CHECK(ed::viewportIconFor(world, directional) == ed::ViewportIconKind::DirectionalLight);
    CHECK(ed::viewportIconFor(world, spot) == ed::ViewportIconKind::SpotLight);
    CHECK(ed::viewportIconFor(world, point) == ed::ViewportIconKind::PointLight);
    CHECK(ed::viewportIconFor(world, camera) == ed::ViewportIconKind::Camera);

    SUBCASE("an entity carrying none of the four draws no icon") {
        const Entity bare = world.create();
        world.add<Transform>(bare, Transform{});
        CHECK_FALSE(ed::viewportIconFor(world, bare).has_value());

        const Entity meshOnly = world.create();
        world.add<Transform>(meshOnly, Transform{});
        world.add<MeshRenderer>(meshOnly, MeshRenderer{});
        CHECK_FALSE(ed::viewportIconFor(world, meshOnly).has_value());

        // The default scene's Environment entity carries NO Transform at all (E.2.1's seed), so it is
        // built with create() + add, never through the editor's createEntity.
        const Entity environment = world.create();
        world.add<Environment>(environment, Environment{});
        CHECK_FALSE(ed::viewportIconFor(world, environment).has_value());
    }
    SUBCASE("a DEAD handle and a NULL handle are both refused") {
        const Entity doomed = world.create();
        world.add<PointLight>(doomed, PointLight{});
        REQUIRE(ed::viewportIconFor(world, doomed) == ed::ViewportIconKind::PointLight);
        REQUIRE(world.destroy(doomed));
        CHECK_FALSE(ed::viewportIconFor(world, doomed).has_value());
        CHECK_FALSE(ed::viewportIconFor(world, Entity{}).has_value());
    }
}

TEST_CASE("viewport icons: priority is the enum's declaration order, one icon per entity (VI9)") {
    World world;
    const auto seed = [&world](bool dir, bool spot, bool point, bool camera) {
        const Entity e = world.create();
        if (dir) {
            world.add<DirectionalLight>(e, DirectionalLight{});
        }
        if (spot) {
            world.add<SpotLight>(e, SpotLight{});
        }
        if (point) {
            world.add<PointLight>(e, PointLight{});
        }
        if (camera) {
            world.add<Camera>(e, Camera{});
        }
        return e;
    };

    const auto iconOf = [&world](Entity e) { return ed::viewportIconFor(world, e); };
    CHECK(iconOf(seed(true, false, false, true)) == ed::ViewportIconKind::DirectionalLight);
    CHECK(iconOf(seed(false, true, true, false)) == ed::ViewportIconKind::SpotLight);
    CHECK(iconOf(seed(false, false, true, true)) == ed::ViewportIconKind::PointLight);
    CHECK(iconOf(seed(true, true, true, true)) == ed::ViewportIconKind::DirectionalLight);
    CHECK(iconOf(seed(false, true, false, true)) == ed::ViewportIconKind::SpotLight);
}

TEST_CASE("viewport icons: viewportIconFor emits NO log record on any path (VI10)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST, after the LogSinkScope
    const ed::LogSinkScope scope;
    std::vector<ed::LogEntry> records;

    World world;
    const Entity directional = world.create();
    world.add<DirectionalLight>(directional, DirectionalLight{});
    const Entity spot = world.create();
    world.add<SpotLight>(spot, SpotLight{});
    const Entity point = world.create();
    world.add<PointLight>(point, PointLight{});
    const Entity camera = world.create();
    world.add<Camera>(camera, Camera{});
    const Entity bare = world.create();
    world.add<Transform>(bare, Transform{});
    const Entity doomed = world.create();
    REQUIRE(world.destroy(doomed));

    for (const Entity e : {directional, spot, point, camera, bare, doomed, Entity{}}) {
        (void)ed::viewportIconFor(world, e);
    }
    scope.sink()->take(records);
    CHECK(records.empty());
}

TEST_CASE("viewport icons: the drawn size's three relationships, never its value (VI11)") {
    CHECK(ed::VIEWPORT_ICON_HALF_POINTS == ed::VIEWPORT_ICON_SIZE_POINTS * 0.5F);
    // What picking.hpp's static_assert and D10's "an upgrade, not a trade" both rest on.
    CHECK(ed::VIEWPORT_ICON_HALF_POINTS > ed::POINT_PICK_RADIUS_POINTS);
    CHECK(ed::VIEWPORT_ICON_HALF_POINTS > ed::POINT_MARKER_HALF_POINTS);
}

TEST_CASE("viewport icons: the four glyphs are four DIFFERENT pictures (VI12)") {
    // THE ONLY TIER THAT CAN SEE A COPY-PASTED RASTERISER ARM. glyphAlpha is a total switch over the
    // same enum viewportIconCell is total over, and VI1 pins that one; nothing pinned this one. Every
    // other case reads a BAND -- ink present (VI5), gutter clear (VI4), RGB 255 (VI3), deterministic
    // (VI6) -- and all four glyphs satisfy every band, so pointing two arms at one rasteriser left the
    // whole file green while the user saw a point glyph on every spot light.
    const std::vector<std::uint8_t> atlas = builtAtlas();
    const std::size_t cellArea =
        static_cast<std::size_t>(ed::VIEWPORT_ICON_CELL_TEXELS) * ed::VIEWPORT_ICON_CELL_TEXELS;

    std::array<std::vector<std::uint8_t>, ed::VIEWPORT_ICON_COUNT> planes;
    for (std::size_t k = 0; k < ed::VIEWPORT_ICON_COUNT; ++k) {
        CAPTURE(k);
        planes[k] = alphaPlaneOf(atlas, ALL_KINDS[k]);
        REQUIRE(planes[k].size() == cellArea);
    }

    SUBCASE("THE POSITIVE CONTROL: each cell equals itself, read out of an INDEPENDENTLY built atlas") {
        // Without this the six inequalities below are vacuous: an extractor returning a fresh block of
        // garbage per call, or reading past the cell it was asked for, would satisfy every one of them.
        const std::vector<std::uint8_t> second = builtAtlas();
        for (std::size_t k = 0; k < ed::VIEWPORT_ICON_COUNT; ++k) {
            CAPTURE(k);
            CHECK(differingTexels(planes[k], alphaPlaneOf(second, ALL_KINDS[k])) == 0U);
        }
    }
    SUBCASE("all six pairs of glyphs differ") {
        for (std::size_t i = 0; i < ed::VIEWPORT_ICON_COUNT; ++i) {
            for (std::size_t j = i + 1; j < ed::VIEWPORT_ICON_COUNT; ++j) {
                CAPTURE(i);
                CAPTURE(j);
                CHECK(differingTexels(planes[i], planes[j]) > 0U);
            }
        }
    }
}
