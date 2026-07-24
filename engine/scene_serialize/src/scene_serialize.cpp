// engine/scene_serialize/src/scene_serialize.cpp — task 1.4.2: the World <-> SceneDocument bridge.
// One internal dispatch table (BUILTINS) drives BOTH load and save, so the two directions can never
// silently drift apart (D5). Declaration order == save-emission order == transform.cpp's registration
// order (Transform, Camera, DirectionalLight, PointLight, MeshRenderer) — pinned by a test (D8).
//
// CORRECTION 1 (plan): saveWorld resolves a parent's file id with a LINEAR std::find over the
// iteration-order std::vector<Entity>, never a std::unordered_map<Entity,...> — Entity (Handle<Tag>)
// has a defaulted operator== but no std::hash/operator<, so a Handle-keyed hash/ordered map does not
// compile. Scenes are a handful of entities; the O(N^2) worst case is irrelevant (correctness, not
// speed, governs — spec §3.5).

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/scene_format.hpp>
#include <aero/scene/world.hpp>
#include <aero/scene_serialize/scene_serialize.hpp>

#include "builtin_serializers.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::scene_serialize {

namespace {

// LOAD thunk: read payload -> fresh T -> attach. Best-effort: attach even when a field was bad
// (1.2.2 policy — a half-read component keeps its good fields + defaults). The bool is the report bit.
template <typename T>
bool loadComponent(World& world, Entity entity, const JsonValue& payload) {
    T value{};
    const bool ok = aeroReadJson(payload, value);  // ADL -> engine::aeroReadJson
    world.add<T>(entity, value);
    return ok;
}

// SAVE thunk: present? -> payload JsonValue. aeroWriteJson emits text; reparse to a DOM for the
// SceneComponentRecord (writeSceneText later re-emits it canonically, so pretty/compact here is moot).
template <typename T>
std::optional<JsonValue> saveComponent(const World& world, Entity entity) {
    const T* component = world.get<T>(entity);
    if (component == nullptr) {
        return std::nullopt;
    }
    JsonWriter writer{JsonWriterConfig{.pretty = false}};
    aeroWriteJson(writer, *component);  // ADL -> engine::aeroWriteJson
    JsonParseResult parsed = parseJson(writer.str());
    assert(parsed.ok() && "generated aeroWriteJson must emit parseable JSON");
    return std::move(parsed.value);
}

struct BuiltinComponent {
    std::string_view name;
    bool (*load)(World&, Entity, const JsonValue&);
    std::optional<JsonValue> (*save)(const World&, Entity);
};

// Single source of names — builtinComponentNames() returns a span over this, so the public query and
// the dispatch table can never disagree with each other. MUST match transform.cpp's registration
// order+names (pinned by a test, D8).
constexpr std::array<std::string_view, 5> BUILTIN_COMPONENT_NAMES{
    "engine::Transform", "engine::Camera", "engine::DirectionalLight", "engine::PointLight", "engine::MeshRenderer",
};

// Declaration order == save emission order.
constexpr std::array<BuiltinComponent, 5> BUILTINS{{
    {BUILTIN_COMPONENT_NAMES[0], &loadComponent<Transform>, &saveComponent<Transform>},
    {BUILTIN_COMPONENT_NAMES[1], &loadComponent<Camera>, &saveComponent<Camera>},
    {BUILTIN_COMPONENT_NAMES[2], &loadComponent<DirectionalLight>, &saveComponent<DirectionalLight>},
    {BUILTIN_COMPONENT_NAMES[3], &loadComponent<PointLight>, &saveComponent<PointLight>},
    {BUILTIN_COMPONENT_NAMES[4], &loadComponent<MeshRenderer>, &saveComponent<MeshRenderer>},
}};

[[nodiscard]] const BuiltinComponent* findBuiltin(std::string_view name) {
    for (const BuiltinComponent& builtin : BUILTINS) {
        if (builtin.name == name) {
            return &builtin;
        }
    }
    return nullptr;
}

}  // namespace

SceneLoadReport loadScene(World& world, const SceneDocument& doc) {
    AERO_PROFILE_ZONE;
    SceneLoadReport report;
    std::unordered_map<std::uint64_t, Entity> idToEntity;  // file id -> live entity (discarded on return)
    idToEntity.reserve(doc.entities.size());

    // Pass 1 — entities + components, in file order (== instantiation order, docs/09 / scene_format.hpp:42).
    for (const SceneEntityRecord& rec : doc.entities) {
        const Entity entity = world.create();
        idToEntity.emplace(rec.id, entity);
        ++report.entitiesCreated;
        for (const SceneComponentRecord& comp : rec.components) {
            const BuiltinComponent* builtin = findBuiltin(comp.type);
            if (builtin == nullptr) {
                AERO_LOG_WARN("scene: unknown component type \"{}\" on entity id {} — skipped", comp.type, rec.id);
                ++report.componentsSkipped;
                continue;
            }
            const bool ok = builtin->load(world, entity, comp.value);
            ++report.componentsAttached;
            if (!ok) {
                ++report.componentsFailed;  // aeroReadJson already WARNed the bad field(s)
            }
        }
    }

    // Pass 2 — parent links (forward references are legal; parseScene already proved the forest).
    for (const SceneEntityRecord& rec : doc.entities) {
        if (rec.parent == 0) {
            continue;
        }
        const auto childIt = idToEntity.find(rec.id);
        const auto parentIt = idToEntity.find(rec.parent);
        if (parentIt == idToEntity.end()) {  // only reachable from an unvalidated hand-built doc (D11)
            AERO_LOG_WARN("scene: entity id {} names unknown parent id {} — link skipped", rec.id, rec.parent);
            continue;
        }
        world.setParent(childIt->second, parentIt->second);  // rejects cycles loudly; never corrupts
    }
    return report;
}

SceneLoadResult loadSceneText(World& world, std::string_view text, const JsonParseConfig& config) {
    AERO_PROFILE_ZONE;
    SceneParseResult parsed = parseScene(text, config);
    // NOT `!parsed.ok()`: bugprone-unchecked-optional-access cannot connect an opaque out-of-line ok()
    // to `document`, and it would flag the deref below on the Linux lint lane (scene_format.cpp:317's
    // precedent).
    if (!parsed.document.has_value()) {
        return {std::move(parsed.error), {}};
    }
    return {std::nullopt, loadScene(world, *parsed.document)};
}

SceneDocument saveWorld(const World& world) {
    AERO_PROFILE_ZONE;
    std::vector<Entity> order;  // eachEntity order == id assignment order
    world.eachEntity([&order](Entity entity) { order.push_back(entity); });

    SceneDocument doc;
    doc.entities.reserve(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        const Entity entity = order[i];
        SceneEntityRecord rec;
        rec.id = i + 1;  // 1-based, contiguous, file-scoped (docs/09 §2.2)
        // rec.name stays empty — the World has no name storage (D7).
        if (const Entity p = world.parent(entity); p != Entity{}) {
            // CORRECTION 1: no std::hash<Entity>/operator< exists, so resolve the parent's file id
            // with a linear scan over the iteration-order vector rather than a Handle-keyed map.
            const auto it = std::find(order.begin(), order.end(), p);
            rec.parent = (it != order.end()) ? static_cast<std::uint64_t>(std::distance(order.begin(), it)) + 1 : 0;
        }
        for (const BuiltinComponent& builtin : BUILTINS) {  // registry order
            if (std::optional<JsonValue> payload = builtin.save(world, entity)) {
                rec.components.push_back(SceneComponentRecord{std::string{builtin.name}, std::move(*payload)});
            }
        }
        doc.entities.push_back(std::move(rec));
    }
    return doc;
}

std::string saveWorldText(const World& world) { return writeSceneText(saveWorld(world)); }

std::span<const std::string_view> builtinComponentNames() noexcept { return std::span{BUILTIN_COMPONENT_NAMES}; }

}  // namespace engine::scene_serialize
