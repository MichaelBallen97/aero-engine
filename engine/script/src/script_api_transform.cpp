// Aero Engine -- the one script-layer source that names the Transform component (task 4.4.2): spawn's identity
// Transform and, from the helpers on, the transform helpers. BN30 holds every other source to the table.

#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>

#include "script_api.hpp"

namespace engine::script {

ComponentTypeId transformTypeId() noexcept { return componentTypeId<Transform>(); }

bool addIdentityTransform(World& world, Entity entity) { return world.add<Transform>(entity, Transform{}) != nullptr; }

}  // namespace engine::script
