# Editor gate ledger — `aero_editor` (tasks 2.1.1, 2.1.3, 2.2.1, 2.2.2, 2.2.3, 2.2.4, 2.2.5, 2.3.1, 2.3.2)

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
- **No camera → the clear colour plus "No camera in scene"**, with exactly ONE WARN, not a stream.
  **Superseded by task 2.3.1:** the Viewport now renders through the editor's own camera, so a World
  with no `Camera` still draws its meshes, lit, with no WARN at all; the overlay text and its
  `worldHasCamera()` helper were deleted (2.3.1 D19). Scene-camera diagnostics move to the Phase 4 Game
  view, which renders through the scene camera by definition. This row was PASS as written at 2.2.3's
  macOS pass and is retained for that record.
- **No camera navigation at all** — **discharged by task 2.3.1**: orbit (Alt+LMB), pan (MMB /
  Alt+Cmd-or-Ctrl+LMB), dolly (wheel / Alt+RMB), fly (RMB + WASD/QE/Shift) and focus (`F`) all ship
  there. Picking is still 2.3.2; gizmos are still 2.3.3.

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
12. **(2.2.3-era; superseded by 2.3.1 — see that section's row 15.)** Delete "Main Camera" — the
    viewport **keeps rendering**, lit; there is **no** "No camera in scene" text and **no** WARN.
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
- **No-camera overlay + single WARN (2.2.3-era expectation; 2.3.1 deletes the overlay — re-validate
  against 2.3.1's row 15, not this one)**
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

# Task 2.3.1 — editor camera

**Deliverable:** the Viewport now renders through the editor's OWN camera — orbit (Alt+LMB), pan
(MMB / Alt+Cmd-or-Ctrl+LMB), dolly (wheel / Alt+RMB drag), fly (RMB + WASD/QE/Shift) and focus the
selection or the scene (`F`) — never through the scene's `Camera` component. This is what makes the
Viewport navigable at all: 2.2.3 shipped a static, fixed-angle render of whatever camera the seeded
scene happened to contain.

**What CI proves automatically:** the tier-0 batteries in `editor_camera_test.cpp` (the gesture
matrix plus the whole `EditorCamera` model — defaults, orbit, pan, dolly, fly, focus, totality,
view/projection matrices, and the no-log-output sequence) and `scene_bounds_test.cpp` (`Aabb`
algebra, `entityBounds`/`selectionBounds`/`sceneBounds`, and the F12 has/get-vs-each asymmetry), both
riding `aero_editor_shell_test`; the four new `scene_render_test.cpp` tier-0 cases proving
`cameraOverride` replaces/coexists with/is-unaffected-by the scene camera, plus the GPU-gated
WARN-suppression case; and the four new `imgui_layer_test.cpp` cases driving a real `EditorApp::tick()`
with the real camera.

**What it cannot prove:** anything requiring synthesised ImGui mouse or keyboard input. Named
explicitly: AC-14 (wheel ownership — `ImGuiWindowFlags_NoScrollWithMouse`'s effect on real scroll
routing), AC-15 (`io.WantTextInput` gating a real focused `InputText`), AC-21's overlay text actually
appearing on screen, the whole *feel* of every gesture's sensitivity, and **S12's HiDPI pan-speed
row**, which no test in this harness can discriminate because pixels and points are equal on a 1×
runner and every tier-0 case supplies `viewportHeightPoints` directly rather than deriving it from a
real DisplayFramebufferScale.

## What was mechanically verified (this implementation pass, macOS, Apple M1 Pro / Metal)

Measured at every one of the eight commit boundaries, not once at the end:

- `ctest --preset macos-debug` and `--preset macos-release`: **94/94** both presets, at every commit.
- `ctest --preset macos-debug -N` → **Total Tests: 94** throughout; `AERO_REQUIRE_GPU=1 ctest` green on
  both presets (the CI ratchet rehearsed, not skipped).
- Fresh `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` configure into `build/tools-off-2.3.1`:
  **5/5**, `aero_editor` launches with **exactly two** WARN lines (2.2.2's reflection WARN, 2.2.3's
  shader WARN) and **no third** — E23 confirmed: `onDraw` returns before the camera block when the
  Viewport is `Unavailable`, so this task adds no new tools-gate WARN.
- Doctest case counts, measured with `--list-test-cases`, never predicted: `aero_tests` **351 → 356**
  (+5: four tier-0 `scene_render` cases plus one GPU-gated case); `aero_editor_shell_test`
  **89 → 114** (+25: the gesture matrix and idempotence cases, the whole `EditorCamera` battery, the
  `scene_bounds`/`Aabb` battery plus case 12b, and the `PanelContext` delta case);
  `aero_editor_imgui_test` **10 → 14** (+4: the four real-`EditorApp::tick()` camera cases).
- The non-interactive launch check (`aero_editor`, `.ini` deleted, seeded scene): **zero** new
  ERROR/CRITICAL/WARN lines, run after every ImGui-touching step and again after the final format
  pass.
**The sabotage table's outcome**, every seed confirmed present before trusting the verdict, every one reverted and re-confirmed green afterward:


| | Seed | Result |
|---|---|---|
| S1 | flip `ORBIT_YAW_SIGN` to `+1.0F` | reddened case 3's right-drag row exactly; case 3's pitch row and cases 2/5/6 stayed green |
| S2 | drop the pitch clamp from `clampState` | reddened case 4 exactly, as predicted; case 7's extreme-pitch "eye invariant" arm did **not** redden (see the finding below) — case 3 stayed green |
| S3 | make `worldPerPoint` a constant | reddened case 5's two scaling rows exactly; its direction rows stayed green |
| S4 | delete `applyFly`'s eye-restore line | reddened case 7's eye-invariant row exactly; its translation rows stayed green |
| S5 | ignore `cameraOverride` in `buildRenderView` | reddened `scene_render` tier-0 cases 1–2 and GPU case 1; tier-0 case 3 stayed green |
| S6 | drop the `cameraOverride == nullptr` gate on the two camera WARNs | reddened GPU case arms a and c exactly; arm d (the light WARN) stayed green |
| S7 | hoist the light walk above the camera resolution | reddened `scene_render` tier-0 case 3 exactly; every other case in the file stayed green |
| S8 | drop the fresh-press requirement (`...Down` instead of `...Pressed`) | reddened the no-fresh-press rows (the E5 two-button row and the dedicated Alt-after-press case) exactly; the continuation rows stayed green |
| S9 | remove `sceneBounds`'s `registered()` guard | **did not redden anything — recorded as non-discriminating, exactly as the plan predicts** (the guard's target state, an unregistered `MeshRenderer`, is unreachable on a live `World`) |
| S9b | add an `AERO_LOG_WARN` on `sceneBounds`'s empty-result path | reddened case 12 exactly; `scene_bounds_test.cpp`'s case 7 stayed green |
| S10 | drop `dt` from `applyFly`'s translation | reddened case 7's frame-rate-independence row (and its single-step "W for 1s" row); the true single-call rows (Shift, Q/E, W+D) stayed green |
| S11 | use `fovY` alone in `focusOn` (ignore aspect) | reddened case 9's `aspect = 0.5` row exactly; its `aspect = 1`/invalid/point/huge-box rows stayed green |
| S12 | pass `drawExtent.height` instead of `avail.y` as `viewportHeightPoints` | **not seeded mechanically — recorded as human-only, exactly as the plan predicts** (no test in this harness can tell pixels from points on a 1× runner) |

**A genuine finding, recorded honestly rather than glossed over:** the plan's §V4 table predicts S2
also reddens "case 7's eye-invariant at extreme pitch." Investigated directly (seed → build → run →
revert, twice, with an intermediate probe isolating the two candidate mechanisms): dropping ONLY the
pitch-clamp *value* does not move `position()`, because the eye-restore identity
(`pivotPoint = eye + forward() * orbitDistance`) holds algebraically regardless of whether
`pitchRadians` itself was clamped — the restore uses whatever `forward()` the (possibly unclamped)
pitch currently produces, and `fromAxisAngle` never produces NaN/Inf for a large finite angle. The
ordering property the comment actually protects against — `clampState()` running BETWEEN the look
and the restore, not after — was separately verified live: moving the call to run AFTER the restore
(a different, non-plan seed) reddens the SAME row hugely (drift of several world units), confirming
the assertion is real and does its job; it simply does not double as an S2 discriminator. Recorded
here in the open, the same way S9/S12 are — see the engineering log for the full trail.
- All five guards green with **no allowlist change**:
  `check-math-boundary.sh`'s scanned count moved **191 → 197** (+6: `editor_camera.hpp/.cpp`,
  `scene_bounds.hpp/.cpp`, `editor_camera_test.cpp`, `scene_bounds_test.cpp` — measured against
  `origin/main`, not assumed); `check-golden-rule.sh`, `check-rhi-boundary.sh`,
  `check-scene-boundary.sh`, `check-platform-boundary.sh` all unaffected.
- `git diff --stat origin/main` empty over `runtime/`, `samples/`, `tools/`, `cmake/`, `shaders/`,
  `.github/` and `vcpkg.json`; the `/vcpkg` submodule SHA unchanged; `editor/src/imgui_layer.{hpp,cpp}`
  and `editor/src/main.cpp` byte-identical for the **seventh** task running; `aero_editor_shell_test`'s
  `target_link_libraries` line byte-identical (verified against `origin/main`, not merely "unedited by
  eye").
- clang-format and clang-tidy clean on every touched file at every commit boundary, with **zero new
  `NOLINT`s** — the one pre-existing `reinterpret_cast` NOLINT in `viewport_panel.cpp` (2.2.3) is
  unchanged, and no `readability-math-missing-parentheses` finding appeared (C9: that check is not in
  this repo's enabled set).

## Known-and-expected, NOT a defect

- **The D4 Linux Alt+drag WM caveat** — GNOME/KDE bind Alt+drag to *move window* by default, which
  affects Blender, Unity and Maya identically; the fix is a window-manager setting, and the MMB pan
  path is unaffected.
- **C8's Wayland caveat** — ImGui's SDL3 backend whitelists `{windows, cocoa, x11, DIVE, VMAN}` for
  `SDL_CaptureMouse`, so on Wayland a drag that leaves the window may stall; release and re-press to
  recover.
- **D14: no cursor lock**, so a fly-look drag stalls at the physical screen edge — declined by the
  user; see the Handoffs section of the plan for the two costed routes (`io.WantSetMousePos` vs
  `platform::Window::setRelativeMouseMode`).
- **E7: macOS "natural" scrolling inverts the wheel** exactly as it does in every other Mac
  application — uncompensated by design.
- **D13: the pose is transient** — never serialized, never undoable, and there is no camera-settings
  UI for fov/near/far/speed.
- **E9: dolly stops at `MIN_DISTANCE`** rather than passing through the pivot.
- **E16: an enormous subtree frames imperfectly** rather than infinitely — clamped at `MAX_DISTANCE`.
- **D9: `LOCAL_MESH_HALF_EXTENT = 0.5F` is exact for the three procedural primitives** (cube, sphere,
  plane) and **wrong the moment a later task imports a real mesh** — the AssetDatabase should then
  publish per-mesh local bounds and `entityBounds()` should read them.
- **The `"No camera in scene"` overlay is gone on purpose** (D19) — see the 2.2.3 section's rewritten
  rows above.

## How to validate one OS

1. **Launch** — the cube is framed in a right-front ¾ view, lit, not edge-on and not clipped.
2. **Alt+LMB orbit** — smooth; **drag right turns the view right**; **drag down raises the eye and
   looks down at the pivot**; **no roll at any angle**; the poles stop cleanly with no flip, no spin
   and no stutter.
3. **MMB pan** — the scene tracks the cursor at the pivot depth; **still 1:1 after dollying all the
   way in and all the way out**.
4. **Alt+Cmd+LMB pan** (Alt+Ctrl+LMB on Windows/Linux) — identical behaviour; this is the
   **Mac-trackpad path**, since a MacBook trackpad has no middle button at all.
5. **Wheel / two-finger dolly** — smooth and continuous; **never passes through the pivot**; never
   inverts the view; fractional trackpad scroll is smooth, not steppy.
6. **Alt+RMB drag dolly** — right **or up** zooms in; left or down zooms out; a right-and-up diagonal
   zooms in **faster**, not slower.
7. **RMB fly** — look around freely; `W A S D` move relative to the view; **`Q`/`E` move straight
   down/up regardless of pitch**; `Shift` is noticeably (≈4×) faster; the wheel changes speed and the
   overlay reads **`fly N.N u/s`**, appearing only while RMB is held.
8. **Frame-rate independence** — hold `W` for ~3 s with the window focused, note where you end up;
   reset (`F`), then repeat with the window **unfocused** (the 20 Hz cap): the distance travelled is
   the **same**, the motion is merely coarser.
9. **HiDPI pan speed — S12's human row.** On a Retina display, a ~100-pt pan drag moves the scene by
   the same **on-screen** amount as the same drag on a 1× display. If it moves roughly **twice** as
   far, **D15 was violated** — `viewportHeightPoints` is being fed pixels.
10. **`F` on the Cube** frames it and **keeps the current viewing angle**. **`F` on a parent** frames
    the whole subtree, not just the parent. **`F` on the Directional Light** (no mesh) gives a sane
    distance, not a nose-touch.
11. **`F` with nothing selected** frames the whole scene. **`F` in an empty scene** (delete
    everything) returns to the launch pose — and **the Console shows no ERROR lines**.
12. **`F` while renaming** — double-click an entity name in the Hierarchy, type `feature`, press
    Enter: **the camera does not move** and the name is `feature`.
13. **Wheel routing** — wheel over the **Hierarchy / Console / Assets** scrolls those panels; wheel
    over the **Viewport** dollies and does **not** scroll the panel or the dock node.
14. **Plain LMB in the Viewport does nothing** — no gesture starts, the panel does not undock, the
    window does not move, and a subsequent Alt+LMB still orbits normally. (This binding is reserved
    for 2.3.2's picking.)
15. **Delete `Main Camera`** — the viewport **keeps rendering**, lit; there is **no** `"No camera in
    scene"` text; the Console gains **no** WARN. *(This is the row that replaces 2.2.3's row 12.)*
16. **Edit `Camera.fovYRadians` in the Inspector** — the Viewport does **not** change. *(It used to.
    This row is the user-visible proof of the whole task.)*
17. **Resize / undock / redock / maximise the Viewport mid-gesture** — no jump, no crash, no stuck
    gesture; the aspect stays correct as the panel narrows; the `{W}×{H}` readout keeps updating.
18. **Drag outside the window and release there** — the gesture continues while outside, then **ends
    cleanly** on release; pressing again inside starts a fresh one; nothing is latched. *(On
    **Wayland** the drag may stall at the window edge — **C8**; record it as environmental.)*
19. **Two buttons at once** — begin an Alt+LMB orbit, press RMB mid-drag: it **keeps orbiting** and
    does not switch to fly; release LMB, then press RMB again: fly starts normally.
20. **Linux only** — if **Alt+LMB is swallowed by the window manager** (GNOME/KDE bind Alt+drag to
    *move window*), confirm it in the WM settings and record it as **environmental, not a defect**
    (D4); **MMB pan must still work**. Also confirm whether the session is X11 or Wayland and note it
    against row 18.

## Validation records

- **Launch: cube framed in a right-front ¾ view, lit**
- **Alt+LMB orbit: right drags right, down raises the eye, no roll, clean pole stops**
- **MMB pan: tracks the cursor at pivot depth, 1:1 at any zoom**
- **Alt+Cmd+LMB pan (the Mac-trackpad path)**
- **Wheel / two-finger dolly: smooth, never passes through the pivot**
- **Alt+RMB drag dolly: right/up zooms in, diagonal reinforces**
- **RMB fly: WASD relative, Q/E world-vertical, Shift ≈4×, overlay reads `fly N.N u/s`**
- **Frame-rate independence: same distance focused vs. unfocused (20 Hz cap)**
- **HiDPI pan speed matches a 1× display (S12's human row)**
- **`F` on Cube/parent/light: correct framing, angle preserved**
- **`F` with nothing selected / in an empty scene: scene-frame or reset, no ERROR**
- **`F` while renaming: camera does not move**
- **Wheel routing: other panels scroll, Viewport dollies, no dock-node bubble**
- **Plain LMB in the Viewport does nothing**
- **Delete `Main Camera`: viewport keeps rendering, no overlay text, no WARN**
- **Editing `Camera.fovYRadians` no longer affects the Viewport**
- **Resize/undock/redock/maximise mid-gesture: no jump, no crash, no stuck gesture**
- **Drag outside the window and release: gesture ends cleanly, nothing latched**
- **Two buttons at once: orbit wins over a mid-drag RMB press**
- **Linux only: Alt+LMB WM caveat and X11/Wayland noted**

### macOS — ✅ PASS (2026-07-28)

Machine: MacBook Pro (Apple M1 Pro), Metal. Human mouse/keyboard pass against `91f3887` — the merged
tree, including the code-review gap fixes and the `<wingdi.h>` rename.

**All nineteen macOS-applicable rows pass.** Row 20 is Linux-only and is not runnable here.

- **1 — Launch: cube framed in a right-front ¾ view, lit, not edge-on, not clipped** — PASS
- **2 — Alt+LMB orbit: drag right turns right, drag down raises the eye, no roll, clean pole stops** — PASS
- **3 — MMB pan: tracks the cursor at pivot depth, still 1:1 fully dollied in and out** — PASS
- **4 — Alt+Cmd+LMB pan (the Mac-trackpad path): identical behaviour** — PASS
- **5 — Wheel / two-finger dolly: smooth, never passes through the pivot, never inverts** — PASS
- **6 — Alt+RMB drag dolly: right/up zooms in, diagonal reinforces** — PASS
- **7 — RMB fly: WASD view-relative, Q/E world-vertical at any pitch, Shift ≈4×, overlay reads
  `fly N.N u/s` only while RMB is held** — PASS
- **8 — Frame-rate independence: same distance travelled focused and unfocused (20 Hz cap)** — PASS
- **9 — HiDPI pan speed (S12's human row): a ~100-pt drag moves the same on-screen amount as on a 1×
  display — D15 holds, `viewportHeightPoints` is fed points, not pixels** — PASS
- **10 — `F` on Cube / on a parent (whole subtree) / on the Directional Light (sane distance):
  correct framing, viewing angle preserved** — PASS
- **11 — `F` with nothing selected frames the scene; `F` in an emptied scene returns to the launch
  pose, with no ERROR lines in the Console** — PASS
- **12 — `F` while renaming an entity: the camera does not move and the name commits** — PASS
- **13 — Wheel routing: Hierarchy/Console/Assets scroll, the Viewport dollies, no dock-node bubble** — PASS
- **14 — Plain LMB in the Viewport does nothing; no undock, no window move; Alt+LMB still orbits
  after** — PASS *(the binding 2.3.2 depends on staying free)*
- **15 — Delete `Main Camera`: the viewport keeps rendering lit, no "No camera in scene" text, no
  WARN** — PASS *(replaces 2.2.3's row 12)*
- **16 — Editing `Camera.fovYRadians` no longer changes the Viewport** — PASS *(the user-visible
  proof of the whole task)*
- **17 — Resize / undock / redock / maximise mid-gesture: no jump, no crash, no stuck gesture,
  aspect stays correct, `{W}×{H}` keeps updating** — PASS
- **18 — Drag outside the window and release: the gesture continues outside, ends cleanly, nothing
  latched** — PASS *(macOS is a capture platform — F18; the Wayland stall is C8 and a Linux row)*
- **19 — Two buttons at once: a mid-drag RMB press does not switch an orbit to fly** — PASS
- **20 — Linux-only WM caveat (Alt+drag swallowed by GNOME/KDE) and X11/Wayland session** — **N/A on
  macOS**

The mechanical/structural pass above also ran and is green (build, full ctest on both presets, the
`AERO_REQUIRE_GPU=1` rehearsal, the tools-OFF proof with exactly two WARNs, the non-interactive launch
proof, thirteen sabotage proofs each seed-confirmed and reverted (two recorded as expected
non-discriminations, one recorded as a partial-discrimination finding), all five guards, clang-format
and clang-tidy clean with zero new `NOLINT`s).

**macOS half of the 2.3.1 gate: CLOSED.**

### Windows — ⏳ pending

Needs a native run. No checks recorded yet.

### Linux — ⏳ pending

Needs a native run (real Vulkan or native Wayland/X11; **not** lavapipe/CI, which cannot exercise
window-manager or compositor interaction). No checks recorded yet.

**Task 2.3.1 gate status: macOS-PASS — mechanically green and human-validated on macOS (19/19
applicable rows), Windows/Linux human passes pending.** Epic 2.3 (Manipulation) is now **OPEN in
code** (2.3.1 is its first landed task). The gate stays open **only** for the two native runs; the
macOS half is closed. Note that Windows and Linux are already green in **CI** (build + full ctest on
both presets, PR #53) — what is outstanding is the *human* mouse/keyboard pass on each, which CI
cannot perform. Row 20 exists specifically for the Linux run.

# Task 2.3.2 — selection & picking

**Deliverable:** clicking a mesh in the Viewport selects it; clicking empty space clears the
selection; `Ctrl`/`Cmd` toggles and `Shift` adds without ever removing; whatever is selected — from
the Viewport or the Hierarchy — draws back into the Viewport as a wireframe box, with the primary
entity visually distinguished by colour and thickness. This is what turns the Viewport from a thing
you only look *through* into a thing you *reach into*; 2.3.1 shipped the camera and deliberately left
plain LMB unbound for exactly this task.

**What CI proves automatically:** the tier-0 batteries in `picking_test.cpp` (the screen mapping, the
basis ray, the local-box slab test, the world pick walk, and all sixteen combinations of the
hit/modifier click-decision table) and `selection_overlay_test.cpp` (the 12-edge table re-derived from
its bit assignment, segment counts and roles, transform tracking including the OBB-vs-AABB rotation
discriminator, near-plane clipping, the entity cap, hostile input, scratch reuse), both riding
`aero_editor_shell_test`; and five new `imgui_layer_test.cpp` cases driving the real
`ViewportPanel::updatePick`/`drawSelectionOverlay` path through a real `EditorApp::tick()` — the
overlay executing and staying ImGui-balanced, a dead handle left in the selection, a non-mesh
selection, 300 real entities under the cap, and a full pick/select/clear sequence mutating nothing but
the `Selection` (entity/component counts and all eight `EditorCamera` accessors, unchanged).

**What it cannot prove:** anything requiring a synthesised ImGui mouse click. `ImGui_ImplSDL3_NewFrame`
overwrites any injected mouse position from SDL every frame, and there is no window under a real
cursor in CI. What *is* mechanically covered is the whole chain
`mouse points -> NDC -> ray -> entity -> PickAction -> Selection`, because every link is a pure
function tested in isolation; what is *not* covered is that `onDraw` hands those functions the right
ImGui values — three lines wide (`updatePick`'s ARM/FIRE gates), named here, and closed only by the
human pass below (rows 1–8). The colour/thickness choices (row 3), the feel of the 4-point slop
(row 6), and the HiDPI pick radius (row 9, S13's human row) are human-only for the same reason.

## What was mechanically verified (this implementation pass, macOS, Apple M1 Pro / Metal)

Measured at every one of the five commit boundaries, not once at the end:

- `ctest --preset macos-debug` and `--preset macos-release`: **94/94** both presets, at every commit.
- `ctest --preset macos-debug -N` → **Total Tests: 94** throughout; `AERO_REQUIRE_GPU=1 ctest` green on
  both presets (the CI ratchet rehearsed, not skipped).
- Fresh `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` configure into `build/tools-off-2.3.2`:
  **5/5**, `aero_editor` launches with **exactly two** WARN lines (2.2.2's reflection WARN, 2.2.3's
  shader WARN) and **no third** — E11 confirmed: `onDraw` returns at its status gate, before phase 8c,
  whenever the Viewport is `Unavailable`, so this task adds no new tools-gate WARN.
- Doctest case counts, measured with `--list-test-cases`, never predicted: `aero_editor_shell_test`
  **117 → 123 → 131 → 141** across the three code-bearing commits, then **141 → 145** in the
  review round below; `aero_editor_imgui_test` **14 → 19**; `aero_tests` unchanged at **356**.
- The non-interactive launch check (`aero_editor`, seeded scene): **zero** new ERROR/CRITICAL/WARN
  lines, run after every ImGui-touching step, after the tools-OFF configure, and again after the final
  format pass.
**The sabotage table's outcome** — all thirteen, every seed confirmed present via `git diff` before trusting the verdict, every one reverted and re-confirmed green afterward:


| | Seed | Result |
|---|---|---|
| S1 | `rayLocalBoxHit`: `!(tMin > 0.0F)` → `!(tMax > 0.0F)` | reddened case 4's inside (both arms) and on-a-face subcases exactly; the aimed-away and entirely-behind subcases stayed green |
| S2a | `rayLocalBoxHit`: normalise `direction` right after the guard | reddened case 6 arm A as predicted, and arm B too — see note (b); cases 4 and 7 stayed green |
| S2b | `pickEntity`: wrap the local direction in `normalizeOrZero` at the call site only | reddened case 6 arm B exactly, and *only* arm B; arm A and cases 4/7 stayed green |
| S3 | `pickEntity`, mesh branch: replace the OBB test with the plan's own literal world-AABB one (`h = max(hs.x, hs.y, hs.z)`) | reddened case 7's MISS subcase (the primary discriminator) exactly; also perturbed the HIT subcase's distance — see note (a) |
| S4 | `pickEntity`: drop the `e.index < mesh.entity.index` tie-break arm | reddened case 8's forced-inversion tie subcase exactly; the nearest/destroy/parenting/dead-handle subcases stayed green |
| S5 | `pickEntity`: `if (point.hit() && (...))` → `if (point.hit())` | reddened case 10's BEHIND subcase exactly; the FRONT subcase and case 9 stayed green |
| S6 | `buildSelectionOverlay`: build the mesh box from the world AABB instead of the OBB | reddened case 4's rotation arm (S6's discriminator) exactly; the translate and scale arms stayed green |
| S7 | `appendBoxEdges`: reject whole corners (`a.w > eps && b.w > eps`) instead of clipping the edge | reddened case 7's straddling subcase exactly (8 → 4 segments); the entirely-behind subcase stayed green |
| S8 | `clipSegmentToNearPlane`: replace the clip-space lerp with a post-divide one | reddened case 6's straddling subcase exactly (both assertions); both-in-front and both-behind stayed green — see note (a) |
| S9 | `pickSelectionAction`: swap the `Toggle`/`Add` returns for `ctrlOrCmd`/`shift` | reddened case 12 exactly (6 assertions across the swapped rows); case 13 stayed green |
| S10 | `pickSelectionAction`: the miss branch → unconditional `Clear` | reddened 3 of case 12's 4 miss rows (6 assertions); the no-modifier row already expects `Clear`, so it is correctly unaffected |
| S11 | `buildSelectionOverlay`: delete the `MAX_HIGHLIGHTED_ENTITIES` cap check | reddened case 8 exactly — 3600 segments instead of 3072, plus the beyond-cap-primary assertion; cases 2–7 and 9–10 stayed green |
| S12 | *(see below)* | **not mechanically seeded — recorded as a documented non-discrimination, exactly as the plan predicts** |
| S12b | `pickEntity`: add `AERO_LOG_WARN` before the final `return mesh` | reddened 3 of case 11's 4 subcases; the 4th is the anti-vacuity canary — see note (c) |
| S13 | `onDraw`'s call to `updatePick`: pass `drawExtent` (pixels) instead of `avail` (points) as the pick's `viewportSizePoints` | **seeded for real, reddened NOTHING** — as predicted — see note (d) |

**(a) S3 and S8 were second-order checked.** Both seeds perturbed an assertion beyond their table
entry, and both were re-tested by weakening the discriminating assertions to `CHECK(true)`: with them
weakened, the seeded defect passes the whole suite. So those assertions — not the harness — do the
real work. S3's extra perturbation is the HIT subcase's *distance* value (a uniform max-extent box is
not the true anisotropic AABB even where both still register a hit); its entity/isPoint checks still
passed.

**(b) S2a reddens one arm more than its table entry.** It normalises inside `rayLocalBoxHit` itself
rather than at the call site, so case 6 arm B — which calls the same sabotaged function — reddens
too. An expected mechanical consequence, not a contradiction; S2b isolates arm B alone.

**(c) S12b's fourth subcase cannot redden from this seed.** Case 11's anti-vacuity canary asserts
that `each<NeverRegistered>` *does* log, and never calls `pickEntity` at all. Its staying green is
the correct outcome and is what proves the other three are not vacuous.

**(d) S13 is human-only by construction.** The whole 94/94 tier-0 suite plus all 19 GPU-gated cases
stayed green with the defect live, exactly as the plan predicts: points and pixels coincide on a
non-Retina runner, so nothing in this harness can discriminate D18's pixel/point distinction. Row 9
of the human pass is the only thing that can.

**S12 — recorded, not forced (§A3).** The spec's stated seed ("use `each<MeshRenderer>` in `pickEntity`
instead of `eachEntity` + `has`") does not compile: `pickEntity` takes `const World&` and
`World::each<Ts...>` is non-const. Widening the signature to `World&` compiles but reddens nothing
either — every `World`'s constructor registers `MeshRenderer` unconditionally, so `beginQuery` never
takes its `AERO_LOG_ERROR` path on a live `World`, and on a moved-from `World` it bails at
`impl == nullptr` before that ERROR anyway. The silence guarantee (AC-11/INV-5) is held
**structurally, by the `const World&` signature itself** — a compile-time property strictly stronger
than any reddening test — and S12b plus case 11's `each<NeverRegistered>` canary are what prove the
assertion is not vacuous. Identical in shape to 2.3.1's own S9/S12 non-discriminations.

- All five guards green with **no allowlist change**: `check-math-boundary.sh`'s scanned count moved
  **197 → 203** (+6: `picking.hpp/.cpp`, `selection_overlay.hpp/.cpp`, `picking_test.cpp`,
  `selection_overlay_test.cpp` — measured against `origin/main` in a disposable `git worktree`, not
  assumed); `check-golden-rule.sh`, `check-rhi-boundary.sh`, `check-scene-boundary.sh`,
  `check-platform-boundary.sh` all unaffected (this task changes no `engine/` file and adds no
  SDL/EnTT identifier).
- `git diff --name-only origin/main` empty over `engine/`, `runtime/`, `samples/`, `tools/`,
  `shaders/`, `cmake/`, `.github/` and `vcpkg.json`; the `/vcpkg` submodule SHA unchanged;
  `editor/src/imgui_layer.{hpp,cpp}` and `editor/src/main.cpp` byte-identical for the **eighth** task
  running; `ViewportPanel::renderScene` byte-identical (no hunk at or after its signature in
  `git diff origin/main -- editor/src/viewport_panel.cpp`); `aero_editor_shell_test`'s and
  `aero_editor_core`'s link lines byte-identical (verified against `origin/main`).
- clang-format and clang-tidy clean on every touched file at every commit boundary, with **zero new
  `NOLINT`s** — the one pre-existing `reinterpret_cast` NOLINT in `viewport_panel.cpp` (2.2.3) is
  unchanged.

### The review round (2026-07-29)

A code review before merge found **no functional defect**, but four assertions that could not fail.
Each was confirmed vacuous the same way the table above works — seed the defect, confirm it landed with
`git diff`, rebuild, and watch the suite stay green — then closed with tests only, **zero
production-code change** (141 → 145 cases):

| | Gap | Proof it is now caught |
|---|---|---|
| 1 | the E4 non-finite guards, both sites, had no coverage | deleting either guard now reddens its own new case |
| 2 | AC-7's zero-scale / non-finite clauses never reached `pickEntity` | seven new degeneracy subcases |
| 3 | the point-candidate index tie-break could not decide | rewritten to force a visit-order inversion and `REQUIRE` it |
| 4 | the A7 "dead handles do not consume cap budget" claim | 11 dead handles among 256+ live, asserting the exact cap |

**The trap worth keeping.** The pre-existing E4 test used `position.x = INF`, which puts `inf` in the
model matrix, makes every clip `w` come out `0 * inf = NaN`, and so drops all twelve edges in
`clipSegmentToNearPlane` *before* the finiteness guard ever runs — `allFinite` over an empty vector is
vacuously true. Reaching that guard needs a huge but **finite** transform (uniform `1e34` straddling
the eye). Handing a function its most extreme input is not automatically the test that reaches the
guard: the extreme input can be rejected earlier, by a different guard, for a different reason.

**A second finding, about a constant rather than a test.** `DETERMINANT_EPSILON`'s guard turns out to
be *redundant* — deleting it leaves the suite green, because GLM's `inverse` of a singular matrix
yields NaN that `rayLocalBoxHit`'s `allFinite` already rejects. It was left in place (readable, cheap,
states intent locally). But retuning it the wrong way was **not** covered: seeding `1e-20 → 1e-6`
reddened nothing, meaning an over-eager epsilon would have silently made small legitimate objects
unclickable with a fully green suite. Now pinned by an explicit "a uniform 1e-4 scale stays pickable"
subcase.

All five CI lanes passed first try, which also settled the one open portability question: the
`1e34`-scale case's expected segment count was measured on macOS/arm64 only, and Windows/MSVC and
Linux/GCC both confirmed it.

## Known-and-expected, NOT a defect

- **D13/F19: the Plane primitive's fat box** — flat at local `y = 0`, but picked and highlighted as a
  full 1-unit-thick box (`LOCAL_MESH_HALF_EXTENT = 0.5F` on every axis). Fixed in one change, alongside
  `LOCAL_MESH_HALF_EXTENT`'s own documented expiry, once 3.1.x publishes real per-mesh bounds.
- **D17: the highlight is not depth-occluded** — it always draws on top, deliberately; a depth-correct
  outline is a stencil-pass feature for a later editor-chrome task, not this one.
- **D8: the point marker is invisible until its owning entity is selected** — a light or camera has no
  always-on gizmo icon in this task; that is Handoffs' "always-on gizmo icons" item.
- **E12: a viewport pick reaches the Hierarchy and Inspector on the *next* frame** — one tick (~16 ms)
  of latency, which should be imperceptible.
- **G6: the seeded `Directional Light` sits at the world origin, *inside* the seeded `Cube`** — D5's
  depth rule correctly makes the cube win every click there, so the light is unpickable until moved.
  Move it to e.g. `{3, 2, 0}` in the Inspector before trying to click it (§H row 13).
- **The highlight draws over other geometry** — always-on-top by design (same as D17 above); revisit
  only as part of a general editor-chrome render pass, never as a one-off.
- **2.2.5's four BLOCKED validation rows stay blocked** — every path this task adds is deliberately
  silent (AC-11/INV-5), so it introduces no triggerable runtime log source. 2.4's undo remains the
  natural candidate to unblock them.

## How to validate one OS

Twenty-two rows, per OS, macOS first. Rows **1–8** close the "does `onDraw` hand the pure functions
the right ImGui values" gap named above; row **9** is S13's human row; rows **12–14** are
known-and-accepted behaviours a validator who does not know them will file as defects.

1. **Click the Cube** — it is selected: an amber box appears around it in the Viewport, the Hierarchy
   row highlights, and the Inspector fills. *(The Hierarchy/Inspector update on the next frame — E12,
   ~16 ms — which should be imperceptible; note it if it is not.)*
2. **Click empty space** — the selection clears, the box disappears, the Inspector empties.
3. **`Ctrl`/`Cmd`+click** a second object — both are boxed, and the newly clicked one is **brighter and
   thicker** (it is the primary). `Ctrl`/`Cmd`+click it again — it is removed and its box goes.
   *This row is also where the colour and thickness choices are judged.*
4. **`Shift`+click** — adds without ever removing; `Shift`+clicking an already-selected object changes
   nothing at all (not even the primary).
5. **`Ctrl`/`Cmd`+click empty space** — the selection is **unchanged**, not cleared.
6. **Press on the Cube, drag 200 points, release** — nothing is selected. Press and release without
   moving — it is selected. *Judge whether the 4-point slop feels right; it is a one-line retune.*
7. **Press on the Cube, drag outside the window, release there** — nothing is selected, nothing
   latches, and the next click inside works normally.
8. **`Alt`+LMB drag** starting on the Cube — the camera orbits and the selection does **not** change.
9. **HiDPI — S13's human row.** On a Retina display, the clickable radius around a (moved — see
   row 13) Directional Light feels the same as on a 1× display, and the slop feels the same. If a
   light needs a pixel-perfect click on Retina, **D18 was violated** — `viewportSizePoints` is being
   fed pixels.
10. **Rotate the Cube 45°** in the Inspector — the box **rotates with it** and still hugs it. Click just
    outside a corner, inside where an axis-aligned box would be: **nothing is selected.** *This row is
    the user-visible proof of D2 and of the whole local-space design.*
11. **Scale the Cube to 3** — the box grows with it and picking follows.
12. **Select a Plane and click half a unit above it** — it **is** selected. This is **D13**, expected,
    and the drawn box shows exactly why. Record it as observed-and-accepted, not as a defect.
13. **Click the Directional Light.** **First move it** — the seeded light sits at the world origin,
    inside the seeded Cube, where D5's depth rule correctly makes the Cube win every click (G6). Set
    its position to e.g. `{3, 2, 0}` in the Inspector, then click it: it is selected and shows a small
    diamond marker. Note honestly that the target is **invisible until hit** (D8).
14. **A light in front of / behind a cube** — position a light 1 unit in **front** of a cube and click
    where they overlap: the **light** is selected. Move it 1 unit **behind**: the **cube** is selected.
    *(D5's depth rule.)*
15. **Fly the camera into the Cube** — the box's near edges **clip cleanly** rather than popping out of
    existence (D14); clicking from inside selects **what is behind it**, not the cube (D3). *If the
    clipped edges shoot off to visibly absurd screen positions, `CLIP_W_EPSILON` is the one-line knob —
    raise it toward the camera's near plane and re-judge.*
16. **Select something, then fly far away** — the box shrinks correctly, stays aligned, and never
    smears or jitters.
17. **Select something, then move the Viewport** — undock, redock, resize, maximise: the box stays on
    the object and is **clipped at the panel edge**, never drawn over the Hierarchy (E8).
18. **Select in the Hierarchy** — the Viewport box appears for it too. One selection, two entry points
    (F4).
19. **Select 10+ entities via the Hierarchy** — every one is boxed, and **exactly one** is
    primary-styled.
20. **Delete the selected entity from the Hierarchy** — the box disappears the same frame the entity
    does; no crash, no ghost box (E2).
21. **Click while renaming** — double-click a Hierarchy name to rename, then click the Viewport: the
    rename commits and the click selects (E14). *Deliberately different from `F`, which **is** gated on
    `io.WantTextInput`: a key press is genuinely ambiguous where a click is not.*
22. **Linux only** — confirm rows 1–6 with 2.3.1's D4 window-manager `Alt`+drag caveat in mind; picking
    itself uses no modifier a WM steals.

## Validation records

- **Click the Cube: it is selected, boxed, Hierarchy/Inspector follow next frame**
- **Click empty space: selection clears**
- **`Ctrl`/`Cmd`+click a second object: both boxed, newly clicked is brighter/thicker (primary); toggling removes it**
- **`Shift`+click: adds without removing; already-selected is a true no-op**
- **`Ctrl`/`Cmd`+click empty space: selection unchanged, not cleared**
- **Drag 200pt then release: no selection; press/release without moving: selected (4-point slop)**
- **Press, drag outside, release outside: nothing selected, nothing latched**
- **`Alt`+LMB drag on the Cube: camera orbits, selection unchanged**
- **HiDPI clickable radius matches a 1x display (S13's human row)**
- **Rotate the Cube 45°: box rotates with it; a corner-adjacent click outside the OBB misses**
- **Scale the Cube to 3: box grows with it, picking follows**
- **Select a Plane, click half a unit above it: selected (D13, expected)**
- **Click the (moved) Directional Light: selected, shows a diamond marker**
- **Light in front of / behind a cube: nearer of the two wins the click**
- **Fly into the Cube: near edges clip cleanly; clicking from inside selects what's behind**
- **Select something, fly far away: box shrinks correctly, stays aligned**
- **Select something, move the Viewport: box stays on the object, clips at the panel edge**
- **Select in the Hierarchy: the Viewport box appears too (one selection, two entry points)**
- **Select 10+ entities via the Hierarchy: all boxed, exactly one primary-styled**
- **Delete the selected entity from the Hierarchy: box disappears same frame, no crash**
- **Click while renaming: rename commits, click selects**
- **Linux only: WM Alt+drag caveat noted; picking itself uses no modifier a WM steals**

### macOS — ✅ PASS (2026-07-29)

Machine: MacBook Pro (Apple M1 Pro), Metal. Human mouse/keyboard pass against `099ce08` — the merged
tree, including the review round's four closed coverage gaps.

**All twenty-one macOS-applicable rows pass.** Row 22 is Linux-only and is not runnable here.

- **1 — Click the Cube: selected, amber box appears, Hierarchy row highlights, Inspector fills** — PASS
  *(the next-frame Hierarchy/Inspector update, E12, was imperceptible)*
- **2 — Click empty space: selection clears, box disappears, Inspector empties** — PASS
- **3 — `Ctrl`/`Cmd`+click a second object: both boxed, the newly clicked one brighter and thicker;
  `Ctrl`/`Cmd`+click again removes it** — PASS *(the colour/thickness choice reads correctly as
  primary-vs-selected — a human-only judgement)*
- **4 — `Shift`+click: adds without ever removing; `Shift`+clicking an already-selected object is a
  true no-op** — PASS
- **5 — `Ctrl`/`Cmd`+click empty space: the selection is unchanged, not cleared** — PASS
- **6 — Press, drag 200 pt, release: nothing selected; press and release without moving: selected** —
  PASS *(the 4-point slop feels right — a human-only judgement)*
- **7 — Press inside, drag outside the window, release there: nothing selected, nothing latched; the
  next click inside works normally** — PASS *(E9/F29 — macOS is a capture platform)*
- **8 — `Alt`+LMB drag starting on the Cube: the camera orbits and the selection does not change** —
  PASS *(D10's already-arbitrated gesture test)*
- **9 — HiDPI (S13's human row): the clickable radius around the Directional Light and the click slop
  feel the same as on a 1× display** — PASS *(**D18 holds** — the pick is fed points, not pixels. This
  is the row no mechanical test in this harness can reach)*
- **10 — Rotate the Cube 45°: the box rotates with it and still hugs it; a click just outside a corner,
  inside where an axis-aligned box would be, selects nothing** — PASS *(the user-visible proof of D2 —
  a local-space OBB test, not a world AABB)*
- **11 — Scale the Cube to 3: the box grows with it and picking follows** — PASS
- **12 — Select a Plane and click half a unit above it: it is selected** — PASS *(**D13, expected and
  accepted** — the Plane's knowingly fat box; the drawn box shows exactly why. Not a defect)*
- **13 — Click the Directional Light (moved off the origin first, per G6): selected, shows a small
  diamond marker** — PASS *(the target is invisible until hit — D8, recorded and accepted)*
- **14 — A light in front of / behind a cube: clicking where it sits in front selects the light, where
  it sits behind selects the cube** — PASS *(D5's depth rule)*
- **15 — Fly the camera into the Cube: the box's near edges clip cleanly rather than popping out of
  existence; clicking from inside selects what is behind it, not the cube** — PASS *(D14's per-edge
  clip-space clipping, and D3's entry-hits-only rule)*
- **16 — Select something, then fly far away: the box shrinks correctly, stays aligned, never smears
  or jitters** — PASS
- **17 — Select something, then undock / redock / resize / maximise the Viewport: the box stays on the
  object and is clipped at the panel edge, never drawn over the Hierarchy** — PASS *(E8 — the clip
  rect is not optional)*
- **18 — Select in the Hierarchy: the Viewport box appears for it too** — PASS *(F4 — one selection,
  two entry points; the sentence `selection.hpp` has carried since 2.2.1)*
- **19 — Select 10+ entities via the Hierarchy: every one is boxed, exactly one is primary-styled** —
  PASS
- **20 — Delete the selected entity from the Hierarchy: the box disappears the same frame the entity
  does; no crash, no ghost box** — PASS *(E2)*
- **21 — Click while renaming: the rename commits and the click selects** — PASS *(E14 — deliberately
  ungated, unlike `F`)*
- **22 — Linux-only WM `Alt`+drag caveat** — **N/A on macOS**

Rows 1–8 are what close the "does `onDraw` hand the pure functions the right ImGui values" gap named
above — the three lines of `updatePick`'s ARM/FIRE gates that no tier-0 or GPU-gated test can reach
(AC-20). Row 9 is the only possible proof of D18's points-vs-pixels split, since S13 is provably
non-discriminating on this harness. Rows 12–14 are the known-and-accepted behaviours, observed and
confirmed as behaviour rather than filed as defects.

The mechanical/structural pass above also ran and is green (build, full ctest on both presets, the
`AERO_REQUIRE_GPU=1` rehearsal, the tools-OFF proof with exactly two WARNs, the non-interactive launch
proof, all thirteen sabotage proofs each seed-confirmed and reverted, the review round's four closed
gaps, all five guards, clang-format and clang-tidy clean with zero new `NOLINT`s, and all five CI
lanes green).

**macOS half of the 2.3.2 gate: CLOSED.**

### Windows — ⏳ pending

Needs a native run. No checks recorded yet.

### Linux — ⏳ pending

Needs a native run (real Vulkan or native Wayland/X11; **not** lavapipe/CI, which cannot exercise
window-manager or compositor interaction). No checks recorded yet.

**Task 2.3.2 gate status: mechanically green (build + full ctest on both presets, the
`AERO_REQUIRE_GPU=1` rehearsal, the tools-OFF proof with exactly two WARNs, the non-interactive launch
proof, all thirteen sabotage proofs each seed-confirmed and reverted — one recorded as a documented
non-discrimination (S12), one confirmed to redden nothing as predicted (S13), two second-order-checked
(S3, S8) — plus the review round's four closed gaps, all five guards green, clang-format and clang-tidy
clean with zero new `NOLINT`s, and all five CI lanes green), and the **macOS human pass is COMPLETE —
21 of 21 applicable rows PASS** (2026-07-29), which closes AC-20's human-only gap and confirms D18 on a
Retina display. Windows and Linux human passes remain pending.** Epic 2.3 (Manipulation) remains **in progress in code**
(2.3.3 ImGuizmo transform gizmos is next).

# Task 2.3.3 — ImGuizmo transform gizmos

**Deliverable:** dragging a gizmo handle in the Viewport transforms the primary selected entity —
translate, rotate or scale — in local or world space, with optional hold-to-snap, writing the result
into the entity's own parent-relative `Transform` through the new `transform_ops` seam task 2.4.2's
undo commands will wrap unchanged. This is the other half of direct manipulation Epic 2.3 opened:
2.3.1 made the Viewport navigable, 2.3.2 made its contents selectable, and this task makes them
*movable* with the mouse — the Inspector's reflection-driven numeric fields are no longer the only
way to move an entity.

**What CI proves automatically:** two tier-0 batteries riding `aero_editor_shell_test` —
`gizmo_test.cpp` (17 cases, G1–G17: the mode/key state machine, `effectiveSpace`'s Scale-forces-Local
rule, the three snap steps as relationships never magnitudes, the drag-edge table, `gizmoModelMatrix`/
`gizmoParentMatrix`'s World-walk arms, the channel-isolation property G7 — this task's highest-value
case, now also asserting the Rotate arm's positive value — the identity round-trip, a rotated-parent
translate, a uniformly-scaled-parent rotate, a genuine-shear `NotDecomposable` (caught by the
code-review round's `isSheared()` guard, restored to the plan's original construction), a
singular-parent `NotFinite`, hostile-input `NotFinite`, a huge-but-finite scale with a structurally
argued step-6-unreachability comment, and `gizmoOriginBehindCamera`'s near-plane predicate — now TWO
tests, mirroring both our own `w`-based check and ImGuizmo's own raw-`z` check, with a positive control
and a dedicated case pinning the code-review round's widened band fix) and `transform_ops_test.cpp` (7
cases, T1–T7: the exact read/write round trip, silent rejections with
an anti-vacuity canary, no-mutation-anywhere on every rejection, no component creation as a side
effect, non-finite values stored as given, and tools-independence with no `entt::` anywhere on the
path) — plus five GPU-gated cases in `imgui_layer_test.cpp` (I1–I5) driving the real
`ViewportPanel::updateGizmo`/`drawGizmoBar` path through a real `EditorApp::tick()`: the gizmo path
executing and staying ImGui-balanced with a selected Cube, the behind-camera skip exercised by flying
the eye past the primary programmatically, an empty selection and a Transform-less primary both
skipping the gizmo cleanly, a hidden-then-shown Viewport surviving (also the proof that
`BeginFrame()`-without-`Manipulate` is safe), and the transparent "gizmo" overlay window leaving no
trace in a persisted `aero_editor.ini`.

**What it cannot prove:** anything requiring a synthesised ImGui mouse press or drag.
`ImGui_ImplSDL3_NewFrame` overwrites any injected mouse position from SDL every frame, and there is no
window under a real cursor in CI. Named precisely, the uncovered surface is: `updateGizmo`'s four ImGui
key-state reads (W/E/R/X) and their three gates (hover, `WantTextInput`, no-camera-gesture);
`io.KeyCtrl` driving the snap; the exact arguments handed to `ImGuizmo::SetRect`/`SetDrawlist`/
`Manipulate`; the `mbOverGizmoHotspot` latch F4 describes (a one-way, no-test-can-see-it failure mode);
and the points-vs-pixels distinction (D18), provably indistinguishable from pixels on a non-Retina
runner (S7 confirms this — see below). Every one of these is named as a specific human row.

## What was mechanically verified (this implementation pass, macOS, Apple M1 Pro / Metal)

Measured at every commit boundary of the implementation pass (eight commits: the plan's seven
code-bearing/docs steps plus one extra fix for a prose collision in T7's own §V7 grep — §5 below), not
once at the end. The code-review round's three further commits are measured separately, in their own
section below.

- `ctest --preset macos-debug` and `--preset macos-release`: **94/94** both presets, at every commit;
  `AERO_REQUIRE_GPU=1 ctest` green on both presets (the CI ratchet rehearsed, not skipped).
- `ctest --preset macos-debug -N` → **Total Tests: 94** throughout — **no new `add_test`**, exactly as
  AC-20 requires; both new test TUs ride the existing `aero_editor_shell_test`/`aero_editor_imgui_test`
  targets.
- Doctest case counts, measured with `--list-test-cases`, never predicted:
  `aero_editor_shell_test` **145 → 151** (step 2, the tool-state model) **→ 162** (step 3, the
  geometry and write pipeline) **→ 169** (step 4, `transform_ops`) — the plan's own per-step
  checkpoints, all confirmed exactly; `aero_editor_imgui_test` **19 → 24** (I1–I5); `aero_tests`
  unchanged at **356**; `aero_editor_core` **23 → 25** sources (`gizmo.cpp`, `transform_ops.cpp`).
- Fresh `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` configure into `build/tools-off-2.3.3`:
  **5/5**, `aero_editor` launches with **exactly two** WARN lines (2.2.2's reflection WARN, 2.2.3's
  shader WARN) and **no third**.
- **New this task (AC-17) — `-DAERO_REFLECT_TOOLS=OFF` alone (shaders ON)** into
  `build/reflect-off-2.3.3`: **18/18**, measured not assumed (the tools-OFF five plus the 13
  `shaderc.*` cases, every `if(AERO_REFLECT_TOOLS)` block absent); the launch log shows
  `editor: shell ready (...)`, the 2.2.2 reflection WARN, and **no** reflection ERROR — the Viewport,
  and therefore the gizmo, both work with reflection off, and the write path never touches
  `entt::meta` (D21), exactly what this configuration exists to prove.
- `check-math-boundary.sh`'s scanned count moved **203 → 209** (+6: `gizmo.hpp/.cpp`,
  `transform_ops.hpp/.cpp`, `gizmo_test.cpp`, `transform_ops_test.cpp` — measured against
  `origin/main` in a disposable `git worktree`, not assumed); the other four guards
  (`check-golden-rule.sh`, `check-platform-boundary.sh`, `check-rhi-boundary.sh`,
  `check-scene-boundary.sh`) all green with **no allowlist change** — this task adds no SDL/EnTT
  identifier and changes no `engine/`/`runtime/` file.
- `git diff --name-only origin/main` empty over `engine/`, `runtime/`, `samples/`, `tools/`,
  `shaders/`, `cmake/`, `.github/`; `vcpkg.json`'s `builtin-baseline` and the `/vcpkg` submodule SHA
  byte-identical; `editor/src/imgui_layer.{hpp,cpp}`, `editor/src/main.cpp` and
  `editor/src/editor_app.cpp` byte-identical for the **ninth** task running;
  `ViewportPanel::renderScene` byte-identical (no hunk at or after its signature); both test targets'
  `target_link_libraries` lines byte-identical.
- clang-format clean over the **whole tracked tree** (mandatory this task — `.clang-format` itself
  changed) and clang-tidy clean on every touched file, with **zero new `NOLINT`s**.
- The non-interactive launch check (`aero_editor`, seeded scene): zero unexpected ERROR/CRITICAL/WARN
  lines at every ImGui-touching commit boundary.

### The sabotage table's outcome

All twelve seeds, every one confirmed present via `git diff` before trusting a verdict, every one
reverted and re-confirmed green afterward:

| | Seed | Result |
|---|---|---|
| S1 | `ImGuizmo::BeginFrame()` deleted from `drawShellUi` | **reddened nothing, by construction** — the `mbOverGizmoHotspot` latch needs a live hover across real frames, which this harness cannot synthesise. Human row 6 only. |
| S2 | `&& !gizmoActive` removed from `updatePick`'s arm condition | reddened nothing — I1 executes identically with no real click to arm. Human row 7 is the real check. |
| S3 | the `gizmoOriginBehindCamera` skip removed (`Manipulate` called unconditionally) | reddened nothing in **either** G15 or I2 — G15 tests the predicate itself (untouched), and I2 (which flies the eye past the primary) does **not** discriminate either, because it asserts only execution/balance/presentation, never the gizmo's screen position. This is a genuine non-discrimination beyond what the plan predicted (it named I2 as the discriminator); recorded honestly rather than forced. **Re-checked after the code-review round's widened predicate (below): unchanged** — S3 sabotages the call site in `viewport_panel.cpp`, not the predicate itself, so widening the predicate does not give it a new discriminator. Human row 12 remains the only real check. |
| S4 | `gizmoWriteFromWorld` made to write all three channels unconditionally | **reddened G7 and G10**, exactly as predicted. Second-order checked: weakening G7's non-primary-channel assertions to `CHECK(true)` makes the seeded defect pass the whole suite silently — proving the original assertions, not the harness, do the work. |
| S5 | the `decompose` failure branch dropped (assume success, use the untouched `Trs`) | **Reddens — but via a DIFFERENT case than before the code-review round, and this was re-measured against the full suite rather than G11 alone.** Seeded and confirmed on the final committed tree: `aero_editor_shell_test` goes 169/169 → **168 passed, 1 failed**, the failure being **G14** (`gizmo: huge-but-finite scale`), `gizmo_test.cpp:444`, `CHECK(write.status == GizmoWriteStatus::NotDecomposable)` reporting `1 == 3` (`Applied` instead of `NotDecomposable`). G14 is the discriminator because a huge-but-finite scale overflows `decompose()`'s internal `length()` and is rejected by its own column-length guard — a path `isSheared()` deliberately declines to answer for (it returns `false` on any non-finite/degenerate column, leaving those to `decompose()`). **What S5 no longer reddens is G11**, because `isSheared()` now runs upstream and rejects G11's shear construction before `decompose()` is reached — so for *that* input `decompose()`'s return value stops mattering. Both halves matter: `decompose()`'s failure branch remains load-bearing and tested (G14), and the new guard is separately load-bearing (S13/G11). |
| S6 | step 6's post-decompose finiteness sweep dropped | **reddened nothing, including G14** — a genuine, honest non-discrimination the plan itself authorised recording. See the G14/step-6 finding below for why. |
| S7 | `SetRect` fed pixels (`drawExtent`) instead of points (`avail`) | reddened nothing, exactly as predicted — points and pixels coincide on this non-Retina runner (the 2.3.2 S13 precedent). Human row 14 is the only real check. |
| S8 | `effectiveSpace` returns `requested` for `Scale` instead of forcing `Local` | **reddened G3**, exactly as predicted. |
| S9 | the `gizmoWarnLatched` latch removed (unconditional WARN every rejected frame) | reddened nothing — no I-series case can produce a real, sustained drag to exercise it. Recorded as **human-pass-only (row 11)**, exactly as the plan's own escape hatch anticipates. |
| S10 | `updateGizmo`'s call moved to *after* `updatePick`'s in `onDraw` | reddened nothing — I1 executes identically; the F8 ordering's user-visible consequence (a gizmo grab also firing a pick) is human row 7. |
| S11 | the `.clang-format` `IncludeCategories` entry for `<ImGuizmo.h>` removed | **reddened nothing in this tree** — see the finding below. Not the "build itself" failure the plan predicted, for a documented, verified reason. |
| S12 | `isUsing` renamed back to `using_` | **caught by clang-tidy** (`readability-identifier-naming`), exactly as A2 predicted — confirms the naming rule is a real CI failure, not a style opinion. |
| S13 *(added in the code-review round)* | `gizmo.cpp`'s new `isSheared()` guard neutered (`if (false && isSheared(local))`) | **reddened G11** — `write.status` comes back `Applied` (0) instead of `NotDecomposable` (3), because `decompose()` itself silently succeeds on the shear input once nothing rejects it first. This is G11's real discriminator post-fix; literal S5 (above) is not. |

### The code-review round (2026-07-29) — one BLOCKING fix, one SHOULD-FIX, two RECORD items

A code review before merge found `decompose()`'s shear-blindness (recorded below as an open finding in
the implementation pass) was **worse than measured**: not "in one scenario" but **never** rejected, with
a quantified consequence (a unit-cube corner up to tens of world units off; a stored quaternion measured
as non-unit down to `|q| = 0.962` across a fuzz sweep) and zero WARNs. Closed with a NEW guard entirely
inside `editor/src/gizmo.cpp` — `isSheared()`, called between `local`'s formation and `decompose()` —
because the engine's own contract assigns this responsibility to the caller, not to `decompose()`:
`transform.hpp:38-41` states a sheared matrix "decomposes to nonsense, which is out of contract."
`engine::decompose()` itself is **unchanged** — it remains shear-blind by contract, zero `engine/` file
touched, `AC-20` holds. `GIZMO_ORTHOGONALITY_EPSILON = 1e-4` is a measured constant (comment in
`gizmo.cpp` records the full derivation): every legitimate (non-sheared) construction tested tops out at
`8.0e-08` `|cos|` between normalised column pairs; the hardest genuine shear case tried measures
`1.7e-04` — three-plus orders of magnitude of separation. Every shipped tier-0 case was re-evaluated
under the guard and no verdict changed. **G11 was restored to the plan's original shear construction**
(parent `scaling({2,1,1})`, child rotated 45° about Z, a world-space rotation delta) and now genuinely
returns `NotDecomposable` — measured `maxAbsCos ≈ 0.33` for that construction, far over the threshold.

A second, SHOULD-FIX gap closed in the same round: `gizmoOriginBehindCamera`'s `w`-based test and
ImGuizmo's own behind-camera test (`ImGuizmo.cpp:2696-2698`, raw clip-space `z < 0.001`, no perspective
divide) are different quantities, leaving a reachable band — measured directly with this task's own
camera setup (eye at `z=5`, near `0.1`): roughly **0.0002 to 0.11** world units in front of the eye,
where the `w`-test alone said "in front" but ImGuizmo's own test would have refused, so `Manipulate` got
called and took exactly the early return that leaks an unmatched `PushClipRect` (F5). Fixed by widening
the predicate to also reject on ImGuizmo's own raw-`z` test, computed from the same `viewProj * origin`
already in hand; a new tier-0 subcase (a gizmo origin at `world z = 4.95` — 0.05 units into the band)
pins it. `gizmo.hpp`'s comment was corrected to describe what the widened predicate now actually does.

Two RECORD-level items closed alongside: G7's `Rotate` arm now asserts the rotation was actually
*written* (`approxEquals(write.transform.rotation, differentTrs.rotation)`), matching the positive
assertion `Translate`/`Scale` already had — AC-2's "changes only its own field" was covered, "changes
its own field" was not, for rotate. And an unused `<aero/editor/gizmo.hpp>` include was dropped from
`imgui_layer_test.cpp` — no symbol from it is used there.

**The sabotage table's S5 row surfaced an honest finding of its own during re-verification** (recorded
in the table above): with `isSheared()` now running upstream of `decompose()`, the literal S5 seed
(dropping only `decompose()`'s own failure branch) no longer discriminates **G11** — `isSheared()`
independently rejects the shear construction first, so `decompose()`'s return value stops mattering for
this input. What discriminates G11 now is dropping `isSheared()` itself (S13, added above).
**S5 nevertheless still reddens, via G14** — re-measured against the full suite, not G11 alone: a
huge-but-finite scale overflows `decompose()`'s internal `length()` and is caught by its own
column-length guard, which `isSheared()` deliberately does not intercept. The first pass through this
re-check concluded "S5 reddens nothing" from a G11-scoped run; running the whole suite corrected it.
**Both guards are independently load-bearing and independently tested.** S3 was
re-checked against the widened predicate and is unchanged: it sabotages the call site in
`viewport_panel.cpp`, not the predicate, so widening the predicate gives it no new discriminator.

### One finding that was FIXED, and one that corrected the plan and remains open — both verified at source against the pinned `engine::decompose()`

**G11 — `decompose()` does not detect shear at all; it detects only a degenerate/non-finite column —
FIXED in the code-review round.** The plan's literal E7 construction for `NotDecomposable` — a
world-space rotation delta applied under a non-uniformly-scaled parent, i.e. genuine shear — was built
and run directly against `engine::decompose()` (`glm_backend.cpp`). Measured result during the
implementation pass: `decompose()` **succeeded** (returned `true`) with a numerically **wrong** but
finite `Trs` — `Applied`, not `NotDecomposable`. `decompose()`'s own guard only ever rejected a column
whose length is non-finite or below `EPSILON`; it had no orthogonality test at all — so this was not
"one scenario" but the general case, confirmed by the code-review round's own quantification (a
unit-cube corner up to tens of world units off; a stored quaternion measured down to `|q| = 0.962`
across a fuzz sweep, silently rescaling per `transform.hpp:35`; zero WARNs). **Fixed** by adding
`isSheared()` in `editor/src/gizmo.cpp` — normalises the three linear-block columns and rejects if any
pairwise `|cos|` exceeds `GIZMO_ORTHOGONALITY_EPSILON = 1e-4` (a measured constant; see the code-review
round above and `gizmo.cpp`'s own comment for the full derivation) — called between `local`'s formation
and `decompose()`. `engine::decompose()` itself is **unchanged**: the fix lives entirely in the editor
layer because the engine's own contract puts it there (`transform.hpp:38-41`: a sheared matrix
"decomposes to nonsense, which is out of contract" — `decompose()` never promised to detect shear;
`gizmoWriteFromWorld` is the layer obliged not to feed it out-of-contract input). Zero `engine/` file
touched; `AC-20` holds. G11 was restored to the plan's original shear construction and now genuinely
returns `NotDecomposable`. **AC-10's guarantee now holds as stated**: shear under a non-uniformly-scaled
ancestor is refused with exactly one Console WARN, not silently applied.

**G14 — the "reaches step 6 and only step 6" case is unreachable for any scale magnitude tested.** The
plan predicted a uniform `1e34` scale would be finite going in, decompose successfully, and produce a
non-finite *result*, caught specifically by the write pipeline's own post-decompose finiteness sweep
(step 6) rather than by `decompose()` itself. Measured instead: `decompose()`'s internal `length()`
computation squares each column (`dot(c,c)`), which overflows `float` at `dot ~ 3.4e38` — i.e. at a
column length around `sqrt(FLT_MAX) ≈ 1.84e19` — so `decompose()` itself returns `false`
(`NotDecomposable`) before step 6 ever runs. The full magnitude range `1e10 .. 1e37` was scanned
looking for a finite input where `decompose()` succeeds yet yields a non-finite `Trs`; none was found —
the transition goes directly from a fully-finite success to an outright `decompose()` rejection, with
no intermediate band. G14 now asserts the **measured** status (`NotDecomposable`), not the predicted
one. **Strengthened in the code-review round: step 6 is STRUCTURALLY unreachable from `decompose()`'s
current implementation, not merely unscanned.** Translation is a direct read of the input matrix's own
column, already finite by the upstream `allFinite` guards. Scale is exactly what `decompose()`'s own
length guard proves finite before it is ever stored. Rotation comes from `glm::quat_cast` of a Mat3
built from NORMALISED columns, and `quat_cast`'s own four candidate terms
(`glm/gtc/quaternion.inl:83-86`) provably sum to zero for any input — verified directly against the
vendored source — so the chosen maximum is always `>= 0`, its `sqrt(max+1) >= 1`, and the resulting
reciprocal can never blow up. A 20-million-sample fuzz across this task's exponent range found zero
non-finite outputs, confirming it empirically as well as structurally. The code (step 6 itself) is kept
regardless: the unreachability rests on `decompose()`'s *current implementation*, not its published
contract, and this task's own new `isSheared()` guard changes what reaches it — structural
unreachability of one input class is not a promise about every future one. It ships as real, correct
defence in depth, **uncovered by any test in this task** — the same honest status as A4's stale-latch
clear.

**S11's own finding.** Both `#include <ImGuizmo.h>` lines carry an explanatory F3 comment immediately
above them. Verified directly: clang-format's `IncludeBlocks: Regroup` treats that comment as a block
separator and does not merge/reorder across it, **independent of whether the `.clang-format` category
exists**. A minimal repro without the comment (just a blank line) reproduces the hoist exactly as F3
describes; with the comment, it does not, category or no category. This means removing the category
does **not** break the build in this specific tree today — but the category remains the *correct*
engineering decision (exactly the plan's own reasoning: "its own trailing category is what makes the
order STRUCTURAL instead of a comment nobody reads") and is kept exactly as specified. A future edit
that strips the comment while leaving only a blank line would be unprotected without it.

## Known-and-expected, NOT a defect

- **E18/D0 — no undo yet.** `Ctrl+Z`/`Ctrl+Shift+Z` are still disabled stubs naming task 2.4.1; a gizmo
  drag is real and permanent until 2.4.2 wraps `transform_ops` in a command. Human row 22.
- **AC-4 — the space button is disabled and reads `Local` for `Scale`.** ImGuizmo forces local space
  for scale internally (skew avoidance); the bar mirrors it rather than silently disagreeing. Human
  row 4.
- **E7 — a sheared child does not move, and says so once.** Fixed in the code-review round (see the
  G11 finding above): `gizmo.cpp`'s `isSheared()` guard now refuses shear from a non-uniformly-scaled
  ancestor unconditionally, with exactly one Console WARN per drag (D12). There is no longer a
  scenario where this guarantee does not hold.
- **A8 — a gizmo-bar button overlapping a handle both presses the button and grabs the gizmo.** The bar
  is submitted after `Manipulate` (so gizmo lines paint over the buttons, not the reverse); a grab with
  no subsequent mouse motion produces `GizmoWriteStatus::NoChange` and writes nothing, so the
  interaction is self-neutralising. Human row 25.
- **A4 — the stale-latch clear (`Enable(false)` on a drag ImGuizmo never saw released) ships as
  documented defence in depth and is not test-covered.** Not GPU-test testable (the harness cannot
  synthesise a mouse press) and not tier-0 testable (ImGuizmo global state). Human row 24 only.
- **The seeded `Directional Light` sits inside the seeded `Cube`** (2.3.2 §G6, still true) — select it
  via the Hierarchy, not by clicking in the Viewport. Human row 20.
- **2.2.5's four BLOCKED validation rows stay blocked**, but this task changes the landscape: D12's
  degenerate/non-finite-transform WARN is a genuinely triggerable runtime log source (the first one
  since 2.2.5 shipped) — **the reliable recipe, verified directly against `gizmoWriteFromWorld`**: select
  ANY single entity (no parent needed), set one of its own `Transform.scale` components to `0` in the
  Inspector, then drag its own translate (or rotate) handle. `decompose()`'s own column-length guard
  rejects the zero-length axis (`GizmoWriteStatus::NotDecomposable`) on the very first frame the drag
  moves anything, and the Console gets exactly one WARN for the whole drag. The code-review round's
  shear fix means a shear drag (a non-uniformly-scaled parent + a rotated child) now reliably reaches
  the SAME WARN branch too, but that needs a parent/child hierarchy set up first, so the single-entity
  zero-scale recipe above is the simpler one to hand whoever re-runs the four blocked rows. It is out
  of scope to re-run 2.2.5's rows here — noted so whoever revisits that gate knows a source now exists,
  with the exact, verified reproduction.

## How to validate one OS

Twenty-five rows, per OS, macOS first. Rows **1–9** close the "does `onDraw` hand the pure functions
the right ImGui values" gap; row **6** is the only check for F4's `mbOverGizmoHotspot` latch; row **14**
is the only check for the points/pixels distinction (S7 cannot catch it on this hardware); row **11**
is the human check for the code-review round's shear fix (G11 — see above); row **24** is the only
check for A4's stale-latch clear, not test-covered; rows **22, 25** are known-and-accepted behaviours a
validator who does not know them will file as defects.

1. **Launch, click the seeded Cube in the Viewport.** Selection highlight (2.3.2) **and** a translate
   gizmo at the cube.
2. **Drag the red / green / blue arrow.** Moves along that world axis only; the Inspector's `position`
   updates live; `rotation` and `scale` do not move at all.
3. **Press `E`, drag a rotation ring.** Rotates about that axis. Inspector `rotation` updates;
   `position` and `scale` **do not**.
4. **Press `R`.** Scale handles appear; the space button reads **`Local`** and is **disabled** with a
   tooltip (AC-4). *If the tooltip does not appear, `SetItemTooltip` was used instead of
   `IsItemHovered(AllowWhenDisabled)`.*
5. **Press `W`, then `X` repeatedly.** Handles alternate between world-axis-aligned and
   entity-aligned. **Rotate the cube first** so the two are visibly different.
6. **Hover every handle for ~10 s without clicking, then try to drag.** Still grabbable. *(The F4
   `mbOverGizmoHotspot` latch — the one failure no test can see.)*
7. **Click directly on a gizmo handle.** The gizmo grabs; the selection **does not** change and no
   other entity is picked.
8. **Click empty space, then another entity.** Picking works exactly as in 2.3.2; the gizmo follows the
   new primary.
9. **Hold `Ctrl`/`Cmd` while dragging translate / rotate / scale.** Snaps to 0.5 u / 15° / 0.1.
   Release mid-drag ⇒ continuous again, **with no jump**.
10. **Parent the Cube under a new entity (Hierarchy drag-drop), move the parent, then drag the child.**
    The child moves where the mouse says; the Inspector shows a **parent-relative** position.
11. **Give the parent a non-uniform scale (`{2,1,1}`), rotate the child 45°, then drag it in WORLD
    space.** Nothing moves; **exactly one** WARN appears in the Console for the whole drag (D12,
    `editor::gizmo`'s `isSheared()` guard, fixed in the code-review round — G11/AC-10). A flood of
    WARNs means the `gizmoWarnLatched` latch is not working; the cube visibly moving to a wrong
    orientation would mean the shear guard regressed — either is a real defect, not expected behaviour.
12. **Select the cube, then fly the camera until the cube is behind you.** No gizmo drawn; **no visual
    corruption anywhere in the Viewport**.
13. **Start a translate drag, then press and hold RMB (fly) without releasing LMB.** The drag ends;
    the cube keeps the position it had reached; flying works normally.
14. **On a Retina display, drag a handle across the full width of the Viewport.** The handle tracks the
    cursor exactly — **no 2× drift**.
15. **Focus the Hierarchy's rename field, type `wersx`.** The text appears; **no gizmo mode change**.
16. **Hover the Hierarchy / Inspector / Console / Assets while the gizmo is on screen.** No handle
    highlights; no drag can start.
17. **Select an entity with no `Transform`** (create one through the raw World API, or remove the
    component in the Inspector). No gizmo; the overlay bar is **greyed with a tooltip**.
18. **Resize the Viewport, dock it elsewhere, tab it away and back.** The gizmo stays correctly
    positioned and clipped to the panel; **no bleed over other panels**.
19. **View menu.** No `"gizmo"` entry. Quit and reopen ⇒ the layout is restored and `aero_editor.ini`
    contains **no** `gizmo` window.
20. **Select the Directional Light and drag translate.** It moves. *(Seeded inside the Cube — select
    via the Hierarchy first.)*
21. **Grab a handle and release without moving.** Nothing changes; no Inspector flicker.
22. **Undo (`Ctrl+Z`).** **Still a disabled stub.** Expected — closes at 2.4.2.
23. *(Linux only)* **Repeat rows 1–3 under the lavapipe lane.** No LSan report.
24. **Start a translate drag, minimize the window mid-drag, release the button while minimized,
    restore, and select a DIFFERENT entity.** The new entity does **not** jump. Repeat with the
    Viewport panel *hidden* mid-drag, and with the dragged entity *destroyed* mid-drag.
25. **Move an entity so its gizmo overlaps the mode bar in the Viewport's top-left, then click a bar
    button.** The mode changes; the entity does **not** move.

## Validation records

- **Launch, click the seeded Cube: selection highlight and a translate gizmo appear**
- **Drag the red/green/blue arrow: moves along that axis only; rotation/scale untouched**
- **Press `E`, drag a rotation ring: rotates about that axis only**
- **Press `R`: scale handles appear; space button reads Local and is disabled with a tooltip**
- **Press `W` then `X` repeatedly: handles alternate world/local alignment**
- **Hover every handle ~10s without clicking, then drag: still grabbable (F4 latch)**
- **Click directly on a handle: gizmo grabs, selection unchanged**
- **Click empty space then another entity: picking works, gizmo follows new primary**
- **Hold Ctrl/Cmd while dragging: snaps to 0.5u/15°/0.1; release mid-drag resumes continuous, no jump**
- **Parent the Cube, move the parent, drag the child: parent-relative position shown**
- **Non-uniform parent scale + rotated child dragged in world space: nothing moves, exactly one WARN**
- **Fly the camera until the cube is behind you: no gizmo, no visual corruption**
- **Start a translate drag then hold RMB to fly: drag ends cleanly, flying works**
- **Retina: drag a handle across the viewport, no 2x drift**
- **Focus the Hierarchy rename field, type wersx: no gizmo mode change**
- **Hover other panels while the gizmo is on screen: no handle highlights, no drag starts**
- **Select an entity with no Transform: no gizmo, overlay bar greyed with a tooltip**
- **Resize/dock/tab the Viewport: gizmo stays correctly positioned and clipped**
- **View menu has no "gizmo" entry; aero_editor.ini has no gizmo window after quit/reopen**
- **Select the Directional Light and drag translate: it moves**
- **Grab a handle, release without moving: nothing changes, no Inspector flicker**
- **Undo: still a disabled stub (expected, closes at 2.4.2)**
- **Linux only: rows 1-3 under lavapipe, no LSan report**
- **Minimize/hide/destroy mid-drag then select a different entity: it does not jump**
- **Gizmo overlapping the mode bar, click a bar button: mode changes, entity does not move**

### macOS — ✅ PASS (2026-07-30)

Machine: MacBook Pro (Apple M1 Pro), Metal. Human mouse/keyboard pass against `db0f782` — the merged
tree, including the code-review round's shear guard and widened behind-camera predicate.

**All twenty-four macOS-applicable rows pass.** Row 23 is Linux-only and is not runnable here.

Rows 1–22, 24 and 25 as listed above — every one **PASS**. The rows worth calling out, because no
mechanical test in this harness can reach them:

- **6 — hover every handle ~10 s without clicking, then drag: still grabbable** — PASS. This is F4's
  `mbOverGizmoHotspot` latch, the one-way failure that would have made the gizmo permanently inert
  from the second frame onward had `ImGuizmo::BeginFrame()` been missed. Sabotage S1 reddens nothing,
  so this row is the *only* proof the call is present and correctly placed.
- **7 / 8 — a press on a handle grabs without re-picking; hovering another panel starts nothing** —
  PASS. The gizmo-vs-picking arbitration (D10) and ImGuizmo's own `_OwnerName == "Viewport"` hover
  gating (F6). S2 and S10 both redden nothing; this row is their only cover.
- **11 — non-uniform parent scale + rotated child dragged in world space: nothing moves, exactly one
  WARN** — PASS. This is AC-10, the criterion that was silently *false* until the code-review round
  added the editor-side shear guard. Confirms the fix end to end, and that the WARN latches once per
  drag instead of flooding the Console at frame rate (D12).
- **12 — fly the camera until the cube is behind you: no gizmo, no visual corruption** — PASS,
  including through the ~0.0002–0.11 world-unit band where our predicate and ImGuizmo's disagreed
  before the widening fix.
- **14 — Retina: drag a handle across the viewport, no 2× drift** — PASS. D18's points-vs-pixels
  split; S7 cannot redden on a non-Retina runner, so this row is its only proof.
- **24 — minimize/hide/destroy mid-drag, then select a different entity: it does not jump** — PASS.
  The §A4 stale-latch clear, which ships as documented defence in depth with no automated coverage.

### Windows — ⏳ pending

Needs a native run. No checks recorded yet.

### Linux — ⏳ pending

Needs a native run (real Vulkan or native Wayland/X11; **not** lavapipe/CI, which cannot exercise
window-manager or compositor interaction). No checks recorded yet.

**Task 2.3.3 gate status: mechanically green, including a code-review round that closed one BLOCKING
finding (G11/shear detection) and one SHOULD-FIX gap (the behind-camera predicate band) before
merge** (build + full ctest on both presets, the `AERO_REQUIRE_GPU=1` rehearsal, both tools-OFF
configurations — including the new reflect-OFF/shaders-ON gate AC-17 adds — the non-interactive launch
proof, all thirteen sabotage proofs each seed-confirmed and reverted: S4 second-order-checked; S13
(added in the code-review round) confirms `isSheared()` is load-bearing for G11; S5 re-verified to
reveal an honest post-fix subtlety — literal S5 no longer discriminates G11 since `isSheared()`
intercepts first, but it **does** still redden **G14** (re-measured against the full suite, not G11
alone), so `decompose()`'s own failure branch remains load-bearing and tested; five recorded as honest
non-discriminations (S1, S2, S7, S9, S10); S3 and S11 each corrected a plan prediction with a verified
finding, both recorded above, and S3 was re-checked against the widened predicate with the same
verdict; S6 also a documented non-discrimination, tied to the (now structurally explained) G14
finding — all five guards green, clang-format and clang-tidy clean with zero new `NOLINT`s). **The
human pass — macOS, Windows and Linux — is pending** and closes AC-1/AC-6/AC-7/AC-8's human-only
surface plus confirms the shear fix (row 11) and the widened behind-camera predicate (row 12) hold on
real hardware.

# Task 2.4.1 — command stack

**Deliverable:** a `Command`/`CommandStack` backbone with an explicit merge chain and a 128-entry bounded
history; `Ctrl+Z`/`Ctrl+Shift+Z` with key repeat, routed `RouteGlobal`, and live Edit-menu items
(`"Undo <label>"`/`"Redo <label>"`, enabled by `canUndo()`/`canRedo()`); and `TransformCommand`, wrapping
2.3.3's `transform_ops` seam exactly as that seam's own header said it would be wrapped, with the
Viewport gizmo routed through it so **one continuous drag is exactly one undo step whose `before` is
where the drag began**. This closes Epic 2.3's Definition of Done's *undoably* clause one task earlier
than the docs previously said (D1) — drag the cube, press `⌘Z`, watch it go back, in one step, not one
frame's worth.

**What CI proves automatically:** two tier-0 batteries riding `aero_editor_shell_test` —
`command_stack_test.cpp` (21 cases, C1–C18 plus three code-review-round additions, C19–C21: the push
contract calling `redo()` exactly once before any
history mutation, a recording push truncating the redo branch first, the merge chain collapsing a
continuous run into one entry and each of its five breakers — undo/redo/clear/setClean/
`breakMergeChain()` — capacity eviction from the front and the ≥ 1 clamp, clean tracking through a
capacity shift and permanently past an eviction, the failed-push and failed-undo paths each producing
exactly one WARN while the redo branch stays untouched, the null push, every label query, complete
destruction with no leak, the drag call sequence end to end, the mirror of C11 on the `redo()` side
(C19), a moved-from stack staying empty/clean/`!canUndo()` (C20), and the release-frame merge-ordering
policy case that documents Gap 1's fix (C21)) and `transform_command_test.cpp` (9
cases, T1–T9: exact apply/revert on all three channels at once, the silent dead-target guard with zero
ERRORs plus an anti-vacuity canary, the three merge arms — same entity, different entity, a
cross-type command via `dynamic_cast` — the label, and a 50-frame simulated drag that undoes to the
exact drag-start `Transform`) — plus six GPU-gated cases in `imgui_layer_test.cpp` (I1–I6) driving the
real `requestUndo()`/`requestRedo()` → `ShellUiState` → `drawMenuBar` → `applyHistoryRequests` →
`CommandStack` → `TransformCommand` → `writeTransform` path through a real `EditorApp::tick()`: the
whole history path executing and staying ImGui-balanced (an unbalanced ImGui call is an `IM_ASSERT`
abort in Debug, so a green Debug run through real frames *is* the balance proof), undo/redo through the
shell genuinely mutating the World, a request consumed exactly once even across three subsequent ticks,
undo-then-redo requested in the same tick netting to a no-op, an empty history staying silent (zero new
log records) across ten requested-undo ticks, and undoing past a destroyed target producing exactly one
WARN and no crash.

**What it cannot prove:** anything requiring a synthesised ImGui key press.
`tests/CMakeLists.txt:96-106` — the GPU-gated editor target links `aero::editor_core` with `imgui::imgui`
`PRIVATE`, so it is ImGui-free at source and cannot name `ImGuiKey`; `requestUndo()`/`requestRedo()`
exercise the whole path *below* the physical chord, but the chord itself is untestable here. Named
precisely, the uncovered surface is: the two `ImGui::Shortcut` call sites and the exact flags they pass
(`ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_Repeat`); `RouteGlobal`'s documented last-place loss to a
focused `InputText`'s own `RouteFocused` binding (F5 — the single most likely place a naive
implementation goes wrong); `ImGuiInputFlags_Repeat`'s hold-to-repeat and its stop-on-modifier-release
(`RepeatUntilKeyModsChange`); the Edit menu's live enabled state and its `"Undo <label>"` text, as
actually rendered; and the fact that the panels of the *same* frame render post-undo state (D19 — I2
proves the World changed through the shell path, not that a panel's own widgets reflect it that frame).
Every one of these is named as a specific human row below.

## What was mechanically verified (this implementation pass, macOS, Apple M1 Pro / Metal)

Measured at every one of the six code-bearing commit boundaries, not once at the end.

- `ctest --preset macos-debug` and `--preset macos-release`: **94/94** both presets, at every commit;
  `AERO_REQUIRE_GPU=1 ctest` green on both presets (the CI ratchet rehearsed, not skipped).
- `ctest --preset macos-debug -N` → **Total Tests: 94** throughout — **no new `add_test`**, exactly as
  AC-25 requires; both new test TUs ride the existing `aero_editor_shell_test`/`aero_editor_imgui_test`
  targets.
- Doctest case counts, measured with `--list-test-cases`, never predicted: `aero_editor_shell_test`
  **169 → 187** (step 1, `command_stack_test.cpp` C1–C18) **→ 196** (step 2, `transform_command_test.cpp`
  T1–T9), unchanged through steps 3–6 — both figures matched the plan's own predictions exactly;
  **196 → 199** in the code-review round below (C19, a moved-from-stack case, and C21 — C7's arm fix and
  T3's rewrite both add `SUBCASE`s, not `TEST_CASE`s, so neither moves this count);
  `aero_editor_imgui_test` **24 → 30** (I1–I6), unchanged by the review round; `aero_tests` unchanged at
  **356**; `aero_editor_core`
  **25 → 27** sources (`command_stack.cpp`, `transform_command.cpp`).
- Fresh `-DAERO_REFLECT_TOOLS=OFF -DAERO_SHADER_TOOLS=OFF` configure into `build/tools-off-2.4.1`:
  **5/5**, `aero_editor` launches with **exactly two** WARN lines (2.2.2's reflection WARN, 2.2.3's
  shader WARN) and **no third**.
- `-DAERO_REFLECT_TOOLS=OFF` alone (shaders ON) into `build/reflect-off-2.4.1`: **18/18**, measured not
  assumed; the launch log shows `editor: shell ready (...)`, the 2.2.2 reflection WARN, and no third —
  `TransformCommand` goes through `transform_ops`, never `component_ops`, so it is tools-independent by
  construction (the same D21 precedent 2.3.3 established); the actual drag-and-undo confirmation under
  this configuration is human row 16's business.
- `check-math-boundary.sh`'s scanned count moved **209 → 215** (+6: `command_stack.hpp/.cpp`,
  `transform_command.hpp/.cpp`, `command_stack_test.cpp`, `transform_command_test.cpp` — measured
  against `origin/main` in a disposable `git worktree` at the start, and reconfirmed directly on the
  finished tree at the end; both readings agree); the other four guards (`check-golden-rule.sh`,
  `check-platform-boundary.sh`, `check-rhi-boundary.sh`, `check-scene-boundary.sh`) all green with **no
  allowlist change** — this task adds no SDL/EnTT identifier and changes no `engine/`/`runtime/` file.
- `git diff --stat origin/main` empty over `engine/`, `runtime/`, `samples/`, `tools/`, `shaders/`,
  `cmake/`, `.github/`; `vcpkg.json`'s `builtin-baseline` and the `/vcpkg` submodule SHA byte-identical;
  `.clang-format`/`.clang-tidy` byte-identical; `editor/include/aero/editor/imgui_layer.hpp`,
  `editor/src/imgui_layer.cpp` and `editor/src/main.cpp` byte-identical for the **eleventh** task
  running (`editor_app.cpp`'s own two-task streak ends here, deliberately, wiring the command stack
  through `tick()`); `ViewportPanel::renderScene` byte-identical (no hunk at or after its signature);
  every `target_link_libraries` line on every target byte-identical.
- clang-format and clang-tidy clean on every touched file, with **zero new `NOLINT`s**.
- The non-interactive launch check (`aero_editor`, seeded scene): zero unexpected ERROR/CRITICAL/WARN
  lines at every commit boundary.

### The sabotage table's outcome

All fourteen original seeds, plus a fifteenth added by the code-review round (the `redo()` half of
AC-5/D20, Gap 3) and re-runs of S2, S6, S7, S10 and S13 against the fixed tree, every one confirmed
present via `git diff` before trusting a verdict, every verdict measured against the whole suite, every
one reverted and re-confirmed green afterward. Rows marked **corrected** below replace a prior version of
this table whose S2/S7/S10/S13 rows were wrong — see `docs/10-engineering-log.md`'s 2.4.1 code-review-round
entry for the full narrative:

| | Seed | Result |
|---|---|---|
| S1 | `push`'s step 3 (redo-branch truncation) deleted | **reddens `aero_editor_shell_test`**, exactly as predicted (C4). |
| S2 | `undo`'s `mergeOpen = false;` deleted | **Code-review round correction: the earlier row here was false.** The single-entry construction the plan literally describes cannot discriminate S2 at all (undoing a stack's sole entry drops `applied` to 0, which masks `mergeOpen` behind the merge guard's own `applied > 0` term regardless of the seed) — and the fix this row previously claimed ("rebuilt as a two-entry construction") was never actually written into `command_stack_test.cpp`: the implementation pass shipped the original single-entry arm unchanged, and S2 had ZERO coverage on the seven-commit branch. A later code review caught this by reading the tree directly. **Now actually rebuilt** as a two-entry construction (push A with `mergeResult = true`, `breakMergeChain()`, push B, `undo()`, push C) and **confirmed reddening** (`logA.mergeCalls == 1`, `count() == 1` with the seed live, against `logA.mergeCalls == 0`/`count() == 2` expected). |
| S3 | `push`'s step 2 (the `redo()` guard) moved after step 3 (truncate) | **reddens**, exactly as predicted (C10). |
| S4 | `trimToCapacity()` deleted from `push` | **reddens**, exactly as predicted (C8). |
| S5 | `push` replaced with an unconditional `label()` call, `redo()` never invoked | **reddens** (C2, T8, and — beyond the plan's own prediction — C3, C5 and C10 independently too). Second-order checked: weakening only the two assertions the plan names (C2's `redoCalls`, T8's three `readTransform` equalities) to `CHECK(true)` does **not** make the seed pass the whole suite silently, because those three other cases catch it on their own — a stronger, more redundant result than predicted, recorded honestly rather than forced to match. |
| S6 | `undo`'s `--applied;` guarded behind `if (ok)` | **reddens**, exactly as predicted (C11). |
| S7 | `setClean`'s `mergeOpen = false;` deleted | **Code-review round correction: only half of the predicted attribution is real.** **Reddens via C7's `setClean` SUBCASE** — confirmed. **Does NOT reden C13** ("clean tracking"): C13 calls its own explicit `breakMergeChain()` immediately before the push that would expose a stale `mergeOpen`, so it passes identically whether or not `setClean()` resets the chain, seed live or not — confirmed by running C13 alone with the seed live and watching it pass. C7's `setClean` arm is S7's only discriminator. |
| S8 | `trimToCapacity`'s `nullopt` branch replaced with `cleanPosition = 0` | **reddens**, exactly as predicted (C14 arm 2). |
| S9 | `TransformCommand::mergeWith` also overwrites `beforeValue` | **reddens** (T4, T8), exactly as predicted. Second-order checked: weakening T4's `a.before() == p0` and T8's post-undo `readTransform` equality to `CHECK(true)` DOES make the whole suite pass silently with the seed live — confirming those two assertions, not the harness, do the work, exactly matching the plan's own prediction. |
| S10 | `TransformCommand::write`'s `readTransform` guard deleted | **Code-review round correction: T3 did not actually discriminate this, pre-fix.** T2 and the I-series discriminate as originally stated. T3 ("no Transform component"), as originally shipped, had no `LogFixture`/`LogSinkScope`/ERROR assertion at all — its two checks (`CHECK_FALSE(cmd.redo(w))`, `CHECK_FALSE(w.has<Transform>(e))`) both stay true with the guard removed, because `writeTransform` also returns `false` for a missing component (it just also emits the `AERO_LOG_ERROR` the guard exists to avoid, which the original T3 never checked for). Verified directly: the original T3 body was run against a live S10 seed and passed unchanged. **T3 rewritten** — wrapped in `LogFixture`/`LogSinkScope` with a `countAtLevel(records, Error) == 0` assertion, plus a new null-`Entity{}` SUBCASE covering the other half of AC-13 that had no coverage anywhere. **Reddens both `aero_editor_shell_test` and `aero_editor_imgui_test`**, now via T2, the rewritten T3 (both SUBCASEs), and I-series execution. |
| S11 | `mergeWith`'s `dynamic_cast` replaced with `static_cast` | **reddens via an ASan stack-out-of-bounds abort**, not merely a red assertion — a stronger result than the plan's own "expect a compiler diagnostic or a red test": reading an `OtherCommand` through a `TransformCommand*`'s wider layout is UB the sanitizer catches directly (T6). |
| S12 | the capacity clamp dropped from `CommandStack`'s constructor | **reddens**, exactly as predicted (C9). |
| S13 | `updateGizmo`'s `End` chain-close dropped entirely | **Code-review round correction: the original verdict's REASON was wrong, even though the "non-discriminating" label was defensible at the time for a different bug.** As originally shipped (the `End` break ran BEFORE that frame's write-back — Gap 1's blocking defect), dropping the `End` arm entirely made the code MORE correct, not less: it removed the premature close that was splitting a release frame's final delta into a second entry, so a suite run with S13 seeded against the ORIGINAL code would have looked identical to correct behaviour, for the wrong reason. **Re-run against the FIXED code** (the `End` close now correctly runs AFTER the write-back): still confirmed non-discriminating, `aero_editor_shell_test`/`aero_editor_imgui_test` both 94/94 whole-suite with the seed live — but now for a genuinely structural reason: `Begin`'s own break is unconditional and independently guarantees one entry per drag-start, so for the ONLY command producer this task wires up (the gizmo), no sequence of ordinary drags can expose a missing `End` close. **Human row 5 is NOT S13's discriminator** (corrected below) — three separate drags produce three entries regardless of whether `End` closes the chain, because each new drag's own `Begin` already forces a fresh entry. `End`'s close remains in the code as INV-3's stated defence-in-depth for the day a second, non-gizmo command producer exists (2.4.2+). |
| S14 | `HISTORY_SHORTCUT_FLAGS` changed from `RouteGlobal` to `RouteAlways` | **confirmed NON-discriminating**, exactly as predicted — `aero_editor_imgui_test` is ImGui-free at source and cannot press a key; **human row 9** is the only real check. |

## Known-and-expected, NOT a defect

- **E14** — an Inspector edit is discarded by a later undo, until 2.4.2 routes the Inspector through a
  command too (D1's honest one-task cost).
- **E5** — `⌘Z` mid-drag: the drag continues from the reverted pose. No crash, no log flood.
- **E13** — undo does not restore the selection (D14, 2.4.2's).
- **E2** — past the 128-entry capacity floor the oldest drag is unrecoverable, silently and by design —
  a WARN there would fire once per session for a user doing nothing wrong.
- **E3** — undoing past a deleted entity moves nothing and logs exactly one WARN, never an ERROR.
- Hierarchy and Inspector edits are still **not** undoable — only the gizmo routes through a command
  after this task.

## How to validate one OS

1. **Launch.** The **Edit** menu shows "Undo" and "Redo", both **greyed**, labelled `Ctrl+Z` /
   `Ctrl+Shift+Z` (⌘Z / ⇧⌘Z on macOS). **No tooltip claiming an owning task** — the stubs are gone.
2. **Drag the seeded Cube's translate gizmo once and release.** Edit > Undo now reads **"Undo
   Transform"** and is **enabled**; Redo is still greyed. The Inspector's `position` shows the new
   value.
3. **Press `Ctrl/⌘+Z`.** The cube returns to where it was **before the whole drag**, in **one** step —
   not one frame's worth, not to an intermediate position. The Inspector updates in the **same** frame
   (D19/AC-21: no visible one-frame lag).
4. **Press `Ctrl/⌘+Shift+Z`.** It returns to the dragged position, in one step. Edit > Undo reads
   "Undo Transform" again.
5. **Three separate drags, then three undos.** The cube walks back through all three, newest first; a
   fourth undo does nothing and the item greys. *(INV-3 and AC-17. Code-review round correction: this
   row is NOT S13's discriminator — each new drag's own `Begin` edge unconditionally closes the merge
   chain before that drag's first push, independent of whether the `End` boundary ever closes it, so
   three separate drags produce three entries whether or not S13 is seeded. This row DOES catch Gap 1's
   real defect, though: with the pre-review-round `End`-closes-before-write-back bug, a release frame's
   final delta recorded as a second entry, so three drags would have produced SIX undo steps, not three
   — this row would have failed had the human pass run before the review round fixed it.)*
6. **Hold `Ctrl/⌘+Z`.** The history rewinds repeatedly (key repeat, D12), and **releasing the modifier
   stops it immediately** rather than leaving a stuck auto-repeat on `Z`.
7. **Rotate (`E`) and Scale (`R`) drags undo and redo the same way**, and undoing a rotate leaves
   `position` and `scale` **untouched** — 2.3.3's channel isolation survives the command layer.
8. **Undo once, then perform a NEW drag.** Redo **greys out** — the redo branch was truncated (AC-3).
9. **⚠ The routing trap.** Press `F2` on a Hierarchy row to rename, type something, then press
   `Ctrl/⌘+Z` **while the field still has focus**: the **text** undoes and **the scene does not move**.
   Then press `Esc`/`Enter` to leave the field and press it again: now the *scene* undoes.
   *(F5. This is S14's only discriminator and the single most likely place a naive implementation —
   `ctx.input()` instead of `ImGui::Shortcut`, or `RouteAlways` instead of `RouteGlobal` — goes wrong.)*
10. **`Ctrl/⌘+Z` with an empty history.** Nothing happens; **no Console spam even when held** (E1: the
    `applied == 0` guard precedes the log).
11. **Drag the Cube, delete it from the Hierarchy, then `Ctrl/⌘+Z`.** Nothing moves and the Console
    shows **exactly one WARN** — not an ERROR, and not two records (E3/D16).
12. **Known-and-expected.** Press `Ctrl/⌘+Z` *while holding* a gizmo handle mid-drag: the drag
    continues from the reverted pose. **No crash, no log flood** (E5).
13. **Known-and-expected.** Drag the Cube, then type a new position into the Inspector, then
    `Ctrl/⌘+Z`: the Inspector's value is **discarded** and the pre-drag transform is restored — D1's
    honest one-task cost, closed at 2.4.2 (E14).
14. **In a Debug build, the Console shows one `DEBUG editor: undo 'Transform' (…)` line per undo** —
    the triggerable log source 2.2.5's four BLOCKED rows have been waiting for (D17), alongside 2.3.3's
    degenerate-transform WARN. In a **Release** build the same undo is silent (`NDEBUG` compiles the
    record out) — that is correct, not a defect.
15. **Perform 130+ drags, then hold undo to the bottom.** It stops cleanly at the capacity floor, the
    Edit item greys, nothing crashes, and there is **no WARN** about the lost oldest entry (E2).
16. **Quit and relaunch.** `aero_editor.ini` is unchanged in shape, the layout returns, and the history
    is **empty** on the new run — no history is persisted, by design.

### macOS — ⏳ pending

Needs a native human pass. No checks recorded yet.

### Windows — ⏳ pending

Needs a native run. No checks recorded yet.

### Linux — ⏳ pending

Needs a native run. No checks recorded yet.

**Task 2.4.1 gate status: mechanically green** (build + full ctest on both presets, the
`AERO_REQUIRE_GPU=1` rehearsal, both tools-OFF configurations, the non-interactive launch proof, all
sabotage proofs each seed-confirmed and reverted — including a code-review round that found and fixed a
blocking AC-16 ordering defect (Gap 1: a release frame recorded as two history entries, not one) and
corrected the S2, S7, S10 and S13 rows above, none of which were what the original pass claimed; S5's
second-order check found MORE redundant coverage than predicted; S9's second-order check matched the
prediction exactly; S11 discriminates via an ASan abort; S14 confirmed non-discriminating exactly as
predicted and routed to human row 9; S13 confirmed non-discriminating against the FIXED code for a
structural reason (`Begin`'s own break is sufficient for this task's only command producer), not the
"closer to correct than the buggy code it was seeded against" reason it would have shown pre-fix — all
five guards green with no allowlist change, clang-format and clang-tidy clean with zero new `NOLINT`s).
**The human pass — macOS, Windows and Linux — is pending** and is the only proof of the physical
`Ctrl+Z`/`Ctrl+Shift+Z` chords, `RouteGlobal`'s loss to a focused `InputText` (row 9), and — now that Gap
1 is fixed — that the merge chain genuinely holds across three consecutive drags (row 5, which is a real
end-to-end check of AC-16/AC-17, not S13's discriminator).

