# Fixture provenance

Most fixtures under this tree (`.meta` sidecars, `.gltf`/`.json` documents, project directories) are
hand-written or generated inline by the test that uses them, and their own comments say so. The one
exception — a binary file that cannot be authored by hand and cannot be told apart from a real export
by any test tier — gets its provenance recorded here instead, the same posture
`samples/phase-2-editor-scene/VALIDATION.md` already takes for its own artifact: a hand-written file is
byte-identical to a real one, so provenance is *recorded*, not *asserted*.

## `assets/cube-binary.fbx` (task 3.2.2)

The only committed binary FBX fixture, and the sole proof that this project's importer decodes the
**binary** FBX container (every other fixture in `tests/editor/fbx_import_test.cpp` is ASCII, assembled
in memory from string literals). Exercised by `FI76` (`tests/editor/fbx_import_test.cpp`).

- **Tool:** Blender 5.2.0 LTS, `/opt/homebrew/bin/blender`.
- **Date:** 2026-08-09.
- **Command**, run non-interactively against a fresh, empty scene:

  ```bash
  blender --background --python-expr "
  import bpy
  bpy.ops.wm.read_factory_settings(use_empty=True)
  bpy.ops.mesh.primitive_cube_add(size=1.0)
  bpy.ops.export_scene.fbx(filepath='/tmp/cube-binary.fbx', use_selection=False,
                           path_mode='RELATIVE', axis_forward='-Z', axis_up='Y')
  "
  ```

- **Export settings:** `path_mode='RELATIVE'`, `axis_forward='-Z'`, `axis_up='Y'` — every other FBX
  exporter option is left at Blender's own default (binary container, FBX 7.4/2013 ASCII-off, version
  7400, `apply_unit_scale` on, `apply_scale_options='FBX_SCALE_NONE'`).
- **Scene content:** a single default cube (`primitive_cube_add(size=1.0)` — a 1 m cube centred on the
  origin), no material, no texture, no armature, no animation.
- **Size:** 11 836 bytes — comfortably under the ~50 KB budget `fbx_import_test.cpp`'s own comment
  block asks for (R3: keep every ASan-instrumented fixture small).
- **SHA-256:** `31e51d97486d00941fcfe28d91126a3f9dc39a7e40fca26bbfd940f06dd6baff`

**What was measured from the file, not assumed**, since it is what `FI76` compares against a
hand-written ASCII twin: Blender authors the cube as 8 control-point vertices referenced by a
24-entry `PolygonVertexIndex` across 6 quad faces (the standard FBX convention), with a per-node
`Lcl Scaling` of `(100, 100, 100)` rather than a baked geometry offset, and it declares `UpAxis: 1`
(Y) while still requiring the identical −90°-about-X conversion a Z-up source would — an internal
convention of Blender's own FBX exporter this task does not attempt to reverse-engineer. `FI76`'s own
comment records exactly which fields match the ASCII twin bit-for-bit and which two do not, and why.

If this fixture is ever regenerated, re-run the exact command above, re-measure the SHA-256, and update
both the hash recorded here and `FI76`'s own measured literals (node transform, bounds, vertex/triangle
counts) — do not assume a re-export reproduces the same bytes.
