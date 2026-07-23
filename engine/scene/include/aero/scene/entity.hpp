#pragma once
// Aero Engine — engine::Entity (task 1.3.1). An entity is a generational handle, exactly like every
// other engine resource (ADR-001 mitigation #1): {index, generation}, generation 0 == null, and a
// destroyed entity's handle is REJECTED by every World operation, never dereferenced.
//
// The tag is a phantom type, DELIBERATELY NEVER DEFINED (the Handle<Job> / rhi-handle precedent). It
// is spelled EntityTag rather than the rhi convention's bare noun because the alias itself takes the
// noun: `using Entity = Handle<Entity>` would be a name clash.
//
// The backing ECS identifier is 64 bits wide (32-bit index + 32-bit version) precisely so this
// mapping is lossless and a stale handle can never alias a live one — see the World's own header and
// engine/scene/internal/aero/scene/internal/world_access.hpp for the conversion.

#include <aero/core/handle.hpp>

namespace engine {

struct EntityTag;  // never defined — a phantom type, purely to make Handle<EntityTag> a distinct type

// A live entity in a World. Stable across other entities' create/destroy; invalidated (permanently,
// for THIS handle) by destroy(), clear(), and ~World. Cheap to copy, trivially serializable.
//
// NOT interchangeable between Worlds: an Entity is only meaningful to the World that minted it.
// Passing one to another World is a caller error. It is mechanically safe — the other World answers
// as if the handle were stale — but semantically undefined, and a same-index/same-generation
// coincidence would silently address that World's own entity. Do not do it.
using Entity = Handle<EntityTag>;

}  // namespace engine
