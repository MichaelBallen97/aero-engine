// Aero Engine -- the World side of the script API (task 4.4.2): the entity table, which makes one engine.Entity object
// per entity per binding. script_api.hpp states the rules every host function keeps; the ones that shape this file are
// 5 (an object is made with JS_NewObjectClass, never by setting properties) and 8 (JS_EXCEPTION only with an exception
// pending).
//
// The namespace functions (spawn, destroy, find, findAll, query) and the VM's destroy queue live here too.
// engine.destroy only queues: the entity stays alive and every script keeps reading it, until ScriptSystem calls
// Vm::destroyQueued at the end of the update.
//
// The table holds JS references (HELD): releaseEntityObjects frees them, and every bindWorld, unbindWorld and
// releaseScriptApi calls it, so no object outlives the binding that made it in the table. An object a script kept
// stays alive through its own reference and reads `alive` false, because its ref names an earlier binding.

#include <aero/core/log.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/vm.hpp>

#include "script_api.hpp"
#include "vm_impl.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {
namespace {

static_assert(MAX_QUERY_CLASSES == World::MAX_QUERY_TYPES);

// "<site>: <reason>" as a TypeError (a RangeError when `range`), returning JS_EXCEPTION.
JSValue throwAtSite(JSContext* ctx, const Site& site, const std::string& reason, bool range = false) {
    const std::string text = describeSite(site) + ": " + reason;
    return range ? throwRangeError(ctx, text) : throwTypeError(ctx, text);
}

}  // namespace

JSValue spawnEntity(JSContext* ctx, ScriptApi& api, int argc, JSValueConst* argv) {
    const Site site{{}, "spawn", {}};
    if (argc > 0) {
        return spawnPrefab(ctx, api, argc, argv);
    }
    if (api.world == nullptr) {
        return throwAtSite(ctx, site, "no World is bound");
    }
    World& world = *api.world;
    const Entity entity = world.create();
    if (!entity.valid()) {
        return throwAtSite(ctx, site, "the bound World cannot create an entity");
    }
    if (!addIdentityTransform(world, entity)) {
        static_cast<void>(world.destroy(entity));
        return throwAtSite(ctx, site, "the bound World cannot give the entity a transform");
    }
    const JSValue object = entityObjectFor(ctx, api, EntityRef{entity, api.binding});
    if (JS_IsException(object)) {
        static_cast<void>(world.destroy(entity));  // a refused object leaves no entity behind
    }
    return object;
}

JSValue queueDestroy(JSContext* ctx, ScriptApi& api, JSValueConst value) {
    const Site site{{}, "destroy", {}};
    const auto* const ref = static_cast<const EntityRef*>(JS_GetOpaque(value, api.entityClass));
    if (ref == nullptr) {
        return throwAtSite(ctx, site, "expected an engine.Entity, got " + describeValue(ctx, value, api));
    }
    if (api.world == nullptr) {
        return throwAtSite(ctx, site, "no World is bound");
    }
    const std::uint64_t key = entityKeyOf(ref->entity);
    if (ref->binding != api.binding || !api.world->alive(ref->entity) || api.destroyMarks.contains(key)) {
        return JS_FALSE;  // an earlier binding, a dead entity or one already queued: silent, nothing changes
    }
    api.destroyQueue.push_back(ref->entity);
    api.destroyMarks.insert(key);
    return JS_TRUE;
}

JSValue findEntity(JSContext* ctx, ScriptApi& api, JSValueConst value) {
    const Site site{{}, "find", {}};
    if (!JS_IsString(value)) {
        return throwAtSite(ctx, site, "expected a string, got " + describeValue(ctx, value, api));
    }
    if (api.world == nullptr) {
        return throwAtSite(ctx, site, "no World is bound");
    }
    const HeldCString text{ctx, value};
    if (!text.ok()) {
        return JS_EXCEPTION;
    }
    if (text.view().empty()) {
        return JS_NULL;
    }
    Entity best{};
    api.world->eachEntity([&](Entity entity) {
        if ((!best.valid() || entity.index < best.index) && api.world->name(entity) == text.view()) {
            best = entity;
        }
    });
    return best.valid() ? entityObjectFor(ctx, api, EntityRef{best, api.binding}) : JS_NULL;
}

JSValue findAllEntities(JSContext* ctx, ScriptApi& api, JSValueConst value) {
    const Site site{{}, "findAll", {}};
    if (!JS_IsString(value)) {
        return throwAtSite(ctx, site, "expected a string, got " + describeValue(ctx, value, api));
    }
    if (api.world == nullptr) {
        return throwAtSite(ctx, site, "no World is bound");
    }
    const HeldCString text{ctx, value};
    if (!text.ok()) {
        return JS_EXCEPTION;
    }
    return findAllArray(ctx, api, text.view());
}

JSValue queryEntities(JSContext* ctx, ScriptApi& api, int argc, JSValueConst* argv) {
    const Site site{{}, "query", {}};
    if (argc < 1 || argc > static_cast<int>(MAX_QUERY_CLASSES)) {
        return throwAtSite(ctx, site, "expected 1 to 8 component classes, got " + std::to_string(argc), true);
    }
    std::array<std::size_t, MAX_QUERY_CLASSES> classes{};
    for (int i = 0; i < argc; ++i) {
        const std::size_t index = componentClassIndex(ctx, api, argv[i]);
        if (index == NO_COMPONENT) {
            return throwAtSite(ctx, site,
                               "argument " + std::to_string(i + 1) + " is not an engine component class, got " +
                                   describeValue(ctx, argv[i], api));
        }
        classes[static_cast<std::size_t>(i)] = index;  // a repeat is checked twice, which changes nothing
    }
    if (api.world == nullptr) {
        return throwAtSite(ctx, site, "no World is bound");
    }
    return queryArray(ctx, api, std::span<const std::size_t>{classes.data(), static_cast<std::size_t>(argc)});
}

JSValue entityObjectFor(JSContext* ctx, ScriptApi& api, const EntityRef& ref) {
    const bool held = api.world != nullptr && ref.binding == api.binding;
    const std::uint64_t key = entityKeyOf(ref.entity);
    if (held) {
        if (const auto found = api.entityObjects.find(key); found != api.entityObjects.end()) {
            return JS_DupValue(ctx, found->second);  // a held object: no allocation
        }
    }
    const JSValue made = newOpaqueObject(ctx, api.entityClass, ref);
    if (JS_IsException(made)) {
        return made;
    }
    // Our class is never a Proxy, so this sets the flag, allocates nothing and answers true (quickjs.c:9426-9437).
    static_cast<void>(JS_PreventExtensions(ctx, made));
    if (held) {
        api.entityObjects.emplace(key, JS_DupValue(ctx, made));
    }
    return made;
}

void releaseEntityObjects(JSContext* ctx, ScriptApi& api) noexcept {
    for (const auto& [key, object] : api.entityObjects) {
        JS_FreeValue(ctx, object);
    }
    api.entityObjects.clear();
    api.destroyQueue.clear();
    api.destroyMarks.clear();
}

JSValue entityArray(JSContext* ctx, ScriptApi& api, const std::vector<Entity>& entities) {
    std::vector<JSValue> values;
    values.reserve(entities.size());
    for (const Entity entity : entities) {
        const JSValue object = entityObjectFor(ctx, api, EntityRef{entity, api.binding});
        if (JS_IsException(object)) {
            for (const JSValue made : values) {
                JS_FreeValue(ctx, made);
            }
            return object;
        }
        values.push_back(object);
    }
    // Takes every value, and frees them itself on its own failure.
    return JS_NewArrayFrom(ctx, static_cast<int>(values.size()), values.data());
}

JSValue childrenArray(JSContext* ctx, ScriptApi& api, Entity parent) {
    std::vector<Entity> children;
    children.reserve(api.world->childCount(parent));
    api.world->eachChild(parent, [&children](Entity child) { children.push_back(child); });  // attach order
    return entityArray(ctx, api, children);
}

JSValue findAllArray(JSContext* ctx, ScriptApi& api, std::string_view name) {
    std::vector<Entity> found;
    if (!name.empty()) {
        api.world->eachEntity([&](Entity entity) {
            if (api.world->name(entity) == name) {
                found.push_back(entity);
            }
        });
    }
    std::ranges::sort(found, {}, &Entity::index);
    return entityArray(ctx, api, found);
}

JSValue queryArray(JSContext* ctx, ScriptApi& api, std::span<const std::size_t> components) {
    std::vector<Entity> found;
    const std::span<const internal::ComponentBinding> bound = api.table->components();
    api.world->eachEntity([&](Entity entity) {
        const auto holdsAll = std::ranges::all_of(
            components, [&](std::size_t component) { return api.world->hasRaw(bound[component].type, entity); });
        if (holdsAll) {
            found.push_back(entity);
        }
    });
    std::ranges::sort(found, {}, &Entity::index);
    return entityArray(ctx, api, found);
}

std::vector<Entity> Vm::takeDestroyQueue() {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: takeDestroyQueue on an invalid Vm (a failed create or a moved-from Vm)");
        return {};
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: takeDestroyQueue called from a thread that is not the VM's; refused");
        return {};
    }
    assert(!impl->inCall);
    std::vector<Entity> taken;
    taken.swap(impl->scriptApi.destroyQueue);  // the marks stay: engine.destroy still answers false for each
    return taken;
}

bool Vm::destroyQueued(Entity entity) {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: destroyQueued on an invalid Vm (a failed create or a moved-from Vm)");
        return false;
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: destroyQueued called from a thread that is not the VM's; refused");
        return false;
    }
    assert(!impl->inCall);
    ScriptApi& api = impl->scriptApi;
    if (api.destroyMarks.erase(entityKeyOf(entity)) == 0U) {
        return false;  // never queued, already destroyed through here, or queued under an earlier binding
    }
    if (api.world == nullptr || !api.world->alive(entity)) {
        return false;  // the mark is dropped, nothing else
    }
    // The subtree, iteratively (no recursion): every member loses its table entry and its mark, so a recreated handle
    // gets a new object and a queued descendant is not destroyed twice.
    std::vector<Entity> pending{entity};
    while (!pending.empty()) {
        const Entity member = pending.back();
        pending.pop_back();
        const std::uint64_t key = entityKeyOf(member);
        if (const auto held = api.entityObjects.find(key); held != api.entityObjects.end()) {
            JS_FreeValue(impl->ctx, held->second);
            api.entityObjects.erase(held);
        }
        api.destroyMarks.erase(key);
        api.world->eachChild(member, [&pending](Entity child) { pending.push_back(child); });
    }
    static_cast<void>(api.world->destroy(entity));
    return true;
}

}  // namespace engine::script
