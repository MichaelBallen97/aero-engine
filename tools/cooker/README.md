# tools/cooker

`aero_cooker` — a first-party CLI that turns one source model into one cooked `.aeromesh` container:
interleaved vertex blobs at GPU stride, one index buffer at the narrowest width that fits, and an
axis-aligned box per submesh plus one for the model. It is the third first-party tool, after
`reflect-gen` and `shaderc`, and the first that produces a **runtime-consumable artifact** — a file
whose whole design premise is that the thing reading it does no parsing at all.

**Task 3.3.2 added its second artifact kind**: `aero_cooker texture` turns one source image into one
KTX2 container — BCn blocks, a gamma-correct integer mip chain, and a strict subset of the Khronos
format, so `ktx info`, `ktx validate` and RenderDoc open what it writes.

This document is the permanent home of the frozen contracts these tasks promise (3.3.1 and 3.3.2). The
normative specification of the containers themselves is `docs/09-file-formats.md`; this file specifies
the *tool*.

## Why it links the editor

The tool needs an `ImportedModel`, which means it needs `importModel()`, which lives in
`aero::editor_core` alongside the five importer paths Epic 3.2 built. Re-implementing a glTF parser
inside this tool was rejected outright: it would be a second parser for the format ADR-003 designates
canonical, and it would be wrong for the other seven claimed extensions on the day it shipped. The
**image decode** arrives the same way and for a sharper reason: stb_image is a vcpkg package, and
`engine/assets` links no vcpkg package at all — which is exactly what makes that target's `PRIVATE`
links a real compile-time boundary. So the decode cannot live beside the cook, and it lives in
`aero::editor_core` instead, in one pure adapter pair with no UI.

That is legal rather than an exception carved for this task. `tools/` sits outside the golden rule on
**both** halves: `.github/scripts/check-golden-rule.sh` scans `engine` and `runtime`, and
`aero_assert_golden_rule`'s `CONSUMER_DIRS` in the root `CMakeLists.txt` is the same pair.
`aero_cooker` is enumerated by neither.

**The accepted, stated cost:** ImGui, SDL3, fastgltf, ufbx, tinyobjloader and assimp all reach this
binary's link line through `aero_editor_core`'s `PRIVATE` links, which become `$<LINK_ONLY:>` on a
static library — so the archives arrive while their include directories do not. The tool initializes
none of them, opens no window and creates no GPU device. If that ever bites, the fix is splitting the
importer translation units into their own ImGui-free target, a self-contained refactor that changes no
consumer. It is not this task's change.

## The four frozen contracts

### 1. The mesh grammar

```
aero_cooker mesh --input <file> --output <file.aeromesh>
                 [--guid <32 hex>] [--scale <float>]
                 [--no-materials] [--no-animations] [--no-skins]
aero_cooker --version
aero_cooker --help
```

- **Subcommand-shaped from day one**, and task 3.3.2 kept that promise: `texture` was added with no
  reshuffle of anything above. A missing or unknown subcommand is a usage error naming the token and
  listing both real ones.
- `--input` and `--output` are both **required**. `--output` names a **file**, not a directory.
- **Every flag may be given at most once.** Unlike `aero_shaderc --define`, this grammar has no
  repeatable flag at all, so the rule is uniform.
- `--guid` takes **exactly 32 hex digits**, any case, and nothing else. A dashed or braced value is a
  usage error, never a silently normalized success. Absent, the container records the **nil** GUID,
  which is legal and deterministic. The tool never probes for a sibling `.meta`: that would make its
  output depend on a file it was not given.
- `--scale` takes one finite decimal number, fully consumed. **Zero and negative are accepted** — the
  editor's own import widget honours a hand-edited zero or negative scale and never clamps, and this
  tool must not be stricter than the editor it mirrors. Only a non-finite value (`nan`, `inf`) is
  refused, because a NaN scale would silently make every cooked position and every cooked bound NaN.
- Exit codes: `0` success, `1` usage error, `2` import or cook error, `3` I/O error. Diagnostics go to
  **stderr only**; stdout is reserved for `--help`/`--version`.

There is no `--platform` flag and no platform field in either artifact. Mesh cook output is
platform-independent outright. Texture cook output is **not** — BCn is a desktop profile and
ASTC/ETC2 are not — but v1 emits exactly one profile, so a flag with one legal value would be a
promise the tool cannot keep. It arrives at task 6.3.1 with the second profile, alongside the
`supercompressionScheme` and `vkFormat` values that would make it mean something.

### 2. The texture grammar

```
aero_cooker texture --input <file> --output <file.ktx2>
                    (--srgb | --linear)
                    [--guid <32 hex>] [--format bc1|bc3|bc4|bc5|rgba8|auto] [--no-mips]
```

- **`--srgb`/`--linear` is MANDATORY and there is no default.** Exactly one, and giving neither or
  both is a usage error naming both flags. Every default is wrong for some large class of textures —
  sRGB for every normal, roughness, metallic and mask map; linear for every base-colour and emissive
  map — and unlike most wrong defaults this one produces an image that still *looks* like a texture,
  just too dark or too washed out, so it survives review and ships.
- **The colour space is not a flag in the artifact; it is the format.** There is no `bool srgb`
  anywhere below this tool: `--srgb --format bc1` becomes `VK_FORMAT_BC1_RGB_SRGB_BLOCK` and
  `--linear --format bc1` becomes `..._UNORM_BLOCK`, and the file's Data Format Descriptor is derived
  from that one enumerator. "An sRGB normal map" is therefore not a runtime error, not a warning and
  not a silently-ignored flag — it cannot be spelled.
- **`--srgb` with `--format bc4` or `--format bc5` is a usage error**, for the same reason: Vulkan
  enumerates 139 `BC4_UNORM`, 140 `BC4_SNORM`, 141 `BC5_UNORM`, 142 `BC5_SNORM` and defines no sRGB
  variant of either.

| `--format` | Result |
|---|---|
| `auto` (the default) | **BC3 iff any texel's alpha is below 255, else BC1**, in the requested colour space. It never answers `bc4`, `bc5` or `rgba8`: those encode *intent*, which pixels cannot reveal — a texture whose green and blue happen to equal red in one asset is not a single-channel texture. |
| `bc1` | `BC1_RGB_UNORM` / `BC1_RGB_SRGB`. Alpha is discarded; the encoder always emits opaque four-colour blocks. |
| `bc3` | `BC3_UNORM` / `BC3_SRGB`. A BC4 alpha block then a BC1 colour block, in that order. |
| `bc4` | `BC4_UNORM`, red only. Linear only. |
| `bc5` | `BC5_UNORM`, red then green. Linear only. |
| `rgba8` | `R8G8B8A8_UNORM` / `R8G8B8A8_SRGB`, uncompressed. Level 0 is the decoded input byte for byte. |

- **`--no-mips` emits level 0 only.** Otherwise the chain is `floor(log2(max(w,h))) + 1` levels — the
  complete pyramid, never a partial one. The filter is gamma-correct (an `*Srgb` format averages in
  linear light and re-encodes), **integer throughout**, and polyphase, so an odd dimension does not
  shift the image by a fraction of a texel per level.
- `--guid` behaves exactly as it does for `mesh`, and is written into the container's
  `AeroSourceGuid` key as 32 lowercase hex characters plus a NUL — **unconditionally**, including for
  the nil GUID, so the layout never depends on whether one was supplied.

### 3. The artifact rule

**Nothing is written unless the whole cook succeeded.** The output file is opened only after `cookMesh`
or `cookTexture` has returned with a complete byte vector, so a failing input leaves **zero** artifacts
— never a partial one, never a stale one, and never the `.aero-tmp` file the atomic write would have
created on its way to the final name.

**The tool creates no directory.** `--output` inside a directory that does not exist is exit `3`.
`aero_shaderc` does create its `--output-dir`, and the difference is deliberate: that flag names a
*directory*, this one names a *file*, and a build-time tool that invents directories is how a typo
becomes a mystery tree.

**The output is deterministic.** The same input cooks to the same bytes across two runs, three
toolchains and any ordering of the model's own primitives. No timestamp, no path, no hostname, no user
name and no build id reaches either container; the only provenance fields are the cooker version (a
compile-time constant) and the GUID the caller supplied. The texture path adds two guarantees the mesh
path did not need: **no floating-point arithmetic anywhere in `engine/assets`**, so FMA contraction and
x87 excess precision cannot reach the output, and **no runtime table generation**, so no libm
implementation can either. Both exist because the same bytes must fall out of clang on arm64, MSVC and
GCC.

### 4. The extension tables

`mesh`:

| Extension | Result |
|---|---|
| `.gltf` `.glb` `.fbx` `.obj` `.mtl` `.dae` `.ply` `.stl` | Accepted — the five importer paths Epic 3.2 built. |
| `.blend` | **Refused, by design**, with exit `2` and a message naming the editor's conversion path. Converting a `.blend` means running Blender, and **this tool spawns no process, ever** — that is the editor's job (task 3.2.4). |
| anything else | Refused with exit `2` and a message naming the extension. Nothing is read. |

`texture`:

| Extension | Result |
|---|---|
| `.png` `.jpg` `.jpeg` `.tga` `.bmp` `.gif` `.psd` | Accepted — stb_image's decode, through the editor's adapter. |
| `.hdr` | **Refused, by design**, with exit `2` and a message naming the reason. `stbi_load` does *not* fail on a Radiance file: it silently applies a fixed gamma-2.2 tone map and hands back 8-bit LDR bytes, so cooking one produces a plausible artifact that is quietly wrong. HDR belongs to BC6H, which v1 does not cook. |
| `.ktx2` `.dds` | Refused with exit `2`. Neither is stb-decodable, and re-cooking a cooked artifact is not a workflow. |
| anything else | Refused with exit `2` and a message listing the seven claimed. Nothing is read. |

The extension is tested against the file **name** before a single byte is read, for both subcommands.
`readFileBytes` refuses an over-cap file *without opening it*, so a tool that read first would answer a
300 MB `.hdr` with "too large" instead of "HDR is not supported", and an over-cap or missing `.blend`
with an I/O complaint instead of the message that helps.

The texture read cap is **64 MiB**, matching the editor's own thumbnail source cap rather than the
256 MiB model cap: it bounds the *compressed source file*, while the decoded pixel count is bounded
separately and per-axis at 16384 texels.

## What the flags actually change in the artifact

Genuinely asymmetric, and measured rather than assumed — this is the part that cannot be guessed:

| Flag | Effect on the cooked artifact |
|---|---|
| `--scale <f>` | **Real.** The importer applies it to positions and to root node translations, so cooked positions **and** cooked bounds change. The cook itself applies nothing — it converts nothing at all. |
| `--no-materials` | **Real.** Every cooked submesh's `materialIndex` becomes `0xFFFFFFFF`. |
| `--no-animations` | **None**, for every backend. v1 cooks geometry only. |
| `--no-skins` | **None for the glTF path**: `gltf_import.cpp` reads `JOINTS_0`/`WEIGHTS_0` unconditionally in its mesh phase, and `importSkins` gates only the skin *table*, which v1 does not cook. Other backends may differ; this row is deliberately not phrased as a symmetric claim. |
| `--srgb` / `--linear` | **Real, and in three places at once.** It selects the `vkFormat`, the descriptor's `transferFunction`, and whether the mip filter averages in linear light. All three are derived from the one format enumerator, so they cannot disagree. |
| `--format <token>` | **Real.** It selects the `vkFormat`, the descriptor, the bytes per block, the level alignment and therefore almost every offset in the file. |
| `--no-mips` | **Real.** `levelCount` becomes 1 and the file is strictly smaller; level 0's bytes are unchanged. |

## What v1 deliberately does not do

- **No upload, no pipeline, no draw.** Nothing here names a GPU type; the tool opens no device.
- **No welding, no dedup, no vertex-cache optimization, no quantized attributes.** Source vertex order
  is preserved exactly.
- **No conversion of any kind** — no axis flip, no winding reversal, no unit scaling, no handedness
  change, no normal renormalization, no tangent orthogonalization, no UV flip, and no setting for any
  of them. Every conversion this pipeline performs already happened inside the importer, per format.
- **No node hierarchy, no materials, no images, no skeletons and no animation** in the container. v1
  stores geometry only, so a consumer that instantiates a cooked mesh with no hierarchy puts every
  submesh at the origin. A cooked model/prefab container carrying the node tree is the right answer
  and belongs to whoever owns instantiation.
- **No cook-on-import in the editor**, no `Library/Cooked/`, no panel and no menu item. This tool is
  the only thing that cooks today; task 3.3.2 added exactly one pure adapter pair to the editor and
  changed no other editor file.
- **No BC7, no BC6H, no ASTC, no ETC2, no Basis Universal, no supercompression.** `supercompressionScheme`
  is `0`, always, and a non-zero value is a parse refusal. The "Basis" in task 3.3.2's title is the
  ecosystem's name for the family; the encoders are first-party and integer-only, because a
  floating-point encoder forfeits the cross-platform byte-identity above.
- **No cubemaps, texture arrays, 3D textures or partial mip chains**, and no general KTX2 *importer*.
  Interop is one-directional and deliberately so: our files open in any conforming reader, and
  arbitrary third-party KTX2 files do not open in us.
- **No normal-map renormalization, alpha-coverage preservation, channel packing, atlassing,
  swizzling, premultiplied alpha, vertical flip or resize-to-POT** — and no setting for any of them.

## Tests

`tests/cooker/run_case.cmake` drives the real binary through 36 `cooker.*` ctest entries — 22 for
`mesh` and 14 for `texture` — argv in, exit code plus files out. A CLI's honest test is its process
boundary, so there is no doctest translation unit for the tool and no tool code links into
`aero_tests`; the pure halves it is built from (`cookMesh`, `parseCookedMesh`, `meshCookPrimitives`,
`cookTexture`, `parseCookedTexture`, `decodeImageRgba8`, `chooseTextureFormat`) are covered there and
in `aero_editor_shell_test` instead. The cases are registered with **no gate flag**, so they run in
every build configuration. The mesh inputs are listed in `tests/cooker/fixtures/README.md`; the two
texture inputs are the committed `tests/fixtures/assets/texture-rgb-5x3.png` and
`texture-rgba-8x8.png`, shared with the editor suite.
