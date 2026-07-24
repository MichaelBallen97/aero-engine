# Phase 1 gate ledger — `samples/phase-1-scene`

The Phase 1 gate (`docs/tasks/phase-1.md`) reads: *"Define a component → save/load a scene as JSON →
it renders."* CI proves the "builds clean on all 3 OSes" half (it configures, builds, and unit-tests
this sample's engine dependencies — including the generated `aero::scene_serialize` component
serializers and the cooked `scene.{vert,frag}` shaders — on macOS, Windows, and Ubuntu on every push).
It **cannot** prove the "it actually renders the tableau, and stays correct across a resize" half: CI
has no attached display on any lane (macOS/Windows CI runners have no monitor; the Linux lane's GPU is
`lavapipe`, a software Vulkan rasterizer with no real display surface). The only way to prove that is a
human running the visible sample on real hardware and watching it — recorded here per OS, mirroring
`samples/phase-0-cube/VALIDATION.md`'s precedent.

Unlike the Phase 0 gate, there is **no fps floor to clear here** — the scene is static (D2: "it's live"
is proven by resize + a stable fps readout, not motion), so a row counts as ✅ once the scene visibly
loads, renders correctly, and survives a resize with the aspect held.

## How to validate one OS

1. **Build tools-ON** (default): `cmake --preset <os>-release && cmake --build --preset <os>-release`
   (needs both `AERO_REFLECT_TOOLS=ON` — the generated component serializers — and
   `AERO_SHADER_TOOLS=ON` — the cooked `scene.{vert,frag}` shaders).
2. **Run it**: launch `aero_sample_phase1_scene` from that preset's build tree. Watch the log line
   `scene: 6 entities, N components (0 skipped, 0 bad-field)` — confirms the load succeeded end to end.
3. **Look at the window**: a grey ground plane, a warm-red cube, and a cool-blue sphere, lit by one
   directional light (the "sun") and one point light (the "lamp," visible as a soft highlight pool on
   the ground). The camera is static; nothing in the scene moves.
4. **Drag-resize** the window: the cube must stay a cube (no stretch) and the sphere must stay round —
   the projection aspect tracks the live window size every frame.
5. **Press Escape** (or close the window). The sample logs a closing summary
   (`closing after <N> frames, <T>s`).
6. **Record the row below**: date, machine/GPU, and a PASS/FAIL verdict for "loads + renders correctly"
   and "resize holds aspect."

## Validation table

| OS | Status | Date | Machine / GPU | Entities loaded | Renders correctly | Resize holds aspect | notes |
|----|--------|------|----------------|------------------|--------------------|----------------------|-------|
| macOS | ✅ validated | 2026-07-24 | MacBook Pro (Apple M1 Pro), Metal backend | 6 (12 components, 0 skipped, 0 bad-field) | PASS | PASS | Window title/log confirm vsync-locked ~60 fps while focused (an unfocused/occluded window free-runs well above 60 — the same known macOS quirk 0.5.3's ledger already documents, not a rendering bug); resized 1280x720 → 900x500 live, cube/sphere stayed correctly proportioned; ground/cube/sphere colors distinguishable and the lamp's highlight pool visible on the ground. Light intensities were tuned down from the spec table's starting point (0.9/1.0 vs. the original 2.0/6.0) because `scene.frag.hlsl` has no tonemapping and the original values blew every surface out to flat white — see `tests/scene_serialize_test.cpp`'s `buildTableau` comment. |
| Windows | ⏳ pending | — | — | — | — | — | needs native run (D3D12) |
| Linux | ⏳ pending | — | — | — | — | — | needs native run (Vulkan; NOT lavapipe/CI) |

**Phase 1 gate status: OPEN** — closes only when all three rows above are ✅ (mirrors the Phase 0
gate's code-free Windows/Linux follow-up, 0.5.3).
