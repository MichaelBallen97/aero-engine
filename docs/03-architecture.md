# 03 — Architecture

---

## The golden rule

> **The editor depends on the engine. The engine NEVER depends on the editor.**

This is what makes it possible for the runtime to go to 5 platforms and the editor only to 3. If it breaks even once, the exporter turns into hell.

**It is the rule most hobby engines break.**

### How it is verified automatically
A CI test that fails if any `#include` under `/engine` or `/runtime` points to `/editor`, **if any CMake target under `/engine` or `/runtime` transitively links an `/editor` target or carries `/editor` on its include path,** or if the runtime binary links ImGui, Assimp, or libclang.

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
│  assets/    AssetDatabase · import cache · loaders        │
│  scene/     EnTT · transforms · cameras · lights          │
│  scene_audio/ World -> audio bridge (sees both)           │
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
  /core        memory, handles, guid, jobs, log, math (own types), VFS, time
  /platform    SDL3 wrapper: window, input, audio device
  /rhi         abstraction over SDL_GPU
  /render      render graph, PBR, shadows, post, culling
  /scene       EnTT world, transform hierarchy, cameras, lights
  /physics     Jolt wrapper
  /audio       graph (public) → miniaudio backend (private)
               (opened at 3.7.1 with the runtime clip; 3.7.2 added the spatializer, the
               lock-free mixer and AudioSystem. It links `aero::core`, `aero::assets` and
               NO VCPKG PACKAGE AT ALL — but the "stray `#include <miniaudio.h>` is a hard
               compile error" that follows from it is a **profiling-OFF** property, measured
               at 3.7.3: `aero::profiling` is PRIVATE here and carries `Tracy::TracyClient`
               when `AERO_ENABLE_PROFILING=ON`, so in the `*-release` presets vcpkg's shared
               include root IS on this target's own compile line and the identical stray
               include compiles clean. Enforcement is therefore in two halves, both from
               3.7.3: `.github/scripts/check-audio-boundary.sh` reddens it textually in every
               configuration before any compiler runs, and `tests/audio_boundary_probe.cpp`
               — which links `aero::audio` alone, hence no profiling — is the only
               compile-time check that survives Release)
  /scene_audio the World → audio bridge (task 3.7.2): the only code in the tree that sees
               both `scene` and `audio`, sitting above both — the `scene_render` shape one
               layer over
  /assets      AssetDatabase, import cache, loaders
               (opened at 3.3.1: the cooked-asset formats -- the `.aeromesh` container and its
               parser, plus the mesh cook. core-only: it links `aero::core` and `aero::profiling`
               and no vcpkg package at all)
               (the `Guid` and `ContentHash` value types live in /engine/core beside Handle --
               zero-dependency primitives that scene, render, script, runtime and /tools all
               need; tasks 3.1.1 D1 and 3.1.2 D1)
  /script      quickjs-ng, bindings, hot reload
  /reflect     entt::meta runtime (the GENERATOR lives in /tools)

/runtime       game loop, .pak loading, per-platform entry points        [5 platforms]

/editor        ImGui, panels, undo/redo, gizmos, IMPORTERS, exporter     [3 platforms]

/tools
  /reflect-gen   libclang → meta registrations + bindings + .d.ts
  /shaderc       HLSL → DXIL/MSL/SPIR-V (SDL_shadercross wrapper)
  /cooker        assets → per-platform binary
                 (opened at 3.3.1: `aero_cooker mesh`, source model → `.aeromesh`)
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

`tools/cooker` (task 3.3.1) links `aero::editor_core` for the importers, which transitively puts
ImGui, SDL3 and the four importer libraries on its link line as `$<LINK_ONLY:>` archives. It
initializes none of them, opens no window and creates no GPU device. **The row above records
authorized *use*, not the transitive link graph**; the fix if that ever becomes a problem is splitting
the importer translation units into an ImGui-free target, a self-contained refactor that changes no
consumer. `tools/` sits outside the golden rule on both halves — `check-golden-rule.sh` scans
`engine` and `runtime`, and `aero_assert_golden_rule`'s `CONSUMER_DIRS` is the same pair — so this is
legal by construction rather than an exception carved for one task.

---

### The scene layer (task 1.3.1)

`engine::World` wraps an EnTT registry; no entt type crosses a public scene header (project rule #3), enforced by `.github/scripts/check-scene-boundary.sh` (the textual half, scanning every public engine header) plus `tests/scene_boundary_probe.cpp` (the compile-time half, linking `aero::scene` alone). `engine::Entity` is `Handle<EntityTag>` over a **64-bit** ECS identifier — 32-bit index + 32-bit generation, so staleness detection is `SlotMap`-grade (the default 32-bit ECS identifier would give 12-bit versions and alias a stale handle after 4096 recycles of one slot). The public component API is the **type-erased façade**: templates whose bodies `static_cast` over six non-template primitives (`addRaw`/`getRaw`×2/`hasRaw`/`removeRaw`/`countRaw`), because creating a typed storage is a template in EnTT and cannot be instantiated from an entt-free header. The one operation that genuinely needs entt on the creation side lives behind the **registration seam**, `engine::scene::internal::registerComponent<T>(world, name)` in `engine/scene/internal/.../world_access.hpp`, shipped through the `aero::scene_internal` INTERFACE target (the 0.4.2 `aero::platform_internal` pattern). Registration is **per-World**, and the name is the durable identity that `docs/09`'s component keys resolve through (`World::findComponentType`). Task 1.3.2 authors the first built-in component (`engine::Transform`) on top of this layer; task 1.4.2 is the loader that consumes `addRaw` + `findComponentType` to bring a JSON scene to life.

- `engine::Transform` (task 1.3.2) is the engine's **first reflected component** — `position`/`rotation`/`scale`, annotated with `AERO_COMPONENT` from the new `<aero/reflect/annotations.hpp>` (promoted out of the test fixtures), registered as `"engine::Transform"` in **every World by construction** through `engine::scene::detail::registerBuiltinComponents`, which `World`'s constructor calls.
- The **parent/child hierarchy is entity-level World state**, not component data — mirroring `docs/09`'s entity-level `parent` key, which any entity may carry with or without components. `setParent`/`parent`/`childCount`/`eachChild` enforce a **forest** (cycles rejected at the API), and `destroy()` destroys the **whole subtree**, which is what makes "a live entity's parent is always live" a structural invariant rather than a convention.
- `worldMatrix(world, entity)` composes on demand, iteratively, up the parent chain (`world = M_root · … · M_parent · M_local`); an entity or ancestor without a `Transform` contributes identity. **No caching** — the deferral is recorded in `docs/08`.
- The reflection claim is proven mechanically over the real header (a `--components` process case plus generated `entt::meta` and JSON artifacts compiled into the two gated test targets), while the engine itself compiles no generated code.
- `engine::MeshRenderer` (task 1.4.1) is the 5th built-in — `{uint32 primitive; Vec3 color}`, the reflected "this entity draws a primitive mesh" component, registered alongside `Transform`/`Camera`/`DirectionalLight`/`PointLight`. `engine::AnimationPlayer` (3.5.2) is the 6th; `engine::AudioSource` and `engine::AudioListener` (3.7.2) are the **7th and 8th**, and like a directional light stores no direction, the listener stores neither position nor orientation — both come from the entity's `Transform`.

### Scene → render bridge (`scene_render`, task 1.4.1)

`engine/scene_render` is a thin composition module that sits *above* both `scene` and `render` (the only code in the tree that sees both) — it turns a `World` into a `render::RenderView` and submits it each frame, which is what keeps `render` scene-free (it never depends on `scene`, upholding the layer rule) and `scene` GPU-free. Its pure half, `scene_render::buildRenderView(World&, RenderViewScratch&, rhi::Extent2D)`, walks the renderable entities (`each<Transform, MeshRenderer>`), resolves the lowest-index `Camera` and lights, and returns a flat `RenderView` — no GPU, tier-0 testable. Its facade, `scene_render::SceneRenderer`, owns a `render::ForwardRenderer` (the scene-free lit-primitive draw engine `render` gained in the same task) and drives `render(World&, Frame&)` once per frame. This is where the Phase-2 editor viewport and Phase-5 runtime will drive rendering.

`engine::render::RenderTarget` (task 2.2.3) is the **offscreen** sibling of `render::Renderer` — it owns a sampleable colour texture (plus an auto-picked depth target) and drives the same `beginFrame → record → endFrame` cycle into it instead of a swapchain, so every `render::Frame` consumer works unchanged. It is what the editor viewport renders into, and what Phase 3 shadow maps and Phase 8 post-processing will reuse.

### Scene → audio bridge (`scene_audio`, task 3.7.2)

`engine/scene_audio` is the same shape as `scene_render`, one layer over: a thin composition module sitting *above* both `scene` and `audio` — the only code in the tree that sees both — which is what keeps `audio` scene-free and `scene` audio-free. Its pure half, `scene_audio::buildAudioView(World&, AudioViewScratch&)`, walks `each<Transform, AudioListener>` and `each<Transform, AudioSource>`, resolves the lowest-index listener into an orthonormal world-space basis (normalising `worldMatrix`'s columns, so a scaled listener entity behaves identically to an unscaled one) and every *playing* source into a world-space `audio::VoiceParams`, sanitised on the way in. It never touches an `AudioSystem`, never resolves a `Guid` and never logs — its whole output is data, which is what makes it tier-0 testable **with no `AudioSystem` in existence**. Its facade, `scene_audio::SceneAudio`, owns the entity↔voice bindings and the parameter coalescing that keeps a static scene at zero commands per frame, and owns **no** `AudioSystem`, so a caller can drive several worlds through one system.

**Why it is a separate target rather than a function in `engine/audio`.** Folding the walk into `audio` would give `aero_audio` a `PUBLIC aero::scene`, so **every binary that links audio would drag EnTT in** — including the Phase 5 runtime, which per [ADR-008](./02-adrs.md#adr-008--per-project-language-choice-and-the-two-export-models) is handed a `.pak` and may reasonably want to play a sound without instantiating a `World` at all. Wiring it only in the sample was the other rejected shape: the *engine* would then ship no `World` → audio path, and the editor's future play mode and the runtime would each rewrite it. It links `PUBLIC aero::scene aero::audio` / `PRIVATE aero::profiling` and never `aero::scene_internal`, which carries `EnTT::EnTT` as an INTERFACE by design.

### Scene serialization bridge (`scene_serialize`, task 1.4.2)

`engine/scene_serialize` sits *above* both `scene` and `reflect` — it owns the runtime name→type dispatch that turns a parsed `SceneDocument`'s opaque, string-keyed component payloads into live typed components on a `World`, and the inverse (`loadScene`/`loadSceneText`, `saveWorld`/`saveWorldText`). It is the **first production consumer** of `aero_reflect_generate_json` (until this task, only test targets generated component serializers): the 5 built-ins' `aeroReadJson`/`aeroWriteJson` compile straight into the shipping `aero::scene_serialize` library, dispatched through one internal table keyed by the component's registered name. Like `engine/rhi`/`engine/render`/`engine/reflect`, no third-party type crosses its public header (only `World`, `SceneDocument`, `JsonValue`, `SceneError`) — entt stays private inside `aero::scene`, so the boundary rule holds by construction with no new grep guard or compile-time probe. Gated on `AERO_REFLECT_TOOLS` (its generated serializers are never committed, so there is nothing to build with the tool off) — the same escape hatch `AERO_SHADER_TOOLS` gives `phase-0-cube`. `samples/phase-1-scene` is the first consumer: it loads a committed `scene.json` from disk through the VFS, instantiates a `World`, and renders it every frame with `scene_render::SceneRenderer` — the Phase 1 gate artifact.

---

### The editor layer opens (tasks 2.1.1–2.1.3)

Epic 2.1 populates `/editor` for the first time: `aero_editor_core` hosts Dear ImGui (docking) directly on `engine::rhi::Device` and `engine::platform::Window`/`Context`, and `aero_editor` is the thin executable. Two non-installed internal seams, cloned from the `aero::platform_internal` pattern, hand the editor the few native handles it cannot synthesize any other way: `aero::rhi_internal` (`NativeDeviceAccessor`, exposing the SDL_GPU device/command-buffer/render-pass as `void*` — never a typed SDL_GPU spelling, which the rhi-boundary guard would reject) and a generic, void-based raw-event observer on `Context` (`RawEventAccessor`), which feeds ImGui's SDL3 backend every event the engine's own pump would otherwise translate-and-discard. Both seams keep the engine naming nothing ImGui — the editor is the only linker of `imgui::imgui`, PRIVATE to `aero_editor_core`, so it never reaches `/engine` or `/runtime` at compile time. The golden-rule CI guard that locks this down structurally is task 2.1.2.

Task 2.1.3 turns that bootstrap into the shell every later panel plugs into: `engine::editor::Panel`
is the polymorphic panel interface, `PanelRegistry` owns the panels and knows their registration
order and visibility, and `EditorApp` owns the ImGui host + registry + frame clock and exposes the
whole editor loop as a callable `tick()` (`run()` is `while (tick()) {}`), which is what makes the
loop drivable from a test. The registry — never the panel — calls `ImGui::Begin`/`End` around
`Panel::onDraw()`, so an unbalanced `End()` is structurally impossible; the first-run and
`View > Reset Layout` dock layout is data-driven from each panel's `defaultDockSlot()`, so
registering a panel in 2.2.x needs no layout-code edit. The three public editor headers
(`panel.hpp`, `panel_registry.hpp`, `editor_app.hpp`) expose engine + std types only — every ImGui
entry point lives in `editor/src/` (`shell_ui.cpp`, `placeholder_panel.cpp`), src-private by
placement.

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
        │  IMPORTER  (editor)      ← fastgltf / ufbx / tinyobjloader / Assimp / Blender CLI
        ▼
canonical ImportedModel (IN MEMORY) + .meta (GUID, import settings)
   ← the .meta goes to git; it is what keeps the GUID stable across machines
   ← created by the editor's AssetDatabase (task 3.1.1); format in docs/09 §5
   ← the importers produce canonical SEMANTICS, not a canonical FILE: nothing writes a glTF
     to disk. The one on-disk intermediate is the .blend path's GLB under Library/ (task 3.2.4)
        │
        │  COOKER  (per platform)
        ▼
Cooked binary
   · textures → ASTC/ETC2 (mobile), BCn (desktop)
   · meshes   → GPU-ready buffers   ← `.aeromesh` v1, docs/09 §9 (task 3.3.1)
   · scripts  → quickjs-ng bytecode
   · shaders  → DXIL / MSL / SPIR-V
        │
        │  PACKAGER
        ▼
game.pak  +  precompiled runtime  =  final build
```

Epic 3.2 deliberately did not build the write-a-glTF-and-read-it-back shape this diagram used to
draw. Round-tripping every FBX through a written glTF would add a serialization step, a second parse,
and a lossy hop for anything the writer did not model — so the importers hand the cooker an
`ImportedModel` in memory instead, and the canonical-format commitment (ADR-003) is honoured in
semantics rather than in bytes on disk.

**There are ultimately two asset databases, and they are worth naming as such so this is not
re-derived in three months.** The editor's (task 3.1.1, source-tree-backed, `.meta`-driven, scans a
project's `assets/` folder and gives every source file a stable GUID) and the runtime's (Phase 5,
`.pak`-backed, reads a cooked, packaged binary). They share the `Guid` value type — `/engine/core` —
and nothing else: different storage, different lifetime, different consumers. The editor's database
also owns a **machine-local, never-committed import cache** at `<projectRoot>/Library/` (task 3.1.2),
which the runtime's has no analogue of — the runtime reads a cooked `.pak`, never a source tree, so it
has nothing to cache an import decision against.

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
