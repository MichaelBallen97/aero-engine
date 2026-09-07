// tests/editor/viewport_gizmos_test.cpp — task E.2.3: the icon/gizmo world walk. Joins
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0 and UNGATED: a real World against a bare
// render::DebugDrawBatch, no device, no ImGui, no GPU. NO #if of any kind.
//
// EVERY BILLBOARD IS MATCHED BY ITS UV RECT, NEVER BY ITS POSITION IN THE BUCKET. The icon pass walks
// World::eachEntity, whose order is the storage's, and EnTT's own walk is not creation order -- a
// case that compared by ordinal would be asserting that two unrelated orders agree (E.2.2's lesson 5).
//
// It reaches render::linearToSrgbEncode because aero::render has been PUBLIC on aero_editor_core
// since 3.4.2 -- axis_palette_test.cpp's AX1 and gizmo_style_test.cpp are the precedents.
#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/viewport_gizmos.hpp>
#include <aero/render/selection_outline.hpp>
#include <aero/render/tonemap.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using engine::Camera;
using engine::DirectionalLight;
using engine::Entity;
using engine::PointLight;
using engine::SpotLight;
using engine::Transform;
using engine::Vec3;
using engine::Vec4;
using engine::World;
namespace ed = engine::editor;
namespace rd = engine::render;

constexpr std::string_view VIEWPORT_GIZMOS_SOURCE_PATH = AERO_EDITOR_SRC_DIR "/viewport_gizmos.cpp";
constexpr std::string_view VIEWPORT_GIZMOS_HEADER_PATH =
    AERO_EDITOR_SRC_DIR "/../include/aero/editor/viewport_gizmos.hpp";

// COMMENT-STRIPPED: a token that appears only in a comment must not satisfy VG15, and both files'
// banners legitimately name `scene_render` and `each<T>` in the sentences saying they are absent.
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

// A batch big enough that nothing is ever dropped, so a count is the count EMITTED rather than the
// count that happened to fit. VG12 is the one case that deliberately uses a small one.
[[nodiscard]] rd::DebugDrawBatch roomyBatch() { return rd::DebugDrawBatch{rd::DebugDrawBudget{}}; }

// A COMPARISON STRUCT IN THE FILE'S ANONYMOUS NAMESPACE, NOT INSIDE A TEST_CASE ([class.friend]/6),
// with its own operator<< so a failure prints something readable instead of {?} == {?}.
struct Rgba {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
    [[nodiscard]] bool operator==(const Rgba&) const = default;
};

std::ostream& operator<<(std::ostream& out, const Rgba& value) {
    out << "rgba(" << value.r << ", " << value.g << ", " << value.b << ", " << value.a << ")";
    return out;
}

// The batch stores a PACKED colour, so a tint is compared through the same packer the batch used --
// never against the Vec4 the caller passed, which would compare a value against itself.
[[nodiscard]] Rgba packedOf(Vec4 linear) {
    const std::uint32_t bits = rd::packDebugColor(linear);
    return Rgba{static_cast<int>(bits & 0xFFU), static_cast<int>((bits >> 8U) & 0xFFU),
                static_cast<int>((bits >> 16U) & 0xFFU), static_cast<int>((bits >> 24U) & 0xFFU)};
}

[[nodiscard]] Rgba rgbaOf(std::uint32_t bits) {
    return Rgba{static_cast<int>(bits & 0xFFU), static_cast<int>((bits >> 8U) & 0xFFU),
                static_cast<int>((bits >> 16U) & 0xFFU), static_cast<int>((bits >> 24U) & 0xFFU)};
}

// THE BILLBOARD FOR A KIND, FOUND BY ITS UV RECT. Returns nullptr when the kind drew none, and
// REQUIREs the match is unique so "found by key" cannot silently mean "found the first of several".
[[nodiscard]] const rd::DebugBillboard* iconFor(const rd::DebugDrawBatch& batch, ed::ViewportIconKind kind) {
    const ed::ViewportIconUv uv = ed::viewportIconUv(kind);
    const rd::DebugBillboard* found = nullptr;
    std::size_t matches = 0;
    for (const rd::DebugBillboard& b : batch.billboards(rd::DebugDepth::Overlay)) {
        if (b.uvMin == uv.uvMin && b.uvMax == uv.uvMax) {
            found = &b;
            ++matches;
        }
    }
    REQUIRE(matches <= 1U);
    return found;
}

[[nodiscard]] Entity seedLight(World& world, Vec3 position) {
    const Entity e = world.create();
    world.add<Transform>(e, Transform{.position = position});
    return e;
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

TEST_CASE("viewport gizmos: an empty world emits nothing at all (VG1)") {
    const World world;
    rd::DebugDrawBatch batch = roomyBatch();
    ed::ViewportGizmoScratch scratch;
    const ed::ViewportGizmoCounts counts = ed::emitViewportGizmos(world, {}, scratch, batch);
    CHECK(counts == ed::ViewportGizmoCounts{});
    CHECK(batch.empty());
}

TEST_CASE("viewport gizmos: one icon per icon-carrying entity, at the caller's size, in Overlay (VG2)") {
    World world;
    world.add<DirectionalLight>(seedLight(world, Vec3{1.0F, 2.0F, 3.0F}), DirectionalLight{});
    world.add<SpotLight>(seedLight(world, Vec3{4.0F, 0.0F, 0.0F}), SpotLight{});
    world.add<PointLight>(seedLight(world, Vec3{0.0F, 5.0F, 0.0F}), PointLight{});
    world.add<Camera>(seedLight(world, Vec3{0.0F, 0.0F, 6.0F}), Camera{});
    // Two entities that must draw NO icon at all.
    (void)seedLight(world, Vec3::zero());
    const Entity mesh = seedLight(world, Vec3{9.0F, 9.0F, 9.0F});
    world.add<engine::MeshRenderer>(mesh, engine::MeshRenderer{});

    rd::DebugDrawBatch batch = roomyBatch();
    ed::ViewportGizmoScratch scratch;
    constexpr float SIZE_PIXELS = 33.0F;
    const ed::ViewportGizmoCounts counts =
        ed::emitViewportGizmos(world, {.iconSizePixels = SIZE_PIXELS}, scratch, batch);

    CHECK(counts.icons == 4U);
    CHECK(counts.iconsDropped == 0U);
    CHECK(batch.billboardCount() == 4U);
    CHECK(batch.billboardCount(rd::DebugDepth::Overlay) == 4U);
    CHECK(batch.billboardCount(rd::DebugDepth::Tested) == 0U);
    CHECK(batch.rejectedBillboards() == 0U);

    // MATCHED BY UV, never by position in the bucket -- EnTT's storage order is not creation order.
    using Kind = ed::ViewportIconKind;
    const std::array<Kind, 4> kinds{Kind::DirectionalLight, Kind::SpotLight, Kind::PointLight, Kind::Camera};
    const std::array<Vec3, 4> expected{Vec3{1.0F, 2.0F, 3.0F}, Vec3{4.0F, 0.0F, 0.0F}, Vec3{0.0F, 5.0F, 0.0F},
                                       Vec3{0.0F, 0.0F, 6.0F}};
    for (std::size_t i = 0; i < 4U; ++i) {
        CAPTURE(i);
        const rd::DebugBillboard* const icon = iconFor(batch, kinds[i]);
        REQUIRE(icon != nullptr);
        CHECK(icon->sizePx == SIZE_PIXELS);
        CHECK(icon->center == expected[i]);
    }
}

TEST_CASE("viewport gizmos: nothing selected means zero gizmo lines (VG3)") {
    World world;
    const Entity point = seedLight(world, Vec3::zero());
    world.add<PointLight>(point, PointLight{});

    rd::DebugDrawBatch batch = roomyBatch();
    ed::ViewportGizmoScratch scratch;
    const ed::ViewportGizmoCounts none = ed::emitViewportGizmos(world, {}, scratch, batch);
    CHECK(none.gizmoLines == 0U);
    CHECK(none.gizmoEntities == 0U);
    CHECK(batch.lineCount() == 0U);

    // THE ANTI-VACUITY ARM: selecting it takes the count from zero to the sphere's own line count.
    rd::DebugDrawBatch selectedBatch = roomyBatch();
    const std::array<Entity, 1> selection{point};
    const ed::ViewportGizmoCounts some =
        ed::emitViewportGizmos(world, {.selected = selection, .primary = point}, scratch, selectedBatch);
    CHECK(some.gizmoLines == 3U * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS);
    CHECK(some.gizmoEntities == 1U);
    CHECK(selectedBatch.lineCount() == some.gizmoLines);
}

TEST_CASE("viewport gizmos: each light kind draws its own shape and a Camera draws none (VG4)") {
    World world;
    const Entity directional = seedLight(world, Vec3::zero());
    world.add<DirectionalLight>(directional, DirectionalLight{});
    const Entity spot = seedLight(world, Vec3::zero());
    world.add<SpotLight>(spot, SpotLight{});
    const Entity point = seedLight(world, Vec3::zero());
    world.add<PointLight>(point, PointLight{});
    const Entity camera = seedLight(world, Vec3::zero());
    world.add<Camera>(camera, Camera{});

    ed::ViewportGizmoScratch scratch;
    const auto linesFor = [&world, &scratch](Entity e) {
        rd::DebugDrawBatch batch = roomyBatch();
        const std::array<Entity, 1> selection{e};
        const ed::ViewportGizmoCounts counts =
            ed::emitViewportGizmos(world, {.selected = selection, .primary = e}, scratch, batch);
        CHECK(batch.lineCount() == counts.gizmoLines);
        return counts;
    };

    // Identified by LINE COUNT and by geometry, never by an ordinal in the bucket. The three shapes:
    // a sphere is 3n, a directional disc is n + rays, a default spot is two caps plus its rim rays.
    const ed::ViewportGizmoCounts pointCounts = linesFor(point);
    CHECK(pointCounts.gizmoLines == 3U * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS);
    const ed::ViewportGizmoCounts directionalCounts = linesFor(directional);
    CHECK(directionalCounts.gizmoLines == rd::LIGHT_GIZMO_CIRCLE_SEGMENTS + rd::LIGHT_GIZMO_DIRECTIONAL_RAYS);
    const ed::ViewportGizmoCounts spotCounts = linesFor(spot);
    CHECK(spotCounts.gizmoLines == (2U * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS) + rd::LIGHT_GIZMO_SPOT_RIM_RAYS);

    const ed::ViewportGizmoCounts cameraCounts = linesFor(camera);
    CHECK(cameraCounts.gizmoLines == 0U);
    CHECK(cameraCounts.gizmoEntities == 0U);
    CHECK(cameraCounts.icons == 4U);  // it still draws its icon, and so do the other three
}

TEST_CASE("viewport gizmos: the four tints, read back off the batch and matched by UV (VG5)") {
    World world;
    const Entity primary = seedLight(world, Vec3::zero());
    world.add<PointLight>(primary, PointLight{});
    const Entity secondary = seedLight(world, Vec3::zero());
    world.add<SpotLight>(secondary, SpotLight{});
    const Entity active = seedLight(world, Vec3::zero());
    world.add<DirectionalLight>(active, DirectionalLight{});
    const Entity plain = seedLight(world, Vec3::zero());
    world.add<Camera>(plain, Camera{});
    const Entity ignored = seedLight(world, Vec3::zero());
    world.add<DirectionalLight>(ignored, DirectionalLight{});

    // `active` is named, so `ignored` is the muted one -- but `active` sorts LOWER only by chance, so
    // the case names the winner explicitly rather than relying on an index rule this walk never applies.
    rd::DebugDrawBatch batch = roomyBatch();
    ed::ViewportGizmoScratch scratch;
    const std::array<Entity, 2> selection{primary, secondary};
    const ed::ViewportGizmoTints tints{};
    const ed::ViewportGizmoParams params{
        .selected = selection, .primary = primary, .activeDirectional = active, .tints = tints};
    (void)ed::emitViewportGizmos(world, params, scratch, batch);

    const rd::DebugBillboard* const pointIcon = iconFor(batch, ed::ViewportIconKind::PointLight);
    const rd::DebugBillboard* const spotIcon = iconFor(batch, ed::ViewportIconKind::SpotLight);
    const rd::DebugBillboard* const cameraIcon = iconFor(batch, ed::ViewportIconKind::Camera);
    REQUIRE(pointIcon != nullptr);
    REQUIRE(spotIcon != nullptr);
    REQUIRE(cameraIcon != nullptr);
    CHECK(rgbaOf(pointIcon->rgba) == packedOf(tints.primarySelected));
    CHECK(rgbaOf(spotIcon->rgba) == packedOf(tints.secondarySelected));
    CHECK(rgbaOf(cameraIcon->rgba) == packedOf(tints.unselected));

    // The two directional icons SHARE a UV rect, so they are separated by their packed colour, which
    // is the only thing that distinguishes them here.
    std::size_t mutedCount = 0;
    std::size_t brightCount = 0;
    for (const rd::DebugBillboard& b : batch.billboards(rd::DebugDepth::Overlay)) {
        if (b.uvMin != ed::viewportIconUv(ed::ViewportIconKind::DirectionalLight).uvMin) {
            continue;
        }
        if (rgbaOf(b.rgba) == packedOf(tints.mutedDirectional)) {
            ++mutedCount;
        } else if (rgbaOf(b.rgba) == packedOf(tints.unselected)) {
            ++brightCount;
        }
    }
    CHECK(mutedCount == 1U);
    CHECK(brightCount == 1U);
}

TEST_CASE("viewport gizmos: naming the other directional light SWAPS which one is muted (VG6)") {
    World world;
    const Entity first = seedLight(world, Vec3{-1.0F, 0.0F, 0.0F});
    world.add<DirectionalLight>(first, DirectionalLight{});
    const Entity second = seedLight(world, Vec3{1.0F, 0.0F, 0.0F});
    world.add<DirectionalLight>(second, DirectionalLight{});

    const ed::ViewportGizmoTints tints{};
    ed::ViewportGizmoScratch scratch;
    // Read the tint back off the billboard whose CENTRE names the entity -- the two share a UV rect.
    const auto tintAt = [&world, &scratch, &tints](Entity active, Vec3 at) {
        rd::DebugDrawBatch batch = roomyBatch();
        (void)ed::emitViewportGizmos(world, {.activeDirectional = active, .tints = tints}, scratch, batch);
        for (const rd::DebugBillboard& b : batch.billboards(rd::DebugDepth::Overlay)) {
            if (b.center == at) {
                return rgbaOf(b.rgba);
            }
        }
        return Rgba{-1, -1, -1, -1};
    };

    CHECK(tintAt(first, Vec3{-1.0F, 0.0F, 0.0F}) == packedOf(tints.unselected));
    CHECK(tintAt(first, Vec3{1.0F, 0.0F, 0.0F}) == packedOf(tints.mutedDirectional));
    // THE SWAP: the rule is about which entity was NAMED, not about creation or storage order.
    CHECK(tintAt(second, Vec3{-1.0F, 0.0F, 0.0F}) == packedOf(tints.mutedDirectional));
    CHECK(tintAt(second, Vec3{1.0F, 0.0F, 0.0F}) == packedOf(tints.unselected));
}

TEST_CASE("viewport gizmos: an INVALID activeDirectional mutes nothing (VG7)") {
    World world;
    world.add<DirectionalLight>(seedLight(world, Vec3{-1.0F, 0.0F, 0.0F}), DirectionalLight{});
    world.add<DirectionalLight>(seedLight(world, Vec3{1.0F, 0.0F, 0.0F}), DirectionalLight{});

    rd::DebugDrawBatch batch = roomyBatch();
    ed::ViewportGizmoScratch scratch;
    const ed::ViewportGizmoTints tints{};
    (void)ed::emitViewportGizmos(world, {.activeDirectional = Entity{}, .tints = tints}, scratch, batch);

    std::size_t muted = 0;
    std::size_t bright = 0;
    for (const rd::DebugBillboard& b : batch.billboards(rd::DebugDepth::Overlay)) {
        if (rgbaOf(b.rgba) == packedOf(tints.mutedDirectional)) {
            ++muted;
        }
        if (rgbaOf(b.rgba) == packedOf(tints.unselected)) {
            ++bright;
        }
    }
    CHECK(muted == 0U);
    CHECK(bright == 2U);
}

TEST_CASE("viewport gizmos: selection BEATS muting, in both selected roles (VG8)") {
    World world;
    const Entity active = seedLight(world, Vec3{-1.0F, 0.0F, 0.0F});
    world.add<DirectionalLight>(active, DirectionalLight{});
    const Entity ignored = seedLight(world, Vec3{1.0F, 0.0F, 0.0F});
    world.add<DirectionalLight>(ignored, DirectionalLight{});

    const ed::ViewportGizmoTints tints{};
    ed::ViewportGizmoScratch scratch;
    const auto ignoredTint = [&](Entity primary, std::span<const Entity> selection) {
        rd::DebugDrawBatch batch = roomyBatch();
        const ed::ViewportGizmoParams params{
            .selected = selection, .primary = primary, .activeDirectional = active, .tints = tints};
        (void)ed::emitViewportGizmos(world, params, scratch, batch);
        for (const rd::DebugBillboard& b : batch.billboards(rd::DebugDepth::Overlay)) {
            if (b.center == Vec3{1.0F, 0.0F, 0.0F}) {
                return rgbaOf(b.rgba);
            }
        }
        return Rgba{-1, -1, -1, -1};
    };

    const std::array<Entity, 2> both{active, ignored};
    CHECK(ignoredTint(active, both) == packedOf(tints.secondarySelected));
    CHECK(ignoredTint(ignored, both) == packedOf(tints.primarySelected));
    // The control: unselected, it is muted again.
    CHECK(ignoredTint(Entity{}, std::span<const Entity>{}) == packedOf(tints.mutedDirectional));
}

TEST_CASE("viewport gizmos: entityCap truncates in SELECTION order and reports the rest (VG9)") {
    World world;
    std::vector<Entity> selection;
    // 100 units apart with a range of 1, so a sphere's every vertex identifies its own entity's slot
    // unambiguously -- which is what lets the surviving SET be read off the batch rather than assumed.
    constexpr float SPACING = 100.0F;
    for (int i = 0; i < 9; ++i) {
        const Entity e = seedLight(world, Vec3{static_cast<float>(i) * SPACING, 0.0F, 0.0F});
        world.add<PointLight>(e, PointLight{.range = 1.0F});
        selection.push_back(e);
    }

    rd::DebugDrawBatch batch = roomyBatch();
    ed::ViewportGizmoScratch scratch;
    constexpr std::uint32_t CAP = 4;
    const ed::ViewportGizmoCounts counts =
        ed::emitViewportGizmos(world, {.selected = selection, .entityCap = CAP}, scratch, batch);
    CHECK(counts.gizmoEntities == CAP);
    CHECK(counts.skippedOverCap == 5U);
    CHECK(counts.gizmoLines == CAP * 3U * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS);

    // THE SURVIVING SET, collected from THIS call's OWN OUTPUT and compared element for element --
    // never against a second emitViewportGizmos, which would compare a value with itself (GR8).
    std::vector<int> slots;
    for (const rd::DebugLineVertex& v : batch.lineVertices(rd::DebugDepth::Overlay)) {
        slots.push_back(static_cast<int>(std::lround(v.position.x / SPACING)));
    }
    REQUIRE_FALSE(slots.empty());
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    const std::vector<int> expected{0, 1, 2, 3};
    CHECK(slots == expected);
}

TEST_CASE("viewport gizmos: a dead or null handle in the selection consumes no cap slot (VG10)") {
    World world;
    const Entity doomed = seedLight(world, Vec3::zero());
    world.add<PointLight>(doomed, PointLight{});
    const Entity alive = seedLight(world, Vec3{2.0F, 0.0F, 0.0F});
    world.add<PointLight>(alive, PointLight{});
    REQUIRE(world.destroy(doomed));

    rd::DebugDrawBatch batch = roomyBatch();
    ed::ViewportGizmoScratch scratch;
    const std::array<Entity, 3> selection{doomed, Entity{}, alive};
    const ed::ViewportGizmoCounts counts =
        ed::emitViewportGizmos(world, {.selected = selection, .entityCap = 1U}, scratch, batch);
    // The dead and the null handle are skipped SILENTLY, so `alive` still fits under a cap of one.
    CHECK(counts.gizmoEntities == 1U);
    CHECK(counts.skippedOverCap == 0U);
    CHECK(counts.gizmoLines == 3U * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS);
}

TEST_CASE("viewport gizmos: a Transform-less light sits at the origin and aims down -Z (VG11)") {
    World world;
    // world.create() + add, NEVER the editor's createEntity, which always adds a Transform.
    const Entity spot = world.create();
    world.add<SpotLight>(spot, SpotLight{});
    REQUIRE_FALSE(world.has<Transform>(spot));

    rd::DebugDrawBatch batch = roomyBatch();
    ed::ViewportGizmoScratch scratch;
    const std::array<Entity, 1> selection{spot};
    const ed::ViewportGizmoCounts counts =
        ed::emitViewportGizmos(world, {.selected = selection, .primary = spot}, scratch, batch);

    const rd::DebugBillboard* const icon = iconFor(batch, ed::ViewportIconKind::SpotLight);
    REQUIRE(icon != nullptr);
    CHECK(icon->center == Vec3::zero());  // worldMatrix's identity contribution, documented behaviour
    CHECK(counts.gizmoLines == (2U * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS) + rd::LIGHT_GIZMO_SPOT_RIM_RAYS);

    // Aimed down -Z: every emitted vertex has z <= 0 to within the cap radius's own tolerance, and
    // the far cap centre sits at -range*cos(outer) on the z axis.
    float minZ = 0.0F;
    for (const rd::DebugLineVertex& v : batch.lineVertices(rd::DebugDepth::Overlay)) {
        minZ = v.position.z < minZ ? v.position.z : minZ;
    }
    // The DEEPEST vertex is the INNER cap's plane, not the outer's: cos(inner) > cos(outer), so the
    // inner circle sits further along the axis. Both caps are drawn, because inner < outer by default.
    const SpotLight defaults{};
    CHECK(minZ < 0.0F);
    CHECK(std::abs(minZ + (defaults.range * std::cos(defaults.innerConeRadians))) < 1.0e-4F);
}

TEST_CASE("viewport gizmos: a full billboard budget is COUNTED, on both counters (VG12)") {
    World world;
    for (int i = 0; i < 4; ++i) {
        const Entity e = seedLight(world, Vec3{static_cast<float>(i), 0.0F, 0.0F});
        world.add<PointLight>(e, PointLight{});
    }

    rd::DebugDrawBatch batch{rd::DebugDrawBudget{.maxBillboards = 2}};
    ed::ViewportGizmoScratch scratch;
    const ed::ViewportGizmoCounts counts = ed::emitViewportGizmos(world, {}, scratch, batch);
    // READ BOTH NUMBERS -- the counts struct and the batch -- so a hand-maintained counter cannot
    // drift from the thing it claims to describe.
    CHECK(counts.icons == 2U);
    CHECK(counts.iconsDropped == 2U);
    CHECK(batch.billboardCount() == 2U);
    CHECK(batch.droppedBillboards() == 2U);
    CHECK(batch.rejectedBillboards() == 0U);
}

TEST_CASE("viewport gizmos: the two selection tints round-trip to E.1.4's own sRGB bytes (VG13)") {
    const ed::ViewportGizmoTints tints{};

    const auto encodedByte = [](float linear) {
        return static_cast<int>(std::lround(rd::linearToSrgbEncode(linear) * 255.0F));
    };
    const auto displayByte = [](float srgb) { return static_cast<int>(std::lround(srgb * 255.0F)); };

    SUBCASE("primary == SELECTION_OUTLINE_PRIMARY_DEFAULT, channel for channel") {
        const Vec4 outline = rd::SELECTION_OUTLINE_PRIMARY_DEFAULT;
        CHECK(encodedByte(tints.primarySelected.x) == displayByte(outline.x));
        CHECK(encodedByte(tints.primarySelected.y) == displayByte(outline.y));
        CHECK(encodedByte(tints.primarySelected.z) == displayByte(outline.z));
        // ALPHA IS NOT GAMMA-ENCODED, so it is compared WITHOUT the encode.
        CHECK(displayByte(tints.primarySelected.w) == displayByte(outline.w));
    }
    SUBCASE("secondary == SELECTION_OUTLINE_SECONDARY_DEFAULT, channel for channel") {
        const Vec4 outline = rd::SELECTION_OUTLINE_SECONDARY_DEFAULT;
        CHECK(encodedByte(tints.secondarySelected.x) == displayByte(outline.x));
        CHECK(encodedByte(tints.secondarySelected.y) == displayByte(outline.y));
        CHECK(encodedByte(tints.secondarySelected.z) == displayByte(outline.z));
        CHECK(displayByte(tints.secondarySelected.w) == displayByte(outline.w));
    }
    SUBCASE("the encode is not the identity -- the anti-vacuity arm") {
        // Without this, a broken linearToSrgbEncode returning its argument would satisfy the two
        // subcases above only if the tints happened to equal the display values, which they do not.
        CHECK(encodedByte(tints.primarySelected.y) != displayByte(tints.primarySelected.y));
        CHECK(encodedByte(tints.secondarySelected.z) != displayByte(tints.secondarySelected.z));
    }
}

TEST_CASE("viewport gizmos: emitViewportGizmos emits NO log record on any path (VG14)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST, after the LogSinkScope
    const ed::LogSinkScope scope;
    std::vector<ed::LogEntry> records;

    World world;
    const Entity directional = seedLight(world, Vec3::zero());
    world.add<DirectionalLight>(directional, DirectionalLight{});
    const Entity spot = world.create();  // deliberately Transform-less
    world.add<SpotLight>(spot, SpotLight{.range = -1.0F});
    const Entity point = seedLight(world, Vec3::zero());
    world.add<PointLight>(point, PointLight{.range = std::numeric_limits<float>::quiet_NaN()});
    const Entity camera = seedLight(world, Vec3::zero());
    world.add<Camera>(camera, Camera{});
    const Entity doomed = seedLight(world, Vec3::zero());
    REQUIRE(world.destroy(doomed));

    ed::ViewportGizmoScratch scratch;
    const std::array<Entity, 5> selection{directional, spot, point, camera, doomed};
    {
        rd::DebugDrawBatch batch = roomyBatch();
        (void)ed::emitViewportGizmos(world, {.selected = selection, .primary = spot}, scratch, batch);
    }
    {
        rd::DebugDrawBatch tiny{rd::DebugDrawBudget{.maxLines = 1, .maxBillboards = 1}};
        (void)ed::emitViewportGizmos(world, {.selected = selection, .entityCap = 1U}, scratch, tiny);
    }
    {
        const World empty;
        rd::DebugDrawBatch batch = roomyBatch();
        (void)ed::emitViewportGizmos(empty, {}, scratch, batch);
    }
    scope.sink()->take(records);
    CHECK(records.empty());
}

TEST_CASE("viewport gizmos: the walk uses eachEntity and the header names no scene_render type (VG15)") {
    const std::string source = strippedSourceAt(VIEWPORT_GIZMOS_SOURCE_PATH);
    const std::string header = strippedSourceAt(VIEWPORT_GIZMOS_HEADER_PATH);
    REQUIRE_FALSE(source.empty());
    REQUIRE_FALSE(header.empty());

    // Non-vacuity: the two files really are the ones under test, comment-stripped.
    CHECK(contains(source, "eachEntity"));
    CHECK(contains(header, "emitViewportGizmos"));

    CHECK_FALSE(contains(source, "each<"));
    CHECK_FALSE(contains(source, "AERO_LOG"));
    CHECK_FALSE(contains(header, "scene_render"));
    CHECK_FALSE(contains(source, "does_not_exist_token"));
}

TEST_CASE("viewport gizmos: the scratch is reused without growth and never leaks across calls (VG16)") {
    World world;
    const Entity first = seedLight(world, Vec3{-1.0F, 0.0F, 0.0F});
    world.add<PointLight>(first, PointLight{});
    const Entity second = seedLight(world, Vec3{1.0F, 0.0F, 0.0F});
    world.add<PointLight>(second, PointLight{});

    ed::ViewportGizmoScratch scratch;
    const ed::ViewportGizmoTints tints{};
    const std::array<Entity, 1> selectFirst{first};
    const std::array<Entity, 1> selectSecond{second};

    const ed::ViewportGizmoParams firstParams{.selected = selectFirst, .primary = first, .tints = tints};
    const ed::ViewportGizmoParams secondParams{.selected = selectSecond, .primary = second, .tints = tints};

    rd::DebugDrawBatch warm = roomyBatch();
    (void)ed::emitViewportGizmos(world, firstParams, scratch, warm);
    const std::size_t capacityAfterWarmUp = scratch.selectedIndices.capacity();
    for (int i = 0; i < 10; ++i) {
        rd::DebugDrawBatch batch = roomyBatch();
        (void)ed::emitViewportGizmos(world, firstParams, scratch, batch);
    }
    CHECK(scratch.selectedIndices.capacity() == capacityAfterWarmUp);

    // A STALE scratch must not leak into the next call: assert the TINTS, not merely the counts --
    // the membership set is exactly what decides which icon is amber.
    rd::DebugDrawBatch batch = roomyBatch();
    (void)ed::emitViewportGizmos(world, secondParams, scratch, batch);
    for (const rd::DebugBillboard& b : batch.billboards(rd::DebugDepth::Overlay)) {
        if (b.center == Vec3{-1.0F, 0.0F, 0.0F}) {
            CHECK(rgbaOf(b.rgba) == packedOf(tints.unselected));
        }
        if (b.center == Vec3{1.0F, 0.0F, 0.0F}) {
            CHECK(rgbaOf(b.rgba) == packedOf(tints.primarySelected));
        }
    }
}
