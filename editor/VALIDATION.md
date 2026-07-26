# Editor gate ledger — `aero_editor` (tasks 2.1.1, 2.1.3, 2.2.1)

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

## How to validate one OS (for the pending ones)

1. **Build** (`AERO_REFLECT_TOOLS`/`AERO_SHADER_TOOLS` not required — AC-9): `cmake --preset
   <os>-debug && cmake --build --preset <os>-debug`.
2. **Delete any existing ini** (`<basePath>/aero_editor.ini`, next to the built exe) to see the
   first-run default layout, then run `aero_editor`.
3. **Look at the window**: "Aero Editor" titled, a full-window dockspace with panels, each a
   one-line placeholder ("... — placeholder (task 2.2.x)"). **Note:** task 2.1.3 changed what the
   binary registers — the current build ships **5** panels (Hierarchy left, Inspector right,
   Viewport center, and Console + Assets **tabbed together** at the bottom) and a File/Edit/View
   menu bar. The 2.1.1 records below were originally written against a 4-panel, menu-bar-less build;
   validate what the current binary registers, not the historical list.
4. **Drag a tab out, redock it, split a panel, tab two panels together** — confirm all four
   operations work smoothly by mouse.
5. **Rearrange, then quit** (the window close box, `File > Exit`, or ⌘/Ctrl+Q — **Esc no longer
   quits**, task 2.1.3 D7: Esc is the universal *dismiss* key an editor needs for cancelling drags,
   popups and renames) **and relaunch** — confirm the exact arrangement you left it in comes back.
6. **On a HiDPI/Retina display**: confirm fonts and widget metrics are crisp — not blurry, not
   rendered at half-size relative to the rest of the desktop.
7. **Record your results below.**

## Validation records

### macOS — ✅ PASS (2026-07-25)

Machine: MacBook Pro (Apple M1 Pro), Metal backend.

- **Window opens** — PASS
- **Panels dockable (mouse)** — PASS (human: drag out / redock / split / tab by mouse)
- **Rearrange persists (mouse, actual restart)** — PASS (human: rearranged, quit, relaunched —
  arrangement restored)
- **First-run default layout** — PASS (DockBuilder split observed in the saved ini, and confirmed
  visually)
- **HiDPI crisp** — PASS (human: Retina, crisp)
- **Clean quit** — PASS

Notes: GPU smoke test (`aero_editor_imgui_test`) green under `AERO_REQUIRE_GPU=1`. Human visual pass
performed against the current 2.1.3 binary (5 panels + menu bar), per the note in step 3.

### Windows — ⏳ pending

Needs a native run (D3D12). No checks recorded yet.

### Linux — ⏳ pending

Needs a native run (real Vulkan; **not** lavapipe/CI). No checks recorded yet.

**Task 2.1.1 gate status: OPEN — macOS ✅, Windows/Linux pending.** The mouse-driven visual half is
confirmed on macOS (2026-07-25); the gate closes when the Windows and Linux records are filled by a
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
- No mouse or keyboard interaction was performed; every OS marked "pending" below needs a human.

## Known-and-expected, NOT a defect

- **A pre-2.1.3 `aero_editor.ini` has no `Assets` entry** (that panel did not exist), so on the
  first launch after upgrading, `Assets` floats loose in the middle instead of tabbing with
  `Console`. This is ImGui behaviour for a window with no saved dock entry, not a bug — `View >
  Reset Layout` re-docks everything. Delete the ini for a true first-run check.
- **One-frame dockspace settle.** On the very first drawn frame the dockspace spans the full
  viewport including the strip under the menu bar, because `DockSpaceOverViewport` reads the work
  area the menu bar reserved the *previous* frame. Frame 2 onward is correct; it is invisible in a
  running app. Do not "fix" it with a manual `GetFrameHeight()` offset — that double-counts.

## How to validate one OS (for the pending ones)

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
13. **Record your results below.**

## Validation records

### macOS — ✅ PASS (2026-07-25)

Machine: MacBook Pro (Apple M1 Pro), Metal.

- **Menu bar (3 menus, labels)** — PASS
- **Disabled items + tooltips** — PASS (tooltips name 2.5.1 / 2.4.1)
- **View toggles / close-'X' sync** — PASS (both directions; close-'X' clears the checkbox)
- **First-run layout (Console+Assets tabbed)** — PASS (`Console` and `Assets` share
  `DockId=0x00000006` at tab indices 0/1 in the saved ini — same node ⇒ tabbed; confirmed visually)
- **Reset Layout** — PASS (rebuilds in the frame it is clicked)
- **Esc does NOT quit** — PASS (Esc does nothing)
- **Quits: close box / File > Exit / ⌘-Ctrl+Q** — PASS / PASS / PASS
- **Layout persists (real restart)** — PASS
- **Unfocused throttle visible** — PASS (visible drop when unfocused, recovers on refocus)
- **HiDPI crisp** — PASS (Retina, crisp)

Notes: both editor ctest targets green; `editor: shell ready (5 panels, layout: default)` then
`layout: restored` observed, ini byte-identical across runs. All four AC-6 quit paths now covered:
the three human ones here plus `EventType::Quit` (SIGTERM), which exits with no crash and no ASan
report.

### Windows — ⏳ pending

Needs a native run (D3D12). No checks recorded yet.

### Linux — ⏳ pending

Needs a native run (real Vulkan; **not** lavapipe/CI). No checks recorded yet.

**Task 2.1.3 gate status: OPEN — macOS ✅, Windows/Linux pending.** Every human check passed on
macOS (2026-07-25): the menu bar and its disabled-item tooltips, View toggles and close-'X' sync,
Console+Assets tabbed, `Reset Layout`, Esc not quitting, all three human quit paths, layout
persistence across a real restart, the unfocused throttle, and HiDPI crispness.
**Epic 2.1 closes when both sections' Windows and Linux records are filled** by a code-free on-hardware
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
- No mouse or keyboard interaction was performed; every OS marked "pending" below needs a human.

## Known-and-expected, NOT a defect

- **Duplicated entities keep their source name verbatim** (D20) — no `" (1)"` suffixing. Two rows
  with the same label is expected; row identity is the ID stack (index+generation), never the text.
- **Context-menu Delete/Duplicate on a row outside the current selection acts on that row alone**;
  on a row inside the selection, it acts on the whole selection — mirroring the drag rule (E16). This
  is the plan's own default (§O-2), not a bug.
- **Entity ordering within a parent is not user-controllable** (D14) — children draw in attach order,
  and there is deliberately no way to reorder siblings in this task.

## How to validate one OS (for the pending ones)

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
8. **Multi-select drag** — this one shipped broken in 2.2.1 and was found by hand, not by CI, so
   test it deliberately: select three rows, then press and drag **one of the already-selected rows**
   onto a new parent.
   **All three must move**, each keeping its own children. Pressing down on a selected row must not
   collapse the selection to that row — that collapse is what made this look unsupported. Then press
   and *release* on a selected row **without** moving the mouse: that one still must collapse the
   selection to just it (a plain click is still a plain click).
9. **Duplicate**: Ctrl/Cmd+D and the context menu; confirm the copy carries the name, the children in
   the same order, and (via the still-placeholder Inspector, or by eye in the viewport once 2.2.3
   lands) its components.
10. **Delete while renaming**: begin a rename, then press Delete via another route — the field closes
    cleanly (E24).
11. **Root order**: create A, B, C; delete B; confirm A and C do **not** reorder (AC-16).
12. **Quit and relaunch**: the layout persists and the panel is still docked left.
13. **Record your results below.**

## Validation records

Each OS records the same twelve checks. Copy this checklist into the OS's section below as you go,
appending ` — PASS` (or FAIL) and what you observed to each line.

- **Seeded scene (3 entities)**
- **Rename (F2 / dbl-click / Enter / Esc)**
- **Create Empty / Create Child**
- **Indentation + expander**
- **Reparent (drag, single row)**
- **Reparent (drag, multi-selection — the whole selection moves, subtrees intact)**
- **No drop highlight on an illegal drop (AC-15)**
- **Multi-select + Delete**
- **Duplicate (name + children + components)**
- **Delete-while-renaming**
- **Root order stable**
- **Layout persists**

### macOS — ✅ PASS (2026-07-26)

Machine: MacBook Pro (Apple M1 Pro), Metal. Human mouse/keyboard pass against `2b25435`.

- **Seeded scene (3 entities)** — PASS
- **Rename (F2 / dbl-click / Enter / Esc)** — PASS
- **Create Empty / Create Child** — PASS
- **Indentation + expander** — PASS
- **Reparent (drag, single row)** — PASS
- **Reparent (drag, multi-selection — the whole selection moves, subtrees intact)** — PASS
- **No drop highlight on an illegal drop (AC-15)** — PASS
- **Multi-select + Delete** — PASS
- **Duplicate (name + children + components)** — PASS
- **Delete-while-renaming** — PASS
- **Root order stable** — PASS
- **Layout persists** — PASS

Notes: this pass closes **AC-15**, the one acceptance criterion with no mechanical proof at all
(sabotage S5's target — the absence of a drop highlight cannot be asserted without synthesized
mouse input). It also confirms the multi-select drag fix (PR #44) in the gesture that exposed the
original defect.

**The multi-select drag row is the reason this pass matters.** That defect shipped past a green
3-OS CI matrix, a high-rigor code review, and 44 tier-0 cases, and was found by hand here — because
every automated layer drove `reparentTargets()` directly instead of the press → drag → drop gesture
that reaches it. Treat the remaining pending OSes as real verification, not paperwork.

### Windows — ⏳ pending

Needs a native run (D3D12). No checks recorded yet.

### Linux — ⏳ pending

Needs a native run (real Vulkan; **not** lavapipe/CI). No checks recorded yet.

**Task 2.2.1 gate status: OPEN — macOS ✅ (2026-07-26, all twelve checks), Windows/Linux pending.**
Every human check passed on macOS, including AC-15 and the post-fix multi-select drag. The gate
closes when the Windows and Linux records are filled by a code-free on-hardware follow-up (the
0.5.3/1.4.2/2.1.1/2.1.3 precedent) — no code change is expected to be needed.

---

# Task 2.2.2 — reflection-driven inspector

Task 2.2.2's deliverable: the `"Inspector"` placeholder is replaced by a real panel that lists the
primary-selected entity's registered components and renders a working editor for every reflected
field, driven entirely by generated `entt::meta` — so a brand-new `[[engine::component]]` type gets a
working UI with zero editor code. New field-level annotations (`AERO_RANGE`/`AERO_COLOR`) and the
`std::string` reflectable category are proven end-to-end. CI proves the structural/mechanical half
automatically: the new gated `aero_editor_inspector_test` (tier-0, no GPU) drives the model builder
and the seam over `InspectorProbe` — a fixture no editor source names — covering model
ordering/filtering, every field kind's range/colour metadata, the `const World&` compile-time pin, the
17-arithmetic-type coverage pins (`long`/`char16_t`), seam round-trips and rejections, the range and
width clamps, `addComponent`/`removeComponent` semantics, a meta-less runtime-registered type, scratch
capacity reuse, and the AC-12 drift pin against the real production aggregator; `aero_editor_imgui_test`
drives the REAL `InspectorPanel` over the seeded scene's Cube entity through `EditorApp::tick()` — an
unbalanced ImGui call here would abort via `IM_ASSERT` in the Debug ImGui build, so a green run proves
the panel's ImGui balance across every drawn state it reaches (single selection, multi-selection, and
a component removed-then-re-added behind the panel's back between ticks).

It **cannot** mechanically prove the mouse-driven half: dragging a `DragScalar`/`DragFloat3` and
watching the value track the cursor, the Quat/euler cache **not jittering** while dragging (D4/E11),
picking a colour and confirming an HDR value **> 1** survives, typing into the string field and
watching it commit only on blur, and the *Add Component*/*Remove Component* menus by mouse. That half
needs a person at the machine — the same "cannot be synthesized without real input" boundary every
prior editor task in this ledger has recorded.

## What was mechanically verified (this implementation pass, macOS, Apple M1 Pro / Metal)

- `aero_editor_core`, `aero_editor`, `aero_editor_shell_test`, `aero_editor_imgui_test` and the new
  `aero_editor_inspector_test` build clean on `macos-debug` and `macos-release`; `ctest` is green at
  **93** entries on both (up from 83 — +9 `reflect-gen.*` process cases and +1 new gated target).
- `AERO_REQUIRE_GPU=1 ctest --preset macos-debug -R aero_editor` passes every editor target;
  `aero_editor_imgui_test` now runs 4 `TEST_CASE`s (was 3), including the new task-2.2.2 case driving
  the Inspector over the seeded Cube entity.
- `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` (fresh configure): `ctest -N` is **5** (was 5),
  `aero_editor` still builds, and launching it non-interactively shows exactly **one**
  `editor: built without AERO_REFLECT_TOOLS — inspector field editing disabled` log line — the
  structural proof of D12's graceful degradation.
- Launched `aero_editor` non-interactively after deleting the ini: the log line
  `editor: shell ready (5 panels, 3 entities, layout: default)` appears, the ini still gains
  `[Docking][Data]` entries for all five panels (the `Inspector` key survives byte-identically, D20),
  and a zero-ERROR grep over the run's log returns nothing — proving the panel's steady-state draw
  makes no rejected `World` or seam call.
- All **seven** §V3 sabotage proofs were performed and reverted (see the implementation report): S1
  (reverse the `.data` emission order) reds `aero_reflect_meta_test`'s order pin; S2 (drop the range
  clamp) reds the range-clamp case while the width clamp still passes; S3 (hardcode the walk to the
  five built-ins) reds the `InspectorProbe` model case; S4 (stop emitting `.custom`) reds both
  `reflect-gen.annotations_meta` and `aero_reflect_meta_test`'s runtime custom assertions; S5 (replace
  the saturating narrow with a plain `static_cast`) reds the `300 → 255` width-clamp case ALONE — the
  `−1 → 0` line stayed green even unclamped, confirming it is coverage, never proof (the 2.2.1 C1
  lesson); S6 (skip the aggregator call in `registerEditorReflection`) reds the AC-12 drift pin while
  `aero_editor_imgui_test` still passed in full, confirming the tools-OFF degradation path is genuinely
  separate; S7 (truncate the arithmetic type list to 15) reds the `long`/`char16_t` O2 pins and fired
  the defensive-skip `AERO_LOG_ERROR` for both fields.
- No mouse or keyboard interaction was performed by this implementation pass — every OS below is
  recorded pending a human.

## Known-and-expected, NOT a defect

- **The Quat editor re-derives from the model on release** (E11) — dragging the euler-degree triplet,
  releasing, then opening the same field again may show a slightly different (but rotationally
  equivalent) triplet near the Y (yaw) pole, since `eulerAngles` is not injective and the asin-based
  yaw axis loses ~3.5e-4 rad of precision at ±90°. The rotation itself is exact; only the displayed
  numbers can drift. This is documented, not a bug — see `quat.hpp`'s own comment.
- **A scene file loaded with an out-of-range `fovYRadians` displays unclamped** until the field is
  first dragged (R11) — the seam clamps on WRITE; `loadScene`'s generated `aeroReadJson` is unrelated
  and unchanged.
- **Subtask ④ ("route edits through 2.4 commands") stays unchecked** — `component_ops` is the seam
  2.4.2 will wrap, not rewrite; this task deliberately does not add an undo/redo stack (D2).

## How to validate one OS (for the pending ones)

1. **Build**, delete `build/<os>-debug/editor/aero_editor.ini`, run `aero_editor`.
2. **Select the seeded Cube** (Hierarchy panel) — the Inspector lists *Transform* and *MeshRenderer*,
   each with a full registration-name tooltip on hover over its header.
3. **Transform.position**: drag each axis of the Vec3 field — the cube moves in the viewport (once
   2.2.3 lands) or the numbers track the cursor smoothly; type a value directly (click into a
   component, type, Tab/Enter) and confirm it commits.
4. **Transform.rotation**: drag the euler-degree triplet and **watch for digit jitter while
   dragging** — if the displayed numbers jump discontinuously mid-drag, the cache is not being
   consulted (a real regression, not E11's documented pole re-derivation on RELEASE).
5. **MeshRenderer.color**: open the colour picker, push a channel above 1.0 (HDR) via the picker's
   own extended-range control, close it, and confirm the value **> 1** survives a re-open.
6. **MeshRenderer.primitive**: drag the selector across 0..2 and confirm it clamps at both ends (no
   value outside the range is ever shown or stored).
7. **Add Component**: click *+ Add Component* on the Cube, confirm the popup lists only ABSENT
   registered types by their short name (e.g. `Camera`, `DirectionalLight`, `PointLight`), pick
   *PointLight*, and confirm its default fields appear immediately.
8. **Remove Component**: right-click the new *PointLight* header → *Remove Component* — it disappears
   from the Inspector without affecting Transform/MeshRenderer.
9. **Multi-select**: select all three seeded entities and confirm the header reads
   `... (3 selected)`; the fields shown remain the PRIMARY's only (2.2.2 does not implement
   multi-entity editing — the note itself is the deliverable, not simultaneous editing).
10. **Empty selection**: click empty space in the Hierarchy to clear the selection — the Inspector
    shows "No entity selected." centred, and no widget IDs collide (nothing logged).
11. **Quit and relaunch**: the layout persists and the panel is still docked right.
12. **Record your results below.**

## Validation records

Each OS records the same eleven checks. Copy this checklist into the OS's section below as you go,
appending ` — PASS` (or FAIL) and what you observed to each line.

- **Cube selected — Transform + MeshRenderer listed, tooltip shows full name**
- **Transform.position drag + typed entry**
- **Transform.rotation drag — no digit jitter while dragging**
- **MeshRenderer.color — HDR value > 1 survives**
- **MeshRenderer.primitive clamps across its range**
- **Add Component lists only absent types; PointLight's defaults appear**
- **Remove Component removes cleanly**
- **Multi-select shows "(N selected)"**
- **Empty selection shows "No entity selected."**
- **Layout persists**

### macOS — ⏳ pending

### Windows — ⏳ pending

Needs a native run (D3D12). No checks recorded yet.

### Linux — ⏳ pending

Needs a native run (real Vulkan; **not** lavapipe/CI). No checks recorded yet.

**Task 2.2.2 gate status: OPEN — all three OSes pending a human mouse/keyboard pass.** The mechanical
half (build, tests, sabotage proofs, non-interactive launch) is green on macOS; no code change is
expected to be needed once the human passes are recorded (the 0.5.3/1.4.2/2.1.1/2.1.3/2.2.1
precedent).
