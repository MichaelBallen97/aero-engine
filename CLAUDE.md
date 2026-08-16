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
merged, macOS-validated, no macOS debt — and Epic 3.4 (PBR materials) is OPEN: 3.4.1 MERGED and
macOS-validated, 3.4.2 COMPLETE IN CODE on its branch and PENDING its macOS validation pass.** Epic 3.1
(AssetDatabase · assets) is merged except **3.1.5 (drag-into-scene), still open**.
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
**3.4.2 (Material inspector editing) is MERGED as PR #79 (merge commit `3aebbad`, 15 commits) and
CI-green on all three lanes with the green run's `headSha` asserted equal to `HEAD` before the merge,
and macOS-validated ✅ PASS on ALL 12 ROWS (2026-08-16), every measurement blank filled.** Rows 3, 4
and 6 of that page are the only coverage six of **seven** declared sabotage seeds have anywhere; the
seventh, S26, is observable on no Apple platform at all and waits on a Windows or Linux pass.

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

**3.4.1 — Material asset + PBR shader, MERGED and macOS-validated.** Epic 3.4 opened with three
firsts: the **first cooked texture this project has ever drawn on a GPU**, the first `.aeromat` parsed
end to end, and the first asset resolved at run time **by GUID**. Four layers moved. **`engine/rhi`**
finished the block-format contract change 3.3.2 assigned it — six BC enumerators (`Count` 19 → **25**),
`texelBlockSize` re-documented in Vulkan's block vocabulary and **bit-for-bit unchanged for every
pre-3.4.1 value**, new `texelBlockWidth`/`texelBlockHeight`/`textureLevelByteSize` (the last being
**the** upload-size formula), and a uniform engine-owned refusal of non-block-aligned BC **top** levels
(D3D12's rule adopted on all three backends per ADR-002) — **all of it recorded per the 0.4.1 D18
amendment protocol**, nine amendments each with its verification source. **`engine/reflect`** gained
`material_format.{hpp,cpp}` and `docs/09` a normative **§11** (Reserved renumbered §11 → **§12**).
**`engine/render`** gained `material.hpp`, `texture_upload.{hpp,cpp}`, the src-private
`material_pack.hpp`, 48-byte vertices, the `MaterialHandle` registry inside `ForwardRenderer`, and
**`PUBLIC aero::assets`** on its link line. **`shaders/`** carried the GGX pair rewritten **in place**,
so `shaders/CMakeLists.txt` saw zero diff. `samples/phase-3-materials` is 16 new files and one
modified. Its two structural lessons are still live: **a file-local packer is unfalsifiable** (the light
mirror moved into `material_pack.hpp` beside `packMaterial`, because zeroing `eyePosition` left all 712
cases green), and **a mapping worth having twice is worth having once** (the hand-written per-slot
default-texture table was deleted, not tested — `defaultTextureKindForSlot` is now the single place the
slot→default decision is made, and 3.4.2's `materialSlotIsSrgb` is composed from it rather than
restated). Full detail in `docs/10`.

**3.4.2 — Material inspector editing, complete in code and awaiting its macOS pass.** The editor learns
what a material is: at the branch point an `.aeromat` classified `Unknown`, selecting one told Import
Details that no importer claims the file type, and editing one meant hand-authoring JSON elsewhere.
Four editor pairs, split by **dependency** rather than by feature: **`material_edit.{hpp,cpp}`**
(PUBLIC and pure — the document→`render::MaterialParams` mapping `docs/09` §11.4 declares normative, the
slot walk both ways, the token→`SamplerDesc` table with `mipFilter: none` as the clamp-to-base idiom,
the per-slot colour space **composed from** `defaultTextureKindForSlot`, and New Material's unique-name
helper; it also discharges the enum-mirror assertion `material_format.hpp` assigned to this task by
name); **`material_session.{hpp,cpp}`** (PUBLIC, GPU-free and ImGui-free — sticky targeting, dirty as
`sessionCopy != fileCopy`, Apply as validate → write once atomically **only when dirty** → adopt, and
the ONE `.aeromat` write path); **`material_panel.{hpp,cpp}`** (src-private, the only new ImGui TU — the
**eighth** panel, id `"Material"` **FROZEN**, right dock, registered last, every §11 field editable with
clamping done in C++ rather than by the widget); and **`material_preview.{hpp,cpp}`** (src-private, the
only new GPU TU — its own `RenderTarget` and its own `ForwardRenderer`, since a `MaterialHandle` is
per-renderer, driving `updateMaterial`'s **first production call site** and loading slot textures
through the real decode → cook → parse → upload chain, one per tick, in the post-draw service pass).
`.aeromat` became `AssetKind::Material`, and New Material closes the authoring loop.

**Two deviations were approved and both are recorded in `docs/10`.** **`aero::render` joined
`aero_editor_core`'s PUBLIC link group**, because the spec's "no link-line change at all" and its own
AC-25 cannot both hold — `aero::scene_render` is PRIVATE, which on a static library propagates as
`$<LINK_ONLY:…>`, so `engine/render/include` never reaches a public editor header. It is the
`aero::assets` edge from 3.3.1 criterion for criterion (no `find_package`, PUBLIC deps already PUBLIC
here, no build gate, no boundary probe links `aero_editor_core`). And **`MaterialParseResult` gained a
`warnings` vector**, invoking D11's own escape hatch for a genuinely missing seam: the alternative was
a second owner for `docs/09` §11's key vocabulary inside `/editor`, and the aggregate notice it replaced
could not tell a **destructive** difference (an unknown key, deleted on save) from a cosmetic one (key
order, an uppercase GUID). **AC-34 is therefore amended**: `engine/` is **not** byte-identical, and the
diff is exactly two files — `engine/reflect/include/aero/reflect/material_format.hpp` and
`engine/reflect/src/material_format.cpp`, one appended field and a sweep that formats each sentence
once and uses it twice. No format change, no version bump, no link-line change, no dependency.

**3.4.2's sabotage matrix ran all 30 seeds; two reddened nothing and both were closed structurally** —
the kind-filter option list moved beside the enum it enumerates (deleting the second copy rather than
testing the panel), and a clamp-call source pin, each re-proven by re-seeding. **Six witness
attributions in the plan were wrong and are recorded rather than smoothed over**, the sharpest being
that **`ME24` compared a mapping function against itself** (`desc.addressU == materialAddressModeFor(wrap)`),
so it cannot see a swapped clamp/mirror at all — `ME15`–`ME17` and `ME22` pin against literals and are
what actually redden. **The declared-unwitnessable class narrowed from eight seeds to seven.**

**3.4.2's code-review round found eleven gaps, two of them blocking, and all eleven are closed.** The
one worth carrying everywhere: **`RenderTarget::allocate`'s "the backend defers the GPU release" is
true of the device MEMORY and false of the HANDLE.** In the pinned SDL 3.4.12 tree, Vulkan
(`SDL_gpu_vulkan.c:7070-7073`) and D3D12 (`SDL_gpu_d3d12.c:1385, :1460`) `SDL_free` the container
**immediately** — *"Containers are just client handles, so we can destroy immediately"* — while Metal
(`SDL_gpu_metal.m:936-944`) queues it. ImGui **records** a texture id in the draw walk and **binds** it
in `endFrame`, after the post-draw service pass, so a texture handed to ImGui and then reallocated is a
**heap use-after-free on Vulkan and D3D12 and benign on Metal** — invisible to every test run here.
**The rule: reallocate where the handle is read, inside the draw walk, never in the service pass
afterwards**, which is exactly what `ViewportPanel` already did. The second lesson is about tests
rather than SDL: **a source-text pin can certify the invariant it is blind to** — `I96` greped for
`destroyTexture(`/`destroyMaterial(`, so the destroy inside `RenderTarget::resize` was invisible to it
and the defective code **satisfied** the pin; it now encodes ordering against ImGui's consumption
instead of function membership. Two more worth knowing: New Material accepted an **incomplete**
`DirectoryListing` as proof a name was free (`truncated`/`skipped` unchecked — a prefix cannot prove
absence, and acting on one overwrites an authored material silently), and `AssetRecord::contentHash`
was read without its validity guard, so an unhashed record's **all-zero digest** (the empty file's real
value, never a sentinel) produced a phantom external-change notice and a poisoned texture-cache key.

**A near-miss worth generalising: a reduced-configuration claim must name which binaries it ran.**
`I88`–`I92` asserted the preview is available unconditionally, which is false with
`-DAERO_SHADER_TOOLS=OFF`; the earlier probe of that path built `aero_editor_shell_test` only and never
built `aero_editor_imgui_test`, so the failure (115/120) surfaced only at the full gate. Both arms now
**assert** rather than skip — a skip would leave AC-32 untested in the one configuration that can test
it.

**The 0.4.1 D18 amendment protocol is the standing rule for `engine/rhi`, not a one-off.** Any change
to a public rhi header is recorded in `docs/10` with the source that verified it — here, the pinned
SDL 3.4.12 tree answered both open questions: the transfer pitch fields are in **texels** (Vulkan
forwards them to `bufferRowLength`, Metal **ignores** them and derives a block-aware pitch from the
region, D3D12 block-rounds), and **the six BC formats are NOT on SDL's universally-supported SAMPLER
list** (seventeen uncompressed formats, no BC format), so `format.hpp`'s universality promise gained a
scoped exception naming `Device::supportsTextureFormat` and task 6.3.1 rather than an SDL-universal
claim it cannot support.

**Named handoffs, each with an owner.** Scene-side material references → **3.1.5** (nothing in a scene
file can name a material today; the `MeshRenderer` field is 3.1.5's by its own task text).
Import-materialization → first needed by **3.1.5**; 3.4.2's slot-derived preview colour space is the
only contribution to that story so far. Drag-drop texture assignment → **3.1.5**, which introduces the
tree's first drag payloads; 3.4.2's picker is the assignment surface until then. Main-viewport live
material preview → automatic once 3.1.5 lands, since the seam and the per-draw resolution both exist.
Rendered material thumbnails → a named deferral, **unowned** (it needs preview readback plus ledger
integration, which is why `isThumbnailDecodable` was deliberately not touched). **BLEND transparency**
→ a named **decision-waiting** gap, renderer-only, since the format already carries everything.
**IBL / environment lighting** → a named **unowned** gap blocked on a format decision, not on shader
work: it needs a cubemap, which `docs/09` §10 currently refuses. Shadows → **3.6.2**; tonemap/gamma →
**3.6.3** (output is raw linear until then). A shared token→`SamplerDesc` helper → decided by the
**second** consumer; 3.4.2's `material_edit.hpp` is now the second, and it kept the mapping editor-side
because the sample and the editor are the only two callers. **`reflect-gen` growth (`Vec4`, enums,
`Guid`, optional-wrapped nested structs)** → the **component** tasks that need it (3.1.5, 3.5.2), never
a panel: 3.4.2's D1 refused it because a generated serializer for `MaterialDocument` would be a
**second writer for a normative on-disk format**, and that reason does not age away even after the
subset grows. Two small ones from 3.4.2: `model_import_session.cpp`'s `sourceHashUsable` is now a
duplicate of `assetContentHashUsable` and should be collapsed by whoever next edits that file, and
`RenderTarget::resize() == false` is unreachable from any tier without an injectable allocation
failure — an engine change nobody has needed yet.

**Carried-forward debt, and 3.4.2 adds to both halves of it.** Seven ticked validation rows across four
tasks were signed off with their measurement blanks empty (3.2.5 rows 3, 8, 9, 11, 13; 3.2.2 row 9;
3.2.4 row 12) — each row's *behaviour* passed, each row's *evidence* is absent, so **R4 and R8's
in-editor half stay unmeasured** and D9's centimetre-versus-metre comparison has no recorded figures.
**None of Epic 3.3's three tasks is among those seven**, nor is 3.4.1 — those four had their
number-bearing rows written so a blank tick is impossible, which is the pattern the other four should
be brought up to rather than the exception, and 3.4.2's twelve-row page is written the same way (row 7
carries two trigger counts, row 9 three costs). Separately: **no Windows or Linux validation pass
exists for any of the thirteen Phase 2 tasks, for 3.1.1–3.1.4, for 3.2.1–3.2.5, or for 3.3.1–3.3.3**,
and Phase 0's gate is still held open on Windows/Linux 60 fps sign-off. **3.4.1 grew that debt by one
task and 3.4.2 grows it by another**, and each carries something specific: 3.4.1's Windows lane is the
**live retirement** of the D3D12 block-alignment rule it adopted engine-wide from documentation, and
3.4.2's Windows and Linux lanes are the only places its preview's destroy timing differs from the
platform it was written on — SDL frees a texture container **immediately** on Vulkan and D3D12 and
queues it on Metal, which is exactly the class of defect its code-review round caught. That is
platform-validation debt spanning four phases, and with macOS otherwise green it is the whole of the
remaining validation risk. **3.4.2's own macOS pass is NOT YET RUN** — the page exists, written before
the pass as always, and until it is run its rows 3, 4 and 6 are six declared seeds' only coverage
*in principle* rather than in fact. The seventh, **S26, has no macOS witness at all** — SDL queues a
texture-container free on Metal and performs it immediately on Vulkan and D3D12 — so that one seed is
the first this project has whose only observational cover is a Windows or Linux pass.

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics (2.1 Editor shell, 2.2 Core panels, 2.3 Manipulation, 2.4 Undo/redo, 2.5 Scene I/O, 2.6 Project system v0) CLOSED in code and macOS-validated PASS, with Windows/Linux rows pending for every task (`editor/VALIDATION.md`). The whole-phase audit (2026-08-02) fixed two silent data-loss paths, a project root that was never made absolute, two CI false-greens, and four stale documentation claims. Full per-task and per-epic history — every defect, every sabotage matrix, every deviation — lives in `docs/10-engineering-log.md`'s Phase 2 entries; this row is deliberately a summary, not a duplicate. |
| **Phase 2 gate** | **MET 2026-08-02.** `samples/phase-2-editor-scene/` holds a project and a 4-entity scene authored entirely through the editor, with the save → New Scene → Open Scene round trip confirmed (`samples/phase-2-editor-scene/VALIDATION.md`); provenance is recorded there rather than asserted, since a hand-written `scene.json` is byte-identical to a real one and no test tier can tell them apart. Deliberately NOT `add_subdirectory`'d — this artifact is data (a provenance proof of the editor), not a compile-proof of engine code. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** **Epic 3.1** is fully merged except **3.1.5 (drag-into-scene), still open**: 3.1.1/3.1.2/3.1.3/3.1.4 (PRs #65/#66/#67/#69), CI-green on all three platforms, sabotage-proven (26/31/35/25 seeds), macOS-validated ✅ PASS 14/14, 14/14, 16/16, 10/10 — Windows/Linux rows pending for all four. `engine::Guid`/`engine::ContentHash`, the `.meta` v1 format, `AssetDatabase::rescan`'s eight phases, the machine-local `Library/asset-cache.json` import cache and the real Asset Browser all shipped across them. **Epic 3.2 (Importers) is CLOSED IN CODE — five merged tasks**, one canonical in-memory `ImportedModel` and eight claimed extensions: 3.2.1 glTF/fastgltf (PR #70 `f02ca65`, ✅ 12/12), 3.2.2 FBX/ufbx (PR #71 `c597a5b`, ✅ 13/13), 3.2.3 OBJ/tinyobjloader (PR #72 `c412e83`, ✅ 13/13), 3.2.4 Blender CLI/`.blend` (PR #73 `5ab07f3`, ✅ 15/15), 3.2.5 Assimp DAE/PLY/STL (PR #74 `7e0224f`, ✅ 14/14). Windows/Linux rows pending for all five, and seven of their ticked rows are missing the measurement they asked for. **Epic 3.3 (Cooker v0) is CLOSED — three merged tasks, every one CI-green on all three lanes with `headSha == HEAD` asserted, and every one macOS-validated with every measurement blank filled**: 3.3.1 Mesh cook → GPU buffers (PR #75 `17a6821`, 13 commits, 42 seeds, ✅ 12/12) opened `engine/assets` and `tools/cooker` and produced the tree's first binary format and first runtime-consumable artifact; 3.3.2 Texture cook → KTX2/Basis (PR #76 `cf8575a`, 15 commits, 53 seeds, ✅ every row) added the KTX2 subset container, the texture cook and the two integer block encoders, and is the first artifact this project produces that a third-party tool can open — `ktx validate` 4.4.2 PASS on all eight artifacts, proven non-vacuous by re-seeding the corrected DFD byte and watching the validator reject it with the exact predicted `error-6028`; 3.3.3 Cook determinism golden test (PR #77 `234a009`, 8 commits, 24 seeds, ✅ 13/13) ships zero C++ and turns cross-lane, cross-config and cross-time byte-identity into a continuous CI check. Windows/Linux rows pending for all three. **Epic 3.4 (PBR materials) is OPEN with BOTH its tasks done in code.** **3.4.1 (Material asset + PBR shader) is MERGED as PR #78 (merge commit `a01765d`, 10 commits), CI-green on all three lanes with `headSha == HEAD` asserted before the merge, and macOS-validated ✅ PASS on ALL 11 ROWS (2026-08-16) with every measurement blank filled — 6 cooked textures upload in 6.2 ms (mean 1.0 ms), the sample holds ~121 fps, and the sidecars read 5/2 and 0/1.** It made a cooked texture mean something: the first cooked texture ever drawn on a GPU here, the first `.aeromat` parsed end to end, the first asset resolved at run time by GUID, six BC formats and `textureLevelByteSize` in `engine/rhi` (recorded per the 0.4.1 D18 protocol), `docs/09`'s normative §11, the `MaterialHandle` registry, and the GGX shader pair rewritten in place. **3.4.2 (Material inspector editing) is MERGED as PR #79 (merge commit `3aebbad`, 15 commits), CI-green on all three lanes with `headSha == HEAD` asserted before the merge, and macOS-validated ✅ PASS on ALL 12 ROWS (2026-08-16) with every measurement blank filled — Apply echoes +1 and Create +1 (not +2), the panel first-opens inside a single vsync-paced frame, and the worst committed fixture uploads in 2.1 ms.** It is the task that makes materials editable rather than hand-authored: four editor pairs (`material_edit` pure, `material_session` GPU-free, `material_panel` the only new ImGui TU, `material_preview` the only new GPU TU), `AssetKind::Material`, the eighth panel (id `"Material"`, FROZEN, right dock, registered last), a live preview with its own `RenderTarget` and its own `ForwardRenderer` that gives `updateMaterial` its **first production call site**, slot textures through the real decode → cook → parse → upload chain, and a New Material button. **Two recorded deviations**: `aero::render` joins `aero_editor_core`'s PUBLIC link group (the spec's "no link-line change" and its own AC-25 cannot both hold, since `aero::scene_render` is PRIVATE), and `MaterialParseResult` gains a `warnings` vector under D11's own escape hatch — so **AC-34 is amended and `engine/` is NOT byte-identical**, the diff being exactly `engine/reflect/{include/aero/reflect/material_format.hpp, src/material_format.cpp}`, with `tools/`, `shaders/`, `runtime/`, `samples/`, `vcpkg.json`, `.github/`, `cmake/` and the determinism manifest all byte-identical and **no dependency of any kind**. Mechanical gate green: **133/133 on both macOS presets** with `AERO_REQUIRE_GPU=1`; fresh `-G Ninja` reduced configurations **44/44** and **57/57**; `ctest -N` **133 / 44 / 57 — unmoved**, because every new test rides an existing binary and each editor test binary is ONE ctest entry, so the growth reads only in the doctest totals (**716 / 1577 / 124 / 23 / 22**; `aero_tests` 713 → 716 is a plan-recorded surprise caused by the reflect deviation); six guards exit 0 (math-boundary **347**, project-no-delete Check B **64**); clang-format and clang-tidy clean by exit code. A **30-seed** matrix ran to completion with **two genuine gaps**, both closed **structurally** and re-proven by re-seeding, and six of the plan's own witness attributions corrected. A code-review round found **eleven gaps, two blocking**, all closed — including a use-after-free that is deterministic on Vulkan and D3D12 and benign on Metal. Full detail for every task in `docs/10-engineering-log.md`'s Phase 3 entries. |
| **Next task** | **~~3.4.1~~ DONE, MERGED and macOS-VALIDATED** (PR #78, `a01765d`). **~~3.4.2~~ DONE, MERGED and macOS-VALIDATED** as PR #79 (`3aebbad`), CI-green on all three lanes with `headSha == HEAD` asserted, ✅ PASS on all 12 rows. Its Windows and Linux lanes were the first run of the preview's second `RenderTarget` under WARP and lavapipe — exactly where a mistimed texture release bites and Metal does not — and both passed. **S26 remains uncovered by any pass and cannot be covered from macOS** — SDL queues the texture-container free on Metal and performs it immediately on Vulkan and D3D12, so it waits on a Windows or Linux run. Immediate next steps, in order: **(a) 3.1.5 (drag-into-scene)**, fully unblocked and now additionally unblocked for material assignment by 3.4.2's picker and session — it owns four decisions: sub-asset identity (3.2.1's D13), replacing `LOCAL_MESH_HALF_EXTENT` (2.3.1's knowingly-wrong constant), **the node-hierarchy gap 3.3.1 named but does not own**, and the `MeshRenderer` material field this epic has been deferring to it. And **(b) the seven ticked-but-unmeasured validation rows**, plus the four-phase Windows/Linux platform-validation debt, which with macOS otherwise green is the whole of the remaining validation risk. **A note for whoever adds a fixture or a cook change next**: `tests/cooker/determinism.sha256` is FROZEN, a red manifest case is `docs/09` §9.11's `cookerVersion` sentence firing, and the regeneration ritual lives in the manifest's own header — never edit a hash to green a red run. See `docs/tasks/phase-3.md`. |

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
its link line — a new link edge, downward and cycle-free, since `assets` links only
`aero::core` (plus private profiling). `engine/reflect` gains `material_format.{hpp,cpp}` with **no
link-line change at all** (`aero_reflect` already linked `PUBLIC aero::core`). `engine/scene` and
`engine/scene_serialize` are byte-identical: a material is not yet nameable from a scene file, and
**3.1.5 owns that** — 3.4.2 did not take it. Still **no dependency of any kind** — `vcpkg.json`,
`.github/`, `cmake/`, `runtime/` and `shaders/CMakeLists.txt` are all byte-identical to `main`.
**Task 3.4.2 touches `engine/` in exactly two files**, and it is a **recorded deviation** from its own
AC-34 rather than a quiet expansion: `engine/reflect/include/aero/reflect/material_format.hpp` gains a
`std::vector<std::string> warnings` field on `MaterialParseResult`, **appended after `error`** so every
pre-existing caller compiles untouched, and `engine/reflect/src/material_format.cpp`'s unknown-key
sweep formats each finding **once** and uses it twice — the `AERO_LOG_WARN` and the returned entry can
never disagree. No format change, no version bump, **no link-line change**, no dependency. It also
adds the tree's second editor→engine link edge: **`aero::render` joins `aero_editor_core`'s PUBLIC
group**, so a public editor header can name `render::MaterialParams`; `aero::scene_render` stays
PRIVATE, and that distinction is the point rather than an oversight.
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
source of a readable decode reason. **Tasks 3.3.3 and 3.4.1 add NO editor pair at all** — `/editor` was
byte-identical to `main` across both branches. **3.4.2 adds FOUR pairs, taking the count to FIFTEEN**
(re-counted at task end, never added to the remembered number): `material_edit.{hpp,cpp}` (PUBLIC and
PURE — the document→render bridge; no ImGui, no disk, no GPU call, no logging),
`material_session.{hpp,cpp}` (PUBLIC, GPU-free and ImGui-free — the target/dirty/apply machine and the
**one** `.aeromat` write path), `material_panel.{hpp,cpp}` (src-private, the only new ImGui TU) and
`material_preview.{hpp,cpp}` (src-private, the only new GPU TU). It also adds two pure predicates to
existing public headers, both forced by its code-review round: `listingIsComplete` in
`project_files.hpp` and `assetContentHashUsable` in `asset_meta.hpp`. The `.hpp`s live under
`editor/include/aero/editor/` (except those named src-private, which live beside their `.cpp` in
`editor/src/` — 21 tracked `editor/src/*.hpp` in total), the `.cpp`s under `editor/src/`.

Test inventory at `main` after PR #79 (`3aebbad`), every number
**re-measured there, never derived by addition and never carried forward from an earlier step or an
earlier task** — read the totals from doctest's own `filters:` line, never from a `grep -c` of case
names. **`ctest -N` reads 133 / 44 / 57 and DID NOT MOVE at 3.4.1 or at 3.4.2** — tools ON, then
`-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, then `-DAERO_REFLECT_TOOLS=OFF` alone. **The
reason matters more than the number**: `aero_tests`, `aero_editor_shell_test` and
`aero_editor_imgui_test` each register with ctest as a **single entry** (`tests/CMakeLists.txt`), so
3.4.1's 57 new doctest cases and 3.4.2's **84** move nothing there at all, and samples register no
test. An unmoved `ctest -N` means "zero C++" for task 3.3.3 and means nothing of the kind for either
3.4 task — check a zero-C++ claim against the **doctest** totals instead. The triple
last moved for the cooker's own cases (**117 → 131 → 133**, **28 → 42 → 44**, **41 → 55 → 57**),
because `aero_cooker` takes **no gate flag** and its 38 cases plus 3.3.3's two manifest cases are
registered everywhere; a future gate flag on the cooker would silently shrink the reduced
configurations' coverage with no test able to report it.

Doctest, all five binaries: **716 / 1577 / 124 / 23 / 22**. `aero_tests` **713 → 716** — `MT35`–`MT37`
on the grown `tests/material_format_test.cpp`, and a **plan-recorded surprise**: 3.4.2's AC-38 expected
this binary unmoved, and the `MaterialParseResult` warnings deviation is exactly why it is not.
`aero_editor_shell_test` **1516 → 1577** (+61: the new `tests/editor/material_edit_test.cpp` with
`ME1`–`ME50` plus `ME14b`, then `AV48`–`AV54`, `PF-c6`/`PF-c7` and `MI154`), and
`aero_editor_imgui_test` **104 → 124** (+20: `I83`–`I102`, of which `I95`–`I98` and `I102` are
comment-stripped **source-text pins** for rules no runtime tier here can see).
`aero_scene_serialize_test` **23** and `aero_editor_inspector_test` **22** are **unmoved**. Both
reduced configurations were built FRESH in `build/tools-off-3.4.2` / `build/reflect-off-3.4.2` and are
green at **44/44** and **57/57**, with `ME1` present in both.
**A reduced-configuration probe must be configured with `-G Ninja`**: `CMAKE_GENERATOR` enters the
shadercross bootstrap's option hash, so the generator-less form reads the cached toolchain as COLD and
pays a from-source DXC rebuild that peaked at 7.6 GB here before a memory guard killed it. **And it
must name which binaries it ran**: 3.4.2's earlier probe built `aero_editor_shell_test` only, so five
preview cases that assert the wrong contract in a tools-OFF build (115/120) survived until the full
gate. `aero_editor_imgui_test` now carries `AERO_SHADER_TOOLS_ENABLED=1` in its own
`if(AERO_SHADER_TOOLS)` block and **both arms assert** — a skip would leave AC-32 untested in the one
configuration that can test it.
`aero_editor_core` sources **59 → 63** and tracked `editor/src/*.cpp` **60 → 64** (3.4.2's four pairs).
`check-math-boundary.sh`'s scanned count **338 → 347** (eight new editor C-family files plus one test
TU — exactly the prediction, re-measured **after `git add`**, since `git ls-files` sees only tracked
files) and `check-project-no-delete.sh`'s Check B scan **60 → 64**, its glob picking the new files up
automatically — **neither script changes, and `.github/scripts/` is byte-identical to `main`.** Guard
count stays **six**; Check A's six-file denylist and Check B's two-file `PERMITTED_DELETERS` are
unchanged in membership, and 3.4.2's four new TUs are in **neither**, which is what makes a future
destructive call in them a hard CI failure. The other four guards read 102 / 61 / 100 / 61.
**Committed images: two at `tests/fixtures/assets/` (3.3.2) plus six 32×32 PNGs and six `.ktx2` under
`samples/phase-3-materials/textures/` (3.4.1)** — the `.ktx2` cooked once with pinned GUIDs and
regenerable byte-identically from the PNGs by the README's recorded commands, which is 3.3.3's
guarantee being spent rather than re-proven. **Committed text fixtures gained seven `.aeromat` files at
`tests/fixtures/materials/` (3.4.2)**, reached through `AERO_MATERIAL_FIXTURES_DIR` — a path, not a
flag, so a missing one is a `REQUIRE` failure rather than a silent skip.
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
