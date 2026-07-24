// engine/scene/src/camera.cpp — task 1.3.3: engine::Camera's view/projection matrices. The THIRD TU
// in engine/scene. Like transform.cpp it knows both World (via worldMatrix's declaration) and a
// component; unlike transform.cpp it needs no internal seam (no registration here — that stays in
// transform.cpp's registerBuiltinComponents, D9) and never touches World's definition (viewMatrix
// only forwards its `world` reference into worldMatrix). Nothing here logs and nothing here fails.

#include <aero/core/profiler.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/transform.hpp>  // worldMatrix (declaration only; World stays incomplete here)

namespace engine {

Mat4 projectionMatrix(const Camera& camera, float aspect) {
    return perspective(camera.fovYRadians, aspect, camera.nearPlane, camera.farPlane);
}

Mat4 viewMatrix(const World& world, Entity entity) {
    AERO_PROFILE_ZONE;  // an inverse + the worldMatrix ancestor walk
    return inverse(worldMatrix(world, entity));
}

}  // namespace engine
