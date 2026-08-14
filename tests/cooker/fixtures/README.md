# `tests/cooker/fixtures/` — inputs for the `cooker.*` process-boundary suite (task 3.3.1)

The `cooker.*` ctest cases run the real `aero_cooker` binary, so they cannot reach
`tests/fixtures/assets/` without hard-coding a relative path. `run_case.cmake` receives `SOURCE_DIR`
instead and composes both roots: `${SOURCE_DIR}/tests/fixtures/assets/…` for the fixtures the editor
suites already share, and `${SOURCE_DIR}/tests/cooker/fixtures/…` for the seven below, which exist only
for this tool. All are committed, none is generated at build time, and only `external.bin` is binary.

| File | What it is for |
|---|---|
| `external.gltf` + `external.bin` | The tree's only **two-file** glTF: the one input that exercises the tool's external-buffer pass. `cooker.gltf_external_bin` runs it twice — once beside its sibling (exit 0), and once copied into a scratch directory **without** it, where the Full pass reports a missing buffer and the tool exits 2 with no artifact. |
| `material.gltf` | One material and one primitive that names it, self-contained through an embedded data URI. It is what makes `--no-materials` observable: the cooked submesh's `materialIndex` is `0` with the flag absent and `0xFFFFFFFF` with it present. |
| `broken.gltf` | Truncated JSON. `cooker.nothing_written_on_failure` drives it into an empty working directory and asserts the directory is still empty afterwards — no artifact, and no `.aero-tmp` either. |
| `no-mtl.obj` | A triangle whose `mtllib` names a file that is not there. A missing `.mtl` is a warning rather than a failure (task 3.2.3's D7), so the tool must still exit 0 and still write an artifact. |
| `cube.blend` | **One byte, and never parsed.** `isImportableModelName` is false for `.blend` by design (task 3.2.4's D15), so the tool refuses it on the file name alone, before reading anything. Its only job is to have that extension — that is why a one-byte `.blend` in a fixtures directory is not a mistake. |
| `multi.gltf` | **The only multi-primitive glTF in the tree** (task 3.3.3): two meshes, three primitives, one embedded data URI. Mesh 0's **first** primitive carries `POSITION`+`NORMAL` and the other two carry `POSITION` alone, so the cook's ascending-`(mask, sourceMeshIndex, sourcePrimitiveIndex)` sort emits the first-declared primitive **last** — declared order is not emission order, by construction. It is the only manifest input whose bytes depend on the sort running at all: every other mesh fixture has one primitive, for which any sort is a no-op. Cooks to 576 bytes, `sectionCount` 2, `submeshCount` 3. |
