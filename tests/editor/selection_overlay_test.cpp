// tests/editor/selection_overlay_test.cpp — task 2.3.2: the selection turned into screen-space
// segments. Tier-0 and UNGATED. EIGHTH TU of aero_editor_shell_test (no
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here -- shell_test.cpp supplies main()).
#include <aero/core/guid.hpp>
#include <aero/editor/editor_camera.hpp>
#include <aero/editor/picking.hpp>
#include <aero/editor/scene_bounds.hpp>
#include <aero/editor/selection_overlay.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <vector>

using engine::Entity;
using engine::Mat4;
using engine::MeshRenderer;
using engine::Quat;
using engine::Transform;
using engine::Vec2;
using engine::Vec3;
using engine::Vec4;
using engine::World;
using engine::editor::aabbCorner;
using engine::editor::BOX_EDGES;
using engine::editor::BoxEdge;
using engine::editor::buildSelectionOverlay;
using engine::editor::CLIP_W_EPSILON;
using engine::editor::clipSegmentToNearPlane;
using engine::editor::EditorCamera;
using engine::editor::entityBounds;
using engine::editor::MAX_HIGHLIGHTED_ENTITIES;
using engine::editor::MeshBoundsKey;
using engine::editor::MeshBoundsLookup;
using engine::editor::OverlayRole;
using engine::editor::OverlaySegment;
using engine::editor::primitiveLocalBounds;

namespace {

constexpr float EPS = 1.0e-4F;
constexpr float INF_F = std::numeric_limits<float>::infinity();
constexpr Vec2 VIEWPORT_POINTS{800.0F, 600.0F};

// Identical to picking_test.cpp's: eye {0,0,10}, forward {0,0,-1}. Deliberately a SEPARATE copy --
// the two TUs keep their own fixtures (the "copied, not shared" call this tree already records for
// test helpers).
[[nodiscard]] EditorCamera testCamera() {
    EditorCamera camera;
    camera.setPivot(Vec3::zero());
    camera.setYaw(0.0F);
    camera.setPitch(0.0F);
    camera.setDistance(10.0F);
    return camera;
}

[[nodiscard]] Mat4 testViewProj(const EditorCamera& camera) {
    return camera.projectionMatrix(1.0F) * camera.viewMatrix();
}

[[nodiscard]] Entity makeMesh(World& world, Vec3 position, Quat rotation = Quat::identity(), Vec3 scale = Vec3::one()) {
    const Entity e = world.create();
    world.add<Transform>(e, Transform{.position = position, .rotation = rotation, .scale = scale});
    world.add<MeshRenderer>(e, MeshRenderer{});
    return e;
}

[[nodiscard]] Entity makePoint(World& world, Vec3 position) {
    const Entity e = world.create();
    world.add<Transform>(e, Transform{.position = position});
    return e;
}

// Every endpoint of every segment, so a case can reason about the projected corner cloud without
// caring which edge produced which point.
[[nodiscard]] std::vector<Vec2> endpointsOf(const std::vector<OverlaySegment>& segments) {
    std::vector<Vec2> out;
    out.reserve(segments.size() * 2);
    for (const OverlaySegment& s : segments) {
        out.push_back(s.a);
        out.push_back(s.b);
    }
    return out;
}

[[nodiscard]] bool allFinite(const std::vector<OverlaySegment>& segments) {
    return std::all_of(segments.begin(), segments.end(), [](const OverlaySegment& s) {
        return std::isfinite(s.a.x) && std::isfinite(s.a.y) && std::isfinite(s.b.x) && std::isfinite(s.b.y);
    });
}

}  // namespace

TEST_CASE("selection_overlay: BOX_EDGES is the 12-edge table DERIVED from F3b's bit assignment") {
    CHECK(BOX_EDGES.size() == 12);
    std::array<int, 8> degree{};
    std::array<bool, 64> seen{};
    std::array<int, 3> perAxis{};  // how many edges differ in bit 0 / bit 1 / bit 2
    for (const BoxEdge edge : BOX_EDGES) {
        REQUIRE(edge.a < 8);
        REQUIRE(edge.b < 8);
        REQUIRE(edge.a != edge.b);
        const auto diff = static_cast<unsigned>(edge.a ^ edge.b);
        // Adjacent iff they differ in EXACTLY one bit: a power of two, and only bits 0..2 exist.
        CHECK((diff == 1U || diff == 2U || diff == 4U));
        perAxis[diff == 1U ? 0 : (diff == 2U ? 1 : 2)] += 1;
        const std::size_t lo = std::min(edge.a, edge.b);
        const std::size_t hi = std::max(edge.a, edge.b);
        const std::size_t key = (lo * 8U) + hi;
        CHECK_FALSE(seen[key]);  // no duplicates
        seen[key] = true;
        degree[edge.a] += 1;
        degree[edge.b] += 1;
    }
    for (const int d : degree) {
        CHECK(d == 3);  // every corner is in exactly three edges
    }
    for (const int n : perAxis) {
        CHECK(n == 4);  // four edges per axis
    }
}

TEST_CASE("selection_overlay: segment counts per entity kind (AC-13)") {
    World w;
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    std::vector<OverlaySegment> scratch;

    SUBCASE("one selected mesh entity -> 12 segments") {
        const Entity cube = makeMesh(w, Vec3::zero());
        buildSelectionOverlay(w, std::array<Entity, 1>{cube}, cube, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.size() == 12);
    }
    SUBCASE("one selected non-mesh entity -> 4 segments") {
        const Entity light = makePoint(w, Vec3::zero());
        buildSelectionOverlay(w, std::array<Entity, 1>{light}, light, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.size() == 4);
    }
    SUBCASE("two mesh entities -> 24 segments") {
        const Entity a = makeMesh(w, Vec3{1.0F, 0.0F, 0.0F});
        const Entity b = makeMesh(w, Vec3{-1.0F, 0.0F, 0.0F});
        buildSelectionOverlay(w, std::array<Entity, 2>{a, b}, a, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.size() == 24);
    }
    SUBCASE("an empty span -> 0 segments, scratch stays empty (E1)") {
        buildSelectionOverlay(w, {}, Entity{}, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.empty());
    }
}

TEST_CASE("selection_overlay: primary vs selected roles (AC-14)") {
    World w;
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    std::vector<OverlaySegment> scratch;

    const Entity a = makeMesh(w, Vec3{2.0F, 0.0F, 0.0F});
    const Entity b = makeMesh(w, Vec3::zero());
    const Entity c = makeMesh(w, Vec3{-2.0F, 0.0F, 0.0F});
    const std::array<Entity, 3> selected{a, b, c};

    SUBCASE("the second entity is primary") {
        buildSelectionOverlay(w, selected, b, viewProj, VIEWPORT_POINTS, scratch);
        const auto primaryCount = std::count_if(scratch.begin(), scratch.end(),
                                                [](const OverlaySegment& s) { return s.role == OverlayRole::Primary; });
        const auto selectedCount = std::count_if(
            scratch.begin(), scratch.end(), [](const OverlaySegment& s) { return s.role == OverlayRole::Selected; });
        CHECK(primaryCount == 12);
        CHECK(selectedCount == 24);
    }
    SUBCASE("no primary -> zero Primary segments") {
        buildSelectionOverlay(w, selected, Entity{}, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(std::none_of(scratch.begin(), scratch.end(),
                           [](const OverlaySegment& s) { return s.role == OverlayRole::Primary; }));
    }
}

TEST_CASE("selection_overlay: the box tracks the entity's transform (AC-15)") {
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);

    SUBCASE("translate along world X: every endpoint's x increases, y unchanged") {
        World originWorld;
        const Entity origin = makeMesh(originWorld, Vec3::zero());
        std::vector<OverlaySegment> originScratch;
        buildSelectionOverlay(originWorld, std::array<Entity, 1>{origin}, origin, viewProj, VIEWPORT_POINTS,
                              originScratch);

        World movedWorld;
        const Entity moved = makeMesh(movedWorld, Vec3{1.0F, 0.0F, 0.0F});
        std::vector<OverlaySegment> movedScratch;
        buildSelectionOverlay(movedWorld, std::array<Entity, 1>{moved}, moved, viewProj, VIEWPORT_POINTS, movedScratch);

        REQUIRE(originScratch.size() == movedScratch.size());
        for (std::size_t i = 0; i < originScratch.size(); ++i) {
            CHECK(movedScratch[i].a.x > originScratch[i].a.x);
            CHECK(movedScratch[i].b.x > originScratch[i].b.x);
            CHECK(std::abs(movedScratch[i].a.y - originScratch[i].a.y) < EPS);
            CHECK(std::abs(movedScratch[i].b.y - originScratch[i].b.y) < EPS);
        }
    }
    SUBCASE("scale roughly doubles the screen extent") {
        World unscaledWorld;
        const Entity unscaled = makeMesh(unscaledWorld, Vec3::zero());
        std::vector<OverlaySegment> unscaledScratch;
        buildSelectionOverlay(unscaledWorld, std::array<Entity, 1>{unscaled}, unscaled, viewProj, VIEWPORT_POINTS,
                              unscaledScratch);

        World scaledWorld;
        const Entity scaled = makeMesh(scaledWorld, Vec3::zero(), Quat::identity(), Vec3{2.0F, 2.0F, 2.0F});
        std::vector<OverlaySegment> scaledScratch;
        buildSelectionOverlay(scaledWorld, std::array<Entity, 1>{scaled}, scaled, viewProj, VIEWPORT_POINTS,
                              scaledScratch);

        const std::vector<Vec2> unscaledPoints = endpointsOf(unscaledScratch);
        const std::vector<Vec2> scaledPoints = endpointsOf(scaledScratch);
        const auto extentX = [](const std::vector<Vec2>& pts) {
            float lo = pts.front().x;
            float hi = pts.front().x;
            for (const Vec2 p : pts) {
                lo = std::min(lo, p.x);
                hi = std::max(hi, p.x);
            }
            return hi - lo;
        };
        const float extentUnscaled = extentX(unscaledPoints);
        const float extentScaled = extentX(scaledPoints);
        CHECK(extentScaled > 1.8F * extentUnscaled);
    }
    SUBCASE("rotation is the OBB-vs-AABB discriminator (S6)") {
        World rotatedWorld;
        const Entity rotated =
            makeMesh(rotatedWorld, Vec3::zero(), engine::fromAxisAngle(Vec3::unitY(), engine::radians(45.0F)));
        std::vector<OverlaySegment> rotatedScratch;
        buildSelectionOverlay(rotatedWorld, std::array<Entity, 1>{rotated}, rotated, viewProj, VIEWPORT_POINTS,
                              rotatedScratch);

        World uprightWorld;
        const Entity upright = makeMesh(uprightWorld, Vec3::zero());
        std::vector<OverlaySegment> uprightScratch;
        buildSelectionOverlay(uprightWorld, std::array<Entity, 1>{upright}, upright, viewProj, VIEWPORT_POINTS,
                              uprightScratch);

        const std::vector<Vec2> rotatedPoints = endpointsOf(rotatedScratch);
        const std::vector<Vec2> uprightPoints = endpointsOf(uprightScratch);
        const float centreX = VIEWPORT_POINTS.x * 0.5F;
        const auto onCentreLine = [centreX](Vec2 p) { return std::abs(p.x - centreX) < 1.0F; };
        // rotated 45 deg about Y -> at least one projected corner lands on the centre line
        CHECK(std::any_of(rotatedPoints.begin(), rotatedPoints.end(), onCentreLine));
        // the SAME cube unrotated -> none does. This pair is what makes the assertion discriminating.
        CHECK_FALSE(std::any_of(uprightPoints.begin(), uprightPoints.end(), onCentreLine));
    }
}

TEST_CASE("selection_overlay: parenting -- the entity's OWN box, never the subtree's (AC-15/D7)") {
    World w;
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    const Entity parent = makeMesh(w, Vec3{5.0F, 0.0F, 0.0F});
    const Entity child = makeMesh(w, Vec3{0.0F, 1.0F, 0.0F});
    REQUIRE(w.setParent(child, parent));

    std::vector<OverlaySegment> childScratch;
    buildSelectionOverlay(w, std::array<Entity, 1>{child}, child, viewProj, VIEWPORT_POINTS, childScratch);

    World standaloneWorld;
    const Entity standalone = makeMesh(standaloneWorld, Vec3{5.0F, 1.0F, 0.0F});
    std::vector<OverlaySegment> standaloneScratch;
    buildSelectionOverlay(standaloneWorld, std::array<Entity, 1>{standalone}, standalone, viewProj, VIEWPORT_POINTS,
                          standaloneScratch);

    REQUIRE(childScratch.size() == standaloneScratch.size());
    for (std::size_t i = 0; i < childScratch.size(); ++i) {
        CHECK(std::abs(childScratch[i].a.x - standaloneScratch[i].a.x) < EPS);
        CHECK(std::abs(childScratch[i].a.y - standaloneScratch[i].a.y) < EPS);
        CHECK(std::abs(childScratch[i].b.x - standaloneScratch[i].b.x) < EPS);
        CHECK(std::abs(childScratch[i].b.y - standaloneScratch[i].b.y) < EPS);
    }

    std::vector<OverlaySegment> parentScratch;
    buildSelectionOverlay(w, std::array<Entity, 1>{parent}, parent, viewProj, VIEWPORT_POINTS, parentScratch);
    CHECK(parentScratch.size() == 12);  // D7: the PARENT's box only, never the subtree's
}

TEST_CASE("selection_overlay: clipSegmentToNearPlane interpolates in clip space, before the divide (D14)") {
    SUBCASE("both endpoints in front: unchanged, returns true") {
        Vec4 a{1.0F, 2.0F, 3.0F, 4.0F};
        Vec4 b{-1.0F, 0.5F, 2.0F, 1.0F};
        const Vec4 a0 = a;
        const Vec4 b0 = b;
        CHECK(clipSegmentToNearPlane(a, b));
        CHECK(a == a0);
        CHECK(b == b0);
    }
    SUBCASE("both behind: returns false") {
        Vec4 a{1.0F, 1.0F, 1.0F, -1.0F};
        Vec4 b{2.0F, 2.0F, 2.0F, -3.0F};
        CHECK_FALSE(clipSegmentToNearPlane(a, b));
    }
    SUBCASE("straddling: the front endpoint survives untouched, the other lands ON w == CLIP_W_EPSILON") {
        // Chosen so the CLIP-space answer and the POST-DIVIDE answer differ enormously (S8):
        //   clip-space  -> ndc.x  = -0.9998 / 1e-4  ~= -9998
        //   post-divide -> ndc.x  = lerp(3, 1, 0.50005) ~= 1.9999
        Vec4 a{-3.0F, 0.0F, 0.0F, -1.0F};  // behind
        Vec4 b{1.0F, 0.0F, 0.0F, 1.0F};    // in front
        const Vec4 a0 = a;
        const Vec4 b0 = b;
        REQUIRE(clipSegmentToNearPlane(a, b));
        CHECK(b == b0);
        CHECK(std::abs(a.w - CLIP_W_EPSILON) < 1.0e-6F);
        // It lies ON the original clip-space line: the SAME t reproduces every component. That IS
        // what "interpolate before the divide" means.
        const float t = (CLIP_W_EPSILON - a0.w) / (b0.w - a0.w);
        CHECK(std::abs(a.x - (a0.x + ((b0.x - a0.x) * t))) < 1.0e-5F);
        // ...and it is NOT the post-divide lerp, or S8 would be invisible.
        const float postDivide = (a0.x / a0.w) + (((b0.x / b0.w) - (a0.x / a0.w)) * t);
        CHECK(std::abs((a.x / a.w) - postDivide) > 1.0F);
    }
    SUBCASE("a NaN endpoint is treated as behind, never propagated") {
        Vec4 a{0.0F, 0.0F, 0.0F, std::numeric_limits<float>::quiet_NaN()};
        Vec4 b{1.0F, 0.0F, 0.0F, 1.0F};
        CHECK_FALSE(clipSegmentToNearPlane(a, b));
    }
}

TEST_CASE("selection_overlay: behind the camera and straddling the near plane (AC-16/E7)") {
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);

    SUBCASE("entirely behind the eye -> 0 segments") {
        World w;
        const Entity behind = makeMesh(w, Vec3{0.0F, 0.0F, 20.0F});
        std::vector<OverlaySegment> scratch;
        buildSelectionOverlay(w, std::array<Entity, 1>{behind}, behind, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.empty());
    }
    SUBCASE("straddling the eye -> exactly 8 segments, every coordinate finite") {
        World w;
        const Entity straddling = makeMesh(w, Vec3{0.0F, 0.0F, 10.0F}, Quat::identity(), Vec3{4.0F, 4.0F, 4.0F});
        std::vector<OverlaySegment> scratch;
        buildSelectionOverlay(w, std::array<Entity, 1>{straddling}, straddling, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.size() == 8);
        CHECK(allFinite(scratch));
    }
}

TEST_CASE("selection_overlay: a HUGE FINITE transform never emits a non-finite coordinate (E4)") {
    World w;
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    // The hostile-input case below uses position.x = INF, which makes EVERY clip w NaN, so all twelve
    // edges are dropped by clipSegmentToNearPlane BEFORE the finiteness guard is ever reached -- an
    // allFinite() over an empty scratch is vacuous. A huge-but-FINITE scale straddling the eye is what
    // actually reaches the guard: the four Z edges each have one endpoint in front and one behind, so
    // they survive clipping, and the clipped endpoint's x/w then overflows inside ndcToViewportPoints.
    const Entity huge = makeMesh(w, Vec3::zero(), Quat::identity(), Vec3{1.0e34F, 1.0e34F, 1.0e34F});
    std::vector<OverlaySegment> scratch;
    buildSelectionOverlay(w, std::array<Entity, 1>{huge}, huge, viewProj, VIEWPORT_POINTS, scratch);

    CHECK(allFinite(scratch));
    // ANTI-VACUITY, and the whole point of the case: the scratch is NOT empty, so allFinite above has
    // something to be true of. Four edges lie wholly in front of the eye and survive; four lie wholly
    // behind and are dropped by clipSegmentToNearPlane; the four straddling Z edges survive clipping
    // and are dropped HERE, by the finiteness guard. Dropping that guard emits all eight.
    CHECK(scratch.size() == 4);
}

TEST_CASE("selection_overlay: the cap bounds both segment count and Primary role (AC-18/E13/D15)") {
    World w;
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);

    std::vector<Entity> all;
    all.reserve(300);
    for (int i = 0; i < 300; ++i) {
        all.push_back(makeMesh(w, Vec3{static_cast<float>(i) * 0.001F, 0.0F, 0.0F}));
    }

    std::vector<OverlaySegment> scratch;
    buildSelectionOverlay(w, all, Entity{}, viewProj, VIEWPORT_POINTS, scratch);
    CHECK(scratch.size() == 12U * MAX_HIGHLIGHTED_ENTITIES);  // exactly 3072

    // the 300th is BEYOND the cap: making it the primary must draw NO Primary segments (E13)
    buildSelectionOverlay(w, all, all.back(), viewProj, VIEWPORT_POINTS, scratch);
    CHECK(std::none_of(scratch.begin(), scratch.end(),
                       [](const OverlaySegment& s) { return s.role == OverlayRole::Primary; }));

    // ...while a primary INSIDE the cap does get its 12
    buildSelectionOverlay(w, all, all.front(), viewProj, VIEWPORT_POINTS, scratch);
    CHECK(std::count_if(scratch.begin(), scratch.end(),
                        [](const OverlaySegment& s) { return s.role == OverlayRole::Primary; }) == 12);
}

// A7 is a claim about the CAP: a dead handle is skipped BEFORE the counter advances, so stale handles
// never consume another entity's budget. Discriminating it needs MORE than MAX_HIGHLIGHTED_ENTITIES
// live entities with dead handles among them -- the cap case above has 300 live entities and no dead
// ones, and the hostile-input case below builds one live entity alone, which draws its 12 segments
// wherever ++drawn sits.
TEST_CASE("selection_overlay: dead handles do NOT consume cap budget (A7/AC-17/AC-18)") {
    World w;
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);

    // Created and destroyed FIRST, so the live entities below recycle their slots with a bumped
    // generation: every handle in `dead` is then stale by GENERATION, not merely a freed index.
    std::vector<Entity> dead;
    dead.reserve(12);
    for (int i = 0; i < 10; ++i) {
        const Entity doomed = makeMesh(w, Vec3::zero());
        REQUIRE(w.destroy(doomed));
        dead.push_back(doomed);
    }
    dead.push_back(Entity{});  // and two NULL handles, which are dead by generation 0
    dead.push_back(Entity{});

    constexpr std::size_t LIVE_COUNT = MAX_HIGHLIGHTED_ENTITIES + 4;
    std::vector<Entity> live;
    live.reserve(LIVE_COUNT);
    for (std::size_t i = 0; i < LIVE_COUNT; ++i) {
        live.push_back(makeMesh(w, Vec3{static_cast<float>(i) * 0.001F, 0.0F, 0.0F}));
    }
    for (const Entity e : dead) {
        REQUIRE_FALSE(w.alive(e));  // ANTI-VACUITY: recycling must not have revived one of them
    }

    // Two dead handles ahead of everything, then one after every 25 live entities -- so they are both
    // BEFORE and AMONG the live ones, and eleven of them fall inside the first 256 span entries.
    std::vector<Entity> span;
    span.reserve(live.size() + dead.size());
    std::size_t nextDead = 0;
    span.push_back(dead[nextDead++]);
    span.push_back(dead[nextDead++]);
    for (std::size_t i = 0; i < live.size(); ++i) {
        span.push_back(live[i]);
        if ((i % 25) == 24 && nextDead < dead.size()) {
            span.push_back(dead[nextDead++]);
        }
    }
    REQUIRE(nextDead == dead.size());
    REQUIRE(span.size() == live.size() + dead.size());

    std::vector<OverlaySegment> scratch;
    buildSelectionOverlay(w, span, Entity{}, viewProj, VIEWPORT_POINTS, scratch);
    CHECK(scratch.size() == 12U * MAX_HIGHLIGHTED_ENTITIES);  // still exactly 3072, not 12 fewer boxes

    // Sharper than the count: the 256th LIVE entity is the LAST one inside the cap...
    buildSelectionOverlay(w, span, live[MAX_HIGHLIGHTED_ENTITIES - 1], viewProj, VIEWPORT_POINTS, scratch);
    CHECK(std::count_if(scratch.begin(), scratch.end(),
                        [](const OverlaySegment& s) { return s.role == OverlayRole::Primary; }) == 12);
    // ...and the 257th is the FIRST one outside it. Counting dead handles against the budget would
    // push the boundary earlier and redden both of these.
    buildSelectionOverlay(w, span, live[MAX_HIGHLIGHTED_ENTITIES], viewProj, VIEWPORT_POINTS, scratch);
    CHECK(std::none_of(scratch.begin(), scratch.end(),
                       [](const OverlaySegment& s) { return s.role == OverlayRole::Primary; }));
}

TEST_CASE("selection_overlay: hostile input never crashes, never emits non-finite output (AC-17/E4/E5)") {
    World w;
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);

    const Entity live = makeMesh(w, Vec3{1.0F, 0.0F, 0.0F});
    const Entity doomed = makeMesh(w, Vec3{2.0F, 0.0F, 0.0F});
    REQUIRE(w.destroy(doomed));
    const Entity infinitePos = makeMesh(w, Vec3{INF_F, 0.0F, 0.0F});
    const Entity zeroScaled = makeMesh(w, Vec3::zero(), Quat::identity(), Vec3::zero());
    const Entity trailingLive = makeMesh(w, Vec3{-1.0F, 0.0F, 0.0F});

    const std::vector<Entity> mixed{live, doomed, Entity{}, infinitePos, zeroScaled, trailingLive};
    const std::size_t entityCountBefore = w.entityCount();
    const std::size_t meshCountBefore = w.componentCount<MeshRenderer>();

    std::vector<OverlaySegment> scratch;
    buildSelectionOverlay(w, mixed, Entity{}, viewProj, VIEWPORT_POINTS, scratch);

    CHECK(allFinite(scratch));  // no crash; not one non-finite coordinate reaches the output
    CHECK(w.entityCount() == entityCountBefore);
    CHECK(w.componentCount<MeshRenderer>() == meshCountBefore);
    CHECK(mixed.size() == 6);

    // the zero-scaled entity STILL contributes 12 segments, all collapsed onto one point (E5's
    // deliberate asymmetry with pickEntity: you cannot CLICK a zero-volume object, but you must still
    // SEE what you selected).
    std::vector<OverlaySegment> zeroOnlyScratch;
    buildSelectionOverlay(w, std::array<Entity, 1>{zeroScaled}, zeroScaled, viewProj, VIEWPORT_POINTS, zeroOnlyScratch);
    CHECK(zeroOnlyScratch.size() == 12);
    const Vec2 first = zeroOnlyScratch.front().a;
    for (const OverlaySegment& s : zeroOnlyScratch) {
        CHECK(std::abs(s.a.x - first.x) < EPS);
        CHECK(std::abs(s.a.y - first.y) < EPS);
        CHECK(std::abs(s.b.x - first.x) < EPS);
        CHECK(std::abs(s.b.y - first.y) < EPS);
    }

    // the dead handle does NOT consume cap budget (A7): the live entity trailing it in the span still
    // draws its full 12 segments on its own.
    std::vector<OverlaySegment> trailingOnlyScratch;
    buildSelectionOverlay(w, std::array<Entity, 1>{trailingLive}, trailingLive, viewProj, VIEWPORT_POINTS,
                          trailingOnlyScratch);
    CHECK(trailingOnlyScratch.size() == 12);
}

TEST_CASE("selection_overlay: scratch is cleared on entry and reused when warm (AC-18)") {
    World w;
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    const Entity a = makeMesh(w, Vec3{1.0F, 0.0F, 0.0F});
    const Entity b = makeMesh(w, Vec3{-1.0F, 0.0F, 0.0F});
    const std::array<Entity, 2> two{a, b};
    const std::array<Entity, 1> one{a};

    std::vector<OverlaySegment> scratch;
    buildSelectionOverlay(w, two, a, viewProj, VIEWPORT_POINTS, scratch);
    CHECK(scratch.size() == 24);
    buildSelectionOverlay(w, one, a, viewProj, VIEWPORT_POINTS, scratch);
    CHECK(scratch.size() == 12);  // CLEARED on entry, not appended to
    const std::size_t warmCapacity = scratch.capacity();
    buildSelectionOverlay(w, one, a, viewProj, VIEWPORT_POINTS, scratch);
    CHECK(scratch.capacity() == warmCapacity);  // warm: an identical second call does not grow
}

// ================================================================================================
// task 3.1.5 (VP1-VP4): the highlight resolves its box through the SAME localBoundsFor the frame walk
// and the pick use (INV-D6), and an unresolved reference draws the non-mesh marker.
// ================================================================================================

namespace {
[[nodiscard]] engine::Guid overlayMeshGuid(std::uint64_t ordinal) { return engine::Guid{ordinal, 0xDEEDULL}; }

// The 8 projected corners of `box` under `model`, computed with aabbCorner and projectToViewport --
// the same two functions the overlay uses, from the OTHER side. A case comparing these against the
// overlay's own endpoints proves the overlay drew THAT box and no other.
[[nodiscard]] std::vector<Vec2> projectedCornersOf(const Mat4& viewProj, const Mat4& model,
                                                   const engine::editor::Aabb& box) {
    std::vector<Vec2> out;
    for (std::size_t i = 0; i < 8; ++i) {
        Vec2 point{};
        REQUIRE(engine::editor::projectToViewport(viewProj, engine::transformPoint(model, aabbCorner(box, i)),
                                                  VIEWPORT_POINTS, point));
        out.push_back(point);
    }
    return out;
}

[[nodiscard]] bool containsPoint(const std::vector<Vec2>& haystack, Vec2 needle) {
    return std::any_of(haystack.begin(), haystack.end(), [needle](Vec2 candidate) {
        return std::abs(candidate.x - needle.x) < EPS && std::abs(candidate.y - needle.y) < EPS;
    });
}
}  // namespace

TEST_CASE("selection_overlay: a PRIMITIVE's highlight draws exactly the bounds walk's box (VP1)") {
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    World w;
    const Entity e = makeMesh(w, Vec3{0.5F, 0.0F, 0.0F}, Quat::identity(), Vec3{2.0F, 1.0F, 1.0F});
    const std::array<Entity, 1> selected{e};

    std::vector<OverlaySegment> segments;
    buildSelectionOverlay(w, selected, e, viewProj, VIEWPORT_POINTS, segments);
    REQUIRE(segments.size() == 12);

    const engine::editor::Aabb local = primitiveLocalBounds(0);
    const std::vector<Vec2> corners = projectedCornersOf(viewProj, engine::worldMatrix(w, e), local);
    for (const Vec2 endpoint : endpointsOf(segments)) {
        CHECK(containsPoint(corners, endpoint));
    }
    // ...and the WORLD box the frame walk builds is the same eight points' extent -- both halves of
    // INV-D6 asserted against one another rather than each against its own implementation.
    const engine::editor::Aabb framed = entityBounds(w, e, /*includeDescendants=*/false);
    engine::editor::Aabb rebuilt = engine::editor::Aabb::empty();
    for (std::size_t i = 0; i < 8; ++i) {
        rebuilt.expand(engine::transformPoint(engine::worldMatrix(w, e), aabbCorner(local, i)));
    }
    CHECK(engine::approxEquals(framed.min, rebuilt.min));
    CHECK(engine::approxEquals(framed.max, rebuilt.max));
}

TEST_CASE("selection_overlay: a RESOLVED reference's highlight draws the referenced box (VP2)") {
    // S32's witness: an overlay that used primitiveLocalBounds for a reference entity draws a unit
    // cube here instead of the 3 x 1 x 2 box the lookup published.
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    World w;
    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e, Transform{.position = Vec3::zero()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(e, MeshRenderer{.mesh = overlayMeshGuid(1), .meshIndex = 0}) != nullptr);
    const std::array<Entity, 1> selected{e};

    const engine::editor::Aabb local{Vec3{-3.0F, -1.0F, -2.0F}, Vec3{3.0F, 1.0F, 2.0F}};
    MeshBoundsLookup lookup;
    lookup.set(MeshBoundsKey{overlayMeshGuid(1), 0}, local);

    std::vector<OverlaySegment> segments;
    buildSelectionOverlay(w, selected, e, viewProj, VIEWPORT_POINTS, segments, &lookup);
    REQUIRE(segments.size() == 12);
    CHECK(allFinite(segments));

    const std::vector<Vec2> corners = projectedCornersOf(viewProj, engine::worldMatrix(w, e), local);
    for (const Vec2 endpoint : endpointsOf(segments)) {
        CHECK(containsPoint(corners, endpoint));
    }
    // The control: the CUBE's corners are a strictly narrower cloud, so a box drawn from
    // primitiveLocalBounds cannot satisfy the loop above.
    const std::vector<Vec2> cubeCorners =
        projectedCornersOf(viewProj, engine::worldMatrix(w, e), primitiveLocalBounds(0));
    CHECK_FALSE(containsPoint(cubeCorners, corners[0]));

    // And the frame walk agrees, entity for entity.
    const engine::editor::Aabb framed = entityBounds(w, e, false, &lookup);
    CHECK(engine::approxEquals(framed.min, local.min));
    CHECK(engine::approxEquals(framed.max, local.max));
}

TEST_CASE("selection_overlay: an UNRESOLVED reference draws the DIAMOND, not a box (VP3)") {
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    World w;
    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e, Transform{.position = Vec3{1.0F, 0.0F, 0.0F}}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(e, MeshRenderer{.mesh = overlayMeshGuid(1), .meshIndex = 0}) != nullptr);
    const std::array<Entity, 1> selected{e};
    std::vector<OverlaySegment> segments;

    SUBCASE("no lookup published yet") {
        buildSelectionOverlay(w, selected, e, viewProj, VIEWPORT_POINTS, segments);
        CHECK(segments.size() == 4);  // the 4-segment marker, never the 12-edge box
        CHECK(allFinite(segments));
    }
    SUBCASE("a lookup that does not hold this key") {
        MeshBoundsLookup lookup;
        lookup.set(MeshBoundsKey{overlayMeshGuid(2), 0},
                   engine::editor::Aabb{Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, 1.0F, 1.0F}});
        buildSelectionOverlay(w, selected, e, viewProj, VIEWPORT_POINTS, segments, &lookup);
        CHECK(segments.size() == 4);
    }
    SUBCASE("the marker is a CLOSED diamond around the entity's projected origin") {
        buildSelectionOverlay(w, selected, e, viewProj, VIEWPORT_POINTS, segments);
        REQUIRE(segments.size() == 4);
        Vec2 origin{};
        REQUIRE(engine::editor::projectToViewport(viewProj, Vec3{1.0F, 0.0F, 0.0F}, VIEWPORT_POINTS, origin));
        for (const OverlaySegment& s : segments) {
            CHECK(std::abs(engine::length(s.a - origin) - engine::editor::POINT_MARKER_HALF_POINTS) < EPS);
        }
        CHECK(segments[3].b == segments[0].a);  // closed
    }
    SUBCASE("once resolved, the SAME entity draws 12 edges") {
        MeshBoundsLookup lookup;
        lookup.set(MeshBoundsKey{overlayMeshGuid(1), 0},
                   engine::editor::Aabb{Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, 1.0F, 1.0F}});
        buildSelectionOverlay(w, selected, e, viewProj, VIEWPORT_POINTS, segments, &lookup);
        CHECK(segments.size() == 12);
    }
}

TEST_CASE("selection_overlay: the FLAT plane still draws 12 edges, 4 of them degenerate (VP4)") {
    // A zero-thickness box has 12 edges like any other; the four Y edges collapse to points on screen.
    // They must still be EMITTED (so the edge count is a constant a reader can rely on) and FINITE (so
    // ImDrawList never sees a NaN).
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    World w;
    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e, Transform{.position = Vec3::zero()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(e, MeshRenderer{.primitive = 2}) != nullptr);
    const std::array<Entity, 1> selected{e};

    std::vector<OverlaySegment> segments;
    buildSelectionOverlay(w, selected, e, viewProj, VIEWPORT_POINTS, segments);
    REQUIRE(segments.size() == 12);
    CHECK(allFinite(segments));

    std::size_t degenerate = 0;
    for (const OverlaySegment& s : segments) {
        if (engine::length(s.a - s.b) < EPS) {
            ++degenerate;
        }
    }
    CHECK(degenerate == 4);  // the four Y edges of a zero-thickness box, and only those
}
