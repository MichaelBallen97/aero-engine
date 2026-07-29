// tests/editor/gizmo_test.cpp — task 2.3.3: the pure gizmo model's tier-0 battery. Ninth TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0/ungated: must pass identically with
// AERO_REQUIRE_GPU unset and set.
//
// Step 2 lands cases G1-G4, G16, G17 -- the tool-state model (nextGizmoMode, effectiveSpace,
// gizmoSnapStep, gizmoDragEdge) and enum totality. Step 3 appends G5-G15 -- the geometry and the
// single-channel write pipeline.
#include <aero/editor/gizmo.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <optional>

using engine::approxEquals;
using engine::Entity;
using engine::fromAxisAngle;
using engine::inverse;
using engine::localMatrix;
using engine::Mat4;
using engine::Quat;
using engine::radians;
using engine::scaling;
using engine::toMat4;
using engine::Transform;
using engine::transformPoint;
using engine::translation;
using engine::Trs;
using engine::Vec2;
using engine::Vec3;
using engine::Vec4;
using engine::World;
using engine::editor::effectiveSpace;
using engine::editor::GIZMO_SNAP_ROTATE_DEGREES;
using engine::editor::GIZMO_SNAP_SCALE;
using engine::editor::GIZMO_SNAP_TRANSLATE;
using engine::editor::GizmoDragEdge;
using engine::editor::gizmoDragEdge;
using engine::editor::GizmoMode;
using engine::editor::GizmoModeInput;
using engine::editor::gizmoModelMatrix;
using engine::editor::GizmoOperation;
using engine::editor::gizmoOriginBehindCamera;
using engine::editor::gizmoParentMatrix;
using engine::editor::gizmoSnapStep;
using engine::editor::GizmoSpace;
using engine::editor::GizmoWrite;
using engine::editor::gizmoWriteFromWorld;
using engine::editor::GizmoWriteStatus;
using engine::editor::nextGizmoMode;

namespace {
constexpr std::array<GizmoOperation, 3> ALL_OPERATIONS = {GizmoOperation::Translate, GizmoOperation::Rotate,
                                                          GizmoOperation::Scale};
constexpr std::array<GizmoSpace, 2> ALL_SPACES = {GizmoSpace::Local, GizmoSpace::World};
constexpr float TIGHT_EPS = 1.0e-4F;
}  // namespace

TEST_CASE("gizmo: nextGizmoMode operation keys (G1)") {
    for (const GizmoOperation start : ALL_OPERATIONS) {
        for (const GizmoSpace space : ALL_SPACES) {
            const GizmoMode current{.operation = start, .space = space};

            const GizmoMode afterW = nextGizmoMode(current, GizmoModeInput{.translatePressed = true});
            CHECK(afterW.operation == GizmoOperation::Translate);
            CHECK(afterW.space == space);

            const GizmoMode afterE = nextGizmoMode(current, GizmoModeInput{.rotatePressed = true});
            CHECK(afterE.operation == GizmoOperation::Rotate);
            CHECK(afterE.space == space);

            const GizmoMode afterR = nextGizmoMode(current, GizmoModeInput{.scalePressed = true});
            CHECK(afterR.operation == GizmoOperation::Scale);
            CHECK(afterR.space == space);

            // Nothing pressed -- current returned unchanged, both fields.
            const GizmoMode unchanged = nextGizmoMode(current, GizmoModeInput{});
            CHECK(unchanged == current);
        }
    }

    SUBCASE("W and X in the same frame apply BOTH") {
        const GizmoMode current{.operation = GizmoOperation::Rotate, .space = GizmoSpace::World};
        const GizmoMode next =
            nextGizmoMode(current, GizmoModeInput{.translatePressed = true, .spaceTogglePressed = true});
        CHECK(next.operation == GizmoOperation::Translate);
        CHECK(next.space == GizmoSpace::Local);
    }
}

TEST_CASE("gizmo: nextGizmoMode space toggle is an involution (G2)") {
    for (const GizmoSpace start : ALL_SPACES) {
        const GizmoMode current{.operation = GizmoOperation::Scale, .space = start};
        const GizmoMode once = nextGizmoMode(current, GizmoModeInput{.spaceTogglePressed = true});
        CHECK(once.space != start);
        CHECK(once.operation == current.operation);  // X never touches operation

        const GizmoMode twice = nextGizmoMode(once, GizmoModeInput{.spaceTogglePressed = true});
        CHECK(twice.space == start);
        CHECK(twice.operation == current.operation);
    }
}

TEST_CASE("gizmo: effectiveSpace forces Local for Scale (G3)") {
    for (const GizmoSpace requested : ALL_SPACES) {
        CHECK(effectiveSpace(GizmoOperation::Scale, requested) == GizmoSpace::Local);
        CHECK(effectiveSpace(GizmoOperation::Translate, requested) == requested);
        CHECK(effectiveSpace(GizmoOperation::Rotate, requested) == requested);
    }
}

TEST_CASE("gizmo: gizmoSnapStep (G4)") {
    for (const GizmoOperation op : ALL_OPERATIONS) {
        CHECK_FALSE(gizmoSnapStep(op, /*snapHeld=*/false).has_value());
    }

    const auto translate = gizmoSnapStep(GizmoOperation::Translate, /*snapHeld=*/true);
    REQUIRE(translate.has_value());
    CHECK(translate->x == GIZMO_SNAP_TRANSLATE);
    CHECK(translate->y == GIZMO_SNAP_TRANSLATE);
    CHECK(translate->z == GIZMO_SNAP_TRANSLATE);
    CHECK(translate->x > 0.0F);  // relationship only -- retuning the constant reddens nothing here

    const auto rotate = gizmoSnapStep(GizmoOperation::Rotate, /*snapHeld=*/true);
    REQUIRE(rotate.has_value());
    CHECK(rotate->x == GIZMO_SNAP_ROTATE_DEGREES);
    CHECK(rotate->y == GIZMO_SNAP_ROTATE_DEGREES);
    CHECK(rotate->z == GIZMO_SNAP_ROTATE_DEGREES);
    CHECK(rotate->x > 0.0F);
    CHECK(rotate->x < 90.0F);  // it is DEGREES, not radians

    const auto scale = gizmoSnapStep(GizmoOperation::Scale, /*snapHeld=*/true);
    REQUIRE(scale.has_value());
    CHECK(scale->x == GIZMO_SNAP_SCALE);
    CHECK(scale->y == GIZMO_SNAP_SCALE);
    CHECK(scale->z == GIZMO_SNAP_SCALE);
    CHECK(scale->x > 0.0F);
    CHECK(scale->x < 1.0F);
}

TEST_CASE("gizmo: gizmoModelMatrix (G5)") {
    SUBCASE("root with a known TRS") {
        World w;
        const Entity e = w.create();
        const Transform t{.position = Vec3{1.0F, 2.0F, 3.0F},
                          .rotation = fromAxisAngle(Vec3::unitY(), radians(30.0F)),
                          .scale = Vec3{2.0F, 1.0F, 0.5F}};
        REQUIRE(w.add<Transform>(e, t) != nullptr);
        const std::optional<Mat4> model = gizmoModelMatrix(w, e);
        REQUIRE(model.has_value());
        const Mat4 expected = engine::compose(Trs{.translation = t.position, .rotation = t.rotation, .scale = t.scale});
        CHECK(approxEquals(*model, expected));
    }

    SUBCASE("child") {
        World w;
        const Entity parent = w.create();
        const Entity child = w.create();
        REQUIRE(w.add<Transform>(parent, Transform{.position = Vec3{5.0F, 0.0F, 0.0F}}) != nullptr);
        REQUIRE(w.add<Transform>(child, Transform{.position = Vec3{1.0F, 0.0F, 0.0F}}) != nullptr);
        REQUIRE(w.setParent(child, parent));
        const std::optional<Mat4> model = gizmoModelMatrix(w, child);
        REQUIRE(model.has_value());
        const Mat4 expected = engine::worldMatrix(w, parent) * localMatrix(*w.get<Transform>(child));
        CHECK(approxEquals(*model, expected));
    }

    SUBCASE("Entity{} is nullopt") {
        const World w;
        CHECK_FALSE(gizmoModelMatrix(w, Entity{}).has_value());
    }

    SUBCASE("a destroyed entity is nullopt") {
        World w;
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e) != nullptr);
        REQUIRE(w.destroy(e));
        CHECK_FALSE(gizmoModelMatrix(w, e).has_value());
    }

    SUBCASE("an entity without a Transform is nullopt") {
        World w;
        const Entity e = w.create();
        CHECK_FALSE(gizmoModelMatrix(w, e).has_value());
    }

    SUBCASE("a moved-from World is nullopt") {
        std::optional<World> source;
        source.emplace();
        const Entity e = source->create();
        REQUIRE(source->add<Transform>(e) != nullptr);
        const World movedTo(std::move(*source));
        CHECK_FALSE(gizmoModelMatrix(*source, e).has_value());
        (void)movedTo;
    }
}

TEST_CASE("gizmo: gizmoParentMatrix (G6)") {
    SUBCASE("root is EXACTLY identity") {
        World w;
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e) != nullptr);
        CHECK(gizmoParentMatrix(w, e) == Mat4::identity());
    }

    SUBCASE("child is the parent's world matrix") {
        World w;
        const Entity parent = w.create();
        const Entity child = w.create();
        REQUIRE(w.add<Transform>(parent, Transform{.position = Vec3{3.0F, 4.0F, 0.0F}}) != nullptr);
        REQUIRE(w.add<Transform>(child) != nullptr);
        REQUIRE(w.setParent(child, parent));
        CHECK(approxEquals(gizmoParentMatrix(w, child), engine::worldMatrix(w, parent)));
    }

    SUBCASE("grandchild is the full ancestor product") {
        World w;
        const Entity grandparent = w.create();
        const Entity parent = w.create();
        const Entity child = w.create();
        REQUIRE(w.add<Transform>(grandparent, Transform{.position = Vec3{1.0F, 0.0F, 0.0F}}) != nullptr);
        REQUIRE(w.add<Transform>(parent, Transform{.position = Vec3{0.0F, 1.0F, 0.0F}}) != nullptr);
        REQUIRE(w.add<Transform>(child) != nullptr);
        REQUIRE(w.setParent(parent, grandparent));
        REQUIRE(w.setParent(child, parent));
        CHECK(approxEquals(gizmoParentMatrix(w, child), engine::worldMatrix(w, parent)));
    }

    SUBCASE("a destroyed parent yields identity") {
        World w;
        const Entity parent = w.create();
        const Entity child = w.create();
        REQUIRE(w.add<Transform>(parent, Transform{.position = Vec3{9.0F, 9.0F, 9.0F}}) != nullptr);
        REQUIRE(w.add<Transform>(child) != nullptr);
        REQUIRE(w.setParent(child, parent));
        REQUIRE(w.destroy(parent));
        CHECK(gizmoParentMatrix(w, child) == Mat4::identity());
    }
}

TEST_CASE("gizmo: channel isolation (G7)") {
    // For each operation, a newWorld differing from `before` in ALL THREE channels -- only the
    // operation's own field may change. The other two compare with EXACT == (D5/AC-2).
    const Transform before{.position = Vec3{1.0F, 2.0F, 3.0F},
                           .rotation = fromAxisAngle(Vec3::unitX(), radians(10.0F)),
                           .scale = Vec3{1.0F, 1.0F, 1.0F}};
    const Mat4 parentWorld = Mat4::identity();
    const Trs differentTrs{.translation = Vec3{9.0F, 8.0F, 7.0F},
                           .rotation = fromAxisAngle(Vec3::unitY(), radians(80.0F)),
                           .scale = Vec3{3.0F, 4.0F, 5.0F}};
    const Mat4 newWorld = engine::compose(differentTrs);

    {
        const GizmoWrite write = gizmoWriteFromWorld(parentWorld, newWorld, before, GizmoOperation::Translate);
        REQUIRE(write.status == GizmoWriteStatus::Applied);
        CHECK(approxEquals(write.transform.position, differentTrs.translation));
        CHECK(write.transform.rotation == before.rotation);
        CHECK(write.transform.scale == before.scale);
    }
    {
        const GizmoWrite write = gizmoWriteFromWorld(parentWorld, newWorld, before, GizmoOperation::Rotate);
        REQUIRE(write.status == GizmoWriteStatus::Applied);
        CHECK(write.transform.position == before.position);
        CHECK(write.transform.scale == before.scale);
    }
    {
        const GizmoWrite write = gizmoWriteFromWorld(parentWorld, newWorld, before, GizmoOperation::Scale);
        REQUIRE(write.status == GizmoWriteStatus::Applied);
        CHECK(write.transform.position == before.position);
        CHECK(write.transform.rotation == before.rotation);
        CHECK(approxEquals(write.transform.scale, differentTrs.scale));
    }
}

TEST_CASE("gizmo: identity round-trip is NoChange (G8)") {
    // `before` is deliberately the IDENTITY Transform, not an arbitrary one: engine::decompose's
    // trace-based rotation extraction and its sqrt-based scale extraction are NOT guaranteed to
    // round-trip a non-trivial compose() BIT-EXACT (measured: a 45-degree rotation composed then
    // decomposed differs from the original quaternion by an ULP or two), and GizmoWriteStatus's
    // Applied/NoChange split is an EXACT compare by design (D6). The identity case round-trips
    // exactly (every column length is precisely 1.0, every off-diagonal term precisely 0) and is
    // sufficient to prove AC-11's property: grab-and-release with no movement writes nothing.
    const Transform before{};
    const Mat4 parentWorld = Mat4::identity();
    const Mat4 newWorld =
        engine::compose(Trs{.translation = before.position, .rotation = before.rotation, .scale = before.scale});
    for (const GizmoOperation op : ALL_OPERATIONS) {
        const GizmoWrite write = gizmoWriteFromWorld(parentWorld, newWorld, before, op);
        CHECK(write.status == GizmoWriteStatus::NoChange);
        CHECK(write.transform == Transform{});  // left default, never partially filled
    }
}

TEST_CASE("gizmo: translate under a rotated parent (G9/AC-9)") {
    const Mat4 parentWorld = engine::compose(Trs{
        .translation = Vec3::zero(), .rotation = fromAxisAngle(Vec3::unitY(), radians(90.0F)), .scale = Vec3::one()});
    const Transform before{};                   // identity local -- child sits exactly at the parent's world pose
    const Mat4 childWorldBefore = parentWorld;  // parentWorld * localMatrix(identity) == parentWorld
    const Vec3 worldDelta{1.0F, 0.0F, 0.0F};    // the mouse moved the child +1 on WORLD X
    const Mat4 newWorld = translation(worldDelta) * childWorldBefore;

    const GizmoWrite write = gizmoWriteFromWorld(parentWorld, newWorld, before, GizmoOperation::Translate);
    REQUIRE(write.status == GizmoWriteStatus::Applied);

    // The stored local position is the correct PARENT-RELATIVE value: the new world position
    // (childWorldBefore's position + worldDelta) pulled back through the parent's inverse.
    const Vec3 newWorldPosition =
        Vec3{childWorldBefore.columns[3].x, childWorldBefore.columns[3].y, childWorldBefore.columns[3].z} + worldDelta;
    const Vec3 expectedLocalPos = transformPoint(inverse(parentWorld), newWorldPosition);
    CHECK(approxEquals(write.transform.position, expectedLocalPos, TIGHT_EPS));

    const Mat4 reconstructed = parentWorld * localMatrix(write.transform);
    CHECK(approxEquals(reconstructed, newWorld, TIGHT_EPS));
}

TEST_CASE("gizmo: rotate under a uniformly scaled parent (G10)") {
    const Mat4 parentWorld = scaling(Vec3{2.0F, 2.0F, 2.0F});
    const Transform before{
        .position = Vec3{1.0F, 0.0F, 0.0F}, .rotation = Quat::identity(), .scale = Vec3{3.0F, 3.0F, 3.0F}};
    const Quat extraRotation = fromAxisAngle(Vec3::unitZ(), radians(30.0F));
    // Position and scale scale with the parent's uniform 2x; the rotation is the new one being
    // dragged in. This is exactly what parentWorld * localMatrix(result) must reproduce.
    const Mat4 newWorld = engine::compose(
        Trs{.translation = Vec3{2.0F, 0.0F, 0.0F}, .rotation = extraRotation, .scale = Vec3{6.0F, 6.0F, 6.0F}});

    const GizmoWrite write = gizmoWriteFromWorld(parentWorld, newWorld, before, GizmoOperation::Rotate);
    REQUIRE(write.status == GizmoWriteStatus::Applied);
    CHECK(write.transform.scale == before.scale);  // BYTE-IDENTICAL (D5)
    CHECK(write.transform.position == before.position);

    const Mat4 reconstructed = parentWorld * localMatrix(write.transform);
    CHECK(approxEquals(reconstructed, newWorld, TIGHT_EPS));
}

TEST_CASE("gizmo: shear under a non-uniform ancestor is NotDecomposable (G11/E7/AC-10)") {
    // Code-review finding (2026-07-29), verified at source against the pinned engine::decompose()
    // (glm_backend.cpp): decompose() itself guards ONLY column length/finiteness -- it has NO
    // orthogonality test, so it never rejects shear on its own. gizmo.cpp's OWN isSheared() guard
    // (GIZMO_ORTHOGONALITY_EPSILON = 1e-4, measured against this exact family of constructions) is
    // what makes THIS construction -- the plan's original E7 one: a world-space rotation delta
    // applied under a non-uniformly-scaled parent, genuine shear -- reach NotDecomposable. Measured
    // maxAbsCos for this construction is ~0.33, three-plus orders of magnitude over the threshold.
    const Mat4 parentWorld = scaling(Vec3{2.0F, 1.0F, 1.0F});
    const Quat childRotation = fromAxisAngle(Vec3::unitZ(), radians(45.0F));
    const Mat4 childLocal = toMat4(childRotation);
    // The world-space delta: NOT commutative with the parent's non-uniform scale, which is exactly
    // what a world-space rotate/translate handle produces and what makes the result genuine shear.
    const Mat4 worldSpaceDelta = toMat4(fromAxisAngle(Vec3::unitY(), radians(20.0F)));
    const Mat4 newWorld = worldSpaceDelta * parentWorld * childLocal;
    const Transform before{};

    const GizmoWrite write = gizmoWriteFromWorld(parentWorld, newWorld, before, GizmoOperation::Rotate);
    CHECK(write.status == GizmoWriteStatus::NotDecomposable);
    CHECK(write.transform == Transform{});  // never partially filled
}

TEST_CASE("gizmo: degenerate parent scale is NotFinite (G12/E8)") {
    // Driven DIRECTLY through gizmoWriteFromWorld so no earlier guard (gizmoModelMatrix,
    // gizmoParentMatrix) can swallow it first (E15's lesson).
    const Mat4 parentWorld = scaling(Vec3{0.0F, 1.0F, 1.0F});  // singular: inverse() blows up to inf/NaN
    const Mat4 newWorld =
        engine::compose(Trs{.translation = Vec3{1.0F, 2.0F, 3.0F}, .rotation = Quat::identity(), .scale = Vec3::one()});
    const Transform before{};

    const GizmoWrite write = gizmoWriteFromWorld(parentWorld, newWorld, before, GizmoOperation::Translate);
    CHECK(write.status == GizmoWriteStatus::NotFinite);
    CHECK(write.transform == Transform{});
}

TEST_CASE("gizmo: NotFinite from a hostile newWorld (G13)") {
    const Mat4 parentWorld = Mat4::identity();
    const Transform before{};
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    SUBCASE("infinite translation") {
        Mat4 hostile = Mat4::identity();
        hostile.columns[3] = Vec4{inf, 0.0F, 0.0F, 1.0F};
        const GizmoWrite write = gizmoWriteFromWorld(parentWorld, hostile, before, GizmoOperation::Translate);
        CHECK(write.status == GizmoWriteStatus::NotFinite);
        CHECK(write.transform == Transform{});
    }

    SUBCASE("NaN basis vector") {
        Mat4 hostile = Mat4::identity();
        hostile.columns[0] = Vec4{nan, 0.0F, 0.0F, 0.0F};
        const GizmoWrite write = gizmoWriteFromWorld(parentWorld, hostile, before, GizmoOperation::Scale);
        CHECK(write.status == GizmoWriteStatus::NotFinite);
        CHECK(write.transform == Transform{});
    }
}

TEST_CASE("gizmo: huge-but-finite scale (G14)") {
    // VERIFIED AT SOURCE (empirically, against the pinned engine::decompose()): a uniform scale
    // large enough to be finite as MATRIX ENTRIES (1e34) overflows engine::decompose()'s internal
    // length() computation (a dot-product of ~1e34*1e34 == 1e68, which overflows float) BEFORE the
    // write pipeline's own step-6 finiteness sweep ever runs -- decompose() itself returns false, so
    // the observed status is NotDecomposable, not the "reaches step 6 and only step 6" NotFinite the
    // plan predicted. Scanning the full magnitude range (1e10 .. 1e37) found NO finite input for
    // which decompose() SUCCEEDS yet produces a non-finite Trs component -- the transition at
    // sqrt(FLT_MAX) (~1.84e19) goes directly from a fully-finite success to an outright decompose()
    // rejection, with no intermediate "succeeds but non-finite" band. Recorded honestly per the
    // plan's own instruction ("if it does not discriminate, say so rather than forcing it") --
    // this is NOT proof step 6 is dead code (a pathological ROTATION-only input was not exhaustively
    // searched), only that THIS construction does not reach it. The step-6 check remains defence in
    // depth, uncovered by this case, exactly like A4's stale-latch clear.
    const Mat4 parentWorld = Mat4::identity();
    const Transform before{};
    const float huge = 1.0e34F;
    const Mat4 newWorld = engine::compose(
        Trs{.translation = Vec3{huge, -huge, huge}, .rotation = Quat::identity(), .scale = Vec3{huge, huge, huge}});

    bool allFiniteInput = true;
    for (const Vec4& c : newWorld.columns) {
        if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(c.w)) {
            allFiniteInput = false;
        }
    }
    REQUIRE(allFiniteInput);  // the input matrix genuinely IS finite going in

    const GizmoWrite write = gizmoWriteFromWorld(parentWorld, newWorld, before, GizmoOperation::Scale);
    // The MEASURED status (see the comment above), not the plan's predicted one.
    CHECK(write.status == GizmoWriteStatus::NotDecomposable);
    CHECK(write.transform == Transform{});
}

TEST_CASE("gizmo: gizmoOriginBehindCamera (G15)") {
    const Mat4 view = engine::lookAt(Vec3{0.0F, 0.0F, 5.0F}, Vec3::zero(), Vec3::unitY());
    const Mat4 proj = engine::perspective(radians(60.0F), 16.0F / 9.0F, 0.1F, 100.0F);
    const Mat4 viewProj = proj * view;
    const Vec2 viewportSize{800.0F, 600.0F};

    SUBCASE("in front of the eye is false") {
        const Mat4 model = translation(Vec3::zero());
        CHECK_FALSE(gizmoOriginBehindCamera(viewProj, model, viewportSize));
    }

    SUBCASE("positive control: a DIFFERENT origin, also in front, is also false") {
        // Guards against a case that would pass vacuously regardless of the model's origin.
        const Mat4 model = translation(Vec3{1.0F, 1.0F, 0.0F});
        CHECK_FALSE(gizmoOriginBehindCamera(viewProj, model, viewportSize));
    }

    SUBCASE("exactly at the eye is true") {
        const Mat4 model = translation(Vec3{0.0F, 0.0F, 5.0F});
        CHECK(gizmoOriginBehindCamera(viewProj, model, viewportSize));
    }

    SUBCASE("behind the eye is true") {
        const Mat4 model = translation(Vec3{0.0F, 0.0F, 10.0F});
        CHECK(gizmoOriginBehindCamera(viewProj, model, viewportSize));
    }

    SUBCASE("a non-finite model fails closed (true)") {
        Mat4 model = Mat4::identity();
        model.columns[3] = Vec4{std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 1.0F};
        CHECK(gizmoOriginBehindCamera(viewProj, model, viewportSize));
    }
}

TEST_CASE("gizmo: gizmoDragEdge transitions (G16)") {
    CHECK(gizmoDragEdge(/*wasUsing=*/false, /*isUsing=*/false) == GizmoDragEdge::None);
    CHECK(gizmoDragEdge(/*wasUsing=*/false, /*isUsing=*/true) == GizmoDragEdge::Begin);
    CHECK(gizmoDragEdge(/*wasUsing=*/true, /*isUsing=*/true) == GizmoDragEdge::Continue);
    CHECK(gizmoDragEdge(/*wasUsing=*/true, /*isUsing=*/false) == GizmoDragEdge::End);
}

TEST_CASE("gizmo: enum totality (G17)") {
    // A switch over each enum with NO default: -- adding an enumerator without a matching case here
    // is caught by bugprone-switch-missing-default-case (CI's Linux clang-tidy lane,
    // --warnings-as-errors='*'), the DOCK_SLOT_COUNT static_assert discipline in its switch form.
    // Each branch writes a DISTINCT slot (bugprone-branch-clone rejects identical branches).
    std::array<bool, 3> operationSeen{};
    for (const GizmoOperation op : ALL_OPERATIONS) {
        switch (op) {
            case GizmoOperation::Translate:
                operationSeen[0] = true;
                break;
            case GizmoOperation::Rotate:
                operationSeen[1] = true;
                break;
            case GizmoOperation::Scale:
                operationSeen[2] = true;
                break;
        }
    }
    CHECK(operationSeen[0]);
    CHECK(operationSeen[1]);
    CHECK(operationSeen[2]);

    std::array<bool, 2> spaceSeen{};
    for (const GizmoSpace space : ALL_SPACES) {
        switch (space) {
            case GizmoSpace::Local:
                spaceSeen[0] = true;
                break;
            case GizmoSpace::World:
                spaceSeen[1] = true;
                break;
        }
    }
    CHECK(spaceSeen[0]);
    CHECK(spaceSeen[1]);

    std::array<bool, 4> edgeSeen{};
    for (const GizmoDragEdge edge :
         {GizmoDragEdge::None, GizmoDragEdge::Begin, GizmoDragEdge::Continue, GizmoDragEdge::End}) {
        switch (edge) {
            case GizmoDragEdge::None:
                edgeSeen[0] = true;
                break;
            case GizmoDragEdge::Begin:
                edgeSeen[1] = true;
                break;
            case GizmoDragEdge::Continue:
                edgeSeen[2] = true;
                break;
            case GizmoDragEdge::End:
                edgeSeen[3] = true;
                break;
        }
    }
    CHECK(edgeSeen[0]);
    CHECK(edgeSeen[1]);
    CHECK(edgeSeen[2]);
    CHECK(edgeSeen[3]);
}
