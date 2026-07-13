# Phase 4 — Scripting (the language fork) · est. 3–5 mo

> **Goal:** gameplay authoring — the thing that makes it an engine you can build a game with. The per-project language choice (ADR-008) lands here, and — added by the July 2026 audit (D1) — so does **play-in-editor**, because "play" is the core loop's third verb and scripts are what make it meaningful.
>
> **Gate:** Press Play in the editor: hot-reloaded TS or native C++ drives entities; press Stop: the scene restores exactly
>
> **Phase non-goals:** no export (Phase 5); no debugger (v1 ships source-mapped errors only — DAP debugger is v2); one language per project, never mixed (ADR-008).
>
> **Notes:** the TS toolchain default is **esbuild** (single Go binary, no Node runtime, invoked externally like Blender) — the esbuild-vs-swc open decision closes at 4.3.1. Play mode is in-process in v1; its crash risk is R11, mitigated at 4.7.3.
>
> **Gate artifact:** the two-language demo (same logic as TS script and as C++ mini-game) is committed under `/samples/phase-4-scripting/`.

---

## Epic 4.1 — quickjs-ng embed · script

**Goal:** the engine hosts a JavaScript VM: lifecycle, modules, errors — nothing script-visible yet.
**Definition of Done:** a JS module loads from the VFS and runs inside the engine with errors surfacing in the engine log.

### 4.1.1 VM lifecycle · P0 · M · depends: 0.2.6
**Goal:** own the VM: create, configure, tear down, and trap errors.
**Deliverable:** context create/destroy with memory limits and error trapping routed into the engine log.
Subtasks:
- Runtime/context lifecycle; memory limits
- Exception → engine log plumbing

### 4.1.2 Module loading via VFS · P0 · M · depends: 4.1.1
**Goal:** ES modules resolved the engine's way — project dir in dev, `.pak` at runtime.
**Deliverable:** module loader resolving imports through the VFS.
Subtasks:
- ES module resolution through the VFS
- Module cache + reload hooks (consumed by 4.3.3)

---

## Epic 4.2 — `reflect-gen` bindings target · reflect

**Goal:** reflection consumers #3 and #4 (ADR-004): generated quickjs bindings and generated `.d.ts` — write a component once, script it with autocomplete.
**Definition of Done:** an annotated component is readable/writable from TS with full VSCode autocomplete, zero hand-written glue.

### 4.2.1 quickjs binding emission · P0 · L · depends: 1.1.4, 4.1.1
**Goal:** the generator learns its third output: binding glue.
**Deliverable:** generated get/set for every reflected component + entity API glue, registered with the VM.
Subtasks:
- Emit component accessors from `entt::meta` data
- Emit registration; wire into the generated-sources build step

### 4.2.2 `.d.ts` emission · P0 · M · depends: 4.2.1
**Goal:** the free tooling payoff: typed autocomplete without writing types.
**Deliverable:** generated `.d.ts` for the engine API and all reflected components.
Subtasks:
- Emit `.d.ts` (engine API + components); ship into the project template

---

## Epic 4.3 — TS pipeline · script

**Goal:** the author loop: edit `.ts` → transpile on import → run → hot reload; errors point at *your* source line.
**Definition of Done:** editing a script updates running behavior without restarting; a thrown error names the `.ts` file and line.

### 4.3.1 esbuild integration on import · P0 · M · depends: 3.1.2
**Goal:** TS → JS + source maps in the import pipeline. **Closes the esbuild-vs-swc open decision: esbuild** (single binary, no Node), swc documented as the fallback.
**Deliverable:** `.ts` assets transpile via an external esbuild binary, producing JS + source map.
Subtasks:
- esbuild binary detection/bundling; import-pipeline invocation
- Emit JS + source map as derived assets; record the decision in docs/08

### 4.3.2 Bytecode compile & cache · P1 · M · depends: 4.3.1
**Goal:** ship bytecode, iterate on JS (docs/03 asset flow: scripts → bytecode in the `.pak`).
**Deliverable:** quickjs bytecode at cook time; dev mode runs JS directly.
Subtasks:
- Bytecode emission in the cooker; loader accepts both forms

### 4.3.3 Script hot reload · P0 · M · depends: 4.3.1, 4.1.2
**Goal:** the killer dev-loop feature.
**Deliverable:** file watch → rebuild → module swap with component state preserved where the shape didn't change.
Subtasks:
- Watch → transpile → swap module
- State carry-over for unchanged component shapes

### 4.3.4 Source-mapped errors · P1 · M · depends: 4.3.1
**Goal:** *(audit D6)* errors in the console panel point at `Player.ts:42`, not `bundle.js:1379`.
**Deliverable:** JS stack traces remapped through source maps in the log/console panel.
Subtasks:
- Source-map lookup for stack frames; console panel rendering

---

## Epic 4.4 — Script components & entity API · script

**Goal:** TypeScript actually drives the world: lifecycle, entity/world access, input, spawning.
**Definition of Done:** a TS script spawns, moves, and destroys entities in response to input.

### 4.4.1 ScriptComponent + lifecycle · P0 · L · depends: 4.2.1
**Goal:** attach behavior to entities the way Unity/Godot users expect.
**Deliverable:** `ScriptComponent` binding a script asset to an entity; `onInit`/`onUpdate(dt)`/`onDestroy` with per-entity instance state.
Subtasks:
- ScriptComponent (reflected) referencing a script asset
- Lifecycle dispatch; per-entity instance state

### 4.4.2 Entity/world API · P0 · M · depends: 4.4.1
**Goal:** the scripting surface for the world itself.
**Deliverable:** spawn/destroy/query entities; typed component get/set; transform helpers.
Subtasks:
- Entity spawn/destroy/find; component access (typed via bindings)
- Transform convenience API (position/rotation/scale)

### 4.4.3 Input API for scripts · P1 · M · depends: 4.4.2, 0.3.2
**Goal:** *(audit D7)* gameplay reads input through a stable engine API (touch joins in Phase 6 through this same surface).
**Deliverable:** keyboard/mouse polling + pressed/released edges from TS.
Subtasks:
- Keyboard/mouse state + edge queries exposed to scripts
- Gamepad support (P2) via SDL3 gamepad API

### 4.4.4 Prefab-lite spawn · P1 · M · depends: 4.4.2, 2.5.1
**Goal:** *(audit D5)* the Phase 5 game must spawn bullets/pickups — instantiate a saved entity-tree.
**Deliverable:** entity-tree asset (a saved sub-scene) instantiable from script and editor. Overrides/nesting stay v2.
Subtasks:
- Save selection as entity-tree asset
- `spawn(asset, transform)` from script; instantiate from the asset browser

---

## Epic 4.5 — C++ project mode · engine

**Goal:** the other tine of the fork (ADR-008): a native game project against the engine's public API — dogfooded internally since Phase 1, productized here.
**Definition of Done:** a fresh C++ project template compiles, links the engine, and runs gameplay.

### 4.5.1 Public C++ API surface audit · P0 · M · depends: 4.4.2
**Goal:** the installable header set is deliberate, boundary-clean, and sufficient.
**Deliverable:** audited public header install set; boundary-rule sweep green; gaps found by writing a real mini-game against it.
Subtasks:
- Define the header install set; sweep for third-party leakage
- Write a mini-game against it; fix what's missing

### 4.5.2 C++ project template · P0 · M · depends: 4.5.1
**Goal:** `New Project (C++)` produces something that builds.
**Deliverable:** CMake template project linking the engine with a hello-cube gameplay loop.
Subtasks:
- Template project (CMake + engine link + entry point)
- Hello-cube gameplay; documented build steps

---

## Epic 4.6 — Language-choice project setting · editor

**Goal:** ADR-008's rule becomes UI: one language, chosen at creation, never mixed.
**Definition of Done:** project creation asks TS or C++; the editor gates features accordingly.

### 4.6.1 Language field + editor gating · P0 · S · depends: 2.6.1, 4.4.1, 4.5.2
**Goal:** wire the fork into the project system.
**Deliverable:** `language: "ts" | "cpp"` set at creation (immutable after); script panels/import active only for TS projects, template generation for C++.
Subtasks:
- Creation-dialog language choice → `project.json`
- Feature gating by language across editor UI

---

## Epic 4.7 — Play-in-editor · editor

**Goal:** *(added by the July 2026 audit, D1)* the core loop's "play": run the game inside the editor and come back to an untouched scene. In-process for v1 (R11 — an engine crash takes the editor with it; autosave is the seatbelt; separate-process mode is v2).
**Definition of Done:** the phase gate — Play runs scripts against the live scene; Stop restores it exactly.

### 4.7.1 Play/stop state machine + toolbar · P0 · M · depends: 4.4.1
**Goal:** the edit/play/pause mode spine and its UI.
**Deliverable:** toolbar Play/Pause/Stop; input routed to the game in play mode, to the editor otherwise.
Subtasks:
- Mode state machine (edit/play/pause) + toolbar
- Input routing per mode; viewport takes game focus in play

### 4.7.2 Scene snapshot & restore · P0 · L · depends: 4.7.1, 1.2.2
**Goal:** Stop means *exactly as it was* — the feature that separates real editors from demos.
**Deliverable:** in-memory world serialization on Play; byte-exact restore on Stop (leak-checked under ASan).
Subtasks:
- Snapshot world state on Play (reuses generated serialization)
- Restore on Stop; assert entity/resource parity

### 4.7.3 Autosave-before-play · P0 · S · depends: 4.7.1
**Goal:** the R11 seatbelt: a play-mode crash must never cost work.
**Deliverable:** scene + project saved to disk automatically before entering play.
Subtasks:
- Autosave hook on Play; recovery prompt after a crash

### 4.7.4 Script lifecycle in play mode · P0 · M · depends: 4.7.2, 4.4.1
**Goal:** scripts live and die with the mode.
**Deliverable:** `onInit` on Play, `onUpdate` while playing (paused = skipped), `onDestroy` on Stop; hot reload still works inside play mode.
Subtasks:
- Lifecycle tied to mode transitions; pause semantics
- Hot reload inside play mode

---

**Phase gate:** Press Play in the editor: hot-reloaded TS or native C++ drives entities; press Stop: the scene restores exactly.
