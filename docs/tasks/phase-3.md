# Phase 3 — Asset Pipeline & 3D Content · est. 3–5 mo

> **Goal:** real content in the engine — models, materials, animation — plus the two audit additions that keep the Phase 5 game credible: rendering essentials (culling, shadows, tonemap) and audible audio.
>
> **Gate:** Drop a rigged glTF/FBX in → PBR materials + shadows + a playing animation + an audible sound
>
> **Phase non-goals:** no scripting (Phase 4), no export (Phase 5), no mobile texture profiles (6.3), no audio buses/effects (6.4). Shadow *quality* (cascades, soft shadows) is Phase 8.
>
> **Note:** largest phase of the project. The 3a/3b split decision (docs/08) is taken at phase start — a natural cut is 3a = 3.1–3.3 (pipeline), 3b = 3.4–3.7 (content & essentials).
>
> **Gate artifact:** the imported-content demo scene is committed under `/samples/phase-3-content/`.

---

## Epic 3.1 — AssetDatabase · assets

**Goal:** every file in the project gets a stable identity (GUID) and an import cache — the foundation the cooker, browser, and hot reload all stand on.
**Definition of Done:** GUIDs survive machine changes (`.meta` committed to git); unchanged assets never re-import.

### 3.1.1 GUIDs + `.meta` files · P0 · M · depends: 2.6.1
**Goal:** stable asset identity across machines and renames (docs/03 asset flow).
**Deliverable:** deterministic GUID generation; versioned `.meta` schema written next to every source asset; committed to git.
Subtasks:
- GUID generation + `.meta` schema (versioned)
- `.meta` lifecycle: create on discovery, preserve on move/rename

### 3.1.2 Import cache & dependency tracking · P0 · M · depends: 3.1.1
**Goal:** import once, reuse until content changes.
**Deliverable:** content-hash-based cache with invalidation; only changed assets re-import.
Subtasks:
- Content-hash cache keyed by source + import settings
- Dependency tracking (e.g. material → texture) and cascade invalidation

### 3.1.3 Asset browser v1 · P1 · M · depends: 3.1.1, 2.2.4
**Goal:** upgrade the 2.2.4 stub into the real thing.
**Deliverable:** browser with thumbnails, type icons, drag-into-scene, and search.
Subtasks:
- Thumbnails + type icons
- Drag into scene/hierarchy; search/filter

### 3.1.4 Hot-reload file watcher · P2 · M · depends: 3.1.2
**Goal:** edit a texture in Photoshop, see it update in the viewport (audit D12; deeper hot reload → v2).
**Deliverable:** filesystem watch → re-import → live refresh of loaded assets.
Subtasks:
- FS watcher on the project's `assets/`
- Re-import + in-place refresh of GPU resources

---

## Epic 3.2 — Importers · editor

**Goal:** ADR-003 made real: N source formats → one canonical representation (glTF 2.0 semantics), importers strictly editor-side.
**Definition of Done:** glTF, FBX, OBJ and `.blend` files land in the project as canonical assets; Assimp formats work editor-only.

### 3.2.1 glTF import (fastgltf) · P0 · L · depends: 3.1.1
**Goal:** the canonical format imports first and best.
**Deliverable:** meshes, materials, textures, skeletons, and animation clips imported from `.gltf`/`.glb`.
Subtasks:
- Meshes + hierarchy; materials + textures
- Skeletons (joints, bind poses); animation clips

### 3.2.2 FBX import (ufbx) · P0 · M · depends: 3.2.1
**Goal:** the industry's lingua franca, via ufbx (lighter and more correct than Assimp for FBX).
**Deliverable:** same coverage as 3.2.1, mapped onto the canonical representation.
Subtasks:
- Static + skinned meshes, materials, clips via ufbx → canonical

### 3.2.3 OBJ import (tinyobjloader) · P1 · S · depends: 3.2.1
**Goal:** static-mesh workhorse; nothing more (OBJ has nothing more).
**Deliverable:** static meshes import with basic materials.
Subtasks:
- Static mesh + mtl basic materials

### 3.2.4 Blender CLI detection + `.blend` import · P1 · M · depends: 3.2.1
**Goal:** the Unity model (ADR-003): invoke Blender headless, never parse `.blend`.
**Deliverable:** auto-detected (or configured) Blender binary; background export to glTF; friendly failure when Blender is absent.
Subtasks:
- Locate Blender (auto-detect + settings override)
- `blender -b <file> --python-expr "export glTF"` pipeline
- Clear error UX when Blender is missing

### 3.2.5 Assimp fallback importers (DAE/PLY/STL) · P2 · M · depends: 3.2.1
**Goal:** breadth via Assimp — editor-linked only (dependency-placement invariant).
**Deliverable:** DAE/PLY/STL import through Assimp in the editor target.
Subtasks:
- Assimp import path (editor target only) for DAE/PLY/STL

---

## Epic 3.3 — Cooker v0 · tools

**Goal:** source assets → per-platform runtime binaries; deterministic by construction.
**Definition of Done:** meshes and textures cook to GPU-ready formats; cooking the same input twice is byte-identical (CI-proven).

### 3.3.1 Mesh cook → GPU buffers · P0 · M · depends: 3.2.1
**Goal:** runtime never parses glTF — it memory-maps cooked buffers.
**Deliverable:** interleaved/indexed vertex data plus computed AABBs (consumed by frustum culling, 3.6.1).
Subtasks:
- Interleave + index; compute AABBs
- Cooked mesh container format (versioned)

### 3.3.2 Texture cook → KTX2/Basis · P0 · M · depends: 3.1.2
**Goal:** GPU-compressed textures, desktop profile first (mobile = 6.3).
**Deliverable:** BCn-compressed KTX2 with mips and correct sRGB handling.
Subtasks:
- stb_image decode → Basis/KTX2 encode (BCn), mip chain
- sRGB/linear flags honored end-to-end

### 3.3.3 Cook determinism golden test · P1 · S · depends: 3.3.1, 3.3.2
**Goal:** determinism is a test, not a hope (docs/04 golden tests).
**Deliverable:** CI job cooking a fixture twice and byte-comparing.
Subtasks:
- Fixture assets; double-cook + byte-compare in CI

---

## Epic 3.4 — PBR materials · render

**Goal:** imported models look like their source: metallic-roughness PBR.
**Definition of Done:** the glTF PBR parameter set renders correctly; materials are editable assets.

### 3.4.1 Material asset + PBR shader · P0 · L · depends: 3.3.2, 0.4.4
**Goal:** the core lit shader and the material asset that feeds it.
**Deliverable:** metallic-roughness shading with normal/occlusion/emissive maps, written in HLSL through `tools/shaderc`.
Subtasks:
- Material asset format (versioned) bound to shader params
- PBR HLSL (metallic-roughness, normal, occlusion, emissive)
- Per-material uniform/texture binding in the renderer

### 3.4.2 Material inspector editing · P1 · M · depends: 3.4.1, 2.2.2
**Goal:** materials are edited, not hand-authored JSON.
**Deliverable:** reflected material parameters editable in the inspector with live viewport preview.
Subtasks:
- Reflect material params; inspector editing; live preview

---

## Epic 3.5 — Skeletal animation · render

**Goal:** rigged characters move: skinning + clip playback (the graph editor stays v2).
**Definition of Done:** a rigged, skinned model plays an imported clip in the viewport.

### 3.5.1 Skeleton & GPU skinning · P0 · L · depends: 3.2.1, 3.4.1
**Goal:** the runtime skeleton and the skinning path.
**Deliverable:** joint hierarchy + bind poses; matrix-palette GPU skinning.
Subtasks:
- Runtime skeleton (joints, bind poses)
- Matrix palette + skinning in the vertex shader

### 3.5.2 Clip playback · P0 · M · depends: 3.5.1
**Goal:** sample and play imported clips.
**Deliverable:** clip sampler with looping and speed; reflected `AnimationPlayer` component.
Subtasks:
- Clip sampling (interpolation), looping, speed
- `AnimationPlayer` component (reflected) + inspector control

---

## Epic 3.6 — Rendering essentials · render

**Goal:** *(added by the July 2026 audit, D3)* the baseline visual features doc/06 already promises for v1 — culling, shadows, tonemap — pulled forward so the Phase 5 game doesn't ship flat and shadowless. Phase 8.2 builds the quality pass on top.
**Definition of Done:** the demo scene is frustum-culled, shadowed by its directional light, and tonemapped.

### 3.6.1 Frustum culling · P1 · M · depends: 3.3.1
**Goal:** don't draw what the camera can't see (uses the cooked AABBs).
**Deliverable:** camera-frustum vs AABB culling with Tracy counters proving the win.
Subtasks:
- Frustum extraction + AABB test in the submit path
- Tracy counters: culled vs drawn

### 3.6.2 Directional shadow map · P1 · L · depends: 3.4.1
**Goal:** one good shadow — single-cascade directional shadow mapping (cascades/soft = 8.2.1).
**Deliverable:** depth-pass shadow map with PCF filtering applied in the PBR shader.
Subtasks:
- Depth pass from the directional light; shadow sampling + PCF
- Bias/peter-panning controls exposed on the Light component

### 3.6.3 Tonemap/gamma pass · P1 · S · depends: 3.4.1
**Goal:** stop outputting raw linear values; seed the Phase 8 post stack.
**Deliverable:** fullscreen tonemap (Reinhard or ACES-approx) + gamma-correct output.
Subtasks:
- Fullscreen pass scaffold; tonemap + gamma

---

## Epic 3.7 — Audio playback v0 · audio

**Goal:** *(added by the July 2026 audit, D2)* the engine makes sound before Phase 6 — otherwise the Phase 5 game ships silent. Clips as assets, a minimal play API, positioned sound. Buses/mixers/effects stay in 6.4 (ADR-006 layering).
**Definition of Done:** a scene emits audible, positioned sound triggered through the engine API; no miniaudio type is public (guard-enforced).

### 3.7.1 Audio clip assets · P0 · M · depends: 3.1.2, 0.3.3
**Goal:** sounds enter through the same asset pipeline as everything else.
**Deliverable:** wav/flac/mp3 (miniaudio decoders) + ogg (stb_vorbis) imported via the AssetDatabase and loadable at runtime.
Subtasks:
- Clip import (wav/flac/mp3 via miniaudio; ogg via stb_vorbis)
- Clip asset + loader through the VFS

### 3.7.2 Playback API + components · P0 · M · depends: 3.7.1
**Goal:** the public audio surface (ADR-006): engine types only.
**Deliverable:** play/stop/volume/pitch API; reflected `AudioSource`/`AudioListener` components; basic 3D panning via the miniaudio spatializer.
Subtasks:
- `engine::audio` play/stop/volume/pitch
- `AudioSource` / `AudioListener` components (reflected)
- Basic 3D spatialization (position-driven pan/attenuation)

### 3.7.3 Audio-boundary CI guard · P1 · S · depends: 3.7.2
**Goal:** the docs/04 audio guard gets an owner: no miniaudio type in public headers.
**Deliverable:** CI-wired scan, proven against a seeded violation and cleaned.
Subtasks:
- Public-header scan for miniaudio tokens; wire into CI; prove + clean seed

---

**Phase gate:** Drop a rigged glTF/FBX in → PBR materials + shadows + a playing animation + an audible sound.
