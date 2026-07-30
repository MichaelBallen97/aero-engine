---
paths:
  - "editor/**"
  - "tests/editor/**"
---

# Editor conventions

The editor depends on the engine; **the engine never depends on the editor** (project
rule #1), enforced by `check-golden-rule.sh` plus `aero_assert_golden_rule()` in
`cmake/golden_rule.cmake` — an include scan *and* a link-graph walk, because a forward
declaration or a `target_link_libraries` entry is invisible to any include scan.

## Public editor headers stay ImGui-free and entt-free

Held by **file placement**, not by a guard: every ImGui and EnTT entry point lives in
`editor/src/`. A probe cannot enforce this (R12 — `aero_editor_shell_test` links
`doctest`, which puts vcpkg's shared include root on the compile line, so a leaked
`#include <imgui.h>` in a public header still compiles). Keep new ImGui/entt code in
`src/`; do not claim guard coverage that does not exist.

`PanelOptions` and friends use **named booleans**, never an `ImGuiWindowFlags`.

## ImGui call balance — asymmetric by API, and an abort if wrong

An unbalanced call is an `IM_ASSERT` **abort** in the Debug build, not a visual glitch.

- `Begin`/`End`, `TreeNodeEx`/`TreePop`, `PushID`/`PopID` are **1:1** — call `End()`
  regardless of what `Begin()` returned.
- `BeginMainMenuBar`/`BeginMenu` are the **opposite**: call `End*` only when `Begin*`
  returned true.
- `CollapsingHeader` needs no `TreePop` (`NoTreePushOnOpen`).

**The registry calls `Begin`/`End`, never the panel.** A hidden panel `continue`s before
`Begin`. This makes the unbalanced-`End` class of bug structurally impossible — keep it
that way.

## Panels

- `PanelRegistry` owns `std::unique_ptr<Panel>`, so a returned `Panel*` is
  **address-stable** across later `add()` calls. Registration order is draw order is
  View-menu order.
- A duplicate `id()` is **rejected** — ImGui silently *merges* two windows sharing a name.
- **Never mutate the World or the Selection during a draw walk.** Record one pending
  action and apply it after the walk. Required by both `eachChild`'s contract and
  ImGui's tree balance.
- **No recursive functions** — `misc-no-recursion` is `--warnings-as-errors` in CI.
- The default layout only splits a dock slot at least one registered panel asks for: an
  empty dock node is not pruned and draws a dead grey rectangle. Node size comes from
  `GetMainViewport()->WorkSize`, not `->Size` (the menu bar shrinks the work area); the
  one-frame settle is expected — do not "fix" it with a manual `GetFrameHeight()` offset.
- Drag-drop payloads: `std::memcpy` into a local, **never a cast** — ImGui's buffer is
  `alignas(1)` and the Debug lanes run UBSan. Decide drop legality by *peeking* with
  `GetDragDropPayload()`, so an illegal drop never draws a highlight.
- Keyboard chords go through `ImGui::Shortcut(..., ImGuiInputFlags_RouteGlobal)`, never
  `ctx.input()` — the latter has no notion of UI focus, so a focused `InputText` would
  swallow the chord. `ImGuiMod_Ctrl` maps to Cmd on macOS automatically.
- `Escape` does **not** quit. Esc is the universal *dismiss* key in an editor.
- Unimplemented menu items ship `enabled = false` with a tooltip naming the owning task —
  never a dead handler behind a stub.
- `<imgui_stdlib.h>` is included **flat**, not under `misc/cpp/` (vcpkg installs it flat).

## Undo/redo

- **`CommandStack::push()` APPLIES the command** (task 2.4.1 D5). A caller must NOT have already
  written the edit — there is exactly one write path and it runs inside `Command::redo()`. Wrapping an
  existing direct write means the write **moves into** the command, not that a command is pushed
  beside it.
- **The merge chain** is open only between one continuous gesture's start and end. It is broken by
  `undo()`, `redo()`, `clear()`, `setClean()` and the explicit `breakMergeChain()`, and opened by any
  push that records a new entry. A panel driving a continuous edit breaks the chain at **both**
  boundaries and at **every** path that abandons the gesture — `ViewportPanel::updateGizmo`'s two
  early returns are the precedent, and they deliver no end edge at all.
- **The gate pair is asymmetric by which side of the push it sits on, and the two edges cannot share a
  call site (task 2.4.2 D17).** An OPEN edge (`IsItemActivated`, `GizmoDragEdge::Begin`) breaks the
  chain **before** that frame's push; a CLOSE edge (`IsItemDeactivated`, `GizmoDragEdge::End`) breaks it
  **after**. Both ImGuizmo and ImGui report a gesture's final delta on the *same* frame they report the
  gesture's end, so closing the chain first records that frame as a second, un-merged history entry —
  this is 2.4.1's blocking Gap 1, and 2.4.2 had to get the same decision right independently in seven
  Inspector arms. No test tier can reach a panel's own application of this rule (panels are src-private
  and ImGui-bound) — it is proven only by a human pass, so get it right by construction, not by review.
- **Undo/redo are applied inside `drawShellUi`, immediately after `drawMenuBar()` returns** and before
  the dockspace (D19): `EndMainMenuBar` has run, no ImGui tree is open, no `eachChild` walk is in
  flight, and the panels of the SAME frame therefore render post-undo state. Do not move it into
  `tick()` — that would render pre-undo panels against a post-undo `renderScene` in one frame.
- Commands hold **values and handles only** — never a `World&`, a `Selection&`, or a pointer into a
  panel. Everything needed at apply time arrives as an argument, which is also why `CommandStack`
  never stores a `World&` (it would delete `EditorApp`'s defaulted move assignment).
- Success logs at `AERO_LOG_DEBUG` (compiled out under `NDEBUG`) and failure at `AERO_LOG_WARN`, from
  the stack and **only** the stack — a command returns `false` silently. A successful `push` logs
  nothing at all: a drag pushes once per frame.

## Inspector / reflection

The inspector is driven entirely by generated `entt::meta`: a new `[[engine::component]]`
type must get a working UI with **zero editor changes**. Do not special-case component
names — walk the registration table. `editor::component_ops` is the seam
`SetFieldCommand`/`AddComponentCommand`/`RemoveComponentCommand` wrap (task 2.4.2); narrow and
clamp in C++ before writing the exact concrete type, never letting EnTT convert (it silently
wraps `300` into a `uint8_t` as `44`).

**No panel writes the scene directly (true since task 2.4.2).** The only call sites of
`entity_ops` / `component_ops` / `transform_ops` mutators under `editor/src/` are their own
TUs and their command TUs (`entity_commands.cpp`, `component_commands.cpp`,
`transform_command.cpp`). A new panel action that mutates the World is a new `Command`
pushed onto the `CommandStack`, never a direct call — that is what keeps every edit
undoable and is the AC-24 grep this task's gate depends on
(`git grep -nE '(writeComponentField|addComponent|removeComponent)\(' -- editor/src/`,
scoped per panel, must stay empty).

Validation is a two-part gate: mechanical/structural (build, ctest, sabotage proofs) and
a **human mouse/keyboard pass** recorded per OS in `editor/VALIDATION.md`.

Full history: `docs/10-engineering-log.md`, Epic 2.1 / 2.2 entries.
