// engine/reflect/src/scene_format.cpp — task 1.2.3: scene-file format v1 (docs/09-file-formats.md).
// The DOCUMENT layer: three-pass envelope validation + extraction, plus canonical emission. Component
// payloads are OPAQUE JsonValue objects here. Everything is ITERATIVE (misc-no-recursion is live);
// nothing asserts on input content (untrusted data — the 1.2.2 D16 lineage); no exceptions cross the
// API. Every message is ASCII-only and deterministic: the hash containers below are LOOKUP-ONLY and
// are never iterated, so no unordered_map ordering can reach the output.
#include <aero/core/log.hpp>
#include <aero/reflect/scene_format.hpp>
#include <aero/reflect/serialize.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine {

namespace {

constexpr std::string_view VERSION_KEY = "version";
constexpr std::string_view ENTITIES_KEY = "entities";
constexpr std::string_view ID_KEY = "id";
constexpr std::string_view NAME_KEY = "name";
constexpr std::string_view PARENT_KEY = "parent";
constexpr std::string_view COMPONENTS_KEY = "components";

// A scene-stage error: line/column/offset stay ZERO by contract (D8/AC-8) — the context lives in the
// message text instead.
SceneError sceneError(std::string message) {
    SceneError error;
    error.message = std::move(message);
    return error;
}

// "(found <kind>)" — or, when a Number failed a FORM rule rather than a KIND rule, the quoted lexeme
// instead (D11): `(found "1.5")`, `(found "-1")`, `(found string)`, `(found null)`.
std::string foundDetail(const JsonValue& v) {
    if (v.isNumber()) {
        return std::format("\"{}\"", v.numberLexeme());
    }
    return std::string(jsonKindName(v.kind()));
}

// ---- one home per shared catalog line, so the DOM path and the typed path can never diverge -------

std::string idMessage(std::size_t index, std::string_view found) {
    return std::format("entities[{}]: \"id\" must be an integer >= 1 (found {})", index, found);
}

std::string duplicateIdMessage(std::size_t index, std::uint64_t id, std::size_t firstIndex) {
    return std::format("entities[{}]: duplicate entity id {} (first used by entities[{}])", index, id, firstIndex);
}

std::string componentNameMessage(std::size_t index, std::uint64_t id) {
    return std::format("entities[{}] (id {}): component name must be a non-empty string", index, id);
}

std::string payloadMessage(std::size_t index, std::uint64_t id, std::string_view type, std::string_view found) {
    return std::format("entities[{}] (id {}): component \"{}\" payload must be an object (found {})", index, id, type,
                       found);
}

// Emitted by BOTH the extract pass and validateScene (1.2 audit, finding 3a). TEXT-parsed scenes can
// still never trip it in extract -- the JSON object form of "components" collapses a duplicate key
// (last-wins) before the scene layer sees it -- but a HAND-BUILT DOM handed to
// parseScene(const JsonValue&) can carry two members with the same key; extract rejects that instead
// of silently producing two records of one type. validateScene keeps its own check for hand-built
// SceneDocuments that never went through parseScene (the editor pre-save path).
std::string duplicateComponentTypeMessage(std::size_t index, std::uint64_t id, std::string_view type) {
    return std::format("entities[{}] (id {}): duplicate component type \"{}\"", index, id, type);
}

// ---- pass 1: envelope + per-entity structure, file order, fail-fast --------------------------------
// Version is checked *before* anything structural (D5), so {"version": 2} reports *unsupported
// version* even when the rest is garbage.

std::optional<SceneError> extract(const JsonValue& root, SceneDocument& out,
                                  std::unordered_map<std::uint64_t, std::size_t>& idToIndex) {
    if (!root.isObject()) {
        return sceneError(std::format("scene root must be a JSON object (found {})", jsonKindName(root.kind())));
    }

    // ---- "version" FIRST (D5): a future-format file must fail with the RIGHT message ----
    const JsonValue* version = root.find(VERSION_KEY);
    if (version == nullptr) {
        return sceneError("missing required key \"version\"");
    }
    // asU64 already rejects non-Number kinds, non-integral forms, negatives and > 2^64-1 (N3).
    const std::optional<std::uint64_t> versionValue = version->asU64();
    if (!versionValue.has_value()) {
        return sceneError(std::format("\"version\" must be an integer (found {})", foundDetail(*version)));
    }
    if (*versionValue != SCENE_FORMAT_VERSION) {
        return sceneError(std::format("unsupported scene format version {} (this build reads version {})",
                                      *versionValue, SCENE_FORMAT_VERSION));
    }

    const JsonValue* entities = root.find(ENTITIES_KEY);
    if (entities == nullptr) {
        return sceneError("missing required key \"entities\"");
    }
    if (!entities->isArray()) {
        return sceneError(std::format("\"entities\" must be an array (found {})", jsonKindName(entities->kind())));
    }

    const std::vector<JsonValue>& rows = entities->elements();
    out.entities.reserve(rows.size());
    idToIndex.reserve(rows.size());

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const JsonValue& row = rows[i];
        if (!row.isObject()) {
            return sceneError(std::format("entities[{}] must be an object (found {})", i, jsonKindName(row.kind())));
        }

        SceneEntityRecord record;

        const JsonValue* id = row.find(ID_KEY);
        if (id == nullptr) {
            return sceneError(std::format("entities[{}]: missing required key \"id\"", i));
        }
        // value_or(0) folds "not a number" / "not integral" / "out of range" / "negative" / "the
        // reserved 0" into the ONE catalog line D11 specifies (N3) -- and is immune to N2.
        record.id = id->asU64().value_or(0);
        if (record.id == 0) {
            return sceneError(idMessage(i, foundDetail(*id)));
        }

        const auto [seen, inserted] = idToIndex.emplace(record.id, i);
        if (!inserted) {
            return sceneError(duplicateIdMessage(i, record.id, seen->second));
        }

        const JsonValue* name = row.find(NAME_KEY);
        if (name != nullptr) {
            const std::optional<std::string_view> nameValue = name->asString();
            if (!nameValue.has_value()) {
                return sceneError(std::format("entities[{}] (id {}): \"name\" must be a string (found {})", i,
                                              record.id, jsonKindName(name->kind())));
            }
            record.name = std::string(*nameValue);
        }

        const JsonValue* parent = row.find(PARENT_KEY);
        if (parent != nullptr) {
            record.parent = parent->asU64().value_or(0);
            if (record.parent == 0) {
                return sceneError(std::format("entities[{}] (id {}): \"parent\" must be an integer >= 1 (found {})", i,
                                              record.id, foundDetail(*parent)));
            }
        }

        const JsonValue* components = row.find(COMPONENTS_KEY);
        if (components != nullptr) {
            if (!components->isObject()) {
                return sceneError(std::format("entities[{}] (id {}): \"components\" must be an object (found {})", i,
                                              record.id, jsonKindName(components->kind())));
            }
            record.components.reserve(components->members().size());
            // Lookup-only, scoped to this entity, never iterated (same determinism argument as
            // validateScene's twin set). Unreachable from TEXT input (the JSON layer collapses
            // duplicate keys last-wins); it bites only on a hand-built DOM (finding 3a).
            std::unordered_set<std::string_view> seenTypes;
            seenTypes.reserve(components->members().size());
            for (const JsonMember& member : components->members()) {
                if (member.key.empty()) {
                    return sceneError(componentNameMessage(i, record.id));
                }
                if (!member.value.isObject()) {
                    return sceneError(payloadMessage(i, record.id, member.key, jsonKindName(member.value.kind())));
                }
                if (!seenTypes.insert(member.key).second) {
                    return sceneError(duplicateComponentTypeMessage(i, record.id, member.key));
                }
                // JsonValue is value-semantic (F3): this COPIES the payload subtree out of the DOM, so
                // the document has no lifetime coupling to the caller's parse tree (E-move/copy).
                record.components.push_back(SceneComponentRecord{.type = member.key, .value = member.value});
            }
        }

        out.entities.push_back(std::move(record));
    }
    return std::nullopt;
}

// ---- pass 2: parent resolution (forward references resolve here by construction) -------------------

std::optional<SceneError> checkParents(const SceneDocument& scene,
                                       const std::unordered_map<std::uint64_t, std::size_t>& idToIndex) {
    for (std::size_t i = 0; i < scene.entities.size(); ++i) {
        const SceneEntityRecord& entity = scene.entities[i];
        if (entity.parent == 0) {
            continue;
        }
        if (idToIndex.find(entity.parent) == idToIndex.end()) {
            return sceneError(
                std::format("entities[{}] (id {}): parent {} does not reference any entity id in this scene", i,
                            entity.id, entity.parent));
        }
    }
    return std::nullopt;
}

// ---- pass 3: the forest check, iterative, amortized O(n), no hash container ever iterated -----------

enum class WalkState : std::uint8_t { Unvisited, InProgress, Acyclic };

// Iterative parent-chain walk per entity, in FILE order, with a tri-state colour array: InProgress
// marks the nodes on the CURRENT walk (a revisit is a cycle), Acyclic memoizes every chain already
// proven to terminate at a root, so a single 10k-long chain still costs O(n) overall (AC-6). Self-
// parenting is just a 1-cycle, and an entity hanging OFF a cycle is caught by the same walk.
std::optional<SceneError> checkForest(const SceneDocument& scene,
                                      const std::unordered_map<std::uint64_t, std::size_t>& idToIndex) {
    std::vector<WalkState> states(scene.entities.size(), WalkState::Unvisited);
    std::vector<std::size_t> path;
    for (std::size_t start = 0; start < scene.entities.size(); ++start) {
        if (states[start] != WalkState::Unvisited) {
            continue;
        }
        path.clear();
        std::size_t current = start;
        bool cyclic = false;
        while (true) {
            if (states[current] == WalkState::Acyclic) {
                break;  // joins a chain already proven to reach a root
            }
            if (states[current] == WalkState::InProgress) {
                cyclic = true;  // revisited a node inside THIS walk
                break;
            }
            states[current] = WalkState::InProgress;
            path.push_back(current);
            const std::uint64_t parent = scene.entities[current].parent;
            if (parent == 0) {
                break;  // reached a root
            }
            const auto next = idToIndex.find(parent);
            if (next == idToIndex.end()) {
                break;  // unreachable: pass 2 resolved every parent. Never assert on data (D14).
            }
            current = next->second;
        }
        if (cyclic) {
            return sceneError(
                std::format("entities[{}] (id {}): parent chain is cyclic", start, scene.entities[start].id));
        }
        for (const std::size_t index : path) {
            states[index] = WalkState::Acyclic;
        }
    }
    return std::nullopt;
}

// ---- success-only WARN sweep (D6/D11) ---------------------------------------------------------------
// Success path only (D6): a rejected document emits exactly one error and ZERO warns. Document order.
// Never descends into component payloads (their tolerance is task 1.2.2's, applied at LOAD time).
void warnUnknownSceneKeys(const JsonValue& root, const SceneDocument& scene) {
    for (const JsonMember& member : root.members()) {
        if (member.key != VERSION_KEY && member.key != ENTITIES_KEY) {
            AERO_LOG_WARN("scene: ignoring unknown key \"{}\"", member.key);
        }
    }
    const JsonValue* entities = root.find(ENTITIES_KEY);
    if (entities == nullptr) {
        return;  // unreachable after a successful extract
    }
    const std::vector<JsonValue>& rows = entities->elements();
    for (std::size_t i = 0; i < rows.size() && i < scene.entities.size(); ++i) {
        for (const JsonMember& member : rows[i].members()) {
            if (member.key == ID_KEY || member.key == NAME_KEY || member.key == PARENT_KEY ||
                member.key == COMPONENTS_KEY) {
                continue;
            }
            AERO_LOG_WARN("scene: entities[{}] (id {}): ignoring unknown key \"{}\"", i, scene.entities[i].id,
                          member.key);
        }
    }
}

}  // namespace

// ---- the public surface ------------------------------------------------------------------------------

bool SceneParseResult::ok() const { return document.has_value(); }

SceneParseResult parseScene(const JsonValue& root) {
    SceneParseResult result;
    SceneDocument document;
    std::unordered_map<std::uint64_t, std::size_t> idToIndex;

    if (std::optional<SceneError> error = extract(root, document, idToIndex)) {
        result.error = std::move(*error);
        return result;
    }
    if (std::optional<SceneError> error = checkParents(document, idToIndex)) {
        result.error = std::move(*error);
        return result;
    }
    if (std::optional<SceneError> error = checkForest(document, idToIndex)) {
        result.error = std::move(*error);
        return result;
    }
    warnUnknownSceneKeys(root, document);  // success-only sweep, document order (D6)
    result.document = std::move(document);
    return result;
}

SceneParseResult parseScene(std::string_view text, const JsonParseConfig& config) {
    const JsonParseResult parsed = parseJson(text, config);
    // NOT `!parsed.ok()`: bugprone-unchecked-optional-access cannot connect an opaque out-of-line
    // ok() to `value`, and it would flag the deref below on the Linux lint lane (N2).
    if (!parsed.value.has_value()) {
        SceneParseResult result;
        result.error = SceneError{.message = parsed.error.message,
                                  .line = parsed.error.line,
                                  .column = parsed.error.column,
                                  .offset = parsed.error.offset};
        return result;  // line > 0 marks a JSON-stage failure (D8/AC-8)
    }
    return parseScene(*parsed.value);
}

std::optional<SceneError> validateScene(const SceneDocument& scene) {
    std::unordered_map<std::uint64_t, std::size_t> idToIndex;
    idToIndex.reserve(scene.entities.size());
    for (std::size_t i = 0; i < scene.entities.size(); ++i) {
        const SceneEntityRecord& entity = scene.entities[i];
        if (entity.id == 0) {
            return sceneError(idMessage(i, "\"0\""));  // quoted lexeme, matching foundDetail's form (D12)
        }
        const auto [seen, inserted] = idToIndex.emplace(entity.id, i);
        if (!inserted) {
            return sceneError(duplicateIdMessage(i, entity.id, seen->second));
        }
        // Lookup-only, scoped to this entity, never iterated (AC-11 determinism holds regardless of
        // unordered_set's iteration order, because iteration order is never observed).
        std::unordered_set<std::string_view> seenTypes;
        seenTypes.reserve(entity.components.size());
        for (const SceneComponentRecord& component : entity.components) {
            if (component.type.empty()) {
                return sceneError(componentNameMessage(i, entity.id));
            }
            if (!component.value.isObject()) {
                return sceneError(payloadMessage(i, entity.id, component.type, jsonKindName(component.value.kind())));
            }
            if (!seenTypes.insert(component.type).second) {
                return sceneError(duplicateComponentTypeMessage(i, entity.id, component.type));
            }
        }
    }
    if (std::optional<SceneError> error = checkParents(scene, idToIndex)) {
        return error;
    }
    return checkForest(scene, idToIndex);
}

void writeScene(JsonWriter& writer, const SceneDocument& scene) {
    writer.beginObject();
    writer.key(VERSION_KEY);
    writer.value(static_cast<unsigned long long>(SCENE_FORMAT_VERSION));  // explicit cast: N1
    writer.key(ENTITIES_KEY);
    writer.beginArray();
    for (const SceneEntityRecord& entity : scene.entities) {
        writer.beginObject();
        writer.key(ID_KEY);
        writer.value(static_cast<unsigned long long>(entity.id));  // N1 -- ambiguous on Linux without it
        if (!entity.name.empty()) {                                // "" == absent (D7 omission rule)
            writer.key(NAME_KEY);
            writer.value(std::string_view(entity.name));
        }
        if (entity.parent != 0) {  // 0 == no parent, expressed by OMISSION (D3)
            writer.key(PARENT_KEY);
            writer.value(static_cast<unsigned long long>(entity.parent));
        }
        if (!entity.components.empty()) {  // empty == absent
            writer.key(COMPONENTS_KEY);
            writer.beginObject();
            for (const SceneComponentRecord& component : entity.components) {
                // D14: the ONLY assert in this file, and it is programmer misuse (a hand-built
                // document), never input content. In release it compiles out and the value is emitted
                // as-is, so the resulting file fails parseScene at the right layer -- surfacing the
                // bug rather than hiding it. NEVER feed writeScene a non-object payload in a test.
                assert(component.value.isObject());
                writer.key(component.type);
                engine::reflect::writeJson(writer, component.value);
            }
            writer.endObject();
        }
        writer.endObject();
    }
    writer.endArray();
    writer.endObject();
}

std::string writeSceneText(const SceneDocument& scene) {
    JsonWriter writer;  // the DEFAULT config: pretty, 2-space (docs/09's canonical form)
    writeScene(writer, scene);
    std::string text = writer.str();
    text += '\n';  // exactly ONE trailing newline (the writer itself has none, F4; parseJson accepts it, F5)
    return text;
}

}  // namespace engine
