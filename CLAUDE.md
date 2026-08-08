# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Aero Engine — an open-source (MIT), cross-platform 3D game engine with an editor and per-project TypeScript **or** C++ scripting. Solo project, started July 2026. The goal is core-workflow parity with Unity/Godot (edit → script → play → export), explicitly **not** feature parity. 3D-first; 2D arrives in Phase 7.

Two platform matrices, never to be conflated: the **editor** runs on macOS/Windows/Linux only; the **runtime** (exported games) targets those three plus iOS and Android. The editor never runs on mobile — no touch UI, no adaptive layouts.

## Current state — read this first

**Phase 2 (Editor) is COMPLETE — gate met 2026-08-02.** All six epics closed in code, every task
macOS-validated with no open FAIL, and the gate artifact committed at
`samples/phase-2-editor-scene/` (a project + 4-entity scene authored entirely through the editor, with
the save → New Scene → Open Scene round trip confirmed). A whole-phase audit (2026-08-02) then found
and fixed two silent data-loss paths, a never-absolute project root, and four stale documentation
claims — full detail in `docs/10-engineering-log.md`. **Phase 3 (Asset Pipeline & 3D Content) is
OPEN. Epic 3.1 (AssetDatabase · assets) has four MERGED tasks and a fifth code-complete on its own
branch.** 3.1.1 (GUIDs + `.meta` files) merged as PR #65 (`2be73e1`), sabotage-proven (26/26 seeds),
CI-green on all three platforms, **macOS-validated ✅ PASS 14/14 (2026-08-04)**. 3.1.2 (import cache &
dependency tracking) merged as PR #66 (`3470b87`, 31/31 sabotage seeds), closing 3.1.1's D8
orphan-re-attachment deferral and the carried-forward symlinked-directory duplicate-GUID defect,
**macOS-validated ✅ PASS 14/14 (2026-08-05)**. 3.1.3 (asset browser v1) merged as PR #67 (`aa914fb`,
35/35 sabotage seeds; its code-review round found 11 findings, 3 BLOCKING, the sharpest a real
use-after-free of GPU textures invisible on macOS because SDL frees synchronously on Vulkan/D3D12 but
only defers on Metal), gave the Asset Browser real decoded thumbnails, a project-wide search and the
Issues list, and closed both 3.1.1's D8 and 3.1.2's D13 deferrals with its one user-initiated
destructive action (deleting an orphaned `.meta`). **macOS-validated ✅ PASS 16/16 (2026-08-06)** — row
4 (no stutter with a folder of photos) is the only evidence anywhere that the two-per-tick blocking
`uploadTexture` fence sync is tolerable, and that evidence is Metal-only. Found and fixed one cosmetic
defect post-merge (the grid's `..` row). 3.1.4 (hot-reload file watcher) merged as PR #69 (`ebc4da6`,
25/25 sabotage seeds; its code-review round closed six gaps against an already-green gate, the sharpest
overturning the task's own "no count-only accessor could ever discriminate this" claim about
`noteExternalScan()`'s call site). It adds `AssetWatcher`, a per-tick budgeted sweep with zero file
reads and zero logging of its own, plus `AssetDatabase::generation()` (the one signal driving every
downstream refresh) and `ThumbnailLedger::supersededBy()`. **macOS-validated ✅ PASS 10/10 (2026-08-07)**
— row 4 (two minutes idle) found the Console silent and the frame rate unchanged, the only evidence
anywhere the watcher's steady state is genuinely costless; **R1's numeric per-sweep cost stayed
unmeasured**, open debt this task's own risk register asks for by number, not estimate. Full
per-task sabotage matrices and every build-time finding for all four: `docs/10-engineering-log.md`'s
Phase 3 entries.

**3.1.5 (drag-into-scene) is queued behind both 3.1.3 and 3.2.1 (glTF import); 3.2.1 is now the more
actionable one.** **3.2.1 is COMPLETE IN CODE on `feat/3.2.1-gltf-import-fastgltf` (eleven commits: ten
feature, one docs), NOT YET MERGED** — the mechanical gate is green through this branch's own local
run (95/95 both presets, both reduced configurations rebuilt fresh in `build/tools-off-3.2.1` /
`build/reflect-off-3.2.1` at 952 doctest cases each — up from 813 pre-task — with `MI1`/`MS1` present
in both, six guards unchanged and byte-identical, clang-format/clang-tidy clean by exit code) — but
**Step 12's 32-seed sabotage matrix has not run, there is no PR, no CI run, and no validation pass on
any OS.** It gives the editor its first working importer, and — for the first time since 3.1.2 added
the field — a real **producer** for `AssetCacheEntry::dependencies`: phase 7.5 (this task's one
`asset_database.cpp` edit) runs a budgeted `Structure`-depth probe over changed model assets inside the
existing scan and turns every resolved external URI into a dependency GUID, so editing a texture a
model references now marks that model `DependencyChanged` on the next scan — the roadmap's own
headline example, working end to end. `gltf_import.{hpp,cpp}` is the **only** fastgltf TU anywhere in
the tree (INV-M1/AC-55: the include grep names exactly one file). `model_import.{hpp,cpp}` holds the
canonical, third-party-free `ImportedModel` and every pure helper — provable from string literals and
committed text fixtures with zero disk on the critical path. `model_import_session.{hpp,cpp}` drives an
on-demand two-pass import (`Structure` then `Full`) from an Asset Browser selection and holds the one
write this whole task adds anywhere. `import_details_panel.{hpp,cpp}` is the task's only ImGui TU: six
CollapsingHeader sections (Overview, Import Settings, Hierarchy, Meshes, Materials, Skeleton &
Animation), all default-open — the Inspector's own per-component precedent, and the only way any
section is reachable by this project's ImGui-free-at-source GPU tier at all. `.meta` gains an optional,
additive `importer` block at format version 1 — never a v2 bump, which would nil every GUID in an older
build. **Zero paths under `engine/`** — the no-engine-change streak that 3.1.3 restarted at one and
3.1.4 carried to two now reaches **three**. Two deviations from the plan's own literal counts, both
logged in `docs/10-engineering-log.md`'s 3.2.1 entry and in `.claude/rules/editor.md`: the
`AssetBrowserPanel::requestSelectEntry` / `EditorApp::requestAssetBrowserSelectEntry` seam (the
code-review-finding-4 shape, a fifth application — without it AC-45/AC-46/AC-47/AC-50's own GPU-tier
cases could not drive a real selection at all), and one `std::move` on a trivially-copyable
`std::optional<ImportSettings>` dropped for clang-tidy's `performance-move-const-arg`. Full build-time
finding list, the fastgltf MUST-VERIFY answers confirmed against the installed v0.9.0 headers, and the
three gate-grep corrections the plan's own review rounds made (AC-55/AC-56) are in
`docs/10-engineering-log.md`'s 3.2.1 entry. **What remains on 3.2.1: the sabotage matrix, the PR, CI on
all three platforms, and a validation pass on every OS** — none of that is this entry's to claim.

**Carried-forward debt, unchanged by 3.2.1 and explicitly not part of any gate:** no Windows or
Linux validation pass exists for any of the thirteen Phase 2 tasks or for 3.1.1/3.1.2/3.1.3/3.1.4, and
Phase 0's gate is still held open on Windows/Linux 60 fps sign-off. That is platform-validation debt
spanning three phases now, and it is worth scheduling as work of its own — the 2.2.5 lesson, one scale
up.

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics (2.1 Editor shell, 2.2 Core panels, 2.3 Manipulation, 2.4 Undo/redo, 2.5 Scene I/O, 2.6 Project system v0) CLOSED in code and macOS-validated PASS, with Windows/Linux rows pending for every task (`editor/VALIDATION.md`). The whole-phase audit (2026-08-02) fixed two silent data-loss paths, a project root that was never made absolute, two CI false-greens, and four stale documentation claims. Full per-task and per-epic history — every defect, every sabotage matrix, every deviation — lives in `docs/10-engineering-log.md`'s Phase 2 entries; this row is deliberately a summary, not a duplicate. |
| **Phase 2 gate** | **MET 2026-08-02.** `samples/phase-2-editor-scene/` holds a project and a 4-entity scene authored entirely through the editor, with the save → New Scene → Open Scene round trip confirmed (`samples/phase-2-editor-scene/VALIDATION.md`); provenance is recorded there rather than asserted, since a hand-written `scene.json` is byte-identical to a real one and no test tier can tell them apart. Deliberately NOT `add_subdirectory`'d — this artifact is data (a provenance proof of the editor), not a compile-proof of engine code. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** Epic 3.1 (AssetDatabase · assets): **3.1.1 (GUIDs + `.meta` files), 3.1.2 (import cache & dependency tracking), 3.1.3 (asset browser v1) and 3.1.4 (hot-reload file watcher) all MERGED to `main`** (PRs #65/#66/#67/#69), CI-green on all three platforms, sabotage-proven (26/31/35/25 seeds respectively), **macOS-validated ✅ PASS on all four** (14/14, 14/14, 16/16, 10/10) — Windows/Linux rows pending for all four, see the paragraph above. `engine::Guid`/`engine::ContentHash` (`engine/core`), the `.meta` v1 format, `AssetDatabase::rescan`'s eight phases, the machine-local `Library/asset-cache.json` import cache with its dependency-cascade worklist, and the real Asset Browser (thumbnails, search, the Issues panel, the one sanctioned orphan-`.meta` delete) all shipped across these four. **Epic 3.2 (Importers) opens with 3.2.1 (glTF import, fastgltf): COMPLETE IN CODE on `feat/3.2.1-gltf-import-fastgltf`, mechanical gate green (95/95 both presets, both reduced configurations at 952 doctest cases each, six guards, lint clean), NOT YET sabotage-tested, NOT YET a PR, NOT YET merged, no validation pass on any OS.** It is the first PRODUCER for `AssetCacheEntry::dependencies` (3.1.2's own field, unfed until now) — editing a texture a model references now marks that model `DependencyChanged` on the next scan, the roadmap's own headline example working end to end. `gltf_import.{hpp,cpp}` is the only fastgltf TU anywhere in the tree; `model_import.{hpp,cpp}` holds the canonical, third-party-free `ImportedModel`; `model_import_session.{hpp,cpp}` drives the on-demand two-pass import; `import_details_panel.{hpp,cpp}` is a new six-section panel, all sections default-open. Zero paths under `engine/` — the no-engine-change streak reaches three. Full detail, every build-time finding and the fastgltf MUST-VERIFY answers confirmed against the installed v0.9.0 headers are in `docs/10-engineering-log.md`'s 3.2.1 entry. |
| **Next task** | **Take 3.2.1 (glTF import) through Step 12 (the 32-seed sabotage matrix) and Step 13 (the mechanical gate, the PR, CI on all three platforms, and the merge)** — code, docs and the local mechanical gate are all done on `feat/3.2.1-gltf-import-fastgltf`. Once it merges, **3.1.5 (drag-into-scene)** becomes available — see `docs/tasks/phase-3.md`; it depends on 3.1.3 (merged) and 3.2.1. The remaining carried-forward item is **platform-validation debt, now spanning three phases and all four merged Epic 3.1 tasks**: no Windows or Linux validation pass exists for any of the thirteen Phase 2 tasks or for 3.1.1/3.1.2/3.1.3/3.1.4, and Phase 0's gate is still held open on Windows/Linux 60 fps sign-off. Schedule it as work of its own rather than as a ride-along row — 2.2.5's lesson at phase scale. |

Engine layers that exist today, in dependency order: `core` (gained `guid.hpp`/`guid.cpp` at task
3.1.1, beside `handle.hpp`; gained `content_hash.hpp`/`content_hash.cpp` at task 3.1.2, beside `guid`)
→ `platform` → `rhi` → `render` → `reflect` → `scene` → `scene_render` → `scene_serialize`, plus
`/editor` (`aero_editor_core` + `aero_editor`) and `/tools` (`reflect-gen`, `shaderc`). `/runtime` is
still empty — it arrives in Phase 5. `engine/assets/` is still just `.gitkeep` — deliberately unopened
until a **runtime** consumer exists (Phase 5's pak table); the editor's `AssetDatabase` and its import
cache (tasks 3.1.1/3.1.2) live entirely in `/editor`, not `/engine/assets`.
`engine/scene` gained one primitive at task 2.4.2, `[[nodiscard]] Entity World::recreate(Entity)` —
the only engine change Epic 2.4 needed. Tasks 2.5.1, 2.5.2, 2.6.1 and 2.6.2 all needed **no** engine
change at all — a four-task streak task **3.1.1 ended**; **3.1.2 used the identical minimal shape a
second time, 3.1.3 restarted the streak at one, 3.1.4 made it two, and 3.2.1 now makes it three**: it
needs no `engine/` change at all (`git diff --name-only main...HEAD -- engine/` is empty on the
feature branch). `/editor` gained **ten** new `.hpp`/`.cpp` pairs across 2.6.2, 3.1.1, 3.1.2, 3.1.3 and
3.1.4 (`project_settings.{hpp,cpp}` / `project_settings_panel.{hpp,cpp}` (2.6.2),
`asset_meta.{hpp,cpp}` / `asset_database.{hpp,cpp}` (3.1.1), `asset_cache.{hpp,cpp}` (3.1.2),
`asset_view.{hpp,cpp}` / `thumbnail_cache.{hpp,cpp}` / `thumbnail_store.{hpp,cpp}` (src-private) /
`asset_actions.{hpp,cpp}` (3.1.3), `asset_watcher.{hpp,cpp}` (3.1.4)); **3.2.1 adds four more pairs plus
one deliberate exception**: `model_import.{hpp,cpp}`, `gltf_import.{hpp,cpp}` (src-private, the only
fastgltf TU), `model_import_session.{hpp,cpp}` and `import_details_panel.{hpp,cpp}` (src-private, the
only ImGui TU this task adds) are real pairs; `import_settings.hpp` is a HEADER WITH NO `.cpp`,
deliberately alone (plan §A-11 — `ImportSettings` is shared by `asset_meta.hpp` and
`model_import.hpp`, and giving it its own tiny, dependency-free header is what stops `asset_meta.hpp`
from dragging in `aero::scene` and the whole math umbrella). The `.hpp`s live under
`editor/include/aero/editor/` (except the three named src-private), the `.cpp`s under `editor/src/`.

Test inventory at HEAD (`feat/3.2.1-gltf-import-fastgltf`), **re-measured, not carried forward**:
**95** ctest entries with tools ON, **6** with `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`,
**19** with `-DAERO_REFLECT_TOOLS=OFF` alone — unchanged by task 3.2.1 (zero new ctest entries; every
new case lives inside an existing TU). `aero_tests` **415** (unchanged — 3.2.1 touches zero `engine/`
paths). `aero_editor_shell_test` **976** (**837** after 3.1.4 → **+139** at 3.2.1:
`tests/editor/model_import_test.cpp`, a new TU (**91** cases, MI1–MI93 plus MI40b);
`tests/editor/model_import_session_test.cpp`, a new TU (**16** cases, MS1–MS16);
`tests/editor/asset_meta_test.cpp` +14 (AM-i1–AM-i14); `tests/editor/asset_cache_test.cpp` +8
(AC-p1–AC-p8); `tests/editor/asset_database_test.cpp` +10 (AD-i1–AD-i10) — **976 measured directly with
`--list-test-cases`, never derived by addition** (91+16+14+8+10 = 139, and 837+139 = 976, both checked).
`aero_editor_imgui_test` **82** (73 after 3.1.4 → +9 at 3.2.1: I52–I60, the panel's registration, the
exactly-one-import/re-import/non-model cases, three real-frame draw states, the reconcile's own
target-sync proof, and the source-text call-site proof). `aero_scene_serialize_test` **23** and
`aero_editor_inspector_test` **22**, both unchanged since 3.1.1. `aero_editor_core` sources **51** (47
before this task → +4: `model_import.cpp`, `gltf_import.cpp`, `model_import_session.cpp`,
`import_details_panel.cpp`) — one new `find_package(fastgltf CONFIG REQUIRED)`, one new PRIVATE
`target_link_libraries` entry (`fastgltf::fastgltf`, confined to `aero_editor_core`, never reaching
`aero_editor` or any test target's link line). `check-math-boundary.sh`'s scanned count: **276** after
3.1.4 **→ 287** at 3.2.1 (eleven new C-family files, matching the plan's own prediction exactly) —
measured after `git add` at every step boundary, never assumed. Guard count stays **six**,
`check-project-no-delete.sh` byte-identical (`git diff main...HEAD -- .github/scripts/` empty) — Check
A's six-file denylist unchanged, Check B's two-file `PERMITTED_DELETERS` allowlist unchanged (none of
this task's five new `editor/src/*.cpp` files is in either — being outside both is what makes a future
`std::filesystem::remove` in any of them a hard CI failure), Check B's scanned-file count grows to
**52** (the glob picks the new files up automatically). Both reduced configurations, freshly rebuilt in
`build/tools-off-3.2.1`/`build/reflect-off-3.2.1`: `ctest -N` **6**/**19** unchanged, both passing 100%
(6/6 and 19/19), `aero_editor_shell_test`'s own doctest `--list-test-cases` count reads **952** in
**both** (up from 813 pre-task), with `MI1` and `MS1` present in both — proving both new importer TUs
need no reflection or scene serialization (AC-59). Counts diverge by OS (Windows skips
`golden-rule.include_scan_e2e`), so never assume one — measure with `ctest -N`.

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
