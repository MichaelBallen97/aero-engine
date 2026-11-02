// Aero Engine -- the World side of the script API (task 4.4.2): the entity table, which makes one engine.Entity object
// per entity per binding. script_api.hpp states the rules every host function keeps; the ones that shape this file are
// 5 (an object is made with JS_NewObjectClass, never by setting properties) and 8 (JS_EXCEPTION only with an exception
// pending).
//
// The table holds JS references (HELD): releaseEntityObjects frees them, and every bindWorld, unbindWorld and
// releaseScriptApi calls it, so no object outlives the binding that made it in the table. An object a script kept
// stays alive through its own reference and reads `alive` false, because its ref names an earlier binding.

#include <aero/scene/world.hpp>

#include "script_api.hpp"

#include <vector>

namespace engine::script {

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

}  // namespace engine::script
