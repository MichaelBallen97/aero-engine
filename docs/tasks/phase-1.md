# Phase 1 — Reflection, ECS & Serialization · est. 2–3 mo

> **Goal:** build the reflection/serialization spine — the backbone four systems will consume (inspector, scenes on disk, script bindings, `.d.ts`) — *before* anything depends on it the wrong way (ADR-004).
>
> **Gate:** Define a component → save/load a scene as JSON → it renders
>
> **Phase non-goals:** no editor UI, no asset importers, no scripting — JSON scenes rendered with primitive meshes are enough. `reflect-gen` handles the minimal subset only: plain structs + primitives + `Vec3`/`Quat` (expand on demand, per ADR-004).
>
> **Gate artifact:** the scene-loading demo is committed under `/samples/phase-1-scene/`.

---

## Epic 1.1 — `reflect-gen` v0 · reflect

**Goal:** the libclang code generator that turns one `[[engine::component]]` annotation into machine-written registration code — the #2 technical risk (R2), built first on purpose.
**Definition of Done:** annotating a struct and rebuilding produces working `entt::meta` registration with zero hand-written glue, on all 3 OSes.

### 1.1.1 libclang harness · P0 · L · depends: 0.1.4
**Goal:** stand up the `tools/reflect-gen` project and prove libclang can parse engine headers.
**Deliverable:** a CLI that parses a translation unit and walks its AST.
Subtasks:
- `tools/reflect-gen` project; link libclang
- Parse a translation unit; walk the AST

### 1.1.2 Annotation detection · P0 · M · depends: 1.1.1
**Goal:** recognize the engine's annotation vocabulary in the AST.
**Deliverable:** `[[engine::component]]` structs detected with their fields collected (primitives + `Vec3`/`Quat` only).
Subtasks:
- Recognize `[[engine::component]]`; parse attributes
- Collect fields (**primitives + `Vec3`/`Quat` only**)

### 1.1.3 `entt::meta` codegen · P0 · M · depends: 1.1.2
**Goal:** the first of the four reflection consumers: runtime meta registration.
**Deliverable:** generated registration code, compiled into the build, registered at startup.
Subtasks:
- Emit registration code
- Integrate the generated file into the build; register at startup

### 1.1.4 Build-step wiring · P0 · M · depends: 1.1.3
**Goal:** make codegen a first-class build step (docs/04): generate → compile, incrementally.
**Deliverable:** `reflect-gen` runs before the main compile via CMake with a generated-sources directory and sane incremental behavior.
Subtasks:
- `reflect-gen` runs before compile in CMake; generated dir; incremental behavior

---

## Epic 1.2 — Serialization · reflect

**Goal:** reflection consumer #2: generated JSON read/write (binary is the cooker's job, Phase 3+).
**Definition of Done:** any reflected component round-trips component → JSON → component byte-equal, proven by tests.

### 1.2.1 JSON writer (generated) · P0 · M · depends: 1.1.4
**Goal:** machine-written serializers — humans never write field-by-field save code.
**Deliverable:** per-field serializer emission; any reflected component serializes to JSON.
Subtasks:
- Emit per-field serializers; component → JSON

### 1.2.2 JSON reader (generated) · P0 · M · depends: 1.2.1
**Goal:** the read path plus proof of round-trip integrity.
**Deliverable:** JSON → component with a round-trip unit test.
Subtasks:
- JSON → component
- Round-trip unit test

### 1.2.3 Scene serialization format · P1 · S · depends: 1.2.2
**Goal:** define the scene-on-disk schema (git-mergeable text, versioned from day one — docs/04 format policy).
**Deliverable:** entity + components JSON schema with a version field.
Subtasks:
- Entity + components JSON schema; version field

---

## Epic 1.3 — `scene` · scene

**Goal:** the EnTT-backed world: entities through handles, transform hierarchy, first reflected components.
**Definition of Done:** entities create/destroy through engine handles (no `entt` type public), transforms compose hierarchically, `Camera`/`Light` are reflected.

### 1.3.1 EnTT world integration · P0 · M · depends: 0.2.1
**Goal:** wrap the registry behind the engine's own world API (boundary rule).
**Deliverable:** entity create/destroy via handles; no `entt` types in public headers.
Subtasks:
- Registry wrapper (no `entt` types public)
- Entity create/destroy via handles

### 1.3.2 Transform hierarchy · P0 · M · depends: 1.3.1
**Goal:** the first real reflected component and the parent/child model everything else hangs off.
**Deliverable:** `Transform` component with parent/child relationships and world-matrix computation.
Subtasks:
- `Transform` component; parent/child
- World-matrix computation

### 1.3.3 Camera & Light components · P1 · S · depends: 1.3.2
**Goal:** the minimum component set a renderable scene needs.
**Deliverable:** reflected `Camera` (view/proj) and `Light` (directional/point) components.
Subtasks:
- `Camera` (view/proj), `Light` (directional/point) — both reflected

---

## Epic 1.4 — Render-a-scene · render

**Goal:** close the loop: a scene defined in data renders on screen.
**Definition of Done:** the phase gate validated — a JSON scene file loads, instantiates, and draws.

### 1.4.1 Scene → render · P0 · M · depends: 1.3.3, 0.5.2
**Goal:** connect `scene` to `render` v0.
**Deliverable:** renderable entities iterated and submitted to the renderer each frame.
Subtasks:
- Iterate renderable entities; submit to `render` v0

### 1.4.2 Load & draw a JSON scene · P0 · S · depends: 1.4.1, 1.2.3
**Goal:** validate the gate end-to-end.
**Deliverable:** a scene file loads, entities instantiate, the scene renders; committed under `/samples/phase-1-scene/`.
Subtasks:
- Load a scene file; instantiate entities; render
- **Validate the gate**

---

**Phase gate:** Define a component → save/load a scene as JSON → it renders.
