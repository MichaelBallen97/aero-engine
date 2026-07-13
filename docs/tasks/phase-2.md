# Phase 2 — The Editor Shell · est. 2–3 mo

> **Goal:** the first genuinely *usable* tool; validate the golden rule and the reflection → inspector path.
>
> **Gate:** Create a project; create / move / edit / save entities visually
>
> **Phase non-goals:** no asset import yet (the browser is a stub until 3.1); no play mode (Phase 4); no 2D affordances (Phase 7). The editor edits scenes of primitive-mesh entities — that is enough to prove the tool.
>
> **Gate artifact:** a scene built entirely in the editor is committed under `/samples/phase-2-editor-scene/`.

---

## Epic 2.1 — ImGui integration · editor

**Goal:** the editor application exists — Dear ImGui (docking) hosted on the engine's own window/RHI, with the golden rule mechanically enforced from the first commit that links ImGui.
**Definition of Done:** a dockable editor app runs on all 3 OSes; the golden-rule CI guard is live and proven.

### 2.1.1 ImGui + docking into the editor target · P0 · M · depends: 0.5.1
**Goal:** bring ImGui into `/editor` only — the first dependency that must never touch `/engine` or `/runtime`.
**Deliverable:** an editor window with dockable dummy panels, layout persisted across restarts.
Subtasks:
- Add Dear ImGui (docking branch) to the manifest, editor target only
- Wire SDL3 + SDL_GPU ImGui backends
- Layout persistence (`imgui.ini`); HiDPI scaling check on all 3 OSes

### 2.1.2 Golden-rule CI guard · P0 · S · depends: 2.1.1
**Goal:** project rule #1 becomes a build failure instead of a code-review hope.
**Deliverable:** CI job that fails if any `#include` under `/engine` or `/runtime` references `/editor`, proven with a seeded violation and then cleaned.
Subtasks:
- Include-scan script (`/engine` + `/runtime` must never reference `/editor`)
- Wire into CI; prove it fails on a seeded violation, then remove the seed

### 2.1.3 Editor app shell & main loop · P0 · M · depends: 2.1.1
**Goal:** the structural skeleton every panel plugs into.
**Deliverable:** `aero_editor` executable hosting the engine frame loop, with a menu bar and a panel registry; opens and quits cleanly.
Subtasks:
- Editor executable target hosting the engine loop
- Menu bar skeleton (File/Edit/View)
- Panel registry (register/show/hide panels)

---

## Epic 2.2 — Core panels · editor

**Goal:** the panels that make a scene editable: hierarchy, reflection-driven inspector, viewport, log — plus a placeholder asset browser.
**Definition of Done:** a live scene can be inspected and edited end-to-end through panels alone.

### 2.2.1 Hierarchy panel · P0 · M · depends: 2.1.3, 1.3.2
**Goal:** see and shape the entity tree without writing code.
**Deliverable:** tree view with parent/child display; create, delete, duplicate, rename; selection synced with the viewport.
Subtasks:
- Entity tree view honoring the Transform hierarchy
- Create / delete / duplicate / rename entities
- Selection state shared with viewport & inspector

### 2.2.2 Reflection-driven inspector · P0 · L · depends: 2.2.1, 1.1.4
**Goal:** the payoff of ADR-004: *any* `[[engine::component]]` renders an editing UI with zero per-component editor code.
**Deliverable:** inspector that walks `entt::meta` for the selected entity; field editors for float/int/bool/string/`Vec3`/`Quat`/color; `[[engine::range]]` respected; every edit flows through the undo system.
Subtasks:
- Meta-walk of the selected entity's components
- Field editors: float, int, bool, string, `Vec3`, `Quat`, color
- Respect `range` attributes; add/remove component UI
- Route edits through 2.4 commands

### 2.2.3 Viewport panel · P0 · M · depends: 2.1.3, 1.4.1
**Goal:** the scene inside a dockable panel instead of the raw window.
**Deliverable:** scene rendered to texture in a panel, resize-safe.
Subtasks:
- Render-to-texture scene view inside a panel
- Resize handling without artifacts

### 2.2.4 Asset browser stub · P1 · S · depends: 2.1.3
**Goal:** reserve the workflow slot; the real browser arrives with the AssetDatabase (3.1.3).
**Deliverable:** read-only panel listing the project directory's files.
Subtasks:
- Panel listing project files (read-only)

### 2.2.5 Log/console panel · P1 · S · depends: 2.1.3
**Goal:** engine diagnostics visible inside the tool (later: script errors, 4.3.4).
**Deliverable:** log sink → filterable panel with levels.
Subtasks:
- Engine log sink feeding a panel; level filter; clear

---

## Epic 2.3 — Manipulation · editor

**Goal:** direct manipulation: fly a camera, pick an entity, drag a gizmo.
**Definition of Done:** entities are selected and transformed with the mouse, undoably.

### 2.3.1 Editor camera · P0 · M · depends: 2.2.3
**Goal:** comfortable navigation — the editor is unusable without it.
**Deliverable:** orbit/pan/zoom/fly controls plus focus-selected (F).
Subtasks:
- Orbit / pan / zoom / fly controls
- Focus selected (F)

### 2.3.2 Selection & picking · P0 · M · depends: 2.3.1
**Goal:** click a thing to select the thing.
**Deliverable:** ray-vs-AABB picking with visible selection highlight.
Subtasks:
- Click-pick via ray vs entity AABBs
- Selection highlight in viewport

### 2.3.3 ImGuizmo transform gizmos · P0 · M · depends: 2.3.2, 2.4.1
**Goal:** the canonical translate/rotate/scale workflow.
**Deliverable:** ImGuizmo gizmos in local/world space with snapping, writing through undo commands.
Subtasks:
- Translate / rotate / scale gizmos (ImGuizmo, editor-only dep)
- Local/world toggle; snapping
- Gizmo edits create undo commands

---

## Epic 2.4 — Undo/redo · editor

**Goal:** every editor mutation is a command; nothing mutates the scene directly.
**Definition of Done:** any sequence of edits (property, structural, gizmo) undoes and redoes correctly.

### 2.4.1 Command stack · P0 · M · depends: 2.1.3
**Goal:** the command-pattern backbone.
**Deliverable:** `ICommand` do/undo, drag-merge for continuous edits, history capacity, Ctrl+Z / Shift+Ctrl+Z.
Subtasks:
- Command interface + stack; keyboard shortcuts
- Merge policy for continuous (drag) edits; history capacity

### 2.4.2 Property-set + structural commands · P0 · M · depends: 2.4.1, 1.1.4
**Goal:** one generic reflected property command covers every component field forever.
**Deliverable:** reflected property-set command plus create/delete/reparent entity commands.
Subtasks:
- Generic property-set command via `entt::meta`
- Create / delete / reparent / duplicate commands

---

## Epic 2.5 — Scene I/O · editor

**Goal:** scenes round-trip through the editor UI using the Phase 1 serialization.
**Definition of Done:** save → close → reopen → identical scene; a golden test locks the format.

### 2.5.1 Save/load/new from editor · P0 · S · depends: 2.2.1, 1.2.3
**Goal:** the File menu does what File menus do.
**Deliverable:** save/load/new with file dialogs, current-scene tracking, dirty flag and unsaved-changes guard.
Subtasks:
- File dialogs; current-scene path; New Scene
- Dirty flag + unsaved-changes confirmation

### 2.5.2 Scene round-trip golden test · P1 · S · depends: 2.5.1
**Goal:** serialization stability becomes a CI fact (docs/04 testing strategy).
**Deliverable:** load→save byte-stable golden test running in CI.
Subtasks:
- Golden scene fixture; load→save byte-comparison in CI

---

## Epic 2.6 — Project system v0 · editor

**Goal:** the editor always operates inside a *project* — the unit that owns assets, scenes, settings, and (from 4.6) the language choice. Added by the July 2026 audit (D4): ADR-008 fixes the language "at project creation", and the Phase 3 AssetDatabase needs a root to scan — so the project concept must exist before both.
**Definition of Done:** the editor cannot run "nowhere": it creates or opens a project and remembers recent ones.

### 2.6.1 `project.json` + create/open flow · P0 · M · depends: 2.1.3
**Goal:** define what a project *is* on disk.
**Deliverable:** versioned `project.json` schema `{name, engineVersion, language (placeholder until 4.6), paths}`; New Project scaffolds `assets/` + `scenes/`; Open Project + recent-projects list.
Subtasks:
- `project.json` schema (versioned) + load/save
- New Project flow (folder scaffold: `assets/`, `scenes/`)
- Open Project + recent-projects list

### 2.6.2 Project settings panel stub · P1 · S · depends: 2.6.1
**Goal:** a home for project settings before there are many.
**Deliverable:** panel displaying `project.json` fields (language becomes editable at creation in 4.6).
Subtasks:
- Read-only settings panel bound to `project.json`

---

**Phase gate:** Create a project; create / move / edit / save entities visually.
