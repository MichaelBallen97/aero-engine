// engine/scene/src/transform.cpp — task 1.3.2: engine::Transform's matrix functions and the built-in
// component registration hook. The SECOND TU in engine/scene, and the only file besides world.cpp
// that reaches the internal registration seam (world_access.hpp names 1.3.2 as a legitimate
// includer). It is also the only file that knows BOTH World and Transform: World itself stays
// deliberately component-agnostic.
//
// Nothing here logs and nothing here fails (D7/D9). A dead or null entity, a moved-from World, and an
// entity or ancestor with no Transform all resolve to an identity contribution, silently. The
// ancestor walk is a plain LOOP (misc-no-recursion is live) and needs no per-level storage.

#include <aero/core/profiler.hpp>
#include <aero/scene/internal/world_access.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>

namespace engine {
namespace {

// The entity's own local matrix, or identity when it carries no Transform — the docs/09
// component-less-parented-entity case: it attaches at its parent's origin.
[[nodiscard]] Mat4 localMatrixOf(const World& world, Entity entity) {
    const auto* transform = world.get<Transform>(entity);
    return transform == nullptr ? Mat4::identity() : localMatrix(*transform);
}

}  // namespace

Mat4 localMatrix(const Transform& transform) {
    return compose(Trs{transform.position, transform.rotation, transform.scale});
}

Mat4 worldMatrix(const World& world, Entity entity) {
    AERO_PROFILE_ZONE;
    Mat4 result = localMatrixOf(world, entity);
    // Left-multiply one ancestor per step, walking UP: world = M_root * ... * M_parent * M_local.
    // The subtree-destroy invariant guarantees every stored parent is live, so parent() is the only
    // query needed and the walk always terminates at a root (the graph is a forest by construction).
    for (Entity ancestor = world.parent(entity); ancestor.valid(); ancestor = world.parent(ancestor)) {
        result = localMatrixOf(world, ancestor) * result;
    }
    return result;
}

namespace scene::detail {

void registerBuiltinComponents(World& world) {
    scene::internal::registerComponent<Transform>(world, "engine::Transform");
}

}  // namespace scene::detail
}  // namespace engine
