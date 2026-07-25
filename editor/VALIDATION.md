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
3. **Look at the window**: "Aero Editor" titled, a full-window dockspace with 4 panels —
   Hierarchy (left), Inspector (right), Console (bottom), Viewport (center) — each a one-line
   placeholder ("... — placeholder (task 2.2.x)").
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
| macOS | ⏳ mechanically verified only | 2026-07-25 | MacBook Pro (Apple M1 Pro), Metal backend | PASS (non-interactive launch; window opened, logged Metal device) | pending human visual confirmation | pending human visual confirmation (mechanical ini round-trip PASS; no mouse drag performed) | PASS (DockBuilder split observed in the saved ini: Hierarchy/Inspector/Console/Viewport) | pending human visual confirmation | PASS (SIGTERM-equivalent kill; no crash; ini written cleanly) | GPU smoke test (`aero_editor_imgui_test`) green under `AERO_REQUIRE_GPU=1`. |
| Windows | ⏳ pending | — | — | — | — | — | — | — | — | needs native run (D3D12) |
| Linux | ⏳ pending | — | — | — | — | — | — | — | — | needs native run (real Vulkan; NOT lavapipe/CI) |

**Task 2.1.1 gate status: OPEN** — closes only when all three rows above are ✅ across every
column, including the mouse-driven visual checks a human must confirm (mirrors the Phase 0/1
gates' code-free Windows/Linux follow-up precedent, 0.5.3/1.4.2).

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
  `editor: shell ready (5 panels, layout: default)` appears, the ini is written, and it contains a
  real `[Docking][Data]` tree with entries for all five panel names
  (`Hierarchy`/`Inspector`/`Viewport`/`Console`/`Assets`) — proving the data-driven DockBuilder
  layout actually built and ImGui saved it. A second launch logs `layout: restored` and leaves the
  ini byte-identical.
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
| macOS | ⏳ mechanically verified only | 2026-07-2X | MacBook Pro (Apple M1 Pro), Metal | pending human | pending human | pending human | PASS (observed in the saved ini) | pending human | pending human | PASS (kill; no crash) / pending human / pending human | PASS (mechanical ini round-trip) | pending human | pending human | Both editor ctest targets green; `editor: shell ready (5 panels, layout: default)` observed. |
| Windows | ⏳ pending | — | — | — | — | — | — | — | — | — | — | — | — | needs native run (D3D12) |
| Linux | ⏳ pending | — | — | — | — | — | — | — | — | — | — | — | — | needs native run (real Vulkan; NOT lavapipe/CI) |

**Task 2.1.3 gate status: OPEN** — closes only when all three rows are ✅ across every column.
**Epic 2.1 closes with it** (2.1.1's table above must be complete too), mirroring the Phase 0/1
gates' code-free on-hardware follow-up precedent (0.5.3, 1.4.2).
