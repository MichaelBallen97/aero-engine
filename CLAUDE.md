# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Aero Engine — an open-source (MIT), cross-platform 3D game engine with an editor and per-project TypeScript **or** C++ scripting. Solo project, started July 2026. The goal is core-workflow parity with Unity/Godot (edit → script → play → export), explicitly **not** feature parity. 3D-first; 2D arrives in Phase 7.

Two platform matrices, never to be conflated: the **editor** runs on macOS/Windows/Linux only; the **runtime** (exported games) targets those three plus iOS and Android. The editor never runs on mobile — no touch UI, no adaptive layouts.

## Current state — read this first

**Phase 2 (Editor) is in progress.**

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | Epic 2.1 (ImGui integration) **CLOSED in code**, macOS human-validated PASS; Windows/Linux rows pending (`editor/VALIDATION.md`). Epic 2.2 (Core panels) **CLOSED in code** — 2.2.1 Hierarchy, 2.2.2 Inspector, 2.2.3 Viewport, 2.2.4 Asset browser and 2.2.5 Log/console panel all landed; **no `PlaceholderPanel` remains**. 2.2.1–2.2.4 are macOS human-validated PASS with Windows/Linux rows pending; **2.2.5 is macOS human-validated PARTIAL — 11 of 15 rows PASS, 4 BLOCKED because the editor emits no log record after startup**, so they cannot be run at all. The gate stays open pending a triggerable runtime log source and a non-macOS pass (`editor/VALIDATION.md`) — **three** triggerable sources now exist: 2.3.3's degenerate/non-finite-transform gizmo WARN, 2.4.1's Debug-build `⌘Z` (one `DEBUG editor: undo 'Transform' (…)` record per undo), and 2.4.2's own Debug-build `⌘Z` with a varied label (`Delete Entity`, `Rename`, `Add MeshRenderer`, …) — but the 2.2.5 rows themselves have not been re-run against any of the three yet. **Epic 2.3 (Manipulation) is CLOSED in code** — 2.3.1 Editor camera, 2.3.2 Selection & picking and 2.3.3 ImGuizmo transform gizmos all landed. All three are macOS human-validated PASS — 2.3.1 19/19, 2.3.2 21/21 and 2.3.3 **24/24** applicable rows (2.3.3's row 23 is Linux-only) — with Windows/Linux rows pending for all three (`editor/VALIDATION.md`). The Viewport is now the whole edit → select → move loop: orbit/pan/dolly/fly navigation (2.3.1), click-to-select with a wireframe highlight (2.3.2), and translate/rotate/scale gizmos in local or world space with hold-to-snap (2.3.3), writing through a new `transform_ops` seam. **Epic 2.4 (Undo/redo) is CLOSED in code — both 2.4.1 (Command stack) and 2.4.2 (Property-set + structural commands) landed.** 2.4.1 shipped the `Command`/`CommandStack` backbone (explicit merge chain, 128-entry bounded history, `Ctrl+Z`/`Ctrl+Shift+Z`, live Edit-menu items) and `TransformCommand` wrapping `transform_ops`, with the Viewport gizmo routed through it — **macOS human-validated PASS 16/16 (2026-07-30)**, confirming the code-review round's AC-16 ordering fix end to end; Windows/Linux rows pending. 2.4.2 removes the last seventeen direct scene writes under `editor/src/`: a reflected `SetFieldCommand` covering every component field (the Inspector's seven field arms plus add/remove-component), and five structural entity commands (create / delete / duplicate / reparent / rename) built on one new engine primitive, `World::recreate(Entity)`, and a `SubtreeSnapshot`/`ComponentSnapshot` facility that restores a deleted entity's *original handle* — the whole reason `⌘Z` after "move the Cube, delete it, undo" also undoes the move, not just the delete. **This satisfies Epic 2.4's own Definition of Done and Goal ("nothing mutates the scene directly") in code** — the only direct scene writes left under `editor/src/` are inside the three `_ops` TUs and the three command TUs. **2.4.2's macOS human pass has not been run yet** (`editor/VALIDATION.md`, `editor/validation/2.4.2-property-set-structural-commands.md`); two things are worth reading before trusting the code alone: AC-10 (tag/empty component values round-tripping through a delete → undo) is a **known, currently-unfixable gap** — no built-in component is a tag, so nothing in this tree can register one into `SubtreeSnapshot`'s private World, and it stays open until Phase 4/5 project-defined components exist — and the Inspector's own application of the D17 merge-chain gate pair (open breaks the chain *before* a frame's push, close breaks it *after*) has **no tier-0 or GPU-gated proof at all**, only a policy-level one (`field_command_test.cpp`'s P8, against a bare `CommandStack`) — the human pass is the *only* thing that can catch a regression there. |
| **Next task** | **2.5.1 Save/load/new from editor** — see `docs/tasks/phase-2.md`. **INV-6 now has teeth**: New/Open Scene must `clear()` the `CommandStack` in the *same operation* that replaces the `World` — with `SubtreeSnapshot`s living inside history entries since 2.4.2, a stack driven against a replaced World would try to `recreate()` handles that mean nothing there, not merely show a stale label. |

Engine layers that exist today, in dependency order: `core` → `platform` → `rhi` → `render` → `reflect` → `scene` → `scene_render` → `scene_serialize`, plus `/editor` (`aero_editor_core` + `aero_editor`) and `/tools` (`reflect-gen`, `shaderc`). `/runtime` is still empty — it arrives in Phase 5. `engine/scene` gained one primitive at task 2.4.2, `[[nodiscard]] Entity World::recreate(Entity)` — the only engine change Epic 2.4 needed; everything else lives in `/editor`.

Test inventory at HEAD: **94** ctest entries with tools ON, **5** with `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, **18** with `-DAERO_REFLECT_TOOLS=OFF` alone — all three unchanged by tasks 2.4.1 and 2.4.2, including 2.4.2's own code-review round (new coverage rides existing targets/TUs, not new ctest entries): `aero_tests` **356 → 363** (`scene_test.cpp` W1–W7, `World::recreate`); `aero_editor_shell_test` **169 → 196 → 199** (2.4.1: `command_stack_test.cpp` C1–C18 plus the review round's C19–C21, `transform_command_test.cpp` T1–T9) **→ 236 → 237 → 240** (2.4.2: `hierarchy_test.cpp` R1–R4, `scene_snapshot_test.cpp` N1–N12+RL1 then **+N13** from the sabotage pass **+N14** from the code-review round, `structural_commands_test.cpp` X1–X20 **+X21/+X22** from the code-review round); `aero_editor_inspector_test` **14 → 22** (2.4.2's `field_command_test.cpp` P1–P8, on the `AERO_REFLECT_TOOLS`-gated target); `aero_editor_imgui_test` **24 → 30** (2.4.1: I1–I6) **→ 35** (2.4.2: I7–I11); `aero_editor_core` **25 → 27** (2.4.1) **→ 30** (2.4.2) sources. 2.4.1's review round fixed a blocking AC-16 defect (a release-frame gizmo drag recorded as two history entries, not one — see `docs/10-engineering-log.md`'s 2.4.1 code-review-round entry) and corrected four sabotage-verdict rows in `editor/VALIDATION.md` that the first pass had gotten wrong. 2.4.2's implementation pass ran four of its plan's twenty mandatory sabotage seeds and found one new `NOLINT` where the plan predicted zero; **a follow-up sabotage pass ran the other sixteen plus all three second-order checks, so §V4 is now discharged** — nine seeds did not behave as predicted (three with no discriminator at all), and the one real coverage gap it exposed, `SubtreeSnapshot::restore`'s untested phase-A rollback, is closed by `scene_snapshot_test.cpp`'s new N13. **A further code-review round then found a tenth, BLOCKING defect the twenty-seed matrix could not see** (`placeAt`/`RootOrder::insert`'s position-restore replay order, three call sites, plus a related capture-ordering bug in `ReparentCommand::redo` found only while writing its mandated test) — fixed, covered by three new tests (N14/X21/X22) and a new sabotage seed (S20), gate re-measured green. Full verdict table in `editor/validation/2.4.2-property-set-structural-commands.md`; all ten findings in `docs/10-engineering-log.md`'s 2.4.2 entry (nine from the sabotage pass, the tenth from the code-review round). Counts diverge by OS (Windows skips `golden-rule.include_scan_e2e`), so never assume one — measure with `ctest -N`.

> **Before touching a subsystem, read its entry in `docs/10-engineering-log.md`.** That file is the full per-task history: what shipped, what was deliberately left out, the traps found, and the dead ends that must never be retried (the lavapipe LSan leak, `LD_PRELOAD`, vcpkg's `sdl3-shadercross` on macOS, …). It is deliberately *not* auto-loaded — grep it before re-deriving anything.

**Maintenance:** rewrite this section as the position moves. Per-task history is appended to `docs/10-engineering-log.md`, never here — that is what grew this file to 175k characters once already.

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

CI (GitHub Actions; macOS + Windows + Ubuntu) configures, builds and tests all six presets on every push to `main` and every PR, plus a `lint` job running clang-format, clang-tidy, the vcpkg-baseline guard, and the five architecture guards in `.github/scripts/`: `check-math-boundary.sh`, `check-platform-boundary.sh`, `check-rhi-boundary.sh`, `check-scene-boundary.sh`, `check-golden-rule.sh`.

Path-scoped working rules live in `.claude/rules/` and load only when the matching files are opened — boundary guards, reflect-gen, CI portability, and editor conventions.

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
- CI (GitHub Actions, macOS + Windows + Ubuntu, from commit #1): Debug build with ASan/UBSan + Release build, codegen steps, doctest unit tests, the five architecture-guard tests (math-boundary — no `<glm/...>` outside `engine/core/src/math/glm_backend.cpp`, the single allowlisted file; **not** the looser "outside `core/math`", which would license GLM in the public math headers —, RHI-boundary, golden-rule, audio-boundary, runtime-purity; each created by its owning task, see `docs/04`), and format/lint checks.

## Scope discipline

`docs/06-scope-and-non-goals.md` is load-bearing. Before anything is added to v1.0 it must pass all three: (1) serves the edit → script → play → export loop; (2) needed by a real shippable game in Phase 5; (3) maintainable solo without derailing the 20–32-month horizon. Explicit v1 non-goals include ray tracing/mesh shaders, Nanite-style geometry, baked GI, terrain, visual scripting, networking, web/WASM export, a mobile editor, and FMOD/Wwise in core. Deferred items live in `docs/future-roadmap.md`.

Some decisions are deliberately deferred (`docs/08-risks.md`): forward+ vs deferred rendering (Phase 8, with Tracy data), ImGui's long-term role, migration to C++26 `std::meta`, GLM → RTM swap. Do not resolve them early.

## Documentation map

- `docs/` is the source of truth for scope and architecture. Execution tracking lives in Notion ("Aero Engine — Build Tracker", linked in README): three linked databases (Phases → Epics → Tasks); phases/epics/tasks are rows, **subtasks are to-do checklists inside their task's page**. On any conflict, the docs win and Notion gets corrected.

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
| `docs/tasks/phase-{0..8}.md` | Full breakdown per phase: epics → tasks → subtasks, each task with goal + deliverable |
| `docs/08-risks.md` | Risk register, open + resolved decisions |
| `docs/09-file-formats.md` | Scene schema v1 (entity/components/version), canonical form, versioning & evolution policy |
| `docs/10-engineering-log.md` | **Per-task build history** — what each task shipped and deliberately did not, traps found, dead ends never to retry, and per-task build/dependency impact. Not normative; not auto-loaded. Read the relevant entry before touching a subsystem. |
| `docs/future-roadmap.md` | v2 / v3–v4 deferred features |
| `.claude/rules/*.md` | Path-scoped working rules, loaded only when matching files are opened (boundary guards, reflect-gen, CI portability, editor) |
