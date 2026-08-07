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
  FSEvents, no `ReadDirectoryChangesW`, no inotify, no owned thread, no `#ifdef` —
  `git grep -n '_WIN32\|__APPLE__\|__linux__' -- editor/` and `git grep -n 'JobSystem' -- editor/`
  are both **empty**, and this task keeps them that way. A native backend replaces `poll()` behind the
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

Full history: `docs/10-engineering-log.md`, Epic 2.1 / 2.2 / 2.5 / 2.6 entries, and tasks 3.1.1, 3.1.2,
3.1.3 and 3.1.4's entries under Phase 3.
