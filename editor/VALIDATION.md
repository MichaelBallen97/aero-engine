# Editor gate ledger — `aero_editor` (tasks 2.1.1, 2.1.3)

Task 2.1.1's deliverable: an editor window with dockable dummy panels, layout persisted across
restarts, HiDPI scaling checked on all 3 OSes. CI proves the "builds clean on all 3 OSes, imgui
strictly editor-only" half automatically (`aero_editor`/`aero_editor_core` build on all three
lanes; `aero_editor_imgui_test` runs the GPU-gated init→frames→shutdown smoke test under
`AERO_REQUIRE_GPU=1`, proving `ImGuiLayer::create`/`beginFrame`/`endFrame`/teardown are
leak-free on real Metal/D3D12/lavapipe backends). It **cannot** prove the mouse-driven, visual
half — dragging/undocking/redocking/tabbing panels by hand, judging font/UI crispness on a real
HiDPI display, and confirming an *actual restart* (not just a re-exec in the same session)
restores the arrangement a human dragged. That half needs a person at the machine — recorded
here per OS, mirroring `samples/phase-0-cube/VALIDATION.md` and `samples/phase-1-scene/VALIDATION.md`'s
precedent.

## What was mechanically verified (this implementation pass, macOS, Apple M1 Pro / Metal)

These are automated or scripted checks I ran myself and can substantiate — not a substitute for
the visual gate below, which needs a human's eyes and hands.

- `aero_editor_core`, `aero_editor`, and `aero_editor_imgui_test` build clean (`cmake --build
  --preset macos-debug` / `macos-release`).
- `AERO_REQUIRE_GPU=1 ctest --preset macos-debug -R aero_editor_imgui_test` passes: `ImGuiLayer::create`
  succeeds on a real Metal device, 3 `beginFrame`/`drawEditorUi`/`endFrame` cycles all return `true`
  (window presentable), and teardown does not crash.
- Launched `aero_editor` directly (`( aero_editor & pid=$!; sleep 3; kill $pid )`, no interactive
  input): the window opens without crashing, logs show a real Metal device created, and
  `aero_editor.ini` is written on quit containing a real `[Docking][Data]` dockspace tree
  (`DockSpace ID=... Split=X`, four child `DockNode`s) with `Hierarchy`/`Inspector`/`Console`/`Viewport`
  window entries — proving the DockBuilder first-run default layout actually built and ImGui actually
  saved it.
- Re-launched the same way a second time (ini already present): the saved ini is byte-identical
  after the second run, i.e. the persisted layout round-trips through a real load→save cycle without
  drifting — this is the *mechanical* half of "rearrange persists"; it does not exercise a human
  dragging panels by mouse.
- No manual mouse interaction was performed (this pass launched and killed the process
  non-interactively) — undock/redock/split/tab-by-mouse and visual font/UI crispness are **not**
  covered by the above and are marked pending below.

## How to validate one OS (for the pending rows)

1. **Build** (`AERO_REFLECT_TOOLS`/`AERO_SHADER_TOOLS` not required — AC-9): `cmake --preset
   <os>-debug && cmake --build --preset <os>-debug`.
2. **Delete any existing ini** (`<basePath>/aero_editor.ini`, next to the built exe) to see the
   first-run default layout, then run `aero_editor`.
3. **Look at the window**: "Aero Editor" titled, a full-window dockspace with panels, each a
   one-line placeholder ("... — placeholder (task 2.2.x)"). **Note:** task 2.1.3 changed what the
   binary registers — the current build ships **5** panels (Hierarchy left, Inspector right,
   Viewport center, and Console + Assets **tabbed together** at the bottom) and a File/Edit/View
   menu bar. The 2.1.1 rows below were originally written against a 4-panel, menu-bar-less build;
   validate what the current binary registers, not the historical list.
4. **Drag a tab out, redock it, split a panel, tab two panels together** — confirm all four
   operations work smoothly by mouse.
5. **Rearrange, then quit** (the window close box, `File > Exit`, or ⌘/Ctrl+Q — **Esc no longer
   quits**, task 2.1.3 D7: Esc is the universal *dismiss* key an editor needs for cancelling drags,
   popups and renames) **and relaunch** — confirm the exact arrangement you left it in comes back.
6. **On a HiDPI/Retina display**: confirm fonts and widget metrics are crisp — not blurry, not
   rendered at half-size relative to the rest of the desktop.
7. **Record the row below.**

## Validation table

| OS | Status | Date | Machine / GPU | Window opens | Panels dockable (mouse) | Rearrange persists (mouse, actual restart) | First-run default layout | HiDPI crisp | Clean quit | Notes |
|----|--------|------|----------------|---------------|---------------------------|-----------------------------------------------|----------------------------|--------------|------------|-------|
| macOS | ✅ PASS | 2026-07-25 | MacBook Pro (Apple M1 Pro), Metal backend | PASS | PASS (human: drag out / redock / split / tab by mouse) | PASS (human: rearranged, quit, relaunched — arrangement restored) | PASS (DockBuilder split observed in the saved ini, and confirmed visually) | PASS (human: Retina, crisp) | PASS | GPU smoke test (`aero_editor_imgui_test`) green under `AERO_REQUIRE_GPU=1`. Human visual pass performed against the current 2.1.3 binary (5 panels + menu bar), per the note in step 3. |
| Windows | ⏳ pending | — | — | — | — | — | — | — | — | needs native run (D3D12) |
| Linux | ⏳ pending | — | — | — | — | — | — | — | — | needs native run (real Vulkan; NOT lavapipe/CI) |

**Task 2.1.1 gate status: OPEN — macOS ✅, Windows/Linux pending.** The mouse-driven visual half is
confirmed on macOS (2026-07-25); the gate closes when the Windows and Linux rows are filled by a
code-free on-hardware follow-up (the Phase 0/1 precedent, 0.5.3/1.4.2).

---

# Task 2.1.3 — editor app shell & main loop

Task 2.1.3's deliverable: `aero_editor` hosting the engine frame loop, with a File/Edit/View menu
bar and a panel registry; opens and quits cleanly. CI proves the structural half automatically
(`aero_editor_shell_test` runs the registry + pacing-policy battery on every lane in both configs
with no GPU; `aero_editor_imgui_test` drives `EditorApp::tick()` through create → 3 presented frames
→ the closed-panel `Begin`/`End` regression → `requestLayoutReset` → quit → teardown on real
Metal/D3D12/lavapipe under `AERO_REQUIRE_GPU=1`). It **cannot** prove the mouse- and
keyboard-driven half: reading the tooltips on the disabled menu items, toggling panels from the
View menu, closing a panel with its 'X' and seeing the View checkbox follow, watching Console and
Assets come up **tabbed together**, rebuilding the layout with `View > Reset Layout` after
rearranging by hand, quitting with ⌘/Ctrl+Q and with `File > Exit`, confirming **Esc does not
quit**, and seeing CPU/GPU use drop when the window loses focus. That half needs a person at the
machine.

## What was mechanically verified (this implementation pass, macOS, Apple M1 Pro / Metal)

- `aero_editor_core`, `aero_editor`, `aero_editor_shell_test` and `aero_editor_imgui_test` build
  clean on `macos-debug` and `macos-release`; `ctest` is green at **83** entries on both (was 82).
- `AERO_REQUIRE_GPU=1 ctest --preset macos-debug -R aero_editor` passes both editor targets.
- Launched `aero_editor` non-interactively (`( aero_editor & pid=$!; sleep 3; kill $pid )`) after
  deleting `build/macos-debug/editor/aero_editor.ini`: the log line
  `editor: shell ready (5 panels, 0 entities, layout: default)` appears (task 2.2.1 widened the log
  line with an entity count; at 2.1.3's own validation time it read `layout: default` with no entity
  count, since the World/seeding did not exist yet), the ini is written, and it contains a
  real `[Docking][Data]` tree with entries for all five panel names
  (`Hierarchy`/`Inspector`/`Viewport`/`Console`/`Assets`) — proving the data-driven DockBuilder
  layout actually built and ImGui saved it. A second launch logs `layout: restored` and leaves the
  ini byte-identical.
- **Console+Assets tabbing is proven from that same ini, not merely inferred from both being
  present:** two windows are tabbed iff they share one `DockId`, and the saved tree has
  `[Window][Console] DockId=0x00000006,0` beside `[Window][Assets] DockId=0x00000006,1` — the same
  node at tab indices 0 and 1. (Five panels each *having* a dock entry would also be true if the
  builder had put them in five separate nodes, so the entry list alone does not establish this.)
- No mouse or keyboard interaction was performed; every row marked "pending" below needs a human.

## Known-and-expected, NOT a defect

- **A pre-2.1.3 `aero_editor.ini` has no `Assets` entry** (that panel did not exist), so on the
  first launch after upgrading, `Assets` floats loose in the middle instead of tabbing with
  `Console`. This is ImGui behaviour for a window with no saved dock entry, not a bug — `View >
  Reset Layout` re-docks everything. Delete the ini for a true first-run check.
- **One-frame dockspace settle.** On the very first drawn frame the dockspace spans the full
  viewport including the strip under the menu bar, because `DockSpaceOverViewport` reads the work
  area the menu bar reserved the *previous* frame. Frame 2 onward is correct; it is invisible in a
  running app. Do not "fix" it with a manual `GetFrameHeight()` offset — that double-counts.

## How to validate one OS (for the pending rows)

1. **Build** (`AERO_REFLECT_TOOLS`/`AERO_SHADER_TOOLS` not required): `cmake --preset <os>-debug &&
   cmake --build --preset <os>-debug`.
2. **Delete `build/<os>-debug/editor/aero_editor.ini`** (it sits next to the exe), then run
   `aero_editor`.
3. **Menu bar**: exactly three menus — File, Edit, View. Open each. `File` has New Scene / Open
   Scene... / Save Scene / Save Scene As... (all greyed) / separator / **Exit**; `Edit` has Undo /
   Redo (both greyed); `View` lists all five panels with checkboxes, a separator, then Reset Layout.
4. **Hover a greyed item** — a tooltip must appear naming the owning task
   ("Not implemented yet — task 2.5.1" / "… task 2.4.1").
5. **First-run layout**: Hierarchy left, Inspector right, Viewport centre, and **Console + Assets
   tabbed together** at the bottom. No empty grey rectangle anywhere.
6. **Toggle a panel from View** — it hides and shows. **Close a panel with its 'X'** — its View
   checkbox clears on the next frame.
7. **Rearrange by mouse** (drag a tab out, redock, split, tab two together), then **View > Reset
   Layout** — the default arrangement comes back immediately, in that frame.
8. **Press Esc** — nothing happens (the editor must NOT quit).
9. **Quit three ways**, relaunching each time: the window close box, `File > Exit`, and ⌘Q (macOS) /
   Ctrl+Q (Windows/Linux). Each must exit cleanly with no crash.
10. **Rearrange, quit, relaunch** — the arrangement returns (real restart, not a re-exec).
11. **Click another application** so the editor loses focus, and watch CPU/GPU use in Activity
    Monitor / Task Manager / `top` — it must visibly drop (the 20 Hz unfocused cap), then recover
    when the editor is focused again.
12. **On a HiDPI display**: fonts and widget metrics crisp.
13. **Record the row below.**

## Validation table

| OS | Status | Date | Machine / GPU | Menu bar (3 menus, labels) | Disabled items + tooltips | View toggles / close-'X' sync | First-run layout (Console+Assets tabbed) | Reset Layout | Esc does NOT quit | Quits: close box / File>Exit / ⌘-Ctrl+Q | Layout persists (real restart) | Unfocused throttle visible | HiDPI crisp | Notes |
|----|--------|------|---------------|-----------------------------|----------------------------|-------------------------------|-------------------------------------------|--------------|--------------------|--------------------------------------------|--------------------------------|-----------------------------|--------------|-------|
| macOS | ✅ PASS | 2026-07-25 | MacBook Pro (Apple M1 Pro), Metal | PASS | PASS (tooltips name 2.5.1 / 2.4.1) | PASS (both directions; close-'X' clears the checkbox) | PASS (`Console` and `Assets` share `DockId=0x00000006` at tab indices 0/1 in the saved ini — same node ⇒ tabbed; confirmed visually) | PASS (rebuilds in the frame it is clicked) | PASS (Esc does nothing) | PASS / PASS / PASS | PASS (real restart) | PASS (visible drop when unfocused, recovers on refocus) | PASS (Retina, crisp) | Both editor ctest targets green; `editor: shell ready (5 panels, layout: default)` then `layout: restored` observed, ini byte-identical across runs. All four AC-6 quit paths now covered: the three human ones here plus `EventType::Quit` (SIGTERM), which exits with no crash and no ASan report. |
| Windows | ⏳ pending | — | — | — | — | — | — | — | — | — | — | — | — | needs native run (D3D12) |
| Linux | ⏳ pending | — | — | — | — | — | — | — | — | — | — | — | — | needs native run (real Vulkan; NOT lavapipe/CI) |

**Task 2.1.3 gate status: OPEN — macOS ✅, Windows/Linux pending.** Every human column passed on
macOS (2026-07-25): the menu bar and its disabled-item tooltips, View toggles and close-'X' sync,
Console+Assets tabbed, `Reset Layout`, Esc not quitting, all three human quit paths, layout
persistence across a real restart, the unfocused throttle, and HiDPI crispness.
**Epic 2.1 closes when both tables' Windows and Linux rows are filled** by a code-free on-hardware
follow-up (the Phase 0/1 precedent, 0.5.3/1.4.2) — no code change is expected to be needed.

# Task 2.2.1 — hierarchy panel

Task 2.2.1's deliverable: the `"Hierarchy"` placeholder is replaced by a real panel that renders the
live `World`'s entity forest and lets the user create / delete / duplicate / rename / reparent
entities and multi-select, with the `Selection` shared for 2.2.2/2.2.3 to read later. CI proves the
structural half automatically (`aero_editor_shell_test`'s new `tests/editor/hierarchy_test.cpp` TU
covers `Selection`, `entity_ops`, `RootOrder`, `walkForest` and `seedDefaultScene` at tier-0 on every
lane in both configs with no GPU; `aero_editor_imgui_test` now drives the REAL `HierarchyPanel` over
the seeded three-entity scene through `EditorApp::tick()` — create → 3 presented frames → a two-level
tree built behind the panel's back → a selected row → a subtree destroyed behind its back → an
empty-selection duplicate no-op → quit → teardown — on real Metal/D3D12/lavapipe under
`AERO_REQUIRE_GPU=1`; an unbalanced `TreePop`/`PopID` would abort via `IM_ASSERT` in the Debug ImGui
build, so a green run proves I3 for a LEAF row (always `open`) and for a COLLAPSED non-leaf row
(never opened — nothing in this GPU test ever calls `SetNextItemOpen`/`_DefaultOpen`, and no real
mouse click reaches it). It does **not** reach the expanded/nested-descent path — no row in this test
is ever actually open with children below it; that path's own invariants (exactly-once child
visitation, back-to-front root order, the child arena returning to its pre-call size, I3/I4) are
instead proven at tier-0, with no GPU, by `hierarchy_test.cpp`'s `walkForest` cases (review round 2,
Gap 1). It **cannot** prove the mouse- and keyboard-driven half: the inline rename
`InputText` gesture (F2 / double-click / Enter / Escape), drag-and-drop reparenting and the "no drop
highlight on an illegal drop" rule (AC-15 — the ONE acceptance criterion with no mechanical proof at
all, closed only by sabotage S5 below plus a human), Shift/Ctrl+click multi-select by mouse, and the
context menu. That half needs a person at the machine.

## What was mechanically verified (this implementation pass, macOS, Apple M1 Pro / Metal)

- `aero_editor_core`, `aero_editor`, `aero_editor_shell_test` and `aero_editor_imgui_test` build
  clean on `macos-debug` and `macos-release`; `ctest` is green at **83** entries on both — no new
  ctest entry was registered (both new TUs ride existing targets, exactly as planned).
- `AERO_REQUIRE_GPU=1 ctest --preset macos-debug -R aero_editor` passes both editor targets;
  `aero_editor_imgui_test` now runs 3 `TEST_CASE`s (was 1), including the two new task-2.2.1 cases.
- `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` (fresh configure): `ctest -N` is **5** (was 5),
  `aero_editor` still builds, and `aero_editor_shell_test` — including every `hierarchy_test.cpp`
  case — is green: the structural proof the editor's scene layer depends on neither codegen nor
  shaders (2.1.3 AC-9 preserved).
- Launched `aero_editor` non-interactively after deleting the ini: the log line
  `editor: shell ready (5 panels, 3 entities, layout: default)` appears (the seeded Main
  Camera/Directional Light/Cube), the ini gains `[Docking][Data]` entries for all five panels as
  before, and a **zero-ERROR grep** over the run's log
  (`scene: setParent|scene: setName on a dead|scene: copyComponent`) returns nothing — proving the
  panel's steady-state draw makes no rejected `World` call.
- All six §V3 sabotage proofs were performed and reverted (see the implementation report): S1 (the
  D17 remove-before-read ordering) reds the AC-5b case while AC-5a stays green, confirming the C1
  correction; S2 (drop `topMost` from `duplicateEntities`) reds the E9 case; S3 (hardcode the
  registration-table walk) reds the D4/AC-13 value assertion; S4 (make `RootOrder::reconcile` a
  plain rescan) reds the AC-16 case; S6 (`#include <imgui.h>` in `panel_context.hpp`) still compiles
  cleanly in `aero_editor_shell_test`, re-documenting R12 as D24 predicts. S5 (AC-15's "no drop
  highlight") has no mechanical proof and is recorded below as human-pending.
- No mouse or keyboard interaction was performed; every row marked "pending" below needs a human.

## Known-and-expected, NOT a defect

- **Duplicated entities keep their source name verbatim** (D20) — no `" (1)"` suffixing. Two rows
  with the same label is expected; row identity is the ID stack (index+generation), never the text.
- **Context-menu Delete/Duplicate on a row outside the current selection acts on that row alone**;
  on a row inside the selection, it acts on the whole selection — mirroring the drag rule (E16). This
  is the plan's own default (§O-2), not a bug.
- **Entity ordering within a parent is not user-controllable** (D14) — children draw in attach order,
  and there is deliberately no way to reorder siblings in this task.

## How to validate one OS (for the pending rows)

1. **Build**, delete `build/<os>-debug/editor/aero_editor.ini`, run `aero_editor`.
2. **Seeded scene**: the Hierarchy panel lists exactly *Main Camera*, *Directional Light*, *Cube*.
3. **Rename**: F2 on a selected row, and double-click on a row — both open an inline field seeded
   with the current name. Enter commits; clicking away commits; **Escape leaves the name
   unchanged**; committing an empty string falls the label back to `Entity <index>`.
4. **Create**: right-click a row → *Create Empty* (a new root, selected) and *Create Child* (indented
   under the right-clicked row — or the primary, if that row is inside the current selection — with
   an expander appearing on the parent, which auto-opens on the very next frame so the new row is
   never hidden behind a collapsed parent, review round 2 Gap 2). Right-click empty space →
   *Create Empty*.
5. **Indentation**: the child sits one level in, under its parent, with an expander only on entities
   that have children.
6. **Reparent**: drag a row onto another — it becomes that entity's child. Drag it onto empty space —
   it returns to the root list. **Drag a parent onto its own child and confirm NO drop highlight
   appears at all** (AC-15, sabotage S5's target) and that nothing is logged.
7. **Multi-select**: click, Ctrl/Cmd+click, Shift+click across depths; confirm the range follows the
   rows **as displayed**. Press Delete (and Backspace) — every selected subtree disappears.
8. **Duplicate**: Ctrl/Cmd+D and the context menu; confirm the copy carries the name, the children in
   the same order, and (via the still-placeholder Inspector, or by eye in the viewport once 2.2.3
   lands) its components.
9. **Delete while renaming**: begin a rename, then press Delete via another route — the field closes
   cleanly (E24).
10. **Root order**: create A, B, C; delete B; confirm A and C do **not** reorder (AC-16).
11. **Quit and relaunch**: the layout persists and the panel is still docked left.
12. **Record the row below.**

## Validation table

| OS | Status | Date | Machine / GPU | Seeded scene (3 entities) | Rename (F2/dbl-click/Enter/Esc) | Create Empty/Child | Indentation + expander | Reparent (drag) | No drop highlight on illegal drop (AC-15) | Multi-select + Delete | Duplicate (name+children+components) | Delete-while-renaming | Root order stable | Layout persists | Notes |
|----|--------|------|---------------|----------------------------|-----------------------------------|----------------------|---------------------------|--------------------|-----------------------------------------------|--------------------------|------------------------------------------|---------------------------|---------------------|-------------------|-------|
| macOS | ⏳ pending | — | — | — | — | — | — | — | — | — | — | — | — | — | mechanical proof complete (see above); mouse/keyboard pass not yet performed |
| Windows | ⏳ pending | — | — | — | — | — | — | — | — | — | — | — | — | — | needs native run (D3D12) |
| Linux | ⏳ pending | — | — | — | — | — | — | — | — | — | — | — | — | — | needs native run (real Vulkan; NOT lavapipe/CI) |

**Task 2.2.1 gate status: OPEN — mechanical proof complete on macOS; the mouse/keyboard human pass
(all three OSes) is pending a code-free on-hardware follow-up** (the 0.5.3/1.4.2/2.1.1/2.1.3
precedent) — no code change is expected to be needed.
