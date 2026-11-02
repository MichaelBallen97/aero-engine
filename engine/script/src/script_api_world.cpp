// Aero Engine -- the World side of the script API (task 4.4.2): the entity table, which makes one engine.Entity object
// per entity per binding. script_api.hpp states the rules every host function keeps; the ones that shape this file are
// 5 (an object is made with JS_NewObjectClass, never by setting properties) and 8 (JS_EXCEPTION only with an exception
// pending).
//
// The table holds JS references (HELD): releaseEntityObjects frees them, and every bindWorld, unbindWorld and
// releaseScriptApi calls it, so no object outlives the binding that made it in the table. An object a script kept
// stays alive through its own reference and reads `alive` false, because its ref names an earlier binding.

#include "script_api.hpp"

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

}  // namespace engine::script
