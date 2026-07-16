#pragma once
// Aero Engine — engine::platform umbrella (task 0.3.1). Pulls in the whole SDL3-wrapper surface:
// Context (SDL lifetime + event pump), Window (RAII), and the Event vocabulary. Include this from an
// app/runtime that wants "the platform layer"; include the individual headers for less.
#include <aero/platform/context.hpp>
#include <aero/platform/event.hpp>
#include <aero/platform/window.hpp>
