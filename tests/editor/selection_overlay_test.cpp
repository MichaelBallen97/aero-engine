// tests/editor/selection_overlay_test.cpp — task 2.3.2: the selection turned into screen-space
// segments. Tier-0 and UNGATED. EIGHTH TU of aero_editor_shell_test (no
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here -- shell_test.cpp supplies main()).
//
// task E.1.4 RETIRED THE AABB HIGHLIGHT, and four cases went with it. Where each one's property now
// lives, so the migration is visible rather than silently lost:
//   * "BOX_EDGES is the 12-edge table DERIVED from F3b's bit assignment" -- the table is gone.
//     aabbCorner's enumeration stays covered by scene_bounds_test.cpp and picking_test.cpp.
//   * VP1 (a PRIMITIVE's highlight draws the bounds walk's box)  -> SQ2's primitive arm + OG6
//   * VP2 (a RESOLVED reference draws the referenced box)        -> SQ2's resolved arm + OG6
//   * VP4 (the FLAT plane still draws 12 edges)                  -> OG10
// VP3 is RE-POINTED: an entity in the marker list draws a diamond, and its *unresolved-reference*
// half moved to SQ10. Every remaining case keeps its id and its intent; only the magnitudes change,
// from 12 box edges per entity to 4 marker segments.
#include <aero/core/guid.hpp>
#include <aero/editor/editor_camera.hpp>
#include <aero/editor/picking.hpp>
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
using engine::editor::buildSelectionOverlay;
using engine::editor::CLIP_W_EPSILON;
using engine::editor::clipSegmentToNearPlane;
using engine::editor::EditorCamera;
using engine::editor::MAX_HIGHLIGHTED_ENTITIES;
using engine::editor::OverlayRole;
using engine::editor::OverlaySegment;

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

TEST_CASE("selection_overlay: segment counts per entity kind (AC-13)") {
    // task E.1.4: the count is 4 per entity REGARDLESS of kind, because `entities` IS the marker list
    // and this builder no longer decides what has geometry. A mesh entity reaches this list only when
    // buildSelectionMaskSet found no instance for it, which is exactly SQ3's and SQ10's subject.
    World w;
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    std::vector<OverlaySegment> scratch;

    SUBCASE("one selected mesh entity -> 4 segments") {
        const Entity cube = makeMesh(w, Vec3::zero());
        buildSelectionOverlay(w, std::array<Entity, 1>{cube}, cube, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.size() == 4);
    }
    SUBCASE("one selected non-mesh entity -> 4 segments, the SAME count") {
        const Entity light = makePoint(w, Vec3::zero());
        buildSelectionOverlay(w, std::array<Entity, 1>{light}, light, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.size() == 4);
    }
    SUBCASE("two entities -> 8 segments") {
        const Entity a = makeMesh(w, Vec3{1.0F, 0.0F, 0.0F});
        const Entity b = makeMesh(w, Vec3{-1.0F, 0.0F, 0.0F});
        buildSelectionOverlay(w, std::array<Entity, 2>{a, b}, a, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.size() == 8);
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
        CHECK(primaryCount == 4);
        CHECK(selectedCount == 8);
    }
    SUBCASE("no primary -> zero Primary segments") {
        buildSelectionOverlay(w, selected, Entity{}, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(std::none_of(scratch.begin(), scratch.end(),
                           [](const OverlaySegment& s) { return s.role == OverlayRole::Primary; }));
    }
}

TEST_CASE("selection_overlay: the marker tracks the entity's transform (AC-15)") {
    // task E.1.4: the diamond's CENTRE follows the world origin, and its SIZE and ORIENTATION are
    // screen-space constants -- which is what a marker is for. The two subcases below that used to
    // assert a box growing with scale and turning with rotation now assert exactly the opposite,
    // because that is what the marker actually does and asserting the effect is the rule.
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
    SUBCASE("scale does NOT change the marker: it is a screen-space constant") {
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
        // IDENTICAL, not merely close: both diamonds are POINT_MARKER_HALF_POINTS about the same
        // projected origin, and the projection of that origin does not depend on the entity's scale.
        CHECK(std::abs(extentScaled - extentUnscaled) < EPS);
        CHECK(extentUnscaled > 0.0F);  // anti-vacuity: the marker has a real width to compare
    }
    SUBCASE("rotation about the entity's OWN origin does not move the marker") {
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
        // The marker's centre is transformPoint(model, Vec3::zero()), which a rotation ABOUT that
        // origin leaves exactly where it was -- so the two diamonds coincide, point for point. This
        // is the property that USED to be the OBB-vs-AABB discriminator; retiring the box retired the
        // question, and the honest replacement is the marker's own invariance.
        REQUIRE(rotatedPoints.size() == uprightPoints.size());
        REQUIRE(rotatedPoints.size() == 8U);
        for (std::size_t i = 0; i < rotatedPoints.size(); ++i) {
            CHECK(std::abs(rotatedPoints[i].x - uprightPoints[i].x) < EPS);
            CHECK(std::abs(rotatedPoints[i].y - uprightPoints[i].y) < EPS);
        }
    }
}

TEST_CASE("selection_overlay: parenting -- the entity's OWN origin, never the subtree's (AC-15/D7)") {
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
    CHECK(parentScratch.size() == 4);  // D7: the PARENT's own origin only, never the subtree's
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
    SUBCASE("AT the eye -> 0 segments: the marker's own origin fails projectToViewport") {
        // task E.1.4: the 12-edge box could STRADDLE the near plane and emit the surviving edges; a
        // marker is a single projected point, so it is all-or-nothing. An entity whose origin sits at
        // or behind the eye contributes NOTHING, silently, whatever its scale.
        World w;
        const Entity straddling = makeMesh(w, Vec3{0.0F, 0.0F, 10.0F}, Quat::identity(), Vec3{4.0F, 4.0F, 4.0F});
        std::vector<OverlaySegment> scratch;
        buildSelectionOverlay(w, std::array<Entity, 1>{straddling}, straddling, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.empty());
    }
    SUBCASE("just IN FRONT of the eye -> the full 4 segments, every coordinate finite") {
        // ANTI-VACUITY for the two arms above: the same fixture one unit nearer DOES draw, so
        // "0 segments" is a decision rather than a builder that never emits anything here.
        World w;
        const Entity ahead = makeMesh(w, Vec3{0.0F, 0.0F, 9.0F}, Quat::identity(), Vec3{4.0F, 4.0F, 4.0F});
        std::vector<OverlaySegment> scratch;
        buildSelectionOverlay(w, std::array<Entity, 1>{ahead}, ahead, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.size() == 4);
        CHECK(allFinite(scratch));
    }
}

TEST_CASE("selection_overlay: a HUGE FINITE transform never emits a non-finite coordinate (E4)") {
    World w;
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    // task E.1.4: a marker is projected from the entity's ORIGIN, so a huge SCALE cannot move it at
    // all -- the coordinate that can overflow is the POSITION. Both are driven here, and both must
    // reach the output finite or not at all: projectToViewport's own finiteness contract is what
    // appendPointMarker relies on, which is why this builder no longer carries a guard of its own.
    const Entity hugeScale = makeMesh(w, Vec3::zero(), Quat::identity(), Vec3{1.0e34F, 1.0e34F, 1.0e34F});
    std::vector<OverlaySegment> scaleScratch;
    buildSelectionOverlay(w, std::array<Entity, 1>{hugeScale}, hugeScale, viewProj, VIEWPORT_POINTS, scaleScratch);
    CHECK(allFinite(scaleScratch));
    // ANTI-VACUITY: the scratch is NOT empty, so allFinite above has something to be true of -- and
    // the marker is exactly where an UNSCALED entity at the same origin would put it.
    REQUIRE(scaleScratch.size() == 4);
    World plainWorld;
    const Entity plain = makeMesh(plainWorld, Vec3::zero());
    std::vector<OverlaySegment> plainScratch;
    buildSelectionOverlay(plainWorld, std::array<Entity, 1>{plain}, plain, viewProj, VIEWPORT_POINTS, plainScratch);
    REQUIRE(plainScratch.size() == 4);
    for (std::size_t i = 0; i < scaleScratch.size(); ++i) {
        CHECK(std::abs(scaleScratch[i].a.x - plainScratch[i].a.x) < EPS);
        CHECK(std::abs(scaleScratch[i].a.y - plainScratch[i].a.y) < EPS);
    }

    // A huge POSITION: whatever survives must be finite, and nothing here may emit a NaN.
    const Entity hugePosition = makeMesh(w, Vec3{1.0e34F, 1.0e34F, -1.0e34F});
    std::vector<OverlaySegment> positionScratch;
    buildSelectionOverlay(w, std::array<Entity, 1>{hugePosition}, hugePosition, viewProj, VIEWPORT_POINTS,
                          positionScratch);
    CHECK(allFinite(positionScratch));
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
    CHECK(scratch.size() == 4U * MAX_HIGHLIGHTED_ENTITIES);  // exactly 1024

    // the 300th is BEYOND the cap: making it the primary must draw NO Primary segments (E13)
    buildSelectionOverlay(w, all, all.back(), viewProj, VIEWPORT_POINTS, scratch);
    CHECK(std::none_of(scratch.begin(), scratch.end(),
                       [](const OverlaySegment& s) { return s.role == OverlayRole::Primary; }));

    // ...while a primary INSIDE the cap does get its 4
    buildSelectionOverlay(w, all, all.front(), viewProj, VIEWPORT_POINTS, scratch);
    CHECK(std::count_if(scratch.begin(), scratch.end(),
                        [](const OverlaySegment& s) { return s.role == OverlayRole::Primary; }) == 4);
}

// A7 is a claim about the CAP: a dead handle is skipped BEFORE the counter advances, so stale handles
// never consume another entity's budget. Discriminating it needs MORE than MAX_HIGHLIGHTED_ENTITIES
// live entities with dead handles among them -- the cap case above has 300 live entities and no dead
// ones, and the hostile-input case below builds one live entity alone, which draws its 4 segments
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
    CHECK(scratch.size() == 4U * MAX_HIGHLIGHTED_ENTITIES);  // still exactly 1024, not 12 fewer markers

    // Sharper than the count: the 256th LIVE entity is the LAST one inside the cap...
    buildSelectionOverlay(w, span, live[MAX_HIGHLIGHTED_ENTITIES - 1], viewProj, VIEWPORT_POINTS, scratch);
    CHECK(std::count_if(scratch.begin(), scratch.end(),
                        [](const OverlaySegment& s) { return s.role == OverlayRole::Primary; }) == 4);
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

    // the zero-scaled entity STILL contributes its 4 segments (E5's deliberate asymmetry with
    // pickEntity: you cannot CLICK a zero-volume object, but you must still SEE what you selected).
    // task E.1.4: they no longer COLLAPSE onto a point -- a marker's size is a screen-space constant,
    // so the diamond is full size, which is strictly more useful and is the whole reason E5 has no
    // determinant guard.
    std::vector<OverlaySegment> zeroOnlyScratch;
    buildSelectionOverlay(w, std::array<Entity, 1>{zeroScaled}, zeroScaled, viewProj, VIEWPORT_POINTS, zeroOnlyScratch);
    CHECK(zeroOnlyScratch.size() == 4);
    Vec2 zeroOrigin{};
    REQUIRE(engine::editor::projectToViewport(viewProj, Vec3::zero(), VIEWPORT_POINTS, zeroOrigin));
    for (const OverlaySegment& s : zeroOnlyScratch) {
        CHECK(std::abs(engine::length(s.a - zeroOrigin) - engine::editor::POINT_MARKER_HALF_POINTS) < EPS);
    }

    // the dead handle does NOT consume cap budget (A7): the live entity trailing it in the span still
    // draws its full 4 segments on its own.
    std::vector<OverlaySegment> trailingOnlyScratch;
    buildSelectionOverlay(w, std::array<Entity, 1>{trailingLive}, trailingLive, viewProj, VIEWPORT_POINTS,
                          trailingOnlyScratch);
    CHECK(trailingOnlyScratch.size() == 4);
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
    CHECK(scratch.size() == 8);
    buildSelectionOverlay(w, one, a, viewProj, VIEWPORT_POINTS, scratch);
    CHECK(scratch.size() == 4);  // CLEARED on entry, not appended to
    const std::size_t warmCapacity = scratch.capacity();
    buildSelectionOverlay(w, one, a, viewProj, VIEWPORT_POINTS, scratch);
    CHECK(scratch.capacity() == warmCapacity);  // warm: an identical second call does not grow
}

// ================================================================================================
// task E.1.4 (VP3, VP7-VP10): `entities` IS THE MARKER LIST. This builder no longer resolves a box,
// no longer takes a MeshBoundsLookup and no longer decides what has geometry -- that decision is
// scene_render::buildSelectionMaskSet's, made once per tick and consumed twice, and SQ3/SQ10 are
// where it is asserted.
// ================================================================================================

namespace {
[[nodiscard]] engine::Guid overlayMeshGuid(std::uint64_t ordinal) { return engine::Guid{ordinal, 0xDEEDULL}; }
}  // namespace

TEST_CASE("selection_overlay: an entity IN THE MARKER LIST draws the diamond (VP3)") {
    // RE-POINTED by task E.1.4. This case used to be about an UNRESOLVED mesh reference falling
    // through to the marker; that half is SQ10's now, because deciding "unresolved" is
    // buildSelectionMaskSet's job and not this builder's. What survives here -- and what this builder
    // still owns entirely -- is the SHAPE of the marker it draws for whatever it is handed.
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    World w;
    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e, Transform{.position = Vec3{1.0F, 0.0F, 0.0F}}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(e, MeshRenderer{.mesh = overlayMeshGuid(1), .meshIndex = 0}) != nullptr);
    const std::array<Entity, 1> selected{e};
    std::vector<OverlaySegment> segments;

    SUBCASE("four segments, whatever the entity carries") {
        buildSelectionOverlay(w, selected, e, viewProj, VIEWPORT_POINTS, segments);
        CHECK(segments.size() == 4);
        CHECK(allFinite(segments));
    }
    SUBCASE("...and a MeshRenderer with a NIL mesh is treated no differently") {
        // The builder does not look at MeshRenderer at all any more, and this is what says so: an
        // entity that WOULD have drawn a primitive box gets the same four segments.
        World plainWorld;
        const Entity plain = makeMesh(plainWorld, Vec3{1.0F, 0.0F, 0.0F});
        buildSelectionOverlay(plainWorld, std::array<Entity, 1>{plain}, plain, viewProj, VIEWPORT_POINTS, segments);
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
}

TEST_CASE("selection_overlay: N marker entities produce exactly 4N segments (VP7)") {
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    World w;
    std::vector<Entity> all;
    all.reserve(MAX_HIGHLIGHTED_ENTITIES);
    for (std::size_t i = 0; i < MAX_HIGHLIGHTED_ENTITIES; ++i) {
        all.push_back(makePoint(w, Vec3{static_cast<float>(i) * 0.001F, 0.0F, 0.0F}));
    }
    std::vector<OverlaySegment> scratch;
    for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{5}, MAX_HIGHLIGHTED_ENTITIES}) {
        INFO("N = ", n);
        buildSelectionOverlay(w, std::span<const Entity>{all.data(), n}, Entity{}, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.size() == 4U * n);
        CHECK(allFinite(scratch));
    }
}

TEST_CASE("selection_overlay: the primary's four segments carry Primary, the rest Selected (VP8)") {
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    World w;
    const Entity a = makePoint(w, Vec3{1.0F, 0.0F, 0.0F});
    const Entity b = makePoint(w, Vec3::zero());
    const Entity c = makePoint(w, Vec3{-1.0F, 0.0F, 0.0F});
    const std::array<Entity, 3> selected{a, b, c};
    std::vector<OverlaySegment> scratch;

    buildSelectionOverlay(w, selected, b, viewProj, VIEWPORT_POINTS, scratch);
    REQUIRE(scratch.size() == 12);
    const auto primaryCount = std::count_if(scratch.begin(), scratch.end(),
                                            [](const OverlaySegment& s) { return s.role == OverlayRole::Primary; });
    CHECK(primaryCount == 4);
    // ...and the four are CONTIGUOUS and are the SECOND entity's, so the role follows the handle
    // rather than landing on whichever four happened to be emitted first.
    for (std::size_t i = 0; i < scratch.size(); ++i) {
        const bool expectPrimary = i >= 4U && i < 8U;
        INFO("segment ", i);
        CHECK((scratch[i].role == OverlayRole::Primary) == expectPrimary);
    }

    SUBCASE("a primary handle ABSENT from the list produces no Primary segment at all") {
        const Entity elsewhere = makePoint(w, Vec3{0.0F, 5.0F, 0.0F});
        buildSelectionOverlay(w, selected, elsewhere, viewProj, VIEWPORT_POINTS, scratch);
        CHECK(scratch.size() == 12);
        CHECK(std::none_of(scratch.begin(), scratch.end(),
                           [](const OverlaySegment& s) { return s.role == OverlayRole::Primary; }));
    }
}

TEST_CASE("selection_overlay: behind the eye or non-finite contributes NOTHING, silently (VP9)") {
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    World w;
    const Entity before = makePoint(w, Vec3{-2.0F, 0.0F, 0.0F});
    const Entity behindEye = makePoint(w, Vec3{0.0F, 0.0F, 20.0F});
    const Entity nonFinite = makePoint(w, Vec3{INF_F, 0.0F, 0.0F});
    const Entity after = makePoint(w, Vec3{2.0F, 0.0F, 0.0F});
    const std::array<Entity, 4> selected{before, behindEye, nonFinite, after};

    std::vector<OverlaySegment> scratch;
    buildSelectionOverlay(w, selected, Entity{}, viewProj, VIEWPORT_POINTS, scratch);
    // TWO entities drew, not four -- and the SURROUNDING two are unaffected, which is the claim that
    // a builder bailing out of the whole walk on the first bad entity would fail.
    CHECK(scratch.size() == 8);
    CHECK(allFinite(scratch));

    // ...and each of the two survivors is exactly where it would be on its own.
    std::vector<OverlaySegment> aloneScratch;
    buildSelectionOverlay(w, std::array<Entity, 1>{before}, Entity{}, viewProj, VIEWPORT_POINTS, aloneScratch);
    REQUIRE(aloneScratch.size() == 4);
    for (std::size_t i = 0; i < aloneScratch.size(); ++i) {
        CHECK(std::abs(scratch[i].a.x - aloneScratch[i].a.x) < EPS);
        CHECK(std::abs(scratch[i].a.y - aloneScratch[i].a.y) < EPS);
    }
    std::vector<OverlaySegment> trailingScratch;
    buildSelectionOverlay(w, std::array<Entity, 1>{after}, Entity{}, viewProj, VIEWPORT_POINTS, trailingScratch);
    REQUIRE(trailingScratch.size() == 4);
    for (std::size_t i = 0; i < trailingScratch.size(); ++i) {
        CHECK(std::abs(scratch[i + 4U].a.x - trailingScratch[i].a.x) < EPS);
        CHECK(std::abs(scratch[i + 4U].a.y - trailingScratch[i].a.y) < EPS);
    }
}

TEST_CASE("selection_overlay: the builder clears its scratch and mutates NOTHING (VP10)") {
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = testViewProj(camera);
    World w;
    const Entity a = makePoint(w, Vec3{1.0F, 0.0F, 0.0F});
    const Entity mesh = makeMesh(w, Vec3{-1.0F, 0.0F, 0.0F});
    const std::array<Entity, 2> selected{a, mesh};

    // A PRE-FILLED scratch, with a sentinel a caller could recognise: it must be gone.
    std::vector<OverlaySegment> scratch;
    scratch.push_back(OverlaySegment{.a = Vec2{-999.0F, -999.0F}, .b = Vec2{-998.0F, -998.0F}});
    scratch.push_back(OverlaySegment{.a = Vec2{-997.0F, -997.0F}, .b = Vec2{-996.0F, -996.0F}});

    const std::size_t entitiesBefore = w.entityCount();
    const std::size_t transformsBefore = w.componentCount<Transform>();
    const std::size_t meshesBefore = w.componentCount<MeshRenderer>();

    buildSelectionOverlay(w, selected, a, viewProj, VIEWPORT_POINTS, scratch);

    CHECK(scratch.size() == 8);  // CLEARED on entry, not appended to
    CHECK(std::none_of(scratch.begin(), scratch.end(), [](const OverlaySegment& s) { return s.a.x < -900.0F; }));
    // ...and the World is untouched: no entity created or destroyed, no component added or removed.
    // The builder specifically does NOT prune -- that is 2.2.1's job, done by the Hierarchy.
    CHECK(w.entityCount() == entitiesBefore);
    CHECK(w.componentCount<Transform>() == transformsBefore);
    CHECK(w.componentCount<MeshRenderer>() == meshesBefore);
    CHECK(w.alive(a));
    CHECK(w.alive(mesh));
}
