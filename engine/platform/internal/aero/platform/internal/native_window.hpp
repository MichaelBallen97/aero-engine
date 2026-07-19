#pragma once
// Aero Engine — INTERNAL platform seam (task 0.4.2). NOT a public header: this directory is not in
// aero_platform's PUBLIC include path; it ships through the aero::platform_internal INTERFACE
// target, consumed PRIVATE and ONLY by aero_rhi's backend TU. It exists so engine::rhi::Device can
// claim a Window for its swapchain (SDL_ClaimWindowForGPUDevice needs the SDL_Window*) without any
// SDL type crossing a public engine header (project rule #3). Deliberately outside the
// engine/*/include/* boundary-scan glob, so check-platform-boundary.sh's public-header scan does
// not flag the SDL_Window forward-decl below. check-rhi-boundary.sh (task 0.4.5) DOES scan this
// file (it scans engine/ wide, not just public headers) and passes: SDL_Window is not an SDL_GPU
// token, and SDL_ClaimWindowForGPUDevice appears only in the comment above.
// Adding ANY other accessor here requires a spec-level decision — the seam stays one window wide.

struct SDL_Window;  // matches SDL_video.h's `typedef struct SDL_Window SDL_Window;`

namespace engine::platform {
class Window;
}

namespace engine::platform::internal {

// Befriended by Window. The Window must be live (not moved-from) — caller contract, same as every
// other Window member.
struct NativeWindowAccessor {
    [[nodiscard]] static SDL_Window* get(const Window& window) noexcept;
};

}  // namespace engine::platform::internal
