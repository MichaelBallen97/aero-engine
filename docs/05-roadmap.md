# 05 — Roadmap & Phases

> 9 phases (0–8). Every phase ends in a **usable deliverable gate** (project rule #2). Durations are part-time solo estimates — planning aids, not commitments — and sum to the ~20–32 month horizon.

---

## Sequencing approach

Three ways to sequence a solo engine build were considered:

- **A — Foundation-first / layer-by-layer:** build each subsystem solidly bottom-up. Solid, but long stretches with nothing usable — fights project rule #2.
- **B — Vertical-slice-first:** force a thin end-to-end playable thing ASAP, then deepen. Great motivation, but more rework.
- **C — Milestone-driven hybrid (chosen):** foundation-first *only* for the non-negotiable spine (core → RHI → reflection), then every phase after ends in a genuinely usable deliverable, pulling an end-to-end slice forward as early as the editor. This is project rule #2, formalized.

---

## Phase overview

| # | Phase | Est. | Deliverable gate |
|---|---|:--:|---|
| **0** | Foundations & First Triangle | 2–3 mo | Spinning textured cube @60fps on mac/win/linux; CI green (ASan/UBSan) from commit #1 |
| **1** | Reflection, ECS & Serialization | 2–3 mo | Define a component → save/load a scene as JSON → it renders |
| **2** | The Editor Shell | 2–3 mo | Create a project; create / move / edit / save entities visually |
| **3** | Asset Pipeline & 3D Content | 3–5 mo | Drop a rigged glTF/FBX in → PBR materials + shadows + a playing animation + an audible sound |
| **4** | Scripting (language fork) | 3–5 mo | Press Play in the editor: hot-reloaded TS or native C++ drives entities; press Stop: the scene restores exactly |
| **5** | Export & **Ship a Game** 🎯 | 2–3 mo | A downloadable, playable 3D game made entirely in Aero Engine (desktop) |
| **6** | Mobile Runtime & Audio Depth | 2–3 mo | The Phase 5 game running on a phone; audio buses/mixers |
| **7** | 2D Support | 2–3 mo | A working 2D scene: Box2D physics + sprites + text |
| **8** | Rendering Maturity & v1.0 | 2–4 mo | Tagged v1.0 release, documented, with sample projects |

**Total: ~20–32 months**; budget to ~34 with slippage.

> **Note on ordering vs. the original draft:** "ship a game" was moved from Phase 4 to **Phase 5**, because you cannot ship before both scripting (Phase 4) and export (Phase 5) exist. 2D was deliberately placed at Phase 7, after the 3D foundation is proven.
>
> **Note on the July 2026 re-plan:** play-in-editor was placed in Phase 4 (epic 4.7); basic shadows/culling (3.6) and audio playback (3.7) were pulled forward into Phase 3 so the Phase 5 game ships with sound and shadows; Phase 2 gained the project system (2.6). Full task depth for every phase now lives in [docs/tasks/](./tasks/) — see [07 — Tasks](./07-tasks.md).

---

## Phase 0 — Foundations & First Triangle · *2–3 mo*

**Goal:** prove the toolchain, build, CI, and the RHI abstraction all work end-to-end. This phase is *also* the C++ learning budget — treat it as such.

**Epics:**
- **0.1 Build & CI bootstrap** — CMake + presets + vcpkg manifest; GitHub Actions ×3 OS; ASan/UBSan; doctest; Tracy wired
- **0.2 `core`** — `Handle<T>`, math types (GLM backend) + the CI test that fails if `glm` leaks past `core/math`, spdlog logging, enkiTS jobs, time, VFS
- **0.3 `platform`** — SDL3 window, input, audio device
- **0.4 `rhi` + `tools/shaderc`** — SDL_GPU wrapper; HLSL → SDL_shadercross pipeline
- **0.5 `render` v0** — swapchain, clear, upload + draw a textured cube

**Gate:** Spinning textured cube @60fps on mac/win/linux; CI green (ASan/UBSan) from commit #1.

---

## Phase 1 — Reflection, ECS & Serialization · *2–3 mo*

**Goal:** build the reflection/serialization spine — the backbone four systems will consume — *before* anything depends on it the wrong way (per [ADR-004](./02-adrs.md#adr-004--reflection-by-code-generation)).

**Epics:**
- **1.1 `reflect-gen` v0** — libclang parses `[[engine::component]]`; emits `entt::meta` registration. **Minimal subset only:** plain structs + primitives + `Vec3`/`Quat`
- **1.2 Serialization** — generated JSON read/write (binary deferred to the cooker)
- **1.3 `scene`** — EnTT world, Transform hierarchy, Camera + Light components
- **1.4 Render-a-scene** — load a JSON scene → draw it

**Gate:** Define a component → save/load a scene as JSON → it renders.

---

## Phase 2 — The Editor Shell · *2–3 mo*

**Goal:** first genuinely *usable* tool; validate the golden rule and the reflection → inspector path.

**Epics:**
- **2.1 ImGui integration** — docking, editor-only, with the golden-rule CI test enforced
- **2.2 Core panels** — hierarchy, reflection-driven inspector, viewport render target, asset browser stub
- **2.3 Manipulation** — editor camera, ImGuizmo transform gizmos, selection
- **2.4 Undo/redo** — command pattern
- **2.5 Scene I/O** — save/load scenes from the editor
- **2.6 Project system v0** — `project.json`, create/open project, settings stub — the unit that owns assets and, from 4.6, the language choice *(added July 2026)*

**Gate:** Create a project; create / move / edit / save entities visually.

---

## Phase 3 — Asset Pipeline & 3D Content · *3–5 mo (may split 3a/3b)*

**Goal:** real content in the engine — models, materials, animation.

**Epics:**
- **3.1 AssetDatabase** — GUIDs, `.meta` files, import cache
- **3.2 Importers** — glTF (fastgltf), FBX (ufbx), OBJ (tinyobjloader), Blender-CLI detection → glTF; Assimp editor-only for DAE/PLY/STL
- **3.3 Cooker v0** — mesh → GPU buffers; textures stb → KTX2/Basis
- **3.4 PBR materials** — materials + textures in the renderer
- **3.5 Skeletal animation** — skinning + playback
- **3.6 Rendering essentials** — frustum culling, basic directional shadow map, tonemap/gamma pass; the quality pass lands in 8.2 *(added July 2026)*
- **3.7 Audio playback v0** — clip assets, play/stop/volume, basic 3D panning, `AudioSource`/`AudioListener`; buses/effects land in 6.4 *(added July 2026)*

**Gate:** Drop a rigged glTF/FBX in → PBR materials + shadows + a playing animation + an audible sound.

---

## Phase 4 — Scripting (the language fork) · *3–5 mo*

**Goal:** gameplay authoring — the thing that makes it an engine you can build a game with. The per-project language choice ([ADR-008](./02-adrs.md#adr-008--per-project-language-choice-and-the-two-export-models)) lands here.

**Epics:**
- **4.1 quickjs-ng embed** — VM lifecycle, module loading
- **4.2 `reflect-gen` bindings target** — emit quickjs-ng bindings + `.d.ts`
- **4.3 TS pipeline** — esbuild/swc import → bytecode; hot reload
- **4.4 Script components & entity API** — TS drives entities
- **4.5 C++ project mode** — public engine API surface + project template so a game is authored in native C++ (this already works internally from Phase 1; here it is productized)
- **4.6 Language-choice project setting** — chosen at project creation
- **4.7 Play-in-editor** — play/stop toolbar, scene snapshot & exact restore, autosave-before-play, script lifecycle bound to play mode *(added July 2026)*

**Gate:** Press Play in the editor: hot-reloaded TS or native C++ drives entities; press Stop: the scene restores exactly.

---

## Phase 5 — Export & Ship a Game 🎯 · *2–3 mo*

**Goal:** the project's validation milestone. If we reach it, the engine exists.

**Epics:**
- **5.1 Packager** — `.pak` format
- **5.2 Per-platform desktop runtimes** — precompiled in CI (mac/win/linux)
- **5.3 TS export** — instant: pack cooked assets + precompiled runtime
- **5.4 C++ export** — native build + package pipeline (desktop)
- **5.5 The game** — design, build, and ship ONE small real 3D game to desktop

**Gate:** A downloadable, playable 3D game made entirely in Aero Engine (desktop). **This is the milestone that proves the engine exists.**

---

## Phase 6 — Mobile Runtime & Audio Depth · *2–3 mo*

**Goal:** complete the 5-platform runtime matrix; deepen audio.

**Epics:**
- **6.1 iOS runtime** — Metal backend, no-JIT quickjs-ng (iOS-safe by design), signing on Mac
- **6.2 Android runtime** — Vulkan backend, NDK, AAudio via miniaudio
- **6.3 Mobile texture path** — ASTC/ETC2 in the cooker
- **6.4 Audio graph depth** — buses, mixers, effects over miniaudio
- **6.5 Re-export & mobile CI** — the Phase 5 game to mobile; Android/iOS CI lanes (runtime coverage → 5 platforms) *(renamed July 2026)*

**Gate:** The Phase 5 game running on a phone.

---

## Phase 7 — 2D Support · *2–3 mo*

**Goal:** the "2D and 3D" capability — deliberately after 3D is solid.

**Epics:**
- **7.1 Box2D v3 wrapper** — behind the engine's own physics API
- **7.2 2D rendering** — sprite batching, tilemaps, 2D camera
- **7.3 Text** — msdf-atlas-gen + rendering
- **7.4 2D editor affordances**
- **7.5 2D sample** — a tiny 2D scene

**Gate:** A working 2D scene: Box2D physics + sprites + text.

---

## Phase 8 — Rendering Maturity & v1.0 · *2–4 mo*

**Goal:** resolve the open rendering decisions with data; polish; ship v1.0.

**Epics:**
- **8.1 Render architecture decision** — forward+ vs deferred, with Tracy profiling data
- **8.2 Lighting & shadow quality + post stack** — cascaded/soft shadows and bloom/AA, building on 3.6
- **8.3 Performance passes** — profiling-driven optimization
- **8.4 Documentation** — it is open source; docs are part of the product
- **8.5 Editor UX polish**
- **8.6 v1.0 release** — tag, samples, announcement

**Gate:** Tagged v1.0 release, documented, with sample projects.

---

## After v1.0

See [future-roadmap.md](./future-roadmap.md) for v2 (baked GI, animation state-machine graph editor, terrain/vegetation) and v3–v4 (ray tracing / mesh shaders / Nanite-style geometry, visual scripting).
