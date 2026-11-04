// Aero Engine -- engine.spawn(asset, ...) (task 4.4.4): a copy of a saved entity tree, named by its res:// path.
//
// The three forms (asset; asset and a Transform; asset, position, rotation?, scale?) run the same checks in the same
// order, each a TypeError at engine.spawn that names the script's own path text where it knows one:
//   1 the argument count  2 the first argument is a string  3 the placement arguments (script_api_transform.cpp, the
//   one source that names the component the placement is read from)  then, with the reflect tools OFF, a stop; then
//   4 a World is bound  5 the path is a res:// file  6 the VM has a file source  7 the file exists  8 it is no larger
//   than the heap limit (before the read, and again after)  9 it reads  10 it parses as a scene  11 it holds exactly
//   one root  12 the bound World can hold a copy  13 the entity object is made (the backend's own out-of-memory).
// Nothing here runs script code (script_api.hpp rules 3 and 4), and a refusal at any step leaves the World as it was:
// a copy that cannot be handed to the script is destroyed whole.
//
// The tools-OFF build has no component serializers, so it cannot parse a prefab; the stop sits after the argument
// checks, so a bad argument draws its own error in either build and spawn() works in both.

#include <aero/core/vfs.hpp>
#include <aero/scene/prefab.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/internal/module_graph.hpp>

#include "script_api.hpp"
#include "vm_impl.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#if defined(AERO_SCRIPT_PREFABS)
    #include <aero/scene_serialize/scene_serialize.hpp>
#endif

namespace engine::script {
namespace {

// "<site>: <reason>" as a TypeError, returning JS_EXCEPTION.
JSValue throwAtSite(JSContext* ctx, const Site& site, const std::string& reason) {
    return throwTypeError(ctx, describeSite(site) + ": " + reason);
}

#if defined(AERO_SCRIPT_PREFABS)

// "'<path>': <reason>", `path` being the script's own text.
JSValue throwForPath(JSContext* ctx, const Site& site, std::string_view path, const std::string& reason) {
    return throwAtSite(ctx, site, "'" + std::string{path} + "': " + reason);
}

// The text as a scene with exactly one root, or an exception pending. A prefab file is a scene v1 document, so its
// tolerance policy is the scene's: an unknown component is a WARN and a skip.
[[nodiscard]] std::optional<Prefab> parsePrefab(JSContext* ctx, const Site& site, std::string_view path,
                                                std::string_view text) {
    World parsed;
    const scene_serialize::SceneLoadResult loaded = scene_serialize::loadSceneText(parsed, text);
    if (loaded.error.has_value()) {
        const SceneError& error = *loaded.error;
        // A scene-stage failure carries no position (line 0): its message names the entity instead.
        const std::string where =
            error.line > 0U ? "line " + std::to_string(error.line) + ", column " + std::to_string(error.column) + ": "
                            : std::string{};
        static_cast<void>(throwForPath(ctx, site, path, where + error.message));
        return std::nullopt;
    }
    std::optional<Prefab> prefab = Prefab::fromWorld(std::move(parsed));
    if (!prefab.has_value()) {
        static_cast<void>(throwForPath(ctx, site, path, "a prefab has exactly one root"));
    }
    return prefab;
}

// The file at `key` (a normalized res:// path) read and parsed, or an exception pending. The size cap is the module
// loader's: a file larger than the heap limit is refused before a byte of it is read.
[[nodiscard]] std::optional<Prefab> loadPrefab(JSContext* ctx, const ScriptApi& api, const Site& site,
                                               const std::string& key, std::string_view path) {
    const std::optional<std::uint64_t> size = api.modules->fileSize(key);
    if (!size.has_value()) {
        static_cast<void>(throwForPath(ctx, site, path, "no such file"));
        return std::nullopt;
    }
    const auto tooLarge = [&](std::uint64_t bytes) {
        return std::to_string(bytes) + " bytes is more than this VM's heap limit of " +
               std::to_string(api.heapLimitBytes);
    };
    if (!internal::sourceWithinCap(*size, api.heapLimitBytes)) {
        static_cast<void>(throwForPath(ctx, site, path, tooLarge(*size)));
        return std::nullopt;
    }
    const std::optional<std::string> text = api.modules->readText(key);
    if (!text.has_value()) {
        static_cast<void>(throwForPath(ctx, site, path, "no such file"));
        return std::nullopt;
    }
    if (!internal::sourceWithinCap(text->size(), api.heapLimitBytes)) {  // the file grew after its size was read
        static_cast<void>(throwForPath(ctx, site, path, tooLarge(text->size())));
        return std::nullopt;
    }
    return parsePrefab(ctx, site, path, *text);
}

// Checks 4 to 13, the placement already read.
[[nodiscard]] JSValue spawnFromFile(JSContext* ctx, ScriptApi& api, const Site& site, JSValueConst pathValue,
                                    const SpawnPlacement& placement) {
    const HeldCString pathText{ctx, pathValue};
    if (!pathText.ok()) {
        return JS_EXCEPTION;
    }
    const std::string_view path = pathText.view();
    if (api.world == nullptr) {
        return throwAtSite(ctx, site, "no World is bound");
    }
    const std::optional<std::string> normal = normalizeVirtualPath(path);
    if (!normal.has_value() || normal->empty()) {
        return throwAtSite(ctx, site, "'" + std::string{path} + "' is not a res:// file path");
    }
    if (api.modules == nullptr) {
        return throwForPath(ctx, site, path, "this VM has no file source");
    }
    const std::optional<Prefab> prefab = loadPrefab(ctx, api, site, "res://" + *normal, path);
    if (!prefab.has_value()) {
        return JS_EXCEPTION;
    }
    World& world = *api.world;
    const Entity root = instantiateAtPlacement(*prefab, world, placement);
    if (!root.valid()) {
        return throwForPath(ctx, site, path, "the bound World cannot hold a copy");
    }
    const JSValue object = entityObjectFor(ctx, api, EntityRef{root, api.binding});
    if (JS_IsException(object)) {
        static_cast<void>(world.destroy(root));  // a refused object leaves no entity of the tree behind
    }
    return object;
}

#endif  // AERO_SCRIPT_PREFABS

}  // namespace

JSValue spawnPrefab(JSContext* ctx, ScriptApi& api, int argc, JSValueConst* argv) {
    const Site site{{}, "spawn", {}};
    if (argc > SPAWN_MAX_ARGUMENTS) {
        return throwAtSite(
            ctx, site,
            "expected at most " + std::to_string(SPAWN_MAX_ARGUMENTS) + " arguments, got " + std::to_string(argc));
    }
    if (!JS_IsString(argv[0])) {
        return throwAtSite(ctx, site, "expected a string, got " + describeValue(ctx, argv[0], api));
    }
    SpawnPlacement placement;
    if (!readSpawnPlacement(ctx, api, argv, placement)) {
        return JS_EXCEPTION;
    }
#if !defined(AERO_SCRIPT_PREFABS)
    return throwAtSite(ctx, site, "built without AERO_REFLECT_TOOLS");  // after the same argument checks
#else
    return spawnFromFile(ctx, api, site, argv[0], placement);
#endif
}

}  // namespace engine::script
