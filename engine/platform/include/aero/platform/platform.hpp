#pragma once
// Aero Engine — engine::platform umbrella (task 0.3.1; input added 0.3.2; audio added 0.3.3). Pulls in
// the whole SDL3-wrapper surface: Context (SDL lifetime + event pump + input), Window (RAII), the Event
// vocabulary, InputState, and a silent audio device. Include this from an app/runtime that wants "the
// platform layer"; include the individual headers for less.
#include <aero/platform/audio.hpp>  // task 0.3.3
#include <aero/platform/context.hpp>
#include <aero/platform/event.hpp>
#include <aero/platform/input.hpp>  // task 0.3.2
#include <aero/platform/window.hpp>
