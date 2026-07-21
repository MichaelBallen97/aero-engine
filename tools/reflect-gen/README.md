# tools/reflect-gen

`aero_reflect_gen` — a first-party CLI that links system libclang (the stable C API, LLVM 18),
parses a translation unit given on its command line, walks the resulting AST, and reports what it
saw. This is the harness rung of Epic 1.1 (ADR-004: reflection is the spine) — it stops at *parse +
walk*; 1.1.2 teaches it to recognize `[[engine::component]]` and collect fields, 1.1.3 emits
`entt::meta` registration, 1.1.4 makes codegen a first-class incremental build step.

## Why system-discovered LLVM 18 (and not vcpkg, and not a pinned download)

`aero_reflect_gen` is build-time-only and never shipped, and it uses only libclang's **stable C
API** (see below) — so the byte-for-byte hermeticity of a pinned-prebuilt-download bootstrap (the
`tools/shaderc` pattern, task 0.4.3) buys nothing here, while costing hundreds of MB per OS and an
awkward Windows extraction. vcpkg's `llvm` port is a non-starter: it builds LLVM from source (hours
per cold CI lane, multi-GB) — the same from-source problem that killed the vcpkg route for shaderc,
only larger.

Instead, `tools/reflect-gen/CMakeLists.txt` discovers an LLVM 18 already provisioned by the
platform's own package manager: Homebrew `llvm@18` (macOS, keg-only — not on `PATH`, so discovery
hints the formula's prefix, never bare `find_program`), apt `libclang-18-dev` + `llvm-18-dev`
(Linux), or `choco install llvm --version=18.1.8` (Windows). This is also consistent with existing
precedent: the repo already provisions LLVM 18 from system packages for `clang-format`/`clang-tidy`.

Discovery order: `find_package(Clang 18 CONFIG QUIET)` first (an imported `libclang` target in one
shot, when it resolves), a hand-rolled `find_library`/`find_path` fallback second — both hinted by
the cache variable `AERO_LLVM_ROOT` (per-OS default; overridable via `-DAERO_LLVM_ROOT=...` or the
`AERO_LLVM_ROOT` environment variable). **Expect the fallback to fire on at least some lanes** — for
example, on Homebrew's `llvm@18` (18.1.8), `ClangConfigVersion.cmake`'s compatibility check requires
an *exact* major.minor match against the requested version, so requesting the bare major `18` (as
this project does) never satisfies it, and the fallback always fires there. That is fine: a
C-API-only consumer needs none of LLVM's CMake machinery. Either path keeps a version-drift gate
alive — the fast path via `find_package`'s own version match, the fallback via a light `llvm-config
--version` assertion (skipped, not required, if `llvm-config` is absent).

`-DAERO_REFLECT_TOOLS=OFF` skips the discovery, the tool, and the `reflect-gen` ctest suite
entirely, with a loud `STATUS` explaining what was skipped — the escape hatch for constrained/offline
dev machines, mirroring `AERO_SHADER_TOOLS`. Never set in CI.

## The C-API-only rule (and why)

`aero_reflect_gen` includes **only** `<clang-c/...>` headers — libclang's stable, versioned C API —
**never** a C++ Clang/LLVM header (`libclang-cpp`/libTooling, which is explicitly unstable and
version-locked). This is what makes system-discovery across a whole LLVM major version safe: the C
API's source and binary compatibility guarantees mean any 18.1.x build resolves the same calls, on
any host, from any vendor's package. It is also the seam ADR-004 keeps open for a future migration to
native C++26 `std::meta`: because this tool's own internals (which Clang API it uses) never leak into
what it produces, that migration stays local to this one tool.

## The frozen CLI contract (extended, never broken, by 1.1.2–1.1.4)

```
aero_reflect_gen [--all] [--main-file-only] [--version] [--help] <input> [-- <clang args>...]
```

- `<input>` — the translation unit to parse (the first non-flag token before `--`; a second is a
  usage error).
- Everything **after `--`** is forwarded verbatim to libclang (`-std=...`, `-I...`, `-isysroot...`,
  `-D...`, ...). `aero_reflect_gen` never derives these itself in this task — see below.
- `--main-file-only` (the **default**) limits the AST walk to cursors physically located in
  `<input>`. `--all` includes cursors from every included header. If both are given, `--all` wins
  (it is not a usage error to pass both).
- `--version` prints the tool version **and** `clang_getClangVersion()`, then exits 0. `--help`
  prints this usage grammar, then exits 0. Both short-circuit before any parse.
- **Output:** an indented AST walk to **stdout**, one line per cursor:
  `<indent><KindSpelling> '<spelling>' @<line>:<col>`. Diagnostics and errors go to **stderr** only.

| Exit code | Meaning |
|---|---|
| `0` | Parsed with **zero** error/fatal diagnostics (warnings — e.g. an unrecognized `[[engine::...]]` attribute — are allowed) |
| `1` | Usage error (unknown flag, missing `<input>`, `<input>` given twice) |
| `2` | Parse failure (a null translation unit, or at least one error/fatal diagnostic) |
| `3` | I/O error (`<input>` does not exist, is not a regular file, or is not readable) |

**What this task deliberately does NOT do** (each is a later, numbered task): derive compile flags
from the build itself (1.1.4 — this task's tests feed a hand-verified, per-OS flag set via
`-- <clang args>`); recognize `[[engine::component]]` as anything other than an unrecognized,
warned-about attribute (1.1.2); emit `entt::meta` registration or any other generated output (1.1.3).

## Local troubleshooting

- **libclang cannot find the C++ standard library / SDK headers, even with a correct `-isysroot`.**
  Because `aero_reflect_gen` is a standalone executable living outside the LLVM install prefix,
  libclang's internal driver cannot locate its own **resource directory** (the compiler-builtin
  headers such as `stdarg.h`, normally found relative to a real `clang` binary's own path) purely
  from `argv[0]` — every parse needs an explicit `-resource-dir <AERO_LLVM_ROOT>/lib/clang/18` (the
  standard LLVM install-tree layout on every platform) alongside `-isysroot`. The `reflect-gen` ctest
  suite computes this for every case that needs it; a manual invocation needs the same flag.
- **macOS: the default SDK may be too new for clang-18.** If this machine's default SDK (`xcrun
  --show-sdk-path`) is newer than what LLVM 18 was built to understand, a heavier parse (anything
  pulling in libc++, e.g. `<type_traits>`) can fail with libc++-internal parse errors even though a
  trivial header (e.g. one that only includes `<cstdint>`) parses fine. If that happens, pin
  `-isysroot` to an older installed SDK instead (for example `xcrun --sdk macosx15.4
  --show-sdk-path`), the same workaround this repo's local `clang-tidy` invocation already needs.
- **Escape hatch:** `-DAERO_REFLECT_TOOLS=OFF` skips the tool and its tests entirely (see above).
- **Override the discovered LLVM install:** `-DAERO_LLVM_ROOT=/path/to/llvm-18` (CMake cache
  variable) or the `AERO_LLVM_ROOT` environment variable (read only when the cache variable is not
  already set).
