# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Aero Engine — an open-source (MIT), cross-platform 3D game engine with an editor and per-project TypeScript **or** C++ scripting. Solo project, started July 2026. The goal is core-workflow parity with Unity/Godot (edit → script → play → export), explicitly **not** feature parity. 3D-first; 2D arrives in Phase 7.

Two platform matrices, never to be conflated: the **editor** runs on macOS/Windows/Linux only; the **runtime** (exported games) targets those three plus iOS and Android. The editor never runs on mobile — no touch UI, no adaptive layouts.

## Current state — read this first

**Phase 2 (Editor) is COMPLETE — gate met 2026-08-02**, all six epics closed, every task
macOS-validated, gate artifact at `samples/phase-2-editor-scene/`. A whole-phase audit (2026-08-02)
found and fixed two silent data-loss paths, a never-absolute project root, and four stale
documentation claims.

**Phase 3 (Asset Pipeline & 3D Content) is OPEN, and Epic 3.3 (Cooker v0) has now opened.** Epic 3.1
(AssetDatabase · assets) is fully merged (3.1.1–3.1.4). Epic 3.2 (Importers) has **five** merged tasks
— 3.2.1 glTF, 3.2.2 FBX, 3.2.3 OBJ, 3.2.4 Blender CLI, 3.2.5 Assimp (DAE/PLY/STL), the last merged as
PR #74 (`7e0224f`) on 2026-08-12. **3.2 is closed in code; 3.1.5 (drag-into-scene) is the one Epic 3.1
task still open.** **3.3.1 (Mesh cook → GPU buffers) is COMPLETE ON ITS BRANCH
(`feat/3.3.1-mesh-cook-gpu-buffers`, 13 commits) and NOT YET MERGED** — CI-green after one Windows-only
fix, sabotage-proven, code-review-closed, docs written; it still needs a green run whose `headSha`
equals `HEAD`, and the merge.

**Epics 3.1 and 3.2, condensed.** Full per-task detail — every sabotage matrix, every build-time
finding, every dead end — lives permanently in `docs/10-engineering-log.md`'s Phase 3 entries; this is
a summary, not a duplicate. **3.1.1** GUIDs + `.meta` (PR #65 `2be73e1`, 26 seeds, ✅ 14/14).
**3.1.2** import cache & dependency tracking (PR #66 `3470b87`, 31 seeds, ✅ 14/14), closing 3.1.1's
orphan-re-attachment deferral and a symlinked-directory duplicate-GUID defect. **3.1.3** asset browser
v1 (PR #67 `aa914fb`, 35 seeds, 11 review findings incl. a GPU-texture use-after-free invisible on
macOS because SDL frees synchronously on Vulkan/D3D12 and only defers on Metal, ✅ 16/16).
**3.1.4** hot-reload watcher (PR #69 `ebc4da6`, 25 seeds, ✅ 10/10) — R1's per-sweep cost stayed
unmeasured. **3.2.1** glTF/fastgltf (PR #70 `f02ca65`, 32 seeds, 12 review findings, ✅ 12/12), the
first PRODUCER for `AssetCacheEntry::dependencies`. **3.2.2** FBX/ufbx (PR #71 `c597a5b`, 35 seeds,
✅ 13/13) — the tree's FIRST vendored library (`editor/third_party/ufbx/`, byte-identical to upstream
v0.23.0), a third hard-coded-importer-identity site, a BLOCKING ASan heap-buffer-overflow that shipped
invisibly with 3.2.1, and x86_64-only UB in ufbx's DEFLATE decoder no local arm64 run could see.
**3.2.3** OBJ/tinyobjloader (PR #72 `c412e83`, 32 seeds, 10 review findings, ✅ 13/13) — `.mtl` became
a claimed importable file, and CI caught a Windows-only `LNK2038` fixed in `cmake/sanitizers.cmake`.
**3.2.4** Blender CLI + `.blend` (PR #73 `5ab07f3`, 36 seeds, 13 review findings, ✅ 15/15) — the
tree's first process spawn, first across-frames work, and only per-OS branch in first-party editor
code. **3.2.5** Assimp DAE/PLY/STL (PR #74 `7e0224f`, 36 seeds, 6 review findings, ✅ 14/14) — the
fourth parser; Ubuntu LSan caught 4704 B in 17 allocations on assimp's own error paths, closed with
two frame-scoped `tests/lsan.supp` entries.

**3.3.1 — Mesh cook → GPU buffers, the current branch.** Epic 3.3's first task and **the first thing
in this project that produces a runtime-consumable artifact**: a file whose whole design premise is
that the thing reading it does no parsing at all. Three pieces land. **`engine/assets` OPENS** as
`aero::assets` — the first new engine subsystem since 1.4.2 — holding the versioned `.aeromesh`
container v1 (`cooked_mesh.{hpp,cpp}`: frozen enums, eight `constexpr` little-endian byte primitives,
and a hostile-input parser that never throws, never reads a file, never logs, and validates every
range by subtraction) and the cook (`mesh_cook.{hpp,cpp}`: classify → sort → caps → layouts → width →
offsets → emit). Both are PURE and link **`aero::core` + `aero::profiling` and NO vcpkg package at
all**, which is what makes their `PRIVATE` links a genuine compile-time boundary rather than the usual
convention-plus-grep (R12). **`/editor` gains one adapter pair**, `mesh_cook_source.{hpp,cpp}`, two
pure functions and no UI. **`/tools/cooker` opens** as the third first-party CLI, `aero_cooker mesh`,
linking `aero::editor_core` for the five importer paths it refuses to re-implement — legal because
`tools/` sits outside the golden rule on **both** halves (`check-golden-rule.sh` scans `engine` and
`runtime`; `aero_assert_golden_rule`'s `CONSUMER_DIRS` is the same pair).

**It is the tree's first BINARY format**, so every determinism guarantee `docs/09` states for canonical
*text* had to be re-derived for bytes. The answer is structural: no struct `memcpy`, no hash container,
no timestamp/path/hostname/build-id, explicit little-endian assembly through primitives whose
endianness is a **`static_assert`** rather than a test, a zero-initialized output buffer, and a sort
that runs **before** the cap pass so a shuffled input cannot produce a different file. `docs/09` gained
a full normative **§9** (with the old "Reserved for future formats" renumbered to §10, not replaced);
`docs/03`'s asset-flow diagram was corrected — the importers produce an in-memory `ImportedModel`, not
a glTF on disk, and the `.blend` path's GLB is the one on-disk intermediate.

**Nine spec statements were wrong and each was corrected before code was written**; three were
load-bearing. `AC-4`'s "every offset is 16-aligned" contradicts the layout's own table — only the two
**stored** offsets are 16-aligned, the three tables are packed, and a one-attribute file legitimately
puts the section table at **104**. The spec's cap pass ran in **input** order, so a shuffled input
produced a different file — and only on inputs that trip a cap, the exact combination a green suite
never exercises. And the sort key is **not total** over a public API: a repeat is now diagnosed with a
warning rather than being a silent determinism hole. Four more were found only by building it: `std::from_chars`'s
floating-point overload does not exist in the pinned macOS SDK's libc++; the plan's phase-1 defensive
drop would have made the vertex cap's own proofs unreachable (closed with `u64` counters instead); the
section cap is unreachable by a **wider** margin than the plan assumed (the joints/weights pairing rule
means only 2⁵ × 2 = **64** masks are producible, not 128); and `bugprone-branch-clone` forced two
switches to merge arms that share a width without sharing a meaning.

**A 42-seed sabotage matrix ran to completion, and FIVE genuine coverage gaps were found and closed**,
each re-proven by re-seeding. The sharpest: **every existing ordering case supplied its primitives so
that ascending mask and ascending source indices AGREED**, so dropping the mask from the comparator
produced byte-identical output for all of them — the ordering rule the whole format rests on had no
case that could see it violated (`MC57`). Also closed: the `±0.0f` fold-order witness no fixture
carried (`MC58`, which also catches a seed that was a non-discriminator until it existed); a
reserve-before-cap-check that is **unobservable at runtime here** because a 274 GB address-space
reserve simply succeeds, measured, so it is asserted in comment-stripped source text instead (`CM50`);
an adapter reading `ImportedPrimitive`'s attribute bitset, invisible because every committed fixture is
internally consistent (`MK17`); and a table-driven case that **passed with zero assertions** until an
anti-vacuity guard was added to it (`MC52`).

**CI caught what a fully green local gate could not, for the fourth time in five tasks.** The first run
failed on **Windows alone, at compile time**: doctest stringifies `std::string_view` operands through
an `operator<<` that MS STL defines inline in `<string_view>` against a `std::basic_ostream` only
`<iosfwd>` has declared. libc++ and libstdc++ supply the complete type transitively, so macOS and Linux
built clean and no local run could have seen it. **This is the 0.4.1 trap's fourth occurrence**, so it
is now a named section in `.claude/rules/ci-portability.md`. The failure incidentally proved the thing
R8 existed to answer: `tools/cooker/src/main.cpp` had **already compiled**, so the
cross-`add_subdirectory` alias resolution the tool depends on works under MSVC's generator.

**A code-review round then found two items, neither a defect**, both closed as documentation and
comments because the code was already right and only the reason was missing. The **model** box folds
submesh boxes in sorted order while an importer folds its own in source order, so the two can disagree
in **the sign of a zero and nowhere else** — and the obvious "fix" is wrong: the model box is written
into the header, so an input-order fold would break order-independence outright. `docs/09` §9.10
carries the caveat, the fold site names AC-29 as the reason it cannot change, and `MK9` records that
its bit-equality holds **because no committed fixture carries a signed zero**. The cooker's
`taken >= MAX_EXTERNAL_URIS` guard cannot fire (every importer bounds `externalUris` before returning)
and is **kept** with a comment naming the four enforcement sites — a guard deleted because it cannot
fire today is a guard nobody restores when that changes.

**R7 is CLOSED with numbers.** A generated `.gltf` + external `.bin` at **7 997 584 vertices and
23 984 268 indices** (essentially both importer caps at once) cooks on `macos-release` in **0.89 s
warm / 1.16 s cold** at a peak resident set of **1.067 GB**, producing a 351 859 984-byte artifact from
a 351 860 625-byte source, byte-identical across three runs. `aero_tests` Debug under ASan/UBSan peaks
at **554 MB in 11.05 s**. The cooked-versus-source ratio is **a property of how compactly the SOURCE
encoded the same floats, not of the cook**: an ASCII `.obj` grid went 31.75 MB → 8.94 MB (**0.28×**)
while a float32 `.glb` whose layout already matched came out at **1.00×**. **One cap the plan did not
account for**, found while measuring: `MAX_EMBEDDED_BYTES` bounds embedded GLB data at **128 MiB**
against 512 MiB for an external buffer, so the format's own ~928 MB worst case is **not reachable
through the CLI at all** — its cook-side arm is genuinely unreachable defence in depth, as its comment
says. Full table in the engineering log.

**The named, unowned gap: v1 stores no node hierarchy**, so a consumer that instantiates a cooked mesh
puts every submesh at the origin. **The cook must not "solve" this by baking node transforms into
vertices** — `ImportedMesh` is shared across nodes by construction, so baking would force per-node mesh
copies. A cooked model/prefab container is the right answer and belongs to whoever owns instantiation;
**3.1.5 is the first task that will hit it.** This is a decision waiting to be taken, not a scope
boundary. `editor/validation/3.3.1-mesh-cook-gpu-buffers.md` ships **unticked**.

**Carried-forward debt, unchanged by this task and worth scheduling as work of its own.** Seven ticked
validation rows across four tasks were signed off with their measurement blanks empty (3.2.5 rows 3, 8,
9, 11, 13; 3.2.2 row 9; 3.2.4 row 12) — each row's *behaviour* passed, each row's *evidence* is absent,
so **R7's in-editor half, R4 and R8's in-editor half stay unmeasured** and D9's centimetre-versus-metre
comparison has no recorded figures. Separately: **no Windows or Linux validation pass exists for any of
the thirteen Phase 2 tasks, for 3.1.1–3.1.4, or for 3.2.1–3.2.5**, and Phase 0's gate is still held
open on Windows/Linux 60 fps sign-off. That is platform-validation debt spanning four phases, and with
macOS fully green it is the whole of the remaining validation risk.

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics (2.1 Editor shell, 2.2 Core panels, 2.3 Manipulation, 2.4 Undo/redo, 2.5 Scene I/O, 2.6 Project system v0) CLOSED in code and macOS-validated PASS, with Windows/Linux rows pending for every task (`editor/VALIDATION.md`). The whole-phase audit (2026-08-02) fixed two silent data-loss paths, a project root that was never made absolute, two CI false-greens, and four stale documentation claims. Full per-task and per-epic history — every defect, every sabotage matrix, every deviation — lives in `docs/10-engineering-log.md`'s Phase 2 entries; this row is deliberately a summary, not a duplicate. |
| **Phase 2 gate** | **MET 2026-08-02.** `samples/phase-2-editor-scene/` holds a project and a 4-entity scene authored entirely through the editor, with the save → New Scene → Open Scene round trip confirmed (`samples/phase-2-editor-scene/VALIDATION.md`); provenance is recorded there rather than asserted, since a hand-written `scene.json` is byte-identical to a real one and no test tier can tell them apart. Deliberately NOT `add_subdirectory`'d — this artifact is data (a provenance proof of the editor), not a compile-proof of engine code. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** **Epic 3.1** is fully merged except **3.1.5 (drag-into-scene), still open**: 3.1.1/3.1.2/3.1.3/3.1.4 (PRs #65/#66/#67/#69), CI-green on all three platforms, sabotage-proven (26/31/35/25 seeds), macOS-validated ✅ PASS 14/14, 14/14, 16/16, 10/10 — Windows/Linux rows pending for all four. `engine::Guid`/`engine::ContentHash`, the `.meta` v1 format, `AssetDatabase::rescan`'s eight phases, the machine-local `Library/asset-cache.json` import cache and the real Asset Browser all shipped across them. **Epic 3.2 (Importers) is CLOSED IN CODE — five merged tasks**, one canonical in-memory `ImportedModel` and eight claimed extensions: 3.2.1 glTF/fastgltf (PR #70 `f02ca65`, ✅ 12/12), 3.2.2 FBX/ufbx (PR #71 `c597a5b`, ✅ 13/13), 3.2.3 OBJ/tinyobjloader (PR #72 `c412e83`, ✅ 13/13), 3.2.4 Blender CLI/`.blend` (PR #73 `5ab07f3`, ✅ 15/15), 3.2.5 Assimp DAE/PLY/STL (PR #74 `7e0224f`, ✅ 14/14). Windows/Linux rows pending for all five, and seven of their ticked rows are missing the measurement they asked for. **Epic 3.3 (Cooker v0) is OPEN and 3.3.1 (Mesh cook → GPU buffers) is COMPLETE ON `feat/3.3.1-mesh-cook-gpu-buffers`, 13 commits, NOT YET MERGED.** It opens `engine/assets` (the `.aeromesh` container v1 + the cook, core-only, no vcpkg package at all), adds one editor adapter pair and `tools/cooker` (`aero_cooker mesh`), and is the first task to produce a runtime-consumable artifact. Mechanical gate green: **117/117 on both macOS presets** with `AERO_REQUIRE_GPU=1`, six guards passing, both reduced configurations rebuilt FRESH with `-G Ninja` and green at **28** and **41** ctest entries with `CM1`/`MC1`/`MK1` present, clang-format and clang-tidy clean by exit code, and `vcpkg.json`/`.github/`/`cmake/`/`runtime/` byte-identical to `main`. A 42-seed sabotage matrix ran to completion with **five genuine gaps found and closed** (`MC57`, `MC58`, `CM50`, `MK17`, `MC52`'s anti-vacuity guard), each re-proven by re-seeding; a code-review round found two more items, neither a defect, both closed as documentation. CI caught a **Windows-only `<ostream>`/`string_view` compile failure** no local run could see — the 0.4.1 trap's fourth occurrence. **R7 is closed with numbers** (7 997 584 vertices, 0.89 s, 1.067 GB peak). **The no-engine-change streak ENDS here at seven, deliberately.** Full detail for every task in `docs/10-engineering-log.md`'s Phase 3 entries. |
| **Next task** | **~~3.2.5 merge and macOS validation~~ DONE**, and every macOS validation row in the tree is ticked. The immediate item is **(a) push, get a green CI run whose `headSha` equals `HEAD`, review and MERGE 3.3.1** (`feat/3.3.1-mesh-cook-gpu-buffers`), then run its macOS validation pass — its rows 6 and 7 are written so a tick without the number is impossible to do quietly. Then: **(b) 3.1.5 (drag-into-scene)**, fully unblocked — it depends on 3.1.3 and 3.2.1 (both merged), `ImportedModel` gives it something to reference, all eight importable extensions work, and 3.3.1 gives it a cooked artifact to place. **3.1.5 owns three decisions now**: sub-asset identity (3.2.1's D13), replacing `LOCAL_MESH_HALF_EXTENT` (2.3.1's knowingly-wrong constant), and **the node-hierarchy gap 3.3.1 named but does not own** — a cooked mesh carries no hierarchy, so a naive instantiation puts every submesh at the origin. **(c) 3.3.2 (texture cook)** is also unblocked and inherits 3.3.1's container shape and CLI grammar (`aero_cooker texture …` needs no reshuffle). And **(d) the seven ticked-but-unmeasured validation rows**, plus the four-phase Windows/Linux platform-validation debt. See `docs/tasks/phase-3.md`. |

Engine layers that exist today, in dependency order: `core` (gained `guid.hpp`/`guid.cpp` at task
3.1.1, beside `handle.hpp`; gained `content_hash.hpp`/`content_hash.cpp` at task 3.1.2, beside `guid`)
→ **`assets`** → `platform` → `rhi` → `render` → `reflect` → `scene` → `scene_render` →
`scene_serialize`, plus `/editor` (`aero_editor_core` + `aero_editor`) and `/tools` (`reflect-gen`,
`shaderc`, **`cooker`**). `/runtime` is still empty — it arrives in Phase 5.
**`engine/assets/` OPENED at task 3.3.1** and its `.gitkeep` is gone. The old reason for keeping it
shut — "unopened until a **runtime** consumer exists (Phase 5's pak table)" — was satisfied by the
task itself: the `.aeromesh` container's *reader* **is** a runtime component by definition, so the
consumer in question is the thing being built. It holds the cooked-asset formats and nothing else
(`cooked_mesh.{hpp,cpp}`, `mesh_cook.{hpp,cpp}`); it links `aero::core` + `aero::profiling` and **no
vcpkg package at all**, and adding a `find_package` there would void that boundary silently while CI
stayed green. The editor's `AssetDatabase` and its import cache (3.1.1/3.1.2) still live entirely in
`/editor`, not here. **`tools/` no longer links zero engine targets**: `aero_cooker` links
`aero::assets` and `aero::editor_core`, which is legal because `tools/` is enumerated by neither half
of the golden rule.
`engine/scene` gained one primitive at task 2.4.2, `[[nodiscard]] Entity World::recreate(Entity)` —
the only engine change Epic 2.4 needed. Tasks 2.5.1, 2.5.2, 2.6.1 and 2.6.2 all needed **no** engine
change at all — a four-task streak task **3.1.1 ended**; 3.1.2 used the identical minimal shape a
second time, and 3.1.3 through 3.2.5 restarted and ran a **seven-task no-engine-change streak**.
**That streak ENDS at seven, at task 3.3.1, deliberately**: `git diff --name-only main...HEAD --
engine/` is not empty for the first time since 3.1.1. The whole engine diff is one new subdirectory
plus one `add_subdirectory(assets)` line — every other subsystem is byte-identical, as are
`vcpkg.json`, `.github/`, `cmake/` and `runtime/`. (3.2.3 touched `cmake/sanitizers.cmake`, forced by
its Windows-only `LNK2038`; that is a build-system path, not an `engine/` one, so the streak held.
3.2.5 touched `vcpkg.json` for one dependency, `assimp`, with the baseline and submodule unmoved.
3.3.1 adds **no dependency of any kind**.)
`/editor` gained **ten** new `.hpp`/`.cpp` pairs across 2.6.2, 3.1.1, 3.1.2, 3.1.3 and 3.1.4
(`project_settings.{hpp,cpp}` / `project_settings_panel.{hpp,cpp}` (2.6.2),
`asset_meta.{hpp,cpp}` / `asset_database.{hpp,cpp}` (3.1.1), `asset_cache.{hpp,cpp}` (3.1.2),
`asset_view.{hpp,cpp}` / `thumbnail_cache.{hpp,cpp}` / `thumbnail_store.{hpp,cpp}` (src-private) /
`asset_actions.{hpp,cpp}` (3.1.3), `asset_watcher.{hpp,cpp}` (3.1.4)); **3.2.1 added four more pairs
plus one deliberate exception**: `model_import.{hpp,cpp}`, `gltf_import.{hpp,cpp}` (src-private, the
only fastgltf TU), `model_import_session.{hpp,cpp}` and `import_details_panel.{hpp,cpp}` (src-private,
the only ImGui TU that task added) are real pairs; `import_settings.hpp` is a HEADER WITH NO `.cpp`,
deliberately alone (`ImportSettings` is shared by `asset_meta.hpp` and `model_import.hpp`, and giving
it its own tiny, dependency-free header is what stops `asset_meta.hpp` from dragging in `aero::scene`
and the whole math umbrella). **3.2.2 adds ONE more pair**, `fbx_import.{hpp,cpp}` (src-private, the
only ufbx TU) — and, separately, `/editor` gains its FIRST `third_party/` directory,
`editor/third_party/ufbx/`, byte-identical to upstream v0.23.0 and never to be patched locally.
**3.2.3 adds ONE more pair**, `obj_import.{hpp,cpp}` (src-private, the only tinyobjloader TU).
**3.2.4 adds THREE more pairs**: `blender_tool.{hpp,cpp}` (PUBLIC and PURE — no disk, no SDL, no
`<filesystem>`, no logging), `blender_service.{hpp,cpp}` (PUBLIC, the polled state machine, naming no
SDL type at all) and `blender_process.{hpp,cpp}` (src-private, the tree's ONLY `SDL_Process` TU,
holding the handle as a `void*` so its own header names no SDL type either); it adds no dependency at
all, because Blender is an external PROCESS, never a library. **3.2.5 adds ONE more pair**,
`assimp_import.{hpp,cpp}` (src-private, the tree's ONLY Assimp TU, the fourth file behind
`model_import.cpp`'s dispatch) plus two pure helpers and one cap constant on the PUBLIC surface
(`scanPlyTextureFiles`, `plyDeclaredCountsExceedBytes`, `scanColladaAssetSpace`, `MAX_NODE_DEPTH`).
**3.3.1 adds ONE more pair**, `mesh_cook_source.{hpp,cpp}` (PUBLIC and PURE — two functions, no UI,
no state, no `Library/` write, no panel), and `aero::assets` on `aero_editor_core`'s **PUBLIC** link
group. The `.hpp`s live under `editor/include/aero/editor/` (except the six named src-private), the
`.cpp`s under `editor/src/`.

Test inventory at the tip of `feat/3.3.1-mesh-cook-gpu-buffers`, every number
**re-measured there, never derived by addition and never carried forward from an earlier step or an
earlier task** — read the totals from doctest's own `filters:` line, never from a `grep -c` of case
names. **`ctest -N` moves in ALL THREE configurations for the first time in this epic**, because
`aero_cooker` takes **no gate flag** and its 22 cases are therefore registered everywhere: **95 → 117**
with tools ON, **6 → 28** with `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, **19 → 41** with
`-DAERO_REFLECT_TOOLS=OFF` alone. A future gate flag on the cooker would silently shrink the reduced
configurations' coverage with no test able to report it. `aero_tests` **415 → 523** (two new TUs,
`cooked_mesh_test.cpp` (`CM*`) and `mesh_cook_test.cpp` (`MC*`), plus `aero::assets` on its link line).
`aero_editor_shell_test` **1481 → 1498** (one new TU, `tests/editor/mesh_cook_source_test.cpp`,
`MK1`–`MK17`). `aero_editor_imgui_test` **104**, `aero_scene_serialize_test` **23** and
`aero_editor_inspector_test` **22**, all unchanged — this task ships **no UI at all**. Both reduced
configurations were built FRESH in `build/tools-off-3.3.1` / `build/reflect-off-3.3.1` and are green.
**A reduced-configuration probe must be configured with `-G Ninja`**: `CMAKE_GENERATOR` enters the
shadercross bootstrap's option hash, so the generator-less form reads the cached toolchain as COLD and
pays a from-source DXC rebuild that peaked at 7.6 GB here before a memory guard killed it.
`aero_editor_core` sources **57 → 58** (`mesh_cook_source.cpp`) and `editor/src/*.cpp` **58 → 59**,
with **no** new `find_package` and **no** new `vcpkg.json` dependency.
`check-math-boundary.sh`'s scanned count **305 → 316** and `check-project-no-delete.sh`'s Check B scan
**58 → 59**, both picked up automatically — **neither script changes, and `.github/scripts/` is
byte-identical to `main`.** Guard count stays **six**; Check A's six-file denylist and Check B's
two-file `PERMITTED_DELETERS` are unchanged in membership, and `mesh_cook_source.cpp` is in **neither**,
which is exactly what makes a future `std::filesystem::remove` there a hard CI failure.
`git grep -nE '_WIN32|__APPLE__|__linux__' -- engine/assets tools/cooker` reads **zero lines**, and the
same grep over `editor/src` + `editor/include` still reads **exactly three lines in one file**
(3.2.4's `currentHostOs()`). Every purity grep over `engine/assets` (`<filesystem>`, `<fstream>`,
`<iostream>`, `AERO_LOG`, `std::cout`/`std::cerr`, `std::map`/`std::set`/`unordered_*`,
`reinterpret_cast`, `memcpy`, `rhi/`) returns **only prose in `//` comments stating the prohibition** —
never a use. Counts diverge by OS (Windows skips `golden-rule.include_scan_e2e`, and **fourteen** whole
`BS` cases plus one arm of `BS11` and one GPU case, `I80`, skip loudly there), so never assume one —
measure with `ctest -N` and `--list-test-cases`. **Those 3.2.4 skips are not a random sample**:
together they are the only coverage anywhere of the two Blender timeouts, cancellation, the `Converted`
state and therefore the whole end-to-end conversion, `ok: false`, exit-0-with-no-status, both
`ArtifactUnusable` arms, a selection change during a live run, and the refused-by-cap log — so on
Windows none of those arms is executed by any test at all. That belongs in the Windows validation
pass's own list, not in a footnote about counts.

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

Path-scoped working rules live in `.claude/rules/` and load only when the matching files are opened — boundary guards, reflect-gen, CI portability, editor conventions, and cooked assets.

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
| `.claude/rules/*.md` | Path-scoped working rules, loaded only when matching files are opened (boundary guards, reflect-gen, CI portability, editor, cooked assets) |
