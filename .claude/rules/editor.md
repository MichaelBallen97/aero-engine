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
- **`buildDefaultLayout` runs ONLY when there is no `imgui.ini` to restore, so it is not
  what places a panel you add today.** It is the only reader of `defaultDockSlot()`, and
  on every machine that has already run the editor the layout is *restored* instead — a
  panel added after that file was written has no settings entry, and ImGui free-floats it
  at a default position. Shipping the Project Settings panel did exactly that to every
  existing install. `placeUnplacedPanels` (`shell_ui.cpp`) closes it: on the first drawn
  frame of a restored layout it docks **only** the panels the ini has never heard of, into
  the node already hosting a panel that declares the *same* `defaultDockSlot()`, falling
  back to the dockspace root. A panel the ini knows is never touched, wherever the user put
  it. **Adding a panel therefore needs no layout migration of its own** — but a test that
  drives this path must set `EditorAppConfig::layoutIniPath`, or it rewrites the developer's
  real editor layout (the `recentProjectsPath` lesson, second instance).
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

## Scene I/O and the File menu (task 2.5.1)

- **`World::clear()`, `Selection::clear()`, `RootOrder::clear()` and `CommandStack::clear()` must
  always be called together, in the same operation, never independently.** `resetSceneState`
  (`scene_session.cpp`) is the **only** call site that clears the World. `World::clear()` bumps
  every existing entity's generation but never un-issues its index (measured,
  `scene_test.cpp` W7) — a `CommandStack` left holding history against a *replaced* World would
  `recreate()` handles that mean nothing there, not merely display a stale undo label. This is
  INV-6, and since task 2.4.2 put `SubtreeSnapshot`s inside history entries it is a
  data-corruption invariant, not a cosmetic one. Never add a second path that swaps the World
  (a future New/Open flow, a project-load flow) without also clearing the stack in that same
  call.
- **Every native scene I/O call site checks `sceneIoAvailable()` first.** The engine's
  serialization bridge lives behind exactly one build gate
  (`AERO_EDITOR_REFLECTION`, confined to `scene_io.cpp`), and building the editor with
  `AERO_REFLECT_TOOLS=OFF` makes every save/open reachably no-op rather than a link error —
  `sceneIoAvailable()` reports `false` in that configuration and every save/open path checks it
  before touching a file. A test or panel action that calls into scene I/O without this guard
  will fail specifically in the tools-OFF configurations, not the default build — always verify
  both.
- **AC-27 (Esc dismisses the unsaved-changes modal as Cancel) is hand-bound, not built-in, and
  this is deliberate, not a workaround to simplify.** ImGui 1.92.8's own nav-cancel path cannot
  close a *modal* popup: `NavUpdateCancelRequest`'s popup branch excludes
  `ImGuiWindowFlags_Modal` (`imgui.cpp:15007`/`15032`), and the editor never sets
  `ImGuiConfigFlags_NavEnableKeyboard` (`imgui_layer.cpp:79`). The modal body checks
  `ImGui::IsKeyPressed(ImGuiKey_Escape, false)` directly. Keep the comment explaining this is not
  redundant with ImGui's own handling — a future ImGui upgrade is the only thing that could make
  it so, and removing the hand-bound check on that assumption without re-verifying against the
  vendored source would silently break AC-27 with no test able to catch it (no tier can press a
  key on a modal; see `editor/validation/2.5.1-save-load-new-from-editor.md` row 14).

## Projects (task 2.6.1)

- **`ProjectSession::set()` has exactly ONE call site: `adoptProject` in `scene_session.cpp` (INV-P1),
  and `adoptProject` always calls `newScene` first (INV-6).** The identical rule the Scene I/O section
  above holds for `World::clear()`/`CommandStack::clear()`, applied one layer up: a project swap that
  ever skips resetting the scene, or resets the scene without going through `adoptProject`, is a
  data-corruption bug, not a style issue. `openProjectPath` and `createAndOpenProject` are the only
  two functions that call `adoptProject`; neither is a second policy, both route through the one
  function. **No CI script enforces this** — it is held by discipline and by `tests/editor/
  project_test.cpp`'s cases plus `imgui_layer_test.cpp`'s I18/I21, not by a grep, the same posture the
  Undo/redo section above takes for the merge-chain gate.
- **The Asset Browser's root is a RECONCILE, never a push (D10).** `EditorApp::tick()` compares
  `assetBrowserPanel->root()` against `project.assetsRoot()` every frame and calls `setRoot()` only on
  a mismatch — the project swap itself never calls `setRoot()`. One `std::string` comparison that
  cannot be half-performed and cannot drift, whether the project changed via the menu, the Welcome
  window, argv, or the startup restore. The RootOrder/Hierarchy precedent (`editor_app.hpp`), applied
  to a second panel. A panel born with the CORRECT root at construction time (e.g. a project passed at
  `EditorApp::create()`) never exercises this path at all — the reconcile is a no-op for it, which is
  why a seed dropping the reconcile block does not redden every project-aware GPU case, only the ones
  that swap projects at *runtime* (2.6.1's sabotage seed S14 found exactly this the hard way).
- **`FileDialogHost::projectRoot` is a `std::string_view` bound to `project.scenesRoot()`, which
  returns BY VALUE.** Binding it directly inside a call expression dangles the instant the
  full-expression ends — the named-local-first pattern (`const std::string scenesRoot =
  project.scenesRoot();` then construct the host from that local) is MANDATORY at every construction
  site, not style. This is the one defect class in this task that could ship green through every CI
  lane: no test tier can trigger a native-dialog launch, so nothing but ASan on a human's own Debug run
  would ever catch it, and even that only on a frame where a dialog was actually opened.
- **The New Project modal and the Welcome window mutate NOTHING directly — every control sets exactly
  ONE request field** (`form.createRequested`, `form.browseRequested`, `flow.requested =
  FileAction::NewProject`, …) **and nothing else.** Consumption happens OUTSIDE the draw walk, in
  `applyFileRequests`, which is what gets both windows the unsaved-changes guard for free — the
  identical "record a pending action, apply it after the walk" rule the Panels section above states
  for the World/Selection, applied to project state. A control that calls `createProject`/
  `ProjectSession::set()`/`promoteRecent` directly from inside `project_ui.cpp`'s draw functions is an
  architecture bug (AC-46) with **no automated test able to catch it** — confirmed directly: sabotage
  seed S20 seeded exactly this violation and the whole 94-entry suite stayed green.
- **A `createProject` failure NEVER deletes anything it already created, on ANY failure path, EVER
  (D7/INV-P4)** — the target directory, `assets/`, `scenes/`, whatever exists at the moment of
  failure, all stay exactly as they are. An editor that recursively deletes a user-chosen directory
  tree on an error path (a full disk, a permission change, an antivirus lock) is worse than one that
  leaves an inert half-made folder behind. **This is proven by test only for the `CreateFailed`
  branch** (a chmod'd read-only target forcing failure at the `assets/` `create_directory` step) —
  the `WriteFailed` branch (the manifest write itself failing after both directories already exist) is
  structurally unreachable by any test in this tree, confirmed by direct sabotage (seed S11: adding a
  `remove_all` rollback there reddens nothing at all, in either build tier). Do not read a green suite
  as evidence this line holds on that specific branch — it is a real, open coverage gap. **Since the
  code-review round, this is also a CI guard, not only a coverage gap**:
  `.github/scripts/check-project-no-delete.sh` (the sixth architecture guard, proven red-on-violation
  by ctest case `project-no-delete.no_delete_e2e`) makes `remove_all`/`std::filesystem::remove`/
  `std::filesystem::rename`/a bare `::copy` a hard CI failure the instant one is WRITTEN into
  `project.cpp`, `project_file.cpp` or `project_ui.cpp` — the untested `WriteFailed` branch cannot be
  "fixed" with a rollback that would violate this rule even once a test could reach it.
- **`std::filesystem::create_directory`/`create_directories` on an ALREADY-EXISTING directory returns
  `false` with NO `error_code` set (measured, not assumed).** Deciding a scaffold failure from the
  bool return instead of from `ec` breaks the legal "adopt an existing empty directory" path every
  time E7/AC-13 legitimately hits it. Confirmed load-bearing by direct sabotage (seed S22).

## Project settings (task 2.6.2)

- **`PanelContext::project` is a `const ProjectSession&`, and the `const` is the enforcement.**
  INV-P1 ("the session's setter has exactly one call site") stops being a grep for panels and becomes
  a compile error: the channel that reaches every panel is the one that must be least able to write.
  A future append to `PanelContext` must clear the same bar — a *panel-general* need, not one panel's;
  a single panel's need is a constructor parameter (2.6.2's own build-version string) or a reconcile
  (2.6.1's `setRoot`).
- **The panel id `"Project Settings"` is FROZEN.** It is the ImGui window name *and* the `imgui.ini`
  settings key; renaming it orphans every user's saved layout for this panel. `"Project"` was
  rejected — Unity calls its *asset browser* the Project window, and this tree already has one of
  those, called `Assets`.
- **The panel is bound to the in-memory session, NOT to the file (D6): there is deliberately no
  Reload button.** A reload would need a mutable path to the session's setter, which INV-P1 forbids
  everywhere but `adoptProject` — and routing it through `adoptProject` from a panel would silently
  discard the user's scene. A panel may not do that; the File menu may, because it goes through the
  unsaved-changes guard. Hand-editing `project.json` while the editor runs therefore does not update
  the panel; `File ▸ Open Project…` on the same root is the documented way, and it resets the scene.
- **The model's `isOpen()` guard is load-bearing for a non-obvious reason.** `ProjectSession::manifest()`
  is NOT guarded by `isOpen()`, and `close()` resets it to a DEFAULT manifest whose `assetsPath` /
  `scenesPath` are `"assets"` / `"scenes"` — **not** empty. A model that read `manifest()` unguarded
  would render a plausible, entirely fictional project rather than obviously breaking.
- **`ImGui::Separator()` does not span table columns.** The `SpanAllColumns` promotion applies only to
  the legacy `Columns()` API; inside a `BeginTable` cell it spans that cell. That is why the settings
  model returns GROUPS and the panel draws `SeparatorText` outside the table, one table per group,
  both columns sized from one shared width.
- **`ImGui::SetWindowFocus(name)` DOES select a docked window's tab — but not where `FocusWindow`
  suggests.** That function's own "Select in dock node" block is COMMENTED OUT in 1.92.8; the
  selection happens in `DockNodeUpdateTabBar` via `g.NavWindow->RootWindow->DockNode == node`, which
  works because a *docked* window keeps `RootWindow == window`. Dock nodes update inside
  `DockSpaceOverViewport`, which runs later in the same frame than the menu bar, so a menu item's
  focus write lands with no one-frame lag. An unknown name is a silent no-op.
- **The Edit-menu item has NO automated coverage, and that is a decision, not a gap in the tests.**
  Its effect is `setVisible` (observable) plus `SetWindowFocus` (a dock tab's `SelectedTabId`, which
  no test tier in this tree can read). A `requestProjectSettings()` hook was rejected precisely
  because it would be a second path to the half that was already testable. Human rows 5-7 are its
  only proof — get it right by construction.
- **A group/row-count sabotage in this model does not stay contained to the row it directly damaged —
  guard the SHAPE before any positional read.** Two independent sabotage seeds (deleting the
  Format-version row; swapping the two groups' push order) each cascaded into every LATER index-based
  assertion and crashed the test binary with an ASan container-overflow, rather than reddening only
  the row(s) the edit touched — and in doing so killed the run before PS22's *independent* eight-row
  sum, the one case designed to catch a row that MOVED rather than vanished, could execute. The
  model's eight-row, two-group shape (D7) is right; the test file is what changed:
  `project_settings_test.cpp` routes every positional case through a `shapedGroups` helper that
  `REQUIRE`s the two group sizes first, so a shape regression now fails cleanly in the case that owns
  it. **A future append (a ninth row, a third group) updates that helper — it does not add unguarded
  `groups[0].rows[N]` reads beside it.** In a Release lane, where there is no ASan, the same
  regression is silent UB rather than a clean abort.

Full history: `docs/10-engineering-log.md`, Epic 2.1 / 2.2 / 2.5 / 2.6 entries.
