# Engineering log — what landed, per task

> **What this is.** The full per-task build history of Aero Engine: what each task
> shipped, what it deliberately did *not* ship, the traps found along the way, and
> the dead ends that must never be retried. It was extracted verbatim from
> `CLAUDE.md`, which had grown to 175k characters — loaded into context in full on
> every session, for content relevant to roughly one session in twenty.
>
> **How to use it.** This file is *not* auto-loaded. Read the entry for a task
> before touching the code it describes; `grep` it for a symbol, guard, or
> dependency before re-deriving anything. `CLAUDE.md` carries the standing rules
> and the current position; this file carries the history behind them.
>
> **What is normative.** Nothing here. `docs/00`–`docs/09` remain the source of
> truth for scope and architecture; on any conflict, those win and this log is
> corrected. Entries are append-only and are never rewritten after the fact.
>
> **Maintenance.** Append one `#### Task N.N.N` entry per landed task, in both
> parts. Do not move history back into `CLAUDE.md`.

---

# Part 1 — Task ledger

What each task shipped, decided, and proved.

## Status snapshot (as extracted)

**Planning is complete; Phase 0 (Foundations & First Triangle) is in progress — the build skeleton plus a sanitized doctest test harness plus dev-builds-only Tracy profiling plus mechanically enforced format/lint configs exist; epic 0.2 (`core`) is CLOSED (0.2.1 handles, 0.2.2 math, 0.2.4 logging, 0.2.5 jobs, 0.2.6 time & VFS); epic 0.3 (`platform`) is CLOSED (0.3.1 window/events, 0.3.2 input, 0.3.3 audio device stub); epic 0.4 (`rhi`) is CLOSED (0.4.1 RHI surface, 0.4.2 SDL_GPU backend, 0.4.3 `tools/shaderc`, 0.4.4 shader loading, 0.4.5 RHI-boundary CI guard); epic 0.5 (`render`) is under way, with 0.5.1 (clear pass), 0.5.2 (textured cube), and 0.5.3 (frame loop & 60 fps validation) landed — the Phase 0 exit gate is reached in code and macOS-validated (PASS), held OPEN pending Windows/Linux on-hardware sign-off; Phase 1 (Reflection, ECS & Serialization) has now OPENED alongside Phase 0's remaining sign-off, with epic 1.1 (`reflect-gen`) now CLOSED — task 1.1.1 (libclang harness), 1.1.2 (annotation detection), 1.1.3 (entt::meta codegen), and 1.1.4 (build-step wiring) all landed — and epic 1.2 (Serialization) now OPENED with task 1.2.1 (JSON writer, generated) and task 1.2.2 (JSON reader, generated) landed, CLOSING Epic 1.2's Definition of Done, with task 1.2.3 (scene serialization format v1) landed on top of that closed DoD.** This is a git repository with a CMake + pinned-vcpkg build, six host-gated presets, a 3-OS GitHub Actions CI matrix, and the canonical folder layout. `aero::core` (STATIC, from `engine/core/CMakeLists.txt`) is the **first compiled engine target** — `aero_tests` is no longer the only one. Epic 0.1 tasks landed so far: 0.1.1 (repo + CMake skeleton), 0.1.2 (vcpkg manifest), 0.1.3 (CI matrix), 0.1.4 (sanitizers + doctest harness), 0.1.5 (Tracy integration), 0.1.6 (format & lint configs). Epic 0.2 (`core`):

## Phase 0 — Foundations & First Triangle

### Epic 0.2 — `core`

#### Task 0.2.1 — Handles & SlotMap (incl. the 0.2.2/0.2.4 STATIC reconciliation)
**0.2.1** landed `engine::Handle<Tag>` (`aero/core/handle.hpp`) and `engine::SlotMap<T, Tag>` (`aero/core/slot_map.hpp`) — the project-wide resource-reference primitive (ADR-001 mitigation #1), with unit tests proving stale-handle rejection — creating `aero::core` as an INTERFACE library, since both are templates; **0.2.2** and **0.2.4** each independently converted that target to **STATIC** on parallel branches (both needed a translation unit — math's GLM backend and logging's spdlog backend respectively) and were reconciled at merge into a single `aero_core` STATIC library building both `src/math/glm_backend.cpp` and `src/log.cpp`, linking `glm::glm` and `spdlog::spdlog` both `PRIVATE`. **0.2.2** exposes `Vec2/3/4`/`Mat3/4`/`Quat` with the common ops via the umbrella header `<aero/core/math.hpp>` (ADR-005); **0.2.4** exposes `AERO_LOG_{TRACE,DEBUG,INFO,WARN,ERROR,CRITICAL}` via `<aero/core/log.hpp>`. `engine/CMakeLists.txt` has `add_subdirectory(core)`, and `aero_tests` links `aero::core` alongside `aero::profiling`. This is also the first engine code the 0.1.6 `clang-format`/`clang-tidy` gates (especially `readability-identifier-naming`) actually lint — 0.2.1, 0.2.2, and 0.2.4 all pass clean, zero new subtractions. **The PRIVATE-link compile-time boundary has one documented limitation, discovered independently by both 0.2.2 and 0.2.4**: vcpkg installs every port into ONE shared per-triplet `include/` directory, so `PRIVATE` linking a backend (GLM, spdlog) makes a stray `#include` a hard compile error only for engine-layer targets that link no vcpkg package directly (verified with a throwaway probe target linking only `aero::core`) — NOT inside `tests/`, which inherits the whole shared root via `doctest::doctest` regardless of what `aero_core` links. See the ADR-005 implementation note in `docs/02-adrs.md` and risk **R12** in `docs/08-risks.md`. 0.2.4's `lint` job gained a `git grep` boundary step enforcing this for spdlog/fmt;

#### Task 0.2.3 — Math-boundary guard
**0.2.3 landed the GLM equivalent**: `.github/scripts/check-math-boundary.sh` (run by the `lint` job) plus the permanent compile-time probe target `tests/math_boundary_probe.cpp`, which links only `aero::core` and fails to build the instant a public math header pulls GLM in.

#### Task 0.2.5 — Jobs-boundary guard
**0.2.5 landed the enkiTS equivalent** — a `lint`-job grep plus a second compile-time probe, `tests/jobs_boundary_probe.cpp` — and, in doing so, re-proved 0.2.3's probe still bites after `aero::profiling` joined `aero_core`'s link line (see the build-commands bullet).

#### Task 0.2.6 — Time & VFS — CLOSES Epic 0.2
**0.2.6 (time & VFS) landed** — purely additive to `aero_core`, appending `src/vfs.cpp` and two public headers: `<aero/core/time.hpp>` (`engine::FrameClock` — per-frame delta/total/frame-count/smoothed-fps timing on `std::chrono::steady_clock` with a disableable spike clamp, plus the free `engine::monotonicSeconds()`), header-only; and `<aero/core/vfs.hpp>` (`engine::VirtualFileSystem`/`FileSystemBackend`/`DirectoryBackend`/`ByteBuffer`) backed by `src/vfs.cpp`, resolving `res://`-scheme virtual paths through an overlay mount table onto a `DirectoryBackend` (a `user://` writable root is reserved-but-not-implemented; task 5.1.1's `PakBackend` plugs into the same `FileSystemBackend` seam). Path normalization rejects `../` escapes and Windows drive-relative paths before any backend sees them. Unlike math/GLM and log/spdlog, **0.2.6 added no third-party dependency and no CI guard** — `<chrono>`/`<filesystem>`/`<fstream>` are standard library and `<filesystem>` is confined to `vfs.cpp` (hygiene, not the boundary rule), so no new `vcpkg.json` entry, `PRIVATE` backend, or grep/probe guard. Epic 0.2 (`core`) is now CLOSED;

### Epic 0.3 — `platform`

#### Task 0.3.1 — Window & event loop — OPENS Epic 0.3
**0.3.1 (window & event loop) landed**, opening epic 0.3 and the `platform` layer: `engine/platform` is the **first engine layer above `core`** and the **first engine target to link a vcpkg package (SDL3) directly** — `aero_platform` STATIC (`engine/platform/CMakeLists.txt`) builds `src/platform.cpp`, links `PUBLIC aero::core` + `PRIVATE SDL3::SDL3 aero::profiling`. `<aero/platform/{event,window,context,platform}.hpp>` expose `engine::platform::{Event,EventType,WindowId,Window,WindowConfig,WindowSize,Context,ContextConfig}`: `Context` is a non-copyable/non-movable, one-per-process, main-thread RAII owner of SDL's video/event subsystem and the process-global event pump (`pollEvent`); `Window` is move-only RAII created only through `Context::createWindow` (returns `std::optional<Window>`, never throws), with SDL fully hidden behind a pimpl. `ContextConfig{headless=true}` selects SDL's `dummy` video driver so the whole surface is testable with no display — that is what CI's headless runners use. The temporary root `find_package(SDL3 CONFIG REQUIRED)` is **gone** (it lived at `CMakeLists.txt:42-44`); `engine/CMakeLists.txt` gained `add_subdirectory(platform)`; root `CMakeLists.txt` gained `add_subdirectory(samples)`, first populated by `samples/phase-0-window` — the first visible artifact, a real resizable window pumped by `engine::FrameClock`. The `lint` job gained an SDL boundary grep (after the enkiTS step), and `tests/platform_boundary_probe.cpp` is the **third** compile-time boundary probe (after math/glm and jobs/enkiTS) — R12's correction from 0.2.5 ("platform must be greps") turned out to be over-cautious: SDL3 is PRIVATE exactly like glm, so the probe mechanism works here too, and both guards ship.

#### Task 0.3.2 — Input
**0.3.2 (input) landed**: `engine::platform` now exposes keyboard/mouse input through `<aero/platform/input.hpp>`'s `InputState` (`keyDown`/`keyPressed`/`keyReleased`, mouse button/position/delta/wheel queries), fed by `Context::pollEvent` as it translates events and frame-reset by the new `Context::newFrame()`; keys are identified by **physical position** (`engine::platform::Key`, D1 — layout-independent WASD), never SDL's own scancodes. `event.hpp` gained six new `EventType`s (`KeyDown/KeyUp/MouseButtonDown/MouseButtonUp/MouseMoved/MouseWheel`) and `Event` was reshaped into a tagged **anonymous union** — still an aggregate, still `std::is_trivially_copyable_v` (guarded by a probe `static_assert`) — with `WindowSize` moved in from `window.hpp` (D12) to double as the resize payload. `src/input.cpp` (new, SDL-free — the fold from `Event` into held/edge state) joined `aero_platform`; the scancode/button/mod mapping (`fromScancode`/`fromSdlButton`/`fromSdlMod`) lives only in `src/platform.cpp`, the one TU that includes SDL. **No new dependency, CI guard, or target** — SDL3 was already linked, and the existing `check-platform-boundary.sh` + `tests/platform_boundary_probe.cpp` (both extended with input assertions) already covered `engine/platform/include`. Reconciles the phase-6 task docs' loose "`engine::input`" prose to the actual namespace, `engine::platform` (D3) — input types sit beside the `Event`/`EventType` they extend rather than in a separate namespace. `samples/phase-0-window` now calls `ctx.newFrame()` each frame and logs key/click/wheel events, quitting on Escape via polled `ctx.input()` — the epic's fullest visible surface yet (window + pump + input).

#### Task 0.3.3 — Audio device stub — CLOSES Epic 0.3
**0.3.3 (audio device stub) landed, CLOSING Epic 0.3**: `engine::platform` now exposes a silent, RAII, move-only `<aero/platform/audio.hpp>`'s `AudioDevice`/`AudioDeviceConfig` — the FIRST use of miniaudio (ADR-006) in the codebase. `AudioDevice::open()` opens AND starts a playback device that continuously writes silence (`ma_silence_pcm_frames` in a Tracy-zoned, alloc/lock/log-free realtime callback), independent of `Context` (no `SDL_Init` needed); `AudioDeviceConfig{.headless=true}` selects miniaudio's null backend — CI's no-hardware path, mirroring `ContextConfig{.headless=true}`'s SDL dummy driver. miniaudio (0.11.25) joined the vcpkg manifest — **0.1.2's port-verification pass had omitted it; now verified at the pinned baseline** — and is consumed **header-only**: the port installs only `miniaudio.h`, so `engine/platform/src/miniaudio_impl.c` is the ONE TU compiling `MINIAUDIO_IMPLEMENTATION` (device-only trimmed via `MA_NO_{ENCODING,DECODING,GENERATION,ENGINE,RESOURCE_MANAGER,NODE_GRAPH}`). That TU is deliberately **`.c`, not `.cpp`**: neither the clang-tidy nor clang-format glob lists `*.c`, so ~90k lines of vendored code are auto-excluded from linting with no override; this required enabling **C** at `project()` scope in the root `CMakeLists.txt` (`LANGUAGES CXX C`) and, in `cmake/sanitizers.cmake`, stripping `/RTC1` from `CMAKE_C_FLAGS_DEBUG` too (MSVC ASan is incompatible with `/RTC1`; the CXX strip already existed, this task mirrors it for C). `engine::platform::AudioDevice::Impl` is a heap pimpl (`ma_context`+`ma_device`, flag-guarded reverse-order teardown) so its address is stable across `AudioDevice` moves — miniaudio's `ma_device` is self-referential and read by the realtime audio thread. The extended `check-platform-boundary.sh` (D8: same script as SDL, not a new audio-specific one — the *device* lives in `engine/platform`; Phase 3.7.3 still owns a *separate* `engine/audio` guard later) and `tests/platform_boundary_probe.cpp` now also reject `<miniaudio.h>`/`ma_` in a public header — miniaudio is the **third** PRIVATE flat-root vcpkg backend the probe bites for (after glm, SDL3), proven by seeding a leak into `audio.hpp` (script + probe both fail) then reverting. Per-OS link libs landed on `aero_platform` PRIVATE (D12): macOS CoreFoundation/CoreAudio/AudioToolbox frameworks; Linux `Threads::Threads`+`${CMAKE_DL_LIBS}`+`m`; Windows none — no new CI apt packages (miniaudio's default backends runtime-link ALSA/PulseAudio via `dlopen`). `samples/phase-0-window` now also opens the silent device (non-fatal on failure) — Epic 0.3's Definition of Done (window + events + input + audio, all through engine APIs, on all 3 OSes) is now met and the epic is **CLOSED**.

### Epic 0.4 — `rhi` + `tools/shaderc`

#### Task 0.4.1 — RHI abstraction surface — OPENS Epic 0.4
**0.4.1 (RHI abstraction surface) landed, OPENING Epic 0.4** (`rhi` + `tools/shaderc`): `engine/rhi` is the **third engine layer** (`core` → `platform` → `rhi`, docs/03), built by `aero_rhi` STATIC (`engine/rhi/CMakeLists.txt`) — the first engine target to link **no vcpkg package at all**, only `PUBLIC aero::core aero::platform` (0.4.2 adds `PRIVATE SDL3::SDL3 aero::profiling`). Six public headers (`<aero/rhi/{handles,format,types,descriptors,device,rhi}.hpp>`, the last an umbrella) ship the whole SDL_GPU-free rendering vocabulary (ADR-002, "the sacred wrapper"): eight `Handle<Tag>` aliases over phantom, never-defined tags (`BufferHandle, TextureHandle, SamplerHandle, ShaderHandle, GraphicsPipelineHandle, SwapchainHandle, CommandBufferHandle, RenderPassHandle`), split into persistent resource handles and transient frame/pass handles; `TextureFormat` (18 curated formats + `Invalid`/`Count` sentinels) with `isDepthFormat`/`hasStencilComponent`/`isSrgbFormat`/`texelBlockSize`/`toString` defined in the task's one TU, `src/format.cpp`; the state/flag enums and small POD types in `types.hpp` (three `constexpr` flag enums — `TextureUsage`/`BufferUsage`/`ColorWriteMask` — with `|`/`&`/`~`/`has()`); every creation/pass descriptor as a trivially-copyable, designated-init-friendly aggregate in `descriptors.hpp`, defaults encoding the ADR-005 conventions (CCW front faces, back-face culling, depth `[0,1]` with 0 = near, clear depth `1.0` + `CompareOp::Less`, vsync); and `Device` — a move-only, heap-pimpl RAII class (`Device::create(const DeviceDesc&) -> std::optional<Device>`) whose full method set is **declared but deliberately NOT defined** until task 0.4.2 — odr-using any member before then is a link error by design (proven once and reverted: naming `engine::rhi::Device::create` as an undefined symbol; `git grep -n "Device::create" -- tests/` is clean on `main`). `tests/rhi_types_test.cpp` and `tests/rhi_format_test.cpp` join `aero_tests` (which now also links `aero::rhi`), covering the vocabulary's compile-time properties (handle identity/layout, aggregate-ness, D16 defaults, flag operators) and `format.cpp`'s classification tables exhaustively — no GPU, no SDL, no behavior yet (that is 0.4.2). **No boundary guard ships yet** (0.4.5's deliverable, mirroring `check-platform-boundary.sh` + its probe) and the three existing boundary probes (math/jobs/platform) are untouched, each still linking exactly one `aero::` library. **No new dependency, no `ci.yml` change** — nothing runs until the backend lands.

#### Task 0.4.2 — SDL_GPU backend
**0.4.2 (SDL_GPU backend) landed, making the surface real**: `engine/rhi/src/sdl_gpu_backend.cpp` is now the ONE TU in the repo that includes `<SDL3/SDL_gpu.h>`, defining every `Device` member declared in 0.4.1's `device.hpp` — the link-error era for `Device::create` and friends is over. `aero_rhi` gained its own `find_package(SDL3 CONFIG REQUIRED)` and now links `PRIVATE SDL3::SDL3 aero::profiling aero::platform_internal` (its `PUBLIC aero::core aero::platform` line is unchanged). The `SDL_Window*` the backend needs for `SDL_ClaimWindowForGPUDevice` crosses the platform→rhi boundary through a new one-window-wide seam (D2): a non-installed internal header (`engine/platform/internal/aero/platform/internal/native_window.hpp`) declaring `engine::platform::internal::NativeWindowAccessor`, exposed through a new header-only `aero_platform_internal` INTERFACE target (alias `aero::platform_internal`) consumed PRIVATE only by `aero_rhi`; `window.hpp`'s only change is an SDL-free forward declaration plus one friend declaration. `Device::Impl` holds eight `SlotMap<XxxSlot, Tag>` pools whose payload types own their SDL object via RAII (destroy* is just `pool.remove()`; the teardown sequence actually lives in `~Impl`, not `~Device`, per the 0.3.1 unique_ptr-move-assign lesson — it waits for GPU idle, disposes any leaked transients, then WARN-and-clears every persistent pool before `SDL_DestroyGPUDevice`). Two small public-header amendments shipped alongside the backend, both recorded per the 0.4.1 D18 amendment protocol: `types.hpp` gained five verified backend-limit constants (`MAX_COLOR_ATTACHMENTS=8`, `MAX_VERTEX_BUFFER_SLOTS=16`, `MAX_VERTEX_ATTRIBUTES=16`, `MAX_PUSH_UNIFORM_SLOTS=4`, `MAX_FRAGMENT_SAMPLER_SLOTS=16`), and `device.hpp`'s `create()` contract comment was corrected — verified against the pinned SDL 3.4.12 source, a headless (dummy-driver) `Context` can create a GPU device only on Windows/D3D12; Metal and Vulkan's `PrepareDriver` require a real video driver, so headless creation gracefully fails (`nullopt` + ERROR) on macOS and Linux. Stale-handle and validation-rejection paths are `AERO_LOG_ERROR` + no-op/false/invalid-handle **without** `assert(false)` — a deliberate, recorded reconciliation of the spec's own D13 parenthetical (assert is reserved for main-thread-ownership checks and internal backend invariants only), because the Debug lanes run the full stale-handle test battery and an abort there would be untestable by construction. Two new tiered test TUs joined `aero_tests` unchanged on its link line: `tests/rhi_device_test.cpp` (tier 0: a headless-create matrix that runs everywhere; tier 1: a real-video `Context` + `Device` per test case, no window — identity pinning, one-per-process, move/inertness, the full E8/D5 validation battery, uploads, command buffers, offscreen render passes, render-pass edge cases, and the teardown-leak WARN path) and `tests/rhi_swapchain_test.cpp` (tier 2: a real visible 320×180 window — claim/identity, a 3-frame acquire→clear→submit loop, cancel-after-acquire, write-only rejection, destroy-while-acquired refusal, and the non-Vsync fail-not-downgrade case), gated by a small tests-local `AERO_REQUIRE_GPU` env-var helper: unset locally, tests skip loudly on a GPU-less machine; set (CI), a missing GPU is a hard failure. `ci.yml`'s three lanes now run the GPU-backed suite for real: Linux installs `mesa-vulkan-drivers`/`libvulkan1` (lavapipe, a software Vulkan ICD) + `xvfb` (a real video driver, needed because the dummy one has no Vulkan/Metal surface support) and wraps both ctest steps in `xvfb-run -a`; macOS uses the runner's native Metal; Windows relies on D3D12's WARP software adapter — all three lanes export `AERO_REQUIRE_GPU: 1`. The first GPU run exercised spec D16 (evidence-gated sanitizer suppressions): LSan on the Linux Debug lane — the only lane where LSan runs — flagged ~3.1 MB of allocations rooted entirely in SDL's x11-video-init and Vulkan-probe internals (third-party only; engine frames appear solely as mid-stack call sites; evidence in PR #16), so `tests/lsan.supp` suppresses exactly those four observed frames, wired via a Linux-only `LSAN_OPTIONS` on the Debug ctest step; first-party leak detection stays fully on, and that file must never grow without observed CI evidence. `samples/phase-0-window` is untouched (D17) — the epic's first visible RHI artifact stays 0.5.1's clear pass; the rhi boundary guard (grep script + compile-time probe linking `aero::rhi` alone) landed at 0.4.5 as `.github/scripts/check-rhi-boundary.sh` + `tests/rhi_boundary_probe.cpp`.

#### Task 0.4.3 — `tools/shaderc`
**0.4.3 (`tools/shaderc`) landed**: the vcpkg route to `sdl3-shadercross` is dead on macOS — the pinned baseline's port depends unconditionally on `directx-dxc`, which vcpkg marks unsupported on `arm64-osx` (a real `vcpkg install --dry-run` proves it; **correction to 0.1.2's port-verification pass**, which checked port name+version but not transitive `supports`, the same species of gap 0.3.3's miniaudio omission found) — so `tools/shaderc/bootstrap.cmake` instead acquires SDL_shadercross `1ca46e0e…` (vendored DirectXShaderCompiler + SPIRV-Cross, built from source) directly from its pinned commit into a per-user cache (`~/.cache/aero-engine/shadercross`, `AERO_SHADER_TOOLS_ROOT`-overridable), identically on all three hosts, without touching `vcpkg.json` or the `/vcpkg` submodule pin — the one configuration upstream's own CI tests on macOS. `aero_shaderc` (`tools/shaderc/src/main.cpp`) is the first-party CLI that links the built `SDL3_shadercross` library in-process: HLSL -> SPIR-V always (the reflection source), then MSL/DXIL as requested, writing sidecar artifacts (`.spv`/`.msl`/`.dxil`) plus aero-schema `.json` metadata only after every requested step has succeeded — this frozen CLI grammar + JSON schema is the R4 isolation seam; a documented raw-DXC + SPIRV-Cross fallback recipe lives in `tools/shaderc/README.md`. **A real gap the plan did not anticipate, found and fixed during implementation**: building `SDL3_shadercross` as a shared library (mandatory — a static export would dangle on the vendored DXC/SPIRV-Cross archives, which are `EXCLUDE_FROM_ALL` and never separately exported) requires SDL3 itself to be a shared build, but vcpkg's `arm64-osx` and `x64-linux` triplets both default to **static** linkage (only `x64-windows` is dynamic) — so `bootstrap.cmake` additionally acquires a **private, dynamic-triplet SDL3** (`arm64-osx-dynamic`/`x64-linux-dynamic`) via a classic-mode (non-manifest) install using the SAME pinned vcpkg tool, purely for this one purpose; `aero_shaderc` links that exact instance by raw path rather than a second `find_package(SDL3 …)` call, which CMake silently short-circuits to the FIRST resolution's cache entry regardless of a different `PATHS` argument (verified against CMake's own documented behavior) — and would otherwise permanently clobber the shared `SDL3_DIR` cache entry that `tests/CMakeLists.txt`'s own later `find_package(SDL3 CONFIG REQUIRED)` call depends on. `cmake/shaders.cmake`'s `aero_add_shaders()` derives each shader's stage from the mandatory `.vert.hlsl`/`.frag.hlsl` filename suffix at configure time (a `FATAL_ERROR` otherwise — the function is the stage oracle; the tool itself never sniffs filenames) and wires one `add_custom_command` per shader plus an aggregate target in `ALL`, so every preset's build is a living compile test of every shader in the tree; `shaders/triangle.{vert,frag}.hlsl` are the first proof, compiling to `triangle.{vert,frag}.{spv,msl,dxil,json}` under `build/<preset>/shaders/` on every host — 0.5.x's seed shaders. A net-new CTest harness (`tests/shaderc/run_case.cmake` + `verify_artifacts.cmake`, no prior `cmake -P`/external-binary `add_test` precedent) runs the real binary and asserts its own exit code (several of the 13 registered cases are deliberately negative, which a raw `add_test` cannot express — any non-zero exit would read as a ctest failure regardless of whether it was expected), gated on the new `AERO_SHADER_TOOLS` option (default ON; `OFF` is the escape hatch for constrained/offline machines, never set in CI). `ci.yml` gained one `actions/cache@v4` step per lane, keyed on the toolchain SHA with no `restore-keys` (a pin bump must rebuild, never resurrect a stale toolchain). No engine, runtime, editor, or samples code changed; `vcpkg.json` and the `/vcpkg` submodule pin are byte-untouched; no new boundary guard (tools are on the editor side of the boundary-rule invariant, like Assimp).

### Epic 0.5 — `render`

#### Task 0.5.1 — Clear pass — OPENS Epic 0.5
**0.5.1 (Clear pass) landed, OPENING Epic 0.5 and the `render` layer**: `engine/render` is the fourth engine layer, `aero_render` STATIC (`engine/render/CMakeLists.txt`) building `src/renderer.cpp`, linking `PUBLIC aero::rhi` + `PRIVATE aero::profiling` — the first engine target above `rhi` and (like `rhi` at 0.4.1) linking **no vcpkg package**. `<aero/render/renderer.hpp>` exposes `engine::render::{Renderer, Frame, RendererConfig}`: a move-only RAII `Renderer` that owns one window's swapchain and drives begin→clear→present; `Frame` carries the open pass/extent/command-buffer so 0.5.2 records draws with no API change. No third-party type appears (no pimpl needed); **no new dependency, CI job, or boundary guard** — `check-platform-boundary.sh` + `check-rhi-boundary.sh` already scan `engine/render/`. `samples/phase-0-clear` is the first visible RHI/render artifact (a pulsing cleared window, vsync-paced); `tests/render_clear_test.cpp` joins `aero_tests` (which now also links `aero::render`) with a tier-0 config check + tier-2 clear/resize/move/leak cases on the existing `AERO_REQUIRE_GPU` ratchet. `engine/CMakeLists.txt` gained `add_subdirectory(render)`; `samples/CMakeLists.txt` gained `add_subdirectory(phase-0-clear)`.

#### Task 0.5.2 — Textured cube
**0.5.2 (Textured cube) landed**: the canonical hello-world of the whole stack — the codebase's FIRST vertex/index buffers, first uploaded texture + sampler, first matrix through `pushVertexUniforms`, and first `drawIndexed`, all recorded through the 0.5.1 `Renderer`/`Frame` API with **no signature change**. The one 0.5.1-forecast engine amendment shipped: `RendererConfig` gained `bool depth = false`; with `depth = true`, `create()` auto-picks the depth format (`D32Float → D24Unorm → D16Unorm` via `supportsTextureFormat`, exposed as `depthFormat()`, `Invalid` when off) and `beginFrame` lazily creates and **resizes** a Renderer-owned depth `TextureHandle` at the acquired swapchain extent (recreated on extent change; destroyed by `~Renderer`/move-assign; depth-create-fail mid-frame submits the acquired image and skips the frame) — with `depth = false` behavior is byte-for-byte 0.5.1 (`render_clear_test`/`phase-0-clear` untouched). `shaders/cube.{vert,frag}.hlsl` are the first shaders in the tree to actually declare `register(...)` bindings (MVP cbuffer `b0,space1`; texture/sampler `t0/s0,space2` — the 0.4.3 F8 binding law, previously comment-only). `samples/phase-0-cube/` is the phase-0 gate artifact (STATIC 3/4-view cube at 0.5.2 by user-approved scope split — time-based rotation, the fps counter, and the 3-OS 60 fps gate are 0.5.3), with a sample-local `cube_mesh.hpp` (24 verts / 36 `Uint16` indices, outward-CCW, per-face UV + per-face color; procedural 256² grey/white checkerboard) and the MVP (`perspective(radians(60), aspect, 0.1, 100) * lookAt * toMat4(fromAxisAngle(...))`) rebuilt every frame from `frame.extent()` — resize-correct, column-major **no-transpose** upload (mat4.hpp's contract, visually confirmed). `tests/render_cube_test.cpp` joined `aero_tests` (tier-0 mesh invariants + GPU-gated tier-2: format picks, full pipeline build, ≥3 `drawIndexed` frames, resize + depth-resize, clean teardown; the TU is wrapped in `#if AERO_SHADER_TOOLS_ENABLED` like `rhi_shader_pipeline_test.cpp`). **No new dependency, CI job, or boundary guard; `ci.yml` is byte-identical** — but greening the Linux Debug lane took SIX CI runs and produced `tests/vulkan_stack_pin.cpp`: the cube is the first test to actually RASTERIZE on lavapipe (CI's software Vulkan ICD), whose per-core worker pool leaks a ~56-byte per-thread struct at process exit (224 B / 4 allocations — doctest was 253/253 green on every red run); SDL's GPU-Vulkan backend loads/unloads `libvulkan.so.1` around EACH `Device` lifecycle and Ubuntu's loader unmaps its ICDs with it, so at LSan-time the leak frames read `<unknown module>` and NO name-based `lsan.supp` entry can match. The pool is **not disableable** (`LP_NUM_THREADS=0` and `MESA_SHADER_CACHE_DISABLE`/`MESA_GLSL_CACHE_DISABLE` all left the leak byte-identical) and `LD_PRELOAD` is fatal twice over (aborts ASan's "runtime must come first" check; leaks Mesa's `libLLVM` into the separate shaderc test processes, clashing with vendored DXC). The landed fix: `vulkan_stack_pin.cpp` (Linux-only, static-init before `main`) `dlopen`s the lavapipe ICD **itself** with `RTLD_NODELETE` — the mapping then survives every later `dlclose`, the frames resolve to real module names, and two module-scoped `lsan.supp` entries (`leak:libvulkan_lvp`, `leak:libLLVM`) match: the ONE documented exception to that file's never-module-wide rule, admissible because every engine GPU resource is pool-tracked and released by `~Device` before LSan runs (rationale in the lsan.supp header; pinning only the *loader* was proven insufficient — this loader still unmaps its ICDs). Future GPU-draw tests inherit the pin + suppressions for free — do **not** re-litigate the lavapipe leak. Also verified at source during this task: SDL_GPU's canonical NDC is +Y-up and the Vulkan backend flips the viewport (`SDL_gpu_vulkan.c` "Viewport flip for consistency with other backends", `KHR_maintenance1` negative-height) — engine code never Y-flips per backend, exactly as ADR-005 assumes.

#### Task 0.5.3 — Frame loop & 60 fps validation — Phase 0 exit gate
**0.5.3 (frame loop & 60 fps validation) landed, reaching the Phase 0 exit gate in code**: `samples/phase-0-cube` now SPINS (time-based rotation from `FrameClock::totalSeconds()`, frame-rate-independent, `fmod`-wrapped), shows a live fps counter (window title ≤ 4 Hz + a 1 Hz `AERO_LOG` line, both from `FrameClock::fps()`), and prints an OBJECTIVE exit verdict from a NEW sample-local, pure, header-only `fps_gate.hpp` (`cube::FpsGate`/`GateVerdict` — raw `monotonicSeconds()` present-to-present intervals, warm-up skipped, minimized gaps excluded, lower-bound rule `avg ≥ 58 ∧ worst-frame ≥ 30 ∧ ≥ 120 samples`, so a high-refresh panel passes honestly). **Zero engine change (D1)** — every API already shipped (`fps()`, `Window::setTitle`, depth `Renderer`, per-frame MVP); the diff is confined to `samples/phase-0-cube/` (+ two comment-only CMakeLists), with `engine/**`/`tests/**` (incl. `render_cube_test.cpp`)/`ci.yml`/`vcpkg.json`/boundary guards byte-identical, and **no new test TU (D7** — `fps_gate.hpp` stays out of `tests/` like `cube_mesh.hpp`; the `static evaluate` verdict rule is review-gated). Two clang-tidy fixes the Linux lint lane needed but a format-only local check misses: `std::lround(clock.fps())` for the title readout (the `static_cast<int>(fps + 0.5F)` idiom trips `bugprone-incorrect-roundings`) and `enum class GateVerdict : std::uint8_t` (`performance-enum-size`, matching every engine enum). **macOS validated PASS** (`samples/phase-0-cube/VALIDATION.md`: MacBook Pro M1 Pro, 60 Hz, avg 60.0 / worst 47.5 fps over a clean focused run; vsync confirmed locking 60 fps @ 60 Hz and ~120 fps @ 120 Hz ProMotion, never uncapped); **Windows/Linux deferred to a code-free on-hardware follow-up, so the Phase 0 gate is documented OPEN** in that ledger (CI cannot prove @ 60 fps: no vsync/display, Linux = lavapipe software raster). Validation gotcha for the Windows/Linux follow-up: the gate correctly reports MARGINAL if the window loses foreground focus mid-run (macOS App Nap freezes it 1–2 s, counted as one slow frame) or the display refresh is changed mid-run — the authoritative row needs a short, focused, hands-off run.

## Phase 1 — Reflection, ECS & Serialization

### Epic 1.1 — `reflect-gen`

#### Task 1.1.1 — libclang harness — OPENS Phase 1
**1.1.1 (libclang harness) landed, OPENING Phase 1 and Epic 1.1 (reflection, ADR-004)**: `tools/reflect-gen` is the second first-party `/tools` CLI (after `tools/shaderc`, 0.4.3) and, like `engine/rhi` at 0.4.1, the first to link **no vcpkg package at all** — `aero_reflect_gen` (`tools/reflect-gen/CMakeLists.txt`, gated by the new root option `AERO_REFLECT_TOOLS`, default ON) links only system-discovered LLVM 18's libclang (the stable C API, `<clang-c/Index.h>` — never a C++ Clang/LLVM header) plus the standard library, discovered via `find_package(Clang 18 CONFIG QUIET)` with a `find_library`/`find_path` fallback, both hinted by the cache variable `AERO_LLVM_ROOT` (per-OS-defaulted, `-D`/env-overridable). `aero_reflect_gen` (`tools/reflect-gen/src/main.cpp`, one TU) parses a translation unit given on argv, walks its AST with libclang's C API, and reports it: the frozen CLI contract is `aero_reflect_gen [--all] [--main-file-only] [--version] [--help] <input> [-- <clang args>…]`, exit `0` clean / `1` usage error / `2` parse failure / `3` I/O error, an indented AST walk to stdout and diagnostics to stderr only — the surface 1.1.2–1.1.4 extend but never break; the task deliberately stops at *parse + walk*. `tests/reflect-gen/run_case.cmake` (a `cmake -P` driver mirroring `tests/shaderc/run_case.cmake`) registers 12 CTest cases against two tracked fixtures (`tests/reflect-gen/fixtures/{plain_component,engine_component}.hpp` — the repo's first `[[engine::component]]` spellings, correctly confined to `tests/`; the real first *reflected* component is task 1.3.2) plus the real, unmodified `engine/core/include/aero/core/handle.hpp` (the "parses engine headers" proof, zero diagnostics on all three lanes) — no doctest TU, no tool code linked into `aero_tests` (a CLI's honest test is its process boundary). **Two integration gaps the plan did not anticipate, found and fixed while de-risking this task locally, before any test was written**: (1) because `aero_reflect_gen` is a standalone executable living outside the LLVM install prefix, libclang's internal driver cannot locate its own resource directory (the compiler-builtin headers, e.g. `stdarg.h`) from `argv[0]` alone — every parse needs an explicit `-resource-dir <AERO_LLVM_ROOT>/lib/clang/18` alongside `-isysroot`, or even `<cstdint>` fails to resolve; applied unconditionally on every OS, not just macOS, in the test driver's `CLANG_ARGS` computation (`tests/CMakeLists.txt`). (2) `CMAKE_OSX_SYSROOT` is **empty** in this project's own cache on an incrementally-reconfigured build tree (this dev machine), so gating the macOS SDK pin on "`CMAKE_OSX_SYSROOT` is truthy" (the literal wording of the plan's own contingency) would have silently skipped it; the pin (needed because the default macOS 26 SDK's libc++ headers are too new for clang-18 — the same class of issue the repo's local `clang-tidy` workaround already documents) is instead attempted **unconditionally** on Apple via `xcrun --sdk macosx15.4 --show-sdk-path`, falling back to `CMAKE_OSX_SYSROOT` and then to an unpinned parse (with a loud `WARNING`, never silently) only if neither resolves. A third, smaller finding: clang-18's actual unknown-attribute diagnostic text is `"unknown attribute 'component' ignored"` — the `engine::` vendor-namespace prefix is not included in the quoted name, unlike the spec's illustrative quote; the `annotation_visible` case asserts on `"unknown attribute"`, the substring verified present. **No vcpkg/boundary-guard change** — libclang is already on the tools-only side of the dependency-placement invariant (docs/01/03/04 unchanged), nothing links `aero_reflect_gen` so the runtime-purity guard (5.2.2) is satisfied structurally, and no new `cmake/*.cmake` module ships (that is 1.1.4's). `ci.yml` gained per-lane LLVM 18 provisioning (Linux apt `libclang-18-dev llvm-18-dev`; macOS `brew install llvm@18`; Windows `choco install llvm --version=18.1.8` + an `AERO_LLVM_ROOT` env export) — no cache step, since nothing is built from source, unlike shaderc's toolchain.

#### Task 1.1.2 — Annotation detection
**1.1.2 ("Annotation detection") landed**: `aero_reflect_gen` gained a new `--components` mode, additive to the same one TU (`tools/reflect-gen/src/main.cpp`) — the raw AST walk (task 1.1.1) is unchanged and still the default. Because a bare `[[engine::component]]` is discarded by Clang (no attribute cursor survives — only a `-Wunknown-attributes` warning, exactly what 1.1.1's `annotation_visible` case keys on), the annotation is authored through a new **`AERO_COMPONENT` macro** that expands to `[[clang::annotate("engine::component")]]` — a first-class `CXCursor_AnnotateAttr` AST node — when the parse-marker `AERO_REFLECT_PARSE` is defined, and to **nothing** under the real compiler (zero attribute, zero warning, zero runtime cost); `aero_reflect_gen` **auto-injects** `-DAERO_REFLECT_PARSE=1` at the front of every parse (both modes) so no caller manages the marker, verified inert against every 1.1.1 fixture (byte-identical walk output, all 13 existing cases still green). `--components` detects every `struct`/`class` **definition** carrying that annotation (both kinds; a direct-child `CXCursor_AnnotateAttr` spelled exactly `engine::component`), builds its fully-qualified name by walking `clang_getCursorSemanticParent`, and collects its non-static data members (`CXCursor_FieldDecl` only — statics/methods/nested types excluded) in declaration order into a new in-memory `Component`/`Field`/`FieldCategory` model (the 1.1.3 codegen handoff). Each field is classified on its **canonical** type: the builtin range `[CXType_Bool, CXType_LongDouble]` → `primitive`; canonical spelling `engine::Vec3`/`engine::Quat` (a leading elaborated `struct`/`class` keyword is stripped first, via a shared `stripElaboratedKeyword` helper also applied to the displayed type name, so the match is host-invariant on the Windows/MSVC-compat lane) → `vec3`/`quat`; anything else (`Vec4`, `Mat4`, `std::string`, a nested struct, …) → `unsupported` — lenient, not fatal: an unsupported field is still collected and listed tagged `[unsupported]`, with a warning naming it on stderr, and the exit code stays 0. The `AERO_COMPONENT` macro lives at `tests/reflect-gen/fixtures/aero_reflect.hpp` — a deliberately **tests-local** home (zero `engine/` files touched, matching 1.1.1's own footprint) — promoted to a permanent `engine/reflect/` public header only when task 1.3.2 authors the first real component in engine code. Five new fixtures joined `tests/reflect-gen/fixtures/` (`aero_reflect.hpp`, `component_basic.hpp`, `component_unsupported.hpp`, `component_multi.hpp`, `component_tag.hpp`); the two 1.1.1 fixtures are byte-untouched, and `plain_component.hpp`'s literal `[[engine::component]]` is reused as the no-false-positive proof (zero components detected — the literal spelling produces no annotate cursor, F1). `tests/reflect-gen/run_case.cmake` gained nine new cases (13 → 22 total `reflect-gen.*` ctest cases): `components_basic`, `components_categories`, `components_unsupported`, `components_multi`, `components_none`, `components_tag`, `components_determinism`, plus `components_bad_syntax`/`components_missing_input` (AC-10's exit-2/exit-3 parse-contract paths exercised under `--components`). One clang-tidy fix the Linux lint lane would need but a format-only local check misses: `buildQualifiedName`'s outermost-first join uses `std::reverse` + a plain range-based `for` rather than a manual `rbegin()/rend()` loop (`modernize-loop-convert`). No `engine/`/vcpkg/CI/CMake-target change — `tools/reflect-gen/CMakeLists.txt` is untouched, and the frozen 1.1.1 CLI + all 13 existing tests are intact.

#### Task 1.1.3 — entt::meta codegen
**1.1.3 (entt::meta codegen) landed**: `aero_reflect_gen` gained a new `--emit-meta` mode (plus `-o <file>`), additive to the same one TU (`tools/reflect-gen/src/main.cpp`) — the AST walk and `--components` are unchanged, dispatch precedence is `--emit-meta` > `--components` > walk. `--emit-meta` reuses the exact 1.1.2 detection walk and serializes the resulting `Component`/`Field` model as a compilable `entt::meta` registration translation unit: `entt::meta_factory<T>{}.type("T"_hs, "T").data<&T::field>("field"_hs, "field")…;` per detected component, inside an explicitly-named, caller-invoked `aero_reflect_register_<sanitized-stem>()` function — **never** static-init auto-registration, a deliberate choice (approach B) to dodge the static-lib dead-code-elimination footgun task 1.3.2's engine-side static lib will hit. Unsupported fields get a `// skipped: <name> (<type> — unsupported)` comment after the `.data<>()` chain's terminating `;` plus the same stderr warning `--components` already emits (exit stays 0); a zero-field tag component emits `.type()` with no `.data<>()` line; zero detected components emits a register function containing only a `// no engine::component annotations detected` comment. `Vec3`/`Quat`-typed fields are registered as members of their owning component, never as standalone `entt::meta` types (D5). Generation is gated on a clean parse (`maxSeverity < Error`) so a parse failure never opens/leaves a `-o` output file; `-o` writes in **text mode**, keeping the file byte-identical to the stdout form on every OS. `entt` (3.16.0, the pinned vcpkg baseline — no `builtin-baseline`/submodule bump) joined the manifest alphabetically between `enkits` and `glm`. `tests/reflect-gen/fixtures/component_codegen.hpp` (`struct ReflectSample`, a distinct name from `component_basic`/`_unsupported`'s `Transform`) is the single runtime-codegen fixture; `tests/CMakeLists.txt` gained a build-time `add_custom_command` (generation MUST be build-time, not configure-time, since `aero_reflect_gen` doesn't exist until this same build produces it) that runs `aero_reflect_gen --emit-meta` over that fixture into `build/<preset>/tests/reflect-generated/component_codegen.meta.gen.cpp` (never committed, outside `git ls-files`/lint globs), plus `find_package(EnTT CONFIG REQUIRED)` and the new standalone doctest target `aero_reflect_meta_test` (Epic 1.1's first runtime doctest target; links `doctest::doctest EnTT::EnTT aero::core`, PRIVATE, and — since it is a single-TU target with no shared `tests/test_main.cpp` — supplies its own `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`) that calls the generated register function and asserts against `entt::resolve<ReflectSample>()`/`entt::resolve("ReflectSample"_hs)`/`.data(...)`, proving "generated registration code, compiled into the build, registered at startup" end-to-end. Nine new process-boundary `emit_*` ctest cases (`tests/reflect-gen/run_case.cmake`, plus a new `aero_expect_file_contains` file-grep helper) join the existing 22 (13 → 22 → **31** total `reflect-gen.*` cases), covering the emitted shape, `-o` byte-identity, skip+warning, multi/namespaced, tag, zero-component, determinism, and the exit-2/exit-3 parse-contract paths under `--emit-meta`. Two local de-risking findings the plan's Step 0 anticipated as contingencies and both fired: `entt::meta_reset()` is declared in `<entt/meta/factory.hpp>`, not `<entt/meta/{resolve,meta}.hpp>` — `meta_test.cpp` consolidates to `#include <entt/entt.hpp>` per the plan's documented fallback; and the generated register function's name (`aero_reflect_register_<stem>`) is deliberately snake_case (the frozen cross-boundary contract, D3/D7), which trips `readability-identifier-naming` on its `meta_test.cpp` forward declaration — a single scoped `NOLINTNEXTLINE(readability-identifier-naming)` there, the codebase's first NOLINT. `aero_tests`' link line and all four boundary-probe `target_link_libraries` lines are byte-identical; no `engine/`/`runtime/`/`editor/`/`samples/`/`ci.yml`/boundary-guard change.

#### Task 1.1.4 — Build-step wiring — CLOSES Epic 1.1
**1.1.4 ("Build-step wiring") landed, CLOSING Epic 1.1**: `aero_reflect_gen` gained `--depfile <file>`, additive to the same one TU (`tools/reflect-gen/src/main.cpp`) — valid only alongside `--emit-meta` **and** `-o` (usage error otherwise, D6), written via a new named `writeDepfile()`/`inclusionVisitor()`/`escapeDepfilePath()` trio using `clang_getInclusions()` (the tool's first use of that libclang C-API entry point), only after the `-o` write fully succeeds and only on a clean parse (D8) — a Makefile-format rule (`<abs -o path>: <sorted deduped escaped abs deps>`), machine-local by nature (a depfile is build-tree metadata like a compiler's own `.d`, not required to be byte-identical across machines the way the generated `.cpp` is). The new `cmake/reflect.cmake` module (`include()`d at ROOT scope, right after the `AERO_REFLECT_TOOLS` option — closing the F8 ordering trap 1.1.1–1.1.3 left open, D2) now owns, for the whole tree: the `AERO_LLVM_ROOT` discovery chain (moved verbatim out of `tools/reflect-gen/CMakeLists.txt`, which keeps only `find_package(Clang)`/`find_library` — the half that *links* the tool) and the per-OS `AERO_REFLECT_CLANG_ARGS` (moved verbatim out of `tests/CMakeLists.txt`, byte-identical content, V7-verified); and defines the reusable `aero_reflect_generate(<target> HEADERS <hdr>... [INCLUDE_DIRS <dir>...] [DEFINES <NAME[=VAL]>...] [AGGREGATOR <name>])` (mirroring `aero_add_shaders()`, 0.4.3): one `add_custom_command(... DEPFILE ...)` per header emitting `<stem>.meta.gen.cpp` under a per-target `${CMAKE_CURRENT_BINARY_DIR}/reflect-generated/<target>/` directory (D5, collision-free across targets), plus one machine-generated aggregator TU (`file(CONFIGURE OUTPUT ... CONTENT ... @ONLY)` — write-if-different, V1-confirmed, so an unchanged reconfigure touches no generated-file mtime, AC-7) that forward-declares and calls every per-header register function in **HEADERS-list order** (D4/D15) inside one explicitly-named, caller-invoked function (never static-init auto-registration — same footgun 1.1.3 already avoided). `-DAERO_REFLECT_TOOLS=OFF` makes the function a defined no-op (D11); calling it twice on one target, with zero `HEADERS`, or before the target exists is a configure-time `FATAL_ERROR` (D18/E3/E7). The 1.1.3 hand-wired `add_custom_command` in `tests/CMakeLists.txt` is now GONE, replaced by one `aero_reflect_generate(aero_reflect_meta_test HEADERS reflect-gen/fixtures/component_codegen.hpp reflect-gen/fixtures/component_wiring.hpp INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/engine/core/include")` call — `aero_reflect_meta_test`'s link line (`doctest::doctest EnTT::EnTT aero::core`) is byte-identical. A second fixture, `component_wiring.hpp` (`struct ReflectWiring`, a `Vec3` among its fields), joined `tests/reflect-gen/fixtures/` so the generated aggregator is proven over more than one header; `meta_test.cpp` gained a second TEST_CASE (plus a second NOLINT'd forward-decl for the aggregator fn) proving one call registers both components. Six new `depfile_*` process-boundary cases joined `run_case.cmake` (format/validation/gating/space-escaping/determinism), and a net-new `reflect-gen.incremental_e2e` ctest case (`tests/reflect-gen/incremental_e2e.cmake`, a `cmake -P` driver mirroring the shaderc external-driver precedent) hermetically copies a tracked-but-never-`add_subdirectory`'d nested probe project (`tests/reflect-gen/incremental/`, a self-contained `Widget` component reaching a transitive `widget_types.hpp` — the depfile's own proof target) into a scratch dir, configures it against the REAL `cmake/reflect.cmake` + the already-built `aero_reflect_gen` binary, and drives five builds asserting: first build generates + regen-log-lines appear; second build is Ninja's verbatim `no work to do` (V6, never mtimes, E13); touching the component header regenerates; touching the *transitive* header also regenerates (the actual depfile proof, D13); touching an unrelated TU does NOT trigger regen; the built probe links and runs. 31→**39** `reflect-gen.*` ctest cases (+7 `depfile_*`, +1 `incremental_e2e`); `aero_reflect_meta_test`'s name/count unchanged. Local verification (both macOS configs) matches the spec exactly: V7's parity diff confirmed byte-identical `CLANG_ARGS` (prefix-only rename); a real-tree spot-check (`touch engine/core/include/aero/core/math/constants.hpp`) regenerated **both** generated TUs (both fixtures transitively include `aero/core/math.hpp`, so both correctly react — a stronger proof than the single-TU case the spec's own AC-7 wording anticipated, not a contradiction of it) and a second rebuild was `no work to do`; a bare reconfigure left the aggregator's mtime untouched (write-if-different, V1); `-DAERO_REFLECT_TOOLS=OFF` dropped `ctest -N` to the same 14 shaderc-only + `aero_tests` inventory 1.1.3 already established, ON restored all 53.

### Epic 1.2 — Serialization

#### Task 1.2.1 — JSON writer, generated — OPENS Epic 1.2
**1.2.1 ("JSON writer, generated") landed, OPENING Epic 1.2 (Serialization)**: `aero_reflect_gen` gained a new `--emit-json` mode, additive to the same one TU (`tools/reflect-gen/src/main.cpp`) — dispatch precedence is now `--emit-json` > `--emit-meta` > `--components` > walk; `--emit-json` and `--emit-meta` are mutually exclusive (one `-o` holds one artifact — requesting both is a usage error, exit 1), and `--depfile` now accepts either emit mode (requires `-o` **and** one of `--emit-meta`/`--emit-json`). `--emit-json` reuses the exact 1.1.2 detection walk and serializes the resulting `Component`/`Field` model as a compilable JSON-serializer translation unit: a free function `void aeroWriteJson(engine::JsonWriter&, const T&)` per detected component, wrapped in the component's namespace when it has one (a new `splitQualifiedName` helper, splitting at the last `::`) so plain ADL resolves the call at any call site — no registry, no aggregator, no register-at-startup concept (unlike `--emit-meta`), since serialization is called directly by name wherever it is needed. Every supported field becomes one uniform `writer.key("field"); engine::reflect::writeJson(writer, value.field);` line; unsupported fields get the same `// skipped:` comment + stderr warning `--emit-meta`/`--components` already emit (exit stays 0); a zero-field tag component emits `{}`; zero detected components emits only a `// no engine::component annotations detected` comment. **`engine/reflect` stood up** as a NEW engine layer above `render` (`engine/reflect/CMakeLists.txt`, `aero_reflect` STATIC, alias `aero::reflect`, `PUBLIC aero::core`, no `find_package`, no boundary guard — like `engine/render` at 0.5.1, the boundary rule is satisfied by construction since it wraps no third-party type) and **builds unconditionally**, independent of `AERO_REFLECT_TOOLS` — real engine code, not tool output. Two new public headers: `<aero/reflect/json_writer.hpp>`'s `engine::JsonWriter`/`JsonWriterConfig` (a hand-rolled streaming, DOM-free JSON writer — a `Frame`-stack state machine placing commas/newlines/indentation, `std::to_chars`-backed shortest-round-trip number formatting, non-finite floats/doubles → `null`, manual `\uXXXX` escaping for control characters) and `<aero/reflect/serialize.hpp>`'s `engine::reflect::writeJson` leaf overloads (`bool`/`float`/`double`/`Vec3`/`Quat`, plus a constrained template covering every other integral type, widened by signedness so `to_chars` never sees a character type and `bool` never binds it). `cmake/reflect.cmake` gained `aero_reflect_generate_json()`, a structural clone of `aero_reflect_generate()` (task 1.1.4) with four deltas: distinct name, a distinct `AERO_REFLECT_JSON_WIRED` target property (so one target may call **both** generators), `<stem>.json.gen.cpp` output running `--emit-json`, and **no aggregator TU**. A new standalone doctest target, `aero_reflect_json_test` (sibling of `aero_reflect_meta_test`, but linking `aero::reflect` instead of `EnTT::EnTT` — serialization is `entt::meta`-independent), reflects three existing fixtures (`component_codegen.hpp`, `component_wiring.hpp`, `component_multi.hpp`) via one `aero_reflect_generate_json()` call and proves, at runtime: an exact, ordered, byte-for-byte pretty-JSON match against a hand-built literal (unsupported/static fields absent); a namespaced (`engine::demo::Light`) + unsigned-integral (`std::uint32_t`) case; every emitted float lexeme round-tripping bit-exactly (`std::strtof`, not `std::from_chars<float>` — the pinned macosx15.4 SDK's libc++ ships no floating-point `from_chars` overload, a real local-verification gap the plan's own from_chars-based sketch did not anticipate, worked around entirely inside the test, with zero effect on the writer itself, which uses only `to_chars`); and `JsonWriter` unit coverage (escaping, nesting, arrays, non-finite→`null`, compact vs. pretty, empty object). Eleven new `json_*` process-boundary cases joined `run_case.cmake`/`_aero_reflect_cases` (39 → **50** `reflect-gen.*` ctest cases), covering the emitted shape, `-o` byte-identity, skip+warning, multi/namespaced, tag, zero-component, determinism, depfile, the exit-2/exit-3 parse-contract paths, and the mutual-exclusion usage error — all under `--emit-json`. Local verification (both macOS Debug/Release configs): full ctest green (66/66, up from the pre-1.2.1 baseline of 54 — +11 new `json_*` process-boundary cases and +1 new `aero_reflect_json_test` target); `reflect-gen.*` alone green at 50/50 (39 existing intact + 11 new); the determinism spot-check (`--emit-json` run twice → byte-identical) and the real-tree `touch engine/core/include/aero/core/math/constants.hpp` spot-check (regenerates all three `.json.gen.cpp`, all transitively include `math.hpp`, and relinks `aero_reflect_json_test`; a second rebuild is `ninja: no work to do`) both confirmed manually; `-DAERO_REFLECT_TOOLS=OFF` builds green with `aero_reflect` still building standalone and `ctest -N` showing no `reflect-gen.*`/`aero_reflect_json_test`/`aero_reflect_meta_test` (back to the 14 shaderc-only baseline), ON restored all 66. Two clang-tidy fixes the Linux lint lane would need but a format-only local check misses (both caught locally via the `SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path) clang-tidy-18` workaround): `json_writer.cpp`'s `char buf[32]` scratch buffers and its hex-digit lookup table became `std::array<char, N>` (`modernize-avoid-c-arrays`); `json_test.cpp`'s `const float x = fn<float>(...)` locals became `const auto x = ...` (`modernize-use-auto`) and two single-character `.find("1")`/`.find("2")` calls became `.find('1')`/`.find('2')` (`performance-faster-string-find`).

#### Task 1.2.2 — JSON reader, generated — CLOSES Epic 1.2 DoD
**1.2.2 ("JSON reader, generated") landed, CLOSING Epic 1.2's Definition of Done** ("any reflected component round-trips component → JSON → component byte-equal, proven by tests"): `aero_reflect_gen`'s `emitJson` now emits, per detected component, BOTH the unchanged `aeroWriteJson` (1.2.1) AND a new `bool aeroReadJson(const engine::JsonValue&, T&)` into the SAME `<stem>.json.gen.cpp` — no new CLI flag, no new CMake generate function, no new target (D2): serialization is one ADR-004 consumer whose two halves must never version-skew. **`engine/reflect` gained the read runtime**: `<aero/reflect/json_value.hpp>`/`src/json_value.cpp` (`engine::JsonValue` — an immutable variant DOM, `JsonKind`/`JsonNumber`/`JsonMember`, checked-never-asserting accessors; numbers stored as the validated LEXEME and converted at ACCESS time at the target's precision — `asF32`/`asF64` route `#if defined(__cpp_lib_to_chars)` `std::from_chars` with `result_out_of_range` falling through to a locale-pinned C fallback (`strtof_l`/`strtod_l`, process-lifetime `"C"` locale, never freed by design), always the C fallback on Apple; `asI64`/`asU64` via integral `std::from_chars`, exact across the full 64-bit range) and `<aero/reflect/json_reader.hpp>`/`src/json_reader.cpp` (`engine::parseJson` — a strict, ITERATIVE (never recursive — `misc-no-recursion` is live and CI runs `--warnings-as-errors='*'`), depth-capped (`JsonParseConfig::maxDepth`, default 256) RFC 8259 parser with three documented tolerances: a single leading UTF-8 BOM skipped, duplicate object keys last-wins, non-key string content not UTF-8-validated; `JsonParseResult` carries either the parsed `JsonValue` or a `JsonParseError` with 1-based line/column + 0-based byte offset, no exceptions, no logging). `serialize.hpp`/`.cpp` gained the read-side mirror of the write half: `readJson` leaves for `bool`/`float`/`double`/`Vec3`/`Quat` (`null` → quiet NaN for float/double, closing 1.2.1's E-nonfinite loop; `Vec3`/`Quat` require all keys, read atomically into locals so a half-good sub-object never partially commits) + the integral template (mirrors the write-side widening exactly), plus `readField`/`expectObject`/`warnUnknownKeys` (the tolerance policy: missing key → silently untouched + `true`, unknown key → WARN + `true`, present-but-bad → WARN + `false` with best-effort continuation — `ok = readField(...) && ok;`, `&& ok` on the right, never short-circuiting). **A latent 1.2.1 defect was fixed alongside** (its own commit): `classifyField`'s old whole-builtin-range test (21 `CXTypeKind`s) is now an explicit 18-kind whitelist, excluding `long double`/`__int128`/`unsigned __int128` — the three kinds `serialize.hpp` has no viable overload for (ambiguous float/double resolution; `__int128` fails `std::is_integral_v` under strict `-std=c++20`), which used to generate **non-compiling** `.json.gen.cpp` — now they tag `[unsupported]`/skip-and-warn uniformly across all three modes, pinned by the new `tests/reflect-gen/fixtures/component_limits.hpp` fixture (`ReflectLimits`: `int64_t`/`uint64_t`/`int16_t`/`uint8_t`/`double`/`long double`). Test wiring: `tests/reflect-gen/run_case.cmake` gained 7 new process-boundary cases (`json_reader_basic/_unsupported/_multi/_tag/_none/_limits`, `components_longdouble` — 50 → **57** `reflect-gen.*` cases) and `tests/reflect-gen/json_test.cpp` gained the parser accept/reject/truncation-sweep batteries, `JsonValue` accessor coverage (incl. the F4 regression pin — `7.038531e-26` bit-equal to `strtof`'s own value — and 64-bit exactness), the leaf/`readField` policy matrix, and the D13 round-trip proof (bit-equal fields AND byte-equal re-serialization) over `ReflectSample`, a gnarly float/double table, `ReflectLimits` extremes, the namespaced `engine::demo::Light` + `ReflectWiring`, and the NaN/null corner — full local ctest **66 → 73** (both Debug/Release configs green; `reflect-gen.*` alone 57/57). Two reusable findings this task surfaced beyond the plan's own grounding note: (1) a genuine parser bug caught by a pre-test smoke probe, not by the eventual doctest suite — the FIRST state-machine draft accepted trailing commas (`[1,]`, `{"a":1,}`) because its "want value or close" state was reachable both at a container's start AND right after a comma; the fix splits each container's states into a start-only "OrClose" variant and a post-comma variant with no close option, and this exact trap is worth remembering for any future hand-rolled state-machine parser; (2) `bugprone-use-after-move` false-positived on `attach()`'s replaced-flag-then-conditional-push pattern (two mutually-exclusive `std::move(value)` calls the checker couldn't prove disjoint from a `bool` flag) — fixed by restructuring to an early `return` inside the loop instead of a flag, which is provably disjoint to clang-tidy's flow analysis and not just to a human reader; zero `NOLINT`s were added. One judgment call recorded at spec time was superseded in-review and this note was stale on it until the 1.2 audit: the DoD's "and `Tag{}`" round-trip clause IS proven via a real generated pair — `component_tag.hpp` rode in as the FIFTH `aero_reflect_generate_json` HEADERS entry, so a genuine zero-field `aeroWriteJson`/`aeroReadJson` pair compiles, links, and round-trips at runtime (`str() == "{}"`), with the text shape still pinned process-boundary-side by the `json_tag`/`json_reader_tag` cases. No new dependency, CI step, boundary guard, or CMake-function change (D15) — `cmake/reflect.cmake`'s `aero_reflect_generate_json()` is content-agnostic and needed zero edit; `vcpkg.json`, `ci.yml`, all four boundary-probe link lines, `aero_tests`' link line, `aero_reflect_meta_test`, `reflect-gen.incremental_e2e`, `tests/lsan.supp`, and `json_writer.{hpp,cpp}` (frozen — the reader adapts to the writer, never the reverse) are all byte-identical.

#### Task 1.2.3 — Scene serialization format v1 (+ the Epic 1.2 close-out audit)
**1.2.3 ("Scene serialization format") landed, composing on top of Epic 1.2's already-CLOSED DoD**: `engine::reflect::writeJson(JsonWriter&, const JsonValue&)` joined the existing overload set in `serialize.hpp`/`serialize.cpp` — an iterative, generic DOM re-emitter (numbers canonicalize through the same typed `value()` overloads the leaf writers use; a canonicalization fixpoint) that `writeScene` uses to re-emit component payloads it does not interpret. `engine/reflect` gained a new header/source pair, `<aero/reflect/scene_format.hpp>` + `src/scene_format.cpp`: `engine::SceneDocument`/`SceneEntityRecord`/`SceneComponentRecord`/`SceneError`/`SceneParseResult`, `SCENE_FORMAT_VERSION == 1`, and `parseScene` (text + DOM overloads)/`validateScene`/`writeScene`/`writeSceneText` — a three-pass (structure+version-gate, parent resolution, forest/cycle check) strict envelope validator over a JSON object `{"version": 1, "entities": [...]}`, with per-entity `id`/`name`/`parent`/`components`, unknown-key WARN+strip tolerance, and component payloads left as opaque `JsonValue` objects (resolving them into live components needs a world + name→type dispatch, out of scope here and deferred to Epics 1.3/1.4, task 1.4.2 specifically). `docs/09-file-formats.md` is the new normative schema document (scene v1: envelope table, identity/hierarchy rules, component-key contract, canonicalization notes, worked examples, the full error/warn catalog, and the versioning/evolution policy), indexed from `docs/00-overview.md`'s documentation table; `docs/01-tech-stack.md`'s Serialization row and `docs/02-adrs.md`'s ADR-004 implementation note and `docs/08-risks.md`'s R2 entry each gained a one-sentence pointer to it. `aero_tests` gained one new TU, `tests/scene_format_test.cpp` (21 `TEST_CASE`s at landing — this note previously said 19, a count error; 22 after the audit's duplicate-key case — tier-0 throughout — no GPU, no reflect-gen, no files, no randomness — covering the D8 surface, both canonical fixtures byte-pinned including a hand-built-document proof, the two round-trip guarantees over five kinds of non-canonical variant, the full version/id/parent/component error batteries, a 1000-entity chain under sanitizers, unknown-key tolerance+stripping, the JSON-stage-vs-scene-stage error-position contract, the DOM re-emitter's kind/nesting/number-canonicalization coverage, `validateScene` as the pre-save hook, and determinism), and **`aero::reflect` joined `aero_tests`' link line** — the only link-line change in the task, and a deliberate one (F8/D10): the scene-format layer consumes zero generated code, so its tests belong in the unconditional suite, not the two `AERO_REFLECT_TOOLS`-gated standalone reflect targets. **The Epic 1.2 close-out audit (2026-07-23) then re-verified and HARDENED the epic, closing it AUDITED**: the DoD was independently confirmed (73/73 both macOS presets at `8dfd013`, tools-OFF parity 14/14, CI green all 3 OSes) and four hardening changes landed in one follow-up PR — (1) a negative `JsonWriterConfig::indentWidth` now clamps to 0 in the ctor (previously it wrapped `writeIndent()`'s size_t multiply and THREW `std::length_error` through a no-exceptions API, probe-proven); (2) `--emit-json` now SKIPS any component not at namespace scope (nested in a struct/class/function) with a `// skipped component:` comment + one stderr warning, exit 0 — previously it emitted `namespace engine::Outer {` where `Outer` is a CLASS, an uncompilable TU (probe-proven "redefinition of 'Outer' as different kind of symbol"; detection stays 1.1.2-E4-conformant and `--emit-meta` is nest-safe by construction, its fully-qualified template argument needs no namespace wrapper — new fixture `component_nested.hpp`, new process-boundary case `json_nested_skip`, 57 → 58 `reflect-gen.*` cases); (3) `parseScene(const JsonValue&)` now REJECTS duplicate component keys on a hand-built DOM root via the shared catalog line (text input was never affected — the JSON layer collapses duplicates last-wins; docs/09 §2.3/§2.6 updated, the dup-type line is no longer "validateScene-only"); (4) `json_test.cpp` gained the REQUIRE-then-deref guard pass over every optional/pointer chain (regressions now fail cleanly instead of UB — the tests-local `.clang-tidy` disables `bugprone-unchecked-optional-access`, so the guard must be runtime), a `Player` runtime round-trip (the global-namespace half of `component_multi.hpp` was compile-proven only), and three exact line/column/offset error-position pins (previously only one `line` value was ever asserted). Post-audit truth: ctest totals **74** tools-ON / **14** OFF; `scene_format_test.cpp` = **22** `TEST_CASE`s; `json_test.cpp` = **18**. Accepted residual debt, all deliberate and documented: recursive `JsonValue`/`JsonMember` implicit special members on uncapped HAND-BUILT trees (new risk **R19** — parser-produced DOMs are capped at 256 and safe; reopen only if code ever hand-builds unbounded-depth trees); no runtime WARN-text assertions until the Phase-2 log sink exists (the 0.2.4 deferral — the two docs/09 warning-catalog lines and the three reflect warn sites are side-effect-tested only); `char8_t` fields classify Unsupported (libclang 18 has no `CXType_Char8` — the safe direction, `serialize.hpp`'s template would accept it; char-family fields serialize as JSON numbers by the widening design); and hand-built-DOM envelope duplicate keys resolve first-wins via `find()` vs the text parser's last-wins (documented contract, no consumer at risk). ADR-004's artifact table was corrected per the audit: binary serialization belongs to the Phase 3+ cooker pipeline, NOT reflect-gen (a placement note now sits under Epic 3.3 in docs/tasks/phase-3.md — no phase 2–8 task carried it, the largest requirement-surface gap the audit found).

### Epic 1.3 — ECS & scene

#### Task 1.3.1 — EnTT world integration — OPENS Epic 1.3
**1.3.1 ("EnTT world integration") landed, OPENING Epic 1.3**: `engine/scene` is the **fifth engine layer** (`core → platform → rhi → render → reflect → scene`) — `aero_scene` STATIC (`engine/scene/CMakeLists.txt`, `PUBLIC aero::core` / `PRIVATE EnTT::EnTT aero::profiling`) plus a sibling `aero_scene_internal` INTERFACE target (alias `aero::scene_internal`, the 0.4.2 `aero::platform_internal` pattern) exposing the ONE registration seam. `engine::Entity` (`Handle<EntityTag>`), `engine::World`, and `engine::ComponentTypeId` live in plain `engine::`; the machinery sits in `engine::scene::{detail,internal}`. The backing ECS identifier is a 64-bit `SceneEntity` (32-bit index + 32-bit generation, D2) — a lossless bijection with `engine::Handle`, chosen specifically because entt's default 32-bit identifier gives only 12-bit versions (stale-handle aliasing after 4096 recycles of one slot). `World`'s public component API is templates that `static_cast` over six type-erased primitives (`addRaw`/`getRaw`×2/`hasRaw`/`removeRaw`/`countRaw`) — the ONLY way to keep `world.hpp` entt-free, since creating a typed component storage is a template in entt with no runtime-typed factory; the one call that genuinely needs entt (`engine::scene::internal::registerComponent<T>(world, name)`) lives behind `engine/scene/internal/.../world_access.hpp`, shipped through `aero::scene_internal` and linked PRIVATE only by `aero_scene` itself and by test/authoring TUs. Registration is per-World (no process-global type table, matching JobSystem's own refusal of one). EnTT is the **fifth** PRIVATE flat-root backend the R12 probe mechanism bites for (after glm, SDL3, miniaudio, SDL_GPU): a new `lint`-job script, `.github/scripts/check-scene-boundary.sh` (public-header scope, like the platform guard, not the rhi guard's source-wide scope — docs/03 reserves `engine/reflect` as a future `entt::meta` runtime), plus the fifth compile-time probe, `tests/scene_boundary_probe.cpp`, linking `aero::scene` alone. `tests/CMakeLists.txt` gained one new TU (`scene_test.cpp`, 14 `TEST_CASE`s, tier-0) and the probe block; `aero_tests`' link line gained exactly two tokens, `aero::scene aero::scene_internal` — the only link-line change. `ctest -N` is unchanged at **74** ON / **14** OFF (the scene tests ride the existing `aero_tests` entry) — the structural proof the scene layer has zero reflect-gen dependency. Three EnTT facts worth never re-deriving, all probe-verified against the pinned 3.16.0 before a line of `world.cpp` was written: the ENTITY storage's `free_list()` is the live count (its `size()` also counts recycle-pending slots, and iterating it yields destroyed identifiers — "the trap"), while a COMPONENT storage is the reverse (`size()` is live, `free_list()` is an unreadable sentinel); `value(e)` is only defined when `contains(e)` — in a Debug build that is an `assert`-abort, not silent UB, and the same guard applies to a second `push()` on an already-contained entity, which is why D11's add-replace is erase-then-insert, not a plain re-push; and the type-erased insert **silently drops** a non-copy-constructible component with no diagnostic and no crash, which is why `add<T>()` carries a mandatory `static_assert(std::is_copy_constructible_v<T>)`.

#### Task 1.3.2 — Transform hierarchy
**1.3.2 ("Transform hierarchy") landed, CLOSING Epic 1.3's Definition of Done clause 2** ("transforms compose hierarchically"): `AERO_COMPONENT` was promoted from `tests/reflect-gen/fixtures/aero_reflect.hpp` to a new public header, `<aero/reflect/annotations.hpp>` (byte-identical macro/string contract with the detector — only its home moved), and `engine::Transform` (`<aero/scene/transform.hpp>`, 40 bytes — `position`/`rotation`/`scale = Vec3::one()`, the reflection pipeline's first NSDMI) is the engine's **first reflected component in shipped engine code**, registered as `"engine::Transform"` in **every World by construction** through a new `engine::scene::detail::registerBuiltinComponents(world)` called from `World`'s constructor — `aero_scene` gained a second TU (`src/transform.cpp`) and `PUBLIC aero::reflect` (a downward-legal edge that propagates zero vcpkg headers, so the scene boundary probe's guarantee is unchanged). `World` gained a **World-owned entity-level parent/child forest** — `setParent`/`parent`/`childCount`/`eachChild` (+ the private `nextChild` cursor primitive) — a TU-local, registry-owned `HierarchyNode` (never in the public registration table), cycles rejected at `setParent` (self-parenting gets its own distinct ERROR line), sibling order preserved by an ORDERED erase (never swap-remove). **`destroy()` now destroys the whole subtree**, post-order (children before parents, order otherwise unspecified — the spec explicitly licenses this), via an O(1)-scratch, ZERO-allocation descend-to-deepest-last-child walk, so its shipped `noexcept` stays honest rather than becoming a documented terminate bargain — this is what establishes the no-stale-parent invariant. `localMatrix(Transform)`/`worldMatrix(World, Entity)` (free functions, not World members — World stays component-agnostic) compose on demand, iteratively, up the parent chain, with an entity or ancestor lacking a `Transform` contributing identity silently; caching is a recorded docs/08 deferral. The reflection claim is proven mechanically over the REAL shipped header (not a fixture): a new process-boundary case, `reflect-gen.components_engine_transform`, runs `--components` over `engine/scene/include/aero/scene/transform.hpp` and asserts zero unsupported fields and zero tool warnings; `transform.hpp` also joined both gated reflect targets' `HEADERS` lists (`aero_reflect_meta_test`/`aero_reflect_json_test`, each gaining `aero::scene` on their link line — the honest cost of parsing/compiling against the real header) proving the generated `entt::meta` registration and JSON writer/reader compile and round-trip against it. **Step 0's pre-flight settled risk R-a**: the tool's `--components` output prints the AS-WRITTEN type spelling, which for `transform.hpp` (bare `Vec3`/`Quat` inside `namespace engine`) is `field position : Vec3 [vec3]`, **not** `engine::Vec3` — pinned as such in the new ctest case; no reflect-gen code changed (this was Step 0 settling an documented uncertainty, not a tool gap — `tools/reflect-gen` is byte-identical). `World`'s constructor now seeding `registerBuiltinComponents` means every fresh World's `componentTypeCount()` starts at 1, not 0 — `scene_test.cpp`'s `"scene: registration table"` case needed **five** absolute-count edits (619/623/631/663/676), not the three a naive read of the task suggests. `tests/transform_test.cpp` (20 `TEST_CASE`s, tier-0, rides `aero_tests` unconditionally — green with `-DAERO_REFLECT_TOOLS=OFF` too, the zero-codegen-dependency structural proof) covers the component defaults, the whole hierarchy API (link/detach/reparent/reject/cycle-at-depth/`eachChild` order), subtree `destroy()` (including a 13-entity wide-and-deep case built specifically to force the node storage's swap-and-pop to relocate a sibling's node mid-destroy — the reusable "never cache a `HierarchyNode*` across a `registry.destroy()`" rule), slot-recycling with no stale parent observable, and `worldMatrix` (translation/rotation/scale/full-TRS-chain/gap-entity/1000-deep-chain/non-uniform-scale-shear). `tests/scene_boundary_probe.cpp` gained Transform's layout re-assertions plus the hierarchy API's shape/`noexcept` contract (link line unchanged, `aero::scene` alone); the sabotage proof (seed `#include <entt/entt.hpp>` into `transform.hpp`, confirm both the textual guard and the compile-time probe fail, confirm the identical leak compiles clean inside `aero_tests`, then revert) was performed and re-verified green. One reusable finding beyond the plan's own text: the probe's hand-rolled `std::declval`-equivalent helper functions (`constWorld()`/`mutableWorld()`/`someTransform()`, declared-never-defined, named only in `decltype`/`noexcept` operands) needed an explicit `noexcept` specifier apiece — omitting it (as the plan's own sketch did) makes `noexcept(constWorld().parent(...))` evaluate **false** regardless of `parent()`'s own `noexcept`, because the `noexcept` operator considers the WHOLE expression, including the helper call itself; real `std::declval` is itself declared `noexcept` for exactly this reason.

#### Task 1.3.3 — Camera & Light — CLOSES Epic 1.3
**1.3.3 ("Camera & Light" reflected components) landed, CLOSING Epic 1.3** (PR #34, squash `62053f3`): `engine::Camera` (perspective-only — `fovYRadians`/`nearPlane`/`farPlane`, the `near`/`far`-avoiding names dodging `<windows.h>`'s empty macros) with free `projectionMatrix(const Camera&, float aspect)`/`viewMatrix(const World&, Entity)`; `engine::DirectionalLight` (`color`/`intensity`) and `engine::PointLight` (`color`/`intensity`/`range`) — **two light components, not one `Light` + enum discriminator** (reflect-gen still cannot reflect an enum; `CXType_Enum` is out of subset), storing NO direction/position (those derive from the entity's `Transform` in 1.4.1). All three are `AERO_COMPONENT`-reflected and registered as built-ins in `registerBuiltinComponents` (order Transform, Camera, DirectionalLight, PointLight), so a fresh World's `componentTypeCount()` became **4**. `camera.cpp` deliberately does NOT include `world.hpp` (`viewMatrix` only forwards its `const World&` into `worldMatrix`, never dereferences it — `World` stays incomplete). Scope confined to `engine/scene/` + `tests/`, zero infra change.

### Epic 1.4 — Scene → render

#### Task 1.4.1 — Scene → render — OPENS Epic 1.4
**1.4.1 ("Scene → render") landed, OPENING Epic 1.4** (`render`, PR #35, squash `2dc0fc4`) — a three-part split preserving the layer law (scene is ABOVE render, so render must never depend on scene): (1) `engine::MeshRenderer {uint32 primitive; Vec3 color}`, the reflected "this entity draws a primitive mesh" component and the **5th built-in** (fresh-World `componentTypeCount()` now **5**); (2) `render` gained the scene-free `engine::render::ForwardRenderer` + the flat `RenderView`/`MeshInstance`/`CameraView`/resolved-light-data vocabulary + procedural cube/sphere/plane geometry + `shaders/scene.{vert,frag}.hlsl` (the FIRST in-tree fragment uniform buffer, `register(b0, space3)`); (3) a new module `engine/scene_render` (`aero::scene_render` STATIC, `PUBLIC aero::scene aero::render`, NEVER `aero::scene_internal`) — the only code that sees both layers — hosting the pure GPU-free `buildRenderView(World&, RenderViewScratch&, rhi::Extent2D)` (walks `each<Transform, MeshRenderer>`, resolves the lowest-index `Camera` + lights into a flat `RenderView`) and the `SceneRenderer` facade driving `render(World&, Frame&)` once per frame — where the Phase-2 editor viewport and Phase-5 runtime will drive rendering. Reflect-gen's `--components` prints the AS-WRITTEN type spelling (`std::uint32_t`, not the canonical `unsigned int` — the `[primitive]`/`[vec3]` classification tag stays canonical).

#### Task 1.4.2 — Load & draw a JSON scene — CLOSES Phase 1
**1.4.2 ("Load & draw a JSON scene") landed, CLOSING Epic 1.4's Definition of Done and validating the Phase 1 gate** (PR #36, squash `baff2db`, all 5 CI checks green on all 3 OSes, 2026-07-24) — the runtime **name→type dispatch** that turns a component-name string + an opaque `JsonValue` payload into a live typed component on an entity, and its inverse, joining the three finished halves (`parseScene` from 1.2.3, `World` + the 5 reflected built-ins from 1.3.x, `SceneRenderer` from 1.4.1). **`engine/scene_serialize` stood up** as a new bridge module (`aero_scene_serialize` STATIC, alias `aero::scene_serialize`, `PUBLIC aero::scene aero::reflect PRIVATE aero::profiling`, NEVER `aero::scene_internal`) sitting above both `scene` and `reflect` — it owns the `World ⇄ SceneDocument` mapping in BOTH directions (load + save round-trip) driven by one internal `constexpr std::array<BuiltinComponent, 5>` table over the 5 built-ins (declaration order == registration order == save emission order, pinned against `registerBuiltinComponents` by a test). `<aero/scene_serialize/scene_serialize.hpp>` exposes `loadScene`/`loadSceneText`/`saveWorld`/`saveWorldText`/`builtinComponentNames` — engine types only (`World`, `SceneDocument`, `JsonValue`, `SceneError`), so the boundary rule holds by construction (no new guard, the 1.2.1 precedent). It is the **first PRODUCTION consumer of `aero_reflect_generate_json`** (until now only tests generated component serializers): the 5 generated `aeroReadJson`/`aeroWriteJson` compile INTO the shipping library (the standalone gated test target only LINKS it), so the module is gated on `AERO_REFLECT_TOOLS` (serializers are generated, never committed — the `AERO_SHADER_TOOLS`/phase-0-cube precedent; OFF ⇒ the module, its test, and the sample's real path all self-skip, `aero_tests` unchanged). `loadScene` instantiates entities in file order, deserializes each recognised component via `aeroReadJson`, links parents in a second pass (forward references legal), and returns a `SceneLoadReport` (unknown component name → WARN + skip); `saveWorld` emits ids 1..N in `eachEntity` order (entity→id resolved by a linear `std::find` over the iteration vector — there is NO `std::hash<Entity>`/`operator<`, so no `std::unordered_map<Entity,…>`), each present built-in via `aeroWriteJson`, entity names NOT persisted (D7 — `std::string` is outside reflect-gen's subset). Round-trip idempotence (`saveWorldText(load(save(W))) == saveWorldText(W)`) is proven bit-exact by a tier-0 test. **`samples/phase-1-scene`** is the committed Phase-1 gate artifact — a flat lit tableau `scene.json` (ground + cube + sphere + one Camera + one DirectionalLight + one PointLight) loaded from disk through the VFS → `World` → `SceneRenderer`, in a resizable vsync loop (macOS-validated PASS in `VALIDATION.md`; Windows/Linux render rows pending a code-free on-hardware follow-up, the 0.5.3 precedent). Root `CMakeLists.txt` reordered so `add_subdirectory(tools)` precedes `engine/` — `engine/scene_serialize` is the first engine-tree caller of `aero_reflect_generate_json`, which `FATAL_ERROR`s at configure if the `aero_reflect_gen` target does not yet exist; `tools/` links no engine target, so the golden rule is untouched. **With 1.4.2, Phase 1 (Reflection, ECS & Serialization) is COMPLETE — all four epics (1.1–1.4) CLOSED and the Phase 1 gate reached in code + macOS-validated (Windows/Linux render sign-off pending, exactly as the Phase 0 gate). Phase 2 (Editor) is next.**

## Phase 2 — Editor

### Epic 2.1 — Editor shell

#### Task 2.1.1 — ImGui + docking — OPENS Phase 2
**2.1.1 ("ImGui + docking into the editor target") landed, OPENING Phase 2 and the `/editor` layer**: `editor/` is populated for the first time — `aero_editor_core` STATIC (`editor/CMakeLists.txt`) hosts Dear ImGui (docking, v1.92.8) directly on `engine::rhi::Device` and `engine::platform::Window`/`Context`, and `aero_editor` is the thin executable that opens one resizable "Aero Editor" window, drives one ImGui frame per loop iteration over the RHI (not `render::Renderer` — ImGui's `PrepareDrawData` must run before the render pass opens, which `Renderer::beginFrame` does not allow for), and shows a full-window dockspace with 4 placeholder panels (Hierarchy/Inspector/Viewport/Console), a first-run `DockBuilder` default split, `imgui.ini` layout persistence at an exe-relative path, and HiDPI style/font scaling (`io.ConfigDpiScaleFonts` + `ScaleAllSizes`). `imgui` (`docking-experimental`+`sdl3-binding`+`sdlgpu3-binding`) joined the vcpkg manifest, editor-only by linkage (only `editor/` links `imgui::imgui`, PRIVATE to `aero_editor_core`). **Exactly two shipped-engine edits, both non-installed internal seams cloned from the `aero::platform_internal` precedent** — the only code this task added to `/engine`: `aero::rhi_internal` (`engine/rhi/internal/aero/rhi/internal/native_device.hpp`'s `NativeDeviceAccessor`, exposing the SDL_GPU device/command-buffer/render-pass as `void*` only — never a typed SDL_GPU spelling, which `check-rhi-boundary.sh` would reject — defined in the one allowlisted `sdl_gpu_backend.cpp`); and a generic, void-based raw-event observer on `engine::platform::Context` (`RawEventAccessor`/`native_event.hpp`), fired for every backend event inside `pollEvent` before translate/discard, null by default (zero behavior change), which ImGui's SDL3 backend installs to receive events the engine's own pump would otherwise translate-and-discard. Both guards (`check-rhi-boundary.sh`, `check-platform-boundary.sh`) stay green by construction — proven by a sabotage proof (a real `SDL_GPUDevice* leak;` code line seeded into `native_device.hpp` makes the rhi guard fail naming the file; reverting restores green). A new standalone GPU-gated doctest target, `aero_editor_imgui_test` (the `aero_reflect_meta_test` precedent — own `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`, gated at runtime by the existing `AERO_REQUIRE_GPU` ratchet, living OUTSIDE every `AERO_*_TOOLS` gate since the editor depends on neither codegen nor shaders), proves `ImGuiLayer::create`/`beginFrame`/`endFrame`/teardown init→3 frames→shutdown leak-free on a real GPU (hidden 320×180 window — tolerated fine, no visible-window fallback needed); `aero_tests`' link line and all five boundary-probe link lines are byte-identical, `ci.yml`/`cmake/**`/`tests/lsan.supp` untouched. Docking-only (`ImGuiConfigFlags_DockingEnable`); multi-viewport is deferred (docs/08 R20) — SDL_GPU's viewport support is experimental and unneeded for the "dockable panels" deliverable. **macOS mechanically verified** (build green both presets, GPU smoke test green, a non-interactive launch confirmed the window opens, the DockBuilder default layout builds and persists byte-identically across a relaunch, and quit is clean/leak-free) — the mouse-driven visual checks (drag/dock by hand, HiDPI crispness, an actual human restart) and the Windows/Linux rows are recorded pending in `editor/VALIDATION.md`, the Phase 0/1 gate ledger precedent. Does **not** land the golden-rule CI guard (task 2.1.2) or the app shell/menu bar/panel registry (task 2.1.3) — both are next.

#### Task 2.1.2 — Golden-rule CI guard
**2.1.2 ("Golden-rule CI guard") landed, making project rule #1 a build failure**: `/editor` became breakable exactly one commit earlier (2.1.1), and only one of the four ways to break it was covered by the documented `#include` scan — the relative escape `#include "../../../editor/include/…"`, a `namespace engine::editor` forward declaration, and `target_link_libraries(aero_core PRIVATE aero::editor_core)` **all compile today**, and the last two are invisible to any include scan. So the guard ships in **two halves** (the task's one deliberate, user-approved scope expansion): `.github/scripts/check-golden-rule.sh` — the fifth boundary script, `bash 3.2`, `engine/`+`runtime/`, tracked-only, `.c` deliberately **IN** the extension set (unlike `check-rhi-boundary.sh`: this is a dependency guard, not a lint, and Phase 5 ships `.c`/`.m` runtime entry points) and matching **`#include` *and* `#import`** (the Objective-C directive — matching only `#include` would have made the `.m`/`.mm` extensions a blind spot, defeating the very reason they are in the set), with an **inverted canary** (`editor/include/aero/editor/` must exist and still match `INCLUDE_RE` — there is no allowlisted file to anchor on) and the `0`/`1`/`2` exit contract; and `cmake/golden_rule.cmake`'s `aero_assert_golden_rule()`, called at the very end of the root `CMakeLists.txt` (after `add_subdirectory(samples)` — it enumerates targets), which BFS-walks `LINK_LIBRARIES` + `INTERFACE_LINK_LIBRARIES` (iteratively **unwrapping `$<IDENT:…>` generator-expression shells** before tokenising — the naive token class swallowed the genex head and silently dropped the target, so `$<$<CONFIG:Debug>:aero::editor_core>` would have evaded the guard entirely) from all 11 `engine/`+`runtime/` targets (10 under `AERO_REFLECT_TOOLS=OFF` — `aero_scene_serialize` early-`return()`s, which is why anti-vacuity checks only that the **combined** set is non-empty, never a count or a roster: `runtime/` has zero targets) plus their include directories, with a **reverse canary** (`aero_editor_core` must reach `aero_core`) proving the walk traverses rather than passing because it found nothing. Both halves are proven red-on-violation by two ungated hermetic `cmake -P` ctest cases: `golden-rule.include_scan_e2e` — a scratch `git init`+`git add` tree, no commit so no `user.email` needed, **seven** stages (one of them seeding the violation under `runtime/`, not `engine/` — see below) ending in the exit-2 vacuity refusal, registered on non-Windows only (D16, since the script never runs on Windows in CI at all); and `golden-rule.link_graph_e2e` — a `LANGUAGES NONE` probe project driven through **six nested configures** (clean + 5 `PROBE_SEED_*` seams) that prove **both** violation arms *and* **all three** self-verify arms (broken reverse canary, empty consumer set, empty forbidden set), with the link violation seeded at the **deepest** target (spec correction C2) so the reported `probe_rhi` finding is only reachable transitively, and run from a scratch path deliberately containing an `editor` segment so the resolved-path comparison is proven not to be a substring match — **which only works because the clean case gives one consumer a real include directory**: with no include dirs anywhere, step 5's loop never iterates, and installing the forbidden naive `if(_gr_entry_abs MATCHES "/editor/")` left all six cases passing (see the review findings below). The clean case and the empty-consumer case together pin the combined-set anti-vacuity semantics from both sides (a hardcoded-`TRUE` check passes the first; a per-root check passes the second). `ci.yml` gained **exactly one** `lint` step; ctest **80 → 82** on both macOS presets (measured; a freshly reconfigured tools-OFF tree measured **2 → 4**, +2 as predicted, consumer set legitimately 10, anti-vacuity did not trip); **zero** change under `engine/`/`runtime/`/`editor/`/`samples/`, `aero_tests`' link line and all five boundary-probe link lines byte-identical, no new dependency, no `vcpkg.json` entry, **no compile-time probe target** (deliberate — spec §4: the golden rule has the inverse geometry of R12, since `editor/include` is on no engine compile line, so a probe would be vacuous by construction). `docs/03:14` and `docs/04:38` + a new CI-checklist item 10 were widened to describe both halves (D14 — docs are the source of truth on conflict). **One plan-text defect found and fixed during implementation**: the probe fixture's `engine/CMakeLists.txt` sketch used `;` to separate two `add_library()` invocations on one physical line (`add_library(probe_core INTERFACE);  add_library(probe::core ALIAS probe_core)`) — CMake script syntax does not support `;` as a command terminator (it is a list separator inside one command's arguments only), so this was a real "Parse error. Expected a command name" at first configure; fixed by one `add_library()` call per line, no semantic change. **The code review then found four real gaps, every one of them hidden behind a fully GREEN test suite — the single most transferable lesson from this task: proving a guard goes red on a seeded violation does NOT prove its test asserts anything; you must break the GUARD and confirm the TEST goes red.** (1) The `link_graph` probe's clean-case targets had **zero include directories**, so the include-directory comparison never executed there at all — the forbidden naive `MATCHES "/editor/"` substring match left **all six cases passing**, making the `editor`-segment `WORK_DIR` decorative; fixed by giving the clean case one real include dir, which also gave step 5 its only negative-direction coverage. (2) Replacing `SCAN_ROOTS=('engine' 'runtime')` with `('engine')` left the include-scan e2e **fully green** while the script still printed `OK -- 80 tracked engine/runtime sources scanned` — a lie; fixed by seeding a `runtime/` source **and** a violation stage against it (never assume a multi-root scan covers every root, especially while `runtime/` is empty). (3) The genex hole above. (4) `#import` unmatched. All four were latent-not-live (zero `.m`/`.mm` files today, no `$<…>`-wrapped engine link entry today, `/runtime` unpopulated until 5.2.1) — i.e. exactly the profile of a guard that would have rotted silently green precisely when it finally mattered. Ten independent sabotage points now pin the pair down. **Does not** ship the runtime-purity guard (5.2.2, which should reuse `_aero_gr_closure` rather than write a second walk — and which is why `aero_assert_golden_rule` already converts a named-but-absent canary target into a `cannot self-verify` failure rather than a raw CMake error) or the editor app shell (2.1.3), and **does not close Epic 2.1** — only its guard clause.

#### Task 2.1.3 — Editor app shell & main loop — CLOSES Epic 2.1 in code
**2.1.3 ("Editor app shell & main loop") landed, CLOSING Epic 2.1 in code**: the 2.1.1 bootstrap became an application. Three new PUBLIC editor headers ship engine + std types only — `<aero/editor/panel.hpp>` (`Panel`, the polymorphic panel interface; `DockSlot : uint8_t {Center,Left,Right,Bottom}`; `PanelOptions`, NAMED BOOLEANS never an `ImGuiWindowFlags`), `<aero/editor/panel_registry.hpp>` (`PanelRegistry` — owns `std::unique_ptr<Panel>`, so `add()`'s returned `Panel*` is **address-stable** across later `add()`s, which is what makes registering a panel from inside another panel's `onDraw()` safe; registration order IS draw order IS View-menu order; a null panel, a null/empty `id()`, or a DUPLICATE `id()` is rejected → `nullptr` + `AERO_LOG_ERROR` + the argument destroyed, because ImGui silently MERGES two windows sharing a name), and `<aero/editor/editor_app.hpp>` (`EditorApp`/`EditorAppConfig`/`framePaceSleepMs`). `EditorApp::create(rhi::Device&, platform::Window&, platform::Context&, const EditorAppConfig& = {})` mirrors `ImGuiLayer::create` exactly — the three engine objects are **INJECTED, not created**, so `main.cpp` stays the honest owner of process-global engine state and the GPU test can drive the shell from a 320×180 window. **`tick()` is the unit of work; `run()` is `while (tick()) {}`** — a quit found in the event drain returns BEFORE `beginFrame()` (no half-frame, no ImGui call at all after quit — `tick()` is idempotent), while a quit raised by the menu DURING the draw finishes that frame and then returns false, so `NewFrame`/`Render` stay balanced on every exit path. **`editor/src/` gained the two src-private ImGui TUs** (the `engine/render/src/primitives.hpp` precedent): `shell_ui.{hpp,cpp}` — the ONLY new ImGui TU, owning the File/Edit/View menu bar, `DockSpaceOverViewport`, the **data-driven** default layout, and the `Begin`/`End` around every `Panel::onDraw()`; and `placeholder_panel.{hpp,cpp}`, one class parameterised by id/slot/note, five instances. **The registry — never the panel — calls `Begin`/`End`**, which makes 2.1.1's code-review Gap 1 (an unbalanced `End()` aborting on `IM_ASSERT`) structurally impossible: a hidden panel `continue`s BEFORE `Begin`, a visible one always calls `End()` regardless of `Begin`'s return, and `BeginMainMenuBar`/`BeginMenu` follow ImGui's OPPOSITE, asymmetric rule (`End*` only when `Begin*` returned true). The default/reset layout is built from each panel's `defaultDockSlot()` and **only splits a slot at least one registered panel asks for** — forced by a verified ImGui fact (`imgui.cpp:19315`: an empty dock node is not pruned, it draws a dead grey rectangle), so registering a panel in 2.2.x needs ZERO layout-code edit; node size comes from `GetMainViewport()->WorkSize`, not `->Size` (2.1.1's spelling), because the main menu bar shrinks the viewport work area, with a documented one-frame settle that must NOT be "fixed" with a manual `GetFrameHeight()` offset. Five placeholders register in a frozen order — Hierarchy(Left), Inspector(Right), Viewport(Center), Console(Bottom), **Assets(Bottom)** — and Console+Assets sharing Bottom is deliberate: it is the runtime proof of the tabbing rule. **`Escape` no longer quits** (a sample-ism, actively wrong in an editor where Esc is the universal *dismiss* key); the four quit paths are the window close box, `EventType::Quit`, `File > Exit`, and `Ctrl/Cmd+Q` read through `ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal)` — **never** `ctx.input()`, which has no notion of UI focus, so a focused `InputText` can swallow the chord (one chord serves all three OSes: `ImGuiMod_Ctrl` is Cmd on macOS automatically). Every unimplemented menu item ships `enabled = false` with an `IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)` tooltip naming its owning task (2.4.1 / 2.5.1), so there is no dead handler behind a stub. Frames are paced by a PURE, tier-0-tested `framePaceSleepMs(presented, focused, capHz, elapsedMs)`: vsync when focused, capped at `unfocusedFrameCapHz` (default 20 Hz) when unfocused, 4 ms when minimized — **minimized beats unfocused**, and `<= 0` *and NaN* disable the throttle (spelled `!(capHz > 0.0F)` so NaN takes the disabled branch), which is what the GPU test uses to keep its frames unpaced. `editor/src/main.cpp` shrank to a **thin bootstrap** (Context → Window → Device → `EditorApp::create` → `run()`, no loop body, no event handling, no ImGui) and `editor/{include/aero/editor/editor_ui.hpp,src/editor_ui.cpp}` were **DELETED** — a cutover that had to be ATOMIC with the GPU test's rewrite, since `tests/editor/imgui_layer_test.cpp` included the deleted header and called `drawEditorUi` (the 1.3.2 "commit #2 doesn't compile" class of problem, caught by inspection before implementation). Tests: a NEW always-on tier-0 target **`aero_editor_shell_test`** (`tests/editor/shell_test.cpp`, **15** `TEST_CASE`s — this note corrects the plan's own "14" figure: the table's row 7/8 split ("visibility defaults... round-trips by index" / "visibility by id agrees with visibility by index", both separately AC-4(d)-mapped) plus the row-14 split into 14a/14b together total 15, not 14 — registry semantics incl. address stability and duplicate rejection, the five-branch pacing matrix, `Panel` defaults, the `DockSlot` layout contract; links `doctest::doctest aero::editor_core aero::core` and deliberately **no** `vulkan_stack_pin.cpp`, no `${CMAKE_DL_LIBS}`, no `rhi_test_support.hpp` — it is ungated and must pass with `AERO_REQUIRE_GPU` unset AND set), and the REWRITTEN `aero_editor_imgui_test`, which now drives `EditorApp::tick()` for real (create → 3 frames each `REQUIRE(tick())` + `CHECK(presentedLastFrame())` → the closed-panel regression restated through the registry, hiding two then all five → `requestLayoutReset()` → `requestQuit()` + two `tick() == false` → teardown), keeping its **VISIBLE** 320×180 window (hidden-window presentation is unproven on lavapipe/WARP, and every assertion depends on the frame actually presenting). `ctest -N` **82 → 83** on both macOS presets; tools-OFF **4 → 5**. **Zero change under `engine/`/`runtime/`/`samples/`/`tools/`/`cmake/`/`shaders/`/`.github/`; `vcpkg.json` and the `/vcpkg` pin byte-identical; `aero_tests`' link line and all five boundary-probe `target_link_libraries` lines byte-identical; `editor/src/imgui_layer.cpp` + `imgui_layer.hpp` byte-identical** (the `io.IniFilename` heap-stable-pointer handling was not disturbed). **No new CI step, no new boundary script, and no compile-time probe for the editor** — the boundary rule governs the ENGINE's public API and the editor is deliberately on its far side; the one editor-side rule 2.1.3 introduces (public editor headers stay ImGui-free) is held by **file placement** (every ImGui entry point lives in `editor/src/`), and **not** by `aero_editor_shell_test`: that target links `doctest::doctest`, which puts vcpkg's shared per-triplet include root on its compile line, and `imgui.h` is installed flat there, so a leaked `#include <imgui.h>` in `panel.hpp` still compiles — **risk R12 again**, proven by seeding exactly that and reverting. D12's *conclusion* (no editor-side probe) was deliberately RE-AFFIRMED against that finding even though D12's *stated rationale* was wrong, and the shipped `tests/CMakeLists.txt` comment now says so instead of claiming enforcement it does not have. Two smaller findings worth keeping: `AERO_PROFILE_FRAME_MARK` in the old `main.cpp` was a **silent no-op in every config** (`aero_editor` links only `aero::editor_core aero::core`; `aero::profiling` is PRIVATE to `aero_editor_core`, so `AERO_PROFILING_ENABLED` never reached that TU) — moving the mark into `editor_app.cpp` activated the editor's Tracy frame marker for the first time; and AC-3's own proof grep is unrunnable as literally specced, because the editor's *own* ImGui-free type is named `ImGuiLayer` and matches `ImGui` (23 hits on `main`, 8 of them code) — the real proof scopes to the three new headers, strips `//` comments with the repo's `nl -ba -w1 -s: | sed -E 's|//.*||'` guard idiom, and excludes the `ImGuiLayer` token. **With 2.1.3, Epic 2.1 is CLOSED in code** — its gate stays formally OPEN only until `editor/VALIDATION.md`'s Windows/Linux rows (and 2.1.1's still-pending mouse-driven macOS row) are filled by a code-free on-hardware follow-up, the 0.5.3/1.4.2 precedent. **Epic 2.2 (Core panels) is next**, and 2.2.1 owns the `onDraw()` context-parameter change (`onDraw(PanelContext&)`), a mechanical one-call-site edit in `drawPanels`.

### Epic 2.2 — Core panels

#### Task 2.2.1 — Hierarchy panel — OPENS Epic 2.2
**2.2.1 ("Hierarchy panel") landed, OPENING Epic 2.2 and putting the first live `engine::World` inside the editor** (branch `feat/2.2.1-hierarchy-panel`, landed as a short commit series, later hardened by a review-round-2 follow-up fixing six review findings on the same branch): three new `World` members close the three documented holes 2.1.3 left for this task — `setName`/`name` (entity-level, purely informational, backed by a TU-local, never-registered `EntityName` storage identical in shape to `HierarchyNode`, so `componentTypeCount()` stays 5 and D1's decision — names are entity data, not a component — holds); `componentTypeAt(index)`, the registration-table walk `findComponentType`'s own comment already promised; and `copyComponent(id, from, to)`, the type-erased duplicate primitive whose whole reason for being a `World` member is D17's ordering law — **remove the destination BEFORE reading the source**, because EnTT's `remove()` swap-and-pops and can relocate the very element about to be read, while `push()` only grows the paged payload and never relocates. The plan's own C1 correction is the load-bearing finding of the task: the spec's single page-boundary test (`>1024` components, copy to a FRESH destination) cannot catch a swapped remove/read order, because a fresh destination never calls `remove` — so AC-5 shipped as **two** cases, AC-5a (push-growth, a fresh destination) and AC-5b (a `std::string`-carrying `Payload` component copied from the last packed element to an ALREADY-OCCUPIED destination, the real D17 proof), and sabotage S1 (hoisting the read above the removal) was verified to red ONLY AC-5b while AC-5a stays green — exactly the C1 prediction, captured and reverted. `scene_serialize` now maps names in both directions (D5) — `docs/09-file-formats.md` §2.2 is rewritten, the old "names never round-trip" test is inverted in place rather than deleted, and the six names already committed in `samples/phase-1-scene/scene.json` now survive a load for free. On the editor side: three new PUBLIC headers (`selection.hpp`'s `Selection` — multi-entity, ordered by selection order, primary = most-recently-added, never stores `Entity{}`; `panel_context.hpp`'s `PanelContext{World&; Selection&;}`, forward-declared-only so it costs nothing on every panel; `entity_ops.hpp`'s pure structural ops — `isDescendantOf`/`topMost`/`entityLabel`/`createEntity`/`destroyEntities`/`duplicateEntities`/`canReparent`/`reparentEntity`/`seedDefaultScene`/`RootOrder`, the D15 seam 2.4.2's command stack will wrap, not re-derive) and one new src-private ImGui TU, `hierarchy_panel.{hpp,cpp}` — the panel itself, a strict five-phase frame (reconcile → shortcuts → read-only tree walk → void target → apply ONE `PendingAction`) that never mutates the World or the Selection during its draw walk (D12 — forced by both `eachChild`'s no-mutation contract and ImGui's `TreeNodeEx`/`TreePop` balance) and contains no recursive function anywhere (D13 — `misc-no-recursion` is `--warnings-as-errors` in CI and has reddened a lane before). `duplicateEntities` walks `componentTypeAt`/`copyComponent` over the WHOLE registration table, not a hardcoded list of built-ins (D4) — proven by a test-local `Marker` component registered through `engine::scene::internal::registerComponent<T>` that no line of editor code has ever heard of, asserting the copy's FIELD VALUE (not merely its presence) survives; sabotage S3 (hardcoding the walk to the first 5 registered types) reds exactly that value assertion. `RootOrder` gives ROOT entities a stable display order the World deliberately does not model (`World::eachEntity` walks a swap-and-popped packed array, which reshuffles on every delete) via a `reconcile()` that diffs against a generation-stamped table rather than tracking notifications, so it survives a World replaced wholesale behind its back (2.5.1's future load, 2.4.2's future undo); sabotage S4 (a plain rescan) reds the post-delete-stability case. The `onDraw()` → `onDraw(PanelContext&)` cutover (`panel.hpp`'s pure virtual) had to be genuinely ATOMIC across six files — `panel.hpp`, `placeholder_panel.{hpp,cpp}`, `shell_ui.{hpp,cpp}`, `editor_app.cpp`, and the five stub overrides in `tests/editor/shell_test.cpp` — because a pure virtual's signature change breaks all of them in the same instant; it built clean on the first try. `aero_editor_core` now links `aero::scene` **PUBLIC** (the first time any editor target links an engine layer above `platform`), which costs the scene boundary probe nothing (`aero_scene` links EnTT PRIVATE, unchanged) and is NOT gated on `AERO_REFLECT_TOOLS` (F21), so the tools-OFF build and the ungated tier-0 shell test both survive. The editor now seeds a real three-entity default scene at startup (Main Camera + Directional Light + Cube, `EditorAppConfig::seedDefaultScene = true`) — the same `seedDefaultScene(World&)` free function 2.5.1 will reuse for File > New Scene — so a freshly launched editor is no longer an empty box; `aero_editor_imgui_test`'s existing GPU smoke test now draws the REAL `HierarchyPanel` over that seeded scene (an unbalanced `TreePop`/`PopID` is an `IM_ASSERT` abort in the Debug ImGui build, so a green run proves I3 for a leaf row, always `open`, and for a collapsed non-leaf row, never opened — not the expanded/nested-descent path, which no row in this GPU test ever reaches; that path's own invariants are instead proven at tier-0, with no GPU, by `hierarchy_test.cpp`'s `walkForest` cases, added in this task's own review-round-2 hardening pass) and gained two new cases: a full create/select/destroy/duplicate cycle exercised behind the panel's back, and a `seedDefaultScene = false` empty-tree case. Two real UBSan/alignment findings from the plan's own corrections were honored exactly as specced: the drag-drop payload is read via `std::memcpy` into a local `Entity`, never a cast (C6 — ImGui's payload buffer is `alignas(1)`, and the Debug lanes run UBSan), and drop legality is decided by PEEKING the payload with `GetDragDropPayload()` rather than calling `AcceptDragDropPayload` speculatively (C7), which is what keeps an illegal drop from ever drawing a highlight (AC-15) — the one acceptance criterion with no mechanical proof at all, closed only by sabotage S5 (human-performed: dropping the `dropLegal` conjunct makes the highlight appear; reverted, it does not) plus a documented-pending human validation row. `<imgui_stdlib.h>` is included flat, never under `misc/cpp/` (C5 — the vcpkg port installs it flat despite the upstream repo's own layout). **One real plan defect was found and fixed during implementation**: spec Step 4f's `hierarchy_test.cpp` skeleton included `<aero/editor/entity_ops.hpp>` a full commit before that header exists (Step 5) — landing it as specced would have broken the green-per-commit contract, so the include (and nothing else) was deferred one commit, with zero effect on the final diff. Sabotage S2 (dropping the `topMost` filter from `duplicateEntities`) reds the E9 parent+child-selected case exactly as predicted. `ctest -N` stayed **83 → 83** throughout (both new TUs ride existing ctest entries — `aero_editor_shell_test` 15 → 16 doctest cases, `aero_editor_imgui_test` 1 → 3), tools-OFF stayed **5 → 5**, and all five boundary-probe `target_link_libraries` lines plus `aero_tests`' own link line are byte-identical — verified by `git diff --stat` against `origin/main`.

#### Task 2.2.2 — Reflection-driven inspector
**2.2.2 ("Reflection-driven inspector") landed, composing on top of Epic 2.2's now-open state (2.2.1 opened it)**: the `"Inspector"` placeholder is replaced by a panel that lists the primary-selected entity's registered components in registration order and renders a working editor for every reflected field, driven entirely by generated `entt::meta` — a brand-new `[[engine::component]]` type gets a working UI with **zero editor changes**, proven by a fixture (`InspectorProbe`, `tests/editor/fixtures/inspector_probe.hpp`) no editor source names (`git grep InspectorProbe -- editor/` is empty). **New annotation vocabulary**: `AERO_RANGE(min, max)`/`AERO_COLOR` (field-position macros, `annotations.hpp`), mirrored at runtime by `engine::reflect::FieldUiMeta` — a plain `bool`/`double` aggregate carried as **sparse** entt custom data per data member (an unannotated field carries none; `--emit-meta`'s conditional `#include <aero/reflect/annotations.hpp>` fires only when at least one custom will exist, so an annotation-free consumer's generated bytes are byte-identical to before). **`std::string` joined the reflectable subset end-to-end** (`classifyField`'s new `String` category, `--emit-meta`, `--emit-json`, `serialize.hpp`'s two new leaves) via **three exact canonical-spelling comparisons** — `std::string` (macOS/libc++), `std::basic_string<char>` (the predicted Linux/Windows spelling), and the fully-qualified default-allocator form — deliberately never a prefix match, which would also accept an allocator-customized `std::basic_string` `serialize.hpp` has no overload for (the exact `long double`/`__int128` class of bug 1.2.2's whitelist rewrite already fixed once). The engine gained the euler pair `eulerAngles(Quat) -> Vec3` / `fromEulerAngles(Vec3) -> Quat` (`core/math`, GLM backend) for the rotation UI — verified numerically (not assumed) that GLM's convention is `fromEulerAngles({x,y,z}) == qZ*qY*qX`, that the asin-based Y (yaw) axis loses ~3.5e-4 rad of precision at the ±90° poles (X/Z stay exact via atan2), and that the ctor is not exactly unit (`normalize()` at the call site is load-bearing). `Camera::fovYRadians` gained `AERO_RANGE(0.0175f, 3.1241f)`, `MeshRenderer::primitive` gained `AERO_RANGE(0, 2)`, and `MeshRenderer::color`/both lights' `color` gained `AERO_COLOR` — headers only, no `.cpp`/layout change (attributes vanish under the real compiler). **`editor::component_ops`** (`editor/include/aero/editor/component_ops.hpp`, entt-free) is the seam **task 2.4.2 will wrap, not rewrite** (subtask ④ stays unchecked by design, D2): `addComponent`/`removeComponent`/`readComponentField`/`writeComponentField`, with **constness deliberately not uniform** — `buildInspectorModel` (`inspector_model.hpp`) takes `const World&` because its read path (`World::getRaw() const` → `entt::meta_type::from_void(const void*)` → a by-value-copied `meta_any`) provably needs nothing more (making the model build's non-mutation a *compiler* fact); the four seam functions take `World&` because the write path (`from_void(void*)` + `meta_data::set`) genuinely refuses a const instance. The seam narrows and clamps in C++ before writing the EXACT concrete type — never lets EnTT convert (measured: EnTT's own `set()` wraps `300` into a `uint8_t` as `44` and silently accepts `bool`↔numeric crossings); the width/range clamp follows a four-row table (int64/uint64 crossing into either a signed or unsigned destination is a defined, tolerated pairing — a `bool`/`double`/`Vec3`/`Quat`/`string` crossing into a mismatched kind is the genuine rejection). `editor/src/meta_utils.{hpp,cpp}` is the ONE shared arithmetic-type map: **17 C++ types, not the spec's 15** — `tools/reflect-gen`'s 18-`CXTypeKind` whitelist maps to 17 distinct types (`Char_U`/`Char_S` both denote plain `char`), and omitting `char`/`signed char`/`char16_t`/`char32_t`/`wchar_t`/`long`/`unsigned long` would silently accept such a field into `entt::meta` while rendering NOTHING for it in the UI — a defensive `AERO_LOG_ERROR` fires if the two lists ever drift apart, and `char`/`wchar_t` signedness is asked via `std::is_signed_v<T>` at compile time on the building host, never assumed (implementation-defined on both). `editor_reflection.{hpp,cpp}` is the D16 bootstrap: idempotent, process-lifetime, called once from `EditorApp::create()` before `ImGuiLayer::create`, degrading to one `AERO_LOG_WARN` under `-DAERO_REFLECT_TOOLS=OFF`. `inputTextString` (and its resize-callback machinery) was promoted from `hierarchy_panel.cpp` into a new shared `editor/src/text_input.{hpp,cpp}` pair, byte-identical in behaviour, so `inspector_panel.cpp` reuses it instead of re-deriving it. **The Inspector panel itself** (`inspector_panel.{hpp,cpp}`, the only new ImGui TU) is a four-phase frame (reconcile a Quat/string edit cache whose target no longer resolves → build a fresh `InspectorModel` into caller-owned scratch every frame, D15, reused at BOTH the outer-component and per-field-vector level so capacity survives a same-shape rebuild → draw, with value edits writing through the seam immediately and Add/Remove only ever recorded → apply, the one place a component is actually added or removed): `Bool`/`Int`/`UInt`/`Float` route through `Checkbox`/`DragScalar`; `Vec3` through `DragFloat3` or `ColorEdit3` (`ImGuiColorEditFlags_HDR`, preserving values > 1) depending on `AERO_COLOR`; `Quat` through a euler-degrees `DragFloat3` backed by a per-frame cache keyed on `(entity, type, field)` so the displayed digits do not jump mid-drag (re-deriving only on release, which is what visibly costs the ~3.5e-4 rad Y-pole precision — documented as known-and-expected, not a defect); `std::string` through the promoted `inputTextString`, committing only on `IsItemDeactivatedAfterEdit` (never per-keystroke). `CollapsingHeader` needs no `TreePop` (`NoTreePushOnOpen`); a component's short name (the substring after its last `::`) is the header label, its full registration name an `IsItemHovered` tooltip; "+ Add Component" lists every ABSENT registered type by short name, "(none)" disabled when none remain. **No new dependency, CI step, or boundary guard** — `component_ops.hpp`/`inspector_model.hpp` wrap no third-party type (the boundary rule holds by construction, the render/reflect precedent), and the editor's public headers stay entt-free BY FILE PLACEMENT exactly as for ImGui (R12: a probe cannot enforce this, proven again by seeding a leak and reverting) — `editor/CMakeLists.txt` gained EnTT PRIVATE (already a manifest dependency since 1.1.3) and an `aero_reflect_generate()` call over the four built-in component headers, compiling their generated `entt::meta` registration INTO `aero_editor_core` unconditionally (gated only by the one `#if` inside `editor_reflection.cpp`). Two new tests: the gated `aero_editor_inspector_test` (tier-0, no GPU, links `aero::editor_core aero::scene aero::scene_internal aero::core`; 12 `TEST_CASE`s) covering model ordering/filtering, every field kind's range/colour metadata, the `const World&` compile-time pin, the `long`/`char16_t` O2 coverage pins (round-tripped through the seam), seam round-trips and rejections (kind mismatch, unknown field, unregistered id, dead/null entity), the range clamp (`99→10`, `−5→0`), the width clamp (`300→255`, discriminating; `−1→0`, coverage-only — S5's exact 2.2.1-C1-shaped lesson), `addComponent`/`removeComponent` semantics (D10 refusal, idempotent-false), a meta-less runtime-registered type (`hasFields == false`, E4), model scratch capacity reuse (D15), and the AC-12 drift pin registering every built-in via `addRaw(id, e, nullptr)` and calling the REAL production `registerEditorReflection()` (forward-declared without its private header, the same technique the generated-aggregator NOLINTs already use); and one new GPU-gated case in `aero_editor_imgui_test` driving the real `InspectorPanel` over the seeded scene's `"Cube"` entity through 3 ticks, a multi-select tick, and a component removed-then-re-added behind the panel's back between ticks with no crash (an unbalanced ImGui call here would abort via `IM_ASSERT` in the Debug build). Nine new `reflect-gen.*` process-boundary cases (61 → 70 `_aero_reflect_cases`) cover every annotation path (valid range/colour, malformed bounds, misapplied range/colour, an unknown `engine::` annotation, a foreign non-`engine::` annotation ignored SILENTLY) and the `std::string` category's positive/negative-include/JSON cases, over two new fixtures (`component_annotations.hpp`, deliberately on no `HEADERS` list since it exists to produce warnings; `component_text.hpp`, a namespaced, annotation-free `Labelled` component — the AC-4 negative-include case) — plus two new runtime round-trip cases in `json_test.cpp` over the generated pair. `meta_test.cpp`'s Camera/Light case gained runtime `FieldUiMeta` assertions (present on the annotated fields, `nullptr` on `nearPlane`, `nullptr` through a wrong type) plus the declaration-order pin over both `Transform`'s and `Camera`'s `.data()` iteration (the F6/S1 target). **A verified correction against the spec's own text (C1, load-bearing)**: `std::string`'s canonical spelling under clang 18's default `PrintingPolicy` is `"std::string"` on macOS/libc++ (not a `basic_string<char, ...>` prefix as the spec assumed) — the exact-match design is what a hypothetical fourth Linux/Windows spelling would fail LOUD AND SAFE against (`[unsupported]` + one warning + exit 0, never a miscompile; a documented, trigger-gated structural-match fallback is recorded but not applied pre-emptively). **All seven mandatory sabotage proofs (§V3) were performed, each seed confirmed present via `git diff` before trusting the verdict, and reverted**: S1 (reverse `emitMeta`'s `.data` emission order) reds `aero_reflect_meta_test`'s order pin; S2 (drop the range clamp, keep the width clamp) reds the range-clamp case; S3 (hardcode `buildInspectorModel`'s walk to the five built-in names) reds the `InspectorProbe` model case — the AC-11 zero-per-component-code proof; S4 (stop emitting `.custom`) reds both `reflect-gen.annotations_meta` and `aero_reflect_meta_test`'s runtime assertions; S5 (replace the saturating `narrowFromUint64` with a plain `static_cast`) reds the `300 → 255` width-clamp case ALONE, confirming the `−1 → 0` line is coverage that cannot fail (verdict read off the `300` line only, per the 2.2.1 C1 lesson); S6 (skip the aggregator call inside `registerEditorReflection`) reds the AC-12 drift pin while `aero_editor_imgui_test` still passed in full, confirming the tools-OFF degradation path is genuinely independent; S7 (truncate `ArithmeticTypes` to the spec's 15 by dropping `long`/`char16_t`) reds both O2 pins and fired the defensive-skip `AERO_LOG_ERROR` for each. **One minor plan deviation, logged**: the plan's own `annotations_components`/`annotations_meta` illustrative substrings for the `speed AERO_RANGE(0.0f, 10.0f)` field disagreed with each other (`"[range 0:10]"` vs `.rangeMin = 0.0, .rangeMax = 10.0`) — the fixture's literal (`0.0f, 10.0f`, which parseRangeToken stores verbatim minus only the trailing `f` suffix) was kept, and the components-mode assertion was written as `"[range 0.0:10.0]"` to match it, since the token-preserving design (verified against the real tool's output) is the internally-consistent choice and the meta-mode assertion is the more load-bearing of the two (it is what the generated `.custom<>` literally emits). **A second, cheaper deviation**: §V2's clang-tidy pass needed three additional fixes beyond the plan's own predictions (`modernize-use-auto` ×4 in `meta_utils.{hpp,cpp}`, `bugprone-signed-char-misuse` on `narrowFromInt64`'s signed branch — fixed via an intermediate `long long` cast rather than a `NOLINT`, `modernize-raw-string-literal` on one new test string literal) — landed as their own `fix:` commit rather than folded backward into an already-landed step commit. **A third, load-bearing deviation from the plan's own literal §S 6a/6d sketch**: the write seam's kind-check is MORE PERMISSIVE than a naive same-signedness read of the seam's own four-row clamp table would suggest — an `int64` input is tolerated into an UNSIGNED destination and a `uint64` input into a SIGNED one (both rows are explicitly defined in the plan's own table), and only a `bool`/`double`/`Vec3`/`Quat`/`std::string` crossing into a mismatched-category destination is the genuine kind-mismatch rejection; a first draft was accidentally stricter (rejecting any signedness crossing) before this was caught locally via the `S5`/O2 test battery. `ctest -N` **83 → 94** (+10 `reflect-gen.*` process cases → 71 `_aero_reflect_cases`, the tenth being `annotations_nonfinite` from the code-review hardening below; +1 gated target `aero_editor_inspector_test`, 14 cases); tools-OFF stayed **5 → 5**; `aero_editor_imgui_test` 3 → 4 `TEST_CASE`s; `meta_test.cpp` 4 → 4 `TEST_CASE`s (extended in place, no new case); `json_test.cpp` gained 4 new `TEST_CASE`s (the `std::string` leaf battery + the generated-pair round trip + the null-string corner). **Zero change under `runtime/`/`samples/`/`shaders/`/`.github/`/`cmake/`; `vcpkg.json` and the `/vcpkg` pin byte-identical; `aero_tests`' link line and all five boundary-probe link lines byte-identical; no new CI step, no new boundary script, no new probe, no new risk row** — verified by `git diff --stat` against `origin/main`. The macOS mechanical/structural pass (build, full ctest, the tools-OFF proof, the `AERO_REQUIRE_GPU=1` rehearsal, the non-interactive launch proof, all seven sabotage proofs) is green, and the **macOS human mouse/keyboard pass is now recorded PASS** (2026-07-27, `editor/VALIDATION.md`) — closing the interactive half of AC-9/AC-10 and edge cases E11/E12/E19; Windows/Linux human rows stay pending a native run, so the gate is macOS-PASS / two-OS-pending, the precedent every prior editor task in this ledger has recorded. **A high-rigor code review then found nine defects, none blocking, every one latent behind a fully green 93/93 local gate — and all nine sat on the "a brand-new component just works with zero editor code" path that IS this task's premise.** The load-bearing one: `parseRangeToken` used `strtod` as its C++-literal grammar oracle, so `AERO_RANGE(0, INFINITY)`, `NAN:10`, `1e400` and a negative bound on an unsigned field were all accepted silently and emitted **verbatim into generated code** — `inf`/`INF` were rejected only by accident (the trailing-`f/F` strip mangles them to `in`/`IN`, while `INFINITY` ends in `Y` and survives), and the resulting TU compiled **only because entt transitively drags in `<cmath>`**; now grammar- plus `errno`/`isfinite`-validated (`looksLikeNumericLiteral()`), with `annotations_nonfinite` pinning it. Downstream of that, unvalidated bounds reached `static_cast<std::uint64_t>(-1.0)` in the panel and seam — **UB that aborts under the Debug UBSan lanes**, proven by a real revert-and-abort, now routed through `doubleToClamped<T>`. Also fixed: a `StringEditCache` strandable across a selection change (displaying then committing a value the World never held), duplicate ImGui IDs in the Add-Component popup (violating the panel's own D13/E14 discipline), a silent unmapped-type read rejection contradicting the seam's own contract and R12's audibility argument, and **two tests that could not fail** — AC-7's "never mutates" clause (the very next line overwrote the evidence) and the D15 scratch-reuse case (which passed even for the `clear()`-and-rebuild implementation D15 exists to forbid); both rewritten and sabotage-proven. **Then CI found two more that macOS structurally cannot see, both the same disease — code relying on a transitive dependency only one toolchain provides.** Linux/GCC: `component_text.hpp` used `std::uint16_t` with only `<string>` included — libc++ supplies `<cstdint>` transitively, libstdc++ does not (fixed there plus `meta_utils.hpp`/`serialize.hpp`, which were the same latent break, unreached only because ninja stopped at the first error). Windows/MSVC: a raw string literal passed **directly as a doctest macro argument** — MSVC's legacy preprocessor does not honour raw-string semantics inside macro arguments, so the `\"` sequences tokenise as escaped quotes and `world` becomes an invalid literal suffix (C2017/C3688/C2661); hoisting the literal into a named local fixes it. **The discriminator matters and is recorded in the code**: five PRE-EXISTING raw literals in that same file sit inside macros and have always passed on MSVC — the killer is `\"` specifically, not raw-strings-in-macros, so those five must not be "fixed". Both risks the plan had flagged as unresolvable locally are now retired by evidence: the `std::string` canonical spelling matched on **both** MS STL and libstdc++ (`reflect-gen.string_components`, the loud canary, passed on all three lanes), and the Linux Debug lane's UBSan exercised the clamp paths. **Merged via a MERGE COMMIT, not a squash** (PR #46, merge `20a52b4`) — a deliberate convention change from PRs #38–#45: GitHub counts contributions only for commits that land on the default branch, and a squash discards the branch's commits in favour of one new GitHub-authored commit, so this task's 24 commits would have become 1. All 24 are on `main`, authored by the project owner's linked address. Since the July 2026 re-plan, **every phase (0–8) is broken down to epic → task → subtask depth** in `docs/tasks/phase-N.md` — `docs/07-tasks.md` is the index. Numbering is append-only (new work takes the next free number; existing numbers never change).

#### Task 2.2.3 — Viewport panel
**2.2.3 ("Viewport panel") landed, closing Epic 2.2's last structural placeholder**: the `"Viewport"` placeholder is replaced by a panel that renders the editor's live `World` — through the existing `scene_render::SceneRenderer` — into an offscreen texture and displays it filling the dock node, resize-safe. The engine gained a new public type, `engine::render::RenderTarget` (`engine/render/include/aero/render/render_target.hpp` + `src/render_target.cpp`), the offscreen sibling of `render::Renderer` — a NEW engine type, not an editor appendage, because `render::Frame`'s constructor is private with the sole `friend class Renderer;`, and making it public would destroy the "only a maker opens a Frame" invariant while a `friend class engine::editor::ViewportPanel;` in `renderer.hpp` would be the golden rule's exact prohibition (D1/A4). `RenderTarget` mirrors `Renderer`'s shape exactly (D2): it acquires and submits its OWN command buffer, so the only edit to `renderer.hpp` is two lines — a `class RenderTarget;` forward declaration and `friend class RenderTarget;` next to the existing `friend class Renderer;` — and `Frame` stays byte-for-byte frozen. Sizing is a **pure, GPU-free, total, `noexcept` free function**, `nextTargetExtent(requested, current, quantum, maxExtent)` (D5/D6): per axis, clamp the request to `[1, maxExtent]`, round up to `quantum`, then apply first-allocation/grow/shrink-at-half-with-hysteresis/keep branches — quantised allocation plus a UV sub-rect turns a continuous resize drag from ~120 reallocations into a handful, proven by a self-checking oracle (§6.2 case 4) that replays the pure policy over the identical resize sequence and asserts the object's real reallocation count matches it exactly. `RenderTargetConfig::quantum` defaults to **1** (exact, `textureExtent() == drawExtent()`) — the engine-neutral default; only the editor's `ViewportPanel` opts into 64. `RenderTarget::beginFrame` reports `Frame::extent()` as the **drawn sub-rect** (`drawRect`), not the (larger, quantised) allocation — this is what makes `SceneRenderer`'s projection aspect correct for free, with zero change to `scene_render`, and is the exact property sabotage S3 (seeding `beginFrame` to pass `allocExtent` instead) reds. The editor's `ViewportPanel` (`editor/src/viewport_panel.{hpp,cpp}`, the only new ImGui TU this task adds) is TWO-PHASE by design (D3): `onDraw()` runs inside the ImGui frame — measures `GetContentRegionAvail() × io.DisplayFramebufferScale` (D7, the pixel-not-logical sizing that keeps the viewport crisp on Retina), lazily/latched-initialises the `RenderTarget` + `SceneRenderer` pair on first draw (D11 — a hidden or never-opened Viewport costs nothing, `EditorApp::create()` cannot fail on a shader problem), resizes, fetches the native texture pointer through a NEW fourth `rhi::internal::NativeDeviceAccessor::texture()` accessor, calls `ImGui::Image`, draws a `"{W}x{H}"` + (when cameraless) `"No camera in scene"` overlay, and records a pending request; `renderScene(World&)` runs OUTSIDE the ImGui frame, called once per tick from `EditorApp::tick()` between the draw walk and `layer.endFrame()` (D3's exact ordering — only the draw walk knows this frame's panel size, which is what removes the one-frame resize lag that rendering at the top of `tick()` would cost) — it is a no-op unless `onDraw()` recorded a request THIS frame (AC-7/AC-10 fall out for free: a hidden panel never draws, a minimized window has zero content area) and touches NO ImGui state (INV-3). Correctness of the two-command-buffer ordering (ours submitted before ImGui's) rests on SDL_GPU's implicit graphics-read synchronisation and its end-of-render-pass transition back to default (sampler-readable) usage, verified at source in the pinned SDL 3.4.12 Vulkan backend (`SDL_gpu_vulkan.c:8202-8231`'s three transition loops) — not recalled from memory. `imgui_layer.{hpp,cpp}` stayed byte-identical for the **fourth task running** (D4/INV-4). Tools-OFF (`-DAERO_SHADER_TOOLS=OFF`) degrades, not breaks (D12): the panel latches `Unavailable` with `"Viewport unavailable — built without AERO_SHADER_TOOLS"` and logs exactly one WARN, mirroring 2.2.2's reflection-tools degradation. **Three verified corrections against the approved spec, all recorded up front rather than absorbed quietly**: **C2 (load-bearing)** — the sizing round-up must be computed in **`std::uint64_t`** and saturated at `maxExtent` before narrowing; a 32-bit `((req + q - 1) / q) * q` WRAPS at an extreme `maxExtent` (`req = max = UINT32_MAX, q = 64` → `req + 63` wraps to `62` → `62 / 64 * 64 == 0`, silently returning an allocation SMALLER than the request and breaking INV-1) — unsigned wrap is defined behaviour, so UBSan never sees it, and the spec's own postcondition sweep (`req ≤ 2048`) cannot reach it; pinned by one matrix row (`req = max = UINT32_MAX, q = 64`) and a dedicated max-boundary sweep (`maxExtent ∈ {8192, UINT32_MAX−1, UINT32_MAX}`). **C3** — `ImVec2 + ImVec2` does not compile in this codebase (`IMGUI_DEFINE_MATH_OPERATORS` is defined nowhere, verified by a zero-hit grep over `editor/ tests/ engine/ samples/ cmake/ CMakeLists.txt`), so the overlay's cursor offset is written component-wise (`ImVec2(imageOrigin.x + OVERLAY_INSET, imageOrigin.y + OVERLAY_INSET)`) rather than as the spec's sketch — deliberately NOT introducing the macro, which would be a new project-wide idiom shipped by one panel. **C4** — `render.hpp`'s own comment ("renderer.hpp is frozen — the three new headers below are purely additive") was corrected in place to four headers plus D2's two-line exception, since leaving it stale would mislead the next reader into treating the `friend` line as an accident. **A fourth correction, found during implementation and NOT anticipated by the plan (load-bearing, environment-verified)**: on this real macOS/Metal hardware, an over-hardware-limit `createTexture()` request does **not** fail gracefully through the RHI's `nullopt`+ERROR contract — Metal's OWN texture-descriptor validation calls `std::abort()` from inside `SDL_CreateGPUTexture`, before the RHI can ever see a null result, **unconditionally** (independent of `DeviceDesc::enableDebugLayer` and the `MTL_DEBUG_LAYER` environment variable, both tried and both still aborting; confirmed against Apple's own developer forums, which document this as inherent to Metal, disableable only through Xcode's own scheme diagnostics, inapplicable to a `ctest` CLI run). Spec §6.2 case 9 ("force the not-renderable path by raising the ceiling to `{100000,100000}`") would therefore **SIGABRT the whole `aero_tests` binary on the macOS lane** — not a graceful test failure but a process crash taking every other case down with it. Fixed by adding one narrowly-scoped, well-documented guard inside `RenderTarget::allocate()` itself (in scope: this task's own new file): a `HARDWARE_SAFE_TEXTURE_DIMENSION = 16384` pre-check (the real ceiling shared by Metal, D3D12 feature-level 11+, and every desktop Vulkan driver this engine targets) that fails gracefully with the SAME documented `false`+ERROR contract BEFORE ever calling into the backend. Case 9 (`tests/render_target_test.cpp`) now exercises exactly this path and passes clean under ASan/UBSan. **Code review then established that the guard was in the wrong LAYER, and the user chose to fix the layer in this same PR rather than defer it.** The abort is a property of `rhi::Device::createTexture`, whose own "invalid handle on failure, nothing throws" contract it violates — patching one caller left every other one exposed (`Renderer::beginFrame`'s depth texture, future shadow-map/asset paths, the cooker, and any test calling `createTexture` directly). So the check moved DOWN to `validateDesc` in `engine/rhi/src/sdl_gpu_backend.cpp`, with the ceiling published as `rhi::MAX_TEXTURE_DIMENSION_2D` alongside the other engine-owned backend limits in `types.hpp`, and the `RenderTarget`-local copy was **deleted** — a second copy of that constant is precisely what drifts out of step with the one the backend actually enforces; `createTexture` now returns an invalid handle and `allocate()`'s existing `!valid()` branch turns it into the same `false`. Verified at SDL source: `SDL_CreateGPUTexture` has **no** 2D dimension check at all — the `MAX_2D_DIMENSION = 16384` test exists only on the CUBE/CUBE_ARRAY branches (`SDL_gpu.c:1293`/`:1315`), and even those are `SDL_assert_release`, i.e. an abort rather than a graceful `NULL`. Pinned by four new subcases in the existing `rhi device: T1-7 … TextureDesc` rejection battery (over-limit width, over-limit height, a wildly over-limit request, **and `exactly MAX_TEXTURE_DIMENSION_2D` ACCEPTED** so a `>=` typo cannot silently cost every caller half the usable range) and **sabotage-proven**: deleting the check does not make those subcases fail, it makes the process **SIGABRT** — `-[MTLTextureDescriptorInternal validateWithDevice:]:1416: failed assertion … width (100000) greater than the maximum allowed size of 16384` — taking the whole `aero_tests` binary down, which is the entire point of validating before the call. **KNOWN GAP, recorded not glossed:** 16384 is the shared **desktop** ceiling; the Phase 5 mobile runtime sits below it (iOS Apple-family-1/2 caps at 8192, some Android devices report 8192/4096), and neither this RHI nor SDL_GPU exposes a per-device limit query today (verified). Closing that needs a real capability query, not a bigger constant — until then a mobile over-limit request still aborts. The `NativeDeviceAccessor::texture` accessor (§O-1's settled decision) refuses a swapchain-acquired handle (`slot->swapchainOwned`) as well as a stale one — a swapchain acquisition is write-only, so handing one to ImGui as an `ImTextureID` would be a silent GPU error; its contract test and refusal live in `tests/rhi_swapchain_test.cpp` (case 7, since no test target in the tree could otherwise `#include <aero/rhi/internal/native_device.hpp>` — `aero_rhi_internal` is an INTERFACE target whose one prior consumer, `aero_editor_core`, links it PRIVATE), which is what AC-15 was amended for (see Part 2). **All nine mandatory sabotage proofs were performed, each seed confirmed present via `git diff` before trusting the verdict, and reverted**: S1 (drop the keep/shrink branches, return `want` unconditionally) reds exactly the tier-0 matrix's keep-shaped rows (4 and 6) while the postcondition sweep and defaults pin STAY GREEN — confirmed at both Step 2 (matrix) and re-confirmed at Step 3 (the AC-6 reallocation-count case, which also reds under this seed); S2 (drop the round-up, return the raw clamp) reds the quantisation rows (1, 3, 7, 8) while INV-1 (the postcondition sweep) STAYS GREEN, proving the sweep and the matrix test genuinely different properties; S3 (pass `allocExtent` instead of `drawRect` into `beginFrame`'s `Frame`) reds exactly the new F12 extent assertions added to case 1 (`frame->extent()` compared against `drawExtent()`, deliberately NOT redundant with `RenderTarget::drawExtent()` itself, which reads a different member and is unaffected by this exact bug class); S7 (seed a real, non-comment `SDL_GPUTexture* leak;` code line into `native_device.hpp`) reds `check-rhi-boundary.sh`, naming the exact file; S9 (drop the `swapchainOwned` conjunct from the refusal) reds exactly assertion 2 of the new `rhi_swapchain_test.cpp` case (8/9 assertions stay green — only the swapchain-owned row fails); S8 (move `renderScene()` to after `layer.endFrame()`) leaves the WHOLE GPU-gated `aero_editor_imgui_test` suite green — confirmed the finding IS the point: temporal one-frame staleness has no mechanical signature this harness can see, and is recorded as a human-only row. **S4 and S5 are human-only by design** (clear-colour alpha and HiDPI crispness have no mechanical proof available in this harness) and are recorded as such, not claimed as covered. **S6 (drop `std::exchange`, reset `renderRequested` only on a literal reading of "the success path") was seeded, confirmed present, and run against the new hide/show test case — and it did NOT redden.** This was investigated, not glossed over: with `setVisible` called strictly BETWEEN `tick()` calls (the only way a black-box test can drive visibility), the flag is already fully consumed by the LAST visible tick's `renderScene` call before hiding ever takes effect, for BOTH the correct "always reset" code and a "reset only when the render completes" bug — the two implementations only diverge when `beginFrame()` itself fails on a tick where the flag was true, a condition this GPU test (real window, valid extents throughout) never reaches. The **shipped code uses the correct unconditional `std::exchange`** — but a second finding from review is that **E2's stated rationale is itself factually wrong for the shipped architecture**, and that error should not be re-derived by a future task: because `renderScene` runs unconditionally on EVERY tick immediately after the draw walk (`editor_app.cpp`), the flag is always already false by the end of any tick, so the "a panel hidden between two ticks renders one stale frame" scenario E2 describes **cannot occur under either spelling**. The unconditional `exchange` is therefore *defensive* (it keeps the flag's meaning "requested THIS frame" true by construction, and stays correct if the ordering ever changes), **not load-bearing** — which is precisely why no black-box test can discriminate it, and is the real root cause of S6's non-discrimination. Recorded here exactly that honestly rather than as false coverage (the same 2.2.1-C1 / 2.2.2-S5 lesson this project has twice already paid to learn). A future task wanting to close this gap would need either a log-capture harness (none exists anywhere in this test tree today) or white-box instrumentation of `RenderTarget`'s call count — neither was in scope here. `ctest -N` **94 → 94** throughout every commit boundary (no step registers a new entry — the new TUs ride `aero_tests` and `aero_editor_imgui_test`); tools-OFF stayed **5 → 5**; `aero_editor_imgui_test` **4 → 5** `TEST_CASE`s; `rhi_swapchain_test.cpp` **+1** `TEST_CASE`; `render_target_test.cpp` is a wholly new file with **11** `TEST_CASE`s (3 tier-0 + 8 GPU-gated — spec §6.2's case 7 lives in `rhi_swapchain_test.cpp` instead, §O-1 — incl. the shader-gated draw round-trip); exactly **1** new `NOLINT` (the documented `cppcoreguidelines-pro-type-reinterpret-cast` on the `void*` → `ImTextureID` conversion). Full local ctest green on both macOS presets at every commit boundary; `AERO_REQUIRE_GPU=1 ctest` green; the tools-OFF proof green (5/5, exe builds and launches, exactly one viewport WARN alongside 2.2.2's one reflection WARN); all five boundary guards green with no allowlist change; zero change under `runtime/`, `samples/`, `tools/`, `cmake/`, `shaders/`, `.github/`; `vcpkg.json` and the `/vcpkg` pin byte-identical; `imgui_layer.{hpp,cpp}` byte-identical; all five boundary-probe `target_link_libraries` lines byte-identical — verified by `git diff --stat` against `origin/main`. **AC-15 shipped AMENDED, per the user's decision of 2026-07-27 (§O-1)**: `aero_tests`' link line gains exactly one declared token, `aero::rhi_internal`, for the identical reason `aero::scene_internal` already sits on that line (tests sit outside the boundary rule) — see Part 2 for the full accounting. Windows/Linux `editor/VALIDATION.md` rows stay pending a native run — the 0.5.3/1.4.2/2.1.1/2.1.3/2.2.1/2.2.2 precedent, not an oversight. **The macOS gate is CLOSED on both halves: the mechanical/structural pass is green end to end, and the interactive human pass (all 14 §V7 rows) was performed and recorded ✅ PASS on 2026-07-27 against `4e21179`, with no code change needed.** That pass is what closes the three acceptance criteria this harness structurally cannot prove — **HiDPI crispness (S5/AC-3)**, **clear-colour alpha opacity (S4/E4)** and **temporal freshness during a resize drag (S8/AC-4)** — each the human half of a sabotage proof that could not discriminate. Recording them as human-verified, rather than inventing a weak automated test and calling them covered, is the whole point of the split gate.

#### Task 2.2.4 — Asset browser stub — the last 2.2 placeholder but one
**2.2.4 ("Asset browser stub") landed, replacing the `"Assets"` placeholder with a read-only two-pane browser over a real directory on disk — the first editor panel that looks OUTSIDE the `World`.** Everything before it read engine state; this one reads the filesystem. It ships as a deliberate two-file split, copying 2.2.2's `inspector_model.hpp` precedent: `editor/include/aero/editor/project_files.hpp` is a PUBLIC editor header that is ImGui-free **and `<filesystem>`-free by file placement** — it speaks UTF-8 `std::string`, `std::vector` and `std::uint64_t` only — while `editor/src/project_files.cpp` is the ONLY TU under `/editor` that includes `<filesystem>`, and `editor/src/asset_browser_panel.{hpp,cpp}` is src-private and the only new ImGui TU, including no `<filesystem>` at all. Both directions are grep-asserted (INV-2). That split is what makes the interesting half — scan, sort, size-format, tree-walk, root resolution — exercised by **17 tier-0 doctest cases with no GPU, no window and no ImGui context**, riding the existing `aero_editor_shell_test` entry. **D3 — the engine's VFS was deliberately NOT extended**: a `listDirectory` virtual on `FileSystemBackend` would force 5.1.1's `PakBackend` to implement enumeration for a consumer that will never exist, because the browser lists *source* assets that never enter a `.pak`; it would also push an editor concern into `core`. **D4 — the tree is a FLAT ROW LIST**, built by a pure, no-recursion, explicit-stack `buildVisibleTree()` over the panel's cache, drawn with `ImGuiTreeNodeFlags_NoTreePushOnOpen` plus a manual `Indent`: with that flag **no `TreePop` is owed on either return path**, so the traversal carries zero ImGui and becomes a tier-0 test — inside the first panel in this codebase that also opens two child windows and a scrolling `ImGuiListClipper` table. **Five verified corrections against the approved spec, each checked at upstream ImGui `v1.92.8-docking` rather than recalled, and none of them quietly absorbed**: **(C1, load-bearing)** `ImGuiTreeNodeFlags_Leaf` makes `TreeNodeEx` return `true` **unconditionally** and `TreeNodeUpdateNextOpen` returns before any `SetNextItemOpen` handling, so the spec's `if (nowOpen != row.open) record(ToggleDir)` would fire on **every leaf row every frame** — inserting leaf paths into `openDirs`, rebuilding the tree forever, and **clobbering a genuine click recorded by an earlier row** because `pending` is one last-writer-wins slot; the shipped guard is `if (!row.knownLeaf && nowOpen != row.open)`. **(C2, load-bearing)** `TreeNodeBehavior`'s **first** statement is `if (window->SkipItems) return false;` and `BeginChildEx` returns `Begin()`'s `!SkipItems`, so with that same unguarded comparison the first frame the Assets pane is collapsed or fully clipped would report every open directory as closed and **silently collapse the user's whole tree** — the tree rows are therefore submitted only when `BeginChild` returned true, while `EndChild()` stays unconditional. **(C3)** `ImGui::Indent(0.0f)` indents by the default `IndentSpacing`, not by zero (`(indent_w != 0.0f) ? indent_w : g.Style.IndentSpacing`), so the spec's `Indent(step * depth)` would make depth 0 and depth 1 render identically; the amount is computed once and both calls are skipped when it is zero. **(C4)** `io.MouseClickedCount` is reset at the top of **every** frame and filled only on the mouse-DOWN transition, while a plain `Selectable` fires on RELEASE — so **`ImGuiSelectableFlags_AllowDoubleClick` is mandatory** or AC-3's "double-click a directory to enter it" is silently dead code. **(C5)** `"Assets"` shares `DockSlot::Bottom` with `"Console"`, and Console registers **first** (`editor_app.cpp:61` before `:62`), so Console is the selected tab and the Assets window is never drawn — both new GPU cases call `panels().setVisible("Console", false)` before the first tick, or `onDraw` would never run and they would prove exactly nothing. Scanning is **lazy, per directory, cached** (D6) and happens **only in phase 1's reconcile** (D7), never inside a widget draw: `buildVisibleTree` holds a `const FileEntry&` into the cache, and an insert can rehash and relocate it — the default root is `$PWD`, which in this repository contains `vcpkg/` and `build/`, so an eager whole-tree walk was never an option. The frame is the `hierarchy_panel.hpp` five-phase discipline verbatim (reconcile · header · tree pane · contents pane · footer, then **one** `switch` over `pending` — the only place `currentDir`, `openDirs`, `cache`, `showHidden` or `selectedEntry` is written, INV-5). Both hard caps are **surfaced, never silent** (D12): `MAX_ENTRIES_PER_DIRECTORY = 10000` sets `truncated` and the footer says "showing the first 10000 (truncated)"; `MAX_TREE_DEPTH = 32` bounds a symlink cycle (D13), and `directory_options::none` — **not** `skip_permission_denied` (F20) — is what keeps an unreadable directory reportable as `Unreadable` instead of an indistinguishable empty `Ok`. Size formatting is **integer-only, locale-free and truncating** (D15), which is what makes 1023 / 1024 / 1048575 assertable identically on three OSes; a size the OS refused renders `—`, never a `0 B` lie (AC-6). `setRoot()` is 2.6.1's **entire** integration point (D16) and `PanelContext` was deliberately not widened for a single consumer; `onDraw`'s `PanelContext&` is ignored outright (D18); the `"Assets"` id and its `DockSlot::Bottom` are FROZEN (D20/INV-1) because that id has keyed every `aero_editor.ini` since 2.1.3 — proved by a byte-identical ini diff across three launches. `editor/src/main.cpp` gained `argc`/`argv`: an optional `argv[1]` is the directory to browse, unvalidated by design (D17) — `aero_editor /definitely/not/here` still opens, docks, explains itself in-panel and quits cleanly, with **zero** ERROR or CRITICAL lines. **Eleven sabotage proofs were run, each seed confirmed present with `git diff` before its verdict and reverted after.** S1 (drop the `isDirectory` sort key) reddened cases 4 **and** 6; S2 (raw compare instead of case folding) reddened case 4 while case 6 **stayed green**, exactly as predicted — its fixture has no case-crossing pair; S3 (delete the entry cap) reddened case 9 (`10005 == 10000` false, `truncated` false); S4 (ignore `includeHidden`) reddened case 7; S5 (`parentOf("assets") == "assets"`) reddened case 2; S6 (delete the `MAX_TREE_DEPTH` conjunct) reddened case 14 — **by assertion (41 rows instead of 32), not by the hang the plan predicted**, because case 14's `openDirs` is a finite 40-entry set, which makes the verdict deterministic rather than a CI timeout; S8 (`skip_permission_denied`) reddened case 8b, which genuinely ran (euid 501, macOS); S9 (delete one `EndChild()`) aborted `aero_editor_imgui_test` with `SIGABRT` on ImGui's *"Must call EndChild() and not End()!"*; and **S10 (hoist `EndTable()` out of its `if`) DID discriminate on this lane, contrary to the plan's prediction** — `aero_editor_imgui_test` aborted on *"EndTable() call should only be done while in BeginTable() scope!"* on frame 2 of the new case, at the default 320×180 window, before the 200×120 shrink arm was even reached, so INV-3's conditional-`EndTable` half is mechanically covered rather than review-only. **S7 is recorded as a Windows-CI-only discriminator and NOT as passed**: seeding `path::string()` in place of `u8string()` leaves case 11 green on macOS because both are UTF-8 there; the MSVC lane is where case 11 catches it. **S11 (delete the row loop's `PushID`/`PopID`) is human-only** — ImGui id merging has no mechanical signature in this harness — and is `editor/VALIDATION.md` row 13, not a claimed test. The inventory moved not at all: `ctest -N` **94 → 94** on both macOS presets, tools-OFF **5 → 5**, no new ctest entry, no new CMake target, no new dependency, no new boundary guard and no new CI step; `aero_editor_shell_test` grew **44 → 61** doctest cases (+17 — the plan said +16 because it counted case 8b inside case 8), `aero_editor_imgui_test` **5 → 7**, and **zero new `NOLINT`s** (the diff contains none). Two clang-tidy findings were fixed rather than suppressed: `bugprone-exception-escape` on `leafOf`, whose declared `noexcept` conflicted with `string_view::substr`'s specified `out_of_range` (replaced by the noexcept pointer+size constructor), and `modernize-pass-by-value` on `EditorApp`'s **private** constructor, which fired the moment `EditorAppConfig` gained a `std::string` field and stopped being trivially copyable (now taken by value and moved). **Handoffs.** 2.2.5 takes the last placeholder and becomes the second tab in this same Bottom node. **2.6.1** fills `EditorAppConfig::projectRoot` with the opened project's path, caches the `AssetBrowserPanel*` the way `viewportPanel` is cached, calls `setRoot()` — and decides, *with a real consumer in hand*, whether the project belongs on `PanelContext` at all. 3.1.1's `.meta` files will list as ordinary files; whether to hide or pair them is 3.1.1/3.1.3's call, not this stub's. 3.1.3 keeps every scan/sort/format/tree-walk helper and replaces only the right pane with a thumbnail grid. **3.1.4's watcher seam already exists and is exactly two lines** — `cache.clear(); treeDirty = true;` is the whole invalidation, which is what `Refresh` and `IsWindowAppearing()` already call. And one gap that belongs to nobody: **a Unicode font for the editor is no task's deliverable.** The *path* half of non-ASCII handling is correct and tested (case 11 round-trips `straße.txt` byte-for-byte through `u8string`), but the default ImGui font is ProggyClean — Basic + Extended Latin only — so a CJK or emoji filename renders as `?`. E14/F16 is the first place that visibly costs something; it is recorded here rather than turned into an invented task. **One declared gap, not papered over:** `AssetBrowserPanel::setRoot()` (E18) has **no automated test**, because the class is src-private and no test target can name it. It ships now so 2.6.1 does not have to add *and* validate it in the same commit, and it is proven by review only — moving the class to a public header to test it would put an ImGui-adjacent panel type into the editor's public surface for a test's sake. Windows/Linux `editor/VALIDATION.md` rows stay pending an on-hardware run — the 0.5.3/1.4.2/2.1.1/2.1.3/2.2.1/2.2.2/2.2.3 precedent, not an oversight.

#### Task 2.2.5 — Log/console panel — CLOSES Epic 2.2 in code
**2.2.5 ("Log/console panel") landed, replacing the last `"Console"` placeholder with a panel showing the engine's own live log stream and closing Epic 2.2 in code — no `PlaceholderPanel` remains anywhere in the tree.** It is the first editor panel fed by an engine *push* seam (`engine::setLogCallback`) rather than by polling engine state, and the first that receives data from a thread other than the frame thread. The model/panel split follows 2.2.2/2.2.4's precedent exactly: `editor/include/aero/editor/console_model.hpp` is a PUBLIC, ImGui-free editor header (`LogEntry`/`LogFilter`/`LogSink`/`LogSinkScope`/`LogHistory` plus seven pure helpers) so the entire sink/ring/filter/formatter surface is exercised by **25 tier-0 doctest cases with no GPU, no window and no ImGui context** — including an 8-concurrent-producer arm and the R14 ownership proof — riding the existing `aero_editor_shell_test` target as a new fourth TU (`tests/editor/console_model_test.cpp`); `editor/src/console_panel.{hpp,cpp}` is src-private and the only new ImGui TU, a four-phase frame (header → the `ImGuiListClipper`-driven row list → footer → one mutating `applyPending()` switch) that never touches `console_model.hpp`'s internals mid-draw. **R14 is RESOLVED here, at the call site, with `engine/core` byte-identical** — the exact "decide at Phase 2" the risk's own mitigation text called for. `LogSinkScope`'s constructor installs a callback that captures a `std::shared_ptr<LogSink>` **by value** (never `this`, never a raw pointer); `log.cpp:123-133` copies the stored callback under its mutex and `:153-156` invokes it outside that mutex, so the `std::function` — and therefore the captured `LogSink` — is alive for the whole invocation no matter when another thread detaches. Detach-then-destroy, the ASan-proven use-after-free R14 named, is now structurally impossible, proven deterministically by tier-0 case 21 (`use_count() >= 2` while installed; a raw-pointer capture makes it exactly 1, sabotage S1's discriminator) and stressed with 4 concurrent logging threads under ASan/UBSan (case 24). A drain/join inside `engine::core` was considered and rejected: it would put a blocking wait into a global logging API for one consumer, need a reader count or a second lock on `logWrite`'s hot path (every record, every thread), and still not save a consumer that captured a raw `this` — the caller's ownership discipline is what actually has to be right, and this task is the worked example. Two more load-bearing design choices: **the sink STAGES, the panel OWNS the history** (`LogSink` is thread-safe and holds only unconsumed records; `take()` swaps the staged vector out in O(1), so the mutex never covers a container walk or a draw — everything interesting, eviction, counting, filtering, ordering, lives in the single-threaded `LogHistory` and is therefore tier-0-testable with no threads at all); and **the pump lives in `EditorApp::tick()`, never in `onDraw`** — a hidden or tabbed-away panel's `onDraw` is never called (`shell_ui.cpp:74-79`), and Console shares its `DockSlot::Bottom` node with Assets, so it is behind another tab a great deal of the time. GPU case B is the discriminator: hide the panel, emit 20 records, tick once, and assert the delta is **exactly 20** — sabotage S11 (moving the pump into `onDraw`) fails it immediately. **Trap, found only by running the case and worth remembering for any future exact-delta assertion over the log stream: `ViewportPanel`'s tools-OFF shader WARN is NOT emitted from its constructor** (the plan's own context table said it was, at `editor_app.cpp:63`) — it comes from `ensureInitialized()` on the panel's **first draw** (`viewport_panel.cpp:95`, called from `onDraw` at `:113`). So an unwarmed `EditorApp` emits that WARN inside the measurement window and the exact-delta assertion reddens for a reason that has nothing to do with the pump. Fixed with one warm-up tick before `before` is sampled; the fix does **not** mask what the case exists to catch, because under S11 the Console stays hidden across the whole window and the delta is 0 no matter how many warm-up ticks precede it. Residual fragility, unverified off macOS: `ViewportPanel::onDraw` returns before `ensureInitialized()` when the content region is non-positive (`viewport_panel.cpp:103-104`), so on a lane that needs the documented one-frame dock settle the WARN could slip back inside the window — plan R8 pre-authorises relaxing to `>= before + 20` with a recorded reason if a Windows or Linux lane ever shows it. The sink is installed as the **first statement** of `EditorApp::create()`, before `registerEditorReflection()` and `ImGuiLayer::create()`, so both tools-OFF WARNs (2.2.2's reflection WARN, 2.2.3's shader WARN), the assets-root line and "shell ready" are already in the panel the first time it draws — mechanically proven by the §V8 ordering check (`sink line < assets-root line < shell-ready line`) and by GPU case A (`logRecordCount() == 0` before the first tick, `> 0` after — the create()-time records were staged, not lost). **Four verified corrections against the approved spec, declared up front (plan §A) rather than absorbed quietly, one of them (C1) load-bearing and the single highest-value line in the plan**: **(C1)** the spec's own §3.3 sketch, copied verbatim, ships an out-of-bounds write — `counts` is a 6-slot `std::array` indexed by `LogLevel`, but `LogLevel::Off == 6` and `logEnabled(Off)` is true against *every* runtime floor (`log.hpp:83`), so a record really can carry level `Off` via `detail::logWrite`'s public entry point (already exercised by a hand-built `LogLocation` in `tests/log_test.cpp:284`) — both the increment and the eviction decrement are now range-checked (`if (index < counts.size())`), proven by a new tier-0 case 25 and reddened deterministically by sabotage S17 — but only because that case asserts `filter() == LogFilter{}`, which is the one observable the corruption actually reaches; the guard's own read-side check makes `levelCount(Off)` blind to it, and no sanitizer fires (see the honesty note below). **(C2)** `logLevelColor`'s `Trace`/`Debug` and `Info`/`Off` arms are merged into shared `case` labels, since `bugprone-branch-clone` is `--warnings-as-errors` on the Linux Debug lane and two token-identical consecutive bodies are exactly what it reports. **(C3)** the `*logScope` dereference at the registration call site is guarded by `logScope.has_value()`, since `bugprone-unchecked-optional-access` (disabled only for `tests/`) cannot be relied on to correlate two separate `if (config.registerDefaultPanels)` blocks across an intervening `ImGuiLayer::create()`/`EditorApp` construction. **(C4)** the level combo walks a `constexpr std::array<LogLevel, LOG_LEVEL_RECORD_COUNT>` of the six selectable levels via a range-`for`, not a `std::uint8_t` counter with two `static_cast`s per iteration and an off-by-one `<=` bound. A fifth, undeclared-by-the-plan fix was found by running the tier-0 battery rather than assumed from the plan's prose: **`LogSinkScope::operator=(LogSinkScope&&)`, as literally specced, moves the incoming `sinkPtr` but never re-installs a callback for it** — so a scope that acquires another scope's sink via move-assignment after that sink's routing has already been displaced by a later constructor's unconditional overwrite (F5/E16) ends up owning a sink nothing routes to, contradicting the plan's own tier-0 case 23 narrative ("a record still reaches it" after the assignment). Fixed by factoring the R14-critical install logic into one file-local `installSink()` helper, called from both the constructor and `operator=` (never duplicating the capture line, keeping exactly one canonical spot for `[sink]` to be read/audited) — `operator=` now detaches its own prior installation, moves the sink in, and re-installs for whatever it ends up holding, matching the constructor's "acquiring a sink always makes you active" invariant. `ConsolePanel` has no production caller of `operator=` (only the move constructor is used, via `sinkScope(std::move(scope))`), so this had zero production impact, but the class's own declared contract required it. **All twenty-five tier-0 cases were run against ten sabotages (S1–S10) with the seed confirmed present via `git diff` before every verdict and reverted immediately after — all ten reddened exactly as the plan predicted**, including S9 (removing the `visibleSeq` front-prune), which reddens case 11's assertion *and* separately triggers an ASan container-overflow abort inside the very next `matchesFilter` call — a louder failure than the plan's own predicted debug-assert path, but the same discriminating property. **S17 (removing the C1 range-check guard) produces NO sanitizer fault at all, and the obvious assertion cannot see it either**: with a stack-local `LogHistory h;` (as tier-0 case 25 uses it) and `counts` not the struct's last member, `++counts[6]` lands squarely on the immediately-following `LogFilter activeFilter` member *within the same stack allocation* — neither ASan's stack-redzone protection (which guards the outer local variable's boundary, not intra-object member boundaries) nor UBSan's `bounds` check (which does not instrument `std::array::operator[]`'s definition, since it lives in a system header excluded from sanitizer instrumentation by default) fires. Worse, `levelCount(Off)` returns 0 either way, because it carries its own independent range check and never reads the corrupted slot — so the assertion that looks like the guard's test is exactly the one that cannot fail. What the corruption *does* do is silently promote `activeFilter.minLevel` from `Trace` to `Debug` (and, on the eviction-decrement half, underflow it to 255), so case 25 asserts **`filter() == LogFilter{}`** and that is what reddens: verified by seeding the guard removal, confirming its presence via `git diff`, and watching both arms fail. The general lesson, worth more than this one guard: **a range check whose only witness is a second, independent range check is untested by construction** — find the observable the corruption actually reaches, or the case is decoration. The C1 fix itself remains correct and load-bearing — `levelCount(Off)` is guaranteed `0` by its own independent read-side range check regardless of what garbage the write side leaves at `counts[6]` — and the guard is still the right thing to ship; only the *sabotage's* mechanical discriminating power in this specific harness is in question, and is recorded here rather than claimed. **The three GPU-gated cases (S11/S12/S13's targets) were all run and all discriminated as predicted**: S11 (pump moved out of `tick()`) reddens case B's exact-delta assertion; S12 (`ImGui::EndChild()` deleted) aborts with *"Must call EndChild() and not End()!"*; S13 (`ImGui::EndCombo()` hoisted unconditional) aborts with *"Calling EndCombo() in wrong window!"* — both IM_ASSERT aborts in the Debug ImGui build, immediate, exactly as the plan's asymmetric-call-pair warnings describe. S8 (the eviction loop) was additionally re-run against GPU case C and reddens it too (`12003 == 10000` false), confirming the property holds through the real ImGui draw path, not only at tier-0. **S14 is grep-only** (`ImGui::Text(runtimeString)` compiles silently in this codebase — no `-Wformat*`, no clang-tidy format check — so the §V6 INV-6 grep is the only guard, and it is part of the gate, not decoration) and **S15/S16 are human-only** (ImGui id merging and `"##"` truncation have no mechanical signature in this harness) — none of the three is claimed as mechanically covered. `ctest -N` stayed **94 → 94** on both macOS presets and **5 → 5** tools-OFF at every one of the six commit boundaries; `aero_editor_shell_test` measured **63** doctest cases at the branch's own baseline (not the plan's assumed 61 — its own §G note said "re-measure in Step 0, do not trust the log," and the re-measurement caught real drift) → **88** (+25); `aero_editor_imgui_test` **7 → 10** (+3); `aero_editor_core` **18 → 19** sources (+2 new, −1 deleted); **zero new `NOLINT`s** — every clang-tidy finding (`misc-const-correctness` on ~15 locals, `performance-unnecessary-copy-initialization` on the R14 install helper, `bugprone-exception-escape` on `logSourceBasename` — fixed by replacing `string_view::substr` with the noexcept pointer+length constructor, `modernize-avoid-c-arrays` ×2 in the test file, `bugprone-use-after-move` on the moves test — fixed with the `std::optional`-wrapper idiom `shell_test.cpp:265-267` already established) was fixed with a real code change, never suppressed. `find_package(Threads REQUIRED)` plus `Threads::Threads` on `aero_editor_shell_test`'s link line (C10) is explicit because the tree's only other `find_package(Threads)` sits inside `engine/platform/CMakeLists.txt`'s `elseif(UNIX)` branch — on macOS and Windows there is **no** `Threads::Threads` anywhere to propagate transitively, so relying on it would have been the kind of thing that is green on macOS and silently red on one CI lane. **INV-6 is grep-only and CI cannot catch a violation** — this is stated as a fact about the toolchain, not a gap invented for this task, and is worth remembering for every future ImGui panel: `ImGui::Text(dynamicString)` compiles clean on all three lanes. **A the editor has NO runtime log source a user can trigger, and this first bit during 2.2.5's own human pass** — four of the fifteen validation rows (the `##`/`%s`/newline row, hidden-panel capture, the 10 000-record ring, and non-ASCII rendering) are **unrunnable**, because after startup nothing in the editor logs at all: there is not one `AERO_LOG_TRACE`/`AERO_LOG_DEBUG` call site in the first-party tree, and every runtime-reachable site is a failure path or a once-per-lifetime notice. A successful resize, click, selection or dock change emits nothing, so the row that says "cause some logging (resize the window, click around)" describes something that cannot happen. All four properties do have mechanical coverage (GPU cases B and C, tier-0 cases 3/4/12, the INV-6 grep), so this is a validation-design gap rather than an unverified panel — but every future log-consuming panel will hit it identically. Fixing it means either emitting `TRACE`/`DEBUG` on ordinary editor events or adding a debug-only trigger behind a CMake option; **neither is anybody's task yet**. A **Unicode font is still nobody's task** — this is the second place it visibly costs something (2.2.4's E14/F16 was the first): non-ASCII log messages render as `?` glyphs while still filtering and copying byte-exactly, a documented font-range limitation, not a defect.

##### Task 2.2.4 — code-review round (four defects fixed after the five-commit series)
**A code review of `feat/2.2.4-asset-browser-stub` found four real defects — three AC violations and one factually wrong comment — all fixed on the same branch as four further green commits, with the original five left untouched.** **Gap 1 (broken symlinks vanished, and the code claimed the opposite).** `entry.is_directory(ec)` **sets** `ec` for a dangling symlink, so the entry hit `++skipped` and was never listed; the comment claiming the `file_size` failure path "is also the broken-symlink case (E6)" described a branch a broken symlink can never reach, because `is_directory` fails first ([fs.op.status] sets `ec` when an element of the path does not exist — standard-mandated, so it holds on all three lanes). **The spec's E6 premise was simply wrong while its intended outcome was right**, and the intended outcome is what shipped: a dangling link is now LISTED as a size-unknown file rendering `—`. The discriminator was measured on this lane rather than assumed — a dangling symlink gives `is_directory ec=ENOENT`, `file_size ec=ENOENT`, but **`symlink_status ec=0 type=symlink`** — so `listDirectory` now falls back to `symlink_status`, which does not follow the link and therefore answers for the directory entry *itself*: succeeding means the entry really is on disk and only its target is unresolvable (list it), failing means the entry is gone or unreachable (`++skipped`, as before). Genuine unclassifiable entries are therefore still distinguished, not conflated. In an asset browser a broken link you can SEE beats one that silently disappears. **Gap 2 (the cap bounded retained entries, not iterations).** Hidden-filtered and skipped entries never grow `entries`, so `MAX_ENTRIES_PER_DIRECTORY` bounded nothing for a directory of 500 000 dotfiles browsed with `Show hidden` off — it would iterate all 500 000 inside a *synchronous per-frame* reconcile, which is D12's "hard caps" and the requirement's "never blocks on a large tree" both failing for that input. A second bound, **`MAX_ENTRIES_EXAMINED = 2 * MAX_ENTRIES_PER_DIRECTORY`**, now caps entries *examined* and sets the same `truncated` flag, so it is surfaced and never silent. Strictly greater than the retained cap on purpose: a directory legitimately holding 10 000 visible plus 10 000 hidden entries still lists in full, and the retained cap stays what fires for an ordinary huge directory. 2× and no more, because the scan runs inside a frame. The footer text changed with it — `"showing the first 10000"` would be a **lie** when the scan cap is what stopped the walk, so it now names both caps. **Gap 3 (the selection line pre-empted the skipped/truncated notice).** `drawFooter()` returned as soon as a selected entry was found, so in a >10 000-entry directory the truncation notice **vanished the moment the user clicked any file** — precisely the "a silent cap reads as 'this is everything'" failure D12 exists to prevent, and AC-8 unmet in that state. The counts are now built first and unconditionally, and a selection APPENDS to that line rather than replacing it; a non-Ok status still contributes nothing, so the right pane keeps its monopoly on the error message. **Gap 4 (filenames containing `##` rendered truncated).** `ImGui::FindRenderedTextEnd()` stops at the first `##`, so `readme##v2.txt` displayed as `readme` and sibling directories `sprites##old` / `sprites##new` were visually identical — the same class as C7 (`%s.txt` passed as a printf format), which C7 caught and closed, and which both spec and plan missed for `##`. IDs were never affected (they come from `PushID(i)`), so this was display-only. **The tree and the contents table — the two places a user actually reads a name — are FIXED**, each against upstream `v1.92.8-docking` rather than recall: the tree now uses the `TreeNodeEx(str_id, flags, "%s", name)` overload, whose `TreeNodeExV` builds the label with `ImFormatStringToTempBufferV` and hands `TreeNodeBehavior` an **explicit `label_end`** (`TreeNodeBehavior` only falls back to `FindRenderedTextEnd` when `label_end` is null), which renders `##` literally *and* stays printf-safe; the contents table uses an empty `"##row"` `Selectable` plus a separate `TextUnformatted`, because `Selectable` has no format overload and `TextEx` never calls `FindRenderedTextEnd`. The layout is exact rather than approximate: `Selectable` calls `ItemSize()` with the label-derived size **before** the `SpanAllColumns` widening, and `CalcTextSize` returns `(0, fontSize)` for an empty display range, so the row keeps full height while `CursorPosPrevLine.x` stays at the cell origin and `SameLine(0, 0)` places the name exactly where `Selectable` would have drawn it. **The breadcrumb case is RECORDED, not fixed** — `SmallButton`/`Button` have no format overload, so closing it needs an `InvisibleButton` plus hand-placed draw-list text with its own hover/active styling: disproportionate for a stub, and new ImGui surface with its own bug potential. Navigation there is unaffected and the same name displays correctly in both fixed places; it carries an inline comment and an `editor/VALIDATION.md` known-and-expected row. **Sabotage discipline held, with two new seeds and three re-runs.** **S12** (revert Gap 1 to always-skip) reds the new dangling-symlink case; **S13** (delete the examined-cap term) reds the new examined-cap case while leaving case 9 GREEN, and **S3 re-run** (delete the retained-cap term) reds case 9 *and* the new case — proving the two bounds are independently covered rather than one masking the other; **S9** and **S10 re-run** against the rewritten draw code both still abort (`"Must call EndChild() and not End()!"` and `"EndTable() call should only be done while in BeginTable() scope!"`). **S14** (revert both `##` fixes) is **GREEN and therefore reported as NOT discriminating** — `##` truncation is a rendering defect with no mechanical signature in this harness, exactly like S11; its protections are the upstream-source verification recorded inline, a launch proof, and a human VALIDATION row, and it is not claimed as tested. One process note worth keeping: seeding a sabotage against an **uncommitted** fix and reverting with `git checkout --` silently wipes the fix, which happened once here and was caught by a follow-up grep — commit the fix first, then seed. **Inventory after the fixes:** `ctest -N` still **94** on both presets and **5** tools-OFF, both presets green with the `AERO_REQUIRE_GPU=1` ratchet, exactly two tools-OFF WARNs, all five guards green, clang-format and clang-tidy clean with **zero new NOLINTs**, and **no CMake change at all** — the four fixes touched only `project_files.{hpp,cpp}`, `asset_browser_panel.cpp` and `project_files_test.cpp`. `aero_editor_shell_test` grew 63 doctest cases (61 + the two new ones). **R6 must be re-read:** the new examined-cap case writes `MAX_ENTRIES_EXAMINED + 5` = 20 005 hidden files, so the target's isolated ctest wall time went **1.21 s → ~3.3 s** (three runs: 3.36 / 3.29 / 3.26 s) against **0.84 s** on `origin/main`. That is still well under the ~10 s threshold at which the honest fix would be to lower a cap rather than weaken an assertion, but it is now the dominant cost of the whole tier-0 target and is recorded so the number is not rediscovered later. A launch proof against a directory holding `sprites##old`, `sprites##new`, `readme##v2.txt`, a literal `%s.txt` and a dangling symlink produced **zero ERROR/CRITICAL lines and zero ImGui asserts**, which is the mechanical half of gaps 1 and 4; the *visual* half stays a human row.

### Epic 2.3 — Manipulation

#### Task 2.3.1 — Editor camera — OPENS Epic 2.3
**2.3.1 ("Editor camera") landed, OPENING Epic 2.3: the Viewport now renders through the editor's OWN camera — orbit, pan, dolly, fly, and focus (`F`) — instead of a static render of whatever `Camera` component the seeded scene happened to contain.** The **one** engine change is a defaulted trailing `const render::CameraView* cameraOverride = nullptr` on `buildRenderView` and `SceneRenderer::render` (`engine/scene_render/`): a raw non-owning pointer, not `std::optional<CameraView>` (D1) — a borrowed reference with a null state is what this codebase already spells as a pointer (`ForwardRenderer::device`, `ViewportPanel::device`), and it copies two `Mat4`s fewer per frame. A second entry point was rejected (two functions differing by one branch is worse than one function with a three-arm decision). The camera resolution became a three-arm decision, in order: the override wins when non-null (whole-struct assignment, all three `CameraView` fields); otherwise the pre-2.3.1 zero-camera early return, byte-for-byte, including the light-walk skip (INV-4); otherwise the existing scene-camera path. The scene walk (instances, `cameraCount`) and the light walk are untouched by all three arms — `cameraCount` stays filled even when an override replaces the camera, because a future Phase 4 Game view will want it. `SceneRenderer::render` gates only the two CAMERA WARNs on `cameraOverride == nullptr` (D3); the directional/point-light WARNs are unconditional. Everything else is new editor code: `editor::EditorCamera` (`editor/include/aero/editor/editor_camera.hpp` + `.cpp`) holds **four numbers** — `{pivot, distance, yaw, pitch}` — with `position() == pivot - forward()*distance` ALWAYS (D17/INV-1), so there is no second stored representation of the eye to drift; orbit changes yaw/pitch only, pan changes pivot only, dolly changes distance only, and fly changes yaw/pitch then RESTORES the eye by recomputing the pivot from the pre-look eye position. Rotation is `qYaw(world Y, outer) * qPitch(local X, inner)` (D16) — derived, not assumed: `right() = qYaw * (qPitch * unitX) = qYaw * unitX` because `{1,0,0}` **is** the pitch axis, so a rotation about world Y can never give it a Y component and roll is structurally impossible (INV-2), not merely unlikely. `ORBIT_YAW_SIGN`/`ORBIT_PITCH_SIGN` are both `-1.0F`, named constants so a post-human-pass flip is a one-character change with a reddening test (S1 proves this). The gesture vocabulary (`nextGesture`, `CameraGesture`, `CameraButton`, `CameraGestureState`, `CameraGestureInput`) is PURE — no ImGui type reaches it — and arbitrates via three rules in strict order: CONTINUE (the latched gesture's button still down, hover irrelevant — a drag that leaves the panel keeps going), END (otherwise, and only if unhovered), START (only on a FRESH PRESS while hovered — this is what makes a plain LMB press permanently inert here, reserved for 2.3.2's click-picking, AC-13). `editor::scene_bounds` (`Aabb` + `entityBounds`/`selectionBounds`/`sceneBounds`) is the world-space bounds walk `focusOn` needs and 2.3.2's ray-vs-AABB picking will reuse (D9) — it lives in the editor, not `engine/core/math`, because the one number that makes it correct (`LOCAL_MESH_HALF_EXTENT = 0.5F`, every built-in primitive fits `[-0.5,0.5]³`) is a render-CATALOG fact that expires the moment a later task imports a real mesh with real bounds; the AssetDatabase should then publish per-mesh local bounds. Both new headers are PUBLIC, ImGui-free, entt-free AND render-free — held by FILE PLACEMENT, not a guard (R12: `aero_editor_shell_test` links doctest, which puts vcpkg's shared include root on the compile line, so a leaked `#include <imgui.h>` would still compile) — and render-free specifically because `aero::scene_render` is PRIVATE on `aero_editor_core`, so naming a `render::` type in either header would break the tier-0 shell test's compile outright (D2); `viewport_panel.cpp`, which is src-private and does see `aero::render` transitively, assembles the one `render::CameraView` at its single call site in `renderScene`. `PanelContext` gained a defaulted `float deltaSeconds` field (D7's "designed to grow", `FrameClock::deltaSeconds()`, the spike-clamped dt — not `io.DeltaTime` — because the editor throttles to 20 Hz unfocused and a stall must not teleport a continuous drag); `PanelOptions` gained `noScrollWithMouse` → `ImGuiWindowFlags_NoScrollWithMouse`, because `ImGui::SetItemKeyOwner` cannot claim the wheel for an item with id 0, which is exactly what `ImGui::Image` submits (verified at source, `imgui_widgets.cpp`/`imgui.cpp` — a corrected spec call, see below). `EditorApp::viewportCamera()` forwards to `ViewportPanel::camera()`, the same "black-box signature for a black-box property" pattern `logRecordCount()` already established. **The traps, worth keeping:** `ImGui::Image`'s item id is 0, which defeats `SetItemKeyOwner` silently (would have looked like AC-14 was addressed while nothing changed) — the window flag is the real mechanism; the eight `EditorCamera` member/accessor name collisions (a data member and a member function cannot share a name — the naive spec spelling does not compile) resolved by the tree's own precedent (`RenderTarget::depthFormatValue` ↔ `depthFormat()`); `+inf > 0.0F` is `true`, so the negated-`>` idiom alone lets infinity sail through a totality guard — every guard here tests `std::isfinite` AND the sign; `std::clamp` does not sanitize NaN (returns it unchanged, by design — the finiteness sweep after every `clampState()` call is what actually catches it); `applyFly`'s ordering is load-bearing — `clampState()` must run BETWEEN the look and the eye-restore, not after, or a clamped pitch silently moves the eye (verified live by moving the call and watching the position drift by several world units). **The dead end never to retry:** do not try to make S9 (removing `sceneBounds`'s `registered()` guard) discriminate anything — `World`'s constructor registers all five built-in component types unconditionally and nothing in the public or internal API ever unregisters one, so the "unregistered `MeshRenderer`" state the guard notionally defends against cannot be constructed on a live `World`, and the one reachable `registered() == false` state (a moved-from `World`) suppresses no log either, because `beginQuery` bails at `impl == nullptr` before its own `AERO_LOG_ERROR`. The guard still ships — one line, an O(1) fast path, a moved-from short-circuit — but it is documented as exactly that, never as an ERROR suppressor; S9b (adding a WARN on the empty-result path) and case 12b (the has/get-vs-each asymmetry, using an *actually* unregistered TU-local type) are what genuinely prove the silence claim. **A second finding, surfaced by direct investigation rather than assumed from the plan:** S2 (dropping the pitch clamp) reddens case 4 exactly as predicted, but does **not** redden case 7's "extreme pitch during a fly-look, position stays invariant" arm — the eye-restore identity holds algebraically whether or not the stored pitch was clamped, because `forward()` is always well-defined for any finite angle and the restore uses whatever `forward()` the *current* pitch produces. The ordering property that arm is meant to protect (`clampState()` running BETWEEN the look and the restore) is real and was verified separately: swapping the two lines reddens the same arm by several world units. Recorded here exactly that honestly, the same way S9/S12 are, rather than either forcing a fit or silently dropping the row. Three spec-vs-tree corrections were load-bearing (the plan's own §A/§D, not re-litigated here): `SetItemKeyOwner` after `ImGui::Image` is a guaranteed no-op (id 0); `sceneBounds` cannot both take `const World&` and call the non-const `each<T>` (`eachEntity`+`has`/`get` instead, all const); the eight member/accessor collisions. `ctest -N` **94 → 94** throughout every commit boundary (no step registers a new entry — the new TUs ride `aero_editor_shell_test`, the new cases ride `aero_tests`/`aero_editor_imgui_test`); tools-OFF stayed **5 → 5** with the same two pre-existing WARNs and no third (E23 — `onDraw` returns before the camera block when `Unavailable`); `aero_tests` **351 → 356** TEST_CASEs; `aero_editor_shell_test` **89 → 114**; `aero_editor_imgui_test` **10 → 14**; the math-boundary scanned count **191 → 197** (+6, measured in a disposable worktree against `origin/main`, not assumed). Full local ctest green on both macOS presets at every commit boundary; `AERO_REQUIRE_GPU=1 ctest` green on both presets; the tools-OFF proof green (5/5, `aero_editor` launches with exactly two WARNs); all five boundary guards green with no allowlist change; zero change under `runtime/`, `samples/`, `tools/`, `cmake/`, `shaders/`, `.github/`; `vcpkg.json` and the `/vcpkg` pin byte-identical; `imgui_layer.{hpp,cpp}` and `main.cpp` byte-identical for the **seventh** task running; `aero_editor_shell_test`'s `target_link_libraries` line byte-identical; clang-format/clang-tidy clean with **zero new `NOLINT`s**. Windows/Linux `editor/VALIDATION.md` rows stay pending a native run — the established precedent for every task before this one. **The macOS human pass is pending as of this implementation commit** — it is recorded in a separate `docs:` commit after the branch merges, per the project's own established sequencing (2.2.1 through 2.2.5 all did the same).

##### Task 2.3.1 — code-review round and the Windows lane (three commits after the nine)

**A code review of `feat/2.3.1-editor-camera` found one real (latent) defect and three coverage gaps; the Windows CI lane then found a fourth defect that no amount of local macOS verification could have reached.** All four closed on the same branch as three further green commits, the original nine untouched. **The `<wingdi.h>` macro collision — the one worth remembering.** `<wingdi.h>` defines `DEFAULT_PITCH` as a font-pitch macro equal to `0`. **A macro ignores namespaces**, so `engine::editor::DEFAULT_PITCH` was substituted to `0` at every *use* site on Windows — while the header itself still compiled, because the constant is *declared* before any Windows header has been pulled into that TU and only the later use sites see the macro. It therefore surfaced not as a build error but as a **wrong value**, in one GPU-gated assertion: `CHECK( camera->pitch() == 0 )` `values: CHECK( -0.349066 == 0 )`. Note what that output proves — doctest stringifies the expression *after* preprocessing, so `0` appearing where the source says `DEFAULT_PITCH` **is** the diagnosis; the camera was correct and the *expectation* had been rewritten. Renaming is the only robust fix (a macro cannot be shadowed by a namespace, so `#undef` in one TU would leave every future consumer exposed): `DEFAULT_PITCH` → `DEFAULT_PITCH_RADIANS`, with `DEFAULT_YAW` renamed alongside it for symmetry and to match the `yawRadians`/`pitchRadians` members it initialises. **The suffix is load-bearing, not decoration — do not "tidy" it away**, and treat any new `SCREAMING_SNAKE` constant in a public editor header as a Win32-macro candidate first (`DEFAULT_*`, `MIN_*`/`MAX_*`, and anything GDI-flavoured are the risky shapes). **The real latent defect: a fixed epsilon is not a valid `a < b` floor at arbitrary magnitude.** `farPlaneValue = max(farPlaneValue, nearPlaneValue + MIN_DEPTH_RANGE)` with `MIN_DEPTH_RANGE = 1.0e-3F` breaks INV-5 (`0 < near < far`) at `near >= 32768`, because `1.0e-3F` is below half an ULP of a `float` there and `near + MIN_DEPTH_RANGE == near` **exactly** — measured, not reasoned: `20000 → 20000.001953` (holds), `32768 → 32768` (**fails**), `40000 → 40000` (**fails**). `far == near` then divides by zero in `perspective()` — which in Debug **asserts and aborts** (`glm_backend.cpp`'s `assert(zFar > zNear)`) rather than merely returning an `±inf` matrix, and which `stateIsFinite()` can never catch because both members are perfectly finite. Fixed by taking the max of *both* floors, `max({far, near + MIN_DEPTH_RANGE, nextafter(near, +inf)})`: `nextafter` **alone** is not the answer either, as it yields `100.000008` where the absolute floor yields `100.001`, needlessly collapsing the usable depth range at ordinary magnitudes. The one input with no finite answer is `near == FLT_MAX`, which now lands `+inf` in `far` — strictly better than `near == far`, because `+inf` **is** visible to `stateIsFinite()` and the next `update()` resets, whereas `near == far` was invisible forever. Unreachable in the shipped editor (nothing calls `setNearPlane`; D13 ships no settings UI) and live the moment 2.6.1 or a 2.3.3-era camera panel exposes it. **Two of the three coverage gaps were assertions that could not fail — the most valuable kind of finding.** (1) The `+inf` totality rows tested only `nan` and `0.0F`, both of which the *buggy* negated-`>` spelling rejects too, so they discriminated nothing; and the `deltaSeconds`/`viewportHeightPoints` rows started from a freshly default-constructed camera and asserted only `isfinite(distance())`, under which "input ignored" and "state reset to the D8 default" are **indistinguishable** — the poisoned path reset to default and every assertion still passed. Closed by starting from an off-default pose and comparing all eight accessors bit-identically. Note the height row needed a *different* assertion than the others: with `+inf` height the unguarded code gives `worldPerPoint = 0`, so a pan moves **nothing** — silently finite and silently wrong — so the discriminator there is that the poisoned pan equals the `1.0F` pan exactly, not that the pose is unchanged. (2) Six setters (`setYaw`, `setPitch`, `setFovYRadians`, `setNearPlane`, `setFarPlane`, `setFlySpeed`) had **zero call sites** anywhere in `editor/` or `tests/`, so four of `clampState()`'s seven statements were dead to the entire suite — each could be deleted individually and the suite stayed green, leaving correction C7 wholly unproven. Closed by a nine-subcase setter battery; each of the four deletions now reddens it. (3) Focus case 9 shipped the distance-formula check but not the plan's *independent* framing check; closed by projecting the eight box corners through `proj * view` and asserting both NDC containment **and** the angle the box subtends — the latter derived from `FOCUS_MARGIN`/fov/aspect and never from `camera.distance()`, so retuning a constant moves the bound with the implementation while a wrong distance formula still reddens. NDC containment alone is weak (it also passes for a camera parked at `MAX_DISTANCE`), and two of the three seeded framing defects are invisible to it. **A third non-discriminating guard, recorded not papered over:** `focusOn`'s aspect guard cannot be discriminated by any input, because `halfFov = min(halfY, atan(tan(halfY)*aspect))` saturates — at `aspect = +inf`, `atan(inf) = π/2` and the `min` returns `halfY`, bit-identical to the `safeAspect = 1.0F` result. It ships as defence in depth; only `projectionMatrix`'s copy is provable. **Process notes.** Every new assertion was proven to discriminate by seeding the defect it targets, watching it redden, and reverting — and two were additionally checked second-order (replace the new assertion with `CHECK(true)`, confirm the defect then passes the whole suite), which is what distinguishes "the assertion does the work" from "the harness does". The test TU was also missing an `<algorithm>` include for `std::max`'s initializer-list overload, which libc++ forgives via transitive includes and libstdc++ would not have — caught before the Linux lane ever saw it. **Corrected inventory** (the entry above was written at the ninth commit, before these three): `aero_editor_shell_test` is **89 → 117**, not 114 — the review round added 3 cases, and 117 is the merged figure. `aero_tests` **356** and `aero_editor_imgui_test` **14** are unchanged. `ctest -N` still **94** / **5**; both presets green under the `AERO_REQUIRE_GPU=1` ratchet with ASan/UBSan and no sanitizer report; all five guards green; clang-format/clang-tidy clean with zero new `NOLINT`s. **Merged as `91f3887` via a merge commit (PR #53), all 12 commits preserved** — 5/5 CI checks green across macOS, Windows and Linux.

#### Task 2.3.2 — Selection & picking

**2.3.2 ("Selection & picking") landed: clicking a mesh in the Viewport selects it, clicking empty space clears the selection, `Ctrl`/`Cmd` toggles and `Shift` adds, and whatever is selected — from the Viewport or the Hierarchy — draws back as a wireframe box with the primary entity visually distinguished.** Two new PUBLIC, ImGui-free/entt-free/render-free editor headers (`picking.hpp`, `selection_overlay.hpp`) plus two sources, a `ViewportPanel` delta of +113 lines in the `.cpp` and +16 in the `.hpp` — two one-line phase insertions in `onDraw`, the two new method bodies, four colour/thickness constants and the includes — the two phases landing between the input phase and the size readout, alongside three new members and two new private methods, two new tier-0 test TUs (`picking_test.cpp` and `selection_overlay_test.cpp`, **16 and 12 `TEST_CASE`s at merge** — 14 and 10 as first delivered, the rest added by the review round below; the 14 cover the plan's 13 numbered cases because case 6 ships as two, arms A and B, so the file's case count and the plan's case *numbering* are deliberately different units) and five GPU-gated cases appended to `imgui_layer_test.cpp` — **zero engine change**, the first Epic-2.3 task to spend none of the epic's engine budget (2.3.1 spent it on `cameraOverride`). The load-bearing design facts, restated because they are what makes the click land where it looks like it should: the local-space ray direction is deliberately **unnormalised** through `rayLocalBoxHit`, which is what keeps the returned `t` in WORLD units so "nearest wins" is meaningful across entities of wildly different scale (normalising it, S2, silently rescales `t` by the entity's own scale and a small near cube loses to a large far one); `tMin > 0` is entry-hits-only (D3), which is what stops a box you have flown into winning every click at distance zero forever, and it agrees with what the eye sees because the forward pipeline culls back faces; the ray itself is built from the camera's own orthonormal basis (D4) rather than an inverse view-projection, so there is no matrix inverse on the click path and the closed forms at the five canonical NDC points are exact; the near-plane clip in `selection_overlay`'s box builder interpolates **in clip space, before the perspective divide** (D14), which is what keeps a clipped edge visually straight as the camera flies through a selection rather than snapping to a wrong slope (S8 seeds the post-divide lerp, and the two assertions written specifically to catch it — the on-the-clip-line check and the "differs from the post-divide answer by more than a screen's width" check — were each proven load-bearing by a second-order `CHECK(true)` pass: with both weakened, the seeded post-divide defect passes the whole suite silently); and the highlight builder returning **segments**, not draw calls, is the whole reason the highlight is tier-0 assertable at all, with no window, no GPU and no ImGui context. `picking.hpp` and `selection_overlay.hpp` share **exactly one** screen mapping (`viewportNdc`/`ndcToViewportPoints`/`projectToViewport`/`clipSegmentToNearPlane`), overlay depending on picking and never the reverse, so the pick and the highlight can never disagree about where a world point lands on screen — you cannot click one place and see the box somewhere else. The traps worth keeping: a cube rotated 45° about **Y** and viewed down **−Z** has *exactly* the same silhouette as its own world AABB (for every world-x in its range some z lies inside the rotated footprint), so the obvious rotation test is vacuous and cannot discriminate an OBB test from an AABB one — a **Z** rotation is required instead, which puts the difference in the XY plane where a −Z ray can see it (§A2); `World::eachEntity` walks the packed array in creation order when nothing has been destroyed, and creation order is index order, so the tie-break's `e.index < mesh.entity.index` branch is never taken without recycling a slot first — the test constructs `[a, doomed, b]`, destroys `doomed`, creates a fourth entity that recycles the middle index with a bumped generation, and **`REQUIRE`s the resulting visit-order inversion** before asserting the winner, so the case fails loudly rather than passing vacuously if EnTT's packing strategy ever changes (§A4); the spec's own INV-4 grep (`git grep -n "aero/render\|aero/scene_render\|imgui\|entt" -- editor/include/`) is already non-empty at HEAD — **25** hits on `main` as the spec literally writes it, and 19 for the narrower `entt::` variant — neither is the 21 first recorded here; all of them prose in header comments or `editor_app.hpp`'s `#include` of `imgui_layer.hpp` (an *editor* header whose name merely contains the substring) — and would prove nothing about this task's own diff; the two comment-stripped greps in the plan's §V7 are the real check, and both are confirmed empty (§A1); `misc-unused-parameters` (`--warnings-as-errors` on the Linux lane) forced `pickSelectionAction`'s `alreadySelected` parameter unnamed in the definition even though it stays in the signature, which turned a dead parameter into a tested independence contract — every row of the sixteen-row click-decision table is run with the flag both `false` and `true` and must agree (§A6); and the seeded `Directional Light` sits at the world origin, *inside* the seeded `Cube` (`entity_ops.cpp`'s `seedDefaultScene` gives the light a rotation but no offset), so D5's depth rule correctly makes the cube win every click there and the light is unpickable until moved — behaviour, not a defect, and the validation rows say so explicitly (§G6). One review-grade finding of this task's own, worth recording for the next one that greps a `.cpp` for a bare substring: the plan's own literal `picking.cpp` comment text — `"eachEntity + has/get, NEVER each<T> -- see the header"` — matches `git grep -n "each<"` against its own prose, which would have made the AC-11 mandatory grep in §4g/§V7 non-empty on an untouched, correct tree; reworded to `"NEVER a typed query walk"` (same meaning, no substring collision) so the grep is a real, passing assertion rather than a false negative papered over by eyeballing the diff. **The dead end recorded so it is never retried: S12.** `pickEntity` takes `const World&`, and `World::each<Ts...>` is non-const, so the spec's stated seed ("use `each<MeshRenderer>` instead of `eachEntity` + `has`") does not even compile; widening the signature to `World&` compiles but reddens nothing either, because every `World`'s constructor registers `MeshRenderer` unconditionally and nothing ever unregisters it, so `beginQuery` never takes its `AERO_LOG_ERROR` path on a live World — and on a moved-from `World` it bails at `impl == nullptr` before that ERROR anyway. The silence guarantee (AC-11/INV-5) is held **structurally, by the `const World&` signature itself** — a compile-time property strictly stronger than any reddening test — and S12b (adding one `AERO_LOG_WARN` to a real `pickEntity` return path) plus case 11's `each<NeverRegistered>` positive-control canary are what prove the assertion is not vacuous: S12b reddened three of case 11's four subcases exactly as expected (the fourth, the canary itself, does not call `pickEntity` at all and could never redden from this seed). Identical in shape to 2.3.1's own S9/S12 non-discriminations — recorded, not forced. **S13 (feeding `updatePick` pixels instead of points for the pick radius) was seeded for real and confirmed to redden nothing** — the whole 94/94 tier-0 suite plus all 19 GPU-gated cases stayed green with the defect live, because points and pixels coincide on every non-Retina runner this harness has, and D18's pixel/point distinction is provably a human-only, Retina-only row (§H row 9). Two sabotages perturbed one extra assertion beyond their table entry, honestly noted rather than smoothed over: S2a (normalising inside `rayLocalBoxHit` itself, not just at the `pickEntity` call site) also reddens case 6 arm B, since arm B ultimately calls the same sabotaged function — a correct, expected mechanical consequence, not a contradiction; and S3 (replacing the OBB test with a world-AABB one, using the plan's own literal `h = max(hs.x, hs.y, hs.z)` single-scalar half-extent) also perturbs the *distance* on case 7's HIT subcase, because a uniform max-extent box is not the same shape as the true anisotropic AABB even where both still register a hit — the primary discriminator (the MISS subcase) reddens exactly as predicted either way. Known-and-accepted, all deliberate: the Plane primitive's fat box (D13/F19 — flat at local y = 0, picked and highlighted as a full 1-unit-thick box; expires with 3.1.x's real mesh bounds, in the same change that expires `LOCAL_MESH_HALF_EXTENT`); the highlight is not depth-occluded (D17); the point marker is invisible until its owning entity is selected (D8); and a viewport pick reaches the Hierarchy and Inspector on the *next* frame, one tick (~16 ms) of latency (E12). **Inventory, measured at every commit boundary, never predicted:** `ctest -N` **94 → 94** (tools ON) and **5 → 5** (tools OFF) throughout; `aero_editor_shell_test` **117 → 123 → 131 → 141** across the three code-bearing commits (picking geometry, the pick walk + click decision, the overlay builder); `aero_editor_imgui_test` **14 → 19**; `aero_tests` unchanged at **356**; `check-math-boundary.sh`'s scanned count **197 → 203** (+6: two headers, two sources, two test TUs — measured against `origin/main` in a disposable `git worktree`, not assumed); `aero_editor_core` **21 → 23** sources with a byte-identical link line; `aero_editor_shell_test`'s `target_link_libraries` byte-identical and **no new `add_test`** anywhere; `editor/src/imgui_layer.{hpp,cpp}` and `editor/src/main.cpp` byte-identical for the **eighth** task running; `ViewportPanel::renderScene` byte-identical (verified with `git diff origin/main`, no hunk at or after its signature); zero new `NOLINT`s; all five guards green with no allowlist change.

**A code review of `feat/2.3.2-selection-picking` found no functional defect — no geometry error, no ImGui-balance or arm/disarm bug, no scope creep — but five gaps, four of them assertions that could not fail. All closed on the same branch as one further green commit (141 → 145 cases), with zero production-code change.** Three were confirmed vacuous *empirically*, by seeding the defect, verifying the seed landed with `git diff`, rebuilding and watching the full suite stay green: deleting `selection_overlay.cpp`'s `allFinite(pa)/allFinite(pb)` guard, deleting `pickEntity`'s point-candidate `e.index <` tie-break arm, and moving `++drawn` above the `alive` check so dead handles consume cap budget. **The trap worth keeping is why the E4 finiteness guard was unreachable by the test that claimed to cover it:** the hostile-input case used `position.x = INF`, which puts `inf` in the model matrix, makes every clip `w` come out `0.0f * inf = NaN`, and so drops all twelve edges in `clipSegmentToNearPlane` *before* the finiteness guard is ever executed — `allFinite(scratch)` over an empty vector is vacuously true. Reaching that guard needs a **huge but FINITE** transform (a uniform `1e34` scale straddling the eye): the straddling edges clip to `w == CLIP_W_EPSILON` with `x ≈ 1e33`, `ndc.x` becomes `1e37`, and only the multiply by `width/2` inside `ndcToViewportPoints` overflows to `+inf`. A totality test that hands a function its most extreme input is not automatically the test that reaches the guard — the extreme input can be rejected earlier, by a different guard, for a different reason. **The second finding is about a constant, not a test:** `DETERMINANT_EPSILON`'s guard at `picking.cpp:191-194` turns out to be *redundant* rather than merely uncovered — deleting it entirely leaves the suite green, because GLM's `inverse` of a singular matrix multiplies a zero adjugate by `1/0`, so the NaN it produces is already caught downstream by `rayLocalBoxHit`'s `allFinite(origin)` (plan A9 predicted exactly this). The guard was left in place: it is readable, cheap, and states the intent locally. But retuning it the *wrong* way is not symmetric — seeding `1e-20 → 1e-6` reddened **nothing** in the pre-review tree, meaning an over-eager epsilon would have silently made small-but-legitimate objects unclickable in the editor with a fully green suite. That is now covered by an explicit "a uniform 1e-4 scale stays pickable" subcase. Also corrected here: three factual errors this entry originally carried — "13 cases" for a file with 14 `TEST_CASE`s, a misquoted INV-4 grep pattern with a hit count (21) matching neither the literal pattern (25) nor the quoted one (19), and "a five-line `ViewportPanel` delta" for a change of +113/+16 lines. The log is what a future task greps; a number in it that nobody measured is worse than no number.

#### Task 2.3.3 — ImGuizmo transform gizmos — CLOSES Epic 2.3 in code

**2.3.3 ("ImGuizmo transform gizmos") landed: dragging a gizmo handle in the Viewport translates, rotates or scales the primary selected entity — local or world space, hold-to-snap, writing into the entity's own parent-relative `Transform` through a new PUBLIC, entt-free `transform_ops` seam.** Two new PUBLIC, ImGui-free/entt-free/IMGUIZMO-free editor headers (`gizmo.hpp`, `transform_ops.hpp`) plus two sources, a `ViewportPanel` delta of one new phase in `onDraw` (8b′, between the camera update and picking — load-bearing in both directions, D4), one new overlay-bar call, one rewritten arm-gate comment/condition in `updatePick`, two new private methods (`updateGizmo`/`drawGizmoBar`) and five new members, one new call (`ImGuizmo::BeginFrame()`, first line of `drawShellUi`) plus its include in `shell_ui.cpp`, two new tier-0 test TUs (`gizmo_test.cpp` — 17 cases, G1–G17 — and `transform_ops_test.cpp` — 7 cases, T1–T7), five new GPU-gated cases appended to `imgui_layer_test.cpp` (I1–I5), one new vcpkg dependency (`imguizmo`, the first since 0.4.3), and one `.clang-format` `IncludeCategories` entry (the first since 0.1.6). **D0 — this task ships without undo, and that is a scope correction to the plan itself, not an omission.** 2.4.1 (Command stack) does not exist yet; `shell_ui.cpp`'s Undo/Redo menu items are still disabled stubs naming task 2.4.1. `docs/tasks/phase-2.md` was corrected in three places, mirroring 2.2.2's own precedent for `component_ops`: 2.3.3's `depends:` line dropped `2.4.1`, its deliverable line now names the `transform_ops` seam explicitly, its "Gizmo edits create undo commands" subtask moved to 2.4.2 as "Transform command wrapping `transform_ops`; gizmo and Inspector edits both route through it", and Epic 2.3's Definition of Done gained a parenthetical stating the *undoably* clause completes at 2.4.2 rather than silently dropping the word. **What deliberately did not ship, all recorded as Handoffs, not gaps:** multi-entity transform (D11 — primary-only, exactly like the Inspector); bounds/`OPERATION::BOUNDS` editing (D14 — no per-mesh local bounds exist yet; 3.1.x's real mesh bounds is what unlocks it, and the same change is what finally expires `LOCAL_MESH_HALF_EXTENT`); `ViewManipulate` (D15 — a **licensing** exclusion, Autodesk US7782319B2, not a taste one); persistent snap-toggle settings and gizmo restyling (D24 — Handoffs item for 2.6.x). **Four traps worth keeping.** F4: `ImGuizmo::BeginFrame()` is the ONLY place in the whole library that resets `gContext.mbOverGizmoHotspot` to `false` (`ImGuizmo.cpp:1016`) — skip it and the gizmo draws forever but latches permanently un-grabbable the first time the cursor crosses a handle, a one-way failure no smoke test can see (human row 6 only). F3: `.clang-format`'s `SortIncludes: CaseSensitive` sorts `'I'` (0x49) before `'i'` (0x69), so without a dedicated `IncludeCategories` entry `#include <ImGuizmo.h>` gets hoisted above `<imgui.h>` — which does not compile, since `ImGuizmo.h` only forward-declares `ImGuiWindow` and never includes `imgui.h` itself; the fix is a *category*, listed first so it wins the tie-break, not a `clang-format off` fence (A10 in the plan's own rejected-approaches list). F5: ImGuizmo's `Manipulate` leaks an unpaired `PushClipRect` on its own behind-camera early return (`ImGuizmo.cpp:2682,2698` — the `return false` at `:2698` has no matching `PopClipRect`, which only runs at `:2728`) — not an `IM_ASSERT` abort (`_ResetForNewFrame` zeroes the clip stack every frame), but a real, silent, intermittent visual-corruption defect in an editor with a fly camera; this task tests the behind-camera condition itself (`gizmoOriginBehindCamera`, sharing `picking.hpp`'s `projectToViewport` with pick and highlight — D9) and never calls `Manipulate` on that path at all. F12: ImGuizmo's rotate snap is read in **DEGREES** (`ImGuizmo.cpp:2482`, `snap[0] * DEG2RAD`) in an engine that is radians-everywhere elsewhere — the constant is spelled `GIZMO_SNAP_ROTATE_DEGREES`, not `GIZMO_SNAP_ROTATE`, specifically so the unit is never lost at a call site. **§A's four corrections to the spec, each confirmed at the tree, not assumed:** the spec's own INV-1 grep (`'ImGuizmo|ImVec|ImDrawList|ImGui|entt::|render::|scene_render::'`) is already non-empty at HEAD — ten hits, all `editor_app.hpp`'s and `imgui_layer.hpp`'s own `ImGuiLayer` class name, because the bare `ImGui` alternative carries a left word boundary but no right one; the corrected, comment-stripped token-list pair (`ImVec2|ImVec4|ImU32|ImDrawList|ImGuiIO|ImGuiKey|ImGuiContext|ImGuizmo|entt::|render::|scene_render::`, plus a separate `#include` anchor) is what §V7 actually runs, and both are empty; `const bool using_ = ImGuizmo::IsUsing();` (the spec's own §3.4 spelling) fails `readability-identifier-naming.VariableCase: camelBack` outright — spelled `isUsing` instead, confirmed by seeding `using_` back and watching clang-tidy redden (S12); three of the spec's §6.6 greps would have matched this task's own prose the same way 2.3.2's `each<T>` collision did — the `Manipulate` named-argument grep, the forbidden-entry-point grep (`ViewManipulate`/`DrawGrid`/`SetImGuiContext`/`IMGUIZMO_NAMESPACE`), and the `IMGUI_DEFINE_MATH_OPERATORS` grep — all three fixed the same way, comment-stripped and/or anchored on a construct prose cannot produce; and the spec's E6 ("nothing reads ImGuizmo's stale `mbUsing` after an abandoned drag") is flatly wrong — `HandleTranslation`'s move branch (`ImGuizmo.cpp:2184`) reads it on the very next `Manipulate` call and writes the OLD entity's `mModelSource` plus a fresh ray delta into whatever entity is handed to it next, for one frame — closed by a paired `Enable(false)` stale-latch clear at both of `updateGizmo`'s early returns (approved at plan review, shipped as documented defence in depth, not test-covered — human row 24 only). **Two findings of this task's own, beyond anything the spec or plan predicted, both verified empirically against the pinned `engine::decompose()` (`glm_backend.cpp`), not assumed:** first, `decompose()` rejects ONLY a near-zero-length or non-finite matrix column — it has no shear/orthogonality check at all — so the spec's own literal E7 construction for `NotDecomposable` (a world-space rotation delta applied under a non-uniformly-scaled parent, i.e. genuine shear) was built and measured to make `decompose()` **succeed** with a numerically wrong but finite `Trs`, `Applied` rather than refused; G11 was rewritten to a construction that reaches `NotDecomposable` for real (a directly degenerate local scale axis surviving a non-uniform parent's inversion), and the practical consequence — a world-space rotate/translate drag on a child of a non-uniformly-scaled parent may silently apply a wrong transform instead of refusing with one WARN — is recorded as a finding for the architect, not silently patched around (patching it would mean changing `engine/core`'s `decompose()`, outside this task's `engine/`-untouched invariant). Second, the plan's predicted "reaches step 6 [the write pipeline's own post-decompose finiteness sweep] and only step 6" case (a uniform `1e34` scale, finite going in, non-finite coming out) does not exist for any scale magnitude: `decompose()`'s internal `length()` call squares each column and overflows `float` at a column length around `sqrt(FLT_MAX) ≈ 1.84e19`, so `decompose()` itself already returns `false` (`NotDecomposable`) before step 6 ever runs — the full range `1e10..1e37` was scanned and no finite input was found where `decompose()` succeeds yet yields a non-finite result. G14 now asserts the measured status; step 6's own finiteness sweep ships correct and real but **uncovered by any test in this task**, the same honest status as the A4 stale-latch clear (confirmed directly: dropping it, S6, reddens nothing at all, including G14). **The dead ends recorded so they are never retried:** consulting `IsOver()`/`IsUsing()` about *this* frame's cursor before `Manipulate` runs (F8 — the 2.3.2 seam comment's own naive proposal, and wrong; consulting `IsUsing()` about whether a drag was in flight *as of the last* `Manipulate`, as the behind-camera skip does, is a different question and is correct); skipping `BeginFrame()` (F4); `ImGuizmo::DecomposeMatrixToComponents` (Euler-degrees, and documented unstable by its own author, `ImGuizmo.h:165`); `ViewManipulate` (a patent exclusion, not a taste one); removing the `.clang-format` category (verified, via a minimal repro without the comment that sits above both real `#include <ImGuizmo.h>` lines, that the hoist is real and the build genuinely stops compiling — but also verified that in THIS tree, S11, that specific comment already acts as an `IncludeBlocks: Regroup` block separator independent of the category, so the sabotage proof reddens nothing today; the category stays, exactly as specified, because a future edit stripping the comment would be unprotected without it); a full three-channel `decompose` round-trip (D5 — corrupts stored scale on every rotation drag, the channel-isolation property G7 exists specifically to hold the line on); and `component_ops::writeComponentField` for the gizmo write (D21 — three string-keyed `entt::meta` lookups per drag frame and a hard `AERO_REFLECT_TOOLS=ON` dependency, which breaks AC-17 outright). **Inventory, measured at every one of the six commit boundaries, never predicted:** `ctest -N` **94 → 94** (tools ON) and **5 → 5** (tools OFF) throughout, **no new `add_test`**; `aero_editor_shell_test` **145 → 151 → 162 → 169** across the three code-bearing commits (the tool-state model, the geometry + write pipeline, `transform_ops`); `aero_editor_imgui_test` **19 → 24**; `aero_tests` unchanged at **356**; `check-math-boundary.sh`'s scanned count **203 → 209** (+6: two headers, two sources, two test TUs — measured against `origin/main` in a disposable `git worktree`, not assumed); `aero_editor_core` **23 → 25** sources with a byte-identical `PRIVATE` link line apart from the one new `imguizmo::imguizmo` token; `aero_editor_shell_test`'s and `aero_editor_imgui_test`'s `target_link_libraries` byte-identical; `editor/src/imgui_layer.{hpp,cpp}`, `editor/src/main.cpp` and `editor/src/editor_app.cpp` byte-identical for the **ninth** task running; `ViewportPanel::renderScene` byte-identical (`git diff origin/main`, no hunk at or after its signature — INV-2/INV-3 both hold); zero new `NOLINT`s; all five guards green with no allowlist change; `vcpkg.json`'s `builtin-baseline` and the `/vcpkg` submodule SHA byte-identical. All twelve sabotage proofs were performed, each seed confirmed landed via `git diff` before trusting a verdict, each reverted and re-confirmed green afterward; S4 and S5 additionally passed the second-order `CHECK(true)` check (weakening the discriminating assertions makes the seeded defect pass the whole suite silently, proving the assertions — not the harness — do the real work); S1, S2, S7, S9, S10 confirmed non-discriminating exactly as the plan predicted (each is genuinely human-row-only); S3 and S6 were non-discriminating in a way the plan did NOT fully predict (S3: the plan named I2 as the discriminator, but I2 asserts only execution/balance, never gizmo position, so it does not discriminate either; S6: tied directly to the G14 finding above) — both recorded honestly rather than forced; S11 corrected the plan's own prediction, recorded above. **With Epic 2.3 closed in code** (2.3.1 camera, 2.3.2 selection, 2.3.3 gizmos, all three landed and macOS-validated-pending-Windows/Linux at worst), **Phase 2's next task is 2.4.1 (Command stack).**

##### Task 2.3.3 — code-review round (five commits after the eight)

**A code review of `feat/2.3.3-imguizmo-transform-gizmos` before merge found the shear finding recorded above was BLOCKING and worse than measured, plus one SHOULD-FIX gap in the behind-camera predicate and two RECORD items. All five closed on the same branch as five further green commits, the original eight untouched.** `engine::decompose()` (`glm_backend.cpp:126`) was re-verified at source, independently: it guards ONLY column length and finiteness plus a determinant-sign flip for handedness — it has NO orthogonality test at all, so it never rejects shear, not "in one scenario" but as a matter of course. The review quantified the real-library consequence: for a `{2,1,1}` parent and a 60° world-space rotate delta, a unit-cube corner lands 1.84 world units off; at `{20,1,1}` and 90°, 21.5 units off; the stored quaternion comes out measurably non-unit (down to `|q| = 0.962`, a 20M-sample fuzz spanning `|q| ∈ [0.541, 1.307]`), which `transform.hpp:35` documents as silently adding scale — with zero WARNs the whole time. **The fix stays entirely inside `editor/src/gizmo.cpp` (zero `engine/` file touched, AC-20 holds), because the engine's own contract puts this responsibility on the caller, not on `decompose()`:** `transform.hpp:38-41` states a sheared matrix "decomposes to nonsense, which is out of contract" — `decompose()` never promised to detect shear; `gizmoWriteFromWorld` is the layer obliged not to feed it out-of-contract input. A new `isSheared()` helper normalises the linear block's three columns and rejects if any pairwise `|cos|` exceeds `GIZMO_ORTHOGONALITY_EPSILON = 1e-4`, called between `local`'s formation and `decompose()`. The threshold is a measured constant, not a guess (full derivation recorded in `gizmo.cpp`'s own comment): every legitimate (non-sheared) construction tested tops out at `8.0e-08` `|cos|` (a rotated + uniformly-scaled parent with a rotated child; an 8-deep chain measures `3.3e-08`; the worst conditioning tried, a `1e3`-scaled parent at a `1e4` offset with a `1e-3`-scaled child, measures `1.7e-08`); the hardest genuine shear case tried, a barely-non-uniform `{1.01,1,1}` parent with a 0.5° delta, measures `1.7e-04` — three-plus orders of magnitude of separation, with every shipped tier-0 case re-evaluated under the guard and no verdict changed. G11 was restored to the plan's original shear construction (parent `scaling({2,1,1})`, child rotated 45° about Z, a world-space rotation delta) and now genuinely returns `NotDecomposable` (measured `maxAbsCos ≈ 0.33`). **AC-10's guarantee — shear refused, one WARN, nothing written — now holds as stated, with no open exception.**

**The re-verification surfaced an honest finding of its own, beyond what the review asked for.** Re-running sabotage S5 (drop `decompose()`'s own failure branch) against the fixed G11 does **not** redden it: `isSheared()` now runs upstream and independently rejects the shear construction before `decompose()` is ever reached, so `decompose()`'s return value stops mattering for this input — S5, as literally specified, is a non-discriminator for G11 post-fix, a fact discovered by re-running it rather than assumed from the review's own prediction that it would "give S5 a real discriminator. **But S5 is not a non-discriminator outright: re-measured against the FULL suite rather than G11 alone, it reddens G14** (`gizmo: huge-but-finite scale`, `gizmo_test.cpp:444`, `CHECK(write.status == NotDecomposable)` reporting `1 == 3`), because a huge-but-finite scale overflows `decompose()`'s internal `length()` and is caught by its own column-length guard — a path `isSheared()` deliberately declines to answer for, returning `false` on any non-finite or degenerate column. So `decompose()`'s failure branch remains load-bearing and tested, and the new guard is separately load-bearing (S13/G11). The first pass through this re-check reported "S5 reddens nothing" from a G11-scoped run; **running the whole suite corrected it** — the general lesson being that a sabotage verdict is only as wide as the test selection it was measured against." What *does* discriminate G11 now is a new seed, S13, dropping `isSheared()` itself — confirmed: `write.status` comes back `Applied` (0) instead of `NotDecomposable` (3), because absent the guard, `decompose()` silently succeeds on the shear input exactly as the original finding measured. S3 (the behind-camera skip removed) was re-checked against the widened predicate below and is unchanged: it sabotages the call site in `viewport_panel.cpp`, not the predicate itself, so widening the predicate gives it no new discriminator.

**The SHOULD-FIX gap: `gizmoOriginBehindCamera`'s `w`-based test and ImGuizmo's own behind-camera test are different quantities.** Ours is `clip.w <= CLIP_W_EPSILON` (via `projectToViewport`); ImGuizmo's own (`ImGuizmo.cpp:2696-2698`) is raw clip-space `z < 0.001f`, no perspective divide, and that early return has no matching `PopClipRect` (the only one is at `:2728` — F5's leak). With this task's real camera defaults (eye at `z=5`, near `0.1`) the two tests disagree across a measured band of roughly **0.0002 to 0.11** world units in front of the eye — confirmed directly with a scratch probe against the compiled `lookAt`/`perspective`/`toVec4` functions, sweeping distances from `0.0002` to `1.0`. In that band our test says "in front" and calls `Manipulate`, which then takes exactly the leaking early return D9 exists to prevent. Fixed by widening the predicate to also reject on ImGuizmo's own raw-`z` test, computed from the same `viewProj * origin` already in hand, spelled `!(clip.z >= 0.001F)` rather than `clip.z < 0.001F` so a non-finite `clip.z` fails CLOSED (the same NaN-safety idiom this codebase uses throughout — a direct `<`/`>=` comparison against NaN is always false, so only the negated form is safe). A new tier-0 subcase (a gizmo origin at `world z = 4.95`, 0.05 units into the band) pins the fix.

**Two RECORD items closed alongside, both test-only.** G7's `Rotate` arm now asserts the rotation was actually *written* (`approxEquals(write.transform.rotation, differentTrs.rotation)`), matching the positive assertion `Translate`/`Scale` already had — AC-2's "changes only its own field" was covered, "changes its own field" was not, for rotate. An unused `<aero/editor/gizmo.hpp>` include was dropped from `imgui_layer_test.cpp` (no symbol from it is used there) — its top-of-file comment, which had claimed SDL3 reaches that TU "purely transitively," was already corrected in the implementation pass when `<SDL3/SDL_filesystem.h>` was added directly for I5, and stays accurate.

**G14's comment was also strengthened, independently re-verified against the vendored GLM source, not merely re-asserted.** The implementation pass's hedge ("this is NOT proof step 6 is dead code... only that THIS construction does not reach it") is replaced with the actual structural argument: translation is a direct read of the input matrix's own column, already finite by the upstream `allFinite` guards; scale is exactly what `decompose()`'s own length guard proves finite before it is ever stored; rotation comes from `glm::quat_cast` of a Mat3 built from normalised columns, and `quat_cast`'s own four candidate terms (`glm/gtc/quaternion.inl:83-86`, `fourXSquaredMinus1` etc.) were checked directly against the vendored header and provably sum to zero for any input (`(m00-m11-m22)+(m11-m00-m22)+(m22-m00-m11)+(m00+m11+m22) == 0`), so the chosen maximum is always `>= 0`, `sqrt(max+1) >= 1` is always well-defined, and the reciprocal can never blow up. A 20M-sample fuzz found zero non-finite outputs, confirming it empirically as well as structurally. Step 6 stays in the code regardless: the unreachability rests on `decompose()`'s *current implementation*, not its published contract, and this same round's `isSheared()` guard changes what reaches it — structural unreachability of one input class is not a promise about every future one.

**Corrected inventory, measured at each of the five new commit boundaries, not assumed.** `aero_editor_shell_test` stays **169** TEST_CASEs (no new ones — every fix landed inside an existing case), with assertions **14542 → 14544** (+2: G7's new rotation check, the new behind-camera band subcase); `aero_editor_imgui_test` unchanged at **24**; `aero_tests` unchanged at **356**; `check-math-boundary.sh` unchanged at **209** scanned (no new files, only edits to existing ones); `ctest -N` unchanged at **94**/**5**/**18** across all three configurations. Local ctest green on both macOS presets at every commit boundary; `AERO_REQUIRE_GPU=1 ctest` green on both presets; all five boundary guards green with no allowlist change; clang-format/clang-tidy clean with **zero new `NOLINT`s**. Thirteen sabotage proofs now stand (S1–S13), all seed-confirmed via `git diff`, all reverted and re-confirmed green.

### Epic 2.4 — Undo/redo

#### Task 2.4.1 — Command stack — OPENS Epic 2.4

**2.4.1 ("Command stack") landed: a `Command`/`CommandStack` backbone with an explicit merge chain and a
128-entry bounded history, `Ctrl+Z`/`Ctrl+Shift+Z` with key repeat and live Edit-menu items, and
`TransformCommand` wrapping 2.3.3's `transform_ops` seam with the Viewport gizmo routed through it, so
one continuous drag is exactly one undo step whose `before` is where the drag began.** Two new PUBLIC,
ImGui-free/entt-free/ImGuizmo-free editor headers (`command_stack.hpp`, `transform_command.hpp`) plus two
sources; one `CommandStack&` reference member on `PanelContext` and one owned `CommandStack` member on
`EditorApp` with two accessors and two request methods (`requestUndo()`/`requestRedo()`, mirroring
`requestLayoutReset()`); two new `ShellUiState` fields, two `ImGui::Shortcut()` chords, a live Edit menu
and one `applyHistoryRequests` call in `shell_ui.cpp`; three `breakMergeChain()` sites and one `push` in
`viewport_panel.cpp`; two new tier-0 test TUs (`command_stack_test.cpp` C1–C18, `transform_command_test.cpp`
T1–T9) and six GPU-gated cases (I1–I6) in `imgui_layer_test.cpp`. **D0/D1 — two scope corrections to
`docs/tasks/phase-2.md`, mirroring 2.3.3's own D0 in the opposite direction.** D0: the deliverable was
written as "`ICommand` do/undo" but `do` is a C++ keyword and no prefixed interface exists anywhere in
this tree (`Panel` is the unprefixed precedent) — the type is `engine::editor::Command`, spelled
`redo()`/`undo()`. D1: `transform_ops.hpp:2-5` and `gizmo.hpp:33-36` were both written *for this task, by
name* in 2.3.3, so 2.4.1 now ships `TransformCommand` and routes the gizmo through it — half of one
2.4.2 bullet ("Transform command wrapping `transform_ops`; gizmo and Inspector edits both route through
it") moves here, becoming 2.4.2's "Inspector edits route through the generic property-set command (the
gizmo already routes through 2.4.1's `TransformCommand`)". Five `docs/tasks/phase-2.md` edits carry this:
`2.4.1`'s `depends:` line (`2.1.3` → `2.1.3, 2.3.3`), its deliverable and subtask list, 2.4.2's bullet, and
Epic 2.3's Definition of Done (the *undoably* clause now completes at **2.4.1**, not 2.4.2 — the word
*undoably* is never dropped, only re-pointed). **What deliberately did not ship, all recorded as
Handoffs, not gaps:** no selection capture/restore (D14 — 2.4.2's, `selection.hpp:11`); no compound/macro
commands and no `CommandContext` aggregate (D22 — a one-field struct today is ceremony this log already
criticises; 2.4.2's multi-entity delete widens `redo`/`undo` to one when `Selection&` is actually needed);
no Inspector routing (D1/E14 — 2.4.2's, the honest one-task cost: drag the cube, type into the Inspector,
`Ctrl+Z` discards the Inspector edit until 2.4.2 lands); no `Ctrl+Y` (D13 — `ImGuiMod_Ctrl` is ⌘ on
macOS, so a global `Ctrl+Y` would bind ⌘Y, redo on no platform); no history panel (a natural 2.6.x
addition — `count()`/`appliedCount()`/`label()` are already the whole model it needs); no memory-budget
eviction and no runtime capacity setter (D8 — a count, not a byte budget, clamped to ≥ 1, fixed at
construction); no `incoming.before() == after()` merge guard in `TransformCommand::mergeWith` (deliberately
absent because it could not fail today — the gizmo reads `before` fresh from the World every frame and
`Transform::operator==` is exact — an assertion that cannot fail is precisely the defect class this log
keeps calling out; the first task that writes a `Transform` from outside the drag loop must add it).

**The traps worth keeping.** (i) **F3** — `IsKeyChordPressed` compares the whole 4-bit mod mask for
*equality* (`imgui.cpp:11386`), so `Ctrl+Z` and `Ctrl+Shift+Z` structurally cannot cross-fire; the
submission order in `drawMenuBar` is documentation, not arbitration. (ii) **F5** — a focused `InputText`
outranks a global route for exactly these three chords: `InputTextEx` binds `Ctrl+Z`/`Ctrl+Y`/
`Ctrl+Shift+Z` on the active item's own id with the default `RouteFocused` (`imgui_widgets.cpp:5130-5131`),
and plain `RouteGlobal` — what this task and F2's pre-existing `Ctrl+Q` both use — is **last** in the
documented priority list (`imgui.h:1758`); using `ctx.input()` instead of `ImGui::Shortcut` would make the
scene jump while renaming (human row 9, S14's only discriminator). (iii) **§A5** — `AERO_LOG_DEBUG` is
compiled out under `NDEBUG` (`log.hpp:137-143`), so any test asserting `records.size()` would be
Debug/Release-divergent (CI runs Release on all three lanes); every log assertion in this task counts
records **by level** via a file-local `countAtLevel` helper in each of the two new test TUs. (iv)
**F9/INV-3** — `updateGizmo`'s two early returns clear `gizmoWasUsing` *without computing an edge*, so
breaking the merge chain has to be a property of every latch-clearing site, not of the `End` edge alone —
both early returns and the `Begin`/`End` edge block all call `breakMergeChain()` (three sites total,
confirmed by a comment-stripped `grep -c`). (v) **the console's pump-before-draw ordering** (2.2.5 D14)
bit two of this task's own GPU-gated cases during the Step 7 verification pass, not during Step 6's own
gate: `EditorApp::tick()` pumps the log sink at the TOP of the frame, before `drawShellUi` runs, so a WARN
or INFO record raised *during* that same tick's draw (a Viewport's tools-OFF WARN on its first draw; a
`CommandStack` WARN from an undo applied inside `drawShellUi`) is not visible through `logRecordCount()`
until the *following* tick's pump — I5 needed a second settling tick before capturing its baseline
(`-DAERO_SHADER_TOOLS=OFF` adds a one-frame-delayed WARN the tools-ON configuration does not have) and I6
needed a second tick after `requestUndo()` before its delta assertion; both fixes are one-tick additions,
found by running §V5's tools-OFF configuration against Step 6's already-committed cases, and both are
folded into the Step 6 commit rather than shipped as a Step 7 fixup (Step 7 carries no commit of its own).

**§A's nine corrections, each confirmed at the tree, not assumed.** **F16 was wrong** — `PanelContext` has
**five** construction sites, four in `tests/` (`editor_app.cpp:147`, `shell_test.cpp` ×3,
`hierarchy_test.cpp` ×1), not the one the spec claimed, so D7's reference member cost five lines, not one;
`shell_test.cpp:246`'s case (which existed to prove `{world, selection}` stayed valid) was **rewritten,
never deleted** — its title and both `CHECK`s survive, proving `deltaSeconds` is the ONE defaulted member
at the new three-brace arity. **`editor/src/imgui_layer.hpp` does not exist** — the header is PUBLIC, at
`editor/include/aero/editor/imgui_layer.hpp`; AC-24's frozen-file grep against the spec's own path is
half-vacuous on any tree, and 2.3.3's own log entry carried the same path error forward without noticing.
**The streak arithmetic was wrong in both directions** — measured with `git log -1`: `imgui_layer.{hpp,cpp}`
last changed at task **2.1.1** (a ten-task streak, this the eleventh), `main.cpp` at **2.2.4** (a
four-task streak, this the fifth), `editor_app.cpp` at **2.3.1** (a two-task streak — **this task breaks
it, deliberately**, wiring the command stack through it). This plan asserts byte-identity only, never a
streak count. **C14's second half was impossible against the spec's own §3.2 algorithm** — after
`CommandStack{2}` + push A + `setClean()` + push B + push C + two undos, a `push(D)` truncates the
(already-empty) redo branch first, so `trimToCapacity` never runs and nothing becomes unreachable; the
correct two-arm construction (Arm 1: the clean position survives one capacity shift and stays reachable;
Arm 2: a second shift evicts it and `cleanPosition` becomes permanently `nullopt`) is what actually
discriminates S8. **A genuine tenth finding, beyond the plan's own nine — but, corrected below, one this
implementation pass identified and then failed to actually land.** The plan's own C7 "undo" arm, as
literally specified (push a single entry, undo it, push a follow-up, assert no merge), cannot discriminate
S2 (a bugged `undo()` that forgets `mergeOpen = false`) — undoing a stack's SOLE entry drops `applied` to
0, and the merge guard's own `applied > 0` term then masks `mergeOpen`'s value regardless of whether
`undo()` reset it, since `push`'s step 3 (truncate) removes that entry before the merge check is ever
reached. Found by actually running the seed and watching the whole suite stay green when the plan
predicted red. **The described fix — a two-entry construction (push A, break the chain, push B, then undo
B, leaving `applied == 1` with A still applied) — was correctly diagnosed here but never actually written
into `tests/editor/command_stack_test.cpp`: the shipped C7 "undo" arm at merge time was byte-identical to
the plan's own single-entry construction, and S2 shipped completely uncovered.** A first-pass code review
of `feat/2.4.1-command-stack` caught this directly (`git show <the seven-commit HEAD>:tests/editor/
command_stack_test.cpp`'s "undo" arm was unchanged from the single-entry shape this very paragraph already
diagnoses as non-discriminating), confirmed the seed landed green against the whole suite, and only then
rebuilt the arm to the two-entry construction described above — now genuinely discriminating (see the
review round below). This is the same class of defect §A4 already found in the spec's own C14 — a plan-
or spec-authored test construction that cannot reach the state it claims to — compounded, this time, by
the fix being described accurately in prose without the corresponding code change actually landing. The
standing lesson: a paragraph that says "fixed" is not evidence: `git diff`/`git show` the tree, always.

**The dead ends recorded so they are never retried:** `ctx.input()` for an editor chord — no notion of UI
focus, so a focused `InputText` could not swallow `Ctrl+Z` (F5); a global `Ctrl+Y` as a second redo — binds
⌘Y on macOS, redo on no platform (D13); recording-after-the-fact (`push` does not apply) — two write paths
forever, a failed first application unrepresentable; pushing once at the drag `End` — a drag abandoned
through either of `updateGizmo`'s early returns records nothing while the World has already changed
(F9); time-window merging — needs a clock in a class that has none and is wrong at both ends; a `World&`
member on `CommandStack` — deletes `EditorApp`'s defaulted move assignment (F15) and would let a stack
straddle a scene swap, which INV-6 exists to make impossible; a nullable `CommandStack*` in `PanelContext`
— a null branch at every present and future call site, plus a silent "the edit was not recorded" failure
mode; applying undo in `tick()` after `drawShellUi` returns — that frame's panels would render pre-undo
state while `renderScene` renders post-undo, an inconsistency *within* one frame; Qt's integer `id()` tag
gating the merge — one `dynamic_cast` per push is not worth a second virtual plus a tag to keep in sync;
comparing a `&incoming` pointer saved during `mergeWith` after the merge destroyed it — record
`incoming.label()` instead, a string literal with static storage.

**Inventory, measured at every commit boundary, never predicted.** `ctest -N` **94 → 94** (tools ON),
**5 → 5** (both tools OFF) and **18 → 18** (reflect OFF alone) throughout — **no new `add_test`** anywhere;
`aero_editor_shell_test` **169 → 187** (Step 1: `command_stack_test.cpp`, C1–C18) **→ 196** (Step 2:
`transform_command_test.cpp`, T1–T9), unchanged through Steps 3–6; `aero_editor_imgui_test` **24 → 30**
(I1–I6); `aero_tests` unchanged at **356**; `check-math-boundary.sh`'s scanned count **209 → 215** (+6:
two headers, two sources, two test TUs — measured against `origin/main` in a disposable `git worktree` at
Step 0, and directly on the finished tree at Step 7; both agree); `aero_editor_core` **25 → 27** sources
with a byte-identical link line (`command_stack.cpp` needs only `aero::core`, already PUBLIC;
`transform_command.cpp` needs only `aero::core`/`aero::scene`, already PUBLIC); `aero_editor_shell_test`'s
and `aero_editor_imgui_test`'s link lines byte-identical; `editor/include/aero/editor/imgui_layer.hpp`,
`editor/src/imgui_layer.cpp` and `editor/src/main.cpp` byte-identical for the **eleventh** task running
(measured last-change commits above, no streak claim per §A3); `editor_app.cpp` changes deliberately,
ending its own two-task streak; `ViewportPanel::renderScene` byte-identical (`git diff origin/main`, no
hunk at or after its signature); zero new `NOLINT`s. Both macOS presets green at every one of the six
commit boundaries, Debug and Release, with and without `AERO_REQUIRE_GPU=1`; both tools-OFF configurations
green (`build/tools-off-2.4.1` 5/5 with exactly the two known pre-existing WARNs and no third;
`build/reflect-off-2.4.1` 18/18); all five architecture guards green with no allowlist change; this is the
**first `dynamic_cast` in the whole tree** (`transform_command.cpp`'s merge downcast), and RTTI was
confirmed on on all three lanes by the absence of any `-fno-rtti`/`/GR-` flag anywhere in `cmake/` or the
presets. **All fourteen sabotage proofs were performed**, each seed confirmed landed via `git diff` before
trusting a verdict, each reverted and re-confirmed green afterward, every verdict measured against the
WHOLE suite, never a filtered case: **S1, S3, S4, S6, S8, S10, S12 discriminate exactly as predicted.**
**S2 was CLAIMED to discriminate "after the C7 undo arm fix above", and that claim was false at the time
it was written** — the fix described in the paragraph above was never actually made to the tree in this
implementation pass, so S2 shipped on `feat/2.4.1-command-stack`'s first seven commits with ZERO coverage,
undetected until a code review re-read the tree directly instead of trusting the log's own prose. Actually
fixed in the review round below, and reconfirmed reddening there. **S7 was predicted to discriminate via
TWO cases, "C7's setClean arm, C13" — only the first half is true.** C13 ("clean tracking") calls its own
explicit `breakMergeChain()` immediately before the push that would expose a stale `mergeOpen`, so C13
passes identically whether or not `setClean()` resets the chain; only C7's `setClean` SUBCASE (which pushes
its follow-up command WITHOUT an intervening `breakMergeChain()`) actually discriminates S7. Found and
corrected by the same review round, below; **S5's second-order check found MORE redundant coverage than the plan
predicted** — weakening only the two assertions the plan named (`log.redoCalls == 1` in C2, the three
`readTransform` equalities in T8) does *not* make the seed pass silently, because C3's `redoCalls == 2`,
C5's `redoCalls == 1` and C10's whole failed-push case each independently catch it too; recorded honestly
as a stronger result than predicted rather than mechanically weakened further to force a silent pass;
**S9's second-order check matched the plan's prediction exactly** — weakening T4's `a.before() == p0` and
T8's post-undo `readTransform` equality does make the whole suite pass silently with the seed live,
confirming those two assertions (not the harness) do the work; **S11 discriminates via an ASan
stack-out-of-bounds abort**, not merely a red assertion — `static_cast`ing an `OtherCommand` through a
`TransformCommand*` and reading its wider layout is UB the sanitizer catches directly, a stronger result
than the plan's own "expect a compiler diagnostic or a red test"; **S13's verdict is corrected by the
review round below — it was CLOSER TO CORRECT than the code it was seeded against, not merely
non-discriminating for an unrelated structural reason.** S14 (`RouteGlobal` → `RouteAlways`) remains
confirmed NON-DISCRIMINATING exactly as predicted — `aero_editor_imgui_test` is ImGui-free at source and
cannot press a key; it is routed to human row 9, and was not forced into a manufactured tier-0
discriminator. Three prose-collision fixes, the `each<T>` class
2.3.2 first found: a `viewport_panel.cpp` comment originally said "the direct `writeTransform` call" and
inflated AC-18's `git grep -n 'writeTransform'` past its expected two files; a `shell_ui.cpp` comment named
`menuItemStub` inside the Edit-menu block and inflated AC-20's five-line expectation to six; a
`tests/CMakeLists.txt` comment paragraph said "target_link_libraries below is UNCHANGED" and matched
AC-25's own "no new link line" grep; and a `transform_command_test.cpp` comment said "T6's `dynamic_cast`
arm" and inflated D9/A20's expected single hit to two. All four reworded to describe the same fact without
the literal token, confirmed back to the expected count, folded into the commit that introduced each
(Steps 1, 2, 4, 5 respectively — the CMakeLists fix folded into Step 1's commit via the same disposable-
worktree-and-fold path as the C7 fix). One measured drift from the plan's own §G9 baseline table, recorded
rather than silently corrected: INV-4's uncomment-stripped identifier scan reads **35 hits across 8
headers** at `bb6de90`, not the plan's stated "34 hits across 9 headers" — the comment-stripped form
(the actual gate) is unaffected and was empty before and after this task, so nothing enforceable moved.

##### Task 2.4.1 — code-review round (six commits after the seven)

**A code review of `feat/2.4.1-command-stack` found six issues, two of them blocking, all six fixed on
the same branch as six further green `fix:`/`test:`/`docs:` commits, the original seven untouched. Every
sabotage verdict this round moved was re-run against the fixed tree, seed confirmed landed via `git diff`
before trusting the result, reverted and re-confirmed green afterward.**

**Gap 1 — BLOCKING — a released drag recorded TWO history entries, so AC-16 was not met.**
`ViewportPanel::updateGizmo`'s merge-chain break ran on `edge == Begin || edge == End`, in ONE place,
*before* that frame's write-back. On the genuine release frame, ImGuizmo reports the drag's FINAL delta
on the SAME frame it clears its own `mbUsing` latch (translate: `ImGuizmo.cpp` ~:2244-2249 — `*(matrix_t*)
matrix = res;` runs, THEN `if (!io.MouseDown[0]) { mbUsing = false; }`; scale/rotate are the same shape),
so `edge == End` and a real `changed == true` land on the SAME frame. Breaking the chain first meant that
frame's push recorded as a SECOND, un-merged entry beside the drag's already-merged one: `⌘Z` only undid
the last frame's motion, not the whole gesture. **Fixed by splitting the single check into two**: the
`Begin` break stays exactly where it was (before the write-back — necessary because ImGuizmo's Scale and
Rotate handlers, unlike Translate, run their "just grabbed" and "apply this frame's delta" logic back to
back on the SAME frame, so a stale `mScaleLast`/`mRotationAngleOrigin` comparison left over from an
EARLIER, unrelated drag can report `changed` on the very Begin frame — confirmed by reading
`HandleScale`/`HandleRotation`, not assumed); the `End` break now runs AFTER the write-back, reached on
every exit path (the old `if (!changed) { return; }` early return became an `if (changed) { … }` block so
the trailing close is never skipped). **This raises the file's comment-stripped `breakMergeChain` count
from 3 to 4** (two early-return sites, unchanged, plus Begin and End as two now-separate call sites) —
a deliberate, evidence-based deviation from the plan's own §V7 expectation of 3. The two boundaries have
structurally opposite positional requirements (Begin must run before ITS OWN frame's push; End must run
after ITS OWN frame's push) and cannot share one call site without reintroducing either this bug (moving
Begin's break late) or a second, symmetric one (moving End's break early); collapsing the two early-return
sites was considered and rejected — they exist for the same INV-3 defence-in-depth reason the End close
does, and nothing in this round asked to weaken them. **New regression coverage at the policy level**
(the panel itself is unreachable from any test target — src-private, ImGui-bound): a new
`command_stack_test.cpp` case drives the exact call sequence a release frame produces two ways — chain
closed AFTER the release frame's push (`count() == 1`, correct) contrasted with chain closed BEFORE it
(`count() == 2`, documents the bug this round fixed). **S13 re-run against the fixed code and given a
corrected verdict**: the plan's own sabotage table called S13 (dropping the `End` arm entirely) "confirmed
NON-DISCRIMINATING, exactly as predicted" and attributed that to the panel being unreachable from any test
target. That framing hid the real finding — **S13, seeded against the ORIGINAL (buggy) code, was CLOSER
TO CORRECT than the code it was tested against**: removing the premature `End` close also removes the bug
this round fixes, so the seeded tree would have produced ONE entry per drag, correctly, for the wrong
reason. Re-run against the FIXED code, S13 (dropping the now-correctly-timed post-write `End` close
entirely) is confirmed non-discriminating for a DIFFERENT and more permanent reason: `Begin`'s own break is
unconditional and does not depend on whatever `End` left behind, so for the ONLY command producer this
task wires up (the gizmo itself), any sequence of ordinary drags produces one entry each regardless of
whether the trailing `End` close exists at all — `aero_editor_shell_test`/`aero_editor_imgui_test` stayed
94/94 (whole tests, not filtered) with the seed live, confirmed. This also means human row 5 ("three
separate drags, then three undos") was never actually S13's discriminator, contrary to `editor/
VALIDATION.md`'s row-5 parenthetical — corrected there. The `End` close remains in the code purely as
INV-3's stated defence-in-depth, for the day a second command producer (2.4.2's Inspector routing, H6's
second continuous-gesture producer) can push while a gizmo drag's chain is nominally still open.

**Gap 2 — BLOCKING — S2 had zero coverage, and two documents claimed otherwise.** Covered above at length
(the "genuine tenth finding" paragraph, corrected in place, and the sabotage-verdict correction two
paragraphs above it): the plan/log's own C7 "undo" arm fix was DESCRIBED but never WRITTEN, so S2 shipped
uncovered on the original seven commits. **Fixed now**: C7's "undo" SUBCASE rebuilt to the two-entry
construction (push A with `mergeResult = true`, `breakMergeChain()`, push B, `undo()`, push C) —
`logA.mergeCalls == 0` and `count() == 2` now hold only because `mergeOpen` is genuinely reset by
`undo()`; seeded and confirmed reddening (`logA.mergeCalls == 1`, `count() == 1` with S2 live), reverted
and re-confirmed green. **The S7/C13 misattribution, a second false claim in the same family, found and
corrected in the same pass**: C13 ("clean tracking") calls its own explicit `breakMergeChain()`
immediately before the push that would expose S7's bug, so it passes identically whether or not
`setClean()` resets `mergeOpen` — confirmed by seeding S7 and watching C13 stay green in isolation while
only C7's `setClean` SUBCASE reddens. **Two related seeds measured and deliberately left alone, per the
review's own finding**: deleting `mergeOpen = false` from `redo()` or from `clear()` both leave the whole
suite green, and correctly so — `redo()`'s own `applied == history.size()` guard and `clear()`'s own
`applied > 0` term already make the stale flag unobservable in both cases; adding assertions that cannot
fail would be exactly the defect class this log keeps calling out, so neither arm was touched.
`editor/VALIDATION.md`'s S2 and S7 rows, and this file's own sabotage-summary sentence above, are
corrected to match.

**Gap 3 — AC-5's `redo()` half was completely untested.** `command_stack.cpp:62-77`'s "consume the step
even on failure" (D20) arm existed on both `undo()` and `redo()`, but C11 only ever drove `undo()` with a
failing command; nothing exercised `redo()` returning false. Proven dead before the fix: seeding
`if (ok) { ++applied; }` into `redo()` left the whole 196-case suite green — a frozen `Ctrl+Shift+Z` (the
exact failure D20 exists to prevent) would have shipped undetected. **New case, the mirror of C11**: push a
`FakeCommand`, `undo()`, set `redoResult = false`, `redo()` — asserts `true` returned, `appliedCount()`
back to 1, exactly one WARN. Seeded and confirmed reddening, reverted and re-confirmed green.

**Gap 4 — T3 did not assert the half of AC-13 it was credited with.** `transform_command_test.cpp`'s
original T3 ("no Transform component") had no `LogFixture`, no `LogSinkScope` and no ERROR assertion — the
"no `AERO_LOG_ERROR`" half of AC-13 was proven only by T2's dead-entity arm, and `editor/VALIDATION.md`'s
own S10 row over-claimed T3 as a second discriminator. **Verified, not assumed**: the ORIGINAL T3 body was
checked out standalone and run against a live S10 seed (`TransformCommand::write`'s `readTransform` guard
removed) — it passed unchanged, because `writeTransform` also returns `false` for a missing component, it
just ALSO emits the `AERO_LOG_ERROR` the guard exists to avoid, and the original T3 never checked for one.
**Fixed**: T3 wrapped in the same `LogFixture`/`LogSinkScope` as T2, with a `countAtLevel(records, Error)
== 0` assertion, plus a genuinely new arm for the null-`Entity{}` case of AC-13 that had no coverage at
all anywhere in the TU. Re-seeded S10 against the fixed T3: both SUBCASEs now redden. `editor/
VALIDATION.md`'s S10 row corrected to remove T3 from the pre-fix discriminator list and note the fix.

**Gap 5 — a moved-from `CommandStack` broke INV-1.** `command_stack.hpp`'s defaulted move moved `history`
(left empty by `std::vector`'s own move) but COPIED the scalars `applied`/`cleanPosition`/`mergeOpen`
(trivially-copyable types have no distinct "move"), so a moved-from stack could have `applied > 0` over an
empty `history`: `canUndo()` would lie true, and `undoLabel()`/`undo()` would index `history[applied - 1]`
on an empty vector. Latent in the shipped code — `EditorApp`'s one move (inside `create()`) only ever
destroys the source afterward — but exactly the shape 2.5.1 trips the moment it clears/replaces stacks.
**Fixed**: the move constructor and move assignment are now hand-written (still `noexcept`, so the two
`static_assert`s and `EditorApp`'s own defaulted move stay valid), resetting the source's three scalars to
`clear()`'s state. New tier-0 case (move construction and move assignment, each a SUBCASE) asserts a
moved-from stack is empty, clean and `!canUndo()`, using the `std::optional<T>`-wrapped-source idiom
(`shell_test.cpp`'s `PanelRegistry` precedent) so `bugprone-use-after-move` does not flag the deliberate
moved-from-state read-back.

**Gap 6 — the `label()` contract permitted the mutation the stack's `string_view` capture cannot
survive.** The original contract allowed a `std::string` a command owns, and `undo()`/`redo()`'s own
"capture the label before the call, a command may mutate itself" comment invited exactly the mutation that
would dangle a `string_view` held across the call. **Tightened, not redesigned**: `Command::label()`'s
contract in `command_stack.hpp` now states explicitly that the view must stay valid AND UNCHANGED across
that command's own `redo()`, `undo()` and `mergeWith()`, and that callers must not hold it across a
`push()`/`undo()`/`redo()`/`clear()` on the owning stack. `undo()`/`redo()`'s capture-before-the-call
comments were rewritten to say the contract makes this safe, not that a mutation is expected. **Handoff to
2.4.2, recorded in the header**: a future command with a genuinely mutable label must have its caller copy
it into a `std::string` before the call that could change it, not hold a `string_view` across it — nothing
in this task needs that (`TRANSFORM_COMMAND_LABEL` is a `constexpr` literal with static storage), so no
behavioural change was made, only the documented contract and the misleading comments.

**Process note, stated plainly because it is the standing lesson of this round.** The prior pass of this
task amended its seven commits and reported a fix ("C7's undo arm rebuilt as a two-entry construction")
that was never actually in the tree — this file and `editor/VALIDATION.md` both repeated that false claim.
This round adds new commits on top instead of amending, and every claim above was verified by reading the
diff/`git show` back out of the tree after writing it, not by re-describing what the previous pass said it
did.

**Inventory, measured before and after this round, never assumed.** `aero_editor_shell_test` **196 → 199**
(three new cases: the redo-failure mirror of C11, the moved-from-stack case, and the release-frame-ordering
policy case; C7's own arm fix and T3's rewrite both add `SUBCASE`s, not `TEST_CASE`s, so neither moves the
count). `aero_editor_imgui_test` and `aero_tests` unchanged at **30** and **356**. `ctest -N` unchanged at
**94** (tools ON), **5** (`build/tools-off-2.4.1`) and **18** (`build/reflect-off-2.4.1`) — no new
`add_test` anywhere, both new/changed sources ride existing targets. `check-math-boundary.sh`'s scanned
count unchanged at **215** — no new tracked C-family file. All five architecture guards green with no
allowlist change. Both macOS presets green at every one of the six new commit boundaries, Debug and
Release, with and without `AERO_REQUIRE_GPU=1`; both tools-OFF configurations green. clang-format and
clang-tidy (`--warnings-as-errors='*'`) clean on every touched file, zero new `NOLINT`s. Sabotage seeds
re-run this round — S2, S6, S7, S10, S13, plus the Gap-3 `redo()` seed — each confirmed landed via `git
diff`, measured against the WHOLE suite, reverted and re-confirmed green: **S2 now reddens** (C7's fixed
"undo" arm); **S6 still reddens exactly as before** (C11, unaffected by this round's changes); **S7 still
reddens, but ONLY via C7's `setClean` arm**, confirmed by watching C13 pass in isolation with the seed
live; **S10 now reddens T3 too**, both its live-entity and its new null-`Entity{}` SUBCASEs, where before
only T2 caught it; **S13 remains non-discriminating against the fixed code, for a different and more
durable reason than the plan recorded** (`Begin`'s own break is unconditional and independently guarantees
one entry per drag for the only producer this task wires up); the new Gap-3 seed (`redo()`'s `++applied`
guarded behind `if (ok)`) now reddens the new C11-mirror case where before nothing in the suite did.

#### Task 2.4.2 — Property-set + structural commands — CLOSES Epic 2.4

**2.4.2 ("Property-set + structural commands") removes the last seventeen direct scene writes under
`editor/src/` and makes the epic's own stated goal true: nothing mutates the scene except the three `_ops`
TUs and the three command TUs.** It ships in nine commits. One engine primitive,
`[[nodiscard]] Entity World::recreate(Entity)` (`engine/scene/include/aero/scene/world.hpp`,
`engine/scene/src/world.cpp`) — five pre-checks (moved-from World, never-issued index, occupied index, plus
a trailing `made != entity` belt) each logging exactly one `AERO_LOG_ERROR` before anything is created,
never after. `RootOrder` gains `indexOf`/`insert` and moves from `HierarchyPanel` to `EditorApp` (member
`rootOrder`, accessor `roots()`), which is what lets `CommandContext` — a three-reference aggregate
(`World&`, `Selection&`, `RootOrder&`) — widen `Command`/`CommandStack`'s `redo`/`undo`/`push` signatures in
a pure, behaviour-preserving refactor with its own commit, kept separate from the ownership move
specifically so a later `git bisect` can tell a signature change from a behaviour change apart. Two new
PUBLIC, pimpl-based value types, `SubtreeSnapshot` and `ComponentSnapshot` (`scene_snapshot.{hpp,cpp}`),
whose private store is a `World` the class owns outright — the only thing in the tree that can hold a typed
value behind a runtime `ComponentTypeId` without reflection, which is what makes structural undo survive
`-DAERO_REFLECT_TOOLS=OFF` (AC-8, proven by `scene_snapshot_test`/`structural_commands_test` both running
green in `build/reflect-off-2.4.2`, 18/18). `component_commands.{hpp,cpp}` adds `SetFieldCommand`,
`AddComponentCommand` and `RemoveComponentCommand` wrapping 2.2.2's `component_ops` seam, written for this
task by name since that task shipped. `entity_commands.{hpp,cpp}` adds `StructuralUndoState` (a
`SubtreeSnapshot` plus captured `RootOrder` slots) and five commands — `CreateEntityCommand`,
`DeleteEntitiesCommand`, `DuplicateEntitiesCommand`, `ReparentCommand`, `RenameEntityCommand` — all wrapping
2.2.1's `entity_ops` seam, one mechanism (`captureAndDestroy`/`restoreState`) in three orientations (D21).
The Inspector's seven `writeComponentField` arms and its `addComponent`/`removeComponent` pair now route
through `SetFieldCommand`/`AddComponentCommand`/`RemoveComponentCommand`, each wrapped in the D17 gate pair
(`gate.opened` breaks the merge chain *before* that frame's push, `gate.closed` breaks it *after* — fourteen
call sites, `inspector_panel.cpp`, measured by comment-stripped `grep -c breakMergeChain`). The Hierarchy's
six mutating arms (`createEntity` ×2, `destroyEntities` ×2, `duplicateEntities` ×2, `world.setName`,
`reparentEntity`) now push the matching command; its selection-only arms are untouched. Five new GPU-gated
`imgui_layer_test.cpp` cases (I7–I11) drive Create/Delete/Duplicate/Reparent/Rename and the `EditorApp`
noexcept-move guarantee through real `EditorApp::tick()` frames. **The canonical sequence spec §0-D2 exists
to make work — drag the Cube with the gizmo, delete it, `⌘Z` (it returns), `⌘Z` again (the move undoes too)
— is now correct**: without `World::recreate` the first `⌘Z` would restore the Cube under a different
handle and the second `⌘Z` would do nothing at all.

**What deliberately did not ship, all recorded as Handoffs (spec §7), not gaps.** No selection-only
commands (D11 — a selection change alone is not undoable, by design). No compound/macro command and no
transaction object (D26). No clipboard and no prefabs (D27) — H2 records that `SubtreeSnapshot` **is** the
future clipboard: cut/copy/paste is `SubtreeSnapshot` plus a paste that allocates fresh handles instead of
restoring them, and the eventual API is a `restoreAsNew(World&)` beside `restore(World&)`, not a
parameterised existing one. No history panel and no per-command memory budget (D28 — `count()`,
`appliedCount()` and `label()` are already the whole model one needs, H7). No sibling-index engine API
(A5) — `placeAt`/`childIndexOf` stay editor-side in `scene_snapshot.{hpp,cpp}`, and H6 reserves promoting
them into `entity_ops` for the day a *third*, non-command caller (Hierarchy drag-to-reorder) needs them.
**No `TransformCommand` in the Inspector (A8/H5) — and this corrects a claim `docs/tasks/phase-2.md` used
to make.** 2.4.1's own H2 asked this task to decide whether the Inspector's Transform fields would route
through the gizmo's `TransformCommand` or through the new generic `SetFieldCommand`; the answer is
`SetFieldCommand`, with zero special-casing for `Transform` — it is one component among five, indistinguishable
from `Camera` or `MeshRenderer` at the seam. `TransformCommand` stays gizmo-only, permanently, and
`docs/tasks/phase-2.md`'s old "gizmo and Inspector edits both route through `TransformCommand`" framing (a
2.3.3-era phrasing already partly walked back at 2.4.1) is now fully corrected rather than implemented. No
`Selection&` widening beyond `CommandContext`'s own three references (2.4.1's own D22 predicted this
exactly). H4 records that a *third* continuous-gesture producer (a curve editor, a timeline scrub) should
promote the two-line open/close idiom to a small RAII scope rather than copying it a third time. H1 and H3
are handed to 2.5.1 and to Phase 4/5's project-defined components respectively — see the findings below,
both of which sharpen a handoff into a currently-measured gap rather than a future one.

**The traps worth keeping.** (i) **D17's asymmetry, now in seven arms at once, not one.** 2.4.1 shipped
this exact defect (a release frame's final delta recorded as a second, un-merged history entry) in ONE call
site and it took a human row to catch. Step 7 introduces the same open/close decision seven times over — one
per `FieldKind` arm — and gets it right in all seven, structurally, by factoring the pair into one place in
each arm's own two-line shape rather than trusting seven independent authorings. (ii) **§A2 — the LIFO push
order in `SubtreeSnapshot::capture`'s subtree walk.** The spec's own prose ("pushing children in `eachChild`
order") is backwards for a LIFO stack — copied verbatim it would ship reversed sibling order on every
restore. `duplicateEntities`'s own precedent (`entity_ops.cpp:174-178`, pushing back-to-front so a LIFO
stack pops them in attach order) was copied instead of re-derived, and the corrected sabotage seed (S5b,
push children front-to-back) is what actually discriminates the defect the spec's literal wording would
have shipped. (iii) **§A5 — `component_ops::addComponent` is not silent on refusal.** The spec's §3.6 says
the stack "WARNs, nothing is recorded" for `AddComponentCommand::redo` on an already-present type; the true
accounting is one ERROR from `component_ops` (`component_ops.cpp:142-146`) *plus* one WARN from the stack —
D15's "silent" rule belongs to `SetFieldCommand` alone and does not generalise. (iv) **§G4 — `registry.clear()`
does not un-issue entity indices.** `World::clear()` leaves every previously-issued index "issued", so
`recreate` of a pre-`clear()` handle *succeeds* — not a bug, but it makes INV-6 ("2.5.1 must `clear()` the
`CommandStack` in the same operation that replaces the World") a *policy* enforced by discipline, not a
mechanism the type system defends; recorded in `CLAUDE.md`'s 2.5.1 pointer so it is not rediscovered as a
surprise. (v) **§A12 — `SubtreeSnapshot::clear()`/`ComponentSnapshot::clear()` are `noexcept`, so they
`impl.reset()`, never rebuild the store.** Constructing a fresh `World` allocates and can throw; `clear()`
drops the previous store wholesale (bounding memory exactly — a command's second undo never leaves a
`World::clear()`ed husk whose entity storage keeps growing) and `capture()` lazily
`std::make_unique<Impl>()`s. The consequences (`empty()` true on a null `impl`, `restore()` on a null `impl`
returning `true` — "nothing to restore" is a legal outcome, N8) are documented in the header rather than
left to be rediscovered by a reader.

**The dead ends never to retry**, spec §4's A1–A8, one line each: **A1**, an entity-remap virtual on
`Command` plus a whole-history rewrite — viral and permanent, touching every future command and every
cached handle (`Selection`, `rangeAnchor`, `renaming`, the gizmo target, the picking result, `RootOrder`)
forever, rejected in favour of one primitive that makes the handle *not change*. **A2**, keep a deleted
entity alive but hidden — doubles every query's filter forever for a feature only undo needs. **A3**,
`entt::meta_any` payloads for the snapshot — silently loses every payload under
`-DAERO_REFLECT_TOOLS=OFF`, which `SubtreeSnapshot`'s reflection-free `addRaw`/`getRaw` route (F7) does not.
**A4**, serialize the subtree to a full `SceneDocument` on delete — Phase-1 serialization is a superset of
what structural undo needs and round-trips through disk formatting for an in-memory operation. **A5**, a
`setParent(child, parent, index)` engine API — `world.hpp:157-163` deliberately refuses to make sibling
indices a public concept; `placeAt` stays editor-side instead (H6). **A6**, a pointer-carrying
`CommandContext` — reintroduces the dangling-reference class `command_stack.hpp` already warns about
(INV-2). **A7**, one command per selected entity for multi-select delete/duplicate/reparent — loses the
"one gesture, one undo step" property AC-25 requires and multiplies push sites. **A8**, routing the
Inspector's Transform fields through `TransformCommand` — superseded by H5's decision, above.

**§A's corrections, each confirmed at the tree, never assumed — the spec was wrong in nine places, and
this is what was actually true.** **A1**: the spec's own narrative ordered `CommandContext` before
`RootOrder`'s ownership move, which cannot compile — `CommandContext` has a `RootOrder&` member and
`toCommandContext` needs `PanelContext::roots`, neither of which exists until the move lands; the plan
reordered the two steps into separate commits. **A2**: covered above as a trap. **A3**: the spec's own S5
seed ("swap the two phase-B loops") discriminates nothing — both loops already run after phase A, so the
swap is a no-op; the corrected seed (hoist the *link* loop above phase A's *recreate* loop) is what actually
reddens N2. **A4**: the spec's §6.6 INV-7 grep pattern (`Im[A-Z]`) matches the editor's own `ImGuiLayer`
class and reads 2 hits on the untouched tree — not a gate; 2.4.1's corrected pattern (comment-stripped,
empty at HEAD, 10-of-20-headers non-vacuous unstripped) was reused instead. **A5**: covered above as a
trap. **A6**: `placeAt` cannot be a TU-local helper in `scene_snapshot.cpp` as the spec's §3.4 places it,
because §3.7 calls it from `entity_commands.cpp` — a second TU; it and `childIndexOf` are declared PUBLIC
in `scene_snapshot.hpp` instead of duplicated. **A7**: `aero_editor_inspector_test` is single-TU with
`main()` inside its only TU (`inspector_test.cpp`); the new `field_command_test.cpp` must not (and does
not) define `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` a second time. **A8**: every `PanelContext` construction
line number the spec cited had drifted — measured fresh (five sites, `editor_app.cpp:156`,
`shell_test.cpp:254,257,380`, `hierarchy_test.cpp:308`) rather than trusted. **A9**: a bare `git grep -n
'recreate'` is polluted by prose in three unrelated files (`renderer.hpp`, `render_cube_test.cpp`,
`inspector_test.cpp`); every `recreate` grep in this task uses the call form `recreate\(`, scoped to
`engine/scene` or `editor/src`.

**Five findings that must be recorded honestly — measured facts, not paragraphs that say "fixed."**

1. **⚠ AC-10 is NOT satisfied, and no test in this tree can prove it is.** None of the five built-in
   components (`Transform`, `Camera`, `DirectionalLight`, `PointLight`, `MeshRenderer`) is an empty
   (tag) type, and `scene::internal::registerComponent` can only ever reach the *source* World —
   `SubtreeSnapshot`'s private store World is built inside its own pimpl and only ever runs
   `registerBuiltinComponents`. A test-defined tag component is therefore captured but never mirrored
   into that private store, and hits D24's WARN-and-drop path every time. `scene_snapshot_test.cpp`'s
   N6 and N11 cases and `structural_commands_test.cpp`'s X16(b) were written (not "fixed after the
   fact") to assert the *real* behaviour — a tag degrades to **absent**, with exactly one WARN, a
   sibling without the tag unaffected — rather than the round-trip AC-10 literally promises. This is an
   **honest gap**, reachable only when H3 lands (project-defined components registering into the
   snapshot's private World too). Nothing in this tree claims tags round-trip.
2. **⚠ S9 and S10 are NOT tier-0-visible, and the plan's own §V4 said they were — that is corrected
   here.** No test target can construct `InspectorPanel` (`git grep -l InspectorPanel -- tests/` is
   empty — its draw path is src-private and ImGui-bound); `field_command_test.cpp`'s P8 pins the D17
   *policy* against a `CommandStack` directly, which is a real and useful test, but it is not the
   panel. Seeding S9 (move the Inspector's `gate.closed` break *before* the push, in five of the seven
   arms) and running the whole suite left it **94/94**, P8 included — and the later sabotage pass
   confirmed the mirror by measurement rather than inference: seeding **S10** (move the `gate.opened`
   break *after* the push, in **all seven** arms, diff-verified as 7 relocations) also left the suite at
   **94/94**, with §V7's comment-stripped `breakMergeChain` count still reading **14**, so not even the
   grep gate catches it. The panel's own application of the
   rule has exactly one proof anywhere in this tree: **human row 6** on
   `editor/validation/2.4.2-property-set-structural-commands.md`. This is the same class of finding
   2.4.1's own code-review round made about S13 — a sabotage table entry that looked covered and was
   not — caught here before merge instead of after, by actually seeding and running rather than trusting
   the plan's own prediction.
3. **§V4 is DISCHARGED: all twenty seeds (S1–S19 plus S5b) and all three second-order checks have been
   run.** Four (S1, S9, S11, S14) ran during the implementation pass; the other sixteen ran in a
   dedicated sabotage pass afterwards. Every seed was proved present with `git diff` before any verdict
   was trusted, rebuilt (never a stale binary), measured against the **whole** 94-entry suite (never a
   filtered or case-scoped run — 2.3.3's S5 was misreported that way), then reverted with
   `git status --short` confirmed empty before the next. Seventeen behaved as predicted or better —
   S1 (3 targets: `aero_tests` W1, `aero_editor_shell_test` X19, `aero_editor_imgui_test`), S2b, S3, S5,
   S5b, S7, S8, S11, S14, S17, S19 all reddened their named discriminator, several of them plus three to
   five extra cases. **Nine did not**, and are set out as finding 5 below rather than relabelled. The
   full per-seed verdict table lives in
   `editor/validation/2.4.2-property-set-structural-commands.md`; the second-order checks came out
   PASS for S7 (weakening P3 `:226` + P4 `:297` + P8 `:433` makes the seed pass 94/94) and PASS for S14
   (weakening X16 `:146` + `:178` does the same), while **S11 is stronger than predicted** — the seed is
   caught independently by 12 cases across 2 targets, and weakening X4's four assertions still leaves 11
   red, so per §V4's own instruction the check stopped there rather than forcing a silent pass.
4. **Three plan errors surfaced during execution, all fixed as minor deviations, not silently.** (a) The
   plan's architectural solution diagram asserted `Traits` was already a `using` alias in `world.cpp`; it
   was not — `using scene::internal::Traits;` (`world.cpp:41`) had to be added for `recreate`'s occupancy
   pre-check. (b) §3i said `imgui_layer_test.cpp` had "one call site" needing migration for the widened
   `CommandContext`; there were **six**, all fixed in the same commit (`fb56708`) the plan's own step
   already owned. (c) The plan predicted `check-math-boundary.sh`'s scanned count would land at **221**
   (§V1's own table flags this "MEASURE IT" rather than trust it, and the plan's own arithmetic — "+9 new
   files" over 2.4.1's 215 — does not even reach 221 on its own terms); it measures **224** at HEAD,
   confirmed directly with `bash .github/scripts/check-math-boundary.sh`.
5. **Nine of §V4's twenty seeds did not behave as the plan predicted — three have NO discriminator at
   all, six are caught by a different case than the one the matrix names.** In order:
   (a) **S2a** — dropping `recreate`'s F5 occupied-index pre-check *alone* leaves the suite 94/94; W3's
   occupied-index arm only reddens once the trailing `made != entity` belt is removed too (S2b). The
   suite proves the belt's **outcome**, never the pre-check's existence. §V4 predicted this branch and
   called it a finding rather than a pass.
   (b) **S6 — phase A's rollback was entirely untested; a real gap found by the matrix, not by review,
   and now CLOSED.** N7 pre-occupies the *first* record's index, so phase A refuses on record 0,
   `created == 0`, and the rollback loop iterates zero times — deleting the loop outright changed
   nothing. Proven rather than inferred: replacing its body with `world.clear()` when `created > 0`, a
   probe that would wipe the World if the body were ever entered, *also* left the suite at 94/94. **N13**
   (`tests/editor/scene_snapshot_test.cpp`, commit `dca6302`) closes it — two independent roots, `b`
   destroyed last so entt's LIFO free list hands its index back first, occupying **only** `b`'s slot, so
   phase A recreates `a`, refuses `b`, and must destroy `a` again. Re-seeding S6 with N13 present reddens
   **N13 and nothing else**, on `CHECK_FALSE(world.alive(a))` and `world.entityCount() == 1`. The LIFO
   assumption is `REQUIRE`d rather than looped around, so the case fails loudly instead of silently
   degrading back into N7 if entt's recycling order ever changes.
   (c) **S9/S10** — finding 2 above; no mechanical proof of the Inspector's gate pair exists at all.
   (d) **S4 — the dangling `string_view` is NOT sanitizer-visible, which inverts §V4's own claim.** The
   plan said "ASan aborts N1/N10 — an abort, not a red assertion… a *stronger* result than a failed
   `CHECK`". Measured on macOS Debug with ASan **and** UBSan confirmed on
   (`AERO_ENABLE_SANITIZERS:BOOL=ON`, `-fsanitize=address,undefined -fno-sanitize-recover=all`): **zero
   sanitizer output**, and five ordinary red `CHECK`s (N1, N10, N12, X4, X18). The World's name records
   live in an EnTT swap-and-pop pool, so destroying the entity leaves the freed record's bytes inside a
   still-mapped, still-live page, overwritten by the swap — nothing ASan can flag. **Standing lesson: a
   dangling view into a pooled component must never be assumed sanitizer-visible**; the ordinary
   assertions are what catch it.
   (e) **S12** — X1 is not a discriminator and structurally cannot be: it only pushes and asserts the
   selection *after the push*, never calling `undo`. X2, X3, X7 and X8 are what fire.
   (f) **S13** — only **R3** reddens. **X7 and I8 do not**: both insert an *in-range* slot (1 into size 2,
   2 into size 2), so the clamp direction is irrelevant to them. Nothing at command or frame level
   exercises an out-of-range slot.
   (g) **S15** — X10 fires as predicted; **X12 does not**, because its two restores coincide with a plain
   append. X12 is not sibling-slot coverage.
   (h) **S16** — the row names two discriminators that need two different seeds: seeding
   `CreateEntityCommand::redo` reddens X3 and X18 but **not** X9, which is only reachable by separately
   seeding `DuplicateEntitiesCommand::redo`. Both were run.
   (i) **S18** — **I9, the named discriminator, is vacuous against this defect class** (as §V4 itself
   predicted): it builds its own `CommandContext` from `app->roots()`, so handing every panel a decoy
   `RootOrder` leaves `CHECK(&cmd.roots == firstTick)` green. The measured addition is that S18 **is**
   caught — by **I8**, which fails hard because the panel reconciles the decoy while `app->roots()` stays
   empty. I9 must not be cited as the proof.

**Inventory, measured at every commit boundary and read back out of the tree, never predicted.**
`ctest -N` **94 → 94** (tools ON), **5 → 5** (both tools OFF, `build/tools-off-2.4.2`) and **18 → 18**
(reflect OFF alone, `build/reflect-off-2.4.2`) throughout — no new `add_test` anywhere (AC-34); `aero_tests`
**356 → 363** (`tests/scene_test.cpp` W1–W7, `World::recreate`'s Step 1 commit); `aero_editor_shell_test`
**199 → 236 → 237** (`hierarchy_test.cpp` R1–R4 in the `RootOrder` move; `scene_snapshot_test.cpp` N1–N12
plus `RL1`'s six-`SUBCASE` `TEST_CASE` in the `SubtreeSnapshot`/`ComponentSnapshot` commit, then **+N13**
in the sabotage pass's `dca6302` — `grep -c '^TEST_CASE'` on that file reads **14**, not 19 or 20;
`structural_commands_test.cpp` X1–X20 split across the
component-commands and structural-entity-commands commits); `aero_editor_inspector_test`
**14 → 22** (`field_command_test.cpp` P1–P8, riding the `AERO_REFLECT_TOOLS`-gated target — its P8 case is
the sole tier-0 proof of finding 2); `aero_editor_imgui_test` **30 → 35** (I7–I11, real-frame Create /
Delete / Duplicate / Reparent-and-Rename / `EditorApp` noexcept-move coverage); **+57 new doctest cases**
from the implementation pass, matching the plan's own §V1 prediction exactly, **plus N13 from the
sabotage pass — 58 in total**, which moves `scene_snapshot_test.cpp` and `aero_editor_shell_test` one
past §V1's pinned 13/236. That pin is plan bookkeeping; the real invariant is `ctest -N` = 94/5/18, which
adding a `TEST_CASE` to an existing TU does not touch, and shipping a false AC-7 coverage claim would
have been the worse trade. `editor/CMakeLists.txt` sources **27 → 30**
(`scene_snapshot.cpp`, `component_commands.cpp`, `entity_commands.cpp`) with **no link-line change** — all
three need only `aero::core`/`aero::scene`, already PUBLIC. `check-math-boundary.sh`'s scanned count
**215 → 224** (finding 4c, above — measured, not the plan's predicted 221). `breakMergeChain`, comment-stripped:
`inspector_panel.cpp` **0 → 14** (seven `FieldKind` arms × two edges), `viewport_panel.cpp` unchanged at
**4** (2.4.1's code-review round already split it into Begin/End; Step 7 touches no Viewport file). **One
new `NOLINT`**, not the plan's predicted zero — `field_command_test.cpp:49`'s
`// NOLINTNEXTLINE(readability-identifier-naming)` on the forward declaration of the generated aggregator
`aero_reflect_register_all_aero_editor_inspector_test()`, identical in shape to `inspector_test.cpp:47`'s
pre-existing one: the snake_case name is the frozen codegen contract, and the plan's "predicted 0" simply
did not anticipate a second TU inside the same single-TU-`main()` target redeclaring it. `git diff
--name-only origin/main -- engine/` reads exactly the two files AC-4/INV-8 require
(`engine/scene/include/aero/scene/world.hpp`, `engine/scene/src/world.cpp`); the AC-24 negative-half greps
(`(writeComponentField|addComponent|removeComponent)\(` over `inspector_panel.cpp`,
`(createEntity|destroyEntities|duplicateEntities|reparentEntity|setName)\(` over `hierarchy_panel.cpp`)
both read **empty**, exit 1, against a non-vacuous 9-line/8-line reading on the untouched tree (§V7's own
anti-vacuity baseline); `editor/include/aero/editor/imgui_layer.hpp`, `editor/src/imgui_layer.cpp` and
`editor/src/main.cpp` are byte-identical against `origin/main` (no streak count asserted, only
byte-identity, per 2.4.1's §A3 correction of its own streak arithmetic). All five architecture guards green
with no allowlist change (`check-golden-rule.sh` 82 tracked engine/runtime sources, `check-math-boundary.sh`
224, `check-platform-boundary.sh` 51, `check-rhi-boundary.sh` 80, `check-scene-boundary.sh` 51 tracked
public headers — `check-scene-boundary.sh` has no allowlist at all, §G9, so AC-4's "no allowlist change" is
trivially true rather than implying one exists). `vcpkg.json`'s `builtin-baseline` and the `/vcpkg`
submodule SHA byte-identical; no new `find_package`; every `target_link_libraries` line on every target
byte-identical (AC-33 — no new dependency at all). Both macOS presets green at every one of the nine
code-bearing commit boundaries, Debug and Release, with and without `AERO_REQUIRE_GPU=1`; clang-format
clean and clang-tidy (`--warnings-as-errors='*'`) clean on every touched file. **macOS human-validated pass
not yet run for this task** — Windows/Linux rows pending too; see
`editor/validation/2.4.2-property-set-structural-commands.md`.

##### Task 2.4.2 — code-review round (three commits after the nine, all after the sabotage matrix passed)

**A code review of `feat/2.4.2-property-set-structural-commands` found one BLOCKING correctness defect,
plus two should-fix items, ALL after the twenty-seed sabotage matrix had already been run and recorded
green. This is the honest lesson: S3 and S15 could not see the blocking defect, and could not have —
every existing sabotage case and every existing test restores exactly ONE entity per parent, and the
defect only exists when TWO OR MORE do.**

**Gap 1 — BLOCKING — `placeAt`/`RootOrder::insert` correctness depends on REPLAY ORDER, and nothing
enforced it.** `placeAt` (and `RootOrder::insert`, the identical shape one level up) repositions ONE
entity at a time by nudging every sibling that must follow it into place — which is only equivalent to
"restore the whole list" if entries sharing a parent are replayed in ASCENDING recorded-slot order.
Three call sites all failed this, independently:

- `SubtreeSnapshot::restore`'s phase-B link loop (`scene_snapshot.cpp`) replayed subtree-root position
  restores in **capture order** — the caller's raw target/selection order, which a bottom-up multi-select
  (Ctrl-click a lower sibling before a higher one) makes descending.
- `entity_commands.cpp`'s `restoreState` replayed `RootOrder::insert` calls in the same raw order.
- `ReparentCommand::undo` replayed in the REVERSE of capture order (`std::ranges::reverse_view`), which
  turns an ORDINARY ascending multi-select into the same descending failure mode.

**Empirically reproduced first, not assumed**: parent `P` with children `[a,b,c,d]`, capturing `{c,b}`
(descending, exactly what Ctrl-clicking C then B produces) and restoring both landed `[a,b,d,c]` — the
untouched sibling `d` moved, confirmed by a temporary probe before any fix landed. **"Fixed" by sorting the
position-restore pass ascending by recorded slot at all three sites** — a plain `std::ranges::stable_sort`
by slot alone, with NO grouping by parent needed: `Entity` has no `operator<` (G5), but a subsequence of a
globally-sorted sequence is itself sorted, so restoring position for any one parent's own entries stays
correctly ordered regardless of how other parents' entries interleave with them. `SubtreeSnapshot::restore`
gained a third pass (names+components unchanged; links unchanged in order — A2's append-order correctness
for non-subtree-root records already held and was left alone; a NEW third pass collects the subtree-root
records needing `placeAt` and sorts that list before replaying it). `entity_commands.cpp`'s `restoreState`
sorts the eligible `(root, slot)` pairs before the `RootOrder::insert` loop. `ReparentCommand::undo`
replaced its reverse walk with an ascending-by-recaptured-slot walk — the reverse walk was only ever
needed to undo NESTING (restoring a parent before its own child), and `targets` can never contain both,
because `topMost()`/`reparentTargets()` (D19) always collapse a selected parent and its own selected child
into one target before this command ever sees them.
**CORRECTION (second code-review round, below): this claim was FALSE for `SubtreeSnapshot::restore`.** The
ascending sort was necessary but not sufficient there — it was measured only against the ONE order (N14's
descending capture) that this round's own fix happened to get right, and the fix introduced a NEW,
worse-in-practice regression in the other order. `entity_commands.cpp`'s `restoreState` and
`ReparentCommand::undo` were NOT affected; both hold in both directions. Full account below.

**Also fixed, same function, low severity**: `placeAt` guarded only the 0/1-child case against a failed
append; if `setParent` ever failed to append `child` while the parent already had ≥2 real children,
`scratch` would not contain `child` at all, and the loop would then detach and re-append genuine siblings
for no reason — spurious reordering on a pure failure path. **Believed unreachable today** (LIFO undo means
the sole refusal, a cycle, cannot be live at undo time), but the function's own comment already anticipated
a failed append, so it is now verified: `scratch.back() == child` is checked before anything else is
touched, which required un-eliding the `child` parameter (now genuinely used, no `NOLINT` needed).
**CORRECTION (second code-review round, below): this guard was NOT merely a defensive belt for a
hypothetical failure path — the pass split this same round introduced made it fire on the ordinary,
successful main path, on the first `placeAt` of every ascending multi-sibling restore, and its effect there
was to silently no-op the reorder for everything but the last-appended entry.**

**A fourth, related defect found ONLY while writing the mandated shared-parent `ReparentCommand` test, not
named by the original review** — recorded here in full rather than folded silently into the three sites
above, since it is outside their literal scope: `ReparentCommand::redo` captured each target's old
parent+slot INTERLEAVED with the actual reparent, one target at a time. When two targets share an
immediate parent, reparenting the first target ALREADY shifts the second target's position in the live
world before its own slot is queried — `childIndexOf` reads a list that has already lost one entry, so the
captured slot is silently wrong (shifted down by the number of already-processed same-parent targets),
independent of any replay-order fix. **Fixed by capturing every target's old parent+slot in a first pass
against the UNCHANGED tree, then applying every reparent in a second pass** — no signature change, purely
internal to `redo()`.

**A genuine anti-vacuity trap, found and proven rather than assumed**: for the specific shared-parent test
scenario (two adjacent, or even two non-adjacent, siblings reparented together), reverting BOTH the
`redo()` capture fix and the `undo()` ordering fix TOGETHER leaves the mandated new test (`X22`) passing —
the two bugs' errors cancel out for this input shape by coincidence (a corrupted-but-still-in-range
captured slot, replayed in the old reversed order, happens to land back on the correct final position).
Reverting EITHER fix ALONE, with the other left in place, reddens `X22` cleanly (two different wrong final
orderings, verified by running both configurations, not inferred). This is precisely this project's own
"verify the seed landed" discipline turned on a same-branch code-review fix instead of a sabotage seed —
recorded here so it is never mistaken for a clean two-fix discrimination proof.

**Three new tests, one per site, each asserting the FULL child list element-wise (including the untouched
siblings), because the untouched sibling is exactly what a wrong replay order silently moves:**

- `scene_snapshot_test.cpp` N14 — two captured siblings of one parent, captured in DESCENDING slot order,
  restored via `SubtreeSnapshot::restore` directly.
- `structural_commands_test.cpp` X21 — two ROOTS deleted together in descending slot order via
  `DeleteEntitiesCommand`, `RootOrder` reconciled before and after.
- `structural_commands_test.cpp` X22 — a two-target `ReparentCommand` whose targets are siblings of ONE
  shared parent, given in ORDINARY ascending (in-row-order) selection order — deliberately NOT X12's shape
  (X12 uses two DIFFERENT parents, which is exactly why it cannot see this class of bug, per the plan's
  own finding 7 on X12/S15).

**Discrimination proof, run and recorded, not assumed**: reverting all three fixes together (full
`git checkout` of both touched `.cpp` files back to the pre-review tree) reddens N14 (`after[2]`/`after[3]`
wrong) and X21 (`roots.entities()[2]`/`[3]` wrong) cleanly; X22 passed in that exact configuration, which
is the anti-vacuity trap above, resolved by the two isolated partial-revert runs (redo fix alone reverted
→ X22 red on `after[1]`/`after[2]`; undo fix alone reverted → X22 red on `after[2]`/`after[3]`; X10/X11/X12
stayed green in both configurations). Restoring the full fix: all three green, whole suite 94/94 both
presets, `ctest -N` unchanged at 94/18/5.

**New seed S20, recorded for the sabotage table**: *"reverse the position-restore order"* (equivalently:
skip the ascending sort at any of the three sites). Measured discriminator: N14 and X21 each redden
cleanly on their own site's revert; `ReparentCommand`'s two component fixes (redo capture, undo order)
must each be isolated from the other to see X22 redden, per the anti-vacuity trap above — a combined
revert of both is NOT a valid S20 rehearsal for that site. **S3 and S15 (the existing table's two
"placeAt"/position-restore entries) provably cannot catch S20**: both delete `placeAt`/the whole
position-restore block entirely, which every existing single-target case already catches on its own; none
of S3/S15/N3/X10/X12 ever exercises two entries sharing one parent, which is the one precondition S20
needs to fire.

**Gap 2 — two §V7 gate greps inflated by this task's own prose, both corrected.** `scene_snapshot.hpp`'s
own doc comment for `SubtreeSnapshot::roots()` named the `RootOrder` class directly, making
`git grep -n 'RootOrder ' -- editor/src editor/include | grep -v 'RootOrder&'` read **3** lines where
AC-31's gate pins exactly **2** (the two real declaration sites, `entity_ops.hpp`'s class and
`editor_app.hpp`'s member, after D10's HierarchyPanel→EditorApp move) — reworded to describe the query
without naming the class. `editor/validation/2.4.2-property-set-structural-commands.md` quoted the AC-24
Hierarchy call-form pattern verbatim, including `destroyEntities`, making
`git grep -ln 'destroyEntities' -- editor/` read **4** files where §V7 pins exactly **3** — reworded to
describe the five mutator names without spelling all of them out. Both confirmed to reproduce their pinned
counts after the reword. This is the same identifier/prose-collision class R9/§A29 already named for panel
comments (never the exact token an AC-24/AC-31 grep counts), now also applied to a public header comment
and to a doc this task itself writes — the real gates (the panel-scoped negative greps) were never
affected, and no shipped document ever asserted the inflated value as fact, so nothing false was recorded
at any point; this is purely a gate-reproducibility fix.

**Gap 3 — `AddComponentCommand::undo` was missing its `alive()` pre-guard.** Plan §S 5b mandates a silent
liveness pre-guard on every command's `undo`/`write` so an undo past a deleted entity returns `false`
quietly rather than relying on the seam's own silence; `RemoveComponentCommand::undo` and
`SetFieldCommand::write` both had theirs, `AddComponentCommand::undo` did not. Behaviourally identical
today (`removeComponent`/`world.cpp` already return `false` silently for a dead entity) — this is plan
conformance and cross-command consistency, not a live bug — fixed to match its two siblings' shape.

**Inventory delta, measured, not predicted.** `aero_editor_shell_test` **237 → 240** (N14, X21, X22 — the
three new `TEST_CASE`s, one per site); `ctest -N` unchanged at **94** (tools ON), **18**
(`build/reflect-off-2.4.2`), **5** (`build/tools-off-2.4.2`) — all new coverage rides the same two existing
TUs, no new `add_test`. `aero_tests`, `aero_editor_imgui_test` and `aero_editor_inspector_test` all
unchanged. Both macOS presets green at 94/94 Debug and Release, the `AERO_REQUIRE_GPU=1` rehearsal green,
both reflect/tools-OFF configures green, the non-interactive launch proof zero unexpected
ERROR/CRITICAL/WARN, all five architecture guards green with no allowlist change, `git diff --stat
origin/main -- engine/ runtime/ samples/ tools/ shaders/ cmake/ .github/` unchanged from the original nine
commits (this round touches only `editor/`, `tests/` and docs), clang-format and clang-tidy
(`--warnings-as-errors='*'`) clean on every touched file with **zero new `NOLINT`**.

##### Task 2.4.2 — second code-review round (found the first round's own fix was wrong; two commits)

**A second code review, run against a tree the first review round had already left mechanically green
(94/94 both presets, N14/X21/X22 all passing), found that the first round's fix for
`SubtreeSnapshot::restore` was itself WRONG — not incomplete, WRONG: it swapped which capture order fails
rather than fixing the defect, and the order it newly broke (ascending) is the ordinary one, while the
order it happened to still get right (descending, N14's own case) is the less common one. This is the
most important lesson this task produced, more than either bug individually: a fix verified only against
the originally-reported input can be a swap, not a fix. Recorded here in full rather than folded silently
into the first round's own entry above, because the first round's entry already asserted (twice) that the
defect was fixed, and both assertions were false — corrected in place above rather than deleted, so this
document does not quietly erase its own prior mistake.**

**Measured reproduction, against `SubtreeSnapshot::restore` directly, before any second-round fix
landed**: parent `P` with children `[a,b,c,d]`, capturing two siblings and restoring —

| capture order | measured result | correct? |
|---|---|---|
| `{b,c}` (ASCENDING — a shift-click range or a top-down Ctrl-click, the ordinary gesture) | `[a,d,c,b]` | **WRONG** — 3 of 4 entries misplaced |
| `{c,b}` (DESCENDING — N14's own case, a bottom-up Ctrl-click) | `[a,b,c,d]` | correct |

Pre-first-round-fix, this was the reverse (descending wrong, ascending — untested by N14 — accidentally
right). The regression the first round shipped is both more likely to be hit (ascending is the common
gesture) and worse (3 of 4 slots wrong instead of the original bug's own shape).

**Root cause: `placeAt` is not an insert-into-list primitive.** Its own contract, stated in its header
comment (`scene_snapshot.hpp`), is *"the child has just been APPENDED to parent"* — it repositions ONE
already-last entity, it does not insert an arbitrary entity at an arbitrary position. The first round's fix
split `SubtreeSnapshot::restore`'s phase B into an append-every-subtree-root pass followed by a SEPARATE,
sorted `placeAt` pass. When two or more subtree roots share one parent, every one of them is appended
before any of them is placed — so by the time the sorted pass starts calling `placeAt`, at most the
LAST-appended entry is still `scratch.back()`. `placeAt`'s own `scratch.back() == child` guard (added by the
first round as a defensive belt against a hypothetical failed-append edge case) then makes every EARLIER
entry silently no-op instead of visibly corrupting the list — converting a loud bug into a silent one. The
ascending-by-slot sort itself was correctly reasoned (`stable_sort`'s "a subsequence of a globally-sorted
sequence stays sorted" claim is true) but insufficient: ascending replay order is NECESSARY, not
SUFFICIENT — `placeAt` also needs child-is-last AT CALL TIME, which the append/place pass split destroyed.
This is exactly why the identical ascending sort is correct at `entity_commands.cpp`'s `restoreState` and
at `ReparentCommand::undo` (both keep their own `setParent` and `placeAt`/`RootOrder::insert` call
adjacent, in the SAME loop iteration, for the SAME entry) and wrong only at `SubtreeSnapshot::restore` (the
only one of the three sites the first round restructured into two passes instead of one).

**Fix, `editor/src/scene_snapshot.cpp` only**: keep the ascending `stable_sort`, but restore append/place
ADJACENCY. The subtree-root records needing a position are still collected and sorted ascending by
`siblingIndex` first, exactly as before; what changed is that `world.setParent(record->handle,
record->parent)` now happens INSIDE that same sorted loop, immediately before that record's own `placeAt`
call — the shape `ReparentCommand::undo` already used correctly. Non-`subtreeRoot` links (a parent already
inside the snapshot) are untouched, in the original pre-order pass, exactly as A2 requires.
**`restoreState` and `ReparentCommand::undo` were NOT touched** — both were independently re-verified
correct in ascending, descending AND mixed order this round (see the new tests below); their own
`RootOrder::insert`/`placeAt` calls were already adjacent to their own `setParent`, so the first round's
defect never applied to them.

**Four new tests, mirroring every order-sensitive site in the direction it did not yet have a case for —
this is the process fix, not just the code fix.** N14 and X21 each only ever tested ONE capture order, and
N14 happened to test the order the first round's broken fix got right, so the gate stayed green through a
real regression:

- `scene_snapshot_test.cpp` N15 — N14's ASCENDING mirror, same two-sibling-of-one-parent shape, capture
  order reversed. **Shown RED against the pre-fix tree** (`after[1]`/`after[3]` wrong, matching the
  measured `[a,d,c,b]` reproduction above), then GREEN after the adjacency fix.
- `scene_snapshot_test.cpp` N16 — a THREE-sibling MIXED-order case (`{c,b,d}` of `[a,b,c,d,e]`), added
  because a two-element case cannot distinguish "restored correctly" from "restored reversed" — both look
  identical for exactly two entries. Also shown RED against the pre-fix tree, then GREEN.
- `structural_commands_test.cpp` X23 — X21's ASCENDING mirror (two roots deleted together, ascending slot
  order). `restoreState`'s own sort-then-`RootOrder::insert` loop was already correct in both directions
  (`RootOrder::insert` is a genuine positional `vector::insert` with no positional precondition, unlike
  `placeAt`), so this is a regression-proofing addition, not a red-then-green case — measured green against
  both the pre-fix and post-fix tree.
- `structural_commands_test.cpp` X24 — X22's DESCENDING mirror (two-target reparent sharing one parent,
  descending slot order). `ReparentCommand::undo` was already correct in both directions for the same
  reason as X23 — measured green against both trees.

**Discrimination proof for the ascending mirror, run and recorded**: reverting the adjacency fix (restoring
the first round's append-pass/sorted-placeAt-pass split) reddens N15 (`after[1]`/`after[3]` wrong,
`[a,d,c,b]`) and N16 (`after[1]`/`after[4]` wrong) cleanly; X23 and X24 stay green in that configuration
(confirming they were never testing the defective code path). Restoring the fix: all four green, whole
suite 244/244 (up from 240) both presets, `ctest -N` unchanged at 94/18/5 (new coverage rides the existing
two TUs).

**`placeAt`'s own comment corrected**: it previously called the `scratch.back() == child` guard
"unreachable today" (a claim written by the first round, describing the guard as a defensive belt for a
failure path that LIFO undo cannot currently produce). That claim was true for the ORIGINAL, single-pass
`placeAt` call shape, but became FALSE the moment the first round's own restructuring split append from
place: for the intervening tree (the one this second round found), the guard fired on the MAIN, successful
path, on the first `placeAt` of every ascending multi-sibling restore — not a hypothetical failure case at
all, but the mechanism that converted a loud corruption into a silent no-op. The comment now describes both
the current (adjacency-restored, guard-unreachable-again) state and this history, rather than asserting
only the current state as if it had always held.

**Inventory delta, measured, not predicted.** `aero_editor_shell_test` **240 → 244** (N15, N16, X23, X24);
`ctest -N` unchanged at **94** (tools ON), **18** (`build/reflect-off-2.4.2`), **5**
(`build/tools-off-2.4.2`) — all new coverage rides the same two existing TUs, no new `add_test`. Both macOS
presets green at 94/94 Debug and Release, the `AERO_REQUIRE_GPU=1` rehearsal green, both reflect/tools-OFF
configures green, clang-format and clang-tidy (`--warnings-as-errors='*'`) clean on every touched file with
**zero new `NOLINT`**.

**The transferable lesson, stated plainly for future review rounds on this codebase: verifying an
order-sensitive fix against only the originally-reported input is not verification — a fix that reorders
rather than corrects will pass that one input by construction. Every order-sensitive fix from here on
needs a mirror case in the OTHER order before it is trusted, not after.**

---

### Epic 2.5 — Scene I/O

#### Task 2.5.1 — Save/load/new from editor — OPENS Epic 2.5

**2.5.1 ships the whole File menu — New Scene, Open Scene…, Save Scene, Save Scene As… — all live, plus
the unsaved-changes guard over New/Open/quit and a window title showing the document name and its dirty
state.** As merged, the task is **seventeen commits** on `main` (measured with
`git log --oneline --no-merges 90c95a0..main`, not counted by hand): eight code-bearing (§S Steps 1–8),
four small follow-up commits found during verification, one documentation commit, and four more from the
2026-07-31 code-review round.

**This count has been wrong twice, in opposite directions, and the history it describes was rewritten
before merge — so read it carefully.** The entry originally claimed "thirteen"; the code-review round
(finding 9b) corrected it to fourteen, having spotted that the uncounted one was `412639c`, a build-break
fix for `fba58b3`, which did not compile (`shell_ui.hpp` dropped `ShellUiState::quitRequested` while
`editor_app.cpp` still read it). **That fix commit no longer exists.** Before merge it was folded back
into `fba58b3` with a non-interactive rebase (`GIT_SEQUENCE_EDITOR`, since `git rebase -i` is unavailable
in the agent environment), so every commit on `main` builds and `git bisect` across this range returns
test verdicts rather than compile errors — which is the entire reason this project mandates a merge
commit over a squash. The rewrite was verified three ways before force-pushing: commit count 18 → 17,
`git diff` between the pre-rebase backup tag and the rewritten HEAD **empty** (tree byte-identical, only
history changed), and the rewritten commit (`3ef893c`) **checked out and built**, exit 0 — a grep that
the symbol mismatch was gone would not have been proof. **The lesson worth carrying: a per-step commit
history is only bisectable if each step actually compiles, and the ordinary way that breaks is a partial
`git add`** — the edit that keeps a commit consistent sitting unstaged in the working tree while its
sibling is committed. `git status --short` immediately after each commit is what catches it. The whole file-flow state machine
(menu → guard → modal → native dialog → atomic write/read) lives as free functions in two ImGui-free,
SDL-free TUs (`scene_session.{hpp,cpp}`), which is what makes almost the entire transition table tier-0
testable with no window and no GPU — the plan's own §A13/§A14 structural decision, taken as given rather
than re-litigated (both were pre-decided in §R-0 before this pass began). `scene_file.{hpp,cpp}` adds the
atomic read/write pair (write to `<path>.aero-tmp`, close the stream, `std::filesystem::rename` over the
target — Windows-safe ordering, `std::ios::binary` on both sides so 2.5.2's future byte-stable golden
test survives CRLF translation). `scene_io.{hpp,cpp}` is the **one** `#if defined(AERO_EDITOR_REFLECTION)`
TU in the whole editor (4 occurrences, `git grep -c`) bridging to `scene_serialize::loadScene`/
`saveWorldText`/`parseScene`; `sceneIoAvailable()` reports `false` when the editor is built with
`AERO_REFLECT_TOOLS=OFF`, and every path that reaches native I/O checks it first (D18). `file_dialog.
{hpp,cpp}` (src-private) wraps SDL3's native async `SDL_ShowOpenFileDialog`/`SDL_ShowSaveFileDialog`
behind `DialogChannel` — a judgement-call addition beyond the plan's literal text:
`DialogChannel : public std::enable_shared_from_this<DialogChannel>`, so the callback's raw
`FileDialogHost` pointer can hand back a `shared_ptr` ("Ticket") the SDL callback keeps alive across the
cross-thread boundary without a manual `new`/`delete` anywhere. `EditorApp` gains a `SceneSession`, a
`FileFlow`, a `shared_ptr<DialogChannel>` and — forced by that forward-declared `DialogChannel` member —
out-of-line special members (`~EditorApp()`, move ctor, move assign, all `= default`d in the `.cpp`, not
the header; §R-0's second pre-decided item). `shell_ui.{hpp,cpp}` loses `menuItemStub` entirely (the
last disabled-stub placeholder anywhere in the editor), gains a live four-item File menu with the
correct `canSave` greying logic (AC-4: greyed only when clean **and** titled), the `FileMenuContext`
threaded through `drawShellUi`'s new fourth parameter, and the unsaved-changes modal.

**§A1 was the plan's own named highest-risk item, and it held exactly as predicted.** ImGui 1.92.8
cannot dismiss a *modal* popup with Escape through its built-in nav-cancel path — verified directly
against the vendored source (`imgui.cpp:15007`/`15032`'s `NavUpdateCancelRequest` popup branch excludes
`ImGuiWindowFlags_Modal`; `imgui_layer.cpp:79` confirms `ImGuiConfigFlags_NavEnableKeyboard` is never
set) — so AC-27 is hand-bound inside the modal body with
`ImGui::IsKeyPressed(ImGuiKey_Escape, false)`, with the plan's own comment kept explaining why this is
not redundant with ImGui's own handling. **No test tier can press a key**; row 14 of the validation page
is the only proof of AC-27 anywhere in this tree.

**What deliberately did not ship, all recorded rather than silently dropped.** No compound "save on
quit without asking" preference (out of scope — the guard always asks). No recent-files list (2.6.1's
concern). No project-relative path storage (`SceneSession` holds an absolute path only; project roots
arrive at 2.6.1). The Linux no-XDG-portal/no-zenity path (F4/S14) logs exactly one ERROR and keeps
running — proven unreachable from any test tier (S14 confirmed GREEN, zero references to
`DialogChannel`/either launcher anywhere under `tests/`) and left as a human-only, Linux-only row (20).

**The traps worth keeping.** (i) **INV-6 is a data-corruption invariant since 2.4.2, not a cosmetic
one** — `World::clear()` bumps every entity's generation but never un-issues an index (measured,
`scene_test.cpp` W7), so a `CommandStack` left holding a `SubtreeSnapshot`/history entry against a
*replaced* World would `recreate()` handles that mean nothing there. `resetSceneState` clears the
World, the Selection, the `RootOrder` **and** the `CommandStack` in the same operation, and S1's seed
(dropping just the `commands.clear()` call) reddened the whole suite far harder than predicted: SS10,
SS11, SS25 **and** SS28, not the two the plan named. (ii) **S5's seed reddened with a different, more
severe symptom than predicted.** The plan predicted `newScene` seeding before clearing would leave 6
entities where 3 were expected (double-seed); measured, the actual defect is that `resetSceneState`'s
`world.clear()` runs *after* the seed and destroys the just-added entities too, leaving **0** entities,
not 6 — a worse defect than the plan's own prediction, caught by SS12, SS13, SS21, SS25 and
`imgui_layer_test.cpp`'s I12. (iii) **S23 exposed a real, previously unwritten test case.** `applyDialogResult`'s
existing `SS29` only ever exercised a failed *dialog* (SDL-level cancel/fail); nothing in the tree
covered a save whose *write itself* failed while `flow.saveBeforePending` was set — D11's own safety
invariant ("a save that fails must abandon the pending action") had zero coverage, and seeding S23
(dropping the `ok &&` guard) left the whole 94-entry suite green. Closed in the same pass with a new
SS29-family case (save to a non-existent directory, `saveBeforePending = true`, assert the pending
action never ran); re-seeding S23 against the strengthened suite reddens it. (iv) **IO5, as first
written, was not actually discriminating S4.** It used an empty, unseeded `World`, so
`entityCount() == countBefore` (0 == 0) held regardless of parse/swap ordering. Fixed by seeding via
`seedDefaultScene` first, matching IO4's own rigor, before re-confirming the seed reddens it too. (v)
**A commit was made with the working tree not fully staged** — Step 7's `shell_ui.hpp` edit removed
`ShellUiState::quitRequested`, but the corresponding read in `editor_app.cpp` was not staged in the
same commit, leaving HEAD itself non-building (`field designator 'quitRequested' does not refer to any
field`). Caught immediately by `git status --short` showing the file still modified post-commit and a
`git stash` rebuild of HEAD failing; fixed with a new commit, never an amend, per the hard rule. The
standing lesson: verify `git status --short` is clean immediately after every commit, and treat a
non-empty result as a build-breaking emergency, not a footnote.

**§A's corrections, each confirmed at the tree rather than trusted.** The plan's own §G13/§S Step 0
NOLINT baseline (asserted **3**) measures **4** at untouched HEAD (`90c95a0`) —
`editor_reflection.cpp:10`'s prose ("five existing `NOLINT`s of this shape") contains the literal
substring "NOLINTs" via its own plural, the identical "prose collides with a literal grep" class this
task's own comments tripped repeatedly (below). Documented as a measured plan inaccuracy rather than
silently corrected to match the plan's assumption; the real invariant (zero new *suppressions*) was
re-confirmed at 4 before and 4 after. `check-math-boundary.sh`'s scanned count moved **224 → 232**,
exactly the plan's own predicted +8 for the eight new tracked C-family files — the one place this
task's own arithmetic held on the first try.

**The recurring trap this task hit four separate times: an explanatory code comment that names the
exact literal string a `git grep` gate is designed to police.** `shell_ui.cpp` accumulated roughly nine
comments containing the literal string "task 2.5.1" (the gate's own AC-30-family check expects the
*deleted* stub text to be gone, not new prose repeating the task number); one comment referencing "the
deleted `menuItemStub`" by name; two occurrences of the literal substring `ImGui::Shortcut` in prose
describing the seven real call sites (the gate does `git grep -c` expecting exactly 7); and three
banner comments in `scene_session.{hpp,cpp}`/`scene_io_test.cpp` naming `scene_serialize` by identifier
(the AC-32/A29 gate expects that string to appear in exactly `scene_io.cpp` and nowhere else). All four
were reworded to describe the same fact without repeating the policed token — no gate was weakened to
make any of them pass. A fifth, smaller instance: inserting a new `#include <aero/editor/
scene_session.hpp>` with a trailing comment in `imgui_layer_test.cpp` triggered clang-format's
comment-alignment cascade, shifting two neighboring pre-existing lines' comment columns and turning an
"insertions-only" diff (AC-29's own gate) into one with two spurious deletions; fixed by dropping the
trailing comment on the new line rather than fighting the formatter. **Also discovered, not introduced:
the plan's own claim that `ctx.input()`/`__APPLE__` were both empty at HEAD in `shell_ui.cpp` was
false** — one pre-existing `ctx.input()`-related comment line predates this task (`shell_ui.cpp:41`,
present at `90c95a0`); documented as a plan inaccuracy rather than "corrected," and this task's own new
`__APPLE__` mention was removed rather than added to avoid inflating a baseline the plan had measured
wrong.

**§V4 is DISCHARGED: all twenty-three seeds (the spec's original twenty plus S21–S23) and all three
mandatory second-order checks (S1, S4, S10) have been run.** Every seed proved present with `git diff`
before any verdict was trusted, rebuilt (never a stale binary), measured against the **whole** 94-entry
suite, then reverted with the build re-confirmed green before the next. The plan's own §V4 summary
claims "six predicted non-discriminating or human-only" (S6, S7, S8, S14, S17, S19), but its own
per-seed table predicts the identical no-automated-tier outcome for **three more it excludes from that
tally** — S15 (ASan use-after-scope, "No automated tier") and S16 (ASan heap-use-after-free, "same
reachability as S15") are both reproducible only under a Debug-preset dialog a human opens; S18 (an
`IM_ASSERT` abort, "not a red assertion; an abort") is caught only the moment a human opens the File
menu. Measured: all nine (S6, S7, S8, S14, S15, S16, S17, S18, S19) came out GREEN exactly as their own
row predicted, confirmed unreachable from any test tier by the same method S14/2.4.2 established (a
tier-0-wide reference-count grep for the symbol the seed touches) — the plan's summary undercounts its
own table by three, documented here rather than silently reconciled. Of the remaining fourteen, eleven
behaved as predicted; three (S1, S5, S23) reddened with a different or stronger symptom than the plan
named, documented above as traps rather than silently matched to the plan's prediction. The full
per-seed predicted-vs-measured table and the three second-order-check results live in
`editor/validation/2.5.1-save-load-new-from-editor.md`.

**Inventory, measured at every commit boundary, never predicted.** `ctest -N` **94 → 94** (tools ON),
**5 → 5** (`build/tools-off-2.5.1`), **18 → 18** (`build/reflect-off-2.5.1`) throughout — no new
`add_test` anywhere. `aero_tests` unchanged at **363** (no engine change this task — the only
`engine/` touch of the whole task is zero files, confirmed by `git diff --name-only origin/main --
engine/` reading empty). `aero_editor_shell_test` **244 → 259 → 264 → 274 → 288 → 289**
(`scene_session_test.cpp` SS1–SS30 across three commits, `scene_io_test.cpp` IO1–IO14 across two, plus
**+1** from the sabotage pass's own S23-gap-closing case). `aero_editor_imgui_test` **35 → 41**
(I12–I17, New/Save-As/Save/Open driven through real `EditorApp::tick()` frames, including a **moved**
`EditorApp` still driving Save As/Open, AC-34). `aero_editor_inspector_test` unchanged at **22** (no
Inspector change this task). `editor/CMakeLists.txt` sources **30 → 34** (`scene_session.cpp`,
`scene_file.cpp`, `scene_io.cpp`, `file_dialog.cpp`) with exactly **one** new link-line change —
`target_link_libraries(aero_editor_core PRIVATE aero::scene_serialize)`, inside the existing
`if(AERO_REFLECT_TOOLS)` block, the first new PRIVATE link entry on `aero_editor_core` since 2.4.1's
quiet no-link-change task. `check-math-boundary.sh` **224 → 232**. All other four guards green with
**no allowlist change**. `-DAERO_REFLECT_TOOLS=OFF` alone (`build/reflect-off-2.5.1`) reads **18/18**
with `scene_session_test.cpp` running and passing and `scene_io_test.cpp` confirmed **absent** (no
object file, no matching string anywhere under the build tree) — not merely skipped; the four new I12–I17
GPU cases fall back to a `sceneIoAvailable()` guard there and stay green with only New Scene exercised
(a real coverage gap found during Step 9's mandatory tools-off re-verification: these four cases, added
in Step 8, had never been run against a tools-OFF configuration and initially failed there with four
concrete `CHECK`/`REQUIRE` failures — fixed by the guard, not by weakening the assertions). Both macOS
presets green at every commit boundary, Debug and Release, with and without `AERO_REQUIRE_GPU=1`;
clang-format and clang-tidy clean on every one of the thirteen touched files (this "thirteen" counts
distinct touched *files*, not commits — unaffected by finding 9b below, which is about the commit
count only); **zero new `NOLINT`** against the corrected baseline of 4 (not the plan's assumed 3).

**The 2026-07-31 code-review round — nine findings, two blocking, all fixed on the same branch.**
This entry's own original text claimed "no code-review round needed"; that claim was itself wrong, and
is corrected here rather than silently edited away.

- **BLOCKING 1.** `flow.requestedPath` was read as the `AskWhereToSave` step's OWN save target, but it
  is also where a *deferred* Open/SaveSceneAs request's own path lives while the unsaved-changes modal
  is up — two different things sharing one field. Concrete data loss: dirty + untitled document,
  `requestOpenScene(path)` (guard raises `Confirm`, `pending = OpenScene`, `requestedPath = path`), user
  answers **Save** on the modal → the current scene was written **over** `path`, then the code tried to
  open the file it had just destroyed. A second half: no abandon path (Cancel, the dialog-in-flight
  swallow, `applyDialogResult`'s failed/cancelled returns) ever cleared the field, so it could also
  survive to hijack a **later, unrelated** Save As with no dialog and no overwrite prompt. **Fixed** by
  removing the `AskWhereToSave` arms' read of `requestedPath` entirely (no hook in this tree ever
  populates it for that purpose) and adding `flow.requestedPath.clear()` to every abandon/defer path —
  Cancel, the `WriteNow` write-failure branch (an extra site found while fixing this, same root cause,
  beyond the review's own three named locations), the `AskWhereToSave` no-channel abandon, the
  dialog/modal swallow, and both of `applyDialogResult`'s early returns. New tests: SS31, SS32, SS33 in
  `scene_session_test.cpp`. **Seed-back:** reintroducing the exact original `AskWhereToSave` branch
  reddens SS31 alone, the whole 296-entry suite otherwise green — proved present with `git diff` first,
  reverted, rebuilt green.
- **BLOCKING 2.** Nothing gated a File request on `flow.confirmOpen` — only `flow.dialog` was checked,
  in both `shell_ui.cpp`'s `fileEnabled` and `scene_session.cpp`'s swallow guard. The unsaved-changes
  modal being up (with no *native* dialog yet in flight) did not stop a chord from reaching
  `AskWhereToSave` and launching a **second**, native Save dialog on top of the still-open ImGui modal —
  verified against the vendored ImGui 1.92.8 source: `CalcRoutingScore` (`imgui.cpp:10326`) grants
  `ImGuiInputFlags_RouteGlobal` with no modal test at all. **Fixed** by widening both gates to
  `flow.dialog != DialogKind::None || flow.confirmOpen`. New test: SS27b. **Seed-back:** dropping the
  `confirmOpen` disjunct reddens SS27b alone (4 assertion failures inside that one case), whole suite
  otherwise green — proved present with `git diff` first, reverted, rebuilt green.
- **Seven should-fix findings**, all fixed: (3) `imgui_layer_test.cpp`'s I12 could not fail if
  `requestNewScene()` were a no-op (the identical defect class self-corrected for IO5 in `1ad4c93`) —
  fixed with a direct World mutation before the request, seed-verified (2 assertions redden under
  `AERO_REQUIRE_GPU=1`, reverted); (4) AC-14's INFO/WARN were asserted nowhere — closed by two new
  `scene_io_test.cpp` cases, IO15/IO16, which measured (not assumed) that a skipped component produces
  **two** WARNs, not one — `engine::scene_serialize` logs its own per-component WARN independently of
  `scene_session.cpp`'s D21 aggregate WARN; (5) SS28 did not discriminate the ordering it is named for
  (its only arm produced an identical end state either order) — closed by SS28b, using `Cancel` instead
  of `Discard`; (6) SS27 asserted only `entityCount()`/`pending`, both green even with the swallow guard
  removed — strengthened to assert `quitConfirmed` and count E4's one INFO under a `LogSinkScope`;
  (7) commit `fba58b3` did not compile, fixed by `412639c` (see above) — left to the developer's
  separate history-handling pass, no further code change here; (8) `shell_ui.cpp`'s `ioTooltip(io)`
  showed the wrong reason (`io` folds in `fileEnabled`, so a true `AERO_REFLECT_TOOLS` claim could be
  false while a dialog/modal was active) — fixed to `ioTooltip(sceneIoAvailable())` at all three call
  sites, per the plan's own §S 7b; no test tier reaches this ImGui-drawn line, human validation only;
  (9) two doc numbers were wrong (274 → **275** measured reflect-OFF case count; "thirteen" → **fourteen**
  commits) and this entry's own "no code-review round needed" claim was wrong — all corrected above and
  in this section.
- **Full local gate re-measured green after every fix**, not assumed from the implementation pass:
  `ctest -N` unchanged at **94/5/18**; both macOS presets, Debug + Release, with and without
  `AERO_REQUIRE_GPU=1`; `aero_editor_shell_test --list-test-cases` now **296** (default) / **280**
  (`build/reflect-off-2.5.1`); all five guards green with no allowlist change; clang-format and
  clang-tidy clean on all five touched files (`editor/src/scene_session.cpp`, `editor/src/shell_ui.cpp`,
  `tests/editor/imgui_layer_test.cpp`, `tests/editor/scene_io_test.cpp`,
  `tests/editor/scene_session_test.cpp`). Full detail — findings, fixes, both seed-back proofs, the
  measured gate output — in `editor/validation/2.5.1-save-load-new-from-editor.md`'s own new
  "The 2026-07-31 code-review round" section.

#### Task 2.5.2 — Scene round-trip golden test — CLOSES Epic 2.5

**2.5.2 turns serialization stability into a CI fact.** Three committed scene fixtures under
`tests/fixtures/scenes/` — `empty` (37 bytes), `full` (3244 bytes, 8 entities / 10 components) and
`edge` (1328 bytes, 4 entities / 3 components) — plus **two** byte-comparison batteries that run on
all three lanes: an engine battery (`tests/scene_serialize_test.cpp`, G1–G10, text → `World` → text,
no disk) and an editor battery (`tests/editor/scene_golden_test.cpp`, EG1–EG8, file → `World` → file
through 2.5.1's real `openSceneFile`/`saveSceneFile` pair). Neither is a new ctest entry; both ride
existing targets, so `ctest -N` stays **94 / 5 / 18**. **`engine/`, `runtime/` and `tools/` are all
byte-identical** — and this is the **first task in the project with an empty `editor/` diff as well**
(the whole task lands in `tests/` plus `.gitattributes` and docs). Five commits.

**The trap the whole design exists for — S12, run in full.** A load→save byte comparison is
*self-consistent under a mutual bug*: `loadScene` was made to stop reading `parent`
(`scene_serialize.cpp:132`) and `saveWorld` was made to stop emitting it
(`scene_format.cpp:378-381`) at the same time, the Release archives were rebuilt, and
`full.scene.json` was **regenerated from the seeded build** (the realistic version of the seed — a
developer who introduced the bug and then "fixed the stale golden" would do exactly this). Measured:
the regenerated fixture is **3187 bytes** (not 3244) with **zero** `"parent"` keys.
**G2's byte comparison then PASSES** — the fixture and the seeded writer agree with each other, which
is the entire point of D0 — while **G5 and G6 both go red**: G5's `forwardParent`/`grandParented`
counts drop to 0, and every `world.parent(...)` assertion in G6 fails. Reverting both source hunks
and the regenerated fixture restores green. If G5/G6 had not shipped, this task would have shipped a
rubber stamp.

**Sabotage: all 19 of 19 seeds run, plus all 3 mandatory second-order checks — every one confirmed
landed before trusting a verdict, every one reverted and reconfirmed green.** S7 and S19 are
Windows-lane-only by construction and **not dischargeable on this machine**; both were run anyway and
confirmed a genuine no-op on macOS (94/94 stayed green with the seed applied), recorded as
*"confirmed no-op, not a pass"* rather than as passes. Full predicted-vs-measured table lives in the
plan (`docs/plans/2.5.2-scene-round-trip-golden-test.md` §V4); the summary below is what was actually
observed, not what was predicted, per this project's standing rule.

**Six findings from the sabotage pass, none of them cosmetic.**

(i) **A whole class of seed cannot redden G5/G5b/G6/G7/G8 — a WRITER-only bug is invisible to any
case that reads the committed fixture bytes or a freshly-loaded `World` without re-deriving them.**
Measured directly, repeatedly: S3 (swap two `BUILTINS` entries), S4 (drop the name assign), S5 (drop
`parent`'s `+ 1`), S6 (sort components alphabetically), S8 (shorten `to_chars`' precision) and S9
(non-finite → `"0"` instead of `null`) each reddened only the cases that **re-derive bytes and
compare them** (G1–G4, G9, G10, the editor's EG3–EG5/EG7) — never G5, G5b, G6, G7 or G8, which parse
the fixture straight off disk or read values a correct `loadScene` already produced correctly. The
plan's own per-seed table named G5–G8 as expected discriminators for several of these seeds; that
prediction does not hold. The design is still sound — S11 (break the *loader's* `setParent` call)
and S12 (the mutual bug) both prove G5/G6/G7 catch exactly what they were built to catch, on the
load side and the load+save side — but six individual seed-row predictions in the plan's table
named the wrong catcher for a writer-only defect, and that asymmetry is worth carrying forward
explicitly rather than re-deriving it from scratch next time.

(ii) **S16 has zero coverage from this task's own new tests.** None of the three golden fixtures is
malformed, so `openSceneFile`'s `!outcome.ok` branch — the one S16 breaks — is never exercised by
EG1–EG8 at all; the plan predicted EG2 would also redden, and it does not. The sole detector anywhere
in the tree is the pre-existing `scene_io_test.cpp` case IO14, which supplies its own malformed
input. This is not a defect in 2.5.2 — a golden fixture is by definition well-formed — but it is a
real gap between the plan's prediction and what the new battery actually contributes for this seed.

(iii) **S17 discriminates nowhere the plan predicted.** Both EG3's `CHECK(ed.commands.isClean())` and
the pre-existing IO11's identical assertion start from a stack that was never dirtied between open
and save, so `isClean()` reads true whether or not `commands.setClean()` ran — neither catches the
missing call. The only detector anywhere in the tree is `aero_editor_imgui_test`'s pre-existing case
I14, in a different binary entirely, which performs a real edit before saving.

(iv) **`git diff` cannot see a CRLF seed (S13).** Under `.gitattributes`' `text=auto eol=lf` git
normalizes the working-tree copy before diffing, so seeding a fixture with carriage returns produces
an **empty `git diff`** and only a stderr warning; `xxd`/`grep -c $'\r'` are the only proof the seed
landed. `hygieneComplaint` breaks with mandatory second-order check 3 confirmed both S13 and S14 are
still caught by the raw byte comparison (G2/G3) with an opaque `"golden mismatch at byte offset …"`
message instead of the friendly *"carriage return at offset 1"* / *"UTF-8 BOM at offset 0"* one — the
hygiene check adds diagnosis, not detection, exactly as designed.

(v) **`aero_scene_serialize_test`'s measured case count is 23, not the plan's predicted 22.** G5b
(`edge.scene.json`'s lexical-corner richness case) is a distinct `TEST_CASE`, not folded into "G5" as
the plan's "+6 for G5–G10" arithmetic assumed — the family is seven physical cases (G5, G5b, G6, G7,
G8, G9, G10), giving 16 → **23**. Every count elsewhere in this entry uses the measured figure.

(vi) **Two self-inflicted "the comment names the policed token" near-misses, both caught and fixed
before committing.** The editor TU's header comment named `tests/scene_serialize_test.cpp` verbatim,
which joined 2.5.1's confinement-grep roster (`git grep -ln "scene_serialize" -- editor/src/
tests/editor/`, baseline **two** files) — reworded to describe the sibling battery without the
literal substring, roster back to two. Separately, an explanatory comment in `tests/CMakeLists.txt`
reading *"No add_test: ctest -N stays 94/5/18"* made AC-21's own `git diff … | grep -E
'add_test|find_package|target_link_libraries'` gate non-empty — a real false positive from prose, not
a real `add_test` call — reworded to drop the substring. Both are new instances of the class of trap
2.5.1's log names four times; this task adds two more to the tally, both closed.

**A mechanical bug found while writing the editor battery, fixed WRONGLY at first, and corrected in
the code-review round.** EG6 (log-count per open) loops over three fixtures with a single `records`
vector declared outside the loop. `LogSink::take()` asserts its argument is empty **and then swaps**
(`console_model.cpp:231-234`), so on the second iteration `records` still held the first iteration's
haul and the drain aborted. The first fix inverted the two lines to `clear(); take();` — which
removes the cross-iteration abort but **inverts the house idiom** (`take(); clear();`, used four
times in `scene_io_test.cpp` at `:393-394`, `:424-425`, `:474-475`, `:512-513`) and leaves a worse
bug in its place: `take()` swaps the pre-open drain INTO `records` and nothing removes it, so the
second `take()` asserts against a non-empty vector. In Debug that aborts all 304 cases; in Release
`NDEBUG` compiles the assert out and the residue is silently **counted**, failing the INFO/WARN/ERROR
checks for a reason unrelated to `openSceneFile`. The correct fix, applied in the review round, is
loop-scoped `records` **plus** the house take-then-clear order: the first restores the iteration
boundary, the second discards the drain. Neither alone is sufficient. **The idiom is take-then-clear
— do not "tidy" it back.**

**The code-review round (2026-07-31): six findings, one blocking, all fixed on the same branch.**
The implementation pass's own claim that the work was ready to merge was wrong — the same mistake
2.5.1's implementation pass made, and worth recording twice for that reason.

1. **BLOCKING — EG6's inverted log drain**, above. The only finding that could redden or abort CI.
2. **An unguarded index that could abort instead of failing.** G5's grandparent walk indexes
   `doc.entities[rec.parent - 1U]`, which is in range only because ids are contiguous `1..N`. That
   precondition was a `CHECK` (reports and continues) sitting in a *different* loop from the index,
   and `parseScene` guarantees only that a parent matches **some** record's id
   (`scene_format.cpp:202-205`), never that it is `<= size()`. A hand-edited or future
   persistent-id fixture would therefore read past the end of the vector: an ASan
   heap-buffer-overflow abort on both Debug lanes, silent UB in Release — in exactly the scenario the
   pin exists to diagnose. Promoted to `REQUIRE`.
3. **Three assertions that could not fail.** EG2's `isClean()`/`count() == 0` and EG3's `isClean()`
   ran against an `EditorFixture` constructed fresh inside the loop, so the stack was *already* clean
   before the operation under test — they would pass with the clearing removed entirely. This is the
   precise vacuity failure the whole task exists to forbid, shipped inside it. EG2 now dirties the
   stack first (the `scene_io_test.cpp:383-391` idiom), which makes AC-15's "history is clean" clause
   real. EG3's was **deleted** rather than fixed: dirtying the stack means mutating the World, and
   EG3's entire purpose is that the saved bytes still equal the fixture's, so the assertion cannot be
   made discriminating there at all. The property is owned by the IO cases and by `I14`.
4. **`.gitattributes` did not cover one of the four files the batteries compare.** `*.scene.json`
   does **not** match `samples/phase-1-scene/scene.json` (verified with `git check-attr`: it fell
   through to the blanket `text: auto`), yet G9 byte-compares that file. The explicit rule's own
   stated rationale was false for it. Given its own line.
5. **`CLAUDE.md`'s new prose broke the roster grep** — a seventh instance of the "comment names the
   policed token" trap. Its state block is one physical line, so the literal fixture path landed
   beside the words "write" and "rename". Reworded to name the directory in prose only.
6. **This log described the EG6 fix backwards**, teaching a future reader the inverse of the house
   idiom. Rewritten above.

Findings 2 and 3 are the ones worth internalising: a golden-test task shipped both an out-of-bounds
read in its own diagnostic path *and* three vacuous assertions. Neither would ever have reddened CI,
which is exactly why neither the implementation pass nor its own sabotage matrix caught them — a seed
proves a test *can* fail, never that an assertion *does* anything.

**What deliberately did not ship.** No regeneration mechanism of any kind. No new ctest entry. No fix
for the destroy-reordering (G10; a format-level decision, unowned). **And no human validation rows:
2.5.2 adds no UI, no menu item, no panel and no keystroke, so there is nothing for a human to click.**
It is recorded as *mechanical only* in `editor/VALIDATION.md` (local-only, gitignored since `035882e`
— see below) so an absent page is not read as a pending gate — and, deliberately, it does **not**
carry a row re-running 2.2.5's four BLOCKED log-panel rows. That clause has now been written into
three consecutive tasks (2.4.2, 2.5.1, 2.5.2) and performed by none of them; attaching it to a task
with *zero* other human rows would be the worst version yet. It needs a session of its own.

**Two spec corrections found at the tree, recorded rather than silently absorbed (both in the plan's
own §A, reconfirmed here).** The spec's AC-27 asks for an `editor/VALIDATION.md` row as a committed
deliverable, but that file and `editor/validation/` were **untracked and gitignored** by `035882e`
the same day the spec was written — the edit is made and is real
(`git status --porcelain --ignored -- editor/VALIDATION.md editor/validation/` shows both `!!`), but
it appears in no commit and no CI run. And the spec's AC-7 grep (`bless|update[_-]?golden|…`) is
**already non-empty at HEAD** by four pre-existing prose uses of the English word, so it can never be
a gate; it was replaced by three greps that are (an `AERO_GOLDEN` roster, a `fixtures/scenes` roster,
a scoped `getenv`/`$ENV{}` scan) plus a stability check that the bare-`bless` count stays at 4 — all
four reconfirmed here. One further correction found only during this pass: the `fixtures/scenes`
roster grep matches the **literal substring**, and the two new `.cpp` files reference the
`AERO_GOLDEN_SCENES_DIR` **macro**, not that literal path text — so the roster reads **one** file
(`tests/CMakeLists.txt`), not the three the plan's own §V7 predicted. INV-1's actual property (nothing
writes a fixture) still holds and is proven by the separate `ofstream|write|remove|rename` grep,
which stays empty either way.

**Inventory, measured at every commit boundary.** `ctest -N` **94 → 94** (tools ON), **5 → 5**,
**18 → 18** — no new `add_test`. `aero_scene_serialize_test` **12 → 16 → 23** (G1–G4, then
G5/G5b/G6/G7/G8/G9/G10 — seven physical cases, not six; see finding v). `aero_editor_shell_test`
**296 → 304** (EG1–EG8), and **280 → 280** in `build/reflect-off-2.5.2`, where
`scene_golden_test.cpp` is confirmed **absent from the build** (no `.o`, no `add_test`, no
`aero_scene_serialize_test*` binary at all), not skipped. `aero_tests` unchanged at **363**;
`aero_editor_imgui_test` at **41**; `aero_editor_inspector_test` at **22**. `check-math-boundary.sh`
**232 → 233 → 234** (+`tests/scene_golden_support.hpp` at the Step-2 boundary,
+`tests/editor/scene_golden_test.cpp` at the Step-4 boundary; the three `.json` fixtures never move
it — `EXTS` is C-family only). All five guards green with no allowlist change. Both macOS presets
green at every commit boundary, Debug and Release, with and without `AERO_REQUIRE_GPU=1`;
clang-format and clang-tidy clean on all three touched/new C-family files
(`tests/scene_golden_support.hpp`, `tests/scene_serialize_test.cpp`,
`tests/editor/scene_golden_test.cpp`) — one clang-format pass was needed after hand-authoring
(comment/vector-literal alignment and one splice-safe hex-escape line), accepted as ordinary
formatting, not a finding; **zero new `NOLINT`** against a `tests/` baseline of 9 (still 9 after).

### Epic 2.6 — Project system v0

#### Task 2.6.1 — `project.json` + create/open flow — OPENS Epic 2.6

**2.6.1 gives the editor a project, not just a scene.** `project.json` v1 (5 root keys in a fixed
order, `paths.assets`/`paths.scenes` nested) with a pure parse/write/validate half
(`editor/src/project.cpp`) and a `<filesystem>`-and-SDL half (`editor/src/project_file.cpp`) that
scaffolds a project on disk, loads one, and maintains a versioned `recent_projects.json`. A New
Project modal and a Welcome window (shown iff no project is open) both route through the *existing*
unsaved-changes guard — `FileAction::NewProject`/`OpenProject` join `NewScene`/`OpenScene`/`Quit` in
`discardsWork`, the **single** pure-function-body change this task makes (D1). `adoptProject` is the
sole call site of `ProjectSession::set()` (INV-P1) and always calls `newScene` first (INV-6), so a
project swap can never leave a stale World/Selection/RootOrder/CommandStack behind — the identical
invariant 2.5.1 established for a scene swap, applied one layer up. The Asset Browser's root is kept
in sync by a **reconcile, never push** comparison inside `EditorApp::tick()` (D10): one
`std::string` comparison per frame, not a second write path the swap itself could half-perform. Nine
feature commits, `docs/09-file-formats.md` §4 documents the format.

**`engine/` diff is empty (AC-36) — this task needed no engine change at all**, the identical property
2.5.1 established for scene I/O, now true of the project layer too. Both `project.hpp` and
`project.cpp` are free of every build gate (D4/AC-35/INV-P5): the project flow reads and writes no
scene, so it never touches the engine's serialization bridge, which is why `project_test.cpp`'s 53
cases are present and passing in **all three** build configurations — unlike 2.5.1's/2.5.2's editor
test TUs, which are absent from the reflect-OFF build by design.

**Inventory, measured at every one of the nine commit boundaries, not once at the end.**
`aero_editor_shell_test` **304 → 327 → 337 → 346 → 357 → 356 → 356** (Step 2's 23 project cases,
Step 3's +10, Step 4's PG1-PG9 golden battery +9, Step 5's +11, Step 6's −1 for the deleted
`resolveProjectRoot` case, Step 7 unchanged, Step 8 unchanged) — **356 at HEAD**;
`aero_editor_imgui_test` **41 → 46** (Step 8's I18/I21–I24, exactly the plan's own predicted +5).
`ctest -N` unchanged at **94** throughout. `check-math-boundary.sh` **239 → 241** (Step 7's
`project_ui.hpp`/`project_ui.cpp`, its only movement). Both tools-OFF configurations verified from
scratch: `build/tools-off-2.6.1` **5/5**, `build/reflect-off-2.6.1` **18/18**, both reading **332**
`aero_editor_shell_test` cases including all 53 `project:`-prefixed ones — the project format rides
no build gate at all, confirmed directly rather than assumed.

**Three self-referential comment traps found and fixed during implementation, the same standing
class §A26/2.5.2's log already names three times over.** A comment saying "the `EditorApp::create(`
count" collided with its own §V7 grep; a comment spelling `restoreLastProject = false` inside a
paragraph explaining that every literal below sets it collided with the count-equality grep counting
that exact literal; a comment in `project_file.cpp` reading "No `remove_all`, no rollback" collided
with INV-P4's own grep for that identifier. All three reworded without repeating the policed token;
no gate was weakened.

**A real implementation bug found and fixed before any commit: `parseProject`'s unknown-key sweep
was two separate loops, not one.** The first draft collected root-level unknowns and `paths`-nested
unknowns in two passes, producing `{"author", "editorLayout", "prefabs"}` for the golden
`unknown-keys.project.json` fixture — wrong order. Cross-checked against the golden battery's own
expected order `{"prefabs", "author", "editorLayout"}` (§4.1/§4.7's document-order rule) and rewritten
as a single depth-first walk that collects a `paths`-nested unknown at the position `paths` itself
occupies among the root members. Found during Step 2's implementation, before the first commit — not
a sabotage finding, a genuine pre-commit bug.

**A second real bug found and fixed during Step 5: `project.flow.requestedPath` wasn't cleared
alongside `flow.requestedPath` on every abandoning path** — the same BLOCKING-1 leak class 2.5.1's
code-review round found for the scene flow's own `requestedPath`, recurring one layer up because the
project flow duplicates the same field shape. Fixed by adding `project.flow.requestedPath.clear()` at
the five places `flow.requestedPath.clear()` already runs.

**Sabotage: all 22 seeds run, plus all 3 mandatory second-order checks (S1, S9, S11) — every one
confirmed landed with `git diff` (or `git check-attr`/regenerated-fixture inspection for the two
data-only seeds) before trusting a verdict, every one reverted and reconfirmed green.** Full
predicted-vs-measured table lives in `editor/validation/2.6.1-project-json-create-open-flow.md`
(local-only); the findings below are the ones worth carrying forward.

**Two seeds proved *completely undetected* by the whole suite — a real coverage gap, not a
non-discriminating-by-construction result like S6/S20/S21.**

(i) **S11 (a `remove_all` rollback added to the `WriteFailed` branch) reddens nothing — 100% pass,
94/94, both default and `AERO_REQUIRE_GPU=1`.** The `WriteFailed` branch of `createProject` is
structurally unreachable by any test in the tree: the plan's own suggested seeding recipe (pre-create
`project.json` or `project.json.aero-tmp` inside the target) was tried first and measured to trip
step 3's `TargetNotEmpty` gate before step 4 ever runs — any pre-existing entry, whatever its name,
makes the adoption check see a non-empty directory. The nearest reachable proof of D7/INV-P4 is
`CreateFailed` at the "assets" step (case 30, via a chmod'd read-only target), which this seed does
not touch at all. D7/INV-P4's "nothing removed" property is therefore **completely unproven for the
`WriteFailed` branch specifically** — carried forward as an open item, not fixed here (fixing it would
mean inventing a new way to reach that branch, a design question, not a one-line change).

(ii) **S15 (`create()` reads recents even when `restoreLastProject == false`) reddens nothing — 100%
pass, both tiers.** I24 only asserts `projectIsOpen()`/`projectRoot()`, both gated by whether recents
is *used* (a separate, unaffected `if (resolved.empty() && config.restoreLastProject)` check), never
by whether recents is *read*. `app.recents` has no public accessor, so no test can observe the read
itself. A real accessor gap, not a false alarm from this task's own tests.

**Four seeds deviated from their table prediction by under-catching — a case named in the plan does
not actually discriminate the seed.** S5 (`promoteRecent` appends instead of prepending): only case
19 reddens, not the recents-count case the table also named, because that case's single
`promoteRecent` call cannot distinguish append-vs-prepend order with one element. S12
(`discardsWork` omits `NewProject`): only the tier-0 case reddens; the GPU-gated I22 the table also
named tests `OpenProject`, not `NewProject`, so it is untouched by this specific seed. S14
(`EditorApp::tick` drops the Asset Browser reconcile): only I21 reddens; I18's panel is born with the
correct root at *construction*, so the reconcile the table also named is a no-op for that scenario
and never runs before I18's own assertion. S10 (`createProject` writes the manifest before creating
the directories): the table's own case 26 only checks final end-state, never call order, so a
reordering that still succeeds end-to-end passes it silently — the only actual discriminator is case
30, and via a different assertion than predicted (`CreateProblem::WriteFailed != CreateProblem::
CreateFailed`, not the file-lands-in-a-missing-directory mechanism the table described).

**One seed over-caught (collateral beyond the named case, a stronger result).** S17
(`projectRootFromPath` fails to strip `project.json`) reddens the named case **and** `loadProjectFrom
reads, validates and reports`, a call site that passes a `project.json` path directly and inherits the
break.

**S9 (the mutual bug, 2.5.2's S12 re-proven for this format) confirmed the design, with two nuances
the table's summary line did not capture.** Both halves of `parseProject`/`writeProjectText` were
made to stop handling `paths.scenes`, `minimal.project.json` was regenerated from the seeded build
(128 bytes, down from 152), and the whole suite was run before any revert. Before regenerating: **11
cases red**, proving the mutual bug is real end to end. After regenerating only `minimal.project.json`
(the plan's own instruction): **PG6 and PG7 stayed red** (they read `full.project.json` and
`unknown-keys.project.json`, neither regenerated) but **PG5 and PG8 went green** — masked, because
`minimal.project.json` is exactly the one fixture the seeded writer's own bytes now agree with, and
`ProjectManifest::scenesPath`'s default ("scenes") happens to coincide with the value the un-seeded
fixture held, so PG5's field-by-field read cannot tell the difference. **PG2 stayed red too, but not
because the masking failed** — it iterates over *both* fixpoint fixtures in one case, and
`full.project.json`'s honest, unrelated mismatch keeps it red regardless of `minimal.project.json`'s
state. The design still holds — PG6/PG7 are the un-maskable discriminators, exactly as 2.5.2's S12
established — but "regenerate the one fixture nearest the bug" turned out to partially mask even a
semantic case, which the redundancy of checking multiple independent fixtures in one battery (PG1,
PG2) happened to catch by a different route. Worth remembering: single-fixture regeneration can mask
a semantic case that reads *that* fixture, even when the design intends semantic cases to be
un-maskable — the mitigation here was accidental (PG2's multi-fixture loop), not by design.

**One minor prose deviation confirmed, not a defect.** S19 (removing the `project.json` line from
`.gitattributes`) is confirmed Windows-only in effect via `git check-attr --all`, but the local
`text` attribute flips from `set` to `auto` — `eol` stays `lf` in **both** states (the blanket `*
text=auto eol=lf` rule already covers it), not "unspecified" as an earlier draft of the plan's prose
stated.

**What was deliberately not built.** No fix for S11's `WriteFailed`-unreachable gap or S15's
`app.recents` accessor gap — both are real, both are carried forward as open coverage debt, neither
is a defect in code that shipped (D7/INV-P4 still hold; they are simply unproven by any *test*, which
is a different claim). No CI script enforces INV-P1 (`ProjectSession::set()`'s single-call-site
invariant) or AC-46 (a panel never mutating project state directly) — both are architectural
properties held by discipline and comment, the same posture 2.5.1 took for AC-27's hand-bound Esc.

**No separate code-review round was run for this task**, unlike 2.4.1/2.4.2/2.5.1's precedent — the
two real bugs found (the unknown-key sweep order, the `project.flow.requestedPath` leak) were both
caught and fixed during the implementation pass itself, before their commits, not in a later pass.

#### Task 2.6.2 — Project settings panel stub — CLOSES Epic 2.6

**2.6.2 gives the editor a way to LOOK at the project 2.6.1 gave it.** A dockable, strictly read-only
`Project Settings` panel bound to the open project's parsed manifest, plus one `Edit ▸ Project
Settings…` menu item and the one `PanelContext` member (`const ProjectSession& project`) that lets
every present and future panel read the open project without being able to write it. This closes
Epic 2.6, and with it every Phase 2 epic is closed in code.

**Two plan-time corrections worth restating exactly, because both are "the plan was right and the
spec was stale" findings, not implementation surprises:**
- **§A1 — `AERO_ENGINE_VERSION` had TWO read sites at HEAD, not the ONE the 2.6.1-era spec recorded**
  (`editor_app.cpp`'s `create()` and `tick()`, both reading the macro directly). The task's own D14
  hoist (`BUILD_ENGINE_VERSION`, a `constexpr std::string_view` read once) is not a tidy-up — it is
  what turns "two macro reads" into "one read, two consumers" before a third consumer (the settings
  panel's constructor argument) could make it three reads. The gate is the comment-stripped,
  directive-excluded grep (`sed -E 's|//.*||' | grep -vE '#[[:space:]]*(ifndef|define)'`), which reads
  **2 at HEAD, 1 after** — a raw, unstripped grep can never read `1` because the `#ifndef`/`#define`
  fallback pair and the two prose comments both spell the macro's name.
- **§A3 — the two on-disk `tools-off-2.6.1` build directories were STALE and read `5/18/332` against
  a true `6/19/337`**, because they were configured before 2.6.1's own code-review round added the
  sixth architecture guard's ctest entry. Fresh directories (`build/tools-off-2.6.2`,
  `build/reflect-off-2.6.2`) were configured from scratch and re-measured before any code was written
  — a stale build directory had already produced one wrong number in this task's own planning
  investigation, which is the exact "vacuous baseline" shape this project's standing lesson names.

**F1 in full, because it is the one fact in this task most likely to be re-derived wrongly from
memory rather than read from source.** The Edit-menu item's `ImGui::SetWindowFocus(name)` selects a
docked window's TAB, but not through the mechanism `FocusWindow`'s own comments suggest: the "Select
in dock node" block inside `FocusWindow` is **commented out** in vendored ImGui 1.92.8
(`imgui.cpp:13763-13766`, tagged for issue #2304). The real selection happens one indirection away, in
`DockNodeUpdateTabBar` (`imgui.cpp:19611-19613`): `if (g.NavWindow && g.NavWindow->RootWindow->DockNode
== node) tab_bar->SelectedTabId = g.NavWindow->RootWindow->TabId;`. That resolves correctly for
`SetWindowFocus` only because a **docked** window keeps `RootWindow == window`
(`UpdateWindowParentAndRootLinks`, `imgui.cpp:7759-7768`, the `!window->DockIsActive` guard at
`:7766`). `DockSpaceOverViewport` — where dock nodes actually update — runs LATER in the same frame
than the menu bar (`shell_ui.cpp`'s `drawMenuBar` → the two modals → `applyFileRequests` →
`applyHistoryRequests` → `DockSpaceOverViewport` → `drawPanels`), so the menu item's focus write lands
with no one-frame lag. No test tier in this tree can read a dock tab's `SelectedTabId`, so this whole
mechanism is proven only by reading the vendored source and by human validation rows 5-6.

**D6's limitation, stated as intended behaviour, not filed as a future bug:** the panel is bound to
the **parsed, in-memory** `ProjectSession`, never to `project.json` on disk. Hand-editing the file
while the editor runs does **not** update the panel — there is deliberately no Reload button, because
a reload would need a mutable path to the session's setter, which INV-P1 restricts to exactly one call
site (`adoptProject`), and routing a panel through it would silently discard the user's scene without
the unsaved-changes guard a panel may not bypass. The documented way to pick up an external edit is
`File ▸ Open Project…` on the same root — which goes through the guard and does reset the scene, an
honest cost recorded rather than hidden.

**Two more known-and-expected behaviours, both surfaced by the code-review round rather than by the
sabotage pass, both cosmetic and neither filed as a bug.** First, **AC-17's "column 0 at the same
width in both groups" holds until the user drags a divider, not forever**: `TABLE_FLAGS` sets
`ImGuiTableFlags_Resizable` deliberately (a user may want a wider label column) and the per-group
`PushID(g)` keeps the two tables distinct entities, so resizing `Manifest`'s label column does **not**
move `Location`'s. The groups are aligned as shipped and after any relayout; they are not *locked*
together. Second, **`widestLabel()`'s per-frame recompute does not re-widen the column on a runtime
font-scale change** — `TableSetupColumn`'s init width reaches `WidthRequest` only through
`TableInitColumnDefaults`, which runs under `if (table->IsInitializing)` (`imgui_tables.cpp:1693-1698`),
true only on a table id's first frame; the one per-frame path that would re-apply it (`:937-938`) is
gated on `!column_is_resizable` and is therefore excluded by our own `Resizable`. Consequence is mild
— labels **wrap** via `TextWrapped` rather than clip, and the column stays user-resizable — but E19
should be read as *tolerated*, not *mitigated*, and the panel's comment was corrected to say so.

**F2/D10, the reason the model returns GROUPS rather than a flat row list with a separator flag:**
`ImGui::Separator()` only promotes to `SpanAllColumns` for the legacy `Columns()` API
(`imgui_widgets.cpp:1726-1741`); inside a `BeginTable` cell it spans only that cell. The panel therefore
calls `SeparatorText` OUTSIDE any table and draws one table per group, both tables sized from one
shared `widestLabel()` pass so the two groups' label columns align as if they were one table.

**D14, stated honestly rather than papered over with a second code path:** the Edit-menu item has NO
automated coverage at all. Its whole observable effect is `PanelRegistry::setVisible` (which a test CAN
read) plus `ImGui::SetWindowFocus` (a dock tab's `SelectedTabId`, which no test tier in this tree can
read). A `requestProjectSettings()` `EditorApp` hook was considered and rejected — it would only
duplicate the half that is already testable, while adding zero coverage for the half that matters.
Human validation rows 5-7 are its only proof, by design.

**The full sabotage matrix — all 22 seeds plus the 3 mandatory second-order checks, run sequentially
against `build/macos-debug` with `AERO_REQUIRE_GPU=1`, each with its edit confirmed landed via `git
diff` before building and confirmed reverted-and-green after:**

| # | Predicted | Actual | Deviation |
|---|---|---|---|
| S1 | PS1, PS2 | PS1, PS2 | none |
| S2 | PS17 | PS17 | none |
| S3 | PS16 | PS16 | none |
| S4 | PS18 (PS19 flagged as possible) | PS18 **and** PS19 | none (flagged possibility confirmed) |
| S5 | PS19 only | PS19 only | none |
| S6 | PS14, PS15 | PS14, PS15 | none |
| S7 | PS10, PS11, PS12 (not PS4) | PS10, PS11, PS12; PS4 stayed green | none |
| S8 | PS20 | PS20 | none |
| S9 | PS3, PS4, PS5 (PS22 stays green) | PS3, PS4, PS5 **plus** PS6, PS7, PS8, PS9 (index-shifted reads), **then a SIGABRT crash in PS10** (out-of-bounds `rows[4]`/`rows[5]` on a 2-row vector) that halted the whole `aero_editor_shell_test` binary before PS11–PS23 (incl. PS22) could run at all | **major over-catch**: the plan's "PS22 stays green" could not even be observed — the process aborted first **RE-RUN AFTER THE CODE-REVIEW ROUND'S `shapedGroups` FIX:** clean failure, no crash — 384 cases run, 367 pass, **17 fail**, and PS22 **correctly stays GREEN** (a swap moves rows, it does not lose one, so the total is still 8). The asymmetry §T claimed for PS22 is now DEMONSTRATED rather than merely asserted. |
| S10 | PS4, PS6, PS22 | PS4, PS6 as predicted, **plus** PS7, PS8, PS9 (index-shifted reads), **then an ASan container-overflow SIGABRT in PS10** (`rows[4]`/`rows[5]` out of bounds on the now-5-row group), halting the binary before PS22 could run | **major over-catch**, same shape as S9: a row-count-changing sabotage in this model cascades past its "intended" blast radius and crashes the binary rather than staying contained **RE-RUN AFTER THE CODE-REVIEW ROUND'S `shapedGroups` FIX:** clean failure, no crash — 384 cases run, 368 pass, **16 fail**, and **PS22 now RUNS and reddens (`CHECK( 7 == 8 )`)**. Paired with S9 above, this is the first actual proof that PS22 discriminates a row that VANISHED from one that merely MOVED. |
| S11 | nothing (non-discriminator by construction — `PROJECT_FORMAT_VERSION` is 1 today) | nothing | none |
| S12 | PS10, PS11, PS12 | PS10, PS11, PS12 | none |
| S13 | I25 only (`panelAt(5)`/`panelAt(1)`) | I25 only, both `CHECK_EQ`s; tier-0 stayed green | none |
| S14 | nothing (real gap, human row 1 only) | nothing **at the time of the sabotage pass** | **CLOSED by the code-review round.** `imgui_layer_test.cpp` now asserts `panelAt(5).defaultDockSlot() == DockSlot::Right` (plus the four `options()` defaults) — `panelAt()` hands back a `Panel&` and both accessors are public, so no src-private header is needed. Re-run with the seed applied: I25 fails `CHECK( 0 == 2 )`. AC-15's dock-slot clause is no longer human-row-only |
| S15 | nothing mechanically (R-S1's gap; the exactly-3 grep is the only guard) | nothing mechanically; the grep's count stayed 3 but the first hit's argument became `text.c_str()` instead of `"%s"` — confirms the grep must be INSPECTED, not merely counted, exactly as designed | none |
| S16 | I25 aborts (`IM_ASSERT`, F9) | **NO abort — all 95 tests pass, I25's 26 assertions all pass.** Hoisting `EndTable()` outside the `if` is behaviourally IDENTICAL to the original on every path this suite exercises | **confirmed deviation, and the code-review round corrected its CLASSIFICATION**: this is a non-discriminator **by construction**, the S11 category — not new coverage debt. `BeginTableEx` has exactly two `return false` sites in the pinned 1.92.8 source. The first (`imgui_tables.cpp:323-324`) requires `outer_window->SkipItems`, which is false **by construction** here: `shell_ui.cpp`'s `drawPanels` calls `onDraw` only when `ImGui::Begin` returned true, and `Begin` returns `!window->SkipItems` (`imgui.cpp:8798`). The second (`:348`) requires `use_child_window`, i.e. `ScrollX\|ScrollY` — and `TABLE_FLAGS` deliberately sets neither (the panel's own window scrolls). So `BeginTable()` cannot return `false` for this panel **at all**, not merely "not in I25's window", and **no test change can close this** — it would require changing the product (adding a scroll flag, or calling `BeginTable` outside the registry's `Begin` gate). Do not file this as debt for a future task to work on |
| S17 | I25 aborts, specifically in step 3's no-project ticks | I25 aborts exactly there — `IM_ASSERT`: "In window 'Project Settings': Missing EndTable()" — 46 cases in that binary skipped as a result | none |
| S18 | "likely nothing mechanically" (an open question the plan deferred to implementation) | **resolved: nothing mechanically.** I25 passes clean (26/26 assertions, no abort, no `[imgui-error]` log line, unlike S17). ImGui silently MERGES the two same-`id` tables rather than asserting — multiple instances of one table id is a *supported* feature: `BeginTableEx` assigns `instance_no = (previous_frame_active != g.FrameCount) ? 0 : table->InstanceCurrent + 1` and proceeds | plan's own open question, now answered definitively. **Caveat added by the code-review round: human row 4 is NOT a reliable catch for this seed.** ImGui's multi-instance tables share column widths, and both groups are already initialised from the same `labelWidth`, so the merged rendering is most likely *indistinguishable* from the correct one. `PushID(g)` is still right — it is what keeps the two tables independent entities — but nothing in this tree, human or mechanical, reliably observes its absence |
| S19 | nothing (human rows 5-6 only) | nothing | none |
| S20 | nothing (human row 7 / E4) | nothing | none |
| S21 | with `const`: probe fails to compile; without `const`: probe compiles | **both halves confirmed** — with `const`, `error: 'this' argument to member function 'set' has type 'const ProjectSession', but function is not marked const`; without `const`, `project_settings_panel.cpp` (the probe's own TU) builds clean. Side note, not part of the seed's own claim: removing `const` from `PanelContext::project` also broke `shell_test.cpp`/`hierarchy_test.cpp`, which bind `const ProjectSession` locals to the aggregate — collateral evidence of how structurally deep the enforcement runs | none against the seed's stated claim |
| S22 | PS23, I25's `panelAt(5).id()` | PS23, I25's `panelAt(5).id()` | none |

**Second-order checks, all three, for every seed without exception:** (1) seed presence confirmed by
reading the `git diff` hunk before every build, never trusted from memory; (2) revert-and-green
confirmed after every single seed — no seed left the tree red; (3) predicted-vs-actual recorded
honestly, including the four real deviations above (S9, S10, S16 real gaps/over-catches; S18 the
plan's own flagged unknown, now resolved).

**Net finding beyond the plan's own predictions**: this task's real, load-bearing coverage gap is not
where the plan's own worked examples (S14/S15/S19/S20, all UI-only) said it would be — those all
landed exactly as predicted. The two genuine surprises are **S9 and S10**, which reveal that
`projectSettingsGroups`'s fixed-shape assumption (group 0 always has 6 rows, group 1 always has 2) is
enforced by NO code, only by every test's own index literals — a group-count or row-count sabotage
does not stay contained to the row(s) it directly damaged, it cascades into every later index-based
assertion and eventually crashes the binary via an out-of-bounds vector access, hiding whatever the
crashed case (PS10) or later cases (PS22 in S9's case) would otherwise have shown. This is real and
useful information about the model's structure, not a defect in the model — the eight-row, two-group
shape is exactly what D7 specifies — but it means a future append to this model (a ninth row, a third
group) should add its OWN index-scoped assertions rather than relying on the existing ones to catch a
regression safely.

**Measured inventory, all re-measured on the finished tree, never inferred:** `ctest -N` **95 / 6 / 19**
unchanged (AC-33); `aero_editor_shell_test` **361 → 384** (+23, exactly PS1–PS23), reflect-OFF and
tools-OFF both **337 → 360** (the identical +23, D4/AC-14's whole point); `aero_editor_imgui_test`
**46 → 47** (+1, I25); `aero_tests`/`aero_editor_inspector_test`/`aero_scene_serialize_test` all
unchanged at **363 / 22 / 23**; `check-math-boundary.sh` **241 → 246** (+5: `project_settings.hpp`,
`project_settings.cpp`, `project_settings_panel.hpp`, `project_settings_panel.cpp`,
`project_settings_test.cpp`), measured after `git add` at every step boundary; `aero_editor_core`
sources **37 → 39**; default panels registered **5 → 6**; `AERO_ENGINE_VERSION`'s comment-stripped,
directive-excluded read-site count **2 → 1** (§A1); `NOLINT` under `tests/` and `editor/` **unchanged
at 9 / 4**; `target_link_libraries`/`find_package`/`vcpkg.json`/the `/vcpkg` submodule SHA all
byte-identical; `git diff --name-only origin/main -- engine/` **empty for a FOURTH consecutive task**
(2.5.1, 2.5.2, 2.6.1, 2.6.2); `docs/09-file-formats.md` byte-identical (AC-32).

**One process deviation from the plan, recorded honestly.** Step 1's commit was made from a stale
`git add` snapshot taken before a post-hoc clang-tidy fix (`misc-const-correctness` wanted several
`ProjectSession session` test locals declared `const`); the fix was applied to the working tree,
verified, but never re-staged before `git commit -m`, so the committed Step 1 diff briefly lacked the
fix. Caught immediately by the mandatory `git status --short` check after the commit. Fixed with a
small, separate, immediately-following commit (never an amend, per this project's git discipline) —
`fix: declare project_settings_test.cpp sessions const (clang-tidy misc-const-correctness)` — rather
than folding it invisibly into Step 1's history. The lesson restated for next time: `git add -A` a
second time, immediately before every commit, is not optional even when nothing "should" have changed
since the first one.

**Unowned, carried forward, untouched by this task:** 2.5.2's entity-renumbering/forward-`parent`
format question (`docs/09` §2.7) is still open. 2.6.1's S11 coverage gap (`createProject`'s
`WriteFailed` rollback branch, unreachable by any test, held instead by
`check-project-no-delete.sh`) and its `app.recents` accessor gap are unchanged. This task's own two
real, recorded gaps were both **re-classified or closed by the code-review round**, and neither is
carried forward. **S16 is NOT debt** — `BeginTable()` is unreachable-by-`false` for this panel by
construction (see its row in the sabotage table above for the two source citations), so it belongs in
S11's non-discriminator-by-construction bucket and no future task should spend effort on it.
**S9/S10's gap is CLOSED**: `project_settings_test.cpp` now routes PS6–PS20 through a `shapedGroups`
helper that `REQUIRE`s `groups.size() == 2`, `rows.size() == 6` and `rows.size() == 2` before any
positional read, so a shape-changing regression fails cleanly in the case that owns it instead of
running off the end and aborting the binary before PS22's independent eight-row sum can run — which
is exactly what cost this task's own sabotage pass the demonstration of PS22's discrimination.

#### Follow-up — newly-registered panels are docked on a RESTORED layout

**Found by the project owner on their own machine, minutes after 2.6.2 merged, and not by any test or
validation row.** After creating a project, `Project Settings` appeared as a free-floating sliver of a
window on top of the Hierarchy panel — narrow enough that "Format version" rendered one letter per
line. The Console line said everything: `shell ready (6 panels, 3 entities, layout: restored)`.

**Cause, and it was never 2.6.2-specific.** `buildDefaultLayout` is the only reader of
`defaultDockSlot()`, and `imgui_layer.cpp` only asks for it when there is no ini to restore
(`wantsDefault = !(persistLayout && exists(iniPath))`). Every machine that had already run the editor
restores instead, and a restored ini written before the panel existed has no `[Window][Project
Settings]` entry at all, so ImGui falls back to a floating window. **Every panel any later task adds
would have landed the same way** — Phase 3's importers, 4.6's language surface, Phase 7's 2D tools.

**Why no gate caught it.** 2.6.2's own human validation row 1 asserts the tab "sits beside Inspector
in the right dock", and it would have PASSED on a clean checkout, because a fresh tree has no
`aero_editor.ini` and therefore takes the default-layout path. The bug is only reachable on an install
with history — which is every real user and no CI lane. A validation row that a fresh machine cannot
fail is not a validation row for this class of defect.

**Fix.** `placeUnplacedPanels` (`shell_ui.cpp`), a one-shot on the first drawn frame, seeded as the
exact complement of `applyDefaultLayout` and `else if`-exclusive with it. It docks **only** panels
whose id has no settings entry (`FindWindowSettingsByID(ImHashStr(id))`), into the node already
hosting a panel that declares the same `defaultDockSlot()`, read from that sibling's
`ImGuiWindowSettings::DockId`; the dockspace root is the fallback. Reading SETTINGS rather than the
live window is mandatory: on the first frame no panel has been submitted, so `FindWindowByName` would
return null for every one of them and the pass would "place" panels the user had already positioned.
Joining a slot-mate's *current* node rather than re-splitting is what keeps a customised layout
untouched — and if the user moved Inspector to the bottom, the new panel belongs beside it there, not
where a fresh layout would have put it.

**`EditorAppConfig::layoutIniPath` is new, and is the `recentProjectsPath` lesson arriving a second
time.** The ini path was hardcoded to an exe/pref-relative location, so the restore path was
untestable: any test setting `persistLayout = true` would read and then overwrite the developer's real
editor layout — 2.6.1's BLOCKING-2 in a new costume. Empty keeps the shipped derivation.

**I26** proves it black-box, without ImGui: it writes a REAL pre-2.6.2 ini captured from the owner's
machine (five windows, no `Project Settings`, node `0x00000004` holding Inspector), runs the editor
against it, quits so `SaveIniSettingsToDisk` fires, then reads the file back and asserts
`[Window][Project Settings]` now carries a `DockId` **equal to Inspector's** — compared to each other,
not to a literal, so it survives ImGui renumbering nodes. The DockId lookup is deliberately BOUNDED to
its section: an unbounded `find` would return the *next* section's DockId exactly when this section
has none, i.e. the bug would supply the value that makes the test pass. Seed-proven both ways —
disabling the fix reddens I26, reverting greens it. `aero_editor_imgui_test` **47 → 48**.

#### Follow-up 2 — the predicate was wrong: dock what is NOT DOCKED, not what is unknown

The first fix above shipped, merged, and **did not fix the machine it was written for**. Reported
immediately with a screenshot: the panel was still an 81px floating sliver over the Hierarchy panel.

**Cause, and it is a lesson about choosing a predicate.** That fix asked *"has the ini heard of this
panel?"* and skipped it if so. But a panel born floating **writes a settings entry on quit** —
`Pos=80,53 / Size=81,593 / Collapsed=0`, with no `DockId` line at all, because ImGui omits the key
when the id is 0. So on the very next launch the ini *had* heard of it, the predicate skipped it, and
the bad placement was faithfully restored forever. The fix therefore only ever helped installs that
had **never run the build that introduced the panel** — precisely the population that did not have
the problem. Everyone who actually hit the bug was excluded by construction.

Confirmed against the reporter's real ini before changing anything: every other registered panel
carried a `DockId`; only `Project Settings` did not. (The other `DockId`-less sections — the dockspace
host, `Debug##Default`, the two modals, an Assets child — are not registered panels and are never
iterated.)

**Fix: one line.** `if (own != nullptr && own->DockId != 0) continue;` — the test becomes *is this
panel docked*, which covers both populations, never-seen and born-floating, in a single condition.
The preserved promise narrows honestly from "a panel the ini knows is never touched" to **"a panel
that IS DOCKED is never touched"**, which is the one that actually matters: an arrangement of docked
panels survives exactly.

**Accepted cost, recorded rather than left to be discovered.** A panel the user deliberately drags out
of the dock is re-docked on the next launch: ImGui records a deliberate float and a never-placed panel
identically, and nothing in the ini separates them. This is the right trade *here* — viewports are
OFF, so a floating panel is trapped inside the main window overlapping its neighbours, which is
strictly worse than docked and is the thing being fixed. Persistent floating, if ever wanted, needs
its own design (an explicit Detach affordance plus somewhere to record the intent), not a looser
predicate. A one-time migration keyed on a persisted marker was considered and rejected as more
machinery than the problem earns.

**I26 now carries both populations in one fixture** — `Project Settings` present but floating
(captured verbatim from the reporter's ini, `Size=81,593` and all), the five older panels docked, and
Hierarchy/Inspector deliberately swapped out of their default nodes. One run proves three things: a
floating panel gets docked, a docked panel is never moved, and the new panel follows its slot-mate to
wherever the user dragged it. Seed-proven against **three** distinct bug classes now — the fix
disabled, a rebuild-on-restore implementation, and the original narrow predicate — each of which
reddens it.

**Standing lesson.** Two rounds were spent because the first predicate was chosen from the *diagnosis*
("the ini has no entry") rather than from the *property that matters* ("the panel is not docked").
Those coincide only on a machine that has never run the broken build — which is every clean checkout,
every CI lane, and no real user.

---

# Part 2 — Build & dependency impact ledger

Per task: what it changed in `vcpkg.json`, `ci.yml`, `cmake/**`, the boundary
guards, target link lines, and the ctest inventory — and, just as often, what it
provably did *not* change.

## Baseline build & CI description (as extracted)

- **Build commands (as of 0.4.2 — skeleton + pinned vcpkg manifest + 3-OS CI + sanitized doctest harness + Tracy profiling + clang-format/clang-tidy enforcement + engine-wide logging + math surface + job system + time & VFS + the platform/SDL3 window & event loop + input + the miniaudio audio-device stub, closing Epic 0.3, plus the RHI abstraction surface / SDL_GPU backend / shader toolchain / shader loading / RHI-boundary guard, closing Epic 0.4, plus the render clear pass and textured cube opening Epic 0.5 (tasks 0.5.1–0.5.3)):** fresh clones need the vcpkg submodule — `git clone --recurse-submodules`, or `git submodule update --init` after a plain clone. Configure with `cmake --preset macos-debug` or `cmake --preset macos-release` (Windows/Linux presets exist but are gated to their host OS by preset conditions), then build with `cmake --build --preset macos-debug` / `cmake --build --preset macos-release`. Run unit tests with `ctest --preset macos-debug` / `ctest --preset macos-release` (host-gated test presets exist for all six platform/config pairs). The three `*-debug` presets build everything with ASan/UBSan via `AERO_ENABLE_SANITIZERS` → `cmake/sanitizers.cmake` (Windows: ASan only — MSVC has no UBSan). The three `*-release` presets build with `AERO_ENABLE_PROFILING=ON` → `cmake/profiling.cmake`, which links the pinned Tracy 0.13.1 client (`Tracy::TracyClient`) into the `aero::profiling` INTERFACE library and defines `AERO_PROFILING_ENABLED`; the wrapper header `engine/core/include/aero/core/profiler.hpp` (the engine's first header) exposes backend-agnostic `AERO_PROFILE_*` macros that no-op when profiling is off. Tracy is dev-builds-only (never in Debug, never in the runtime) — enforced by the option's default-OFF + link-time gating, not just convention. The first configure bootstraps the vcpkg tool and compiles SDL3 from source (needs network; takes minutes; later configures hit vcpkg's binary cache). Dependency pinning invariant: `builtin-baseline` in `vcpkg.json` and the `/vcpkg` submodule commit are the **same SHA** — bump them together, never separately. CI (GitHub Actions, 0.1.3) now configures and builds all six presets on macOS + Windows + Ubuntu on every push to `main` and every PR — with vcpkg binary caching, a `main`-status badge in the README, and a guard that the `/vcpkg` submodule SHA matches `builtin-baseline`; this is the first live proof of the Windows/Linux presets. CI also runs the doctest suite (`ctest`) for Debug and Release on every lane (0.1.4) — a failing test turns the lane red, and the Release lanes now exercise the Tracy client wrappers on all 3 OSes (0.1.5), with the Debug lanes proving zero Tracy linkage by default. `.clang-format` and `.clang-tidy` now live at the repo root, pinned to LLVM 18 (0.1.6): a standalone `lint` job runs `clang-format-18 --dry-run --Werror` over every tracked C/C++ source (fast, parallel, no build), and a step on the Linux Debug lane runs `clang-tidy-18 -p build/linux-debug --warnings-as-errors='*'` over the first-party `.cpp` TUs, reusing that lane's compile DB — the docs/04 naming law (`PascalCase`/`camelCase`/`SCREAMING_SNAKE_CASE`/`lower_case`) is now machine-enforced via `readability-identifier-naming`, first proven by 0.2.1's `Handle`/`SlotMap`, then by 0.2.2's math types and 0.2.4's `log.cpp` (all pass clean).

## Phase 0 — Foundations & First Triangle

### Epic 0.2 — `core`

#### Task 0.2.4 — Logging / spdlog
**0.2.4 (logging) landed**: `spdlog` (1.17.0, default features → fmt 12.2.0) joined the vcpkg manifest; `<aero/core/log.hpp>`'s `AERO_LOG_{TRACE,DEBUG,INFO,WARN,ERROR,CRITICAL}` macros are the engine-wide logging API, format with `std::format` at the call site, and compile out below `AERO_LOG_ACTIVE_LEVEL` (Trace in Debug, Info in Release); spdlog is confined entirely to `engine/core/src/log.cpp` — the `lint` job gained a `git grep` boundary step that fails if any spdlog/fmt type leaks into a public engine header.

#### Task 0.2.2 — Math / GLM
**0.2.2 (math) landed**: `glm` (1.0.3) joined the vcpkg manifest; `<aero/core/math.hpp>` is the umbrella for `Vec2/3/4`/`Mat3/4`/`Quat` and their common ops (ADR-005); GLM is confined entirely to `engine/core/src/math/glm_backend.cpp`. Both backends' PRIVATE linkage rests on the same documented vcpkg limitation above.

#### Task 0.2.5 — Jobs / enkiTS
**0.2.5 (jobs) landed**: `enkits` (1.12, Zlib) joined the vcpkg manifest; `<aero/core/jobs.hpp>` exposes `JobSystem` (an explicit RAII instance — no global), `parallelFor`, and a build-then-submit `JobGraph` DAG keyed by `Handle<Job>`; enkiTS is confined entirely to `engine/core/src/jobs.cpp`, enforced by BOTH a `lint`-job grep (matching `<TaskScheduler.h>`, `<enkiTS/...>`, and `enki::`) and the compile-time probe `tests/jobs_boundary_probe.cpp`. 0.2.5 also **took the `aero::profiling` call and reversed the 0.2.4 note**: `aero_core` now DOES link `aero::profiling`, **PRIVATE** — that gives `jobs.cpp` live `AERO_PROFILE_*` macros in Release while `$<LINK_ONLY:>` keeps Tracy's usage requirements (and vcpkg's shared include root) off every `aero::core` consumer. That was re-verified empirically on both halves: with GLM installed in the Release tree, `aero_math_boundary_probe` still fails with `'glm/vec3.hpp' file not found` while `aero_tests` compiles the same include clean. `profiler.hpp` gained `AERO_PROFILE_ZONE_NAME` (runtime zone rename; needs an enclosing zone macro). Note the R12 hole is **narrower for enkiTS than for glm/spdlog**: enkiTS installs to `include/enkiTS/`, so `<TaskScheduler.h>` resolves only for targets linking `enkiTS::enkiTS` — but `<enkiTS/TaskScheduler.h>` still resolves anywhere the shared root lands, which is why the grep matches both spellings.

### Epic 0.3 — `platform`

#### Task 0.3.1 — Window & event loop / SDL3
**0.3.1 (platform/SDL3 window & event loop) landed**: SDL3 needed no new vcpkg manifest entry (already present for the temporary root probe) — the temporary root `find_package(SDL3 CONFIG REQUIRED)` (`CMakeLists.txt:42-44`) is **gone**, replaced by `engine/platform/CMakeLists.txt`'s own `find_package(SDL3 CONFIG REQUIRED)`, which links `SDL3::SDL3` **PRIVATE** into `aero_platform`; `engine/CMakeLists.txt` gained `add_subdirectory(platform)`, root `CMakeLists.txt` gained `add_subdirectory(samples)`. `<aero/platform/{event,window,context,platform}.hpp>` are confined entirely to `engine/platform/src/platform.cpp`, enforced by BOTH a `lint`-job step (`.github/scripts/check-platform-boundary.sh`, mirroring 0.2.3's math script, NOT a bare grep — SDL has no namespace, so a bare `(^|[^a-zA-Z0-9_])SDL_` grep fires on legitimate documentation prose that already cites real SDL identifiers, e.g. math.hpp's "SDL_GPU"/time.hpp's "SDL_GetTicks", and was proven to already match on `main` before this task; the script strips `//` comments before checking) and the compile-time probe `tests/platform_boundary_probe.cpp` — R12's hole does **not** widen the same way it did for enkiTS: SDL3 installs its headers under the shared `include/SDL3/` root the same way glm does, so both the script and the probe are needed exactly as documented for math. `aero_tests` now links `aero::platform` and `SDL3::SDL3` explicitly (for the white-box `platform_event_test.cpp`, which pushes real `SDL_Event`s through `Context::pollEvent`) — re-verified this does not weaken the sibling `aero_math_boundary_probe`/`aero_jobs_boundary_probe` targets, which still link only `aero::core`. `samples/phase-0-window` is the first populated sample, the first visible artifact: a real window opened via `engine::platform::Context`/`Window`, pumped with `engine::FrameClock`. `tools/reflect-gen` (Phase 1) remains the only unwired item from the original R12 note.

#### Task 0.3.2 — Input
**0.3.2 (input) needed no vcpkg/CI change**: SDL3 was already linked PRIVATE into `aero_platform`, so the new `.cpp` (`src/input.cpp`, SDL-free) and the new test TU (`tests/input_test.cpp`) are picked up by the existing `add_library`/`add_executable` source lists and the `lint` job's `git ls-files` glob with no new `find_package`, no new `target_link_libraries` line, and no new CI step — `check-platform-boundary.sh` and `tests/platform_boundary_probe.cpp` (task 0.3.1) already scanned/compiled `engine/platform/include`, so both were simply extended with input assertions rather than duplicated.

#### Task 0.3.3 — Audio device stub / miniaudio
**0.3.3 (audio device stub) needed one new vcpkg dependency (`miniaudio`, header-only) and enabled the C language project-wide**: `vcpkg.json` gained `"miniaudio"` alphabetically before `sdl3` (the `builtin-baseline`/submodule-SHA pin was NOT touched — the `vcpkg-baseline` CI job still asserts they match); the CMake configure now runs `find_path(MINIAUDIO_INCLUDE_DIR NAMES miniaudio.h REQUIRED)` and adds it `PRIVATE` to `aero_platform`, alongside two new sources (`src/audio_device.cpp` — the wrapper, declarations-only miniaudio include; `src/miniaudio_impl.c` — the one `MINIAUDIO_IMPLEMENTATION` TU) and per-OS PRIVATE link libs (Apple frameworks / Linux pthread+dl+m / Windows none). `check-platform-boundary.sh` and `tests/platform_boundary_probe.cpp` (both from 0.3.1) were extended, not duplicated, with a `ma_`/`<miniaudio.h>` regex pair and five new `static_assert`s respectively — confirmed to bite locally (seeded `#include <miniaudio.h>` into `audio.hpp`: the script fails naming the file, and `aero_platform_boundary_probe` fails to compile with `'miniaudio.h' file not found`; both reverted clean). No `ci.yml` change was needed (D12) — miniaudio's default backends runtime-link ALSA/PulseAudio via `dlopen`, so no new apt `-dev` packages. Epic 0.3's Definition of Done — one app opens a window, pumps events, reads input, and opens a silent audio device, on all 3 OSes, through engine APIs only — is met by the updated `samples/phase-0-window`, and the epic is **CLOSED**.

### Epic 0.4 — `rhi` + `tools/shaderc`

#### Task 0.4.1 — RHI abstraction surface
**0.4.1 (RHI abstraction surface) needed no vcpkg/CI change either**: `engine/CMakeLists.txt` gained `add_subdirectory(rhi)` after `platform`; the new `engine/rhi/CMakeLists.txt` links only `PUBLIC aero::core aero::platform` (no `find_package` at all — the first engine target with none); `tests/CMakeLists.txt` gained the two new TUs plus `aero::rhi` on `aero_tests`' link line, and nothing else — the three existing boundary-probe `target_link_libraries` lines are untouched, each still linking exactly one `aero::` library (the AC-12 regression check). `engine/rhi/include/aero/rhi/*.hpp` fall inside the `engine/*/include/*` glob `check-platform-boundary.sh` already scans, so it silently started covering the new headers too, even though no rhi-specific guard exists yet — confirmed clean (only `//`-comment SDL_GPU citations, no code-level SDL identifier). No boundary guard/probe pair ships for `engine/rhi` itself until 0.4.5, by design.

#### Task 0.4.2 — SDL_GPU backend
**0.4.2 (SDL_GPU backend) needed no `vcpkg.json`/submodule change**: SDL3 was already a manifest dependency (pulled in by `engine/platform`); `engine/rhi/CMakeLists.txt` gained its own `find_package(SDL3 CONFIG REQUIRED)` and `PRIVATE SDL3::SDL3 aero::profiling aero::platform_internal` on `aero_rhi`'s link line (its `PUBLIC aero::core aero::platform` line is unchanged); `engine/platform/CMakeLists.txt` gained the header-only `aero_platform_internal` INTERFACE target (alias `aero::platform_internal`) exposing the new `engine/platform/internal/` directory, consumed PRIVATE only by `aero_rhi`. `tests/CMakeLists.txt` gained two new TUs (`rhi_device_test.cpp`, `rhi_swapchain_test.cpp`) and nothing else — `aero_tests`' link line and all three existing boundary-probe link lines are byte-identical to before. `ci.yml` gained three deltas: the Linux apt list adds `mesa-vulkan-drivers libvulkan1 xvfb` (lavapipe — a software Vulkan ICD — plus a real X11 video driver, both needed because SDL's dummy driver has no Vulkan/Metal surface support, D6); both `Test` steps on all three lanes now set `AERO_REQUIRE_GPU: 1` and the Linux ones run under `xvfb-run -a`. Locally, `ctest --preset macos-debug`/`macos-release` now exercise real Metal end-to-end (device creation, resource/upload/frame-flow/render-pass paths, and — once per preset — a real visible 320×180 window that flashes for about a second; this is by design, not a bug, see the swapchain test file's header comment). `AERO_REQUIRE_GPU=1 ctest --preset macos-debug` is the local rehearsal of CI's ratchet: unset (the default), a machine with no usable GPU/display skips the gated tiers with a loud `MESSAGE`; set, the same absence is a hard `FAIL`.

#### Task 0.4.3 — `tools/shaderc`
**0.4.3 (`tools/shaderc`) needed no `vcpkg.json`/submodule change either**: root `CMakeLists.txt` gained `option(AERO_SHADER_TOOLS "..." ON)` + `include(cmake/shaders.cmake)` (beside the sanitizers/profiling includes, before `enable_testing()`) and `add_subdirectory(shaders)` (after `tools`); `tools/CMakeLists.txt` gained `add_subdirectory(shaderc)`; `tests/CMakeLists.txt` gained the 13-case shaderc ctest block, gated on `AERO_SHADER_TOOLS` — none of the four existing boundary-probe `target_link_libraries` lines changed. The first configure on a cold machine now also bootstraps the SDL_shadercross toolchain from source (network access needed; a from-source vendored DirectXShaderCompiler build, several minutes to ~20 minutes depending on the machine, once per machine — not once per preset, not once per worktree — cached at `~/.cache/aero-engine/shadercross`, `%LOCALAPPDATA%\aero-engine\shadercross` on Windows); every later configure on that machine short-circuits instantly and fully offline (`shadercross toolchain: warm at …`). `-DAERO_SHADER_TOOLS=OFF` skips the bootstrap, the tool, the `shaders/` build, and the shaderc ctest suite entirely, with a loud `STATUS` explaining what was skipped — the option exists for constrained/offline dev machines, CI never sets it OFF. `ci.yml` gained one `actions/cache@v4` step per lane (matrix var `shadercross_cache`, key `shadercross-${{ runner.os }}-1ca46e0ef7a9`, no `restore-keys`) between the existing vcpkg cache step and the Debug configure/build step; nothing else in CI changed — the bootstrap runs inside the existing configure step and the new tests run inside the existing ctest steps on all three lanes.

### Epic 0.5 — `render`

#### Task 0.5.2 — Textured cube
**0.5.2 (textured cube) needed no vcpkg/CI change**: `shaders/CMakeLists.txt`'s existing `aero_add_shaders(aero_shaders …)` call gained the cube pair (every preset build now also compiles `cube.{vert,frag}.hlsl` to all four artifact formats); `samples/CMakeLists.txt` gained `add_subdirectory(phase-0-cube)` — target `aero_sample_phase0_cube`, linking the same five `aero::` libs as `phase-0-clear` plus an `AERO_CUBE_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders"` compile define gated on `AERO_SHADER_TOOLS` (the sample is a CI compile-proof; run it locally to SEE the cube — tools-OFF builds compile a stub `main` that logs and exits 1); `tests/CMakeLists.txt` gained two TUs on `aero_tests` — `render_cube_test.cpp` and `vulkan_stack_pin.cpp` — plus `${CMAKE_DL_LIBS}` on its link line (the pin's `dlopen`), and **nothing else**: the four boundary-probe `target_link_libraries` lines are byte-identical. `tests/lsan.supp` gained the two lavapipe module entries (`leak:libvulkan_lvp`, `leak:libLLVM`; its header documents why module scope is admissible there and nowhere else). `ci.yml` itself is byte-identical to 0.5.1 — the six-run Linux LSan saga (see the state note above) was resolved entirely in the test harness. Future GPU-draw tests inherit the pin + suppressions; `LP_NUM_THREADS`, `MESA_SHADER_CACHE_DISABLE`, and `LD_PRELOAD` approaches are proven dead ends — do not retry them.

#### Task 0.5.3 — Frame loop & 60 fps validation
**0.5.3 (frame loop & 60 fps validation) needed no vcpkg/CI/CMake-source change** — `samples/phase-0-cube` gained `fps_gate.hpp` (a header, rides `main.cpp`'s existing include) + `VALIDATION.md` (the committed 3-OS gate ledger, not compiled) and edited `main.cpp` + two comment-only CMakeLists; `ci.yml`/`vcpkg.json`/`.github/scripts/**`/boundary probes are byte-identical, and no test TU was added (D7). macOS is validated PASS in the ledger; the Phase 0 gate stays OPEN until a code-free follow-up fills the Windows/Linux rows.

## Phase 1 — Reflection, ECS & Serialization

### Epic 1.1 — `reflect-gen`

#### Task 1.1.1 — libclang harness
**1.1.1 (`tools/reflect-gen`, the libclang harness) needed no vcpkg/CI-cache/CMake-module change**: root `CMakeLists.txt` gained one `option(AERO_REFLECT_TOOLS "..." ON)` line (beside `AERO_SHADER_TOOLS`, no `include()` — no cross-cutting cmake module in 1.1.1, that is 1.1.4's); `tools/CMakeLists.txt` gained `add_subdirectory(reflect-gen)` (before `shaderc`, matching docs/03's listing order); `tests/CMakeLists.txt` gained one gated `if(AERO_REFLECT_TOOLS)` block registering the 12 `reflect-gen.*` ctest cases — `aero_tests`' link line and the four boundary-probe `target_link_libraries` lines are byte-identical. The first configure now also discovers a system-installed LLVM 18 for `reflect-gen` (fast on a machine that already has it — no bootstrap, no from-source build, unlike `AERO_SHADER_TOOLS`): macOS via `brew --prefix llvm@18` (keg-only, not on `PATH` — install with `brew install llvm@18` if missing), Linux via the `/usr/lib/llvm-18` default (install `libclang-18-dev llvm-18-dev`), Windows via `$ENV{ProgramFiles}/LLVM` (install via `choco install llvm --version=18.1.8`). `-DAERO_REFLECT_TOOLS=OFF` skips discovery, the tool, and its tests entirely with a loud `STATUS`, mirroring `AERO_SHADER_TOOLS`; `-DAERO_LLVM_ROOT=...` (or the `AERO_LLVM_ROOT` environment variable) overrides the discovered install on any OS. `ci.yml` gained one provisioning step per lane (apt packages on Linux, `brew install llvm@18` on macOS, `choco install llvm --version=18.1.8` + an `AERO_LLVM_ROOT` env export on Windows) ahead of the existing Debug configure step — no `actions/cache` step, no `vcpkg.json` change, no `AERO_REQUIRE_GPU` change; the new `reflect-gen` ctest cases run inside the existing Debug and Release ctest steps on all three lanes.

#### Task 1.1.2 — Annotation detection
**1.1.2 (`--components` detection) needed no vcpkg/CI/CMake-target change**: `tools/reflect-gen/CMakeLists.txt` is untouched (detection lives inside the existing `main.cpp` TU, no new source file, no new link); `tests/CMakeLists.txt`'s existing `if(AERO_REFLECT_TOOLS)` block gained nine new case names on the `_aero_reflect_cases` list (13 → 22 `reflect-gen.*` ctest cases) backed by five new fixtures under `tests/reflect-gen/fixtures/` — the per-OS `CLANG_ARGS` computation, the `foreach`/`add_test` registration, `aero_tests`' link line, and all four boundary-probe `target_link_libraries` lines are byte-identical.

#### Task 1.1.3 — entt::meta codegen
**1.1.3 (entt::meta codegen) needed exactly one `vcpkg.json` line and no other vcpkg/CI/`tools/reflect-gen/CMakeLists.txt` change**: `vcpkg.json` gained `"entt"` alphabetically between `"enkits"` and `"glm"` (3.16.0, already the pinned `builtin-baseline` — no baseline/submodule bump, the `vcpkg-baseline` CI job stays green); `tools/reflect-gen/CMakeLists.txt` and root `CMakeLists.txt` are untouched (the emitter lives inside the existing `main.cpp` TU). `tests/CMakeLists.txt`'s existing `if(AERO_REFLECT_TOOLS)` block gained: nine new case names on `_aero_reflect_cases` (22 → 31 `reflect-gen.*` ctest cases); a `find_package(EnTT CONFIG REQUIRED)` call; one `add_custom_command` (build-time, `DEPENDS aero_reflect_gen` — generation cannot be configure-time since the tool doesn't exist yet at that point) that runs `aero_reflect_gen --emit-meta` over the new `component_codegen.hpp` fixture into `build/<preset>/tests/reflect-generated/component_codegen.meta.gen.cpp`, reusing the same per-OS `${_aero_reflect_clang_args}` list 1.1.1 already computes plus one added `-I engine/core/include` (passed by real path — `ENGINE_INCLUDE` is `run_case.cmake`-local, not visible here); and the new standalone target `aero_reflect_meta_test` (`reflect-gen/meta_test.cpp` + the generated `.cpp`, linking `doctest::doctest EnTT::EnTT aero::core` PRIVATE) plus its `add_test`. `aero_tests`' link line and all four boundary-probe `target_link_libraries` lines are byte-identical — `entt` links only into `aero_reflect_meta_test`. `-DAERO_REFLECT_TOOLS=OFF` skips `find_package(EnTT)`, the custom command, the target, and every `reflect-gen.*`/`emit_*` test (verified locally: `ctest -N` count drops to the 14 shaderc-only cases, ON restores 46). No `ci.yml` change — `entt` is header-only and the cache key (`hashFiles('vcpkg.json')`) refreshes for the new dependency automatically.

#### Task 1.1.4 — Build-step wiring
**1.1.4 ("Build-step wiring") needed no `vcpkg.json`/`ci.yml`/`/vcpkg` pin/boundary-guard change**: root `CMakeLists.txt` gained one `include(cmake/reflect.cmake)` line right after the existing `AERO_REFLECT_TOOLS` option; the new `cmake/reflect.cmake` module absorbed the `AERO_LLVM_ROOT` discovery block deleted from `tools/reflect-gen/CMakeLists.txt` (which keeps everything from `find_package(Clang 18 CONFIG QUIET ...)` down, byte-for-byte) and the per-OS clang-arg computation deleted from `tests/CMakeLists.txt` (which now just references the module's `AERO_REFLECT_CLANG_ARGS`/`AERO_LLVM_ROOT` directly); `tests/CMakeLists.txt`'s `_aero_reflect_cases` list grew by 7 (`depfile_basic/_requires_output/_requires_emit/_parse_fail/_space_escape/_determinism/_unwritable`, 31→38) plus a separately-registered `reflect-gen.incremental_e2e` (38→39); the 1.1.3 hand-wired `add_custom_command` block was replaced by one `aero_reflect_generate(aero_reflect_meta_test HEADERS ... INCLUDE_DIRS ...)` call — `aero_reflect_meta_test`'s link line and `aero_tests`' link line are byte-identical, and all four boundary-probe `target_link_libraries` lines are untouched. `tests/reflect-gen/run_case.cmake` gained 6 new `elseif` branches (reusing existing helpers, incl. `aero_expect_file_contains`); two new tracked files joined the tree: `tests/reflect-gen/incremental_e2e.cmake` (the nested-build `cmake -P` driver) and `tests/reflect-gen/incremental/` (a nested probe CMake project + two self-contained fixtures, `widget.hpp`/`widget_types.hpp` — never `add_subdirectory`'d by the real build, only ever configured standalone inside a ctest-driven scratch copy); `tests/reflect-gen/fixtures/component_wiring.hpp` is the only new *tracked* fixture (the nested probe's own `probe_main.cpp` is `file(WRITE)`ten at configure time, never tracked, F9). No engine/runtime/editor/samples change. Local verification (both macOS Debug/Release configs): full ctest green (53/53, including the pre-existing `aero_tests`); `reflect-gen.*` alone green at 38/38; the 6 depfile cases + `incremental_e2e` + `aero_reflect_meta_test` (both TEST_CASEs) all pass; AC-7's real-tree spot-check and AC-8's OFF-mode `ctest -N` parity both confirmed manually.

### Epic 1.2 — Serialization

#### Task 1.2.1 — JSON writer
**1.2.1 ("JSON writer, generated") needed no `vcpkg.json`/`ci.yml`/`/vcpkg` pin/boundary-guard change** — the lightest backend task in the project's history, because JSON serialization hides no third-party type (std-only + `engine::Vec3`/`Quat`), so the boundary rule is satisfied by construction with no grep script or compile-time probe to add: `engine/CMakeLists.txt` gained one `add_subdirectory(reflect)` line (after `render`); the new `engine/reflect/CMakeLists.txt` builds `aero_reflect` STATIC **unconditionally** (not gated on `AERO_REFLECT_TOOLS` — it is real engine code, not tool output), linking only `PUBLIC aero::core`, no `find_package`; `tools/reflect-gen/CMakeLists.txt` is untouched (the `--emit-json` emitter lives inside the existing `main.cpp` TU); `cmake/reflect.cmake` gained one sibling function, `aero_reflect_generate_json()`, reusing the same `AERO_LLVM_ROOT`/`AERO_REFLECT_CLANG_ARGS` the module already computes. `tests/CMakeLists.txt`'s existing `if(AERO_REFLECT_TOOLS)` block gained 11 new case names on `_aero_reflect_cases` (39 → 50 `reflect-gen.*` ctest cases) plus one new standalone target, `aero_reflect_json_test` (`doctest::doctest aero::reflect aero::core` — no EnTT), and its own `aero_reflect_generate_json()` call over three existing fixtures — `aero_tests`' link line, all four boundary-probe `target_link_libraries` lines, `aero_reflect_meta_test` (name/link line/call), and `reflect-gen.incremental_e2e` are all byte-identical. No new CI provisioning step, no `actions/cache` entry — nothing runs that wasn't already running.

#### Task 1.2.2 — JSON reader
**1.2.2 ("JSON reader, generated") needed no `vcpkg.json`/`ci.yml`/`/vcpkg` pin/boundary-guard/CMake-function change either** — the read runtime is std-only + `engine::Vec3`/`Quat` (same by-construction boundary argument as 1.2.1), and `aero_reflect_generate_json()` is content-agnostic so emitting a second function per component needed zero CMake edit: `engine/reflect/CMakeLists.txt` gained two source lines (`src/json_value.cpp src/json_reader.cpp`); `tools/reflect-gen/src/main.cpp` gained the `classifyField` whitelist rewrite, the `emitJson` reader-emission extension, and the `printUsage`/header-comment text-only edits — `Args`/`parseArgs`/the dispatch chain/`ExitCode`/`writeDepfile` are byte-unchanged; `tests/CMakeLists.txt`'s existing `if(AERO_REFLECT_TOOLS)` block gained 7 new case names on `_aero_reflect_cases` (50 → 57 `reflect-gen.*` cases) plus one new `HEADERS` entry (`reflect-gen/fixtures/component_limits.hpp`) on the existing `aero_reflect_generate_json(aero_reflect_json_test …)` call — `aero_reflect_json_test`'s name/sources/link line/`add_test`, `aero_reflect_meta_test`, `reflect-gen.incremental_e2e`, `aero_tests`' link line, and all four boundary-probe `target_link_libraries` lines are byte-identical. No new CI provisioning step, no `actions/cache` entry — nothing runs that wasn't already running. Local verification (both macOS Debug/Release configs): full ctest green (73/73, up from the pre-1.2.2 baseline of 66 — +7 new `json_reader_*`/`components_longdouble` process-boundary cases and +13 new runtime `TEST_CASE`s in `aero_reflect_json_test`, now 17 total incl. the 4 pre-existing 1.2.1 cases); `reflect-gen.*` alone green at 57/57 (50 existing intact + 7 new); the determinism spot-check (`--emit-json` run twice on `component_limits.hpp` → byte-identical) and the `-o` byte-identity spot-check both confirmed manually; the real-tree `touch engine/core/include/aero/core/math/constants.hpp` spot-check regenerated the three `.json.gen.cpp` files whose FIXTURE HEADER transitively includes `math.hpp` (`component_codegen`/`component_wiring`/`component_multi`) but correctly did NOT regenerate `component_limits.json.gen.cpp` (that fixture is self-contained, no `math.hpp` — the depfile's precision, not a bug) — though its `.o` still recompiled, since every generated TU's OWN `#include <aero/reflect/serialize.hpp>` transitively reaches `constants.hpp` regardless of what the source fixture includes (two separate dependency graphs: the reflect-gen custom command's depfile vs. the compiler's own header dependency scan); a second rebuild was `ninja: no work to do`; `-DAERO_REFLECT_TOOLS=OFF` left `aero_reflect` (incl. the new parser/DOM) building standalone with `ctest -N` back to the 14 shaderc-only + `aero_tests` baseline, ON restored all 73.

#### Task 1.2.3 — Scene serialization format (+ the audit PR)
**1.2.3 ("Scene serialization format") needed no `vcpkg.json`/`ci.yml`/`/vcpkg` pin/boundary-guard/CMake-function change** — std-only + `engine::Vec3`/`Quat` and JSON-object-shaped by construction, same by-construction boundary argument as 1.2.1/1.2.2, and `engine/reflect/CMakeLists.txt` needed only one new source line (`src/scene_format.cpp`, unconditional, not gated on `AERO_REFLECT_TOOLS`): `engine/reflect` still links `PUBLIC aero::core` only. `tests/CMakeLists.txt` gained one new source on `aero_tests` (`scene_format_test.cpp`) and **`aero::reflect`** inserted into `aero_tests`' link line between `aero::platform` and `aero::render` — the only link-line change in the task; all four boundary-probe `target_link_libraries` lines, `tools/reflect-gen/**`, `cmake/reflect.cmake`, the 57 `reflect-gen.*` cases, and both standalone reflect targets (`aero_reflect_meta_test`, `aero_reflect_json_test`) are byte-identical. No new CI provisioning step, no `actions/cache` entry — nothing runs that wasn't already running. Local verification (both macOS Debug/Release configs): full ctest green (73/73, unchanged from the pre-1.2.3 baseline — the new TU rides the single `aero_tests` ctest entry, adding zero new entries); `-DAERO_REFLECT_TOOLS=OFF` dropped `ctest -N` to 14 with `aero_tests` (now including all scene tests) still green — the structural proof that the scene-format layer has no codegen dependency (AC-12); ON restored 73. All 21 `TEST_CASE`s in `scene_format_test.cpp` (the true landing count — 19 was a tally error in this note) passed on the FIRST build with zero byte-pin corrections needed — the plan's own pre-verification of the canonical fixtures (its grounding note N5) held exactly. **The Epic 1.2 close-out audit hardening PR needed no `vcpkg.json`/`ci.yml`/`/vcpkg` pin/boundary-guard/CMake-function change** — five commits, all gates green per commit: `json_writer.cpp` ctor clamp + a `json_writer.hpp` comment (+ the first non-default `indentWidth` unit subcases); `tools/reflect-gen/src/main.cpp` `isNamespaceScoped()` + `Component.atNamespaceScope` + the `--emit-json` skip (new fixture `tests/reflect-gen/fixtures/component_nested.hpp`, `json_nested_skip` appended to `_aero_reflect_cases` — ctest **73 → 74** tools-ON, 14 OFF unchanged); `scene_format.cpp` `extract()` per-entity seen-types set + docs/09 + `scene_format.hpp` comment (+ a new hand-built-DOM rejection `TEST_CASE`, scene count 21 → **22**); `json_test.cpp` hardening only (17 → **18** `TEST_CASE`s); docs (this file, docs/02 ADR-004 artifact row, docs/08 R2 close-out sentence + new R19, docs/tasks/phase-3.md Epic 3.3 binary-serialization note). `aero_tests`' link line and all four boundary-probe `target_link_libraries` lines are byte-identical.

### Epic 1.3 — ECS & scene

#### Task 1.3.1 — EnTT world integration
**1.3.1 ("EnTT world integration") needed no `vcpkg.json`/`ci.yml`/`/vcpkg` pin/`cmake/**`-function change** — EnTT was already a manifest dependency since 1.1.3, and the guard mechanism is a straight copy of the platform/rhi precedent: `engine/CMakeLists.txt` gained one `add_subdirectory(scene)` line (after `reflect`); the new `engine/scene/CMakeLists.txt` builds `aero_scene` STATIC (`PUBLIC aero::core`, `PRIVATE EnTT::EnTT aero::profiling`) plus the sibling `aero_scene_internal` INTERFACE target exposing `engine/scene/internal/`; `tests/CMakeLists.txt` gained one new source (`scene_test.cpp`) on `aero_tests`, two new tokens on its link line (`aero::scene aero::scene_internal` — the only link-line change), and a fifth boundary-probe block (`aero_scene_boundary_probe`, linking `aero::scene` alone, never `aero::scene_internal` — that target carries `EnTT::EnTT` INTERFACE by design and would silently void the guard) — the four existing probe `target_link_libraries` lines are byte-identical. `ci.yml` gained exactly one `lint`-job step running the new `.github/scripts/check-scene-boundary.sh`. No new CI provisioning step, no `actions/cache` entry — nothing runs that wasn't already running. Local verification (both macOS Debug/Release configs): full ctest green (74/74, unchanged from the pre-1.3.1 baseline — the 14 new `scene:` `TEST_CASE`s ride the single `aero_tests` ctest entry, adding zero new entries); `-DAERO_REFLECT_TOOLS=OFF` dropped `ctest -N` to 14 with `aero_tests` (now including all scene tests) still green — the structural proof the scene layer has no codegen dependency; ON restored 74. Both sabotage proofs (AC-11/AC-12) were performed, captured, and reverted: the textual guard refuses to pass on a real `#include <entt/entt.hpp>` leak, on a real `entt::` identifier leak, AND on a renamed/missing canary (exit 2, distinct from the exit-1 leak case) — while a real prose citation of `entt::type_hash` in a shipped public header (`component.hpp`) passes clean; the compile-time probe fails to compile the identical seeded leak (`'entt/entt.hpp' file not found`) while the SAME leak compiles clean inside `aero_tests` (which links `aero::scene_internal` + doctest and inherits the shared vcpkg include root) — proving neither guard is vacuous and confirming why both ship. Zero `NOLINT` added (`git grep -c NOLINT -- engine/scene tests/scene_test.cpp tests/scene_boundary_probe.cpp` → 0), matching the plan's own zero-`NOLINT` prediction.

#### Task 1.3.2 — Transform hierarchy
**1.3.2 ("Transform hierarchy") needed no `vcpkg.json`/`ci.yml`/`/vcpkg` pin/`cmake/**`-function/boundary-script change** — the by-construction boundary argument holds again (`aero::reflect`, the one new PUBLIC link `aero_scene` gained, wraps no third-party type and links no vcpkg package): `engine/scene/CMakeLists.txt` gained one source (`src/transform.cpp`) and `aero::reflect` on `aero_scene`'s PUBLIC link line; `engine/reflect/CMakeLists.txt` is byte-identical (headers aren't listed there — `annotations.hpp` rides the existing `target_include_directories(aero_reflect PUBLIC include)`); `tests/CMakeLists.txt` gained one source on `aero_tests` (`transform_test.cpp`, link line unchanged), one appended case name (57 → 58 `_aero_reflect_cases`, `components_engine_transform`), and `+aero::scene` plus two `INCLUDE_DIRS` entries and one `HEADERS` entry on EACH of the two gated reflect targets (`aero_reflect_meta_test`/`aero_reflect_json_test`) — all four boundary-probe `target_link_libraries` lines are byte-identical, and so is `aero_tests`' own link line. No new CI provisioning step, no `actions/cache` entry — nothing runs that wasn't already running. Local verification (both macOS Debug/Release configs, clean configures): full ctest green (**75/75**, up from the pre-1.3.2 baseline of 74 — the one new `reflect-gen.components_engine_transform` process case; `transform_test.cpp`'s 20 new `TEST_CASE`s ride the single `aero_tests` ctest entry, adding zero new entries); `-DAERO_REFLECT_TOOLS=OFF` dropped `ctest -N` to 14 with `aero_tests` (now including all transform tests) still green — the structural proof the component/hierarchy/matrix layer has no codegen dependency; ON restored 75. Both sabotage-proof halves were performed, captured, and reverted (see above). One deviation from the plan's own suggested 8-commit split, recorded here because it changes nothing about the shipped diff, only where its commit boundaries fall: the plan's commit #2 (Parts 2–4 only, i.e. `transform.hpp`/`transform.cpp`/the umbrella/the CMakeLists edits, WITHOUT Part 5's `world.hpp` hierarchy declarations) does not actually compile — `transform.cpp` calls `World::parent()`/`get<Transform>()`, which are declared only in Part 5 — so Part 5 was folded one commit earlier than suggested (into the same commit as Parts 2–4) to keep every commit buildable, and the plan's commits #3+#4 (Part 6's `world.cpp` hierarchy/destroy rewrite/ctor-seeding + Part 9's five-constant `scene_test.cpp` fix) were landed together, exactly as the plan's own text explicitly licenses ("land it together with #4 if you prefer a green-per-commit history") — two pairs merged, identical final diff.

#### Task 1.3.3 — Camera & Light
**1.3.3 (Camera & Light) needed no vcpkg/ci.yml/cmake/boundary-guard change** — three header-only reflected components in `engine/scene/`; `transform.cpp` gained three `registerComponent` calls (fresh-World `componentTypeCount()` 1 → 4). The count shift hit absolute assertions in BOTH `scene_test.cpp` AND `transform_test.cpp` (the hidden-count trap — `git grep 'componentTypeCount() ==' tests/` finds all).

### Epic 1.4 — Scene → render

#### Task 1.4.1 — Scene → render
**1.4.1 (Scene → render) needed no vcpkg/`/vcpkg` pin/ci.yml/boundary-guard/`cmake/**`-function change** — `engine/CMakeLists.txt` gained `add_subdirectory(scene_render)` (after `scene`); the new `engine/scene_render/CMakeLists.txt` links `PUBLIC aero::scene aero::render PRIVATE aero::profiling` (no `find_package`, no vcpkg package — boundary rule holds by construction, like 0.5.1); `shaders/CMakeLists.txt` gained the `scene.{vert,frag}` pair; `tests/CMakeLists.txt` gained `scene_render_test.cpp` + `aero::scene_render` on `aero_tests`' link line (its only change) + the `components_engine_mesh_renderer` process case. MeshRenderer wraps no third-party type, so no new probe.

#### Task 1.4.2 — Load & draw a JSON scene
**1.4.2 (Load & draw a JSON scene) needed no `vcpkg.json`/`/vcpkg` pin/ci.yml/boundary-guard/`cmake/**`-function change, but DID reorder the root `add_subdirectory` list** — `add_subdirectory(tools)` now runs BEFORE `add_subdirectory(engine)` because `engine/scene_serialize` is the first engine-tree caller of `aero_reflect_generate_json`, whose tool-resolution check `FATAL_ERROR`s at configure if the `aero_reflect_gen` target is absent (every prior caller lived in `tests/`, which runs after `tools/`); `tools/` links no engine target, so the golden rule is untouched (the stale SDL3-ordering comments this exposed in `tools/shaderc/CMakeLists.txt` were corrected in the same PR). `engine/CMakeLists.txt` gained `add_subdirectory(scene_serialize)` (after `scene_render`); the new `engine/scene_serialize/CMakeLists.txt` early-`return()`s under `AERO_REFLECT_TOOLS=OFF`, else builds `aero_scene_serialize` and calls `aero_reflect_generate_json(aero_scene_serialize HEADERS transform/camera/light/mesh_renderer INCLUDE_DIRS core/scene/reflect)` — the FIRST non-test call of that function. `tests/CMakeLists.txt` gained the standalone gated `aero_scene_serialize_test` (11 tier-0 cases; links `aero::scene_serialize` only — does not itself generate); `samples/CMakeLists.txt` gained `add_subdirectory(phase-1-scene)` (real path gated on `AERO_REFLECT_TOOLS AND AERO_SHADER_TOOLS`, a stub `main` otherwise). `aero_tests`' link line and all five boundary-probe link lines are byte-identical; local gate 79/79 both presets + tools-OFF proof, CI green all 3 OSes. **With Epic 1.4 closed, Phase 1 is COMPLETE; Phase 2 (Editor) is next. Update this note again as each future task lands.**

## Phase 2 — Editor

### Epic 2.1 — Editor shell

#### Task 2.1.3 — Editor app shell & main loop
**2.1.3 ("Editor app shell & main loop") needed no `vcpkg.json`/`/vcpkg` pin/`ci.yml`/`cmake/**`/boundary-guard/`.github/scripts/**` change** — the lightest-infrastructure Phase-2 task, with a diff confined to `editor/`, `tests/` and docs: `editor/CMakeLists.txt` gained four sources on `aero_editor_core` (`src/panel_registry.cpp src/editor_app.cpp src/shell_ui.cpp src/placeholder_panel.cpp`) and lost one (`src/editor_ui.cpp`, deleted with its header) — **every other line of that file, including both `find_package` calls, the alias, `target_include_directories` and both link lines, is byte-identical**; `tests/CMakeLists.txt` gained ONE new block right after the `aero_editor_imgui_test` block (`add_executable(aero_editor_shell_test editor/shell_test.cpp)` + `target_link_libraries(... doctest::doctest aero::editor_core aero::core)` + `add_test`, with **no** `target_include_directories`, **no** `vulkan_stack_pin.cpp` and **no** `${CMAKE_DL_LIBS}` — a tier-0 target must not inherit the GPU harness), and `tests/editor/imgui_layer_test.cpp` was rewritten in place with its target/name/gate/link-line untouched. `aero_tests`' source list and link line and all five boundary-probe `target_link_libraries` lines are byte-identical; no root `CMakeLists.txt` change (`add_subdirectory(editor)` already precedes `tests/`). Local verification (both macOS Debug/Release configs, clean configures): full ctest green at **83/83**, up from the pre-2.1.3 baseline of 82 — the single new `aero_editor_shell_test` entry; `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` went **4 → 5** with `aero_editor` still building and `aero_editor_shell_test` still green (the editor depends on neither codegen nor shaders). The non-interactive launch proof is the 2.1.1 recipe with new greps: delete `build/<preset>/editor/aero_editor.ini` (it sits next to the exe), run `( aero_editor & pid=$!; sleep 3; kill $pid )`, and assert the log line `editor: shell ready (5 panels, layout: default)` plus a written ini containing `[Docking][Data]` entries for all five panel names; relaunch and assert the ini is byte-identical and the log now says `layout: restored`. Four sabotage proofs were performed and reverted, per the 2.1.2 lesson that a green suite proves nothing about what a test asserts: an unconditional `ImGui::End()` on the hidden-panel path makes `aero_editor_imgui_test` abort; dropping the duplicate-id check reds the tier-0 registry case; swapping the `!presented`/`focused` branch order in `framePaceSleepMs` reds the E12 case; and seeding `#include <imgui.h>` into `panel.hpp` **still compiles** in `aero_editor_shell_test` — the R12 limitation, now documented in that block's own comment instead of being claimed as a guard.

### Epic 2.2 — Core panels

#### Task 2.2.1 — Hierarchy panel
**2.2.1 ("Hierarchy panel") needed no `vcpkg.json`/`/vcpkg` pin/`ci.yml`/`cmake/**`/`.github/scripts/**`/boundary-guard change** — the engine side is 4 edits and 0 new files (`world.hpp`/`world.cpp` gain `setName`/`name`/`componentTypeAt`/`copyComponent`; `scene_serialize.{hpp,cpp}` gain the 2-line name mapping); the editor side adds three new PUBLIC headers (`selection.hpp`, `panel_context.hpp`, `entity_ops.hpp`) + two new public sources + one new src-private ImGui pair (`hierarchy_panel.{hpp,cpp}`, the only new ImGui TU) — `editor/CMakeLists.txt` gained 3 sources and `aero::scene` PUBLIC on `aero_editor_core`'s link line (its `PRIVATE` line untouched); `tests/CMakeLists.txt` changed exactly ONE block (`aero_editor_shell_test` gained `editor/hierarchy_test.cpp` and `aero::scene`/`aero::scene_internal` on its link line, mirroring the reasoning `aero_tests` already uses for the same two tokens) plus per-file edits to `scene_test.cpp` (+9 `TEST_CASE`s), `scene_serialize_test.cpp` (the F14 case rewritten in place + 1 new byte-exact-idempotence case), `scene_boundary_probe.cpp` (+shape/noexcept asserts, link line UNCHANGED), `shell_test.cpp` (5 stub signatures + 1 new case, 15 → 16), and `imgui_layer_test.cpp` (+2 GPU-gated cases, 1 → 3) — `aero_tests`' own source list/link line and all five boundary-probe `target_link_libraries` lines are byte-identical throughout, verified with `git diff --stat` against `origin/main` showing zero touched files under `runtime/`, `samples/`, `tools/`, `cmake/`, `shaders/`, `.github/`. No new ctest entry anywhere — both new TUs ride existing targets, so the inventory holds at **83 → 83** (tools-ON) and **5 → 5** (`-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`, a freshly configured tree, `aero_editor` still building). Full local ctest green on both macOS presets end to end; `AERO_REQUIRE_GPU=1 ctest --preset macos-debug` (the CI ratchet rehearsal) green at 83/83; all five boundary guards (`check-{math,jobs,platform,rhi,scene}-boundary.sh` + `check-golden-rule.sh`) green with no allowlist change.

#### Task 2.2.2 — Reflection-driven inspector
**2.2.2 ("Reflection-driven inspector") needed no `vcpkg.json`/`/vcpkg` pin/`ci.yml`/`cmake/**`/`.github/scripts/**`/boundary-guard change** — EnTT was already a manifest dependency since 1.1.3, and the annotation/`std::string` extensions are additive to the same one TU (`tools/reflect-gen/src/main.cpp`): `engine/core/include/aero/core/math/quat.hpp` + `engine/core/src/math/glm_backend.cpp` gained the euler pair (no new `#include`); `engine/reflect/include/aero/reflect/serialize.hpp` + `.cpp` gained the two `std::string` leaves; `engine/reflect/include/aero/reflect/annotations.hpp` gained `AERO_RANGE`/`AERO_COLOR`/`FieldUiMeta`; `engine/scene/include/aero/scene/{camera,light,mesh_renderer}.hpp` gained annotations only (no `.cpp`, no registration change). `editor/CMakeLists.txt` gained six new sources (`component_ops.cpp`, `inspector_model.cpp`, `meta_utils.cpp`, `editor_reflection.cpp`, `text_input.cpp`, `inspector_panel.cpp`), `EnTT::EnTT` PRIVATE on `aero_editor_core` (mirroring the ImGui precedent exactly), and one `aero_reflect_generate()` call over the four built-in component headers (gated `if(AERO_REFLECT_TOOLS)`, defining `AERO_EDITOR_REFLECTION=1`) — `editor_app.cpp` changed exactly two lines (the `registerEditorReflection()` call + the `PlaceholderPanel` → `InspectorPanel` swap). `tests/CMakeLists.txt` gained: nine new case names on `_aero_reflect_cases` (61 → 70); one new `HEADERS` entry (`reflect-gen/fixtures/component_text.hpp`) on the existing `aero_reflect_generate_json(aero_reflect_json_test …)` call; and one new gated target block (`aero_editor_inspector_test`, mirroring `aero_reflect_meta_test`'s shape but linking `aero::editor_core aero::scene aero::scene_internal aero::core` and generating meta for its own fixture, `editor/fixtures/inspector_probe.hpp`) — `aero_tests`' own source list/link line and all five boundary-probe `target_link_libraries` lines are byte-identical throughout, verified with `git diff --stat` against `origin/main` showing zero touched files under `runtime/`, `samples/`, `shaders/`, `.github/`, `cmake/`. `ctest -N` **83 → 93** (+9 `reflect-gen.*` process cases → 70 `_aero_reflect_cases`; +1 gated target `aero_editor_inspector_test`); tools-OFF stayed **5 → 5** (a freshly configured tree, `aero_editor` still building, exactly one D12 `AERO_LOG_WARN` on launch). Full local ctest green on both macOS presets (Debug 93/93, Release 93/93); `AERO_REQUIRE_GPU=1 ctest --preset macos-debug` (the CI ratchet rehearsal) green at 93/93; all five boundary guards (`check-{math,jobs,platform,rhi,scene}-boundary.sh` + `check-golden-rule.sh`) green with no allowlist change; local clang-tidy (`SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path) clang-tidy-18 --warnings-as-errors='*'`) clean over every new/changed TU after three small fixes (`modernize-use-auto` ×4, `bugprone-signed-char-misuse` ×1 via an intermediate `long long` cast, `modernize-raw-string-literal` ×1) — landed as a separate `fix:` commit; the codebase gained **three** new `NOLINT`s, not the plan's predicted two (a third, `readability-identifier-naming`, was needed for `meta_test.cpp`'s new `aero_reflect_register_mesh_renderer()` forward declaration — the exact same frozen-snake-case pattern its five pre-existing NOLINTs already use, which the plan's own V2 section undercounted by one).

#### Task 2.2.3 — Viewport panel
**2.2.3 ("Viewport panel") needed no `vcpkg.json`/`/vcpkg` pin/`ci.yml`/`cmake/**`/`.github/scripts/**`/boundary-guard change.** `engine/rhi`: `internal/aero/rhi/internal/native_device.hpp` gained one declaration (`NativeDeviceAccessor::texture`) and `src/sdl_gpu_backend.cpp` gained one definition — **no CMake change** (the file was already the sole allowlisted rhi-boundary TU). `engine/render`: gained one new source (`src/render_target.cpp`) on `aero_render`'s source list and **no link-line change** (`RenderTarget` uses only `aero::rhi`, already PUBLIC, and `aero::profiling`, already PRIVATE); `renderer.hpp` changed by exactly two code lines (a forward declaration + one `friend` line) and `render.hpp` gained one include plus a one-sentence comment correction. `editor/CMakeLists.txt` gained one source (`src/viewport_panel.cpp`), `aero::scene_render` PRIVATE on `aero_editor_core`'s link line (its PUBLIC line untouched), and one new `if(AERO_SHADER_TOOLS)` block (`add_dependencies(aero_editor_core aero_shaders)` + `AERO_EDITOR_SHADERS`/`AERO_SHADERS_DIR` compile definitions), placed after the existing `if(AERO_REFLECT_TOOLS)` block. **`tests/CMakeLists.txt` changed exactly TWO functional lines, both declared up front (§A/§O-1) rather than discovered in review**: `render_target_test.cpp` joined `aero_tests`' source list (zero new ctest entries, F20), and `aero_tests`' `target_link_libraries` gained exactly one token, `aero::rhi_internal` — the AC-15 amendment the user decided on 2026-07-27 (§O-1). The fact behind the amendment: **no test target in the tree could otherwise `#include <aero/rhi/internal/native_device.hpp>`** — `aero_rhi` holds that directory `PRIVATE` (`engine/rhi/CMakeLists.txt:47`) and `aero_rhi_internal` is a header-only INTERFACE target whose one prior consumer, `aero_editor_core`, links it `PRIVATE` (so its usage requirements do not propagate); `git grep -n 'rhi/internal' -- tests/` was empty before this task. `aero::rhi_internal` on `aero_tests` mirrors the EXACT precedent `aero::scene_internal` already sits on that same line for (`tests/CMakeLists.txt:63-66`: "tests sit OUTSIDE the boundary rule") and carries one include directory, no library, no vcpkg root and no SDL — it cannot weaken any guard, and **all five `*_boundary_probe` `target_link_libraries` lines remain byte-identical**, verified by `git diff origin/main -- tests/CMakeLists.txt | grep -E 'boundary_probe'` returning empty. `ctest -N` **94 → 94** (both new TUs ride existing `add_test` entries — `aero_tests` and `aero_editor_imgui_test`, F20); tools-OFF **5 → 5** (a fresh `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` configure, `aero_editor` still building and launching, exactly one new WARN — `"editor: viewport disabled — built with -DAERO_SHADER_TOOLS=OFF"` — alongside 2.2.2's pre-existing reflection WARN). Both macOS presets green throughout every one of the seven commits; `AERO_REQUIRE_GPU=1 ctest` green; all five boundary guards green with no allowlist change; local clang-tidy clean with exactly **one** new `NOLINT` (`cppcoreguidelines-pro-type-reinterpret-cast` on the `void*` → `ImTextureID` conversion, scoped `NOLINTNEXTLINE` with a reason comment, exactly as predicted) — three `misc-const-correctness`/`bugprone-unchecked-optional-access` findings surfaced and were fixed with real code changes (a `const` local, an explicit `Device`-move-const local, and two defensive `!target`/`!sceneRenderer` guard clauses in `ViewportPanel` that also serve as genuine extra safety, not merely lint suppression) rather than `NOLINT`s. **Zero change under `runtime/`/`samples/`/`tools/`/`cmake/`/`shaders/`/`.github/`; `vcpkg.json` and the `/vcpkg` pin byte-identical; `imgui_layer.{hpp,cpp}` byte-identical for the fourth task running; `aero_editor_shell_test` unaffected (44/44 cases, verified by running it, not assumed)** — all confirmed by `git diff --stat` against `origin/main`. One deviation beyond the declared AC-15 amendment, found during implementation and not anticipated by the plan or spec, and then **relocated during code review**: an over-hardware-limit `createTexture()` request `std::abort()`s the whole process via Metal's own texture-descriptor validation. It first shipped as a `HARDWARE_SAFE_TEXTURE_DIMENSION = 16384` check inside `RenderTarget::allocate()` (this task's own new file), but review established that patched one caller while leaving `rhi::Device::createTexture` — the function whose contract is actually violated — broken for everyone else. **The user chose to fix the layer in the same PR**, so the check now lives in `validateDesc` (`engine/rhi/src/sdl_gpu_backend.cpp`) against the published `rhi::MAX_TEXTURE_DIMENSION_2D` in `types.hpp`, and the `render`-local copy was deleted. That widened this task's file scope by two `engine/rhi` files and four subcases in `tests/rhi_device_test.cpp` beyond the plan's fence — a deliberate, user-approved expansion, recorded here rather than absorbed. Two further review findings were fixed on the same branch, both contract breaches in the new public type and neither reachable through the shipped editor: `resize()` wrote `drawRect` unconditionally even when `allocate()` had already zeroed `allocExtent` (leaving a non-zero `drawExtent()` over a **zero** `textureExtent()`, breaking INV-1 and making the header's own documented `uvMax = drawExtent / textureExtent` divide by zero), and `resize()` on a **moved-from** target reached `device->createTexture()` through a null pointer (UBSan-confirmed) while `beginFrame()`/`colorTexture()`/the destructor were all already inert. Both shipped because the tests asserted the neighbouring properties but not these; both now carry assertions, and both were sabotage-verified — reverting each fix reddens exactly the new assertion and nothing else.

#### Task 2.2.5 — Log/console panel
**2.2.5 ("Log/console panel") needed no `vcpkg.json`/`/vcpkg` pin/`ci.yml`/`cmake/**`/`.github/scripts/**`/boundary-guard change, and made ZERO change under `engine/` — the whole diff is new files plus edits inside `editor/`, `tests/` and docs, plus two file deletions.** `editor/CMakeLists.txt` gained **two source lines** on `aero_editor_core` (`src/console_model.cpp`, `src/console_panel.cpp`) and lost **one** (`src/placeholder_panel.cpp`, task 2.1.3's last surviving placeholder file), net 18 → 19 sources, and **no link-line change** — `<mutex>`, `<atomic>` and `<chrono>` need no library on any lane for this target, and `imgui::imgui`/`aero::scene` were already on the link line. `tests/CMakeLists.txt` gained **one `find_package(Threads REQUIRED)` call, one source token (`editor/console_model_test.cpp`) and one link token (`Threads::Threads`)** on `aero_editor_shell_test`'s existing block — the first `find_package(Threads)` this target has ever needed, since `console_model_test.cpp` runs real `std::thread`s (the sink concurrency and R14 stress arms) and the tree's only other `find_package(Threads)` sits inside `engine/platform/CMakeLists.txt`'s `elseif(UNIX)` branch, so on macOS/Windows there is no `Threads::Threads` anywhere to inherit transitively — every other link line in the file, every `*_boundary_probe` line, `aero_tests`' whole block and `aero_editor_imgui_test`'s whole block, is byte-identical, verified by `git diff origin/main -- tests/CMakeLists.txt | grep -E 'boundary_probe|add_test|aero_tests'` returning empty. `tests/editor/imgui_layer_test.cpp` gained two includes and three `TEST_CASE`s (7 → 10); no new `add_test` anywhere, so `ctest -N` reads **94** on both macOS presets and **5** tools-OFF at every one of the six commit boundaries. `check-math-boundary.sh` is the one guard that sees this diff at all (it scans every tracked C-family file tree-wide minus the single allowlisted `glm_backend.cpp`); its anti-vacuity `scanned` count moved by net **+3** (188 on `origin/main` → 191 here: five new files — `console_model.{hpp,cpp}`, `console_panel.{hpp,cpp}`, `console_model_test.cpp` — minus two deleted `placeholder_panel.{hpp,cpp}` files, measured with a disposable `git worktree` against `origin/main` rather than assumed) — nothing in this task includes GLM, so the verdict is unchanged and no allowlist moved. The other four guards scan only `engine`/`runtime` or `engine/*/include/*` and cannot see this diff at all; the configure-time `aero_assert_golden_rule()` link-graph walk is unaffected (this task creates no CMake target). **This task adds NO tools gate and NO new tools-OFF WARN** — it depends on neither codegen nor shaders, and the console is fully functional under `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`: a fresh tools-OFF configure (`build/tools-off-2.2.4`, rebuilt in place) still reads **5/5** ctest entries green, and the launched `aero_editor` log carries **exactly two WARN lines and no third** (2.2.2's reflection WARN, 2.2.3's shader WARN) plus, for the first time, the console-sink-attached INFO line preceding both of them — the new half this task adds is that **both pre-existing WARNs are now visible inside the editor itself**, the whole point of the panel. `editor/include/aero/editor/editor_app.hpp` gained one `#include <cstddef>` (ahead of the existing `<cstdint>`, matching `SortIncludes: CaseSensitive`), one forward declaration (`class ConsolePanel;`), one accessor (`logRecordCount()`) and one non-owning member (`consolePanel`) — `editor_app.cpp` gained two includes, one new local `std::optional<LogSinkScope>` scope block in `create()`, one replaced emplace line (was `PlaceholderPanel`, now `ConsolePanel`), one four-line pump in `tick()`, and one accessor definition. `imgui_layer.{hpp,cpp}`, `text_input.{hpp,cpp}` and `main.cpp` stayed byte-identical for the **sixth task running** (INV-7) — verified by `git diff origin/main` returning empty for all five. Both macOS presets configure, build warning-free and pass 94/94 at every one of the six commit boundaries, with the `AERO_REQUIRE_GPU=1` CI ratchet rehearsed green on both (`aero_editor_shell_test`'s isolated wall time, which now starts 8+4=12 threads across cases 18 and 24 under ASan, measured at ~3.3 s — no meaningful regression from 2.2.4's code-review-round baseline). Local lint was run before every commit with the keg-only Homebrew LLVM 18 (`clang-format-18 --dry-run --Werror`; `SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path) clang-tidy-18 -p build/macos-debug --warnings-as-errors='*'` over every new/changed TU) — both clean, **zero new `NOLINT`s**, every finding fixed with a real code change (see the Part 1 entry for the list). Step 5's `PlaceholderPanel` deletion touched zero CMake beyond the one source-line removal already counted above; the historical comments that merely name `PlaceholderPanel` (`shell_ui.cpp:1`, `editor_app.cpp`'s "`-- was a PlaceholderPanel`" trail) were deliberately left alone, since they describe what task 2.1.3 did and remain true — the D24 grep for the three *live* forms (a build entry, an `#include`, a construction) is empty; only that one historical-prose hit remains, exactly as the plan predicted it would.

#### Task 2.2.4 — Asset browser stub
**2.2.4 ("Asset browser stub") needed no `vcpkg.json`/`/vcpkg` pin/`ci.yml`/`cmake/**`/`.github/scripts/**`/boundary-guard change, and — unlike every other Epic 2.2 task — it made ZERO change under `engine/`.** The whole diff is five new files plus four edits, all inside `editor/`, `tests/` and docs. `editor/CMakeLists.txt` gained **exactly two source lines** on `aero_editor_core` (`src/project_files.cpp`, `src/asset_browser_panel.cpp`; 16 → 18 sources) and **no link-line change** — `std::filesystem` needs no library on any lane, which is precedent rather than assumption (`engine/core/CMakeLists.txt:72` compiles `vfs.cpp` and links only `spdlog glm enkiTS aero::profiling`) — and `imgui::imgui` was already PRIVATE on that target, so the new ImGui TU adds nothing to the link line either. `tests/CMakeLists.txt` changed **exactly one functional line**: `aero_editor_shell_test`'s `add_executable` gained the single token `editor/project_files_test.cpp` (wrapped onto a second line for the 120-column limit) plus a two-sentence comment; its `target_link_libraries`, every `*_boundary_probe` link line, `aero_tests`' whole block and `aero_editor_imgui_test`'s whole CMake block are **byte-identical** — verified by an added/removed-lines-only grep for `target_link_libraries|boundary_probe|add_test` over the hunk, which is empty. **No new ctest entry, no new CMake target, no new dependency, no new CI step, no allowlist change**: both new test TUs ride existing targets, so `ctest -N` reads **94** on both macOS presets at **every one of the five commit boundaries**, and the tools-OFF configure reads **5**. `check-math-boundary.sh` is the one guard that sees this diff at all (it scans every tracked C-family file tree-wide, minus the single allowlisted `glm_backend.cpp`), and its anti-vacuity `scanned` count grew from **184 to 189** — **five files, not the four the plan predicted**, because the plan overlooked that `tests/editor/project_files_test.cpp` is tracked and C-family too. Nothing in this task includes GLM, so its verdict is unchanged and no allowlist moved; the other four guards scan only `engine`/`runtime` or `engine/*/include/*` and cannot see this diff. All five ran green locally, as did the configure-time `aero_assert_golden_rule()` link-graph walk (this task creates no target, so the walked target set is identical). **This task adds NO tools gate and NO new tools-OFF WARN** — unlike 2.2.2 (reflection) and 2.2.3 (shaders), it depends on neither codegen nor shaders, and the browser is **fully functional** under `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF`. The proof is a fresh tools-OFF configure whose launched `aero_editor` log carries **exactly two WARN lines and no third** — both pre-existing (2.2.2's `built without AERO_REFLECT_TOOLS`, 2.2.3's `built with -DAERO_SHADER_TOOLS=OFF`) — alongside a hit on the new `editor: assets root '…'` INFO line and zero ERROR/CRITICAL lines. `editor/include/aero/editor/editor_app.hpp` gained one `#include <string>` and one field (`projectRoot`, appended LAST so every existing designated-initializer call site keeps compiling in declaration order), and one **private** constructor signature change from `const EditorAppConfig&` to by-value-and-move: `EditorAppConfig` stopped being trivially copyable the moment it gained a `std::string`, and `modernize-pass-by-value` is `--warnings-as-errors` on the Linux Debug lane. That was the only unforeseen ripple. `imgui_layer.{hpp,cpp}` stayed byte-identical for the **fifth task running** (INV-7), as did every other existing editor source and every public editor header but `editor_app.hpp`. The four post-review fix commits changed **no CMake file at all** (`git diff` between the docs commit and HEAD over `editor/CMakeLists.txt` and `tests/CMakeLists.txt` is empty), so every count above still holds: `ctest -N` 94/94/5, no new target, no new entry, no new dependency. Local lint was run before every commit with the keg-only Homebrew LLVM 18 (`clang-format-18 --dry-run --Werror` over the changed files; `SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path) clang-tidy-18 -p build/macos-debug --warnings-as-errors='*'` over all six new/changed TUs) and both are clean with **zero new `NOLINT`s** — the two findings clang-tidy raised (`bugprone-exception-escape` on `leafOf`, `modernize-pass-by-value` on `EditorApp`'s private constructor) were **fixed**, not suppressed. Both macOS presets configure, build warning-free and pass 94/94 at every commit boundary, with the `AERO_REQUIRE_GPU=1` CI ratchet rehearsed green on both. R6 was measured rather than assumed: tier-0 case 9 writes 10 005 zero-byte files into an RAII temp directory on every run, and `aero_editor_shell_test`'s isolated ctest wall time went **0.84 s → ~1.21 s** under Debug/ASan (three runs: 1.27 / 1.20 / 1.21 s) — a **~+0.4 s** tax. (The code-review round's new examined-cap case later took this to **~3.3 s**; see the review entry in Part 1.) Case 9's own share is the bulk of it: running the target with case 9 *excluded* takes 0.20 s, and case 9 alone takes ~1.09 s. That is far under the ~10 s threshold at which the honest fix would have been to lower `MAX_ENTRIES_PER_DIRECTORY` rather than weaken the assertion, so the cap stands unchanged.

#### Task 2.3.1 — Editor camera
**2.3.1 ("Editor camera") needed no `vcpkg.json`/`/vcpkg` pin/`ci.yml`/`cmake/**`/`.github/scripts/**`/boundary-guard change.** `engine/scene_render/`: `scene_renderer.{hpp,cpp}` changed by a defaulted trailing parameter on two existing signatures (`buildRenderView`, `SceneRenderer::render`) plus the three-arm restructure of the camera-resolution block — **no new file, no CMake change** (the source lists and link lines for `aero_scene_render` are untouched). `tests/scene_render_test.cpp` gained four tier-0 `TEST_CASE`s and one GPU-gated case in the existing `#if AERO_SHADER_TOOLS_ENABLED` block — `aero_tests`' source list and link line are byte-identical (the file already rode that target). `editor/CMakeLists.txt` gained **two source lines** on `aero_editor_core` (`src/editor_camera.cpp`, `src/scene_bounds.cpp`; 19 → 21 sources) and **no link-line change** — both new TUs need only `aero::core`/`aero::scene`, already PUBLIC. `tests/CMakeLists.txt` gained **two source tokens** on `aero_editor_shell_test`'s existing `add_executable` (`editor/editor_camera_test.cpp`, `editor/scene_bounds_test.cpp`) — its `target_link_libraries`, every `*_boundary_probe` line, `aero_tests`' whole block and `aero_editor_imgui_test`'s whole block are byte-identical, verified by `git diff origin/main -- tests/CMakeLists.txt | grep -E 'target_link_libraries|boundary_probe|add_test'` returning only the unchanged `target_link_libraries` line as context, never a diff line. `tests/editor/shell_test.cpp` gained one `CHECK_FALSE` on the existing Panel-defaults case plus one new `TEST_CASE` (the `PanelContext.deltaSeconds` tier-0 half of AC-20); `tests/editor/imgui_layer_test.cpp` gained four GPU-gated `TEST_CASE`s. **No new ctest entry anywhere** — every new TU rides an existing target (`aero_tests`, `aero_editor_shell_test`, `aero_editor_imgui_test`), so `ctest -N` reads **94** on both macOS presets at every one of the eight commit boundaries, and a fresh `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` configure (`build/tools-off-2.3.1`) reads **5**. `check-math-boundary.sh` is the one guard that sees this diff at all (it scans every tracked C-family file tree-wide minus the single allowlisted `glm_backend.cpp`); its anti-vacuity `scanned` count moved **191 → 197** (+6: `editor_camera.hpp`, `editor_camera.cpp`, `scene_bounds.hpp`, `scene_bounds.cpp`, `editor_camera_test.cpp`, `scene_bounds_test.cpp` — measured with a disposable `git worktree` against `origin/main`, not assumed) — nothing in this task includes GLM, so the verdict is unchanged and no allowlist moved. `check-golden-rule.sh`, `check-rhi-boundary.sh` and `check-scene-boundary.sh` all see the `scene_render_test.cpp`/`scene_renderer.{hpp,cpp}` half of this diff (the one `engine/` change) and stayed unchanged — no editor include, no `SDL_*GPU` identifier, no `entt::`/`ENTT_` identifier was added; `check-platform-boundary.sh` cannot see this diff at all. The other four guards' scan roots are untouched by the editor-only steps. **This task adds NO tools gate and NO new tools-OFF WARN**: a fresh tools-OFF configure builds 5/5 and the launched `aero_editor` carries **exactly two WARN lines and no third** (2.2.2's reflection WARN, 2.2.3's shader WARN) — E23 explains why: `ViewportPanel::onDraw` returns at its step-4 unavailable-message branch, before the new navigation block, when the panel is `Unavailable`, so no gesture ever latches and nothing about the camera logs. `editor/include/aero/editor/editor_app.hpp` gained one forward declaration (`class EditorCamera;`, mirroring the existing `ViewportPanel`/`ConsolePanel` pattern) and one accessor pair (`viewportCamera()`); `editor_app.cpp` gained one include and two accessor definitions. `editor/include/aero/editor/panel_context.hpp` gained one defaulted field (`deltaSeconds`); `panel.hpp` gained one named boolean (`noScrollWithMouse`), appended last so every existing designated initializer keeps compiling; `shell_ui.cpp` gained one flag-mapping line. `imgui_layer.{hpp,cpp}` and `main.cpp` stayed byte-identical for the **seventh task running** (INV-7) — verified by `git diff origin/main` returning empty for all three. Both macOS presets configure, build warning-free and pass 94/94 at every one of the eight commit boundaries, with the `AERO_REQUIRE_GPU=1` CI ratchet rehearsed green on both. Local lint was run before every commit with the keg-only Homebrew LLVM 18 (`clang-format-18 --dry-run --Werror` over every changed file; `SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path) clang-tidy-18 -p build/macos-debug --warnings-as-errors='*'` over every new/changed TU) — both clean, **zero new `NOLINT`s** (the one pre-existing `reinterpret_cast` NOLINT in `viewport_panel.cpp`, 2.2.3, is unchanged), every finding fixed with a real code change (two `misc-const-correctness` locals, one `modernize-use-std-numbers` call-site, one `readability-identifier-naming` rename of a non-`camelBack` test global) rather than suppressed.

#### Task 2.3.2 — Selection & picking
**2.3.2 ("Selection & picking") needed no `vcpkg.json`/`/vcpkg` pin/`ci.yml`/`cmake/**`/`.github/scripts/**`/boundary-guard change, and made ZERO change under `engine/`, `runtime/`, `samples/`, `tools/`, `shaders/`, `cmake/` or `.github/`** — the first Epic-2.3 task to spend none of the epic's engine budget. `editor/CMakeLists.txt` gained **two source lines** on `aero_editor_core` (`src/picking.cpp`, `src/selection_overlay.cpp`; 21 → 23 sources) and **no link-line change** — both new TUs need only `aero::core`/`aero::scene`, already PUBLIC on that target. `tests/CMakeLists.txt` gained **two source tokens** on `aero_editor_shell_test`'s existing `add_executable` (`editor/picking_test.cpp`, `editor/selection_overlay_test.cpp`) plus two comment paragraphs — its `target_link_libraries`, every `*_boundary_probe` line, `aero_tests`' whole block and `aero_editor_imgui_test`'s whole block are byte-identical, verified with `git diff origin/main -- tests/CMakeLists.txt editor/CMakeLists.txt` showing only source-list and comment hunks. `tests/editor/imgui_layer_test.cpp` gained two includes and five GPU-gated `TEST_CASE`s on its existing target — **no new `add_test` anywhere**. `ctest -N` reads **94** on both macOS presets at every one of the five commit boundaries, and a fresh `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` configure (`build/tools-off-2.3.2`) reads **5**. `check-math-boundary.sh` is the one guard that sees this diff at all; its anti-vacuity `scanned` count moved **197 → 203** (+6: `picking.hpp`, `selection_overlay.hpp`, `picking.cpp`, `selection_overlay.cpp`, `picking_test.cpp`, `selection_overlay_test.cpp` — measured with a disposable `git worktree` against `origin/main`, not assumed) — nothing in this task includes GLM, so the verdict is unchanged and no allowlist moved; `check-golden-rule.sh`, `check-rhi-boundary.sh`, `check-scene-boundary.sh` and `check-platform-boundary.sh` cannot see this diff at all (no `engine/` file changed, no SDL/EnTT identifier touched). **This task adds NO tools gate and NO new tools-OFF WARN** (E11): a fresh tools-OFF configure builds 5/5 and the launched `aero_editor` carries **exactly two WARN lines and no third** (2.2.2's reflection WARN, 2.2.3's shader WARN) — `ViewportPanel::onDraw` returns at its status gate, before phase 8c, whenever the Viewport is `Unavailable`, so no picking or overlay code runs at all under tools-OFF. `editor/src/viewport_panel.hpp` gained one include (`selection_overlay.hpp`), one include (`<vector>`), two private methods (`updatePick`, `drawSelectionOverlay`) and three members (`pickArmed`, `pickPressPos`, `overlayScratch`). `editor/src/viewport_panel.cpp` gained two includes (`picking.hpp` alongside the existing `scene_bounds.hpp`/`selection.hpp`), four colour/thickness constants beside `VIEWPORT_CLEAR_COLOR`, two pure insertions in `onDraw` (phase 8c between the aspect computation and the `F`-focus block, phase 8e between the `F`-focus block and the size-readout overlay) and the two new method bodies placed between `focusSelection` and `renderScene` — **`ViewportPanel::renderScene` itself is byte-identical**, confirmed by `git diff origin/main -- editor/src/viewport_panel.cpp` showing no hunk at or after its signature (AC-19/INV-3). `editor/src/imgui_layer.{hpp,cpp}` and `editor/src/main.cpp` stayed byte-identical for the **eighth task running** (INV-7). Both macOS presets configure, build warning-free and pass 94/94 at every one of the five commit boundaries, with the `AERO_REQUIRE_GPU=1` CI ratchet rehearsed green on both. Local lint was run before every commit with the keg-only Homebrew LLVM 18 (`clang-format-18 --dry-run --Werror` over every changed file — several needed a formatting pass after hand-authoring, accepted as-is per the plan's own R-1 guidance; `SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path) clang-tidy-18 -p build/macos-debug --warnings-as-errors='*'` over every new/changed TU) — both clean, **zero new `NOLINT`s** (the one pre-existing `reinterpret_cast` NOLINT in `viewport_panel.cpp`, 2.2.3, is unchanged); the one clang-tidy finding raised during development (`modernize-use-auto` on a `static_cast`-initialised local in `selection_overlay_test.cpp`) was fixed with a real code change, not suppressed. All thirteen sabotage proofs were performed, each seed confirmed landed via `git diff` before trusting the verdict, each reverted and re-confirmed green afterward; S3 and S8 additionally passed the second-order `CHECK(true)` check (with the discriminating assertions weakened, the seeded defect passes the whole suite, proving the assertions — not the harness — do the work).

#### Task 2.3.3 — ImGuizmo transform gizmos

**2.3.3 is the first task to add a new vcpkg dependency since 0.4.3 (`shaderc`'s SDL_shadercross toolchain) and the first editor-side one since 2.1.1's `imgui` itself** — `imguizmo` joins `vcpkg.json`'s `dependencies` array, alphabetically after the `imgui` object and before `miniaudio`, with **`builtin-baseline` and the `/vcpkg` submodule SHA byte-identical**: F1 (verified at source against the pinned submodule commit) proved `imguizmo` already resolves to `1.10` at the existing baseline, so the CI job asserting `submodule SHA == builtin-baseline` stays green by changing neither. The port is `vcpkg_check_linkage(ONLY_STATIC_LIBRARY)`, MIT-licensed, installs its single header **flat** into `include/` (so the spelling is `#include <ImGuizmo.h>`, not `<ImGuizmo/ImGuizmo.h>`), and exports `imguizmo::imguizmo` with **no `INTERFACE_LINK_LIBRARIES`** — it reaches `imgui` only via `PRIVATE` include dirs baked in at the port's own build time, so `find_package(imguizmo CONFIG REQUIRED)` pulls nothing else onto `editor/CMakeLists.txt`'s line. `imguizmo::imguizmo` is linked `PRIVATE` on `aero_editor_core` only, immediately after `imgui::imgui`, for the identical reason (D1/2.1.3): it confines every ImGuizmo symbol to `editor/src/` TUs, so `aero_editor` and `aero_editor_shell_test` stay ImGuizmo-free at source, held by file placement and review (R12), not by a probe. **No guard or allowlist changed anywhere** (G2, confirmed at source): `check-golden-rule.sh`'s `SCAN_ROOTS` is `('engine' 'runtime')`, an include scan with no dependency allowlist at all, and `aero_assert_golden_rule`'s `CMAKE_CURRENT_FUNCTION` link-graph walk is directory-rooted (`file(RELATIVE_PATH)`), never a substring match on a target or package name — adding `imguizmo::imguizmo` to an `/editor` target is structurally invisible to both, by design, and `golden-rule.link_graph_e2e`'s six-reconfigure fixture proves it directly: it now sees `imguizmo::imguizmo` on an editor target, exactly where the golden rule says a third-party dependency is allowed to live. The math/platform/rhi/scene boundary guards are likewise untouched: this task adds no GLM, SDL, SDL_GPU or EnTT identifier anywhere, and touches no `engine/` file at all. **`.clang-format` also changed — the first `IncludeCategories` edit since task 0.1.6 stood the file up.** One new entry, `- Regex: '^<ImGuizmo\.h>' / Priority: 5`, listed FIRST so it wins clang-format's "first matching category" tie-break ahead of the generic third-party-with-path rule; empirically reproduced against the pinned Homebrew clang-format 18.1.8 (the CI version): without the entry, `<ImGuizmo.h>` sorts as a no-path third-party include and gets grouped with `<algorithm>`/`<cmath>`/etc under `SortIncludes: CaseSensitive`, where `'I'` (0x49) sorts before every lowercase entry and hoists it above `<imgui.h>` — which does not compile, since `ImGuizmo.h` never includes `imgui.h` itself. The patched config was run over **every tracked `*.cpp/*.hpp/*.h/*.inl` file at HEAD before this task's own includes existed** and is a byte-for-byte **no-op** (`--dry-run --Werror`, exit 0, empty output) — expected, since the new regex matches nothing until this task's own two `#include <ImGuizmo.h>` lines land, and confirmed a second time, tree-wide, at the end of this task with those two lines in place. `tests/CMakeLists.txt` gained **two source tokens** on `aero_editor_shell_test`'s existing `add_executable` (`editor/gizmo_test.cpp`, `editor/transform_ops_test.cpp`) plus two comment paragraphs in the file's running style — its `target_link_libraries`, every `*_boundary_probe` line, `aero_tests`' whole block and `aero_editor_imgui_test`'s whole block are byte-identical, confirmed with `git diff origin/main -- tests/CMakeLists.txt editor/CMakeLists.txt` showing only source-list/comment hunks plus the one `imguizmo::imguizmo` link token. `tests/editor/imgui_layer_test.cpp` gained two includes (`<aero/editor/gizmo.hpp>`, `<SDL3/SDL_filesystem.h>` — the latter a genuinely new DIRECT SDL3 touch in a TU whose own header comment previously claimed SDL3 reached it "purely transitively"; the comment was corrected in the same commit, not left stale) plus three more standard-library includes (`<filesystem>`, `<fstream>`, `<sstream>`) and five GPU-gated `TEST_CASE`s on its existing target — **no new `add_test` anywhere**. `ctest -N` reads **94** on both macOS presets at every one of the six commit boundaries, a fresh `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` configure (`build/tools-off-2.3.3`) reads **5**, and the new **`-DAERO_REFLECT_TOOLS=OFF` alone** configuration this task's own AC-17 adds to the gate (`build/reflect-off-2.3.3`) reads **18**, measured not assumed (the tools-OFF five plus the thirteen `shaderc.*` cases, every `if(AERO_REFLECT_TOOLS)` CMake block simply absent from the build graph). `check-math-boundary.sh` is the one guard that sees this task's diff at all; its scanned count moved **203 → 209** (+6: `gizmo.hpp`, `transform_ops.hpp`, `gizmo.cpp`, `transform_ops.cpp`, `gizmo_test.cpp`, `transform_ops_test.cpp` — measured with a disposable `git worktree` against `origin/main`, not assumed). `editor/src/viewport_panel.hpp` gained one include (`gizmo.hpp`), two private methods (`updateGizmo`, `drawGizmoBar`) and five members (`gizmoMode`, `gizmoActive`, `gizmoHasTarget`, `gizmoWasUsing`, `gizmoWarnLatched`). `editor/src/viewport_panel.cpp` gained three includes (`gizmo.hpp`, `transform_ops.hpp` alongside the existing `picking.hpp`/`scene_bounds.hpp`/`selection.hpp`, plus `<ImGuizmo.h>` in its own trailing category), a two-function anonymous-namespace enum bridge, one phase insertion in `onDraw` (8b′, between the camera and picking phases — load-bearing in both directions per D4), one rewritten condition plus comment in `updatePick`'s arm gate (folding the pre-existing camera-gesture disarm and the new gizmo disarm into one check, not two), and the two new method bodies placed between `updatePick` and `drawSelectionOverlay` — **`ViewportPanel::renderScene` itself is byte-identical**, confirmed by `git diff origin/main -- editor/src/viewport_panel.cpp` showing no hunk at or after its signature (INV-2/INV-3). `editor/src/shell_ui.cpp` gained one include and one call (`ImGuizmo::BeginFrame()`, first statement in `drawShellUi`, before `drawMenuBar`) — no other line of that file changed. `editor/src/imgui_layer.{hpp,cpp}`, `editor/src/main.cpp` and `editor/src/editor_app.cpp` stayed byte-identical for the **ninth task running** (INV-7). Both macOS presets configure, build warning-free and pass 94/94 at every one of the six commit boundaries, with the `AERO_REQUIRE_GPU=1` CI ratchet rehearsed green on both. Local lint was run before every commit with the keg-only Homebrew LLVM 18 — `clang-format-18 --dry-run --Werror` over every changed file, **plus a tree-wide pass this task specifically** because `.clang-format` itself changed (S11 is the reason); `SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path) clang-tidy-18 -p build/macos-debug --warnings-as-errors='*'` over every new/changed TU — both clean, **zero new `NOLINT`s** (the one pre-existing `reinterpret_cast` NOLINT in `viewport_panel.cpp`, 2.2.3, is unchanged); two clang-tidy findings surfaced during development and were both fixed with real code changes, never suppressed: `bugprone-branch-clone` on the initial G17 enum-totality test (three/two/four identical branches; fixed by writing to a distinct `std::array<bool,N>` slot per branch) and `modernize-use-auto` on two `Transform*`-typed locals in `transform_ops.cpp` initialised from a template-cast `world.get<Transform>(...)`. All twelve sabotage proofs were performed, each seed confirmed landed via `git diff` before trusting the verdict, each reverted and re-confirmed green afterward; S4 and S5 additionally passed the second-order `CHECK(true)` check.

### Epic 2.4 — Undo/redo

#### Task 2.4.1 — Command stack

**2.4.1 ("Command stack") is the quiet one — it changes NO dependency at all.** `vcpkg.json`'s
`builtin-baseline` and the `/vcpkg` submodule SHA are byte-identical; `.clang-format` and `.clang-tidy`
are byte-identical (`git diff origin/main -- .clang-format .clang-tidy` empty); **every
`target_link_libraries` line on every target is byte-identical** — `editor/CMakeLists.txt` gains **two
source lines** on `aero_editor_core`'s existing `add_library` (`src/command_stack.cpp`,
`src/transform_command.cpp`; 25 → 27) and no link-line change, because both new TUs need only
`aero::core`/`aero::scene`, already PUBLIC; `tests/CMakeLists.txt` gains **two source tokens** on
`aero_editor_shell_test`'s existing `add_executable` (`editor/command_stack_test.cpp`,
`editor/transform_command_test.cpp`) plus one comment paragraph, and **four includes** on
`aero_editor_imgui_test`'s existing `imgui_layer_test.cpp` TU — **no new `add_test` anywhere**, confirmed
by `git diff origin/main -- tests/CMakeLists.txt editor/CMakeLists.txt | grep -E 'add_test|find_package'`
returning empty. `ctest -N` reads **94** (tools ON), **5** (both tools OFF, `build/tools-off-2.4.1`) and
**18** (reflect OFF alone, `build/reflect-off-2.4.1`) at every one of the six commit boundaries — all
three configurations measured, none assumed. `check-math-boundary.sh` is the only guard that sees this
task's diff at all; its scanned count moved **209 → 215** (+6: `command_stack.hpp`,
`transform_command.hpp`, `command_stack.cpp`, `transform_command.cpp`, `command_stack_test.cpp`,
`transform_command_test.cpp` — measured with a disposable `git worktree` against `origin/main` at Step 0,
and reconfirmed directly on the finished tree at Step 7; both readings agree). `check-golden-rule.sh`,
`check-platform-boundary.sh`, `check-rhi-boundary.sh` and `check-scene-boundary.sh` cannot see this diff
at all — zero `engine/`, `runtime/`, `tools/`, `samples/`, `shaders/`, `cmake/` or `.github/` file changed
(`git diff --stat origin/main -- engine runtime tools samples cmake shaders .github vcpkg.json vcpkg`
empty), no SDL/SDL_GPU/EnTT identifier touched. `editor/include/aero/editor/panel_context.hpp` gains a
`class CommandStack;` forward declaration and a `CommandStack& commands` reference member (no new
`#include` — the header's own "forward declarations only" rule holds); `editor/include/aero/editor/
editor_app.hpp` gains one `#include <aero/editor/command_stack.hpp>` (a VALUE member needs the
definition), two accessors and two request methods. `editor/src/viewport_panel.cpp` gains two includes
(`command_stack.hpp`, `transform_command.hpp`) and no others — `<memory>` was already present since
2.2.3, so `std::make_unique` needed no new include. `editor/src/imgui_layer.{hpp,cpp}` and
`editor/src/main.cpp` stayed byte-identical for the **eleventh task running** (INV-7), measured against
the four genuine last-change commits (`imgui_layer.{hpp,cpp}` at task **2.1.1**, `main.cpp` at task
**2.2.4**, `editor_app.cpp` at task **2.3.1** — a two-task streak this task deliberately breaks, wiring
the command stack through `tick()`); no streak COUNT is asserted, only the byte-identity itself, per
§A3's correction of the streak arithmetic 2.3.3's own log entry carried forward in error.
`ViewportPanel::renderScene` byte-identical (`git diff origin/main -- editor/src/viewport_panel.cpp`
shows no hunk at or after its signature — the sole grep hit inside the diff is a REMOVED context line
from a comment this task's own edit moved away from the word "renderScene", not a touch on the function
itself). Both macOS presets green at every one of the six commit boundaries, Debug and Release, with and
without `AERO_REQUIRE_GPU=1`; clang-format/clang-tidy clean on every touched file, **zero new `NOLINT`s**;
this is the **first `dynamic_cast` in the tree** (`transform_command.cpp`'s merge downcast) and RTTI was
confirmed on on all three lanes by the absence of any `-fno-rtti`/`/GR-` flag. All fourteen sabotage
proofs performed and confirmed against the whole suite (see the Part 1 entry for the full per-seed
detail, the two second-order checks, and the one test-construction defect found and fixed along the way).

#### Task 2.4.2 — Property-set + structural commands

**2.4.2 touches exactly one `engine/` file pair and no `vcpkg.json`/`/vcpkg` pin/`ci.yml`/`cmake/**`/
boundary-guard change.** `engine/scene/include/aero/scene/world.hpp` and `engine/scene/src/world.cpp`
gain one primitive, `Entity World::recreate(Entity)`, plus a `using scene::internal::Traits;` alias its
occupancy pre-check needs (`git diff --name-only origin/main -- engine/` reads exactly those two files,
confirming AC-4/INV-8) — `tests/scene_test.cpp` gains seven cases (W1–W7) on `aero_tests`' existing
source list, **no link-line change**. `editor/CMakeLists.txt` gains **three source lines** on
`aero_editor_core`'s existing `add_library` (`src/scene_snapshot.cpp`, `src/component_commands.cpp`,
`src/entity_commands.cpp`; 27 → 30) and **no link-line change** — all three need only
`aero::core`/`aero::scene`, already PUBLIC. `tests/CMakeLists.txt` gains **five source tokens** across
two existing blocks: `aero_editor_shell_test` gains `editor/scene_snapshot_test.cpp` and
`editor/structural_commands_test.cpp`; the `AERO_REFLECT_TOOLS`-gated `aero_editor_inspector_test` gains
`editor/field_command_test.cpp` — every `target_link_libraries` line, every `*_boundary_probe` line and
`aero_editor_imgui_test`'s whole block are byte-identical, confirmed by `git diff origin/main --
tests/CMakeLists.txt editor/CMakeLists.txt` showing only source-list hunks and the CMakeLists comment
paragraphs already accounted for above. **No new `add_test` anywhere**: `ctest -N` reads **94** (tools
ON), **5** (`build/tools-off-2.4.2`) and **18** (`build/reflect-off-2.4.2`) at every one of the nine
commit boundaries — all three configurations measured, none assumed, and the reflect-OFF figure is
AC-8's actual proof (`scene_snapshot_test`/`structural_commands_test` run and pass there;
`field_command_test`'s target is correctly absent, living inside `if(AERO_REFLECT_TOOLS)`).
`check-math-boundary.sh` is the only guard that sees this task's diff at all; its scanned count moved
**215 → 224** (measured directly against the finished tree — the plan's own predicted 221 was wrong on
its own arithmetic, Part 1's finding 4c). `check-golden-rule.sh`, `check-platform-boundary.sh`,
`check-rhi-boundary.sh` and `check-scene-boundary.sh` all green with **no allowlist change** —
`check-scene-boundary.sh` has none to change (`HEADER_GLOB='engine/*/include/*'`, no allowlist at all,
§G9), and `world.hpp`'s one new declaration adds no third-party identifier. `editor/include/aero/editor/
panel_context.hpp` gains a `RootOrder&` member and the one bridge function, `toCommandContext`;
`editor/include/aero/editor/editor_app.hpp` gains a `RootOrder rootOrder;` member and a `roots()`
accessor pair (A11 — no trailing underscore, distinct accessor name); `command_stack.hpp` widens
`Command`/`CommandStack`'s three entry points to take `CommandContext&` (still **no engine header** —
`command_stack.cpp` needs none, §A25). Public editor headers **20 → 23**
(`scene_snapshot.hpp`, `component_commands.hpp`, `entity_commands.hpp`); the corrected INV-7
comment-stripped scan (2.4.1's pattern, not the spec's vacuous `Im[A-Z]` one) stays **empty** across all
23. `editor/src/imgui_layer.{hpp,cpp}` and `editor/src/main.cpp` stay byte-identical against
`origin/main` (no streak count asserted, only byte-identity, per 2.4.1's §A3 correction).
`ViewportPanel::renderScene` is untouched — Step 7/8's routing lands entirely in `inspector_panel.cpp`
and `hierarchy_panel.cpp`; `viewport_panel.cpp`'s only change is the Step 3 `CommandContext` signature
widening at its one existing push site. Both macOS presets green at every one of the nine commit
boundaries, Debug and Release, with and without `AERO_REQUIRE_GPU=1`; clang-format clean and clang-tidy
(`--warnings-as-errors='*'`) clean on every touched file; **one new `NOLINT`**
(`field_command_test.cpp:49`, `readability-identifier-naming` on a frozen codegen forward declaration,
identical in shape to `inspector_test.cpp:47`'s pre-existing one — Part 1's finding 4c region covers the
"why"). Local lint was run with the keg-only Homebrew LLVM 18 before every commit boundary check in this
Step 11 pass (`clang-format-18 --dry-run --Werror`, `SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path)
clang-tidy-18 -p build/macos-debug --warnings-as-errors='*'`), both clean. See the Part 1 entry above for
the full per-step build, the nine-commit rundown, the traps, the dead ends, the nine spec corrections and
the five findings that must be read before touching this subsystem again — most importantly, **§V4's
twenty sabotage seeds are now all run (finding 3/5): nine did not behave as the plan predicted, three of
those have no discriminator at all, and the phase-A rollback gap S6 exposed is closed by N13** — and
AC-10 (tag round-tripping) is an **honest, currently-unfixable gap** until Phase 4/5's project-defined
components land (H3).

### Epic 2.5 — Scene I/O

#### Task 2.5.1 — Save/load/new from editor

**2.5.1 changes no `engine/` file at all** (`git diff --name-only origin/main -- engine/` empty) and
touches no `vcpkg.json`/`/vcpkg` pin/`ci.yml`/`cmake/**`/boundary-guard file — the first Phase 2 editor
task since 2.4.1 to add a link-line change, and the smallest one yet: **exactly one** new PRIVATE entry,
`target_link_libraries(aero_editor_core PRIVATE aero::scene_serialize)`, inside the existing
`if(AERO_REFLECT_TOOLS)` block. `editor/CMakeLists.txt` gains **four source lines** on
`aero_editor_core`'s existing `add_library` (`src/scene_session.cpp`, `src/scene_file.cpp`,
`src/scene_io.cpp`, `src/file_dialog.cpp`; 30 → 34). `tests/CMakeLists.txt` gains two source tokens on
`aero_editor_shell_test`'s existing `add_executable` (`editor/scene_session_test.cpp`, unconditional) and
one `if(AERO_REFLECT_TOOLS) target_sources(aero_editor_shell_test PRIVATE editor/scene_io_test.cpp)
endif()` block (the same target gains a conditional source, not a second target — mirroring
`aero_editor_inspector_test`'s existing pattern rather than inventing a new one); `tests/editor/
imgui_layer_test.cpp` gains one include and six new GPU-gated cases on its existing target — **no new
`add_test` anywhere**, confirmed by `git diff origin/main -- tests/CMakeLists.txt editor/CMakeLists.txt
| grep -E 'add_test|find_package'` returning empty. `ctest -N` reads **94** (tools ON), **5** (both tools
OFF, `build/tools-off-2.5.1`) and **18** (reflect OFF alone, `build/reflect-off-2.5.1`) at every one of
the nine code-bearing commit boundaries — all three configurations measured, none assumed, and the
reflect-OFF figure is AC-6/E21's actual proof: `scene_session_test.cpp` runs and passes there (**275** of
its own cases, `--list-test-cases` — corrected from this entry's own original claim of 274, an arithmetic
error found by the 2026-07-31 code-review round: 289 total minus IO1–IO14's 14 cases is 275),
`scene_io_test.cpp` is confirmed **absent** (no object file, no matching
string anywhere under the build tree, not merely skipped), and the four new I12–I17 GPU cases in
`imgui_layer_test.cpp` fall back to a `sceneIoAvailable()` guard rather than failing. `check-math-boundary.
sh` is the only guard that sees this task's diff at all; its scanned count moved **224 → 232**, exactly
the plan's own predicted +8 for the eight new tracked C-family files (`scene_session.{hpp,cpp}`,
`scene_file.{hpp,cpp}`, `scene_io.{hpp,cpp}`, `file_dialog.{hpp,cpp}` — `file_dialog.hpp` is src-private,
not under `include/`, but still C-family and still tracked). `check-golden-rule.sh`,
`check-platform-boundary.sh`, `check-rhi-boundary.sh` and `check-scene-boundary.sh` cannot see this diff
at all — zero `engine/`, `runtime/`, `tools/`, `samples/`, `shaders/`, `cmake/` or `.github/` file changed,
no SDL/SDL_GPU/EnTT identifier touched in any public header (comment-stripped grep confirmed empty).
`editor/include/aero/editor/editor_app.hpp` gains forward declarations for `platform::Window` and
`DialogChannel`, four new members (`window`, `session`, `fileFlow`, `dialogChannel`), two new accessors
(`scenePath()`, `sceneDirty()`) and six request hooks; because `DialogChannel` is only forward-declared,
`~EditorApp()`, the move constructor and the move-assignment operator must be declared in the header and
defined (`= default`) out-of-line in `editor_app.cpp`, where the two pre-existing `static_assert`s
asserting `EditorApp`'s move-only, noexcept-move properties move to as well (§R-0's second pre-decided
item — the compiler cannot instantiate a defaulted special member against an incomplete type). `editor/
src/shell_ui.hpp` loses `ShellUiState::quitRequested` entirely and gains `FileMenuContext` (a
three-reference/value aggregate: `SceneSession&`, `FileFlow&`, `FileDialogHost` by value); `drawShellUi`
widens to a fourth parameter. `editor/src/imgui_layer.{hpp,cpp}`, `editor/src/main.cpp` and `editor/src/
project_files.{hpp,cpp}` stay byte-identical against `origin/main` (AC-37, confirmed by `git diff --stat`
reading empty for all five). `ViewportPanel::renderScene` and every other Viewport/Inspector/Hierarchy
file are untouched — this task's whole surface is the File menu, the shell, and the new scene-session/
scene-file/scene-io/file-dialog TUs. Both macOS presets green at every one of the nine commit boundaries,
Debug and Release, with and without `AERO_REQUIRE_GPU=1`; clang-format clean and clang-tidy
(`--warnings-as-errors='*'`) clean on every touched file; **zero new `NOLINT`**, against a corrected
baseline of **4** (not the plan's assumed 3 — `editor_reflection.cpp:10`'s own "NOLINTs" prose collision,
pre-existing, not introduced by this task). Local lint was run with the keg-only Homebrew LLVM 18 before
every commit boundary (`clang-format-18 --dry-run --Werror`, `SDKROOT=$(xcrun --sdk macosx15.4
--show-sdk-path) clang-tidy-18 -p build/macos-debug --warnings-as-errors='*'`), both clean throughout. See
the Part 1 entry above for the full per-step build, the commit rundown (**seventeen as merged**, after
`412639c` was folded back into `fba58b3` pre-merge — the arithmetic there has been wrong twice and is now
measured, not counted), the traps (INV-6, S1/S5's stronger
symptoms, S23's exposed gap, the missed-file commit, the recurring prose/grep-token collision), and the
full §V4 sabotage matrix — **all twenty-three seeds run and confirmed, all three mandatory second-order
checks (S1, S4, S10) run, §V4 discharged**, with the plan's own summary undercounting its predicted
non-discriminating/human-only count by three (nine measured, not six) and one seed (S23) exposing a real,
previously unwritten test case now closed.

**2.5.1 macOS human validation — PASS 20/20 applicable (2026-07-31).** Twenty of the twenty-two rows are
applicable on macOS and all twenty pass, closing every ⚠ row this platform can reach: the parented native
Save panel with its pre-filled name, the Open panel starting in the scene's own folder, the held `Ctrl+S`
writing exactly once (AC-3 — no test tier can hold a key), the File items greying while a dialog is open,
the D11 chain (Open → modal → Save → **native** Cancel → the Open is abandoned and nothing is lost), and
the atomic write surviving a force-quit with the original intact and no `.aero-tmp` beside it (the only
proof of temp-then-rename anywhere, since seed S6 measured non-discriminating at tier-0). Two rows carry
weight nothing else in this tree does. **Row 14** — Esc dismisses the modal as Cancel — is the *only*
evidence that AC-27 works: ImGui cannot deliver it (`NavUpdateCancelRequest`'s popup branch excludes
`ImGuiWindowFlags_Modal` at `imgui.cpp:15031`, `BeginPopupModal` always sets that flag at
`imgui.cpp:13232`, and the editor never enables `ImGuiConfigFlags_NavEnableKeyboard` at
`imgui_layer.cpp:79`), so the check is hand-bound at `shell_ui.cpp:212` and **nothing in CI would notice
its removal**. **Row 15** — move the Cube, delete it, New Scene (Don't Save), `⌘Z` `⌘Z`, and nothing
happens — is the INV-6 ghost-entity trap confirmed end to end, which is the whole reason
`resetSceneState` clears the `CommandStack` in the same operation that clears the `World`.

**Three things stay open despite the pass, and none of them should be read as closed.** Row 20 is
**Linux-only** by nature (a box with neither XDG portal nor zenity). Row 22 — re-run 2.2.5's four BLOCKED
rows against this task's INFO/WARN sources — was **not performed**, so 2.2.5 stays macOS-PARTIAL at 11/15;
**this is the second consecutive task (after 2.4.2) whose row 22 carried that clause and left it
undone**, which is a pattern rather than an accident: the clause keeps riding on a task that has twenty
rows of its own, and it will keep being skipped until it is scheduled as work in its own right. And
**AC-38's "including a run that opens and closes a dialog" half is unevidenced** — the pass exercised
dialogs but not under a sanitizer build, so sabotage seeds **S15** (the dangling `SCENE_FILTERS` array)
and **S16** (the `Ticket` use-after-free) still have no proof on any platform; they are ASan aborts
reachable only by a human running the Debug preset with those seeds present.

### Epic 2.6 — Project system v0

#### Task 2.6.1 — `project.json` + create/open flow

**`engine/` diff is empty** (`git diff --name-only origin/main -- engine/` empty) — the same property
2.5.1 established, now true of the project layer too: nothing here touches `vcpkg.json`, the `/vcpkg`
pin, `ci.yml`, `cmake/**`, or any of the five boundary-guard scripts' allowlists except
`check-math-boundary.sh`. `editor/CMakeLists.txt` gains **three** new source lines on
`aero_editor_core`'s existing `add_library` across the task (`src/text_file.cpp` — renamed from
`src/scene_file.cpp`, Step 1; `src/project.cpp`, Step 2; `src/project_file.cpp`, Step 3;
`src/project_ui.cpp`, Step 7) plus one new `target_compile_definitions(aero_editor_core PRIVATE
AERO_ENGINE_VERSION="${PROJECT_VERSION}")` line (Step 5) — no new `target_link_libraries` entry
anywhere; the project format needs nothing from `aero::scene_serialize` or any other engine target.
`tests/CMakeLists.txt` gains `editor/project_test.cpp` **unconditionally** on
`aero_editor_shell_test`'s existing `add_executable` (Step 2), an `AERO_PROJECT_FIXTURES_DIR` compile
definition and a relocation of `target_include_directories(aero_editor_shell_test ...)` out of the
`if(AERO_REFLECT_TOOLS)` gate (Step 4, §A16) — **no new `add_test` anywhere**, confirmed by
`git diff origin/main -- tests/CMakeLists.txt editor/CMakeLists.txt | grep -E
'add_test|find_package'` returning empty at every commit. `ctest -N` reads **94** (tools ON), **5**
(both tools OFF, `build/tools-off-2.6.1`) and **18** (reflect OFF alone, `build/reflect-off-2.6.1`) at
every one of the nine code-bearing commit boundaries and again in the final sabotage-pass sweep — all
three configurations measured, none assumed. `check-math-boundary.sh` is the only guard whose scanned
count moves at all: **239 → 241**, exactly `project_ui.hpp`/`project_ui.cpp` (Step 7's two new
C-family files) — every other boundary guard (`check-golden-rule.sh`, `check-platform-boundary.sh`,
`check-rhi-boundary.sh`, `check-scene-boundary.sh`) cannot see this diff at all: zero `engine/`,
`runtime/`, `tools/`, `samples/`, `shaders/`, `cmake/` or `.github/` file changed, no SDL/SDL_GPU/EnTT
identifier introduced in any public header. `editor/include/aero/editor/editor_app.hpp` gains a
renamed config field (`projectRoot` → `projectPath`), two new config fields
(`restoreLastProject`/`recentProjectsPath`), four accessors
(`projectIsOpen`/`projectRoot`/`projectName`/`assetBrowserRoot`), four request hooks
(`requestNewProject`/`requestOpenProjectDialog`/`requestOpenProject`/`requestClearRecentProjects`),
and a non-owning `AssetBrowserPanel*` member; `editor/include/aero/editor/project_files.hpp` **loses**
`resolveProjectRoot` entirely (superseded by the recents-driven resolution in `EditorApp::create()`).
`editor/src/scene_session.hpp`/`.cpp` gain the two new `FileAction`/`DialogKind` enumerators, the
`ProjectFlow`/`ProjectContext`/`NewProjectForm` structs, and `adoptProject`/`openProjectPath`/
`createAndOpenProject` — the only pure-function-body change anywhere in this task is `discardsWork`
gaining two `case` labels (D1). `editor/src/file_dialog.hpp`/`.cpp` gain
`launchOpenProjectFolderDialog`; `editor/src/shell_ui.hpp` gains `FileMenuContext::project` (a
reference, not a value, per the plan's own §S 5e correction). Both macOS presets green at every one of
the nine commit boundaries, Debug and Release, with and without `AERO_REQUIRE_GPU=1`; clang-format and
clang-tidy (`--warnings-as-errors='*'`) clean on all 25 touched `.cpp`/`.hpp` files, re-verified in a
final comprehensive pass after all 22 sabotage seeds were reverted. See the Part 1 entry above for the
full per-step build, the two real pre-commit bugs found and fixed (the unknown-key sweep's document
order, `project.flow.requestedPath`'s leak), the three self-referential comment traps, and the full
§V4 sabotage matrix — **all twenty-two seeds run and confirmed, all three mandatory second-order
checks (S1, S9, S11) run, §V4 discharged** — with two seeds (S11, S15) exposing real coverage gaps
this task's own tests cannot reach, four seeds under-catching their table-predicted case, one
over-catching, and S9 re-proving 2.5.2's mutual-bug design with a nuance (single-fixture regeneration
partially masks a semantic case, caught only by an unrelated multi-fixture loop) worth carrying
forward.

**2.6.1 has no human validation pass recorded by this implementation pass** — the sixteen rows in
`editor/validation/2.6.1-project-json-create-open-flow.md` (local-only) are all `⏳ pending`, following
the project's own standing rule never to record a PASS in the same pass that writes the code.

##### Task 2.6.1 — code-review round (2 blocking + 6 should-fix, zero new commits sharing docs)

**Found 2 blocking + 6 should-fix defects, all closed** — no new engine change (`git diff
--name-only origin/main -- engine/` stays empty), no `editor/VALIDATION.md`/`editor/validation/`
churn committed.

**BLOCKING-1 — the New Project modal never called `ImGui::CloseCurrentPopup()`.** `project_ui.cpp`
had `OpenPopup`/`BeginPopupModal`/`EndPopup` and zero `CloseCurrentPopup`, unlike
`drawUnsavedChangesModal`'s four call sites in `shell_ui.cpp`. Clearing `form.open` only stops THIS
TU from re-submitting the popup next frame; ImGui itself owns `g.OpenPopupStack` and never GCs an
entry for a popup that simply stops being submitted (`GetTopMostPopupModal()` checks only the
`Modal` flag, never `Active`), so `imgui.cpp`'s own bookkeeping sets `g.HoveredWindow = NULL` every
frame thereafter — every menu, panel, button and dock tab becomes unhoverable and unclickable after
any Create or Cancel, until a stray click happens to trim the stack. Fixed by calling
`CloseCurrentPopup()` UNCONDITIONALLY on Create, Cancel and the hand-bound Esc handler — a FAILED
create leaves `form.open == true`, so `project_ui.cpp`'s own `OpenPopup` re-opens the popup next
frame from that state, exactly like a fresh `NewProject` request would. **No test tier can drive a
live ImGui popup** (the FileDialogHost/AC-27 precedent), so the regression proof
(`project_test.cpp`'s `PU1`) is mechanical: it reads `project_ui.cpp`'s OWN SOURCE TEXT (a new
`AERO_EDITOR_SRC_DIR` compile definition, the `AERO_PROJECT_FIXTURES_DIR` precedent) and asserts
each of the three closing paths' own `*Requested = true;` line is followed, within a small window,
by a `CloseCurrentPopup()` call — the same textual-grep principle the five CI architecture guards
already use, run as a doctest case because what is being checked is one file's own text, not a
repo-wide invariant.

**BLOCKING-2 — the GPU suite overwrote the real, machine-wide `recent_projects.json`.** I18 and I21
(`imgui_layer_test.cpp`) set `.restoreLastProject = false` but no `.recentProjectsPath`; `restoreLastProject
== false` only ever suppressed the READ (`readRecentProjects` at `create()`), never the WRITE — the
recents flush inside `tick()` (`if (projectFlow.recentsDirty) { writeRecentProjects(recentsPath,
recents); }`) runs unconditionally on `recentsDirty`, and `recentsPath` resolves to
`defaultRecentProjectsPath()` (`~/Library/Application Support/AeroEngine/AeroEditor/recent_projects.json`
on macOS) whenever `config.recentProjectsPath` is empty, with no dependency on `restoreLastProject`
at all. **Confirmed already landed on the implementation machine**: the real file read a stray
`aero_imgui_layer_project_2/MyGame` entry before this fix. Fixed by pointing both cases at
`uniqueRecentsFile()` (I23/I24's own helper) and auditing every other `EditorAppConfig` literal in
the file for the same hole — none found (only three `.projectPath` assignments exist tree-wide: `""`,
a nonexistent path, and I18's `created.root`; only I18/I21/I22 call `requestOpenProject`, and I22's
own scenario is a GUARDED swap that never reaches `adoptProject`). The file-top banner (previously
"no test ever reads the real pref directory") now states the write-side risk explicitly too.
**Decision on the harder question — should `restoreLastProject == false` also suppress the flush:
NO.** The two are deliberately orthogonal: `restoreLastProject` is about auto-opening at LAUNCH;
the recents flush is about remembering whatever project a session actually opened, for a FUTURE
launch (which might set `restoreLastProject = true`). Conflating them would silently break "don't
auto-restore, but do remember what I just opened" for any real embedder that wants exactly that
combination — a new bug disguised as a fix. The real defect was that tests left
`recentProjectsPath` unset, not that the write path exists; comprehensive test coverage is the
correct fix, not narrowing production behavior. **Acceptance test, run and confirmed**: deleted the
polluted `~/Library/Application Support/AeroEngine/AeroEditor/recent_projects.json`, re-ran the full
`AERO_REQUIRE_GPU=1 ctest` suite (95/95 green), and confirmed the directory stayed empty afterward —
the file is not recreated.

**Closed S15 (the reviewer's condition for merge).** `EditorApp::recentProjectCount()` added (the
`assetBrowserRoot()`/A9 precedent — a fourth black-box accessor existing only because AC-34 had no
signature to assert against otherwise), and I24 now asserts `recentProjectCount() == 0` directly
rather than only through the downstream consequence (`projectIsOpen()`/`projectRoot()` staying
empty) — this is what makes sabotage seed S15 (dropping the `if (config.restoreLastProject)` guard
around `readRecentProjects`) redden.

**SHOULD-FIX 5 — the Browse button had no in-flight dialog interlock, and an orphaned result fell
into the Save arm (2.5.1's BLOCKING-2 shape again).** `applyFileRequests`'s `browseRequested`
consumer ran at step 0, before the refusal check further down (`flow.dialog != DialogKind::None ||
...`), with no dialog-state guard of its own — every OTHER launcher in the file is protected by that
later check, this one uniquely was not. A second Browse click before the first answers overwrites
`flow.dialog` and (with a real channel) launches a SECOND native dialog; `DialogChannel::take()`
always resets its slot, so whichever result answers SECOND is consumed with `flow.dialog` already
reset to `None` by the first. `applyDialogResult`'s kind chain had no arm for `kind == None` and its
final arm never even checked `kind == DialogKind::Save` explicitly ("by elimination," per its own
comment) — an orphan fell straight through into it, silently saving the CURRENT scene to
`"<picked folder>.scene.json"` and rebinding the session path. **Two independent one-line closures,
both applied**: the browse arm now also requires `flow.dialog == DialogKind::None`, and
`applyDialogResult` now returns immediately when `kind == DialogKind::None`, before touching
`flow.pending`/`saveBeforePending`/`requestedPath` (none of those belong to an unrelated orphan).
`scene_session_test.cpp`'s new `SS35` drives the orphan-result half directly (a `flow.dialog ==
DialogKind::None` result arriving with `ready = true` and its own path); the browse-arm interlock
itself is **not independently testable at any tier** — launching a real dialog with no parent window
would risk a blocking `[dialog runModal]` on macOS (`file_dialog.cpp`'s own A1 note), so proving it
would require the one thing this codebase has never risked in a test.

**SHOULD-FIX 8 — `createProject` computed the wrong root for a project literally named
`"project.json"`.** `validateProjectName("project.json")` is `Ok` (no separator, not a dot-name, not
a reserved device name), so `createProject` legitimately scaffolds
`<location>/project.json/{assets,scenes,project.json}` — but `out.root` was re-derived by running the
scaffolded `target` path back through `projectRootFromPath`, whose manifest-name strip fires
whenever the FINAL path component reads `"project.json"` (a rule meant for a CALLER-supplied path
that might already end in the manifest file name, not for a root this function just built with its
own hands) — taking `out.root` one level too high, silently. Fixed by normalizing `target` ONCE with
`.lexically_normal()` at construction and using `utf8FromPath(target)` VERBATIM for `out.root`,
never re-deriving it. New case `P83` proves both `createProject`'s own output and the real
`createAndOpenProject` flow (which adopts `outcome.root` directly) land at the correct directory;
re-opening that SAME directory later by PATH remains a distinct, pre-existing, out-of-scope
ambiguity (`projectRootFromPath` cannot tell "the manifest file" from "a root whose own name happens
to be `project.json`") and is documented, not fixed, in the new test's own comment.

**SHOULD-FIX 3/4 — two ACs claimed proven in the plan's §T with nothing that actually executed
them.** Every `ProjectContext` under `tests/` carried `engineVersion = ""` (`FlowFixture`'s own
default), so AC-7's mismatch WARN and its `!empty()` guard were both unexercised; no case ever
called `openProjectPath` on a manifest with unknown keys, so AC-6's WARN **emission loop** (as
opposed to the parse-level collection PG7 already covered) never ran. New cases `P81` (a minimal
pair: `FlowFixture`'s own `""` engine version asserts `Warn == 0`, a second `ProjectContext` sharing
the same session/flow/recents but with `engineVersion = "9.9.9"` asserts `Warn == 1` and
`isOpen()`) and `P82` (the `unknown-keys.project.json` fixture's bytes written into a fresh
`TempDir` and opened through the real `openProjectPath`, asserting `Warn == 3`). The AC-7 WARN
message was also added to `docs/09` §4.7's "Success-only warnings" table, which Step 10a required
and had omitted.

**SHOULD-FIX 6 — `project_file.cpp`'s banner was false.** It claimed "NEVER LOGS, with exactly ONE
exception" (the pref-path CWD fallback); the file actually has three `AERO_LOG_WARN` sites — the
other two (`readRecentProjects`'s corrupt-file WARN, `writeRecentProjects`'s write-failure WARN) are
required by AC-23/AC-24 and were simply left out of the count. The code was already correct; only
the comment was rewritten.

**SHOULD-FIX 7 — `docs/09` §4.3 (project name validation) contradicted `validateProjectName` in
three rows.** Row 1 omitted that the Empty check trims first; Row 6 said "starts or ends with a
space," but the code checks only `utf8.back()` — a LEADING space is legal (`" Foo"` is `Ok` and
scaffolds a directory literally named that); Row 5's `IllegalChar` set listed `/` and `\`, which are
actually caught earlier by the `Separator` rule and can never reach `IllegalChar`. **Decision: the
code is spec-correct (D6 says only "no trailing space and no trailing `.`") and the doc
over-stated it — `docs/09` §4.3 was corrected to describe actual behavior; `validateProjectName`
was not touched.** Noted here, per the reviewer's own instruction, as the record of a deliberate
choice: a leading space in a project name is legal, on purpose, not an oversight.

**Sixth architecture guard: `check-project-no-delete.sh` (D7/INV-P4), promoted from a plan-only
grep.** The reviewer accepted sabotage seed S11 (`createProject`'s `WriteFailed` rollback branch is
genuinely unreachable by any test in the tree) as documented debt on ONE condition: the only
artefact proving a future `remove_all` would be caught was the plan's own §V7 grep, and
`docs/plans/` is gitignored — that grep ceases to exist the moment this branch merges. Added
`.github/scripts/check-project-no-delete.sh`, following the five existing guards' exact shape
(anti-vacuity self-tests, an 0/1/2 exit contract, `GITHUB_ACTIONS` annotations), asserting that
`remove_all`/`std::filesystem::remove`/`std::filesystem::rename`/a bare `::copy` never appears as
CODE (comment-stripped, the scene/rhi-boundary precedent) in exactly `editor/src/project.cpp`,
`project_file.cpp` and `project_ui.cpp` — a THREE-NAMED-FILE allowlist, not a glob over
`editor/src/`, since the invariant is specific to the project flow. Proven red-on-violation by a new
hermetic ctest case, `project-no-delete.no_delete_e2e` (`tests/project-no-delete/no_delete_e2e.cmake`,
mirroring `golden-rule.include_scan_e2e`'s scratch-git-repo shape exactly: six stages covering all
four forbidden forms, the comment-stripping false-positive proof, and the canary/vacuity exit-2
path), wired into `tests/CMakeLists.txt` right after the golden-rule e2e block and into `ci.yml`'s
`lint` job right after the golden-rule step. **Sabotage-proofed the guard itself, both directions**:
seeding a real `std::filesystem::remove_all` call into `project_file.cpp` reddens the guard
(confirmed, then reverted); weakening `FORBIDDEN_RE` to drop the `remove_all` alternative makes the
e2e ctest case itself go FATAL (self-test 2 catches it before the seeded stage even runs) — the
"breaking the guard is the real test" rule (`.claude/rules/boundary-guards.md`), applied here rather
than merely asserted. This is a **new ctest entry** in all three configurations: `94/5/18 → 95/6/19`.

**Verification, all green:** `macos-debug` and `macos-release` full ctest (95/95 both, `git status
--short` after each build empty), `AERO_REQUIRE_GPU=1 ctest --preset macos-debug` (95/95, including
the real-recents-file acceptance test above), `-DAERO_REFLECT_TOOLS=OFF` (19/19 ctest,
`aero_editor_shell_test` **337** cases, up from 332 — a figure `CLAUDE.md` had never recorded after
2.6.1's own implementation, closed alongside this round), `-DAERO_REFLECT_TOOLS=OFF
-DAERO_SHADER_TOOLS=OFF` (6/6 ctest). `aero_editor_shell_test` tools-ON: **361**, up from 356 (+5:
`P81`/`P82`/`P83`/`PU1` in `project_test.cpp`, `SS35` in `scene_session_test.cpp`).
`check-math-boundary.sh` stayed at **241** (no new C-family file — only `.sh`/`.cmake` additions and
edits to existing `.cpp`/`.hpp`). `git diff --name-only origin/main -- engine/` stayed empty.
clang-format and clang-tidy (`--warnings-as-errors='*'`) clean on every touched file, including one
real finding of its own: a new `constexpr std::string_view` local named `sourcePath` failed
`readability-identifier-naming` (must be `SCREAMING_SNAKE_CASE` even as a local) — renamed to
`SOURCE_PATH`. Two doctest bugs found and fixed while writing the new cases before they ever reached
CI: `P81`'s two `LogSink::take()` calls shared one `records` vector without an intervening
`.clear()` between blocks (the API's own documented precondition), aborting under `assert`; and
`P83`'s second `createAndOpenProject` call reused the SAME `TempDir` a prior `createProject` call had
already scaffolded a non-empty `"project.json"` directory into, tripping `TargetNotEmpty` — fixed
with a second, fresh `TempDir`.

#### Task 2.6.2 — Project settings panel stub

**`engine/` diff is empty for a FOURTH consecutive task** (2.5.1, 2.5.2, 2.6.1, 2.6.2) — nothing here
touches `vcpkg.json`, the `/vcpkg` pin, `ci.yml`, `cmake/**`, or any boundary-guard script's
allowlist except `check-math-boundary.sh`'s scanned-file count (which is not an allowlist, only a
count). Two new PUBLIC/src-private pairs land in `editor/`: `project_settings.{hpp,cpp}` (the pure
model, depends on `project.hpp` and nothing else — gate-free, D4) and `project_settings_panel.{hpp,cpp}`
(the src-private `Panel` subclass, the only ImGui in the panel). `editor/CMakeLists.txt` gains two
source lines on `aero_editor_core` and **no new `target_link_libraries` entry, no new
`target_compile_definitions`** — the model needs nothing the target does not already link, and the
panel needs only `imgui::imgui`, already `PRIVATE` since task 2.1.3.

`tests/CMakeLists.txt` gains one line — `editor/project_settings_test.cpp` on the UNCONDITIONAL
`aero_editor_shell_test` source list, next to `project_test.cpp` — and no new `add_test`, no new
`target_include_directories`, no new `target_compile_definitions`. `aero_editor_shell_test`'s
unconditional TU count moves **16 → 17**; `tests/CMakeLists.txt`'s `add_test` call count stays **18**.
`tests/editor/imgui_layer_test.cpp` gains one new `TEST_CASE` (I25, appended after I24) and rewrites
four existing `count() == 5` assertions to `count() == 6` — a rewrite, not an addition, so this
target's own `add_test` registration is untouched.

`docs/09-file-formats.md` is byte-identical (AC-32) — this task DISPLAYS a format 2.6.1 already froze,
it does not touch it. `.github/scripts/check-project-no-delete.sh` is byte-identical (§A14 of the
plan) — its `FORBIDDEN_FILES` allowlist stays the three named files from 2.6.1, deliberately not
widened to the two new `.cpp` files this task adds, because their real invariant is "no I/O at all"
(INV-S2), which the task's own `§V7` grep enforces and which is strictly stronger than "no delete".

**Inventory deltas, measured, never inferred:** `aero_editor_core` sources **37 → 39**;
`check-math-boundary.sh` scanned-file count **241 → 246** (+5: the two new headers, the two new
`.cpp` implementation files, and the one new test TU — measured after `git add` at every commit
boundary, the guard's own documented precondition since it scans tracked files only); `ctest -N`
**stays 95 / 6 / 19 in all three configurations** (AC-33) — no new `add_test` anywhere; default
panels registered **5 → 6**. `aero_editor_shell_test` case count **361 → 384** (+23, exactly
PS1–PS23) in the default build, and the IDENTICAL **337 → 360** in BOTH tools-OFF configurations
(`build/tools-off-2.6.2`, `build/reflect-off-2.6.2`, both configured fresh per §A3 rather than
reusing 2.6.1's stale directories) — proving D4/AC-14's whole claim that this TU inherits
`project.hpp`'s gate-free property rather than merely resembling it. `aero_editor_imgui_test`
**46 → 47** (+1, I25).

---

## 2.2.5-R — log/console panel, standalone re-validation (2026-08-01)

**Not a task.** A validation-only session with no code change: `git diff --name-only` over the
engine, editor, tests and tools trees is empty, and the only tracked edits are this entry and
`CLAUDE.md`'s state block. It exists because the work it did had been deferred four times.

**What ran.** The four macOS rows that task 2.2.5's human pass recorded as **BLOCKED** on 2026-07-28
— row 9 (`##`, `%s` and embedded-newline messages), row 10 (records captured while the Assets tab is
selected, AC-6), row 11 (exceeding 10 000 records stays smooth, footer shows the aged-out count) and
row 15 (non-ASCII renders as `?` but filters and copies byte-exactly). **All four PASS**, moving 2.2.5
from ⚠️ PARTIAL 11/15 to ✅ **PASS 15/15** on macOS. Every Phase 2 task now carries a full macOS pass,
and 2.2.5's gate is held open only by the phase-wide Windows/Linux debt.

**Why they were blocked, and what unblocked them.** The original pass verified exhaustively that the
editor emitted **no log record after startup** — no `AERO_LOG_TRACE`/`AERO_LOG_DEBUG` call site
existed anywhere in the first-party tree, and every reachable site was either a failure path or a
once-per-lifetime startup notice. A successful resize, click, selection or dock change logged nothing,
so a row instructing the tester to "cause some logging" could not be carried out *as written*. That is
worth restating precisely: the rows were not failing and the panel was not broken — the rows were
**unrunnable**, which is a third state, and recording it as such rather than as a failure is what made
it obvious later exactly what had to land to close them. Four triggerable sources have since arrived:
2.3.3's degenerate/non-finite-transform gizmo WARN, 2.4.1's Debug-build `⌘Z` record, 2.4.2's
Debug-build `⌘Z` with a varied label, and **2.5.1's load-outcome INFO/WARN — the first that is not
Debug-only**, and therefore the first that makes these rows runnable in a Release build on any
platform.

**Not re-run, and unchanged:** rows 3, 7 and 8 keep the partial sub-clauses from the first sitting
(Trace/Debug/Error/Critical colouring by eye; the "while records arrive" half of auto-scroll; the
identical-text tooltip / ImGui id-merging half). All three are now *reachable* with the sources above
and each already has mechanical coverage, so they are closable work rather than structural gaps.

**The process finding, which outlives the rows themselves.** The clause "re-run 2.2.5's four rows" was
written into **three consecutive tasks** — 2.4.2's row 22, 2.5.1's row 22, and considered for 2.5.2 —
and performed by **none** of them. 2.5.2's spec (D3) refused to carry it on the grounds that attaching
it to a task with *zero* other human rows would be the worst version yet, and named a standalone
successor instead. That refusal was correct: scheduled as work of its own, the item took one short
session. **A ride-along validation row on a task that already has twenty of its own does not get
done** — not through negligence, but because the tester is deep in that task's own checklist and an
unrelated row at the end is the first thing dropped when the session runs long. Schedule cross-task
validation debt separately, and make the blocker explicit so it is obvious when it clears.

---

## Phase 2 audit — whole-phase review (2026-08-02)

**Scope:** every epic of Phase 2 (2.1–2.6) read end to end by seven parallel reviewers, one per
subsystem, against the task specs, the local plans, and this log's own entries. Everything below was
independently verified in the source before being acted on; a reviewer's claim was never taken on
trust. Mechanical baseline at the start: 95/95 ctest with `AERO_REQUIRE_GPU=1` on macos-debug and
macos-release, 6/6 tools-OFF, 19/19 reflect-OFF, CI green on `main` at `90d812f`, and CLAUDE.md's
test-inventory numbers (363/384/23/22/48) accurate to the case.

### The headline: the phase was in genuinely good shape, and had two silent data-loss paths

Both were in the *seams between* well-built subsystems, not in any subsystem itself — which is
exactly what a per-task review cannot see and a whole-phase read can.

**1. `CommandStack::isClean()` reported CLEAN over a document that shared nothing with the file.**
`push()` step 3 erases the redo branch, and that erase can destroy the entry `cleanPosition` denotes.
`trimToCapacity` already reasons about this hazard for the *eviction* path (AC-9/E17) and gets it
right; truncation is the same hazard reached from the other end, and nothing handled it. Save, undo
past the save point, make two new edits: `applied` returns to its saved value while counting entirely
different commands. Because `isClean()` is the sole definition of dirty (D3, deliberately no second
copy), that one wrong bool switched off the title's marker, the Save item **and** `guardFor`'s
unsaved-changes prompt simultaneously — File ▸ New/Open, `⌘Q` and the window [X] then discarded the
work with no prompt at all. Fixed by invalidating the mark when the erase destroys it. **C21 is the
load-bearing test, not C20:** it pins that truncation *forward* of a still-reachable clean position
must KEEP it, so the fix cannot degrade into marking everything dirty forever. A fix for a
false-clean is trivially "achievable" by never reporting clean; only the second test forbids that.

**2. The undo/redo chords fired behind modals.** `fileEnabled` gated every File chord and neither
history chord. ImGui grants `RouteGlobal` with no modal test at all (`CalcRoutingScore`) — the exact
fact 2.5.1's BLOCKING-2 turned on for the File chords. So a reflex `⌘Z` behind the unsaved-changes
modal mutated the World that modal was still asking about; answering "Save" wrote a document that was
never on screen. **The fix that matters is not the two `&&`s** — it is that the condition had been
written out by hand in two places, and `shell_ui.cpp`'s own banner already named what a disagreement
between them costs. Extracted as `modalInputActive` (SS36), so the shell's chords, the Edit items and
`applyFileRequests`' refusal all read one definition.

### The pattern worth keeping: reviews catch defects in code, not claims about code

Three prior review rounds each found a real defect. **None found a sentence that had stopped being
true**, and the audit found four:

- **Epic 2.4's goal** claimed "the only direct scene writes left under `editor/src/` are inside the
  three `_ops` TUs and the three command TUs". False when written: `scene_snapshot.cpp` is a
  *seventh* mutating TU and shipped inside 2.4.2 itself. (Three further sites write the World as part
  of a whole-document swap that clears the stack in the same operation — INV-6, correct.) No code was
  wrong. Restated as the form that is true *and* greppable: no panel writes the scene directly.
- **`TransformCommand::mergeWith`'s absent guard** carried its own trigger condition in a comment:
  "the first task that can write a Transform from outside the drag loop must add it (E9/H7)." That
  task was the very *next* one — 2.4.2's `SetFieldCommand` writes `Transform` from the Inspector —
  and the handoff went unread for four tasks. Merging a stale pair silently records an undo step
  restoring a value the entity never held. Guard added, T10 pins both directions.
- **CLAUDE.md and docs/04 said "five architecture guards"** when six exist, and CLAUDE.md's
  enumeration named two that do **not exist as scripts** (audio-boundary, runtime-purity) while
  omitting three that do.
- **README's Status section** was two whole phases behind, telling every visitor to the public repo
  that Phase 0 was still in progress.

**A prose invariant with no guard behind it drifts silently, and nothing goes red.** Where the
literal enumeration is worth asserting, it wants a script plus a hermetic ctest case — the
`check-project-no-delete.sh` shape.

### Two false-greens in the mechanical gate itself

**`cancel-in-progress: true` applied to pushes on `main`,** where every push shares one ref, so
back-to-back merges cancelled each other. Measured, not theorised: `96f06a4` (PR #63's merge) and
`a8b296ab` (PR #60's merge) are both on `main` with **no completed CI run**. "main is always green"
had quietly become "green at the tip", which is not what a bisect needs — and this repo merges small
PRs back to back, so it was the normal case, not a corner. Scoped the cancel to `pull_request`.

**Four guards asserted their scan set was non-empty, never that it covered anything.** Measured
directly: narrowing `check-scene-boundary.sh`'s `HEADER_GLOB` from `engine/*/include/*` to
`engine/scene/include/*` — a plausible edit, the guard is *named* scene-boundary — took the scan from
51 files to 8, and the guard still exited 0 printing its usual OK banner, with a real `entt::` leak in
`engine/core/include/` now invisible. This is precisely the class 2.1.2's review found in
`check-golden-rule.sh`'s `SCAN_ROOTS` ("OK — 80 tracked sources scanned", a lie); the lesson was
applied forward to `check-project-no-delete.sh` but **never applied backward**, so the four guards
written before the lesson are the four that still had it.

Self-test 1b (scene + platform) asserts coverage with **both sides derived from the tree**, which is
what keeps it clear of `boundary-guards.md`'s standing ban on a hardcoded per-root roster: a
subsystem shipping no public header never enters the expected set, so unlike a roster it cannot fail
on a correct tree. The left side deliberately does **not** reuse `HEADER_GLOB` — deriving the
expectation from the value under test is circular and agrees with any narrowing. Proven both
directions: clean tree exits 0 at 51 files, narrowed glob exits 2 naming the seven uncovered
subsystems. `check-math-boundary.sh` and `check-rhi-boundary.sh` have the same shape of hole and are
**not** fixed here (their scan sets are an extension list and a root list, not a subsystem glob) —
open, and recorded below.

### The project root was never made absolute

`ProjectSession::root()` and `RecentProjects::paths` are both documented "absolute, normalized", and
`set()`/`promoteRecent()` both *name their parameter* `absoluteRootUtf8` — four statements of one
contract, enforced nowhere: `absolute()`/`weakly_canonical()` appeared nowhere under `editor/`.
`main.cpp` passes `argv[1]` verbatim, so `aero_editor MyGame` wrote the literal string `MyGame` into
the machine-wide recents file; launched later from a different directory (or from Finder, whose CWD
is `/`) that entry resolves to a **different project** — the outcome `editor_app.cpp`'s own comment
calls "worse than showing Welcome" — or to nothing, while `promoteRecent`'s byte-dedup keeps it as a
second row for one project. Phase 3's AssetDatabase scans `paths.assets` off this same root.

Resolved in `loadProjectFrom` and `createProject`, where a value *becomes* a root, and deliberately
**not** inside `projectRootFromPath`: on Windows `/a/b` is rooted but **not absolute**, so doing it
there would rewrite that function's normalization contract on one platform only and break the P35
expectations on the MSVC lane alone. That is PR #60's standing lesson applied *before* the Windows
lane found it rather than after. `absolute` rather than `weakly_canonical`, so a project reached
through a symlink is not silently recorded under its target.

### Open, deliberately not fixed here

Ranked. Everything below was confirmed in the source; none is speculative.

1. **`placeUnplacedPanels`' fallback docks to the dockspace ROOT, which on a restored layout is a
   split node.** `DockBuilderDockWindow` on a not-yet-created window takes the settings branch and
   writes `DockId` verbatim; `DockContextBindNodeToWindow` then sees a split node, undocks, and zeroes
   `DockId` — so the fallback branch, the one the comment calls the safety net, is the one that cannot
   work, and the ini degrades each launch. Reachable today when a slot's panels are *all* floating.
   I26 never exercises it (its fixture always leaves Inspector docked). **Not fixed deliberately:**
   this is the third defect in this exact code path, and the first two attempts (PRs #62, #63) each
   shipped a wrong predicate — #62 "fixed nothing on the reporting machine". A fourth blind change to
   ImGui docking, verifiable only by a human looking at a window, is how that sequence repeats. Fix
   with the central-node dig plus a second I26 fixture (new panel, no placed slot-mate, split root),
   and confirm on the machine.
2. **`Selection::prune()` and `RootOrder::reconcile()` are called only from `HierarchyPanel::onDraw`**
   — flagged independently by two reviewers. The shell skips `onDraw` whenever the panel is hidden,
   collapsed, or tabbed behind another, so with the Hierarchy tabbed behind Assets nothing prunes.
   Contained today only because every consumer re-guards with `alive()`. Belongs in `EditorApp::tick()`,
   the Asset-Browser reconcile precedent.
3. **`aero_editor_shell_test` is 390 cases behind one ctest entry.** One ASan abort kills every later
   case and reports as a single failure — the S9/S10 cascade in 2.6.2's entry is the consequence, not
   the cause. `tests/CMakeLists.txt` already sanctions `doctest_discover_tests`.
4. **Both tools-OFF configurations are load-bearing and verified only by hand, per task.** The
   cheapest closure is one extra Linux Debug lane: it needs no LLVM and skips the shadercross
   bootstrap, so it is the *fastest* job in the matrix. Prefer it to a weekly schedule, which reports
   that a configuration broke sometime in the last seven merges.
5. **Neither the Hierarchy nor the Asset Browser's tree pane uses `ImGuiListClipper`**, while
   `console_panel` and the Asset Browser's *contents* pane both do — an internal inconsistency, and
   the Asset Browser additionally allocates a `std::string` per row per frame and never evicts its
   listing cache. Phase 3.1.3 inherits both verbatim.
6. **`writeTextFileAtomic` is atomic against process death, not machine death** — no `fsync` on the
   temp file or the parent directory before/after the rename, while `text_file.hpp` advertises ATOMIC
   unqualified. A power loss can land the rename without the data, replacing a good scene with a
   truncated one after the user was told the save succeeded.
7. **`ConsolePanel::applyPending` — the single mutating step the four-phase split exists for — has no
   automated coverage**, nor does `LogHistory::appendAll`, whose `batch.clear()` postcondition keeps
   `LogSink::take`'s debug assert true.
8. **`check-project-no-delete.sh` has a transitive hole:** `createProject` reaches
   `std::filesystem::remove`/`rename` one call away, inside `writeTextFileAtomic`, in a file the guard
   never scans. Correct today (it touches only the temp path); the *promise* is narrower than it reads.
9. **S11 is closable.** `createProject`'s `WriteFailed` branch is reachable under a POSIX
   `RLIMIT_FSIZE` of 0: the directory creates succeed (no file data) and the 152-byte manifest write
   fails, landing the branch with `assets/` and `scenes/` already on disk — which is exactly D7/INV-P4's
   "nothing is removed" property, on the one branch that has never been proven.
10. The recents cap is applied only on promote, so a hand-edited file of N entries is read and
    rendered in full every frame. The pure parser's leniency is deliberate (E13) — cap in the loader.

### Deliverables

**The gate artifact `samples/phase-2-editor-scene/` does not exist**, and by the deliverable rule a
phase without one is not finished. It cannot be fabricated here: the requirement is a scene *built in
the editor by a human*, which is the whole point. Together with the Windows/Linux human passes — still
zero for every Phase 2 task — that is the honest remaining Phase 2 debt. No git tags exist either,
though docs/04 specifies `phase-0-complete` and Phases 0 and 1 are done.

**Verification for this audit's own changes:** six guards green, 95/95 macos-debug and 95/95
macos-release with `AERO_REQUIRE_GPU=1`, 6/6 tools-OFF, 19/19 reflect-OFF, clang-format and
clang-tidy clean on every changed file (three real format violations were caught locally and fixed
before commit — a local format pass never predicts CI, and this time it did not predict itself).

---

## Phase 3 — Asset Pipeline & 3D Content

### Epic 3.1 — AssetDatabase · assets

#### Task 3.1.1 — GUIDs + `.meta` files — OPENS Phase 3, Epic 3.1

**3.1.1 gives every file in a project's assets tree a permanent 128-bit identity, recorded in a
versioned JSON sidecar committed to git beside it, that survives renames, content edits, machine
changes and fresh clones — without ever destroying, overwriting or silently rewriting anything the
user owns.** Three layers: `engine::Guid` (a 16-byte trivially-copyable POD beside `Handle`, `core`),
`planAssetMetas` (a pure lifecycle planner — every create/never-rewrite/never-overwrite/never-delete/
repair-duplicates rule is provable from a `std::vector` literal and a fixed seed, no disk touched),
and `AssetDatabase::rescan` (a five-phase, `<filesystem>`-free, `<fstream>`-free, non-recursive walk
composed entirely from 2.2.4's `listDirectory` and 2.5.1/2.6.1's `text_file` primitives). This task
**ends the four-task empty-`engine/`-diff streak** (2.5.1, 2.5.2, 2.6.1, 2.6.2) deliberately and
minimally: one new header (`guid.hpp`), one new source (`guid.cpp`), one CMake line, zero new
dependencies, zero new link entries. `engine/assets/` is deliberately left untouched (`.gitkeep`
only) — it opens when a **runtime** consumer exists, Phase 5's pak table; the editor's AssetDatabase
lives entirely in `/editor`.

**Six commits landed the feature** (`323634e` Guid · `ee1136c` `.meta` format + planner · `0ba591e`
AssetDatabase scan · `482939d` widened no-delete guard · `4304845` Asset Browser integration ·
`64bd976` EditorApp integration), followed by this documentation commit. Branch
`feat/3.1.1-guids-and-meta-files`, cut from `main @ 83af319`.

**What shipped, in order:**
1. `engine::Guid` — `hi`/`lo` `uint64_t` halves, nil = reserved none, 32-lowercase-hex canonical text
   (`hi` first, so lexicographic order == numeric order), a seedable `splitmix64`-based
   `GuidGenerator`. Not an RFC 4122 UUID — no version/variant bits, all 128 bits are random. Every
   test in the tree pins exact values from a fixed seed; **no test anywhere touches an entropy
   source.**
2. The `.meta` v1 format (`asset_meta.hpp`/`.cpp`) — a two-key (`version`, `guid`), one-level JSON
   document sharing scene v1's canonical form and two-layer strictness policy (unknown keys tolerated
   and preserved; a *known* key with the wrong kind or value is a hard reject), plus `planAssetMetas`,
   the pure planner. Two committed golden fixtures (`minimal.meta` 65 bytes, `unknown-keys.meta`) with
   a byte-fixpoint battery, shipped in the SAME commit as the format, not after it.
3. `AssetDatabase` (`asset_database.hpp`/`.cpp`) — the scan. Guard → explicit-stack walk → pair each
   directory's assets against its own sidecars → plan → write-and-index. It never logs (INV-A3) and
   never throws; a report is returned and `EditorApp` turns it into log records.
4. The sixth architecture guard (`check-project-no-delete.sh`) widened from three files to five —
   `asset_meta.cpp` and `asset_database.cpp` join the allowlist, because "an invalid `.meta` is never
   overwritten" (D7) and "an orphan is never deleted" (D8) are the same class of safety-critical,
   unreachable-by-ordinary-test rule 2.6.1's seed S11 already documented for `createProject`'s
   rollback branch. The hermetic ctest case gained two new stages (7, 8) proving the widening is
   red-on-violation, and the canary stage (6) was retargeted to one of the two new files, so the
   widening itself — not just the original three-file scope — is what the vacuity refusal proves.
5. The Asset Browser (`asset_browser_panel.{hpp,cpp}`) — `.meta` rows filtered at cache-fill time
   (never at draw time, so the footer count, the tree builder and the selection lookup all agree with
   one filtered view), a footer segment showing the selected file's elided GUID / `no .meta` /
   `invalid .meta`, and plumbing (`setDatabase`, `database()`, `takeRescanRequest()`) for the
   integration in Step 6.
6. `EditorApp` integration — one `AssetDatabase` value member, one `GuidGenerator` seeded from
   `fromEntropy()` at `create()`, and `tick()`'s existing 2.6.1 reconcile block extended (not
   duplicated) to scan the database before pushing the panel's root, so the first frame after a
   project opens already shows real GUIDs instead of "no .meta" self-correcting a frame later. Three
   new GPU-tier cases (I27/I28/I29).

**What was deliberately left out**, named so nobody re-derives it as an oversight: folder GUIDs (D11
— directories get no `.meta` in v1); orphan re-attachment (D8 stands, re-attachment needs a content
hash and is 3.1.2's); async scanning (the scan is synchronous and blocking on the frame a project
opens, bounded by `MAX_ASSETS`/`MAX_TREE_DEPTH` and surfaced by a WARN — 3.1.2's cache and 3.1.4's
watcher are the real answers); a `Copy GUID` context-menu item and any issues-list UI (3.1.3's); and
teaching `reflect-gen` to serialize a `Guid` field (3.4.x's — adding it now would be a reflection
change with no consumer, and ADR-004's spine is untouched by this task).

**One KNOWN DEFECT is carried forward deliberately, and it is 3.1.2's to close** (full statement in
the code-review-round entry below): a symlinked directory inside the assets tree makes one physical
file reachable via two relative paths, so it gets two records, a duplicate GUID, and a repair that
rewrites the *shared* sidecar — which means **D6's zero-write property never holds for that tree and
the asset's identity changes on every scan**. It is noisy rather than silent (one repair WARN per
scan), and it needs `assets/link -> assets/real` to trigger, but it is a real identity-churn path.
Both correct fixes were outside this task's scope: physical-identity dedup needs
`<filesystem>` canonical/equivalent, which AC-30/INV-A6 forbid in `asset_database.cpp`, or 2.2.4's
documented D13 ("a symlinked asset folder behaves like a real one") has to change. **3.1.2 is the
right owner because it introduces the content hash** — with it, two paths hashing identically are
provably the same content, which is the same primitive D8's orphan re-attachment needs. Do not close
it by weakening the duplicate-repair rule; the repair is correct, the double-discovery is the bug.

**Four findings from the build itself, recorded because they are the point, not colour:**

1. **The `MAX_ASSETS` coverage gap, accepted deliberately.** AD23 drives `listDirectory`'s cheaper
   `MAX_ENTRIES_PER_DIRECTORY` cap (~10,001 files), not the `MAX_ASSETS = 50000` branch — creating
   50,001 real files inside a Debug/ASan ctest case was measured and judged unaffordable, and
   `MAX_ASSETS` was NOT lowered to make it cheap (a constant a test changes is a constant no test
   pins, and a test-only injection seam was rejected for the same reason). `AssetScanReport::truncated`
   stays plumbed and reachable through the `listDirectory`-level cap; the `MAX_ASSETS` branch itself
   is a real, open, and now-documented coverage gap, the 2.6.1-S11 posture applied a second time.
2. **The `AssetBrowserPanel` member/accessor name collision.** The plan's literal spelling used
   `database` as both the private member and the public accessor, which does not compile (a data
   member and a member function cannot share a name). Resolved with the tree's own precedent
   (`RenderTarget::depthFormatValue` ↔ `depthFormat()`, cited in `.claude/rules/ci-portability.md`'s
   "distinct name on accessor collision" rule): the member is `databasePtr`, the accessor stays
   `database()`. This is the 2.3.1 `EditorCamera` trap recurring a second time — a plan's literal
   identifier spelling is not proof it compiles.
3. **The `setDatabase` gating ambiguity.** The plan's data-flow shorthand read `panel->setDatabase(&db)
   on mismatch` without saying which mismatch. Read literally as gated on the SAME root-mismatch
   comparison that drives `setRoot`, it would never fire on the commonest path — a project open at
   launch, where the panel is born already holding the correct root (from `EditorAppConfig`/argv),
   so `assetBrowserPanel->root() != assetDatabase.root()` is false from tick 1 onward and the pointer
   would stay null forever, leaving the GUID footer permanently empty for every user who never swaps
   projects mid-session. Resolved by calling `setDatabase(&assetDatabase)` **unconditionally, every
   tick**, decoupled from the root-mismatch gate: a raw pointer write has no side effects (unlike
   `setRoot()`, which clears the panel's whole UI state), so there is no cost to doing it every frame.
   **Corrected justification (code-review finding 6):** this entry originally claimed the unconditional
   call "is what makes I27 … actually see a populated database through the panel's own accessor" —
   false as written: I27 never reads `AssetBrowserPanel::database()`, which has zero call sites in the
   whole tree (`.claude/rules/editor.md` correctly records that sabotage seed S25, dropping this exact
   reconcile, reddens nothing in the automated suite). The real reason unconditional matters is that
   `EditorApp` is movable and `create()` returns `std::optional<EditorApp>` (D13) — a pointer set once
   at construction would go stale across a move, so it is re-pushed at the top of every `tick()`,
   before any panel draws, purely to stay valid across a move that may or may not have happened. The
   panel's database pointer therefore has **no automated coverage at all**; AC-37's footer behaviour is
   proven only by human validation rows 3 and 8, exactly as `.claude/rules/editor.md` already states.
4. **The AD21 test-design bug found in Step 3, and the reason it matters beyond one test.** The first
   draft of AD21 ("a left-behind `*.aero-tmp` is skipped, not deleted") planted a leftover
   `wood.png.meta.aero-tmp` beside `wood.png`, but gave `wood.png` NO real sidecar — so `wood.png` was
   `Created`, and `writeTextFileAtomic`'s own transient working file for that write
   (`wood.png.meta.aero-tmp`, the atomic-rename mechanism every `.meta` write goes through) landed at
   the EXACT path AD21's "leftover" occupied, and the legitimate create-write silently consumed and
   replaced it before the test's own assertions ran. The behaviour under test was correct throughout;
   the test was not proving what its own title claimed. Fixed by giving `wood.png` a real, valid
   sidecar first, so no legitimate write ever touches `wood.png.meta.aero-tmp`'s path. **This exact
   collision surfaced again, independently, during sabotage seed S13** (a planner bug that flags a
   valid record for write): breaking D6 made `wood.png`'s ALREADY-VALID sidecar get rewritten too,
   recreating the identical `wood.png.meta.aero-tmp` collision and consuming AD21's leftover file a
   second, unrelated way — proof that the fixed test's isolation from `writeTextFileAtomic`'s own
   working-file path is load-bearing, not cosmetic.

**§A of the plan corrected the spec in sixteen places** (A1–A16) — none of them re-litigated here
individually since the plan is the executable record, but two are worth restating because they are
exactly the "measure, don't trust arithmetic" lesson this project keeps re-learning: the canonical
`.meta` is **65 bytes**, not the spec's stated 59 (`{`+LF=2, `  "version": 1,`+LF=16,
`  "guid": "<32 hex>"`+LF=45, `}`+LF=2); and `next()`'s nil-retry loop is **provably dead code today**
(A12) — `hi` and `lo` are drawn from two distinct, consecutive generator states and `splitmix64`'s
finaliser is a bijection on `uint64_t`, so a bijection cannot map two distinct inputs to zero, meaning
both halves can never be zero at once. The loop is kept as defence in depth for a future generator
whose halves could share one state or whose mixer is not a bijection, and its comment says so
verbatim — sabotage seed S6 (below) proves the point directly rather than asserting it.

**The `docs/09-file-formats.md` §5 reservation ("future .meta GUID system") is now the past tense** —
replaced with a normative §5 (the format, the fixtures, the round-trip guarantees), with the old §6
`.pak` reservation renumbered up by one and both forward-references it used to sit behind corrected.

**Test inventory, measured fresh, not arithmetic'd:** `ctest -N` **95** tools-ON / **6** tools-OFF /
**19** reflect-OFF — unchanged, because this task registers **zero** new ctest entries (all of its
~120 new cases live inside the three existing TUs `aero_tests`, `aero_editor_shell_test` and
`aero_editor_imgui_test`). `aero_tests` **363 → 389** (`GU1`–`GU26`, the seven-include `guid.hpp`
codec/ordering/hash/generator battery). `aero_editor_shell_test` **390 → 467** (`AM1`–`AM27` naming/
classification/parse/write; `AP1`–`AP14` the pure planner; `AG1`–`AG6` the golden battery; `AD1`–`AD30`
the real-disk scan battery — **corrected from this entry's original "`AM1`–`AM40`"/"`AP1`–`AP18`",
which were the plan's own predictions, not a measurement; `tests/CMakeLists.txt`'s own comment already
had the true ranges right at the time this paragraph shipped, so the tree contradicted itself until
code-review finding 6 caught it: re-measure with `--list-test-cases`, do not trust a plan's
arithmetic, the 2.5.2 lesson yet again**) — **identical +77** in BOTH fresh tools-OFF configurations
(`build/tools-off-3.1.1` 443, `build/reflect-off-3.1.1` 443, both containing all 77 `AM`/`AP`/`AG`/`AD`
cases, confirmed by `--list-test-cases` rather than assumed), which is AC-17's whole claim: the format
and the planner need no serialization and are therefore present, not skipped, in every reduced
configuration. `aero_editor_imgui_test` **48 → 51** (`I27`, `I28`, `I29`). `aero_scene_serialize_test`
and `aero_editor_inspector_test`: unchanged. `check-math-boundary.sh`: **246 → 249** (Step 1) **→ 252**
(Step 2) **→ 255** (Step 3; unchanged through Steps 4–8) — nine new C-family files scanned, measured
after `git add` at every step boundary. Guard count stays **six**, `check-project-no-delete.sh`'s own
final line now reads "5 files scanned" instead of 3.

**The sabotage matrix — all 26 seeds (S1–S26) plus all 3 mandatory second-order checks, run and
confirmed against the real built binaries, not reasoned about.** Every seed was applied, confirmed
PRESENT with `git diff` before trusting any verdict (the standing BSD-`sed`-false-PASS lesson), rebuilt
(never a stale binary — confirmed via `ninja`'s own incremental-build step list), run through the full
95-test `AERO_REQUIRE_GPU=1` suite, and reverted with the revert confirmed byte-for-byte clean against
`HEAD` (`git show HEAD:<file> | diff -q - <file>`) before the next seed began. **Seven seeds matched
their prediction exactly** (S7, S8, S9, S10, S18, S19, S20). **Six were confirmed non-discriminators**
(S4 — `parseGuid`'s length-31 acceptance, masked by `std::string`'s guaranteed null terminator and
ASan's non-tracking of a string literal's storage; S6 — A12's dead retry loop; S23 — the
reference-vs-pointer coverage gap; S24 — `byGuid` indexing an Invalid record, masked by `findByGuid`'s
own independent nil-guid guard; S25/S26 — "no test reads the footer" / "no test builds an unreadable
sub-directory"), and **S17's own predicted contingency** ("if nothing else reddens, that IS the
finding") also came true exactly as written. **Corrected count (code-review finding 6): this entry's
own original text read "Four … the remaining fourteen", which accounted for only 25 of the 26 seeds,
silently dropping S17, and separately filed S4 and S24 under "differently-shaped" even though their
OWN bullets below say they redden NOTHING — the defining property of a non-discriminator, not of a
differently-shaped result. 7 + 6 + 1 + 12 = 26.** The remaining **twelve**
seeds (S1–S3, S5, S11–S16, S21, S22) reddened a real but **differently-shaped** set than predicted —
every one of those differences is a genuine finding about which test actually discriminates which bug,
not a failure of the matrix. (S4's and S24's own bullets are kept below for their diagnostic detail —
they explain two of the six confirmed non-discriminators above, not two of the twelve
differently-shaped seeds.)

- **S1/S2/S3 (formatGuid corruptions)** each reddened a DIFFERENT subset of the `AG1`/`AG2`/`AG3`
  golden trio than predicted, because the committed fixture's pinned GUID (`a3f1c07e…`) has specific
  bit patterns (a nonzero leading nibble, distinct `hi`/`lo` halves) that make some corruptions
  invisible to it and others fully visible — S2 (leading-zero-drop) in particular reddens NOTHING at
  the golden-fixture layer, because neither committed fixture's GUID has a leading zero nibble.
- **S4 (`parseGuid` accepts length 31)** reddens NOTHING at all, confirmed and traced precisely:
  `std::string`'s guaranteed null terminator at `text[size()]` and ASan's non-tracking of a global
  string-literal's storage both independently mask the off-by-one for the two length probes GU13
  actually uses.
- **S5/S9/S10 (parser-order and validation bugs)** each cascaded one layer up, from `asset_meta.cpp`'s
  own `AM`-family cases into the `AssetDatabase`-level `AD`-family cases that plant a real sidecar on
  real disk and read it back through the whole scan — a useful confirmation that the two layers'
  test suites overlap in coverage rather than leaving a gap between them.
- **S11/S12 (writeMetaText corruptions)** are non-discriminators for exactly TWO of the five predicted
  cases (`AG2`, `AD7`) for the identical, structural reason 2.5.2's S12 and 2.6.1's S9 already
  documented: both are SELF-consistency checks (a second cycle against the first; a fresh call against
  itself) that stay internally consistent under a corruption applied uniformly to both sides of the
  comparison. `AM25`/`AM26`/`AG6` — not named in the prediction — are what actually catch it.
- **S13 (breaking D6, the most important seed in the matrix)** and **S14 (writing an Invalid record)**
  each reddened SEVEN cases, not the two headline ones each prediction named — the wider cascade is
  every other test in the same family that also plants a valid/invalid sidecar and checks it survives
  untouched. S13's cascade includes a genuinely interesting, independently-confirmed replay of Step 3's
  own AD21 test-design finding (see item 4 above): the seed's erroneous rewrite of `wood.png`'s ALREADY
  valid sidecar recreates the exact `wood.png.meta.aero-tmp` collision that finding describes.
- **S15 (case-folded sort)** reddens only ONE of its two same-labelled predicted cases: `AP10`
  (asserting `'Z.png'` precedes `'a.png'`, byte-order-specific) reddens; `AP9` ("shuffled input gives
  an identical result"), despite literally carrying `seed S15` in its own title, does not — because
  `AP9` only proves sort-order-independence-of-input, a property any consistent comparator satisfies
  regardless of WHICH ordering rule is in force. `AP9` cannot discriminate the rule; `AP10` can.
- **S16 (repair rewrites the keeper)** reddened its whole family plus two more (`AD17`, `AD18`) not
  individually named.
- **S17 (case-sensitive `isMetaFileName`)** confirms the plan's own flagged contingency verbatim:
  only the `AM`-layer cases redden; nothing at the `AssetDatabase` layer does, because this machine's
  APFS volume is case-insensitive, so no `AD` case can even construct the `x.meta`/`x.META` pair the
  regression would need to be visible through a real scan — a genuine, machine-dependent coverage gap
  the plan predicted before it was run.
- **S21 (an orphan gets deleted)** confirms BOTH of its predicted facts exactly — the widened guard
  fails BEFORE any test runs, and `project-no-delete.no_delete_e2e` stays green because it tests the
  script against its own scratch tree — and, going one step further than the plan's own prediction
  (since this invocation's instructions required actually building and running the suite anyway, not
  stopping at the guard failure), a real functional test (`AD15`) independently reddens too once the
  code actually runs, confirming defense-in-depth: the static guard and a live behavioural test each
  independently catch the same class of violation.
- **S22 (deleting the whole reconcile block) is the one MAJOR, confirmed deviation from the plan's own
  prediction, not a coverage nuance.** Predicted "I28 and ONLY I28"; actual is FOUR cases — `I21`
  (2.6.1's own case), `I27`, `I28`, `I29`. Root cause, traced to source: the block D12 extends is the
  SAME block that already existed for 2.6.1's panel-root reconcile (D10) — 3.1.1 rewrote the existing
  block into the shape its own "Data flow" diagram shows, it did not add a second block beside the
  first. Deleting "the reconcile block" wholesale therefore deletes BOTH reconciles at once, and `I21`
  (which asserts the panel's root followed a runtime project swap) fails for exactly the reason 2.6.1
  wrote it to prove. `I27` ALSO reddens, for a reason the plan's own explanation gets half right: the
  PANEL's root is seeded correctly at construction (`AssetBrowserPanel(app.project.assetsRoot())` in
  `create()`), independent of the reconcile — but the DATABASE is not: `create()` deliberately does
  not scan (D12's own stated design), so `assetCount()` stays 0 until the FIRST `tick()` reconcile
  runs `assetDatabase.rescan(...)`, and `I27` ticks exactly once to exercise that first scan.
- **S24 (`byGuid` indexes Invalid records too)** reddens NOTHING, confirmed and traced: `findByGuid`
  carries its OWN independent guard (`if (!guid.valid()) return nullptr;`) that runs before the
  corrupted map is even consulted, and an `Invalid` record's `guid` is always nil by construction — so
  `AD27`'s own probe for this never gets far enough to observe the corrupted `byGuid`. `INV-A7` still
  holds in spirit for this record shape through a second, independent mechanism even with the first
  one broken; a discriminator would need an Invalid record with a non-nil guid, which the current
  `AssetMetaState` contract makes impossible to construct.

**The three mandatory second-order checks, run for every one of the 26 seeds, no exceptions:** (1) the
seed was actually present — `git diff` shown and grepped before trusting any verdict; (2) the suite
was actually rebuilt, not stale — confirmed via `ninja`'s own step list on every build; (3) the revert
restored the file byte-for-byte — confirmed via `git diff` (empty) AND `git show HEAD:<file> | diff -q
- <file>` for every touched file after every seed. All three passed for all 26 seeds.

**Traps found, beyond the four numbered above:** `formatGuid`'s zero-padding pre-fills the output
buffer with `'0'` before any nibble is written, which is why a "drops a leading zero" bug (S2) is
invisible for the nil GUID specifically — the buggy code path never executes for an all-zero input,
and the correct-looking output is coincidental, not a passing property. `writeTextFileAtomic`'s own
`<path>.aero-tmp` working file occupies the SAME namespace `isScannableAssetName`'s `.aero-tmp` suffix
check is built to exclude — any test planting a "leftover `.aero-tmp`" fixture beside an asset that
will ALSO be legitimately written must give that asset a valid sidecar first, or the legitimate write's
own working file silently consumes the planted fixture before the test's assertions run (found twice,
independently, in this task alone — see finding 4 and S13 above).

**Verification for this task:** six guards green; 95/95 `macos-debug` and 95/95 `macos-release` with
`AERO_REQUIRE_GPU=1`; 6/6 tools-OFF and 19/19 reflect-OFF, both measured fresh (`build/tools-off-3.1.1`,
`build/reflect-off-3.1.1`), never trusted from a stale directory (2.6.2's §A3 lesson, applied a third
time); `check-math-boundary.sh` **255**; every changed file confirmed byte-identical to `HEAD` after
every one of the 26 sabotage reverts.

##### Task 3.1.1 — code-review round (7 findings, 2 blocking, all fixed on the same branch)

Six commits on PR #65's own branch. CI on that PR went red on the Windows/MSVC lane. Two findings below
(1 and 3) are what a macOS/Linux-only local gate structurally cannot see, and both are BLOCKING — the
tree was never green on all three lanes until they were fixed.

1. **BLOCKING — `tests/guid_test.cpp` used `std::array<char, 33>` (GU17) without including `<array>`.**
   Reached transitively through libc++ only, exactly the `imgui_layer_test.cpp:47`/`project_files.cpp:8`
   class of failure this tree has already paid for twice. One-line fix, in the file's own sorted include
   order, with the same comment style as the precedent.
2. **BLOCKING — a `Created`/`Repaired` write could destroy a valid orphaned sidecar on any
   case-insensitive filesystem (APFS, NTFS — most users).** The scenario: a case-only rename
   (`wood.png` → `Wood.png`) leaves `wood.png.meta` behind as a real, valid, ORPHANED sidecar (pairing
   is exact bytes, AC-19, so it never claims the renamed asset). The scan then mints a fresh GUID for
   `Wood.png` and writes `Wood.png.meta` — which on a case-insensitive filesystem **is**
   `wood.png.meta`, and `writeTextFileAtomic`'s atomic rename silently replaces it. A committed GUID is
   destroyed, every scene reference to that asset dangles, and the same scan's own log says the file
   was preserved — this contradicts D8 head-on, and neither the no-delete guard (a WRITE, not a
   `remove`) nor any existing test (AD29's own anti-vacuity note says a case-insensitive filesystem
   short-circuits it) could see it. **Fixed** by checking every Created/Repaired write, before it
   happens, against the walk's own UNCAPPED orphan list, ASCII-case-insensitively, on every platform
   unconditionally (D7's "one rule, no exceptions" — conservative on a case-sensitive filesystem,
   load-bearing on a case-insensitive one). A conflicting write is refused; the record is downgraded to
   `AssetMetaState::Invalid` with a nil guid (D7's own posture: no identity this session) and reported
   in a new capped `AssetScanReport::writeConflicts` list with its own total, never folded into
   `invalidPaths` (verified by construction: every conflict increments both `report.invalid` and
   `report.writeConflictTotal` exactly once, so `logAssetScan`'s "invalid asset meta file(s)" WARN
   subtracts the latter from the former to keep its own total exact). New case **AD31** — the exact
   scenario, which (unlike AD29's `x.meta`/`x.META`) needs no case-insensitive-volume skip, since
   `Wood.png` and `wood.png.meta` are not case-variants of the same full name and coexist as distinct
   directory entries on any filesystem. Verified against a seeded regression that disabled the guard:
   on this machine's own APFS volume the orphan's bytes were actually destroyed, confirming this is a
   real, exploitable bug here and not merely a theoretical one.
3. **BLOCKING (Windows CI, found after the local round) — MSVC's `std::unordered_map` move
   CONSTRUCTOR is not `noexcept`, reddening `asset_database.hpp`'s aggregate `static_assert` (C2607)
   before a single test could run.** The plan's own §A2 part 2 anticipated this failure class for move
   ASSIGNMENT; the real regression landed one level up, on move CONSTRUCTION, on the same member — a
   detail worth recording precisely because it is not the exact case the plan described. Fixed exactly
   as documented, not improvised: `byPath` disappears entirely (`records` is already sorted by
   `relativePath`, so `findByPath` is a `std::lower_bound` directly over it) and `byGuid` becomes a
   `std::vector<std::pair<Guid, std::size_t>>` sorted by `Guid` (`operator<`, AC-2), rebuilt with a
   STABLE sort after every rescan so the "first claimant wins" property `std::unordered_map::emplace`'s
   silent-no-op-on-duplicate-key gave this method survives the swap exactly. Both accessors stay
   `noexcept` and allocate nothing — the comparator compares `std::string` against `std::string_view`
   without constructing a `std::string`. `PathHash` is deleted with `byPath`; `std::hash<Guid>` is
   untouched (still required by AC-3, still used by `planAssetMetas`' own local duplicate map, which has
   no movability requirement at all).
4. **The `||` short-circuit left the Asset Browser panel's own one-shot Refresh flag undrained.**
   `EditorApp::tick()`'s reconcile read
   `assetRescanRequested || (assetBrowserPanel != nullptr && assetBrowserPanel->takeRescanRequest())` —
   when `assetRescanRequested` was already true, `takeRescanRequest()` was never CALLED, so a Refresh
   click landing the same frame as a `requestAssetRescan()` call would survive un-consumed and trigger a
   second, redundant synchronous scan the following frame. It also left the panel's own half of AC-38
   with zero automated coverage, since I29's left operand is always true. **Fixed** by draining the
   panel into a named local FIRST, unconditionally, then combining. New case **I30**: since this target
   is ImGui-free at source and cannot synthesize a Refresh-button click (the AC-27/FileDialogHost
   precedent), I30 is a mechanical, source-text-reading proof — the PU1 precedent (`project_test.cpp`),
   extended to a second target for the first time (`AERO_EDITOR_SRC_DIR` is now also defined for
   `aero_editor_imgui_test`) — that the drain statement precedes the combine statement in
   `editor_app.cpp`'s own source, and that no line fuses `assetRescanRequested`, `||` and
   `takeRescanRequest()` together (the exact shape of the regression). Verified against a seeded
   revert of the fix: I30 catches it directly, by name and line number.
5. **AD8's D6 proof rested on timestamp granularity alone.** A D6 regression rewrites the SAME GUID, so
   `size` is unchanged — `mtime` is the entire discriminator, and on a coarse-granularity filesystem, or
   two scans landing within one clock tick, the case could pass with the bug present. **Fixed** by
   back-dating both sidecars by an hour (`std::filesystem::last_write_time`, the `error_code` overload)
   immediately before the second scan, so any real rewrite necessarily moves `mtime` forward regardless
   of granularity. Verified against a seeded "rewrite everything" regression: caught, then reverted.
6. **Four claims about the code that had stopped being true, the Phase 2 audit's standing lesson
   recurring a fourth time.** (a) `AM1`–`AM27`/`AP1`–`AP14`, not the plan's own predicted
   `AM1`–`AM40`/`AP1`–`AP18` — corrected everywhere in `CLAUDE.md` and in this file (both the original
   landing paragraph above and this one), while `tests/CMakeLists.txt`'s own comment had the true ranges
   right the whole time. (b) The `setDatabase` unconditional-call justification above (item 3) claimed
   it "makes I27 … actually see a populated database through the panel's own accessor" — false: I27
   never calls `AssetBrowserPanel::database()`, which has zero call sites in the tree; the real reason
   is keeping the pointer valid across an `EditorApp` move (D13), and the panel's database pointer has
   NO automated coverage at all, exactly as `.claude/rules/editor.md` already says for sabotage seed
   S25. Corrected in place, item 3 above. (c) `.claude/rules/editor.md`'s naming-collision paragraph
   attributed the accessor/member collision to `AssetDatabase::findByGuid`/`findByPath` — neither is
   involved; the collision is `AssetBrowserPanel::database()` vs. its own `databasePtr` member.
   Corrected in that file. (d) The sabotage classification's arithmetic accounted for only 25 of the 26
   seeds (silently dropping S17) and filed S4 and S24 under "differently-shaped" even though their own
   bullets say they redden NOTHING — the defining property of a non-discriminator. Corrected above and
   in `CLAUDE.md`: **six** non-discriminators (S4, S6, S23, S24, S25, S26), not four; **twelve**
   differently-shaped seeds (S1–S3, S5, S11–S16, S21, S22), not fourteen; 7 + 6 + 1 + 12 = 26.

**Deliberately out of scope, documented separately rather than fixed here:** a symlinked directory
making one physical file reachable via two paths (duplicate GUID; a repair rewrites the shared sidecar
on every scan) needs either physical-identity dedup via `<filesystem>` canonical/equivalent — which
AC-30/INV-A6 forbid in `asset_database.cpp` — or a change to 2.2.4's documented D13 behaviour. Both are
architectural decisions outside a code-review round's scope.

**Test inventory after this round, re-measured, not carried forward from the landing numbers above:**
`aero_tests` **389** (unchanged — finding 1 is an include fix, no new case). `aero_editor_shell_test`
**467 → 468** (`AD31`, finding 2). `aero_editor_imgui_test` **51 → 52** (`I30`, finding 4). Both
tools-OFF configurations, freshly rebuilt, confirmed **444/444** (`ctest -N` unchanged at 6/19) —
AC-17's claim still holds after the round. `check-math-boundary.sh`: **255** (unchanged — no new
tracked file; the round only edits existing ones). `check-project-no-delete.sh`: **5** files scanned
(unchanged).

#### Task 3.1.2 — Import cache & dependency tracking — closes two inherited deferrals

**3.1.2 gives the editor a memory of what it last observed about every identified asset — its
content, its sidecar, its importer and its dependencies — so a scan can name, per asset, exactly why
it would (or would not) be re-imported, without reading a single byte of an unchanged asset and
without ever writing, overwriting or destroying anything the user owns.** The one sentence the task
exists to make true: open a project twice, and the second open reads not one byte of any asset,
writes not one byte anywhere, and reports `N up to date, 0 new, 0 changed`. It also **closes two
things 3.1.1 deliberately deferred**: the D8 orphan-re-attachment deferral (a moved asset with no
sidecar gets its old GUID back, on five simultaneous conditions, never a heuristic) and the
carried-forward symlinked-directory duplicate-GUID defect (a symlinked folder no longer gives one
physical file two records — closed by canonical-path directory dedup, not by the content hash the
original 3.1.1 entry wrongly predicted would close it; see the correction below).

**Ten commits landed the feature** (`195f0a1` `ContentHash` · `8f8a6b5` `hashFileContents`/
`ensureDirectory` · `045284b` `FileEntry` mtime+symlink, `canonicalDirectory` · `d808960` the index
format v1 + docs/09 §6 · `95bfdf1` the no-delete guard widened to six · `aa99dc9` `planImports`/
`commitImports` · `7e71338` `planReattachments` + `Reattached` · `af420c6` the eight-phase scan +
alias dedup · `a385e98` the editor surface + I31–I33 · `f7198fc` the sabotage-matrix gap fixes),
followed by this documentation commit as the eleventh. Branch
`feat/3.1.2-import-cache-and-dependency-tracking`, cut from `main @ a651e28`.

**What shipped, in order:**
1. `engine::ContentHash` (`engine/core`, beside `Guid`) — MurmurHash3 x64_128 seed 0, explicit
   little-endian loads, pure `uint64` arithmetic (byte-identical on all three platforms, UBSan-clean),
   a 32-lowercase-hex canonical text form (`hi` first, big-endian per word — the same convention
   `Guid` uses), and an incremental hasher that lets a multi-gigabyte file be hashed in bounded chunks.
   A distinct type from `Guid` with no conversion either way, so `findByGuid(someHash)` is a compile
   error rather than a lookup that silently fails forever. The engine diff is again exactly three
   paths (`engine/core/CMakeLists.txt`, `content_hash.hpp`, `content_hash.cpp`) — the same minimal
   shape 3.1.1 used to end the four-task empty-`engine/`-diff streak, now used a second time.
2. `hashFileContents`/`ensureDirectory` (`editor/src/text_file.cpp`) — streaming, 1 MiB chunks, never
   materializing the file (a 2 GB source must never become a 2 GB `std::string`), binary on both
   sides, the `error_code` overload everywhere, never throws, never logs. **The first test TU these
   three disk primitives (`readTextFile`/`writeTextFileAtomic`/`fileExists`) have ever had**
   (`tests/editor/text_file_test.cpp`, new — see A1 below) ships in the same commit.
3. `FileEntry` gains `mtime`/`mtimeKnown`/`isSymlink`, **appended, never inserted** (A2 below), and
   `project_files.cpp` gains `canonicalDirectory` — a dedup key and nothing else; it never reaches a
   record, a report or the cache.
4. The asset import cache index v1 format (`docs/09-file-formats.md` §6, normative) — three root
   keys, ten entry keys, a fixed order, a byte fixpoint over both a single-entry and a multi-entry
   fixture, and a strictness policy that is the deliberate **inverse** of `.meta`'s: a damaged index
   is discarded whole and rebuilt, never repaired; a malformed entry is dropped and the rest survive;
   unknown keys are ignored **silently**, no WARN, because a newer build wrote them and the user
   cannot act on them (D7). `mtime` is a JSON **number**, never a string, because this tree's DOM
   stores number lexemes verbatim and a 19-digit tick count must round-trip exactly past 2^53 — the
   committed minimal fixture pins the digit count on purpose (F1).
5. The sixth architecture guard widened a second time, from **five files to six** —
   `editor/src/asset_cache.cpp` joins the allowlist (D18), with the header comment's nuance recorded
   because it reads like a contradiction of D7: the cache's *data* is disposable, but nothing in this
   task deletes a file to dispose of it. The hermetic ctest case gained a ninth stage and its
   missing-file canary was retargeted at the new entry, so the widening itself is proven
   red-on-violation.
6. `planImports`/`commitImports` (`asset_cache.cpp`) — the pure change-detection half. `planImports`
   names, per asset, exactly why it would be re-imported, in a fixed precedence order, from a
   byte-sorted input so the answer never depends on walk order; the dependency cascade is a monotone
   worklist over reverse edges, seeded with every already-dirty node and pushing only on the
   clean-to-dirty transition, so it terminates in O(V+E) on **any** graph — cycles included — with no
   cycle detection, no visited set and no recursion, because a cycle is a set of nodes that all become
   dirty together, which is the correct answer. `commitImports` never fabricates a hash: an unreadable
   or unbudgeted asset keeps its previous entry or has none, because a false `UpToDate` is the one
   failure this task must never produce (R-C2). An entry whose GUID is momentarily absent survives
   `MISSING_SCAN_GRACE` (3) scans before it is dropped, so a git checkout or a sync mid-flight cannot
   destroy the only record that could later re-attach it.
7. `planReattachments` — five conditions, **all** required, no heuristics: the candidate has no
   sidecar; its content hash was computed this scan (not skipped by budget); exactly one previous
   cache entry has that hash **and** a path this scan no longer sees; that entry's GUID is claimed by
   no live asset; and exactly one orphaned sidecar on disk parses to that GUID. Any failure produces
   nothing — silence, not a near-miss log. Fires only on exact byte equality, which is what makes a
   "wrong" answer harmless: the referenced bytes are identical either way, which is why re-attaching
   by name similarity or file size stays rejected — those are *usually* right, the worst kind. The old
   sidecar is never deleted.
8. The eight-phase scan (`asset_database.cpp`, five phases become eight: load the index, hash what
   the `(size, mtime)` fast path could not vouch for, re-attach orphans before identity is planned).
   The index is written **only** when its text differs from the text it was read from, extending
   3.1.1's zero-write rule to a second file — a scan of an unchanged project now writes zero bytes
   anywhere, not "identical bytes" (D15/INV-C5). The walk dedups **directories** by canonical path,
   closing the carried-forward symlink defect (see the D9 correction below). Two deviations from the
   plan, both logged in the code and restated here: `rescan()`'s call site in `editor_app.cpp` needed
   a one-line update (project root and assets root as two separate arguments); and `invalidateCache()`
   needed a one-shot flag, because the plan's own phase-3 snippet reloaded the cache from disk
   unconditionally on every scan, which would have silently undone the in-memory clear before phase 4
   ever ran — this is **the `invalidateCache()` defect**, found from a failing test, not by reading
   (see below).
9. The editor surface (`asset_browser_panel.{hpp,cpp}`, `editor_app.{hpp,cpp}`) — a Reimport All
   button beside Refresh, the selected file's import-state segment appended to the footer, and
   `tick()`'s reconcile draining **both** one-shots first and unconditionally, neither on the right of
   a `\|\|` (a bug this tree already shipped once, in 3.1.1's own code-review round). Three new
   GPU-tier cases (I31–I33). A scan of a clean, fully-cached project logs exactly two INFO lines and
   nothing else.
10. The sabotage-matrix gap fixes (`f7198fc`) — see the sabotage section below; four real,
    test-invisible defects, all fixed, none of them behavioural regressions found in the shipped code
    itself.

**What was deliberately left out**, named so nobody re-derives it as an oversight: no importer runs
(D16 — nothing in this tree has ever imported anything; `importer`/`importerVersion` are plumbed and
inert); no cooked artifacts (3.3's `artifacts` key, additive); no per-asset cache files, one index for
the whole project (A7 weighs the alternative and defers it to 3.3 if `parseAssetCache`'s wall time
ever demands it — see the measurement below); no async hashing (A19 of the spec — the scan is
synchronous and bounded by the hash budget, surfaced by a WARN; 3.1.4's watcher is the real answer);
no cycle report (A18 — a cycle needs no detection code at all, by construction, so there is nothing to
report); and no `Delete orphaned .meta` action (3.1.3's — this task reports an orphan and leaves it,
exactly as 3.1.1's D8 already required for every other kind of orphan).

**Both inherited items are now CLOSED, not carried forward.** The D8 orphan-re-attachment deferral is
closed by `planReattachments`' five conditions (item 7 above; `AD46` proves it end to end: rename a
file without its sidecar, the GUID survives, and the old sidecar is still present byte for byte). The
symlinked-directory duplicate-GUID defect is closed by canonical-path directory dedup (item 8 above;
`AD40` is the defect's own regression test, and `AD41`/`IP36` are the two "identical copies keep two
identities" cases that prove content-equality dedup was never the right mechanism).

**The D9 rationale correction — recorded here because the earlier paragraph in this same file was
wrong, and the wrongness matters.** 3.1.1's own entry above says *"3.1.2 is the right owner because it
introduces the content hash — with it, two paths hashing identically are provably the same content."*
**That is false as written, and this task's own plan caught and corrected it (§A/D9-A12) before any
code was written to the wrong design.** A content hash proves two *paths* hold the same *bytes*; it
cannot distinguish one physical file reached twice through a symlink from two legitimate, independent
files that simply happen to be byte-identical (two copies of the same stock texture, say). Deduping by
content hash would have silently deleted the identity of every duplicated asset in every project — the
opposite of what D6/D8 exist to prevent. The defect is a **physical-identity** problem and needed
`canonicalDirectory` (physical path, symlinks resolved), which this task placed in `project_files.cpp`
precisely so `asset_database.cpp` still includes no `<filesystem>` (INV-A6 survives untouched). The
correct, narrow claim: 3.1.2 owns the fix because it is the task that finally has somewhere principled
to put the `<filesystem>`-touching helper, not because of anything the content hash itself proves. File
symlinks and hardlinks are deliberately **not** deduped — only a symlinked *directory*, by its resolved
canonical path — and 2.2.4's D13 ("a symlinked asset folder behaves like a real one") remains true for
the Asset Browser and changes only for the asset scan; the split is recorded explicitly in
`.claude/rules/editor.md`.

**Findings from the build itself, named explicitly because they are the point, not colour:**

1. **A1 — `tests/editor/text_file_test.cpp` did not exist.** The spec said the disk primitives'
   coverage would be "extended"; measured at `a651e28`, there was no such file — `readTextFile`,
   `writeTextFileAtomic` and `fileExists` had only ever been exercised incidentally, through the scene
   and project test batteries. Created as a new, unconditional TU on `aero_editor_shell_test`
   (`TF1`–`TF22`), the honest home for `hashFileContents`/`ensureDirectory` and the first direct
   coverage the three older primitives have ever had.
2. **A2 — inserting `mtime` between `size` and `isDirectory` would have silently re-mapped two
   positional aggregate initializers in `project_files_test.cpp`.** `bool → std::int64_t` is an
   integral promotion, not a narrowing conversion, so nothing diagnoses it — a `dirEntry("x")` helper
   would have silently started building a *file*. Fixed by appending the three new fields after
   `sizeKnown` instead of inserting them, and converting both helpers to designated initializers in
   the same commit as defence in depth for the *next* field addition.
3. **A3 — the spec's claim that `mtime` is "effectively free" once `listDirectory` has already
   stat'd every file is FALSE on libc++ and libstdc++.** Reading the SDK's own
   `__filesystem/directory_entry.h`: `is_symlink`/`is_directory` return the type POSIX `readdir`
   already cached — zero syscalls — but `last_write_time` reaches the un-cached branch and issues a
   **second, independent `stat`**. `isSymlink` is free everywhere; `mtime` costs one extra `stat` per
   file on POSIX and nothing on Windows (`FindFirstFileW` already returns it). Taken deliberately —
   what this task removes is *reading every asset's bytes*, three to six orders of magnitude more
   expensive than a warm `stat` — and `entry.refresh(ec)` was deliberately **not** added, because it
   would change the dangling-symlink asymmetry 2.2.4's code review already had to discover and fix
   once, on a behaviour this machine cannot verify against Windows or Linux.
4. **A4 — MurmurHash3 x64_128 of the empty input with seed 0 is ALL ZEROS**, so `hashBytes({})`
   returns a nil `ContentHash` and `valid()` is false for it. Verified, not recalled: the canonical
   public-domain reference was fetched, compiled and run in a scratch directory (see the AC-6
   cross-check below), and it is arithmetic, not luck — every finalization step for `len == 0` is an
   xor-shift or a multiply, and `fmix64(0) == 0`. **Four consequences:** (i) the empty-input literal
   has essentially zero discriminating power as a sabotage discriminator — confirmed directly by seed
   S2 below; (ii) `AssetRecord::contentHash` cannot be documented "nil when unhashed", since nil is a
   legitimate value — the real discriminator is `AssetRecord::change` (`Unhashable`/`NotHashed` mean
   "no hash this scan"); (iii) `parseAssetCache` **accepts** a nil `contentHash`/`metaHash` — only
   `guid` may never be nil — or every zero-byte asset would be `New` forever; (iv)
   `ContentHash::valid()`'s header comment says explicitly it is not a "was this hashed?" flag — the
   only such flag is engagement of `std::optional<ContentHash>`.
5. **A7 — the spec's phase-1 instruction to compute a "relative path" for `Library/` needs
   `<filesystem>`, which `asset_database.cpp` may not include (INV-A6).** Resolved by reusing D9's own
   `canonicalDirectory` machinery: the library directory's canonical path is computed once, and a
   child directory whose canonical path matches it is skipped in the walk rather than descended into
   — no new string logic, no new helper, one comparison inside a loop that already computes exactly
   this value for every child. Two corollaries make it safe on the *first* scan: `Library/.gitignore`
   is dot-prefixed, so `listDirectory(..., includeHidden=false)` never lists it; and `Library/` is
   created only in phase 8, after the walk, so the first scan cannot see it at all.
6. **A9 — a defaulted parameter, not a mutable constant, applied TWICE.** `rescan`'s `hashBudgetBytes`
   and (in the `f7198fc` gap-closing round) `parseAssetCache`'s `maxEntries` both take the shape
   `listDirectory(root, rel, includeHidden)` already established: production calls with the default,
   so the real constant (`MAX_HASH_BYTES_PER_SCAN`, `MAX_CACHE_ENTRIES = 200000`) is what every real
   scan exercises and stays pinned at its own declaration, while a test reaches the budget/truncation
   branch by passing a small value — settled with the user four days before this task started (3.1.1's
   §W5/AD23), and re-applied rather than re-litigated.
7. **A10 — the metaHash defect, the most important completion in this task.** `metaHash` for a
   newly-written sidecar (`Created`/`Repaired`/`Reattached`) has no sidecar to read at plan time — the
   sidecar is written later, in phase 7. The obvious reading (commit `metaHash = nil` for those
   records) makes the **very next** scan read the real sidecar, compute a real digest, see a mismatch
   against the nil it committed, and report every freshly-created asset as `MetaChanged` — which would
   fail human-validation row 2, the exact demonstration this task exists to produce. Fixed: phase 7
   computes the digest of the text it actually wrote and records it; phase 8 reads that digest for
   every record, whatever its state; and a write **failure** excludes that record from the cache
   entirely — the bytes on disk are not the bytes that were hashed, and committing a hash for a file
   this scan failed to write is exactly the false-`UpToDate` R-C2 forbids. `AD62` pins the fix
   directly (a freshly created asset is `UpToDate` on the very next scan); `AD53` pins the write-failure
   exclusion.

**The `invalidateCache()` defect** (found from a failing test while implementing Step 8, not by
reading): the plan's own phase-3 snippet reloaded the cache index from disk unconditionally on entry,
which made a call to `invalidateCache()` a silent no-op — the in-memory clear it performed was
immediately undone before phase 4 ever ran, and AC-35 (Reimport All actually re-hashes everything) was
structurally unobservable by any test. Fixed with a one-shot `cacheInvalidated` flag, consumed by
exactly the next phase 3 and never left set across a scan that could not run at all (an empty root,
say). This mattered beyond the one test: `Reimport All` is the user's entire escape hatch for a
corrupted or stale cache, and the unfixed code would have made the button do nothing while reporting
success.

**The amended INV-A1 / AC-32.** 3.1.1's invariant — "exactly one `writeTextFileAtomic` call site" —
is unsatisfiable as literally restated for this task, because D6 (`Library/.gitignore`, written once
on first scan) and D15 (the index itself) both mandate additional writes. There are now **three** call
sites in `asset_database.cpp`: exactly **one** built from the **assets** root (still behind the
case-insensitive write-conflict guard 3.1.1's code review added) and exactly **two** built from the
**library** directory. Each path is assembled into a named local first (`metaAbsolutePath`,
`ignorePath`, `indexPath`), which is what keeps the amended invariant grep-decidable rather than
heuristic. The invariant's real content — one guarded write path into the user's own tree, everything
else confined to derived, disposable data — holds exactly as before.

**The AC-6 cross-check record.** The canonical public-domain MurmurHash3 reference
(`aappleby/smhasher`, `src/MurmurHash3.cpp`) was fetched from
`https://raw.githubusercontent.com/aappleby/smhasher/master/src/MurmurHash3.cpp`, SHA-256
`30f121ed155ebf336af398aabb7d8d157afdfafc8d981e7b48d2a1ceb4b63e4e`, compiled with
`clang++ -std=c++20 -O2` and run in a scratch directory. The published SMHasher `VerificationTest`
value for Murmur3F (x64_128) reproduced exactly as `0x6384BA69`. Two pinned inputs, both cross-checked
independently a second time from the published algorithm and matching exactly:
`"The quick brown fox jumps over the lazy dog"` → `e34bbc7bbc071b6c7a433ca9c49a9347`, and the
1 048 581-byte repeating pattern the spec names → `912d8bc874074f7eb99738f4eaeb2311`. **The word-order
trap, worth stating precisely because a naive comparison would call this a mismatch:** the reference's
own raw buffer dump for the fox case reads `6c1b07bc7bbc4be3…` — this tree's `formatContentHash`
deliberately emits `hi` first, big-endian per word (the same convention `formatGuid` already uses), so
the two hex strings are genuinely the same 128-bit value read in a different, chosen byte order, not a
discrepancy.

**Three measurements, taken on this machine (macOS arm64, `macos-debug`, ASan+UBSan — the build every
other measurement in this project has been taken from, so the number is comparable across tasks even
though it is not release-representative):**
- **A3's extra-`stat` cost** — measured directly against the SDK's own `directory_entry`
  implementation (not timed): ~7–9 ms of extra wall time per 5 000-file `listDirectory` call
  (~45–50% of that call's own time in isolation), all of it the second `stat` `last_write_time` issues.
- **`AssetDatabase::rescan` wall time over a synthetic 5 000-file tree** (a standalone harness linked
  against this tree's own built `aero_editor_core`/`aero_core`/`aero_scene` static libraries, run
  twice for stability): a **cold** scan (5 000 new files, 5 000 `.meta` writes, 5 000 hashes) took
  **~60.6 s**; the immediately following **warm** scan of the identical, unchanged tree (0 created, 0
  bytes read, the D15 fast path exercised throughout) took **~7.3–7.4 s**. Both numbers are Debug/ASan
  — a Release build with no sanitizer instrumentation would be substantially faster — but they are the
  first real measurement of the scan's own wall time this project has recorded, and the ratio (roughly
  8×) is the number that matters: the warm path is doing real work (5 000 stats plus 5 000 sidecar
  reads plus the index comparison) even though it writes nothing.
- **`parseAssetCache` wall time for a synthetic 5 000-entry index**: **~336–355 ms**. Linear in entry
  count and small enough that A7's per-asset `Library/Artifacts/` alternative is not warranted yet —
  recorded as the number 3.3 should re-measure against before deciding, not acted on here.

**Test inventory, measured fresh with `--list-test-cases`, not arithmetic'd:** `ctest -N` **95**
tools-ON / **6** tools-OFF / **19** reflect-OFF — unchanged, this task registers **zero** new ctest
entries (every new case lives inside an existing TU). `aero_tests` **389 → 415**
(`tests/content_hash_test.cpp` `CH1`–`CH26`, the `ContentHash` codec/order/hash-mix/MurmurHash3
battery — a new TU, `aero_tests`' first for this task). `aero_editor_shell_test` **468 → 617**
(`tests/editor/text_file_test.cpp` `TF1`–`TF22`, new; `tests/editor/asset_cache_test.cpp`
`IC1`–`IC34`/`IG1`–`IG6`/`IP1`–`IP40`, new; `tests/editor/asset_database_test.cpp` `AD32`–`AD62`,
extended — **617 measured directly with `--list-test-cases`, not derived by addition**, the standing
lesson this project keeps re-verifying rather than re-learning the hard way a third time).
`aero_editor_imgui_test` **52 → 56** (`I31`–`I34`: the cache survives a project reopen, an edit is
detected and scoped to the changed file, `Reimport All` drives a full re-hash through its one-shot
channel, and `I34` mechanically proves the reimport flag drains unconditionally). `aero_scene_serialize_test`
and `aero_editor_inspector_test`: **23**/**22**, both unchanged. Both reduced configurations, freshly
rebuilt for this documentation step rather than trusted from a stale directory (`build/tools-off-3.1.2`,
`build/reflect-off-3.1.2`): `ctest -N` **6**/**19**, unchanged, and — this is the number that actually
proves AC-17's claim for this task — `aero_editor_shell_test`'s own `--count` reads **593** in *both*
reduced configurations, up from the 589 measured before commit 10's four new shell cases landed, so the
format and the three planners are present, not skipped, in every reduced configuration; all six ctest
entries and all nineteen respectively still pass 100%. `check-math-boundary.sh`: **255 → 262** (nine
new/extended C-family files scanned across Steps 1–8, unchanged through Steps 9–11 since docs are not
C-family) — measured after `git add` at every step boundary, never assumed. Guard count stays **six**;
`check-project-no-delete.sh`'s own final line now reads **"6 files scanned"**, up from 5.

**The sabotage matrix — 31 seeds (S1–S28, S27b, S29–S31) plus all 3 mandatory second-order checks, run
and confirmed against the real built binaries, not reasoned about.** Every seed was applied, confirmed
**present** with `git diff` before trusting any verdict (the standing BSD-`sed`-false-PASS lesson),
rebuilt (never a stale binary), run through the full `AERO_REQUIRE_GPU=1` suite, and reverted with the
revert confirmed byte-for-byte clean against `HEAD` before the next seed began. **Exact arithmetic: 8
matched their prediction** (S2, S8, S9, S20, S22, S23, S30, S31) **+ 2 confirmed non-discriminators**
(S1 — `readLe64`'s aligned-load shortcut is invisible on any little-endian, non-strict-alignment
target this project builds for; S6 — `hashFileContents` implemented as `readTextFile` + `hashBytes`
breaks a memory property, not a correctness one, so nothing functional reddens) **+ 1 predicted
contingency came true** (S28 — the Reimport All one-shot moving to the right of a `\|\|` reddens
nothing through `I33` alone, exactly as the plan's own honest prediction said it might, because `I33`'s
second tick never sets `assetReimportRequested`; **this is precisely the gap `f7198fc`'s `I34` was
written to close**) **+ 20 differently-shaped findings, every one a genuine discovery about which test
actually discriminates which bug, not a failure of the matrix. 8 + 2 + 1 + 20 = 31.**

The notable differently-shaped findings, beyond the four `f7198fc` fixed outright (§ above):
- **S5 (`formatContentHash` emits `lo` first) reddened TWENTY cases against four predicted**
  (`CH12`/`CH21`/`CH22`/`IG1`) — the word-order convention this task's own AC-6 cross-check record
  above had to state precisely is exercised by every case that round-trips a hash through text, not
  only the four the plan named.
- **S19 (the cascade enqueues on every visit instead of only on the clean→dirty transition) revealed
  that `IP18`/`IP20` HANG rather than fail an assertion on a non-terminating cascade.** The plan's own
  §B claim — "written with a hard bound so it fails an assertion, never hangs" — is **wrong**, measured
  directly. An iteration cap was considered and rejected as the fix: it would be a disguised cycle
  detector, which D12 explicitly forbids (a cycle is supposed to converge by becoming uniformly dirty,
  not by hitting a cap). Left as a real, documented risk: a regression here costs a hung CI job, not a
  red one, until a human notices the timeout. Recorded rather than silently patched around.
- **S28 reddened nothing**, exactly the contingency above — and the `cacheTruncated` flag it touches
  tangentially was found, while investigating this seed, to be **entirely unasserted anywhere in the
  suite** until `f7198fc`'s `IC25` closed it (via the A9-pattern defaulted `maxEntries` parameter,
  applied a second time).
- **S13/S14 (breaking the fast path; comparing only `size`, not `mtime`) each confirmed exactly as
  predicted** (`AD33`, `AD37`) — the two seeds most directly testing "the cache's whole reason to
  exist" needed no correction.
- **S24 (re-attachment deletes the old sidecar) reproduced 3.1.1's own S21 finding exactly, one task
  later:** the widened no-delete guard fails **before any test even runs**
  (`check-project-no-delete.sh` exits 1), and `project-no-delete.no_delete_e2e` stays green throughout
  because it exercises the script against its own hermetic scratch tree, never the real source — both
  facts together are the intended behaviour, not a contradiction.

**AD45's unreachable branch.** On this machine's APFS volume, `listDirectory`'s `is_directory()` and
`canonicalDirectory()` never disagree — proven with four separate constructions (a dangling symlink, a
`chmod`-000 target, a mutual symlink cycle, a 60-hop symlink chain) — so the WARN for "could not be
resolved" is unreachable from any test this tree can run here. The defensive branch stays in
production code regardless: `AD45` documents the gap and reports it as a real, open, machine-dependent
coverage limitation rather than asserting coverage that does not exist, the same posture 3.1.1's S17
took for its own case-insensitive-filesystem gap.

**A process note worth recording, not colour.** During this task's execution, one subagent invocation
died mid-run and left sabotage seed **S3** (the `ContentHasher` carry-buffer top-up deletion) present
in the working tree, uncommitted. It was caught by inspecting `git diff` before trusting the tree as
clean and before starting the next seed — **a dead agent process is not evidence of a clean working
tree**, and the standing "confirm the seed landed, confirm the revert landed" discipline this project
already practices for every sabotage seed is exactly what caught it here, on the meta-level of the
process itself rather than on any one seed's own prediction.

**The three mandatory second-order checks, run for every one of the 31 seeds, no exceptions:** (1) the
seed was actually present — `git diff` shown before trusting any verdict; (2) the suite was actually
rebuilt, not stale — confirmed via the build tool's own incremental step list on every build; (3) the
revert restored the file byte-for-byte — confirmed via `git diff` (empty) for every touched file after
every seed. All three passed for all 31 seeds.

**Verification for this task:** six guards green; the full `ctest --preset macos-debug` suite passing
100% of 95 (~163 s, `AERO_REQUIRE_GPU=1`); both reduced configurations freshly rebuilt and passing
100% (6/6 tools-OFF, 19/19 reflect-OFF), `aero_editor_shell_test` reading **593** doctest cases in each;
`check-math-boundary.sh` **262**; `check-project-no-delete.sh` **6 files scanned**; every changed file
confirmed byte-identical to `HEAD` after every one of the 31 sabotage reverts.

#### Task 3.1.3 — Asset browser v1

**3.1.3 upgrades 2.2.4's read-only file lister into the real Asset Browser: a thumbnail grid with
generated type icons and real decoded image previews, a project-wide search with a kind filter, the
list view preserved verbatim, a surfaced Issues list reading the scan report 3.1.1/3.1.2 already
compute, and the one user-initiated destructive action this whole subsystem permits — deleting an
orphaned `.meta` sidecar, closing both 3.1.1's D8 and 3.1.2's D13 deferrals in the same task.** Four
new `/editor` pairs: `asset_view.{hpp,cpp}` (pure — kinds, icons, filter/search, grid geometry, no
ImGui/`<filesystem>`/GPU), `thumbnail_cache.{hpp,cpp}` (pure — the key, the state machine, the
budget/LRU ledger, and a deterministic INTEGER box resampler, no floating point anywhere so the
output is byte-identical across macOS/Windows/Linux), `thumbnail_store.{hpp,cpp}` (src-private — the
ONLY stb_image TU and the ONLY GPU-touching TU for thumbnails; `STBI_NO_STDIO`/`STBI_NO_FAILURE_STRINGS`
keep it disk-primitive-free and text-free), and `asset_actions.{hpp,cpp}` (the fifth editor TU to
include `<filesystem>`, holding exactly one `std::filesystem::remove` call, gated by a six-step
re-verify-everything-before-deleting algorithm). `vcpkg.json` gained one new port, `stb` — editor-only,
confined to `thumbnail_store.cpp` by file placement (no third-party image-decode type crosses any
public header). **Zero paths under `engine/`** — the empty-`engine/`-diff streak 3.1.1 ended and 3.1.2
repeated restarts at one here; 3.1.3 needed no engine change at all, exactly as the plan predicted
(`engine::MeshRenderer` has no asset-referencing field yet).

**Drag-into-scene was excised into a new task, 3.1.5 (D2), not shipped here.** Verified directly before
cutting it: `engine::MeshRenderer` is `{std::uint32_t primitive AERO_RANGE(0,2); Vec3 color
AERO_COLOR;}`, pinned by `static_assert(sizeof(MeshRenderer) == 4 * sizeof(float))`, and
`git grep -ln 'Guid' -- engine/scene/` is empty — nothing in `engine::scene` can point at an asset
today, so a drop could only create an entity that does not and cannot refer to the dropped file,
invisible now and still wrong once 3.2.1 gives assets something to be referenced by. `docs/tasks/phase-3.md`
is amended accordingly: 3.1.3's size `M → L` (it also carries the Issues list and the orphan-delete
action neither original subtask named), its second subtask renamed to "Search/filter", and a new
3.1.5 row appended to Epic 3.1 depending on `3.1.3, 3.2.1`; 3.1.4 (hot-reload) keeps its number and
its dependency on 3.1.2 untouched.

**Build-time findings, three of them CI-only and deterministic, none of them a design flaw:**
- **MSVC's `<string_view>`/`<ostream>` completeness trap, a new instance of an old class.** `INFO("ext:
  ", ext)` with `ext` a `std::string_view` cascaded into an MSVC STL compile error inside
  `<__msvc_string_view.hpp>`. Fixed by converting to `std::string` at every affected call site — the
  same fix class this project has now hit more than once, never in a way a macOS-local build can see.
- **Linux clang-tidy, nine findings, all mechanical:** `bugprone-string-literal-with-embedded-nul`
  (`"before\0after"` truncates silently at the embedded NUL — fixed via the `(ptr, len)` constructor),
  two `misc-unused-using-decls`, a `bugprone-misplaced-widening-cast`, a
  `bugprone-implicit-widening-of-multiplication-result`, and a `readability-identifier-naming` local
  constexpr rename to `SCREAMING_SNAKE_CASE`.
- **A Windows-only, fully deterministic failure: a test fixture literally named `nul.bin`.** Windows
  treats `NUL` as a reserved device name **regardless of extension** — the write silently discarded
  every byte, and the read-back assertion failed. Confirmed deterministic (re-run, identical failure)
  before fixing: renamed to `embedded-nul.bin`, and replaced the truncating literal-assignment with the
  explicit-length `std::string(ptr, len)` constructor already used elsewhere in the same file. After
  this fix all three CI lanes, lint and the vcpkg-baseline guard went green together — **Step 5's own
  isolated CI round (the point of pushing it alone) needed no `stb`/UBSan contingency at all**; every
  finding was in this task's own new test code, not in the new dependency.
- **A duplicate-member compile error the plan's own text would have caused.** §D-6 said `return
  records;` for the new `AssetDatabase::records()` accessor, but the private member was ALSO already
  named `records`. Fixed by renaming the private member to `recordList`, the identical
  `databasePtr`/`database()` naming-collision precedent 3.1.1 already established for exactly this
  class of clash. Logged as a minor deviation, not a stop.
- **A UBSan abort on `+inf → int`.** `gridColumnsFor(inf, 64, 8)` cast an infinite quotient to `int`,
  which is undefined behaviour, not merely a warning. Fixed with a `MAX_REPRESENTABLE_COLUMNS` clamp
  before the final cast — the same "NaN/inf must never reach a narrowing cast" discipline this project
  already applies elsewhere in `core/math`.
- **A grid-layout design bug caught in review before any test ran:** drawing `".."` then `SameLine()`
  ahead of the clipper-driven grid loop would have made row 0 hold `columns + 1` tiles. Fixed by giving
  `".."` its own dedicated row unconditionally, matching the List view's own existing treatment — no
  `SameLine()` after it at all.
- **A doctest `REQUIRE_FALSE` "Expression Too Complex" trap, the third time this exact shape has hit
  this tree.** `REQUIRE_FALSE(hasTake && hasOr)` in I42's mechanical source-text proof needed the extra
  parens `REQUIRE_FALSE((hasTake && hasOr))` doctest's expression-decomposition requires for a raw
  `&&` — I30/I34 already carry this exact idiom in the same file.
- **I42's own mechanical proof initially false-failed on its own explanatory comment**, which
  legitimately contains both `takeOrphanDeleteRequest()` and `\|\|` on one line. Fixed by
  comment-stripping before matching (cut at `//`), mirroring the shell guard scripts' own load-bearing
  comment-stripping rule — a check that cannot tell code from comment is not a mechanical proof.

**Two genuine test-quality gaps, found by sabotage and fixed, not merely logged (commit `2f19c52` and
the sabotage-completion commit `acb52fe`):**
- **I36's baseline-attempt count was captured AFTER the first `tick()`, not before** — hiding a decode
  burst that happens entirely within that first tick (found directly: sabotage seed S4 stayed green
  against the original, later-baseline shape of the case). Fixed by reading
  `app->thumbnailLoadAttempts()` (== 0, no tick has run yet) before the first `tick()` call.
- **TC20's touch order never exercised the vulnerable `std::lower_bound` collision path a hash-dropping
  `ThumbnailKey::operator==` (seed S8) would expose** — the smaller-hash key was touched first, the
  larger-hash key second, which never forces the comparator through the case that matters. Fixed by
  swapping the touch order; confirmed it now reddens correctly under S8.
- **`searchAssets`' own leaf-vs-full-path extraction had NO test that actually exercised it.** The
  plan named `AV36` as S14's discriminator, but `AV36` calls `matchesFilter` directly with an
  already-bare leaf (`"plank.png"`) and never reaches `searchAssets`' own leaf-slicing call site — a
  seed passing `record.relativePath` straight through instead of the sliced leaf stayed **739/739
  green**. Added `AV39b`: a record under a directory literally named after the query, asserting zero
  hits — confirmed it passes against real code and reddens under the S14 mutation. This is a genuine,
  differently-shaped finding, not a contingency: the plan's own claimed discriminator for S14 does not
  actually discriminate it.

**The sabotage matrix — all 35 seeds (S1–S35), plus all 3 mandatory second-order checks per seed, run
and confirmed against the real built binaries.** Every seed was applied, confirmed **present** with
`git diff` before trusting any verdict, rebuilt (never a stale binary — confirmed via the build tool's
own incremental step list), run through the relevant suite(s) (targeted binaries for most seeds,
reserving full-`ctest` runs for the highest-risk/broadest-blast-radius seeds — S23 and S35, which
touch the architecture guard itself, were additionally confirmed against the hermetic
`project-no-delete.no_delete_e2e` ctest case, not merely the raw script), and reverted with the
revert confirmed byte-for-byte clean against `HEAD` (`git diff` empty **and**
`git show HEAD:<file> | diff -q - <file>`) before the next seed began. **Exact arithmetic: 23 matched
their prediction** (S1, S2, S4, S5, S6, S8, S12, S15, S16, S18, S19, S20, S21, S22, S23, S24, S26,
S28, S29, S31, S32, S33, S35) **+ 7 confirmed non-discriminators** (S3, S7, S9, S10, S11, S30, S34)
**+ 0 predicted contingencies** **+ 5 differently-shaped findings** (S13, S14, S17, S25, S27) —
**23 + 7 + 0 + 5 = 35.**

**The eight seeds the task explicitly required regardless of prediction (S3, S7, S9, S10, S11, S25,
S30, S34) — every one run, every one recorded honestly:**
- **S3, S7, S9, S10, S11** — all five confirmed non-discriminators exactly as predicted, on this
  machine, in this build. No invented coverage was added for any of them; each stays a documented,
  machine-dependent gap, the 3.1.1 S17 posture applied a fifth time.
- **S25 (`serviceThumbnails()` called from inside `onDraw` instead of after `renderScene`) is the one
  nuanced result in the whole matrix.** Seeded literally at the very START of `onDraw()` — before any
  tile's `touch()` call has run that frame — it reddened **4 GPU-tier cases** (I36, I37, I38 and one
  more), because thumbnails decode a full frame behind their own visibility. Seeded instead at the
  natural END of `onDraw()` (after every tile has been drawn and touched that frame, still structurally
  "inside the draw walk"), it matched the plan's own prediction exactly: **64/64 green**, confirming
  §V6's grep cannot see this class of violation and only human row 4 ("never stutters") covers it.
  Both placements are legitimate readings of "called from inside `onDraw`"; the first is a stronger,
  differently-shaped finding worth recording precisely rather than discarding in favour of the
  matching result.
- **S30 (`~ThumbnailStore` does not destroy its textures) matched its non-discriminator prediction
  exactly under ASan — 64/64 tests stayed green, because `rhi::TextureHandle` is not a heap allocation
  ASan tracks** — but the run's own log carried `rhi: ~Device releasing 1 leaked texture(s)`, the RHI
  layer's own destroy-accounting catching what ASan cannot. This confirms, rather than merely asserts,
  the plan's own stated rationale: AC-10's leak claim rests on the RHI's own accounting and on
  `I38`/`I40`'s resident counts, never on ASan.
- **S34 (the grid clipper's `items_height` omits `ItemSpacing.y`) matched its non-discriminator
  prediction exactly** — 739/740 unit + 64/64 GPU cases all stayed green; only a human's own eyes on a
  scrolled grid (human row 3) can ever catch a wrong scroll-range estimate.

**Two further differently-shaped findings beyond S14 and S25, both real and both left as documented
gaps rather than patched into artificial coverage:**
- **S13 (`readFileBytes` opens in text mode) is invisible on this machine, and that is expected, not a
  gap in the seeding.** `std::ifstream`'s text-mode CRLF translation is a Windows-only behaviour;
  on macOS/Linux, text mode and binary mode read identical bytes, so `TF23`/`TF24` cannot discriminate
  it here by construction — the identical asymmetry `text_file.cpp`'s own comments already document
  for `readTextFile`/`hashFileContents`. This is real evidence the seed is a genuine, Windows-only
  discriminator, not evidence the guard is weak.
- **S17 (`classifyAssetKind` folds case with `std::tolower(char)`/the C locale) did not redden `AV11`/
  `AV12`, and clang-tidy's `bugprone-signed-char-misuse` did not fire on either a `static_cast`-guarded
  or a naive `std::tolower(c)` shape tried in isolation on this toolchain.** `AV11`'s inputs are pure
  ASCII, where `std::tolower` and the manual fold agree exactly; `AV12`'s non-ASCII input short-circuits
  on a length mismatch in `extensionEqualsFolded` before folding is ever reached, so neither case's
  assertion depends on which folding function is used. The plan's own comment in `asset_view.cpp`
  citing this check as "two independent nets" is not reproduced by this seeding on this LLVM 18 build;
  recorded as a real, differently-shaped finding rather than silently assumed to still hold.
- **S27 (`EditorApp` drops the `setScanReport` reconcile) is a genuine NON-discriminator across the
  WHOLE suite, not narrower than predicted.** `assetOrphanCount()` — the accessor `I41` actually
  asserts on — reads `EditorApp::lastAssetReport` directly, never through the panel's reconciled
  pointer `setScanReport` maintains; dropping that reconcile therefore leaves `I41` (and every other
  GPU case) fully green. The Issues list `drawIssues()` renders is the reconcile's entire observable
  effect, and no test tier in this tree renders pixels — so this reconcile's whole proof surface is
  human validation, the identical posture the plan itself states for `S25` but did not state for `S27`.

**The three mandatory second-order checks, run for every one of the 35 seeds, no exceptions:** (1) the
seed was actually present — `git diff` shown before trusting any verdict; (2) the suite was actually
rebuilt, not stale — confirmed via the build tool's own incremental step list on every build; (3) the
revert restored the file byte-for-byte — confirmed via `git diff` (empty) **and**
`git show HEAD:<file> | diff -q -` for every touched file after every seed. All three passed for all
35 seeds.

**Verification for this task:** six guards green (`check-project-no-delete.sh` now running **two**
checks, Check A's six-file denylist and Check B's two-file positive allowlist); the full
`ctest --preset macos-debug` suite passing 100% of 95 (`AERO_REQUIRE_GPU=1`); both reduced
configurations freshly rebuilt and passing 100% (6/6 tools-OFF, 19/19 reflect-OFF),
`aero_editor_shell_test` reading **716** doctest cases in each. Measured inventory, not carried
forward: `aero_tests` **415** (unchanged — zero engine paths touched), `aero_editor_shell_test`
**753** (was **621** before this task; **+132**: `asset_view_test.cpp` AV1–AV42 (+AV39b) new TU,
`thumbnail_cache_test.cpp` TC1–TC38 new TU, `asset_actions_test.cpp` AA1–AA22 new TU, plus
`text_file_test.cpp`/`asset_database_test.cpp` extended in place and the code-review round's own
cases), `aero_editor_imgui_test` **65** (was 57; +8: I36–I42 plus the code-review round's I43), `aero_scene_serialize_test`/`aero_editor_inspector_test` **23**/**22**, both
unchanged. `aero_editor_core` sources **46** (was 42 — the four new TUs). `check-math-boundary.sh`
**262 → 273** (eleven new C-family files: the four new `/editor` pairs' eight files plus three new
test TUs — `asset_view_test.cpp`, `thumbnail_cache_test.cpp`, `asset_actions_test.cpp`);
`check-project-no-delete.sh`'s six-file Check A allowlist is unchanged in count; Check B's own
two-file `PERMITTED_DELETERS` allowlist is new this task, and its own final banner line now reports
both counts (six files for Check A, the full `editor/src/*.cpp` count for Check B). Every changed
file confirmed byte-identical to `HEAD` after every one of the 35 sabotage reverts.

**The code-review round (PR #67) found 11 findings, 3 BLOCKING — and it found them against a fully
green gate.** That is the entry's most important sentence: 95/95 ctest, six green guards, 35 sabotage
seeds, and all three CI platforms were green at `628bbfc` when the review started. A green matrix was
not evidence.

- **BLOCKING 1 — a real use-after-free of GPU textures, invisible on macOS by construction.**
  `applyPending()` is the last statement of `onDraw()`, so the `ReimportAll` arm's `store.clear()`
  destroyed every `rhi::TextureHandle` whose native `SDL_GPUTexture*` `drawTile` had already written
  into that frame's ImGui draw list; `endFrame()` then bound the freed pointer.
  `SDL_ReleaseGPUTexture` frees **synchronously** on Vulkan (`SDL_gpu_vulkan.c:7070-7073`) and D3D12
  (`SDL_gpu_d3d12.c:1460-1463`) — *"Containers are just client handles, so we can destroy
  immediately"* — and only **defers** on Metal (`SDL_gpu_metal.m:936-944`). So the defect was
  deterministic on Linux and Windows, in this task's own headline scenario, and **structurally
  unreachable on the only platform with a human pass**. Fixed by deferring the teardown to a flag
  drained by `serviceThumbnails()`, after its touch loop so keys drawn this frame are protected from
  eviction by E12's own rule. **No test caught it because `I33` drives
  `EditorApp::requestAssetReimport()`, a different path that never touches the panel's `pending` at
  all** — the seam `requestAssetBrowserReimportAll()` exists to close exactly that.
- **BLOCKING 2** — AC-13's first clause was never implemented: `refreshSearchRows()` early-returned on
  an empty query and neither content view consulted `filter` for the directory listing, so the kind
  combo was a **dead control** without a search term. `AV37`/`AV38` proved `matchesFilter` correct and
  nothing wired it to the listing — model coverage mistaken for feature coverage.
- **BLOCKING 3** — `drawFooter` resolved the selection by leaf name inside the *current* directory's
  listing while a search hit records a full relative path. Usually the footer went blank; when a
  same-named file existed in the current directory it paired the **search hit's path and GUID with a
  different file's size**, presenting two files as one record.

**Finding 8, recorded rather than fixed — `uploadTexture` is a full device stall, twice per tick.**
`Device::uploadTexture` ends in `SDL_SubmitGPUCommandBufferAndAcquireFence` +
`SDL_WaitForGPUFences` (`engine/rhi/src/sdl_gpu_backend.cpp:1620-1631`), and `device.hpp:134-141`
documents it in its own words as *"blocking"* and *"NOT a per-frame path"*. This task nevertheless
issues up to `MAX_THUMBNAIL_DECODES_PER_TICK = 2` of them per tick from `serviceThumbnails()`, inside
the frame. **This is human validation row 4's ("40 photos, progressive fill-in, no stutter") first
suspect, and it is a contradiction with the RHI's own stated contract, not an oversight.** It is left
as-is deliberately: the budget of 2 came from that same warning rather than from a measurement, no
stutter has actually been observed yet, and the alternatives (a non-blocking upload path, a staging
queue) are **engine** changes — this task holds zero `engine/` paths, and the RHI is treated as
sacred. If row 4 fails on any platform, lower the budget first and measure before touching the RHI.

**Finding 4's fix produced a second-order lesson worth more than the fix.** Extending `I39` to cover
the List view, both search branches and the confirmation modal required four new `EditorApp` seams —
and the first two attempts at the test **passed while executing none of the code they named**. The
Assets panel shares `DockSlot::Bottom` with the Console, `drawPanels()` calls `onDraw()` only when
`ImGui::Begin()` returns true (`shell_ui.cpp:356`), and a tabbed-behind panel therefore never drains
`pending` at all. A deliberately unbalanced `EndTable()` seeded into the List search branch failed to
redden the case twice — once because the panel never drew, and once more even after focusing it,
which exposed the real finding below. The case now `REQUIRE`s `assetBrowserListViewActive()` and
`assetBrowserSearchHitCount() > 0` **before** the frames that matter, so it cannot silently degrade to
covering nothing again, and `requestPanelFocus` must be issued **after** the dock layout exists — a
focus requested before the first tick lands while `buildDefaultLayout` is still running and does
nothing.

**A correction to this project's own stated rule: a presented frame is NOT proof of ImGui call
balance in this build.** `.claude/rules/editor.md` says an unbalanced call is an `IM_ASSERT` abort.
ImGui 1.92.8 ships `ConfigErrorRecovery = true`, and a `BeginTable` with its `EndTable` deleted leaves
`I39` **43/43 green** — verified directly, with a control seed proving the build was picking the edit
up. The GPU-tier cases therefore prove the draw paths **execute** (which is what makes ASan/UBSan and
any bad read inside them meaningful), but AC-21's balance claim rests on the source and on the human
rows, exactly as the modal's does. Do not read a green GPU tier as balance proof.
