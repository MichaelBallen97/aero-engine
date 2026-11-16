# 05 — Roadmap & Phases

> 12 phases — 0–8, plus **Phase E** (Editor Experience) inserted between 3 and 4, and **Phase F** (Editor Workflow) and **Phase G** (3D Physics) inserted, in that order, between 4 and 5. Every phase ends in a **usable deliverable gate** (project rule #2). Durations are part-time solo estimates — planning aids, not commitments.

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
| **E** | Editor Experience | 3–4 mo | Open a project and land in the scene you were last editing, on a lit grid floor under a sky; create a Cube from the menu, drop a material on it and see it shade; aim a spot light with a visible gizmo; rename, move and delete assets without leaving the editor |
| **4** | Scripting (language fork) | 3–5 mo | Press Play in the editor: hot-reloaded TS or native C++ drives entities; press Stop: the scene restores exactly |
| **F** | Editor Workflow | 2–3 mo | Open the editor and it fills the screen; drag a model in from the file manager and it imports; click the object under the cursor, even one standing behind another object's bounding box, and delete it with the Delete key; select a camera and see what it sees; create a script and a second scene from the browser and switch scenes from the breadcrumb; save a script while the game plays and see the change at once |
| **G** | 3D Physics | 3–4 mo | In the editor, give a cube a Rigid Body and a collider and see the collider drawn around it; press Play and it falls, lands on the floor and comes to rest; a ball pushed by a script knocks over a stack of crates; a character walks up a ramp and stops at a wall; a door swings on its hinge when the character pushes it; a script hears a trigger volume and finds a crate with a raycast; press Stop and the scene restores exactly; and the same scene runs as a native C++ game |
| **5** | Export & **Ship a Game** 🎯 | 2–3 mo | A downloadable, playable 3D game made entirely in Aero Engine (desktop) |
| **6** | Mobile Runtime & Audio Depth | 2–3 mo | The Phase 5 game running on a phone; audio buses/mixers |
| **7** | 2D Support | 2–3 mo | A working 2D scene: Box2D physics + sprites + text |
| **8** | Rendering Maturity & v1.0 | 2–4 mo | Tagged v1.0 release, documented, with sample projects |

**Total: ~28–43 months**; budget to ~45 with slippage. (Phase E added 3–4 months to the original ~20–32, Phase F 2–3 more, and Phase G 3–4 more.)

> **Note on ordering vs. the original draft:** "ship a game" was moved from Phase 4 to **Phase 5**, because you cannot ship before both scripting (Phase 4) and export (Phase 5) exist. 2D was deliberately placed at Phase 7, after the 3D foundation is proven.
>
> **Note on the July 2026 re-plan:** play-in-editor was placed in Phase 4 (epic 4.7); basic shadows/culling (3.6) and audio playback (3.7) were pulled forward into Phase 3 so the Phase 5 game ships with sound and shadows; Phase 2 gained the project system (2.6). Full task depth for every phase now lives in [docs/tasks/](./tasks/) — see [07 — Tasks](./07-tasks.md).
>
> **Note on Phase E (inserted after Phase 3 closed in code):** the editor built through Phases 2 and 3 *works*, and using it surfaced a cluster of correctness and workflow gaps that belong to no existing phase — a viewport with no ground, horizon or orientation reference; no cone light and no light gizmos, so a light's aim is invisible; a material silently ignored on primitive meshes; an asset browser that is read-only by contract; a project that never reopens the scene you were editing and does not notice a scene from a different project. Phase 8.5 is a *polish* epic scheduled after v1.0's feature set and after 8.2's quality pass — the wrong home for a missing light type or for file management. Phase 4 authors gameplay **through** this editor, so closing these before Phase 4 is cheaper than after it. It is lettered rather than fractioned because `3.5` is already taken; see [07 — Tasks](./07-tasks.md)'s numbering conventions.
>
> **Note on Phase F (inserted after Phase 4's gate was met):** Phase 4 closed with a small game made in the editor, and making it — then building a test project around an imported model — surfaced workflow gaps no phase owns: a viewport click that selects the object whose bounding *box* the ray enters first rather than the one under the cursor; a Delete key that works only while the Hierarchy holds the keyboard; a window that opens at 1280 × 720 and remembers nothing; no way to bring a file in except copying it on disk; scripts and scenes that cannot be created or opened from the asset browser; and a pause of about 2.5 s between saving a script and seeing it in the running game, nearly all of it the polling watcher. Phase 5 builds and ships a real game **through** this editor, so each gap is paid daily there unless it is closed first — the same argument that placed Phase E before Phase 4. It is lettered because `4.5` is already Phase 4's C++-project-mode epic. A change of on-disk format was weighed for this phase and left out: the JSON formats of `docs/09` stay.
>
> **Note on Phase G (inserted on the day Phase F was planned):** Jolt is the 3D physics choice in the stack table, `physics/` is a box in the layer diagram and "Jolt (3D)" is part of what v1.0 is — and no task in any phase built it. Measured on 2026-11-16, `engine/physics/` held a `.gitkeep` from Phase 0's folder skeleton and nothing else, Jolt was not in `vcpkg.json`, none of the eleven built-in components was a body or a collider, a script could not apply a force, cast a ray or hear a collision, and the editor's Play ran the scripts' update and nothing else. Two tasks assumed physics anyway: 5.2.1's runtime "runs the full loop (scene, physics, audio, script)", and 7.1.1 wraps Box2D "the way Jolt is wrapped". Phase 5 ships a real 3D game, and without physics its motion and its hit tests would be written by hand in scripts, as Phase 4's sample writes them — so the gap is closed first, after Phase F so that its viewport and menu work is there to build the collider tooling on. It is lettered for the reason Phases E and F are.

---

## Phase 0 — Foundations & First Triangle · *2–3 mo*

**Goal:** prove the toolchain, build, CI, and the RHI abstraction all work end-to-end. This phase is *also* the C++ learning budget — treat it as such.

**Epics:**
- **0.1 Build & CI bootstrap** — CMake + presets + vcpkg manifest; GitHub Actions ×3 OS; ASan/UBSan; doctest; Tracy wired
- **0.2 `core`** — `Handle<T>`, math types (GLM backend) + the CI test that fails if `glm` leaks past `engine/core/src/math/glm_backend.cpp`, spdlog logging, enkiTS jobs, time, VFS
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

## Phase E — Editor Experience · *3–4 mo*

**Goal:** the editor stops being a thing that *works* and becomes a thing that is *usable* — you can see where you are, see what your lights do, edit what you select, manage your files, and reopen your work where you left it.

**Epics:**
- **E.1 Viewport legibility** — the engine's first world-space line renderer, a grid floor and world axes, a view-axis gizmo, a silhouette selection outline replacing the bounding-box wireframe, and a restyled transform gizmo
- **E.2 Lighting & environment** — a reflected `Environment` (gradient sky + hemispheric ambient) replacing the hardcoded 3 % constant, inverse-square point falloff, a new `SpotLight`, light gizmos and icons, and a material preview that agrees with the viewport
- **E.3 Inspector & context routing** — axis-labelled vector fields, selection raising the panel that edits it, a searchable asset-reference picker with previews, and a redesigned material inspector
- **E.4 Project, scene & asset management** — reopen the last scene, refuse a scene from another project, full file CRUD in the browser with `.meta` identity preserved, an extended ignore roster, and material names + rendered thumbnails
- **E.5 Content creation UX** — a Create menu for every built-in, a named primitive selector, and the fix for materials being ignored on primitive meshes
- **E.6 Shell identity & visual system** — the editor's first fonts, icon set and theme, a main toolbar and status bar, a panel chrome pass, and a restyled Welcome / New Project flow

**Gate:** Open a project and land in the scene you were last editing, on a lit grid floor under a sky; create a Cube from the menu, drop a material on it and see it shade; aim a spot light with a visible gizmo; rename, move and delete assets without leaving the editor.

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

## Phase F — Editor Workflow · *2–3 mo*

**Goal:** the editor stops getting in the way of the loop it exists for — a click selects what is under the cursor, a key deletes it, a file dragged in from the desktop becomes an asset, scripts and scenes are made and reached from the browser, and a saved script reaches the running game at once.

**Epics:**
- **F.1 Viewport selection & orientation** — per-triangle picking behind the box pass, whole-model selection with a second click for the part, Delete from the viewport and the Edit menu, axis lines on the view-axis widget, and a camera frustum with a preview inset
- **F.2 Window, menus & panels** — a full-screen start with remembered window state, the viewport's options moved to the View menu and saved, and selection routing that always brings the right panel forward
- **F.3 Assets & scenes** — files dragged in from the file manager, New Script and New Scene in the browser, scenes as recognisable assets that open on a double-click, a scene switcher in the breadcrumb, folder icons, and scene pictures
- **F.4 Iteration speed** — a native file watcher behind the polling seam, and a proven table of what reloads while the game runs

**Gate:** Open the editor and it fills the screen; drag a model in from the file manager and it imports; click the object under the cursor, even one standing behind another object's bounding box, and delete it with the Delete key; select a camera and see what it sees; create a script and a second scene from the browser and switch scenes from the breadcrumb; save a script while the game plays and see the change at once.

---

## Phase G — 3D Physics · *3–4 mo*

**Goal:** the engine gets the 3D physics its stack table names — a game developer gives an entity a rigid body and a collider in the editor, sees the collider in the viewport, presses Play and gravity takes over; game code in TypeScript or C++ pushes bodies, casts rays and hears collisions and triggers; a character walks, a door swings on its hinge; and Stop restores the scene exactly.

**Epics:**
- **G.1 Physics core** — Jolt behind the engine's own types: a world that steps, bodies and primitive shapes addressed by handles, and the physics-boundary guard
- **G.2 Bodies & colliders in the scene** — reflected `RigidBody` and collider components, the `scene_physics` bridge on a fixed timestep, and convex-hull and triangle-mesh colliders taken from a model
- **G.3 Physics for gameplay** — forces and a fixed update for scripts, raycasts and overlap queries, collision and trigger events, collision layers, a character controller, and joints
- **G.4 Physics in the editor** — physics under Play, Pause, Step and Stop, the Inspector and Create menu for physics, collider wireframes and an overlay, drag handles that resize a collider, and project physics settings
- **G.5 Sample & SDK** — physics in the installed C++ SDK, and the sample scene the gate runs on, in TypeScript and in C++

**Gate:** In the editor, give a cube a Rigid Body and a collider and see the collider drawn around it; press Play and it falls, lands on the floor and comes to rest; a ball pushed by a script knocks over a stack of crates; a character walks up a ramp and stops at a wall; a door swings on its hinge when the character pushes it; a script hears a trigger volume and finds a crate with a raycast; press Stop and the scene restores exactly; and the same scene runs as a native C++ game.

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
