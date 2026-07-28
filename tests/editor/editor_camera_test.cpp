// tests/editor/editor_camera_test.cpp — task 2.3.1: the editor camera's tier-0 battery. Fifth TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0/ungated: must pass identically with
// AERO_REQUIRE_GPU unset and set.
//
// This step (2.3.1 step 2) lands only section 6.1's case 8, the gesture matrix (AC-12/AC-13). The
// EditorCamera model's own cases (1-7, 9-12) arrive in step 4.
#include <aero/editor/editor_camera.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

using engine::editor::CameraButton;
using engine::editor::CameraGesture;
using engine::editor::CameraGestureInput;
using engine::editor::CameraGestureState;
using engine::editor::nextGesture;

namespace {

struct GestureRow {
    std::string name;
    CameraGestureState current;
    CameraGestureInput input;
    CameraGestureState expected;
};

}  // namespace

TEST_CASE("editor camera: enum layout (performance-enum-size pins)") {
    static_assert(sizeof(CameraGesture) == 1);
    static_assert(sizeof(CameraButton) == 1);
    static_assert(std::is_same_v<std::underlying_type_t<CameraGesture>, std::uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<CameraButton>, std::uint8_t>);
}

TEST_CASE("editor camera: nextGesture matches D5's three rules exactly (AC-12, AC-13)") {
    const CameraGestureState none{};
    const CameraGestureState orbitLeft{.gesture = CameraGesture::Orbit, .button = CameraButton::Left};

    const std::vector<GestureRow> rows = {
        // --- START, hovered + fresh press: one row per rule-3 clause + its near-misses ---
        // `...Down` is set alongside `...Pressed` in every row below, matching real ImGui semantics
        // (a `IsMouseClicked` edge this frame always coincides with `IsMouseDown` being true THIS
        // frame too) -- this is what lets S8 (dropping the fresh-press requirement, `...Down` instead
        // of `...Pressed`) actually discriminate every START row, not just the two-button E5 row.
        {"Alt+LMB -> Orbit/Left",
         none,
         {.hovered = true, .leftDown = true, .leftPressed = true, .alt = true},
         {.gesture = CameraGesture::Orbit, .button = CameraButton::Left}},
        {"Alt+Cmd+LMB -> Pan/Left (beats Orbit)",
         none,
         {.hovered = true, .leftDown = true, .leftPressed = true, .alt = true, .ctrlOrCmd = true},
         {.gesture = CameraGesture::Pan, .button = CameraButton::Left}},
        {"MMB -> Pan/Middle",
         none,
         {.hovered = true, .middleDown = true, .middlePressed = true},
         {.gesture = CameraGesture::Pan, .button = CameraButton::Middle}},
        {"RMB -> Fly/Right",
         none,
         {.hovered = true, .rightDown = true, .rightPressed = true},
         {.gesture = CameraGesture::Fly, .button = CameraButton::Right}},
        {"Alt+RMB -> Dolly/Right (beats Fly)",
         none,
         {.hovered = true, .rightDown = true, .rightPressed = true, .alt = true},
         {.gesture = CameraGesture::Dolly, .button = CameraButton::Right}},

        // --- AC-13: plain LMB starts NOTHING, at any hover state ---
        {"plain LMB, hovered, no Alt -> None", none, {.hovered = true, .leftDown = true, .leftPressed = true}, none},
        {"plain LMB + ctrlOrCmd alone, hovered -> None",
         none,
         {.hovered = true, .leftDown = true, .leftPressed = true, .ctrlOrCmd = true},
         none},

        // --- not hovered + fresh press -> None, for every button/modifier combination ---
        {"not hovered, Alt+LMB -> None",
         none,
         {.hovered = false, .leftDown = true, .leftPressed = true, .alt = true},
         none},
        {"not hovered, RMB -> None", none, {.hovered = false, .rightDown = true, .rightPressed = true}, none},
        {"not hovered, MMB -> None", none, {.hovered = false, .middleDown = true, .middlePressed = true}, none},

        // --- CONTINUE ---
        {"continuing orbit, not hovered -> unchanged (E3)",
         orbitLeft,
         {.hovered = false, .leftDown = true, .alt = true},
         orbitLeft},
        {"continuing orbit, Alt released -> unchanged (E6, D5 rule 1)",
         orbitLeft,
         {.hovered = true, .leftDown = true, .alt = false},
         orbitLeft},

        // --- END ---
        {"orbit ends: button released, hovered -> None", orbitLeft, {.hovered = true, .leftDown = false}, none},
        {"orbit ends: button released, not hovered -> None", orbitLeft, {.hovered = false, .leftDown = false}, none},

        // --- E5: two buttons at once ---
        {"orbiting + RMB fresh press -> still Orbit/Left (rule 1 wins)",
         orbitLeft,
         {.hovered = true, .leftDown = true, .rightPressed = true, .alt = true},
         orbitLeft},
        {"LMB released, RMB held (no fresh press) -> None",
         orbitLeft,
         {.hovered = true, .leftDown = false, .rightDown = true},
         none},
    };

    for (const GestureRow& row : rows) {
        CAPTURE(row.name);
        const CameraGestureState actual = nextGesture(row.current, row.input);
        CHECK(actual.gesture == row.expected.gesture);
        CHECK(actual.button == row.expected.button);
    }
}

TEST_CASE("editor camera: nextGesture's Alt-after-press row (S8's discriminator)") {
    // AC-13, explicit: a plain LMB press must NEVER retroactively become an orbit, even if Alt is
    // pressed on a LATER frame while the button is still held -- rule 3 needs a FRESH press.
    const CameraGestureInput pressFrame{.hovered = true, .leftDown = true, .leftPressed = true};
    const CameraGestureState afterPress = nextGesture(CameraGestureState{}, pressFrame);
    CHECK(afterPress.gesture == CameraGesture::None);

    const CameraGestureInput heldWithAltNow{.hovered = true, .leftDown = true, .leftPressed = false, .alt = true};
    const CameraGestureState afterAltAppears = nextGesture(afterPress, heldWithAltNow);
    CHECK(afterAltAppears.gesture == CameraGesture::None);
    CHECK(afterAltAppears.button == CameraButton::None);
}

TEST_CASE("editor camera: nextGesture is idempotent for a continuing gesture") {
    const CameraGestureState orbitLeft{.gesture = CameraGesture::Orbit, .button = CameraButton::Left};
    const CameraGestureInput held{.hovered = true, .leftDown = true, .alt = true};
    const CameraGestureState once = nextGesture(orbitLeft, held);
    const CameraGestureState twice = nextGesture(once, held);
    CHECK(once.gesture == twice.gesture);
    CHECK(once.button == twice.button);
}
