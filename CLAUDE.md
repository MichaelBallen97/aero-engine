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
OPEN. Epic 3.1 (AssetDatabase · assets) is fully merged; Epic 3.2 (Importers) has one merged task and
a second code-complete on its own branch.**

**Epic 3.1, condensed** (full per-task detail, every sabotage matrix and every build-time finding live
permanently in `docs/10-engineering-log.md`'s Phase 3 entries — this paragraph is a summary, not a
duplicate): **3.1.1** (GUIDs + `.meta` files) merged as PR #65 (`2be73e1`, 26/26 sabotage seeds),
**macOS-validated ✅ PASS 14/14**. **3.1.2** (import cache & dependency tracking) merged as PR #66
(`3470b87`, 31/31 seeds), closing 3.1.1's D8 orphan-re-attachment deferral and a carried-forward
symlinked-directory duplicate-GUID defect, **macOS-validated ✅ PASS 14/14**. **3.1.3** (asset browser
v1) merged as PR #67 (`aa914fb`, 35/35 seeds; code review found 11 findings, 3 BLOCKING, the sharpest a
real GPU-texture use-after-free invisible on macOS because SDL frees synchronously on Vulkan/D3D12 but
only defers on Metal), gave the Asset Browser real thumbnails, search and the Issues list,
**macOS-validated ✅ PASS 16/16**. **3.1.4** (hot-reload file watcher) merged as PR #69 (`ebc4da6`,
25/25 seeds; code review closed six gaps against an already-green gate), added `AssetWatcher`,
`AssetDatabase::generation()` and `ThumbnailLedger::supersededBy()`, **macOS-validated ✅ PASS 10/10** —
**R1's numeric per-sweep cost stayed unmeasured**, open debt its own risk register asked for by number.

**3.2.1 (glTF import, fastgltf) is MERGED to `main` as PR #70 (merge commit `f02ca65`, 28 commits),
CI-green on all three platforms with the green run's `headSha` asserted equal to `HEAD` before merging,
and macOS-validated ✅ PASS 12/12 (2026-08-09) with no defects found.** It is the editor's first working
importer and the first **producer** for `AssetCacheEntry::dependencies` (3.1.2's own field, unfed until
then): phase 7.5 turns every resolved external URI into a dependency GUID during the scan, so editing a
texture a model references marks that model `DependencyChanged` on the next scan — the roadmap's own
headline example, working end to end. `gltf_import.{hpp,cpp}` is the only fastgltf TU in the tree;
`model_import.{hpp,cpp}` holds the canonical, third-party-free `ImportedModel`; `model_import_session.
{hpp,cpp}` drives the on-demand two-pass import; `import_details_panel.{hpp,cpp}` is a six-section
panel, all sections default-open. `.meta` gained an optional, additive `importer` block at format
version 1. **Zero paths under `engine/`** — the no-engine-change streak reached three. A **12-finding
code review (3 BLOCKING)** and a separate **32-seed sabotage matrix** (18 matched, 1 non-discriminator,
3 contingencies, 10 findings, six coverage gaps found and five closed) both ran against a fully green
gate and each found what the other missed — the clearest evidence the two rounds are not redundant.
**R4 closed with a measurement**: a steady-state scan is size-independent (0 models probed, 0 bytes
read) and costs ~1.0–1.1 ms, ~6% of a 16.7 ms frame, whichever fixture size is used.

**3.2.2 (FBX import, ufbx) is MERGED to `main` as PR #71 (merge commit `c597a5b`, 20 commits: one
build, one test-harness, ten feature, five fix/coverage, three docs), CI-green on macOS, Windows and
Ubuntu with the green run's `headSha` asserted equal to `HEAD` before merging.** Mechanical gate
green: 95/95 on both presets with `AERO_REQUIRE_GPU=1`, both reduced configurations rebuilt fresh at
**1088** cases each with `FI1` present in both, six guards passing, clang-format/clang-tidy clean by
exit code. **The macOS validation pass has NOT been run** — `editor/validation/3.2.2-fbx-import-ufbx.md`
holds thirteen unchecked rows, and rows 2, 3, 5 and 11 are the only cover for the requirement-level
claims with no automated proof. `editor/third_party/ufbx/` is
the **first vendored third-party library in this tree** (v0.23.0, byte-identical to upstream, MIT/
Unlicense, `aero_ufbx` STATIC over one ASan/UBSan-instrumented `.c`, PRIVATE to `aero_editor_core`) —
vendored because ufbx is in no vcpkg registry, not by preference. `fbx_import.{hpp,cpp}` is the only
ufbx TU anywhere; ufbx never touches the filesystem (`ufbx_load_memory` only). The axis/unit conversion
is three `ufbx_load_opts` fields, never hand-rolled coordinate math. `modelImporterIdentity` became a
genuinely **per-format** pure function, closing a gap the plan itself found only two thirds of: beyond
the two hard-coded-`GLTF_IMPORTER_NAME` sites the spec named (`asset_database.cpp`'s phase 7.5 probe,
`import_details_panel.cpp`'s Overview line), a **third** turned up in `asset_meta.cpp`'s `writeMetaText`
while wiring `applySettings()` for FBX — closed with two trailing, defaulted parameters rather than a
new boundary-crossing type. A **BLOCKING, ASan-confirmed heap-buffer-overflow** was found and fixed in
`import_details_panel.cpp`'s Hierarchy and Skins sections, both of which indexed `ImportedModel::nodes`
directly by `ImportedNode::localId` since task 3.2.1 shipped — harmless for glTF, where the two always
coincide, and a real out-of-bounds read for the first FBX hierarchy with more than one node.

**Two adversarial rounds ran against a green gate and each found what the other missed.** The
**35-seed sabotage matrix** graded 20 matched · 5 predicted non-discriminators · 5 differently-shaped
findings · **2 real coverage gaps found and closed** — `load_external_files` and `pivot_handling` each
reddened nothing, because for FBX the first gates only `scene.cache_files` (no texture fixture reaches
it) and no fixture anywhere declared a pivot. The **code-review round then found 5 gaps, 2 BLOCKING**,
none visible to a 1106-case green suite: `Structure` and `Full` disagreed about the URI set for an
embedded texture (`ignore_all_content` folds in `ignore_embedded`, so an embedded texture's
`RelativeFilename` was recorded as an ordinary external URI, putting a dependency edge on a file the
model does not need — fixed by setting `ignore_geometry`/`ignore_animation` individually, so **AC-21
now asserts `embedded_ignored == false` at Structure**); and **AC-20's only automated proof was
vacuous**, comparing `0 == 0` on five of eight clauses against a fixture with no textures, materials,
skins or animations — which is why both blocking defects survived to review at all.

**CI caught what neither round could: undefined behaviour inside ufbx's own DEFLATE decoder**
(`shift exponent 136 is too large`), reachable only through the compressed binary fixture and only on
x86_64 — `ufbxi_wrap_shr64`'s fast path is guarded on `UFBXI_ARCH_X64`, so an arm64 Mac already takes
the well-defined branch and no local run could ever have seen it. Fixed with ufbx's own **`UFBX_UBSAN`**
macro applied from `editor/third_party/ufbx/CMakeLists.txt` under `AERO_ENABLE_SANITIZERS AND NOT
MSVC` — a build-system knob, never a patch to the vendored source, so INV-F5 holds. **The recurring
lesson across all three defects: they are each two things that coincide for glTF and diverge for FBX**
(`localId` vs array index; embedded-with-a-path vs `data:` URI; arm64 vs x64 shift semantics), and none
was reachable by reasoning from the backend that shipped first.

Zero paths under `engine/` — the no-engine-change streak reaches **four**. Full detail — the measured D6 conversion
table, all six MUST-VERIFY answers, every build-time finding including three separate test-numbering
collisions with post-merge review-round additions, and the honest FI72 allocator-cap resolution — is in
`docs/10-engineering-log.md`'s 3.2.2 entry.

**Carried-forward debt, unchanged by 3.2.1/3.2.2 and explicitly not part of any gate:** no Windows or
Linux validation pass exists for any of the thirteen Phase 2 tasks or for 3.1.1–3.1.4 or 3.2.1, and
Phase 0's gate is still held open on Windows/Linux 60 fps sign-off. That is platform-validation debt
spanning three phases now, and it is worth scheduling as work of its own — the 2.2.5 lesson, one scale
up.

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics (2.1 Editor shell, 2.2 Core panels, 2.3 Manipulation, 2.4 Undo/redo, 2.5 Scene I/O, 2.6 Project system v0) CLOSED in code and macOS-validated PASS, with Windows/Linux rows pending for every task (`editor/VALIDATION.md`). The whole-phase audit (2026-08-02) fixed two silent data-loss paths, a project root that was never made absolute, two CI false-greens, and four stale documentation claims. Full per-task and per-epic history — every defect, every sabotage matrix, every deviation — lives in `docs/10-engineering-log.md`'s Phase 2 entries; this row is deliberately a summary, not a duplicate. |
| **Phase 2 gate** | **MET 2026-08-02.** `samples/phase-2-editor-scene/` holds a project and a 4-entity scene authored entirely through the editor, with the save → New Scene → Open Scene round trip confirmed (`samples/phase-2-editor-scene/VALIDATION.md`); provenance is recorded there rather than asserted, since a hand-written `scene.json` is byte-identical to a real one and no test tier can tell them apart. Deliberately NOT `add_subdirectory`'d — this artifact is data (a provenance proof of the editor), not a compile-proof of engine code. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** Epic 3.1 (AssetDatabase · assets) is FULLY MERGED: 3.1.1/3.1.2/3.1.3/3.1.4 (PRs #65/#66/#67/#69), CI-green on all three platforms, sabotage-proven (26/31/35/25 seeds respectively), **macOS-validated ✅ PASS on all four** (14/14, 14/14, 16/16, 10/10) — Windows/Linux rows pending for all four. `engine::Guid`/`engine::ContentHash` (`engine/core`), the `.meta` v1 format, `AssetDatabase::rescan`'s eight phases, the machine-local `Library/asset-cache.json` import cache, and the real Asset Browser all shipped across these four. **Epic 3.2 (Importers): 3.2.1 (glTF, fastgltf) MERGED as PR #70 (`f02ca65`, 28 commits), sabotage-proven (32 seeds, 10 findings) and code-review-hardened (12 findings, 3 BLOCKING), macOS-validated ✅ PASS 12/12.** It is the first PRODUCER for `AssetCacheEntry::dependencies` (3.1.2's own field) — editing a texture a model references now marks that model `DependencyChanged` on the next scan. **3.2.2 (FBX, ufbx) MERGED as PR #71 (`c597a5b`, 20 commits), CI-green on all three platforms with `headSha == HEAD` asserted, sabotage-proven (35 seeds: 20 matched / 5 predicted non-discriminators / 5 differently-shaped / 2 gaps found and closed) and code-review-hardened (5 gaps, 2 BLOCKING, all closed). macOS validation NOT yet run — thirteen rows open.** It is the first vendored third-party library in the tree (`editor/third_party/ufbx/`, byte-identical to upstream v0.23.0) and closes a THIRD hard-coded-importer-identity site the spec itself missed, plus a BLOCKING ASan heap-buffer-overflow in the Import Details panel's Hierarchy/Skins sections that shipped invisibly with 3.2.1 and was only reachable once an FBX hierarchy existed to trigger it. CI additionally caught UB inside ufbx's own DEFLATE decoder that is x86_64-only and therefore invisible to every local arm64 run — closed with ufbx's `UFBX_UBSAN` macro from the build system, never a patch to the vendored source. Zero paths under `engine/` for both 3.2.1 and 3.2.2 — the no-engine-change streak reaches **four**. Full detail for every task in `docs/10-engineering-log.md`'s Phase 3 entries. |
| **Next task** | **3.1.5 (drag-into-scene)** — now fully unblocked: it depends on 3.1.3 and 3.2.1 (both merged), `ImportedModel` gives it something to reference, and with 3.2.2 merged it can be dragged an `.fbx` as easily as a `.gltf`. **First, though, 3.2.2's own macOS validation pass is outstanding** (`editor/validation/3.2.2-fbx-import-ufbx.md`, thirteen rows) — row 11 in particular is the behavioural cover for the embedded-texture dependency path the code-review round had to fix, and rows 2/3/5 are the only cover for the requirement-level claims with no automated proof. 3.1.5 owns two decisions 3.2.1 deliberately left open: **sub-asset identity** (D13 — both a stable `localId` and the source `name` are recorded for every mesh/material/skin/animation, with a fixed ordering rule) and **replacing `LOCAL_MESH_HALF_EXTENT`** (2.3.1's knowingly-wrong constant). See `docs/tasks/phase-3.md`. The remaining carried-forward item is **platform-validation debt, now spanning three phases**: no Windows or Linux validation pass exists for any of the thirteen Phase 2 tasks, for 3.1.1–3.1.4, or for 3.2.1, and Phase 0's gate is still held open on Windows/Linux 60 fps sign-off. Schedule it as work of its own rather than as a ride-along row — 2.2.5's lesson at phase scale. |

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
second time, 3.1.3 restarted the streak at one, 3.1.4 made it two, 3.2.1 made it three, and 3.2.2 now
makes it four**: it needs no `engine/` change at all (`git diff --name-only main...HEAD -- engine/` is
empty on the feature branch). `/editor` gained **ten** new `.hpp`/`.cpp` pairs across 2.6.2, 3.1.1,
3.1.2, 3.1.3 and 3.1.4 (`project_settings.{hpp,cpp}` / `project_settings_panel.{hpp,cpp}` (2.6.2),
`asset_meta.{hpp,cpp}` / `asset_database.{hpp,cpp}` (3.1.1), `asset_cache.{hpp,cpp}` (3.1.2),
`asset_view.{hpp,cpp}` / `thumbnail_cache.{hpp,cpp}` / `thumbnail_store.{hpp,cpp}` (src-private) /
`asset_actions.{hpp,cpp}` (3.1.3), `asset_watcher.{hpp,cpp}` (3.1.4)); **3.2.1 added four more pairs
plus one deliberate exception**: `model_import.{hpp,cpp}`, `gltf_import.{hpp,cpp}` (src-private, the
only fastgltf TU), `model_import_session.{hpp,cpp}` and `import_details_panel.{hpp,cpp}` (src-private,
the only ImGui TU that task added) are real pairs; `import_settings.hpp` is a HEADER WITH NO `.cpp`,
deliberately alone (plan §A-11 — `ImportSettings` is shared by `asset_meta.hpp` and
`model_import.hpp`, and giving it its own tiny, dependency-free header is what stops `asset_meta.hpp`
from dragging in `aero::scene` and the whole math umbrella). **3.2.2 adds ONE more pair**,
`fbx_import.{hpp,cpp}` (src-private, the only ufbx TU) — and, separately, `/editor` gains its FIRST
`third_party/` directory, `editor/third_party/ufbx/` (`ufbx.h`, `ufbx.c`, `LICENSE`, `README.md`,
`CMakeLists.txt`), byte-identical to upstream v0.23.0 and never to be patched locally. The `.hpp`s live
under `editor/include/aero/editor/` (except the four named src-private), the `.cpp`s under
`editor/src/`.

Test inventory at HEAD (`c597a5b`, `main`), **re-measured after merge, not carried forward**: **95**
ctest entries with tools ON, **6** with `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, **19** with
`-DAERO_REFLECT_TOOLS=OFF` alone — unchanged by either 3.2.1 or 3.2.2 (zero new ctest entries; every
new case lives inside an existing TU). `aero_tests` **415** (unchanged — neither task touches
`engine/`). `aero_editor_shell_test` **1112** (997 at 3.2.1 → **+115** at 3.2.2: a new TU,
`tests/editor/fbx_import_test.cpp` (**74** cases spanning FI1–FI79, with five deliberate gaps —
FI28, FI43, FI46, FI68, FI72 — each documented in the file as unreachable from a tier-0 fixture
rather than shipped as a case that only looks like proof), plus `model_import_test.cpp` (+15,
MI103–MI117), `model_import_session_test.cpp` (+6,
MS22–MS27, renumbered from the plan's own predicted MS24–MS29 — the file's highest EXISTING case was
MS21, not MS23), `asset_meta_test.cpp` (+6, AM-i15–AM-i20), `asset_cache_test.cpp` (+6,
AC-p9–AC-p14) and `asset_database_test.cpp` (+8, AD-i14–AD-i21, renumbered from the plan's own
predicted AD-i11–AD-i18 — a post-3.2.1-merge review round had already claimed AD-i11–AD-i13) — **1112
measured directly with `--list-test-cases`, never derived by addition**. `aero_editor_imgui_test`
**89** (83 at 3.2.1 → +6 at 3.2.2: I62–I67, renumbered from the plan's own predicted I61–I66 — a
post-3.2.1-merge review round had already claimed I61). `aero_scene_serialize_test` **23** and
`aero_editor_inspector_test` **22**, both unchanged since 3.1.1. `aero_editor_core` sources **52** (51
at 3.2.1 → +1: `fbx_import.cpp`) — no new `find_package` beyond ufbx's own vendored `add_subdirectory`,
one new PRIVATE `target_link_libraries` entry (`aero::ufbx`, declared by `aero_editor_core` and by no
other target). **The checkable invariant is the INCLUDE boundary, not the link line, and an earlier
draft of this paragraph got that wrong.** Measured from `compile_commands.json`: ufbx's `SYSTEM PUBLIC`
include directory reaches **zero** test TUs and **zero** `engine/` TUs. The static archive itself
*does* appear on `aero_editor`'s and the test targets' link lines, and always will — a PRIVATE
dependency of a STATIC library propagates as `$<LINK_ONLY:…>`, exactly as `fastgltf` has since 3.2.1.
Any future criterion phrased as "nowhere on their link lines" is unsatisfiable by construction.
`check-math-boundary.sh`'s scanned count: **287** at 3.2.1 **→ 291** at 3.2.2
(four new C-family files — `ufbx.h`, `fbx_import.hpp`, `fbx_import.cpp`, `fbx_import_test.cpp`; `ufbx.c`
is not in its extension set) — measured after `git add` at every step boundary, never assumed. Guard
count stays **six**. **`.github/scripts/` is NO LONGER byte-identical to `main`, unlike every prior
task's gate** — `git diff main...HEAD -- .github/scripts/` shows exactly 2 changed lines, both
comments, in `check-rhi-boundary.sh` (it claimed miniaudio was the only vendored `.c` in the tree;
there are now two). `check-project-no-delete.sh`'s own logic is unchanged — Check A's six-file denylist
and Check B's two-file `PERMITTED_DELETERS` allowlist are both unchanged in membership (`fbx_import.cpp`
is in neither, which is what makes a future `std::filesystem::remove` in it a hard CI failure), Check
B's scanned-file count grows to **53** (the glob picks the new file up automatically). Both reduced
configurations, freshly rebuilt in `build/tools-off-3.2.2`/`build/reflect-off-3.2.2`: `ctest -N`
**6**/**19** unchanged, both passing 100% (6/6 and 19/19), `aero_editor_shell_test`'s own doctest
`--list-test-cases` count reads **1088** in **both** (up from 952 at 3.2.1), with `FI1` present in
both — proving the FBX importer TU needs neither reflection nor scene serialization (AC-68). Counts
diverge by OS (Windows skips `golden-rule.include_scan_e2e`), so never assume one — measure with
`ctest -N`.

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
