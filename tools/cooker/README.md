# tools/cooker

`aero_cooker` — a first-party CLI that turns one source model into one cooked `.aeromesh` container:
interleaved vertex blobs at GPU stride, one index buffer at the narrowest width that fits, and an
axis-aligned box per submesh plus one for the model. It is the third first-party tool, after
`reflect-gen` and `shaderc`, and the first that produces a **runtime-consumable artifact** — a file
whose whole design premise is that the thing reading it does no parsing at all.

This document is the permanent home of the frozen contracts this task promises (task 3.3.1). The
normative specification of the container itself is `docs/09-file-formats.md`; this file specifies the
*tool*.

## Why it links the editor

The tool needs an `ImportedModel`, which means it needs `importModel()`, which lives in
`aero::editor_core` alongside the five importer paths Epic 3.2 built. Re-implementing a glTF parser
inside this tool was rejected outright: it would be a second parser for the format ADR-003 designates
canonical, and it would be wrong for the other seven claimed extensions on the day it shipped.

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

## The three frozen contracts

### 1. The CLI grammar

```
aero_cooker mesh --input <file> --output <file.aeromesh>
                 [--guid <32 hex>] [--scale <float>]
                 [--no-materials] [--no-animations] [--no-skins]
aero_cooker --version
aero_cooker --help
```

- **Subcommand-shaped from day one.** `mesh` is the only subcommand in v1; `aero_cooker texture …`
  arrives at task 3.3.2 with no reshuffle of anything above. A missing or unknown subcommand is a
  usage error naming the token.
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

There is no `--platform` flag and no platform field in the artifact: mesh cook output is
platform-independent. That changes at task 3.3.2, where BCn/ASTC/ETC2 diverge.

### 2. The artifact rule

**Nothing is written unless the whole cook succeeded.** The output file is opened only after
`cookMesh` has returned with a complete byte vector, so a failing input leaves **zero** artifacts —
never a partial one, never a stale one, and never the `.aero-tmp` file the atomic write would have
created on its way to the final name.

**The tool creates no directory.** `--output` inside a directory that does not exist is exit `3`.
`aero_shaderc` does create its `--output-dir`, and the difference is deliberate: that flag names a
*directory*, this one names a *file*, and a build-time tool that invents directories is how a typo
becomes a mystery tree.

**The output is deterministic.** The same input cooks to the same bytes across two runs, three
toolchains and any ordering of the model's own primitives. No timestamp, no path, no hostname, no user
name and no build id reaches the container; the only provenance fields are the cooker version (a
compile-time constant) and the GUID the caller supplied.

### 3. The extension table

| Extension | Result |
|---|---|
| `.gltf` `.glb` `.fbx` `.obj` `.mtl` `.dae` `.ply` `.stl` | Accepted — the five importer paths Epic 3.2 built. |
| `.blend` | **Refused, by design**, with exit `2` and a message naming the editor's conversion path. Converting a `.blend` means running Blender, and **this tool spawns no process, ever** — that is the editor's job (task 3.2.4). |
| anything else | Refused with exit `2` and a message naming the extension. Nothing is read. |

The extension is tested against the file **name** before a single byte is read, so an over-cap or
missing `.blend` still gets the message that helps rather than an I/O complaint.

## What the flags actually change in the artifact

Genuinely asymmetric, and measured rather than assumed — this is the part that cannot be guessed:

| Flag | Effect on the cooked artifact |
|---|---|
| `--scale <f>` | **Real.** The importer applies it to positions and to root node translations, so cooked positions **and** cooked bounds change. The cook itself applies nothing — it converts nothing at all. |
| `--no-materials` | **Real.** Every cooked submesh's `materialIndex` becomes `0xFFFFFFFF`. |
| `--no-animations` | **None**, for every backend. v1 cooks geometry only. |
| `--no-skins` | **None for the glTF path**: `gltf_import.cpp` reads `JOINTS_0`/`WEIGHTS_0` unconditionally in its mesh phase, and `importSkins` gates only the skin *table*, which v1 does not cook. Other backends may differ; this row is deliberately not phrased as a symmetric claim. |

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
- **No cook-on-import in the editor**, no `Library/Cooked/`, no panel and no menu item. The editor is
  byte-identical to what it was; this tool is the only thing that cooks today.

## Tests

`tests/cooker/run_case.cmake` drives the real binary through 22 `cooker.*` ctest entries — argv in,
exit code plus files out. A CLI's honest test is its process boundary, so there is no doctest
translation unit for the tool and no tool code links into `aero_tests`; the pure halves it is built
from (`cookMesh`, `parseCookedMesh`, `meshCookPrimitives`) are covered there and in
`aero_editor_shell_test` instead. The cases are registered with **no gate flag**, so they run in every
build configuration. Their inputs are listed in `tests/cooker/fixtures/README.md`.
