#pragma once
// Aero Engine — the render layer's umbrella header (task 1.4.1), matching <aero/rhi/rhi.hpp> and
// <aero/scene/scene.hpp>. Include this to get the whole public surface; include the individual
// headers when you want a narrower dependency. renderer.hpp (task 0.5.1/0.5.2) is frozen — the three
// new headers below are purely additive.

#include <aero/render/forward_renderer.hpp>  // task 1.4.1
#include <aero/render/lighting.hpp>          // task 1.4.1
#include <aero/render/mesh.hpp>              // task 1.4.1
#include <aero/render/renderer.hpp>
