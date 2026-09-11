#pragma once
// Aero Engine -- the material preview's rig (task E.2.4). PUBLIC and PURE: no ImGui, no EnTT, no rhi
// type, no World, no allocation, no logging, nothing that can throw.
//
// WHAT THE PREVIEW DRAWS IS A UNIT SPHERE AT THE WORLD ORIGIN UNDER THE OPEN SCENE'S LIGHTING, seen
// from a camera that orbits it -- so the "rig" is the CAMERA and nothing else. The environment and the
// sun are the SCENE's, resolved by the bridge's own resolvers (scene_render::resolveEnvironment /
// resolveDirectionalLight, which buildRenderView itself calls) and handed in here as render types.
// This header never learns where its lighting came from, which is what lets E.4.5's thumbnail producer
// feed it whatever a thumbnail wants at whatever orbit angle it likes.
//
// EVERY NUMBER BELOW IS A TUNING CONSTANT judged on the validation page. Tier 0 asserts RELATIONSHIPS
// between them -- the eye is outside the unit sphere, the near plane is closer than the sphere's
// nearest point, the far plane is beyond its farthest, the sphere fits the vertical field of view --
// never a magnitude, so a retune reddens nothing.
//
// libm reached: std::cos and std::sin (the eye), std::isfinite and std::fmod (the orbit), plus
// whatever engine::perspective and engine::lookAt reach (std::tan). NO BIT-DETERMINISM CLAIM IS MADE
// for the camera -- light_gizmo.hpp's posture, for its reason: neither sin nor cos nor tan is required
// by IEEE-754 to be correctly rounded. Assertions over the camera use an epsilon EXCEPT where both
// sides come from ONE computation (PV3 compares against engine::lookAt / engine::perspective called
// with the same arguments, which is exact by construction, not by rounding).

#include <aero/core/math.hpp>
#include <aero/render/lighting.hpp>  // CameraView, DirectionalLightData, EnvironmentData, RenderView
#include <aero/render/mesh.hpp>      // MeshInstance

#include <span>

namespace engine::editor {

struct MaterialPreviewRig {
    // The framing, COPIED from samples/phase-3-materials/main.cpp at 3.4.2 rather than re-derived, and
    // MOVED here from material_preview.hpp: a unit sphere at the origin, comfortably framed, turning
    // slowly enough that GGX highlights are judgeable by eye as they sweep.
    float orbitRadius = 3.0F;  // eye distance from the Y axis; MUST exceed 1, the sphere's radius
    float orbitHeight = 1.2F;  // eye height; the camera looks slightly DOWN at the sphere
    float orbitSpeed = 0.35F;  // radians per second
    float fovYDegrees = 60.0F;
    float nearPlane = 0.1F;
    float farPlane = 100.0F;
    [[nodiscard]] bool operator==(const MaterialPreviewRig&) const = default;
};

inline constexpr MaterialPreviewRig DEFAULT_MATERIAL_PREVIEW_RIG{};

// What the SCENE contributes, as the bridge resolved it. `hasSun` is the RESOLUTION's answer -- an
// entity carries a DirectionalLight -- and NOT `sun.intensity != 0`: a sun the user set to 0 is a sun
// they switched off, and the panel's notice must not claim there is none.
struct MaterialPreviewLighting {
    render::EnvironmentData environment{};
    render::DirectionalLightData sun{};  // intensity 0 == none, the bridge's own encoding
    bool hasSun = false;
};

// TOTAL. The orbit angle after `deltaSeconds`, kept in [0, TWO_PI). A non-finite or NEGATIVE delta, or
// a step that would leave the angle non-finite, returns `angle` UNCHANGED -- never a NaN this function
// made: a NaN angle is a NaN eye, and packSkyCamera would then refuse the sky while the forward pass
// still drew from a NaN view. PanelContext::deltaSeconds is finite and spike-clamped upstream; this is
// the function's OWN promise, not a reliance on that.
//
// A garbage `angle` IN stays garbage OUT, deliberately: this function refuses to MANUFACTURE a NaN, it
// does not sanitise its caller's state.
//
// THE WRAP IS std::fmod AND IT IS BIT-IDENTICAL TO THE `-= TWO_PI` IT REPLACES for every reachable
// input: for TWO_PI <= next < 2*TWO_PI, Sterbenz makes `next - TWO_PI` exact and fmod returns exactly
// that. fmod only adds totality for a single huge step. Do not "simplify" it back.
[[nodiscard]] float advanceMaterialPreviewOrbit(float angle, float deltaSeconds,
                                                const MaterialPreviewRig& rig) noexcept;

// The orbit camera at `orbitAngle`: eye = (R cos a, H, R sin a), looking at the origin, +Y up --
// exactly the sample's and 3.4.2's framing. `aspect` is width / height of the DRAWN extent; a
// non-positive or non-finite aspect takes 1.0 through the negated-`>` idiom, so a NaN takes the
// fallback rather than propagating (the containsHalfOpen rule, and 3.7.2's std::clamp(NaN) lesson).
[[nodiscard]] render::CameraView materialPreviewCamera(const MaterialPreviewRig& rig, float orbitAngle,
                                                       float aspect) noexcept;

// The whole view. `camera` verbatim; `lighting.sun` and `lighting.environment` VERBATIM -- no
// re-normalisation, no clamp: they are the bridge's numbers and must stay them, which is what makes
// the parity A/B exact rather than approximate. `instances` is BORROWED under RenderView's span rule,
// and EVERY instance's mvp is FILLED HERE as
//
//     (camera.proj * camera.view) * instance.model
//
// -- that association, spelled the way scene_renderer.cpp spells it, is what makes the preview's bytes
// EQUAL the bridge's; `proj * (view * model)` agrees only by rounding (E.2.2's fl(1e-4)^2 species).
// RenderView's own trap note warns that a hand-built view which fills mvp without assigning `camera`
// culls against extractFrustum(identity); this assigns BOTH, from one source, in one place, so
// cullingEnabled stays true and its contract holds by construction.
//
// Shadows OFF (the preview lights a single sphere with no caster and no receiver), stated ON THE VIEW
// rather than implied by the renderer's shadowMapResolution == 0 -- either alone yields ShadowView{}
// (forward_renderer.cpp's six opt-outs), and saying it here is what makes the opt-out readable.
// hasCamera true and points/spots EMPTY, both by RenderView's own defaults.
[[nodiscard]] render::RenderView materialPreviewView(const render::CameraView& camera,
                                                     const MaterialPreviewLighting& lighting,
                                                     std::span<render::MeshInstance> instances) noexcept;

}  // namespace engine::editor
