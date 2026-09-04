#pragma once
// Aero Engine — the render layer's umbrella header (task 1.4.1), matching <aero/rhi/rhi.hpp> and
// <aero/scene/scene.hpp>. Include this to get the whole public surface; include the individual
// headers when you want a narrower dependency. renderer.hpp (task 0.5.1/0.5.2) is otherwise frozen —
// the four new headers below are purely additive, except D2's two-line RenderTarget friend
// exception recorded in renderer.hpp itself (task 2.2.3).

#include <aero/render/animation.hpp>         // task 3.5.2
#include <aero/render/culling.hpp>           // task 3.6.1
#include <aero/render/debug_draw.hpp>        // task E.1.1
#include <aero/render/debug_grid.hpp>        // task E.1.2
#include <aero/render/forward_renderer.hpp>  // task 1.4.1
#include <aero/render/lighting.hpp>          // task 1.4.1
#include <aero/render/material.hpp>          // task 3.4.1
#include <aero/render/mesh.hpp>              // task 1.4.1
#include <aero/render/post_process.hpp>      // task 3.6.3
#include <aero/render/render_target.hpp>     // task 2.2.3
#include <aero/render/renderer.hpp>
#include <aero/render/shadow.hpp>          // task 3.6.2
#include <aero/render/skinning.hpp>        // task 3.5.1
#include <aero/render/texture_upload.hpp>  // task 3.4.1
#include <aero/render/tonemap.hpp>         // task 3.6.3
