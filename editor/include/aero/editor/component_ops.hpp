#pragma once
// Aero Engine -- the reflected-field write seam (task 2.2.2, D2). PUBLIC and entt-free precisely so
// 2.4.2's generic property-set command can CALL these rather than re-derive them -- the entity_ops
// precedent (2.2.1 D15). Until 2.4.2 the inspector calls them directly; when the command stack lands,
// commands WRAP these four functions and the panel's call sites move, not the logic.
//
// ENTT-FREE BY RULE, like every header under editor/include (2.1.3 D9). The entt::meta walk lives in
// editor/src/component_ops.cpp; enforcement is file placement + review, exactly as for ImGui (R12).
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
using FieldValue = std::variant<bool, std::int64_t, std::uint64_t, double, Vec3, Quat, std::string>;

enum class FieldKind : std::uint8_t { Bool, Int, UInt, Float, Vec3, Quat, String };

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
