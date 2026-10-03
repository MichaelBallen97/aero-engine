#pragma once
// Aero Engine — the axis colour palette (task E.1.2), shared by everything that draws an axis.
//
// ONE COLOUR PER AXIS, WRITTEN TWICE. The linear Vec4 is the SOURCE OF TRUTH -- the debug batch and
// every GPU consumer take linear, because the HDR target is linear and 3.6.3's resolve encodes. The
// three sRGB bytes are the SAME colour for ImGui, which has no notion of linear at all and is what a
// person actually picks in a colour wheel. Two spellings of one colour is a drift surface, so AX1
// pushes each linear value through render::linearToSrgbEncode and asserts the byte comes back --
// a real pin, not a restatement, and possible because aero::render has been PUBLIC on
// aero_editor_core since 3.4.2.
//
// IMGUI-FREE BY RULE, like every public editor header (editor_camera.hpp's rule). It exposes bytes;
// the one call site that needs an ImU32 builds it there with IM_COL32.
//
// X = red, Y = green, Z = blue: Unity, Unreal, Blender, Godot and ImGuizmo all agree, and a viewport
// that disagreed with the gizmo drawn on top of it would be worse than no colour at all.
//
// THREE CONSUMERS, and only the first exists today:
//   * E.1.2 -- the grid's two axis lines (this task).
//   * E.3.1 -- the Inspector's Vec3/Quat rows. AXIS_Y exists for it: this task draws NO Y axis
//              (a vertical line through the origin describes nothing), but a Vec3 row needs three.
//   * E.1.5 -- ImGuizmo's style config.
// Folded into `EDITOR_THEME` (editor_theme.hpp) at E.6.1; these names are aliases, so every consumer
// and AX1 are unchanged.

#include <aero/core/math.hpp>
#include <aero/editor/editor_theme.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::editor {

// LINEAR. The nine components were computed from the nine bytes below with the sRGB EOTF and each
// was confirmed to round-trip back to its own byte -- four decimal places is enough for that and no
// more precision is implied. AX1 is what decides whether a fifth is ever needed, not this comment.
inline constexpr Vec4 AXIS_X_LINEAR = EDITOR_THEME.axis.xLinear;  // sRGB 226, 65, 73
inline constexpr Vec4 AXIS_Y_LINEAR = EDITOR_THEME.axis.yLinear;  // sRGB 125, 199, 61
inline constexpr Vec4 AXIS_Z_LINEAR = EDITOR_THEME.axis.zLinear;  // sRGB  56, 133, 226

// sRGB BYTES, in r, g, b order. What ImGui consumes.
inline constexpr std::array<std::uint8_t, 3> AXIS_X_SRGB = EDITOR_THEME.axis.xSrgb;
inline constexpr std::array<std::uint8_t, 3> AXIS_Y_SRGB = EDITOR_THEME.axis.ySrgb;
inline constexpr std::array<std::uint8_t, 3> AXIS_Z_SRGB = EDITOR_THEME.axis.zSrgb;

// The palette's KEY, added at task E.1.3 because the view-axis gizmo needs to say "this ball's
// colour" without a switch of its own. E.1.5 (ImGuizmo's style config) and E.3.1 (the Inspector's
// Vec3/Quat rows) are the other two consumers named above and inherit it.
enum class Axis : std::uint8_t { X = 0, Y, Z };
inline constexpr std::size_t AXIS_COUNT = 3;

// TOTAL: no `default:`, so a fourth enumerator is a -Wswitch error rather than a silent fallthrough,
// and an out-of-range cast returns X's bytes rather than reading past an array (culling.cpp's
// Frustum::plane precedent). constexpr, like everything else in this header.
//
// THERE IS DELIBERATELY NO LINEAR ACCESSOR. Its only consumers are viewport_panel.cpp's
// viewportGridStyle(), which names AXIS_X_LINEAR / AXIS_Z_LINEAR directly and belongs to E.1.2 --
// so an accessor here would have no caller, which is the abstraction this project refuses. Add it
// with its first caller, not before.
[[nodiscard]] constexpr std::array<std::uint8_t, 3> axisColorSrgbBytes(Axis axis) noexcept {
    switch (axis) {
        case Axis::X:
            return AXIS_X_SRGB;
        case Axis::Y:
            return AXIS_Y_SRGB;
        case Axis::Z:
            return AXIS_Z_SRGB;
    }
    return AXIS_X_SRGB;
}

}  // namespace engine::editor
