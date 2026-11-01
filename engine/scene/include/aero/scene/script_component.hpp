#pragma once
// engine::ScriptComponent (task 4.4.1): the reflected "this entity runs a script" component. Pure
// reflected data, no .cpp. Registered as the ELEVENTH built-in in engine/scene/src/transform.cpp.
//
// It is DECLARATIVE: it names the script asset an entity runs and nothing else. Nothing here knows
// what a VM, a class or a lifecycle hook is, and engine/scene links none of them; engine/script is
// the only code that sees this component and a running instance of its script.
//
// ONE SCRIPT PER ENTITY. A second behaviour on the same entity is a second entity, as with every
// other built-in.
#include <aero/core/guid.hpp>            // Guid
#include <aero/reflect/annotations.hpp>  // AERO_COMPONENT, AERO_ASSET

#include <type_traits>

namespace engine {

struct AERO_COMPONENT ScriptComponent {
    // The script asset this entity runs, by the SOURCE asset's Guid (the `.meta` beside the `.ts`),
    // which is why the annotation says `script`. NIL == no script, which is the ORDINARY state of a
    // freshly added component and is COUNTED, never warned: a WARN would fire once per add on
    // correct behaviour (the AudioSource::clip rule verbatim).
    Guid script AERO_ASSET(script){};

    bool operator==(const ScriptComponent&) const = default;
};

static_assert(std::is_trivially_copyable_v<ScriptComponent>);
static_assert(std::is_standard_layout_v<ScriptComponent>);
static_assert(std::is_aggregate_v<ScriptComponent>);
static_assert(sizeof(ScriptComponent) == 16);
static_assert(alignof(ScriptComponent) == 8);

}  // namespace engine
