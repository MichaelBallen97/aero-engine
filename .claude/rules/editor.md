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
  frame of a restored layout it docks every registered panel that is **not docked**, into
  the node already hosting a panel that declares the *same* `defaultDockSlot()`, falling
  back to the dockspace root. **The test is `DockId != 0`, not "does the ini know this
  panel" — that narrower predicate shipped first and helped nobody who mattered.** A panel
  born floating writes a settings entry on quit (Pos/Size/Collapsed, and no `DockId` line at
  all, since ImGui omits it when the id is 0), so on the next launch the ini *has* heard of
  it and the narrow test skipped it, preserving an 81px sliver forever. A panel that **is**
  docked is never touched, wherever the user dragged it — that is the promise that matters.
  Accepted cost, stated rather than discovered later: a panel deliberately dragged out of the
  dock is re-docked next launch, because ImGui records a deliberate float and a never-placed
  panel identically and nothing in the ini separates them. Persistent floating needs its own
  design, not a looser predicate. **Adding a panel therefore needs no layout migration of its own — provided it is
  registered before the first `tick()`**, as all six defaults are (`EditorApp::create`). The
  pass is a one-shot consumed on the first drawn frame, so a panel registered through
  `panels()` *later* (a dynamically opened tool window) is not covered by it and free-floats:
  that case needs its own placement, and would be the same defect again. A test driving this
  path must set `EditorAppConfig::layoutIniPath`, or it rewrites the developer's real editor
  layout (the `recentProjectsPath` lesson, second instance) — and that field is **inert unless
  `persistLayout` is also true**, so setting the path and forgetting the flag silently takes
  the default-layout path and can false-pass.
- Drag-drop payloads: `std::memcpy` into a local, **never a cast** — ImGui's buffer is
  `alignas(1)` and the Debug lanes run UBSan. Decide drop legality by *peeking* with
  `GetDragDropPayload()`, so an illegal drop never draws a highlight.
- Keyboard chords go through `ImGui::Shortcut(..., ImGuiInputFlags_RouteGlobal)`, never
  `ctx.input()` — the latter has no notion of UI focus, so a focused `InputText` would
  swallow the chord. `ImGuiMod_Ctrl` maps to Cmd on macOS automatically.
- `Escape` does **not** quit. Esc is the universal *dismiss* key in an editor.
- Unimplemented menu items ship `enabled = false` with a tooltip naming the owning task —
  never a dead handler behind a stub.
- **`<imgui_stdlib.h>` is never included at all, and this line used to say the opposite.** Every text
  field in this tree goes through `engine::editor::inputTextString` (`editor/src/text_input.hpp`,
  promoted to a shared TU at task 2.2.2), a hand-rolled `ImGui::InputText(const char*, std::string*, ...)`
  wrapper — because vcpkg's prebuilt `imguid.lib` ships `imgui_stdlib.cpp.obj` built **without** ASan,
  and linking an unsanitized object into the Windows Debug lane's ASan-instrumented binary is an
  LNK2038 runtime-library mismatch. `git grep -n 'imgui_stdlib' -- editor/ tests/` finds only
  `text_input.hpp`'s own comments explaining why it exists, never an `#include`. Task 3.1.3's search
  box (A1) uses `inputTextString` a second time, for the identical reason; do not reach for
  `imgui_stdlib.h` "because it's the standard ImGui helper" — it is specifically the one header this
  project cannot link against on one of its three CI lanes.

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
  "fixed" with a rollback that would violate this rule even once a test could reach it. **Task 3.1.1
  widened the guard's allowlist from these three files to FIVE**, adding `asset_meta.cpp` and
  `asset_database.cpp` (D7/D8's "an invalid `.meta` is never overwritten" / "an orphan is never
  deleted" — see the Assets section below) — the guard's scope is now the project flow **and** the
  asset flow. **Task 3.1.2 widened it a second time, from five files to SIX**, adding
  `asset_cache.cpp` (D18) — see the Import cache section below for the nuance that widening needed.
  Its name stays narrower than its scope on purpose (a rename was considered and rejected both times
  because it would touch the workflow YAML, the ctest case name, `CLAUDE.md` and this file, each a
  place a rename can go half-done).
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

## Assets (task 3.1.1)

- **A valid `.meta` is NEVER rewritten (D6).** A scan of a fully-described tree writes **zero bytes**
  to disk — not "identical bytes", zero. `AssetDatabase::rescan` calls `writeTextFileAtomic` from
  exactly ONE call site (INV-A1), and only for records the planner marked `Created` or `Repaired`; an
  `Ok` record's `.meta` is untouched, mtime and all. Get this wrong and every user's repository dirties
  itself the moment they open a project — sabotage seed S13 (a planner bug that flags a valid record
  for write) is the most important seed in the whole matrix for exactly this reason.
- **An invalid `.meta` is never overwritten (D7).** A parse failure, a bad version, a nil GUID — none
  of them get "fixed" by a rewrite. One rule, no exceptions: a merge-conflicted sidecar still holds the
  real GUID one `git checkout --theirs` away, and a carve-out for "obviously broken" cases gets applied
  to that case by analogy the first time someone is in a hurry.
- **An orphaned `.meta` is reported and left on disk, never deleted (D8).** A transient
  `exists() == false` is indistinguishable from a real deletion, and the destructive reading is the
  irreversible one. This is enforced doubly: by discipline in `asset_database.cpp`, and by the widened
  `check-project-no-delete.sh` guard (see the Projects section above) — sabotage seed S21 (adding a
  `std::filesystem::remove` call for an orphan) reddens the guard **before any test even runs**, and
  `project-no-delete.no_delete_e2e` stays green throughout because it exercises the script against its
  own hermetic scratch tree, not the real source — both facts together are the intended CI behavior.
- **Duplicate GUIDs ARE repaired, deterministically (D9).** Sort byte-lexicographically by
  project-relative path; the FIRST claimant keeps the GUID, every later one gets a fresh, unclaimed
  GUID and is rewritten. The determinism (never case-folded — `'Z.png'` sorts before `'a.png'`, byte
  order) is what stops two developers on two machines ping-ponging the "fix" back and forth forever.
- **The move/rename invariant (INV-A8, D10) — a rule for 3.1.3 to meet, not yet enforced:** an editor
  operation that moves, renames or copies an asset MUST move, rename or copy its `.meta` in the same
  operation. A move that drops the sidecar is a silent identity loss — every scene reference to that
  asset dangles, and **no test in this tree can see it** happen.
- **The panel holds a `const AssetDatabase*`, reconciled every tick — never a reference member (D13).**
  `EditorApp` is movable and `create()` returns `std::optional<EditorApp>`, so every live editor has
  been through at least one move. A reference bound at panel-construction time binds to whatever
  address the pre-move `EditorApp` happened to occupy. Sabotage seed S23 tried exactly this and — on
  this machine, under ASan, through every test this tree drives — **reddened nothing**, a real,
  documented coverage gap rather than proof the reference form is safe: the likely reason is that this
  build's `EditorApp::create()` elides the conversion-move of its named local into the returned
  `std::optional`, so the address a reference would have captured never actually moves on this specific
  compiler/build combination. Do not read that green run as license to switch back to a reference.
- **`.aero-tmp` is skipped by the scan, not deleted (D16).** `writeTextFileAtomic` transiently creates
  `<path>.aero-tmp` inside the user's own assets tree; a killed editor leaves one behind. Without the
  suffix check in `isScannableAssetName`, a leftover `wood.png.meta.aero-tmp` would itself be treated
  as a scannable asset and given `wood.png.meta.aero-tmp.meta`.
- **The panel's root and the database's root are the SAME string by construction (INV-A9/A16).** Both
  are reconciled from `project.assetsRoot()` in the same `EditorApp::tick()` block, which is the only
  reason `AssetBrowserPanel::selectedEntry` (relative to the panel's root) is a valid
  `AssetDatabase::findByPath` key. If they ever diverge, the footer silently shows `no .meta` for
  **every** file — no crash, no log, no red test (confirmed directly: sabotage seed S25, dropping the
  panel's database-pointer reconcile, reddens nothing in this tree's automated suite either — AC-37's
  whole surface is human validation rows 3 and 8).
- **`asset_meta.cpp` and `asset_database.cpp` never log (INV-A3).** `rescan` returns a report; ALL
  logging for the asset scan lives in `editor_app.cpp`'s `logAssetScan`, called from `tick()`'s
  reconcile block AFTER `rescan()` returns — a pre-write announcement was considered (D14) and rejected
  as unachievable through a report-returning function and worthless anyway, since the Console panel's
  `pumpLog()` runs at the top of the NEXT tick regardless of when within the current tick a line was
  logged.
- **`EditorApp::tick()`'s reconcile block does double duty, and that is load-bearing to remember when
  reading or changing it.** The block that scans the database (D12, task 3.1.1) is the SAME block that
  already existed for the Asset Browser's panel-root reconcile (D10, task 2.6.1) — 3.1.1 extended the
  existing block rather than adding a second one beside it. A seed that deletes "the reconcile block"
  wholesale (S22) therefore reddens FOUR GPU-tier cases, not one: I21 (2.6.1's own panel-root case),
  I27, I28 and I29 (3.1.1's database cases) — confirmed directly, and a real deviation from what a
  reader might expect from the two tasks' separate D-numbers.
- **`AssetBrowserPanel::database()`'s accessor and the `databasePtr` member cannot share a name (D13's
  naming note).** Neither `AssetDatabase::findByGuid` nor `AssetDatabase::findByPath` is involved — the
  collision is entirely within `AssetBrowserPanel` itself (code-review finding 6, correcting this
  paragraph's earlier, wrong attribution). The member is `databasePtr`, the accessor `database()` — the
  `RenderTarget::depthFormatValue` / `depthFormat()` precedent from 2.3.1, applied a second time to this
  exact class of collision.

## Import cache (task 3.1.2)

- **The cache is NEVER in `.meta` (D3).** `.meta` v1 is untouched by this task: no new key, no version
  bump, no parser change beyond the additive `Reattached` state. A content hash in a committed file
  would need a third write path, repealing D6 — the whole reason the cache lives at
  `<projectRoot>/Library/asset-cache.json` instead.
- **Derived data is disposable (D7), and the policy is the deliberate INVERSE of `.meta`'s.** `.meta`
  discards a bad key and preserves everything else, because losing a `.meta` loses an identity
  permanently. The cache discards the WHOLE file on any structural failure and never repairs a single
  bad entry, because losing the cache costs exactly one re-import — the cheaper failure mode gets the
  cheaper, blunter policy.
- **A scan of an unchanged project writes ZERO bytes anywhere (D15 / INV-C5) — not "identical bytes",
  zero — and now across TWO files, not one.** The index is written only when its text differs from the
  text it was read from. The writer must stay deterministic (fixed key order, entries sorted by
  `guid`), because non-determinism silently reintroduces a per-scan write with no error and no failing
  test to catch it.
- **The cache is keyed by GUID, never by path (D11).** Moving an asset together with its sidecar must
  not invalidate its import — the entry's `path` is updated in place, its `contentHash` stays valid.
  An asset with no valid identity (no `.meta`, or an invalid one) is never in the cache at all.
- **Orphan re-attachment needs ALL FIVE of D13's conditions, or it does nothing — never a near-miss
  log.** The candidate has no sidecar; its content hash was computed this scan (not skipped by budget);
  exactly one previous cache entry has that hash **and** a path this scan no longer sees; that entry's
  GUID is claimed by no live asset; and exactly one orphaned sidecar on disk parses to that GUID. Any
  one condition failing means nothing happens — no match, no partial state. The old, now-redundant
  sidecar is never deleted; a "Delete orphaned .meta" action is user-initiated and is 3.1.3's, not
  this task's.
- **D9 and its correction — the alias rule is DIRECTORY-ONLY and keys on canonical path, never on
  content.** A content hash CANNOT fix the symlinked-directory duplicate-GUID defect: it cannot
  distinguish one file reached twice from two legitimate identical copies, and using it to suppress a
  second record would silently delete the identity of every duplicated texture in every project.
  File-level symlinks and hardlinks are deliberately **not** deduped — only a symlinked *directory*,
  by its resolved canonical path. **This corrects an earlier, wrong rationale recorded in
  `docs/10-engineering-log.md`'s 3.1.1 entry**, which said the content hash was why 3.1.2 would own
  this defect; it was not, and the correction is recorded in that same file. 2.2.4's D13 ("a symlinked
  asset folder behaves like a real one") **remains true for the Asset Browser** and changes **only for
  the asset scan** — the split is deliberate, record it explicitly whenever either D13 is cited.
- **D18's guard nuance, easy to misread as a contradiction of D7.** The cache's DATA is disposable, but
  nothing in this task deletes a file to dispose of it: a discard means "do not carry entries
  forward", `Reimport All` means clear the in-memory index, a rebuild means an atomic overwrite. Listing
  `asset_cache.cpp` in `check-project-no-delete.sh`'s allowlist costs nothing today and stops a future
  "clean the Library folder" `remove_all` from being written without a review; if that is ever wanted,
  it is a deliberate, reviewed relaxation that must delete only inside `Library/`, never inside the
  assets tree.
- **The amended INV-A1.** 3.1.1's "exactly one `writeTextFileAtomic` call site" was about the assets
  tree only. This task adds two more call sites, both building their path from the library directory —
  the amended invariant is exactly **one** call built from the **assets** root and exactly **two**
  built from the **library** directory, each path assembled into a named local first
  (`metaAbsolutePath`, `ignorePath`, `indexPath`) so the invariant stays grep-decidable rather than
  heuristic.
- **A4's trap carries over from `Guid` to `ContentHash`.** `ContentHash::valid()` is false for the
  digest of the empty input, which is a perfectly legitimate value — it is NOT a "was this hashed?"
  flag. The only such flag is engagement of `std::optional<ContentHash>`. `parseAssetCache` accepts a
  nil `contentHash`/`metaHash`; only `guid` may never be nil.
- **A2's trap: `FileEntry`'s three new fields (`mtime`, `mtimeKnown`, `isSymlink`) are APPENDED, never
  inserted.** `tests/editor/project_files_test.cpp` holds two positional aggregate initializers of
  `FileEntry`; inserting a field ahead of `isDirectory` would silently re-map them (`bool` → `int64` is
  a promotion, not a narrowing conversion, so nothing diagnoses it). Both helpers were converted to
  designated initializers as defence in depth for the *next* field addition.
- **A3's cost, measured, not assumed.** `isSymlink` is free everywhere (the cached `d_type`/symlink
  status from the directory iteration). `mtime` costs one extra `stat` per file on POSIX (libc++ and
  libstdc++ both take the uncached branch) and nothing on Windows (`FindFirstFileW` already returns it).
  Do not add `entry.refresh(ec)` to make it "free" — that changes the dangling-symlink asymmetry
  2.2.4's code review already had to discover and fix once.

## Asset browser v1 (task 3.1.3)

- **Thumbnails are a strict two-phase system, and the phases live in different TUs on purpose.**
  `editor/include/aero/editor/thumbnail_cache.hpp` (`ThumbnailLedger`) is PURE — no ImGui, no
  `<filesystem>`, no GPU — and owns only the key, the `Absent`/`Ready`/`Failed`/`Skipped` state
  machine, and a budget/LRU eviction policy; `editor/src/thumbnail_store.hpp` (src-private) is the
  ONLY stb_image TU and the ONLY GPU-touching TU for thumbnails, and does the actual
  read → decode → resample → upload. Nothing above the pair (the panel, `EditorApp`) ever sees a
  decoded pixel or an `rhi::TextureHandle` — only `ThumbnailState`/`ThumbnailKey`.
- **A `ThumbnailKey` is `{Guid, ContentHash}`, never a bare `Guid`.** A record whose content hash was
  never computed this scan (skipped by budget, or `metaWriteFailed`) has no valid key at all and is
  never touched — a garbage/zero key would decode a file that was never actually hashed. Dropping
  either guard is sabotage seeds S9/S10, both `I36`-visible only when the GPU-tier test's own budget
  is small enough to force an un-hashed record into existence.
- **`Failed` and `Skipped` are STICKY, and `nextDecodes()` is where that stickiness is enforced — not
  at the call site, where a future edit could forget it.** A file that failed to decode once is never
  retried, forever, in the same session: `nextDecodes` returns `Absent` keys only, oldest-touched
  first. This is the single most important property in the whole subsystem (D9) — without it, a
  broken image in a folder re-reads and re-fails every tick, forever, at the decode budget's full
  rate.
- **Eviction excludes anything touched THIS frame, even when that leaves the resident count above the
  cap.** Evicting a tile that is currently on screen would decode it again next tick, forever, in any
  folder with more visible tiles than `MAX_THUMBNAILS_RESIDENT` (E12) — a thrash loop, not a bug that
  merely wastes memory.
- **`serviceThumbnails()` runs OUTSIDE the ImGui draw walk, in the same slot as `renderScene()`
  (`EditorApp::tick()`, between `drawShellUi` and `endFrame`), never from inside `onDraw()`.** This is
  a real, load-bearing architectural rule, but **no automated test tier can see it violated in the
  general case** — sabotage proved this precisely: moving the call to the very START of `onDraw()`
  (before any tile has been touched that frame) DOES redden four GPU-tier cases, because thumbnails
  then decode a full frame behind their own visibility; moving it to the natural END of `onDraw()`
  (after every tile has already been drawn and touched) reddens nothing at all, 64/64 green. Get the
  call site right by construction — human validation row 4 ("never stutters") is this rule's only
  general-case cover.
- **`fitRgbaIntoTile`'s box-filter resampler is 100% INTEGER — no floating point anywhere.** This is
  what makes a decoded thumbnail's output bytes identical on macOS, Windows and Linux, and therefore
  assertable in a portable test at all; a float accumulation would be exactly representable for the
  sample counts this project's images produce (≤ 500 000 uint8 samples fit exactly below 2²⁴), so a
  seed swapping in `float` accumulation is a documented, machine-dependent non-discriminator here
  (S7) — never read that as license to introduce floating point into this one function.
- **The orphan-`.meta`-delete action re-verifies ALL FIVE conditions on the real filesystem, in a
  fixed order, immediately before deleting — never trusts a stale scan result.** `validateOrphanPath`
  (no `..` segment, no absolute path, no drive letter, no `\` separator, a real `.meta` filename) →
  the sidecar still exists → it still reads as text → it still parses as a `.meta` v1 sidecar with a
  valid GUID → **the asset it describes does not exist again** (the race-closing check: something
  could have re-created the asset between the scan and the click). Any single failure refuses the
  delete and leaves the file untouched — never a partial state, never a "probably fine" heuristic.
  Reordering this sequence (sabotage seed S22) is the single most important seed in the delete half:
  it reddens every "still on disk after a refusal" case in the suite simultaneously.
- **Check B (`check-project-no-delete.sh`) is a POSITIVE two-file allowlist, added beside Check A's
  six-file denylist, not instead of it.** Check A only catches a delete/rename/copy written into one
  of six NAMED files; a new destructive call in a SEVENTH, unnamed `editor/src/*.cpp` file passes
  Check A silently. Check B closes that hole by scanning every tracked `editor/src/*.cpp` and refusing
  a `remove_all`/`std::filesystem::remove`/`std::filesystem::rename` outside
  `PERMITTED_DELETERS = {text_file.cpp, asset_actions.cpp}`. **`::copy` is deliberately NOT in Check
  B's pattern** — unlike Check A's `FORBIDDEN_RE`, Check B's `DELETE_RE` would false-positive on the
  first ordinary `std::copy` (an `<algorithm>` call, not a filesystem one) written anywhere under
  `editor/src/`, and a guard that cries wolf is a guard that gets relaxed. `text_file.cpp` is
  permitted because its internal `rename` IS the mechanism that makes `writeTextFileAtomic` atomic;
  `asset_actions.cpp` is permitted because it holds the ONE sanctioned orphan-sidecar delete (D12/D13
  above). A third self-test (B-self-test 3) makes renaming `asset_actions.cpp` without updating
  `PERMITTED_DELETERS` fail LOUDLY (exit 2, "cannot self-verify") rather than silently widening the
  check to "nobody is permitted, so nothing matches, so we pass" (sabotage seed S24).
- **`inputTextString`, not `imgui_stdlib.h`, for the search box (A1)** — see the corrected
  `<imgui_stdlib.h>` entry in the ImGui section above; this task's search box is imgui_stdlib.h's
  second confirmed non-use, not its first.
- **A hidden Asset Browser panel still runs `serviceThumbnails()` every tick (A5/AC-34).** The
  thumbnail budget/eviction pass is NOT gated on the panel's own visibility — a tabbed-away Asset
  Browser keeps decoding and evicting exactly as if it were on screen. This is a deliberate choice,
  not an oversight: gating the service call on visibility would make "switch to the Assets tab" a
  visible multi-tick stutter every time, trading a background cost nobody sees for a foreground one
  everybody would.

## Hot-reload watcher (task 3.1.4)

- **The watcher's VISIBLE SET must equal the scan's, byte for byte, or the editor rescans forever
  (D4/INV-W3).** If the watcher observes one file the scan ignores, and that file changes, the
  watcher fires a rescan; the rescan writes nothing and reports nothing; the watcher fires again next
  sweep; forever. `.DS_Store` alone would do it on every macOS machine. The equality is held by
  `isWatchableAssetName` (`asset_meta.hpp` — exactly `isScannableAssetName(n) || isMetaFileName(n)`,
  defined beside the two predicates it composes) plus IDENTICAL traversal bounds:
  `includeHidden=false`, `MAX_TREE_DEPTH`, `MAX_ASSETS`, the symlink-only `canonicalDirectory`
  optimisation, and the canonical `Library/` exclusion. **A change to either walk is a change to
  both.**
- **`Library/` is excluded by CANONICAL PATH, derived from the PROJECT root — never from
  `assetsRoot + "/.."`.** `paths.assets` is user-configurable, and a project whose assets root is
  `"."` puts `Library/` inside the watched tree, where every scan may rewrite
  `Library/asset-cache.json`. **This is the one exclusion no normal project's tests can reach**, and
  `AW34`'s `Library/` arm — driven with an assets root equal to the project root — is the only place
  in the tree that proves it (sabotage confirmed `AW41`'s arm (c), a project-root-only variant, also
  discriminates the same guard).
- **A sub-directory whose listing FAILS carries its committed children forward; an unreadable ROOT
  aborts the sweep (D5/INV-W6).** An antivirus lock, a cloud-sync pass or a permission change would
  otherwise present as "every file under it was deleted" — which means dropped records, orphan
  reports, released GPU thumbnails, and the exact reverse one sweep later. **Absence observed through
  a failure is not absence.** This is 3.1.1 D8's instinct ("an orphan is reported and left on disk,
  never deleted") and `canonicalDirectory`'s ("a caller that cannot prove two paths are distinct must
  refuse to descend, never guess"), applied a third time.
- **Settlement has TWO conditions and both are decided in `isSettled`, never at a call site
  (INV-W4).** Age alone is wrong on Windows (NTFS does not flush last-write-time while a handle is
  open, so a 2 GB copy can present a stale, OLD mtime for its whole duration); stability alone is
  wrong on FAT32/HFS+ (a 1–2 s mtime quantum makes two same-size writes indistinguishable). That is
  why `WATCH_SETTLE_MS` is **2000** — above FAT32's own quantum, not merely "a bit".
- **An unsettled entry's stamp never enters `committed` (INV-W5/AC-12)**, including on a FORCED fire:
  `applyChanges` applies only the settled changes. The single sanctioned exception is
  `noteExternalScan()`, which adopts `lastProbe` wholesale — safe because a torn stamp means the file
  was still growing, so its final stamp necessarily differs and the next two sweeps still report it.
  Do not "fix" that into `primeOnNextSweep`.
- **`asset_watcher.{hpp,cpp}` never log, never write, never rename, never copy and never delete
  (INV-W1).** `asset_watcher.cpp` must **never** be added to `check-project-no-delete.sh`'s Check B
  `PERMITTED_DELETERS` allowlist — being outside it is exactly what makes a future
  `std::filesystem::remove` there a hard CI failure. It is also `<filesystem>`-free, `<fstream>`-free,
  `<thread>`/`<mutex>`/`<atomic>`-free, and performs **zero file reads**: `poll()` opens directories
  and reads nothing.
- **The watcher POLLS, and that is a decision with recorded reversal conditions, not a default.** No
  FSEvents, no `ReadDirectoryChangesW`, no inotify, no owned thread, no `#ifdef` — the watcher itself
  contains no per-OS branch and no `JobSystem` reference, and this task keeps it that way.
  **The claim this bullet used to make — that `git grep -n '_WIN32\|__APPLE__\|__linux__' -- editor/`
  is EMPTY — has been false since task 3.2.2**, which vendored ufbx into `editor/third_party/` with
  three of upstream's own per-OS lines, and it is false a second time since task 3.2.4, whose
  `currentHostOs()` adds exactly three more. The live invariant is the one in the Blender CLI section
  below: over `editor/src` + `editor/include`, exactly three lines in exactly one file. A native backend replaces `poll()` behind the
  same `AssetWatcher` seam without touching a consumer, and only once a measured sweep cost exceeds
  ~2 ms/tick or a script-reload loop makes 1–2 s of latency painful.
- **GPU textures are destroyed ONLY from `serviceThumbnails()`, only AFTER its touch loop, and only
  for keys not touched at `currentFrame` (D9/INV-W8).** `ThumbnailLedger::supersededBy` takes
  `currentFrame` as a PARAMETER precisely so this is structural rather than a call-site convention.
  This is 3.1.3's BLOCKING-1: `SDL_ReleaseGPUTexture` frees synchronously on Vulkan/D3D12 and only
  defers on Metal, so the bug is deterministic on Windows and Linux and invisible on the only
  platform with a human pass. **Confirmed directly**: sabotage seed S23 (moving the sweep into
  `onDraw()`) reddened no runtime test on Metal, matching 3.1.3's own S25 precedent exactly, and is
  caught only by a human reading `git grep -n 'supersededBy' -- editor/src/`'s two lines and noticing
  which function the call now sits inside.
- **`AssetDatabase::generation()` is the ONE signal that drives every downstream refresh (D8).** It is
  bumped as the FIRST statement of `rescan()` — not the last — because `rescan()` has three return
  paths. Do not derive "a scan happened" from the report's contents or from `assetRescanRequested`;
  both miss a path.
- **`EditorApp::tick()`'s reconcile block now does QUADRUPLE duty** — 2.6.1's panel root, 3.1.1's
  database, 3.1.3's report, and 3.1.4's watcher. Extend it; never twin it. A seed that deletes the
  block wholesale reddens a growing number of GPU cases (four at 3.1.1; more now) — confirmed
  directly: sabotage seed S15 (skipping ONE STATEMENT inside it, the priming branch, not even the
  whole block) already reddens **twelve** tier-0 cases by itself, since every case that builds a
  quiescent baseline depends on the very first sweep priming silently.
- **`AssetWatcher` holds no `std::unordered_map` and no `std::set` (INV-W9).** It is a value member of
  `EditorApp`, whose move is `noexcept = default`; MSVC's node-based containers are not
  nothrow-movable (3.1.2's R9, measured in CI as C2607). Sorted `std::vector`s only, with the
  `recordList` / `byGuid` / `ThumbnailLedger::entries` precedents. **Confirmed as a Windows-only
  discriminator**: sabotage seed S25 (swapping `visitedCanonical` for a `std::set`) compiles clean and
  reddens nothing on macOS/libc++, whose `std::set` move constructor happens to be `noexcept` too —
  the aggregate `static_assert`s are the ONLY thing standing between this and a real MSVC C2607.
- **The FIRST scan of a fresh project WRITES `.meta` sidecars, which the next sweep legitimately sees
  as additions and acts on exactly once (D4/E17) — and this is NOT limited to the first scan.**
  **Confirmed directly, correcting the task's own plan**: ANY brand-new file with no prior sidecar
  produces the SAME two-trigger echo (the file, then its freshly-written `.meta`) — a plan draft that
  asserts `+1` trigger for a later file is wrong for the identical reason the first-scan case is `+2`,
  not `+1`. Every GPU-tier case in this task ticks to QUIESCENCE first and asserts a trigger-count
  DELTA, never an absolute value, and — after any write of a genuinely new file — a SECOND quiescence,
  never a fixed sweep count.
- **Proving `noteExternalScan()`'s call site needs a MODIFICATION and a sweep that cannot complete —
  never a poison file.** Two independently-designed GPU-tier scenarios (`I50`'s) both failed to
  discriminate the call site, and the conclusion first recorded here — that no count-only accessor
  ever could, and that closing it needed a new seam exposing `lastDiff()`'s paths — was **wrong**, as
  the code-review round showed. The defeating property was the scenarios' own **permanently unsettled
  poison file**: the eventual forced fire produces a trigger whether or not the call ran, so the
  re-reported path merely rides along inside it and the signal collapses from "0 vs 1 trigger" to
  "2 vs 3 items inside one trigger". `I51` closes it with the existing accessors: a **modification**
  (D6 never rewrites a valid `.meta`, and the hash lives in the excluded `Library/`, so there is no
  sidecar echo for a duplicate to hide inside) plus `dirsPerPoll == 1` over a two-directory tree, so
  the tick carrying the manual rescan provably cannot complete a sweep and `watchFired` is therefore
  false — the only branch that reaches `noteExternalScan()`. With the call, delta 0; without it,
  delta 1. **The general lesson: a scenario that ends in a FORCED fire cannot discriminate anything
  about what a trigger contained, because the trigger was going to happen regardless.**

## Model import (task 3.2.1)

- **fastgltf NEVER touches the filesystem; the editor supplies every byte (D3).**
  `GltfDataBuffer::FromBytes` over bytes `readFileBytes` already read is the ONLY construction path.
  `Options::LoadExternalBuffers`, `Options::LoadExternalImages`, `GltfDataBuffer::FromPath`,
  `MappedGltfFile` and `GltfFileStream` must NEVER appear anywhere in this tree -- a `.gltf` is a
  user-supplied document from the internet, and `"uri": "file:///etc/passwd"` is syntactically legal
  glTF that fastgltf's own `isLocalPath()` would accept. With the editor resolving, every URI passes
  `classifyUri` first and a refusal is a WARNING, not a read.
- **`classifyUri` receives an ALREADY-PERCENT-DECODED URI.** `fastgltf::URI` decodes on construction
  (`src/fastgltf.cpp`: `URI::URI` calls `decodePercents`). Do not decode again -- `100%2520.png` would
  become `100 .png`. Decode-then-classify is also the SECURE order: an encoded scheme (`%68ttp:`) is
  already `http:` when we classify it.
- **THE IMPORTER CONVERTS NOTHING, and this is the easiest thing in the task to get catastrophically
  wrong.** `aero/core/math.hpp`'s own header comment says the engine's conventions were chosen to match
  glTF 2.0 and names importers as one of the four consumers that inherit them. Verified at the bit
  level against fastgltf 0.9.0: `math::quat` is `{x,y,z,w}` defaulting to `{0,0,0,1}`; `math::mat`
  stores COLUMNS. **No handedness flip, no axis swap, no winding reversal, no quaternion reorder, no
  transpose, no degree conversion.** There is no `convertFromGltfSpace()`, and `MI40b` plus sabotage
  seeds S29/S30 exist to prove that adding one is caught. **3.2.2 (FBX: Z-up, centimetres) IS the task
  that needs a conversion**, precisely because glTF is the canonical format these types were built
  around.
- **Every accessor is validated BEFORE any fastgltf tool touches it.** fastgltf `assert`s on a type
  mismatch (a Debug abort in our lanes), dereferences a possibly-disengaged `Optional` for a sparse
  accessor with no `bufferView` (its own header says that case is real), and never bounds-checks the
  accessor -> bufferView -> buffer chain; `fastgltf::span` is unchecked and
  `DefaultBufferDataAdapter` range-checks nothing. `validateAccessor` is what makes a hostile document
  cost a warning instead of a crash or an out-of-bounds read.
- **The custom buffer-data adapter must apply the bufferView's own `byteOffset`/`byteLength`.**
  fastgltf's callers apply only `accessor.byteOffset` on top. An adapter that returns the whole buffer
  reads from the wrong offset with NO error -- plausible-looking garbage geometry.
- **`Structure` and `Full` are ONE function with a depth parameter (INV-M4).** They can never disagree
  about the URI set, the counts, the names or the hierarchy, because they compute them with the same
  code. The SCAN runs `Structure` only, budgeted, and only for assets already in `plan.jobIndices`; the
  PANEL runs `Full`, on demand, for one asset, in two passes.
- **A scan still writes ZERO bytes to a fully-described tree (D7).** The `importer` block is written by
  exactly ONE code path -- `ModelImportSession::applySettings`, reached from the panel's Apply button,
  driven by a person who changed a value. The scan never mints, upgrades or normalizes a block.
  `writeMetaText(guid, ImportSettings{})` is byte-identical to `writeMetaText(guid)` BY CONSTRUCTION
  (the one-argument overload delegates), so `minimal.meta`'s 65-byte fixpoint keeps passing unedited --
  and THAT is the evidence this task did not disturb the committed format.
- **`.meta` STAYS AT VERSION 1 (D6).** A v2 bump makes an older build nil EVERY GUID in the project
  (`parseMeta` rejects the version -> `Invalid` -> nil guid; D7 then forbids repairing it), with no
  recovery path that does not involve hand-editing files. An additive optional key degrades instead.
- **A malformed `importer` block NEVER invalidates an identity (AC-12/INV-M11).** `error` stays
  `MetaError::None`, `guid` stays engaged, and the failure surfaces only through `importer`
  (disengaged) + `importerMessage` (non-empty). Version and guid are IDENTITY; the block is PREFERENCE.
- **`ImportInput::probe` engagement is the "was this probed?" signal (D9).** A disengaged probe makes
  `commitImports` carry the PREVIOUS entry's importer, importerVersion and dependencies forward
  verbatim. Passing `""`/`0` instead would flip the importer, make `planImports` report
  `ImporterChanged` next scan, flip it back, and oscillate FOREVER.
- **The probe records THIS scan's dependencies for the NEXT scan's cascade (D8/E11).** It runs after
  `planImports` and before `commitImports`. A brand-new model plus its brand-new texture cascade
  correctly from the SECOND scan onward -- correct, since both are `New` on the first scan anyway.
  **A test that edits a texture immediately after adding a model must tick TWICE.**
- **`dependencies` never contains a nil GUID, is sorted, is deduplicated, and drops self-edges
  (INV-M8/E9).** A nil would make the cascade's `find` return `nullptr` for a reason unrelated to the
  graph; a self-edge would make the worklist treat the node as permanently dirty.
- **The Full import runs from `EditorApp::tick()` only, never from `onDraw()` (INV-M12).** Like
  3.1.3's BLOCKING-1 and 3.1.4's D9, **no automated test tier can see the general-case violation** --
  `I60` reads `editor_app.cpp`'s own source text and manual row 8 is the only behavioural cover. Get it
  right by construction.
- **`"Import Details"` is FROZEN.** It is the ImGui window name AND the `imgui.ini` settings key.
  Renaming it orphans every user's saved layout for that panel.
- **Editing import settings is NOT undoable, and that is a decision (D19).** `CommandStack` is
  scene-scoped by construction and is cleared on every World swap; a file edit does not belong in it.
  Apply is a plain, explicit, single-shot atomic write, and the file itself is the record. This is the
  same posture the Project Settings panel takes toward `project.json` (2.6.2 D6).
- **`extensionsRequired` fails the parse INSIDE fastgltf, before we see the asset.** Both
  `Error::MissingExtensions` and `Error::UnknownRequiredExtension` map to
  `ImportStatus::MissingExtension`, and neither error names the extension -- **but only one of them is
  unrecoverable.** `MissingExtensions` means fastgltf KNOWS the extension and it was merely not enabled,
  so a **throwaway re-parse with `~fastgltf::Extensions::None`** recovers the names; the `Asset` is
  discarded, so D20 is intact. `UnknownRequiredExtension` means fastgltf does not know it at all, and no
  setting changes that. **`Extensions::All` does not exist in 0.9.0** -- that `All` belongs to
  `Category`. See §A-6 and §D's `describeRequiredExtensions`.
- **`gltf_import.cpp` sees `<filesystem>` transitively and cannot avoid it** (`Parser::loadGltf` takes
  a `std::filesystem::path` positionally). The rule that IS true and IS checked: **it performs no file
  operation at all.** `directory` is `{}`, which fastgltf explicitly supports when no `LoadExternal*`
  option is set.
- **`Options::DecomposeNodeMatrices` destroys the fact that a node used a `matrix`.** The AC-19 warning
  is therefore a deliberate, labelled HEURISTIC: one `find` for `"\"matrix\""` over the source bytes,
  producing ONE aggregate warning per document. A false positive costs one warning; a false negative
  costs none; neither affects correctness.
- **UPSTREAM DEFECT, do not trip it:** `fastgltf::URI::decodePercents` reads out of bounds on a URI
  ending in `%` (`chars = {x[i+1], x[i+2]}` with `i == size()-1`). No fixture or test in this tree may
  contain one. `%zz` (three characters) is in range and is the form used for the control-character
  refusal case.
- **The panel's service() call site has NO automated general-case cover, the identical
  `serviceThumbnails()` shape 3.1.3 and 3.1.4 both already carry.** `ModelImportSession::service()` runs
  from `EditorApp::tick()`'s post-draw slot, the THIRD occupant after `renderScene()` and
  `serviceThumbnails()` -- never from `ImportDetailsPanel::onDraw()`. `I60` proves it the only way this
  target can: reading `editor_app.cpp`'s own source text (comment-stripped) and asserting the ONE call
  site sits textually AFTER `drawShellUi(`, the single call that invokes every panel's `onDraw()`. That
  is a proof about THIS file's current text, not a structural guarantee against a future refactor that
  moves the call somewhere else that still reads as `editor_app.cpp` -- manual validation row 8 (ten
  models selected one after another, no stutter) is the only behavioural cover, exactly as it is for
  `serviceThumbnails()` and the watcher's superseded-texture sweep.
- **`AssetBrowserPanel::selectedEntry` needed a SECOND public seam beyond `selection()` to be drivable
  from the ImGui-free-at-source GPU tier, not anticipated by the plan's own literal count.**
  `requestSelectEntry` (panel) / `requestAssetBrowserSelectEntry` (`EditorApp`) are the
  code-review-finding-4 shape (`requestViewMode`/`requestSearchQuery`/`requestKindFilter`/
  `requestDeleteOrphanClick`), a fifth application: each records EXACTLY what a real single click on a
  row/tile records (`ActionKind::SelectEntry`). Without it, `ModelImportSession`'s whole reconcile path
  has no way to be driven from `imgui_layer_test.cpp` at all. **A case that calls it still needs
  `AssetBrowserPanel::onDraw()` to actually RUN for the click-equivalent action to be drained at all** --
  "Assets" shares `DockSlot::Bottom` with "Console" (registered first, so it wins the tab by default),
  so every such case calls `app->panels().setVisible("Console", false)` first (2.2.4's C5 precedent,
  restated for a sixth reason).

## FBX import (task 3.2.2)

- **ufbx is VENDORED at `editor/third_party/ufbx/` and is BYTE-IDENTICAL TO UPSTREAM v0.23.0. No local
  patches, ever.** It is in no vcpkg registry (measured twice), so vendoring is not a preference. The
  decisive property: `ufbx.c` compiles with THIS project's directory-scope options, so it is
  ASan/UBSan-instrumented in all three Debug lanes -- the single most valuable place in this tree for a
  sanitizer to be watching, since ufbx parses untrusted binary files. A needed fix goes upstream or
  into a wrapper in `fbx_import.cpp`. A patch here must be re-applied by hand at every bump, correctly,
  with its reason invisible at the call site.
- **ufbx NEVER touches the filesystem.** `ufbx_load_memory` is the only load call. `ufbx_load_file`,
  `ufbx_load_file_len`, `ufbx_load_stdio*` and `ufbx_load_stream` must never appear anywhere in this
  tree. `load_external_files` stays FALSE, and `open_file_cb` is set to a callback that CANNOT SUCCEED
  -- the default is stdio, so a future ufbx that opens a file for a reason `load_external_files` does
  not gate finds a closed door. `obj_mtl_path`/`obj_mtl_data` are never set: ufbx's own header notes
  those "sidestep `load_external_files` as they are explicitly requested."
- **`MODIFY_GEOMETRY` is the only correct `space_conversion`, and the reason is the Bounds row.**
  `ADJUST_TRANSFORMS` converts node translations but not geometry, so every mesh's positions and bounds
  stay in the source's units while the hierarchy is in metres -- the panel prints 100x the truth and
  nothing is internally inconsistent enough to notice. `TRANSFORM_ROOT` parks the factor on every root
  node, where it compounds with `ImportSettings::scale`. **Do not re-litigate this.** Measured against
  ufbx v0.23.0: a Z-up centimetre source converts with `geometry_scale 0.01`, `root_scale 1`, a -90 deg
  X rotation on the roots, and `mirror_axis 0` -- a pure rotation, hence no winding change.
- **`settings.axes.up` is the source's declared up axis; `original_axis_up` is NOT.** The latter reads
  the Autodesk round-trip properties `OriginalUpAxis`/`OriginalUpAxisSign`, which most exporters never
  write, and comes back `UNKNOWN` for an ordinary file. ufbx preserves `settings.axes` across the
  conversion by design. `SourceSpace` reads `axes.up`, with `original_axis_up` only as a fallback.
  **A real Blender export still declares `UpAxis: 1` (Y) while requiring the identical -90 deg-about-X
  geometric correction a Z-up source would** (measured against `cube-binary.fbx`, `FI76`) -- an
  internal convention of Blender's own FBX exporter, not evidence the rule above is wrong.
- **`UnitScaleFactor` in a hand-written ASCII FBX is in CENTIMETRES.** `1` means `unit_meters == 0.01`.
  **A Y-up METRE fixture needs `UnitScaleFactor: 100`.** Writing `1` makes it a centimetre fixture that
  the importer then scales by 100, and the case silently tests the wrong thing while looking green.
- **Geometry transforms, inherit modes: HELPER NODES. Pivots: RETAIN.** `ImportedMesh` is SHARED --
  `ImportedNode::meshIndex` points into one flat list -- so baking a geometry transform into vertices
  would force per-node mesh copies, changing the canonical model's shape for one format. A helper node
  is an ordinary `ImportedNode` to every consumer. `geometry_transform_helper_name` and
  `scale_helper_name` are set EXPLICITLY so the names are stable across a ufbx bump.
- **`opts.ignore_geometry`/`opts.ignore_animation` are set individually at `Structure` depth;
  `opts.ignore_embedded` NEVER is.** An earlier draft set `opts.ignore_all_content` instead, which is
  EXACTLY `ignore_geometry + ignore_animation + ignore_embedded` (`ufbx.c`'s own `ufbxi_load` folds it
  into the three sub-flags before anything else runs) -- so it also zeroed embedded-texture content at
  `Structure`, and an embedded texture fell through to the external-URI path and was recorded as a
  dependency: `Structure` and `Full` disagreed about the URI set for every embedded texture
  (AC-20/INV-M4), measured on the `FI54` fixture (`Structure`: `externalUris=[embedded.png]`; `Full`:
  `externalUris=[]`). At `Structure` depth `num_vertices`, `num_triangles`, `max_face_triangles` and
  `material_parts` are all zero/empty (gated on `ignore_geometry` alone, unaffected by the fix). What
  survives: nodes with full converted transforms, mesh/material/skin/animation IDENTITY, an embedded
  texture's own CONTENT (identical at both depths now), and all of `scene.settings`/`metadata` --
  including the three `*_ignored` flags, which make the depth OBSERVABLE rather than asserted:
  `geometry_ignored`/`animation_ignored` are true and `embedded_ignored` is FALSE at `Structure`, never
  "all three true". So `summary.primitiveCount == meshCount` and `jointCount == 0` at `Structure` for
  FBX, where glTF reports both structurally. That asymmetry is a property of the containers.
- **Fold `\` -> `/` BEFORE `classifyUri`, never after.** `..\..\..\etc\passwd` has already become
  `../../../etc/passwd` when the escape check runs, so the refusal fires. Folding after classification
  would let a backslash traversal straight through. `classifyUri` is NOT modified --
  `UriClass::RefusedBackslash` stays reachable for glTF, where a backslash really is a Windows path
  leaking into a format that has no such concept.
- **`ufbx_texture.absolute_filename` is NEVER read.** It is `C:\Users\bob\Desktop\wood.png` in every
  Autodesk export. `relative_filename`, falling back to the BASENAME of `filename`, and nothing else.
- **Materials come from `material->pbr` ONLY. `material->fbx` is never read.** There is no correct
  Phong -> metallic-roughness formula; every candidate disagrees with the DCC tool's own viewport, and
  the disagreement is invisible until someone compares renders. `use_blender_pbr_material = true` lets
  ufbx invert Blender's own deterministic PBR->Phong export, which is the most likely real input.
  `AlphaMode::Mask` and `TextureWrap::MirroredRepeat` are UNREACHABLE from this backend.
- **Animations are BAKED, never translated.** FBX animates Euler angles under a per-node rotation order
  composed with pre/post rotation and pivots; there is no key-for-key mapping onto quaternion TRS, and
  writing one is a re-implementation of FBX's transform chain. `ufbx_bake_anim` exists for exactly
  this. Every field of `ufbx_bake_opts` that affects output is PINNED EXPLICITLY, even where it equals
  ufbx's default, so a bump cannot move key counts or sample times. Every channel is `Linear`.
- **Skin weights arrive ALREADY SORTED by descending weight, and NOT normalized.** The four-largest
  take is a prefix; the explicit sort is kept for the tie-break and for a future ufbx. Renormalization
  is mandatory. A zero-total-weight vertex gets all-zero joints AND weights -- never `{1,0,0,0}`, which
  would bind it to joint 0.
- **`mesh->skin_deformers` is a LIST, not an optional -- a mesh CAN carry more than one Skin
  deformer.** Only the FIRST survives (`ImportedPrimitive` has exactly one set of four joints/weights,
  so a second is not representable at all), but that drop is NAMED: one warning per mesh giving the
  dropped count, unlike an earlier draft that dropped it silently. The Structure-depth shell pass
  MIRRORS phase 7's own per-mesh, first-only selection (`mesh->skin_deformers.data[0]`, walking
  `s.meshes`) rather than iterating `s.skin_deformers` (every deformer in the scene) -- iterating the
  scene-wide list is what let Structure's `skinCount` disagree with Full's whenever one mesh carried
  more than one deformer. Any future code resolving "the skin for this mesh" must use the identical
  selection, in both places, or the two depths diverge again.
- **NO FIFTH `ImportSettings` KEY, EVER, without making it OPTIONAL.** The four `importer.settings`
  keys are REQUIRED once the block is present, so a required fifth makes every sidecar written by an
  older build report `missing required key` and degrade to defaults. Axis and unit conversion is
  correctness, not preference: a toggle would let a person produce a model that is wrong and then file
  a bug about it. `.meta` STAYS AT VERSION 1.
- **`ImportedNode::localId` is the RAW ufbx `typed_id`, not the position in `ImportedModel::nodes`.**
  `ufbx_baked_node::typed_id` (the animation target) maps to `scene.nodes[]`, and resolving it without
  a side table is why. `parent`, `children` and `roots` all hold `localId`s. For glTF the two happen to
  coincide; for FBX they do not. **`ImportedSkin::joints` holds `localId`s too** (`fbx_import.cpp`
  pushes `cluster->bone_node->typed_id` verbatim) -- the identical rule, one field over.
- **BLOCKING, FOUND BY THIS TASK'S OWN GPU-TIER CASES: `import_details_panel.cpp`'s Hierarchy and
  Skins sections indexed `ImportedModel::nodes` DIRECTLY by `localId`, and that is a real ASan
  heap-buffer-overflow, not a cosmetic bug.** `drawHierarchy`/`drawNodeTree` (`model.roots`/
  `ImportedNode::children`) and `drawSkeletonAndAnimation`'s skin loop (`ImportedSkin::joints`) all
  shipped this way at task 3.2.1, and it was invisible for glTF because glTF's `localId` and its
  `ImportedModel::nodes` position always coincide (no root exclusion, see 3.2.1's own rule above). The
  FIRST FBX model with more than a single unparented node reads out of bounds the instant the Hierarchy
  section draws it -- `I64`'s own four-deep chain is what caught it, as a real ASan abort, not a wrong
  picture. Both draw functions now resolve `localId -> nodes[] position` through a
  `std::unordered_map<std::uint32_t, std::uint32_t>` built once per call, the identical shape
  `fbx_import.cpp`'s own `nodeIndexByLocalId` already uses on the import side. **Any future panel code
  that reads `ImportedNode::children`, `ImportedModel::roots`, `ImportedSkin::joints` or
  `ImportedAnimationChannel::targetNode` must resolve through a map like this one -- never index
  `ImportedModel::nodes` with the raw value.**
  **`skeleton_cook_source.cpp` (task 3.5.1) is the fourth named consumer of this rule and the first
  outside a panel**: it reads `ImportedSkin::joints` AND `ImportedNode::parent` for its ancestor
  closure, and every one of those resolutions goes through one `localId -> position` **sorted vector**
  built once per call (a sorted vector rather than a hash container, because the adapter's output
  order must not depend on an iteration order) -- never `nodes[localId]`.
  **`animation_cook_source.cpp` (task 3.5.2) is the FIFTH named consumer of this rule and the FIRST
  that must NOT convert.** It reads `ImportedAnimationChannel::targetNode` and writes it into the
  cooked clip **verbatim**, as `.aeroanim`'s `targetNodeLocalId`, because `.aeroskel`'s
  `sourceNodeLocalId` (`docs/09` section 12.3) is the same kind of value and the two must be
  comparable at bind time. Everything above still holds for anyone who **indexes**
  `ImportedModel::nodes`; this adapter indexes nothing, which is why the inversion is safe here and
  only here. Mapping the target through a `localId -> position` table would make every FBX clip bind
  to the wrong joints, silently, and `AS9` is the case that reddens if anyone does -- it is hand-built
  precisely because glTF, whose `localId` and position coincide, cannot see the difference.

  **`instantiate_plan.cpp` (task 3.1.5) is the SIXTH named consumer, and it is the one that has to
  hold BOTH numbers at once.** Its BFS reads `ImportedModel::roots` and `ImportedNode::children`, so
  every link crosses one sorted `localId -> position` map built once per call -- while
  `ImportedNode::meshIndex` is **already a position** (into `ImportedModel::meshes`, the same number
  `CookedSubmesh::sourceMeshIndex` records) and is stored **verbatim**. Resolving `meshIndex` through
  that map is seed `S6`: it works for glTF, where the two coincide, and picks the wrong mesh for FBX --
  the exact shape of the bug the map exists to prevent, wearing the map as a disguise. **The rule is
  "resolve links, never positions", and the two are one field apart.**
- **Every platform-dependent ufbx default is set EXPLICITLY.** `path_separator` above all: its default
  is `'\'` on Windows and `'/'` everywhere else, inside a function whose output three CI lanes must
  agree about byte for byte in tests comparing `relativePath` against literals.
- **`ufbx_load_opts` and `ufbx_bake_opts` MUST be value-initialised** (`= {}`). Both carry
  `_begin_zero`/`_end_zero` guards and ufbx returns `UFBX_ERROR_UNINITIALIZED_OPTIONS` otherwise.
- **The `ufbx_error_type` switch has NO `default:`.** A ufbx bump that adds an enumerator must be a
  `-Wswitch` failure on the Linux lane, not a silent "unknown".
- **The MAX_FBX_TEMP_BYTES/MAX_FBX_RESULT_BYTES/MAX_FBX_ALLOCATIONS caps are wired but their OWN
  "fires on a real document" half is UNPROVEN, and this is a documented, deliberate gap, not an
  oversight.** Unlike `MAX_FBX_NODE_DEPTH` (a "many minimal nested objects" shortcut, `FI27`) or
  `MAX_PRIMITIVES_PER_MODEL` (`FI38`, "many minimal meshes"), there is no cheap tier-0 shortcut for a
  1 GiB running total or 4 000 000 individual allocations: ufbx's temp/result allocators back EVERY
  allocation the parser makes for the WHOLE document. The error-to-status MAPPING is proven by
  construction (the shared `ufbxStatusFor` switch arm `FI27` already exercises for the sibling
  `NODE_DEPTH_LIMIT` enumerator); a real document tripping either byte cap is not, and the comment
  beside `FI27` in `fbx_import_test.cpp` says so by name rather than shipping a case that only looks
  like proof.
- **The importer identity is a per-format FUNCTION (`modelImporterIdentity`), never a hard-coded
  constant, and task 3.2.2 found it hard-coded in THREE places, not the two the spec named.**
  `asset_database.cpp`'s phase 7.5 probe and `import_details_panel.cpp`'s Overview line were the two
  the plan expected; `asset_meta.cpp`'s `writeMetaText` (which used to write `GLTF_IMPORTER_NAME`/
  `GLTF_IMPORTER_VERSION` unconditionally into every sidecar's `importer` block) was the third, found
  while wiring `ModelImportSession::applySettings()` for FBX. `writeMetaText` now takes the identity as
  two trailing, DEFAULTED parameters (`importerName`/`importerVersion`, defaulting to the glTF pair) --
  not an `ImporterIdentity`, because `asset_meta.hpp` deliberately never includes `model_import.hpp`
  (plan §A-11's boundary, unchanged). **Any future format-dispatching site that reads or writes an
  importer's name/version must go through `modelImporterIdentity`, never repeat the pair as a literal.**

## OBJ import (task 3.2.3)

- **`.mtl` IS A CLAIMED IMPORTABLE FILE, and that is the whole reason texture dependencies work for
  OBJ (D4).** Phase 7.5 probes at `Structure` depth with NO external bytes, so a probe of `chair.obj`
  can name `chair.mtl` and can NEVER name `wood.png`. Claiming `.mtl` turns one two-hop problem into two
  ordinary one-hop edges, and 3.1.2's cascade is transitive (`planImports` step 4 is a BFS over reverse
  edges that pushes on the clean->dirty transition only), so `wood.png` dirty => `chair.mtl` =>
  `chair.obj` falls out with no new mechanism, no second probe round and no extra read. `asset_view.cpp`
  is NOT touched: a `.mtl` stays `AssetKind::Unknown`, so it is invisible while the Asset Browser's kind
  filter is set to Model. That is accepted and documented, not a defect.
- **`modelImporterNeedsExternalBuffers` SPLITS BY EXTENSION WITHIN ONE IMPORTER, and this is the first
  place in the tree that happens.** `.obj` -> TRUE (its `.mtl` is a genuine external file the Full pass
  needs); `.mtl` -> FALSE (its whole content is local). Answering TRUE for `.mtl` would make
  `ModelImportSession` read every texture the library names, hand them to an arm that ignores every one,
  and -- once they exceed `MAX_EXTERNAL_BYTES_PER_MODEL` -- take the E21 branch and report `Truncated`
  for a result that was COMPLETE at `Structure` depth. Answering FALSE for `.obj` would import every
  model with zero materials, status `Ok`, and no warning at all. **Confirmed by direct sabotage: the
  `.obj`-false seed reddens `MS29` (materials silently vanish, no status change); the `.mtl`-true seed
  is structurally UNOBSERVABLE through `ImportResult` (`importMtlOnly` discards its `external` parameter
  unconditionally) and is caught only at the pure `modelImporterNeedsExternalBuffers` level, never at the
  session/behavioural level -- proven by reading the source, not merely by a seed reddening nothing.**
- **`.obj` at `Structure` depth is a PURE TEXT SCAN, and this is a STATED DEVIATION FROM INV-M4 (D5).**
  `scanObjMtlLibs` is the whole pass: tinyobjloader is not entered, no stream is constructed, no vertex
  is allocated. `Structure` and `Full` therefore DISAGREE about counts, names and the URI set for
  `.obj` -- Full additionally names every accepted texture. The `.mtl` arm keeps INV-M4 perfectly (D6):
  the depth parameter is accepted and deliberately ignored, and `AC-23`'s field-for-field equality is
  what stops a future "optimisation" splitting the two. **Measured, not assumed: at ~150 MB the scan
  alone costs ~107-113 ms warm, roughly 3x the read's own ~34-46 ms -- the SCAN dominates, not the read,
  because the per-line walk does real work on every line of an overwhelmingly `v`/`f` file. Steady-state
  stays ~0 (D15/INV-C5 unaffected); only a scan where the file actually changed pays this cost.**
- **tinyobjloader NEVER touches the filesystem. `ParseFromFile`, `MaterialFileReader`,
  `ObjReaderConfig::mtl_search_path` and the whole `ObjReader` class must appear NOWHERE in this tree.**
  A `.obj` is a user-supplied document and `mtllib /etc/passwd` is syntactically legal Wavefront. The
  only sanctioned entry points are the two `std::istream` overloads, `LoadObj` and `LoadMtl`, fed from
  bytes the editor already read. `ObjReader` is banned for a second reason too: its only from-memory
  entry point copies both inputs, up to ~768 MB resident at the 256 MiB file cap.
- **`TINYOBJLOADER_USE_MAPBOX_EARCUT` is a COMPILE ERROR, not a hope (D21).** The mapbox branch is the
  only unchecked position read in the library's triangulation, guarded by a bare `assert()` that vanishes
  under `NDEBUG` -- a Debug abort on the sanitiser lanes and a heap over-read in Release, both from an
  ordinary broken `.obj`. The `#error` sits BEFORE the library include. `TINYOBJLOADER_IMPLEMENTATION`
  must never be defined either: vcpkg compiles the library, and a second copy risks an ODR conflict.
  **Confirmed by sabotage that this guard has NO automated cover**: removing it reddens nothing in 297
  targeted cases -- AC-13's one-time manual check is deliberately its only cover, outside CI.
- **A vcpkg-prebuilt static library linked into an MSVC ASan lane is a Windows-only `LNK2038` waiting to
  happen, and `tinyobjloader.lib` was CI-caught hitting it -- the second library in this tree to do so,
  after `imguid.lib`/`imgui_stdlib.cpp.obj` (task 2.2.1, see the ImGui section's own entry above).** MSVC's
  STL stamps every object with an `annotate_string`/`annotate_vector` record matching its own ASan state,
  and the linker's `/FAILIFMISMATCH` makes a 0-vs-1 disagreement a hard link error, not a warning; vcpkg
  never builds a port with `/fsanitize=address`, so ANY of its prebuilt static libraries mismatches the
  instant it lands beside this project's own ASan-instrumented objects on the Windows Debug lane --
  `aero_ufbx`/`fastgltf` never hit this because the former compiles in-tree and the latter evidently emits
  no such records. Fixed project-wide in `cmake/sanitizers.cmake`'s `if(MSVC)` branch
  (`_DISABLE_STRING_ANNOTATION=1`/`_DISABLE_VECTOR_ANNOTATION=1`), trading away only the `[size(),
  capacity())` slice of container-overflow detection -- every out-of-allocation heap overflow, including
  this task's own INV-O4 checks, is unaffected on all three lanes. The next vcpkg static library added to
  an ASan-lane target inherits this fix automatically; a library with NO vcpkg port at all (this tree's
  actual bar for vendoring, per ufbx) can instead be compiled in-tree and keep full instrumentation.
- **EVERY INDEX IS RANGE-CHECKED BY US BEFORE ANY ARRAY ACCESS, and the library's own checks do not make
  that redundant (INV-O4).** `fixIndex` (`tiny_obj_loader.h:819`) maps a positive Wavefront index to
  `idx - 1` with NO upper bound; an out-of-range index produces only a WARNING
  (`:3109`, "Vertex indices out of bounds") and `LoadObj` still returns `true`. The built-in ear clipping
  bounds-checks its reads, but one branch SUBSTITUTES ZEROS rather than skipping, so a triangle carrying
  a bad index is still emitted. A face failing any of the three checks is dropped WHOLE, never partially,
  with one capped warning. **Confirmed load-bearing by REAL ASan reports, not merely failed `CHECK`s**:
  disabling any one of the three checks in turn produces a genuine `AddressSanitizer: BUS on unknown
  address` abort against the vertex/normal/texcoord arrays respectively -- the single most important
  proof in this task.
- **The library has TWO GRADES of malformed index, and the asymmetry is upstream.** `f 99999 1 2`
  (past the end) warns and returns `true` -> `Ok`, that face dropped by us. `f 0 1 2` (a zero index) and
  `f -99 1 2` (a relative index below zero) make `LoadObj` `return false` -> `ParseFailed` for the WHOLE
  file. Do not "correct" that into a uniform rule; delivering one needs our own parser.
- **A missing, unreadable or refused `.mtl` is a WARNING, never `MissingBuffer` (D7).** glTF's external
  `.bin` holds the geometry; Wavefront's `.mtl` holds only appearance, so without it there is still a
  mesh. The Full pass proceeds with an empty material stream, every face gets
  `materialIndex = INVALID_SUBASSET`, one warning per `mtllib` line whose candidates were all unsupplied
  names the raw operand, and status stays `Ok`. The warning is OURS, derived from the candidate list
  versus the supplied buffers -- never inferred from a library string.
- **TWO library sentences are dropped, and only two -- reachable at THREE `mtllib` directives, not two,
  a code-review-round correction of this section's own earlier claim.** `"Material stream in error
  state."` (`:2536`, `MaterialStreamReader`'s own guard) and `"Failed to load material file(s). Use
  default material."` (`:2926`/`:3344`, `LoadObj`'s mtllib branch) both describe OUR single-stream
  plumbing -- but `MaterialStreamReader`'s own `if (!m_inStream)` guard tests `std::istream::fail()`
  (failbit/badbit), NEVER `eof()`, so the SECOND `mtllib` directive's `readMatFn` call does not take that
  branch: it silently re-enters `LoadMtl` on an already-exhausted stream, which parses nothing but still
  flushes its own phantom. This section and the engineering log both used to claim, on the strength of a
  two-`mtllib` fixture, that both sentences were "PROVEN unreachable for this library version" -- true for
  two lines, false for three. Probe-confirmed directly against the real vendored header: it is the THIRD
  `mtllib` directive that flips `fail()` true, and the library's raw `warn` string then reads verbatim
  `"Material stream in error state. \nFailed to load material file(s). Use default material.\n"`. `OI23`
  now uses three directives and is confirmed, by direct sabotage, to redden when either filter clause is
  dropped -- both sentences are genuinely reachable and the filter is genuinely load-bearing, not merely
  defence in depth. The library's `"Both \`d\` and \`Tr\` parameters defined"` warning (`:2237`/`:2249`)
  is the OPPOSITE -- a real statement about the file -- and is NEVER dropped; confirmed load-bearing by
  sabotage (`OI62` reddens when it is).
- **`LoadObj`'s RETURN VALUE is the authority, never the `err` string -- and `LoadMtl`'s `err` parameter
  is PROVEN dead code for this library version, not merely unobserved.** Both `LoadMtl` overloads
  (`:2069`, `:2532`) open with `(void)err;` and never touch it again anywhere in the body, so `err_mtl`
  (`LoadObj`'s own internal call, `:2905-2913`/`:3322-3331`) can never become non-empty for ANY input.
  Mapping "non-empty `err` -> `ParseFailed`" was sabotage-seeded directly and reddened nothing, confirming
  the mapping is unreachable rather than merely untested. A `true` return with a non-empty `err` (from
  some OTHER source, should one ever exist) appends each line as a WARNING, never a failure.
- **THE ZERO-FACTOR RULE, and it exists because the library cannot tell you (D14).** `InitMaterial`
  (`:1386`) zeroes `diffuse`, `roughness` and `metallic`, and there is NO "was it set" flag for any of
  them. Read verbatim, every classic `.mtl` imports as a black perfect mirror and `newmtl wood / map_Kd
  wood.png` imports as an invisible texture. So: where a zero factor would ANNIHILATE a texture the same
  material supplies, it is read as the neutral 1; and `roughness` additionally takes 1 when there is no
  map at all -- UNCONDITIONALLY, the one clause with no "has a texture" gate. Nothing else is adjusted --
  `Kd 0 0 0` with NO texture stays black, which is a legitimately black material. **Each of the four
  clauses is confirmed independently load-bearing by sabotage**: dropping the roughness clause turns
  every mirrorless material into one; applying the `Kd` clause unconditionally turns a legitimately
  black, textureless material white.
- **`d` ALWAYS beats `Tr`, in either order -- verified at the literal source (`:2229-2260`), where `Tr`'s
  own handler comment reads "`d` wins. Ignore `Tr` value."** The library warns from BOTH handlers when
  both appear. No `Ns` -> roughness curve, ever: there is no standard behind any of the candidate
  formulas, and the panel prints `roughnessFactor` as a number. `map_Ka` is NOT mapped to occlusion --
  `Ka` is an ambient COLOUR map, and mapping it would be a guess the panel prints as a fact.
- **Fold `\` -> `/` BEFORE `classifyUri`, never after** -- `foldBackslashesToSlashes`, the function
  3.2.2 added for FBX, reused verbatim (D15). A `mtllib`/`map_Kd` operand is a FILESYSTEM PATH, never a
  URI, so `UriClass::RefusedBackslash` is right for glTF and wrong here. `classifyUri` is NOT modified;
  the class simply becomes unreachable from this importer. **The stated, accepted cost: a backslash
  cannot be both a separator and an escape, so `mtllib my\ file.mtl` is NOT supported.** An ambiguous
  operand is instead offered BOTH ways -- the whole trimmed operand first, then each token -- and the
  filesystem decides. This function's own doc comment now names BOTH backends and BOTH reasons, since
  "CALLED BY THE FBX BACKEND ONLY" became false the moment this task's own `obj_import.cpp` called it.
- **The importer CONVERTS NOTHING (D12/INV-O6):** no axis flip, no winding reversal, no unit scaling, no
  handedness change, and no setting for one. Wavefront agrees with glTF on everything that matters. FBX
  is the format that needs conversion, precisely because it declares Z-up and centimetres.
- **`ImportedNode::localId == its position in `nodes` == `meshIndex` for OBJ (D11)**, because there are
  exactly N root nodes, one per mesh, in source order, with no synthetic parent. That means 3.2.2's
  BLOCKING `nodes[localId]` heap-overflow cannot resurrect here -- but only BY CONSTRUCTION. The panel's
  `indexByLocalId` map still runs and still must; a case pins `nodes[i].localId == i` so a future change
  that breaks the coincidence is caught rather than discovered.
- **An empty mesh's `bounds` is `Aabb{}` -- a POINT BOX -- never the `Aabb::empty()` sentinel; and
  `summary.bounds` is folded from SURVIVING PRIMITIVES, never from mesh bounds.** OBJ is the first format
  in this tree that produces empty meshes routinely (an `l`/`p`-only shape, or one whose faces were all
  dropped by INV-O4), so this pairing became reachable for the first time here. Both shipped backends
  already do exactly this and their comments say why: a mesh left at the sentinel has a NaN centre, and
  folding the model bounds from mesh bounds would leak the world origin into it for every empty mesh.
  **Confirmed as two genuinely independent properties by sabotage: one seed reddens only the mesh-bounds
  half, the other only `summary.bounds`, never both at once.**
- **`.obj` is NEVER routed through ufbx, permanently.** ufbx v0.23.0 genuinely parses `.obj`
  (`UFBX_FILE_FORMAT_OBJ`) and will even open a sibling `.mtl` FROM DISK via `obj_mtl_path`/
  `obj_mtl_data`, which its own header notes "sidestep `load_external_files`". That is a filesystem read
  behind a user-supplied document, which is exactly what D3 forbids. 3.2.2 already banned those two
  fields by name; this entry is the answer to the temptation, written down.
- **No `.obj`/`.mtl` WRITING, ever.** This importer is read-only; the only write anywhere in the model
  import subsystem stays `ModelImportSession::applySettings`'s `.meta` sidecar (INV-M9).
- **TWO build-time discoveries about `LoadMtl` share ONE observable signature (an empty `name`) and are
  closed with ONE mechanism, not two special cases.** First: `LoadMtl` unconditionally flushes a
  trailing "material" at end-of-parse (`:2459`'s own comment, "// flush last material.") with NO guard
  on whether any `newmtl` was ever seen -- a completely empty `.mtl` stream produces `materials.size()
  == 1`, not 0. Second: because `MaterialStreamReader`'s guard tests `fail()` not `eof()`, a SECOND
  `mtllib` line's `readMatFn` call does not take the "stream in error" branch at all -- it silently
  re-enters `LoadMtl` on an already-exhausted stream, which flushes ITS OWN second, empty-named phantom.
  `declaredWithEmptyName(combinedWarnings)` -- checking for the library's OWN "empty material name in
  `newmtl`" warning -- is the ONE signal that tells a genuine, if malformed, user-authored empty-named
  `newmtl` apart from either phantom shape; `convertMaterials` filters on it with a separate `nextLocalId`
  counter so a dropped phantom never consumes a local id. **Any future change to material parsing must
  route through this same filter, never assume `LoadMtl`'s `materials` vector holds only real
  declarations.**



## Blender CLI (task 3.2.4)

- **Never parse `.blend`.** ADR-003 and the GPL boundary (`docs/01-tech-stack.md`). Blender is an
  external PROCESS: no header, no library, no vcpkg entry, ever. A contributor reaching for a
  `.blend`-reading library is repealing an ADR, not optimising. The only read of a `.blend`'s bytes
  anywhere is 3.1.2's opaque `hashFileContents`, which treats every file identically as a byte stream.
- **`isBlendFileName` is NOT `isImportableModelName`, and `.blend` must never join the latter's table.**
  Phase 7.5 gates its probe on that predicate; adding `.blend` there makes the SCAN feed raw `.blend`
  bytes to `importModel` and every `.blend` in the project reports an import failure on every scan.
  Sabotage-confirmed to be worse than that in practice: the seeded six-entry table did not merely fail
  cases, it **aborted the test binary** (`SIGABRT` in `MI28`). `MI133` pins both halves — including
  `modelImporterNeedsExternalBuffers("x.blend") == false`, a SECOND fact that breaks independently
  (proven by seed S35, which reddens only that half while S22 reddens both).
- **The `<guid>.glb` artifact bypasses `modelImporterNeedsExternalBuffers` on purpose.** ONE
  `importModel` call, `Full` depth, empty external span, empty `assetRelativeDir`. It lives in
  `Library/`, so it has no assets-relative directory and any URI it named would resolve against an
  unrelated tree. If it names any, ONE warning is appended and the list is cleared. **The path and the
  observable need SEPARATE proofs**: `MS41` reads `serviceBlend`'s own source text (one `importModel`,
  one `readFileBytes`, zero gate consults, one empty `assetRelativeDir`) and `BS41` asserts the warning
  against a GLB fixture that genuinely declares an external URI. Measured: routing the artifact through
  the two-pass driver reddens `MS41`/`MS22` and **not** `BS41` — an ordinary two-pass route produces an
  identical `ImportResult` — so neither proof substitutes for the other.
- **`SDL_WaitProcess` is only ever called with `block == false`, and no pipe is ever created.** SDL's
  own header documents the deadlock. `SDL_ReadProcess`/`SDL_GetProcessOutput`/`SDL_GetProcessInput`
  appear nowhere in this tree and must not start. Sabotage-confirmed: a blocking wait does not fail the
  suite, it **hangs** it past every budget — the comment-stripped gate grep is the real cover.
- **`BlenderService::poll()` has exactly ONE call site in the tree, inside
  `ModelImportSession::serviceBlend()`, and `EditorApp::tick()` gains no fourth post-draw call.** The
  service is reached through the session that owns it. `I74` re-asserts `I60`'s ordering proof against
  this task's own edits; `I75` is the only mechanical cover for "the panel never polls", because no
  runtime tier in this tree can see that violation.
- **`currentHostOs()` is the only per-OS branch in first-party editor code**, and it is exactly three
  lines, in the `#elif defined(__linux__)` + `#error` form. The bare-`#else` form produces TWO lines and
  silently falls back to Linux on an unknown host. Do not cite the three platform macros in any comment
  under `editor/src` or `editor/include`: the gate grep does not strip comments, unlike the platform and
  rhi guards. Vendored sources under `editor/third_party/` are out of scope, exactly as the clang-format
  glob already excludes them.
- **The `.blend` arm sits BEFORE `service()`'s `isImportableModelName` early return**, and the
  `(target, generation)` consume guard has exactly ONE exception: a `.blend` target whose session is
  `Converting` or whose service is `Probing`/`Converting`. It is narrow on both axes on purpose — a
  `.gltf` selected while some probe is alive must not be re-imported every tick, and a settled `.blend`
  must still cost ten early returns over ten ticks. **A case proving that second half needs a cache
  HIT**: on a cache MISS nothing is imported either way, so a widened guard reddens nothing (measured —
  an earlier `MS36` was exactly that vacuous and was rewritten).
- **A nil-GUID `.blend` is `NeedsConversion` with a DISABLED button, never `NotImportable`.** That
  enumerator's panel branch renders one sentence and returns before any section, so it would draw no
  button to disable — and after this task it would be telling the user something false.
- **The provenance record and the run's status document SHARE the path `<guid>.json`, deliberately.**
  On success the record overwrites the status; a half-finished run leaves a document
  `parseExportProvenance` rejects, which is exactly the "no valid cache entry" answer. A test asserting
  "no provenance was written" must assert what the document IS, not that the path is empty.
- **`blender_tool.cpp`, `blender_process.cpp` and `blender_service.cpp` never log, and none of them is
  in either of `check-project-no-delete.sh`'s lists** — being outside both is exactly what makes a
  future `std::filesystem::remove` there a hard CI failure (sabotage-confirmed: the guard fires before
  any test binary runs). All three are also `<thread>`/`<mutex>`/`<atomic>`/`<future>`/
  `<condition_variable>`-free: the OS runs the child concurrently and one non-blocking wait per tick is
  the whole mechanism.
- **Any test that can reach the Blender resolve path MUST set `EditorAppConfig::toolPrefsPath`**, or it
  reads — and through `Locate…`/`Re-detect`, WRITES — the developer's real machine-wide
  `editor_tools.json`. This is the third instance of the `recentProjectsPath`/`layoutIniPath` lesson.
- **A RE-ENTRY THROUGH THAT EXCEPTION POLLS, AND DOES NOTHING ELSE.** The exception exists so a child
  gets waited on, not so the cache is re-evaluated: falling through to the probe re-read the artifact and
  re-ran `importModel` on every frame for the whole duration of an unrelated asset's run, and wiped
  `resultValue` on the way in. Nothing below the poll can change a settled target's answer anyway —
  `setTarget`, `requestConversion` and `cancelConversion` all clear `serviced`, so none of them arrives
  as a re-entry. `BS35` is the case, and it asserts `importCount()`, which reads 39 instead of 1 without
  the guard.
- **`SessionState` and `BlenderState` are the first values in the editor that span frames, and all three
  of the code-review round's worst findings are the same mistake: a per-tick OUTPUT read as the next
  tick's INPUT with nothing resetting it.** `setTarget()` must reset `stateValue` with everything else it
  already resets — a stale `Converting` made the panel report a run against an asset that had none, and
  made the session consume another asset's result. `serviceBlend` additionally requires
  `blender().conversionGuid()` to match its own target before consuming `Converting`/`Converted`. **Any
  new cross-frame field on either type inherits both obligations.**
- **The panel names the version from the ARTIFACT'S OWN provenance record
  (`ModelImportSession::artifactBlenderVersion()`), never `BlenderService::versionString()`.** The latter
  is the currently installed Blender: empty on a pure cache hit, and — once anything has probed — a
  binary that did not produce the file on screen. "Convert with 4.2, upgrade to 5.2, and the panel still
  says 4.2" is only true if this distinction is kept.
- **EVERY child this service starts is bounded, not just the export.** A hung `blender --version` left
  the service in `Probing` for the life of the editor, which also re-entered the `.blend` arm every tick.
  `BLENDER_PROBE_TIMEOUT_SECONDS` and `BLENDER_TIMEOUT_SECONDS` are three orders of magnitude apart on
  purpose. **A bounded WAIT loop in a test must inject ZERO seconds** (`WAIT_DT`): forking a fake tool
  costs thousands of poll iterations, so a plausible-looking 16 ms per iteration is minutes of fake time
  and trips a real timeout while the child takes milliseconds. Every timeout is driven by an explicit
  one-shot injection instead.
- **`cancel()` is `noexcept`, so it RECORDS and `poll()` completes** — the same division of labour
  `requestConversion`/`startExport` already has. A cancel arriving between the request and the spawn must
  still produce a verdict, or the session waits forever on a run that will never start; and assigning the
  message inside `cancel()` is an allocation clang-tidy rejects outright.
- **The panel's Blender log node is DEFAULT-OPEN, and that is what makes its contents testable at all.**
  No tier in this tree can click a `TreeNode`, so a closed node's branches never execute anywhere, under
  any sanitizer — which is precisely how the refused-by-cap branch shipped undriven. The same reasoning
  the six `CollapsingHeader` sections already carry.
- **`<projectRoot>/Library/BlenderExports` has ONE rule, `blenderExportDir()`, and it returns EMPTY for
  an empty project root.** Concatenating onto an empty root yields `/Library/BlenderExports` — an
  absolute path at the filesystem root that the probe's own directory creation then attempts, harmlessly
  on POSIX and for real on Windows. An empty result is the service's own documented "resolve, but do not
  spawn"; `EditorApp::resolveBlender()` defers on it, leaving the state `Unknown`, which is exactly what
  the lazy resolve re-tests once a project is open.

## Assimp import (task 3.2.5)

- **`ReadFileFromMemory` is NOT enough, and `SetIOHandler(nullptr)` is a TRAP.** The from-memory entry
  point *wraps* the installed IO handler in a `MemoryIOSystem` that serves one magic filename from the
  buffer and **delegates every other path to the wrapped handler** — which out of the box is a live,
  unrestricted `DefaultIOSystem`. And `SetIOHandler(nullptr)` does not clear the handler, it **installs**
  a fresh `DefaultIOSystem`. The only sanctioned sequence in this tree is
  `SetIOHandler(new RefusingIoSystem())` **then** `ReadFileFromMemory`. `Importer::ReadFile`,
  `DefaultIOSystem`, the whole C API, `Assimp::Exporter`, `ZipArchiveIOSystem` and `.zae` must appear
  nowhere in this tree, permanently. **All twelve `IOSystem` virtuals are overridden** — four are pure and
  the compiler forces them, the other eight are not, and an un-overridden one silently inherits a base
  that keeps a directory stack and compares real paths.
- **The loader that ran is ASSERTED, never assumed** — `AI_METADATA_SOURCE_FORMAT`, compared against a
  distinctive fragment of the expected loader's own `aiImporterDesc::mName`. `UnregisterLoader` is
  rejected because it leaks. **Assert `Get(AI_METADATA_SOURCE_FORMAT,`, never the bare key**: the bare
  token is a PREFIX of `AI_METADATA_SOURCE_FORMAT_VERSION`, which this same file reads for a display
  string, and a gate spelled the short way stays green when the whole assertion is deleted (sabotage
  seed S4 found exactly that).
- **The forbidden post-process flags**, with `aiProcess_FindInvalidData` called out by name: *it deletes
  meshes, and if it deletes them all it fails the whole file*, which collides head-on with 3.2.1's D11
  (an empty mesh survives so the panel can show the user why their file looks empty). Its one genuine
  benefit — nulling an all-zero normal/tangent/UV channel — is taken over by this backend's own check,
  which keeps the mesh, keeps the counts and names the channel in a warning. `PreTransformVertices`,
  `MakeLeftHanded`, `ConvertToLeftHanded`, `FlipUVs`, `FlipWindingOrder`, `GlobalScale`, `GenNormals`,
  `GenSmoothNormals`, `CalcTangentSpace`, `JoinIdenticalVertices`, `RemoveRedundantMaterials`,
  `GenBoundingBoxes` and `FindDegenerates` are forbidden for the reasons `assimp_import.cpp`'s own header
  lists.
- **`aiMatrix4x4` is ROW-major and `aiQuaternion` is `{w,x,y,z}`.** `engine::Mat4` is column-major and
  `engine::Quat` is `{x,y,z,w}`, so every matrix conversion **transposes** and every quaternion conversion
  is a **named field copy** — never a `memcpy`, never a `bit_cast`, never a positional construction. There
  are **three** call sites (`convertNodes`, `convertSkins`, `convertAnimations`) and they break
  independently: sabotage confirmed that one reordered `toQuat` reddens cases in two of them and one
  un-transposed `toMat4` reddens only the third. Both defects produce a plausible **wrong model**, not a
  failure.
- **`metallic 0` / `roughness 1` are this importer's DEFAULTS, not claims about the file**, and they must
  be written EXPLICITLY because `ImportedMaterial`'s own default for `metallicFactor` is 1 — leaving it
  ships every DAE/PLY/STL material as a full metal. No `SHININESS` → roughness curve, ever. No
  `LIGHTMAP` → occlusion: `aiTextureType_LIGHTMAP` is where the Collada loader puts `<ambient>`, and an
  ambient colour map is not an occlusion map.
- **Collada CANNOT express a black diffuse COLOUR together with a diffuse TEXTURE.** `<diffuse>` holds a
  `<color>` or a `<texture>`, and `ColladaLoader` writes `AI_MATKEY_COLOR_DIFFUSE` unconditionally from
  `Effect::mDiffuse`, whose default is `(0.6, 0.6, 0.6)`. The zero-factor rule's paired arms are therefore
  driven from a `.ply`, whose `element material` supplies the colour independently of the `TextureFile`
  comment. Do not re-write that case against a `.dae`; it cannot be written.
- **`.dae` bounds are in the SOURCE's own units** while the hierarchy carries the unit/axis factor,
  because Assimp's Collada loader bakes the conversion into the root node's transform and exposes neither
  input. **This is not to be "fixed" by re-scaling geometry behind the user's back.** It is the named
  asymmetry with the FBX path, whose `MODIFY_GEOMETRY` choice was made for exactly the opposite reason.
- **`thumbnail_store.cpp` defines `STB_IMAGE_STATIC` and must keep defining it.** Removing it re-opens a
  duplicate-symbol collision with the second, unprefixed `stb_image` implementation assimp's vcpkg port
  compiles (its `build_fixes.patch` turns upstream's `assimp_stbi_*` prefixing off) — a static-link
  concern on macOS and Linux, structurally absent on Windows where the port builds a DLL.
- **The library LEAKS on its own error paths, and `tests/lsan.supp` carries exactly two frame-scoped
  entries for it.** `ColladaParser`'s CONSTRUCTOR parses the whole document from its ctor body, so a
  rejected document throws before the object exists and its destructor never runs — every node, mesh,
  submesh, input channel and transform already allocated is orphaned; `STLImporter::LoadASCIIFile` does
  the same on a truncated file. This backend's failure-mode fixtures are the first things in the tree to
  reach those paths at all, so the frames surfaced here (CI run 31559183254, 4704 B / 17 allocations)
  and only on the Linux Debug lane, the one lane that runs LSan — invisible to every macOS run, local or
  CI, and to Windows. **A future format added to this backend may surface further such frames. Each
  needs its OWN entry, gated on observed CI evidence and naming its own function — never a module-wide
  `leak:assimp`**, which would blind LSan to a first-party leak allocated anywhere inside the library
  (the same reason that file refuses `leak:SDL`). The two entries create no first-party blind spot
  because nothing of ours allocates inside either frame: the only code of ours reachable from there is
  `RefusingIoSystem`'s twelve virtuals, none of which allocates. Do not weaken the triggering cases —
  the leak is the library's, and the port is never patched locally.
- **Never write the character sequence `assimp/` in prose** anywhere under `editor/`, `tests/`, `engine/`,
  `runtime/` or `tools/`: the include-boundary gate greps those roots and a prose mention turns it red for
  a reason that is not a violation. Write "the assimp port", "assimp 6.0.4", "lib/cmake/assimp".
- **`.stl` and `.ply` do NOT enter Assimp at Structure depth**, which is a stated deviation from INV-M4 —
  the two depths disagree about counts, names and hierarchy, exactly as `.obj` has since 3.2.3. The one
  thing they owe each other is an identical `externalUris`, and **the `.ply` Full pass must seed it from
  the same header scan the Structure pass uses**. With a single `TextureFile` line the loader's own
  material happens to reproduce the scan's answer, so deleting the seeding step is invisible until a
  header names TWO different textures — Assimp keeps only the LAST, the scan returns both.
- **`.ply` needs a first-party pre-allocation bound and `.stl` does not**, and that asymmetry is a property
  of the two LOADERS, not of the two formats. `STLLoader.cpp` refuses at
  `mFileSize < 84ull + mNumFaces * 50ull` in 64-bit arithmetic **before** its own
  `new aiVector3D[mNumFaces * 3]`; the `Ply` sources contain no comparison against the file size anywhere.
  A `.ply` header declaring 2³²−1 vertices over an empty body sends the loader into a multi-minute grind at
  unbounded, climbing resident memory, reachable from a 120-byte file. `plyDeclaredCountsExceedBytes` runs
  **after** `seedPlyExternalUris` (so the depth-equality still holds on a refused file) and **before**
  `runAssimp`. Do not add an `.stl` equivalent: it would be dead code duplicating a guarantee the loader
  already makes.
- **`aiProcess_ValidateDataStructure` runs FIRST**, before `ScenePreprocessor` and before every other
  post-process step, and it **throws**. Its message begins `Validation failed:`, which is what maps a
  refusal to `ImportStatus::Malformed` rather than `ParseFailed`. Two consequences worth knowing before
  writing a case: an out-of-range face index or bone-weight vertex id is refused whole, so this backend's
  own range checks are **unreachable while that flag is on** (they stay as defence in depth for a
  validation-off build and are pinned in the source text, not by a runtime case); and an out-of-range `<p>`
  index in a `.dae` never reaches validation at all — `ColladaParser` rejects it first with
  `Invalid data index (n/m) in primitive specification`, which is a PARSE failure. The input that does
  reach the validation arm is two identically-named joints.
- **`aiProcess_SortByPType` + `AI_CONFIG_PP_SBP_REMOVE` DELETES a mesh** whose only primitive type is
  removed, rewrites the node graph around it, and throws `No meshes remaining` when nothing is left. A
  lines-only `.dae` is therefore refused outright and a mixed one keeps exactly the triangle mesh — there
  is no surviving empty mesh to observe, and the "no triangles survived" arm is unreachable for the same
  family of reasons as the range checks.
- **A NODE-ID MAP IS BUILT BY THE WALK THAT ASSIGNS THE IDS, NEVER BY A SECOND WALK OF THE SAME TREE —
  3.2.2's `localId` lesson in a new costume, and this task's BLOCKING finding.** `convertNodes` returns the
  `aiNode*` -> localId map `convertSkins` resolves `aiBone::mNode`/`mArmature` through; it used to be
  re-derived by a second, plain pre-order walk numbering `0,1,2,…`, one index per `aiNode`. The two diverge
  in two places, **both ordinary content**: a MULTI-MESH node consumes `mNumMeshes` slots (its own plus the
  synthesized `<name>.<n>` children), where a plain walk consumes one — and
  `ColladaLoader::BuildMeshesForNode` pushes one mesh ref per submesh **per material**, so a node whose
  geometry carries two materials is one — and a DEPTH-DROPPED subtree is never emitted while an unbounded
  walk descends straight into it. **A count bound truncates the tail; it realigns nothing.** The failure is
  silent: joints bind to the wrong nodes, `skeletonRoot` lands on a synthesized split child, status stays
  `Ok`, no warning. **The split children are deliberately NOT in the map and cannot be** — they have no
  `aiNode`, so no key — and nothing is lost, because every key the map is asked for is a pointer the
  LIBRARY produced and can never name a node this file invented. Any future pass needing "the localId of
  this `aiNode`" takes the map as a parameter; it does not rebuild one.
- **Every cap site LATCHES its report with its own bool, and a `break` inside a nested loop is a cap site
  too.** The multi-mesh split loop's `break` claimed "the shared cap message above already fired or will",
  which is false on the LAST node processed: the main loop's check runs only when another node is POPPED,
  so a childless multi-mesh node hitting the cap with an empty stack reported `Ok` with nodes dropped
  (AC-53). `convertSkins`' two `escalate` calls had no latch at all, so N over-cap skins appended N copies
  of one sentence. **Two cap sites with different CAUSES get different WORDING**, even when the constant is
  the same one: `convertMeshes`' model-wide vertex total and `convertSkins`' single-mesh influence table
  both trip `MAX_VERTICES_PER_MODEL`, and one shared sentence leaves the message unable to say which fired.
- **`aiNode::mMeshes[i]` is range-checked like `face.mIndices[k]` and `aiVertexWeight::mVertexId`, and for
  the same reason.** All three are unreachable while `aiProcess_ValidateDataStructure` is on and all three
  are pinned in `AI34`'s comment-stripped source text rather than by a runtime case. The `mMeshes` one
  matters most downstream: 3.1.5 resolves `ImportedNode::meshIndex` into `ImportedModel::meshes`, where an
  unchecked value is an out-of-bounds READ rather than a wrong picture.
- **A refused image is FOUND-OR-APPENDED on its RAW uri, not on `relativePath`** — a refusal has no
  resolved path, so the accepted branch's dedup can never match one, and two materials naming the same bad
  path would append two identical `ImportedImage`s and spend two identical warnings out of the
  `MAX_IMPORT_WARNINGS` budget.
- **`Assimp::SkipSpaces` is `' '` OR `'\t'`, and `scanPlyTextureFiles` must mirror it exactly.**
  `PLY::Element::ParseElement` opens with `PLY::DOM::SkipSpaces`, which forwards to it, so a TAB-indented
  `comment TextureFile wood.png` reaches the loader. A scan skipping only `' '` made Structure return `{}`
  where Full returned `{wood.png}` — the depth disagreement AC-19 forbids — and phase 7.5's Structure-depth
  probe recorded **no dependency**, so editing the texture never marked the model `DependencyChanged`.
  `plyDeclaredCountsExceedBytes` carries the identical rule; it fails safe there (it under-counts, never
  rejects), and the two must still never diverge.
- **A node instancing the SAME geometry twice is not a multi-mesh node** — the loader caches by
  `(geometry, submesh, material)`, so the two refs collapse onto one index, which `ValidateDataStructure`
  then refuses outright (`aiNode::mMeshes[i] is already referenced by this node`). A multi-mesh fixture
  needs two DISTINCT geometries, or one geometry with two materials.
- **`ArmaturePopulate::GetArmatureRoot` walks UP from a bone until it finds a node that is not one**, so
  `skeletonRoot` lands on the wrapper node. A fixture whose joints hang directly off the visual scene gets
  `skeletonRoot == 0` under every numbering and cannot discriminate a shifted map — wrap the joints in an
  explicit `Armature` node when that is the property under test.
- **A `.dae` fixture in a doctest macro must be hoisted into a named local first** — MSVC's legacy
  preprocessor breaks on a raw string literal containing `\"` passed directly as a macro argument, and XML
  is nothing but escaped quotes.
- **A reduced-configuration probe must be configured with `-G Ninja`.** `CMAKE_GENERATOR` enters the
  shadercross bootstrap's option hash, so a Makefiles configure reads the cached toolchain as COLD and pays
  a twenty-minute from-source DXC rebuild that has nothing to do with what the probe tests.

## Materials (task 3.4.2)

- **`"Material"` is FROZEN** — the panel id is both the ImGui window name and the `imgui.ini` settings
  key, so renaming it orphans every saved layout. `imgui_layer_test.cpp`'s `frozenPanelIds` array is the
  pin, and it now covers all eight ids rather than the first six.
- **The target is STICKY: only a DIFFERENT, EXISTING `.aeromat` retargets the panel.** Selecting a
  texture, a folder or nothing leaves it alone. Import Details keeps retargeting on everything; the two
  are **tabs, not shared state**. Invert this and every click made while hunting for a texture to
  reference destroys an unapplied edit session — the browser has exactly one selection. The target
  clears only when its record disappears from the database, or on a project swap.
- **`.aeromat` bytes are written by exactly two logical operations — Apply and New Material — through
  ONE helper**, `saveMaterialFile` in `material_session.cpp`, whose body holds the single
  `writeTextFileAtomic` call site. **The amended INV-A1**: the assets-root call-site count is now
  **two** (`asset_database.cpp`'s `metaAbsolutePath`, `material_session.cpp`'s helper) and the
  library-directory count is still two, with every absolute path assembled into a **named local**
  (`materialAbsolutePath`) so the invariant stays a grep rather than a heuristic. No scan, rescan,
  watcher, selection or draw path writes one, ever.
- **Dirty is `sessionCopy != fileCopy`** through `MaterialDocument`'s defaulted `==` — never "would
  Apply change the bytes". A valid but non-canonical file therefore loads **clean** and the editor never
  rewrites a file nobody edited. Apply validates first, writes **only when dirty**, and a validation
  failure changes nothing anywhere.
- **New Material refuses a directory it could not enumerate IN FULL.** `listDirectory` signals
  incompleteness three ways — `status`, `truncated`, `skipped` — and a truncated or partially-skipped
  listing still carries `ScanStatus::Ok` while handing back a **prefix**. Ask `listingIsComplete`, never
  `status == Ok`: a prefix cannot prove a name is unused, and acting on one renames the default document
  over an authored material with no warning and no undo. A caller that merely **displays** a listing is
  still right to ignore all of it.
- **`AssetRecord::contentHash` is meaningless unless the scan hashed the file this pass.** Ask
  `assetContentHashUsable`, never "is the digest zero?" — an all-zero digest is the **empty file's real
  value**, not a sentinel, and `metaWriteFailed` records are never assigned a `change` at all, so they
  read as `UpToDate` to any test on `change` alone. An unhashed record gets no cache key and no
  external-change notice.
- **Every preview GPU create and destroy lives in `servicePreview`**, never in `onDraw` — with **one
  deliberate exception, which is the whole rule below**.
- **THE RESIZE-BEFORE-READ ORDERING RULE — any future panel embedding a `RenderTarget` inherits this
  trap.** ImGui **records** an `ImTextureID` during the draw walk and **binds** it inside
  `ImGuiLayer::endFrame`, which runs **after** `tick()`'s post-draw service pass. `RenderTarget::allocate`
  destroys the previous pair first, and `RenderTarget`'s own comment — "the backend defers the actual GPU
  release" — is true of the device **memory** and **false of the handle**: in the pinned SDL 3.4.12 tree,
  Vulkan (`SDL_gpu_vulkan.c:7070-7073`) and D3D12 (`SDL_gpu_d3d12.c:1385, :1460`) `SDL_free` the container
  **immediately** ("Containers are just client handles, so we can destroy immediately") while Metal
  (`SDL_gpu_metal.m:936-944`) queues it. So a reallocation between the record and the bind is a **heap
  use-after-free on Vulkan and D3D12 and benign on Metal** — deterministic on the two platforms with no
  validation pass, invisible under every sanitizer on the one that has one. **Resize where the handle is
  read: inside the draw walk, immediately before the read** (`MaterialPreview::prepareFrame`,
  `ViewportPanel::onDraw`'s steps 5–8), and return false when there is nothing to bind — including the
  allocation-failure arm, where the previous pair is already gone and the `Image` must be skipped that
  very frame. This is why the viewport has never had the defect, and it shipped green in the Material
  panel until the code-review round.
- **A source-text pin must encode the property that matters, not a proxy for it.** `I96` originally
  greped for `destroyTexture(`/`destroyMaterial(` and required them inside the service function — and the
  destroy that mattered was inside `RenderTarget::resize` → `allocate`, invisible to that grep, so the
  defective code **satisfied** the pin. It now pins **ordering against ImGui's consumption**: `resize`
  belongs to `prepareFrame` and nowhere else, cache destroys stay in the service pass and the destructor,
  and `material_panel.cpp`'s three preview statements must read prepare, read, `Image`.
- **The reconcile block now does SEXTUPLE duty and the post-draw slot QUADRUPLE.** Extend; never twin.
- **`aero::render` is PUBLIC on `aero_editor_core`** so `material_edit.hpp` can be a public, tier-0
  testable header — the `aero::assets` precedent from 3.3.1, criterion for criterion.
  `aero::scene_render` stays PRIVATE; that distinction is the point, not an oversight, and it is why a
  public editor header may name `render::MaterialParams` but not a `SceneRenderer`.
- **The slot→colour-space rule is COMPOSED from `render::defaultTextureKindForSlot`**, never restated
  beside it — 3.4.1 deleted a hand-written per-slot table for exactly this reason. The preview's texture
  cache key is `(guid, contentHash, srgb)`, so one source in two slots with different spaces loads twice.
- **The Material panel writes files, and it still mutates nothing in the draw walk.** Every control
  writes into a per-frame copy and records ONE pending whole-document edit; `tick()` drains it. Numeric
  edits clamp **in C++** against the same ranges `material_format.cpp` validates with — an ImGui slider
  with a `v_min` still lets a Ctrl+Click type anything at all, and no tier in this tree can perform that
  click, so the clamp's only mechanical witness is a source-text pin (`I98`).
- **A reduced-configuration claim must name which binaries it ran.** `I88`–`I92` failed in a
  tools-OFF build (115/120) after an earlier probe of that path built `aero_editor_shell_test` only.
  `aero_editor_imgui_test` now carries `AERO_SHADER_TOOLS_ENABLED=1` inside its own
  `if(AERO_SHADER_TOOLS)` block, and both arms **assert** — a skip would leave AC-32 untested in the one
  configuration that can test it.

## Drag-into-scene (task 3.1.5)

- **`"AERO_ASSET"` is the tree's SECOND payload type, and the two can never cross-fire.** The first is
  the Hierarchy's own `"AERO_ENTITY"` reparent payload (`hierarchy_panel.cpp`), untouched by this task:
  the type strings differ, so `ImGuiPayload::IsDataType` refuses each other's payloads structurally
  rather than by a check somebody has to remember to write. A third payload type inherits that
  property only if it is likewise a distinct string.
- **`AssetDragPayload` is 24 bytes and is DECODED, never cast.** ImGui's payload buffer is `alignas(1)`
  and, at 24 bytes, is the **heap** buffer rather than the inline 16-byte one; the Debug lanes run
  UBSan. `decodeAssetDragPayload(data, sizeBytes)` is the **only** reader of an `ImGuiPayload::Data` in
  this tree -- it `memcpy`s into a local and refuses null data, a size that is not exactly
  `sizeof(AssetDragPayload)`, and a nil guid (a nil guid in a payload is a corrupt payload, never a
  "none" value). It takes the raw pointer and size so the public header names no ImGui type.
- **Value-initialisation does NOT zero this struct's padding, and the fix belongs at the ONE
  `SetDragDropPayload` call site.** Measured on Apple clang against a poisoned stack slot: `engine::Guid`
  carries `hi`/`lo` NSDMIs, which makes `AssetDragPayload` **not** trivially default constructible, so
  `[dcl.init]` value-init runs the constructor and the compiler elides the whole-object zeroing -- seven
  tail bytes come back indeterminate. Nothing reads them (the decode reads `guid` and `kind` only), so
  this is a determinism property, not a correctness one; the source `memcpy`s into an explicitly zeroed
  buffer at the single call site. **Do not "fix" it with `{}` at the declaration -- that was measured
  and does not work for this type.**
- **THE PEEK RULE: `classifyAssetDrop` runs BEFORE `AcceptDragDropPayload`, at every target, always.**
  ImGui draws the drop highlight as a side effect of `AcceptDragDropPayload`, so calling it and then
  deciding is a **visible promise the editor then breaks**. `GetDragDropPayload()` is the peek;
  `AcceptDragDropPayload` is the commitment. The whole accept/refuse matrix is one total pure function
  so the decision is a tier-0 table test and the ImGui half stays four lines per site:

  | kind \ surface | HierarchyRow | HierarchyVoid | Viewport | MaterialSlot |
  |---|---|---|---|---|
  | Model | Instantiate | Instantiate | Instantiate | None |
  | Material | Assign **iff** the target has a `MeshRenderer` | None | Assign **iff** the hit has one | None |
  | Texture | None | None | None | BindTextureSlot |
  | Folder / Audio / Text / Unknown | None | None | None | None |

  It is a `switch (kind)` around a `switch (surface)`, **both without `default:`**, so a new `AssetKind`
  or a new `DropSurface` is a `-Wswitch` error rather than a silent `None`. `targetHasMeshRenderer` is
  recomputed from the **live** `World` at the accept site and is false for every surface with no target
  entity -- never remembered, never taken from the payload.
- **The SOURCE side refuses too, and that is where an illegal payload stops existing.** One helper,
  three call sites (grid tile, list row, and the list row's caption); it starts no drag for a folder, a
  non-draggable kind, or a path with no record / no valid guid. Note `.mtl` classifies `Unknown` and
  therefore starts **no** drag even though it is importable.
- **The viewport's drop target is `BeginDragDropTargetCustom`, and the reason is `ImGui::Image`.** That
  widget submits its item with **id 0**, so `BeginDragDropTarget()` cannot attach to it. The custom form
  takes a rect and an **explicit** id, consults **no item state at all** (verified in the pinned
  `imgui.cpp`), and `IM_ASSERT`s a non-zero id -- so the `!= 0` guard turns a would-be Debug abort into
  "the target does not exist this frame". While any payload is live the viewport is a **drop target,
  not an input surface**: `hovered` is suppressed for the camera gesture, the pick and the `F`-focus
  guard, and the pick arm is disarmed, so a drop never also orbits.
- **THE LEDGER'S DEFERRED DESTROY IS ONE LINE AND IT IS THE WHOLE GPU HALF OF THE DESIGN.**
  `SceneAssetLedger::service()` returns the **previous** pass's destroy list **first**, before any step
  of this pass can add to it. A whole service pass therefore separates "the binding table stopped naming
  this handle" from "the GPU object dies", so nothing recorded in frame N can still name a handle
  destroyed in pass N+1. That is why `pendingDestroy` is a **member** and not a local, and why the
  deferral **cannot** be reconstructed from the retire list -- rebuilding it there destroys this pass's
  retirements this pass, which is the same defect in a different hat. The caller's order is **destroy
  first, retire second, execute third**. A test for this must be **one sequence case**: two independent
  cases both pass under the very defect they exist to catch.
- **The ledger is pure and executes nothing.** Facts in (five plain values per referenced guid), a
  directive out; the src-private loader executes exactly one directive per pass and **owns no GPU
  object** -- every handle it mints is handed to the caller and adopted by the ledger entry, so the
  loader can be destroyed at any point in teardown. Its texture cache is **negative on purpose**: a
  success is never shared, because two materials of one model naming the same image in the same colour
  space would give one handle two owners and one premature destroy; only a **failure** is remembered,
  which is the half that matters (one decode per key per session, the `ThumbnailLedger` stickiness rule).
- **The reconcile block is now the SEVENTH occupant and the post-draw slot the FIFTH** -- superseding the
  sextuple/quadruple counts recorded at 3.4.2. Reconcile: 2.6.1's panel root, 3.1.1's database, 3.1.3's
  report, 3.1.4's watcher, 3.2.1's import session, 3.4.2's material session, **3.1.5's drop drain**,
  which sits at the **end** of the block -- after everything that can retarget the material session and
  before the draw walk, so a texture drop is judged against this frame's session and an entity created by
  a model drop appears in the Hierarchy in the frame the drop landed. Post-draw: `renderScene`,
  `serviceThumbnails`, the import session, the material preview, **`serviceSceneAssets()`**, appended
  last because the ledger touches no ImGui-sampled texture. **Extend; never twin.** F9's rule gets an
  eighth application: every one-shot is drained as its **own** statement, unconditionally, before it is
  inspected.
- **Every GPU create and every GPU destroy this feature performs happens in that post-draw slot**, and
  unlike the material preview there is **no draw-walk exception** -- no texture it creates is ever handed
  to ImGui, so the 3.4.2 "reallocate where the handle is read" carve-out does not apply here. The destroy
  ordering above is what makes that true.
- **A pending drop must be consumed at the TOP of `onDraw`, before every early return.** The Material
  panel folded its pending slot drop at the fold point, so a drop on an **untargeted** panel survived
  indefinitely and bound a slot on whatever material was selected next. `std::exchange` at the top is the
  shape; the same trap exists for any future one-shot a panel folds into a frame copy.
- **A source-text pin over `->Data` must match the WHOLE token.** `->Data` is a prefix of `->DataSize`,
  and `hierarchy_panel.cpp` has legitimately read `->DataSize` since 2.2.1, so a substring form
  false-positives on a **correct** tree -- and the POSIX-ERE `\b` form degrades to a literal on
  BSD/macOS. The working shell form is `git grep -nE -- '->Data([^a-zA-Z0-9_]|$)'`; the in-tree pin is a
  token-boundary predicate with an allow-list of the two legal shapes (hand the pointer to the decoder,
  or the one legacy `memcpy`). **Verify a pin in BOTH directions**: seed the cast and watch it redden,
  then seed a legal second decode call and watch it stay green.
- **A drop is ONE undoable command**, the tree's sixth structural command and the first that creates more
  than one entity -- which is why `captureAndDestroySubtrees` / `restoreStructuralState` were **promoted**
  onto `entity_commands.hpp` rather than copied. `ActionKind` is deliberately **not** extended for the
  Hierarchy's drop: that panel's `applyPending` cannot finish the job (instantiation needs the database,
  the importer and the ledger), and appending an enumerator `applyPending` must then refuse to handle is
  worse than not appending one.
- **The pick box and the frame box are ONE box, produced by `localBoundsFor`** -- TWO consumers since
  E.1.4, not three. A `nullopt` -- loading, failed or missing -- is treated by both exactly as an entity
  with **no** `MeshRenderer` already was: a point for bounds, the screen-space disc for picking. **No new
  fallback code exists anywhere**, and `MeshBoundsLookup` is threaded as a **defaulted, last** parameter
  on the bounds entry points so every pre-existing caller compiles and behaves identically.
  `LOCAL_MESH_HALF_EXTENT` is **gone**; 2.3.2's deliberately fat plane box went with it, and the flat
  plane needs **no epsilon** -- only a precondition of `box.valid()` (`min <= max`, never `min < max`).
  **THE HIGHLIGHT NO LONGER RESOLVES A BOX AT ALL** (E.1.4): `buildSelectionOverlay` has lost its
  `MeshBoundsLookup*` parameter, and it draws a point marker for the entities
  `scene_render::buildSelectionMaskSet` reports as having no geometry while everything else gets a GPU
  silhouette from the mask pass. The rule this bullet protects -- one box, no drift between the pick and
  the bounds walk -- is unchanged for its two remaining consumers.
- **`aabbCorner` is the single corner enumeration**, and it still is: `pickEntity` and the bounds walk
  read it and cannot drift. E.1.4 deleted `BOX_EDGES`, the table that used to be derived from it, when it
  retired the AABB highlight. *(`engine/render/src/debug_draw.cpp` carries an unrelated file-local
  `BOX_EDGES` for its wire-box helper; it is not this rule's subject and must not be touched.)*
- **3.4.2's resize-before-read rule has a STRUCTURAL exemption, and `renderSelectionMask` is the first.**
  The rule exists because ImGui *records* an `ImTextureID` during the draw walk and *binds* it inside
  `ImGuiLayer::endFrame`, which runs **after** `tick()`'s post-draw slot, while SDL's Vulkan and D3D12
  backends `SDL_free` a destroyed texture container **immediately** -- so a reallocation between the
  record and the bind is a use-after-free on two backends and **benign on the one with a validation
  pass**. `ForwardRenderer::renderSelectionMask` destroys and recreates its mask texture inside
  `renderScene`, which **is** in that slot, and it is nevertheless safe **structurally**: the mask texture
  is never handed to ImGui. The only handle the Viewport records is `target->colorTexture()`, and `target`
  is resized in `onDraw` where the rule requires. This is 3.1.5's `SceneAssetLedger` position verbatim.
  **`I122`'s clause (c) is what keeps it true** -- it pins as source text that no `ImGui::Image` call in
  `viewport_panel.cpp` names anything but `target->colorTexture()`, because the defect it prevents is
  invisible to every pass this project can run locally.

Full history: `docs/10-engineering-log.md`, Epic 2.1 / 2.2 / 2.5 / 2.6 entries, and tasks 3.1.1, 3.1.2,
3.1.3, 3.1.4, 3.1.5, 3.2.1, 3.2.2, 3.2.3, 3.2.4, 3.2.5 and 3.4.2's entries under Phase 3.
