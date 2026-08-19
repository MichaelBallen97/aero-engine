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
// primitiveLocalBounds correct -- what shape each built-in primitive actually is -- is a render-CATALOG
// fact (engine/render/src/primitives.cpp), not a math fact. Promoting Aabb happens when a real *engine*
// consumer appears.

#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>
#include <aero/scene/entity.hpp>
#include <aero/scene/mesh_renderer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace engine {
class World;  // forward-declared: the three free functions below take it by reference, and
              // scene_bounds.cpp includes <aero/scene/world.hpp> itself -- the panel_context.hpp /
              // tree_walk.hpp precedent.
}  // namespace engine

namespace engine::editor {

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

// ---- task 3.1.5: local bounds, primitive and referenced -----------------------------------------

// The exact local box of each built-in primitive, from the render CATALOG (engine/render/src/
// primitives.cpp): the cube's faces sit at +/-0.5, the sphere's RADIUS is 0.5 (so the same box), and
// the PLANE IS A UNIT QUAD IN XZ -- {-0.5, 0, -0.5}..{0.5, 0, 0.5}, ZERO THICKNESS IN Y. That last one
// retires task 2.3.2's knowingly-fat plane pick box, which its own note said would expire "in the same
// change" as this one. Out-of-range clamps to the CUBE box, exactly as clampPrimitive clamps to Cube.
[[nodiscard]] Aabb primitiveLocalBounds(std::uint32_t primitive) noexcept;

// Corner i of `box`: bit 0 selects X, bit 1 Y, bit 2 Z -- min when 0, max when 1. BOX_EDGES
// (selection_overlay.hpp) is DERIVED from exactly this assignment, so the bounds walk and the highlight
// agree BY CONSTRUCTION rather than by two matching comments. An index past 7 reads the low three bits
// and is therefore total; engine code only ever iterates 0..7.
[[nodiscard]] Vec3 aabbCorner(const Aabb& box, std::size_t i) noexcept;

// The key a resolved mesh reference is looked up by: the asset GUID plus WHICH mesh of it -- the same
// pair MeshRenderer::mesh/meshIndex carries and CookedSubmesh::sourceMeshIndex records.
struct MeshBoundsKey {
    Guid mesh;
    std::uint32_t meshIndex = 0;
    [[nodiscard]] constexpr bool operator==(const MeshBoundsKey&) const noexcept = default;
    [[nodiscard]] constexpr bool operator<(const MeshBoundsKey& o) const noexcept {
        return mesh == o.mesh ? meshIndex < o.meshIndex : mesh < o.mesh;  // Guid::operator< first
    }
};

// The per-mesh local boxes the editor publishes each service pass, borrowed by picking, framing and the
// overlay -- all three, or none. SORTED VECTOR keyed by MeshBoundsKey::operator<, never a hash
// container (the 3.1.2 R9 / MSVC C2607 rule); all entries of one guid are CONTIGUOUS under that order,
// which is what makes removeMesh a single erase range.
class MeshBoundsLookup {
public:
    void set(MeshBoundsKey key, const Aabb& box);  // replaces an existing entry wholesale
    void removeMesh(Guid mesh) noexcept;           // every meshIndex of one guid, in one erase range
    void clear() noexcept;
    // A POINTER, not an optional<Aabb>: find runs once per entity per click on the picking path, and a
    // borrowed box avoids a copy there. nullptr for a nil guid and for an absent key.
    [[nodiscard]] const Aabb* find(MeshBoundsKey key) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::vector<std::pair<MeshBoundsKey, Aabb>> entries;  // SORTED by key; std::lower_bound
};

// The ONE place a MeshRenderer becomes a local box, so the pick box, the frame box and the highlight
// box are the same box BY CONSTRUCTION (INV-D6) rather than by three matching implementations.
// nullopt means "this entity has a reference the editor cannot resolve yet" -- loading, failed, or
// missing -- and EVERY consumer treats a nullopt exactly as it already treats an entity with NO
// MeshRenderer: a point at the origin for bounds, the screen-space disc for picking, the diamond
// marker for the overlay. No new fallback code exists anywhere.
[[nodiscard]] std::optional<Aabb> localBoundsFor(const MeshRenderer& renderer, const MeshBoundsLookup* lookup) noexcept;

// `entity`'s own contribution plus, when includeDescendants, its whole subtree (each descendant
// through its OWN world matrix, so parent scale/rotation compose correctly).
//   * an entity WITH a MeshRenderer whose local box RESOLVES contributes that box's 8 transformed
//     corners (task 3.1.5: the primitive's own box, or the referenced mesh's, via localBoundsFor);
//   * an entity WITHOUT one -- or one whose reference does not resolve YET -- contributes the single
//     POINT at its world translation;
//   * a dead or null handle contributes NOTHING.
// worldMatrix's silent identity for a never-transformed entity is inherited DELIBERATELY: such an
// entity contributes a point at the world origin. The subtree walk goes through editor::walkForest --
// an EXPLICIT stack, never recursion (misc-no-recursion is --warnings-as-errors on the Linux lane).
//
// `lookup` (task 3.1.5) is DEFAULTED AND LAST, so every caller written before it compiles and behaves
// identically: a null lookup resolves only primitives, which is exactly what this walk did before.
[[nodiscard]] Aabb entityBounds(const World& world, Entity entity, bool includeDescendants,
                                const MeshBoundsLookup* lookup = nullptr);

// The union over `entities`, each WITH its descendants. Dead and null handles are skipped and do
// NOT drag the box to the origin. This does NOT call Selection::prune -- mutating the selection
// during a draw walk is forbidden (.claude/rules/editor.md), and pruning is 2.2.1's job.
[[nodiscard]] Aabb selectionBounds(const World& world, std::span<const Entity> entities,
                                   const MeshBoundsLookup* lookup = nullptr);

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
[[nodiscard]] Aabb sceneBounds(const World& world, const MeshBoundsLookup* lookup = nullptr);

}  // namespace engine::editor
