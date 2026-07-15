# 04 — Conventions & Setup

> How the code is written, built, and shipped. These conventions exist to keep a solo, long-horizon C++ project maintainable and to make the ADR mitigations enforceable rather than aspirational.

---

## The non-negotiable mitigations (from ADR-001)

These are enforced, not encouraged:

1. **Handles, not pointers** for every resource. No raw owning pointers cross a subsystem boundary.
2. **Never manual `new`/`delete`.** RAII, `std::unique_ptr`/`std::vector`, standard containers.
3. **ASan + UBSan run in CI on every commit**, on all three OSes (Windows: ASan only — MSVC has no UBSan).
4. **No third-party type crosses the engine's public API** (project rule #3), enforced by CI tests (below).

---

## C++ style

- **Standard:** C++20 baseline; opt into C++23 features only where all three compilers (Clang, MSVC, GCC) support them.
- **Naming:** `PascalCase` types, `camelCase` functions/variables, `SCREAMING_SNAKE_CASE` compile-time constants, `snake_case` files and directories.
- **Namespaces:** everything under `engine::`; subsystem sub-namespaces (`engine::render`, `engine::rhi`, …) as needed.
- **Headers:** `#pragma once`. Public headers expose only engine types — never `glm`, `SDL`, `entt`, `miniaudio`, or other third-party types.
- **Errors:** no exceptions across public API boundaries; prefer explicit result/status types and asserts in debug. Handles return invalid rather than throw.
- **Formatting:** `clang-format` (config committed); enforced in CI. `clang-tidy` for lint. `.clang-format`/`.clang-tidy` live at the repo root, pinned to **LLVM 18** (`clang-format-18`/`clang-tidy-18`; task 0.1.6). The docs/04 naming law above is machine-enforced by `readability-identifier-naming`. Enforcement is split for speed: a standalone `lint` CI job runs `clang-format --dry-run --Werror` over every tracked C/C++ source (no build); a step on the Linux Debug lane runs `clang-tidy --warnings-as-errors='*'` over the first-party `.cpp` translation units, reusing that lane's compile-commands database.
- **No `using namespace` in headers.**

---

## The architecture-guard CI tests

Automated tests that fail the build if an invariant is broken. Every guard has an owning task that creates it (July 2026 re-plan):

| Guard | Fails when… | Created by |
|---|---|---|
| **Math-boundary guard** | any `#include <glm/...>` appears outside `engine/core/src/math/glm_backend.cpp` (plus a compile-time probe target for public headers) | [0.2.3](./tasks/phase-0.md) |
| **RHI-boundary guard** | an SDL_GPU type appears in a public engine header | [0.4.5](./tasks/phase-0.md) |
| **Golden-rule guard** | any `#include` under `/engine` or `/runtime` references `/editor` | [2.1.2](./tasks/phase-2.md) |
| **Audio-boundary guard** | a miniaudio type appears in a public engine header | [3.7.3](./tasks/phase-3.md) |
| **Runtime-purity guard** | the runtime binary links ImGui, Assimp, or libclang | [5.2.2](./tasks/phase-5.md) |

---

## Build & dependencies

- **Build system:** CMake with **Presets** (`CMakePresets.json`) — one preset per platform/config.
- **Dependencies:** **vcpkg in manifest mode** (`vcpkg.json`) pinned to a baseline commit for reproducibility.
- **Layout:** out-of-source builds only; generated files (reflection output, cooked assets) never committed.
- **Reflection build step:** `tools/reflect-gen` runs **before** the main compile; its output is a generated source directory consumed by `/engine`.
- **Shaders:** authored in HLSL under a `shaders/` tree; `tools/shaderc` compiles them offline to DXIL/MSL/SPIR-V during the build.
- **Format stability:** scene/asset formats may break without migration until v1.0; every format carries a version field from day one (so post-1.0 migrations are possible without archaeology).

---

## Git conventions

- **Branching:** trunk-based. Short-lived feature branches, merged to `main` via PR (even solo — the PR is where CI runs and where the review discipline lives).
- **`main` is always green.** No merging on red CI.
- **Commits:** conventional-commit style (`feat:`, `fix:`, `refactor:`, `docs:`, `build:`, `ci:`, `test:`). Imperative mood.
- **`.meta` files are committed** (they carry stable asset GUIDs). Cooked binaries and build output are `.gitignore`d.
- **Tags:** phases and releases are tagged (`phase-0-complete`, `v1.0.0`, …).

---

## CI (GitHub Actions, from commit #1)

Matrix across **macOS + Windows + Ubuntu**. Every push/PR runs:

1. Configure (CMake presets) + build (Debug with ASan/UBSan; Release)
2. `reflect-gen` + `shaderc` codegen steps
3. Unit tests (doctest)
4. The five architecture-guard tests above (each activates once its owning task lands)
5. `clang-format` / `clang-tidy` check
6. Task-local boundary check: no spdlog/fmt types in public engine headers (0.2.4)

Runtime binaries for each platform are built and archived by CI (this is what makes TS-project export instant — see [ADR-008](./02-adrs.md#adr-008--per-project-language-choice-and-the-two-export-models)).

---

## Development environment

- **Editor/IDE:** VSCode (matches the TypeScript-scripting audience) or CLion; both drive CMake presets.
- **Toolchains:** Clang (macOS/Linux), MSVC (Windows), GCC (Linux secondary). The **Mac is the only machine that can build/sign macOS + iOS**.
- **Profiling:** Tracy client wired from Phase 0 (task 0.1.5); profile early, not after. Dev-builds-only, gated by `AERO_ENABLE_PROFILING` (default OFF, ON in the three `*-release` presets); wrapper header `engine/core/include/aero/core/profiler.hpp` exposes backend-agnostic `AERO_PROFILE_*` macros (`AERO_PROFILE_ZONE`, `AERO_PROFILE_ZONE_NAMED`, `AERO_PROFILE_FRAME_MARK`, …) that no-op when profiling is off.
- **Logging:** spdlog wired from Phase 0 (task 0.2.4), hidden entirely behind `engine/core/include/aero/core/log.hpp`'s `AERO_LOG_{TRACE,DEBUG,INFO,WARN,ERROR,CRITICAL}` macros — `AERO_LOG_INFO("window {}x{}", w, h)`. spdlog never appears in a public header; CI enforces that.
- **Blender:** installed locally for the `.blend` → glTF import path (auto-detected, or point the editor at the path).

---

## Testing strategy

- **Unit tests (doctest):** core utilities, math, handles, serialization round-trips, the reflection generator's output.
- **Golden/snapshot tests:** cooked-asset determinism, serialization stability.
- **Guard tests:** the five architecture guards (treated as tests).
- **Manual validation gates:** each phase's deliverable gate ([05 — Roadmap](./05-roadmap.md)) is the human acceptance test for that phase.
