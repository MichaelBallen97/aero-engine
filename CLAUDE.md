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

**Phase 3 (Asset Pipeline & 3D Content) is OPEN. Epic 3.1 (AssetDatabase · assets) CLOSES with
3.1.5 (drag-into-scene) — its last open task, now COMPLETE IN CODE on its branch and PENDING its
sixteen-row macOS validation pass.** Epics 3.3 (Cooker v0) and 3.4 (PBR materials) are
both CLOSED — every task merged, CI-green on all three lanes with `headSha == HEAD` asserted, and
macOS-validated with every measurement blank filled — and Epic 3.5 (Skeletal animation · render) is
OPEN: 3.5.1 is MERGED as PR #80 and PENDING its macOS validation pass.
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
unmeasured. **3.1.5** drag-into-scene (18 commits, 37 seeds, macOS pass PENDING) — the epic's closer,
detailed below. **3.2.1** glTF/fastgltf (PR #70 `f02ca65`, 32 seeds, 12 review findings, ✅ 12/12), the
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
normative §12 and Reserved renumbers §12 → §13**, so §13 is the current Reserved section and its bullet
list now names `.aeroanim` for 3.5.2); the **first mesh
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

**3.1.5 — Drag-into-scene: Epic 3.1 CLOSES with it. COMPLETE IN CODE on
`feat/3.1.5-drag-into-scene` (eighteen commits, tip `78450e0`) and PENDING its sixteen-row macOS
validation pass.** This is where an asset stops being a row in a browser. Everything upstream existed —
GUIDs, an import cache, a browser, five importers, two cooks, materials, and 3.5.1's working
`createMesh`/draw path — and **nothing in a scene file could name any of it**. Sized **S** in the phase
plan and landed at **L**, recorded as such in `docs/tasks/phase-3.md`: the referencing field cannot land
alone. Six layers moved. **`tools/reflect-gen` + `engine/reflect`**: the reflectable subset grows by
exactly **`engine::Guid`** — its first growth since 2.2.2's `std::string` — as one `classifyField` arm,
one `categoryTag` arm and **one `serialize.{hpp,cpp}` overload pair**, because **neither emitter gains a
branch** (overload resolution routes them). The wire form is the canonical 32-lowercase-hex string, and
**nil is a value, not an omission** (`docs/09` §2.3), a deliberate divergence from §11.1 where absence
has its own spelling. **`engine/scene`**: `MeshRenderer` gains `mesh`/`meshIndex`/`material`,
**appended** because declaration order is JSON key order and inspector row order; `sizeof` 16 → **56**,
with four padding bytes **stated rather than removed**. **`engine/scene_render`**: `AssetBindingTable`
(two sorted vectors, never a hash container — MSVC's node containers are not nothrow-movable and this
becomes a `SceneRenderer` member whose move is `noexcept = default`) plus three emission arms in
`buildRenderView`, reached through a **fifth, defaulted, last** parameter so every prior caller is
untouched. **`engine/render`**: two appended `RenderView` counts, **deliberately NOT latched WARNs** —
unresolved is transient by design, so a WARN would fire once per session on correct behaviour.
**`/editor`**: **seven new pairs, sixteen → twenty-three** — `asset_drag` (pure, the payload and the
whole routing matrix), `instantiate_plan` (pure, the **fifth** `localId` consumer), `asset_commands`
(the sixth structural command and the first that creates more than one entity), `scene_asset_ledger`
(pure — decides, never executes), `material_from_import` (pure, the lossless direction
`material_format.hpp` predicted), and the two src-private ones, `scene_asset_loader` and `texture_load`.
**And `LOCAL_MESH_HALF_EXTENT` is DELETED** with all three consumers moving in one commit, because
`picking.hpp` demands the pick box, the frame box and the highlight box never disagree and a
half-migrated state **is** that disagreement; the plane goes **flat**, retiring 2.3.2's knowingly-fat
box, and **no epsilon is added anywhere** — only a precondition change to `box.valid()`.

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

**Named handoffs, each with an owner.** **Animation clips → 3.5.2**, with its seam written down on both
sides rather than guessed at: `render::JointPose` is the clip sampler's output type (which is why the
format stores bind **locals** as TRS — a clip drives T, R and S member-wise), `sourceNodeLocalId` is the
clip→joint binding key, and **`.aeroanim` is reserved by name in `docs/09` §13**; it now also inherits a
scene that **can** name an asset, which is what an `AnimationPlayer` needs, plus the `AERO_ASSET(kind)`
annotation for kind-aware inspector drop targets if it wants it. **The narrowed node-hierarchy residual**
(a cooked model/prefab container for a consumer with **no importer**) → **4.4.4 / Phase 5's pak**.
**Sub-asset identity beyond an index** (3.2.1's D13) → still open with its original trigger: the
reference is `(guid, position)`, so a re-export that **reorders** meshes silently retargets it, and a
content-hash encoding is reachable for whoever needs cross-reorder stability. **Ledger eviction and a
`Library/cooked/` store** → unowned; nothing is evicted today and every dropped model stays resident for
the session, which is why validation rows 12–13 are the only numbers anyone will have. **Per-triangle
picking** → unowned; 3.1.5's box pass is written to be its broadphase. **The storage-buffer palette
unlock** (raising 85) → whoever hits that wall: it needs an rhi surface change, since `BufferUsage` has
`Vertex` and `Index` only, and a fingered humanoid fits inside 85. **`TexCoord1`/`Color0` seats in
`MeshVertex`** (they decode and drop with one latched WARN today) → the first feature needing a second
UV set or vertex colours. **The `UShort4` / `.aeromesh` v2 bundle** → a deliberate bundle, never alone,
because it is a `formatVersion` bump: `Joints0` is `Uint4` only because `rhi::VertexFormat` has no
unsigned-16×4 enumerator, so a skinned vertex pays 8 bytes per vertex it does not need. **Extracting
imported materials to disk** → unowned: 3.1.5 materializes them **in memory** through the normative
`MaterialDocument` type, so no `.aeromat` appears when a model is dropped. Rendered material thumbnails
→ a named deferral, **unowned** (it needs preview readback plus ledger integration, which is why
`isThumbnailDecodable` was deliberately not touched). **BLEND transparency** → a named
**decision-waiting** gap, renderer-only, since the format already carries everything.
**IBL / environment lighting** → a named **unowned** gap blocked on a format decision, not on shader
work: it needs a cubemap, which `docs/09` §10 currently refuses. Shadows → **3.6.2**; tonemap/gamma →
**3.6.3** (output is raw linear until then). A shared token→`SamplerDesc` helper → decided by the
**second** consumer; 3.4.2's `material_edit.hpp` is now the second, and it kept the mapping editor-side
because the sample and the editor are the only two callers. **`reflect-gen` growth (`Vec4`, enums,
optional-wrapped nested structs)** → the **component** tasks that need it (3.5.2 next), never
a panel: 3.4.2's D1 refused it because a generated serializer for `MaterialDocument` would be a
**second writer for a normative on-disk format**, and that reason does not age away even after the
subset grows — `Guid` landing at 3.1.5 is the pattern to copy. Two small ones from 3.4.2, both still
open: `model_import_session.cpp`'s `sourceHashUsable` is now a
duplicate of `assetContentHashUsable` and should be collapsed by whoever next edits that file, and
`RenderTarget::resize() == false` is unreachable from any tier without an injectable allocation
failure — an engine change nobody has needed yet.

**Carried-forward debt, and 3.5.1 and 3.1.5 both add to it.** Seven ticked validation rows across four
tasks were signed off with their measurement blanks empty (3.2.5 rows 3, 8, 9, 11, 13; 3.2.2 row 9;
3.2.4 row 12) — each row's *behaviour* passed, each row's *evidence* is absent, so **R4 and R8's
in-editor half stay unmeasured** and D9's centimetre-versus-metre comparison has no recorded figures.
**None of Epic 3.3's or Epic 3.4's five tasks is among those seven** — all five had their
number-bearing rows written so a blank tick is impossible, which is the pattern the other four should
be brought up to rather than the exception, and 3.5.1's twelve-row page and 3.1.5's sixteen-row page are
both written the same way (3.5.1's rows 8–12 and 3.1.5's rows 5 and 11–13 each carry their blank in
bold). Separately: **no Windows or Linux validation pass exists for any
of the thirteen Phase 2 tasks, for 3.1.1–3.1.4, for 3.2.1–3.2.5, for 3.3.1–3.3.3, or for 3.4.1/3.4.2**,
and Phase 0's gate is still held open on Windows/Linux 60 fps sign-off. **3.5.1 grows that debt by one
more task, but by less than a full task's worth**, and the difference is worth stating: CI already
covers its sharpest cross-platform half, because `SN3` compiles and draws the skinned pipeline under
**WARP and lavapipe** on every push — which is exactly where the tree's first `uint4` vertex attribute
and first two-UBO vertex stage could have diverged. What the lanes do **not** cover is what a
validation pass is for: the picture. That is platform-validation debt spanning four phases, and with
macOS otherwise green it is the whole of the remaining validation risk. **3.5.1's own macOS pass is NOT
YET RUN** — the page exists, written before the pass as always, and until it is run its rows 4, 5 and 6
are four declared shader-only seeds' (S33–S36) only coverage *in principle* rather than in fact.
**3.1.5's sixteen-row pass is NOT YET RUN either, and its debt is the one CI cannot narrow at all**: the
three lanes compile and run every tier-0 and GPU case it ships, but **no lane performs a mouse
gesture**, so its five declared seeds (N1–N5) are as uncovered on Windows and Linux as on macOS until a
pass runs. **The one seed still uncovered by any pass anywhere is 3.4.2's S26**, which no Apple platform
can observe.

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux on-hardware 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE** — epics 1.1–1.4 all CLOSED. Gate reached in code, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics (2.1 Editor shell, 2.2 Core panels, 2.3 Manipulation, 2.4 Undo/redo, 2.5 Scene I/O, 2.6 Project system v0) CLOSED in code and macOS-validated PASS, with Windows/Linux rows pending for every task (`editor/VALIDATION.md`). The whole-phase audit (2026-08-02) fixed two silent data-loss paths, a project root that was never made absolute, two CI false-greens, and four stale documentation claims. Full per-task and per-epic history — every defect, every sabotage matrix, every deviation — lives in `docs/10-engineering-log.md`'s Phase 2 entries; this row is deliberately a summary, not a duplicate. |
| **Phase 2 gate** | **MET 2026-08-02.** `samples/phase-2-editor-scene/` holds a project and a 4-entity scene authored entirely through the editor, with the save → New Scene → Open Scene round trip confirmed (`samples/phase-2-editor-scene/VALIDATION.md`); provenance is recorded there rather than asserted, since a hand-written `scene.json` is byte-identical to a real one and no test tier can tell them apart. Deliberately NOT `add_subdirectory`'d — this artifact is data (a provenance proof of the editor), not a compile-proof of engine code. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** **Epic 3.1 is CLOSED — all five tasks landed.** 3.1.1/3.1.2/3.1.3/3.1.4 (PRs #65/#66/#67/#69), CI-green on all three platforms, sabotage-proven (26/31/35/25 seeds), macOS-validated ✅ PASS 14/14, 14/14, 16/16, 10/10 — Windows/Linux rows pending for all four. `engine::Guid`/`engine::ContentHash`, the `.meta` v1 format, `AssetDatabase::rescan`'s eight phases, the machine-local `Library/asset-cache.json` import cache and the real Asset Browser all shipped across them. **3.1.5 (drag-into-scene) closes the epic: COMPLETE IN CODE on `feat/3.1.5-drag-into-scene` (18 commits, tip `78450e0`) and PENDING its sixteen-row macOS pass.** Sized S, landed L (recorded in `docs/tasks/phase-3.md`): the reflectable subset grows by `engine::Guid` — its first growth since 2.2.2 — `MeshRenderer` gains `mesh`/`meshIndex`/`material` (`sizeof` 16 → **56**), `engine/scene_render` gains the `AssetBindingTable` and three emission arms, `RenderView` gains two deliberately-unlatched counts, `/editor` gains **seven pairs (sixteen → twenty-three)**, and `LOCAL_MESH_HALF_EXTENT` is **deleted** with the plane going flat. It also **decides** the node-hierarchy gap `docs/09` §9.0 has carried since 3.3.1 — entities, not a container — leaving a narrower residual owned by 4.4.4 / Phase 5's pak, with `engine/assets` byte-identical. Mechanical gate: **147/147 on both macOS presets** with `AERO_REQUIRE_GPU=1`; `ctest -N` **147 / 55 / 68**, an asymmetry of **+3 / +0 / +0** that is D19's prediction being met — the three new entries are the gated `reflect-gen.guid_*` cases, absent by design from both reduced configurations — while every other new case rides an existing binary; doctest across **seven** binaries (the first task to track `aero_reflect_meta_test` and `aero_reflect_json_test`) **822 / 1709 / 137 / 27 / 26 / 7 / 28**, +190 cases; six guards exit 0 (math-boundary **363 → 387**, project-no-delete Check B **65 → 72**, `PERMITTED_DELETERS` unchanged and none of the seven new TUs in it); `.github/scripts/` byte-identical. A **37-seed** matrix ran in two halves — nineteen during implementation, nineteen as an independent pass — and **not one seed reddened nothing**; six amendments and corrections are recorded, two defects were found by reading and by tests rather than by seeding, and five seeds (N1–N5) have no automated witness anywhere. `engine/core`, `engine/assets`, `engine/platform`, `engine/rhi`, `engine/scene_serialize`, `runtime/`, `shaders/`, `vcpkg.json`, `cmake/` and `.github/` are byte-identical; **no dependency of any kind lands**. **Epic 3.2 (Importers) is CLOSED IN CODE — five merged tasks**, one canonical in-memory `ImportedModel` and eight claimed extensions: 3.2.1 glTF/fastgltf (PR #70 `f02ca65`, ✅ 12/12), 3.2.2 FBX/ufbx (PR #71 `c597a5b`, ✅ 13/13), 3.2.3 OBJ/tinyobjloader (PR #72 `c412e83`, ✅ 13/13), 3.2.4 Blender CLI/`.blend` (PR #73 `5ab07f3`, ✅ 15/15), 3.2.5 Assimp DAE/PLY/STL (PR #74 `7e0224f`, ✅ 14/14). Windows/Linux rows pending for all five, and seven of their ticked rows are missing the measurement they asked for. **Epic 3.3 (Cooker v0) is CLOSED — three merged tasks, every one CI-green on all three lanes with `headSha == HEAD` asserted, and every one macOS-validated with every measurement blank filled**: 3.3.1 Mesh cook → GPU buffers (PR #75 `17a6821`, 13 commits, 42 seeds, ✅ 12/12) opened `engine/assets` and `tools/cooker` and produced the tree's first binary format and first runtime-consumable artifact; 3.3.2 Texture cook → KTX2/Basis (PR #76 `cf8575a`, 15 commits, 53 seeds, ✅ every row) added the KTX2 subset container, the texture cook and the two integer block encoders, and is the first artifact this project produces that a third-party tool can open — `ktx validate` 4.4.2 PASS on all eight artifacts, proven non-vacuous by re-seeding the corrected DFD byte and watching the validator reject it with the exact predicted `error-6028`; 3.3.3 Cook determinism golden test (PR #77 `234a009`, 8 commits, 24 seeds, ✅ 13/13) ships zero C++ and turns cross-lane, cross-config and cross-time byte-identity into a continuous CI check. Windows/Linux rows pending for all three. **Epic 3.4 (PBR materials) is CLOSED — both tasks merged, CI-green on all three lanes with `headSha == HEAD` asserted, and macOS-validated with every measurement blank filled.** **3.4.1 (Material asset + PBR shader) is MERGED as PR #78 (merge commit `a01765d`, 10 commits), CI-green on all three lanes with `headSha == HEAD` asserted before the merge, and macOS-validated ✅ PASS on ALL 11 ROWS (2026-08-16) with every measurement blank filled — 6 cooked textures upload in 6.2 ms (mean 1.0 ms), the sample holds ~121 fps, and the sidecars read 5/2 and 0/1.** It made a cooked texture mean something: the first cooked texture ever drawn on a GPU here, the first `.aeromat` parsed end to end, the first asset resolved at run time by GUID, six BC formats and `textureLevelByteSize` in `engine/rhi` (recorded per the 0.4.1 D18 protocol), `docs/09`'s normative §11, the `MaterialHandle` registry, and the GGX shader pair rewritten in place. **3.4.2 (Material inspector editing) is MERGED as PR #79 (merge commit `3aebbad`, 15 commits), CI-green on all three lanes with `headSha == HEAD` asserted before the merge, and macOS-validated ✅ PASS on ALL 12 ROWS (2026-08-16) with every measurement blank filled — Apply echoes +1 and Create +1 (not +2), the panel first-opens inside a single vsync-paced frame, and the worst committed fixture uploads in 2.1 ms.** It is the task that makes materials editable rather than hand-authored: four editor pairs (`material_edit` pure, `material_session` GPU-free, `material_panel` the only new ImGui TU, `material_preview` the only new GPU TU), `AssetKind::Material`, the eighth panel (id `"Material"`, FROZEN, right dock, registered last), a live preview with its own `RenderTarget` and its own `ForwardRenderer` that gives `updateMaterial` its **first production call site**, slot textures through the real decode → cook → parse → upload chain, and a New Material button. **Two recorded deviations**: `aero::render` joins `aero_editor_core`'s PUBLIC link group (the spec's "no link-line change" and its own AC-25 cannot both hold, since `aero::scene_render` is PRIVATE), and `MaterialParseResult` gains a `warnings` vector under D11's own escape hatch — so **AC-34 is amended and `engine/` is NOT byte-identical**, the diff being exactly `engine/reflect/{include/aero/reflect/material_format.hpp, src/material_format.cpp}`, with `tools/`, `shaders/`, `runtime/`, `samples/`, `vcpkg.json`, `.github/`, `cmake/` and the determinism manifest all byte-identical and **no dependency of any kind**. Mechanical gate green: **133/133 on both macOS presets** with `AERO_REQUIRE_GPU=1`; fresh `-G Ninja` reduced configurations **44/44** and **57/57**; `ctest -N` **133 / 44 / 57 — unmoved**, because every new test rides an existing binary and each editor test binary is ONE ctest entry, so the growth reads only in the doctest totals (**716 / 1577 / 124 / 23 / 22**; `aero_tests` 713 → 716 is a plan-recorded surprise caused by the reflect deviation); six guards exit 0 (math-boundary **347**, project-no-delete Check B **64**); clang-format and clang-tidy clean by exit code. A **30-seed** matrix ran to completion with **two genuine gaps**, both closed **structurally** and re-proven by re-seeding, and six of the plan's own witness attributions corrected. A code-review round found **eleven gaps, two blocking**, all closed — including a use-after-free that is deterministic on Vulkan and D3D12 and benign on Metal. **Epic 3.5 (Skeletal animation · render) is OPEN, and 3.5.1 (Skeleton & GPU skinning) is MERGED as PR #80 (merge commit `c3a2bc7`, fifteen commits), CI-green on all three lanes with `headSha == HEAD` asserted before the merge, and PENDING its macOS validation pass.** It builds the whole missing half between the importers and the GPU: the **first `.aeroskel`** (`docs/09` gains normative **§12**; Reserved renumbers §12 → **§13** and gains `.aeroanim` for 3.5.2), the tree's **first mesh registry**, the **first integer vertex attribute** and **first two-UBO vertex stage**, and **pipelines 2 → 4**. Two `engine/assets` pairs (`cooked_skeleton`, `skeleton_cook`), one editor pair (`skeleton_cook_source`, the **sixteenth**, and the fourth consumer of the `localId` rule), four `engine/render` files (`skinning.{hpp,cpp}` + src-private `skinning_pack.hpp`/`mesh_pack.hpp`) plus the registry inside `ForwardRenderer`, one new shader, one cooker subcommand, and `samples/phase-3-skinning`. **`GLTF_IMPORTER_VERSION` moves 1 → 2** because `--no-skins` finally means something for glTF, and the machine-local import cache re-imports every `.gltf`/`.glb` once per machine and nothing else. Mechanical gate green: **144/144 on both macOS presets** with `AERO_REQUIRE_GPU=1`; fresh `-G Ninja` reduced configurations; `ctest -N` **144 / 55 / 68**, +11 in **all three** configurations in lockstep (the eleven ungated `cooker.*` cases; every other new case rides an existing binary), doctest **776 / 1594 / 124 / 23 / 22**; six guards exit 0 (math-boundary **363**, project-no-delete Check B **65**); clang-format and clang-tidy clean by exit code. The determinism manifest grows **13 → 15 lines / 26 → 30 cross-lane comparisons**, `ktx validate`'s 8 unchanged, with all 13 existing hashes byte-identical. **`engine/rhi`, `engine/scene*`, `engine/reflect`, `engine/platform`, `engine/core`, `runtime/`, `vcpkg.json`, `cmake/` and `.github/scripts/` are byte-identical; no link line moves anywhere; no dependency of any kind lands.** A **36-seed** matrix ran to completion with **three genuine gaps**, all closed structurally and re-proven by re-seeding, six of the plan's own witness attributions corrected, and four declared shader-only seeds (S33–S36) whose only coverage anywhere is validation rows 4, 5 and 6. A **code-review round found five gaps, none blocking, all closed** — the shader's own copy of the 85-joint cap had nothing tying it to the C++ constant (now `JP14`, a comment-stripped source-text pin through a new `AERO_SHADERS_SRC_DIR`); `createMesh` ignored `packMeshSection`'s empty-stream refusal signal and recorded a section offset that pointed at the NEXT section's data; the stale-handle WARN was the one unlatched diagnostic in the draw loop; nothing drew a static and a skinned instance in one view; and the cooked-assets rule's "never a memory error" justification for unvalidated index values went false the moment a GPU consumed them through `drawIndexed`. **The sharpest lesson is about the test, not the code**: `SN8`'s first version stayed GREEN under the very defect it was written for — a static instance silently inheriting the skinned pipeline moves neither `skinnedDrawCount()` nor Metal's validation, so a third diagnostics accessor, `pipelineBindCount()`, was added to make pipeline TRANSITIONS observable, and re-seeding now reddens exactly that one line at 1 instead of 2. Full detail for every task in `docs/10-engineering-log.md`'s Phase 3 entries. |
| **Next task** | **~~3.4.1~~ / ~~3.4.2~~ DONE, MERGED and macOS-VALIDATED** (PRs #78 `a01765d` and #79 `3aebbad`, ✅ 11/11 and 12/12); **3.4.2's S26 remains uncovered by any pass and cannot be covered from macOS**. **~~3.1.5~~ is COMPLETE IN CODE and CLOSES Epic 3.1** (18 commits, tip `78450e0`). **Two validation passes are now outstanding and both are the immediate next step**: 3.5.1's twelve-row pass (`editor/validation/3.5.1-skeleton-gpu-skinning.md`) — rows 4, 5 and 6 are the only coverage S33–S36 have anywhere, and rows 7 and 8 need locally-generated content that is deliberately not committed, driven through the sample's `argv[1]` override — and **3.1.5's sixteen-row pass** (`editor/validation/3.1.5-drag-into-scene.md`, written before the pass): rows 3, 4, 9 and 10 are the only coverage N1–N5 have anywhere, rows 5 and 11–13 carry measurement blanks in bold, and rows 12–13 need a **Mixamo-class textured FBX that is deliberately never committed** and must be downloaded locally first. After that: **(a) 3.5.2 (animation clips)**, unblocked with its seam already written down — `render::JointPose` is the sampler's output type, `sourceNodeLocalId` is the clip→joint binding key, `.aeroanim` is reserved by name in `docs/09` §13 — and now additionally handed a scene that **can** name an asset, which is what an `AnimationPlayer` needs, plus the `AERO_ASSET(kind)` annotation if it wants kind-aware inspector drop targets. And **(b) the seven ticked-but-unmeasured validation rows**, plus the four-phase Windows/Linux platform-validation debt, which with macOS otherwise green is the whole of the remaining validation risk — and which **3.1.5 widens in the one way CI cannot narrow**, since no lane performs a mouse gesture. **A note for whoever adds a fixture or a cook change next**: `tests/cooker/determinism.sha256` is FROZEN at **15 lines across three arms**, a red manifest case is `docs/09` §9.11's `cookerVersion` sentence firing, and the regeneration ritual lives in the manifest's own header — never edit a hash to green a red run. See `docs/tasks/phase-3.md`. |

Engine layers that exist today, in dependency order: `core` (gained `guid.hpp`/`guid.cpp` at task
3.1.1, beside `handle.hpp`; gained `content_hash.hpp`/`content_hash.cpp` at task 3.1.2, beside `guid`)
→ **`assets`** → `platform` → `rhi` → `render` → `reflect` → `scene` → `scene_render` →
`scene_serialize`, plus `/editor` (`aero_editor_core` + `aero_editor`) and `/tools` (`reflect-gen`,
`shaderc`, **`cooker`**). `/runtime` is still empty — it arrives in Phase 5.
**`engine/assets/` OPENED at task 3.3.1** and its `.gitkeep` is gone. The old reason for keeping it
shut — "unopened until a **runtime** consumer exists (Phase 5's pak table)" — was satisfied by the
task itself: the `.aeromesh` container's *reader* **is** a runtime component by definition, so the
consumer in question is the thing being built. It holds the cooked-asset formats and nothing else — **seven pairs since task
3.5.1**: `cooked_mesh.{hpp,cpp}`, `mesh_cook.{hpp,cpp}`, `cooked_texture.{hpp,cpp}`,
`texture_cook.{hpp,cpp}`, `bc_block.{hpp,cpp}`, `cooked_skeleton.{hpp,cpp}` and
`skeleton_cook.{hpp,cpp}`; it links `aero::core` + `aero::profiling` and **no
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
**Task 3.1.5 touches four engine subsystems, and it is the first task to touch `engine/scene` since
2.4.2.** `engine/scene`: `mesh_renderer.hpp` gains three appended fields (`sizeof` 16 → **56**, with
four padding bytes stated in the `static_assert` rather than removed). `engine/reflect`:
`serialize.{hpp,cpp}` gain one `writeJson`/`readJson` overload pair for `engine::Guid` — the read side
declared **above** `readField`, because ADL for `engine::Guid` searches `engine`, not
`engine::reflect`. `engine/scene_render`: the new `asset_bindings.{hpp,cpp}` pair (the subsystem's
second source file, one `CMakeLists.txt` line), three emission arms and a `bindings()` accessor.
`engine/render`: **two appended `RenderView` counts and nothing else at all**. **`engine/core`,
`engine/assets`, `engine/platform`, `engine/rhi` and `engine/scene_serialize` are byte-identical**, as
are `runtime/`, `shaders/`, `vcpkg.json`, `cmake/`, `.github/` and `tools/cooker`; the only `tools/`
diff is `reflect-gen`'s subset arm. **No link line moves anywhere and no dependency of any kind
lands** — `aero_scene_render` already linked `PUBLIC aero::scene aero::render`, and
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
**3.1.5 adds SEVEN pairs, taking the count to TWENTY-THREE** (re-counted at task end, never added to
the remembered number), split by **dependency** the way 3.4.2's four were: `asset_drag.{hpp,cpp}`
(pair 17, PUBLIC and PURE — the payload, the decode and the whole accept/refuse matrix; names no ImGui
type and calls no ImGui function), `instantiate_plan.{hpp,cpp}` (18, PUBLIC and PURE — the
`ImportedModel` → entity-subtree planner and the **fifth** named `localId` consumer),
`asset_commands.{hpp,cpp}` (19, PUBLIC — the sixth structural command, the first creating more than
one entity), `scene_asset_ledger.{hpp,cpp}` (20, PUBLIC and PURE — decides, never executes),
`material_from_import.{hpp,cpp}` (21, PUBLIC and PURE — `ImportedMaterial` → `MaterialDocument`),
`scene_asset_loader.{hpp,cpp}` (22, src-private — the only TU that mints a `MeshHandle` here) and
`texture_load.{hpp,cpp}` (23, src-private — the decode → cook → parse → upload chain **extracted**
verbatim from `MaterialPreview`, with zero test edits). It also deletes `LOCAL_MESH_HALF_EXTENT` and
`selection_overlay.cpp`'s duplicate corner enumeration, promotes `captureAndDestroySubtrees` /
`restoreStructuralState` onto `entity_commands.hpp` and `blendExportSettingsFingerprint` onto
`blender_tool.hpp`, and adds no new panel. The `.hpp`s live under
`editor/include/aero/editor/` (except those named src-private, which live beside their `.cpp` in
`editor/src/` — **23** tracked `editor/src/*.hpp`, up from 21 for 3.1.5's two src-private headers),
the `.cpp`s under `editor/src/` (**72**, up from 65).

Test inventory on `feat/3.1.5-drag-into-scene`, measured at `32b2d1f` (the one later code commit adds an `#else` arm inside an existing case and moves no count), every number
**re-measured there, never derived by addition and never carried forward from an earlier step or an
earlier task** — read the totals from doctest's own `filters:` line, never from a `grep -c` of case
names. **`ctest -N` reads 147 / 55 / 68** — tools ON, then
`-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, then `-DAERO_REFLECT_TOOLS=OFF` alone. **The
reason matters more than the number**: `aero_tests`, `aero_editor_shell_test` and
`aero_editor_imgui_test` each register with ctest as a **single entry** (`tests/CMakeLists.txt`), so
3.4.1's 57 new doctest cases, 3.4.2's 84, 3.5.1's 74 and 3.1.5's **190** move it not at all, and
samples register no test. **Until 3.1.5 the triple moved only for `cooker.*` cases**
(**117 → 131 → 133 → 144**, **28 → 42 → 44 → 55**, **41 → 55 → 57 → 68**), because `aero_cooker` takes
**no gate flag** and every one of its cases is registered in every configuration — 3.5.1's +11 was
therefore **identical in all three**, and that lockstep was itself an assertion. **3.1.5 is the first
task whose move is deliberately ASYMMETRIC: +3 / +0 / +0**, the three being the gated
`reflect-gen.guid_components` / `guid_meta` / `guid_json` cases, which live inside
`if(AERO_REFLECT_TOOLS)` and are **absent by design** from both reduced configurations. **The flat
reduced pair is the prediction being met, not a missed registration** — read the two kinds of move
differently: a `cooker.*` addition must be identical in all three, and a `reflect-gen.*` addition must
be tools-ON only. **A future gate flag on the cooker would silently shrink the reduced configurations'
coverage with no test able to report it.** An unmoved `ctest -N` means "zero C++" for task 3.3.3 and
means nothing of the kind for a task that grows an existing binary — check a zero-C++ claim against the
**doctest** totals instead.

**Doctest, SEVEN binaries: 822 / 1709 / 137 / 27 / 26 / 7 / 28.** The tracked list read **five** until
this task, because `aero_reflect_meta_test` and `aero_reflect_json_test` both sit inside
`if(AERO_REFLECT_TOOLS)` blocks and are absent from the reduced configurations — but they are real
binaries with real cases, and 3.1.5 is the first task to add to them since 1.2.2. They are tracked from
here on. `aero_tests` **776 → 822** (+46: `SJ1`–`SJ10` on the `Guid` overload pair, `AB1`–`AB14` on the
binding table and `BR1`–`BR22` on `buildRenderView`'s three arms, across two new TUs).
`aero_editor_shell_test` **1594 → 1709** (+115: `DR1`–`DR18`, `PL1`–`PL21`, `IA1`–`IA15`, `MF1`–`MF20`
and `LG1`–`LG24` across five new TUs, plus `PK`/`LB`/`VP` growth in three existing ones).
`aero_editor_imgui_test` **124 → 137** (the `SL*` loader and `DP*` drop integration cases).
`aero_scene_serialize_test` **23 → 27** (the four §2.3 tolerance rows on the three new keys).
`aero_editor_inspector_test` **22 → 26** (the Guid field row, `IR6`/`IR7`).
`aero_reflect_meta_test` **4 → 7** and `aero_reflect_json_test` **23 → 28** (`GD1`–`GD8`).
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
`aero_editor_core` sources **64 → 71** and tracked `editor/src/*.cpp` **65 → 72** (3.1.5's seven pairs).
`check-math-boundary.sh`'s scanned count **363 → 387** (+24 tracked C-family files: 14 `editor`, 2
`engine/scene_render`, 7 `tests` and 1 `reflect-gen` fixture), re-measured
**after `git add`**, since `git ls-files` sees only tracked files. `check-project-no-delete.sh`'s
Check B scan reads **65 → 72**, its glob
picking the new files up automatically — **neither script changes, and `.github/scripts/` is
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
**3.1.5 commits one new fixture, `tests/reflect-gen/fixtures/component_guid.hpp`** (annotation-free, a
`Guid` beside a `uint32` and a `Vec3`, so the new category is proven to **coexist** with the old
subset) and hand-edits two existing goldens — `tests/fixtures/scenes/full.scene.json` gains the three
new keys on **both** `MeshRenderer` payloads, one arm defaulted and one carrying a **non-nil** guid
pair with `meshIndex 3`, and `samples/phase-1-scene/scene.json` is re-emitted through the engine's own
writer. **That non-nil arm is load-bearing**: with all-nil values `G2` cannot see an uppercasing writer
at all, because `toupper('0') == '0'`.
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
