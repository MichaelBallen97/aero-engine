#include <aero/editor/material_preview_rig.hpp>

#include <cmath>

namespace engine::editor {

float advanceMaterialPreviewOrbit(float angle, float deltaSeconds, const MaterialPreviewRig& rig) noexcept {
    // NEGATED `>=` so a NaN delta takes the REFUSAL. 2.3.2's A10 idiom, and E.2.3's lesson 2 beside it:
    // this is a REFUSAL, so `!(d >= 0)` is the NaN-safe spelling. The ACCEPTANCE form of the same rule
    // would be `d >= 0`, and `!(d < 0)` -- which reads like the same thing -- ACCEPTS a NaN.
    if (!(deltaSeconds >= 0.0F)) {
        return angle;
    }
    const float next = angle + (deltaSeconds * rig.orbitSpeed);
    if (!std::isfinite(next)) {
        return angle;  // an infinite or NaN step leaves the orbit where it was
    }
    // fmod rather than one subtraction, so a single huge step still lands in range. For the ordinary
    // case (next < TWO_PI) this returns `next` untouched, which is bit-identical to the pre-E.2.4
    // `orbitAngle += ...` it replaces.
    return next < TWO_PI ? next : std::fmod(next, TWO_PI);
}

render::CameraView materialPreviewCamera(const MaterialPreviewRig& rig, float orbitAngle, float aspect) noexcept {
    // NEGATED `>` so a NaN aspect takes the fallback (3.7.2's std::clamp(NaN) lesson, applied to a
    // guard rather than to a clamp).
    const float safeAspect = (aspect > 0.0F) && std::isfinite(aspect) ? aspect : 1.0F;
    const Vec3 eye{rig.orbitRadius * std::cos(orbitAngle), rig.orbitHeight, rig.orbitRadius * std::sin(orbitAngle)};
    return {lookAt(eye, Vec3::zero(), Vec3{0.0F, 1.0F, 0.0F}),
            perspective(radians(rig.fovYDegrees), safeAspect, rig.nearPlane, rig.farPlane), eye};
}

render::RenderView materialPreviewView(const render::CameraView& camera, const MaterialPreviewLighting& lighting,
                                       std::span<render::MeshInstance> instances) noexcept {
    // DESIGNATED, IN DECLARATION ORDER -- camera, directional, (points, spots), environment, instances,
    // ..., shadowsEnabled. C++20 requires designators to follow declaration order; clang accepts a
    // wrong order with -Wreorder-init-list (a WARNING) while GCC and MSVC REJECT, so a reordering here
    // is green locally and a compile failure on two lanes (E.2.2's lesson 3).
    //
    // `view.instances` is assigned from the SAME span the loop below mutates, and the mutation happens
    // AFTER the assignment. That is safe -- a span is a view, so writing through `instances` writes
    // through `view.instances` -- and it is why the loop can come last without a second assignment.
    // Do not "fix" the order.
    //
    // hasCamera and cullingEnabled are deliberately NOT restated: both default to true on RenderView,
    // and restating them would make a future default change invisible here.
    render::RenderView view{.camera = camera,
                            .directional = lighting.sun,
                            .environment = lighting.environment,
                            .instances = instances,
                            .shadowsEnabled = false};
    // ONE composition, reused per instance -- and spelled as (proj * view) * model, which is
    // buildRenderView's own association. Never proj * (view * model).
    const Mat4 viewProj = camera.proj * camera.view;
    for (render::MeshInstance& instance : instances) {
        instance.mvp = viewProj * instance.model;
    }
    return view;
}

}  // namespace engine::editor
