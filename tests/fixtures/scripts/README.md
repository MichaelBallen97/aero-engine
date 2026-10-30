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
| `bytecode/features.ts`, `features.js` | BY15 | one script per language area (classes, generators, regexps, BigInt, `using`, proxies, top-level await, `import.meta`), results in `globalThis.results` |
| `bytecode/throws.ts`, `throws.js` | BY15 | fails on purpose at top level through a nested call |
| `bytecode/source-text.ts`, `source-text.js` | BY15 | a function, a class, a method and two arrows, each one's `toString()`, `name` and `length` in `globalThis.results` |

The stack text ML14 asserts — `    at fail (res://dod/throws.js:5:15)` and `    at <anonymous>
(res://dod/throws.js:8:6)` — depends on these files' exact lines and columns: an edit to `dod/throws.js` (its
comment line included) moves it. Every file is LF-terminated (`.gitattributes`), and `encoding/bom.js` was written
with `printf '\xef\xbb\xbfglobalThis.bomLoaded = true;\n'`, so its first three bytes are the mark itself.

The three `bytecode/*.js` files are esbuild 0.28.2's bundles of the `.ts` beside them, made with 4.3.1's fixed flag list
(`ESBUILD_FIXED_FLAGS`, `editor/include/aero/editor/esbuild_tool.hpp`) plus `--outfile`, from inside `bytecode/`:

```
esbuild <name>.ts --bundle --format=esm --platform=neutral --target=es2023 --supported:using=false \
  --preserve-symlinks --sourcemap=linked \
  '--tsconfig-raw={"compilerOptions":{"verbatimModuleSyntax":true,"useDefineForClassFields":true,"experimentalDecorators":false}}' \
  --log-level=warning --color=false --log-limit=0 --outfile=<name>.js
```

The `.js.map` files are not committed; the `sourceMappingURL` comment at the end of each bundle is harmless.
`features.js` keeps `import.meta` textually. Regenerate all three together when the pinned esbuild moves.
