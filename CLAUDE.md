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

**Phase 3 (Asset Pipeline & 3D Content) is OPEN. Epic 3.1 (AssetDatabase · assets) is CLOSED —
3.1.5 (drag-into-scene) merged as PR #81 (`5a1bc69`) and macOS-validated 16/16. Epics 3.3 (Cooker v0)
and 3.4 (PBR materials) are both CLOSED — every task merged, CI-green on all three lanes with
`headSha == HEAD` asserted, and macOS-validated with every measurement blank filled. Epic 3.5
(Skeletal animation · render) is CLOSED IN CODE with BOTH tasks merged (3.5.1 as PR #80, 3.5.2 as
PR #82 `5622a77`) and BOTH pending their macOS passes. Epic 3.6 (Rendering essentials) is now OPEN and
3.6.1 (Frustum culling) is MERGED as PR #84 (merge commit `40d83a6`, eleven commits) — the full local
gate green, the 30-seed matrix run to completion, a code-review round closed with one BLOCKING gap
among its three, and all six CI checks green with `headSha == HEAD` asserted before the merge. Its
ten-row macOS pass is ✅ **PASS on ALL 10 ROWS** with every blank filled. **3.6.2 (Directional shadow
map) is MERGED as PR #85 (merge commit `3ffaadf`, sixteen commits)**, CI-green on all six checks with
`headSha == HEAD` asserted before the merge, and PENDING its twelve-row macOS pass — so **two of Epic
3.6's three tasks are now merged**. 3.6.3 (Tonemap/gamma pass) is COMPLETE IN CODE on
`feat/3.6.3-tonemap-gamma-pass` (cut from `main` @ `64df342`, now **twelve own commits plus one merge
commit**) — the full local gate green, the 31-seed matrix run to completion with three genuine gaps
all closed structurally and re-proven, two code-review rounds closed with eight findings between them,
and its twelve-row macOS pass PENDING. **It is NOT merged and no CI run exists for it yet.** The two
were built in parallel off the same branch point in separate worktrees; 3.6.2 landed first, so 3.6.3
has taken `origin/main` in with a MERGE (the 3.1.5 pattern, not a rebase) and **every whole-tree count
on its page was re-derived from the merged tree rather than carried forward**.**

**THE STANDING "output is raw linear until 3.6.3" CAVEAT IS RESOLVED IN CODE.** Every validation page
since 3.4.1 has carried it and `shaders/scene.frag.hlsl:3` named this task as the owner. The scene now
draws into an `RGBA16Float` target `render::PostProcess` owns, and a one-triangle `SV_VertexID` pass
applies exposure, a tone curve and the **sRGB OETF** into the 8-bit surface. **Every picture gets
brighter and the highlights stop clipping**; that is the point, and it invalidates no recorded pass,
because those passes judged relative behaviour. **The existing pages and sample READMEs are NOT
edited** (forward-only) — this block is where the resolution is recorded. `--tonemap=none` means no
tone CURVE, never no ENCODE: there is deliberately no flag anywhere that can disable the OETF, and the
only escape hatch is at the call site (`samples/phase-3-tonemap --raw`).

Epic 3.2 (Importers) is **closed in code** — five merged tasks: 3.2.1 glTF, 3.2.2 FBX, 3.2.3 OBJ,
3.2.4 Blender CLI, 3.2.5 Assimp (DAE/PLY/STL), the last merged as PR #74 (`7e0224f`) on 2026-08-12.
**Epic 3.3's three tasks are PR #75 (`17a6821`, ✅ 12/12), PR #76 (`cf8575a`, ✅ every row) and PR #77
(`234a009`, ✅ 13/13)**; **Epic 3.4's two are PR #78 (`a01765d`, 10 commits, ✅ 11/11, 2026-08-16) and
PR #79 (`3aebbad`, 15 commits, ✅ 12/12, 2026-08-16)** — 3.4.1's rows 2, 4, 5 and 6 are the ONLY
witness anywhere for five shader-only sabotage seeds (S24–S28) and probe X3, and 3.4.2's rows 3, 4 and
6 are the only coverage six of its **seven** declared seeds have anywhere. **3.4.2's seventh seed, S26,
is observable on no Apple platform at all** — SDL queues a texture-container free on Metal and performs
it immediately on Vulkan and D3D12 — so it is the first seed in this project whose only observational
cover is a Windows or Linux pass, and it is still uncovered.

**Epics 3.1 and 3.2, condensed.** Full per-task detail — every sabotage matrix, every build-time
finding, every dead end — lives permanently in `docs/10-engineering-log.md`'s Phase 3 entries; this is
a summary, not a duplicate. **3.1.1** GUIDs + `.meta` (PR #65 `2be73e1`, 26 seeds, ✅ 14/14).
**3.1.2** import cache & dependency tracking (PR #66 `3470b87`, 31 seeds, ✅ 14/14), closing 3.1.1's
orphan-re-attachment deferral and a symlinked-directory duplicate-GUID defect. **3.1.3** asset browser
v1 (PR #67 `aa914fb`, 35 seeds, 11 review findings incl. a GPU-texture use-after-free invisible on
macOS because SDL frees synchronously on Vulkan/D3D12 and only defers on Metal, ✅ 16/16).
**3.1.4** hot-reload watcher (PR #69 `ebc4da6`, 25 seeds, ✅ 10/10) — R1's per-sweep cost stayed
unmeasured. **3.1.5** drag-into-scene (25 commits, 37 seeds, 5 review findings closed, macOS pass
PENDING) — the epic's closer, detailed below. **3.2.1** glTF/fastgltf (PR #70 `f02ca65`, 32 seeds, 12 review findings, ✅ 12/12), the
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
**Its named gap was DECIDED at 3.1.5, and the answer was not a container**: v1 still stores no node
hierarchy and **the cook must still never "solve" this by baking node transforms into vertices**
(`ImportedMesh` is shared across nodes by construction), but the editor now materializes the node tree
into **scene entities** at drop time, so placement lives in the scene file. The residual — a cooked
model/prefab container for a consumer with **no importer**, i.e. a script's runtime `spawn()` — is
narrower and belongs to **4.4.4 (prefab-lite) / Phase 5's pak**.
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

**Epic 3.4 (PBR materials), condensed — two merged tasks, both macOS-validated, no macOS debt.** Full
per-task detail lives in `docs/10-engineering-log.md`; this is the residue that still governs new work.

**3.4.1 — Material asset + PBR shader.** Epic 3.4 opened with three
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

**3.4.2 — Material inspector editing.** The editor learns
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

**3.5.1 — Skeleton & GPU skinning: Epic 3.5 is OPEN, and this task is MERGED as PR #80 (merge commit
`c3a2bc7`, fifteen commits) and PENDING its macOS validation pass.** CI-green on all three lanes with
the green run's `headSha` asserted equal to `HEAD` before the merge. **Its Windows and Linux lanes are
the retirement of V2**: the tree's first integer (`uint4`) vertex attribute and first two-UBO vertex
stage compiled through DXC to DXIL and SPIR-V and DREW under WARP and lavapipe on their first
exposure, so the recorded `Float4` fallback was never applied. The `cook-determinism` job reports
`30 byte comparisons agreed: macOS == Windows == Linux, byte for byte` over the 15-line manifest, with
`ktx validate`'s 8 unchanged — the new format is cross-lane anchored from day one. The renderer
learns what a rig is: the import layer has carried complete skeletal data since 3.2.1 and `.aeromesh`
has carried `Joints0`/`Weights0` semantics since 3.3.1, but **nothing downstream could hold a
skeleton** — no container, no runtime type, no way to get a cooked mesh onto a GPU at all, and no
vertex shader that had ever seen a matrix palette. Four firsts: the **first `.aeroskel`** (the tree's
second first-party binary format, designed as a *sibling* of `.aeromesh` rather than a region inside
it, so `.aeromesh` is byte-untouched — no `formatVersion` bump, no golden churn; **`docs/09` gains a
normative §12 and Reserved renumbers §12 → §13**, which task 3.5.2 then filled with `.aeroanim` and
pushed to **§14** — so **§14 is the current Reserved section**); the **first mesh
registry** (before this, `ForwardRenderer` could draw only its three built-in procedural primitives);
the **first integer vertex attribute** anywhere here (`rhi::VertexFormat::Uint4`) and the **first
two-UBO vertex stage**; and **pipelines 2 → 4** from three shader loads.

Its load-bearing design choice is an **ordering invariant that makes topology a byte-layout property**:
records are parents-before-children (ties by ascending `sourceNodeLocalId`), the parser enforces it as
the one-line `parent < index`, so **a cycle is unrepresentable rather than detected** and every
consumer walks the records in a **single forward pass with no recursion and no visited set** — *do not
add one*. The producer reaches that order with **Kahn's algorithm over sorted vectors**, so **Kahn
exhaustion IS the cycle detector** and there is no second traversal to keep in sync. Two more standing
rules from it: **a `.aeroskel` is never empty** (both counts ≥ 1 at parse, because the cook is
per-**skin** and a model with no skin produces no artifact and a CLI error, unlike §9.2's legal empty
mesh file); and the cook does **zero floating-point arithmetic** — every TRS component and every
inverse-bind cell travels `std::bit_cast` bit for bit, and **INV-T4 does not extend here**: that
invariant is about the *texture* files, and mesh and skeleton data are float data moved bit for bit.

**`MAX_SKINNING_JOINTS` is 85, measured rather than guessed, and the measurement is the interesting
part.** SDL's Vulkan backend binds **every** push-uniform descriptor with a fixed 4096-byte range
(`MAX_UBO_SECTION_SIZE` 4096 at `SDL_gpu_vulkan.c:71`, applied at `:5300`, `:5419`, `:8671`), so bytes
past 4096 are memcpy'd into the ring and **never visible to a Vulkan shader**, while D3D12 and Metal
are bounded by the 32 KiB block and show the full data — a **silent cross-backend divergence**, so the
engine adopts the portable ceiling uniformly and every skinned draw pushes the **full zeroed 4080-byte
block**. The **format's** caps are wider on purpose (1024 records / 256 palette slots) and live in
`cooked_skeleton.hpp`, never mixed with the renderer's: **formats outlive renderers**, so a 200-slot
rig is a valid file today's renderer refuses to draw with a latched WARN, not a file the cook should
have refused to write.

**Two AC amendments, both recorded.** **AC-2's include list**: it asked for a core-and-std-only include
list *and* for bytes formed exclusively through the eight `put*`/`get*` primitives — both cannot hold,
because those primitives live in `cooked_mesh.hpp`, so `cooked_skeleton.hpp` includes it with the
identical one-line comment `cooked_texture.hpp:24` has carried since 3.3.2 and **the eight-places rule
wins**. **AC-46 (byte-identity) holds exactly as written**, verified path by path; what was amended is
a plan *assertion* — §V.5 claimed the determinism manifest's diff removes nothing, and the two new
lines made **four statements in its own header false** (the artifact count, the arm list, the
cooker-version list, and the regeneration `ctest -R` regex, which would otherwise have **silently
skipped the new arm**). Every rule sentence and all 13 existing hashes are byte-identical.

**Its sabotage matrix ran all 36 seeds; three reddened nothing and all three were closed structurally
and re-proven by re-seeding.** The generalizable one: **`paletteJointCount > jointCount` ALWAYS leaves
a hole too**, so no fixture anywhere can separate that check from the unclaimed-slot check *by status*
— only the message can, so the wording is pinned. Also: `SK15`'s two-record fixture made a duplicate
slot indistinguishable from an unclaimed one (fixed with a three-record/two-slot arm claiming
`0,1,0`), and **`cookImportedSkeleton` uses `skinIndex` twice** while `KS11` pinned only the header —
so an artifact could be numbered skin 1 while carrying skin 0's rig with status, warning and
`sourceSkinIndex` all correct. **A parameter used twice needs both uses witnessed.** Six of the plan's
own witness attributions were wrong and are corrected in `docs/10`, the sharpest being that **seed S29
does not compile at all** — `mesh_pack.hpp`'s own `static_assert(sizeof(SkinVertex) == 32)` plus four
narrowing errors catch it, which is a stronger witness than any test. **Four declared shader-only seeds
(S33–S36) reddened nothing in the whole 144-entry suite** (with the build log checked for the shader
recompile each time, so the green is not vacuous); **validation rows 4, 5 and 6 are their only coverage
anywhere**, and the page names each row's seeds beside it. **A real defect in that same blind spot was
then found by reading rather than by seeding** (`73052bc`): the skinned VS accumulated the weight sum
**before** its out-of-range joint guard, so a vertex whose every influence named an index past the cap
summed non-zero over a position nothing had contributed to and **collapsed to the origin** — the exact
outcome the Σw = 0 passthrough exists to prevent. Reachable only from a corrupt or hand-built artifact,
and unfindable by any tier here, because nothing in this tree renders a pixel.

**Three things from it that outlive the task.** (1) **`JP12` cannot witness what its title implies** —
its largest joint index is **65535**, exactly what a `u16` holds, so it cannot see a `u16` narrowing in
the repack; defensible today because `ImportedPrimitive::joints` is `u16`, but it is not the
independent width witness it reads as, and the `UShort4` bundle is the day it must gain a larger value.
(2) **The stale-binary trap**: after a seed's `git checkout --`, the built binaries still hold the seed,
so any "clean baseline" measurement must be preceded by a **rebuild**. (3) **`computeJointPalette`'s
scratch cost**, named as a future optimization: the fixed `std::array<Mat4, 1024>` is
default-initialised to identity on every call because `Mat4` carries an identity member-initialiser —
roughly 64 KiB of stores (~3 µs) per skinned draw, invisible for one instance and scaling **per
instance**. The allocation-free fixed array is what the design mandates and every alternative is an
`engine/core` change, so it stays; **this is the first place to look if skinned-instance counts grow.**

**3.5.2 — Clip playback: MERGED as PR #82 (merge commit `5622a77`, seventeen commits), CI-green on all three lanes with `headSha == HEAD` asserted before the merge, and PENDING its twelve-row macOS pass.** A rigged model finally *moves*. The import layer
had carried complete channel data since 3.2.1 and Import Details had displayed it for five tasks, but
**there was no motion anywhere in the tree**: nothing could cook a clip, read one back, evaluate one at
a time, or hold a playback clock. Four firsts: the **first `.aeroanim`** — the tree's **third**
first-party binary format and the second designed as a *sibling*, so `.aeromesh` and `.aeroskel` are
**byte-untouched** (`docs/09` gains a normative **§13** and Reserved renumbers **§13 → §14**); the
**first evaluator in `engine/render`** (which computed poses but had never sampled anything over time);
the **sixth reflected built-in** and the **first with a `bool`**, so `full.scene.json` carries the first
`true`/`false` any committed scene golden has ever held; and the **first sample to build a real
`World`** and drive its picture from a component living in it.

**Three rules the format states normatively and one it refuses to.** (1) **`.aeroanim` has exactly ONE
padding site** — between the times and values regions, 0–12 bytes, present iff `keyCount % 4 != 0` —
and **the parser CHECKS it rather than deriving around it**; the padding formula and the cubic
multiplier each live in **exactly one function**, and a test pins both **against literals**. (2) **The
parser is EXACT on both region offsets and on the padding site**, unlike its permissiveness everywhere
else, because the format has exactly one legal layout and **equality is the only check that can see a
mispositioned padding site at all**. (3) **A `.aeroanim` is never empty** — §12.0's asymmetry inherited
a second time, at parse: the cook is per-**clip**, so a clip whose every channel was dropped produces no
artifact and a CLI error. And the refusal: **sampling is NOT part of the determinism contract** —
`render::sampleAnimation` reaches `sin`, `acos` and `sqrt`, libm differs between three C libraries, so
**§13.7 says normatively that no cross-lane claim is made about a sampled pose and it must never enter
the manifest.** The bytes on disk are deterministic; what a renderer computes from them is not.

**The split between the two layers, stated so nobody has to infer it:** `engine/scene` owns *what time
is it* (`engine::AnimationPlayer` + `advanceAnimationPlayer`), `engine/render` owns *what pose is that*
(`bindAnimation` + `sampleAnimation`). `sampleAnimation` takes a bare `float` and neither knows nor
cares whether the caller looped, and it **writes only the T, R or S member each bound channel drives** —
which is the whole reason §12.3 stores bind locals as TRS, discharged here rather than predicted.
`bindAnimation` **reports, it never refuses**: an unbound channel is a *normal* outcome, since glTF
clips routinely animate camera and mesh nodes alongside joints.

**`AnimationPlayer` carries NO clip reference, and that is a decision rather than an omission** —
recorded in `docs/tasks/phase-3.md` as an amendment. The spelling of a scene → asset reference is
3.1.5's by its own task text; a clip reference *alone* cannot produce a picture, because playing a clip
needs a mesh, a skeleton **and** a clip. **The reversal is one appended field after `playing`**, and
§2.3's missing-key rule keeps every earlier scene file loading. Its "+ inspector control" subtask is
satisfied by the reflection spine — all four fields are `float`/`bool` and already rendered — so a
bespoke panel would be a regression against ADR-004.

**`AERO_BUILTIN_COMPONENT_HEADERS` names the built-in component headers ONCE**, at root scope, in its
own commit **before** the component exists. Four sites generate reflection artifacts from that list and
**three of the four are silently optional**: a component added to the editor's list and not the
serializer's is registered, inspectable, editable and **NOT SAVED**, with every test green. **The site
the variable does not reach is the one it exists to protect, one layer down**:
`engine/scene_serialize/src/scene_serialize.cpp` holds a **hand-written dispatch table** that decides
what is actually *saved*, while the CMake list decides only what is *compiled in*. Both had to move —
**AC-49 is amended**, and `engine/scene_serialize/include/` alone is byte-identical.

**Its 47-seed matrix ran in two halves — eighteen at their own build step, twenty-nine as one pass
afterwards — with ONE genuine gap, closed structurally and re-proven by re-seeding.** `S37` (delete the
clock's `!playing` guard) left the **entire 859-case suite green**, because `PL2` advanced a paused
player by `±1000.0` against a `DURATION` of `2.0` — **whole multiples**, so the seeded clock's `fmod`
came straight back to the bit-identical `0.375` the case asserts. Closed by adding a **non-multiple**
pair (`±0.75`); re-seeded, `PL2` reddens alone. **The generalizable form: a "no change" assertion whose
delta is a whole multiple of the modulus proves nothing about the modulus.** Two plan wordings were
wrong and both seeds were re-run in corrected form rather than recorded as passes (`S16a`'s
input-order fold is a **provable** no-op; `S41a` moves the block below `runSkeleton`, which is still
**above** the pinned region), and `S40b` was added beyond the plan because the manifest arm carries two
independent literals and seeding both together cannot show the second is witnessed at all.

**Three of its own correctness claims turned out to be structurally unwitnessable, and all three are
recorded rather than smoothed over.** **`S47`: the sampler's `u` clamp is UNREACHABLE, not merely
untriggered** — `locate()`'s own search establishes `u ∈ [0, 1)` for **any** times array, monotonic or
not, so the plan's extrapolation justification is refuted by the code it justifies. The clamp **ships**
(one instruction, load-bearing the moment the control flow changes) with its comment restated as the
invariant. **`S33` is a behavioural no-op as written** — the guard's `joint >= pose.size()` disjunct
**subsumes** the INVALID test, since `pose.size()` can never reach 2³²−1 — so the stronger form (the
whole guard) is what ran. And **`S23`'s witness is `CL23`, not `CL7`**: with the `u` clamp present,
dropping the low clamp is a no-op for every *finite* time below the range, and **its only observable
role is NaN**. Also corrected: **`S39` is a LINK failure, not a compile failure** (the src-private
`builtin_serializers.hpp` *declares* the generated functions, so only the definition is missing);
**`S45` reddens seven cases, not four**; and **§R.8 undercounted its own survey — nine literal sites
named, EIGHTEEN exist, in FIVE test files rather than four**, including `tests/transform_test.cpp`,
which it does not mention at all. **A component-count literal is not confined to the tests that are
about components** — a test that merely seeds a `World` and counts types carries one too.

**Three declared sample-only seeds (S42, S43, S44) reddened nothing in the whole 154-entry suite, by
design**, and **validation rows 4, 6 and 7 are their only coverage anywhere**. The class is smaller than
3.5.1's because this task writes **no shader** — and, unlike 3.5.1, **CI covers none of it and needs to
cover none of it**, since no new pipeline, vertex format or GPU-tier case is exercised.

**Three deviations, all recorded.** `CL5`'s mismatched-span arms are behind **`#if defined(NDEBUG)`**,
because §D-4 makes the span equality a **debug assert** while §T.3 asks for mismatched calls — both
cannot hold in a Debug build, the assert is normative code and was kept, and the two arms run on
`macos-release` and all three CI Release lanes. **`engine/scene_serialize/src/builtin_serializers.hpp`
is modified and no §F entry names it** — the component headers and the generated declarations live in
that src-private header, which is exactly what makes `S39` a link rather than a compile failure. And
**`.github/workflows/ci.yml` is not byte-identical**, resolving a contradiction the spec had with
itself (D1 said `.github/` was byte-identical; AC-46 required the printed total to move 30 → 36).
`.github/scripts/` **is** byte-identical.

**One named, unowned defect it found and deliberately did not fix (D13).** `aero_cooker animation`
offers **no `--scale`**, because all four importers apply `ImportSettings::scale` in exactly three
places — root node translations, mesh positions and inverse-bind translation columns — and to **no
animation channel anywhere**, so the flag would change no byte and **a flag that lies is worse than one
that is absent**. The deeper gap: **the scale scheme is already incoherent for a multi-joint skinned
hierarchy at `scale != 1`**, because a joint's global transform is a product of *unscaled* bind locals,
so making the flag work for clips alone would make it *look* correct while the rig stayed wrong. **The
honest fix is upstream in the importers; the trigger is the first task that needs `scale != 1` on a
skinned model.** `docs/09` §13.0 states the consequence normatively: clip values are in the importer's
own output units.

**One more thing that outlives the task: no Full-depth import from any backend can emit a zero-key
channel** (glTF's `validateAccessor` refuses `count == 0`; FBX's append helpers return early), so the
cook's zero-key **drop** path is **test-reachable and production-unreachable** — written down so `KA7`
is never read as live cover. And validation **row 7 has an inherent observability limit**: row 5's
seamless loop forces the clip's last pose to equal its first, so "held at the end" and "snapped back to
t = 0" are **the same picture by construction**. It is still a valid `S44` witness (frozen versus still
cycling is unambiguous, and the held pose is visibly not the bind pose) but it must never be written as
"holds its final pose".

**3.6.3 — Tonemap/gamma pass: COMPLETE IN CODE on `feat/3.6.3-tonemap-gamma-pass`, twelve own commits
plus one merge commit, cut
from `main` @ `64df342`. NOT MERGED; no CI run exists yet; the twelve-row macOS pass
(`editor/validation/3.6.3-tonemap-gamma.md`, written before the pass as always) is PENDING.** Sized S
in the roadmap and landed M, recorded in the spec before implementation. `scene.frag.hlsl` has ended
with `return float4(lit, 1.0); // raw linear` since 3.4.1, and every target that line writes into is
read as **sRGB-encoded** — SDL's own header says so (`SDL_gpu.h:1349`) for the SDR composition this
engine hardcodes at both of its call sites, with no composition knob on `SwapchainDesc` to change it.
Nothing converted, so **the picture has been uniformly too dark by roughly the difference between
46/255 and 118/255 at glTF's middle grey** — provably wrong rather than suspected. Two new
`engine/render` pairs plus one src-private packer, two shaders, one sample, and a two-line wiring
change in each editor GPU consumer. **`forward_renderer.{hpp,cpp}`, `lighting.hpp` and all of
`engine/scene_render` are byte-identical**, which is the point: the forward pass already wrote linear
values and already built its pipelines from a caller-supplied format, so it simply gets a target that
can hold them.

**Four rules from it that outlive the task.** (1) **The OETF is UNCONDITIONAL, and `None` means no
CURVE** — there is deliberately no `applyOetf`, `gammaEnabled` or `linearOutput` flag anywhere,
because a flag that can be set wrong is a way to ship the exact defect this removes; the only escape
hatch is at the CALL SITE (`--raw` creates no `PostProcess` at all). **A third grep joins the tree's
"not literally zero, must be READ" list**: `applyOetf|gammaEnabled|linearOutput|skipEncode` matches
two `//` comment lines in `tonemap.hpp` stating the prohibition. (2) **The blit is 1:1 BY
CONSTRUCTION**, because both targets are resized on **adjacent lines** from the same value in both
consumers — which is what makes `Filter::Nearest` provably exact and the unrendered margin past
`drawExtent` structurally unreachable, and why the resize lives in the draw walk beside `target`'s.
That is **not** the `I96` violation it resembles: `I96` is about ordering against ImGui's CONSUMPTION,
and **ImGui never sees the HDR texture**. (3) **The Narkowicz ACES fit is NOT near-identity in the
shadows** (origin slope `0.03/0.14 ≈ 0.2143`, so a linear 0.02 maps to ≈0.0105); `TM20` pins it
against that literal **with the measuring epsilon as part of the assertion** (`1e-6` reads 0.21430;
`1e-4` already reads 0.21599, which would make a 1 % tolerance meaningless). **If a pass judges the
default too dark, the fix is the default EXPOSURE, recorded as an amendment — never a silent constant
edit.** (4) **`renderer.hpp` stays byte-identical**: `PostProcess` never constructs a `Frame`, so it
needs no `friend` — `beginScene`/`endScene` forward to the owned `RenderTarget` and `resolve` reads
only the three public accessors.

**The plan's own expected-output table was WRONG IN EIGHT OF ELEVEN ROWS, and the sample prints
measured values instead.** The sharpest correction is in a pinned row: `linearToSrgbEncode(0.21404)`
is **0.4999987**, not the 0.5000085 the plan derived, so `255 x that` is 127.49968 and the reading is
**127, not 128**. The headline pair is **~55 (`--raw`) against ~127 (`--tonemap=none`)** at the patch
that encodes to sRGB one half, and **~46 against ~118** at glTF's middle grey. Both anchor rows the
tests actually pin (0.18 and 1.0) were already right. **The sample computes its table at startup from
`render::tonemapAndEncode` itself**, which is exactly why it is printed rather than documented — a
validation pass compares a screen reading against a number that build produced.

**Two more arithmetic claims were refuted by running them.** `linearToSrgbEncode(1.0F)` is **not**
exactly 1.0 in fp32 (`1.055F - 0.055F` rounds to 0.99999994, one ULP below), so `TM9` asserts the
error's direction and magnitude instead — an OETF that OVERSHOT would hand the 8-bit conversion a
value outside the `[0,1]` domain, which is the property worth pinning. And **Reinhard's asymptote IS
reachable in fp32**, because `1.0F + x == x` once `x >= 2^24`, so `f(1e30F)` is exactly 1.0; `TM14`
pins the strict inequality at the HDR buffer's own ceiling (`65504/65505 = 0.99998474`).

**The `toString` trap fired for real, on `rhi::TextureFormat` rather than on this task's own enum.**
`CHECK(cfg.sceneColorFormat == rhi::TextureFormat::RGBA16Float)` is a hard compile error on every
lane; the fix is `render_target_test.cpp`'s idiom, **double parentheses**. Comparisons of
`TonemapOperator` need none, which is itself a small proof that AC-2 holds — and the plan's AC-2 grep
is restated, because `git grep 'toString('` over `engine/render/` matches four pre-existing QUALIFIED
calls to `engine::assets::toString` in `texture_upload.cpp` plus one prose comment, and cannot exit 1.

**Its 31-seed matrix ran to completion (38 seeded builds) with THREE genuine gaps, all closed
structurally and all re-proven by re-seeding.** (a) **A source-text "A occurs after B" pin over a
chain of arms proves nothing about which arm A is in**: `T6`'s swap left `TM29`'s reinhard clause
GREEN, because the call moved into the second arm is still textually after the first comparison; the
clause now pins it to the span BETWEEN the two comparisons. (b) **A dropped `Frame` still draws** —
`~Frame` force-ends and submits it with one WARN, so `T23` left the whole tier green while emitting 19
of those WARNs; closed by asserting in `PP10` that `endScene` FORWARDS its `RenderTarget`'s answer,
which on a moved-from pass is the one reachable `false`. (c) **A leaked GPU shader is invisible to
every tier here** — `~Device` RELEASES it and merely WARNs, so ASan sees no process leak; `T25` left
the tier green while `~Device` reported `releasing 1 leaked shader(s)`. Closed BOTH ways: the two
shader handles are now **scope-owned**, so there is no destroy line on any exit from `create()` and
forgetting one is unspellable (the 3.6.1 `C19` shape), and `PP4` captures the log across `~Device`
through the `setLogCallback` seam and asserts no leak line.

**Nine plan witness attributions were corrected by measurement**, the useful ones being: `TM10`
survives `T2` (0.21404 is above the decode threshold and takes the power arm unchanged) while `TM11`
reddens instead (the swap makes the OETF DISCONTINUOUS); `TM9` survives `T4` exactly as flagged;
`TM15` reddens under `T5` because it asserts SATURATION, not only monotonicity; `TM23`'s per-axis
mixed arms redden under `T13`; `T10`'s weak form is a behavioural NO-OP and the strong form reddens
`TM13` alone; and **`T21` does not join the declared class — it CRASHES `PP12`(b) with SIGABRT**,
because Metal's debug layer aborts on a depth-format mismatch. **This tree cannot READ a backend
error, but it can be KILLED by one.** Also measured, answering the plan's open question: **libc++'s
`std::clamp(NaN, lo, hi)` returns NaN**, not `lo`, which is exactly why the explicit NaN arm exists.

**The declared class is NINE seeds — the largest in the project so far** (3.5.1 declared four, 3.5.2
three, 3.6.1 three), because **nothing in this tree renders a pixel** and this task's whole deliverable
is a picture. Two of the nine are predicted to look IDENTICAL to correct code and that is written down
rather than discovered during the pass: **`T24` is derivably inert** (the clear colour and
`scene.frag.hlsl` both write alpha 1, so sampling the alpha produces the same picture — correcting the
spec's "the viewport becomes translucent" claim), and **`T15`/`T16` are numerically identical under
the 1:1 invariant**, so row 4 witnesses the pair `{filter, address mode}` failing TOGETHER.

**One process finding worth carrying: the four new editor cases first landed inside 3.1.5's
file-level `#if AERO_SHADER_TOOLS_ENABLED` block at the end of `imgui_layer_test.cpp`.** Everything was
green and `I105`'s tools-OFF arm — the only place AC-16's degradation path is tested at all — **never
ran**. **The measurement that caught it was counting cases in the reduced configuration, not running
the suite**: a suite that omits a case passes just as loudly as one that runs it.

**What 3.6.2 inherits, named so it is not discovered by a red check**: the editor's scene and preview
targets are now `RGBA16Float`, and **both output targets are depth-free** — a shadow pass wanting the
viewport's depth takes it from `post->sceneDepthFormat()`.

**A SECOND code-review round found five more, none blocking, and the two sharpest are about tests
lying rather than about code.** (1) **`post->resize()`'s `false` was discarded on the theory that
`resolve()` would log once and the picture would fall back to the clear colour — and `resolve()`
CANNOT BE REACHED on that path**: both consumers return at their own `if (!sceneFrame)`, above the
resolve. Three consequences at once — the one latched diagnostic for a not-renderable pass is dead;
`RenderTarget::resize` zeroes its allocation extent before `allocate()` can fail, so `allocate()`
re-runs and re-emits its ERROR **once per frame forever**; and the 4 B/texel output target can still
succeed where the 8 B/texel HDR pair failed, so `ImGui::Image` samples **undefined content**, not a
clear colour. Both results are now captured and either failure latches, still on adjacent lines.
(2) **The `T23` closure did not bite**: asserting `endScene` returns `false` on a *moved-from* pass
pins only the `scene &&` null-guard, and the minimal seed `return scene.has_value();` left all 42
cases green. The arm that bites hands a **live** pass an **already-consumed** frame, which
`RenderTarget::endFrame` rejects. (3) AC-17's UI half was unwitnessed — `I105` drives only the seam,
so deleting the sanitize from `drawViewOptions` stayed green while a Ctrl+Click-typed value would
reach the uniform unsanitized. (4) **AC-4 IS AMENDED**: `tonemapAndEncode(NaN)` returns NaN, and the
"total" claim is narrowed rather than the behaviour changed — a CPU-side NaN mapping would make it a
DIFFERENT function from the HLSL it defines, which is what `TM29` exists to prevent; `TM9` now
asserts the propagation and the sample's two `std::lround` sites became one finiteness-guarded
helper. (5) A **real UX defect this task widened**: the overlay row is submitted after `updatePick`,
so clicking the combo or dragging the exposure slider **armed a scene pick and changed the selection**
— fixed at the shared cause with one `ImGui::IsAnyItemActive()` disarm after the strip, which covers
the gizmo bar's buttons too (same defect since 2.3.3). All four test fixes were **proven by
re-seeding**; validation row 2b is the behavioural cover for the last one.

**3.1.5 — Drag-into-scene: Epic 3.1 CLOSES with it. MERGED as PR #81 (merge commit `5a1bc69`,
twenty-seven commits, all six checks green with `headSha == HEAD` asserted before the merge)
and macOS-validated ✅ PASS on ALL 16 ROWS (2026-08-21).** This is where an asset stops
being a row in a browser. Everything upstream existed — GUIDs, an import cache, a browser, five
importers, two cooks, materials, and 3.5.1's working `createMesh`/draw path — and **nothing in a scene
file could name any of it**. Sized **S** in the phase plan and landed at **L**, recorded as such in
`docs/tasks/phase-3.md`: the referencing field cannot land alone. Six layers moved.
**`tools/reflect-gen` + `engine/reflect`**: the reflectable subset grows by exactly **`engine::Guid`** —
its first growth since 2.2.2's `std::string` — as one `classifyField` arm, one `categoryTag` arm and
**one `serialize.{hpp,cpp}` overload pair**, because **neither emitter gains a branch** (overload
resolution routes them). The wire form is the canonical 32-lowercase-hex string, and **nil is a value,
not an omission** (`docs/09` §2.3), a deliberate divergence from §11.1 where absence has its own
spelling. **`engine/scene`**: `MeshRenderer` gains `mesh`/`meshIndex`/`material`, **appended** because
declaration order is JSON key order and inspector row order; `sizeof` 16 → **56**, with four padding
bytes **stated rather than removed**. **`engine/scene_render`**: `AssetBindingTable` (two sorted
vectors, never a hash container — MSVC's node containers are not nothrow-movable and this becomes a
`SceneRenderer` member whose move is `noexcept = default`) plus three emission arms in
`buildRenderView`, reached through a **fifth, defaulted, last** parameter so every prior caller is
untouched. **`engine/render`**: two appended `RenderView` counts, **deliberately NOT latched WARNs** —
unresolved is transient by design, so a WARN would fire once per session on correct behaviour.
**`/editor`**: **seven new pairs** — `asset_drag` (pure, the payload and the whole routing matrix),
`instantiate_plan` (pure, another named `localId` consumer — the **sixth** once this branch lands,
since 3.5.2's `animation_cook_source` took the fifth seat while this branch was in flight),
`asset_commands` (the sixth structural command and the first that creates more than one entity),
`scene_asset_ledger` (pure — decides, never executes), `material_from_import` (pure, the lossless
direction `material_format.hpp` predicted), and the two src-private ones, `scene_asset_loader` and
`texture_load`. **And `LOCAL_MESH_HALF_EXTENT` is DELETED** with all three consumers moving in one
commit, because `picking.hpp` demands the pick box, the frame box and the highlight box never disagree
and a half-migrated state **is** that disagreement; the plane goes **flat**, retiring 2.3.2's
knowingly-fat box, and **no epsilon is added anywhere** — only a precondition change to `box.valid()`.

**Four rules from it that outlive the task.** (1) **THE PEEK RULE**: ImGui draws the drop highlight as a
**side effect** of `AcceptDragDropPayload`, so every target peeks with `GetDragDropPayload()`, decodes,
runs `classifyAssetDrop` and only then accepts — otherwise the editor makes a visible promise it
immediately breaks. The matrix is a `switch (kind)` around a `switch (surface)`, **both without
`default:`**. (2) **THE LEDGER'S DEFERRED DESTROY IS ONE LINE**: `service()` returns the **previous**
pass's destroy list **first**, before any step of this pass can add to it, so a whole service pass
separates "the table stopped naming this handle" from "the GPU object dies"; `pendingDestroy` is a
**member** and the deferral cannot be rebuilt from the retire list. Its test must be **one sequence
case** — two independent cases both pass under the very defect they exist to catch. (3) **The viewport's
target is `BeginDragDropTargetCustom`**, because `ImGui::Image` submits with **id 0**; the custom form
consults no item state and `IM_ASSERT`s a non-zero id. (4) **A one-shot a panel folds into a frame copy
is consumed at the TOP of `onDraw`, before every early return** — `MaterialPanel` consumed its pending
slot drop at the fold point, so a drop on an **untargeted** panel survived indefinitely and bound a slot
on whatever material was selected next.

**Its 37-seed matrix ran in two halves and NOT ONE SEED REDDENED NOTHING** — nineteen during
implementation while the code was fresh, nineteen as an independent pass afterwards. **Six amendments
and corrections are recorded in `docs/10` rather than smoothed over**, the sharpest three being: the
spec's D5 refusal predicate would **never fire for the three formats it was written for**
(`.obj`/`.ply`/`.stl` produce zero nodes **and zero meshes** at Structure depth — measured, and `S37`
exists to witness it); the plan's `S2` witness is **structurally blind**, because all six guids in
`samples/phase-1-scene/scene.json` are nil and `toupper('0') == '0'` — the real witnesses are `SJ3`,
`G2`, `G4` and `GD3`, and `G2` works only because `full.scene.json` carries a **non-nil** guid; and
**`S17` does not compile at all** (`ByGuid` is the partitioning comparator, so `upper_bound` reverses
its arguments), which makes the wrong lookup **unspellable** rather than merely tested. **The `I96`
failure mode was caught in the act while writing this task's own pins**: `->Data` is a prefix of
`->DataSize`, so the plan's grep false-positives on a correct tree, and the obvious `(^|[^a-zA-Z0-9_])`
correction is **worse — it matches nothing at all**, reporting "empty" even on the seeded tree. The
working form is `git grep -nE -- '->Data([^a-zA-Z0-9_]|$)'`, verified non-vacuous in **both**
directions. **Two defects were found by reading and by tests rather than by seeding**: the
`MaterialPanel` drop above, and `MF15`'s first draft, which exercised **one** omission arm five times
and stayed **green** under `S25` until rewritten to drive all four — the `SN8` failure mode, one epic
later. **Five seeds (N1–N5) have no automated witness anywhere** and their only coverage is validation
rows 3, 4, 9 and 10 — *in principle rather than in fact* until the pass runs.

**Its code-review round found five gaps, two of them blocking, and all five are closed** — and they
share a through-line worth carrying everywhere: **each was a place where a counter or a case observed
the INTENTION rather than the EFFECT.** `sceneAssetDevice` was declared and read but **never assigned**,
so every retired slot texture leaked while the destroy *counter* still climbed; a dropped model was
imported, cooked and uploaded **twice** with the first handles orphaned (reachable for
`.blend`/`.obj`/`.ply`/`.stl`, and invisible to the `.gltf`-based cases); a failed reload left the
binding table naming **destroyed** handles while `unresolvedMeshes` under-reported; two material drops
could merge into one undo entry; and the one-per-pass budget could be starved by an `Absent` entry
whose record vanished.

**And Windows caught a real transitive-include break neither of the other two lanes can see**:
`editor_app.cpp` used `std::sort`/`std::unique` on the referenced-guid collection with **no
`#include <algorithm>`** — libc++ and libstdc++ both supply it transitively here, MSVC's STL does not,
so the Windows lane failed with `C2039 'sort': is not a member of 'std'`. It stayed hidden through the first CI run
because a Chocolatey 504 killed that job during setup **before it compiled anything**, so **an
infrastructure failure was masking a real one**: a lane that dies in setup is not a lane that passed,
and a red-for-infrastructure job must be re-run rather than read as noise.

**Named handoffs, each with an owner.** **Scene-side asset references → DONE at 3.1.5**: a scene file
can now name a mesh (`mesh` + `meshIndex`) and a material, the spelling is a reflectable
`engine::Guid`, and 3.5.1's working mesh path — `createMesh`, `MeshInstance::mesh`/`submesh`,
per-section byte-offset draws and a registry with generational staleness — is what a dragged-in model
now lands on. **Animation clips → DONE at 3.5.2**: the seam 3.5.1 wrote down was used exactly as
written — `render::JointPose` is the sampler's output type (which is why the format stores bind
**locals** as TRS, a clip driving T, R and S member-wise) and `sourceNodeLocalId` is the clip→joint
binding key — and `.aeroanim` is now `docs/09`'s **normative §13** rather than a reserved name.
**The clip reference on `AnimationPlayer` is the one half of that pair still missing, and it is now
unowned**: 3.1.5 shipped the *spelling* it was waiting for, but the two tasks ran in parallel, so
`AnimationPlayer` still carries `time`/`speed`/`loop`/`playing` and **no clip**. The reversal is
unchanged — **one appended field after `playing`** — and a `World`-wide
`advanceAnimationPlayers(World&, dt)` sweep becomes correct and trivial the moment an entity can name a
clip. **A drop target on an inspector `Guid` field, and the `AERO_ASSET(kind)` annotation it needs →
also unowned**: it was named for 3.5.2, which needed no `reflect-gen` growth and did not take it, so
3.1.5's kind-aware drop routes remain the only assignment surface. **`ImportSettings::scale`
coherence on skinned hierarchies → unowned**, with its trigger and evidence recorded in `docs/10`
(3.5.2's D13); the honest fix is upstream in the importers, never a cooker flag. **Animation events, a
`finished` observable, auto-stop at the end, blend trees, state machines, transitions, cross-fade and
additive layers → the v2 animation graph**, which the epic's own goal line already names. **A
monotonic-playback cursor cache → whoever profiles a clip-heavy scene**; it needs per-instance mutable
state, which is the animation-instance type v2 introduces. **Morph targets and an
`AnimationPath::Weights` code → the first task with a morph consumer** (adding one later is additive;
shipping a code nothing produces is a lie no `switch` can catch). **Clip compression, quantization,
sparse keys and per-path value packing → one bundled `formatVersion` bump, never alone**; the uniform
16-byte stride's documented 25 % cost on the two three-component paths is that bundle's evidence. **A
multi-clip container and clip names → the `.pak` work.** **A Tracy zone on the sampler or the clock →
the first task that wants clip timings**, which is adding the first one rather than restoring a lost
one. **A second, differently-named built-in-header variable → the first component that should be
REGISTERED but not SERIALIZED** — never a fork of `AERO_BUILTIN_COMPONENT_HEADERS` in place. **The
narrowed node-hierarchy residual** (a cooked model/prefab container for a consumer with **no
importer**) → **4.4.4 / Phase 5's pak**. **Sub-asset identity beyond an index** (3.2.1's D13) → still
open with its original trigger: the reference is `(guid, position)`, so a re-export that **reorders**
meshes silently retargets it, and a content-hash encoding is reachable for whoever needs cross-reorder
stability. **Ledger eviction and a `Library/cooked/` store** → unowned; nothing is evicted today and
every dropped model stays resident for the session, which is why 3.1.5's validation rows 12–13 are the
only numbers anyone will have. **Per-triangle picking** → unowned; 3.1.5's box pass is written to be
its broadphase. **Extracting imported materials to disk** → unowned: 3.1.5 materializes them **in
memory** through the normative `MaterialDocument` type, so no `.aeromat` appears when a model is
dropped. **The storage-buffer palette unlock** (raising
85) → whoever hits that wall: it needs an rhi surface change, since `BufferUsage` has `Vertex` and
`Index` only, and a fingered humanoid fits inside 85. **`TexCoord1`/`Color0` seats in `MeshVertex`**
(they decode and drop with one latched WARN today) → the first feature needing a second UV set or
vertex colours. **The `UShort4` / `.aeromesh` v2 bundle** → a deliberate bundle, never alone, because
it is a `formatVersion` bump: `Joints0` is `Uint4` only because `rhi::VertexFormat` has no
unsigned-16×4 enumerator, so a skinned vertex pays 8 bytes per vertex it does not need.
Rendered material thumbnails → a named deferral, **unowned** (it needs preview readback plus ledger
integration, which is why `isThumbnailDecodable` was deliberately not touched). **BLEND transparency**
→ a named **decision-waiting** gap, renderer-only, since the format already carries everything.
**IBL / environment lighting** → a named **unowned** gap blocked on a format decision, not on shader
work: it needs a cubemap, which `docs/09` §10 currently refuses. Shadows → **3.6.2**; tonemap/gamma →
**3.6.3** (output is raw linear until then). A shared token→`SamplerDesc` helper → decided by the
**second** consumer; 3.4.2's `material_edit.hpp` is now the second, and it kept the mapping editor-side
because the sample and the editor are the only two callers. **`reflect-gen` growth (`Vec4`, enums,
optional-wrapped nested structs)** → the **component** tasks that need it, never
a panel — 3.5.2 needed none of it, because `AnimationPlayer`'s four fields are `float`/`bool` and v1's
two looping behaviours are spelled completely by a `bool`: 3.4.2's D1 refused it because a generated serializer for `MaterialDocument` would be a
**second writer for a normative on-disk format**, and that reason does not age away even after the
subset grows — **`Guid` landing at 3.1.5 is the pattern to copy** (one `classifyField` arm, one
`categoryTag` arm, one `serialize` overload pair, and no emitter branch). Two small ones from 3.4.2,
both still open: `model_import_session.cpp`'s `sourceHashUsable` is now a
duplicate of `assetContentHashUsable` and should be collapsed by whoever next edits that file, and
`RenderTarget::resize() == false` is unreachable from any tier without an injectable allocation
failure — an engine change nobody has needed yet.

**Carried-forward debt, and 3.5.1, 3.5.2 and 3.1.5 all add to it.** Seven ticked validation rows across four
tasks were signed off with their measurement blanks empty (3.2.5 rows 3, 8, 9, 11, 13; 3.2.2 row 9;
3.2.4 row 12) — each row's *behaviour* passed, each row's *evidence* is absent, so **R4 and R8's
in-editor half stay unmeasured** and D9's centimetre-versus-metre comparison has no recorded figures.
**None of Epic 3.3's or Epic 3.4's five tasks is among those seven** — all five had their
number-bearing rows written so a blank tick is impossible, which is the pattern the other four should
be brought up to rather than the exception, and 3.5.1's and 3.5.2's twelve-row pages and 3.1.5's
sixteen-row page are all written the same way (3.5.1's rows 8–12, 3.5.2's rows 2, 6 and 9–12, and
3.1.5's rows 5 and 11–13 each carry their blank in bold). Separately: **no Windows or Linux validation pass exists for any
of the thirteen Phase 2 tasks, for 3.1.1–3.1.4, for 3.2.1–3.2.5, for 3.3.1–3.3.3, or for 3.4.1/3.4.2**,
and Phase 0's gate is still held open on Windows/Linux 60 fps sign-off. **3.5.1 grows that debt by one
more task, but by less than a full task's worth**, and the difference is worth stating: CI already
covers its sharpest cross-platform half, because `SN3` compiles and draws the skinned pipeline under
**WARP and lavapipe** on every push — which is exactly where the tree's first `uint4` vertex attribute
and first two-UBO vertex stage could have diverged. What the lanes do **not** cover is what a
validation pass is for: the picture. That is platform-validation debt spanning four phases, and with
macOS otherwise green it is the whole of the remaining validation risk. **3.5.2 grows that debt by a
FULL task's worth, and the reason is the mirror image of 3.5.1's**: it adds no shader, no pipeline and
no GPU-tier case, so there is nothing new for a backend to diverge on and **CI covers none of its
picture and needs to cover none of it** — but that also means no lane exercises any part of what its
pass is for. **NEITHER 3.5.1's NOR 3.5.2's macOS pass has been run** — both pages exist, written before
the pass as always. Until they run, 3.5.1's rows 4, 5 and 6 are four declared shader-only seeds'
(S33–S36) only coverage *in principle* rather than in fact, and 3.5.2's rows 4, 6 and 7 are three
declared sample-only seeds' (S42–S44). **3.1.5's sixteen-row pass is NOT YET RUN either, and its debt
is the one CI cannot narrow at all**: the three lanes compile and run every tier-0 and GPU case it
ships, but **no lane performs a mouse gesture**, so its five declared seeds (N1–N5) are as uncovered on
Windows and Linux as on macOS until a pass runs. **The one seed still uncovered by any pass anywhere is
3.4.2's S26**, which no Apple platform can observe.

**3.6.1 — Frustum culling: Epic 3.6 OPENS with it. MERGED as PR #84 (merge commit `40d83a6`,
eleven commits, three of them closing and recording the code-review round), CI-green on all six
checks with `headSha == HEAD` asserted before the merge, and macOS-validated ✅ **PASS on ALL 10 ROWS** (2026-08-22) with **every measurement blank filled**. The default grid draws 670/1000 at yaw 0 and **1** at yaw 180, whole-run record **0.183 ms against 0.714 ms** for `--no-cull` (**−74.4 %**); the 10648-instance grid **1.460 vs 4.115 ms** (**−64.5 %**); the grid holds a vsync-paced **60.0 fps**. **The three declared seeds are all witnessed**: C22 by two clean consoles (0 unexpected lines across four runs, and the editor's own status line reading `warn 0 | err 0 | crit 0`), C28 by the timing A/B, and **C30 by a real `tracy-capture` 0.13.1 session** — the exact pinned version, so the protocol matches — whose `-u -p` export shows both plots present, `drawn + culled == 1000` at every sample, and the labels correct (`drawn 1 / culled 999` facing away, `drawn 669 / culled 331` facing in). **Culling removes nothing visible, proven rather than watched**: 18 matched-yaw A/B pairs with **nine byte-identical** and a mean difference of 0.096/255, while the culled run drew as few as 1 instance against 1000. **The mirror behaves exactly like its twin** — red peaks at 4072 px at yaw 284°, green at 4073 px at yaw 76°, and 284 = 360 − 76 exactly. A degenerate projection draws all 1000 every frame and warns **exactly once** across 2066 frames. **One gap was found in the page itself and is recorded there**: at the default N=10 the mirrored pair is **never inside the frustum at any yaw** (48.0° and 53.4° below the axis against a 30° half-FOV), so **row 6 must be run at a small grid** — the Windows and Linux runs need `N=2`..** The renderer learns to not draw. `.aeromesh` has carried `CookedBounds` per submesh and
per model since 3.3.1 — written, validated, round-tripped and read by **nothing** for three epics — and
`ForwardRenderer::draw` issued a `drawIndexed` for every instance in the view. The whole task is one
new PURE pair (`engine/render/culling.{hpp,cpp}` — no rhi type, no `Device`, no logging, no
allocation, so 24 of its 34 cases run in every configuration) plus about forty lines inside the draw
loop, and it is the FIRST consumer of the cooked bounds field.

**The one load-bearing line is that the NEAR plane is `r2` ALONE.** Gribb-Hartmann is normally written
for GL's `-w <= z <= w` volume, where near is `r3 + r2`; ADR-005 and SDL_GPU put clip Z in `[0, 1]`, so
the near condition is `z >= 0`, which is `r2` by itself. For `perspective(60°, 1, 0.1, 100)` the
correct normalised near plane is `d = -0.1` and the GL form gives `d ≈ -0.05002` — the near plane at
HALF its distance, quietly clipping geometry in front of the camera. **Normalisation divides `d` too**:
dividing only the normal shifts the plane by one part in a thousand, which `FC5` catches with a 0.98 %
margin and `FC7`'s five-micron boundary box catches independently. **`transformAabb` is Arvo in WORLD
space and the `std::abs` is load-bearing** — without it a mirrored box comes out with a negative
half-extent, so `min` lands above `max`, the box is invalid, and an on-screen mirrored object vanishes.

**Four rules from it that outlive the task.** (1) **Bounds are FOLDED for primitives and COPIED for
cooked meshes, and never restated** — `create()` folds each built-in's box over the vertices
`make{Cube,Sphere,Plane}()` actually returned (accumulator-first, the `mesh_cook.cpp` `expandBox`
rule), `createMesh` copies `CookedSubmesh::bounds` verbatim on the `materialIndex` posture, and there
is **no constant table of extents anywhere in `forward_renderer.{hpp,cpp}`** because `primitives.cpp`
is the single source for what each shape is. (2) **`instanceBounds` is SILENT on every path** and
`nullopt` means "cannot prove anything", never an error report — a `nullopt` instance falls through
undecided and uncounted into ARM 2/3/4, whose latched WARNs it must never consume. (3)
**`lastFrameDrawn`/`lastFrameCulled` are per-frame, reset BEFORE the `!hasCamera` early return, and
NEED NOT SUM** — `++lastDrawn` lives at the two `drawIndexed` sites and nowhere else, which makes the
gap true by construction for all four of its residents; `CD5` pins the reset as ONE sequence, because
two independent cases both pass under the defect. (4) **The cull sits BEFORE material resolution**, and
`materialBindCount()` exists solely to prove it: `pipelineBindCount()` is structurally BLIND, because
`bindPipelineFor` is called inside the draw arms, downstream of both candidate placements.

**Its 30-seed matrix ran to completion with ONE genuine gap, closed structurally and re-proven by
re-seeding.** `draw()`'s cull condition read `!local->valid() || !isVisible(...)`; deleting the first
disjunct left the whole 34-case tier GREEN, because `transformAabb` propagates an invalid box unchanged
and `isVisible` already returns false for one — the two clauses can never disagree. The duplicate is
**deleted**, `isVisible` owns the decision alone, and re-seeding `C16` now reddens `FC14` **and** `CD9`,
which the plan's own witness table always claimed and could not previously deliver.

**One plan claim was refuted BY MEASUREMENT, and the source comments stating it were corrected.** The
plan (and the spec's D4) said a mirrored instance witnesses a per-instance frustum extraction, "because
a negative determinant flips every half-space". It does not: measured two ways — a standalone probe
linking the real `culling.cpp` over 30 model matrices including 10 mirrored ones, and seed `C29b`
against the whole tier — the pure local-space form gives **bit-identical worst-plane margins** to the
shipped world-space form on every fixture, mirrors included, because Gribb-Hartmann's derivation holds
in whatever space the matrix maps FROM. The design stands on the **cost** argument alone (the frustum is
a per-VIEW quantity). What a per-instance extraction genuinely does not survive is being **half
applied** — extracting from `mvp` while still testing the WORLD box applies the model transform twice —
and `CD1` witnesses that through a cube 60 units down the view axis (slack +5.8 correct, −10.0 seeded).

**Three declared seeds — `C22` (a log line on the `nullopt` path), `C28` (extracting the frustum with
culling off) and `C30` (the two plot names swapped) — were APPLIED, BUILT AND RUN, and redden nothing.**
Their only coverage anywhere is validation rows 1/3, row 5 and row 7. **`C19` is closed BOTH ways**: a
hard-coded `[-0.5, 0.5]³` table cannot be written without reintroducing a `0.5` literal the guard grep
sees (proven in both directions), and it also reddens `CD7`, because the **plane** primitive is flat in
Y and a naive unit box lies about it.

**Its code-review round found three gaps, one BLOCKING, and all three are closed.** The blocking one
is the sharpest thing this task learned: **the plane-normalisation guard applied a NORMALISED-vector
tolerance (`EPSILON`) to RAW extraction coefficients**, and `perspectiveRH_ZO`'s far row is exactly
`(0, 0, zNear/(zFar − zNear), …)`, whose length SHRINKS with the depth ratio — so past a ratio of about
**1e5 a perfectly valid camera** produced an invalid frustum, `draw()` disabled culling **for the whole
view**, and the WARN blamed a projection that was fine. **Reachable, not theoretical**:
`EditorCamera::MIN_NEAR_PLANE` is `1e-3` against a `DEFAULT_FAR` of `1000` (ratio **1e6**), and
`engine::Camera::nearPlane`/`farPlane` carry **no `AERO_RANGE` at all** by decision. The guard now
refuses only `k == 0.0F || !std::isfinite(k)`. **The sentence that licensed it is the spec's own
AC-5/AC-6** — *"no normal shorter than EPSILON"* is wrong for raw coefficients and right only for
normalised ones, which is why `Frustum::valid()`'s identical-looking length test is correct and stays.
`FC25` (a six-range depth sweep) and `CD11` (a `0.1/20000` view that must still cull without latching)
are the new arms, and restoring the old guard reddens **exactly those two**. Two further findings came
out of measuring rather than reasoning: the review's single `~1e-2` relative tolerance for the far
plane's `d` **does not cover** the 1e6-ratio ranges it also named (measured **4.86 %** — inherent
cancellation in `−1 − m[2][2]`, not a defect), so `FC25` carries a per-row measured tolerance; and the
`!std::isfinite(k)` half of the fix has **no automated witness** (seeding it away leaves the tier green,
because `Frustum::valid()` rejects a NaN normal anyway) — it ships because `extractFrustum` is public
and inspectable without `valid()`, and that is recorded rather than claimed as a behavioural win. The
other two gaps: the two Tracy plots were **skipped on the `!hasCamera` early return**, so a
camera-less frame left them holding the previous frame's values while the accessors correctly read 0
(closed with a single `plotCounters` lambda called on both exits, which also keeps the two plot names
at ONE site so seed `C30` stays single-site); and **an unassigned `camera` is a VALID unit-cube
frustum, not "no frustum"** — `hasCamera` defaults true and `view`/`proj` default to identity, so a
hand-built view that never assigns `camera` culls against the world-space box `[−1,1]²×[0,1]` with
`valid()` **true and no WARN**. No caller does it; it is closed with one sentence in the
`cullingEnabled` contract rather than by moving the `hasCamera` default.

**`engine::perspective` asserts its own preconditions** (`aspect > 0`, `zNear > 0`, `zFar > zNear`), so
calling it with `zNear == zFar` or `aspect == 0` **aborts a Debug build** rather than producing a
degenerate matrix — `FC16` builds both as hand-built column literals instead, which is stronger than an
`#if defined(NDEBUG)` gate because it runs in the configuration CI sanitizes. **The two Tracy plots
`render.drawn` and `render.culled` are the first production plots in the tree**; the names are the
contract and live in `docs/10`. **`samples/phase-3-culling`** is the A/B rig: an N×N×N grid seen from a
fixed eye whose look direction yaws a full turn every 12 s, `--no-cull` for the twin run, a red
mirrored cube beside a green twin, and a blue palette-exempt instance that is the only thing left at
yaw 180°. Measured on this machine in Release: culling cuts whole-run CPU record time **71 %** at
N=10 (1000 instances) and **62 %** at N=22 (10648).

**3.6.2 — Directional shadow map: MERGED as PR #85 (merge commit `3ffaadf`, sixteen commits),
CI-green on all six checks — the three platform lanes plus the vcpkg-baseline, lint and
cook-determinism jobs — with `headSha == HEAD` asserted before the merge, its code-review round
CLOSED (eight findings, three blocking, all fixed). Its twelve-row page
(`editor/validation/3.6.2-directional-shadow-map.md`) is WRITTEN and **rows 1 and 10 PASS with their
blanks filled** (2026-08-22): both console gates 0 over 2088 frames, `shadow drawn 6 / culled 0` at
every elevation, and the depth pass costing **0.083 ms** median against a `draw` that is **unchanged at
0.025 ms** either way — so the shadow lookup adds no measurable cost to the main pass at six casters,
and `--no-shadows` measures `renderShadowMap` at **0.000 ms**, confirming the early-out by measurement.
**Row 11 also PASSES with its blanks filled**: a zero-length light direction latches the fit WARN
**exactly once across 2196 frames** and `castsShadows = false` fires it **0 times across 2197** — and
both arms report `shadow drawn 0 / culled 0` every frame, which confirms **AC-33's counter reset
before the early return on hardware** rather than by argument. **Rows 8 and 12 are partly measured**:
all three shadow resolutions allocate exactly as requested with `texelWorldSize` **0.0420 / 0.1678 /
0.3356**, matching the plan's predicted table exactly, and the cost is **flat across a 64× texel
change** (0.112 → 0.121 ms), so at six casters the depth pass is per-draw bound rather than fill
bound; all five existing samples run with **0 unexpected console lines and 0 non-`[info]` lines**.
**Rows 2, 3, 4, 5, 7 and 9 also PASS — 9 of 12 in all — and every one is judged by PIXEL
MEASUREMENT against a matched `--no-shadows` twin rather than by eye**, capturing the sample's own
window in isolation (`CGWindowListCopyWindowInfo` + `screencapture -l<id>`, 3.6.1's method reused).
**Six of the seven declared seeds are witnessed.** The sharpest numbers: at `--distance 5` the frame
is **100.00 % unchanged** against the twin, so an out-of-range lookup resolves to **lit** (SH28
refuted); shadowed pixels retain **~40 %** of their lit brightness with **0 of 1499 near-black**, so
ambient and the point light survive (SH27 refuted); and mean darkening scales **3.2×** from elev 15 to
75 against a predicted sin ratio of **3.73×**, the shadow removing the directional term and nothing
else (SH22/SH23 refuted). The rig's shadow tracks its pose **nearly 1:1** (32.3 px against 34.4 px)
and never drops below 7088 px. **ALL SEVEN declared seeds are witnessed and the page leaves none
uncovered** — SH26 by a **shader** seed (`directionalShadow(worldPos, N)` instead of `geoN`), which is
what that seed actually changes: **1740 px at mean magnitude 62.8** under the static casters at elev
75, and **runs/px 0.385** at elev 15 against a bias change's **0.045**, the run density separating a
boundary that follows the ground's ripple from a solid region that merely moved. **Two findings are
recorded rather than smoothed over.** (1) **Row 6's acne half is NOT reproducible**: `shadowBias = 0`
changes **nothing** on static geometry at either elevation, because the pipeline's slope-scaled
**rasterizer** bias already covers every configuration this sample produces — D6's two-mechanism split
working as designed — so **the row asked for the wrong evidence and was corrected, not the code**.
(2) **`--elevation` freezes the sun but NOT the rig**, whose 3 s cycle contaminates any A/B that does
not exclude its columns; an initial row-6 reading showed a textbook striping signature that was
entirely the rig at a different pose. **Rows 8 and 12 remain partly measured** (a blockiness judgment;
the picture-unchanged A/B and the inspector rows). Measurements through row 12 were taken at
`6046c9e`; row 6's two SH26 arms were captured after 3.6.3 (PR #86) merged, both on the same build. **Row 1's gate as first written could never return 0**: the sample's own
closing line contains the path `editor/validation/…`, so it matched the word it was grepping for; the
page now excludes that line and carries an independent log-level cross-check beside it.** The renderer
learns that light is occluded. Until now every lit surface received the full directional term
regardless of what stood between it and the sun: nothing computed a light-space transform, no depth
texture carried `Sampler` usage, no shader declared a `SamplerComparisonState`, and the RHI **actively
refused** a render pass with no colour attachment. One new PURE pair (`engine/render/shadow.{hpp,cpp}`
— the layer's **third** pure module and the **second** naming no rhi type), `renderShadowMap` on its
**own command buffer**, two depth-only pipelines, three shaders, four appended `DirectionalLight`
fields, and `samples/phase-3-shadows`.

**The load-bearing find, and it cost a step.** The plan's texel snap was a NO-OP and worse than inert:
it snapped `transformPoint(lightView, sphere.center)`, which is identically `(0, 0, -k)` **for every
input** because `lookAt` puts its target on the light's own axis — so there was nothing
camera-dependent to quantise, and `std::floor` turned the sign of ±2e-6 rounding noise into a discrete
**one-full-texel jump**. **Measured over a 360-step yaw sweep at 2048**: a fixed world point's
sub-texel position spanned **0.997 texels** (the crawl the snap exists to remove, entirely unmitigated)
against **0.022** for the corrected form. The fix quantises the centre's position on the light's
**lateral axes** — `dot(s, c)` and `dot(u, c)`, read from the basis rows — by flooring the ortho's
**min corner** onto a world lattice and shifting the bounds by the residue; **the sign is subtract**,
and the width stays exactly `2r` so `texelWorldSize` is untouched. **INV-8 is the rule it earned**: *a
"the value is quantised" assertion is satisfied by a value that is always zero, so every quantisation
claim needs a companion that fails when the quantity is CONSTANT.* The reference fixture moved off the
origin for the same reason — its residues are non-zero **by design**, because a fixture with zero
residues makes every snap assertion vacuous.

**The RHI takes its first validation change since 0.4.2**, recorded per the 0.4.1 D18 protocol against
the pinned SDL 3.4.12 tree. Two predicates widen so zero colour targets is legal, each keeping an
`iff`. **Relaxing the predicate alone would have turned a validated refusal into a crash**:
`beginRenderPass` dereferences `colorSlots[0]` to derive the pass extent, and that array is
zero-initialised. `device.hpp` gains one corrected comment (AC-6 amended — a false sentence on a public
header outlives an acceptance criterion).

**Three classes worth carrying.** (1) **The positional brace-init**: `buildRenderView`'s directional
assignment was positional, so four appended fields compiled clean at their DEFAULTS with every test
green — it is designated now, and **a mirror assignment should name every field**. (2) **A plan that
extracts a helper into an existing function must check the surrounding scope for the name it
chooses** — `resolved` already existed in `draw()`'s loop as a `MaterialHandle`. (3) **SPIRV-Cross
numbers MSL `[[attribute(n)]]` by DECLARATION ORDER, not by the HLSL semantic index**, so the skinned
depth stage declares all six stream inputs and reads three; declaring only `TEXCOORD0/4/5` renumbers
them to 0/1/2 and hands Metal a `Float3` where the shader wants a `uint4`. The static depth VS is fine
only because index 0 coincides.

**Steps 5 and 6 are ONE commit, measured rather than assumed**: with six declared fragment samplers and
nothing binding slot 5 yet, SDL's `Missing fragment sampler binding!` assertion does not merely redden
— it **HANGS** `aero_tests` on Metal.

**A 30-seed matrix ran to completion in 36 runs** (`SH8` is three arms; `SH11`/`SH12` were re-run
against Release because they **abort** a Debug build at `ortho`'s own assert) with **TWO genuine gaps,
both closed structurally and re-proven by re-seeding**. `SH14` (drop the caster box's `valid()` guard)
reddened nothing because **`std::max(0.0F, NaN)` returns `0.0F`** — the invalid sentinel's NaN is
LAUNDERED, so it is the one invalid box that cannot witness the guard; the closure is the **fixture**
(an inverted but FINITE box), not a new case. `SH29` reddened nothing because
`createGraphicsPipeline` returns an invalid handle for **both** the structural and the shader-handle
refusal; `SM15` discriminates with REAL shaders, which is why `aero_tests` lands at **986** rather than
the plan's predicted 985. **Three plan witness attributions were wrong and are recorded** — `SH6`
reddens `SF11`/`SF27` only, `SH11` does **not** redden `SF24` (a near/far swap leaves the centre at the
midpoint, so `ndc.z == 0.5` survives), and `SH19` reddens `SM14` alone. **`SH30` cannot even be
written**: swapping the two appended `GpuLightBlock` fields fails to compile at both `offsetof` lines.
**Seven seeds redden nothing by design** (`SH24`–`SH28`, plus `SH22`/`SH23`'s second half) and each was
APPLIED, BUILT and RUN to prove it, with the shader recompile confirmed in the build log; their only
coverage anywhere is validation rows 2, 3, 4, 5, 6 and 9.

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics (2.1 Editor shell, 2.2 Core panels, 2.3 Manipulation, 2.4 Undo/redo, 2.5 Scene I/O, 2.6 Project system v0) CLOSED in code and macOS-validated PASS, with Windows/Linux rows pending for every task (`editor/VALIDATION.md`). The whole-phase audit (2026-08-02) fixed two silent data-loss paths, a project root that was never made absolute, two CI false-greens, and four stale documentation claims. Full per-task and per-epic history — every defect, every sabotage matrix, every deviation — lives in `docs/10-engineering-log.md`'s Phase 2 entries; this row is deliberately a summary, not a duplicate. |
| **Phase 2 gate** | **MET 2026-08-02.** `samples/phase-2-editor-scene/` holds a project and a 4-entity scene authored entirely through the editor, with the save → New Scene → Open Scene round trip confirmed (`samples/phase-2-editor-scene/VALIDATION.md`); provenance is recorded there rather than asserted, since a hand-written `scene.json` is byte-identical to a real one and no test tier can tell them apart. Deliberately NOT `add_subdirectory`'d — this artifact is data (a provenance proof of the editor), not a compile-proof of engine code. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** **Epic 3.1 is CLOSED — all five tasks merged.** 3.1.1/3.1.2/3.1.3/3.1.4 (PRs #65/#66/#67/#69), CI-green on all three platforms, sabotage-proven (26/31/35/25 seeds), macOS-validated ✅ PASS 14/14, 14/14, 16/16, 10/10 — Windows/Linux rows pending for all four. `engine::Guid`/`engine::ContentHash`, the `.meta` v1 format, `AssetDatabase::rescan`'s eight phases, the machine-local `Library/asset-cache.json` import cache and the real Asset Browser all shipped across them. **3.1.5 (drag-into-scene) closes the epic: MERGED as PR #81 (merge commit `5a1bc69`, twenty-seven commits), CI-green on all three lanes plus the vcpkg-baseline, lint and cook-determinism jobs with `headSha == HEAD` asserted before the merge, and macOS-validated ✅ **PASS on ALL 16 ROWS** (2026-08-21) with every measurement blank filled — the first task since 3.4.2 to close with none left empty. **The headline number is row 12's: a 5.42 MiB rigged FBX (72 source nodes → 73 entities) takes 261 ms from drop to entities and 278 ms to visible geometry, in ONE service tick — a visible hitch, and exactly the cost D8 accepted knowingly under the 3.2.1 D16 posture.** That is the figure R3 said to take before deciding, and it is the motivating number for both named follow-ups (`Library/cooked/` import-once-per-content, and the async drop queue); nothing in this design blocks either. Row 13 reads **57.8 MB resident (peak-RSS delta, meshes only) at a vsync-capped 60.0 fps / 16.67 ms mean tick**, and row 11 reads **1 frame from Apply to registry** against the watcher's 2000 ms settle. **Three caveats are recorded on the page rather than buried**: the resident figure is a peak-RSS proxy because nothing exposes ledger byte sizes and it includes the importer's transient peak (3.3.1 R7's dominant term); the texture half is 0 because that model's textures were not beside it, so a textured session's cost is still untaken; and 60.0 fps is the cap, not a ceiling. Sized S, landed L (recorded in `docs/tasks/phase-3.md`): the reflectable subset grows by `engine::Guid` — its first growth since 2.2.2 — `MeshRenderer` gains `mesh`/`meshIndex`/`material` (`sizeof` 16 → **56**), `engine/scene_render` gains the `AssetBindingTable` and three emission arms, `RenderView` gains two deliberately-unlatched counts, `/editor` gains **seven pairs**, and `LOCAL_MESH_HALF_EXTENT` is **deleted** with the plane going flat. It also **decides** the node-hierarchy gap `docs/09` §9.0 has carried since 3.3.1 — entities, not a container — leaving a narrower residual owned by 4.4.4 / Phase 5's pak, with `engine/assets` byte-identical. Mechanical gate, re-measured on the MERGED tree (never derived by addition): **157/157 on both macOS presets** with `AERO_REQUIRE_GPU=1`; `ctest -N` **157 / 65 / 78** against main's 154 / 65 / 78 — an asymmetry of **+3 / +0 / +0** that is D19's prediction being met — the three new entries are the gated `reflect-gen.guid_*` cases, absent by design from both reduced configurations — while every other new case rides an existing binary; doctest across **seven** binaries (the first task to track `aero_reflect_meta_test` and `aero_reflect_json_test`) **906 / 1725 / 138 / 29 / 27 / 7 / 28**, +193 cases over main; six guards exit 0, and their two scanned counts are **re-measured on the merged tree** rather than carried over (math-boundary **380 → 404**, project-no-delete Check B **66 → 73**, `PERMITTED_DELETERS` unchanged and none of the seven new TUs in it); `.github/scripts/` byte-identical. **Every count above was re-measured on the merged tree**, because the pre-merge figures went stale the moment this branch merged `origin/main` (3.5.2); each delta was then reconciled against main's own recorded baseline rather than assumed. A **37-seed** matrix ran in two halves — nineteen during implementation, nineteen as an independent pass — and **not one seed reddened nothing**; six amendments and corrections are recorded, two defects were found by reading and by tests rather than by seeding, and five seeds (N1–N5) have no automated witness anywhere. A **code-review round found five gaps, two blocking, all closed** — `sceneAssetDevice` was declared and read but **never assigned**, so every retired slot texture leaked while the destroy COUNTER still climbed; a dropped model was imported, cooked and uploaded **twice** with the first handles orphaned, because the drop reports from the reconcile block before any entry exists (reachable for `.blend`/`.obj`/`.ply`/`.stl`, and invisible to the `.gltf`-based cases); a failed reload left the binding table naming **destroyed** handles while `unresolvedMeshes` under-reported; two material drops could merge into one undo entry; and the one-per-pass budget could be starved forever by an `Absent` entry whose record vanished. **The through-line: each is a place where a counter or a case observed the INTENTION rather than the EFFECT.** `LG25`/`LG26` were proven by re-seeding; `DP23` is recorded as **not** a witness for its own gates, because removing them leaves it green. **Windows then caught a real transitive-include break** — `editor_app.cpp` used `std::sort`/`std::unique` with no `#include <algorithm>`, which libc++ and libstdc++ supply transitively and MSVC's STL does not — hidden through the first CI run because a Chocolatey 504 killed that job during setup before it compiled anything, so an infrastructure failure was masking a real one. 3.1.5's own diff leaves `engine/core`, `engine/assets`, `engine/platform`, `engine/rhi`, `engine/scene_serialize`, `runtime/`, `shaders/`, `vcpkg.json`, `cmake/` and `.github/` byte-identical; **no dependency of any kind lands**. **Epic 3.2 (Importers) is CLOSED IN CODE — five merged tasks**, one canonical in-memory `ImportedModel` and eight claimed extensions: 3.2.1 glTF/fastgltf (PR #70 `f02ca65`, ✅ 12/12), 3.2.2 FBX/ufbx (PR #71 `c597a5b`, ✅ 13/13), 3.2.3 OBJ/tinyobjloader (PR #72 `c412e83`, ✅ 13/13), 3.2.4 Blender CLI/`.blend` (PR #73 `5ab07f3`, ✅ 15/15), 3.2.5 Assimp DAE/PLY/STL (PR #74 `7e0224f`, ✅ 14/14). Windows/Linux rows pending for all five, and seven of their ticked rows are missing the measurement they asked for. **Epic 3.3 (Cooker v0) is CLOSED — three merged tasks, every one CI-green on all three lanes with `headSha == HEAD` asserted, and every one macOS-validated with every measurement blank filled**: 3.3.1 Mesh cook → GPU buffers (PR #75 `17a6821`, 13 commits, 42 seeds, ✅ 12/12) opened `engine/assets` and `tools/cooker` and produced the tree's first binary format and first runtime-consumable artifact; 3.3.2 Texture cook → KTX2/Basis (PR #76 `cf8575a`, 15 commits, 53 seeds, ✅ every row) added the KTX2 subset container, the texture cook and the two integer block encoders, and is the first artifact this project produces that a third-party tool can open — `ktx validate` 4.4.2 PASS on all eight artifacts, proven non-vacuous by re-seeding the corrected DFD byte and watching the validator reject it with the exact predicted `error-6028`; 3.3.3 Cook determinism golden test (PR #77 `234a009`, 8 commits, 24 seeds, ✅ 13/13) ships zero C++ and turns cross-lane, cross-config and cross-time byte-identity into a continuous CI check. Windows/Linux rows pending for all three. **Epic 3.4 (PBR materials) is CLOSED — both tasks merged, CI-green on all three lanes with `headSha == HEAD` asserted, and macOS-validated with every measurement blank filled.** **3.4.1 (Material asset + PBR shader) is MERGED as PR #78 (merge commit `a01765d`, 10 commits), CI-green on all three lanes with `headSha == HEAD` asserted before the merge, and macOS-validated ✅ PASS on ALL 11 ROWS (2026-08-16) with every measurement blank filled — 6 cooked textures upload in 6.2 ms (mean 1.0 ms), the sample holds ~121 fps, and the sidecars read 5/2 and 0/1.** It made a cooked texture mean something: the first cooked texture ever drawn on a GPU here, the first `.aeromat` parsed end to end, the first asset resolved at run time by GUID, six BC formats and `textureLevelByteSize` in `engine/rhi` (recorded per the 0.4.1 D18 protocol), `docs/09`'s normative §11, the `MaterialHandle` registry, and the GGX shader pair rewritten in place. **3.4.2 (Material inspector editing) is MERGED as PR #79 (merge commit `3aebbad`, 15 commits), CI-green on all three lanes with `headSha == HEAD` asserted before the merge, and macOS-validated ✅ PASS on ALL 12 ROWS (2026-08-16) with every measurement blank filled — Apply echoes +1 and Create +1 (not +2), the panel first-opens inside a single vsync-paced frame, and the worst committed fixture uploads in 2.1 ms.** It is the task that makes materials editable rather than hand-authored: four editor pairs (`material_edit` pure, `material_session` GPU-free, `material_panel` the only new ImGui TU, `material_preview` the only new GPU TU), `AssetKind::Material`, the eighth panel (id `"Material"`, FROZEN, right dock, registered last), a live preview with its own `RenderTarget` and its own `ForwardRenderer` that gives `updateMaterial` its **first production call site**, slot textures through the real decode → cook → parse → upload chain, and a New Material button. **Two recorded deviations**: `aero::render` joins `aero_editor_core`'s PUBLIC link group (the spec's "no link-line change" and its own AC-25 cannot both hold, since `aero::scene_render` is PRIVATE), and `MaterialParseResult` gains a `warnings` vector under D11's own escape hatch — so **AC-34 is amended and `engine/` is NOT byte-identical**, the diff being exactly `engine/reflect/{include/aero/reflect/material_format.hpp, src/material_format.cpp}`, with `tools/`, `shaders/`, `runtime/`, `samples/`, `vcpkg.json`, `.github/`, `cmake/` and the determinism manifest all byte-identical and **no dependency of any kind**. Mechanical gate green: **133/133 on both macOS presets** with `AERO_REQUIRE_GPU=1`; fresh `-G Ninja` reduced configurations **44/44** and **57/57**; `ctest -N` **133 / 44 / 57 — unmoved**, because every new test rides an existing binary and each editor test binary is ONE ctest entry, so the growth reads only in the doctest totals (**716 / 1577 / 124 / 23 / 22**; `aero_tests` 713 → 716 is a plan-recorded surprise caused by the reflect deviation); six guards exit 0 (math-boundary **347**, project-no-delete Check B **64**); clang-format and clang-tidy clean by exit code. A **30-seed** matrix ran to completion with **two genuine gaps**, both closed **structurally** and re-proven by re-seeding, and six of the plan's own witness attributions corrected. A code-review round found **eleven gaps, two blocking**, all closed — including a use-after-free that is deterministic on Vulkan and D3D12 and benign on Metal. **Epic 3.5 (Skeletal animation · render) is OPEN, and 3.5.1 (Skeleton & GPU skinning) is MERGED as PR #80 (merge commit `c3a2bc7`, fifteen commits), CI-green on all three lanes with `headSha == HEAD` asserted before the merge, and PENDING its macOS validation pass.** It builds the whole missing half between the importers and the GPU: the **first `.aeroskel`** (`docs/09` gains normative **§12**; Reserved renumbers §12 → **§13** and gains `.aeroanim` for 3.5.2), the tree's **first mesh registry**, the **first integer vertex attribute** and **first two-UBO vertex stage**, and **pipelines 2 → 4**. Two `engine/assets` pairs (`cooked_skeleton`, `skeleton_cook`), one editor pair (`skeleton_cook_source`, the **sixteenth**, and the fourth consumer of the `localId` rule), four `engine/render` files (`skinning.{hpp,cpp}` + src-private `skinning_pack.hpp`/`mesh_pack.hpp`) plus the registry inside `ForwardRenderer`, one new shader, one cooker subcommand, and `samples/phase-3-skinning`. **`GLTF_IMPORTER_VERSION` moves 1 → 2** because `--no-skins` finally means something for glTF, and the machine-local import cache re-imports every `.gltf`/`.glb` once per machine and nothing else. Mechanical gate green: **144/144 on both macOS presets** with `AERO_REQUIRE_GPU=1`; fresh `-G Ninja` reduced configurations; `ctest -N` **144 / 55 / 68**, +11 in **all three** configurations in lockstep (the eleven ungated `cooker.*` cases; every other new case rides an existing binary), doctest **776 / 1594 / 124 / 23 / 22**; six guards exit 0 (math-boundary **363**, project-no-delete Check B **65**); clang-format and clang-tidy clean by exit code. The determinism manifest grows **13 → 15 lines / 26 → 30 cross-lane comparisons**, `ktx validate`'s 8 unchanged, with all 13 existing hashes byte-identical. **`engine/rhi`, `engine/scene*`, `engine/reflect`, `engine/platform`, `engine/core`, `runtime/`, `vcpkg.json`, `cmake/` and `.github/scripts/` are byte-identical; no link line moves anywhere; no dependency of any kind lands.** A **36-seed** matrix ran to completion with **three genuine gaps**, all closed structurally and re-proven by re-seeding, six of the plan's own witness attributions corrected, and four declared shader-only seeds (S33–S36) whose only coverage anywhere is validation rows 4, 5 and 6. A **code-review round found five gaps, none blocking, all closed** — the shader's own copy of the 85-joint cap had nothing tying it to the C++ constant (now `JP14`, a comment-stripped source-text pin through a new `AERO_SHADERS_SRC_DIR`); `createMesh` ignored `packMeshSection`'s empty-stream refusal signal and recorded a section offset that pointed at the NEXT section's data; the stale-handle WARN was the one unlatched diagnostic in the draw loop; nothing drew a static and a skinned instance in one view; and the cooked-assets rule's "never a memory error" justification for unvalidated index values went false the moment a GPU consumed them through `drawIndexed`. **The sharpest lesson is about the test, not the code**: `SN8`'s first version stayed GREEN under the very defect it was written for — a static instance silently inheriting the skinned pipeline moves neither `skinnedDrawCount()` nor Metal's validation, so a third diagnostics accessor, `pipelineBindCount()`, was added to make pipeline TRANSITIONS observable, and re-seeding now reddens exactly that one line at 1 instead of 2. **3.5.2 (Clip playback) is MERGED as PR #82 (merge commit `5622a77`, seventeen commits), CI-green on all three lanes with `headSha == HEAD` asserted before the merge, and PENDING its twelve-row macOS pass.** A rigged model finally moves: the tree's **third first-party binary format** (`.aeroanim`, `docs/09` gains normative **§13** and Reserved renumbers **§13 → §14**, so §14 is the current Reserved section), the **first evaluator in `engine/render`**, the **sixth reflected built-in** and the first with a `bool`, and the **first sample to build a real `World`** and drive its picture from a component in it. Two `engine/assets` pairs (`cooked_animation`, `animation_cook`), one `engine/render` pair (`animation`), one `engine/scene` pair (`animation_player`), one editor pair (`animation_cook_source`, the **seventeenth**, the **fifth** consumer of the `localId` rule and the **first that must NOT convert**), `AERO_BUILTIN_COMPONENT_HEADERS` at root scope, a fourth cooker subcommand, and `samples/phase-3-animation`. Mechanical gate green: **154/154 on both macOS presets** with `AERO_REQUIRE_GPU=1`; fresh `-G Ninja` reduced configurations; `ctest -N` **154 / 65 / 78**, +10 in **all three** in lockstep (the ten ungated `cooker.animation_*` cases), doctest **860 / 1608 / 124 / 25 / 23** (+101, across five new TUs) with `aero_reflect_meta_test` 4 and `aero_reflect_json_test` 23 **unmoved by design**; six guards exit 0 (math-boundary **380**, project-no-delete Check B **66**); clang-format and clang-tidy clean by exit code. The determinism manifest grows **15 → 18 lines / 30 → 36 cross-lane comparisons** with all 15 existing hashes byte-identical and `ktx validate`'s 8 unchanged. **`engine/rhi`, `engine/scene_render`, `engine/reflect`, `engine/platform`, `engine/core`, `engine/scene_serialize/include`, `shaders/`, `runtime/`, `vcpkg.json`, `cmake/`, `.github/scripts/`, `samples/phase-3-skinning/` and `samples/phase-3-materials/` are byte-identical; no link line moves anywhere; no dependency of any kind lands.** **Three recorded deviations**: `CL5`'s mismatched-span arms sit behind `#if defined(NDEBUG)` because a debug assert and a mismatched-span test cannot both hold; `engine/scene_serialize/src/{scene_serialize.cpp,builtin_serializers.hpp}` are **not** byte-identical (**AC-49 amended** — the hand-written dispatch table decides what is actually SAVED, which is the silent "registered, inspectable, editable and NOT saved" failure one layer below where the CMake variable looks); and `.github/workflows/ci.yml` is not byte-identical, resolving the spec's own D1-vs-AC-46 contradiction. A **47-seed** matrix ran in two halves with **one genuine gap** (`S37`, closed structurally and re-proven by re-seeding — `PL2`'s ±1000.0 delta was a whole multiple of the 2.0 duration, so a paused player that silently ran was indistinguishable), **two plan wordings wrong and re-run corrected** (`S16a`, `S41a`), **one seed added beyond the plan** (`S40b`), **three seeds structurally unwitnessable** (`S47`'s `u` clamp is unreachable, `S33` as written is a no-op, and `S23`'s witness is `CL23` not `CL7`), and **three declared sample-only seeds (S42–S44) whose only coverage anywhere is validation rows 4, 6 and 7**. A **code-review round found one gap, non-blocking, and it is closed**: `locate()` sanitized its interpolation parameter and then forwarded the RAW segment duration, which `hermite` multiplies both tangent terms by — so an overflowing segment (`-3.0e38` to `3.0e38` is legal, strictly increasing, and differences to `+inf`) or a NaN time (§13.10's stated non-check) produced a **NaN pose that reached `computeJointPalette` and the GPU**. `u` survived both on its own, which is what hid it: `NaN > 0` is false and `finite/inf` is 0, but `inf * 0` and `NaN * 0` are both NaN, and `normalizeOrIdentity` does not catch NaN either because `lenSq <= epsilon * epsilon` is FALSE for it. Closed with one predicate and `CL26`, which reddens 21 assertions on the parent commit. It also records **one named, unowned defect**: `ImportSettings::scale` reaches **no animation channel from any importer**, and the scale scheme is already incoherent for a multi-joint skinned hierarchy at `scale != 1`. **Epic 3.6 (Rendering essentials) is OPEN, and 3.6.1 (Frustum culling) is MERGED as PR #84 (merge commit `40d83a6`, eleven commits), CI-green on all three lanes plus the vcpkg-baseline, lint and cook-determinism jobs with `headSha == HEAD` asserted before the merge, and macOS-validated ✅ **PASS on ALL 10 ROWS** (2026-08-22) with **every measurement blank filled**. The default grid draws 670/1000 at yaw 0 and **1** at yaw 180, whole-run record **0.183 ms against 0.714 ms** for `--no-cull` (**−74.4 %**); the 10648-instance grid **1.460 vs 4.115 ms** (**−64.5 %**); the grid holds a vsync-paced **60.0 fps**. **The three declared seeds are all witnessed**: C22 by two clean consoles (0 unexpected lines across four runs, and the editor's own status line reading `warn 0 | err 0 | crit 0`), C28 by the timing A/B, and **C30 by a real `tracy-capture` 0.13.1 session** — the exact pinned version, so the protocol matches — whose `-u -p` export shows both plots present, `drawn + culled == 1000` at every sample, and the labels correct (`drawn 1 / culled 999` facing away, `drawn 669 / culled 331` facing in). **Culling removes nothing visible, proven rather than watched**: 18 matched-yaw A/B pairs with **nine byte-identical** and a mean difference of 0.096/255, while the culled run drew as few as 1 instance against 1000. **The mirror behaves exactly like its twin** — red peaks at 4072 px at yaw 284°, green at 4073 px at yaw 76°, and 284 = 360 − 76 exactly. A degenerate projection draws all 1000 every frame and warns **exactly once** across 2066 frames. **One gap was found in the page itself and is recorded there**: at the default N=10 the mirrored pair is **never inside the frustum at any yaw** (48.0° and 53.4° below the axis against a 30° half-FOV), so **row 6 must be run at a small grid** — the Windows and Linux runs need `N=2`. It is the first consumer of the `CookedBounds` field `.aeromesh` has carried since 3.3.1: one new PURE pair (`engine/render/culling.{hpp,cpp}` — `Aabb`, `Plane`, `FrustumPlane`, `Frustum`, `extractFrustum`, `transformAabb`, `isVisible`, `toAabb`; no rhi type, no logging, no allocation), primitive bounds FOLDED in `create()` and cooked bounds COPIED verbatim in `createMesh`, the cull ahead of material resolution in `draw()`, `RenderView::cullingEnabled = true` by default (all 16 existing draw call sites verified consistent with `mvp == viewProj * model` first), four diagnostics accessors, a latched degenerate-projection WARN, the tree's first two production Tracy plots (`render.drawn` / `render.culled`), and `samples/phase-3-culling`. Mechanical gate green: **157/157 on both macOS presets** with `AERO_REQUIRE_GPU=1`; fresh `-G Ninja` reduced configurations **65/65** and **78/78** (each having built and run `aero_tests`, `aero_editor_shell_test`, `aero_editor_imgui_test` and `aero_cooker` — and NOT the four reflect-gated binaries); `ctest -N` **157 / 65 / 78 — unmoved in all three**, because the new file rides `aero_tests` (one ctest entry) and the sample registers no test, so the growth reads only in doctest: `aero_tests` **906 → 942** (+36: `FC1`–`FC25` tier-0, `CD1`–`CD11` GPU-gated) with the other six binaries **1725 / 138 / 29 / 27 / 7 / 28 unmoved**; six guards exit 0 (math-boundary **404 → 408**, project-no-delete Check B **73 unmoved**); clang-format and clang-tidy clean by exit code; the determinism manifest untouched at **18 lines / 36 comparisons**. The whole diff is **16 tracked files**, and `engine/core`, `engine/platform`, `engine/rhi`, `engine/reflect`, `engine/scene`, `engine/scene_render`, `engine/scene_serialize`, `engine/assets`, `/editor`, `/tools`, `shaders/`, `runtime/`, `vcpkg.json`, `cmake/`, `.github/`, `docs/09-file-formats.md`, `docs/tasks/phase-3.md`, every existing sample, every existing test file and every golden are **byte-identical**; **no link line moves anywhere and no dependency of any kind lands**. A **30-seed** matrix ran to completion (35 runs including re-seeds) with **one genuine gap**, closed structurally and re-proven; **four plan witness attributions were wrong and are corrected in `docs/10`**, the sharpest being that the mirrored instance witnesses NOTHING about a per-instance frustum extraction — measured, twice — so the local-space form is an equivalent implementation rather than a defect, and two source comments stating the false rationale were corrected. **Three declared seeds (C22, C28, C30) were applied, built and run, and redden nothing**; their only coverage is validation rows 1/3, 5 and 7. A **code-review round found three gaps, one BLOCKING, all closed**: the plane-normalisation guard applied a **normalised-vector tolerance to RAW extraction coefficients**, so past a depth ratio of ~1e5 — reachable, since `EditorCamera`'s `MIN_NEAR_PLANE` 1e-3 against `DEFAULT_FAR` 1000 is 1e6 and `engine::Camera`'s near/far carry no `AERO_RANGE` — a **perfectly valid camera** silently disabled culling for the whole view while warning about a projection that was fine (**the spec's own AC-5/AC-6 is the sentence that licensed it**; `FC25`/`CD11` are the new arms and the old guard reddens exactly those two); the two Tracy plots were skipped on the `!hasCamera` early return, leaving them frozen while the accessors read 0; and an unassigned `camera` is a **valid unit-cube frustum** rather than "no frustum", `valid()` true and no WARN. **3.6.2 (Directional shadow map) is MERGED as PR #85 (merge commit `3ffaadf`, sixteen commits)**, CI-green on all six checks with `headSha == HEAD` asserted before the merge and its code-review round closed, and PENDING its twelve-row macOS pass.** It is the first shadow this project has ever cast: one new PURE pair (`engine/render/shadow.{hpp,cpp}`), `ForwardRenderer::renderShadowMap` recording every caster on its **own command buffer**, two depth-only pipelines from three new shaders, a comparison sampler and a 3×3 PCF lookup multiplying **only** the directional term, four appended `DirectionalLight` fields (`sizeof` 16 → **32**), the RHI's **first validation change since 0.4.2**, and `samples/phase-3-shadows`. Mechanical gate green: **157/157 on both macOS presets** with `AERO_REQUIRE_GPU=1` (Debug 239.8 s, Release 60.7 s); fresh `-G Ninja` reduced configurations **65/65** and **78/78**; `ctest -N` **157 / 65 / 78 — unmoved in all three**, so the growth reads only in doctest: `aero_tests` **942 → 986** (+44: `SF1`–`SF28` tier-0, `SM1`–`SM15` GPU-gated, `SW1` in the rhi battery) with the other six binaries **1725 / 138 / 29 / 27 / 7 / 28 unmoved**; the shadow filter lists **43** with tools on and **28** with them off (the `SM` tier absent by its gate); six guards exit 0 (math-boundary **408 → 412**, project-no-delete **6 / 73 unmoved**); the determinism manifest **untouched at 18 hash lines**; clang-format and clang-tidy clean by exit code. The diff is **35 tracked files** and `engine/core`, `engine/assets`, `engine/platform`, `engine/reflect`, `engine/scene_serialize`, `/tools`, `runtime/`, `vcpkg.json`, `cmake/`, `.github/`, `docs/09-file-formats.md`, `docs/tasks/phase-3.md`, the manifest, the root `CMakeLists.txt`, every other sample, and **both scene vertex shaders** are byte-identical; **no link line moves on any existing target and no dependency of any kind lands**. **The plan's file inventory was short by one and the COMPILER found it**, not a grep: `tests/scene_boundary_probe.cpp` carries a **third** `sizeof(DirectionalLight)` assertion where §R.7 said two. **3.6.3 (Tonemap/gamma pass) is COMPLETE IN CODE on `feat/3.6.3-tonemap-gamma-pass` (twelve own commits plus one merge commit, cut from `main` @ `64df342`), NOT MERGED, with no CI run yet and its twelve-row macOS pass PENDING.** It resolves the standing "output is raw linear until 3.6.3" caveat every validation page has carried since 3.4.1: the scene draws into an `RGBA16Float` target `render::PostProcess` owns, and the tree's **first zero-vertex-buffer pipeline** resolves it through exposure, a tone curve and the **sRGB OETF** into the 8-bit surface. Two `engine/render` pairs (`tonemap`, `post_process`) plus the src-private `tonemap_pack.hpp`, the first fullscreen shader pair, the ninth sample, and a two-line wiring change in each editor GPU consumer. **`forward_renderer.{hpp,cpp}`, `lighting.hpp` and all of `engine/scene_render` are byte-identical**; the whole diff is **26 tracked files** (24 of code, assets and build, plus `docs/10` and this file) and `engine/core`, `engine/platform`, `engine/rhi`, `engine/assets`, `engine/reflect`, `engine/scene`, `engine/scene_serialize`, `tools/`, `runtime/`, `cmake/`, `.github/`, `vcpkg.json`, `docs/09-file-formats.md`, `docs/tasks/phase-3.md`, the determinism manifest, all nine pre-existing samples and all seven pre-existing shaders are byte-identical too. **No rhi change, no on-disk format, no dependency, no link-line change** — `engine/render/CMakeLists.txt` gains two source lines and its `target_link_libraries` block is untouched, and §14 is still `docs/09`'s Reserved section. Mechanical gate green: **157/157 on both macOS presets** with `AERO_REQUIRE_GPU=1`; fresh `-G Ninja` reduced configurations **65/65** and **78/78** (each having built and run `aero_tests`, `aero_editor_shell_test`, **`aero_editor_imgui_test`** and `aero_cooker` — the imgui binary matters this task, because it carries `I105`'s tools-OFF arm); `ctest -N` **157 / 65 / 78 — unmoved in all three**, so the growth reads only in doctest: **re-measured on the MERGED tree**, `aero_tests` **986 → 1028** (+42: `TM1`–`TM29` tier-0, `PP1`–`PP13` GPU-gated, one new TU) and `aero_editor_imgui_test` **138 → 142** (+4: `I103`–`I106`), the other five unmoved at **1725 / 29 / 27 / 7 / 28**; six guards exit 0 (math-boundary **412 → 419**, project-no-delete Check B **73 unmoved**); clang-format and clang-tidy clean by exit code; the determinism manifest untouched at **18 hash lines / 36 comparisons**. A **31-seed** matrix ran to completion (38 seeded builds) with **three genuine gaps**, all closed structurally and all re-proven by re-seeding, **nine plan witness attributions corrected by measurement**, and **nine declared seeds** — the largest declared class in the project so far, because nothing here renders a pixel and this task's whole deliverable is a picture. Full detail for every task in `docs/10-engineering-log.md`'s Phase 3 entries. |
| **Next task** | **~~3.4.1~~ / ~~3.4.2~~ DONE, MERGED and macOS-VALIDATED** (PRs #78 `a01765d` and #79 `3aebbad`, ✅ 11/11 and 12/12); **3.4.2's S26 remains uncovered by any pass and cannot be covered from macOS**. **3.5.1 is MERGED (PR #80, `c3a2bc7`) and 3.5.2 is MERGED (PR #82, `5622a77`)**, which **CLOSES Epic 3.5 in code**. **3.1.5 (drag-into-scene) is MERGED (PR #81, `5a1bc69`), which CLOSES Epic 3.1** — it merged `origin/main` (3.5.2) into itself first, and every whole-tree count on its page was re-measured on the merged tree rather than carried over. **Three validation passes are now outstanding and all three are the immediate next step**: **3.5.1's twelve-row pass** (`editor/validation/3.5.1-skeleton-gpu-skinning.md`) — rows 4, 5 and 6 are the only coverage S33–S36 have anywhere, and rows 7 and 8 need locally-generated content that is deliberately not committed, driven through the sample's `argv[1]` override; **3.5.2's twelve-row pass** (`editor/validation/3.5.2-clip-playback.md`, written before the pass) — rows 4, 6 and 7 are the only coverage S42–S44 have anywhere, rows 2 and 9–12 carry measurement blanks in bold, and row 12 needs a locally-downloaded rigged and animated model that is deliberately never committed; and **3.1.5's sixteen-row pass** (`editor/validation/3.1.5-drag-into-scene.md`, written before the pass) — rows 3, 4, 9 and 10 are the only coverage N1–N5 have anywhere, rows 5 and 11–13 carry measurement blanks in bold, and rows 12–13 need a **Mixamo-class textured FBX that is deliberately never committed** and must be downloaded locally first. **3.6.1 (Frustum culling) is MERGED (PR #84, `40d83a6`), which OPENS Epic 3.6** — every count on its page was re-measured on the merged tree rather than carried over. Its **ten-row macOS pass** was RUN on 2026-08-22 (`editor/validation/3.6.1-frustum-culling.md`) and is ✅ **PASS on ALL 10 ROWS with every measurement blank filled** — including all three declared seeds (C22, C28 and C30), the last via a real `tracy-capture` 0.13.1 session. **One gap in the page itself was found and recorded there**: at the default N=10 the mirrored pair is never inside the frustum at any yaw, so **row 6 must be run at `N=2`** — which the Windows and Linux runs will need. That page's rows 4, 5 and 10 carry measurement blanks in bold and whose rows 1/3, 5 and 7 are the ONLY coverage seeds C22, C28 and C30 have anywhere. **3.6.2 (Directional shadow map) is MERGED as PR #85 (merge commit `3ffaadf`, sixteen commits)**, CI-green on all six checks with `headSha == HEAD` asserted before the merge and its code-review round closed (eight findings, three blocking, all fixed); its **twelve-row macOS pass** is the IMMEDIATE next thing to run. **Seven of its seeds redden nothing in the whole 43-case shadow tier** — `SH24`–`SH28` plus `SH22`/`SH23`'s second half — and validation rows 2, 3, 4, 5, 6 and 9 are their only coverage anywhere. **`Aabb`'s promotion trigger did NOT fire**: it fires when a layer that is not `engine/render` needs these types (Phase 6's `physics` is the first real candidate), and 3.6.2 is a second CONSUMER inside the same layer — `culling.cpp` is byte-identical and `culling.hpp` changed by one comment, its normalisation note discharged. **3.6.3 (Tonemap/gamma pass) is COMPLETE IN CODE on `feat/3.6.3-tonemap-gamma-pass` and NOT MERGED** — twelve own commits plus one merge commit, the full local gate green, a 31-seed matrix run to completion with three genuine gaps closed structurally and re-proven, and **no CI run yet**. Its **twelve-row macOS pass** (`editor/validation/3.6.3-tonemap-gamma.md`, written before the pass) is PENDING and is the immediate next step for it: rows 4, 5, 6, 7, 8, 9, 11 and 12 carry measurement blanks in bold, and rows 1, 2, 4, 5, 6 and 11 are the ONLY coverage its **nine** declared seeds have anywhere. **Two of those nine are predicted to look identical to correct code** and the page says so — `T24` is derivably inert on the current clear/shader pair, and `T15`/`T16` are numerically identical under the 1:1 blit invariant, so row 4 witnesses the pair failing together. **It resolves the standing "output is raw linear until 3.6.3" caveat** every validation page has carried since 3.4.1 — the existing pages and the three sample READMEs are deliberately NOT edited (forward-only), and retrofitting the four existing samples is a named, unowned follow-up. **It also widens the Windows/Linux platform-validation debt by a FULL task's worth**: CI runs `PP1`–`PP13` under Metal, WARP and lavapipe on every push — which genuinely covers the tree's first zero-vertex-buffer pipeline and its first `RGBA16Float` target — and covers **none of the grade**, because no lane produces a judgeable picture. After that: **(a) 3.6.3 (tonemap/gamma)**, which closes Epic 3.6 and retires the last standing "known and expected" caveat on every validation page since 3.4.1 — 3.6.2 retires the "no shadows" one **for the directional light and for nothing else**. And **(b) the seven ticked-but-unmeasured validation rows**, plus the four-phase Windows/Linux platform-validation debt, which with macOS otherwise green is the whole of the remaining validation risk — and which **3.5.2 widens by a full task's worth**, because it adds no shader, no pipeline and no GPU-tier case, so **no lane exercises any part of what its pass is for**, and which **3.1.5 widens in the one way CI cannot narrow**, since no lane performs a mouse gesture. **A note for whoever adds a fixture or a cook change next**: `tests/cooker/determinism.sha256` is FROZEN at **18 lines across four arms**, a red manifest case is `docs/09` §9.11's `cookerVersion` sentence firing, and the regeneration ritual lives in the manifest's own header — never edit a hash to green a red run. See `docs/tasks/phase-3.md`. |

Engine layers that exist today, in dependency order: `core` (gained `guid.hpp`/`guid.cpp` at task
3.1.1, beside `handle.hpp`; gained `content_hash.hpp`/`content_hash.cpp` at task 3.1.2, beside `guid`)
→ **`assets`** → `platform` → `rhi` → `render` → `reflect` → `scene` → `scene_render` →
`scene_serialize`, plus `/editor` (`aero_editor_core` + `aero_editor`) and `/tools` (`reflect-gen`,
`shaderc`, **`cooker`**). `/runtime` is still empty — it arrives in Phase 5.
**`engine/assets/` OPENED at task 3.3.1** and its `.gitkeep` is gone. The old reason for keeping it
shut — "unopened until a **runtime** consumer exists (Phase 5's pak table)" — was satisfied by the
task itself: the `.aeromesh` container's *reader* **is** a runtime component by definition, so the
consumer in question is the thing being built. It holds the cooked-asset formats and nothing else — **nine pairs since task
3.5.2**: `cooked_mesh.{hpp,cpp}`, `mesh_cook.{hpp,cpp}`, `cooked_texture.{hpp,cpp}`,
`texture_cook.{hpp,cpp}`, `bc_block.{hpp,cpp}`, `cooked_skeleton.{hpp,cpp}`,
`skeleton_cook.{hpp,cpp}`, `cooked_animation.{hpp,cpp}` and `animation_cook.{hpp,cpp}`; it links
`aero::core` + `aero::profiling` and **no
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
`engine/scene_serialize` are byte-identical across both 3.4 tasks: a material was not yet nameable from
a scene file, and **3.1.5 owned that and has since taken it** — 3.4.2 did not. Still **no dependency of any kind** — `vcpkg.json`,
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
**Task 3.5.1's whole `engine/` diff is two new `engine/assets` pairs** (`cooked_skeleton`,
`skeleton_cook` — the subdirectory's first growth since 3.3.2, `engine/CMakeLists.txt` untouched) **and
four new `engine/render` files** (`skinning.{hpp,cpp}` public, `src/skinning_pack.hpp` and
`src/mesh_pack.hpp` src-private), plus the mesh registry inside `forward_renderer.{hpp,cpp}`, three
defaulted members on `MeshInstance`, `MeshTag`/`MeshHandle` in `mesh.hpp` and one umbrella include.
**`engine/rhi`, `engine/scene`, `engine/scene_render`, `engine/scene_serialize`, `engine/reflect`,
`engine/platform`, `engine/core` and all five pre-existing `engine/assets` pairs are byte-identical**,
as are `runtime/`, `vcpkg.json`, `cmake/` and `.github/scripts/`. **No link line moves anywhere and no
dependency of any kind lands**: `skinning.hpp` naming `assets::CookedSkeleton` rides `aero_render`'s
existing `PUBLIC aero::assets` (3.4.1's edge), the adapter rides `aero_editor_core`'s, and the
subcommand rides `aero_cooker`'s. `shaders/CMakeLists.txt` gains **exactly one line** and both existing
scene shaders are byte-identical — a second vertex shader shares the fragment stage rather than forking
it, so nothing downstream can tell which VS fed it.
**Task 3.5.2 touches FOUR engine subsystems and adds four pairs**: two in `engine/assets`
(`cooked_animation`, `animation_cook` — the subdirectory's second growth since 3.3.2,
`engine/CMakeLists.txt` untouched), one in `engine/render` (`animation.{hpp,cpp}` public, the layer's
**first evaluator**), one in `engine/scene` (`animation_player.{hpp,cpp}`, the sixth built-in), plus
two umbrella includes (each sorting **first**), `transform.cpp` moving by exactly **two lines**, and —
as a **recorded deviation** — `engine/scene_serialize/src/{scene_serialize.cpp,builtin_serializers.hpp}`,
whose hand-written dispatch table decides what is actually *saved*. **`engine/rhi`,
`engine/scene_render`, `engine/reflect`, `engine/platform`, `engine/core`,
`engine/scene_serialize/include`, all seven pre-3.5.2 `engine/assets` pairs, `engine/render`'s four
pre-existing public headers and both src-private packers, `forward_renderer.{hpp,cpp}`,
`engine/scene`'s five existing headers, `world.cpp` and `camera.cpp` are byte-identical**, as are
`shaders/`, `runtime/`, `vcpkg.json`, `cmake/`, `.github/scripts/`, `samples/phase-3-skinning/` and
`samples/phase-3-materials/` — **this task adds no draw path and no shader**. `.github/workflows/ci.yml`
**is not** byte-identical (the manifest total moves 30 → 36). **No link line moves anywhere and no
dependency of any kind lands**: `animation.hpp` naming `assets::CookedAnimation` rides `aero_render`'s
existing `PUBLIC aero::assets` (3.4.1's edge), `animation_player.hpp` includes
`<aero/reflect/annotations.hpp>` only — which `transform.hpp` already forces PUBLIC on `aero_scene` —
the adapter rides `aero_editor_core`'s group, and the subcommand rides `aero_cooker`'s. Root
`CMakeLists.txt` gains **`AERO_BUILTIN_COMPONENT_HEADERS`**, one `set()` at root scope reaching all
four reflection-generation sites.
**Task 3.1.5 touches four engine subsystems, and it is the SECOND task to touch `engine/scene` since
2.4.2** — 3.5.2's `animation_player` pair took the first seat while this branch was in flight.
`engine/scene`: `mesh_renderer.hpp` gains three appended fields (`sizeof` 16 → **56**, with four
padding bytes stated in the `static_assert` rather than removed). `engine/reflect`:
`serialize.{hpp,cpp}` gain one `writeJson`/`readJson` overload pair for `engine::Guid` — the read side
declared **above** `readField`, because ADL for `engine::Guid` searches `engine`, not
`engine::reflect`. `engine/scene_render`: the new `asset_bindings.{hpp,cpp}` pair (the subsystem's
second source file, one `CMakeLists.txt` line), three emission arms and a `bindings()` accessor.
`engine/render`: **two appended `RenderView` counts and nothing else at all**. **Its own diff leaves
`engine/core`, `engine/assets`, `engine/platform`, `engine/rhi` and `engine/scene_serialize`
byte-identical**, as are `runtime/`, `shaders/`, `vcpkg.json`, `cmake/`, `.github/` and `tools/cooker`;
the only `tools/` diff is `reflect-gen`'s subset arm. **No link line moves anywhere and no dependency
of any kind lands** — `aero_scene_render` already linked `PUBLIC aero::scene aero::render`, and
`aero_editor_imgui_test` gains `aero::scene_render` only to put its **include** directory on the
compile line (the archive was already there, since a `PRIVATE` link on a static library propagates as
`$<LINK_ONLY:…>`).
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
`project_files.hpp` and `assetContentHashUsable` in `asset_meta.hpp`.
**3.5.1 adds ONE more pair, taking the count to SIXTEEN** (re-counted at task end, never added to the
remembered number): `skeleton_cook_source.{hpp,cpp}` (PUBLIC and PURE — the `ImportedModel` → skeleton
cook adapter, on `mesh_cook_source`'s terms: no disk, no UI, no SDL, no `<filesystem>`, no logging, and
warnings **returned** rather than printed). It is the **fourth named consumer of the `localId` rule**
and the first outside a panel; it also gates `gltf_import.cpp`'s `JOINTS_0`/`WEIGHTS_0` reads on
`importSkins` and moves `GLTF_IMPORTER_VERSION` 1 → 2.
**3.5.2 adds ONE more pair, taking the count to SEVENTEEN** (re-counted at task end, never added to the
remembered number): `animation_cook_source.{hpp,cpp}` (PUBLIC and PURE — the `ImportedModel` → animation
cook adapter, on `skeleton_cook_source`'s terms: no disk, no UI, no SDL, no `<filesystem>`, no logging,
and warnings **returned** rather than printed; its result type is **owning and contains no span**,
deliberately, because the rejected shape dangles on any copy *and* during construction). It is the
**FIFTH named consumer of the `localId` rule and the FIRST that must NOT convert**: it writes
`ImportedAnimationChannel::targetNode` into the file **verbatim**, because `.aeroskel`'s
`sourceNodeLocalId` is the same kind of value and the two must be comparable at bind time — mapping it
would make every FBX clip bind to the wrong joints, **silently**, and `AS9` is hand-built precisely
because glTF cannot see the difference.
**3.1.5 adds SEVEN pairs, taking the count to TWENTY-FOUR** (re-counted at task end, never added to
the remembered number — this branch merged `origin/main` after its own count was taken, so the running
total must be re-derived from the tree rather than from either side's page), split by **dependency**
the way 3.4.2's four were: `asset_drag.{hpp,cpp}` (PUBLIC and PURE — the payload, the decode and the
whole accept/refuse matrix; names no ImGui type and calls no ImGui function),
`instantiate_plan.{hpp,cpp}` (PUBLIC and PURE — the `ImportedModel` → entity-subtree planner and
another named `localId` consumer, the **sixth** once this branch lands),
`asset_commands.{hpp,cpp}` (PUBLIC — the sixth structural command, the first creating more than one
entity), `scene_asset_ledger.{hpp,cpp}` (PUBLIC and PURE — decides, never executes),
`material_from_import.{hpp,cpp}` (PUBLIC and PURE — `ImportedMaterial` → `MaterialDocument`),
`scene_asset_loader.{hpp,cpp}` (src-private — the only TU that mints a `MeshHandle` here) and
`texture_load.{hpp,cpp}` (src-private — the decode → cook → parse → upload chain **extracted**
verbatim from `MaterialPreview`, with zero test edits). It also deletes `LOCAL_MESH_HALF_EXTENT` and
`selection_overlay.cpp`'s duplicate corner enumeration, promotes `captureAndDestroySubtrees` /
`restoreStructuralState` onto `entity_commands.hpp` and `blendExportSettingsFingerprint` onto
`blender_tool.hpp`, and adds no new panel. The `.hpp`s live under
`editor/include/aero/editor/` (except those named src-private, which live beside their `.cpp` in
`editor/src/` — **23** tracked `editor/src/*.hpp`, up from 21 for 3.1.5's two src-private headers),
the `.cpp`s under `editor/src/` (**73**, up from 66).
**Task 3.6.1 touches ONE engine subsystem and adds ONE pair**: `engine/render/culling.{hpp,cpp}`
(PUBLIC and PURE — the layer's second pure module after `animation`, and the first anywhere that names
no rhi type at all), plus `engine/render/CMakeLists.txt` (one source line; the
`target_link_libraries` block byte-identical), `render.hpp` (one include, sorted after
`animation.hpp`), `lighting.hpp` (`RenderView::cullingEnabled` appended last), `mesh.hpp` (a comment
on `MeshInstance::model` only) and `forward_renderer.{hpp,cpp}` (two `Aabb bounds` fields on the
private registry structs, four diagnostics accessors, three counters, one latch, the fold in
`create()`, the copy in `createMesh`, `instanceBounds`, and the cull in `draw()`). **`engine/core`,
`engine/assets`, `engine/platform`, `engine/rhi`, `engine/reflect`, `engine/scene`,
`engine/scene_render`, `engine/scene_serialize`, `/editor`, `/tools`, `shaders/`, `runtime/`,
`vcpkg.json`, `cmake/`, `.github/` and every existing sample and test file are byte-identical**, as is
`tests/cooker/determinism.sha256`. **No link line moves anywhere and no dependency of any kind lands**:
`toAabb` naming `assets::CookedBounds` rides `aero_render`'s existing `PUBLIC aero::assets` edge from
3.4.1, and `samples/phase-3-culling` links `aero::render aero::rhi aero::platform aero::core
aero::profiling` and deliberately **not** `aero::assets` (it reads no cooked artifact and commits no
fixture — every mesh it draws is a built-in primitive).
**Task 3.6.2 touches FOUR engine subsystems and adds ONE pair**: `engine/render/shadow.{hpp,cpp}`
(PUBLIC and PURE — the layer's **third** pure module after `animation` and `culling`, and the **second**
naming no rhi type at all), plus `engine/render`'s `CMakeLists.txt` (one source line, the
`target_link_libraries` block byte-identical), `render.hpp` (one include, sorted after `renderer.hpp`),
`lighting.hpp` (`ShadowView`, four fields on `DirectionalLightData`, two on `RenderView`),
`material_pack.hpp` (`GpuLightBlock` 320 → **400**, pinned by two `offsetof` assertions) and
`forward_renderer.{hpp,cpp}` (four config fields, `renderShadowMap`, five diagnostics accessors, the
shared `resolveInstanceDraw`, `createShadowResources` and eleven private members). **`engine/rhi` takes
its FIRST validation change since 0.4.2** — two widened predicates in `sdl_gpu_backend.cpp` plus three
corrected prose claims across `descriptors.hpp` and `device.hpp` — and **`engine/scene`'s
`DirectionalLight` grows 16 → 32 bytes**, which is the THIRD task to touch `engine/scene` since 2.4.2.
`engine/scene_render`'s `scene_renderer.cpp` gains the designated init, one dropped `const` and one
call. **`engine/core`, `engine/assets`, `engine/platform`, `engine/reflect`, `engine/scene_serialize`,
`/tools`, `runtime/`, `vcpkg.json`, `cmake/` and `.github/` are byte-identical**, as are
`engine/render`'s `culling.cpp`, `animation.*`, `material.hpp`, `texture_upload.*`, `render_target.*`,
`renderer.*`, `primitives.*`, `skinning.*`, `mesh.hpp` and both src-private packers, and
**`shaders/scene.vert.hlsl` and `shaders/scene_skinned.vert.hlsl`**. **No link line moves on any
EXISTING target and no dependency of any kind lands**: `shadow.hpp` names only `CameraView`, `Aabb`,
`Plane` and the engine math types, all of which `aero_render` already had. **`/editor` is NOT
byte-identical, by ONE file**: the code-review round found that `MaterialPreview` inherited the new
`shadowMapResolution = 2048` default and would have allocated ~16.8 MB of dead VRAM per editor session
for a renderer that never calls `renderShadowMap` — neither the spec nor the plan mentions
`material_preview` at all. **The general form: a defaulted field appended to a widely-constructed
config struct is a cost every existing caller silently starts paying.** `samples/phase-3-shadows`
names `aero::assets` on its own new line — a recorded deviation from the spec's D18 parenthetical,
because the skinned depth pipeline is reachable only through a registered cooked mesh, and the rig is
cooked **in memory** so no artifact is read and no fixture is committed.

**Task 3.6.3 touches ONE engine subsystem and adds TWO pairs plus one src-private header**:
`engine/render/tonemap.{hpp,cpp}` (PUBLIC and PURE — the layer's THIRD pure module after `animation`
and `culling`, and the second that names no rhi type at all), `engine/render/post_process.{hpp,cpp}`
(PUBLIC — `tonemapSourceUvMax`, `PostProcessConfig`, `PostProcess`) and
`engine/render/src/tonemap_pack.hpp` (src-private, the two 16-byte uniform blocks and the enum
mirror's five `static_assert`s), plus `engine/render/CMakeLists.txt` (two source lines; the
`target_link_libraries` block byte-identical) and `render.hpp` (two sorted includes). `shaders/` gains
its **first fullscreen pair** (`fullscreen.vert.hlsl` + `tonemap.frag.hlsl`, two `aero_add_shaders`
entries) and `samples/` its **ninth** entry (`phase-3-tonemap`, linking the same five targets
`phase-3-culling` does and deliberately **not** `aero::assets`). `/editor` gains **no new pair** — the
count stays at **twenty-four** — and the four files it touches are `viewport_panel.{hpp,cpp}`,
`material_preview.{hpp,cpp}`, `material_panel.{hpp,cpp}` and `editor_app.cpp`, each with a
trailing-parameter or two-line change. **`engine/core`, `engine/assets`, `engine/platform`,
`engine/rhi`, `engine/reflect`, `engine/scene`, `engine/scene_render`, `engine/scene_serialize`,
`engine/render`'s six pre-existing public headers and its three pre-existing src-private packers,
`forward_renderer.{hpp,cpp}`, `lighting.hpp`, `renderer.hpp`, `tools/`, `runtime/`, `vcpkg.json`,
`cmake/`, `.github/` and every pre-existing shader and sample are byte-identical**; **no link line
moves anywhere and no dependency of any kind lands**.

Test inventory measured on `feat/3.6.3-tonemap-gamma-pass` **AFTER it merged `origin/main` @ `3ffaadf`
(3.6.2, PR #85)** — so every figure below is a MERGED-TREE reading, taken with both tasks' code in the
tree at once. Every number **re-measured on the tree in front of you, never derived by addition and
never carried forward from an earlier step or an earlier task** — read the totals from doctest's own
`filters:` line, never from a `grep -c` of case names. **This is 3.1.5's recorded lesson applied a
second time**: the moment a branch merges `origin/main`, every whole-tree count on its own page goes
stale, and adding one task's delta to another task's baseline is exactly the arithmetic that produces
a confident wrong number. 3.6.2 moved `aero_tests` **942 → 986** and 3.6.3 moves it **986 → 1028**;
3.6.3 alone moves `aero_editor_imgui_test` **138 → 142**. When 3.6.3 merges, re-measure rather than
trusting that sentence.
**`ctest -N` reads 157 / 65 / 78** — tools ON, then
`-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, then `-DAERO_REFLECT_TOOLS=OFF` alone. **The
reason matters more than the number**: `aero_tests`, `aero_editor_shell_test` and
`aero_editor_imgui_test` each register with ctest as a **single entry** (`tests/CMakeLists.txt`), so
3.4.1's 57 new doctest cases, 3.4.2's 84, 3.5.1's 74, 3.5.2's 100, 3.1.5's **190**, 3.6.1's **36** and
3.6.3's **46** move it not at all, and samples register no test. **Until 3.1.5 the triple moved only for `cooker.*` cases**
(**117 → 131 → 133 → 144 → 154**, **28 → 42 → 44 → 55 → 65**, **41 → 55 → 57 → 68 → 78**), because
`aero_cooker` takes **no gate flag** and every one of its cases is registered in every configuration;
3.5.2's +10 (nine animation-subcommand arms plus the fourth manifest arm) is therefore **identical in
all three**, exactly as 3.5.1's +11 was, and that lockstep is itself an assertion — a smaller move in a
reduced configuration would mean the cooker block had accidentally grown a gate. **3.1.5 is the first
task whose move is deliberately ASYMMETRIC: +3 / +0 / +0**, the three being the gated
`reflect-gen.guid_components` / `guid_meta` / `guid_json` cases, which live inside
`if(AERO_REFLECT_TOOLS)` and are **absent by design** from both reduced configurations. **The flat
reduced pair is the prediction being met, not a missed registration.** **A future gate
flag on the cooker would silently shrink the reduced configurations' coverage with no test able to
report it.** Read the two kinds of move differently: a `cooker.*` addition must be identical in all
three, and a `reflect-gen.*` addition must be tools-ON only. An unmoved `ctest -N` means "zero C++" for
task 3.3.3 and means nothing of the kind for a
task that grows an existing binary — check a zero-C++ claim against the **doctest** totals instead.

**Doctest, SEVEN binaries: 1028 / 1725 / 142 / 29 / 27 / 7 / 28 on the MERGED tree.** Two tasks
contributed and the totals are read from the merged tree rather than summed: 3.6.2 added **+44** to
`aero_tests` (`SF1`–`SF28` tier-0, `SM1`–`SM15` behind `AERO_SHADER_TOOLS_ENABLED` in one new TU, and
`SW1` appended to `rhi_device_test.cpp`), and 3.6.3 adds **+42** more (`TM1`–`TM29` tier-0 and
`PP1`–`PP13` GPU-gated, one new TU) plus **+4** to `aero_editor_imgui_test` (`I103`–`I106`); the other
five are **unmoved by both**. Per-filter, tools on / tools off: the shadow filter lists **43 / 28**
and the tonemap filter **42 / 29**, each one's GPU tier absent by its own gate. **`aero_editor_imgui_test`
reads 142 tools-on and 128 tools-off**, and that second number is a real check rather than a
decoration: 3.6.3's four editor cases first landed inside 3.1.5's file-level
`#if AERO_SHADER_TOOLS_ENABLED` block, everything was green, and `I105`'s tools-OFF arm — the only
place AC-16's degradation path is tested at all — never ran. **Counting cases in the reduced
configuration is what caught it, because a suite that omits a case passes just as loudly as one that
runs it.** 3.6.1 had added **+36** (`FC1`–`FC25` and `CD1`–`CD11`), and its culling filter still lists
**25** tools-off. The tracked list read **five** until 3.1.5, because
`aero_reflect_meta_test` and `aero_reflect_json_test` both sit inside `if(AERO_REFLECT_TOOLS)` blocks
and are absent from the reduced configurations — but they are real binaries with real cases, and 3.1.5
is the first task to add to them since 1.2.2. They are tracked from here on. **What each side
contributed, so the merged totals can be checked rather than guessed** — 3.5.2 moved `aero_tests`
**776 → 860** (+84: `AN1`–`AN24` and `KA1`–`KA22` on the `.aeroanim` container and its cook,
`CL1`–`CL25` on the clip sampler's bind, both clamps and all three interpolation modes, and
`PL1`–`PL12` on the playback clock's six steps, across **four** new TUs), `aero_editor_shell_test`
**1594 → 1608** (+14: `tests/editor/animation_cook_source_test.cpp` with `AS1`–`AS14`),
`aero_scene_serialize_test` **23 → 25** (`G11` and `G12`, the sixth built-in's round trip and its
dispatch) and `aero_editor_inspector_test` **22 → 23** (the D16 case, asserting all four reflected
fields rather than merely that a lookup did not crash), leaving `aero_editor_imgui_test` **unmoved at
124** and — **deliberately** — `aero_reflect_meta_test` (4) and `aero_reflect_json_test` (23) unmoved
too, since both consume `AERO_BUILTIN_COMPONENT_HEADERS` rather than a new fixture, so their generated
artifacts change *content* without changing case count. 3.1.5 then adds **+46** to `aero_tests`
(`SJ1`–`SJ10` on the `Guid` overload pair, `AB1`–`AB14` on the binding table and `BR1`–`BR22` on
`buildRenderView`'s three arms, across two new TUs), **+115** to `aero_editor_shell_test`
(`DR1`–`DR18`, `PL1`–`PL21`, `IA1`–`IA15`, `MF1`–`MF20` and `LG1`–`LG24` across five new TUs, plus
`PK`/`LB`/`VP` growth in three existing ones), **+13** to `aero_editor_imgui_test` (the `SL*` loader and
`DP*` drop integration cases), **+4** to `aero_scene_serialize_test` (the four §2.3 tolerance rows on
the three new keys), **+4** to `aero_editor_inspector_test` (the Guid field row, `IR6`/`IR7`), **+3** to
`aero_reflect_meta_test` and **+5** to `aero_reflect_json_test` (`GD1`–`GD8`). **Those two lists are
deltas, not a sum** — the merged totals are 906 / 1725 / 138 / 29 / 27 / 7 / 28, read from doctest's own `filters:`
line on a build of the merged tree. Both reduced configurations must be rebuilt FRESH with `-G Ninja`.
**One collision the merge creates and nothing detects**: the `PL` prefix now names 3.5.2's playback
clock in `aero_tests` **and** 3.1.5's instantiate-plan cases in `aero_editor_shell_test`, so a
`--test-case=*PL7*` filter means two different things depending on which binary it is aimed at. Both
are legal — doctest names are per-binary — but a case id is only unambiguous **with its binary named**.
**A reduced-configuration probe must be configured with `-G Ninja`**: `CMAKE_GENERATOR` enters the
shadercross bootstrap's option hash, so the generator-less form reads the cached toolchain as COLD and
pays a from-source DXC rebuild that peaked at 7.6 GB here before a memory guard killed it. **And it
must name which binaries it ran**: 3.4.2's earlier probe built `aero_editor_shell_test` only, so five
preview cases that assert the wrong contract in a tools-OFF build (115/120) survived until the full
gate. `aero_editor_imgui_test` now carries `AERO_SHADER_TOOLS_ENABLED=1` in its own
`if(AERO_SHADER_TOOLS)` block and **both arms assert** — a skip would leave AC-32 untested in the one
configuration that can test it. **3.1.5 applies the identical shape to the OTHER gate**: a material
drop assigns through `SetFieldCommand`, which rides the reflection seam, so with
`-DAERO_REFLECT_TOOLS=OFF` there is no `entt::meta` for `engine::MeshRenderer` and the drop **cannot**
land. That is correct there, not a defect — but the drain used to reach `readComponentField` and log an
`AERO_LOG_ERROR` **from the seam** in the one configuration where nothing is wrong, because **there are
two registries and they do not agree**: the `World`'s hand-written component table resolves
`engine::MeshRenderer` **by name** even with no meta anywhere, so a guard on `ComponentTypeId::valid()`
alone sails straight past. `componentFieldsAreReflected` asks the meta registry directly, and both arms
of the test **assert** — field set and undo restores it with tools on; field untouched and nothing
pushed with them off — selected by a compile definition, because the editor is built in every
configuration.
`aero_editor_core` sources **65 → 72** and tracked `editor/src/*.cpp` **66 → 73** (3.1.5's seven pairs).
`check-math-boundary.sh`'s scanned count **380 → 404** at 3.1.5 (+24 tracked C-family files: 14
`editor`, 2 `engine/scene_render`, 7 `tests` and 1 `reflect-gen` fixture), **404 → 408** at 3.6.1
(+4: `culling.hpp`, `culling.cpp`, `render_culling_test.cpp` and `samples/phase-3-culling/main.cpp` —
the sample's `CMakeLists.txt` and `README.md` are not C-family), **408 → 412** at 3.6.2 (+4:
`shadow.hpp`, `shadow.cpp`, `render_shadow_test.cpp` and `samples/phase-3-shadows/main.cpp` — the three
new `.hlsl` files are not C-family either) and **412 → 419** at 3.6.3 (+7: `tonemap.hpp`,
`tonemap.cpp`, `post_process.hpp`, `post_process.cpp`, `tonemap_pack.hpp`, `render_tonemap_test.cpp`
and `samples/phase-3-tonemap/main.cpp` — its two `.hlsl` files are not C-family either), with
`check-project-no-delete.sh`'s Check B scan **66 → 73** at 3.1.5 and **unmoved at 73** through 3.6.1,
3.6.2 and 3.6.3 — none of the three adds an `editor/src/*.cpp`. All re-measured **after `git add`**, since `git ls-files` sees
only tracked files, and both globs picking the new files up automatically — **neither script changes,
and `.github/scripts/` is
byte-identical to `main`.** Guard count stays **six**; Check A's six-file denylist and Check B's
two-file `PERMITTED_DELETERS` are unchanged in membership, and **none of 3.1.5's seven new TUs** is in
either, which is what makes a future destructive call in one of them a hard CI failure.
**Committed images: two at `tests/fixtures/assets/` (3.3.2) plus six 32×32 PNGs and six `.ktx2` under
`samples/phase-3-materials/textures/` (3.4.1)** — the `.ktx2` cooked once with pinned GUIDs and
regenerable byte-identically from the PNGs by the README's recorded commands, which is 3.3.3's
guarantee being spent rather than re-proven. **Committed text fixtures gained seven `.aeromat` files at
`tests/fixtures/materials/` (3.4.2)**, reached through `AERO_MATERIAL_FIXTURES_DIR` — a path, not a
flag, so a missing one is a `REQUIRE` failure rather than a silent skip. **3.5.1 commits the tree's
first `.aeromesh` and first `.aeroskel`** — `samples/phase-3-skinning/arm.aeromesh` (7216 B) and
`arm.aeroskel` (704 B), two cooks of one self-authored `arm.gltf` under **one pinned GUID**, since
`sourceGuid` means "the asset these bytes came from" — plus one `tests/cooker/fixtures/skinned-quad.gltf`
whose two manifest lines are the first cross-lane witness the skinned emit path has ever had.
**3.5.2 commits the tree's first `.aeroanim`** — `samples/phase-3-animation/wave.aeroanim` (1152 B)
beside `wave.aeromesh` (7216 B) and `wave.aeroskel` (704 B), **three cooks of one self-authored
`wave.gltf` under ONE pinned GUID** (`352a0000000000000000000000000001`), all three verified to re-cook
**byte-identically** from a clean temp directory and deliberately **not** in the frozen manifest.
`wave.aeroanim`'s 1152 bytes are `80 + 32x5 + 4x37 + 12 + 16x47` exactly — the header, five channel
records, 37 keys, the format's single padding site and 47 values. **Neither 3.6.1 nor 3.6.2 commits an artifact or a fixture of any kind** — `samples/phase-3-culling`
draws only built-in primitives, and `samples/phase-3-shadows` adds one two-bone strip cooked **in
memory** at startup plus a 64×64 normal map generated in memory, so there is nothing on disk to keep in
step. **It adds NO new test fixture
anywhere**: the manifest's fourth arm and the `AS` battery both drive the existing
`tests/fixtures/assets/skinned.gltf`. **The determinism manifest is now FROZEN at 18 lines across four
arms / 36 cross-lane comparisons**, with all 15 pre-3.5.2 hashes byte-identical and `ktx validate`'s 8
unchanged. **3.1.5 adds no manifest arm and no cooked artifact at all** — it commits one new fixture,
`tests/reflect-gen/fixtures/component_guid.hpp` (annotation-free, a `Guid` beside a `uint32` and a
`Vec3`, so the new category is proven to **coexist** with the old subset) and hand-edits two existing
goldens: `tests/fixtures/scenes/full.scene.json` gains the three new keys on **both** `MeshRenderer`
payloads, one arm defaulted and one carrying a **non-nil** guid pair with `meshIndex 3`, and
`samples/phase-1-scene/scene.json` is re-emitted through the engine's own writer. **That non-nil arm is
load-bearing**: with all-nil values `G2` cannot see an uppercasing writer at all, because
`toupper('0') == '0'`.
`git grep -nE '_WIN32|__APPLE__|__linux__' -- engine/assets engine/render engine/scene tools/cooker`
reads **zero lines**, and the
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
