#pragma once
// Aero Engine -- the Transform write seam (task 2.3.3). PUBLIC and entt-free precisely so 2.4.2's
// transform command can CALL these rather than re-derive them -- the component_ops (2.2.2 D2) and
// entity_ops (2.2.1 D15) precedent. Until 2.4.2 the gizmo calls them directly; when the command
// stack lands, commands WRAP these two and the panel's call sites move, not the logic.
//
// NOT routed through component_ops::writeComponentField, deliberately (D21): that path costs three
// string-keyed entt::meta lookups per drag frame, cannot express "write position and leave rotation
// and scale byte-identical" atomically, and hard-depends on AERO_REFLECT_TOOLS being ON -- while
// -DAERO_REFLECT_TOOLS=OFF with shaders ON is a legal configuration in which the Viewport works and
// the gizmo must too (AC-17). World::get<Transform>() is direct, typed, allocation-free and
// tools-independent.
//
// ENTT-FREE and ImGui-FREE BY RULE, like every header under editor/include (2.1.3 D9), held by FILE
// PLACEMENT and review, not by a probe (R12).

#include <aero/scene/entity.hpp>
#include <aero/scene/transform.hpp>

#include <optional>

namespace engine {
class World;
}  // namespace engine

namespace engine::editor {

// ---- CONSTNESS NOTE, stated once so 2.4.2 does not have to guess -----------------------------------
// readTransform takes `const World&` (unlike component_ops::readComponentField, which takes `World&`
// because entt::meta's write path genuinely refuses a const instance -- component_ops.hpp:35-41).
// This path has no such constraint: World::get<T>(Entity) const exists (world.hpp:209). Do NOT
// "harmonise" the two seams' constness -- each is the honest minimum for what it does, exactly as
// component_ops.hpp:40-41 already warns.

// nullopt for a moved-from World, a null or dead entity, or an entity with no Transform. SILENT --
// this is a polling path, not an error path (worldMatrix's own stance, scene/transform.hpp:66-67).
[[nodiscard]] std::optional<Transform> readTransform(const World& world, Entity entity);

// false + exactly ONE AERO_LOG_ERROR on every rejection (moved-from World, null/dead entity, no
// Transform present), and NEVER a partial mutation. Values are NOT validated: a non-finite field is
// stored as given, matching the engine-wide stance (component_ops E10, scene/transform.hpp:72-73).
// The gizmo's own finiteness guards live in gizmo.cpp, UPSTREAM of this call.
//
// Deliberately does NOT create the component when absent: adding a Transform as a side effect of a
// gizmo drag would be a STRUCTURAL edit, which is 2.4.2's create/delete territory (T5).
bool writeTransform(World& world, Entity entity, const Transform& value);

}  // namespace engine::editor
