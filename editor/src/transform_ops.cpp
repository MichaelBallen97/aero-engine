// editor/src/transform_ops.cpp — task 2.3.3: the Transform read/write seam 2.4.2 wraps.
#include <aero/core/log.hpp>
#include <aero/editor/transform_ops.hpp>
#include <aero/scene/world.hpp>

namespace engine::editor {

std::optional<Transform> readTransform(const World& world, Entity entity) {
    const auto* t = world.get<Transform>(entity);
    if (t == nullptr) {
        return std::nullopt;  // silent -- a polling path, not an error path
    }
    return *t;
}

bool writeTransform(World& world, Entity entity, const Transform& value) {
    auto* t = world.get<Transform>(entity);
    if (t == nullptr) {
        // ONE ERROR per rejection, not per cause -- naming the entity is what T4 counts.
        AERO_LOG_ERROR(
            "editor: writeTransform on a dead/null entity or one with no Transform (index {}, "
            "generation {})",
            entity.index, entity.generation);
        return false;
    }
    *t = value;
    return true;
}

}  // namespace engine::editor
