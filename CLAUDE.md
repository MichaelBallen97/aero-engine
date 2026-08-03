# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Aero Engine — an open-source (MIT), cross-platform 3D game engine with an editor and per-project TypeScript **or** C++ scripting. Solo project, started July 2026. The goal is core-workflow parity with Unity/Godot (edit → script → play → export), explicitly **not** feature parity. 3D-first; 2D arrives in Phase 7.

Two platform matrices, never to be conflated: the **editor** runs on macOS/Windows/Linux only; the **runtime** (exported games) targets those three plus iOS and Android. The editor never runs on mobile — no touch UI, no adaptive layouts.

## Current state — read this first

**Phase 2 (Editor) is COMPLETE — gate met 2026-08-02.** All six epics closed in code, every task
macOS human-validated with no open FAIL, and the gate artifact committed at
`samples/phase-2-editor-scene/` (a project + 4-entity scene authored entirely through the editor, with
the save → New Scene → Open Scene round trip confirmed). A whole-phase audit (2026-08-02) then found
and fixed two silent data-loss paths, a never-absolute project root, and four stale documentation
claims — full detail in `docs/10-engineering-log.md`. **Phase 3 (Asset Pipeline & 3D Content) is now
OPEN. Epic 3.1 (AssetDatabase · assets) is in progress — task 3.1.1 (GUIDs + `.meta` files) is
CLOSED in code**, sabotage-proven (26/26 seeds plus all 3 mandatory second-order checks run and
confirmed), and awaiting only its human validation pass. **3.1.2 (import cache) is next.**
**Carried-forward debt, unchanged by this task and explicitly not part of any gate:** no Windows or
Linux human pass exists for any of the thirteen Phase 2 tasks, and Phase 0's gate is still held open
on Windows/Linux 60 fps sign-off. That is platform-validation debt spanning three phases now, and it
is worth scheduling as work of its own — the 2.2.5 lesson, one scale up.

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics (2.1 Editor shell, 2.2 Core panels, 2.3 Manipulation, 2.4 Undo/redo, 2.5 Scene I/O, 2.6 Project system v0) CLOSED in code and macOS human-validated PASS, with Windows/Linux rows pending for every task (`editor/VALIDATION.md`). The whole-phase audit (2026-08-02) fixed two silent data-loss paths, a project root that was never made absolute, two CI false-greens, and four stale documentation claims. Full per-task and per-epic history — every defect, every sabotage matrix, every deviation — lives in `docs/10-engineering-log.md`'s Phase 2 entries; this row is deliberately a summary, not a duplicate. |
| **Phase 2 gate** | **MET 2026-08-02.** `samples/phase-2-editor-scene/` holds a project and a 4-entity scene authored entirely through the editor, with the save → New Scene → Open Scene round trip confirmed (`samples/phase-2-editor-scene/VALIDATION.md`); provenance is recorded there rather than asserted, since a hand-written `scene.json` is byte-identical to a real one and no test tier can tell them apart. Deliberately NOT `add_subdirectory`'d — this artifact is data (a provenance proof of the editor), not a compile-proof of engine code. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** Epic 3.1 (AssetDatabase · assets) in progress: **3.1.1 (GUIDs + `.meta` files) CLOSED in code.** `engine::Guid` (`engine/core`, beside `Handle`) — a 16-byte trivially-copyable POD, seedable `splitmix64` generator, no test anywhere touches an entropy source. The `.meta` v1 format (`editor/include/aero/editor/asset_meta.hpp` + `.cpp`) and its pure lifecycle planner `planAssetMetas`, provable from a `std::vector` literal with no disk touched. `AssetDatabase::rescan` (`asset_database.hpp`/`.cpp`) — a five-phase, `<filesystem>`-free, non-recursive scan composed entirely from 2.2.4's `listDirectory` and 2.5.1/2.6.1's `text_file`; it never logs (INV-A3) and never throws. **This task ENDS the four-task empty-`engine/`-diff streak** (2.5.1, 2.5.2, 2.6.1, 2.6.2) deliberately and minimally: one new header, one new source, one CMake line, zero new dependencies. The Asset Browser (2.2.4) now filters `.meta` rows at cache-fill time and shows the selected file's elided GUID / `no .meta` / `invalid .meta` in its footer. The sixth architecture guard, `check-project-no-delete.sh`, widened from **three files to five** — `asset_meta.cpp` and `asset_database.cpp` join the allowlist, because "an invalid `.meta` is never overwritten" (D7) and "an orphan is never deleted" (D8) are the same class of safety-critical, unreachable-by-ordinary-test rule 2.6.1's seed S11 already documented; the script's name stays narrower than its widened scope on purpose (a rename would touch the workflow YAML, the ctest case name, `CLAUDE.md` and `.claude/rules/editor.md`). **Sabotage: all 26 seeds (S1–S26) plus all 3 mandatory second-order checks run and confirmed against the real built binaries.** Seven seeds matched their prediction exactly (S7, S8, S9, S10, S18, S19, S20); **six** were confirmed non-discriminators (S4 — `parseGuid`'s length-31 acceptance is masked by `std::string`'s guaranteed null terminator and ASan's non-tracking of a global string-literal's storage; S6 — the nil-retry loop is provably dead code, a bijection cannot map two distinct inputs to zero; S23 — a reference-bound panel member reddens nothing on this compiler, a real coverage gap, not proof the reference form is safe; S24 — `byGuid` indexing an Invalid record is masked by `findByGuid`'s own independent nil-guid guard, which runs before the corrupted map is even consulted; S25/S26 — no test reads the footer / builds an unreadable sub-directory); S17's own predicted contingency (this machine's case-insensitive APFS volume makes the `AssetDatabase`-layer case unreachable) also came true exactly as written; the remaining **twelve** (S1–S3, S5, S11–S16, S21, S22) reddened a real but differently-shaped set than predicted, each one a genuine finding recorded in `docs/10-engineering-log.md`. (7 + 6 + 1 + 12 = 26 — the arithmetic is exact, not approximate.) **One MAJOR, confirmed deviation:** seed S22 (deleting the `tick()` reconcile block) was predicted to redden only I28; it actually reddens FOUR GPU-tier cases (I21, I27, I28, I29), because the block bundles 2.6.1's pre-existing panel-root reconcile (D10) with this task's new database-scan reconcile (D12) — 3.1.1 rewrote the existing block rather than adding a second one beside it, so the two cannot be deleted independently. Full sabotage matrix, every deviation, and four build-time findings (the `MAX_ASSETS` coverage gap, an accessor/member naming collision, the `setDatabase` gating ambiguity, and an `AD21` test-design bug involving `writeTextFileAtomic`'s own working-file path) are in `docs/10-engineering-log.md`'s 3.1.1 entry. Local-only human validation page (`editor/validation/3.1.1-guids-and-meta-files.md`, fourteen rows) is not committed and not yet run. **3.1.1's code-review round (PR #65) found 7 findings, 2 BLOCKING:** both caught only by Windows CI — a missing `<array>` include (MSVC-only) and MSVC's `std::unordered_map` move constructor not being `noexcept` (reddening `AssetDatabase`'s aggregate static_assert; fixed with the plan's own documented sorted-vector fallback, `byPath`/`PathHash` deleted entirely) — plus a third BLOCKING finding local review caught first: a `Created`/`Repaired` write with no case-insensitive collision guard could silently destroy a valid orphaned `.meta` on any case-insensitive filesystem (APFS, NTFS), now refused and reported in a new `AssetScanReport::writeConflicts` category (AD31). Also fixed: the reconcile's `||` short-circuit left the Asset Browser panel's own Refresh flag undrained (I30, a mechanical source-text proof — this target cannot synthesize a UI click); AD8's D6 proof strengthened against timestamp-granularity false-passes; and four stale documentation claims (the `AM`/`AP` id ranges, the `setDatabase` justification, a naming-collision misattribution, and the sabotage classification's arithmetic, which dropped S17 and mis-filed two non-discriminators as differently-shaped) corrected here, in `.claude/rules/editor.md` and in `docs/10-engineering-log.md`'s own code-review-round entry. Post-round: `aero_editor_shell_test` **468**, `aero_editor_imgui_test` **52**, both tools-OFF configurations still **444/444**. |
| **Next task** | **3.1.2 (import cache)** — see `docs/tasks/phase-3.md`. Depends on 3.1.1. The one carried-forward item is **platform-validation debt, now spanning three phases**: no Windows or Linux human pass exists for any of the thirteen Phase 2 tasks, and Phase 0's gate is still held open on Windows/Linux 60 fps sign-off, plus 3.1.1's own Windows/Linux rows (pending, none of the thirteen human rows above them either). Schedule it as work of its own rather than as a ride-along row — 2.2.5's lesson at phase scale. |

Engine layers that exist today, in dependency order: `core` (gained `guid.hpp`/`guid.cpp` at task
3.1.1, beside `handle.hpp`) → `platform` → `rhi` → `render` → `reflect` → `scene` → `scene_render` →
`scene_serialize`, plus `/editor` (`aero_editor_core` + `aero_editor`) and `/tools` (`reflect-gen`,
`shaderc`). `/runtime` is still empty — it arrives in Phase 5. `engine/assets/` is still just
`.gitkeep` — deliberately unopened until a **runtime** consumer exists (Phase 5's pak table); the
editor's `AssetDatabase` (task 3.1.1) lives entirely in `/editor`, not `/engine/assets`.
`engine/scene` gained one primitive at task 2.4.2, `[[nodiscard]] Entity World::recreate(Entity)` —
the only engine change Epic 2.4 needed. Tasks 2.5.1, 2.5.2, 2.6.1 and 2.6.2 all needed **no** engine
change at all — a four-task streak task **3.1.1 deliberately ends**: its `engine/` diff is exactly
three paths (`engine/core/CMakeLists.txt`, `engine/core/include/aero/core/guid.hpp`,
`engine/core/src/guid.cpp`), the smallest engine change this project's convention allows short of
none at all. `/editor` gains four new pairs across 2.6.2 and 3.1.1: `project_settings.{hpp,cpp}` /
`project_settings_panel.{hpp,cpp}` (2.6.2), and `asset_meta.{hpp,cpp}` / `asset_database.{hpp,cpp}`
(3.1.1, the `.hpp`s under `editor/include/aero/editor/`, the `.cpp`s under `editor/src/`).

Test inventory at HEAD, **re-measured, not carried forward**: **95** ctest entries with tools ON,
**6** with `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, **19** with `-DAERO_REFLECT_TOOLS=OFF`
alone — unchanged by task 3.1.1 or its code-review round, both of which register **zero** new ctest
entries (every new case lives inside an existing TU). `aero_tests` **389** (390's Phase-2-audit
baseline was 363 before 3.1.1's `guid_test.cpp`'s `GU1`–`GU26` — 26 new cases, all pinned-literal, no
entropy source touched by any test). `aero_editor_shell_test` **467 → 468** (Phase-2-audit baseline
390 → +77 at 3.1.1 landing: `AM1`–`AM27` the `.meta` format's naming/classification/parse/write
battery, `AP1`–`AP14` the pure planner, `AG1`–`AG6` the golden byte-fixpoint battery, `AD1`–`AD30` the
real-disk `AssetDatabase` scan battery — **the ranges above were mis-stated at landing as `AM1`–`AM40`/
`AP1`–`AP18`; re-measured with `--list-test-cases` rather than trusted from the plan's own prediction,
the standing lesson this project keeps re-learning** — **→ +1** in the code-review round: `AD31`
(finding 2's write-conflict guard) — confirmed **444/444** in BOTH freshly-rebuilt tools-OFF
configurations (`build/tools-off-3.1.1`, `build/reflect-off-3.1.1`; **6**/**19** `ctest -N` entries,
both unchanged), which is AC-17's whole claim — the format and planner need no serialization and are
present, not skipped, in every reduced configuration. `aero_scene_serialize_test` **23** and
`aero_editor_inspector_test`
**22**, both unchanged by 3.1.1. `aero_editor_imgui_test` **51 → 52** (Phase-2-audit baseline 48 → +3
at 3.1.1 landing: `I27`/`I28`/`I29`, the asset-scan-on-open, runtime-project-swap and manual-rescan
GPU-tier cases; **→ +1** in the code-review round: `I30`, finding 4's mechanical source-text proof).
`aero_editor_core` sources **41** (39 at the Phase 2 audit → +2:
`asset_meta.cpp`, `asset_database.cpp`) — **no new `target_link_libraries` entry**: `aero_editor_core`
already links `aero::core` PUBLIC and reaches the JSON layer through `aero::scene`.
`check-math-boundary.sh`'s scanned count: **246** at the Phase 2 audit **→ 249** (3.1.1 Step 1:
`guid.hpp`/`guid.cpp`/`guid_test.cpp`) **→ 252** (Step 2: `asset_meta.hpp`/`.cpp` +
`asset_meta_test.cpp`) **→ 255** (Step 3: `asset_database.hpp`/`.cpp` + `asset_database_test.cpp`;
unchanged through Steps 4–8, since docs are not C-family) — measured after `git add` at every step
boundary, never assumed. Guard count stays **six**, but `check-project-no-delete.sh`'s own scope
widened at task 3.1.1 from three files to **five** (D7/D8 — an invalid `.meta` is never overwritten,
an orphan is never deleted — the same class of rule 2.6.1's `createProject` rollback branch already
had this guard for); the script's final line now reads "5 files scanned". Counts diverge by OS
(Windows skips `golden-rule.include_scan_e2e`), so never assume one — measure with `ctest -N`.

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

CI (GitHub Actions; macOS + Windows + Ubuntu) configures, builds and tests all six presets on every push to `main` and every PR, plus a `lint` job running clang-format, clang-tidy, the vcpkg-baseline guard, and the **six** architecture guards in `.github/scripts/`: `check-math-boundary.sh`, `check-platform-boundary.sh`, `check-rhi-boundary.sh`, `check-scene-boundary.sh`, `check-golden-rule.sh`, `check-project-no-delete.sh`. The count is measured, not remembered — `ls .github/scripts/`.

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
- CI (GitHub Actions, macOS + Windows + Ubuntu, from commit #1): Debug build with ASan/UBSan + Release build, codegen steps, doctest unit tests, the **six** architecture guards that exist today — math-boundary (no `<glm/...>` outside `engine/core/src/math/glm_backend.cpp`, the single allowlisted file; **not** the looser "outside `core/math`", which would license GLM in the public math headers), platform-boundary, rhi-boundary, scene-boundary, golden-rule, project-no-delete; each created by its owning task, see `docs/04` — and format/lint checks. *(Audio-boundary and runtime-purity are planned for their owning phases and do **not** exist yet; do not cite them as live.)*

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
| `docs/tasks/phase-{0..8}.md` | Full breakdown per phase: epics → tasks → subtasks, each task with goal + deliverable |
| `docs/08-risks.md` | Risk register, open + resolved decisions |
| `docs/09-file-formats.md` | Scene schema v1 (entity/components/version), canonical form, versioning & evolution policy |
| `docs/10-engineering-log.md` | **Per-task build history** — what each task shipped and deliberately did not, traps found, dead ends never to retry, and per-task build/dependency impact. Not normative; not auto-loaded. Read the relevant entry before touching a subsystem. |
| `docs/future-roadmap.md` | v2 / v3–v4 deferred features |
| `.claude/rules/*.md` | Path-scoped working rules, loaded only when matching files are opened (boundary guards, reflect-gen, CI portability, editor) |
