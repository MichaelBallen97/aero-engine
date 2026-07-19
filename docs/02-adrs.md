# 02 — Architecture Decision Records

The decisions that define the project. Each records the discarded alternatives and **why**, so we do not re-litigate them in six months. ADR-001 through ADR-007 come from the original plan; ADR-004 and ADR-007 were updated in July 2026, and ADR-008 was added.

---

## ADR-001 — Core language: C++20

**Status:** Accepted · **Date:** July 2026

### Context
Developer with a web background (TypeScript), some Rust, no C++ experience. Goal: an open-source engine **with an editor**.

### Options

| Dimension | C++20 | Rust |
|---|---|---|
| Current familiarity | None | Medium |
| Engine ecosystem (ImGui, Jolt, EnTT, SDL_GPU, quickjs-ng) | Native, first-class | Bindings or less mature equivalents |
| **Editor** ecosystem | Excellent | Acceptable (egui), unproven in complex editors |
| Memory safety | None by default | Guaranteed |
| Dependency / build management | Painful (CMake + vcpkg) | Excellent (cargo) |

### Decision
**C++20.**

Rust would be the comfortable option — already somewhat known, and `cargo` would save months of cross-platform build pain. It is overruled by one empirical fact: **the editor-tooling ecosystem is still C++.** Bevy, Rust's flagship engine, has gone years without a stable editor — not because Rust prevents it, but because the path is less trodden.

Since the goal is a product **with an editor**, the ecosystem outweighs language comfort.

### Mandatory mitigations (non-negotiable)

1. **Handles, not pointers.** Everything (entities, textures, meshes, materials) is referenced by `{index: u32, generation: u32}`. Eliminates use-after-free at the root — the bug that kills C++ beginners and abounds in engines — and makes serialization trivial.
2. **Never manual `new`/`delete`.** RAII, `std::unique_ptr`, standard containers.
3. **ASan + UBSan in CI from commit #1.**
4. **Phase 0 is also a learning phase.** Budgeted as such.

### Consequences
- ✅ Direct access to the entire gamedev ecosystem
- ✅ External contributions more likely (open-source gamedev is C++)
- ❌ Cross-platform build will be painful. CMake + vcpkg from day 1
- ❌ Memory bugs are possible. The mitigations are not optional

---

## ADR-002 — RHI: SDL3 GPU API

**Status:** Accepted

### Context
Must render on Metal (macOS/iOS), Vulkan (Linux/Android), and D3D12 (Windows) from one codebase.

### Options

| Option | Effort | Control | Notes |
|---|---|---|---|
| **SDL3 GPU API** | Low | Medium | Ships with SDL3. No ray tracing |
| wgpu-native / Dawn | Medium | Medium | Modern API, free web target. Ideal if the core were Rust |
| bgfx | Medium | Medium | Mature but old API; maintenance in question |
| Custom RHI over Vulkan/Metal/D3D12 | **Very high** | Total | Only if learning graphics *were* the goal |

### Decision
**SDL3 GPU API**, wrapped in our own `engine/rhi`.

It abstracts the three backends with one API; shaders are written once in HLSL and **SDL_shadercross** compiles them to DXIL, MSL, and SPIR-V. Since SDL3 is already the platform layer, it adds no new dependency. Compute shaders are supported.

### Consequences
- ✅ Months saved vs. a custom RHI
- ✅ One dependency covers platform + render
- ❌ **No ray tracing or mesh shaders.** Ceiling accepted consciously (deferred to v3–v4)
- ⚠️ Wrapped in our own `engine/rhi` → if SDL_GPU falls short, it can be replaced without touching `engine/render`. **This wrapper is the escape hatch for the v3–v4 RT path — treat it as sacred.**

### Implementation note (task 0.4.1)

The public surface of `engine::rhi` landed: namespace `engine::rhi`, target `aero_rhi` STATIC (`engine/rhi/CMakeLists.txt`), linking `PUBLIC aero::core aero::platform` and **no vcpkg package yet** — the first engine target with neither. Eight `Handle<Tag>` aliases (`BufferHandle, TextureHandle, SamplerHandle, ShaderHandle, GraphicsPipelineHandle, SwapchainHandle, CommandBufferHandle, RenderPassHandle`) over phantom, never-defined tag structs cover the vocabulary, split into PERSISTENT handles (buffer/texture/sampler/shader/pipeline/swapchain, valid until their `destroy*()`) and TRANSIENT ones (command buffer/render pass, and swapchain-acquired textures, invalidated by the frame flow itself). All operations are `Device` member functions taking handles (the Godot-`RenderingDevice` shape) — `Device` is a move-only, heap-pimpl RAII class created via `Device::create(const DeviceDesc&) -> std::optional<Device>`, with its full method set **declared but deliberately NOT defined** until task 0.4.2 (`src/sdl_gpu_backend.cpp`); odr-using any member before then is a link error by design, not a runtime stub. The adopted SDL_GPU frame model is load-bearing, not incidental: swapchain textures are acquired onto a command buffer and presented automatically at `submit()` (no standalone `present()`), per-draw data travels through push uniforms, and there are no explicit barriers — the backend hides SDL's `cycle` mechanism entirely. The backend lands at 0.4.2; the mechanical boundary guard — `.github/scripts/check-rhi-boundary.sh` plus the compile-time probe `tests/rhi_boundary_probe.cpp` — landed at 0.4.5, mirroring the **math** pair's repo-wide-scan-plus-one-allowlisted-file structure, not `engine/platform`'s header-only scan (SDL_GPU is legal in exactly one TU, not across a whole layer's `src/`). Defaults across the new descriptor types restate the ADR-005 conventions at the GPU boundary for the first time: counter-clockwise front faces with back-face culling, depth range `[0,1]` with 0 = near, clear depth `1.0` paired with `CompareOp::Less`.

### Implementation note (task 0.4.2)

The backend landed in ONE TU, `engine/rhi/src/sdl_gpu_backend.cpp`, which defines every `Device` member declared in 0.4.1's `device.hpp` on top of real SDL_GPU calls; `aero_rhi` now links `PRIVATE SDL3::SDL3 aero::profiling aero::platform_internal` alongside its unchanged `PUBLIC aero::core aero::platform` line. The `SDL_Window*` seam (H1) is a one-window-wide friend-accessor struct (`engine::platform::internal::NativeWindowAccessor`) in a non-installed internal header, exposed through a header-only `aero::platform_internal` INTERFACE target consumed PRIVATE by `aero_rhi` alone — `window.hpp`'s only change is an SDL-free forward declaration plus one friend declaration, so the boundary rule holds without widening `engine::platform`'s public surface. E13 (destroy-while-in-flight safety) is now RESOLVED, verified at the pinned SDL 3.4.12 source: all three backends (Vulkan, Metal, D3D12) gate actual GPU-side reclamation on an atomic per-resource reference count that in-flight command buffers hold, draining pending destroys only once that count reaches zero — so the engine's `destroy*()` (a SlotMap remove followed by `SDL_ReleaseGPU*`) needs no engine-side deferral of its own; SDL already does the safe thing. Verify-don't-guess also corrected the `create()` doc comment's platform matrix: Metal's and Vulkan's `PrepareDriver` gate on the *video driver's* surface-creation capability, which SDL's headless (dummy) driver lacks, so headless `Device::create` fails gracefully on macOS and Linux and happens to succeed only on Windows/D3D12 (SDL's D3D12 backend has no such gate). The mechanical boundary guard (grep script + compile-time probe linking `aero::rhi` alone) landed at 0.4.5 — `.github/scripts/check-rhi-boundary.sh` + `tests/rhi_boundary_probe.cpp` — this task's public-header footprint is exactly the D5 constants and the D6 comment correction, nothing else.

---

## ADR-003 — Canonical format: glTF 2.0 + N importers

**Status:** Accepted

### Context
The request was to support `.obj` and `.blend` "like Unity does."

### Two important corrections

**1. Unity does NOT read `.blend` natively.** It detects that Blender is installed and **invokes it by command line** to export FBX, then imports that FBX. No Blender, no import. Same for `.max` and `.ma`. And rightly so: `.blend` is a **dump of Blender's internal memory**, tied to the version, with no stable public spec. Nobody sensible parses it by hand.

**2. `.obj` is from 1992.** No PBR, no skeletal animation, no scene hierarchy, no skinning. Good for importing a single static mesh and nothing else. **It can never be the canonical format.**

### Decision
**glTF 2.0 as the internal canonical format + importers.**

```
.blend  ──(Blender CLI: blender -b file.blend --python-expr "export glTF")──┐
.fbx    ──(ufbx)────────────────────────────────────────────────────────────┤
.obj    ──(tinyobjloader)───────────────────────────────────────────────────┼──> glTF 2.0 ──> cooked binary
.dae/.ply/.stl ──(Assimp, editor only)───────────────────────────────────────┤     (canonical)
.gltf/.glb ─────────────────────────────────────────────────────────────────┘
```

- **glTF 2.0**: open, modern, PBR-native, with skinning and animation
- **ufbx** (MIT, single-file C) instead of Assimp for FBX: lighter and more correct
- **Blender** auto-detected on the system (or the user points to the path). Blender ships the best glTF exporter that exists
- **Assimp never enters the runtime.** Only the editor links it

### Consequences
- ✅ Adding a new format = writing an importer. **The renderer is never touched**
- ✅ The runtime knows only one format → smaller binary
- ⚠️ `.blend` requires Blender installed on the user's machine (just like Unity)

---

## ADR-004 — Reflection by code generation

**Status:** Accepted · **From Phase 1, not Phase 2** · **Reasoning updated July 2026**

### Context
Reflection is the spine of the engine: it feeds the editor inspector, serialization, **and** the scripting bindings. It is the #2 technical risk of the project.

### Options

| Option | Boilerplate | Robustness | Build step |
|---|---|---|---|
| Manual registration with `entt::meta` | High | Low (drifts out of sync) | No |
| Macros + `boost::pfr` | Medium | Medium | No |
| **Code-gen with libclang** | **None** | **High** | Yes |
| C++26 static reflection | None | High | No — but not yet portable |

### Decision
**Code-gen with libclang, from Phase 1.**

Starting with manual registration and migrating later is exactly how you end up rewriting half an engine.

A component is annotated once:

```cpp
struct [[engine::component]] Transform {
    Vec3 position;
    Quat rotation;
    Vec3 scale [[engine::range(0.01f, 100.0f)]];
};
```

`tools/reflect-gen` parses the AST with libclang and **generates**:

| Generated artifact | Consumer |
|---|---|
| `entt::meta` registration | Runtime |
| Serialization readers/writers (JSON + binary) | Scenes, prefabs |
| quickjs-ng bindings | Scripting |
| **`.d.ts` files** | TypeScript autocomplete in VSCode |

### Updated note on C++26 static reflection (July 2026)

The original draft dismissed C++26 static reflection as "years away." That is no longer accurate: **P2996 was voted into C++26** (Sofia feature freeze, June 2025). However, it is only partially implemented in Clang (Bloomberg's fork) and GCC trunk, and **MSVC does not support it at all yet**. Because the build must compile on all three compilers today, **libclang code-gen remains the correct choice** — but `reflect-gen`'s output boundary is designed so we can **migrate to native `std::meta` reflection** (likely 2027–2028, when MSVC catches up), turning this from forever-infrastructure into a temporary bridge.

### Consequences
- ✅ Write the component **once** and it appears in the inspector, on disk, in TS, and in autocomplete
- ✅ Impossible for the four consumers to drift out of sync
- ❌ Up-front cost: 2–3 weeks building the generator
- ❌ Adds a build step (`reflect-gen` runs before compiling)
- ⚠️ Start with the minimal subset: plain structs + primitives + `Vec3`/`Quat`. Expand on demand

**This is the best-return investment in the whole project.**

---

## ADR-005 — Math: own types, swappable backend

**Status:** Accepted

### Context
"Is GLM scalable?"

### Analysis
GLM is proven and its GLSL-style API is easy to learn, but it has two real weaknesses: **slow compile times** (heavy templates) and **inconsistent SIMD support**. It is essentially in maintenance mode.

### Decision
**The right question is not "which library," it is "where does the boundary live."**

```cpp
// core/math/types.hpp — the ONLY public surface
namespace engine {
    struct Vec3 { float x, y, z; /* ... */ };
    struct Mat4 { /* ... */ };
    struct Quat { /* ... */ };
}
```

- GLM (or RTM) is an **implementation detail** behind these types
- **No `glm::vec3` ever crosses the engine's public API**
- Start with **GLM** for startup speed

### Consequences
- ✅ If profiling shows GLM costs, swap the backend to **RTM** (SIMD-first, SSE + NEON, engine-designed) without touching another line
- ✅ The exit door is open **by design, not by luck**
- ❌ A thin indirection layer to maintain
- ⚠️ Requires discipline: a single `#include <glm/glm.hpp>` outside `engine/core/src/math/glm_backend.cpp` breaks the guarantee. **A CI test verifies this: `.github/scripts/check-math-boundary.sh` (the `lint` job) plus the compile-time probe `tests/math_boundary_probe.cpp` (task 0.2.3).** The exit door is exactly one file wide — the public headers under `engine/core/include/aero/core/math/` are GLM-free, so "outside `core/math`" would understate the rule.

### Implementation note (task 0.2.2)

`Vec2/3/4`, `Mat3/4`, `Quat` landed in `engine/core/include/aero/core/math/`, reachable via the umbrella `<aero/core/math.hpp>`. GLM is confined to the single TU `engine/core/src/math/glm_backend.cpp` and linked `PRIVATE` in `engine/core/CMakeLists.txt` — the RTM exit door this ADR describes is now exactly one file wide. This task pins engine-wide conventions every later shader, camera, importer, and physics wrapper inherits:

- **Right-handed, Y-up, −Z forward** world/view space (glTF 2.0's convention).
- **Clip-space depth Z ∈ [0,1], 0 = near** (SDL_GPU's documented NDC). **Never** `proj[1][1] *= -1` — SDL converts Vulkan's NDC internally.
- **Column-major storage, column-vector math** (`M * v`, `Model = T * R * S`) ⟹ GPU upload is a memcpy, no transpose.
- **Radians** everywhere; the editor converts at the UI boundary.
- **`Quat{x, y, z, w}`**, identity `(0,0,0,1)` — glTF's accessor order; conversion to GLM goes through `glm::quat::wxyz()` only (GLM's own ctor argument order is `#ifdef`-dependent — `wxyz()` is not).
- Deferred on purpose: Euler angles (the Phase-2 inspector owns the order convention), `lookRotation`, integer/double vectors, SIMD alignment.

**The compile-time boundary, and its one documented limitation.** The `PRIVATE` link is verified to make `#include <glm/...>` a hard compile error (`fatal error: 'glm/vec3.hpp' file not found`) for **engine-layer targets that link only `aero::core`** — confirmed empirically with a throwaway probe target linking nothing but `aero::core`. **But** vcpkg installs every port into one shared, flat, per-triplet directory (`vcpkg_installed/<triplet>/include/{glm,doctest,SDL3,tracy,...}`), and every vcpkg CONFIG package's `INTERFACE_INCLUDE_DIRECTORIES` points at that same shared root — not a private, per-package directory. Consequently, **any target that links a vcpkg CONFIG package directly** (e.g. `tests/` → `doctest::doctest`) inherits the whole shared root and *can* resolve `<glm/...>`, independent of whether it also links `aero::core`. Inside `tests/`, the compile-level boundary does **not** hold; the textual grep guard `.github/scripts/check-math-boundary.sh` (task 0.2.3, run by the `lint` job) is the enforcement there. The permanent compile-time probe `tests/math_boundary_probe.cpp` (also 0.2.3) covers the public math headers at compile time instead. See risk **R12** in `docs/08-risks.md` for the project-wide consequence.

**Discharged by 0.2.3.** `aero_tests` compiling while linking `aero::core` but not `glm::glm` (AC-9(i) in the 0.2.2 plan) was a **weaker** regression test than it looked: because `aero_tests` inherits vcpkg's shared include root via `doctest::doctest`, a public math header could start including GLM and `aero_tests` would still compile. The robust, permanent version of that check is `tests/math_boundary_probe.cpp` — a tiny `OBJECT` library target that links only `aero::core`, includes `<aero/core/math.hpp>`, and links nothing else vcpkg-provided, alongside the grep guard above — a compile-time guard being strictly stronger than grep for the symbol-leak case.

---

## ADR-006 — Audio: own abstraction, miniaudio backend

**Status:** Accepted — same pattern as ADR-005

### Context
"Is miniaudio the best option, or is there something more scalable?"

### Prior filter: the license
The project is **MIT open source** → **FMOD and Wwise are excluded.** They are proprietary; they cannot be distributed. (Godot supports them as *third-party plugins* the user integrates. That is the pattern to copy.)

### Analysis
Of the permissive options, miniaudio wins clearly: public domain, single header, and it covers CoreAudio / WASAPI / ALSA / AAudio / AVFoundation — all 5 platforms. It provides device, mixer, decoders (wav/flac/mp3), and basic 3D spatialization. **Its real limit:** no advanced DSP graph, no serious HRTF, no zone-based reverb.

### Decision
**Layers.**

```
audio/graph      ← OUR abstraction (buses, mixers, effects)    ← the only thing the user sees
audio/backend    ← miniaudio (device + mix + basic 3D)
audio/spatial    ← Steam Audio / Resonance (HRTF, occlusion)   ← Phase 5+, optional
```

**No miniaudio type crosses the public API.**

### Consequences
- ✅ The backend is replaceable and extensible without breaking anyone
- ✅ HRTF/occlusion can be added later without a redesign
- ✅ FMOD/Wwise remain viable as third-party plugins
- ❌ One more indirection layer

---

## ADR-007 — Scripting: TypeScript on quickjs-ng

**Status:** Accepted · **Updated July 2026 (quickjs-ng; per-project language)**

### Context
The request was TypeScript support. The original proposal was Luau. (*Luau is a Lua 5.1 derivative by Roblox — Lua syntax with optional gradual typing.*)

### Options

| Criterion | TypeScript / quickjs-ng | Luau | C# / .NET |
|---|---|---|---|
| Author productivity | **Immediate** — it is the developer's language | Learning curve | Medium |
| Tooling (LSP, VSCode, autocomplete) | **Free, mature** | Must be built | Excellent |
| Typing | Strong, structural | Gradual, optional | Strong, nominal |
| Differential value to the community | **High** — neither Godot nor Unity give first-class TS | Low | None (that is Unity) |
| Pure-compute performance | Lower | Higher | Higher |
| Embedding complexity | Low | Low | **High** |
| iOS (forbids JIT) | ✅ quickjs-ng is an interpreter | ✅ | ⚠️ Requires AOT (IL2CPP-style) |

### Decision
**TypeScript on quickjs-ng.**

**quickjs-ng, not Bellard's original** (decided July 2026): quickjs-ng is the better-maintained fork — releases roughly every 2 months, 40+ contributors, tested across 50+ OS/build/sanitizer configs, and a **CMake build with proper Windows support**. That build/maintenance story is decisive for a solo 3-OS CMake+vcpkg project with sanitizers in CI. It is a **JIT-less interpreter** — exactly what iOS requires.

### Pipeline

```
user writes  Player.ts
        ↓  esbuild / swc  (on import, in the editor)
      Player.js  (+ source map)
        ↓  quickjs-ng
     bytecode  →  baked into the final build's .pak
```

The engine API's `.d.ts` files are **generated by `reflect-gen`** (ADR-004) → full VSCode autocomplete without writing a single type by hand.

### Honest trade-off
quickjs-ng is slower than LuaJIT or Luau in pure compute. **It does not matter:** gameplay logic is not the bottleneck — meshes, physics, and rendering live in C++. Same bet Unity makes with C#. **If a script becomes the bottleneck, the answer is to move it to a C++ ECS system, not to change languages.**

### Consequences
- ✅ Immediate productivity, free tooling, strong typing
- ✅ Real, differential contribution to open-source gamedev
- ❌ Scripting performance below JIT alternatives
- ⚠️ **One scripting language for TS projects.** The bindings layer is designed **generically** (reflection-fed) → Luau, C#, or Rust remain community plugins
- ➡️ C++ is also a first-class authoring option, but as a *per-project choice*, not combined with TS — see ADR-008

---

## ADR-008 — Per-project language choice, and the two export models

**Status:** Accepted · **Date:** July 2026 (new)

### Context
The engine must support authoring gameplay in **TypeScript or C++**, chosen per project and **never combined** in one project. This choice has a consequence the export model must account for.

### The consequence
The export design (see [03 — Architecture](./03-architecture.md#the-export-models)) — *"the engine does NOT compile the user's game; the runtime is precompiled per platform, export = pack cooked assets next to it"* — only works when gameplay is **not native code**. That is the Godot model, and it is why export is instant and needs no Xcode/MSVC/NDK.

The moment a project's gameplay is **C++**, that logic *is* native code and must be compiled + linked per target. That is the Unreal model.

| | **TypeScript project** | **C++ project** |
|---|---|---|
| Gameplay form | TS → JS → quickjs-ng bytecode, baked into `.pak` | Native code, linked into the binary |
| Export | Instant, all 5 platforms from one machine | A native build **per platform** (local for your OS; others via CI) |
| User needs toolchains? | No | Yes (or CI does it) |

### Decision
**Support both, but sequence them.** C++ authoring comes essentially for free and early — the engine's own sample games *are* C++ programs linking the engine from Phase 1 (this is the dogfooding path). The **TypeScript scripting layer is the larger dedicated build** (Phase 4). The **productized "pick a language, then export"** experience, with both export pipelines, consolidates in Phase 5.

The language is chosen at **project creation** and is a fixed project setting.

### Consequences
- ✅ C++ authors get native performance and full engine access; TS authors get instant multi-platform export and free tooling
- ✅ The engine is dogfooded in C++ from day 1
- ❌ Two export pipelines to build and maintain (more surface than either alone)
- ⚠️ A C++ project cannot be exported "instantly to 5 platforms from a Mac" the way a TS project can — it needs per-platform compilation (local or CI)
