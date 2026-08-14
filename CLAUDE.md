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

**Phase 3 (Asset Pipeline & 3D Content) is OPEN, and Epic 3.3 (Cooker v0) now has two tasks.** Epic 3.1
(AssetDatabase · assets) is fully merged (3.1.1–3.1.4). Epic 3.2 (Importers) has **five** merged tasks
— 3.2.1 glTF, 3.2.2 FBX, 3.2.3 OBJ, 3.2.4 Blender CLI, 3.2.5 Assimp (DAE/PLY/STL), the last merged as
PR #74 (`7e0224f`) on 2026-08-12. **3.2 is closed in code; 3.1.5 (drag-into-scene) is the one Epic 3.1
task still open.** **3.3.1 (Mesh cook → GPU buffers) is MERGED as PR #75 (`17a6821`) and
macOS-validated ✅ PASS 12/12 (2026-08-13) with every measurement blank filled.**
**3.3.2 (Texture cook → KTX2/Basis) is MERGED as PR #76 (merge commit `cf8575a`, 15 commits) and
macOS-validated ✅ 15/17 rows on 2026-08-14** — CI-green on all three lanes with the green run's
`headSha` asserted equal to `HEAD` before the merge, every measurement row filled with real numbers,
and **the two external-verification rows (3 and 4) deliberately left OPEN rather than ticked** because
`ktx` and RenderDoc are not installed here. The code-review round that preceded the merge found
**two real defects**. The round found **two real defects**: a division by zero in `cookTexture` for a format
outside the eight (a UBSan report and a `SIGABRT`, unreachable from today's callers and reachable the
moment a format is read from a `.meta` or a `.pak`), and a parser that **accepted** the partial mip
chain `docs/09` §10.8 says it refuses — closed by adding the check, never by rewording the document.
Plus two coverage holes that were green by construction: **BC4 was the one format with neither a byte
golden nor a golden-pinned sibling**, so 27 of its 44 DFD bytes were asserted nowhere and declaring
BC5's colour model in it left the suite green at 131/131; and AC-32's **repeated-flag arm was never
added**, so `refuseRepeat` on all four texture-only flags had zero coverage.

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

**3.3.1 — Mesh cook → GPU buffers, compressed to its load-bearing residue.** Epic 3.3's first task and
the first thing in this project that produces a **runtime-consumable artifact**. It opened
`engine/assets` as `aero::assets` (the `.aeromesh` container v1 + the cook, both PURE, linking
`aero::core` + `aero::profiling` and **no vcpkg package at all** — which is what makes their `PRIVATE`
links a genuine compile-time boundary rather than convention-plus-grep, R12), added one editor adapter
pair, and opened `/tools/cooker` as the third first-party CLI, legal because `tools/` sits outside the
golden rule on **both** halves. **It is the tree's first BINARY format**, so `docs/09` gained a
normative §9 restating every determinism guarantee for bytes: no struct `memcpy`, no hash container, no
timestamp/path/hostname/build-id, explicit little-endian assembly through eight primitives whose
endianness is a **`static_assert`**, a zero-initialized output buffer, and a sort that runs **before**
the cap pass so a shuffled input cannot produce a different file. Nine spec statements were wrong, three
load-bearing; a 42-seed matrix found five genuine gaps (`MC57`, `MC58`, `CM50`, `MK17`, `MC52`); CI
caught a Windows-only `<ostream>`/`string_view` failure no local run could see. **R7 closed with
numbers, and the numbers reframed its premise**: peak memory is dominated by the IMPORTER, not the cook
(~3.1× source for a binary `.gltf`, ~9.0× for an ASCII `.obj`). **Its named, unowned gap is still
open**: v1 stores no node hierarchy, so a consumer that instantiates a cooked mesh puts every submesh at
the origin, and **the cook must not "solve" this by baking node transforms into vertices** —
`ImportedMesh` is shared across nodes by construction. **3.1.5 is the first task that will hit it.**

**3.3.2 — Texture cook → KTX2/Basis, merged as PR #76 (`cf8575a`).** Epic 3.3's second task and **the first thing
this project produces that a third-party tool can open**: a strict subset of Khronos **KTX2**, byte for
byte, so `ktx info`, `ktx validate` and RenderDoc read our artifacts. That one property changes the
register of the whole task — every field is a **fact to be verified against `ktxspec.adoc`**, not a
decision taken here. `engine/assets` grows from two pairs to **five**: `cooked_texture.{hpp,cpp}` (the
container — the frozen `CookedTextureFormat` enum whose values *are* Khronos's `VkFormat` numbers, the
eight frozen DFD tables, and a ten-status hostile-input parser), `texture_cook.{hpp,cpp}` (the cook —
two committed gamma tables, an integer polyphase mip filter, the block loop and the assembly) and
`bc_block.{hpp,cpp}` (the BC1 and BC4 encoders, from which BC3 and BC5 are composed with no third
encoder). **`/editor` gains one adapter pair**, `texture_cook_source.{hpp,cpp}` — the tree's **second**
stb_image implementation TU, the `auto`-format rule, and no UI at all. **`aero_cooker` gains its second
subcommand**, `texture`, with a **mandatory** `--srgb`/`--linear` and no default. `docs/09` gained a
full normative **§10** (with the old "Reserved for future formats" renumbered to **§11**, added rather
than replaced), which also answers §9.0's forward reference: **there is still no platform field and no
`--platform` flag**, because v1 emits exactly one profile; ASTC/ETC2 and the flag arrive together at
6.3.1.

**Three properties are the whole task.** The output is a real KTX2 file, so conformance is verified
rather than asserted. **The texture files carry NO FLOATING POINT** — no `float`, no `double`, no
`<cmath>`, no runtime table generation — so cross-lane byte-identity is *structural*: a
float here is a byte-identity hazard on three lanes (FMA contraction differs between clang and MSVC,
libm between three C libraries), which is exactly why `stb_dxt.h` is installed, provides these four
formats, and **is not used** — its BC1 path finds the principal axis by float power iteration. And
**sRGB is not a flag but the format itself**: there is no `bool srgb` anywhere, Vulkan defines no
`BC4_SRGB` or `BC5_SRGB`, so "an sRGB normal map" is **unspellable** rather than merely rejected.

**Eleven spec statements were wrong, and TWO were blocking in a way no test in this tree could catch.**
`138 BC3_SRGB`'s byte 31 and `43 R8G8B8A8_SRGB`'s byte 79 must be `1F`, not `0F`: dfdutils'
`setChannelFlags` sets `KHR_DF_SAMPLE_DATATYPE_LINEAR` when the channel id equals
`KHR_DF_CHANNEL_RGBSDA_ALPHA` (15), and `KHR_DF_CHANNEL_BC3_ALPHA` is **also 15** — a numeric, not
semantic, comparison. KTX2 makes it a **`must`**. Following the spec literally would have produced
artifacts `ktx validate` rejects **with every test in this repository green**, because our own parser
compares the descriptor against the same table our writer emits. A third correction moved the cook's
test prefix from `TC` (13 cases in `thumbnail_cache_test.cpp` already own it) to `TX`; a fourth found
that **there were no image fixtures in this tree at all**, which the two committed PNGs now close.

**A 53-seed sabotage matrix ran to completion and found FIVE genuine gaps plus one test-robustness
defect**, each closed and re-proven by re-seeding. The sharpest three: **`TX40`'s "the cap is on BYTES,
not on dimension" had no case that could see it violated** — a dimension clause added beside the byte
comparison reddened nothing at all, because no case cooks at a dimension where the two families
disagree (that needs a 576 MB input on every lane); **`TX19` proves the FILTER composes, never that the
COOK chains its levels**, so pointing the cook's filter at level 0 for every level left it green and
only the byte goldens caught it; and **every anti-vacuity guard guarded itself**, reading
`REQUIRE(checked == TABLE.size())`, so deleting a row — including the two rows in `TX1` that exist
specifically to catch another seed — left the suite green while it tested less. Also closed:
`cookedTextureLevelAlignment`'s `lcm`, which **no format can distinguish** from `blockBytes` (BCn is 8
or 16 throughout and ASTC is 16, so no fixture can ever close it), and the CLI's write-after-cook-status
ordering, unreachable behaviourally because the decode's own bound refuses everything the cook's byte
cap would. Three of the five are pinned in **comment-stripped source text**, the `CM50` precedent.
**One seed reddened nothing exactly as predicted in advance** (reserve-before-cap-check, `S27`), and
three plan predictions were falsified and recorded: dropping `STB_IMAGE_STATIC` does **not** produce a
link failure on macOS today, so `TK18` is the only cover; `STBI_NO_FAILURE_STRINGS` still leaves a
non-empty error through the adapter's own fallback; and the alpha-threshold seed reddens `TK6` alone,
because the committed fixture's alpha is far below 254.

**A new CI-portability trap is now a named section in `.claude/rules/ci-portability.md`, and it is a
recurring class rather than a one-off.** doctest's `DOCTEST_STRINGIFY` expands to an **unqualified**
`toString(...)`, so an engine `toString(SomeEnum)` is found by ADL, beats doctest's own template, and
the decomposer then tries `std::string_view + const char*` — a **hard compile error on every lane**,
reported inside `doctest.h` rather than at the `CHECK`. This is its **second** occurrence
(`tests/rhi_format_test.cpp` already dodges it with double parens for `engine::rhi::toString`). Every
future `toString(SomeEnum)` on a public engine header inherits it; a label function named something else
(`cookedTextureStatusLabel`) is unaffected, which is why it is named that way.

**INV-T4, stated accurately.** The invariant is that **the texture files carry no floating point** —
not that `engine/assets` contains none, which is and always was false: 3.3.1's `mesh_cook.cpp` includes
`<cmath>` and `cooked_mesh.hpp` has `putF32`/`getF32`, because mesh vertex data *is* float. The three
new files add no `float`, no `double`, no `<cmath>` and no runtime table; `<numeric>`'s `std::lcm` is
integer-only and is the one include the rule does not reach.

**The named, unowned gap: nothing in this tree can upload the artifact.** `rhi::TextureFormat` carries
no block formats, and adding them is a **contract change, not an enumerator addition** —
`texelBlockSize` is documented as bytes per *texel* and `uploadTexture`'s precondition is
`data.size() == texelBlockSize(format) × mipWidth × mipHeight`, which is not expressible for a
block-compressed format. **Task 3.4.1 owns it** and depends on this one. The deliverable here is a file,
proven as a file. **`editor/validation/3.3.2-texture-cook-ktx2-basis.md` is macOS ✅ 15/17 rows
(2026-08-14, on merge commit `cf8575a`), with every measurement blank filled.**
**R2 is closed with numbers**: BC1 PSNR against `stb_dxt` over five images reads 44.60/45.45 dB
(gradient), 13.01/13.58 (noise), exact/exact (hard-edged), 20.56/21.26 (`texture-rgba-8x8.png`) and
**20.55/20.20 — ours AHEAD** (`texture-rgb-5x3.png`); the worst deficit is **0.85 dB**, so D4's
accepted cost is real but small and no `COOKED_TEXTURE_COOKER_VERSION` bump is called for.
**R3 is closed with numbers, and says something the row did not predict**: a 4096² BC3 cook with a
full chain runs in **0.68–0.71 s at 181.3 MB peak RSS**, byte-identical across three runs, and a 512²
in 0.03 s at 12.6 MB; `aero_tests` under ASan/UBSan is 10.32 s at 530.5 MB. **Unlike 3.3.1's R7,
nothing here is importer-dominated** — the decode is one stb_image call and the mip chain is the peak,
about 2.7× the decoded input. The cooked-vs-source *ratio* is meaningless on a synthetic source: both
fixtures deflate so well that the artifact is larger than the PNG, which is a fact about the source's
compressibility and not about the cook, exactly as the format's own arithmetic fixes the artifact size.
**R1 is the one that stays OPEN**: `ktx` is not installed here and there is genuinely no Homebrew
formula or cask (`brew info ktx` errors; `brew search ktx` returns only `mktxp`/`kty`). The row names
the real path — the Khronos 4.4.2 `.pkg`, `gh release download v4.4.2 --repo KhronosGroup/KTX-Software`
then `sudo installer` — and is recorded as an **attempt**, with no golden declared PASS. The eight DFD
tables have now been derived independently **three times** (spec, a compile of `createdfd.c`, and the
code-review round) and all three agree including both corrected `1F` bytes — but that is three readings
of one specification, **so the circularity R1 exists to break is still unbroken.** Row 4 (RenderDoc or
a second independent reader) falls with it: a reader of ours cannot be independent evidence about a
writer of ours.

**Carried-forward debt, unchanged by this task and worth scheduling as work of its own.** Seven ticked
validation rows across four tasks were signed off with their measurement blanks empty (3.2.5 rows 3, 8,
9, 11, 13; 3.2.2 row 9; 3.2.4 row 12) — each row's *behaviour* passed, each row's *evidence* is absent,
so **R4 and R8's in-editor half stay unmeasured** and D9's centimetre-versus-metre comparison has no
recorded figures. **Neither 3.3.1 nor 3.3.2 is among those seven** — both had their number-bearing rows
written so a blank tick is impossible, which is the pattern the other four should be brought up to
rather than the exception. Separately: **no Windows or Linux validation pass exists for any of the
thirteen Phase 2 tasks, for 3.1.1–3.1.4, for 3.2.1–3.2.5, or for 3.3.1**, and Phase 0's gate is still
held open on Windows/Linux 60 fps sign-off. That is platform-validation debt spanning four phases, and
with macOS fully green it is the whole of the remaining validation risk. **3.3.2's macOS pass IS run
(15/17)**, but its rows 3 and 4 — `ktx validate` and a second independent reader — are open for want of
tools on this machine, and they are the only external check that the KTX2 bytes are right.

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics (2.1 Editor shell, 2.2 Core panels, 2.3 Manipulation, 2.4 Undo/redo, 2.5 Scene I/O, 2.6 Project system v0) CLOSED in code and macOS-validated PASS, with Windows/Linux rows pending for every task (`editor/VALIDATION.md`). The whole-phase audit (2026-08-02) fixed two silent data-loss paths, a project root that was never made absolute, two CI false-greens, and four stale documentation claims. Full per-task and per-epic history — every defect, every sabotage matrix, every deviation — lives in `docs/10-engineering-log.md`'s Phase 2 entries; this row is deliberately a summary, not a duplicate. |
| **Phase 2 gate** | **MET 2026-08-02.** `samples/phase-2-editor-scene/` holds a project and a 4-entity scene authored entirely through the editor, with the save → New Scene → Open Scene round trip confirmed (`samples/phase-2-editor-scene/VALIDATION.md`); provenance is recorded there rather than asserted, since a hand-written `scene.json` is byte-identical to a real one and no test tier can tell them apart. Deliberately NOT `add_subdirectory`'d — this artifact is data (a provenance proof of the editor), not a compile-proof of engine code. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** **Epic 3.1** is fully merged except **3.1.5 (drag-into-scene), still open**: 3.1.1/3.1.2/3.1.3/3.1.4 (PRs #65/#66/#67/#69), CI-green on all three platforms, sabotage-proven (26/31/35/25 seeds), macOS-validated ✅ PASS 14/14, 14/14, 16/16, 10/10 — Windows/Linux rows pending for all four. `engine::Guid`/`engine::ContentHash`, the `.meta` v1 format, `AssetDatabase::rescan`'s eight phases, the machine-local `Library/asset-cache.json` import cache and the real Asset Browser all shipped across them. **Epic 3.2 (Importers) is CLOSED IN CODE — five merged tasks**, one canonical in-memory `ImportedModel` and eight claimed extensions: 3.2.1 glTF/fastgltf (PR #70 `f02ca65`, ✅ 12/12), 3.2.2 FBX/ufbx (PR #71 `c597a5b`, ✅ 13/13), 3.2.3 OBJ/tinyobjloader (PR #72 `c412e83`, ✅ 13/13), 3.2.4 Blender CLI/`.blend` (PR #73 `5ab07f3`, ✅ 15/15), 3.2.5 Assimp DAE/PLY/STL (PR #74 `7e0224f`, ✅ 14/14). Windows/Linux rows pending for all five, and seven of their ticked rows are missing the measurement they asked for. **Epic 3.3 (Cooker v0) is OPEN and 3.3.1 (Mesh cook → GPU buffers) is MERGED as PR #75 (`17a6821`, 13 commits), macOS-validated ✅ PASS 12/12 (2026-08-13) with every measurement blank filled; Windows/Linux rows pending.** It opens `engine/assets` (the `.aeromesh` container v1 + the cook, core-only, no vcpkg package at all), adds one editor adapter pair and `tools/cooker` (`aero_cooker mesh`), and is the first task to produce a runtime-consumable artifact. Mechanical gate green: **117/117 on both macOS presets** with `AERO_REQUIRE_GPU=1`, six guards passing, both reduced configurations rebuilt FRESH with `-G Ninja` and green at **28** and **41** ctest entries with `CM1`/`MC1`/`MK1` present, clang-format and clang-tidy clean by exit code, and `vcpkg.json`/`.github/`/`cmake/`/`runtime/` byte-identical to `main`. A 42-seed sabotage matrix ran to completion with **five genuine gaps found and closed** (`MC57`, `MC58`, `CM50`, `MK17`, `MC52`'s anti-vacuity guard), each re-proven by re-seeding; a code-review round found two more items, neither a defect, both closed as documentation. CI caught a **Windows-only `<ostream>`/`string_view` compile failure** no local run could see — the 0.4.1 trap's fourth occurrence, now a named section in `.claude/rules/ci-portability.md`. **R7 is closed with numbers on the validated build**: a 120.1 MB binary `.gltf` (2 250 000 verts, 13 482 006 indices) cooks in **0.37 s at 371.2 MB peak, ratio 1.00×**, and a 53.4 MB ASCII `.obj` in **0.87 s at 478.0 MB peak, ratio 0.52×** — **a smaller source costing MORE memory, because peak is dominated by the importer, not the cook** (~3.1× source for the binary path, ~9.0× for ASCII), which reframes R7's premise that the cook's single zero-initialized output vector was the binding constraint. **The no-engine-change streak ENDED there at seven, deliberately.** **3.3.2 (Texture cook → KTX2/Basis) is MERGED as PR #76 (`cf8575a`, 15 commits), CI-green on all three lanes with `headSha == HEAD` asserted, and macOS-validated ✅ 15/17 rows (2026-08-14) — R2 and R3 closed with numbers, R1 and row 4 left OPEN for want of `ktx` and a second reader on this machine.** It grows `engine/assets` to **five** pairs (the KTX2 subset container, the cook and the two integer block encoders — still no vcpkg package at all), adds one editor adapter pair (the tree's second stb_image TU) and the `aero_cooker texture` subcommand, and is the first artifact this project produces that a third-party tool can open. Mechanical gate green: **131/131 on `macos-debug`** with `AERO_REQUIRE_GPU=1`, six guards passing, both reduced configurations built FRESH with `-G Ninja` and green at **42** and **55** ctest entries with `CT1`/`TX1`/`BB1`/`TK1` and all fourteen `cooker.texture_*` names present, clang-format and clang-tidy clean by exit code, and `vcpkg.json`/`.github/`/`cmake/`/`runtime/` byte-identical to `main`. A **53-seed** matrix found **five genuine gaps plus one test-robustness defect**, each re-proven by re-seeding; **eleven spec statements were wrong and two were blocking** (the sRGB DFD tables' alpha-qualifier byte, which only `ktx validate` can catch). **A code-review round then closed five more findings, two of them real defects**: `cookTexture` divided by zero for a format outside the eight (UBSan + `SIGABRT`, unreachable from today's callers), and the parser accepted the partial mip chain `docs/09` §10.8 says it refuses (closed by adding the check, not by rewording the doc, as `UnsupportedShape`). The other three were a **BC4 DFD pinned by no tier at all** (27 of 44 bytes, and declaring BC5's colour model in it left the suite green at 131/131 — now every byte of all eight tables is pinned by `CT11` against a literal table, and R1 gains a BC4 artifact), **AC-32's repeated-flag arm, never added** (so `refuseRepeat` on all four texture-only flags had zero coverage — each guard now reddens `cooker.repeated_flag` on its own), and one literal header offset in a TU that says it has none. **R1, R2 and R3 are open until the post-merge validation pass**, and `ktx` is not installed on this machine. Full detail for every task in `docs/10-engineering-log.md`'s Phase 3 entries. |
| **Next task** | **~~Finish 3.3.2~~ DONE** — merged as PR #76 (`cf8575a`) with `headSha == HEAD` asserted, and macOS-validated ✅ 15/17 (2026-08-14). R2 (BC1 PSNR, worst deficit **0.85 dB**, ours AHEAD on one image) and R3 (**0.68–0.71 s at 181.3 MB peak** for a 4096² BC3 cook, byte-identical across three runs, and NOT importer-dominated unlike 3.3.1's R7) are both closed with numbers. **The one thing still owed: R1's `ktx validate` and row 4's second reader**, which need the Khronos 4.4.2 `.pkg` (`gh release download v4.4.2 --repo KhronosGroup/KTX-Software` then `sudo installer`) and RenderDoc — they are the ONLY non-circular proof the two corrected sRGB DFD bytes are right, and three first-party derivations agreeing is not a substitute. Then: **(b) 3.1.5 (drag-into-scene)**, fully unblocked — it depends on 3.1.3 and 3.2.1 (both merged), all eight importable extensions work, and both cooks now give it artifacts to place. **3.1.5 owns three decisions**: sub-asset identity (3.2.1's D13), replacing `LOCAL_MESH_HALF_EXTENT` (2.3.1's knowingly-wrong constant), and **the node-hierarchy gap 3.3.1 named but does not own**. **(c) 3.3.3 (cook determinism golden test)** is unblocked and now has **both** cook kinds to compare — it is the task that turns cross-platform byte-identity into a CI job, which is what the no-floating-point design exists for. **(d) 3.4.1** owns the `rhi::TextureFormat` block-format contract change 3.3.2 names, and nothing can upload a cooked texture until it lands. And **(e) the seven ticked-but-unmeasured validation rows**, plus the four-phase Windows/Linux platform-validation debt. See `docs/tasks/phase-3.md`. |

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
inside the subdirectory 3.3.1 opened, and `engine/CMakeLists.txt` is untouched by it.)
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
six named src-private), the `.cpp`s under `editor/src/`.

Test inventory at `main` after PR #76 (`cf8575a`), every number
**re-measured there, never derived by addition and never carried forward from an earlier step or an
earlier task** — read the totals from doctest's own `filters:` line, never from a `grep -c` of case
names. **`ctest -N` moves in ALL THREE configurations again**, because `aero_cooker` still takes **no
gate flag** and its cases (22 mesh + 14 texture) are therefore registered everywhere: **117 → 131**
with tools ON, **28 → 42** with `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, **41 → 55** with
`-DAERO_REFLECT_TOOLS=OFF` alone. A future gate flag on the cooker would silently shrink the reduced
configurations' coverage with no test able to report it — and the **code-review round moved none of the
three ctest numbers**, because both of its coverage closures extended an existing case rather than
adding an entry. `aero_tests` **523 → 656** (three new TUs, plus `CT27a` and `TX49` from that round;
`cooked_texture_test.cpp` (`CT*`), `texture_cook_test.cpp` (`TX*`) and `bc_block_test.cpp` (`BB*`) —
**`TC*` was TAKEN** by `tests/editor/thumbnail_cache_test.cpp`, which is why the cook's prefix is `TX`;
`aero::assets` was already on the link line, so no link-line change at all).
`aero_editor_shell_test` **1498 → 1516** (one new TU, `tests/editor/texture_cook_source_test.cpp`,
`TK1`–`TK18`). `aero_editor_imgui_test` **104**, `aero_scene_serialize_test` **23** and
`aero_editor_inspector_test` **22**, all unchanged — this task ships **no UI at all**. Both reduced
configurations were built FRESH in `build/tools-off-3.3.2` / `build/reflect-off-3.3.2` and are green,
with `CT1`, `TX1`, `BB1`, `TK1` and all fourteen `cooker.texture_*` entries present in both.
**A reduced-configuration probe must be configured with `-G Ninja`**: `CMAKE_GENERATOR` enters the
shadercross bootstrap's option hash, so the generator-less form reads the cached toolchain as COLD and
pays a from-source DXC rebuild that peaked at 7.6 GB here before a memory guard killed it.
`aero_editor_core` sources **58 → 59** (`texture_cook_source.cpp`) and `editor/src/*.cpp` **59 → 60**,
with **no** new `find_package` and **no** new `vcpkg.json` dependency.
`check-math-boundary.sh`'s scanned count **316 → 329** and `check-project-no-delete.sh`'s Check B scan
**59 → 60**, both picked up automatically — **neither script changes, and `.github/scripts/` is
byte-identical to `main`.** Guard count stays **six**; Check A's six-file denylist and Check B's
two-file `PERMITTED_DELETERS` are unchanged in membership, and `texture_cook_source.cpp` is in
**neither**, which is exactly what makes a future `std::filesystem::remove` there a hard CI failure.
**The tree's first two committed images** land here, `tests/fixtures/assets/texture-rgb-5x3.png` and
`texture-rgba-8x8.png` — before them `git ls-files | grep -iE '\.(png|jpg|tga|bmp|gif|psd|hdr)$'`
returned nothing at all.
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
