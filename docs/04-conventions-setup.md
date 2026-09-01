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
| **RHI-boundary guard** | an SDL_GPU token appears anywhere in `engine/`/`runtime/` outside `engine/rhi/src/sdl_gpu_backend.cpp` (plus a compile-time probe target for public headers) | [0.4.5](./tasks/phase-0.md) |
| **Golden-rule guard** | any `#include` under `/engine` or `/runtime` references `/editor`, or any CMake target under `/engine` or `/runtime` transitively links an `/editor` target or carries `/editor` on its include path (a `lint`-job script plus a configure-time CMake assertion) | [2.1.2](./tasks/phase-2.md) |
| **Audio-boundary guard** | a miniaudio token appears anywhere under `engine/audio`/`engine/scene_audio` (**sources included**, not only public headers), **or** a dependency hook (`find_package`, `find_path`, …) or a non-`aero::` link token enters a vcpkg-free CMakeLists (`engine/assets`/`engine/audio`/`engine/scene_audio`), **or** another CMake file adds a cross-directory `target_link_libraries` to one of those three targets (a `lint`-job script plus the `aero_audio_boundary_probe` compile-time probe, which links `aero::audio` alone) | [3.7.3](./tasks/phase-3.md) |
| **Boundary-probe integrity guard** | any `aero_*_boundary_probe` target's link line stops being exactly one `aero::` library, `PRIVATE` — the one way every compile-time probe rots, and it rots while CI stays green. The probe set is **derived** from `tests/CMakeLists.txt`, never enumerated, so a future probe is covered on arrival | [3.7.3](./tasks/phase-3.md) |
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
4. The eight architecture-guard tests below (each activates once its owning task lands)
5. `clang-format` / `clang-tidy` check
6. Task-local boundary check: no spdlog/fmt types in public engine headers (0.2.4)
7. Task-local boundary check: no enkiTS types in public engine headers (0.2.5; `lint`-job grep + the `aero_jobs_boundary_probe` compile-time probe)
8. Task-local boundary check: no SDL types in public engine headers (0.3.1; `lint`-job step running `.github/scripts/check-platform-boundary.sh` — a script, not a bare grep, because SDL's un-namespaced identifiers collide with legitimate documentation prose that cites them — + the `aero_platform_boundary_probe` compile-time probe)
9. Task-local boundary check: no entt types in public engine headers (1.3.1; `lint`-job step running `.github/scripts/check-scene-boundary.sh` — a script, not a bare grep, because the scene headers cite `entt::basic_registry` in documentation prose — + the `aero_scene_boundary_probe` compile-time probe, which links `aero::scene` alone)
10. Golden-rule check: nothing under `/engine` or `/runtime` may reference `/editor` (2.1.2; two surfaces — a `lint`-job step running `.github/scripts/check-golden-rule.sh` for the textual half, a script rather than a bare grep because 22 first-party comments under `engine/` legitimately say "editor", and `cmake/golden_rule.cmake`'s `aero_assert_golden_rule()` at the end of the root `CMakeLists.txt` for the link/include-directory half, which runs in every configure on every lane; both proved red-on-violation by the hermetic ctest cases `golden-rule.include_scan_e2e` and `golden-rule.link_graph_e2e`)

11. Math-boundary check: no `<glm/...>` include outside `engine/core/src/math/glm_backend.cpp`, the single allowlisted file (`lint`-job step running `.github/scripts/check-math-boundary.sh` + the `aero_math_boundary_probe`)
12. Project, asset and cache no-delete check — now **two checks in one script**: Check A, a NEGATIVE six-file denylist (`editor/src/project.cpp`, `project_file.cpp`, `project_ui.cpp`, `asset_meta.cpp`, `asset_database.cpp` and `asset_cache.cpp` may never call `remove_all` / `std::filesystem::remove` / `std::filesystem::rename` / a bare `::copy`; 2.6.1's code-review round, widened by task 3.1.1 to cover the asset flow and by task 3.1.2 to cover the import cache), plus Check B (task 3.1.3, D13), a POSITIVE two-file allowlist scanning every tracked `editor/src/*.cpp` and refusing a delete/rename anywhere outside `text_file.cpp` (the atomic-write primitive) and `asset_actions.cpp` (the one sanctioned orphan-sidecar delete) — closing the hole a denylist alone cannot: a delete written into a SEVENTH, unnamed file passes Check A silently. Both checks live in `.github/scripts/check-project-no-delete.sh`, proved red-on-violation by the hermetic ctest case `project-no-delete.no_delete_e2e`. A failed `createProject` must never delete a user-chosen directory tree; an invalid `.meta` is never overwritten and an orphaned `.meta` is never deleted except through that one sanctioned, user-initiated action; a stale cache entry is discarded from the in-memory index, never removed as a file. The script's name stayed narrower than its scope on purpose — see the script's own header for why a rename was rejected.
13. Audio-boundary check (task 3.7.3; epic 3.7's Definition of Done, *"no miniaudio type is public (guard-enforced)"*): a `lint`-job step running `.github/scripts/check-audio-boundary.sh`, in two prongs. **Prong A — the no-vcpkg property.** `engine/assets`, `engine/audio` and `engine/scene_audio` link no vcpkg package at all, which is what makes their `PRIVATE` links a real compile-time boundary rather than convention-plus-grep (R12); the guard refuses any of `find_package`/`find_path`/`find_library`/`find_file`/`find_program`/`pkg_check_modules`/`pkg_search_module`/`include_directories`/`link_directories`/`link_libraries`/`add_subdirectory`/**`include`**/**`set_target_properties`**/**`set_property`** in those three files (case-insensitively — CMake command names are; `include` is in the list because *nothing else in prong A follows an `include()`*, so without it a two-line edit moves a `find_package` and a foreign include dir entirely out of the guard's reach), any link token that is not an `aero::` engine target (**any `*_internal` target refused in both spellings** — the `aero::…` alias *and* the raw `aero_…` name, since both exist and both carry a backend INTERFACE by design), any include directory not rooted at `${CMAKE_CURRENT_SOURCE_DIR}` (the `SYSTEM`/`BEFORE`/`AFTER` keywords are admitted as the keywords they are), and **any mention at all** of `aero_assets`/`aero_audio`/`aero_scene_audio` in any **other** tracked CMake file — not a list of commands, because CMake ≥ 3.13 lets a dozen of them reach a target from another directory and there are zero legitimate references to those three names outside their own CMakeLists, which makes 'none' the complete predicate. The guarded files themselves may likewise contain **only** `add_library`, `target_include_directories` and `target_link_libraries`, the three they have ever used. Two vectors reach a target without naming it and are refused directly: a directory-scoped `include_directories`/`link_libraries`/`link_directories` in an ancestor CMakeLists, whose directory properties are inherited by everything below. **Prong B — the miniaudio token ban**, over every tracked C-family file under `engine/audio` and `engine/scene_audio`, **sources included**; item 8's platform guard already covers every public header, and what it cannot reach is an audio `.cpp` reaching for a device type. A script, not a bare grep, for the same reason as items 8–12: three shipped audio headers cite `ma_device_uninit`/`ma_spatializer` in first-party `//` prose and all three CMakeLists carry the word `find_package` inside their own prohibition comments — the script strips comments first and **requires those citations to exist** as its anti-vacuity canaries, so deleting one is a loud exit 2 rather than a quiet pass. The compile-time half is `tests/audio_boundary_probe.cpp`, and it is not interchangeable with the textual half: `aero_audio` links `aero::profiling` `PRIVATE`, so in the `*-release` presets Tracy puts vcpkg's shared include root on `aero_audio`'s **own** compile line and a stray `#include <miniaudio.h>` there compiles clean — the probe, which links `aero::audio` alone, is the only compile-time enforcement that survives Release. Proved red-on-violation by the hermetic ctest case `audio-boundary.guard_e2e`.
14. Boundary-probe integrity (task 3.7.3, taking the handoff task 0.2.3 opened and 0.4.5 §7.1 routed here by name): a `lint`-job step running `.github/scripts/check-boundary-probes.sh`. Every compile-time boundary probe works by linking exactly one `aero::` library, so vcpkg's shared per-triplet `include/` root never reaches its compile line; anything that puts it back — a second library, a foreign include directory, a `-I` in compile options, or the target simply not being built — reduces the probe to something that compiles forever and asserts nothing, **without turning CI red**. The guard states one bounded predicate rather than a list of banned commands: **a probe may be named by exactly two calls, its own `add_library(<probe> OBJECT …)` and its single `target_link_libraries(<probe> PRIVATE aero::x)`, both in `tests/CMakeLists.txt`; any other command, in any tracked CMake file, that names a derived probe is a violation.** That shape is deliberate and was arrived at the hard way — three review rounds each closed the spellings they had been shown and left the class open, because CMake has too many ways to reach a target for enumeration to converge. The link call's own contents are checked separately (exactly one `aero::` library, exactly one `PRIVATE` keyword, exactly one call — a second one appends rather than replaces), as are the two vectors that reach a probe **without naming it**: a directory-scoped `include_directories`/`link_libraries`/`link_directories` in an ancestor of the registry, and `add_subdirectory(<registry dir> EXCLUDE_FROM_ALL)`. The probe set is **derived** from the registry's `add_library(… OBJECT …)` lines filtered to `_boundary_probe$` rather than enumerated in the script, so a future probe is covered the moment it lands; the one hardcoded name is an anti-vacuity canary, an empty derived set is exit 2, and so is a sweep that walked no other CMake file.
15. Cook determinism (task 3.3.3): the `cook-determinism` job downloads the three lanes' cooked artifacts, byte-compares them pairwise (26 `cmp` runs, first divergent byte named on failure), re-checks the frozen manifest `tests/cooker/determinism.sha256` with coreutils `sha256sum` rather than CMake, and runs a checksum-pinned Khronos `ktx validate` 4.4.2 over every cooked `.ktx2`. The manifest itself is enforced six times per push by the ungated `cooker.golden_manifest` and `cooker.texture_golden_manifest` ctest cases (three lanes × two configurations), so the job is defence in depth rather than the mechanism — and the contract is runnable locally with plain `ctest`. The testing-strategy section's "Golden/snapshot tests: cooked-asset determinism" bullet is this. (Named rather than cited by line number: the number was already stale by one and any insertion above it widens the drift.)

**A guard asserting its scan set is non-empty is not asserting that it covers anything.** The scene- and platform-boundary guards additionally verify that the scan spans every engine subsystem shipping a public `include/`, with both sides derived from the tree (self-test 1b) — added by the Phase 2 audit after a narrowed glob was measured to take the scan from 51 files to 8 while still exiting 0. See `.claude/rules/boundary-guards.md`.

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
- **Guard tests:** the eight architecture guards (treated as tests). Measured from `.github/scripts/`, never remembered — the count was stale at "five" for a whole task after `check-project-no-delete.sh` landed.
- **Manual validation gates:** each phase's deliverable gate ([05 — Roadmap](./05-roadmap.md)) is the human acceptance test for that phase.
