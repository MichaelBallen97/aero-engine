# tools/shaderc

`aero_shaderc` — a first-party CLI that compiles one authored HLSL source into SPIR-V, MSL, and
DXIL bytecode, offline, at build time, plus aero-owned JSON metadata. This is the origin of every
shader `ShaderDesc` in the engine consumes (`engine/rhi/include/aero/rhi/descriptors.hpp`); task
0.4.4 owns the loading side.

This document is the permanent home of the three frozen contracts this task promises (task 0.4.3;
docs/08-risks.md R4) and the documented fallback if the underlying toolchain ever blocks us.

## Why this exists (and why it isn't `vcpkg install sdl3-shadercross`)

The pinned vcpkg baseline's `sdl3-shadercross` port depends unconditionally on `directx-dxc`, which
vcpkg marks unsupported on `arm64-osx` — a real `vcpkg install --dry-run` on this baseline fails
with vcpkg's own "known build failures" warning. Both DXC vcpkg ports build DXC **from source**
(LLVM-scale) even where they are supported, so there is no prebuilt shortcut to fall back to inside
vcpkg either.

Instead, `tools/shaderc/bootstrap.cmake` acquires
[SDL_shadercross](https://github.com/libsdl-org/SDL_shadercross) directly from its pinned commit
with `SDLSHADERCROSS_VENDORED=ON` (vendored DirectXShaderCompiler + SPIRV-Cross, built from source)
— the one configuration upstream's own CI tests on macOS — identically on all three hosts. This
never touches `vcpkg.json` or the `/vcpkg` submodule pin.

## The pin table

| Vendored submodule | Commit |
|---|---|
| SDL_shadercross itself | `1ca46e0ef7a9e50c706e7be6ef73ce467bac3b2e` |
| `external/DirectXShaderCompiler` (libsdl-org fork, branch `1.8.2502-SDL`) | `d61b73f565d699a4d1cbb9ed6cf9179e1378b18f` |
| `external/SPIRV-Cross` | `d1d4adbefd411fc4721a2fece15a7f4aaa3dcdfa` |
| `external/SPIRV-Headers` | `54a521dd130ae1b2f38fef79b09515702d135bdd` |
| `external/SPIRV-Tools` | `aafd524577cc90fcdd13a6f0bcbfb929a30ee90f` |

Bumping the toolchain means bumping the SHA in `tools/shaderc/bootstrap.cmake` (and the matching
`toolchain` JSON constant in `src/main.cpp`) in the same commit; the submodule pins above land by
construction from `git submodule update --init --recursive` at that exact SHA.

## The three frozen contracts

Everything downstream (CMake, 0.4.4's shader loader, the future cooker) depends on exactly these
three — never on SDL_shadercross's own API, CLI, or CMake package directly. That indirection is the
whole point (R4's isolation seam): if a preview bug in SDL_shadercross ever blocks us, only the
*implementation* of `src/main.cpp` needs to change; nothing that calls into it does.

### 1. The `aero_shaderc` CLI grammar

```
aero_shaderc --input <file.hlsl> --stage vertex|fragment --output-dir <dir>
             [--name <base>] [--formats spirv,msl,dxil] [--include <dir>]
             [--define NAME[=VALUE]]...
aero_shaderc --version
aero_shaderc --help
```

- `--name` defaults to the input filename minus `.hlsl`.
- `--formats` defaults to all three; a comma list with no duplicates and no unknown tokens.
- `--include` may be given **at most once** (a single directory, not a search path list).
- `--define` may repeat; if a name repeats, the **last** occurrence wins.
- The entry point is always `main` — there is no `--entry` in v1.
- Exit codes: `0` success, `1` usage error, `2` compile/transpile/reflect error, `3` I/O error.
  Diagnostics go to stderr only; stdout is reserved for `--help`/`--version`.

No `--debug`, `--msl-version`, compute stage, or DXBC output in v1. These are append-later
extensions (a new flag/value, never a reshuffle of the existing grammar).

### 2. The artifact schema

For `<name>.vert.hlsl` / `<name>.frag.hlsl`, in `--output-dir`: `<base>.spv`, `<base>.msl`,
`<base>.dxil` (whichever `--formats` requested) plus **always** `<base>.json`, where `<base>` =
`<name>.vert` / `<name>.frag`.

**Nothing is written until every requested step has succeeded** — a failing shader leaves zero
artifacts in the output directory, never a partial or stale set.

JSON schema v1 — exact key order, 2-space indent, LF, trailing newline:

```json
{
  "schema": 1,
  "toolchain": "sdl-shadercross@1ca46e0e",
  "name": "triangle.vert",
  "stage": "vertex",
  "entryPoint": "main",
  "mslEntryPoint": "main0",
  "samplerCount": 0,
  "storageTextureCount": 0,
  "storageBufferCount": 0,
  "uniformBufferCount": 0,
  "formats": ["spirv", "msl", "dxil"]
}
```

The four counts come from SPIR-V reflection only — never hand-typed. `mslEntryPoint` is always
`"main0"` (Metal disallows a `main` entry point; SPIRV-Cross's "cleansed" rename of our fixed `main`
convention is a fixed, load-bearing constant, not something re-derived from the transpiled text).

### 3. `aero_add_shaders()` (CMake integration)

```cmake
aero_add_shaders(<target> SHADERS <src.hlsl>... [OUTPUT_DIR <dir>] [INCLUDE_DIR <dir>]
                  [DEFINES <NAME[=VALUE]>...])
```

Defined in `cmake/shaders.cmake`. The stage comes from the mandatory `.vert.hlsl`/`.frag.hlsl`
filename suffix (a configure-time `FATAL_ERROR` otherwise) — the *function* is the stage oracle;
`aero_shaderc` itself never sniffs filenames, only `--stage`. One `add_custom_command` per shader,
one aggregate custom target (added to `ALL`) per `aero_add_shaders()` call.

When `AERO_SHADER_TOOLS` is `OFF`, the function degrades to a no-op with a `STATUS` line — callers
never need their own guard.

## The HLSL resource-binding law

Normative for every HLSL file this repo ever authors (`SDL_gpu.h`'s `SDL_CreateGPUShader` contract):

- Vertex shaders: textures/storage → `t[n], space0`; samplers → `s[n], space0`; uniform buffers →
  `b[n], space1`.
- Fragment (pixel) shaders: textures/storage → `t[n], space2`; samplers → `s[n], space2`; uniform
  buffers → `b[n], space3`.

(SPIR-V descriptor-set equivalents and MSL `[[buffer]]`/`[[texture]]` binding orders are derived by
SDL_shadercross automatically from conforming HLSL — nothing else in the pipeline needs to know
this law, only the HLSL author does.)

## The raw-DXC fallback recipe

If a preview bug in SDL_shadercross ever blocks this pipeline (docs/08 R4), the fallback is to
replace the SDL_shadercross API calls **inside `src/main.cpp`** with direct calls to:

1. **DirectXShaderCompiler** (`IDxcCompiler3`, from the same fork pin above, or a Microsoft release)
   for HLSL → SPIR-V and HLSL → DXIL.
2. **SPIRV-Cross** (the C API, `spvc_*`) for SPIR-V → MSL transpilation and for the resource-count
   reflection SDL_shadercross's `SDL_ShaderCross_ReflectGraphicsSPIRV` currently provides.

The CLI grammar, the JSON schema, the artifact file names, and `aero_add_shaders()`'s signature do
**not** change. Every consumer — the CMake wiring, 0.4.4's loader, the future cooker — depends only
on those three contracts above, so this swap is invisible to them by construction.

## Local troubleshooting

- **First configure is slow and needs network.** The bootstrap clones SDL_shadercross + its four
  submodules and builds vendored DirectXShaderCompiler from source — expect several minutes to
  ~20 minutes depending on the machine, once per machine (not once per preset, not once per
  worktree). A second configure with a warm cache is instant and fully offline (look for `shadercross
  toolchain: warm at ...` in the configure log).
- **Escape hatch:** `-DAERO_SHADER_TOOLS=OFF` skips the bootstrap, the tool, the `shaders/` build,
  and the shaderc ctest suite entirely, with a loud `STATUS` explaining what was skipped. Never set
  in CI; it exists for constrained/offline dev machines.
- **Cache location:** `~/.cache/aero-engine/shadercross` (macOS/Linux, honoring `XDG_CACHE_HOME`) or
  `%LOCALAPPDATA%\aero-engine\shadercross` (Windows); overridable via the `AERO_SHADER_TOOLS_ROOT`
  CMake cache variable.
- **A stale lock file** (`<prefix>.lock`) after a killed configure is safe to delete by hand; the
  bootstrap otherwise waits on it (two presets or two worktrees can race the same prefix safely).
