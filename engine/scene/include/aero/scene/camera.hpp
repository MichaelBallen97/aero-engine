#pragma once
// Aero Engine — engine::Camera (task 1.3.3): a reflected perspective-camera component, plus the
// view and projection matrices a renderer reads. Perspective-only by decision (D2): orthographic
// has no consumer until Phase 2/7 and arrives, if ever, as an additive OrthographicCamera.
//
// REFLECTION: every field is float (the reflectable subset), so all three consumers serialize with
// ZERO skips/warnings — pinned by reflect-gen.components_engine_camera and by the generated
// meta/JSON artifacts compiled into aero_reflect_meta_test / aero_reflect_json_test.
//
// The projection is RIGHT-HANDED, -Z forward, Y-up, clip Z in [0,1], fovY in RADIANS (ADR-005 /
// math/transform.hpp). Do NOT Y-flip for Vulkan — SDL_GPU converts behind the scenes.

#include <aero/core/math.hpp>            // Mat4, inverse, perspective, radians
#include <aero/reflect/annotations.hpp>  // AERO_COMPONENT
#include <aero/scene/entity.hpp>         // Entity (viewMatrix parameter)

#include <type_traits>

namespace engine {

// Declared, not included: viewMatrix takes a World by reference and never needs its definition
// (identical pattern to transform.hpp). MUST stay `class` to match world.hpp.
class World;

// A perspective camera's lens. The VIEW transform (where the eye is) comes from the entity's
// Transform via viewMatrix(); this component holds only the projection parameters. Plain reflected
// data — mutate freely through World::get<Camera>().
//
// nearPlane/farPlane, NOT near/far: <windows.h> #defines near/far as empty macros (D8).
struct AERO_COMPONENT Camera {
    float fovYRadians = radians(60.0f);  // vertical field of view, RADIANS (ADR-005 D6)
    float nearPlane = 0.1f;              // > 0; must be < farPlane (not validated — D11)
    float farPlane = 100.0f;

    bool operator==(const Camera&) const = default;  // EXACT (Transform's E11 rationale)
};

static_assert(std::is_trivially_copyable_v<Camera>);
static_assert(std::is_standard_layout_v<Camera>);
static_assert(std::is_aggregate_v<Camera>);
static_assert(sizeof(Camera) == 3 * sizeof(float));  // no padding

// perspective(fovYRadians, aspect, nearPlane, farPlane). aspect (width/height) is supplied by the
// viewport each frame, never stored (D4). Degenerate params (near>=far, aspect<=0, fov out of
// (0, pi)) are the math layer's contract — a degenerate matrix, never a crash, never re-validated
// here (D11), exactly as localMatrix never validates a Transform.
[[nodiscard]] Mat4 projectionMatrix(const Camera& camera, float aspect);

// inverse(worldMatrix(world, entity)) — the view matrix of the entity treated as a camera. Depends
// only on the hierarchy, NOT on any Camera component (D5): the view transform is about where the
// eye IS. A dead/null/never-transformed entity, and a moved-from World, yield worldMatrix ==
// identity, so viewMatrix == identity, silently (a polling path, not an error path — matches
// worldMatrix). A zero-scale camera makes worldMatrix singular and inverse() produces inf/NaN: out
// of contract (a camera should not be zero-scaled).
[[nodiscard]] Mat4 viewMatrix(const World& world, Entity entity);

}  // namespace engine
