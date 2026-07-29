// tests/editor/gizmo_test.cpp — task 2.3.3: the pure gizmo model's tier-0 battery. Ninth TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0/ungated: must pass identically with
// AERO_REQUIRE_GPU unset and set.
//
// Step 2 lands cases G1-G4, G16, G17 -- the tool-state model (nextGizmoMode, effectiveSpace,
// gizmoSnapStep, gizmoDragEdge) and enum totality. Step 3 appends G5-G15 -- the geometry and the
// single-channel write pipeline.
#include <aero/editor/gizmo.hpp>

#include <doctest/doctest.h>

#include <array>

using engine::editor::effectiveSpace;
using engine::editor::GIZMO_SNAP_ROTATE_DEGREES;
using engine::editor::GIZMO_SNAP_SCALE;
using engine::editor::GIZMO_SNAP_TRANSLATE;
using engine::editor::GizmoDragEdge;
using engine::editor::gizmoDragEdge;
using engine::editor::GizmoMode;
using engine::editor::GizmoModeInput;
using engine::editor::GizmoOperation;
using engine::editor::gizmoSnapStep;
using engine::editor::GizmoSpace;
using engine::editor::nextGizmoMode;

namespace {
constexpr std::array<GizmoOperation, 3> ALL_OPERATIONS = {GizmoOperation::Translate, GizmoOperation::Rotate,
                                                          GizmoOperation::Scale};
constexpr std::array<GizmoSpace, 2> ALL_SPACES = {GizmoSpace::Local, GizmoSpace::World};
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
