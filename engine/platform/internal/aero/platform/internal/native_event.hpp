#pragma once
// Aero Engine — INTERNAL platform seam (task 2.1.1). Lets a privileged consumer OBSERVE the raw
// backend event stream that Context::pollEvent otherwise translates-and-discards, so the editor's
// ImGui SDL3 backend sees EVERY event (including the text-input/gamepad events pollEvent drops).
// Void-based by design: no SDL type crosses even this internal header — and certainly not the
// public context.hpp (the platform boundary rule). `sdlEvent` IS a `const SDL_Event*`; the consumer
// casts. Context keeps owning the single global pump and the input fold; this only tees a copy of
// each raw event to one observer. Pass {nullptr,nullptr} to clear (the editor does so before ImGui
// shutdown, so the sink never outlives ImGui).

namespace engine::platform {
class Context;
namespace internal {

struct RawEventAccessor {
    static void setRawEventSink(Context& ctx, void (*sink)(void* user, const void* sdlEvent), void* user) noexcept;
};

}  // namespace internal
}  // namespace engine::platform
