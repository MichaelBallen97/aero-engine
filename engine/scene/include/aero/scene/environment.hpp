#pragma once
// engine::Environment (task E.2.1): the scene's sky and its ambient light. Pure reflected data, no
// .cpp. Registered as the NINTH built-in in engine/scene/src/transform.cpp.
//
// ONE PER SCENE, RESOLVED BY THE Camera / DirectionalLight / AudioListener RULE: the lowest entity
// index wins, more than one is a latched WARN in the bridge, and ZERO IS NOT A WARN -- a world with
// none renders with this struct's own defaults, so a scene authored before this task and a scene
// with a freshly added component look the SAME. It stores no position and never consults a
// Transform; the entity that carries it needs none, and the one seedDefaultScene creates has none.
//
// THE TWO MODES ARE uint32 SELECTORS, NOT ENUMS: reflect-gen cannot reflect an enum (light.hpp's
// D1, mesh_renderer.hpp's `primitive`), a uint32 and a future `enum class : uint32` serialize
// identically, and the bridge clamps anything out of range to 0 -- which is the DEFAULT mode on both
// selectors, deliberately, so "out of range" and "the default" are one number. The enums that NAME
// the values live in engine/render/environment.hpp, the layer that consumes them; this header cannot
// include that one (scene never learns about render) and does not need to.
//
// EVERY COLOUR IS LINEAR RGB, UNCLAMPED (HDR-legal), exactly as the light colours are, and nothing
// sanitises them: a negative intensity darkens and a NaN propagates visibly, exactly as
// DirectionalLight::intensity and MaterialParams::emissiveFactor already do. Every default is a
// TUNING CONSTANT judged on the validation page; a change is a RECORDED AMENDMENT, never a silent
// constant edit -- and it must move on BOTH sides of the render boundary, which the bridge test's
// witness case enforces.
#include <aero/core/math.hpp>            // Vec3
#include <aero/reflect/annotations.hpp>  // AERO_COMPONENT, AERO_COLOR, AERO_RANGE

#include <cstdint>
#include <type_traits>

namespace engine {

struct AERO_COMPONENT Environment {
    // 0 = Sky (the three-colour gradient below), 1 = Solid (`solidColor` everywhere).
    std::uint32_t backgroundMode AERO_RANGE(0, 1) = 0;
    // `= Vec3{...}`, never a brace-initialiser directly after the annotation: the annotation's
    // canonical position is AFTER the name and BEFORE an `=` initializer (annotations.hpp:21-28),
    // and that is the only form reflect-gen's per-header cases have ever parsed.
    Vec3 skyColor AERO_COLOR = Vec3{0.16f, 0.26f, 0.48f};      // zenith
    Vec3 horizonColor AERO_COLOR = Vec3{0.52f, 0.58f, 0.68f};  // the horizon band; BACKGROUND ONLY
    Vec3 groundColor AERO_COLOR = Vec3{0.10f, 0.09f, 0.085f};  // nadir
    Vec3 solidColor AERO_COLOR = Vec3{0.06f, 0.06f, 0.07f};    // read ONLY when backgroundMode == 1;
                                                               // the editor's pre-E.2.1 clear colour
    // 0 = Hemisphere (skyColor facing up, groundColor facing down, blended by the surface normal),
    // 1 = Flat (`ambientColor` on every surface).
    std::uint32_t ambientMode AERO_RANGE(0, 1) = 0;
    Vec3 ambientColor AERO_COLOR = Vec3{0.03f, 0.03f, 0.03f};  // read ONLY when ambientMode == 1;
                                                               // the pre-E.2.1 hardcoded constant
    // Scales BOTH ambient modes -- a knob that is inert in one mode is the class of silent field this
    // project avoids. No AERO_RANGE: 1.3.3's D19 -- an HDR multiplier has no defensible upper bound,
    // and "min > 0" cannot be expressed by a two-sided bound (Camera::nearPlane carries none either).
    float ambientIntensity = 0.5f;

    bool operator==(const Environment&) const = default;
};

static_assert(std::is_trivially_copyable_v<Environment>);
static_assert(std::is_standard_layout_v<Environment>);
static_assert(std::is_aggregate_v<Environment>);
// 4 + 12 + 12 + 12 + 12 + 4 + 12 + 4 = 72: every member is 4-aligned, so there is NO padding, and
// reordering to group the two selectors would pack to 72 ANYWAY while changing JSON key order and
// Inspector row order. Declaration order is background-first then ambient, deliberately: a reader of
// the saved file sees the picture's parameters before the shade's.
static_assert(sizeof(Environment) == 18 * sizeof(float));
static_assert(alignof(Environment) == alignof(float));

}  // namespace engine
