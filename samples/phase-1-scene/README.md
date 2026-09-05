# phase-1-scene — task 1.4.2's deliverable, and what task E.2.1 changed under it

The Phase 1 gate artifact: a committed `scene.json` loaded from disk through the VFS into a fresh
`World` and drawn every frame by `scene_render::SceneRenderer` — the whole Phase 1 stack (reflect →
scene → scene_serialize → scene_render) driven end to end from a file. The World is never mutated
after load; "it's live" is proven by a resize (the aspect tracks) and a stable fps readout, not by
motion.

## Since task E.2.1: it renders under the engine's default environment

This sample's own code did not change. It is the one sample that drives
`scene_render::SceneRenderer`, so it follows the engine: since E.2.1 that renderer owns a
`render::SkyPass` and records it between the shadow pass and the opaque pass, and the ambient term
is the environment's hemispheric sky/ground blend rather than the small fixed constant it used to
be.

`scene.json` carries **no** `engine::Environment` component, and that is the point worth keeping:
this is exactly the "a scene authored before this task still loads, and still renders under a sky"
case. Zero `Environment`s in a World is not a diagnostic — it is the ordinary state of every scene
authored before E.2.1, and it resolves to `render::EnvironmentData`'s own defaults.

**The clear colour is still `{0.05, 0.06, 0.08}` and is still correct.** It is simply no longer
visible anywhere the sky covers, which is everywhere the camera looks: the sky writes no depth, so
the geometry that follows overdraws it. If you want the pre-E.2.1 picture back for a comparison,
there is deliberately no flag for it — build the branch point instead.

## Running it

Needs **both** `AERO_REFLECT_TOOLS` (the generated component serializers) and `AERO_SHADER_TOOLS`
(the cooked `scene.{vert,frag}` plus, since E.2.1, `sky.{vert,frag}`). With either OFF this compiles
a stub `main` that logs and returns 1.

```bash
cmake --build --preset macos-debug --target aero_sample_phase1_scene
./build/macos-debug/samples/phase-1-scene/aero_sample_phase1_scene
```

Escape or the window close button quits. CI builds this on all three platforms as a compile-proof;
the visual pass is local — record it in `samples/phase-1-scene/VALIDATION.md`, whose rows describe
Phase 1's gate and are **not** rewritten by this task.
