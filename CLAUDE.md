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

**Phase 3 (Asset Pipeline & 3D Content) is OPEN. Epic 3.3 (Cooker v0) is CLOSED — all three tasks
merged, macOS-validated, no macOS debt — and Epic 3.4 (PBR materials) is OPEN with 3.4.1 MERGED and
macOS-validated.** Epic 3.1 (AssetDatabase · assets) is merged except **3.1.5 (drag-into-scene), still open**.
Epic 3.2 (Importers) is **closed in code** — five merged tasks: 3.2.1 glTF, 3.2.2 FBX, 3.2.3 OBJ,
3.2.4 Blender CLI, 3.2.5 Assimp (DAE/PLY/STL), the last merged as PR #74 (`7e0224f`) on 2026-08-12.
**Epic 3.3's three tasks are PR #75 (`17a6821`, ✅ 12/12), PR #76 (`cf8575a`, ✅ every row) and PR #77
(`234a009`, ✅ 13/13)** — every one CI-green on all three lanes with `headSha == HEAD` asserted before
the merge, and every one macOS-validated with **every measurement blank filled**.
**3.4.1 (Material asset + PBR shader) is MERGED as PR #78 (merge commit `a01765d`, 10 commits) and
macOS-validated ✅ PASS on ALL 11 ROWS (2026-08-16), every measurement blank filled** — CI-green on all
three lanes (macOS 133/133, Linux 133/133, Windows 131/131, each in Debug/ASan and Release, plus the
`cook-determinism` job) with the green run's `headSha` asserted equal to `HEAD` before the merge.
**Rows 2, 4, 5 and 6 of that page are the ONLY witness anywhere for five shader-only sabotage seeds
(S24–S28) and probe X3** — no automated tier in this tree can see them, which is why the page states
each row's seeds beside it.

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

**Epic 3.3 (Cooker v0), condensed — three merged tasks, no macOS debt.** Full per-task detail lives in
`docs/10-engineering-log.md`'s Phase 3 entries; this is the residue that still governs new work.
**3.3.1 (Mesh cook → GPU buffers, PR #75 `17a6821`, 13 commits, 42 seeds, ✅ 12/12)** opened
`engine/assets` and `/tools/cooker`, produced the tree's **first binary format** (`.aeromesh` v1) and
the first runtime-consumable artifact, and made `docs/09` §9 normative for bytes: no struct `memcpy`,
no hash container, no timestamp/path/hostname/build-id, explicit little-endian assembly through
primitives whose endianness is a **`static_assert`**, a zero-initialized output buffer, and a sort that
runs **before** the cap pass. Its R7 numbers reframed their own premise — peak memory is dominated by
the **importer**, not the cook (~3.1× source for a binary `.gltf`, ~9.0× for an ASCII `.obj`).
**Its named, unowned gap is still open**: v1 stores no node hierarchy, so a consumer that instantiates
a cooked mesh puts every submesh at the origin, and **the cook must not "solve" this by baking node
transforms into vertices** — `ImportedMesh` is shared across nodes by construction. **3.1.5 is the
first task that will hit it.**
**3.3.2 (Texture cook → KTX2/Basis, PR #76 `cf8575a`, 15 commits, 53 seeds, ✅ every row)** made this
project produce **the first artifact a third-party tool can open** — a strict subset of Khronos KTX2,
byte for byte, so `ktx info`, `ktx validate` and RenderDoc read our files. Three properties still
govern anything that touches those files: conformance is **verified**, not asserted; **the texture
files carry NO floating point** (no `float`, no `double`, no `<cmath>`, no runtime table), so
cross-lane byte-identity is *structural* rather than lucky — which is exactly why `stb_dxt.h` is
installed, provides these four formats, and **is not used**; and **sRGB is the format, never a flag**,
so "an sRGB normal map" is unspellable rather than merely rejected. Its sharpest lesson is a rule for
every future first-party format: two DFD bytes derived correctly from the spec were **wrong**
(dfdutils compares channel ids numerically, and `KHR_DF_CHANNEL_BC3_ALPHA` is also 15), and building
to the spec as written would have shipped artifacts `ktx validate` rejects **with every test in this
repository green**, because our parser compares against the same table our writer emits. That was
demonstrated, not argued: re-seeding the fix makes `ktx validate` exit 3 with the exact predicted
`error-6028`. R1/R2/R3 are all closed with numbers on the validated build (`ktx validate` PASS on all
eight artifacts, BC1 PSNR within 0.85 dB of `stb_dxt` and ahead on one image, a 4096² BC3 cook at
0.68–0.71 s / 181.3 MB peak). **`ktx` 4.4.2 needs no `sudo` and has no Homebrew formula**:
`pkgutil --expand-full` unpacks the Khronos `.pkg` without installing, and `libktx.4.dylib` must be
**symlinked beside the binary** because it resolves through `@rpath` — `DYLD_LIBRARY_PATH` alone does
not work.
**3.3.3 (Cook determinism golden test, PR #77 `234a009`, 8 commits, 24 seeds, ✅ 13/13)** ships **zero
C++** and turns determinism from a per-lane property into a **cross-lane, cross-config, cross-time**
contract: a frozen 13-artifact manifest (`tests/cooker/determinism.sha256`), two ungated ctest cases,
and a `cook-determinism` CI job that runs `ktx validate` on every push and prints
`26 byte comparisons agreed: macOS == Windows == Linux, byte for byte.` in 8 s. **A note for whoever
adds a fixture or changes a cook**: that manifest is FROZEN, a red manifest case is `docs/09` §9.11's
`cookerVersion` sentence firing, and the regeneration ritual lives in the manifest's own header —
never edit a hash to green a red run. **Its two halves are not anchored equally**:
`mesh-triangle.aeromesh` equals the SHA-256 of a 3.3.1 golden derived with no cooker involved, while
no texture line has an equivalent tie — what keeps those eight honest is `ktx validate`, the one check
our own code cannot self-confirm.

**INV-T4, stated accurately, because it is a live rule and not a historical note.** The invariant is
that **the texture files carry no floating point** — not that `engine/assets` contains none, which is
and always was false: `mesh_cook.cpp` includes `<cmath>` and `cooked_mesh.hpp` has `putF32`/`getF32`,
because mesh vertex data *is* float. `<numeric>`'s `std::lcm` is integer-only and is the one include
the rule does not reach.

**A standing CI-portability trap, named in `.claude/rules/ci-portability.md`.** doctest's
`DOCTEST_STRINGIFY` expands to an **unqualified** `toString(...)`, so an engine `toString(SomeEnum)` is
found by ADL, beats doctest's own template, and the decomposer then tries
`std::string_view + const char*` — a **hard compile error on every lane**, reported inside `doctest.h`
rather than at the `CHECK`. Every future `toString(SomeEnum)` on a public engine header inherits it;
a label function named something else is unaffected, which is why `cookedTextureStatusLabel` and
3.4.1's four `material*Label` functions are named the way they are.

**3.4.1 — Material asset + PBR shader, the current branch, and the task that makes a cooked texture
mean something.** Epic 3.4 opens here with **three firsts**: the **first cooked texture this project
has ever drawn on a GPU**, the first `.aeromat` parsed end to end, and the first asset resolved at run
time **by GUID** — the Phase 5 pak-resolution shape in miniature, three phases early and carried by a
sample rather than by a runtime. Four layers move, bottom-up. **`engine/rhi`** finishes the
block-format contract change 3.3.2 assigned here: six BC enumerators (`Count` 19 → **25**),
`texelBlockSize` re-documented in Vulkan's block vocabulary (**bit-for-bit unchanged for every
pre-3.4.1 value**), new `texelBlockWidth`/`texelBlockHeight`/`textureLevelByteSize` — the last being
**the** upload-size formula, `docs/09` §10's arithmetic spelled once — a uniform engine-owned refusal
of non-block-aligned BC **top** levels (D3D12's rule adopted on all three backends per ADR-002), six
SDL format mappings and a block-rounded explicit transfer pitch. **All of it is recorded per the 0.4.1
D18 public-header amendment protocol**, nine amendments each with its verification source, in the
docs/10 entry. **`engine/reflect`** gains `material_format.{hpp,cpp}` — `.aeromat` v1, normative as
`docs/09` **§11** (Reserved renumbers §11 → **§12**), parser + canonical writer beside `scene_format`,
with **no link-line change at all**. **`engine/render`** gains `material.hpp`,
`texture_upload.{hpp,cpp}` (the cooked-texture bridge), the src-private `material_pack.hpp`, 48-byte
vertices with analytic UVs/tangents across the primitive catalog, and a `MaterialHandle` registry
inside `ForwardRenderer` (SlotMap-generational, three 1×1 default textures, sampler dedup, two
pipelines differing only in `cullMode`); its link line gains **`PUBLIC aero::assets`**, the one new
edge. **`shaders/`** carries the GGX pair rewritten **in place** — same filenames, so
`shaders/CMakeLists.txt` is byte-identical and the build sees zero diff. **`samples/phase-3-materials`**
is 16 new files and one modified: a 6×6 roughness×metallic grid, a mapped cube and an alpha-mask cube,
six committed PNGs and the six `.ktx2` cooked from them with pinned GUIDs.

**Two properties are worth carrying forward.** **The `ctest -N` triple does not move** — 133/44/57
before and after — because `aero_tests` registers as **one ctest entry**, so 57 new doctest cases are
invisible there; the growth shows up only in doctest's own totals (`aero_tests` **656 → 713**, the
other four binaries unmoved). Read an unmoved `ctest -N` as "no tests were added" and you would be
right about 3.3.3 and wrong about this task. And **the two fragment UBOs were verified before any
picture was judged**: the cooked `scene.frag.json` reports `samplerCount 5, uniformBufferCount 2` and
`scene.vert.json` reports `0, 1`, while `PB11` pushes a 320-byte block at slot 0 and a 48-byte block at
slot 1 and draws — so a crossed slot is a **size mismatch**, not a plausible picture.

**The 30-seed matrix ran to completion with no gap inside it**, but the interesting results are the
four that missed their prediction: **S20** (a draw ignoring the instance's material) reddens `PB12`
rather than nothing, and **S22** (transposed cube UVs) reddens `PB3`'s corner-convention case rather
than being purely visual — so the on-screen transposition class is instead witnessed by a
*vertex-shader* UV swap that stays green and belongs to validation row 4; **S5** does **not** redden
`PB7` (the bridge checks alignment first, so the backend arm's removal is invisible there —
`rhi device T1-7` catches it) and **S8** does **not** redden `PB6` (Golden A's three levels are each
exactly one 8-byte block, so a reversed order is size-identical — `PB5`'s 8×8 chain catches it).
**Five shader-only seeds stay green by design** — S24, S25, S26, S27, S28 — and each names the
validation row that is its only witness anywhere.

**Two coverage gaps were found OUTSIDE the matrix and closed structurally**, each re-proven by
re-seeding. **`packLights`' `eyePosition` had no witness at all** — zeroing it left all 712 cases
green, and it feeds every specular highlight, which is exactly R5's "plausible garbage" class. Closed
by hoisting the light mirror and its packer into `engine/render/src/material_pack.hpp` beside
`packMaterial`, whose own comment already argued that **a file-local packer is unfalsifiable** — the
reasoning simply had not been applied to the light block — plus `PB13`, which pins all 320 bytes field
by field. **The per-slot default-texture table had no witness** — binding the flat normal to slot 0
left the suite green, which renders every untextured primitive with `80 80 FF` as its base colour.
Closed by **deleting the defect site**: `MaterialDefaultTextureKind` + `defaultTextureKindForSlot` in
`material.hpp` is now the single place the slot→default mapping is decided, `defaultTextureTexel` is
its composition rather than a second switch, and the hand-written five-entry table is gone.

**A code-review round over the branch found no correctness defects.** Its sharpest check is a fact
about SDL rather than about our code and is worth keeping: the draw loop binds fragment textures
*before* a possible pipeline rebind, and all three backends' `BindGraphicsPipeline` only acquire
uniform buffers and mark descriptor sets for rebuild — **none clears the stored fragment
texture/sampler bindings** — so the ordering is safe on Vulkan, Metal and D3D12.

**Eight deviations were forced by reality — all eight are in the docs/10 entry, and five of them were
outright errors in the plan or the spec.** A 5×3 BC5 level is
**32** bytes, not 16. **`1e999` does not become `null` at the JSON layer** — `parseJson` stores number
lexemes verbatim, so it arrives as a valid `Number`, passes the kind check, and only `asF32()` refuses
it; without a third "not representable as a 32-bit float" arm the parser would have silently kept the
default. Uppercase GUIDs are **accepted**, because `guid.hpp` documents `parseGuid` as case-tolerant
and `docs/09` states the tolerant-read/lowercase-write rule three times for other formats.
`NON_NEGATIVE_RANGE`'s upper bound is a **finite** float max, because an infinite factor would pass
validation, write as `null`, and fail re-parse — breaking the round-trip guarantee the validator
exists to protect. "Upload one committed golden per block family" is **unexecutable** (only the BC1
4×4 golden is block-aligned), so per-family uploads cook in memory and two goldens became refusal
fixtures. The VFS mount prefix must be **`res://materials`**, not `materials` — the short form mounts
nothing, and only a run could catch it.

**The 0.4.1 D18 amendment protocol is the standing rule for `engine/rhi`, not a one-off.** Any change
to a public rhi header is recorded in `docs/10` with the source that verified it — here, the pinned
SDL 3.4.12 tree answered both open questions: the transfer pitch fields are in **texels** (Vulkan
forwards them to `bufferRowLength`, Metal **ignores** them and derives a block-aware pitch from the
region, D3D12 block-rounds), and **the six BC formats are NOT on SDL's universally-supported SAMPLER
list** (seventeen uncompressed formats, no BC format), so `format.hpp`'s universality promise gained a
scoped exception naming `Device::supportsTextureFormat` and task 6.3.1 rather than an SDL-universal
claim it cannot support.

**Named handoffs, each with an owner.** Scene-side material references → **3.1.5 / 3.4.2** (nothing in
a scene file can name a material today). The material inspector, an `.aeromat` browser kind and
reflection of material params → **3.4.2**, whose starting point is that the reflect-gen subset has no
`Vec4` and no enums, so "just reflect `MaterialDocument`" is not available. Import-materialization →
first needed by **3.1.5**. **BLEND transparency** → a named **decision-waiting** gap, renderer-only,
since the format already carries everything. **IBL / environment lighting** → a named **unowned** gap
blocked on a format decision, not on shader work: it needs a cubemap, which `docs/09` §10 currently
refuses. Shadows → **3.6.2**; tonemap/gamma → **3.6.3** (output is raw linear until then). A shared
token→`SamplerDesc` helper → decided by the **second** consumer; the sample is the first.

**Carried-forward debt, and 3.4.1 adds to one half of it.** Seven ticked validation rows across four
tasks were signed off with their measurement blanks empty (3.2.5 rows 3, 8, 9, 11, 13; 3.2.2 row 9;
3.2.4 row 12) — each row's *behaviour* passed, each row's *evidence* is absent, so **R4 and R8's
in-editor half stay unmeasured** and D9's centimetre-versus-metre comparison has no recorded figures.
**None of Epic 3.3's three tasks is among those seven** — all three had their number-bearing rows
written so a blank tick is impossible, which is the pattern the other four should be brought up to
rather than the exception, and 3.4.1's eleven-row page is written the same way. Separately: **no
Windows or Linux validation pass exists for any of the thirteen Phase 2 tasks, for 3.1.1–3.1.4, for
3.2.1–3.2.5, or for 3.3.1–3.3.3**, and Phase 0's gate is still held open on Windows/Linux 60 fps
sign-off. **3.4.1 grows that debt by one task**, and its Windows lane carries something the others do
not: it is the **live retirement** of the D3D12 block-alignment rule this task adopted engine-wide
from documentation. That is platform-validation debt spanning four phases, and with macOS otherwise
green it is the whole of the remaining validation risk. **3.4.1's macOS pass is COMPLETE — all 11 rows, every measurement filled** —
eleven rows, of which four are the only witness five declared shader-only sabotage seeds have
anywhere, so it is a coverage tier rather than a formality.

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics (2.1 Editor shell, 2.2 Core panels, 2.3 Manipulation, 2.4 Undo/redo, 2.5 Scene I/O, 2.6 Project system v0) CLOSED in code and macOS-validated PASS, with Windows/Linux rows pending for every task (`editor/VALIDATION.md`). The whole-phase audit (2026-08-02) fixed two silent data-loss paths, a project root that was never made absolute, two CI false-greens, and four stale documentation claims. Full per-task and per-epic history — every defect, every sabotage matrix, every deviation — lives in `docs/10-engineering-log.md`'s Phase 2 entries; this row is deliberately a summary, not a duplicate. |
| **Phase 2 gate** | **MET 2026-08-02.** `samples/phase-2-editor-scene/` holds a project and a 4-entity scene authored entirely through the editor, with the save → New Scene → Open Scene round trip confirmed (`samples/phase-2-editor-scene/VALIDATION.md`); provenance is recorded there rather than asserted, since a hand-written `scene.json` is byte-identical to a real one and no test tier can tell them apart. Deliberately NOT `add_subdirectory`'d — this artifact is data (a provenance proof of the editor), not a compile-proof of engine code. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** **Epic 3.1** is fully merged except **3.1.5 (drag-into-scene), still open**: 3.1.1/3.1.2/3.1.3/3.1.4 (PRs #65/#66/#67/#69), CI-green on all three platforms, sabotage-proven (26/31/35/25 seeds), macOS-validated ✅ PASS 14/14, 14/14, 16/16, 10/10 — Windows/Linux rows pending for all four. `engine::Guid`/`engine::ContentHash`, the `.meta` v1 format, `AssetDatabase::rescan`'s eight phases, the machine-local `Library/asset-cache.json` import cache and the real Asset Browser all shipped across them. **Epic 3.2 (Importers) is CLOSED IN CODE — five merged tasks**, one canonical in-memory `ImportedModel` and eight claimed extensions: 3.2.1 glTF/fastgltf (PR #70 `f02ca65`, ✅ 12/12), 3.2.2 FBX/ufbx (PR #71 `c597a5b`, ✅ 13/13), 3.2.3 OBJ/tinyobjloader (PR #72 `c412e83`, ✅ 13/13), 3.2.4 Blender CLI/`.blend` (PR #73 `5ab07f3`, ✅ 15/15), 3.2.5 Assimp DAE/PLY/STL (PR #74 `7e0224f`, ✅ 14/14). Windows/Linux rows pending for all five, and seven of their ticked rows are missing the measurement they asked for. **Epic 3.3 (Cooker v0) is CLOSED — three merged tasks, every one CI-green on all three lanes with `headSha == HEAD` asserted, and every one macOS-validated with every measurement blank filled**: 3.3.1 Mesh cook → GPU buffers (PR #75 `17a6821`, 13 commits, 42 seeds, ✅ 12/12) opened `engine/assets` and `tools/cooker` and produced the tree's first binary format and first runtime-consumable artifact; 3.3.2 Texture cook → KTX2/Basis (PR #76 `cf8575a`, 15 commits, 53 seeds, ✅ every row) added the KTX2 subset container, the texture cook and the two integer block encoders, and is the first artifact this project produces that a third-party tool can open — `ktx validate` 4.4.2 PASS on all eight artifacts, proven non-vacuous by re-seeding the corrected DFD byte and watching the validator reject it with the exact predicted `error-6028`; 3.3.3 Cook determinism golden test (PR #77 `234a009`, 8 commits, 24 seeds, ✅ 13/13) ships zero C++ and turns cross-lane, cross-config and cross-time byte-identity into a continuous CI check. Windows/Linux rows pending for all three. **Epic 3.4 (PBR materials) is OPEN, and 3.4.1 (Material asset + PBR shader) is MERGED as PR #78 (merge commit `a01765d`, 10 commits), CI-green on all three lanes with `headSha == HEAD` asserted before the merge, and macOS-validated ✅ PASS on ALL 11 ROWS (2026-08-16) with every measurement blank filled — 6 cooked textures upload in 6.2 ms (mean 1.0 ms), the sample holds ~121 fps, and the sidecars read 5/2 and 0/1.** It is the task that makes a cooked texture mean something: the **first cooked texture this project has ever drawn on a GPU**, the first `.aeromat` parsed end to end, and the first asset resolved at run time **by GUID**. Four layers move — `engine/rhi` finishes the block-format contract change 3.3.2 assigned here (six BC enumerators, `Count` 19 → 25, `texelBlockSize` re-documented in block units and bit-for-bit unchanged for every existing value, `textureLevelByteSize` as THE upload formula, and a uniform refusal of non-block-aligned BC top levels, all recorded per the 0.4.1 D18 amendment protocol); `engine/reflect` gains `material_format.{hpp,cpp}` and `docs/09` a normative §11 (Reserved renumbers to §12); `engine/render` gains `material.hpp`, `texture_upload.{hpp,cpp}`, the src-private `material_pack.hpp`, 48-byte vertices and the `MaterialHandle` registry, plus `PUBLIC aero::assets` on its link line; and `shaders/scene.{vert,frag}.hlsl` are rewritten IN PLACE, so the build sees zero diff. `samples/phase-3-materials` is 16 new files and one modified. Mechanical gate green: **133/133 on both macOS presets** with `AERO_REQUIRE_GPU=1`; `ctest -N` **133 / 44 / 57 — unchanged in all three configurations, because `aero_tests` is a SINGLE ctest entry**, so this task's growth appears only in the doctest totals (**713 / 1516 / 104 / 23 / 22**; `aero_tests` 656 → 713, the other four unmoved); both reduced configurations rebuilt FRESH with `-G Ninja` and green at **44** and **57** with `MT1`/`PB1`/`PB10`/`PB13`/`CT1`/`TX1`/`BB1` and all fifteen `cooker.texture_*` entries present; six guards exit 0; clang-format and clang-tidy clean by exit code; 3.3.3's two `golden_manifest` cases green with the manifest untouched; and `/editor`, `/tools`, `engine/assets`, `engine/scene`, `engine/scene_serialize`, `vcpkg.json`, `.github/`, `cmake/`, `runtime/`, `tests/cooker/determinism.sha256` and `shaders/CMakeLists.txt` byte-identical to `main` — **no new dependency of any kind**. A **30-seed** matrix ran to completion with **no gap inside it** (four seeds missed their prediction and are recorded); **two coverage gaps were found OUTSIDE it and closed structurally** — `packLights`' `eyePosition`, which had no witness at all, and the per-slot default-texture table, whose defect site was deleted rather than merely tested. A code-review round found **no correctness defects**. Full detail for every task in `docs/10-engineering-log.md`'s Phase 3 entries. |
| **Next task** | **~~3.4.1~~ DONE, MERGED and macOS-VALIDATED** as PR #78 (`a01765d`), CI-green on all three lanes with `headSha == HEAD` asserted, and ✅ PASS on all 11 macOS rows (2026-08-16). Its Windows half remains the **live retirement** of the D3D12 block-alignment rule this task adopted engine-wide from documentation — CI has run the T1 upload cases under WARP, but no on-hardware Windows pass exists. Next: **(a) 3.4.2 (material inspector editing)**, unblocked by this task and owning the reflection decision it names — the reflect-gen subset has no `Vec4` and no enums, so reflecting `MaterialDocument` is real work rather than a checkbox, and the `.aeromat` browser kind and live-preview seam (`updateMaterial`, built now) come with it. **(b) 3.1.5 (drag-into-scene)**, fully unblocked — it depends on 3.1.3 and 3.2.1 (both merged), all eight importable extensions work, and both cooks now give it artifacts to place. **3.1.5 owns three decisions**: sub-asset identity (3.2.1's D13), replacing `LOCAL_MESH_HALF_EXTENT` (2.3.1's knowingly-wrong constant), and **the node-hierarchy gap 3.3.1 named but does not own**. And **(c) the seven ticked-but-unmeasured validation rows**, plus the four-phase Windows/Linux platform-validation debt, which with macOS otherwise green is the whole of the remaining validation risk. **A note for whoever adds a fixture or a cook change next**: `tests/cooker/determinism.sha256` is FROZEN, a red manifest case is `docs/09` §9.11's `cookerVersion` sentence firing, and the regeneration ritual lives in the manifest's own header — never edit a hash to green a red run. See `docs/tasks/phase-3.md`. |

Engine layers that exist today, in dependency order: `core` (gained `guid.hpp`/`guid.cpp` at task
3.1.1, beside `handle.hpp`; gained `content_hash.hpp`/`content_hash.cpp` at task 3.1.2, beside `guid`)
→ **`assets`** → `platform` → `rhi` → `render` → `reflect` → `scene` → `scene_render` →
`scene_serialize`, plus `/editor` (`aero_editor_core` + `aero_editor`) and `/tools` (`reflect-gen`,
`shaderc`, **`cooker`**). `/runtime` is still empty — it arrives in Phase 5.
**`engine/assets/` OPENED at task 3.3.1** and its `.gitkeep` is gone. The old reason for keeping it
shut — "unopened until a **runtime** consumer exists (Phase 5's pak table)" — was satisfied by the
task itself: the `.aeromesh` container's *reader* **is** a runtime component by definition, so the
consumer in question is the thing being built. It holds the cooked-asset formats and nothing else — **five pairs since task
3.3.2**: `cooked_mesh.{hpp,cpp}`, `mesh_cook.{hpp,cpp}`, `cooked_texture.{hpp,cpp}`,
`texture_cook.{hpp,cpp}` and `bc_block.{hpp,cpp}`; it links `aero::core` + `aero::profiling` and **no
vcpkg package at all**, and adding a `find_package` there would void that boundary silently while CI
stayed green — which is also why the stb_image decode lives in `/editor` rather than here. The editor's `AssetDatabase` and its import cache (3.1.1/3.1.2) still live entirely in
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
Neither 3.3.1 nor 3.3.2 adds **any dependency of any kind** — 3.3.2's engine diff is three new pairs
inside the subdirectory 3.3.1 opened, and `engine/CMakeLists.txt` is untouched by it. 3.3.3's engine
diff is EMPTY, so the streak resumed for exactly one task before 3.4.1 ended it again.)
**Task 3.4.1 is the first task since 1.4.2 to touch `engine/rhi`, `engine/reflect` and `engine/render`
in one branch**, and the only public-surface change of the three is the rhi one, recorded per the
0.4.1 D18 amendment protocol. `engine/render` gains three public headers plus one src-private one
(`material.hpp`, `texture_upload.{hpp,cpp}`, `src/material_pack.hpp`) and **`PUBLIC aero::assets`** on
its link line — the one new link edge in the tree, downward and cycle-free, since `assets` links only
`aero::core` (plus private profiling). `engine/reflect` gains `material_format.{hpp,cpp}` with **no
link-line change at all** (`aero_reflect` already linked `PUBLIC aero::core`). `engine/scene` and
`engine/scene_serialize` are byte-identical: a material is not yet nameable from a scene file, and
3.1.5/3.4.2 own that. Still **no dependency of any kind** — `vcpkg.json`, `.github/`, `cmake/`,
`runtime/` and `shaders/CMakeLists.txt` are all byte-identical to `main`.
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
group. **3.3.2 adds the ELEVENTH pair**, `texture_cook_source.{hpp,cpp}` (PUBLIC and PURE — a decode,
an auto-format rule and two name predicates; no UI, no `.meta` change, no `Library/` write) — the
tree's **second stb_image implementation TU**, which keeps `STB_IMAGE_STATIC` and deliberately does
**not** define `STBI_NO_FAILURE_STRINGS`, unlike `thumbnail_store.cpp`, because it is the CLI's only
source of a readable decode reason. The `.hpp`s live under `editor/include/aero/editor/` (except the
six named src-private), the `.cpp`s under `editor/src/`. **Tasks 3.3.3 and 3.4.1 add NO editor pair at
all** — the count stays at eleven, and `/editor` was byte-identical to `main` across the 3.4.1 branch.

Test inventory at `main` after PR #78 (`a01765d`), every number
**re-measured there, never derived by addition and never carried forward from an earlier step or an
earlier task** — read the totals from doctest's own `filters:` line, never from a `grep -c` of case
names. **`ctest -N` reads 133 / 44 / 57 and DID NOT MOVE at task 3.4.1** — tools ON, then
`-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, then `-DAERO_REFLECT_TOOLS=OFF` alone. **The
reason matters more than the number**: `aero_tests` registers with ctest as a **single entry**
(`tests/CMakeLists.txt`), so a task that adds two test TUs and 57 doctest cases moves nothing there at
all, and the sample registers no test. An unmoved `ctest -N` means "zero C++" for task 3.3.3 and means
nothing of the kind here — check a zero-C++ claim against the **doctest** totals instead. The triple
last moved for the cooker's own cases (**117 → 131 → 133**, **28 → 42 → 44**, **41 → 55 → 57**),
because `aero_cooker` takes **no gate flag** and its 38 cases plus 3.3.3's two manifest cases are
registered everywhere; a future gate flag on the cooker would silently shrink the reduced
configurations' coverage with no test able to report it.

Doctest, all five binaries: **713 / 1516 / 104 / 23 / 22**. `aero_tests` **656 → 713** — two new TUs,
`tests/material_format_test.cpp` (`MT*`) and `tests/render_material_test.cpp` (`PB*`), plus grown
`tests/rhi_format_test.cpp` and `tests/rhi_device_test.cpp`; both prefixes were verified unclaimed
before use, and **no link-line change was needed**, since `aero_tests` already links `aero::render`,
`aero::reflect`, `aero::rhi` and `aero::assets`. `aero_editor_shell_test` **1516**,
`aero_editor_imgui_test` **104**, `aero_scene_serialize_test` **23** and `aero_editor_inspector_test`
**22** — all four **unmoved**, which is what "this task ships no UI" means as a measurement rather
than a claim. Both reduced configurations were built FRESH in `build/tools-off-3.4.1` /
`build/reflect-off-3.4.1` and are green, with `MT1`, `PB1`, `PB10`, `PB13`, `CT1`, `TX1`, `BB1` and
all fifteen `cooker.texture_*` entries present in both.
**A reduced-configuration probe must be configured with `-G Ninja`**: `CMAKE_GENERATOR` enters the
shadercross bootstrap's option hash, so the generator-less form reads the cached toolchain as COLD and
pays a from-source DXC rebuild that peaked at 7.6 GB here before a memory guard killed it.
`aero_editor_core` sources **59** and `editor/src/*.cpp` **60**, both unchanged — `/editor` is
byte-identical to `main`.
`check-math-boundary.sh`'s scanned count **329 → 338** (six new engine C-family files, two test TUs
and the sample's `main.cpp`; the predicted 337 was one short because `engine/render/src/material_pack.hpp`
was not anticipated) and `check-project-no-delete.sh`'s Check B scan **60**, unchanged — **neither
script changes, and `.github/scripts/` is byte-identical to `main`.** Guard count stays **six**;
Check A's six-file denylist and Check B's two-file `PERMITTED_DELETERS` are unchanged in membership.
**Committed images: two at `tests/fixtures/assets/` (3.3.2) plus six 32×32 PNGs and six `.ktx2` under
`samples/phase-3-materials/textures/` (3.4.1)** — the `.ktx2` cooked once with pinned GUIDs and
regenerable byte-identically from the PNGs by the README's recorded commands, which is 3.3.3's
guarantee being spent rather than re-proven.
`git grep -nE '_WIN32|__APPLE__|__linux__' -- engine/assets tools/cooker` reads **zero lines**, and the
same grep over `editor/src` + `editor/include` still reads **exactly three lines in one file**
(3.2.4's `currentHostOs()`). Every purity grep over `engine/assets` (`<filesystem>`, `<fstream>`,
`<iostream>`, `AERO_LOG`, `std::cout`/`std::cerr`, `std::map`/`std::set`/`unordered_*`,
`reinterpret_cast`, `memcpy`, `rhi/`) returns **only prose in `//` comments stating the prohibition** —
never a use. **Two gate greps are NOT literally zero and must be READ rather than counted**:
`git grep -n 'find_package' -- engine/assets/` returns two comment lines stating the prohibition, and
the `tools/` process-spawn grep matches "libsdl-org fork" twice in `tools/shaderc/README.md`.
Counts diverge by OS (Windows skips `golden-rule.include_scan_e2e`, and **fourteen** whole
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
