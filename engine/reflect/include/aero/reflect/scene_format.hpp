#pragma once
// engine/reflect/include/aero/reflect/scene_format.hpp — task 1.2.3: scene-file format v1.
// The DOCUMENT layer only: entities/ids/hierarchy/component-name keys, with payloads held as opaque
// JsonValue objects (decoding them needs a world + a name->type dispatch — Epics 1.3/1.4). The
// envelope is STRICT (fail-fast, one deterministic first error); unknown keys WARN + are ignored;
// component-payload tolerance is task 1.2.2's readField policy, applied at LOAD time, not here.
// docs/09-file-formats.md is the normative schema; this header enforces it.
#include <aero/reflect/json_reader.hpp>  // JsonParseConfig (the text overload's passthrough)
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

inline constexpr std::uint32_t SCENE_FORMAT_VERSION = 1;

// One member of an entity's "components" object: the reflected type's fully-qualified name (the exact
// string reflect-gen registers) and the payload object aeroWriteJson emitted. Opaque at this layer.
struct SceneComponentRecord {
    std::string type;
    JsonValue value;  // always object-kind in a valid document
};

// One row of "entities". id is file-scoped (>= 1; 0 is the reserved null sentinel `parent` uses for
// "no parent"); name is informational (empty == absent); `components` is in file order.
struct SceneEntityRecord {
    std::uint64_t id = 0;
    std::string name;
    std::uint64_t parent = 0;
    std::vector<SceneComponentRecord> components;
};

// A parsed scene file, always at SCENE_FORMAT_VERSION semantics (the version lives in the FILE; parse
// rejects any other value, so an in-memory document needs no version member).
struct SceneDocument {
    std::vector<SceneEntityRecord> entities;  // file order == instantiation order (task 1.4.2)
};

// line > 0  <=>  the failure happened at the JSON stage (fields copied verbatim from JsonParseError);
// scene-stage failures carry zeros and put their context (entities[i], ids) in the message.
struct SceneError {
    std::string message;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::size_t offset = 0;
};

struct SceneParseResult {
    std::optional<SceneDocument> document;
    SceneError error;

    [[nodiscard]] bool ok() const;
};

// DOM entry point (the primitive): validate + extract per docs/09. WARNs (unknown keys) are emitted
// only when the whole document validates; a rejected document yields exactly one error and zero WARNs.
[[nodiscard]] SceneParseResult parseScene(const JsonValue& root);

// Text convenience: parseJson (position-carrying errors) then the DOM overload.
[[nodiscard]] SceneParseResult parseScene(std::string_view text, const JsonParseConfig& config = {});

// The same semantic checks parseScene runs, for hand-built documents (the editor's pre-save hook) --
// PLUS one check parseScene can never trigger: a duplicate component type on one entity (the JSON
// object form of "components" collapses that before the scene layer ever sees it; only a hand-built
// std::vector can still violate the <=1-component-per-type invariant, D2). Engaged == invalid.
// writeScene does NOT call this (pure emission) — callers wanting the round-trip guarantee run it
// first.
[[nodiscard]] std::optional<SceneError> validateScene(const SceneDocument& scene);

// Emit the document through any writer config. Debug-asserts each payload isObject() (programmer
// misuse); performs NO graph validation. Canonical key order + omission rules per docs/09.
void writeScene(JsonWriter& writer, const SceneDocument& scene);

// Canonical form: default (pretty, 2-space) writer + ONE trailing '\n'. Byte-stable fixpoint over
// ENVELOPE-valid documents (validateScene(d) == nullopt):
//   writeSceneText(*parseScene(writeSceneText(d)).document) == writeSceneText(d).
// Payload-internal shapes are out of scope for this guarantee: duplicate keys *inside* a payload
// object, and payloads nested deeper than JsonParseConfig::maxDepth (256), will not round-trip --
// neither can occur in a real payload, since aeroWriteJson cannot produce either.
[[nodiscard]] std::string writeSceneText(const SceneDocument& scene);

}  // namespace engine
