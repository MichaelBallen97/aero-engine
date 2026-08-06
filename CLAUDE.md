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
OPEN. Epic 3.1 (AssetDatabase · assets) is in progress — tasks 3.1.1 (GUIDs + `.meta` files), 3.1.2
(import cache & dependency tracking) and 3.1.3 (asset browser v1) are all COMPLETE in code.** 3.1.1
merged to `main` as PR #65 (merge commit `2be73e1`, 17 commits), sabotage-proven (26/26 seeds),
CI-green on macOS, Windows and Linux, and **macOS human-validated ✅ PASS 14/14 on 2026-08-04**. 3.1.2
merged as PR #66 (merge commit `3470b87`, mechanical gate green — 95/95 both presets, both reduced
configurations, six guards, 31/31 sabotage seeds plus all 3 mandatory second-order checks confirmed)
and **closed two items 3.1.1 deliberately deferred**: the D8 orphan-re-attachment deferral and the
carried-forward symlinked-directory duplicate-GUID defect. **macOS human-validated ✅ PASS 14/14 on
2026-08-05.** **3.1.3 lands on branch `feat/3.1.3-asset-browser-v1`** (mechanical gate green — 95/95
both presets, both reduced configurations, six guards including Check B's new positive allowlist,
**all 35/35 sabotage seeds** plus all 3 mandatory second-order checks per seed confirmed) and upgrades
2.2.4's read-only file lister into the real Asset Browser: real decoded thumbnails, type icons, a
project-wide search, the scan's Issues list surfaced, and the one user-initiated destructive action
this subsystem permits — deleting an orphaned `.meta` sidecar, **closing both 3.1.1's D8 and 3.1.2's
D13 deferrals in the same task**. Drag-into-scene was deliberately excised into a new task, **3.1.5**,
because nothing in `engine::scene` can reference an asset yet. **macOS human validation for 3.1.3 is
scheduled, not yet run** (⏳ pending in `editor/VALIDATION.md`) — Windows/Linux rows pending for
3.1.1/3.1.2/3.1.3, as for every Phase 2 task. **3.1.5 (drag-into-scene) is next**, once 3.2.1 exists
for it to reference.
**Carried-forward debt, unchanged by 3.1.3 and explicitly not part of any gate:** no Windows or
Linux human pass exists for any of the thirteen Phase 2 tasks or for 3.1.1/3.1.2/3.1.3, and Phase 0's
gate is still held open on Windows/Linux 60 fps sign-off. That is platform-validation debt spanning
three phases now, and it is worth scheduling as work of its own — the 2.2.5 lesson, one scale up.

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics (2.1 Editor shell, 2.2 Core panels, 2.3 Manipulation, 2.4 Undo/redo, 2.5 Scene I/O, 2.6 Project system v0) CLOSED in code and macOS human-validated PASS, with Windows/Linux rows pending for every task (`editor/VALIDATION.md`). The whole-phase audit (2026-08-02) fixed two silent data-loss paths, a project root that was never made absolute, two CI false-greens, and four stale documentation claims. Full per-task and per-epic history — every defect, every sabotage matrix, every deviation — lives in `docs/10-engineering-log.md`'s Phase 2 entries; this row is deliberately a summary, not a duplicate. |
| **Phase 2 gate** | **MET 2026-08-02.** `samples/phase-2-editor-scene/` holds a project and a 4-entity scene authored entirely through the editor, with the save → New Scene → Open Scene round trip confirmed (`samples/phase-2-editor-scene/VALIDATION.md`); provenance is recorded there rather than asserted, since a hand-written `scene.json` is byte-identical to a real one and no test tier can tell them apart. Deliberately NOT `add_subdirectory`'d — this artifact is data (a provenance proof of the editor), not a compile-proof of engine code. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** Epic 3.1 (AssetDatabase · assets) in progress: **3.1.1 (GUIDs + `.meta` files), 3.1.2 (import cache & dependency tracking) and 3.1.3 (asset browser v1) all CLOSED in code.** `engine::Guid` (`engine/core`, beside `Handle`) — a 16-byte trivially-copyable POD, seedable `splitmix64` generator, no test anywhere touches an entropy source. The `.meta` v1 format (`editor/include/aero/editor/asset_meta.hpp` + `.cpp`) and its pure lifecycle planner `planAssetMetas`, provable from a `std::vector` literal with no disk touched. `AssetDatabase::rescan` — an eight-phase, `<filesystem>`-free, non-recursive scan composed from 2.2.4's `listDirectory`, 2.5.1/2.6.1's `text_file` and 3.1.2's `asset_cache`; it never logs (INV-A3) and never throws. `engine::ContentHash` (task 3.1.2, beside `Guid`) — a 16-byte MurmurHash3 x64_128 fingerprint. The **machine-local, never-committed import cache** at `<projectRoot>/Library/asset-cache.json` (task 3.1.2): `planImports`/`commitImports`, a byte-sorted precedence-ordered pure planner, cascading transitively through a dependency graph with a monotone O(V+E) worklist; `planReattachments` closed 3.1.1's own D8 deferral; directory dedup by canonical physical path closed the carried-forward symlinked-directory duplicate-GUID defect. A scan of an unchanged project writes **zero bytes across two files**, `.meta` sidecars and the cache index both (D15/INV-C5). **3.1.3 adds the real Asset Browser**: `asset_view.{hpp,cpp}` (pure — kinds, icons, filter/search, grid geometry), `thumbnail_cache.{hpp,cpp}` (pure — key/state/LRU/budget + a deterministic INTEGER box resampler, no floating point anywhere, so output is byte-identical across all three OSes), `thumbnail_store.{hpp,cpp}` (src-private — the ONLY stb_image TU and the ONLY GPU-touching thumbnail TU; `stb` is vcpkg's new editor-only port), and `asset_actions.{hpp,cpp}` (the fifth editor `<filesystem>` TU, exactly one `std::filesystem::remove` call, a six-step re-verify-before-deleting algorithm). The Asset Browser gained a Grid/List toggle, real decoded thumbnails (128×128, budgeted at 2 decodes/tick, LRU-evicted past a 256-texture resident cap), a project-wide search with a kind filter, an Issues panel reading the scan report, and the ONE user-initiated destructive action this whole subsystem permits: deleting an orphaned `.meta` sidecar — **closing both 3.1.1's D8 and 3.1.2's D13 deferrals in the same task.** Drag-into-scene was deliberately excised into new task **3.1.5** (`engine::MeshRenderer` has no asset-referencing field yet; `git grep -ln 'Guid' -- engine/scene/` is empty). The sixth architecture guard, `check-project-no-delete.sh`, widened **three times** — three files → five at 3.1.1 (D7/D8) → six at 3.1.2 (`asset_cache.cpp`, D18) → **3.1.3 added a second check, Check B, a POSITIVE two-file allowlist** (`text_file.cpp`, `asset_actions.cpp`) closing the "a delete written into a seventh, unnamed file passes silently" hole Check A's denylist shape could never close; the script's name stays narrower than its scope on purpose. **Sabotage: 3.1.1 ran all 26 seeds, 3.1.2 ran all 31, 3.1.3 ran all 35 (S1–S35) — every task's full matrix plus all 3 mandatory second-order checks each, confirmed against the real built binaries.** 3.1.3: 23 matched their prediction, 7 confirmed non-discriminators (S3, S7, S9, S10, S11, S30, S34), 0 predicted contingencies, 5 differently-shaped findings (23+7+0+5=35) — notably S14 (`searchAssets` matching the whole path instead of the leaf) stayed green against the plan's own claimed discriminator (`AV36` tests `matchesFilter` directly, never `searchAssets`' leaf-extraction call site), a genuine gap closed by a new case (`AV39b`); S25 (`serviceThumbnails()` called from inside `onDraw`) gave two different verdicts depending on exactly where in `onDraw` it was seeded — reddening 4 GPU cases at the very start, matching the plan's own non-discriminator prediction exactly when seeded at the natural end; and S30 (`~ThumbnailStore` leaks its textures) matched its ASan non-discriminator prediction exactly, while the RHI's own destroy-accounting logged the leak ASan could not see. Full sabotage matrices, every deviation, and every build-time finding (3.1.3: an MSVC `<string_view>`/`<ostream>` completeness trap, nine Linux clang-tidy findings, a Windows-only `nul.bin` reserved-device-name failure, a duplicate-member compile error the plan's own text would have caused, a UBSan `+inf → int` abort, and two genuine sabotage-discovered test-quality gaps closed outright) are in `docs/10-engineering-log.md`'s 3.1.1/3.1.2/3.1.3 entries. **3.1.1 macOS human-validated ✅ PASS 14/14 (2026-08-04)**; **3.1.2 macOS human-validated ✅ PASS 14/14 (2026-08-05)**; **3.1.3's macOS human pass is ⏳ pending** — Windows/Linux rows pending for all three tasks. |
| **Next task** | **3.1.5 (drag-into-scene)** — see `docs/tasks/phase-3.md`. Depends on 3.1.3, 3.2.1 (glTF import), so it cannot start in earnest until 3.2.1 gives `engine::scene` something to reference. **3.1.4 (hot-reload file watcher)** remains available in parallel — depends only on 3.1.2, unaffected by 3.1.3. The remaining carried-forward item is **platform-validation debt, now spanning three phases and all three landed Epic 3.1 tasks**: no Windows or Linux human pass exists for any of the thirteen Phase 2 tasks or for 3.1.1/3.1.2/3.1.3, and Phase 0's gate is still held open on Windows/Linux 60 fps sign-off. Schedule it as work of its own rather than as a ride-along row — 2.2.5's lesson at phase scale. |

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
second time, and 3.1.3 restarts the streak at one**: it needed no `engine/` change at all (predicted by
the plan itself — `engine::MeshRenderer` has no asset-referencing field yet). `/editor` gains nine new
pairs across 2.6.2, 3.1.1, 3.1.2 and 3.1.3: `project_settings.{hpp,cpp}` / `project_settings_panel.{hpp,cpp}`
(2.6.2), `asset_meta.{hpp,cpp}` / `asset_database.{hpp,cpp}` (3.1.1), `asset_cache.{hpp,cpp}` (3.1.2),
and `asset_view.{hpp,cpp}` / `thumbnail_cache.{hpp,cpp}` / `thumbnail_store.{hpp,cpp}` (src-private) /
`asset_actions.{hpp,cpp}` (3.1.3) — the `.hpp`s under `editor/include/aero/editor/` (except
`thumbnail_store.hpp`, deliberately src-private), the `.cpp`s under `editor/src/`.

Test inventory at HEAD, **re-measured, not carried forward**: **95** ctest entries with tools ON,
**6** with `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, **19** with `-DAERO_REFLECT_TOOLS=OFF`
alone — unchanged by task 3.1.3 (zero new ctest entries; every new case lives inside an existing TU).
`aero_tests` **415** (unchanged — 3.1.3 touches zero `engine/` paths). `aero_editor_shell_test`
**753** (**621** after 3.1.2 → **+132** at 3.1.3: `tests/editor/asset_view_test.cpp` AV1–AV42 plus AV39b, a
new TU; `tests/editor/thumbnail_cache_test.cpp` TC1–TC38, a new TU; `tests/editor/asset_actions_test.cpp`
AA1–AA22, a new TU; `tests/editor/text_file_test.cpp` and `tests/editor/asset_database_test.cpp`
extended in place, plus the code-review round's own cases. **753 measured directly with
`--list-test-cases`, never derived by addition** — and the baseline is **621**, measured at `94e57e7`,
not the **617** this file and the 3.1.2 log entry both carried forward: that stale figure survived into
3.1.3's first draft and produced a wrong `+123`, which is precisely the class of error AC-30 exists to
prevent. Both numbers are now measured, neither is derived).
`aero_editor_imgui_test` **65** (57 after 3.1.2 → +8 at 3.1.3: I36–I42, the real-thumbnail-decode,
budget/eviction, orphan-delete-round-trip and mechanical source-text GPU-tier cases, plus the
code-review round's I43). Both reduced
configurations, freshly rebuilt for this documentation step (`build/tools-off-3.1.3`,
`build/reflect-off-3.1.3`): `ctest -N` **6**/**19** unchanged, `aero_editor_shell_test`'s own doctest
`--count` reads **729** in **both**, all passing 100% (6/6 and 19/19), re-measured after the
code-review round on freshly configured directories. `aero_scene_serialize_test` **23** and
`aero_editor_inspector_test` **22**, both unchanged since 3.1.1. `aero_editor_core` sources **46** (42
before this task → +4: `asset_view.{hpp,cpp}`, `thumbnail_cache.{hpp,cpp}`, `thumbnail_store.{hpp,cpp}`,
`asset_actions.{hpp,cpp}`) — one new `find_package(Stb REQUIRED)` plus one new
`target_include_directories(... SYSTEM PRIVATE ${Stb_INCLUDE_DIR})`, no new `target_link_libraries`
entry. `check-math-boundary.sh`'s scanned count: **262** after 3.1.2 **→ 273** at 3.1.3 (eleven
new C-family files: the four new `/editor` pairs' eight files plus three new test TUs) — measured
after `git add` at every step boundary, never assumed. Guard count stays **six**, but
`check-project-no-delete.sh` now runs **two checks**: Check A's six-file denylist (unchanged in count)
plus a new Check B, a two-file `PERMITTED_DELETERS` POSITIVE allowlist (`text_file.cpp`,
`asset_actions.cpp`) closing the "a delete in a seventh, unnamed file passes silently" hole a denylist
alone can never close; the script's final banner now reports both counts. Counts diverge by OS
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
