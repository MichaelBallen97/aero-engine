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

## The named, unowned gap

**v1 stores no node hierarchy**, so a consumer that instantiates a cooked mesh puts every submesh at
the origin. **The cook must not "solve" this by baking node transforms into vertices**: `ImportedMesh`
is shared across nodes by construction (3.2.2's helper-node decision), so baking would force per-node
mesh copies and change the canonical model's shape for one format. A cooked model/prefab container
carrying the node tree is the right answer and belongs to whoever owns instantiation — task 3.1.5 is
the first task that will hit it. This is a **decision waiting to be taken**, not a scope boundary.

Full history: `docs/10-engineering-log.md`, task 3.3.1's entry under Phase 3.
