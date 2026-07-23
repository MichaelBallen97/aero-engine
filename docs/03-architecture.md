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

### The scene layer (task 1.3.1)

`engine::World` wraps an EnTT registry; no entt type crosses a public scene header (project rule #3), enforced by `.github/scripts/check-scene-boundary.sh` (the textual half, scanning every public engine header) plus `tests/scene_boundary_probe.cpp` (the compile-time half, linking `aero::scene` alone). `engine::Entity` is `Handle<EntityTag>` over a **64-bit** ECS identifier — 32-bit index + 32-bit generation, so staleness detection is `SlotMap`-grade (the default 32-bit ECS identifier would give 12-bit versions and alias a stale handle after 4096 recycles of one slot). The public component API is the **type-erased façade**: templates whose bodies `static_cast` over six non-template primitives (`addRaw`/`getRaw`×2/`hasRaw`/`removeRaw`/`countRaw`), because creating a typed storage is a template in EnTT and cannot be instantiated from an entt-free header. The one operation that genuinely needs entt on the creation side lives behind the **registration seam**, `engine::scene::internal::registerComponent<T>(world, name)` in `engine/scene/internal/.../world_access.hpp`, shipped through the `aero::scene_internal` INTERFACE target (the 0.4.2 `aero::platform_internal` pattern). Registration is **per-World**, and the name is the durable identity that `docs/09`'s component keys resolve through (`World::findComponentType`). Task 1.3.2 authors the first built-in component (`engine::Transform`) on top of this layer; task 1.4.2 is the loader that consumes `addRaw` + `findComponentType` to bring a JSON scene to life.

- `engine::Transform` (task 1.3.2) is the engine's **first reflected component** — `position`/`rotation`/`scale`, annotated with `AERO_COMPONENT` from the new `<aero/reflect/annotations.hpp>` (promoted out of the test fixtures), registered as `"engine::Transform"` in **every World by construction** through `engine::scene::detail::registerBuiltinComponents`, which `World`'s constructor calls.
- The **parent/child hierarchy is entity-level World state**, not component data — mirroring `docs/09`'s entity-level `parent` key, which any entity may carry with or without components. `setParent`/`parent`/`childCount`/`eachChild` enforce a **forest** (cycles rejected at the API), and `destroy()` destroys the **whole subtree**, which is what makes "a live entity's parent is always live" a structural invariant rather than a convention.
- `worldMatrix(world, entity)` composes on demand, iteratively, up the parent chain (`world = M_root · … · M_parent · M_local`); an entity or ancestor without a `Transform` contributes identity. **No caching** — the deferral is recorded in `docs/08`.
- One sentence: the reflection claim is proven mechanically over the real header (a `--components` process case plus generated `entt::meta` and JSON artifacts compiled into the two gated test targets), while the engine itself compiles no generated code.

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
