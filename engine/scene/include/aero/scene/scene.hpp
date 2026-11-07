#pragma once
// Aero Engine — the scene layer's umbrella header (task 1.3.1), matching <aero/rhi/rhi.hpp> and
// <aero/platform/platform.hpp>. Include this to get the whole public surface; include the individual
// headers when you want a narrower dependency.
//
// The ECS seam, where EnTT lives, is engine-internal: it is not part of this umbrella and not in an installed SDK. A
// game registers its own components through World::registerComponent<T> (world.hpp).

#include <aero/scene/animation_player.hpp>  // task 3.5.2
#include <aero/scene/audio_listener.hpp>    // task 3.7.2
#include <aero/scene/audio_source.hpp>      // task 3.7.2
#include <aero/scene/camera.hpp>            // task 1.3.3
#include <aero/scene/component.hpp>
#include <aero/scene/entity.hpp>
#include <aero/scene/environment.hpp>       // task E.2.1
#include <aero/scene/light.hpp>             // task 1.3.3
#include <aero/scene/mesh_renderer.hpp>     // task 1.4.1
#include <aero/scene/prefab.hpp>            // task 4.4.4
#include <aero/scene/script_component.hpp>  // task 4.4.1
#include <aero/scene/spot_light.hpp>        // task E.2.2
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
