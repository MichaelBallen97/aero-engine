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
OPEN. Epic 3.1 (AssetDatabase · assets) is fully merged; Epic 3.2 (Importers) has three merged tasks
— 3.2.1 glTF, 3.2.2 FBX and 3.2.3 OBJ — and a fourth, 3.2.4 (Blender CLI), COMPLETE ON A BRANCH and
awaiting its PR.**

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

**Epic 3.2's three merged tasks, condensed.** **3.2.1 (glTF, fastgltf)** merged as PR #70 (`f02ca65`,
28 commits), macOS-validated ✅ PASS 12/12: the editor's first working importer and the first PRODUCER
for `AssetCacheEntry::dependencies`, so editing a texture a model references marks that model
`DependencyChanged` on the next scan. **3.2.2 (FBX, ufbx)** merged as PR #71 (`c597a5b`, 20 commits),
CI-green on all three lanes: the tree's FIRST vendored third-party library
(`editor/third_party/ufbx/`, byte-identical to upstream v0.23.0), a THIRD hard-coded-importer-identity
site the spec missed, a BLOCKING ASan heap-buffer-overflow in the Import Details panel that shipped
invisibly with 3.2.1, and x86_64-only UB inside ufbx's own DEFLATE decoder that CI caught and no local
arm64 run could. **Its macOS validation pass is STILL NOT RUN — thirteen rows open.** **3.2.3 (OBJ,
tinyobjloader)** merged as PR #72 (`c412e83`, 27 commits), macOS-validated ✅ PASS 13/13 (2026-08-10):
`.mtl` became a claimed importable file sharing OBJ's identity, so `chair.obj → chair.mtl → wood.png`
propagates through the existing transitive cascade with zero edits to `asset_database.cpp`; a
code-review round found ten gaps (one BLOCKING — `materialIndex` resolved against an index space
`convertMaterials` filters); and CI caught a Windows-only `LNK2038` between vcpkg's unsanitized
`tinyobjloader.lib` and this project's ASan-instrumented objects, fixed in `cmake/sanitizers.cmake`.

**3.2.4 (Blender CLI detection + `.blend` import) is COMPLETE ON THE BRANCH
`feat/3.2.4-blender-cli-blend-import` (cut from `main @ 1b1d335`, **21 green commits** — eleven for the
implementation, ten from the code-review round) and has NOT been pushed, has NO PR, and has NOT been
merged.** It is the fourth importer path and the only one that is
not an importer: it turns a file the editor refuses to parse into one it already parses, by running an
external program. It is the first place in this codebase that **spawns a process**, that **runs work
across frames**, and that **branches on the host OS in first-party editor code** — `currentHostOs()` is
exactly three lines in exactly one file, in the `#elif defined(__linux__)` + `#error` form, because the
bare-`#else` form produces two lines and silently falls back to Linux on an unknown host. `blender_tool.
{hpp,cpp}` is the pure half (candidates, version parsing, argv builders, the export script constant and
three JSON formats); `blender_process.{hpp,cpp}` is the tree's ONLY `SDL_Process` TU; `blender_service.
{hpp,cpp}` is one polled state machine with at most one live child; the `.blend` arm lives in
`ModelImportSession` and holds **the one and only `BlenderService::poll()` call site in the tree**.
`docs/09-file-formats.md` gained §7 (the export provenance record) and §8 (the tool preferences), with
"Reserved for future formats" renumbered §9. **Zero paths under `engine/` — the no-engine-change streak
reaches SIX — and, unlike 3.2.3, zero under `cmake/` too: this is the first task in Epic 3.2 that adds
no third-party code at all**, so `vcpkg.json`, the baseline, the submodule and `.github/` are all
byte-identical to `main` and 3.2.3's Windows `LNK2038` class is structurally unreachable.

**The spec was 52 commits stale and five of its statements were wrong**, each corrected before code was
written and each proved load-bearing: `AC-5`/`INV-B6` were falsified by 3.2.2's vendored ufbx (the
per-OS grep is re-scoped to `editor/src` + `editor/include`); `modelImporterNeedsExternalBuffers` is an
integration seam the spec never named, and the `<guid>.glb` artifact **deliberately never consults it**
(one `importModel` call, `Full` depth, empty external span, empty `assetRelativeDir`, plus one warning
if the artifact names any external URI); `AC-22`'s "zero processes" was unsatisfiable while the version
probe is itself a process, resolved by splitting `exportRunCount()`/`probeRunCount()`, resolving lazily
on a cache MISS, and comparing `blenderVersion` only when a version is known; a nil-GUID `.blend` is
`NeedsConversion` with a **disabled** button, never `NotImportable`; and `AC-25`'s write-site arithmetic
was off by one. Two further findings were only reachable by building it: **`resolve()` needed a fifth
parameter** (the version probe's stdout must go to a FILE, and no log path exists before an asset GUID
does — so `Library/BlenderExports/` holds **FIVE** files per asset set, not four), and **`applySettings`
on a `.blend` writes `"name": ""`, not `"name": "gltf"`**, correcting the plan's own prediction — the
empty pair is the right answer, and it makes the sidecar agree with the import-cache entry.

**A 36-seed sabotage matrix ran to completion: 27 matched, 6 are confirmed macOS non-discriminators
with their real cover named, and 4 GENUINE COVERAGE GAPS were found and closed**, each re-proven by
re-seeding afterwards. The four gaps: two seeds (an assets-root write, and writing `export_gltf.py`
unconditionally) reddened nothing because the case predicted to catch them, `BS31`, is a **pure cache
hit** where `startExport()` never runs — closed with `BS46`/`BS47`, which drive real conversions;
`MS36`'s first half was **vacuous** because its fixture was a cache MISS, where a widened `serviced`
guard costs no import either way — rewritten to use a cache HIT; and launching `Locate…` without the
in-flight guard is **unreachable by every runtime tier in this tree**, closed with a source-text
assertion in `I73`. Three seeds are worth naming for their shape: a blocking `SDL_WaitProcess` does not
fail the suite, it **hangs** it (the comment-stripped gate grep is the real cover); having the panel
call a mutating member is a **compile error**, because it holds a `const ModelImportSession*`; and
adding `.blend` to `isImportableModelName` does not merely fail cases, it **aborts the test binary**.
The plan's own prediction that seeds S26 and S27 would both go through `BS41` was wrong — the PATH and
the OBSERVABLE need separate proofs (`MS41`, a case the plan did not have, reads `serviceBlend`'s source
text; `BS41` asserts the warning), and each seed reddens exactly one of them.

**A CODE-REVIEW ROUND then found thirteen more items, TWO BLOCKING, none of them visible to the green
suite the matrix had just finished attacking — the fourth task running in which the two rounds each
catch what the other cannot.** Both blocking findings, and the sharpest should-fix, are **one mistake in
three costumes**: this task made `SessionState`/`BlenderState` span frames for the first time in the
editor, and each defect is a per-tick OUTPUT being read as the next tick's INPUT with nothing resetting
it. (1) A **cache-hit `.blend`** — the task's headline flow — drew a permanently false "Checking the
Blender version…" and **no controls at all**, because `BlenderState::Unknown` shared `Probing`'s panel
arm while a pure hit never resolves the service at all; there was no UI path to force a re-conversion.
(2) `setTarget()` never reset `stateValue`, so selecting a second `.blend` mid-run drew a running-Blender
readout against it, consumed the first run's completion on its behalf, and wrote it a provenance record
for an export that never ran. (3) A settled `.blend` **re-read and re-imported its whole GLB every tick**
while any other asset's probe or conversion was alive — `importCount()` 39 instead of 1 — breaking
AC-45's own "ten ticks cost ten early returns" quoted verbatim in the guard's comment. Also closed: the
panel named the **installed** Blender rather than the artifact's recorded producer; the version probe had
**no timeout** (a hung `--version` sat in `Probing` for the life of the editor); a cancel arriving
between the request and the spawn left the session **stuck in `Converting` forever**; `resolveBlender()`
built `/Library/BlenderExports` from an empty project root; AC-34's oversized-artifact arm was mapped to
a case that structurally could not reach it; and the log node's refused-by-cap branch was **undriven and
undrivable**, because the node shipped default-CLOSED and no tier here can click one. **AC-24's WORDING
was corrected rather than its behaviour**: the status document and the provenance record share
`<guid>.json` deliberately, so a run that starts destroys any previous record — AC-24 reads "leaves no
**valid** record", and the cost is now in the validation page's known-and-expected list. Every fix ships
with a case that reddens without it, verified by re-seeding: `BT63`, `BS48`–`BS53` and `I77`–`I80`.

**Carried-forward debt, unchanged by 3.2.4 and explicitly not part of any gate:** no Windows or Linux
validation pass exists for any of the thirteen Phase 2 tasks or for 3.1.1–3.1.4, 3.2.1, 3.2.2 or 3.2.3;
**3.2.2's macOS pass is still outstanding** (thirteen rows); 3.2.4's own validation page
(`editor/validation/3.2.4-blender-cli-blend-import.md`, fifteen rows) is pending on all three; and
Phase 0's gate is still held open on Windows/Linux 60 fps sign-off. That is platform-validation debt
spanning four phases, and it is worth scheduling as work of its own — the 2.2.5 lesson, one scale up.

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics (2.1 Editor shell, 2.2 Core panels, 2.3 Manipulation, 2.4 Undo/redo, 2.5 Scene I/O, 2.6 Project system v0) CLOSED in code and macOS-validated PASS, with Windows/Linux rows pending for every task (`editor/VALIDATION.md`). The whole-phase audit (2026-08-02) fixed two silent data-loss paths, a project root that was never made absolute, two CI false-greens, and four stale documentation claims. Full per-task and per-epic history — every defect, every sabotage matrix, every deviation — lives in `docs/10-engineering-log.md`'s Phase 2 entries; this row is deliberately a summary, not a duplicate. |
| **Phase 2 gate** | **MET 2026-08-02.** `samples/phase-2-editor-scene/` holds a project and a 4-entity scene authored entirely through the editor, with the save → New Scene → Open Scene round trip confirmed (`samples/phase-2-editor-scene/VALIDATION.md`); provenance is recorded there rather than asserted, since a hand-written `scene.json` is byte-identical to a real one and no test tier can tell them apart. Deliberately NOT `add_subdirectory`'d — this artifact is data (a provenance proof of the editor), not a compile-proof of engine code. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** Epic 3.1 (AssetDatabase · assets) is FULLY MERGED: 3.1.1/3.1.2/3.1.3/3.1.4 (PRs #65/#66/#67/#69), CI-green on all three platforms, sabotage-proven (26/31/35/25 seeds respectively), **macOS-validated ✅ PASS on all four** (14/14, 14/14, 16/16, 10/10) — Windows/Linux rows pending for all four. `engine::Guid`/`engine::ContentHash` (`engine/core`), the `.meta` v1 format, `AssetDatabase::rescan`'s eight phases, the machine-local `Library/asset-cache.json` import cache, and the real Asset Browser all shipped across these four. **Epic 3.2 (Importers) has three MERGED tasks and one COMPLETE ON A BRANCH.** **3.2.1 (glTF, fastgltf)** — PR #70 (`f02ca65`), sabotage-proven (32 seeds) and code-review-hardened (12 findings, 3 BLOCKING), macOS-validated ✅ PASS 12/12; the first PRODUCER for `AssetCacheEntry::dependencies`. **3.2.2 (FBX, ufbx)** — PR #71 (`c597a5b`), sabotage-proven (35 seeds) and code-review-hardened (5 gaps, 2 BLOCKING); the tree's first vendored library, a third hard-coded-importer-identity site, a BLOCKING ASan heap-buffer-overflow that shipped invisibly with 3.2.1, and x86_64-only UB in ufbx's DEFLATE decoder that only CI could see. **Its macOS validation pass is still NOT RUN — thirteen rows open.** **3.2.3 (OBJ, tinyobjloader)** — PR #72 (`c412e83`), the original 32-seed matrix run to completion plus a code-review round (ten gaps, one BLOCKING), macOS-validated ✅ PASS 13/13 (2026-08-10); `.mtl` became a claimed importable file, and CI caught a Windows-only `LNK2038` fixed in `cmake/sanitizers.cmake`. **3.2.4 (Blender CLI detection + `.blend` import) is COMPLETE on `feat/3.2.4-blender-cli-blend-import` (21 green commits: eleven for the implementation, ten from the code-review round) — NOT pushed, NO PR, NOT merged.** Mechanical gate green: 95/95 on both macOS presets with `AERO_REQUIRE_GPU=1`, both reduced configurations rebuilt FRESH at **1342** cases each with `BT1` and `BS1` present in both, six guards passing, `.github/scripts/` and `cmake/` byte-identical to `main`, clang-format and clang-tidy clean by exit code. `aero_editor_shell_test` **1232 → 1366**, `aero_editor_imgui_test` **90 → 102**, `aero_tests` **415** unchanged. A 36-seed sabotage matrix ran to completion: 27 matched, 6 confirmed macOS non-discriminators (S6/S17 — ASan does not report either, Linux LSan and the Windows overwrite are their cover; S24/S24b — macOS `std::set` moves are `noexcept`, so the Windows lane's aggregate `static_assert` is the only cover; S33 — POSIX kill semantics; S30 — unreachable by any runtime tier), and **4 genuine coverage gaps found and closed** (`BS46`, `BS47`, `MS36`'s rewrite, and a source-text guard assertion in `I73`), each re-proven by re-seeding. **A CODE-REVIEW ROUND then found thirteen more items, TWO BLOCKING** — a cache-hit `.blend` drew a permanently false "Checking the Blender version…" with no controls at all, and a selection change mid-conversion attributed one asset's run to another (importing its artifact and writing it a provenance record for an export that never ran) — plus a settled `.blend` re-importing its whole GLB every tick, the panel naming the installed Blender rather than the artifact's recorded producer, a version probe with no timeout, a cancel-before-spawn that stuck the session in `Converting` forever, an export directory built from an empty project root, AC-34's mapping naming a case that could not reach it, and an undrivable log branch behind a default-CLOSED `TreeNode`. AC-24's WORDING was corrected (it leaves no **valid** record; the status document and the provenance record share `<guid>.json` deliberately) rather than its behaviour. All closed on the same branch, each with a case that reddens without it: `BT63`, `BS48`–`BS53`, `I77`–`I80`. Zero paths under `engine/` for 3.2.1, 3.2.2, 3.2.3 and 3.2.4 — the no-engine-change streak reaches **six**. Full detail for every task in `docs/10-engineering-log.md`'s Phase 3 entries. |
| **Next task** | **Push `feat/3.2.4-blender-cli-blend-import`, open its PR, and merge it once CI is green with the green run's `headSha` asserted EQUAL to `HEAD`** — pushing cancels the in-flight run, so merging without that check merges an untested commit. Merge with a MERGE COMMIT, never a squash. Then run 3.2.4's own macOS validation pass (`editor/validation/3.2.4-blender-cli-blend-import.md`, fifteen rows; rows 2, 3, 6 and 12 are the only cover for the requirement-level claims with no automated proof, and row 12 is R4's measurement of a large `.blend`'s wall-clock time against the five-minute timeout). **The other Epic 3.2 item still open is 3.2.2's own macOS validation pass** (`editor/validation/3.2.2-fbx-import-ufbx.md`, thirteen rows). **3.1.5 (drag-into-scene)** is otherwise fully unblocked: it depends on 3.1.3 and 3.2.1 (both merged), `ImportedModel` gives it something to reference, and with 3.2.2/3.2.3/3.2.4 in place it can be dragged an `.fbx`, an `.obj` or a `.blend` as easily as a `.gltf`. 3.1.5 owns two decisions 3.2.1 deliberately left open: **sub-asset identity** (D13) and **replacing `LOCAL_MESH_HALF_EXTENT`** (2.3.1's knowingly-wrong constant). See `docs/tasks/phase-3.md`. The remaining carried-forward item is **platform-validation debt, now spanning four phases**: no Windows or Linux validation pass exists for any of the thirteen Phase 2 tasks, for 3.1.1–3.1.4, or for 3.2.1–3.2.4, and Phase 0's gate is still held open on Windows/Linux 60 fps sign-off. Every macOS pass to date has been clean, which is exactly why the other two lanes staying unvalidated is worth scheduling as work of its own — 2.2.5's lesson at phase scale. |

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
second time, 3.1.3 restarted the streak at one, 3.1.4 made it two, 3.2.1 made it three, 3.2.2 made it
four, 3.2.3 made it five, and 3.2.4 now makes it SIX**. 3.2.3 touched `cmake/sanitizers.cmake` — the
first non-`editor`/`tests`/docs path in this epic, forced by its Windows-only `LNK2038`; that is a
build-system path, not an `engine/` one, so the streak held. **3.2.4 touches neither**: its diff against
`main` is confined to `editor/`, `tests/`, `docs/` and `.claude/rules/`, with `engine/`, `cmake/`,
`.github/` and `vcpkg.json` all byte-identical.
`/editor` gained **ten**
new `.hpp`/`.cpp` pairs across 2.6.2, 3.1.1,
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
`CMakeLists.txt`), byte-identical to upstream v0.23.0 and never to be patched locally. **3.2.3 adds ONE
more pair**, `obj_import.{hpp,cpp}` (src-private, the only tinyobjloader TU) — no
new `third_party/` directory, since tinyobjloader is a normal vcpkg port. **3.2.4 adds THREE more
pairs**: `blender_tool.{hpp,cpp}` (PUBLIC and PURE — no disk, no SDL, no `<filesystem>`, no logging),
`blender_service.{hpp,cpp}` (PUBLIC, the polled state machine, naming no SDL type at all) and
`blender_process.{hpp,cpp}` (src-private, the tree's ONLY `SDL_Process` TU, holding the handle as a
`void*` so its own header names no SDL type either). It adds no `third_party/` directory and no
dependency of any kind — Blender is an external PROCESS, never a library. The `.hpp`s live
under `editor/include/aero/editor/` (except the six named src-private), the `.cpp`s under
`editor/src/`.

Test inventory **on `feat/3.2.4-blender-cli-blend-import` at its code-review docs commit**, every number
**re-measured at that point, never carried forward from an earlier step or an earlier task**: **95**
ctest entries with tools ON, **6** with `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, **19** with
`-DAERO_REFLECT_TOOLS=OFF` alone — **unchanged by 3.2.4** (zero new `add_test`; every new case lives
inside an existing target). `aero_tests` **415** (unchanged — `engine/` is untouched).
`aero_editor_shell_test` **1366** (1232 at `1b1d335` → **+134**: two new TUs,
`tests/editor/blender_tool_test.cpp` (BT1–BT63) and `tests/editor/blender_service_test.cpp`
(BS1–BS53), plus `model_import_session_test.cpp` (MS34–MS41 and `MS32`'s rewrite),
`model_import_test.cpp` (MI133), `text_file_test.cpp` (TF33–TF37) and `scene_session_test.cpp`
(SS37–SS40)) — **measured directly with `--list-test-cases`, never derived by addition**.
`aero_editor_imgui_test` **102** (90 → +12: I69–I80). `aero_scene_serialize_test` **23** and
`aero_editor_inspector_test` **22**, both unchanged since 3.1.1. **Both reduced configurations, built
FRESH in `build/tools-off-3.2.4-cr` / `build/reflect-off-3.2.4-cr`, read 1342 cases EACH with `BT1` and
`BS1` present in both** — proving the whole Blender path needs neither reflection nor scene
serialization.
`aero_editor_core` sources **56** (53 → +3: `blender_tool.cpp`, `blender_process.cpp`,
`blender_service.cpp`) with **no** new `find_package`, **no** new `target_link_libraries` entry and
**no** `vcpkg.json` change: `aero_editor_core` already links `SDL3::SDL3` PRIVATE, which is the entire
dependency this task needs. `check-math-boundary.sh`'s scanned count **294 → 302** (eight new tracked
C-family files) and `check-project-no-delete.sh`'s Check B scan **54 → 57**, both picked up
automatically — **neither script changes, and `.github/scripts/` is byte-identical to `main`**, unlike
3.2.3's gate. Guard count stays **six**. Check A's six-file denylist and Check B's two-file
`PERMITTED_DELETERS` are both unchanged in membership, and all three new `editor/src/*.cpp` are in
**neither**, which is exactly what makes a future `std::filesystem::remove` in them a hard CI failure
(sabotage-confirmed: the guard fires before any test binary runs). `git grep -nE
'_WIN32|__APPLE__|__linux__' -- editor/src editor/include` reads **exactly three lines in one file**.
`grep -c '\.service(' tests/editor/model_import_session_test.cpp` **42 → 61**, with **none of the
original 42 edited** — the trailing defaulted parameter's whole point. Counts diverge by OS (Windows
skips `golden-rule.include_scan_e2e`, and **fourteen** whole `BS` cases plus one arm of `BS11` and one
GPU case, `I80`, skip loudly there), so never assume one — measure with `ctest -N` and
`--list-test-cases`. **Those skips are not a random sample**: together they are the only coverage
anywhere of the two timeouts, cancellation, the `Converted` state and therefore the whole end-to-end
conversion, `ok: false`, exit-0-with-no-status, both `ArtifactUnusable` arms, a selection change during a
live run, and the refused-by-cap log — so on Windows none of those arms is executed by any test at all,
and the plan's own §V2 R5 claim that "they hold identically on all three lanes" is inaccurate as
written. It belongs in the Windows validation pass's own list, not in a footnote about counts.

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
