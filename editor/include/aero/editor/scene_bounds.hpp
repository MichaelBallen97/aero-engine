#pragma once
// Aero Engine — engine::editor::Aabb + the world-bounds walk (task 2.3.1). Feeds `EditorCamera::
// focusOn` (F to frame the selection or the scene) and is the shape 2.3.2's ray-vs-AABB picking will
// reuse (D9) -- do NOT write a second bounds walk there.
//
// PUBLIC, ImGui-free, entt-free AND render-free BY RULE, held by FILE PLACEMENT like every header
// under editor/include (2.1.3 D9) -- see editor_camera.hpp's header comment for the full reasoning
// (R12: no guard can enforce this against aero_editor_shell_test's link line).
//
// Why the EDITOR and not engine/core/math (D9, settled): the only knowledge that makes
// LOCAL_MESH_HALF_EXTENT correct -- that every built-in primitive fits [-0.5, 0.5]^3 -- is a
// render-CATALOG fact (engine/render/src/primitives.cpp), not a math fact, and it expires the moment
// a later task imports a real mesh. Promoting Aabb happens when a real *engine* consumer appears.

#include <aero/core/math.hpp>
#include <aero/scene/entity.hpp>

#include <span>

namespace engine {
class World;  // forward-declared: the three free functions below take it by reference, and
              // scene_bounds.cpp includes <aero/scene/world.hpp> itself -- the panel_context.hpp /
              // tree_walk.hpp precedent.
}  // namespace engine

namespace engine::editor {

// The local box EVERY built-in primitive fits inside: the cube's faces sit at face.normal * 0.5
// (engine/render/src/primitives.cpp), the sphere's RADIUS is 0.5 (matching the cube's [-0.5, 0.5]
// scale), and the plane is a unit quad in XZ.
//
// TEMPORARY BY CONSTRUCTION -- this is the ONE number in task 2.3.1 that is knowingly wrong the
// moment a later task imports a real mesh with real bounds. The AssetDatabase should then publish
// per-mesh local bounds and entityBounds() should read them.
inline constexpr float LOCAL_MESH_HALF_EXTENT = 0.5F;

// An axis-aligned world-space box. A POINT box (min == max) is VALID -- an entity with no
// MeshRenderer contributes exactly that.
struct Aabb {
    Vec3 min{};
    Vec3 max{};

    // The INVERTED sentinel: min = +inf, max = -inf, so valid() is false and the first expand()
    // lands both corners on the point.
    [[nodiscard]] static Aabb empty() noexcept;

    // Every component FINITE and min <= max on all three axes. The finiteness half is not
    // decoration: without it a box with an infinite corner would pass, center() would be NaN
    // (inf - inf), and focusOn would write NaN into the pose (AC-18/E11).
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] Vec3 center() const noexcept;   // (min + max) * 0.5 -- caller checks valid() first
    [[nodiscard]] Vec3 size() const noexcept;     // max - min
    [[nodiscard]] float radius() const noexcept;  // half the diagonal; exactly 0 for a point box

    void expand(Vec3 point) noexcept;
    void expand(const Aabb& other) noexcept;  // NO-OP when `other` is invalid
};

// `entity`'s own contribution plus, when includeDescendants, its whole subtree (each descendant
// through its OWN world matrix, so parent scale/rotation compose correctly).
//   * an entity WITH a MeshRenderer contributes the 8 transformed corners of the local box;
//   * an entity WITHOUT one contributes the single POINT at its world translation;
//   * a dead or null handle contributes NOTHING.
// worldMatrix's silent identity for a never-transformed entity is inherited DELIBERATELY: such an
// entity contributes a point at the world origin. The subtree walk goes through editor::walkForest --
// an EXPLICIT stack, never recursion (misc-no-recursion is --warnings-as-errors on the Linux lane).
[[nodiscard]] Aabb entityBounds(const World& world, Entity entity, bool includeDescendants);

// The union over `entities`, each WITH its descendants. Dead and null handles are skipped and do
// NOT drag the box to the origin. This does NOT call Selection::prune -- mutating the selection
// during a draw walk is forbidden (.claude/rules/editor.md), and pruning is 2.2.1's job.
[[nodiscard]] Aabb selectionBounds(const World& world, std::span<const Entity> entities);

// Every entity holding a MeshRenderer, each through its own world matrix (so subtrees are covered
// without a second traversal). Returns Aabb::empty() for an empty World.
//
// SILENT BY CONSTRUCTION: this walk calls World::each<T> NOWHERE, and that -- not the registered()
// test below -- is what makes it silent. World::each<T> over a type that is not registered in that
// World logs one AERO_LOG_ERROR PER CALL, which would spray straight into the Console panel 2.2.5
// shipped; has<T>/get<T> are completely silent for the same case. This function uses only eachEntity
// + has/get, all of which are CONST -- which is also why this signature can take a const World& at
// all, where each<Ts...>() could not.
//
// The registered() test in the .cpp is an O(1) FAST PATH, honestly documented as nothing more. It can
// NEVER be false on a live World: World's constructor registers every built-in component type
// unconditionally, and no public or internal API unregisters a type. The one state in which it fires
// is a MOVED-FROM World, and even there it suppresses no log, because beginQuery already returns
// silently on impl == nullptr before its own ERROR. See scene_bounds.cpp for the full citation trail.
[[nodiscard]] Aabb sceneBounds(const World& world);

}  // namespace engine::editor
