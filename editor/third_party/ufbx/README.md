# ufbx — vendored, task 3.2.2

**Version:** v0.23.0 · **Upstream:** https://github.com/ufbx/ufbx · **Tag:** `v0.23.0`
**Fetched:** 2026-08-09 from
`https://raw.githubusercontent.com/ufbx/ufbx/v0.23.0/{ufbx.h,ufbx.c,LICENSE}`
**Licence:** dual MIT / Public Domain (Unlicense) — either satisfies `docs/01`'s MIT-compatibility
requirement. `LICENSE` is upstream's own file, unmodified.

## Why vendored rather than a vcpkg package

ufbx is in **no** vcpkg registry — not in the pinned baseline (2851 ports; the only FBX-adjacent one
is `openfbx`) and not upstream. Measured, not assumed. See `docs/02-adrs.md` and task 3.2.2's entry in
`docs/10-engineering-log.md`.

The decisive property of vendoring: `ufbx.c` compiles with **this project's** directory-scope options,
so it is **ASan/UBSan-instrumented in all three Debug lanes**. ufbx parses untrusted binary files —
the single most valuable place in this tree for a sanitizer to be watching. A vcpkg build would use
vcpkg's own triplet flags and be a blind spot. `engine/platform/src/miniaudio_impl.c` records the same
property for miniaudio.

## NO LOCAL PATCHES, EVER

`ufbx.h` and `ufbx.c` are byte-identical to upstream v0.23.0, and a CI-adjacent check at merge proves
it by re-downloading and diffing. A needed fix goes **upstream**, or into a wrapper in
`editor/src/fbx_import.cpp` — never into these two files. A patch here must be re-applied by hand at
every version bump, correctly, with its reason invisible at the call site.

## Bumping the version

1. Replace all three files from the new tag; update the version, tag and date above.
2. Re-read `editor/src/fbx_import.cpp`'s `ufbx_load_opts` block — every field there is set
   **explicitly** precisely so a changed default cannot move this importer's output silently.
3. Re-run the full tier-0 suite. `tests/editor/fbx_import_test.cpp` is the regression net.
4. If ufbx ever appears in vcpkg's curated registry, deleting this directory for a
   `find_package(ufbx CONFIG REQUIRED)` is defensible — but it **costs sanitizer coverage of ufbx's
   own code**, which is a real loss and must be weighed then, not assumed away.
