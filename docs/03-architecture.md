# 03 — Architecture

---

## The golden rule

> **The editor depends on the engine. The engine NEVER depends on the editor.**

This is what makes it possible for the runtime to go to 5 platforms and the editor only to 3. If it breaks even once, the exporter turns into hell.

**It is the rule most hobby engines break.**

### How it is verified automatically
A CI test that fails if any `#include` under `/engine` or `/runtime` points to `/editor`, or if the runtime binary links ImGui, Assimp, or libclang.

---

## Layers

```
┌───────────────────────────────────────────────────────────┐
│  editor/          (3 platforms: macOS, Windows, Linux)    │
│  ImGui · panels · gizmos · importers · exporter           │
└─────────────────────────┬─────────────────────────────────┘
                          │  depends on ↓  (never the reverse)
┌─────────────────────────┴─────────────────────────────────┐
│  engine/          (5 platforms)                           │
│                                                            │
│  script/    quickjs-ng · bindings · hot reload            │
│  assets/    AssetDatabase · GUIDs · cache                 │
│  scene/     EnTT · transforms · cameras · lights          │
│  render/    render graph · PBR · shadows · culling        │
│  physics/   Jolt wrapper                                  │
│  audio/     graph (public) → miniaudio (private)          │
│  reflect/   entt::meta runtime                            │
│  rhi/       abstraction over SDL_GPU                      │
│  platform/  SDL3 wrapper                                  │
│  core/      handles · math · jobs · log · VFS · time      │
└───────────────────────────────────────────────────────────┘
                          ↑
┌─────────────────────────┴─────────────────────────────────┐
│  runtime/         (5 platforms)                           │
│  game loop · .pak loading · entry points                  │
└───────────────────────────────────────────────────────────┘
```

**Dependency rule:** a layer may only depend on the layers **below** it. `core` depends on nothing.

---

## Repository layout

```
/engine
  /core        memory, handles, jobs, log, math (own types), VFS, time
  /platform    SDL3 wrapper: window, input, audio device
  /rhi         abstraction over SDL_GPU
  /render      render graph, PBR, shadows, post, culling
  /scene       EnTT world, transform hierarchy, cameras, lights
  /physics     Jolt wrapper
  /audio       graph (public) → miniaudio backend (private)
  /assets      AssetDatabase, GUIDs, cache, loaders
  /script      quickjs-ng, bindings, hot reload
  /reflect     entt::meta runtime (the GENERATOR lives in /tools)

/runtime       game loop, .pak loading, per-platform entry points        [5 platforms]

/editor        ImGui, panels, undo/redo, gizmos, IMPORTERS, exporter     [3 platforms]

/tools
  /reflect-gen   libclang → meta registrations + bindings + .d.ts
  /shaderc       HLSL → DXIL/MSL/SPIR-V (SDL_shadercross wrapper)
  /cooker        assets → per-platform binary
  /packager      .pak + runtime → final build

/samples       validation games for each phase
/tests
/docs          ← it is open source: docs are part of the product
/third_party   (or vcpkg manifest)
```

### Where each dependency lives

| Dependency | `/engine` | `/runtime` | `/editor` | `/tools` |
|---|:---:|:---:|:---:|:---:|
| SDL3, EnTT, Jolt, miniaudio, quickjs-ng, GLM | ✅ | ✅ | ✅ | |
| Dear ImGui, ImGuizmo | ❌ | ❌ | ✅ | |
| Assimp, ufbx, tinyobjloader, stb_image | ❌ | ❌ | ✅ | ✅ |
| libclang | ❌ | ❌ | ❌ | ✅ |
| esbuild / swc | ❌ | ❌ | ✅ | ✅ |
| Tracy | ⚠️ dev builds | ⚠️ dev builds | ⚠️ | |

**If a dependency from the "❌ / ✅ editor" row ends up linked into the runtime, it is an architecture bug, not a pending optimization.**

---

## Handles, not pointers

Mitigation #1 of ADR-001. Applies to **everything**: entities, textures, meshes, materials, sounds, scripts.

```cpp
template <typename Tag>
struct Handle {
    uint32_t index      = 0;
    uint32_t generation = 0;   // incremented when the slot is freed

    bool operator==(const Handle&) const = default;
    bool valid() const { return generation != 0; }
};

using TextureHandle = Handle<struct TextureTag>;
using MeshHandle    = Handle<struct MeshTag>;
```

**Why:**
- **Eliminates use-after-free.** If the resource was freed, the generation no longer matches → `get()` returns `nullptr` instead of corrupting memory
- **Trivial serialization.** A handle is a `uint64`. No pointers to fix up on load
- **Cache-friendly.** Resources live in contiguous arrays, not scattered on the heap

Implemented at task 0.2.1: `engine::Handle<Tag>` (`engine/core/include/aero/core/handle.hpp`) and the generational pool that mints/validates it, `engine::SlotMap<T, Tag>` (`engine/core/include/aero/core/slot_map.hpp`).

---

## Asset flow

Source assets live inside a **project** — a folder whose root is marked by `project.json` (created by epic 2.6, Phase 2).

```
Source (.blend / .fbx / .obj / .png / .wav / .ts)
   ← lives in the user's project, NEVER distributed
        │
        │  IMPORTER  (editor)      ← Assimp / ufbx / Blender CLI live HERE
        ▼
glTF + .meta (GUID, import settings)
   ← the .meta goes to git; it is what keeps the GUID stable across machines
        │
        │  COOKER  (per platform)
        ▼
Cooked binary
   · textures → ASTC/ETC2 (mobile), BCn (desktop)
   · meshes   → GPU-ready buffers
   · scripts  → quickjs-ng bytecode
   · shaders  → DXIL / MSL / SPIR-V
        │
        │  PACKAGER
        ▼
game.pak  +  precompiled runtime  =  final build
```

---

## The export models

There are **two** export pipelines, one per project language (see [ADR-008](./02-adrs.md#adr-008--per-project-language-choice-and-the-two-export-models)).

### TypeScript project — the Godot model (instant)

**The engine does NOT compile the game.** The runtime is compiled **once per platform** (in CI) and stored as a binary. Exporting = packaging the cooked assets (including the TS bytecode) next to that precompiled runtime.

This is why export is instant instead of taking 20 minutes, and why the **user does not need Xcode, MSVC, or the Android NDK installed** to export — only the engine author does, to build the runtimes.

### C++ project — the Unreal model (compile per platform)

The gameplay is native code, so it must be **compiled and linked per target platform**. The user builds locally for their own OS; other platforms are produced via CI. This is the price of native performance and full engine access.

---

## The four consumers of reflection

The diagram that justifies ADR-004. One annotation feeds four systems:

```
              struct [[engine::component]] Transform { ... }
                              │
                    tools/reflect-gen (libclang)
                              │
        ┌─────────────┬───────┴───────┬──────────────┐
        ▼             ▼               ▼              ▼
   entt::meta   serialization     bindings        .d.ts
   (runtime)    (JSON + bin)     (quickjs-ng)   (autocomplete)
        │             │               │              │
        ▼             ▼               ▼              ▼
    INSPECTOR    SCENES ON      SCRIPT API        VSCODE
    (editor)      DISK          in TypeScript
```

Write the component **once**. It appears in the inspector, saves to disk, is scriptable from TypeScript, and VSCode autocompletes it.

If this is designed wrong, half the engine has to be rewritten. That is why it is in Phase 1.
