# Phase F — Editor Workflow · est. 2–3 mo

> **Goal:** the editor stops getting in the way of the loop it exists for. A click selects what is under the cursor, a key deletes it, a file dragged in from the desktop becomes an asset, scripts and scenes are made and reached from the browser, and a saved script reaches the running game at once.
>
> **Gate:** Open the editor and it fills the screen; drag a model in from the file manager and it imports; click the object under the cursor, even one standing behind another object's bounding box, and delete it with the Delete key; select a camera and see what it sees; create a script and a second scene from the browser and switch scenes from the breadcrumb; save a script while the game plays and see the change at once
>
> **Phase non-goals:** no new on-disk format — scenes, projects and every other JSON document keep the formats `docs/09` specifies (weighed for this phase and left as they are); no hot reload "everywhere" (`docs/future-roadmap.md`, v2) — F.4.2 proves and bounds the kinds that already reload and adds none; no shader hot reload; no keyboard shortcuts for the asset browser (its F2/Del handoff in `docs/08` stays where it is); no attaching a script by dropping it on a Hierarchy row or the Viewport (4.4.1's handoff, unchanged); no key-binding registry or rebinding UI; no multi-viewport and no second editor window. The editor platform matrix is unchanged — desktop only, no touch affordances, no adaptive layouts.
>
> **Why a lettered phase.** Numbering is append-only and `4.5`/`4.5.1`/`4.5.2` are already Phase 4's C++-project-mode epic and its tasks, so a "Phase 4.5" would collide with referenced numbers exactly as a "Phase 3.5" would have (`docs/07`, numbering conventions). This phase is therefore lettered: **F.1 … F.4**, tasks **F.n.k**. It executes between Phase 4 and Phase 5; in the Notion Build Tracker its `Phase #` is `4.5`, which is a sort key, not an identifier.
>
> **Why it exists at all.** Phase 4 closed with a small game made in the editor, and making it — and then building a test project with an imported model — surfaced a cluster of workflow gaps no phase owns: a click that selects the wrong object, a Delete key that works in one panel only, a window that opens at 1280 × 720, no way to bring a file in except copying it on disk, scripts and scenes that cannot be created or opened from the browser, and a 2.5-second pause between saving a script and seeing it. Phase 8.5 is a *polish* epic after v1.0's feature set; these are not polish. Phase 5 builds and ships a real game **through** this editor, so each of them is paid on every working day of Phase 5 unless it is closed first — the argument that placed Phase E before Phase 4.
>
> **Gate artifact:** the project the gate is run on is committed under `/samples/phase-F-workflow/` — two scenes, an imported model with a cube standing behind its bounding box (the selection case this phase was opened on), a camera, and a script.

---

## Stories

The gate as a scenario: Given the editor and a project, when I open it, drag a model in from the file manager, click
objects in the viewport, press Delete, select a camera, create a script and a scene from the browser, switch scenes
from the breadcrumb and save a script while the game plays, then the window fills the screen, the model imports,
exactly the object under the cursor is selected and deleted, the camera shows what it sees, the new script and scene
exist and open, and the running game follows the saved script at once. Each story below makes more of it run; its
demo lives in `samples/phase-F-workflow/`.

### Story 1 — I open the editor full screen, drag a model in, click exactly what I see and delete it with a key
Tasks: F.1.1, F.1.3, F.2.1, F.3.1
As a game developer, I open the editor, bring a model in by dragging it from the file manager, click the object I am
looking at and remove it with the Delete key, so that the first minute of work needs no workaround.
- Given the editor has not run before, when I start it, then its window fills the screen, and when I turn View ▸ Full
  Screen off, then the window is 1920 × 1080, clamped to the display's usable area.
- Given I last quit the editor full screen, or windowed at some size and place, when I start it again, then it comes
  back the same way.
- Given a project is open and the Assets panel shows a folder, when I drag a model file from the file manager onto the
  panel, then a copy of the file appears in that folder and imports as if it had been placed there on disk.
- Given a cube standing behind an imported model whose bounding box covers it on screen, when I click a pixel of the
  cube, then the cube is selected, and when I click a pixel of the model, then the model's part under the cursor is
  selected.
- Given an entity selected by a click in the viewport, when I press Delete or Backspace, or choose Edit ▸ Delete, then
  the entity is removed exactly as the Hierarchy's Delete removes it, and one Undo brings it back.
Demo: `samples/phase-F-workflow/` — start the editor and see it fill the screen, drag the sample's model file in from
the file manager, click the cube behind the model and then the model, press Delete on each and undo; about two
minutes.

### Story 2 — The viewport shows me what I need and selects the way I expect
Tasks: F.1.2, F.1.4, F.1.5, F.2.2
As a game developer, I select a whole model with one click, see what a camera sees before I press Play, read the
orientation widget at a glance and find the view options in the menu bar, so that I can lay out a scene without
guessing.
- Given an imported model made of several parts, when I click it once, then the whole model is selected and outlined,
  and when I click the same part again, then that part alone is selected.
- Given the view-axis widget, when the camera turns, then each positive axis draws a line from the centre to its ball,
  ordered with the balls from far to near.
- Given a Camera entity, when I select it, then the viewport draws its frustum and an inset showing the scene from
  that camera, and when I change its field of view in the Inspector, then both follow in the same frame.
- Given the viewport, when I look for its options, then the projection, field of view, tonemap, exposure and the four
  overlay toggles are in the View menu, the viewport carries no settings chip, the stats card is unchanged, and the
  options are the same after I restart the editor.
Demo: `samples/phase-F-workflow/` — click the model once and then again, orbit and watch the widget's axis lines,
select the camera and change its field of view, turn the grid off from the View menu and restart the editor to find it
still off; about two minutes.

### Story 3 — I make scripts and scenes in the browser, move between scenes at a glance, and the right panel comes forward by itself
Tasks: F.2.3, F.3.2, F.3.3, F.3.4, F.3.5
As a game developer, I create a script and a scene where my assets are, open and switch scenes without the File menu,
and see the panel that edits my selection without picking its tab, so that organising a project is direct.
- Given a selection made in the viewport, the Hierarchy or the Assets panel, when it is an entity, a material or a
  model that still needs importing, then the Inspector, the Material panel or the Import Details panel is in front
  without my choosing its tab.
- Given a TypeScript project and a folder in the Assets panel, when I choose New Asset ▸ Script and name it, then a
  `.ts` file with the lifecycle hooks exists there, type-checks clean, and can be set on a ScriptComponent.
- Given the Assets panel, when I choose New Asset ▸ Scene, then a scene file exists there, shown with a scene icon,
  and when I double-click a scene, then it opens after the unsaved-changes question, if one is due.
- Given a project with several scenes, when I click the scene's name in the breadcrumb, then a list of the project's
  scenes opens with the current one marked, and choosing one opens it.
- Given a scene I have saved, when I look at it in the Assets panel or in the breadcrumb's list, then it shows a
  picture of the scene as it was when saved.
Demo: `samples/phase-F-workflow/` — select a material and then an entity and watch the panels follow, create a script
and a scene from the browser, double-click the new scene and save it, return to the first scene through the
breadcrumb's list and save it too, and find both scenes pictured; about two minutes.

### Story 4 — I save a script while the game runs and the game changes at once
Tasks: F.4.1, F.4.2
As a game developer, I save a script, a material or a model while the game plays and see the change without a pause,
so that iterating costs no waiting.
- Given the game is playing, when I save a change to a script, then the running game follows the new code within the
  budget F.4.1 records, against the 2.2–3.2 s measured before it.
- Given a file is still being written by a slow copy, when the watcher sees it, then nothing imports until the file is
  whole.
- Given the game is playing or the editor is in Edit, when I save a change to a material or replace a model file, then
  the viewport shows it within the same budget, and each asset kind that does not reload is named in the
  documentation with its reason.
- Given the gate's scenario, when I run it from start to end on a build of `main`, then every line of it holds.
Demo: `samples/phase-F-workflow/` — press Play on the sample's scripted scene, change the script's speed and save,
then change the material's colour and save, and watch both arrive; about one minute.

---

## Epic F.1 — Viewport selection & orientation · editor · render

**Goal:** what the cursor is on is what gets selected, and what is selected can be removed with a key. Today the pick
tests bounding boxes only, so the nearest *box* wins whatever the pixel shows; an imported model can only be selected
one part at a time; the Delete key works only while the Hierarchy holds the keyboard; the orientation widget draws
six balls with nothing joining them; and a camera is an icon with no view.
**Definition of Done:** a click selects the object whose pixel was clicked, a first click on a model selects the whole
model, Delete removes the selection from the viewport, the view-axis widget draws its axes, and a selected camera
shows its frustum and its view.

### F.1.1 Per-triangle picking · P0 · M · depends: 2.3.2, 3.1.5, 3.3.1
_(The case this phase was opened on, measured before the task was written: a red cube stands behind an imported boat
whose oars span a wide box, and clicking the cube's pixels selects a part of the boat. `picking.cpp` slab-tests each
`MeshRenderer`'s local box and keeps the nearest box **entry**, so a box that is mostly empty air wins over the solid
object seen through it. 3.1.5's handoff named the fix and left it unowned — "per-triangle picking is still unowned;
this task's box pass is written to be its broadphase" — and `PickResult::distance` is kept for exactly that.)_
_(The triangles are not in memory today: `loadFromImportedModel` cooks into a local buffer that dies with the call,
after `createMesh` has read through it. What the pick tests against, and what it costs to keep, is this task's first
decision.)_
**Goal:** a click selects the object whose surface is under the cursor, not the object whose box the ray enters first.
**Deliverable:** a ray–triangle narrow phase behind the existing box pass — every box the ray enters is tested against
its mesh's triangles, the nearest triangle hit wins, and a box whose triangles the ray misses is not a hit at all —
for imported meshes and built-in primitives, with the click's cost measured on the largest sample model.
Subtasks:
- Triangle source: positions and indices kept per loaded mesh for the pick (or re-derived on demand), primitives included; the memory it adds is measured and stated
- Narrow phase behind the box broadphase: nearest triangle wins, back faces and degenerate triangles have a stated rule, ties keep today's lowest-index rule
- Skinned meshes: the pose the test uses is stated — the drawn pose, or the box pass kept for them with the reason
- Non-mesh entities and the light/camera icons keep their screen-space discs; the rule ordering a disc against a triangle hit is restated
- The opening case pinned as a test: a cube behind a wide, mostly empty box is selected by a ray through the cube; the click's time on the largest sample model recorded
- The gate project, committed as a TypeScript project under `samples/phase-F-workflow/`: the case this phase was opened on — an imported model, one the repository may carry, with a cube standing behind its bounding box — plus a camera, a second scene holding a scripted cube and its material, and a model file kept outside the project to drag in; every story demo starts from it

### F.1.2 Whole-model selection, with a second click for the part · P0 · M · depends: F.1.1, 3.1.5, E.1.4
_(Measured: every entity is its own pick candidate and nothing promotes a hit to an ancestor. An instantiated model's
root is an ordinary entity with no mesh (`instantiate_plan.hpp`: slot 0 is the synthetic root), so it is reachable
only through the 8-dp disc at its origin — clicking a boat selects an oar, never the boat.)_
**Goal:** one click selects the thing I think of as one object; a second click reaches inside it.
**Deliverable:** a click on a part of a model selects the model's root; a click on the same part while that root (or
anything under it) is already selected selects the part itself. The outline, the gizmo pivot and F-to-frame follow
the root as one object.
Subtasks:
- The rule for "the whole object": which ancestor a hit promotes to — the outermost ancestor, or a recorded instance root — decided against what a scene stores today and written down
- The second click: a hit inside the selected root's subtree selects the hit entity; modifier-clicks (add, toggle) keep their meaning under promotion
- A selected root with no mesh of its own outlines its whole subtree, frames it with F, and puts the gizmo at its own transform
- The Hierarchy shows the promoted selection: the root's row is selected and revealed, without expanding the subtree

### F.1.3 Delete from the viewport and the Edit menu · P0 · S · depends: 2.2.1, 2.2.3, 2.4.2
_(Measured: `hierarchy_panel.cpp` is the only place `ImGuiKey_Delete` / `ImGuiKey_Backspace` are read, under
focus-scoped routing, so they fire only while the Hierarchy holds the keyboard. A viewport click selects without
moving the keyboard there, so after selecting in the viewport the key does nothing. The Edit menu has no Delete
item.)_
**Goal:** pressing Delete removes what is selected, wherever it was selected.
**Deliverable:** Delete and Backspace delete the selection while the Viewport is focused, through the same
`DeleteEntitiesCommand` the Hierarchy uses — one undo step, the same refusal while a game plays — and the Edit menu
gains Delete and Duplicate with their key hints.
Subtasks:
- Viewport binding for Delete and Backspace, gated like the tool keys: not while flying, not during a gizmo drag, not while a text field holds the keyboard (the Hierarchy's own latch for a selection shortcut beside a text field, `I339`)
- Edit ▸ Delete and Edit ▸ Duplicate, enabled only with a selection, each a single shared action with the Hierarchy's
- Locked in Play like every scene edit (4.7.1's scene lock); the key reaches the game's input instead

### F.1.4 View-axis widget arrows · P0 · S · depends: E.1.3
_(Measured: `drawViewAxisGizmo` draws a divider circle, six balls and the centre badge; the only `AddLine` in
`viewport_panel.cpp` belongs to the selection overlay. The widget has never drawn an axis.)_
**Goal:** the orientation widget should read as three axes, not six dots.
**Deliverable:** each positive axis draws a line from the centre to its ball in the axis colour, depth-ordered with
the balls so a far axis passes behind a near ball; the negative balls, the centre badge and every click target are
unchanged.
Subtasks:
- Axis lines in `viewAxisLayout`'s pure model, so their order and endpoints are testable without ImGui
- Draw order: far to near with the balls; the line ends at the ball's edge, not its centre
- Hover, the 0.55 far-side alpha and the snap animation apply to a ball's line as to the ball

### F.1.5 Camera frustum and preview · P0 · L · depends: E.1.1, E.2.3, 4.7.1
_(Measured: a Camera draws an icon and **no gizmo** (`viewport_gizmos.cpp`, "a Camera draws its icon and NO gizmo"),
E.2.3's handoff "a camera FRUSTUM gizmo" is unowned, and nothing in the tree renders a second view of the scene. In
Play the game renders from the lowest-index Camera (`scene_renderer.cpp`); the preview must show that same picture.)_
_(Sized L, not M: the frustum is a handful of debug lines, but the preview is the editor's first second scene view in
one frame — its own target, its own camera, and a stated answer for the shadow pass and for what the frame costs.)_
**Goal:** see what a camera sees while placing it, without pressing Play.
**Deliverable:** a selected Camera draws its frustum (near and far planes, from its own field of view and the preview's
aspect) through E.1.1's debug lines, and the viewport shows an inset rendering the scene from that camera — no grid,
no gizmos, no icons, as the game would draw it.
Subtasks:
- Frustum gizmo for a selected Camera, from `fovYRadians`, `nearPlane` and `farPlane`; the far plane clamped for drawing, with the rule stated
- A second scene view per frame: target, camera and render call; what is shared with the main view (shadow map, culling scratch) is decided and measured
- The inset: placement beside the existing HUD elements, sizing, the aspect it uses, and its own hit area so a click on it does not pick through
- The preview draws game content only — editor chrome off — and matches Play's picture for the same camera
- Frame cost with the inset shown, measured against the frame without it; the inset hides when the viewport is too small, like the view-axis widget

---

## Epic F.2 — Window, menus & panels · editor · platform

**Goal:** the shell opens ready to work in and keeps its controls where they are looked for. Today the window opens
at a fixed 1280 × 720 and remembers nothing; the viewport carries its options in a chip over the picture, and loses
them on restart; and the panel that edits a selection is not reliably the one in front.
**Definition of Done:** the editor opens full screen and returns to how it was left; every viewport option lives in
the View menu and survives a restart; selecting an entity, a material or an importable model brings its panel
forward every time.

### F.2.1 Full-screen start and remembered window state · P0 · M · depends: 0.3.1, 2.1.3, E.6.2
_(Measured: `editor/src/main.cpp` creates the window as `{.title = "Aero Editor", .width = 1280, .height = 720}`.
`platform::WindowConfig` offers `resizable`, `highDpi` and `hidden` only; the platform layer has no fullscreen or
maximize call and no window-position API; nothing about the OS window is saved between runs — `aero_editor.ini` holds
the dock layout alone.)_
**Goal:** the editor opens using the whole screen, and comes back the way it was left.
**Deliverable:** the editor's window opens full screen; View ▸ Full Screen and a chord toggle it; the last state — full
screen or windowed, and the windowed size and position — is saved per user and restored, with a windowed default of
1920 × 1080 clamped to the display's usable area.
Subtasks:
- `platform`: fullscreen in `WindowConfig`, a `Window` call to enter and leave it and to read the state, window position get/set — engine types only (rule #3)
- The editor requests it in `main.cpp` alone, so no test window and no sample ever opens full screen
- View ▸ Full Screen with its chord; the menu bar, toolbar and status bar are the editor's own and stay
- Persistence in `editor_prefs.json` (`docs/09` §8.5): state, windowed size and position; a saved position on a display that is gone falls back to the default
- A minimum OS window size, closing E.6.4's handoff (nothing stops a window narrower than the 280-point dialogs)

### F.2.2 Viewport options in the View menu · P0 · M · depends: E.1.3, E.2.4, E.6.3
_(Measured: the viewport's top-left chip opens a popover holding the projection, the tonemap operator, the exposure
and four overlay toggles — Grid, Gizmos, View axis, Stats (`ViewportPanel::drawViewOptions`). None of them is saved:
they are session state, and persisting them is a handoff 3.6.3, E.1.2, E.2.3 and E.6.3 each passed on.
`EditorCamera::setFovYRadians` has no caller, so the "60°" the chip prints cannot be changed — "a FOV control" is
unowned too.)_
**Goal:** the picture is for the scene; its options belong in the menu bar, and they should not reset every launch.
**Deliverable:** the settings chip and its popover are removed from the viewport; a View ▸ Viewport submenu holds the
projection, a field-of-view control, the tonemap operator, the exposure and the four overlay toggles; all of them are
saved per user. The stats card and the two hint chips stay as they are.
Subtasks:
- View ▸ Viewport: projection, field of view, tonemap, exposure, Grid, Gizmos, View axis, Stats — one state, read by the viewport as today
- Remove the chip and its popover; the centre badge of the view-axis widget stays the in-viewport reading of the projection
- A field-of-view control over `EditorCamera::setFovYRadians`, with its range stated
- Persist all of it in `editor_prefs.json` (`docs/09` §8.5), with the absent-key-is-default rule
- The HUD layout after the chip is gone: the stats card, the hint chips and F.1.5's inset do not overlap at the minimum viewport size

### F.2.3 Selection routing that always lands · P0 · M · depends: E.3.2
_(Measured: E.3.2's router exists and maps an entity selection to the Inspector, a `.aeromat` to the Material panel
and an importable model to Import Details; "Focus Follows Selection" defaults to on. Using the editor on a real
project still left the wrong panel in front — an entity selected in the viewport with the Material tab showing, and
tabs raised by hand. **The cause is not known**, and the task's first step is to reproduce it rather than assume one.
Candidates the code admits: the route is held while a text field, a drag, a gizmo drag or a popup is active and
dropped if the panel is hidden; an automatic route selects the dock tab without moving the keyboard; the material
session retargets only on a different, existing `.aeromat`; the import route settles a tick later.)_
**Goal:** selecting something shows the panel that edits it, every time, without choosing a tab.
**Deliverable:** the misses reproduced on the real editor and their causes named and fixed, and a written route table
— selection kind → panel — in which every row is proven: an entity, a material, a model that needs importing, an
imported model, and each asset kind no panel edits.
Subtasks:
- Reproduce: the sequences that leave the wrong tab in front, recorded with their cause before anything is changed
- Fix each cause; a held route is delivered when its hold ends rather than lost
- The route table, including what selecting a script, a scene, a texture or an audio file does — and Import Details no longer reporting "no importer claims this file type" for a kind that is not an import
- The raised tab is visible when the dock's tab row overflows
- The preference still turns all of it off

---

## Epic F.3 — Assets & scenes · editor · platform

**Goal:** the browser is where project content comes from and where it is reached. Today a file enters a project only
by being copied into its folder outside the editor; the browser creates materials and folders and nothing else; a
scene is a tile labelled "JSON" that cannot be opened from it; scenes are switched through a file dialog; and the
folder tree draws no icons.
**Definition of Done:** files dropped from the file manager become assets; scripts and scenes are created in the
browser; a scene is a recognisable asset that opens on a double-click and can be switched from the breadcrumb.

### F.3.1 Drag files in from the file manager · P0 · M · depends: 0.3.1, 3.1.3, E.4.3
_(Measured: no drop event is handled anywhere — the platform layer translates window, key, mouse and gamepad events
and discards the rest, and no SDL drop event name appears in `editor/` or `engine/`. Files reach a project only by
being placed on disk, where the watcher's rescan finds them.)_
_(`platform::Event` is a trivially-copyable union of POD payloads, so a dropped file's path cannot ride inside it as
the other payloads do; where the path lives is this task's first design point.)_
**Goal:** bring content into a project the way every desktop tool allows — by dragging it in.
**Deliverable:** files and folders dragged from the OS file manager onto the Assets panel are copied into the folder
under the cursor (or the folder being shown), then scanned and imported exactly as if placed there on disk; the panel
shows where the drop will land; a drop anywhere else in the window is refused with one Console line.
Subtasks:
- `platform`: file-drop events (begin, position, file, complete) as engine types, with the path's storage outside the POD event (rule #3)
- Drop target: the folder under the cursor in the tree or the grid, else the current folder; highlighted while the drag is over the panel
- The copy: files and folders, recursive, off the main thread for large files, a name collision resolved by a stated rule, a source already inside the project refused or moved by a stated rule
- After the copy, one rescan; sidecars and the import pipeline run as for any new file, and E.4.4's ignore roster applies
- Refusals: outside the Assets panel, during Play, with no project open — each with its Console line

### F.3.2 New Script and New Scene from the browser · P0 · M · depends: E.4.3, 4.4.1, 4.6.1
_(Measured: the browser's header has one button, "New Material", and its context menu offers New Folder and New Asset ▸
Material. A `.ts` file is written outside the editor, and a new scene exists only after File ▸ New Scene and a Save
As dialog.)_
**Goal:** create the two things a game is made of where the rest of the project's files are.
**Deliverable:** New Asset ▸ Script (in a TypeScript project) writes a `.ts` file with the lifecycle hooks, named in
place like a new material; New Asset ▸ Scene writes an empty scene file; the header's single button becomes a New menu
carrying Material, Script, Scene and Folder; double-clicking a script opens it in the system's editor for that file.
Subtasks:
- New Asset ▸ Script: a starter with `onInit`, `onUpdate` and `onDestroy` that type-checks clean under the project's `tsconfig.json`; offered only in a TypeScript project (4.6.1's gating)
- New Asset ▸ Scene: a valid, empty scene v1 document (`docs/09` §2) that opens without a warning
- One New menu in the header and the same entries in the context menu; inline naming and the collision rule shared with New Material
- Double-click on a script hands the file to the OS's default application; a failure is one Console line
- The new file's `.meta`, its scan and — for a script — its first transpile happen without a manual Refresh

### F.3.3 Scenes as assets, and folder icons · P0 · M · depends: 2.5.1, E.4.1, E.4.3, E.6.1
_(Measured: there is no Scene asset kind. A `.scene.json` classifies on its last extension as Text and draws a hatched
tile labelled "JSON"; a double-click on a file only selects it; `openSceneFile` is reachable from the File menu's
dialog and from project open alone. The folder tree's rows draw a label and a count with no icon, and list rows are
plain text, although `AERO_ICON_FOLDER` and `AERO_ICON_FOLDER_OPEN` are in the icon set.)_
**Goal:** a scene should look like a scene and open like one, and a folder should look like a folder.
**Deliverable:** a Scene asset kind with its own icon tile; double-clicking a scene opens it through the
unsaved-changes guard; the open scene is marked in the browser; the folder tree and the list view draw folder icons,
open and closed.
Subtasks:
- `AssetKind::Scene`, decided before the extension tables as Script is; filter, kind dot, tile and list row
- Double-click opens the scene: the unsaved-changes guard, the project-containment rule (E.4.2) and Play's scene lock all apply
- The open scene's tile and row are marked; the mark follows Save As and a rename
- Folder icons in the tree (open / closed) and the list; the grid's drawn folder shape is kept or replaced by one decision
- Save Scene on a scene never saved: re-run the September observation (no write, no dialog, no log line) on the real editor, and fix it if it reproduces — the code today routes that case to the Save dialog

### F.3.4 Scene switcher in the breadcrumb · P0 · M · depends: F.3.3, E.6.2
_(Measured: the toolbar's breadcrumb is display-only text — "project / scene" and a dirty dot, no button and no menu.
"Open Recent" lists projects, not scenes. Moving to another scene is File ▸ Open Scene and a native file dialog.)_
**Goal:** move between a project's scenes without leaving the editor's own window.
**Deliverable:** the breadcrumb's scene name is a button that opens a list of the project's scenes — name, folder and
picture — with the current one marked and a search field once the list is long; choosing one opens it through the same
path as a double-click. New Scene is reachable from the list.
Subtasks:
- The scene crumb as a button, with the dirty dot kept; disabled in Play with the scene lock's reason
- The list: every scene the asset database knows, most recently opened first, the current one marked; search when it outgrows the popup
- Recently opened scenes recorded per project in `Library/editor-state.json` (`docs/09` §4.10)
- Choosing a scene shares F.3.3's open path; New Scene in the list shares F.3.2's
- Keyboard: arrows and Enter choose, Escape closes — bound by hand, since navigation keys are off in the editor

### F.3.5 Scene pictures · P1 · M · depends: F.3.3, E.1.1, E.4.5
_(E.4.5 left "a third thumbnail producer (mesh, scene, audio waveform)" and "a persisted thumbnail store under
`Library/`" as handoffs. A scene's picture is cheapest taken where it already exists: the viewport's image at the
moment of saving, read back through `Device::readbackTexture`, which E.1.1 added.)_
**Goal:** tell scenes apart by looking, in the browser and in the switcher.
**Deliverable:** saving a scene stores a small picture of the viewport under `Library/`; the scene's tile and its row
in F.3.4's list show it; a scene never saved in this editor shows the scene icon.
Subtasks:
- Capture at save: the viewport's resolved image without editor chrome, downsampled; the save itself never waits on it and never fails because of it
- The store under `Library/` keyed by the scene's GUID — machine-local, derived, never committed; its envelope specified in `docs/09`
- The tile and the switcher read the store through the thumbnail service; a missing or stale picture falls back to the icon
- A deleted or renamed scene's picture is released or follows it

---

## Epic F.4 — Iteration speed · editor

**Goal:** a save should be seen, not waited for. Scripts already reload into a running game (4.3.3, 4.7.4), but the
change takes about 2.5 s to appear, and almost none of that is work: the transpile is 10–25 ms, and the rest is the
polling watcher's 2 s settle and up to 1 s of cooldown.
**Definition of Done:** a saved script reaches the running game within a recorded budget an order of magnitude under
today's, with half-written files still never imported, and what reloads while a game runs is written down and proven.

### F.4.1 Native file watcher · P0 · L · depends: 3.1.4, 4.3.3
_(3.1.4 kept the watcher on polling until "a Phase 4 script-reload loop where 1–2 s of poll latency is felt on every
save", and wrote its `poll()` seam for the day that came. It came, measured three times: 4.3.3's spike, 2.19–3.2 s from
a save to new JavaScript, flat over 1, 10 and 50 scripts; 4.7.3 + 4.7.4's validation, 2.5–2.7 s to a visible change;
Story 2's demo on `main`, 2.23–2.56 s, felt as a pause. `docs/08` records the backend as unowned.)_
_(The 2 s settle is not arbitrary and must not simply be shortened: `isSettled` holds two conditions that each close
a hole the other cannot — NTFS presents a stale last-write-time for the whole of a long copy, and FAT32 / HFS+ quantise
it to 2 s / 1 s. A native backend changes what is known — the OS says *that* a file changed — and the settle rule is
restated on that footing, not dropped.)_
**Goal:** the editor learns of a saved file when the OS does, instead of finding it on a later sweep.
**Deliverable:** an OS-notification backend behind the watcher's `poll()` seam on each editor platform — FSEvents on
macOS, `ReadDirectoryChangesW` on Windows, inotify on Linux — filling the same `WatchSnapshot`, with polling kept as
the fallback, and a recorded save-to-publish budget; the target is under half a second, confirmed or corrected by the
task's own measurement.
Subtasks:
- A spike first: each OS's event latency and coalescing, and what a slow copy and an editor's write-to-temp-then-rename look like as events
- Three backends behind one seam, using the OS APIs directly — no new dependency; started and stopped with the project
- The settle rule restated for events: a file is imported once it is whole, on NTFS's stale stamp and on a coarse-mtime volume alike; the two holes each keep a test
- Fallback: a backend that fails to start, overflows or drops its stream returns the watcher to polling, with one Console line
- The budget measured end to end — save to transpiled publish, and save to a visible change in Play — and recorded beside the three earlier measurements

### F.4.2 What reloads while the game runs · P0 · M · depends: F.4.1, 4.7.4
_(Measured: a script published while Playing or Paused reaches the game at once; published in Edit it is kept for the
next Play. The scene's asset ledger retires and reloads a model, a `.blend` or a material when its content hash
changes. Whether a texture-only edit reaches the viewport is **not measured**; a material's thumbnail is known not to
follow one (`docs/08`). A script whose transpile failed keeps running its last good build while the Console shows the
error (4.7.4's handoff).)_
**Goal:** know exactly what a save changes in a running game, and make the cases that should work, work.
**Deliverable:** a reload table — asset kind × Edit / Play → reloads, or does not and why — in which every row is
proven on the real editor inside F.4.1's budget, for scripts, materials, models and textures; a script running its
last good build is visibly marked as such; the kinds that do not reload are named as v2's "asset hot-reload
everywhere", not left to be discovered.
Subtasks:
- Measure every row first: script, material, model, texture, prefab and scene file, in Edit and in Play
- Fix the rows this phase owns: a texture edit reaching the materials that use it, if the measurement shows it does not
- A script whose last transpile failed: the stale build is named in the Console once and marked where the script is shown
- The table in the editor's documentation, and the gate artifact's script and material used as its demo

---

**Phase gate:** Open the editor and it fills the screen; drag a model in from the file manager and it imports; click the object under the cursor, even one standing behind another object's bounding box, and delete it with the Delete key; select a camera and see what it sees; create a script and a second scene from the browser and switch scenes from the breadcrumb; save a script while the game plays and see the change at once.
