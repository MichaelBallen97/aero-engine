#pragma once
// Aero Engine — the scene layer's umbrella header (task 1.3.1), matching <aero/rhi/rhi.hpp> and
// <aero/platform/platform.hpp>. Include this to get the whole public surface; include the individual
// headers when you want a narrower dependency.
//
// The INTERNAL seam (<aero/scene/internal/world_access.hpp>, which is where EnTT lives) is
// deliberately NOT part of this umbrella and ships through a separate target, aero::scene_internal.

#include <aero/scene/camera.hpp>  // task 1.3.3
#include <aero/scene/component.hpp>
#include <aero/scene/entity.hpp>
#include <aero/scene/light.hpp>  // task 1.3.3
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
