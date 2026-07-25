# Editor ImGui-docking gate ledger — `aero_editor` (task 2.1.1)

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
5. **Rearrange, then quit** (Esc or the window close box) **and relaunch** — confirm the exact
   arrangement you left it in comes back.
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
