# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Aero Engine — an open-source (MIT), cross-platform 3D game engine with an editor and per-project TypeScript **or** C++ scripting. Solo project, started July 2026. The goal is core-workflow parity with Unity/Godot (edit → script → play → export), explicitly **not** feature parity. 3D-first; 2D arrives in Phase 7.

Two platform matrices, never to be conflated: the **editor** runs on macOS/Windows/Linux only; the **runtime** (exported games) targets those three plus iOS and Android. The editor never runs on mobile — no touch UI, no adaptive layouts.

## Current state — read this first

**Phase 4 (Scripting) is open.** Epics 4.1 and 4.2 are closed, every task macOS-validated: 4.1.1 (VM lifecycle, PR
#123 `efdd0b9`), 4.1.2 (module loading via the VFS, PR #124 `ccf776c`), 4.2.1 (quickjs binding emission, PR #125
`fab6db8`: the frozen `engine` namespace) and 4.2.2 (`.d.ts` emission, PR #126 `1c10fad`: `aero-engine.d.ts` and a
`tsconfig.json` written into a TypeScript project, type-checked by a pinned TypeScript 5.9.3). Epic 4.3 is open, its
first three tasks merged and macOS-validated. **4.3.1 (esbuild integration on import, PR #127 `d06a635`, 7 / 7)**: after
every scan of a TypeScript project, each changed `.ts` is bundled in the background by a pinned esbuild 0.28.2 to
`Library/Scripts/<guid>.js` + `.js.map` + a provenance record, with `using` lowered (R21) and each diagnostic a Console
line. **4.3.2 (bytecode compile & cache, PR #129 `c26b890`, 3 / 3)**: `aero_cooker script` cooks one such bundle to a
`.aeroscript` (an 80-byte header and quickjs-ng bytecode, `docs/09` §16), and the module loader runs bytecode and
JavaScript with the same results; a read needs 64 KiB + 48 × the payload of free heap (R31), and the import path's C
stack per level is a budget that Windows Debug sees first (R33). **4.3.3 (script hot reload, PR #130 `08025f9`,
3 / 3 on 2026-10-31)**: a script is an ES module whose default export is a class; `Vm::loadScript` loads it and is
every reload, and the VM holds its instances behind `ScriptInstance` handles. Changed bytes re-create every instance
from the new class, all or nothing, and each field whose name and kind did not change keeps its value; identical bytes
are `Unchanged` and the last refused bytes `AlreadyRefused`, so any trigger is safe (R29). An interrupted promise
executor leaves a job the VM's next call answers for (R34, accepted). Nothing under `editor/` changed. No quickjs-ng
patch shipped (R21, R23, R24, R25, R31 stay open decisions in `docs/08-risks.md` and `docs/future-roadmap.md`). In
`docs/tasks/phase-4.md`'s `## Stories`, Story 2 (4.7.3, 4.3.3, 4.7.4's hot-reload part, 4.3.4) has the engine half of
its third line and continues with **4.3.4 (source-mapped errors)**, the next task: the order stays numeric, 4.4.1
after it. Story 1 has half of its first line (the JavaScript and source map exist; 4.4.1 makes a script attachable and
adds `AssetKind::Script`). Story 3 (4.4.2, 4.4.3, 4.4.4, 4.2.2, 4.3.2) has its `.d.ts` and cook lines done. No story's
demo has run.

**Three phase gates are still open**, each owing its deliverable rather than code:
- **Phase 0**: macOS-PASS, held open for the Windows/Linux 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`).
- **Phase 3**: all seven epics closed in code; the gate artifact `samples/phase-3-content/` does not exist, and the
  gate's audible half has never been heard.
- **Phase E**: all 24 tasks merged; the gate artifact `samples/phase-E-editor/` does not exist, and E.1.5's validation
  page has never been run.

No Windows or Linux validation pass exists for any task in any phase. The validation debt is itemised in the
"Validation debt" section of `docs/08-risks.md`.

**Phase E is lettered, not fractioned.** `3.5` and `3.5.1`/`3.5.2` are already Phase 3's skeletal-animation
epic and its tasks, and numbering is append-only, so a "Phase 3.5" would collide with referenced numbers.
In Notion its `Phase #` is `3.5` — a sort key, not an identifier.

**Where things live.**
- Per-task history (what shipped, what was left out, every trap and dead end) and the merged-task index (PR, merge sha
  and macOS verdict of every task) are in `docs/10-engineering-log.md`. Grep it before re-deriving anything; never read
  it whole.
- The standing rules that govern new work live in `.claude/rules/`, one file per area. Each loads when a file matching
  its `paths` is read or edited, so open the files a change touches before relying on a rule.
- Risks, open decisions, unowned handoffs and the validation debt are in `docs/08-risks.md`.

| Rules file | Loads with | Holds |
|---|---|---|
| `boundary-guards.md` | `engine/**` sources, `runtime/**` | rule #3's eight backends, the guard/probe pairs, the audio subsystems |
| `build-and-guards.md` | `CMakeLists.txt`, `cmake/**`, `.github/**`, the boundary probes | guard design, the `find_package` boundary, generated sources, grep traps |
| `ci-portability.md` | every C++ source, CI workflows | what fails on MSVC, libstdc++ or the Linux clang-tidy lane alone |
| `tests.md` | `tests/**` | the test method, the sabotage method, measuring totals, case ids, fixtures |
| `numerics.md` | engine, editor, test and sample sources | floating-point facts |
| `render.md` | `engine/render`, `engine/rhi`, `engine/scene_render`, `shaders/`, the viewport's editor sources | GPU and rendering rules |
| `components.md` | `engine/scene*`, `engine/reflect`, `tools/reflect-gen`, the Inspector and its tests | built-in components, reflection, serialization |
| `scripting.md` | `engine/script/**`, the script tests | the quickjs-ng embedding |
| `cooked-assets.md` | `engine/assets`, `tools/cooker`, the cooker tests | cooked formats, the determinism manifest |
| `reflect-gen.md` | `tools/reflect-gen`, `engine/reflect` | the generator |
| `editor.md`, `editor-invariants.md`, `editor-projects.md` | `editor/**`, `tests/editor/**` | conventions, panels, focus, scenes and projects |
| `editor-assets.md` | the editor's asset, material and thumbnail sources | the AssetDatabase, browser, watcher, materials, thumbnails |
| `editor-importers.md` | the editor's importer sources | glTF, FBX, OBJ, Blender and Assimp import |

### The phase table

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE.** Gate reached, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics closed and macOS-validated; Windows/Linux rows pending for every task (`editor/VALIDATION.md`). Gate artifact: `samples/phase-2-editor-scene/` — data, deliberately not `add_subdirectory`'d. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** All seven epics (3.1–3.7) **CLOSED in code**. What is left is the gate below and the validation debt. |
| **Phase 3 gate** | Drop a rigged glTF/FBX in → PBR materials + shadows + a playing animation + **an audible sound**. The audible half exists in code as of 3.7.2 and **has never been heard on any platform.** |
| **Phase E** — Editor Experience | **OPEN.** Epics E.1–E.6 **CLOSED in code** — 24 of 24 merged, see the merged-task index in `docs/10` (E.6.4, `52a5b68`, the last). What is left is the gate below — its artifact `samples/phase-E-editor/` does not exist yet — and ONE unrun validation page (E.1.5), the whole of this OS's remaining Phase E risk. |
| **Phase E gate** | Open a project and land in the scene you were last editing, on a lit grid floor under a sky; create a Cube from the menu, drop a material on it and see it shade; aim a spot light with a visible gizmo; rename, move and delete assets without leaving the editor. Gate artifact: `samples/phase-E-editor/`. |
| **Phase 4** — Scripting | **OPEN.** Epic 4.1 **CLOSED** — 4.1.1 (#123) and 4.1.2 (#124) merged; Epic 4.2 **CLOSED** — 4.2.1 (#125) and 4.2.2 (#126) merged; Epic 4.3 **OPEN** — 4.3.1 (#127), 4.3.2 (#129) and 4.3.3 (#130) merged. Next: 4.3.4 (Story 2), then 4.4.1. |
| **Phase 4 gate** | Press Play in the editor: hot-reloaded TS or native C++ drives entities; press Stop: the scene restores exactly. Gate artifact: `samples/phase-4-scripting/` (the two-language demo). |

### Engine layers, in dependency order

`core` → `assets` → `audio` → `platform` → `rhi` → `render` → `reflect` → `scene` → `scene_render` →
**`scene_audio`** → `scene_serialize`, and **`script`** (on `core`, since 4.2.1 `scene`, and since 4.3.2 `assets`, PRIVATE), plus `/editor` (`aero_editor_core` +
`aero_editor`) and
`/tools` (`reflect-gen`, `shaderc`, `cooker`). **`/runtime` is still empty** — it arrives in Phase 5.

### Test inventory — measured, never remembered

**After 4.3.3 (#130)**, measured on both presets and on fresh build directories at `993e8f4` (the merge's second
parent; `main` had not moved, so the merge tree is the same).

| Measurement | Value |
|---|---|
| `ctest -N` | **232** in both presets; **219** shader-tools-OFF (exactly the 13 `shaderc.*` removed), **107** reflect-tools-OFF (119 `reflect-gen.*`, four doctest binaries and `typescript.clean` / `typescript.broken` removed), nothing added in either; `cooker.*` **81 / 81 / 81**; 4.3.3 added no entry |
| doctest, seven binaries | **1730 / 2500 / 372 / 40 / 73 / 14 / 28** (`aero_tests` +41: HR1–HR41); reflect-tools-OFF `aero_tests` **1716**, shader-tools-OFF **1590** |
| guards | math **636**, platform **96**, rhi **188**, scene **96**, golden-rule **190**, project-no-delete **A=8 B=113** (2 permitted), audio **11-3-62**, probes **7-64**, script **176-96-14** |
| `git ls-files` | `editor/src/*.cpp` **113**, `editor/include/aero/editor/*.hpp` **83** |

The seven doctest binaries, in order: `aero_tests`, `aero_editor_shell_test`, `aero_editor_imgui_test`,
`aero_scene_serialize_test`, `aero_editor_inspector_test`, `aero_reflect_meta_test`, `aero_reflect_json_test`.
**On a task that adds no component, only the binaries it writes cases into may move.** A moved
`aero_scene_serialize_test` or `aero_editor_inspector_test` means a component crept in; the two reflect
binaries generate from a fixture aggregator, not the built-in list. **A third guard moving on a task that adds
no target is a stop-and-find-out** — math and project-no-delete prong B move with new tracked C-family files
(prong B only for `editor/src/*.cpp`); platform, rhi, scene, golden-rule and script count engine and runtime files, so
they move only with new ones; audio's and probes' last figures count CMake files. 4.1.1, which added a target, moved
every guard's figures but project-no-delete's and opened script's; 4.1.2, which added files to `engine/script` and
none elsewhere in `engine/`, moved math, rhi, golden-rule and script; 4.2.1, which added files to `engine/script`
and `tests/` and no target, moved the same four; 4.2.2, which added `editor/src/script_types.cpp`, its header and
two test files, moved math and project-no-delete's B, and audio's and probes' last figures with its two new `.cmake` files;
4.3.1, which added `esbuild_tool` and `script_transpiler` to the editor, two test files and `cmake/esbuild.cmake`, moved
math, project-no-delete's A (its forbidden list gained `script_transpiler.cpp`) and B, and audio's and probes' last figures;
4.3.2, which added seven engine files (two public headers, two internal headers, three sources, two of them on
`engine/script`'s private side), three test files and no target, moved math, platform, rhi, scene, golden-rule and script;
4.3.3, which added two files to `engine/script` (`src/vm_scripts.cpp` and the internal `script_carry.hpp`), one test
file and no target, moved math, rhi, golden-rule and script, as 4.1.2 did.

How to measure and compare these figures: `.claude/rules/tests.md`.

> **Before touching a subsystem, read its entry in `docs/10-engineering-log.md`.** That file is the full
> per-task history: what shipped, what was deliberately left out, the traps found, and the dead ends that must
> never be retried (the lavapipe LSan leak, `LD_PRELOAD`, vcpkg's `sdl3-shadercross` on macOS, …). It is
> deliberately *not* auto-loaded — grep it before re-deriving anything.

**Maintenance.** Rewrite "Current state" as the position moves; never append history to it. A merged task adds its row
to the merged-task index and its entry to `docs/10-engineering-log.md`, updates the phase table and "Current state",
records new risks and unowned handoffs in `docs/08-risks.md`, and puts a new standing rule into the rules file for its
area, never here. Keep this file under about 250 lines: it loads into every session, and it has swollen three times
(to 207 k, 121 k and 134 k characters) when history or rules accumulated in it.

## Build, test & lint

A fresh clone needs the vcpkg submodule: `git clone --recurse-submodules`, or `git submodule update --init` after a plain clone.

```bash
cmake --preset macos-debug          # or macos-release; windows-*/linux-* are gated to their host
cmake --build --preset macos-debug
ctest --preset macos-debug          # prefix AERO_REQUIRE_GPU=1 to rehearse the CI ratchet
```

Six presets — `{macos,windows,linux}-{debug,release}` — each gated to its host OS by preset conditions, building into `build/<preset>/`.

- **`*-debug`** — ASan/UBSan via `AERO_ENABLE_SANITIZERS` → `cmake/sanitizers.cmake` (Windows: ASan only, MSVC has no UBSan).
- **`*-release`** — `AERO_ENABLE_PROFILING=ON` links the pinned Tracy 0.13.1 client into `aero::profiling` and defines `AERO_PROFILING_ENABLED`. Tracy is dev-builds-only — never Debug, never the runtime — enforced by default-OFF plus link gating, not convention. Use the `AERO_PROFILE_*` macros from `<aero/core/profiler.hpp>`, which no-op when profiling is off.
- **`AERO_REQUIRE_GPU`** — unset, GPU-gated tests skip loudly; set (as all three CI lanes do), a missing GPU is a hard failure.
- **`-DAERO_SHADER_TOOLS=OFF` / `-DAERO_REFLECT_TOOLS=OFF`** — escape hatches for constrained or offline machines. CI never sets them OFF; both must stay green.

**The first configure is slow and needs network.** It bootstraps vcpkg, builds SDL3 from source, and builds the SDL_shadercross toolchain from source into `~/.cache/aero-engine/shadercross` — once per machine, not per preset or worktree. Later configures are instant and fully offline. `reflect-gen` additionally needs a system LLVM 18 (`brew install llvm@18`, `apt install libclang-18-dev llvm-18-dev`, or `choco install llvm --version=18.1.8`); override discovery with `-DAERO_LLVM_ROOT=…`.

**Pinning invariant:** `builtin-baseline` in `vcpkg.json` and the `/vcpkg` submodule commit are the **same SHA**. Bump them together, never separately — a CI job asserts it.

**Lint locally before pushing.** clang-format alone does not catch what CI's clang-tidy rejects, and a local format pass has been proven not to predict CI:

```bash
clang-format-18 --dry-run --Werror <files>
SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path) \
  clang-tidy-18 -p build/macos-debug --warnings-as-errors='*' <files>
```

CI (GitHub Actions; macOS + Windows + Ubuntu) configures, builds and tests all six presets on every push to `main` and every PR, plus a `lint` job running clang-format, clang-tidy, the vcpkg-baseline guard, and the **nine** architecture guards in `.github/scripts/`: `check-math-boundary.sh`, `check-platform-boundary.sh`, `check-rhi-boundary.sh`, `check-scene-boundary.sh`, `check-golden-rule.sh`, `check-project-no-delete.sh`, `check-audio-boundary.sh`, `check-boundary-probes.sh`, `check-script-boundary.sh`. The count is measured, not remembered — `ls .github/scripts/`.

Path-scoped working rules live in `.claude/rules/` and load only when the matching files are opened — the table in "Current state" lists them.

## The three project rules (non-negotiable)

1. **Golden architecture rule** — the editor depends on the engine; the engine NEVER depends on the editor. Enforced by CI guards: no `#include` under `/engine` or `/runtime` may reference `/editor`, and the runtime binary must never link ImGui, Assimp, or libclang.
2. **Deliverable rule** — every phase ends in something playable or usable; a phase without a deliverable is not finished.
3. **Boundary rule** — no third-party type crosses the engine's public API. Not `glm::vec3`, not an SDL handle, not a miniaudio type. Everything lives behind the engine's own types (e.g. `engine::Vec3` wraps GLM inside `core/math`).

## Architecture (planned — full detail in docs/03)

Layers; each depends only on layers below it, and `core` depends on nothing:

- `/editor` (3 desktop platforms) — Dear ImGui, panels, gizmos, undo/redo, **importers**, exporter
- `/engine` (5 platforms) — subsystems in dependency order: `core` (handles, math, jobs, log, VFS, time) → `platform` (SDL3 wrapper) → `rhi` (SDL_GPU wrapper — the escape hatch for future ray tracing; treat as sacred) → `render`, `scene` (EnTT), `physics` (Jolt 3D / Box2D 2D), `audio` (own graph → miniaudio backend), `assets`, `script` (quickjs-ng), `reflect`
- `/runtime` (5 platforms) — game loop, `.pak` loading, per-platform entry points
- `/tools` — `reflect-gen` (libclang codegen), `shaderc` (HLSL → DXIL/MSL/SPIR-V via SDL_shadercross), `cooker`, `packager`

Load-bearing decisions (rationale in `docs/02-adrs.md` — settled ADRs are not re-litigated):

- **Handles, not pointers** for every resource: `Handle<Tag>` = `{index: u32, generation: u32}`. Never manual `new`/`delete`; RAII everywhere. ASan/UBSan run in CI on every commit.
- **Reflection is the spine (ADR-004).** `tools/reflect-gen` parses `[[engine::component]]` annotations with libclang and generates four consumers: `entt::meta` registration (inspector), JSON/binary serialization (scenes on disk), quickjs-ng bindings (script API), and `.d.ts` files (VSCode autocomplete). Write a component once; all four stay in sync. Built in Phase 1, before anything depends on it. Start with the minimal subset: plain structs + primitives + `Vec3`/`Quat`.
- **Asset flow:** source files (`.blend`/`.fbx`/`.obj`/…) → importer (editor-only: ufbx, tinyobjloader, Assimp, Blender invoked as external CLI) → canonical **glTF 2.0** + `.meta` file (stable GUID, committed to git) → cooker (per-platform binaries: KTX2/Basis textures, GPU buffers, script bytecode, compiled shaders) → packager (`game.pak` + precompiled runtime).
- **Two export models (ADR-008).** TypeScript projects: instant export — cooked assets packed next to a CI-precompiled runtime; user needs no toolchains (the Godot model). C++ projects: native compile + link per platform (the Unreal model). The language is fixed per project at creation and never mixed.
- **Dependency placement is an invariant:** ImGui, ImGuizmo, Assimp, ufbx, tinyobjloader, stb_image are editor/tools-only; libclang is tools-only; Tracy is dev-builds-only. An editor-only dependency linked into `/engine` or `/runtime` is an architecture bug, not an optimization issue.

## Conventions (docs/04)

- **C++20** baseline; C++23 features only where Clang, MSVC, and GCC all support them.
- Naming: `PascalCase` types, `camelCase` functions/variables, `SCREAMING_SNAKE_CASE` compile-time constants, `snake_case` files/directories. Everything under `engine::` (subsystem sub-namespaces like `engine::rhi` as needed).
- Headers: `#pragma once`; public headers expose only engine types; no `using namespace` in headers.
- Errors: no exceptions across public API boundaries — explicit result/status types, asserts in debug; handles return invalid rather than throw.
- Git: trunk-based; short-lived feature branches merged to `main` via PR even solo; `main` is always green. Conventional-commit style (`feat:`, `fix:`, `refactor:`, `docs:`, `build:`, `ci:`, `test:`), imperative mood. Phases and releases are tagged. `.meta` files are committed; cooked/build output is gitignored. **Do not add a `Co-Authored-By` trailer to commits.** **Merge with a MERGE COMMIT (`gh pr merge <n> --merge`), never a squash** — the plans deliberately split a task into one green commit per step, and a squash discards every one of them in favour of a single new GitHub-authored commit, which both loses the bisectable per-step history and drops the contributions (GitHub counts only commits that land on the default branch). PRs #22–#26 used merge commits, #38–#45 were squashed (the regression), and #46 onward restores merge commits.
- CI (GitHub Actions, macOS + Windows + Ubuntu, from commit #1): Debug build with ASan/UBSan + Release build, codegen steps, doctest unit tests, the **nine** architecture guards that exist today — math-boundary (no `<glm/...>` outside `engine/core/src/math/glm_backend.cpp`, the single allowlisted file; **not** the looser "outside `core/math`", which would license GLM in the public math headers), platform-boundary, rhi-boundary, scene-boundary, golden-rule, project-no-delete, and since 3.7.3 audio-boundary (the no-vcpkg property of `engine/assets`/`engine/audio`/`engine/scene_audio`'s CMakeLists + a miniaudio token ban over both audio roots, sources included) and boundary-probes (every `aero_*_boundary_probe` links exactly one `aero::` library, `PRIVATE`, with the probe set derived from `tests/CMakeLists.txt`), and since 4.1.1 script-boundary (no quickjs token in a public engine header, none in `engine/`/`runtime/` outside `engine/script/src`); each created by its owning task, see `docs/04` — and format/lint checks. *(Runtime-purity is planned for its owning phase (5.2.2) and does **not** exist yet; do not cite it as live.)*

## Scope discipline

`docs/06-scope-and-non-goals.md` is load-bearing. Before anything is added to v1.0 it must pass all three: (1) serves the edit → script → play → export loop; (2) needed by a real shippable game in Phase 5; (3) maintainable solo without derailing the 20–32-month horizon. Explicit v1 non-goals include ray tracing/mesh shaders, Nanite-style geometry, baked GI, terrain, visual scripting, networking, web/WASM export, a mobile editor, and FMOD/Wwise in core. Deferred items live in `docs/future-roadmap.md`.

Some decisions are deliberately deferred (`docs/08-risks.md`): forward+ vs deferred rendering (Phase 8, with Tracy data), ImGui's long-term role, migration to C++26 `std::meta`, GLM → RTM swap. Do not resolve them early.

## Documentation map

- `docs/` is the source of truth for scope and architecture. Execution tracking lives in Notion ("Aero Engine — Build Tracker", linked in README): three linked databases (Phases → Epics → Tasks); phases/epics/tasks are rows, **subtasks are to-do checklists inside their task's page**. On any conflict, the docs win and Notion gets corrected.
- **When a validation status changes, update this file's state block as well as the task's validation page** — the state block is the authoritative summary of where every task's gate stands.

| Doc | Contents |
|---|---|
| `docs/00-overview.md` | Objective, rules, platform matrices, stack table, horizon |
| `docs/01-tech-stack.md` | Choice per layer, licenses (MIT-compatibility is a hard requirement), accepted stack limits |
| `docs/02-adrs.md` | ADR-001…008 with discarded alternatives |
| `docs/03-architecture.md` | Layers, repo layout, handles, asset flow, export models, reflection consumers |
| `docs/04-conventions-setup.md` | C++ style, git, CMake/vcpkg, CI guards, testing strategy |
| `docs/05-roadmap.md` | Phases 0–8 with deliverable gates |
| `docs/06-scope-and-non-goals.md` | What v1.0 is and is not |
| `docs/07-tasks.md` | Task index: legend, numbering conventions, per-phase links |
| `docs/tasks/phase-{0..8}.md`, `docs/tasks/phase-E.md` | Full breakdown per phase: epics → tasks → subtasks, each task with goal + deliverable. **Phase E** (Editor Experience) executes between 3 and 4 and is lettered because `3.5` is already taken — see `docs/07`'s numbering conventions |
| `docs/08-risks.md` | Risk register, open + resolved decisions, unowned handoffs |
| `docs/09-file-formats.md` | Scene schema v1 (entity/components/version), canonical form, versioning & evolution policy |
| `docs/10-engineering-log.md` | **Per-task build history** — what each task shipped and deliberately did not, traps found, dead ends never to retry, and per-task build/dependency impact. Not normative; not auto-loaded. Read the relevant entry before touching a subsystem. |
| `docs/future-roadmap.md` | v2 / v3–v4 deferred features |
| `.claude/rules/*.md` | Path-scoped working rules, loaded only when matching files are opened; the table in "Current state" lists the files and what each holds |
