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

## Stories

The gate as a scenario: Given a project whose entities carry game logic written in its one language, when I press
Play in the editor, then that logic drives the entities and picks up my edits while the game runs, and when I press
Stop, then the scene is exactly as it was before Play; the same logic also runs as a C++ mini-game built against the
engine's public API. Each story below makes more of it run; its demo lives in `samples/phase-4-scripting/`.

### Story 1 — I press Play and my TypeScript script moves my entities; I press Stop and my scene is exactly as I left it
Tasks: 4.2.1, 4.3.1, 4.4.1, 4.7.1, 4.7.2, 4.7.4 (lifecycle part)
As a game developer, I attach a TypeScript script to entities, press Play to watch it drive them and press Stop to get
my scene back exactly, so that I can try gameplay in the editor without risking the scene I built.
- Given a `.ts` file in my project, when the editor imports it, then JavaScript and a source map are produced for it,
  and it becomes a script asset I can attach to an entity.
- Given two entities with that script attached, when I press Play, then each entity's `onInit` runs once and its
  `onUpdate(dt)` moves that entity in the viewport every frame, each with its own script state.
- Given the game is playing, when I press Pause, then `onUpdate` stops running, and when I then press Step, then
  exactly one more frame of `onUpdate` runs.
- Given the game is playing with the viewport focused, when I press the viewport's tool keys (Q, W, E, R), then the
  editor's transform tool does not change, and after Stop the same keys switch it again.
- Given the game is playing, when I press Stop, then `onDestroy` runs once per script and the scene is exactly what it
  was before Play: the same entities and component values, with nothing left behind.
Demo: `samples/phase-4-scripting/` — open the sample's scene with two scripted cubes, press Play and watch them move,
press Pause and Step, press W and see the tool stay put, then press Stop and see the scene exactly as before; about
two minutes.

### Story 2 — I edit my script while the game runs and see the change at once, and a crash or an error costs me nothing
Tasks: 4.7.3, 4.3.3, 4.7.4 (hot-reload part), 4.3.4
As a game developer, I change my TypeScript while the game is running and see the change at once, so that I can
iterate without restarting and without losing work when something goes wrong.
- Given my scene and project have unsaved changes, when I press Play, then both are written to disk before the first
  `onInit` runs.
- Given the editor went down while the game was playing, when I reopen the project, then it offers to recover my work.
- Given the game is playing and my script keeps a value on each entity, when I edit the `.ts` file and save it, then
  the entities follow the new code without Stop and Play, and each kept value whose shape did not change keeps its
  value.
- Given my script throws while the game is playing, when the error reaches the Console panel, then it names my `.ts`
  file and the line in it, not the generated JavaScript.
Demo: `samples/phase-4-scripting/` — edit the scene and press Play to find the edit on disk, change the script's speed
and save while the cubes move, make the script throw and read the Console line, then end the editor mid-play and
reopen the project to see the recovery offer; about two minutes.

### Story 3 — I write a small game in TypeScript: keys move my player, a key fires bullets, and autocomplete knows the whole API
Tasks: 4.4.2, 4.4.3, 4.4.4, 4.2.2, 4.3.2
As a game developer, I write a small game in TypeScript with keyboard input, spawning and destroying entities and a
typed API, so that I can build real gameplay and know that what I play in the editor is what the cooked game runs.
- Given a script that spawns, finds, moves and destroys entities, when it runs during play, then the entities appear,
  move and disappear in the viewport as scripted, and after Stop the scene is exactly as before Play.
- Given a script that reads the keyboard and mouse, when I press, hold and release keys and mouse buttons with the
  viewport focused during play, then the script sees the held state and each pressed and released edge.
- Given entities I select and save as an entity-tree asset, when a script calls `spawn(asset, transform)`, then a copy
  of the whole tree appears at that transform, and instantiating the same asset from the asset browser puts the tree
  in my scene.
- Given a TypeScript project, when I open one of its scripts in VSCode, then the generated `.d.ts` offers every
  reflected component and the engine API with their types, and a wrong field name or type is flagged.
- Given the small game, when the cooker runs on the project, then each script is emitted as bytecode, the engine runs a
  script's bytecode and its JavaScript with the same results, and Play in the editor still runs the JavaScript.
Demo: `samples/phase-4-scripting/` — play the small game with the keyboard and fire a few bullets, press Stop and see
the scene as before, drop the bullet asset into the scene from the asset browser, type in a script to see
autocomplete, then cook the project and find each script's bytecode; about two minutes.

### Story 4 — I make the same game in C++: I pick the language when I create the project, build it and run it
Tasks: 4.5.1, 4.5.2, 4.6.1
As a game developer, I create a C++ project, build it against the engine's public API and run it, so that I can make
natively the same game I made in TypeScript.
- Given the engine's installed public headers, when I build the sample's C++ mini-game against them alone, then it
  builds, plays the same logic as the TypeScript small game, and no header it includes names a third-party type.
- Given the New Project dialog, when I choose C++ and create the project, then `project.json` records
  `language: "cpp"`, the CMake template with its hello-cube loop is generated, and the language cannot be changed
  afterwards.
- Given that new C++ project, when I follow its documented build steps, then it builds against the engine and the
  hello-cube loop runs.
- Given a TypeScript project and a C++ project, when I open each in turn, then script panels and `.ts` import are
  active only in the TypeScript one, and template generation only in the C++ one.
Demo: `samples/phase-4-scripting/` — create a TypeScript project and a C++ project from the New Project dialog and
compare what the editor offers in each, then build the sample's C++ mini-game with the documented steps and play it
beside the TypeScript version; about two minutes once the engine is built.

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
**Outcome:** `reflect-gen --emit-script` generates quickjs-free binding tables for the ten built-in components,
`aero_script` (now above `aero::scene`) compiles them, and every VM exposes them as the frozen `engine` namespace
with `Vec3`, `Quat` and `Entity`; `Vm::bindWorld` and `defineEntityGlobal` hand a script an entity whose components it
reads and writes. Spawn, destroy, find and the lifecycle stay with 4.4.x.

### 4.2.2 `.d.ts` emission · P0 · M · depends: 4.2.1
**Goal:** the free tooling payoff: typed autocomplete without writing types.
**Deliverable:** generated `.d.ts` for the engine API and all reflected components.
Subtasks:
- Emit `.d.ts` (engine API + components); ship into the project template
**Outcome:** `reflect-gen --emit-dts` emits one declaration block per component header, and the build joins them with a
checked-in prelude for `Vec3`, `Quat` and `Entity` into one `aero-engine.d.ts`, which the editor embeds. Adopting a
TypeScript project writes `Library/types/aero-engine.d.ts` and, only when absent, a `tsconfig.json`, so VS Code offers
every component and the engine API with their types and flags a wrong field name or type. A pinned TypeScript 5.9.3
type-checks the output in ctest. The runtime ships none of it.

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
**Outcome:** configure fetches a pinned esbuild 0.28.2 for the host from the npm registry, checks it against an
in-repo SHA-512 table, caches it once per machine and copies it beside the editor; `AERO_ESBUILD_PATH` overrides it.
After every scan of a TypeScript project, each `.ts` whose source, imports, flags or binary changed is bundled in the
background, one esbuild process at a time, to a self-contained `Library/Scripts/<guid>.js` with a source map and a
provenance record (`docs/09` §15). `using` is lowered for the VM (R21), and a hung esbuild is stopped after 30 s. Each
diagnostic is a Console line naming the `.ts` file, line and column, and a failed script keeps its last good
JavaScript and shows its errors at every pass until it is fixed. The esbuild-vs-swc decision is recorded in docs/08:
esbuild, with swc the fallback. `.ts` files stay Text assets; the script asset kind is 4.4.1's.

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
**Deliverable:** `language: "ts" | "cpp"` set at creation (immutable after); script panels/import active only for TS projects, template generation for C++. Task E.6.4 ships the New Project dialog's language control disabled, TypeScript shown selected, with the tooltip "Not implemented yet -- task 4.6.1"; this task enables it and gives createProject the choice (NEW_PROJECT_LANGUAGE today).
Subtasks:
- Creation-dialog language choice → `project.json`
- Feature gating by language across editor UI

---

## Epic 4.7 — Play-in-editor · editor

**Goal:** *(added by the July 2026 audit, D1)* the core loop's "play": run the game inside the editor and come back to an untouched scene. In-process for v1 (R11 — an engine crash takes the editor with it; autosave is the seatbelt; separate-process mode is v2).
**Definition of Done:** the phase gate — Play runs scripts against the live scene; Stop restores it exactly.

### 4.7.1 Play/stop state machine + toolbar · P0 · M · depends: 4.4.1
**Goal:** the edit/play/pause mode spine and its UI.
**Deliverable:** toolbar Play/Pause/Step/Stop -- Step advances one frame while paused; input routed to the game in play
mode, to the editor otherwise. Task E.6.2 ships Play, Pause and Step as disabled placeholders in the shell toolbar, each
with the tooltip "Not implemented yet -- task 4.7.1"; this task enables them and adds Stop.
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
