# Phase 7 — 2D Support · est. 2–3 mo

> **Goal:** the "2D and 3D" capability — deliberately after the 3D foundation is proven. Box2D v3 behind the engine's own physics API, sprite rendering, real text (formalizing the Phase 5 HUD hack), and the editor affordances 2D work needs.
>
> **Gate:** A working 2D scene: Box2D physics + sprites + text
>
> **Phase non-goals:** no tile-editor authoring suite (import + render is enough for v1); no 2D lighting/shaders beyond sprite tinting; no physics interop between Jolt and Box2D worlds (a scene is 2D or 3D).
>
> **Gate artifact:** the 2D sample committed under `/samples/phase-7-2d/`.

---

## Epic 7.1 — Box2D v3 wrapper · physics

**Goal:** 2D physics with the same discipline as 3D: engine-owned API, no Box2D type public (boundary rule).
**Definition of Done:** 2D bodies simulate on a fixed timestep through reflected components only.

### 7.1.1 2D physics API + components · P0 · L · depends: 1.3.2
**Goal:** wrap Box2D v3 the way Jolt is wrapped.
**Deliverable:** reflected `RigidBody2D`/`Collider2D` components; fixed-timestep stepping; no `b2*` type in public headers.
Subtasks:
- World lifecycle + fixed-timestep integration
- `RigidBody2D` / `Collider2D` (box/circle) components (reflected)
- Boundary sweep: no Box2D types public

---

## Epic 7.2 — 2D rendering · render

**Goal:** efficient sprite scenes: batching, atlases, tilemaps, an orthographic camera with sane sorting.
**Definition of Done:** thousands of sprites render in few draw calls, sorted predictably, from an ortho camera.

### 7.2.1 Sprite batching + atlases · P0 · L · depends: 3.3.2
**Goal:** the quad batcher every 2D engine lives on.
**Deliverable:** batched sprite renderer with texture-atlas support and a reflected `SpriteRenderer` component.
Subtasks:
- Quad batcher (dynamic vertex buffer, texture binning)
- Atlas support; `SpriteRenderer` component (reflected)

### 7.2.2 Tilemaps · P1 · M · depends: 7.2.1
**Goal:** big 2D worlds without per-tile entities.
**Deliverable:** tile layers rendered chunked; import format decided at task time (simple JSON or a Tiled subset — recorded in docs/08 when chosen).
Subtasks:
- Tilemap asset + chunked rendering
- Import path (JSON/Tiled subset — decide + record)

### 7.2.3 2D camera & sorting layers · P0 · M · depends: 7.2.1
**Goal:** predictable 2D compositing.
**Deliverable:** orthographic camera mode; sorting layers + order-in-layer on sprites.
Subtasks:
- Ortho camera (pixels-per-unit); layer/order sorting keys

---

## Epic 7.3 — Text · render

**Goal:** real text as a first-class scene feature — the msdf pipeline (crisp at any scale), formalizing what 5.5.3 hacked in for the game.
**Definition of Done:** a `TextRenderer` component renders font assets sharply at arbitrary scales.

### 7.3.1 msdf text system · P0 · M · depends: 7.2.1, 5.5.3
**Goal:** promote the Phase 5 quad-text path into the real system.
**Deliverable:** font assets baked via msdf-atlas-gen in the import pipeline; reflected `TextRenderer` component; the 5.5.3 internal path replaced by it.
Subtasks:
- Font asset import (msdf-atlas-gen in the editor pipeline)
- `TextRenderer` component (reflected); glyph layout basics
- Replace/retire the 5.5.3 internal path

---

## Epic 7.4 — 2D editor affordances · editor

**Goal:** editing 2D scenes without fighting a 3D-first editor.
**Definition of Done:** a 2D scene is comfortably editable: ortho view, grid, snapping, sprite-friendly gizmos.

### 7.4.1 2D view mode + grid/snapping · P1 · M · depends: 7.2.3, 2.3.3
**Goal:** the handful of editor behaviors 2D demands.
**Deliverable:** 2D view toggle (ortho, Z-locked), pixel grid + snapping, rect-style gizmos for sprites.
Subtasks:
- 2D view toggle in the viewport; grid + snap settings
- Sprite rect gizmo behavior

---

## Epic 7.5 — 2D sample · samples

**Goal:** the gate artifact: a tiny 2D scene proving physics + sprites + text together.
**Definition of Done:** the phase gate validated.

### 7.5.1 Tiny 2D sample scene · P0 · M · depends: 7.1.1, 7.2.1, 7.3.1
**Goal:** integrate everything the phase built.
**Deliverable:** a small playable 2D scene (e.g. physics platformer screen) under `/samples/phase-7-2d/`.
Subtasks:
- Build the scene (bodies, sprites, text HUD)
- **Validate the gate**

---

**Phase gate:** A working 2D scene: Box2D physics + sprites + text.
