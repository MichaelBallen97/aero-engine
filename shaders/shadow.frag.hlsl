// Task 3.6.2 — the fragment stage SDL requires and nothing uses. SDL_CreateGPUGraphicsPipeline's
// "Fragment shader cannot be NULL!" (SDL_gpu.c:1041) is UNCONDITIONAL — there is no depth-only
// pipeline without one — so this exists to be that, and to cost nothing: PROBED on the pinned
// toolchain at samplerCount 0 / uniformBufferCount 0, compiling to all three formats, with an MSL
// body of `fragment void main0() {}`.
//
// It writes no colour because the pass has no colour attachment, and it must NOT discard: a
// depth-only stage with no UVs and no material bind cannot evaluate an alpha mask, which is exactly
// why alpha-masked casters are out of scope here (they cast a solid silhouette, with a latched WARN
// so the gap is discoverable at the moment it matters). 8.2.1 owns the fix.
void main() {}
