#pragma once
// Aero Engine -- the reflected-field write seam (task 2.2.2, D2). PUBLIC and entt-free precisely so
// 2.4.2's generic property-set command can CALL these rather than re-derive them -- the entity_ops
// precedent (2.2.1 D15). Until 2.4.2 the inspector calls them directly; when the command stack lands,
// commands WRAP these four functions and the panel's call sites move, not the logic.
//
// ENTT-FREE BY RULE, like every header under editor/include (2.1.3 D9). The entt::meta walk lives in
// editor/src/component_ops.cpp; enforcement is file placement + review, exactly as for ImGui (R12) --
// there is no compile-time probe for this header, only manual review (a probe would be vacuous by
// construction, per 2.1.2's own reasoning for why the golden rule has none either). The literal
// `git grep -n 'entt::\|EnTT' -- editor/include/` is NOT empty (review finding 9) -- this file and
// inspector_model.hpp both cite `entt::`/`EnTT` in PROSE (this comment, the constness note below, the
// D8 clamp note). The real check strips comments first, the same way the boundary scripts do
// (nl -ba -w1 -s: <file> | sed -E 's|//.*||'), and THAT output is empty: no line of actual code in
// either public header ever names an entt:: type.
#include <aero/core/guid.hpp>  // engine::Guid -- task 3.1.5's one new category
#include <aero/core/math.hpp>  // engine::Vec3, engine::Quat
#include <aero/scene/entity.hpp>
#include <aero/scene/world.hpp>  // engine::ComponentTypeId (a value type -- needs the definition)

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace engine::editor {

// The WIDE transport for one reflected field value crossing the 2.4.2 command seam. Integrals widen to
// i64/u64 by signedness and floats to double: lossless on the way out, CLAMPED-then-narrowed on the way
// back in (D8). std::string is carried by value -- a command must own its before/after payload.
//
// task 3.1.5: Guid is APPENDED LAST to both, never inserted, so every existing std::get<>/
// std::holds_alternative<> and every FieldKind value keeps its meaning AND its number -- FieldKind is
// switched on exhaustively with no `default:` anywhere, so an inserted enumerator would renumber a
// serialized-nothing but would silently re-map every positional read of the variant. A Guid carries no
// range, no clamp and no widening: it goes in and comes out as itself.
using FieldValue = std::variant<bool, std::int64_t, std::uint64_t, double, Vec3, Quat, std::string, Guid>;

enum class FieldKind : std::uint8_t { Bool, Int, UInt, Float, Vec3, Quat, String, Guid };

// ---- CONSTNESS IS DELIBERATELY NOT UNIFORM ACROSS THIS SEAM (plan decision O1, 2026-07-26) --------
// These four take World& because the WRITE path is meta_type::from_void(void*) + meta_data::set, which
// genuinely refuses a const instance (measured against the pinned EnTT: set through a const handle
// returns false). readComponentField matches writeComponentField so 2.4.2's capture-before-write pairs
// ONE signature shape. buildInspectorModel (inspector_model.hpp) takes `const World&` instead, because
// its read path provably needs nothing more. TASK 2.4.2 MUST NOT ASSUME A UNIFORM CONSTNESS ACROSS THE
// FIVE ENTRY POINTS -- each is the honest minimum for what it does.

// All four are the DIRECT-WRITE seam (never bypassed by the panel). Rejections return
// false/nullopt + one AERO_LOG_ERROR and NEVER mutate: moved-from World, dead/null entity,
// unregistered id, no meta for the type, unknown field, kind mismatch.
bool addComponent(World& world, Entity entity, ComponentTypeId id);     // default-construct;
                                                                        // REFUSES a present type (D10)
bool removeComponent(World& world, Entity entity, ComponentTypeId id);  // silent false when absent

// Answers "can this component's fields be reached through the reflection seam at all?" -- the guard a
// caller needs BEFORE readComponentField, in a -DAERO_REFLECT_TOOLS=OFF build.
//
// There are TWO registries and they do not agree. The World's own component table is hand-written and
// always present (engine/scene/src/transform.cpp registers the five built-ins), so findComponentType
// resolves engine::MeshRenderer BY NAME even when no generated entt::meta exists anywhere. A caller
// that guards only on ComponentTypeId::valid() therefore sails past and reaches readComponentField,
// which logs an AERO_LOG_ERROR from the seam -- in the one configuration where nothing is wrong. This
// predicate asks the meta registry directly, exactly as buildInspectorModel's `hasFields` does.
//
// const World&: this is the honest minimum (see the constness note above) -- it reads a type name and
// resolves a meta type, and touches no entity.
[[nodiscard]] bool componentFieldsAreReflected(const World& world, ComponentTypeId id);

// World& (not const): see the constness note above. The read itself mutates nothing -- the signature
// pairs the write path, it is not a semantic. Every caller holds a World&.
[[nodiscard]] std::optional<FieldValue> readComponentField(World& world, Entity entity, ComponentTypeId id,
                                                           std::string_view field);
// Clamps TWICE (D8): to the field's FieldUiMeta range when present, AND to the destination type's
// numeric limits -- a 300 into a uint8_t stores 255, never wraps (EnTT's own conversion WRAPS to 44;
// measured). NaN is NOT sanitized: std::clamp's comparisons are false for NaN, so it passes through,
// matching the engine-wide "values are never validated" stance (E10).
bool writeComponentField(World& world, Entity entity, ComponentTypeId id, std::string_view field,
                         const FieldValue& value);

}  // namespace engine::editor
