---
paths:
  - "engine/assets/**"
  - "tools/cooker/**"
  - "tests/cooker/**"
---

# Cooked assets — the container, the cook and the cooker CLI

`engine/assets` (task 3.3.1) holds the tree's **first binary format**: `cooked_mesh.{hpp,cpp}` is the
`.aeromesh` container v1 and its hostile-input parser, `mesh_cook.{hpp,cpp}` is the producer.
`tools/cooker` is the CLI that drives them end to end. The **normative** specification is
`docs/09-file-formats.md` section 9 — if a header and that document ever disagree, docs/09 wins and one
of them is a bug.

## The subsystem's link line is a real boundary — do not void it

**`engine/assets` links `aero::core` (PUBLIC) and `aero::profiling` (PRIVATE) and NO vcpkg package,
ever.** That is rarer here than it looks and it is what makes its `PRIVATE` links a genuine
compile-time boundary rather than the usual convention-plus-grep: R12 says a `PRIVATE` vcpkg link
cannot enforce a header boundary, because vcpkg installs every port into one shared per-triplet
`include/` root that lands on the compile line of any target linking any vcpkg package. `aero_assets`
links none, so a stray `#include <fastgltf/…>` there is a hard compile error.

**Adding a `find_package` to `engine/assets/CMakeLists.txt`, ever, voids that silently while CI stays
green.** The CMakeLists' own comment says so; the grep is the second line of defence, not the first.

## Bytes

- **Bytes are formed in exactly eight places** — the `constexpr` `putU16`/`putU32`/`putU64`/`putF32`
  and their `get*` inverses in `cooked_mesh.hpp` — and **the endianness is a `static_assert`**, so a
  big-endian mistake is a build failure rather than a red test.
- **No struct `memcpy`, no `reinterpret_cast` of a record pointer, no packed-struct pragma**, and
  `sizeof` is never taken of an on-disk record: the four `COOKED_MESH_*_BYTES` constants (96/8/32/64)
  are the only sizes. A struct's size is a compiler's opinion and a format's is not.
- **No `std::map`, `std::unordered_map`, `std::set` or `std::unordered_set` in `engine/assets`.** There
  must be no iteration order for the output to depend on, and MSVC's node-based containers are not
  nothrow-movable (3.1.2's R9, measured in CI as `C2607`). Grouping is a sorted vector.

## The cook

- **The cook sorts BEFORE it applies caps.** Reversing that makes the surviving prefix depend on the
  caller's input order, so a shuffled input produces a different file — and only on inputs that trip a
  cap, which is exactly the combination a green suite does not otherwise exercise.
- **Bounds fold with `std::min`/`std::max`, ACCUMULATOR FIRST**, matching `Aabb::expand`
  (`scene_bounds.cpp:72-75`) bit for bit. The argument order is not cosmetic: with `-0.0f` and `+0.0f`
  the two orders return different zeros, and the cooked submesh box is compared byte for byte against
  the importer's.
- **The MODEL box folds in EMISSION order, and that is not negotiable** — it is written into the
  header, so an input-order fold would make a shuffled input produce different header bytes. An
  importer folds its own model box in SOURCE order, so the two can disagree in the **sign of a zero**
  and nowhere else. Same box either way; `docs/09` section 9.10 carries the caveat and `MK9` records
  that its bit-equality holds only because no committed fixture carries a signed zero. **Do not
  "fix" this by folding in input order.**
- **The cook converts nothing and re-indexes nothing.** No axis flip, no winding reversal, no unit
  scaling, no renormalization, no welding, no dedup, no vertex-cache optimization, no quantization.
  Source vertex order is preserved exactly. There is no `MeshCookSettings` type, deliberately: an
  empty settings struct is a shape that invites a field.
- **The cook drops WHOLE primitives and demotes WHOLE attributes, never anything partial**, and
  joints/weights are cooked together or not at all.
- **Every cap latches its own bool**, and "the first cap to trip" means the first *candidate*, not the
  first *cap*: two caps produce two messages only when the same candidate violates both. Two cap sites
  sharing a constant would still get different wording.
- **`MAX_COOKED_SECTIONS` and `MAX_COOKED_ATTRIBUTES` are PARSER caps.** `Position` is mandatory and
  the joints/weights pairing rule clears a mask carrying exactly one of the pair, so the cook can
  produce at most 64 sections against a cap of 128. Their cook-side arms are unreachable defence in
  depth and say so in their own comment — do not write a synthetic case that only looks like proof.

## The format's evolution rules

- **A reserved field is refusal-on-non-zero, so OCCUPYING one is a `formatVersion` bump.** Adding a
  semantic or format code is the same, because the parser refuses unknown codes. **`cookerVersion`
  means "the same input now cooks to different bytes" and nothing else** — it never gates a parse.
- **`sourceMeshIndex` is the POSITION in `ImportedModel::meshes`, not `ImportedMesh::localId`.** They
  coincide for glTF and OBJ and differ for FBX, where `localId` is a raw ufbx `typed_id`.
- **The parser deliberately does not validate index VALUES or region overlap.** Both are stated
  residuals with the same Phase 5 trigger, not oversights — every read goes through a bounds-checked
  accessor, so either is a wrong picture and never a memory error. The Phase 5 answer for the first is
  an opt-in `parseCookedMeshStrict`, chosen by the caller, never forced on every load.
- **The three byte goldens in `tests/cooked_mesh_golden.hpp` are FROZEN.** A change to any layout rule,
  offset, ordering rule or padding rule fails them by construction. If a golden needs to change, the
  format changed — and that is a `formatVersion` decision, not a test edit.

## The CLI

- **`tools/cooker` spawns no process, creates no directory, and passes an empty `assetRelativeDir`.**
  A `.blend` is refused with exit 2 and a message naming the editor's conversion path; converting one
  means running Blender, which is the editor's job (task 3.2.4).
- **Nothing is written unless the whole cook succeeded** — no partial artifact, no stale one, and not
  even the `.aero-tmp` the atomic write would have created.
- **`aero_cooker` takes no gate flag**, unlike `reflect-gen` and `shaderc`, so its ctest cases are
  registered in every configuration and `ctest -N` moves in all three. A future gate would silently
  shrink the reduced configurations' coverage with no test able to report it.
- **`tools/cooker/src/main.cpp` is the first `/tools` TU that is always in the compile database**, so
  CI's `--warnings-as-errors='*'` clang-tidy applies to it in full — unlike the two gated tools.

## Textures (task 3.3.2) — the container, the cook and the encoders

`engine/assets` also holds the **cooked texture container v1**: `cooked_texture.{hpp,cpp}` (a strict
subset of Khronos **KTX2**, and a real one — `ktx info`, `ktx validate` and RenderDoc open our files),
`texture_cook.{hpp,cpp}` (the cook) and `bc_block.{hpp,cpp}` (the two encoders). The **normative**
specification is `docs/09-file-formats.md` section 10.

- **`engine/assets` still links NO vcpkg package, and the stb_image decode lives in `/editor` BECAUSE
  OF THAT**, not for convenience. `find_package(Stb)` in `engine/assets/CMakeLists.txt` voids the
  boundary silently while CI stays green. The same sentence as the mesh half, one task later, and it is
  the reason the adapter pair exists at all.
- **NO FLOATING POINT IN `engine/assets`. AT ALL.** Not a style rule: a float here is a byte-identity
  hazard on three lanes (FMA contraction differs between clang and MSVC; libm differs between three C
  libraries), and 3.3.3 turns byte-identity into a CI job for **both** cook kinds. That is the whole
  reason `stb_dxt.h` is installed, provides exactly these four formats, and **is not used** — its BC1
  path finds the principal axis by float power iteration. `<numeric>`'s `std::lcm` is integer-only and
  is the one include the rule does not reach; do not "tidy" it away.
- **The gamma tables are committed literals with `static_assert`ed monotonicity and a `static_assert`ed
  derivation of the threshold table from the forward one.** Never generate them at runtime, never add
  the generator to the build. Its source is in `docs/10`'s 3.3.2 entry, verbatim, and nowhere else.
- **The sRGB DFD tables are NOT one-byte edits of their UNORM siblings** for BC3 and RGBA8: the alpha
  sample carries `KHR_DF_SAMPLE_DATATYPE_LINEAR`, so byte 31 of 138 and byte 79 of 43 are `1F`. KTX2
  makes it a **`must`**; **our own parser cannot catch a wrong table**, because it compares against the
  same one the writer emits. **Only `ktx validate` can.** Do not "simplify" the three sRGB tables into
  byte patches — one of them is, two of them are not.
- **The level INDEX is level-0-first and the level DATA is smallest-first.** The writer computes the
  offsets in reverse and stores them forward, so `levels[0].byteOffset` is the numerically largest
  offset in the file.
- **`mipPadding` occurs at exactly ONE site per file**, between the key/value data and the smallest
  level. The parser checks every level's alignment anyway — a hostile file is not obliged to share our
  arithmetic.
- **BC3 and BC5 have no encoders and may never grow one.** BC3 is alpha-then-colour; BC5 is
  red-then-green; both orders are output-byte decisions with their own seeds, because swapping either
  produces a plausible image rather than an obviously broken one.
- **Partial edge blocks CLAMP, never zero-fill.** Zero-fill drags the endpoints toward black and
  visibly darkens the right and bottom edges.
- **`--srgb`/`--linear` is mandatory on the CLI and there is no default**, and `--srgb` with `bc4`/`bc5`
  is a usage error because Vulkan defines no such format. sRGB is carried by the format enumerator and
  nowhere else, which is what makes "an sRGB normal map" unspellable rather than merely rejected.
- **`.hdr` is refused by a THIRD extension table, before the read.** `stbi_load` does not fail on a
  Radiance file — it silently tone-maps it through a fixed gamma-2.2 curve and hands back 8-bit LDR
  bytes, which cook to a plausible artifact that is quietly wrong.
- **`texture_cook_source.cpp` defines `STB_IMAGE_STATIC` and must keep defining it, and deliberately
  does NOT define `STBI_NO_FAILURE_STRINGS`**, unlike `thumbnail_store.cpp`. Copying that TU's macro
  block wholesale silently loses every decode reason. **`TK18` is the only cover for both halves** —
  measured, not assumed: dropping `STB_IMAGE_STATIC` links clean on macOS today, and adding
  `STBI_NO_FAILURE_STRINGS` still leaves a non-empty error string through the adapter's own fallback.
- **Three properties have no case that can see them violated and are pinned in comment-stripped SOURCE
  TEXT instead** (the `CM50` shape): the byte-cap check comes before the allocation (`TX48`); that check
  compares the byte TOTAL and names neither axis (`TX40`); `cookedTextureLevelAlignment` computes
  `std::lcm` (`CT6`); and, one tier up, the CLI checks the cook's status before it writes
  (`cooker.texture_nothing_written_on_failure`, scoped to `runTexture` because the mesh path's write
  call is character-identical). Do not replace any of them with a runtime case that only looks like
  proof.
- **Every case-local table in the four test TUs pins a LITERAL row count**, never `TABLE.size()`: a
  guard derived from the table it guards cannot see a row deleted, and two seeds proved it by deleting
  the rows that existed to catch two other seeds.

## The named, unowned gap

**v1 stores no node hierarchy**, so a consumer that instantiates a cooked mesh puts every submesh at
the origin. **The cook must not "solve" this by baking node transforms into vertices**: `ImportedMesh`
is shared across nodes by construction (3.2.2's helper-node decision), so baking would force per-node
mesh copies and change the canonical model's shape for one format. A cooked model/prefab container
carrying the node tree is the right answer and belongs to whoever owns instantiation — task 3.1.5 is
the first task that will hit it. This is a **decision waiting to be taken**, not a scope boundary.

Full history: `docs/10-engineering-log.md`, tasks 3.3.1 and 3.3.2's entries under Phase 3.
