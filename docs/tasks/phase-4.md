# Phase 4 — Scripting (the language fork) · est. 3–5 mo

> **Goal:** gameplay authoring — the thing that makes it an engine you can build a game with. The per-project language choice (ADR-008) lands here, and — added by the July 2026 audit (D1) — so does **play-in-editor**, because "play" is the core loop's third verb and scripts are what make it meaningful.
>
> **Gate:** Press Play in the editor: hot-reloaded TS or native C++ drives entities; press Stop: the scene restores exactly
>
> **Reading of the gate's "or native C++":** Play runs TypeScript only (a C++ game runs as its own build, so Play draws disabled in a C++ project). The native-C++ half of the gate is the C++ mini-game built against the SDK, as in the two-language demo below (task 4.7.1).
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
**Outcome:** a cooked script is a `.aeroscript` (`docs/09` §16): an 80-byte header (the source GUID, the backend
release, the bytecode version, a payload hash) and quickjs-ng bytecode of one ES module. `aero_cooker script` cooks a
`.js` bundle into one, deterministically on every lane, and refuses a script with a static import (the cook resolves
nothing; an `import()` cooks).
The module loader accepts either form: a module file is bytecode iff it begins with the cooked script magic, whatever
its path. Before the backend reads a payload the loader checks the header, the version bytes and the checksum, renames
the module to the name its JavaScript would have had (so a reload and R22's retire rule behave as for JavaScript), and
refuses a read the heap has no room for (R31). A script's bytecode and its JavaScript give the same results; the one
difference is that `toString()` of a script function returns its native-code form. The cook-determinism manifest gained
a sixth arm (22 lines). The title's "cache" is read as: the bytecode is the cache, and skipping unchanged scripts is
5.1.2's; Play in the editor still runs the JavaScript, and nothing under `editor/` changed.

### 4.3.3 Script hot reload · P0 · M · depends: 4.3.1, 4.1.2
**Goal:** the killer dev-loop feature.
**Deliverable:** file watch → rebuild → module swap with component state preserved where the shape didn't change.
Subtasks:
- Watch → transpile → swap module
- State carry-over for unchanged component shapes
**Outcome:** a script is an ES module whose default export is a class; the engine creates and holds its instances, and
an instance's own fields are its state. `Vm::loadScript` loads a script and is also every reload, so any trigger may
call it: a file whose bytes equal those the running class was compiled from is `Unchanged`, and nothing runs. Changed
bytes evaluate a new generation of the module and re-create every live instance from the new class, all or nothing. A
field (an own, enumerable data property) whose name and kind did not change keeps its running value, even when its
default changed in code; a new field takes its default, a removed one is dropped, functions and methods come from the
new code, and a `#private` field restarts. A refused reload changes nothing and is reported once, and the same broken
bytes are not tried again until they change. The VM holds the instances behind `ScriptInstance` handles
(`createInstance`, `destroyInstance`). The watch and the transpile are 3.1.4's watcher and 4.3.1's transpiler,
unchanged: connecting their output to a reload during play is 4.7.4's hot-reload part, attaching a script to an entity
is 4.4.1's, and nothing under `editor/` changed.

### 4.3.4 Source-mapped errors · P1 · M · depends: 4.3.1
**Goal:** *(audit D6)* errors in the console panel point at `Player.ts:42`, not `bundle.js:1379`.
**Deliverable:** JS stack traces remapped through source maps in the log/console panel.
Subtasks:
- Source-map lookup for stack frames; console panel rendering
**Outcome:** a VM maps every stack it describes through the source map its module's JavaScript names. When a module's
last line is `//# sourceMappingURL=<url>`, the map is read through the VM's VFS with the JavaScript and kept with that
generation, and each frame of the ERROR record and of `ScriptError::stack` names the original `.ts` file, line and
column (`    at <fn> (res://assets/scripts/util.ts:4:11)`), while the record's first line names the throw site. No map
changes nothing, a map that cannot be used is one WARN per map per VM, and a cooked script has no map. The Console
shows a record's first line as its row and the lines after it in the row's tooltip, above the C++ location; the filter
searches them and Copy writes them. Nothing in the editor runs a script yet: the play session that brings a script's
error to the Console, and the VFS mount that decides whether a frame reads `res://assets/...`, are 4.7.4's.

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
**Outcome:** a `.ts` file the transpiler bundles is a Script asset (its own kind, colour and filter entry; `.d.ts`,
`.js` and an upper-case `.TS` stay Text), and `engine::ScriptComponent`, the eleventh built-in, names one by GUID: the
Inspector sets it by drop or by picker, and scene v1 saves it as `{"script": "<32 hex>"}`. `engine::script::ScriptSystem`
runs the lifecycle over one World: one instance of the script's default-exported class per entity, holding its entity
as `this.entity` (a class that extends `engine.Script` already sees it in its field initializers); `onInit` once,
`onUpdate(dt)` on every update in attach order, and `onDestroy` once at a detach or at `stop()`, each hook one VM call
under its own budget. A failure is one ERROR and stops that instance, and an interrupt suspends its whole script with
one WARN, until `reloadScript` succeeds. Nothing in the editor runs a script yet: Play and Stop are 4.7.1's, the
restore 4.7.2's, and the play session that calls `update`, `stop` and `reloadScript` 4.7.4's.

### 4.4.2 Entity/world API · P0 · M · depends: 4.4.1
**Goal:** the scripting surface for the world itself.
**Deliverable:** spawn/destroy/query entities; typed component get/set; transform helpers.
Subtasks:
- Entity spawn/destroy/find; component access (typed via bindings)
- Transform convenience API (position/rotation/scale)
**Outcome:** a script works on the world it runs in. `engine.spawn()` makes a new root entity holding an identity
Transform; `engine.find(name)`, `findAll(name)` and `query(...types)` (1 to 8 component classes) look entities up in
ascending entity index; and `engine.destroy(e)` queues an entity, which `ScriptSystem`'s sixth phase, the flush,
destroys at the end of the update with its subtree, after `onDestroy` has run for each attached instance in it while
the entity is still alive. Every path hands out one non-extensible `engine.Entity` object per entity per binding, so
`===` holds; `entity.name`, `entity.parent` (which keeps the local Transform) and `entity.children()` read and write the
World's names and hierarchy, and `entity.script` is the instance attached to that entity, or `null`. `Vec3` and `Quat`
gain in-place math helpers that never reach a core assert, `engine.Transform` gains `worldPosition`, `forward`,
`right`, `up`, `translate`, `rotate` and `lookAt`, and the prelude declares all of it. Typed component get and set
shipped with 4.2.1. Nothing in the editor runs a script yet: Play and Stop are 4.7.1's, the restore 4.7.2's and the play
session 4.7.4's, and `spawn(asset, transform)` is 4.4.4's.

### 4.4.3 Input API for scripts · P1 · M · depends: 4.4.2, 0.3.2
**Goal:** *(audit D7)* gameplay reads input through a stable engine API (touch joins in Phase 6 through this same surface).
**Deliverable:** keyboard/mouse polling + pressed/released edges from TS.
Subtasks:
- Keyboard/mouse state + edge queries exposed to scripts
- Gamepad support (P2) via SDL3 gamepad API
**Outcome:** a script reads input through the frozen `engine.input`, from an `engine::platform::InputState` its caller
binds with `Vm::bindInput` and owns: `keyDown`, `keyPressed` and `keyReleased`; `mouseButtonDown`,
`mouseButtonPressed` and `mouseButtonReleased`; `mousePosition`, `mouseDelta` and `mouseWheel`, each a new `Vec3`; and,
for four player slots, `gamepadConnected`, `gamepadButtonDown`, `gamepadButtonPressed`, `gamepadButtonReleased`,
`gamepadAxis` (raw) and `gamepadStick` (through a round deadzone). Keys, buttons, axes and sticks are named by strings
spelt as the platform enumerators, and the prelude types each set as a string-literal union, so a misspelt name is a
TypeScript error. Gamepads come through the platform layer behind an opt-in `ContextConfig::gamepads` switch (`Off` by
default, `System`, or `VirtualOnly` for headless tests); a pad takes the lowest free slot, and a fifth waits for one.
Nothing in the editor binds input yet: the viewport's routing, game focus and the editor's gamepad switch are 4.7.1's,
the play session's binding 4.7.4's, the runtime's Phase 5's, and touch 6.1.2's.

### 4.4.4 Prefab-lite spawn · P1 · M · depends: 4.4.2, 2.5.1
**Goal:** *(audit D5)* the Phase 5 game must spawn bullets/pickups — instantiate a saved entity-tree.
**Deliverable:** entity-tree asset (a saved sub-scene) instantiable from script and editor. Overrides/nesting stay v2.
Subtasks:
- Save selection as entity-tree asset
- `spawn(asset, transform)` from script; instantiate from the asset browser

**Outcome:** a saved entity tree is a `.aeroprefab` (`docs/09` section 17), a scene v1 document with exactly one root.
`engine::Prefab` captures a tree from a World and copies it into one: a flat copy with no link back, and the placement
composed with the root's stored transform. A script copies one with `engine.spawn(path, transform?)` or
`engine.spawn(path, position, rotation?, scale?)`; the `res://` path is read through the VM's VFS, under the module
loader's size cap, and parsed once per World binding, and every refusal is a `TypeError` that leaves the World as it
was. The Hierarchy's Save as Prefab writes the selection beside a `.meta`, under a unique name, and never overwrites a
file. `AssetKind::Prefab` drags from the asset browser onto the Viewport, a Hierarchy row or the Hierarchy's empty
space, and `InstantiatePrefabCommand` makes each drop one history entry. Overrides, nesting and a prefab editing mode
stay v2; the play session's VFS is 4.7.4's and packing is 5.1.2's.

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

**Outcome:** a C++ game builds against an installed engine and nothing else. The `<os>-sdk` presets (`AERO_SDK`: Release,
no profiling, no sanitizers, both tool options ON) install the eleven static layers, 87 of the 95 public headers, the
80 compiled shaders, four third-party link files and a hand-written CMake package. A consumer runs
`find_package(AeroEngine)` and `aero_add_game` with no toolchain file, and no third-party include directory reaches its
compile line (`docs/04`, "The C++ SDK"). The writing of the mini-game found two gaps.
`World::registerComponent<T>` registers a game's own component types per World, with no ECS type in a public header, and
`platform::basePath()` names the folder the shipped data sits in. `scene_serialize::loadPrefabText` lets a C++ game load
a prefab without quickjs. On MSVC the package pins the release DLL runtime in every configuration.
`samples/phase-4-scripting/cpp` is the mini-game, and its README's rules are the reference for the TypeScript half. Six
`sdk.*` entries prove the prefix on all three CI lanes: its layout, a moved prefix, every header compiling alone, the
refusals and the mini-game's scripted run.

### 4.5.2 C++ project template · P0 · M · depends: 4.5.1
**Goal:** `New Project (C++)` produces something that builds.
**Deliverable:** CMake template project linking the engine with a hello-cube gameplay loop.
Subtasks:
- Template project (CMake + engine link + entry point)
- Hello-cube gameplay; documented build steps

**Outcome:** a C++ project's four files (`CMakeLists.txt`, a hello-cube `src/main.cpp`, `README.md` and `.gitignore`)
render from `editor/templates/cpp-project/` in one pass over three tokens, with the name sanitised into `game_<...>`
for CMake and escaped into the window's title (`docs/09` section 4.12). `writeCppProject` writes each file only when
nothing is at its path, and never reads, merges, renames or removes anything. `sdk.template` writes a project into a
folder with a space in its name, builds it from the README's own commands against the SDK prefix, and runs the
hello-cube to its frame limit. New Project calls the writer in 4.6.1.

---

## Epic 4.6 — Language-choice project setting · editor

**Goal:** ADR-008's rule becomes UI: one language, chosen at creation, never mixed.
**Definition of Done:** project creation asks TS or C++; the editor gates features accordingly.

### 4.6.1 Language field + editor gating · P0 · S · depends: 2.6.1, 4.4.1, 4.5.2
**Goal:** wire the fork into the project system.
**Deliverable:** `language: "ts" | "cpp"` set at creation (immutable after); script panels/import active only for TS projects, template generation for C++. Task E.6.4 ships the New Project dialog's language control disabled, TypeScript shown selected, with the tooltip "Not implemented yet -- task 4.6.1"; this task enables it and gives createProject the choice (NEW_PROJECT_LANGUAGE today). Template generation is `writeCppProject` (4.5.2, `docs/09` section 4.12): New Project calls it for a C++ project, into the project folder it has just made.
Subtasks:
- Creation-dialog language choice → `project.json`
- Feature gating by language across editor UI

**Outcome:** a project's language is chosen once, in New Project, and never changes: `createProject` takes a
`ProjectLanguage` with no default, the dialog's two cards are live with TypeScript preselected on every open, and
nothing writes `project.json` after creation (Project Settings shows the language read-only). Creating a C++ project
writes the C++ template (`writeCppProject`, `docs/09` section 4.12) once, at creation; a file it cannot write is one
WARN, and the project opens anyway. In a C++ project a `.ts` is Text, never a Script asset, wherever the editor
classifies one (the browser, its filter and search, the picker, the Inspector's reference row), and Add Component leaves
out any component whose field names a Script asset, found by its metadata, never by its name (`docs/09` section 4.2).

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

**Outcome:** `engine::editor::PlaySession`, a pure value in `aero_editor_core`, owns the mode (Edit, Playing, Paused)
and every rule around it, with one writer, `PlaySession::apply` (`docs/03`, "Play-in-editor"). The toolbar's Play,
Pause, Step and Stop and the chords Cmd/Ctrl+P, +Shift+P and +Alt+P go through one enabled rule; Play needs an open
TypeScript project, and a disabled control's tooltip names why. A game frame runs once per editor tick with
`FrameClock::deltaSeconds()` as dt, none runs while paused, and Step runs exactly one frame of 1/60 s. While the game
has focus (playing or paused, the Viewport holding the keyboard, the window in front), keys, buttons pressed over the
image and the pointer feed the game's own `platform::InputState`, and a focus loss releases everything it holds; the
editor's Context starts the system gamepad drivers. In play the Viewport draws through the scene's lowest-index Camera
with the editor's own visuals hidden and its keys inert, every scene edit is locked, and a Quit stops play first. No
script runs in play yet: that is 4.7.4's.

### 4.7.2 Scene snapshot & restore · P0 · L · depends: 4.7.1, 1.2.2
**Goal:** Stop means *exactly as it was* — the feature that separates real editors from demos.
**Deliverable:** in-memory world serialization on Play; byte-exact restore on Stop (leak-checked under ASan).
Subtasks:
- Snapshot world state on Play (reuses generated serialization)
- Restore on Stop; assert entity/resource parity

**Outcome:** Play copies the scene's World with `World::clone` (`engine/scene`), which keeps every identity: each live
handle, each free slot's version and the free-list order, every pool in packed order, the registration table, the
hierarchy and the names. `ParkedScene` (`aero_editor_core`) holds the original untouched while the game runs on the
copy, and every Stop (the toolbar, the chord, a request or a Quit) moves the original back as a whole World with its
root order, so the scene after Stop is the scene before Play by construction (`docs/03`, "The park and the restore").
The generated serialization is the tests' oracle rather than the snapshot: the text a scene saves before Play equals
the text after Stop, beside equal handles, per-type counts, root order and the asset ledger's bindings, and fifty park
and restore cycles run under ASan. The selection after Stop is the current one, pruned; the undo history is untouched;
and Stop drops every entity handle the editor kept from play. No parity check runs in the product: the tests prove it.

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
