# `tests/fixtures/scripts/` — ES modules read through the VFS (task 4.1.2)

Epic 4.1's Definition of Done, read off real files: *"a JS module loads from the VFS and runs inside the engine
with errors surfacing in the engine log."* Every case that reads this directory mounts it **at the `res://` root**
through a `DirectoryBackend`, so `dod/main.js` here is `res://dod/main.js` there. The directory reaches
`aero_tests` as the path `AERO_SCRIPT_FIXTURES_DIR` (`tests/CMakeLists.txt`), never as a flag: a missing fixture
is a `REQUIRE` failure, never a silent skip.

| File | Read by | What it is |
|---|---|---|
| `README.md` | — | this file |
| `dod/main.js` | ML13, ML14 | the entry: imports `./lib/math.js` and `../shared/greeting.js`, and leaves `dodResult` |
| `dod/lib/math.js` | ML13, ML14 | imported by both entries; counts its own evaluations (`mathEvaluations`) |
| `shared/greeting.js` | ML13, ML14 | reached through `../`, one directory up from the entry |
| `dod/throws.js` | ML14 | fails on purpose; its error must reach the log once, naming this file, line 5 and line 8 |
| `encoding/bom.js` | ML43 | starts with a UTF-8 byte-order mark (`EF BB BF`), which the parser reads as whitespace |

The stack text ML14 asserts — `    at fail (res://dod/throws.js:5:15)` and `    at <anonymous>
(res://dod/throws.js:8:6)` — depends on these files' exact lines and columns: an edit to `dod/throws.js` (its
comment line included) moves it. Every file is LF-terminated (`.gitattributes`), and `encoding/bom.js` was written
with `printf '\xef\xbb\xbfglobalThis.bomLoaded = true;\n'`, so its first three bytes are the mark itself.
