#pragma once
// Aero Engine — rhi umbrella (task 0.4.1). Include this to speak the engine's rendering vocabulary
// (ADR-002). The layer above (render, task 0.5.x) and samples consume exactly this; nothing below
// SDL_GPU ever crosses it.

#include <aero/rhi/descriptors.hpp>
#include <aero/rhi/device.hpp>
#include <aero/rhi/format.hpp>
#include <aero/rhi/handles.hpp>
#include <aero/rhi/types.hpp>
