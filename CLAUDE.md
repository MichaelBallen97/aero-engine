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
OPEN. Epic 3.1 (AssetDatabase · assets) is in progress — tasks 3.1.1 (GUIDs + `.meta` files) and 3.1.2
(import cache & dependency tracking) are both COMPLETE in code.** 3.1.1 merged to `main` as PR #65
(merge commit `2be73e1`, 17 commits), sabotage-proven (26/26 seeds), CI-green on macOS, Windows and
Linux, and **macOS human-validated ✅ PASS 14/14 on 2026-08-04**. 3.1.2 landed on branch
`feat/3.1.2-import-cache-and-dependency-tracking` (11 commits, mechanical gate green — 95/95 both
presets, both reduced configurations, six guards, 31/31 sabotage seeds plus all 3 mandatory
second-order checks confirmed) and **closes two items 3.1.1 deliberately deferred**: the D8
orphan-re-attachment deferral and the carried-forward symlinked-directory duplicate-GUID defect —
neither is carried-forward debt any longer. **macOS human validation for 3.1.2 is scheduled, not yet
run** (⏳ pending in `editor/VALIDATION.md`) — Windows/Linux rows pending for both 3.1.1 and 3.1.2, as
for every Phase 2 task. **3.1.3 (asset browser v1) is next.**
**Carried-forward debt, unchanged by 3.1.2 and explicitly not part of any gate:** no Windows or
Linux human pass exists for any of the thirteen Phase 2 tasks or for 3.1.1/3.1.2, and Phase 0's gate is
still held open on Windows/Linux 60 fps sign-off. That is platform-validation debt spanning three
phases now, and it is worth scheduling as work of its own — the 2.2.5 lesson, one scale up.

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics (2.1 Editor shell, 2.2 Core panels, 2.3 Manipulation, 2.4 Undo/redo, 2.5 Scene I/O, 2.6 Project system v0) CLOSED in code and macOS human-validated PASS, with Windows/Linux rows pending for every task (`editor/VALIDATION.md`). The whole-phase audit (2026-08-02) fixed two silent data-loss paths, a project root that was never made absolute, two CI false-greens, and four stale documentation claims. Full per-task and per-epic history — every defect, every sabotage matrix, every deviation — lives in `docs/10-engineering-log.md`'s Phase 2 entries; this row is deliberately a summary, not a duplicate. |
| **Phase 2 gate** | **MET 2026-08-02.** `samples/phase-2-editor-scene/` holds a project and a 4-entity scene authored entirely through the editor, with the save → New Scene → Open Scene round trip confirmed (`samples/phase-2-editor-scene/VALIDATION.md`); provenance is recorded there rather than asserted, since a hand-written `scene.json` is byte-identical to a real one and no test tier can tell them apart. Deliberately NOT `add_subdirectory`'d — this artifact is data (a provenance proof of the editor), not a compile-proof of engine code. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** Epic 3.1 (AssetDatabase · assets) in progress: **3.1.1 (GUIDs + `.meta` files) and 3.1.2 (import cache & dependency tracking) both CLOSED in code.** `engine::Guid` (`engine/core`, beside `Handle`) — a 16-byte trivially-copyable POD, seedable `splitmix64` generator, no test anywhere touches an entropy source. The `.meta` v1 format (`editor/include/aero/editor/asset_meta.hpp` + `.cpp`) and its pure lifecycle planner `planAssetMetas`, provable from a `std::vector` literal with no disk touched. `AssetDatabase::rescan` — an eight-phase (3.1.1 shipped five; 3.1.2 added the cache load, hash-what-changed and orphan-re-attachment phases), `<filesystem>`-free, non-recursive scan composed from 2.2.4's `listDirectory`, 2.5.1/2.6.1's `text_file` and 3.1.2's `asset_cache`; it never logs (INV-A3) and never throws. 3.1.1 **ended** the four-task empty-`engine/`-diff streak with a three-path `guid.hpp`/`.cpp` diff; 3.1.2 used the identical minimal shape a second time for `engine::ContentHash` (`content_hash.hpp`/`.cpp`, beside `Guid`) — a 16-byte MurmurHash3 x64_128 fingerprint, distinct from `Guid` with no conversion either way. 3.1.2 gave the editor a **machine-local, never-committed import cache** at `<projectRoot>/Library/asset-cache.json`: `planImports`/`commitImports` name, per asset, exactly why it would re-import (a byte-sorted, precedence-ordered pure function) and cascade the answer transitively through a dependency graph with a monotone O(V+E) worklist that needs no cycle detection at all; `planReattachments` closes 3.1.1's own D8 deferral (a moved asset with no sidecar gets its old GUID back, on five simultaneous conditions, never a heuristic); and the walk now dedups **directories** by canonical, physical path, closing the carried-forward symlinked-directory duplicate-GUID defect — **not** via the content hash, which cannot distinguish one file reached twice from two legitimate identical copies (a correction to this file's own earlier, wrong rationale, recorded in `docs/10-engineering-log.md`). A scan of an unchanged project now writes **zero bytes across two files**, `.meta` sidecars and the cache index both (D15/INV-C5). The Asset Browser gained a Reimport All button and an import-state footer segment. The sixth architecture guard, `check-project-no-delete.sh`, widened **twice** — three files → five at 3.1.1 (`asset_meta.cpp`, `asset_database.cpp`, D7/D8) → **six** at 3.1.2 (`asset_cache.cpp`, D18: the cache's data is disposable, but nothing in either task deletes a file to dispose of it); the script's name stays narrower than its scope on purpose. **Sabotage: 3.1.1 ran all 26 seeds (S1–S26); 3.1.2 ran all 31 (S1–S28, S27b, S29–S31) — both plus all 3 mandatory second-order checks each, confirmed against the real built binaries.** 3.1.1: 7 matched, 6 confirmed non-discriminators, 1 predicted contingency, 12 differently-shaped (7+6+1+12=26). 3.1.2: 8 matched (S2, S8, S9, S20, S22, S23, S30, S31), 2 confirmed non-discriminators (S1, S6), 1 predicted contingency (S28 — the Reimport All drain-order regression, exactly closed by `I34`), 20 differently-shaped (8+2+1+20=31) — notably S5 (word-order in `formatContentHash`) reddened 20 cases against 4 predicted, and S19 revealed `IP18`/`IP20` **hang** rather than assert on a non-terminating cascade, contradicting the plan's own stated claim; an iteration cap was rejected as the fix since it would be a disguised cycle detector, which D12 forbids. Full sabotage matrices, every deviation, and every build-time finding (3.1.1: the `MAX_ASSETS` coverage gap, an accessor/member naming collision, the `setDatabase` gating ambiguity, an `AD21` test-design bug; 3.1.2: a missing test TU for the disk primitives, a silent positional-aggregate re-map trap, `mtime`'s real per-file `stat` cost measured at ~7–9 ms/5000 files, `ContentHash::valid()`'s all-zero-empty-digest trap, an uncomputable relative path for `Library/` resolved via canonical-path dedup, the `metaHash`-for-a-fresh-sidecar defect that would have failed human row 2, and an `invalidateCache()` no-op found from a failing test) are in `docs/10-engineering-log.md`'s 3.1.1 and 3.1.2 entries. **3.1.1 macOS human-validated ✅ PASS 14/14 (2026-08-04)**; row 2 is the load-bearing one (sidecar mtimes unchanged across a reopen). **3.1.2 macOS human-validated ✅ PASS 14/14 (2026-08-05)** — every step run and passed as written, none partial, none skipped, on merge commit `3470b87`; its own step 2, extended to cover the cache index's mtime as well as the sidecars', is the load-bearing equivalent for D15, and step 9 proved the symlink closure against a real filesystem. Windows/Linux rows pending for both tasks. **3.1.1's code-review round (PR #65) found 7 findings, 2 BLOCKING** — full detail unchanged from the previous entry, retained in `docs/10-engineering-log.md`. **3.1.2's code-review round (PR #66) found 6 findings, 1 BLOCKING:** `canonicalDirectory` returned `std::filesystem::canonical()`'s **native** form while the walk derived non-symlink children by joining with `/`, so on Windows the two never compared equal — `Library/` was never skipped, the scan discovered its own output, and the index was rewritten **every scan, forever**, inverting this task's headline property; AC-31's symlink closure also failed there. Caught by Windows CI and independently by review; fixed by normalising that one dedup key to the generic form (INV-C9 already forbids it reaching a record, a report or the user). Also fixed: a duplicate-GUID fast-path race that could commit another file's content hash (`AD63`), a write-failed record reporting a false `up to date` (`I35`), a `uint32` grace-counter wrap (`IP41`), a precedence comment contradicting the code (`IP42`), and two mismatched clang-tidy argument comments that reddened the Linux lane. Post-3.1.2 test inventory, **re-measured on `main` after the merge**: `aero_tests` **415**, `aero_editor_shell_test` **621**, `aero_editor_imgui_test` **57**, both reduced configurations **597/597** (`aero_editor_shell_test`'s own doctest count, an identical +153 delta), `check-math-boundary.sh` **262**, `check-project-no-delete.sh` **6 files scanned**. |
| **Next task** | **3.1.3 (asset browser v1)** — see `docs/tasks/phase-3.md`. Depends on 3.1.1, 2.2.4. It upgrades the 2.2.4 stub into the real thing: thumbnails, type icons, drag-into-scene, search/filter, and — now that 3.1.2 has closed both items that used to sit here — it is also the natural home for a user-initiated "Delete orphaned `.meta`" action, which both 3.1.1's D8 and 3.1.2's D13 explicitly deferred to it. **The symlinked-directory duplicate-GUID defect and the D8 orphan-re-attachment deferral are STRUCK from this row: 3.1.2 closed both in code** (canonical-path directory dedup; `planReattachments`' five conditions) — see the Phase 3 row above and `docs/10-engineering-log.md`'s 3.1.2 entry for the full detail and the D9 rationale correction. The remaining carried-forward item is **platform-validation debt, now spanning three phases and both landed Epic 3.1 tasks**: no Windows or Linux human pass exists for any of the thirteen Phase 2 tasks or for 3.1.1/3.1.2, and Phase 0's gate is still held open on Windows/Linux 60 fps sign-off. Schedule it as work of its own rather than as a ride-along row — 2.2.5's lesson at phase scale. |

Engine layers that exist today, in dependency order: `core` (gained `guid.hpp`/`guid.cpp` at task
3.1.1, beside `handle.hpp`; gained `content_hash.hpp`/`content_hash.cpp` at task 3.1.2, beside `guid`)
→ `platform` → `rhi` → `render` → `reflect` → `scene` → `scene_render` → `scene_serialize`, plus
`/editor` (`aero_editor_core` + `aero_editor`) and `/tools` (`reflect-gen`, `shaderc`). `/runtime` is
still empty — it arrives in Phase 5. `engine/assets/` is still just `.gitkeep` — deliberately unopened
until a **runtime** consumer exists (Phase 5's pak table); the editor's `AssetDatabase` and its import
cache (tasks 3.1.1/3.1.2) live entirely in `/editor`, not `/engine/assets`.
`engine/scene` gained one primitive at task 2.4.2, `[[nodiscard]] Entity World::recreate(Entity)` —
the only engine change Epic 2.4 needed. Tasks 2.5.1, 2.5.2, 2.6.1 and 2.6.2 all needed **no** engine
change at all — a four-task streak task **3.1.1 ended**, and **3.1.2 used the identical minimal shape
a second time**: each task's `engine/` diff is exactly three paths (`engine/core/CMakeLists.txt` plus
one header and one source — `guid.{hpp,cpp}` for 3.1.1, `content_hash.{hpp,cpp}` for 3.1.2), the
smallest engine change this project's convention allows short of none at all. `/editor` gains five new
pairs across 2.6.2, 3.1.1 and 3.1.2: `project_settings.{hpp,cpp}` / `project_settings_panel.{hpp,cpp}`
(2.6.2), `asset_meta.{hpp,cpp}` / `asset_database.{hpp,cpp}` (3.1.1), and `asset_cache.{hpp,cpp}`
(3.1.2) — the `.hpp`s under `editor/include/aero/editor/`, the `.cpp`s under `editor/src/`.

Test inventory at HEAD, **re-measured, not carried forward**: **95** ctest entries with tools ON,
**6** with `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, **19** with `-DAERO_REFLECT_TOOLS=OFF`
alone — unchanged by tasks 3.1.1 or 3.1.2 or either task's code-review/gap-closing round, all of which
register **zero** new ctest entries (every new case lives inside an existing TU). `aero_tests` **415**
(389 after 3.1.1 → +26 at 3.1.2: `tests/content_hash_test.cpp`'s `CH1`–`CH26`, a new TU, the
`ContentHash` codec/order/hash-mix/MurmurHash3 battery, cross-checked against the published SMHasher
reference — see `docs/10-engineering-log.md`'s 3.1.2 entry for the exact hashes). `aero_editor_shell_test`
**617** (468 after 3.1.1's code-review round → +149 at 3.1.2: `tests/editor/text_file_test.cpp`
`TF1`–`TF22`, a new TU — the first direct coverage `readTextFile`/`writeTextFileAtomic`/`fileExists`
have ever had; `tests/editor/asset_cache_test.cpp` `IC1`–`IC34`/`IG1`–`IG6`/`IP1`–`IP40`, a new TU —
the index format, the golden byte-fixpoint battery, and the three pure planners; `tests/editor/
asset_database_test.cpp` `AD32`–`AD62`, extended — the eight-phase scan, the zero-write property
across two files, the hash budget, canonical-path alias dedup. **617 measured directly with
`--list-test-cases`, never derived by addition** — the standing lesson this project keeps re-verifying
rather than re-learning a third time). `aero_editor_imgui_test` **56** (52 after 3.1.1's code-review
round → +4 at 3.1.2: `I31`–`I34`, the cache-survives-a-reopen, edit-detection, Reimport-All and
unconditional-drain GPU-tier cases). Both reduced configurations, freshly rebuilt for this
documentation step (`build/tools-off-3.1.2`, `build/reflect-off-3.1.2`): `ctest -N` **6**/**19**
unchanged, `aero_editor_shell_test`'s own doctest `--count` reads **593** in **both** (up from 589
before 3.1.2's commit 10), all passing 100% — AC-17's claim for 3.1.2, the format/planners/scan need
no serialization and are present, not skipped, in every reduced configuration. `aero_scene_serialize_test`
**23** and `aero_editor_inspector_test` **22**, both unchanged since 3.1.1. `aero_editor_core` sources
**43** (41 after 3.1.1 → +2: `asset_cache.hpp`/`.cpp`) — **no new `target_link_libraries` entry**:
`aero_editor_core` already links `aero::core` PUBLIC and reaches the JSON layer through `aero::scene`.
`check-math-boundary.sh`'s scanned count: **255** after 3.1.1's code-review round **→ 262** at 3.1.2
(nine new/extended C-family files across Steps 1–8: `content_hash.{hpp,cpp}`, `content_hash_test.cpp`,
`text_file_test.cpp`, `asset_cache.{hpp,cpp}`, `asset_cache_test.cpp`, plus `project_files.cpp` and
`asset_database.cpp` extended in place; unchanged through Steps 9–11, since docs are not C-family) —
measured after `git add` at every step boundary, never assumed. Guard count stays **six**, but
`check-project-no-delete.sh`'s own scope widened a **second** time at task 3.1.2, from five files to
**six** (`asset_cache.cpp`, D18 — the cache's data is disposable, but nothing in either task deletes a
file to dispose of it; see `.claude/rules/editor.md`'s "Import cache (task 3.1.2)" section for the
nuance); the script's final line now reads "6 files scanned". Counts diverge by OS (Windows skips
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
