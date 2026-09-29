#pragma once
// Aero Engine -- the Create menu's DATA (task E.5.2). PUBLIC and PURE: no ImGui, no entt, no <filesystem>,
// no logging, no World. Which kinds exist, how they are labelled and grouped, where each lands and what it
// is built from. The ImGui half is src-private (editor/src/create_menu_ui.hpp); the pipeline that APPLIES a
// kind is EditorApp's alone (applyCreate) -- the hosts only record one. A new entry is ONE row in
// createMenuEntries() plus ONE arm in createSeed(), and nothing else.
#include <aero/core/math.hpp>               // Vec3, radians
#include <aero/editor/entity_commands.hpp>  // EntitySeed

#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::editor {

enum class CreateKind : std::uint8_t {
    Empty = 0,
    Cube,
    Sphere,
    Plane,
    DirectionalLight,
    PointLight,
    SpotLight,
    Camera,
    Count,  // not a kind: the bound every total function here tests
};
inline constexpr std::size_t CREATE_KIND_COUNT = static_cast<std::size_t>(CreateKind::Count);

enum class CreateMenuGroup : std::uint8_t { TopLevel = 0, Object3D, Light };

struct CreateMenuEntry {
    CreateKind kind = CreateKind::Empty;
    CreateMenuGroup group = CreateMenuGroup::TopLevel;
};

// The TYPED entries in menu order, each non-Empty kind exactly once, each group's entries CONTIGUOUS (the
// drawing helper opens one submenu per run). Empty is not a row: each host spells its own item, because the
// right label differs by context ("Empty" under a menu already called Create, "Create Empty" in a context
// menu beside Create Child).
[[nodiscard]] std::span<const CreateMenuEntry> createMenuEntries() noexcept;

// STRING LITERALS -- static storage, NUL-terminated -- so ImGui takes them directly. NEVER named toString:
// doctest's DOCTEST_STRINGIFY finds an unqualified toString by ADL and fails to compile on every lane.
[[nodiscard]] const char* createKindLabel(CreateKind kind) noexcept;             // "" for Count and beyond
[[nodiscard]] const char* createMenuGroupLabel(CreateMenuGroup group) noexcept;  // "" for TopLevel

// Placement (D3). TUNING CONSTANTS, judged on the validation page; a change is a recorded amendment.
inline constexpr float CREATE_PLANE_EXTENT = 10.0F;       // a Plane is a 10 x 10 floor tile
inline constexpr float CREATE_POINT_LIGHT_HEIGHT = 2.0F;  // a metre above a resting unit Cube's top
inline constexpr float CREATE_SPOT_LIGHT_HEIGHT = 3.0F;
inline constexpr float CREATE_DIRECTIONAL_LIGHT_HEIGHT = 3.0F;       // placement only: a sun has no position
inline constexpr float CREATE_SPOT_PITCH_RADIANS = radians(-90.0F);  // -Z turned to -Y: straight down

// (pivot.x, GROUND, pivot.z), GROUND = render::DEBUG_GRID_PLANE_HEIGHT -- named once, in the .cpp, so if the
// grid's plane ever moves creations follow. A non-finite x or z becomes 0: EditorCamera::clampState
// guarantees ordering, not finiteness. noexcept and total.
[[nodiscard]] Vec3 createAnchor(Vec3 cameraPivot) noexcept;

// Everything a create of `kind` at `anchor` builds (D3's table): name, undo label ("Create " + the kind's
// label), Transform and the one component -- the component's own T{} defaults, except MeshRenderer's
// primitive, which IS the choice. PURE and TOTAL: Count, or any out-of-range value, yields Empty's seed.
[[nodiscard]] EntitySeed createSeed(CreateKind kind, Vec3 anchor);

}  // namespace engine::editor
