#pragma once
// engine::scene_serialize (task 1.4.2): the World <-> SceneDocument bridge — the runtime name->type
// dispatch 1.2.3 deferred. LOAD (SceneDocument -> World) and SAVE (World -> SceneDocument), driven by
// one internal table of the 5 built-in reflected components. Exposes only engine types (World,
// SceneDocument, JsonValue, SceneError) — entt stays private inside aero::scene, and the generated
// aeroReadJson/aeroWriteJson are compiled into this library (the first PRODUCTION consumer of
// aero_reflect_generate_json). Gated on AERO_REFLECT_TOOLS at the CMake layer.
#include <aero/reflect/json_reader.hpp>   // JsonParseConfig (loadSceneText passthrough)
#include <aero/reflect/scene_format.hpp>  // SceneDocument, SceneError
#include <aero/scene/world.hpp>           // World, Entity

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::scene_serialize {

// What loadScene did. Counts, not errors — a load is best-effort (docs/09 tolerance policy).
struct SceneLoadReport {
    std::size_t entitiesCreated = 0;
    std::size_t componentsAttached = 0;  // recognised name + attached (fields may have been defaulted)
    std::size_t componentsSkipped = 0;   // unrecognised component type name (WARN + skip)
    std::size_t componentsFailed = 0;    // aeroReadJson returned false (bad payload; best-effort applied)
};

// loadSceneText's result: a parse/validate error (World untouched), or the report.
struct SceneLoadResult {
    std::optional<SceneError> error;  // engaged => parse/validate failed, nothing was loaded
    SceneLoadReport report;
};

// Instantiate `doc` into `world` (APPENDS; clear the World first for a fresh load). Entities are
// created in file order (== doc.entities order); parents linked in a second pass. Recognised
// components are deserialized+attached; unrecognised names WARN+skip. Best-effort on an UNVALIDATED
// hand-built doc (D11): an unresolvable parent id WARNs+skips the link, a cycle is refused by
// setParent. `world` must be a live (not moved-from) World.
[[nodiscard]] SceneLoadReport loadScene(World& world, const SceneDocument& doc);

// parseScene(text) then loadScene. On a parse/validate error, returns {error, {}} and does NOT touch
// `world`. The strict path (parseScene guarantees a valid forest before any entity is created).
[[nodiscard]] SceneLoadResult loadSceneText(World& world, std::string_view text, const JsonParseConfig& config = {});

// Serialize every LIVE entity of `world` into a SceneDocument: ids 1..N in eachEntity() order,
// `parent` resolved to the parent's assigned id (0 for roots), each present built-in emitted in
// registry order via the generated aeroWriteJson. Entity names are NOT stored in the World (D7), so
// every record's name is empty. A moved-from World yields an empty document.
[[nodiscard]] SceneDocument saveWorld(const World& world);

// saveWorld then writeSceneText — canonical scene text (one trailing '\n').
[[nodiscard]] std::string saveWorldText(const World& world);

// The fully-qualified names this module can load/save, in registry order. The editor's "which
// components are serializable" query, and the seam a test uses to pin these names against a fresh
// World's registrations (D8).
[[nodiscard]] std::span<const std::string_view> builtinComponentNames() noexcept;

}  // namespace engine::scene_serialize
