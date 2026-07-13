# 06 — Scope & Non-Goals

> This is the most load-bearing document in the plan. The non-goals list protects the whole project. "Similar to Unity/Godot" means **core-workflow parity** (edit → script → play → export), **not** feature parity.

---

## v1.0 — what Aero Engine IS

Core-workflow parity, 3D-first.

### Editor (macOS / Windows / Linux)
- Scene hierarchy panel
- Reflection-driven inspector (edit any reflected component's fields)
- 3D viewport with camera controls and transform gizmos (ImGuizmo)
- Undo/redo
- Asset browser
- Project system (`project.json`, create/open, per-project settings)
- Play-in-editor *(in-process in v1; separate-process play mode is a v2 item)*

### Rendering (3D)
- Physically-based rendering (PBR)
- Shadows
- Basic post-processing
- Frustum culling
- Render architecture (forward+ vs deferred) decided in Phase 8 with profiling data

### Asset pipeline
- Import: glTF, FBX (ufbx), OBJ (tinyobjloader), `.blend` via Blender CLI, plus DAE/PLY/STL via Assimp (editor only)
- AssetDatabase with stable GUIDs and `.meta` files (git-committed)
- Cooker producing per-platform cooked binaries
- Textures: stb_image import → KTX2 / Basis Universal (BCn desktop, ASTC/ETC2 mobile)
- Skeletal animation (skinning + playback)

### Scripting — pick one language per project
- **TypeScript** on quickjs-ng, with reflection-generated bindings + `.d.ts` autocomplete + hot reload, **OR**
- **Native C++** against the engine's public API + project template
- The two are **never combined in a single project**; the language is chosen at project creation. See [ADR-008](./02-adrs.md#adr-008--per-project-language-choice-and-the-two-export-models).

### Physics & audio
- Jolt (3D) · Box2D v3 (2D)
- miniaudio-backed audio graph (buses, mixers, basic 3D spatialization)

### 2D
- Sprites, tilemaps, msdf text (Phase 7 — deliberately after 3D is solid)

### Export
- 5 runtime platforms: macOS, Windows, Linux, iOS, Android
- **TypeScript projects:** instant export (cooked assets + precompiled runtime), no toolchains needed
- **C++ projects:** native build per platform (local for your own OS, CI for others)

### Cross-cutting
- Reflection-driven everything (inspector, serialization, scripting bindings, `.d.ts`)
- Handles, not pointers, for every resource
- ASan/UBSan green in CI from commit #1

---

## v1.0 — what Aero Engine IS NOT (explicit non-goals)

Each of these is a conscious door left closed. Knowing them now prevents frustration and scope creep later.

| Non-goal | Why / where it lives |
|---|---|
| ❌ Ray tracing / mesh shaders | SDL_GPU ceiling — deferred to **v3–v4** (needs sub-`rhi` native backends) |
| ❌ Nanite-style virtualized geometry | Deferred to **v3–v4** |
| ❌ Baked global illumination | Deferred to **v2** |
| ❌ Animation state-machine graph editor | Playback yes; graph editor deferred to **v2** |
| ❌ Terrain / vegetation systems | Deferred to **v2** |
| ❌ Visual / node scripting | Deferred to **v3–v4** |
| ❌ Networking / multiplayer | Not planned for v1; community plugin territory |
| ❌ Web / WASM export | Not in v1 (would argue for wgpu/Dawn over SDL_GPU) |
| ❌ Mobile **editor** | Never. The editor is desktop-only by design |
| ❌ Two scripting languages combined in one project | By design — one language per project |
| ❌ Built-in FMOD / Wwise | Proprietary; third-party plugin only (the Godot model) |
| ❌ Asset store / marketplace | Out of scope |
| ❌ Particle system | A whole subsystem the Phase 5 game doesn't need — deferred to **v2** |
| ❌ Navmesh / pathfinding / AI | Deferred to **v2** / community-plugin territory |
| ❌ In-game UI framework | v1 ships only the minimal msdf HUD-text path (5.5.3, formalized by 7.3); a real UI system is **v2** |
| ❌ Save-game framework | Games hand-roll saves with the serialization primitives; helpers deferred to **v2** |
| ❌ LOD / impostors / streaming | Deferred to **v2** |
| ❌ Crash reporter / telemetry | Deferred to **v2** (minidump-based) |
| ❌ TypeScript step-debugger | v1 has source-mapped errors (4.3.4); a DAP debugger is **v2** |
| ❌ **Feature parity with Unity/Godot** | The goal is core-workflow parity, full stop |

See [future-roadmap.md](./future-roadmap.md) for what happens to the deferred items in v2 and v3–v4.

---

## The scope discipline test

Before adding anything to v1.0, it must pass all three:

1. **Does it serve the edit → script → play → export loop?** If not, it is probably v2+.
2. **Does a real game I want to ship need it in Phase 5?** If not, defer it.
3. **Can one person build and maintain it solo without derailing the horizon?** If not, it is a plugin or a later version.

If a feature fails any of these, it goes to [future-roadmap.md](./future-roadmap.md), not into v1.
