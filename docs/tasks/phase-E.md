# Phase E — Editor Experience · est. 3–4 mo

> **Goal:** the editor stops being a thing that *works* and becomes a thing that is *usable* — you can see where you are, see what your lights do, edit what you select, manage your files, and reopen your work where you left it.
>
> **Gate:** Open a project and land in the scene you were last editing, on a lit grid floor under a sky; create a Cube from the menu, drop a material on it and see it shade; aim a spot light with a visible gizmo; rename, move and delete assets without leaving the editor
>
> **Phase non-goals:** no IBL/HDRI environment (8.2), no cascaded or soft shadows (8.2.1), no post stack beyond 3.6.3's tonemap (8.2.2), no play mode (4.7), no project templates and no scripting-language choice (4.6.1), no particle/terrain/UI systems (v2, docs/06). The editor platform matrix is unchanged — desktop only, no touch affordances, no adaptive layouts.
>
> **Why a lettered phase.** Numbering is append-only and `3.5`/`3.5.1`/`3.5.2` are already taken by Phase 3's Skeletal-animation epic, so a "Phase 3.5" with epics `3.5.1…` would collide head-on with existing, referenced numbers. This phase is therefore lettered: **E.1 … E.6**, tasks **E.n.k**. It executes between Phase 3 and Phase 4; in the Notion Build Tracker its `Phase #` is `3.5`, which is a sort key, not an identifier.
>
> **Why it exists at all.** Phase 8.5 ("Editor UX polish") is a *polish* epic scheduled after v1.0's feature set exists and after 8.2 raises rendering quality. It is not the place for a missing light type, a material binding that silently does nothing on primitives, or the absence of file management. Those are correctness and workflow gaps, and Phase 4's scripting loop is authored *through* this editor — so they are cheaper to close before Phase 4 than after it.
>
> **Gate artifact:** the editor-built demo scene is committed under `/samples/phase-E-editor/` — grid, sky, all three light types, primitives carrying assigned materials, and a nested asset folder.

---

## Epic E.1 — Viewport legibility · editor · render

**Goal:** you always know which way is up, where the floor is, and exactly what is selected. Today the viewport is an unlit void with no horizon, no ground reference and no orientation widget, and the "selection outline" is a bounding box whose far edges draw through the object.
**Definition of Done:** a fresh scene reads as a 3D space, and the selection highlight traces the object's silhouette rather than a box.

### E.1.1 Debug line renderer · P0 · M · depends: 0.4.1, 3.6.3
_(The enabling task for E.1.2 and E.2.3, and it is sized M rather than S because it is the tree's
first use of a non-triangle topology. `rhi::PrimitiveType::LineList`/`LineStrip`/`PointList` and
`FillMode::Line` have existed since 0.4.1 and are translated in the SDL_GPU backend, and **no
pipeline in the tree has ever requested one** — so this task is where that path gets exercised,
tested and proven on all three backends for the first time.)_

_(**Sized M in the roadmap and landed L**, recorded before implementation and confirmed after — the
3.6.3 / 3.7.3 precedent. The batch, four small shaders and the editor slot are M; what pushed it to L
is two things the subtask list names without owning. "Batch buffer" means per-frame vertex data and
the RHI had no per-frame upload path — `device.hpp`'s own comment deferred it to a Phase 3 that
closed without it, and the portable push-uniform ceiling of 4096 bytes is 128 lines — so
`Device::recordBufferUpload` landed here. And "the three-backend proof" is worth nothing if it means
"no error was logged", so `Device::readbackTexture` landed here too, and the GPU tier asserts bytes:
`RU5` is the first pixel-level assertion in this tree.)_
**Goal:** give the renderer a way to draw a world-space line, which it has never had — every overlay
in the editor today is 2D `ImDrawList` work in screen space, which cannot be occluded by geometry and
cannot describe a shape in the scene.
**Deliverable:** a `render::DebugDraw` batch (world-space lines and camera-facing billboards) on a
`PrimitiveType::LineList` pipeline, recorded into the still-open HDR pass so it goes through 3.6.3's
tonemap with everything else, depth-tested against scene geometry, with a per-frame budget and a
stated overflow behaviour.
Subtasks:
- Vertex format, batch buffer and the first `LineList` pipeline; depth test on, depth write off
- Submission slot inside the open HDR scene pass (before `PostProcess::endScene`), so lines tonemap
- Billboard quads for icon sprites, sized in screen space so they do not shrink with distance
- Budget, overflow policy and Tracy counters; the three-backend proof (Metal / D3D12 / Vulkan)

### E.1.2 Grid floor + world axes · P0 · M · depends: E.1.1
_(**Sized M in the roadmap and landed M**, recorded before implementation and confirmed after. It
landed in **six** commits rather than the seven planned, and the reason is a measured negative
result rather than a saving: the planned first commit put a sanitized depth bias on the debug
`Tested` pipelines, taking the knob E.1.1 handed this task by name — and **a rasterizer depth bias
does not apply to line primitives**. D3D12 states it outright ("Bias is not applied to any point or
line primitives, except for lines drawn in wireframe mode"), Metal states it outright (bias "only
influences triangle primitives"), and Vulkan permits it for lines without guaranteeing it. Measured
before it was dropped: a sweep of 13 line depths x 5 bias magnitudes moved a line at no gap down to
`1e-5`, while the identical sweep with a `TriangleList` billboard moved predictably and bracketed
Metal's bias unit at `2^-24` for a `D32Float` target. So the answer to E.1.1's handoff is that the
mechanism does not exist for this primitive type; the coplanar-geometry problem passes to **E.5.2**,
which is the task that first creates a `Plane` at `y = 0`, and a bias for the billboard pipeline —
where it demonstrably works — passes to **E.2.3**. `DebugDrawConfig` is unchanged and
`debug_draw.{hpp,cpp}` are byte-identical after this task.)_
**Goal:** replace the featureless void with the ground plane every 3D tool has, so translation,
scale and camera distance are readable at a glance.
**Deliverable:** a distance-faded grid with a major/minor cadence that stays legible at every zoom,
coloured X and Z axis lines through the origin, correctly occluded by scene geometry, editor-only
and toggleable from the viewport's view options.
Subtasks:
- Grid geometry with major/minor cadence and distance fade; near/far behaviour stated, not accidental
- Coloured world-axis lines sharing E.3.1's axis palette
- Viewport toggle; the grid never appears in a game/exported view (it is editor chrome, not scene content)

### E.1.3 View-axis gizmo · P0 · M · depends: 2.2.3, 2.3.1
_(Sized M in the roadmap and landing L, recorded here rather than discovered mid-task: the
perspective/orthographic toggle is one line in `projectionMatrix` and five correctness sites
everywhere else — the pick ray, both clip-space predicates and their four readers, and ImGuizmo's
own flag — because every "in front of the eye" test in the editor is vacuous under a projection
whose clip `w` does not depend on the world point. The non-defaulted `ProjectionMode` that makes
those sites a compile error rather than a silent wrong picture costs 57 mechanical call-site edits,
37 of them in one test file.)_
**Goal:** the corner orientation widget every 3D application has — you can always name which way the
camera is pointing, and you can get back to a canonical view in one click.
**Deliverable:** a viewport-corner widget with labelled X/Y/Z balls tracking the camera orientation;
clicking an axis animates the camera to that orthographic-style view about the current pivot;
clicking the centre toggles perspective/orthographic.
Subtasks:
- Corner widget: axis balls with labels, near/far ordering, hover highlight
- Click-an-axis to snap the view (animated, about the current focus point)
- Perspective/orthographic toggle, with the current mode readable in the viewport

### E.1.4 Silhouette selection outline · P0 · L · depends: 2.3.2, 3.6.3
_(Sized L rather than M, recorded here rather than discovered mid-task: this replaces a pure
CPU-side screen-space overlay with a real GPU pass that needs its own target, its own pipeline and
its own place in the frame, and it must keep working for multi-selection, for skinned meshes and for
entities whose mesh reference has not resolved yet.)_
**Goal:** the highlight should trace the object, not a box around it. Today `selection_overlay.cpp`
projects the entity's local AABB and draws its twelve edges, so the far edges show through the model
and a rotated or non-boxy mesh is badly described by its own highlight.
**Deliverable:** an outline that hugs the selected geometry's silhouette and shows **no back edges** —
selected instances rendered to a mask, edge-detected and composited over the resolved image — with
primary/secondary selection distinguished, and the existing point marker kept only for entities that
have no geometry at all.
Subtasks:
- Selection mask pass (static + skinned) and the edge-detect/composite step
- Primary vs secondary selection styling; multi-selection at the existing entity cap
- Retire the AABB path, keeping the point marker for geometry-less entities until E.2.3 lands icons
- Prove the box is gone: an entity whose AABB is much larger than its mesh outlines the mesh

_Outcome: **L, as estimated.** Nine commits, 7 new files and 16 edited source/header files (plus 4
CMakeLists and 4 docs) — two HLSL fragment stages and
zero new vertex stages, four defaulted growths across `render_target.hpp` / `post_process.hpp`, a new
public `render::SelectionOutline`, four pipelines and one lazily-allocated `R8Unorm` texture on
`ForwardRenderer`, a pure `buildSelectionMaskSet` in `scene_renderer.cpp`, the editor rewire, and the
deletion of `BoxEdge`/`BOX_EDGES`/`appendBoxEdges`. No new dependency and no link-line change
anywhere; `ctest -N` unmoved at 172. Full detail in `docs/10-engineering-log.md`._

### E.1.5 Transform-gizmo restyle · P1 · M · depends: 2.3.3, E.1.4
**Goal:** the manipulation handles should read like Unity's or Unreal's — visible against any
background, obviously grabbable, and the same apparent size wherever the object is.
**Deliverable:** ImGuizmo style configuration and drawing options giving cone-tipped translate
arrows, plane handles, a screen-constant gizmo size, hover/active highlighting, and axis colours
shared with E.3.1's Inspector palette.
Subtasks:
- Style config: thickness, arrow/cone geometry, plane-handle size, screen-constant scaling
- Hover and active states; the axis colour palette shared with the Inspector and the grid
- Confirm the write path is untouched — the restyle must not disturb 2.4.1's merge-chain edges

---

## Epic E.2 — Lighting & environment · render · scene

**Goal:** the light types behave like their equivalents in Unity/Godot/Blender, are *visible* in the viewport, and light a scene that is not a black void. The components are already correct — a directional light does aim down its entity's −Z axis (1.4.1's D6) — but nothing draws that direction, nothing says which of two directional lights won, there is no cone light for a lamp, and the only ambient in the engine is a hardcoded 3 % constant with no component behind it.
**Definition of Done:** a scene with no lights at all is legibly lit by its environment; every light type has a gizmo and a defensible falloff; the material preview and the viewport agree.

### E.2.1 `Environment` component + sky pass · P0 · L · depends: 1.3.3, 3.4.1, 3.6.3
_(Sized L, recorded before implementation. It is a reflected component, a new background pass, a
growth of the fragment light block with its pinned offsets, and the built-in-component sweep — and
that last part is the expensive half, not the shader.)_
**Goal:** give the scene an environment, so "no lights" means *dim*, not *black*, and so the
background is authored rather than a `constexpr` clear colour in three unrelated files.
**Deliverable:** a reflected `engine::Environment` component (sky / horizon / ground colour, ambient
mode and intensity, background mode) — a **component**, because scene format v1 serializes entities
and components and nothing else — plus a background pass that draws the gradient sky, and
hemispheric ambient replacing `scene_render`'s hardcoded `Vec3{0.03}`.
Subtasks:
- The reflected component, its fields and its defaults; one-per-scene resolution and the multi-instance WARN
- Background pass (gradient sky / solid colour), drawn before opaque and inside the HDR target
- Hemispheric sky/ground ambient in the PBR shader, replacing the constant; the light block and its `static_assert`ed offsets grow
- **The built-in sweep**: `AERO_BUILTIN_COMPONENT_HEADERS` plus `scene_serialize.cpp`'s hand-written dispatch table and `builtin_serializers.hpp` move together; every component-count literal across the scene, transform, hierarchy, inspector and serialize tests is found and updated
- `docs/09` §2.3 gains the component's payload; a scene authored before this task still loads

### E.2.2 Point falloff + `SpotLight` · P0 · L · depends: E.2.1
_(Sized L for the same reason as E.2.1 — a third light type is a component, a shader arm, a growth
of the GPU light block, five generation sites and a second built-in-count sweep.)_
**Goal:** make a lamp possible. A point light is correctly omni today and correctly ignores rotation;
what does not exist is the *cone* light that "a lamp pointing at something" actually needs. And the
point light's falloff is a squared linear window with a hard cutoff, which reads flat next to the
inverse-square falloff other engines use.
**Deliverable:** physically-shaped inverse-square-with-window falloff for `PointLight`, and a new
reflected `engine::SpotLight` (colour, intensity, range, inner/outer cone angle) aimed down its
entity's −Z axis exactly as `DirectionalLight` is — so one rule covers every directional thing in
the engine.
Subtasks:
- `PointLight` falloff: inverse-square with a smooth window to the range cutoff; the visual A/B recorded
- The reflected `SpotLight` component and its cone parameters; direction from the Transform, never stored
- Gather in `scene_render`; the GPU light block grows with its pinned offsets and its truncation WARN
- Shader arm: cone attenuation with inner/outer smoothing, composed with the existing falloff
- **The built-in sweep** again, as in E.2.1; `docs/09` §2.3 gains the component

### E.2.3 Light gizmos & viewport icons · P0 · M · depends: E.1.1, E.2.2
**Goal:** close the debt `selection_overlay.cpp` already records in its own comment — *"the pick
target for a light is INVISIBLE until you hit it"* — and make a light's aim, reach and cone
something you can see and therefore something you can aim.
**Deliverable:** always-visible, pickable billboard icons for every light and for the camera, plus
wire gizmos drawn through E.1.1: a ray bundle along −Z for directional, a range sphere for point, a
cone for spot. The scene's *active* directional light is distinguishable from the ones the bridge
ignored.
Subtasks:
- Billboard icons per light type and for cameras; screen-constant size; pickable at their drawn extent
- Wire gizmos: directional ray bundle, point range sphere, spot cone (inner and outer)
- Selected vs unselected styling; the active-directional-light indicator
- Editor chrome only — icons and gizmos never appear in a scene render outside the editor viewport

### E.2.4 Material-preview parity + exposure relocation · P1 · M · depends: 3.4.2, E.2.1
**Goal:** the material preview should predict what the material will look like in the scene. Today it
cannot: it hardcodes its own light — a fixed direction at intensity **3.0**, white, no point lights,
no shadows — against a scene whose default directional light is intensity **1.0**.
**Deliverable:** the preview lit by the open scene's `Environment` plus a documented, stated preview
rig rather than three anonymous constants; and the exposure/tonemap controls moved off the viewport's
button strip into a view-options popover, which is where the mock puts them and where they stop
competing with the gizmo buttons for the same row.
Subtasks:
- Preview lighting derived from the scene `Environment`; the remaining rig constants named and justified
- Preview/viewport A/B recorded, so "they match" is a measurement rather than an impression
- Exposure + tonemap operator move to a view-options popover; the viewport strip keeps only mode controls

---

## Epic E.3 — Inspector & context routing · editor

**Goal:** the Inspector says what it is showing, follows what you click, and lets you *pick* an asset instead of reading a GUID. All three are missing: a `Vec3` renders as three unlabelled drag boxes, clicking an entity does not raise the Inspector, and an asset reference is a text string plus a `Clear` button.
**Definition of Done:** every vector field is axis-labelled; selecting anything raises the panel that edits it; every asset reference is a searchable picker with a preview.

### E.3.1 Axis-labelled vector fields · P0 · M · depends: 2.2.2
**Goal:** you should be able to tell which box is Y. `inspector_panel.cpp`'s `Vec3` arm is a bare
`DragFloat3` with a hidden label; the only visible text is the field name in the left column.
**Deliverable:** X/Y/Z (and W) labels with axis colours, per-axis reset, and a consistent label
column — implemented **inside the generic `Vec3`/`Quat` arms**, with no per-component special-casing,
so ADR-004's "a new `[[engine::component]]` gets a working UI with zero editor changes" holds
unbroken.
Subtasks:
- Axis-labelled, axis-coloured `Vec3` row; the same treatment for the euler `Quat` row
- Per-axis reset-to-default; the label column and row rhythm made consistent across every field kind
- Colour picker rows keep their existing behaviour (an `AERO_COLOR` `Vec3` is not an axis triple)
- The merge-chain gate pair (2.4.1's asymmetric open/close edges) preserved on every new sub-widget

### E.3.2 Selection-follows-focus router · P0 · M · depends: 2.2.1, 3.1.3, 3.4.2
**Goal:** clicking a thing should show you the thing. Today selecting an entity leaves the Inspector
tabbed away if you last looked at Import Details, and opening a material means finding the Material
tab by hand.
**Deliverable:** one context router over the panel-focus plumbing that already exists
(`requestPanelFocus`, used today only by **Edit ▸ Project Settings…**): selecting an entity raises
the Inspector; activating a material raises the Material panel; activating an importable asset raises
Import Details. Overridable by the user, and never stealing focus mid-edit or mid-drag.
Subtasks:
- The routing rule as one pure decision (source of selection → panel id), tested without ImGui
- Wire the entity, material and importable-asset sources through it
- Suppression while a text field is active, a gesture is in flight or a drag is live
- A preference to turn the behaviour off, because focus stealing is a taste

### E.3.3 Asset-reference picker · P0 · L · depends: 3.1.3, 3.1.5, E.3.1
_(Sized L: it is a reusable popup with search, thumbnails and filtering that must serve both the
Inspector's reflected `Guid` rows and the Material panel's five texture slots, and it must not
duplicate the drop-legality matrix 3.1.5 established.)_
**Goal:** picking an asset should not require knowing where it is. The Inspector's `Guid` row is text
plus `Clear`, and the Material panel's texture slots are a `BeginCombo` with no search and no preview
— on a project with hundreds of textures, neither is usable.
**Deliverable:** one picker popup — type-to-search, thumbnails, kind filter, an explicit "None", and a
drop target — used by every asset-reference field in the editor.
Subtasks:
- The picker popup: search, kind filter, thumbnail grid, "None", keyboard navigation
- Adopt it on the Inspector's reflected `Guid` rows, routed through the existing property command
- Adopt it on the Material panel's five texture slots, replacing the combo
- Legality composed from 3.1.5's existing drop matrix, never re-stated beside it

### E.3.4 Material inspector redesign · P1 · M · depends: 3.4.2, E.3.3
**Goal:** the material panel is a flat list of numbers and five identical dropdowns; it should read
as a material.
**Deliverable:** grouped sections, texture-slot rows carrying a thumbnail, the E.3.3 picker, a clear
action and their colour-space note, a larger live preview, and Apply/Revert affordances that look
like what they do.
Subtasks:
- Section grouping and row layout; the slot row (thumbnail + picker + clear + colour space)
- Preview sizing and placement; Apply/Revert affordance and dirty-state legibility
- The sticky-target rule, the `sessionCopy != fileCopy` dirty rule and the single write path all unchanged

---

## Epic E.4 — Project, scene & asset management · editor

**Goal:** the editor should remember where you were, refuse to wander into another project, and let you manage files without a file manager. None of the three is true: there is no per-project editor state at all, `openSceneFile` performs zero validation of the path against the project root, and the Asset Browser is read-only by contract with sixteen actions of which none is a file mutation.
**Definition of Done:** reopening a project lands in the scene you last saved; a scene outside the project is refused with an explanation; the browser can create, rename, move and delete safely, with `.meta` identity preserved.

### E.4.1 Reopen the last scene · P0 · M · depends: 2.5.1, 2.6.1
**Goal:** opening a project should resume your work. Today opening one always lands on an Untitled
seeded scene, because adopting a project is defined as "new scene, clear path".
**Deliverable:** a versioned, machine-local per-project editor state file under the project's
existing `Library/` directory (already excluded from the asset scan by canonical path, already
git-ignored, already the home of machine-local derived state), recording the last scene. Opening a
project opens that scene; if it is missing or unreadable, the first scene under `paths.scenes`; if
there is none, a new scene.
Subtasks:
- The state format (versioned, tolerant, one WARN and a clean fallback on anything malformed) in `docs/09`
- Write on save and on project close; read on project open
- The open-project resolution order: recorded scene → first scene under `paths.scenes` → new scene
- Respect INV-P1 — no second `ProjectSession::set()` call site; `project.json`'s five-key envelope untouched

### E.4.2 Scene/project containment · P0 · S · depends: 2.5.1, 2.6.1
**Goal:** the editor should not silently open a scene belonging to a different project. It does today:
`openSceneFile` and `saveSceneFile` take a raw absolute path from any of four callers and act on it
with no comparison against the open project's root, which leaves the document pointing at project B
while the AssetDatabase still resolves every GUID against project A.
**Deliverable:** one containment predicate applied at the two choke points every entry path passes
through, refusing an out-of-project scene with a message that says why and offers to open that
project instead.
Subtasks:
- The containment predicate as a pure function (normalisation, symlinks, case, the drive-letter case), tested alone
- Applied at `openSceneFile` and `saveSceneFile`; the "open that project instead" offer
- The no-project case stated explicitly rather than falling through

### E.4.3 Asset file operations · P0 · L · depends: 3.1.1, 3.1.3, 3.1.4
_(Sized L, recorded before implementation: it is five destructive-capable operations, a guard
extension, a modal, drag-to-move, and the discharge of a standing invariant that no test in this
tree can currently see violated.)_
**Goal:** manage project files from inside the editor. The browser creates nothing but a material and
deletes nothing but an orphaned sidecar, so every real file operation means leaving the tool — and
doing it outside means the `.meta` sidecar is left behind or orphaned by hand.
**Deliverable:** New Folder, New Asset, Rename, Move (including drag-to-move in the tree) and Delete,
with context menus and a confirmation modal, each preserving `.meta` identity, each coherent with the
scan and the watcher.
Subtasks:
- Context menus and the action set; the confirmation modal for destructive operations
- **INV-A8 discharged**: the `.meta` moves, renames and dies with its asset in the *same* operation — and this task is where that finally becomes testable
- Every destructive call lands in `editor/src/asset_actions.cpp`, keeping `check-project-no-delete.sh` Check B a two-file positive allowlist; the guard's self-tests are extended in the same commit
- Path validation re-verified against the live filesystem immediately before acting, in a fixed order — the `deleteOrphanMeta` pattern, not a new one
- Write-then-rescan in the same tick so the watcher's settle window never double-fires
- Refuse on an incomplete directory listing (`listingIsComplete`, never `status == Ok`) — a prefix cannot prove a name is free

### E.4.4 Browser ignore rules · P1 · S · depends: 3.1.1, 3.1.4
**Goal:** stop indexing files that are not assets. Blender's `.blend1`/`.blend2` rolling backups pass
today's scan predicate, so each one gets a GUID, a `.meta` sidecar, a content hash, a cache entry and
a tile in the browser labelled `BLE1`.
**Deliverable:** an extended ignore roster, honoured everywhere, from **one** edit.
Subtasks:
- Extend `isScannableAssetName`'s roster (`.blend1`, `.blend2`, editor backup suffixes) — the single source
- Confirm the derivation holds: `isWatchableAssetName` composes it, so scan, watcher, browser and thumbnails follow without a second list
- Existing sidecars for newly-ignored files become orphans and are reported by the existing issues path, never silently deleted

### E.4.5 Material names & thumbnails in the browser · P1 · M · depends: 3.1.3, E.2.4
**Goal:** a folder of materials should be readable. Today every `.aeromat` is an identical flat tile
labelled `AERO` with its filename, and `MaterialDocument::name` — the name you typed in the Material
panel — is never shown anywhere but that panel.
**Deliverable:** the material's document name shown alongside its filename, and a **rendered** sphere
thumbnail per material.
Subtasks:
- Surface the document name on the tile and in the list row, without deriving either name from the other
- A rendered material thumbnail producer behind the existing `ThumbnailLedger` — a second producer, not a second cache
- Inherit the ledger's stickiness, budget and eviction rules rather than re-deriving them; the service call stays outside the draw walk

---

## Epic E.5 — Content creation UX · editor · scene_render

**Goal:** making a cube should be a menu item, and the material you assign should show up. Neither is true: there is no create-primitive command anywhere, and a material assigned to a primitive is written to the component and then ignored by the renderer.
**Definition of Done:** every built-in entity type is creatable from a menu; a material dropped on a primitive renders with that material.

### E.5.1 Primitive material binding fix · P0 · S · depends: 3.1.5, 3.4.1
**Goal:** fix a confirmed defect. `buildRenderView`'s primitive arm never assigns
`instance.material`; `resolveMaterial` is called only from the imported-mesh arm. So a `.aeromat`
dropped on a primitive entity **is** written to `MeshRenderer::material` through the undoable
property command, and is then silently discarded at draw time — the entity always resolves to the
renderer's default material.
**Deliverable:** the primitive arm resolves `MeshRenderer::material` exactly as the imported-mesh arm
does, with the unresolved case counted the same way.
Subtasks:
- Resolve the material on the primitive arm; the unresolved/in-flight case counted, not silently dropped
- A test that a primitive plus a valid material GUID draws with that material's parameters
- A test that a primitive with a nil material GUID still draws byte-identically to today

### E.5.2 Create menu + named primitive selector · P0 · M · depends: 2.2.1, 2.4.2, E.2.2
**Goal:** creating content should not require knowing that `1` means Sphere. Today a primitive is made
by Create Empty → Add Component → dragging a clamped `0..2` number with no names on it, and there is
no way at all to create a light or a camera except the same route.
**Deliverable:** a Create menu covering every built-in — 3D Object (Cube / Sphere / Plane), Light
(Directional / Point / Spot), Camera — each seeding a sensible transform, each going through the
existing structural commands so it undoes; and the reflected `primitive` field rendered as a **named**
selector rather than a number drag.
Subtasks:
- The Create menu in the Hierarchy and the menu bar; sensible default transforms per type
- Every entry routed through the existing structural commands — no new direct World write
- A named selector for `primitive`, driven by a reflection-visible label annotation inside the generic integer arm, never by a component-name special case
- Newly created entities are selected and framed, so creation is visible

---

## Epic E.6 — Shell identity & visual system · editor

**Goal:** the editor should look like the mocks — one type scale, one palette, real icons, a toolbar and a status bar. Today it loads no font of its own (ImGui's built-in ProggyClean, which is why every literal in the tree is ASCII-only), has no icon set, and its entire theme is `StyleColorsDark()` plus a DPI size scale.
**Definition of Done:** every panel is restyled against one shared theme; no colour, radius or spacing is a bare ImGui default or an anonymous file-local constant.

### E.6.1 Font, icon set and theme system · P0 · L · depends: 2.1.1
_(Sized L: it is the editor's first font load, its first icon set and the first central owner of
values that are currently scattered `constexpr`s in six panels — and it changes the DPI story, which
`ConfigDpiScaleFonts` carries alone today.)_
**Goal:** give the editor a visual system instead of a default.
**Deliverable:** a loaded UI font and monospace font, an icon set, and a single `EditorTheme` owning
the palette, rounding, spacing and per-role colours — with the panels reading from it rather than
from local constants.
Subtasks:
- Font loading (UI + monospace) with a stated licence, and the HiDPI story re-checked on all three OSes
- The icon set and its lookup; a stated policy for what the editor may now put in a string literal
- `EditorTheme`: palette, rounding, spacing, per-role colours; the scattered panel constants adopt it
- The dependency lands editor-only, per the dependency-placement invariant

### E.6.2 Main toolbar, breadcrumb and status bar · P0 · M · depends: E.6.1, 2.3.3
**Goal:** the shell's chrome. There is no toolbar, no status bar and no indication of which
project or scene is open beyond the window title.
**Deliverable:** the top strip — Select / Move / Rotate / Scale, Local|World, Snap, and Play / Pause /
Step shipped **disabled with a tooltip naming task 4.7.1**, per this project's convention for
unimplemented items — plus a project/scene breadcrumb with its dirty indicator, and a bottom status
bar carrying the project root, the watcher state, frame timing and the active backend.
Subtasks:
- The toolbar and its bindings to the existing gizmo mode model; the disabled play controls and their tooltip
- Project/scene breadcrumb with dirty state; the undo-label affordance
- Status bar: project root, watcher state, fps/frame time, backend
- The viewport's own overlay strip reduced to what the toolbar does not own

### E.6.3 Panel chrome pass · P1 · L · depends: E.6.1, E.6.2
_(Sized L because it is eight panels, not one; each is small on its own and the total is not.)_
**Goal:** apply the visual system everywhere, so the editor reads as one product.
**Deliverable:** every panel restyled against the theme — Hierarchy (search, count badge, type dots,
per-row meta), Inspector (component cards with icons and an overflow menu), Console (columns, level
tags, row tint, footer counts), Assets (breadcrumb, footer, item count, selection ring), Viewport
(view-options row, hint chips, stats overlay).
Subtasks:
- Hierarchy, Inspector, Console, Assets, Viewport and the three secondary panels, each against the theme
- The viewport stats overlay (frame time, draw calls, triangles, entities, GPU time) and hint chips
- **Panel ids stay frozen** — they are the `imgui.ini` settings keys, and the existing pin covers them
- Every panel re-checked at HiDPI and at a small window size

### E.6.4 Welcome + New Project restyle · P2 · M · depends: 2.6.1, E.6.1
**Goal:** first contact with the editor should look like the rest of it.
**Deliverable:** the Welcome window and New Project dialog restyled — card layout, live path
validation, footer summary — carrying **today's fields only**.
Subtasks:
- Welcome window: recents as cards, empty state, primary actions
- New Project dialog: layout, live "creates <path>" validation, footer summary
- The scripting-language control ships `enabled = false` with a tooltip naming **4.6.1**, which owns it
- Project templates, scene units and git-init are explicitly **out of scope** and recorded as such

---

**Phase gate:** Open a project and land in the scene you were last editing, on a lit grid floor under a sky; create a Cube from the menu, drop a material on it and see it shade; aim a spot light with a visible gizmo; rename, move and delete assets without leaving the editor.
