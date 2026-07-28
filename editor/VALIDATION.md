# Editor gate ledger — `aero_editor` (tasks 2.1.1, 2.1.3, 2.2.1, 2.2.2, 2.2.3, 2.2.4, 2.2.5)

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
and the seam over the D18 proof fixture (`tests/editor/fixtures/inspector_probe.hpp`) — a component
type no editor source names — covering model
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
  five built-ins) reds the D18 proof fixture's model case; S4 (stop emitting `.custom`) reds both
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

### macOS — ✅ PASS (2026-07-27)

Machine: MacBook Pro (Apple M1 Pro), Metal. Human mouse/keyboard pass against `3c6ef97`.

- **Cube selected — Transform + MeshRenderer listed, tooltip shows full name** — PASS
- **Transform.position drag + typed entry** — PASS
- **Transform.rotation drag — no digit jitter while dragging** — PASS
- **MeshRenderer.color — HDR value > 1 survives** — PASS
- **MeshRenderer.primitive clamps across its range** — PASS
- **Add Component lists only absent types; PointLight's defaults appear** — PASS
- **Remove Component removes cleanly** — PASS
- **Multi-select shows "(N selected)"** — PASS
- **Empty selection shows "No entity selected."** — PASS
- **Layout persists** — PASS

Notes: this pass closes the interactive half of **AC-9** and **AC-10**, plus edge cases **E11**
(euler pole re-derivation on release, not mid-drag), **E12** and **E19** — none of which has a
mechanical proof, because each depends on a real drag/click gesture rather than a direct API call.

Two of the checks here are the human half of defects the code review caught by inspection and no
automated layer could reach: the **rotation-jitter** row exercises the `StringEditCache`/`QuatEditCache`
reconciliation (review finding 3), and the **Add Component** row exercises the popup's `PushID`
discipline (review finding 4, which manifests as ImGui's "conflicting ID" overlay rather than a test
failure). Both behave correctly.

### Windows — ⏳ pending

Needs a native run (D3D12). No checks recorded yet.

### Linux — ⏳ pending

Needs a native run (real Vulkan; **not** lavapipe/CI). No checks recorded yet.

**Task 2.2.2 gate status: macOS PASS; Windows/Linux pending a human mouse/keyboard pass.** The
mechanical half (build, tests, sabotage proofs, non-interactive launch) is green on all three OSes
via CI; no code change is expected to be needed once the two remaining human passes are recorded
(the 0.5.3/1.4.2/2.1.1/2.1.3/2.2.1 precedent).

---

# Task 2.2.3 — viewport panel

Task 2.2.3's deliverable: the `"Viewport"` placeholder is replaced by a panel that renders the
editor's live `World` — through the existing `scene_render::SceneRenderer` — into an offscreen
`render::RenderTarget` and displays it filling the dock node, resize-safe, on real Metal/D3D12/
lavapipe. CI proves the structural/mechanical half automatically: the new `render_target_test.cpp`
(tier-0, no GPU) proves `nextTargetExtent` is total, quantised and hysteretic over a 13-row matrix
plus two postcondition sweeps (incl. the C2 64-bit max-boundary sweep); its GPU-gated half proves
`RenderTarget` create/resize/move/not-renderable/sampleable/draw-round-trip lifecycle with **no
window and no swapchain** (AC-13 — `RenderTarget` is a general engine type); the new case in
`rhi_swapchain_test.cpp` proves `NativeDeviceAccessor::texture` refuses a swapchain-acquired handle;
`aero_editor_imgui_test` drives the REAL `ViewportPanel` through `EditorApp::tick()` — 3 ticks, a
window resize, hide/show, and the seeded camera destroyed behind the panel's back — an unbalanced
ImGui call or a bad `ImTextureID` would abort/fault in the Debug build, so a green run proves the
offscreen pass, the submit ordering and ImGui's sample of the texture all survive real hardware.

It **cannot** mechanically prove: that the rendered image is *correct* (a lit cube, not garbage) —
no pixel readback exists in this harness; **pixel-exact, DPI-correct crispness on a Retina display**
(a 1× CI runner cannot see a missing `× DisplayFramebufferScale` factor — the single highest-value
row below); that a continuous resize drag is visually artifact-free (no stretch/tear/black-flash/
stale-frame at intermediate sizes); that the clear colour's alpha is genuinely opaque (no editor
chrome bleeding through); and that reordering the offscreen pass relative to `endFrame()` would be
visible (temporal one-frame staleness has no mechanical signature — sabotage S8 proved the whole GPU
suite stays green under that exact reordering). That half needs a person at the machine.

## What was mechanically verified (this implementation pass, macOS, Apple M1 Pro / Metal)

- `aero_editor_core`, `aero_editor`, `aero_editor_shell_test`, `aero_editor_imgui_test`,
  `aero_editor_inspector_test` and `aero_tests` (which gained `render_target_test.cpp`) build clean
  on `macos-debug` and `macos-release`; `ctest` is green at **94** entries on both (unchanged from
  2.2.2 — no step in this task registers a new ctest entry).
- `AERO_REQUIRE_GPU=1 ctest --preset macos-debug` passes in full (94/94), and again with the ratchet
  unset; `aero_editor_imgui_test` now runs **5** `TEST_CASE`s (was 4), including the new task-2.2.3
  case driving the real Viewport through resize/hide-show/no-camera.
- `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` (fresh configure): `ctest -N` is **5** (was 5),
  `aero_editor` still builds and launches, and the log shows exactly **one**
  `editor: viewport disabled — built with -DAERO_SHADER_TOOLS=OFF (no cooked shaders)` line alongside
  2.2.2's pre-existing one reflection-tools WARN — the structural proof of D12's graceful degradation.
- Launched `aero_editor` non-interactively after deleting the ini: the log line
  `editor: shell ready (5 panels, 3 entities, layout: default)` appears, `aero_editor.ini` gains a
  real `[Window][Viewport]` entry (survives byte-identically across two consecutive runs, D16), and a
  **zero-ERROR/zero-WARN grep** over `ERROR|CRITICAL` and `SceneRenderer:|render::|rendertarget`
  returns nothing — proving the steady-state seeded-scene draw hits no camera warning, no extent
  clamp and no allocation failure on a default launch.
- All nine mandatory §V3 sabotage proofs were performed, each seed confirmed present via `git diff`
  before trusting the verdict, and reverted (full detail in `docs/10-engineering-log.md`): S1/S2 (the
  tier-0 sizing matrix's keep/quantisation rows redden exactly as predicted, while the postcondition
  sweep stays green — coverage, not discrimination); S3 (`beginFrame` passing the allocation instead
  of the drawn sub-rect into `Frame`) reddens exactly the new F12 extent assertions; S7 (a real
  `SDL_GPUTexture*` code line in `native_device.hpp`) reddens `check-rhi-boundary.sh`, naming the
  file; S9 (dropping the swapchain-owned refusal) reddens exactly one of `rhi_swapchain_test.cpp`'s
  four new assertions; **S8** (moving `renderScene()` to after `endFrame()`) leaves the whole GPU
  suite green — the documented finding, not a gap; **S6 was seeded, confirmed present, and did NOT
  redden the hide/show case** — investigated and recorded honestly in the engineering log rather than
  claimed as covered (the shipped code is still the correct, unconditional `std::exchange`). **S4 and
  S5 are human-only by design** (alpha and HiDPI crispness have no mechanical proof in this harness)
  and are recorded as such below, not claimed as mechanically covered.
- No mouse or keyboard interaction was performed by this implementation pass — every OS below is
  recorded pending a human, per the established precedent.

## Known-and-expected, NOT a defect

- **The allocated texture is larger than the rendered rect** (quantum 64) — the unused margin is
  cleared to the viewport background every frame and is never sampled at UV ≤ `uvMax`. Do not "fix"
  the sub-rect edge with a half-texel inset — that resamples and blurs the whole image.
- **Panel resolution jumps in 64-px steps in the allocation but never in the image** — the overlay
  text shows `drawExtent`, which is exact; only the allocation quantises (AC-6).
- **No camera → the clear colour plus "No camera in scene"**, with exactly ONE WARN, not a stream
  (`SceneRenderer`'s WARNs are latched once per lifetime). There is no fallback editor camera in this
  task by design — 2.3.1 owns it.
- **No camera navigation at all** — orbit/pan/zoom/focus is 2.3.1; picking is 2.3.2; gizmos are 2.3.3.

## How to validate one OS (for the pending ones)

1. **Build**, delete `build/<os>-debug/editor/aero_editor.ini`, run `aero_editor`.
2. A lit cube is visible in the Viewport panel on first launch.
3. The image fills the dock node — no padding gap, no scrollbar.
4. **Drag the window edge slowly** — no stretch, no black flash, no tearing, no stale frame at any
   intermediate size; the overlay resolution updates continuously.
5. **Drag the dock splitter between Viewport and Inspector** — same, and the aspect ratio stays
   correct as the panel narrows.
6. **Undock the Viewport to a floating window, resize it, redock it** — no artifact at any step.
7. **Maximise / restore / minimise / restore** the whole window — clean, no crash, no black frame.
8. **HiDPI**: on a Retina/2× display, the cube's edges are as crisp as `samples/phase-1-scene` at the
   same on-screen size — not softened as if upscaled from half resolution.
9. **Alpha**: the viewport background is opaque — no editor chrome bleeding through the empty area
   around the cube.
10. **Live edit**: drag `Transform.position` in the Inspector — the cube moves in the viewport in the
    same frame, smoothly.
11. **Live structure**: create / duplicate / delete / reparent entities in the Hierarchy — the
    viewport tracks every change immediately.
12. **Delete "Main Camera"** — the viewport shows the clear colour plus "No camera in scene"; the log
    gains one WARN, not a stream. Re-add via Hierarchy + Inspector to restore the view.
13. **`View > Viewport` off / on** — the panel disappears and returns with its content intact.
14. **Quit and relaunch** — the layout persists byte-identically, the viewport comes back rendering.
15. **A visibly non-square panel** (very tall and narrow) — geometry is not distorted.
16. **Record your results below.**

## Validation records

Each OS records the same fourteen checks (§V7 rows 1–14 of the plan). Copy this checklist into the
OS's section below as you go, appending ` — PASS` (or FAIL) and what you observed to each line.

- **Lit cube visible on first launch**
- **Image fills the dock node**
- **Slow window-edge resize — no artifact (S8's human proof)**
- **Dock-splitter resize — aspect ratio correct**
- **Undock / resize / redock**
- **Maximise / restore / minimise / restore**
- **HiDPI crisp (S5's human proof)**
- **Alpha opaque, no chrome bleed-through (S4's human proof)**
- **Live edit (Transform.position) reflected immediately**
- **Live structure edits reflected immediately**
- **No-camera overlay + single WARN**
- **View > Viewport off/on**
- **Quit and relaunch**
- **Non-square panel — no distortion**

### macOS — ✅ PASS (2026-07-27)

Machine: MacBook Pro (Apple M1 Pro), Metal. Human mouse/keyboard pass against `4e21179`.

- **Lit cube visible on first launch** — PASS
- **Image fills the dock node** — PASS
- **Slow window-edge resize — no artifact (S8's human proof)** — PASS
- **Dock-splitter resize — aspect ratio correct** — PASS
- **Undock / resize / redock** — PASS
- **Maximise / restore / minimise / restore** — PASS
- **HiDPI crisp (S5's human proof)** — PASS
- **Alpha opaque, no chrome bleed-through (S4's human proof)** — PASS
- **Live edit (Transform.position) reflected immediately** — PASS
- **Live structure edits reflected immediately** — PASS
- **No-camera overlay + single WARN** — PASS
- **View > Viewport off/on** — PASS
- **Quit and relaunch** — PASS
- **Non-square panel — no distortion** — PASS

Notes: this pass closes the three acceptance criteria that have **no mechanical proof available in
this harness**, and which the implementation deliberately refused to claim automated coverage for —
each is the human half of a sabotage proof that could not discriminate:

- **HiDPI crispness (S5 / AC-3)** — the highest-value row in the table. Dropping the
  `× io.DisplayFramebufferScale` factor produces a half-resolution, visibly soft viewport that no
  test on a 1× surface can see, and that lavapipe could never settle.
- **Alpha opacity (S4 / E4)** — a 0-alpha clear colour lets the editor's chrome show *through* the
  viewport wherever no geometry was drawn, because ImGui alpha-blends the image.
- **Temporal freshness (S8 / AC-4)** — moving `renderScene()` to after `ImGuiLayer::endFrame()`
  leaves the entire GPU-gated suite green while showing one-frame-stale content. That the automated
  suite stays green under that seed *is* the recorded finding; only a human eye on a slow resize
  drag distinguishes it.

### Windows — ⏳ pending

Needs a native run (D3D12). No checks recorded yet.

### Linux — ⏳ pending

Needs a native run (real Vulkan; **not** lavapipe/CI). No checks recorded yet.

**Task 2.2.3 gate status: macOS-PASS — held OPEN pending the Windows/Linux on-hardware records.**
Both halves are green on macOS: the mechanical one (build, full ctest, the tools-OFF proof, the
`AERO_REQUIRE_GPU=1` rehearsal, the non-interactive launch proof, all nine sabotage proofs) and the
interactive human pass recorded above. No code change was needed. The gate closes when the two
remaining OS records land — the 0.5.3/1.4.2/2.1.1/2.1.3/2.2.1/2.2.2 precedent.

---

# Task 2.2.4 — asset browser stub

Task 2.2.4's deliverable: the `"Assets"` placeholder is replaced by a dockable, **read-only**
two-pane browser over the project directory on disk — a directory tree on the left, the current
directory's contents with sizes on the right, a clickable breadcrumb, `Refresh` and `Show hidden` —
navigable, sorted, sized, and degrading to a clear in-panel message when the root is unusable. CI
proves the structural/mechanical half automatically: the new `tests/editor/project_files_test.cpp`
(tier-0, no GPU, no window, no ImGui context, riding the existing `aero_editor_shell_test` entry)
proves the entire scan / sort / size-format / tree-walk / root-resolution surface over **17
`TEST_CASE`s** — including a real temp directory, a 10 005-file cap, a `0000`-mode unreadable
directory, a non-ASCII filename round-trip and a `MAX_TREE_DEPTH` symlink-cycle bound; and
`aero_editor_imgui_test` drives the **real** `AssetBrowserPanel` through `EditorApp::tick()` in two
new cases (a real directory tree with hide/re-show and a 200×120 shrink, and an unusable root), where
an unbalanced `EndChild`, a wrongly-called `EndTable` or a leaked `PushID` is an `IM_ASSERT` **abort**
in the Debug build — so a green run *is* the balance assertion.

It **cannot** mechanically prove: anything a human has to **look at**, or anything requiring
**synthesised mouse input**. No ImGui input can
be injected in this harness, so the breadcrumb, the `..` row, single-click navigation,
double-click-to-enter, the expand/collapse arrows, the `Show hidden` checkbox, the splitter drag and
the ImGui-id-merging class of bug (sabotage S11) are all **human-only**. It also cannot prove that
scrolling a 10 000-entry directory *feels* smooth (frame time has no mechanical signature here), that
the `—` unknown-size glyph actually renders as an em dash rather than `?` with ProggyClean's
Latin-only range, or that indentation visibly distinguishes depth 0 from depth 1 (the C3 check).
That half needs a person at the machine.

## What was mechanically verified (this implementation pass, macOS, Apple M1 Pro / Metal)

- `aero_editor_core`, `aero_editor`, `aero_editor_shell_test`, `aero_editor_imgui_test`,
  `aero_editor_inspector_test` and `aero_tests` build clean on `macos-debug` and `macos-release`;
  `ctest` is green at **94** entries on both, at **every one of the five commit boundaries** —
  unchanged from 2.2.3, because no step in this task registers a new ctest entry.
- `AERO_REQUIRE_GPU=1 ctest --preset macos-debug` and `--preset macos-release` both pass in full
  (94/94), and identically with the ratchet unset. `aero_editor_imgui_test` now runs **7**
  `TEST_CASE`s (was 5); `aero_editor_shell_test` now runs **63** doctest cases (was 44 — 17 from the
  original series plus 2 from the code-review round).
- `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` (fresh configure): `ctest -N` is **5**
  (unchanged), `aero_editor` still builds and launches, and its log carries **exactly two WARN lines
  and no third** — both pre-existing (2.2.2's reflection-tools line, 2.2.3's shader-tools line) —
  plus a hit on the new `editor: assets root '…'` INFO line and **zero** ERROR/CRITICAL lines. This
  task adds no tools gate and no new WARN: the browser is **fully functional** in that configuration.
- Three non-interactive launches: bare `aero_editor` logs
  `editor: shell ready (5 panels, 3 entities, layout: default)` and
  `editor: assets root '<repo root>'`; `aero_editor /tmp` logs `editor: assets root '/tmp'` and
  `layout: restored`; `aero_editor /definitely/not/here` logs that exact root with **zero
  ERROR/CRITICAL lines and a 4-line log** — the mechanical proof of D17 (a missing root is a *panel*
  state, never a startup failure and never a log storm). `aero_editor.ini` is **byte-identical**
  across the runs, which is D20/INV-1's proof that `"Assets"` still keys the same saved layout entry
  it has keyed since 2.1.3.
- All **eleven** mandatory sabotage proofs were performed, each seed confirmed present via
  `git diff` **before** trusting the verdict, and reverted (full detail in
  `docs/10-engineering-log.md`): **S1** (drop the `isDirectory` sort key) reds cases 4 and 6;
  **S2** (raw compare, no case folding) reds case 4 while case 6 stays green as predicted;
  **S3** (delete the entry cap) reds case 9; **S4** (ignore `includeHidden`) reds case 7;
  **S5** (`parentOf("assets") == "assets"`) reds case 2; **S6** (delete the `MAX_TREE_DEPTH`
  conjunct) reds case 14 **by assertion** (41 rows instead of 32) rather than by the hang the plan
  predicted — case 14's `openDirs` is finite, which makes the verdict deterministic instead of a CI
  timeout; **S8** (`skip_permission_denied`) reds case 8b, which genuinely ran here (euid 501,
  macOS, so neither of its two vacuity guards fired); **S9** (delete one `EndChild()`) aborts
  `aero_editor_imgui_test` with `SIGABRT` on *"Must call EndChild() and not End()!"*; and **S10**
  (hoist `EndTable()` out of its `if`) **did discriminate on this lane** — abort on *"EndTable()
  call should only be done while in BeginTable() scope!"* on frame 2 of the new case, at the default
  320×180 window — so INV-3's conditional-`EndTable` half is mechanically covered, not review-only.
- **A code-review round fixed four further defects** (full detail in `docs/10-engineering-log.md`),
  each with its own re-verified sabotage: **S12** (revert the broken-symlink fix to always-skip) reds
  the new dangling-symlink case; **S13** (delete the examined-cap term) reds the new examined-cap case
  while leaving case 9 green, and **S3 re-run** reds both — proving the two caps are independently
  covered; **S9** and **S10 re-run** against the rewritten draw code still abort. A launch proof
  against a directory holding `sprites##old`, `sprites##new`, `readme##v2.txt`, a literal `%s.txt` and
  a dangling symlink produced **zero ERROR/CRITICAL lines and zero ImGui asserts**.
- **S14 (revert the `##` display fixes) is GREEN and is therefore recorded as NOT discriminating.**
  `##` truncation is a rendering defect with no mechanical signature in this harness — no pixel
  readback, no text-extraction API — exactly like S11. Its protections are the upstream-source
  verification recorded inline in `asset_browser_panel.cpp`, the launch proof above, and **row 16**
  below. It is not claimed as tested.
- **S7 is recorded as a Windows-CI-only discriminator, NOT as passed**: seeding `path::string()` in
  place of `u8string()` leaves case 11 green on macOS, because both are UTF-8 there. The MSVC lane is
  where case 11 catches it. **S11 is human-only** (row 13 below) — ImGui id merging has no
  mechanical signature in this harness. Neither is claimed as covered.
- `clang-format-18 --dry-run --Werror` and
  `SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path) clang-tidy-18 -p build/macos-debug
  --warnings-as-errors='*'` are clean over all six new/changed TUs, with **zero new `NOLINT`s** —
  the two findings raised (`bugprone-exception-escape` on `leafOf`, `modernize-pass-by-value` on
  `EditorApp`'s private constructor) were fixed, not suppressed.
- All five architecture-guard scripts run green locally with no allowlist change;
  `check-math-boundary.sh`'s anti-vacuity scan count grew 184 → 189 and its verdict is unchanged
  (nothing here includes GLM). `git diff origin/main` is **empty** over `engine/`, `runtime/`,
  `samples/`, `tools/`, `cmake/`, `shaders/`, `.github/` and `vcpkg.json`; the `/vcpkg` submodule SHA
  is unchanged; `imgui_layer.{hpp,cpp}` is byte-identical for the **fifth** task running (INV-7).
- No mouse or keyboard interaction was performed by this implementation pass — every OS below is
  recorded pending a human, per the established precedent.

## Known-and-expected, NOT a defect

- **E12 — a directory with no subdirectories still shows an expand arrow until it is first opened.**
  Knowing better beforehand costs one `opendir` per sibling on every tree rebuild. Once a directory
  has been visited even once, `knownLeaf` removes its arrow **permanently** (the listing stays
  cached). Documented, **not fixed** — do not "fix" it.
- **E14/F16 — a non-ASCII filename renders as `?` glyphs.** Two different facts, only the second
  visible: the *path* is handled correctly end to end (`u8string` in both directions, proven
  byte-for-byte by tier-0 case 11), while the *font* — ImGui's default ProggyClean — carries Basic +
  Extended Latin only. **Loading a Unicode font is nobody's task yet**; it is recorded in the
  engineering log rather than invented as a task.
- **The `—` unknown-size sentinel may render as `?`** on a lane whose font range lacks U+2014. The
  repo already ships U+2014 through ImGui (`editor_app.cpp`, `inspector_panel.cpp`) and four macOS
  human passes have not flagged it, so it is kept. If a human sees `?`, the fix is a one-character
  swap to ASCII `--` and is **not** a design change.
- **A directory name containing `##` is truncated IN THE BREADCRUMB ONLY** — `sprites##old` shows as
  `sprites` there. `ImGui::FindRenderedTextEnd()` stops at the first `##`, and `SmallButton`/`Button`
  have **no format overload** to bypass it, unlike `TreeNodeEx`. **The tree pane and the contents
  table — the two places a name is actually read — ARE fixed** and show the full name; navigation is
  unaffected everywhere, because the ids come from `PushID(i)`, not from the label. Closing the
  breadcrumb case needs an `InvisibleButton` plus hand-placed draw-list text with its own hover/active
  styling — disproportionate for a stub. **Deliberately recorded, not fixed** (code-review gap 4);
  row 16 below looks for it.
- **E15 — a Windows path over `MAX_PATH`** (> 260 characters) fails to enumerate and shows
  `Cannot read this directory…`. MSVC's `std::filesystem` does not transparently apply the `\\?\`
  prefix. A known limitation of the stub; recorded, not worked around.
- **No live refresh.** `Refresh` plus tab-reselect (`IsWindowAppearing()` drops the cache) is the
  whole story. A filesystem watcher is **3.1.4**'s deliverable, and its seam already exists:
  `cache.clear(); treeDirty = true;` *is* the entire invalidation.
- **A broken symlink is LISTED, with `—` for its size** — it is not an error and it does not count
  toward `skipped`. The spec's E6 assumed such an entry reached `file_size()`; it never does
  (`is_directory()` fails first), so before the code-review round it silently VANISHED from the
  listing. It is now listed as a size-unknown *file* — never as a directory, so the tree never offers
  to descend into it. A broken link you can see beats one that disappears.
- **A directory can report `truncated` with far fewer than 10 000 entries listed.** There are TWO
  caps: 10 000 entries **retained** and 20 000 entries **examined**. Hidden-filtered entries grow only
  the second, so a directory of 500 000 dotfiles browsed with `Show hidden` off lists nothing and says
  `truncated` — which is correct and deliberate, and why the footer names both caps rather than
  claiming "showing the first 10000".
- **Read-only by contract, not by omission.** No create, rename, move, delete or open — and no
  double-click-to-open-a-scene (2.5.1 owns scene I/O). Nothing in the panel or the model mutates the
  disk; `git status` in the browsed tree stays clean no matter how much you click (row 15).
- **The `$PWD` default root is a foot-gun in this repository**: launched from the repo root, the tree
  offers `vcpkg/` and `build/`. Lazy per-directory scanning bounds the work to one directory per
  click, and the 10 000-entry cap bounds *that* — with the footer saying so. 2.6.1 replaces the
  default with a real project root.

## How to validate one OS (for the pending ones)

1. **Build**, then run `aero_editor` with **no argument**. The **Assets** tab sits beside Console in
   the bottom dock; select it; the listing matches the process working directory.
2. Run `aero_editor <a real project dir>` — it lists that directory instead, and the breadcrumb head
   shows its name.
3. **Expand two levels in the tree.** The right pane follows the clicked directory, the breadcrumb
   grows, and clicking a breadcrumb segment goes back. **Also the C3 check: each depth is visibly
   indented one step further than its parent — depth 0 and depth 1 must NOT look identical.**
4. `..` goes up on a **single** click; **double-clicking a directory row enters it** — the C4 check;
   without `ImGuiSelectableFlags_AllowDoubleClick` this does nothing at all.
5. Create a file outside the editor → **Refresh** shows it. Hide the Assets tab, create another, then
   select the tab again → it appears **without** pressing Refresh (`IsWindowAppearing`).
6. Tick **Show hidden** → dotfiles appear; untick → they vanish; tick/untick returns to the identical
   view.
7. Sizes look right against the OS file manager for files of several magnitudes, and **a file whose
   size cannot be read shows `—` (or `?`), never `0 B`**.
8. Sort order: **directories first**, then case-insensitive alphabetical.
9. Drag the pane splitter; quit; relaunch → **the width is remembered** and the panel is still docked
   where it was.
10. Point it at a directory with **≥ 10 000 files**: scrolling stays smooth and the truncation note
    appears in the footer.
11. `aero_editor /definitely/not/here` → the panel explains it, naming the path; the editor still
    docks and quits cleanly.
12. `aero_editor <a regular file>` → the **"Not a directory"** message.
13. **Two directories with the same name at different depths both appear correctly** — sabotage
    **S11**'s human half (deleting the row loop's `PushID`/`PopID` merges them into one row).
14. **Expand a directory that turns out to have no subdirectories** — its arrow disappears, the tree
    does not flicker, and **no other selection or expansion is lost** — sabotage **C1**'s human half.
15. **`git status` in the browsed tree is clean after all of the above** — the read-only proof.
16. **Create `readme##v2.txt` and two directories `sprites##old` / `sprites##new`.** In the **tree**
    and the **contents table** every name must display **in full, including the `##`** — sabotage
    **S14**'s human half, and the only proof that exists: `##` truncation is a rendering defect with
    **no** mechanical signature in this harness. In the **breadcrumb** the crumb is expected to show
    as `sprites` — that one is recorded above as known-and-expected, not a defect.
17. **Create a broken symlink** (`ln -s no-such-target broken.link`). It must **appear** in the
    listing with `—` for its size, and the footer must **not** count it as skipped — code-review
    gap 1. It must not appear in the directory tree.
18. **Select any file in a directory that reports `truncated`.** The `truncated` / `skipped` counts
    must **stay visible** in the footer alongside the selection — code-review gap 3. Before the fix,
    clicking a file made the truncation notice disappear.

### macOS — ✅ PASS (2026-07-28)

Machine: MacBook Pro (Apple M1 Pro), Metal. Human mouse/keyboard pass against `c8ab8a0` — i.e.
against the post-code-review tree, with all four gap fixes in place.

- **1 — bare `aero_editor`: Assets tab beside Console, listing matches `$PWD`** — PASS
- **2 — `aero_editor <dir>` lists that directory; breadcrumb head shows its name** — PASS
- **3 — expand two levels; right pane follows; breadcrumb grows and navigates back; each depth
  visibly indented one step further (C3)** — PASS
- **4 — `..` on a single click; double-click a directory row enters it (C4)** — PASS
- **5 — Refresh picks up an out-of-editor change; tab-reselect does too** — PASS
- **6 — Show hidden on/off round-trips to the identical view** — PASS
- **7 — sizes match Finder across magnitudes; an unreadable size shows `—`, never `0 B`** — PASS
- **8 — directories first, then case-insensitive alphabetical** — PASS
- **9 — splitter width and dock position remembered across a quit/relaunch** — PASS
- **10 — a ≥ 10 000-file directory scrolls smoothly and reports truncation** — PASS
- **11 — `aero_editor /definitely/not/here` explains it, names the path, docks and quits cleanly** — PASS
- **12 — `aero_editor <a regular file>` shows "Not a directory"** — PASS
- **13 — two same-named directories at different depths both appear (S11's human half)** — PASS
- **14 — expanding a subdirectory-free directory drops its arrow, loses no other state (C1's human
  half)** — PASS
- **15 — `git status` in the browsed tree stays clean** — PASS
- **16 — `##` names display in full in the tree and the contents table (S14's human half)** — PASS
- **17 — a broken symlink is listed with `—`, is not counted as skipped, and does not appear in the
  tree (code-review gap 1)** — PASS
- **18 — the truncated/skipped counts stay visible while an entry is selected (code-review gap 3)** — PASS

Notes: this pass closes the interactive half of **AC-3** — which had **no** proof of any other kind,
mechanical or otherwise, and whose spec spelling C4 showed would have been silently dead — plus the
human halves of **C1**, **C3**, **AC-10**, **AC-12** and **AC-13**.

Rows 16, 17 and 18 are the human halves of three code-review gaps. Row 16 in particular is the
**only** proof that exists for the `##` fix: sabotage **S14** is green (non-discriminating), because
`##` truncation is a rendering defect with no mechanical signature in this harness. The breadcrumb's
`##` behaviour is recorded above as known-and-expected and was **not** treated as a failure here.

### Windows — ⏳ pending

Needs a native run (D3D12). No checks recorded yet. Rows 7 and 12 are additionally where **E15**
(`MAX_PATH`) and **S7** (`path::string()` vs `u8string()`) would first show.

### Linux — ⏳ pending

Needs a native run (real Vulkan; **not** lavapipe/CI). No checks recorded yet.

**Task 2.2.4 gate status: macOS-PASS — held OPEN pending the Windows/Linux on-hardware records.**
Both halves are green on macOS: the mechanical one (both presets at 94/94, the `AERO_REQUIRE_GPU=1`
rehearsal, the tools-OFF proof with exactly two WARNs, three non-interactive launch runs, fourteen
sabotage proofs, all five guards, clang-format and clang-tidy clean with zero new NOLINTs) and the
interactive one (the 18-row human pass above, which is the only proof AC-3 and the `##` fix have).
The gate closes when the Windows and Linux records land — the 0.5.3/1.4.2/2.1.1/2.1.3/2.2.1/2.2.2/
2.2.3 precedent.

---

# Task 2.2.5 — log/console panel

Task 2.2.5's deliverable: the `"Console"` placeholder is replaced by a dockable panel showing the
engine's own log stream **live**, from any thread, whether or not the panel is visible — filterable by
minimum level and by text, clearable, copyable, bounded in memory, honest about everything it drops.
Landing it deletes the last `PlaceholderPanel` in the tree and closes Epic 2.2 in code. CI proves the
structural/mechanical half automatically: the new `tests/editor/console_model_test.cpp` (tier-0, no
GPU, no window, no ImGui context, riding the existing `aero_editor_shell_test` entry) proves the
entire sink / ring / filter / formatter surface over **26 `TEST_CASE`s** — including an 8-concurrent-
producer arm, the R14 shared-ownership proof, a move-assignment case proving an assignment never
seizes a third scope's installation, and the C1 `Off`-level counter-overflow guard (whose case asserts
`filter() == LogFilter{}`, the only observable the overflow reaches — `levelCount(Off)` is blind to it
and no sanitizer fires); and
`aero_editor_imgui_test` drives the **real** `ConsolePanel` through `EditorApp::tick()` in three new
cases (the live log stream, hidden-panel capture with an exact-delta assertion, and a full
10 000-record ring under a window resize), where an unbalanced `EndChild`, a wrongly-called `EndCombo`
or a leaked `PushID`/`PushStyleColor` is an `IM_ASSERT` **abort** in the Debug build — so a green run
*is* the balance assertion.

It **cannot** mechanically prove: anything a human has to **look at**, or anything requiring
**synthesised mouse input**. No ImGui input can be injected in this harness, so Clear/Copy's clipboard
round-trip, auto-scroll's *feel*, hover-tooltip timing, level/colour/alignment legibility, id-merging
(sabotage S15's human half) and `"##"`/`%s`/newline-in-message rendering (S14/S16's human half) are all
**human-only**. It also cannot prove that scrolling a full 10 000-row buffer *feels* smooth (frame time
has no mechanical signature here), or that a non-ASCII message visibly renders as `?` while still
filtering and copying byte-exactly. That half needs a person at the machine.

## What was mechanically verified (this implementation pass, macOS, Apple M1 Pro / Metal)

- `aero_editor_core`, `aero_editor`, `aero_editor_shell_test`, `aero_editor_imgui_test`,
  `aero_editor_inspector_test` and `aero_tests` build clean on `macos-debug` and `macos-release`;
  `ctest` is green at **94** entries on both, at **every one of the six commit boundaries** —
  unchanged from 2.2.4, because no step in this task registers a new ctest entry.
- `AERO_REQUIRE_GPU=1 ctest --preset macos-debug` and `--preset macos-release` both pass in full
  (94/94), identically with the ratchet unset. `aero_editor_imgui_test` now runs **10** `TEST_CASE`s
  (was 7); `aero_editor_shell_test` now runs **88** doctest cases — measured at the branch's own
  baseline of **63** (not the plan's assumed 61; its own grounding notes said to re-measure rather than
  trust the log, and the re-measurement caught real drift since the 2.2.4 entry was recorded), +25.
- `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` (fresh configure, rebuilt in place): `ctest -N`
  is **5** (unchanged), `aero_editor` still builds and launches, and its log carries **exactly two
  WARN lines and no third** — both pre-existing (2.2.2's reflection-tools line, 2.2.3's shader-tools
  line) — with, for the first time, the console-sink-attached INFO line preceding both of them. This
  task adds no tools gate and no new WARN: the console is **fully functional** in that configuration,
  and both pre-existing WARNs are now visible **inside the panel itself** (the mechanical proof stops
  at the log line; that they render legibly in the panel is row 12 below).
- The non-interactive launch proof (`aero_editor`, ini deleted first, run and killed after 4 s):
  `editor: console log sink attached (4096 staged / 10000 held)` appears, **before** both
  `editor: assets root '…'` and `editor: shell ready (5 panels, 3 entities, layout: default)` — the
  D5 ordering claim made mechanical — and **zero** ERROR/CRITICAL lines. A second launch reads
  `layout: restored` and the `.ini` is **byte-identical** across the two runs, D23/INV-1's proof that
  `"Console"` still keys the same saved layout entry it has keyed since 2.1.3.
- All **ten** S1–S10 sabotages were performed against the tier-0 battery, each seed confirmed present
  via `git diff` **before** trusting the verdict, and reverted (full detail in
  `docs/10-engineering-log.md`): S1 (raw-pointer capture) reds case 21 deterministically
  (`use_count()` drops to 1); S2 (unconditional `setLogCallback({})` in `detach()`) reds case 22; S3
  (drop the control-byte mapping) reds case 3; S4 (drop the message cap) reds case 4; S5 (drop the
  UTF-8 back-off loop) reds case 4's straddling-sequence assertion; S6 (drop the level test in
  `matchesFilter`) reds cases 7 and 10; S7 (compare raw bytes, no folding) reds cases 6 and 7; S8
  (delete the eviction loop) reds cases 9 and 14; S9 (delete the `visibleSeq` front-prune) reds case
  11's assertion **and** separately triggers an ASan container-overflow abort in the very next
  `matchesFilter` call — louder than the plan's predicted debug-assert path, same discriminating
  property; S10 (drop the newest silently, no counter) reds case 17.
- **S17 (the C1 range-check guard) is reported honestly as NOT mechanically discriminating** in the
  exact stack-local construction tier-0 case 25 uses, despite the seed being confirmed present via
  `git diff`: `LogHistory h;` is a plain stack local and `counts` is not its last member, so the
  out-of-bounds write corrupts the immediately-following member's padding **within the same stack
  allocation** — neither ASan's stack-redzone protection (which guards the *outer* local variable's
  boundary, not intra-object member boundaries) nor UBSan's `bounds` check (which does not instrument
  `std::array::operator[]`'s definition, a system header excluded from sanitizer instrumentation by
  default) fires. The C1 fix itself is unaffected and still correct — `levelCount(Off)` is guaranteed
  `0` by its own independent read-side range check regardless of what the write side leaves behind —
  only the sabotage's mechanical discriminating power in this harness is in question. Recorded here
  rather than claimed.
- Sabotages S11, S12 and S13 were performed against the GPU-gated battery and all three discriminated:
  S11 (pump moved out of `tick()`) reds GPU case B's exact-delta assertion (`0 == 20`); S12
  (`ImGui::EndChild()` deleted) aborts with *"Must call EndChild() and not End()!"*; S13
  (`ImGui::EndCombo()` hoisted unconditional) aborts with *"Calling EndCombo() in wrong window!"* — an
  immediate `IM_ASSERT` abort in the Debug ImGui build, no test even needs to open the combo. S8 was
  additionally re-run against GPU case C and reds it too (`12003 == 10000`), confirming the eviction
  property through the real ImGui draw path, not only at tier-0.
- **S14 is grep-only** (`ImGui::Text(runtimeString)` compiles silently in this codebase — no
  `-Wformat*`, no clang-tidy format check — so the §V6 INV-6 grep is the only guard) and **S15/S16 are
  human-only** (ImGui id merging and `"##"` truncation have no mechanical signature in this harness).
  None of the three is claimed as mechanically covered.
- `clang-format-18 --dry-run --Werror` and
  `SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path) clang-tidy-18 -p build/macos-debug
  --warnings-as-errors='*'` are clean over every new/changed TU, with **zero new `NOLINT`s** — every
  finding raised (`misc-const-correctness`, `performance-unnecessary-copy-initialization`,
  `bugprone-exception-escape` on `logSourceBasename`, `modernize-avoid-c-arrays` ×2,
  `bugprone-use-after-move` in the moves test) was fixed with a real code change, never suppressed.
- All five architecture-guard scripts run green locally with no allowlist change (only
  `check-math-boundary.sh` sees this diff at all, net +3 files scanned — 188 on `origin/main` → 191
  here); `git diff origin/main` is
  **empty** over `engine/`, `runtime/`, `samples/`, `tools/`, `cmake/`, `shaders/`, `.github/` and
  `vcpkg.json`; the `/vcpkg` submodule SHA is unchanged; `imgui_layer.{hpp,cpp}`, `text_input.{hpp,cpp}`
  and `main.cpp` are byte-identical — `imgui_layer.{hpp,cpp}` for the **sixth** task running (INV-7).
- No mouse or keyboard interaction was performed by this implementation pass — every OS below is
  recorded pending a human, per the established precedent.

## Known-and-expected, NOT a defect

- **E12/F30 — a non-ASCII log message renders as `?` glyphs.** The *bytes* are stored, filtered and
  copied exactly (`sanitizeLogMessage` passes bytes `>= 0x80` through untouched); the *font* — ImGui's
  default ProggyClean — carries Basic + Extended Latin only. This is the **second** place a Unicode
  font's absence visibly costs something (2.2.4's E14/F16 was the first). **Loading a Unicode font is
  nobody's task yet.**
- **E28 — the horizontal scroll extent grows as long lines scroll into view.** ImGui's own
  `ExampleAppLog` behaves identically; deliberately not worked around.
- **E15 — `shutdownLogging()` called from elsewhere silently stops delivery**, undetectable through the
  public API. Documented on `LogSinkScope`'s header comment; nothing in the shipped editor calls it.
- **D22 — filter text, the minimum level and the auto-scroll flag do not persist** across a restart.
  2.6.1 owns project-scoped settings; a dedicated `imgui.ini` handler for three fields with obvious
  defaults is not the shape.
- **D9 — there is no UI control to change the engine's runtime log level.** Lowering the floor cannot
  recover records already dropped by a stricter one, so it would read as a broken filter; changing the
  floor is a Handoff, not this task's job.
- **A `"##"` in a log message displays literally in the row list, never truncated** — the `cd4caab`
  `"##row"` + `TextUnformatted` idiom, applied here to content far more likely to contain `##` than a
  2.2.4 filename. Sabotage S16 (reverting to `Selectable(message.c_str())`) is human-only — no
  mechanical signature exists to catch it — so row 9 below is the only proof.

## How to validate one OS (for the pending ones)

1. **Build**, then run `aero_editor` with no argument. **Console** is the **selected** bottom tab
   beside Assets and already contains the startup lines (`console log sink attached`, `assets root`,
   `shell ready`) — AC-2.
2. The rows match the terminal's own spdlog output line for line, in content and order (F9).
3. Levels are visually distinct — Trace/Debug dim, Info default, Warn amber, Error red, Critical the
   brightest red — and **the level column is aligned** across rows.
4. `Level >=` → `WARN`: Info rows vanish. Back to `TRACE`: they return **in the same order**.
   `CRITICAL` on a normal session shows an empty list, and the footer **still reads `engine floor
   TRACE`** — AC-3/AC-9.
5. Type a substring into `Filter` → only matching rows remain; type it in the **opposite case** → the
   **same** rows (AC-4). Clear the box → everything returns.
6. `Clear` empties the panel and zeroes the aged-out/dropped notices. `Copy`, then paste into a text
   editor → the visible rows with timestamps, padded levels and `(file:line)` (AC-5).
7. Scroll up while records arrive → the view **stays put**. Scroll back to the bottom → auto-scroll
   resumes. Untick `Auto-scroll` → the view never moves on its own (AC-5).
8. Hover a row → a tooltip shows `file:line` after the normal delay. **Two rows with identical text
   must behave independently** — sabotage S15's human half.
9. Trigger a message containing `##`, one containing `%s`, and one with an embedded newline (a
   temporary `AERO_LOG_INFO` in `main.cpp` is the cheapest way, reverted before committing anything):
   each renders as **exactly one complete row**, nothing hidden, nothing garbled, no crash — S14/S16's
   human half (AC-8).
10. Select the **Assets** tab, cause some logging (resize the window, click around), then select
    Console → the records are **all there, in order** (AC-6).
11. Exceed 10 000 records (a temporary log loop) → scrolling **stays smooth** and the footer shows the
    aged-out count (AC-7/AC-13).
12. Build with `-DAERO_SHADER_TOOLS=OFF -DAERO_REFLECT_TOOLS=OFF` and launch → **the two WARNs are
    visible in the panel** (AC-2/AC-15b). This is the row §V4 cannot cover.
13. Shrink the bottom dock node until the panel is a sliver; collapse, undock and redock it → **no
    crash, no abort**, the footer stays legible or clips cleanly (AC-12).
14. Quit and relaunch → the panel is **docked where it was**, and the filter / level / auto-scroll are
    back at their **defaults** (AC-14 / D22, the documented non-persistence).
15. A **non-ASCII** log message renders as `?` glyphs but **filters and copies byte-exactly** — the
    documented font-range limitation (E12/F30), not a defect.

### macOS — ⏳ pending

The mechanical/structural pass above ran and is green (build, full ctest on both presets, the
`AERO_REQUIRE_GPU=1` rehearsal, the tools-OFF proof with exactly two WARNs, the non-interactive launch
proof with the D5 ordering check, seventeen sabotage proofs each seed-confirmed and reverted, all five
guards, clang-format and clang-tidy clean with zero new `NOLINT`s).

### macOS — ⚠️ PARTIAL PASS (2026-07-28)

Machine: MacBook Pro (Apple M1 Pro), Metal. Human mouse/keyboard pass against `40908fb` — the merged
tree, with the code-review gap fixes in place.

Eleven of the fifteen rows pass. **Four are BLOCKED, and the cause is a defect in the rows themselves,
not in the panel:** every one of them requires a log record to be emitted *after* startup, and
**nothing in the editor emits one**. Verified exhaustively against the tree — there is not a single
`AERO_LOG_TRACE` or `AERO_LOG_DEBUG` call site anywhere in the first-party tree, and every remaining
site reachable at editor runtime is a failure path (`component_ops`' eleven `ERROR`s, `panel_registry`'s
four rejection `ERROR`s, `imgui_layer`/`main`/`platform`/`rhi`/`render` init and swapchain `ERROR`s) or
a once-per-lifetime startup notice. A successful resize, click, selection or dock change logs nothing.
Row 10's instruction to "cause some logging (resize the window, click around)" therefore cannot be
carried out as written.

- **1 — Console is the selected bottom tab; startup diagnostics already present** — PASS
- **2 — rows match the terminal's spdlog output line for line, in content and order (F9)** — PASS
- **3 — levels visually distinct** — PARTIAL: only `INFO` (startup) and `WARN` (tools-OFF build) are
  reachable. Trace/Debug/Error/Critical colouring is **unverified by eye**.
- **4 — `Level >= WARN` hides Info; back to `TRACE` restores the same order** — PASS
- **5 — text filter, including the opposite-case spelling** — PASS
- **6 — `Clear` empties and zeroes the notices; `Copy` pastes the visible rows** — PASS
- **7 — scroll position holds; auto-scroll resumes at the bottom** — PARTIAL: exercised against a
  static history only. The "while records arrive" half is unverified.
- **8 — hover tooltip shows `file:line` after the normal delay** — PASS. The "two rows with identical
  text keep separate tooltips" half (ImGui id merging) is unverified: no way to produce duplicates.
- **9 — `##`, `%s` and embedded-newline messages** — **BLOCKED**, no reachable log source.
- **10 — records captured while the Assets tab is selected (AC-6)** — **BLOCKED**, no reachable log
  source. Note this property *is* proven mechanically by GPU case B (hide the panel, emit 20 records,
  tick once, assert the delta is exactly 20); what is unverified is only the visual confirmation.
- **11 — exceeding 10 000 records stays smooth; footer shows the aged-out count** — **BLOCKED**, no
  reachable log source. `ImGuiListClipper` behaviour under a full ring is covered by GPU case C.
- **12 — tools-OFF build: the two WARNs are visible in-panel, not just in the terminal (AC-2)** — PASS
- **13 — sliver-height panel, collapse, undock and redock: no assert, no visual breakage** — PASS
- **14 — quit and relaunch: docked where it was; filter/level/auto-scroll reset by design (D22)** — PASS
- **15 — non-ASCII renders as `?` but filters and copies byte-exactly** — **BLOCKED**, no reachable
  log source.

**Follow-up this pass identified (nobody's task yet):** the editor has no runtime log source a user can
trigger, which makes four validation rows unrunnable and will make every future log-consuming panel
equally hard to validate. Either the engine should emit `TRACE`/`DEBUG` records on ordinary events
(resize, selection, dock change), or a debug-only log trigger should exist behind a CMake option. The
four blocked rows should be re-run once one of those lands.

### Windows — ⏳ pending

Needs a native run (D3D12). No checks recorded yet.

### Linux — ⏳ pending

Needs a native run (real Vulkan; **not** lavapipe/CI). No checks recorded yet.

**Task 2.2.5 gate status: mechanically green on macOS, human pass PARTIAL on macOS — held OPEN.** The
mechanical/structural half is green end to end on macOS (both presets at 94/94, the
`AERO_REQUIRE_GPU=1` rehearsal, the tools-OFF proof, the non-interactive launch proof, seventeen
sabotage proofs, all five guards, clang-format/clang-tidy clean with zero new NOLINTs). The macOS
human pass ran on 2026-07-28: **eleven of fifteen rows PASS, three are partial, and four are BLOCKED**
because the editor emits no log record after startup, so they cannot be performed at all (see the
macOS block above for the exhaustive verification of that claim).

What the blocked rows leave unproven by eye: `##`/`%s`/newline rendering (AC-8), the visual half of
hidden-panel capture (AC-6), full-ring scrolling smoothness (AC-13), and non-ASCII glyph handling.
**Each of those four has mechanical coverage** — GPU cases B and C, tier-0 cases 3, 4 and 12, and the
INV-6 grep — so nothing here is entirely unverified; what is missing is human confirmation of
appearance and feel, which is exactly what this ledger exists to record separately.

Windows and Linux remain **pending in full**. Epic 2.2 is **CLOSED in code** (no `PlaceholderPanel`
remains); the gate stays **OPEN** until the four blocked rows can be run — which needs a triggerable
runtime log source that does not exist today — and until at least one non-macOS human pass lands.
