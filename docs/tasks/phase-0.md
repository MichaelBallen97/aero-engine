# Phase 0 — Foundations & First Triangle · est. 2–3 mo

> **Goal:** prove the toolchain, build, CI, and the RHI abstraction all work end-to-end. This phase is *also* the C++ learning budget — treat it as such (ADR-001).
>
> **Gate:** Spinning textured cube @60fps on mac/win/linux; CI green (ASan/UBSan) from commit #1
>
> **Phase non-goals:** no ECS, no reflection, no editor — a cube is enough. Anything beyond drawing one textured cube on three OSes with green CI belongs to a later phase.
>
> **Gate artifact:** the spinning-cube app is committed under `/samples/phase-0-cube/`.

---

## Epic 0.1 — Build & CI bootstrap · build-ci

**Goal:** a reproducible CMake+vcpkg build and a 3-OS CI pipeline that exists before any engine code does.
**Definition of Done:** a fresh clone configures, builds, and passes a trivial doctest on macOS, Windows, and Ubuntu in CI, with ASan/UBSan active in Debug.

### 0.1.1 Repo & CMake skeleton · P0 · M
**Goal:** create the repository and the build skeleton every later task builds on.
**Deliverable:** a repo with root `CMakeLists.txt`, presets, and the canonical folder layout that configures successfully on all 3 OSes.
Subtasks:
- Init repo, `.gitignore`, license (MIT), root `CMakeLists.txt`
- `CMakePresets.json` (one preset per platform/config)
- Folder structure: `/engine/{core,platform,rhi,render,scene,physics,audio,assets,script,reflect}`, `/runtime`, `/editor`, `/tools`, `/tests`, `/samples`, `/docs`

### 0.1.2 vcpkg manifest · P0 · S · depends: 0.1.1
**Goal:** pin all third-party dependencies reproducibly from day one.
**Deliverable:** `vcpkg.json` with a pinned baseline; CMake presets resolve dependencies through the vcpkg toolchain.
Subtasks:
- `vcpkg.json` with SDL3; pin a vcpkg baseline commit
- Wire the vcpkg toolchain file into CMake presets
- Verify every dependency's vcpkg port availability at the pinned baseline (SDL3, EnTT, Jolt, quickjs-ng, spdlog, GLM, enkiTS, doctest, fastgltf, KTX/Basis, sdl3-shadercross); document FetchContent/vendoring fallback for any gap

### 0.1.3 GitHub Actions CI matrix · P0 · M · depends: 0.1.2
**Goal:** CI exists from commit #1 — `main` is always green (docs/04).
**Deliverable:** a workflow building all presets on macOS + Windows + Ubuntu with vcpkg caching and a status badge.
Subtasks:
- Workflow across macOS + Windows + Ubuntu
- Configure + build steps; cache vcpkg; status badge in README

### 0.1.4 Sanitizers & test harness · P0 · S · depends: 0.1.3
**Goal:** make the ADR-001 mitigations enforceable — sanitizers and tests run on every commit.
**Deliverable:** Debug preset with ASan/UBSan; doctest integrated; CI goes red on a failing test.
Subtasks:
- Debug preset with ASan/UBSan; integrate doctest
- First trivial test; CI runs tests and fails red on failure

### 0.1.5 Tracy integration · P1 · S · depends: 0.1.1
**Goal:** profile early, not after — Tracy wired from Phase 0 (docs/04).
**Deliverable:** Tracy client in dev builds only, with macro wrappers and one live profiling zone.
Subtasks:
- Add Tracy (dev builds only); macro wrappers; first profiling zone

### 0.1.6 Format & lint configs · P0 · S · depends: 0.1.1, 0.1.3
**Goal:** enforce the docs/04 C++ style mechanically before the first engine source file lands — clang-format and clang-tidy configs committed and checked in CI (docs/04: "config committed; enforced in CI").
**Deliverable:** `.clang-format` and `.clang-tidy` at the repo root; the CI workflow runs the format/lint check and goes red on violations.
Subtasks:
- `.clang-format` encoding the docs/04 style
- `.clang-tidy` configuration
- Wire the format/lint check into the CI workflow as its own step

---

## Epic 0.2 — `core` · core

**Goal:** the zero-dependency foundation layer: handles, math, logging, jobs, time, VFS.
**Definition of Done:** every `core` utility is unit-tested; no third-party type appears in a public `core` header; the math-boundary CI guard is live.

### 0.2.1 `Handle<T>` · P0 · M · depends: 0.1.4
**Goal:** the project-wide resource-reference primitive (ADR-001 mitigation #1) — handles, not pointers.
**Deliverable:** `Handle` template + generational slot pool with unit tests proving stale handles are rejected.
Subtasks:
- `Handle` template (index + generation); generational slot pool/allocator
- `valid()`, equality, `get()` returning invalid on stale generation; unit tests

### 0.2.2 Math types + GLM backend · P0 · M · depends: 0.1.4
**Goal:** the engine's own math surface (ADR-005): public `engine::` types, GLM strictly private.
**Deliverable:** `Vec2/3/4`, `Mat3/4`, `Quat` in `core/math` with the common operations, round-trip tested.
Subtasks:
- Public `Vec2/3/4`, `Mat3/4`, `Quat` in `core/math`; GLM as private backend
- Common ops (transforms, dot/cross, normalize, lookAt, perspective); round-trip tests

### 0.2.3 Math-boundary CI guard · P0 · S · depends: 0.2.2
**Goal:** enforce ADR-005 mechanically — a single leaked `<glm/...>` include is a build failure, not a review comment.
**Deliverable:** CI-wired script that fails when `#include <glm/...>` appears outside `engine/core/src/math/glm_backend.cpp`, the single allowlisted file.
Subtasks:
- Script that fails if `#include <glm/...>` appears outside `engine/core/src/math/glm_backend.cpp`; wire into CI (plus a compile-time probe target covering the public headers)

### 0.2.4 Logging (spdlog) · P1 · S · depends: 0.1.4
**Goal:** one logging API for the whole engine, spdlog hidden behind it.
**Deliverable:** leveled logging callable from any subsystem; no spdlog types in public headers.
Subtasks:
- Thin logging API; levels; no spdlog types in public headers

### 0.2.5 Jobs (enkiTS) · P1 · M · depends: 0.1.4
**Goal:** a task scheduler the engine owns, enkiTS as the private backend.
**Deliverable:** scheduler wrapper with `parallel_for` and a job-graph smoke test.
Subtasks:
- Scheduler wrapper; `parallel_for`; a job-graph smoke test

### 0.2.6 Time & VFS · P1 · M · depends: 0.1.4
**Goal:** frame timing and a virtual file system — the abstraction the `.pak` runtime path (5.1) will mount into.
**Deliverable:** clock/frame timing utilities and a VFS with path resolution, unit-tested.
Subtasks:
- Frame/clock timing; virtual file system abstraction + path resolution

---

## Epic 0.3 — `platform` (SDL3) · platform

**Goal:** the SDL3 wrapper layer: window, events, input, audio device — no SDL type escapes it.
**Definition of Done:** an engine app opens a window, pumps events, reads input, and opens a (silent) audio device on all 3 OSes through engine APIs only.

### 0.3.1 Window & event loop · P0 · M · depends: 0.2.4
**Goal:** the first visible artifact — a window owned by engine code.
**Deliverable:** SDL3-backed window creation + event pump with resize handling behind `engine::platform`.
Subtasks:
- SDL3 init; window-creation wrapper; event pump; resize handling

### 0.3.2 Input · P1 · S · depends: 0.3.1
**Goal:** keyboard/mouse state readable through an engine API (script exposure arrives in 4.4.3; touch in Phase 6).
**Deliverable:** input API with no SDL types public.
Subtasks:
- Keyboard/mouse state behind an engine input API (no SDL types public)

### 0.3.3 Audio device stub · P2 · S · depends: 0.3.1
**Goal:** prove the miniaudio device path early; real playback lands in 3.7.
**Deliverable:** a silent, cleanly opened+closed audio device via `platform`.
Subtasks:
- Open a miniaudio device via `platform`; silent output (real audio in 3.7)

---

## Epic 0.4 — `rhi` + `tools/shaderc` · rhi

**Goal:** the sacred wrapper (ADR-002): an SDL_GPU-free rendering API surface plus the offline HLSL shader pipeline.
**Definition of Done:** a triangle-capable RHI whose public headers contain zero SDL_GPU types, with shaders compiled offline to DXIL/MSL/SPIR-V, guard-enforced.

### 0.4.1 RHI abstraction surface · P0 · L · depends: 0.2.1
**Goal:** design the engine's rendering handle vocabulary — this boundary is the future escape hatch for ray tracing (v3–v4).
**Deliverable:** public handle types for device, swapchain, command buffer, pipeline, buffer, texture, sampler — SDL_GPU-free.
Subtasks:
- Public handles: device, swapchain, command buffer, pipeline, buffer, texture, sampler — SDL_GPU-free API

### 0.4.2 SDL_GPU backend · P0 · L · depends: 0.4.1
**Goal:** implement the surface on SDL_GPU (Vulkan/Metal/D3D12 for free).
**Deliverable:** device init, swapchain, command submission, and resource creation working behind the 0.4.1 surface on all 3 OSes.
Subtasks:
- Device init, swapchain, command submission, resource creation behind the surface

### 0.4.3 `tools/shaderc` · P0 · M · depends: 0.1.2
**Goal:** one HLSL source → all three backend formats, offline, at build time (isolating the SDL_shadercross preview risk, R4).
**Deliverable:** a CLI wrapper with CMake integration and a documented raw-DXC fallback path.
Subtasks:
- SDL_shadercross wrapper CLI; HLSL → SPIR-V/MSL/DXIL
- CMake integration; documented raw-DXC fallback path

### 0.4.4 Shader loading · P1 · S · depends: 0.4.2, 0.4.3
**Goal:** the runtime side of the shader pipeline — load the right format per backend.
**Deliverable:** compiled shaders load and produce a valid pipeline on each OS.
Subtasks:
- Load the right compiled format per backend; pipeline creation

### 0.4.5 RHI-boundary CI guard · P1 · S · depends: 0.4.1
**Goal:** enforce that no SDL_GPU type ever appears in a public engine header (docs/04 guard table).
**Deliverable:** CI-wired script failing on violation, proven against a seeded violation and then cleaned.
Subtasks:
- Scan public headers for SDL_GPU tokens (`SDL_GPU`, `SDL_gpu.h`); fail on hit
- Wire into the CI workflow
- Prove the guard fails on a seeded violation, then remove the seed

---

## Epic 0.5 — `render` v0 · render

**Goal:** the smallest real renderer: clear, upload, draw one textured cube.
**Definition of Done:** the phase gate validated on all 3 OSes.

### 0.5.1 Clear pass · P0 · S · depends: 0.4.2
**Goal:** first pixels — prove the frame loop end-to-end.
**Deliverable:** begin frame → clear color → present, stable across resize.
Subtasks:
- Begin frame, clear color, present

### 0.5.2 Textured cube · P0 · M · depends: 0.5.1, 0.4.4
**Goal:** the canonical hello-world of the whole stack: geometry, uniforms, texture, draw.
**Deliverable:** a textured cube rendered with MVP transform.
Subtasks:
- Vertex/index buffers; cube mesh; MVP uniform
- Texture upload + sampler; draw

### 0.5.3 Frame loop & 60fps validation · P0 · S · depends: 0.5.2
**Goal:** validate the gate — not "it drew once" but "it runs at 60".
**Deliverable:** the spinning cube at 60fps with vsync and an fps counter, checked on macOS, Windows, and Linux; committed under `/samples/phase-0-cube/`.
Subtasks:
- Rotate over time; vsync; fps counter
- **Validate the gate on all 3 OSes**

---

**Phase gate:** Spinning textured cube @60fps on mac/win/linux; CI green (ASan/UBSan) from commit #1.
