---
paths:
  - "editor/**"
  - "tests/editor/**"
---

# Editor: scenes, projects and containment

Split out of `editor.md` so that no editor rules file is cut at the 150 000-character load limit. A reference to another section may point into a sibling `editor*.md` file.

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
- **A native dialog closed FROM THE KEYBOARD leaves no key window on macOS, so the editor hands the keyboard
  back itself (fix 2.5.1-focus).** After a sheet closes by Return or Escape, AppKit makes no window key — the
  editor stays main, the app stays active — so SDL never sees `windowDidBecomeKey:`, its keyboard focus stays
  NULL, and SDL's Cocoa key handler drops every key until a click (measured: 8 of 8 keyboard closes, 0 of 6
  mouse closes). The repair is ONE gated call, `restoreKeyboardFocusAfterDialog` (`file_dialog.cpp`), in
  `EditorApp::tick()`'s `result.ready` arm — the one `dialogChannel->take()` — and **above**
  `applyDialogResult`, which can launch the next sheet in the same call. Three things a change must keep: it
  runs on the **main thread**, never in `onDialogResult` (an arbitrary thread on Windows and under zenity, and
  both SDL calls are main-thread-only); it runs **once per result**, never per frame — anywhere else in
  `tick()` it pulls the editor in front of every other application for as long as it lacks the keyboard; and
  the gate is `dialogCloseNeedsRaise` (no window, or the window already holding SDL's focus, raises nothing),
  which makes it a no-op wherever the keyboard is ALREADY back when the result is taken (every mouse close in
  the active editor, measured) with no per-OS branch — Windows and Linux are unmeasured, and wherever their
  result arrives before the focus does, it asks for a raise there too. **A new native
  dialog needs nothing as long as its result arrives through `dialogChannel`**; one that bypasses the channel
  inherits the defect. ImGui viewports are OFF (`imgui_layer.cpp:82`); if that ever changes, the gate must ask
  whether ANY editor window holds the keyboard, or a result will pull the main window over a focused viewport.
  Out of reach and recorded: a sheet macOS puts ON a panel — its "replace?" alert closed with Escape, its
  Go-to-Folder sheet closed with Return — leaves the panel itself un-key, and the editor sees nothing until
  the panel closes. `I275` pins the gate and `I276` the
  placement as source text; no tier can press a key or read AppKit's key window, so the validation page is the
  only behavioural witness.
- **Containment is decided at `openSceneFile` and `saveSceneFile` and NOWHERE ELSE (task E.4.2).** Both
  take a **non-defaulted** `const SceneFileContext&` — a default would let a future call site silently
  take the permissive arm, which is a wrong picture with no error and no failing test, while
  non-defaulted makes every unconverted site a compile error. The context is built **at the call
  expression and never hoisted**: `ProjectSession::root()` returns a view into the live session and
  `adoptProject` replaces it from inside `performAction` (`CN19` pins the absence of a hoist; `CN20`
  asserts `editor/src` spells `NO_PROJECT_SCENE_CONTEXT` zero times). A refusal is **exactly one ERROR**
  whose reason clause is `containmentReason`'s own bytes, handed to the modal verbatim so the two
  wordings cannot drift. `NoProject` **permits and logs nothing** — the Welcome window is a supported
  state and `File ▸ Open Scene…` is enabled in it. The verdict is **lexical first**, so a permitted open
  or save performs **no filesystem call at all**, and the canonical rescue runs only on a refusal and can
  only ever **widen** it — never run it on the permitted path, and never invert it: a mistake anywhere in
  this stack must produce a false **refusal**, never a false accept. A surviving `..` on either side is
  `Unresolvable` and is never resolved. **A refused SAVE never offers a project**, spelled at both the
  raiser and the modal, because accepting one runs `adoptProject` → `newScene` → `World::clear()` +
  `CommandStack::clear()` and would discard the very work being saved. `restoreLastScene`'s two sites
  pass a **permanent null offer** — they have no `FileFlow` in scope by design, and a modal there would
  offer the project that was just opened.
- **`FileDialogHost::projectRoot` is the WRONG value for containment (task E.4.2), and reaching for it is
  the most likely accidental defect in this area.** It is bound to `project.scenesRoot()`
  (`editor_app.cpp`, two sites), so using it checks every scene against `<root>/scenes` and refuses
  anything the user deliberately put in `assets/levels/`. It is also **mixed-separator on Windows by
  design** (`project.hpp`'s join rule), which makes half of that defect invisible on macOS and Linux
  forever. The authority is `ProjectSession::root()`. `IO18` holds the rule at the predicate but supplies
  its own context, so it cannot see which root production chose — **`SS51` is the case that drives five
  real call sites**, and the seed that motivated it left both binaries entirely green.
- **Every abandon path in `scene_session.cpp` clears BOTH `flow.requestedPath` and
  `project.flow.requestedPath` (task E.4.2, `SS52`).** A failed write abandons the pending action, so that
  action's own target must go with it, whichever flow object it lives in. `applyDialogResult`'s Save arm
  was the one hole in that roster: a containment-refused save left a project parked in
  `project.flow.requestedPath`, and the next `File ▸ Open Project…` found it, took `performAction`'s
  no-dialog seam and adopted that project with no folder dialog and no click. When you add a new failure
  arm here, clear the roster, not just the field you were thinking about.
- **A native Save panel's answer is normalised to a `.scene.json` leaf** (`normalizeSceneSavePath`, D13
  revised by fix 2.5.1): `X`, `X.json`, `X.scene` → `X.scene.json`; a leaf `isSceneFileName` accepts is
  kept byte for byte. **Every trailing suffix is stripped, one at a time, stopping at the first scene name**,
  because macOS's panel appends its hidden `.json` to WHATEVER is typed (measured: `r11.scene.json` arrives
  as `r11.scene.json.json`, `R12.JSON` as `R12.JSON.json`) — never assume a panel returns what was typed.
  **All three name refusals — no stem, a HIDDEN leaf (`isHiddenName`, the listing's own
  rule, so `.r9` or `.foo.scene.json` never becomes a scene the cascade cannot see), or a normalised name
  that differs from the answer and already exists — happen after containment and before serialization**,
  in that order, log one ERROR and raise the save-refusal modal; the panel's own "Replace?" prompt covered
  the name the user saw, never the corrected one, and a leftover `X.json` never blocks saving `X` again
  (`SS59`, `IO23`). Only `applyDialogResult`'s Save arm passes `appendExtension = true`; a literal path is
  the caller's. **Two modal labels share `###aero_scene_containment`** — a third label must share it too
  (`I270`), and `I192` pins both `BeginPopupModal` entries by their spelling. **The modal answers Return by
  hand** (#109's recipe: every button and key records an answer, one resolution acts, dismiss first, each
  arm closing the popup), and `I270` pins that as source text.

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
  architecture bug (AC-46). Since E.6.4 the direct spellings are refused by source text: `I358(a)` bans
  `createProject(`, `promoteRecent(`, `adoptProject(` and `session.set(` over `project_ui.cpp`'s code lines. At
  2.6.1 no tier saw it (sabotage seed S20 left the whole 94-entry suite green), and a write reached through an
  alias is still invisible to that pin.
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

### The per-project editor state (task E.4.1)

`<projectRoot>/Library/editor-state.json` (docs/09 §4.10) records the last scene, as a
project-relative path. `project_state.{hpp,cpp}` is PURE — no `<filesystem>`, no ImGui, no SDL, no
entt, no logging, no `#if` — and the whole of the format, the path arithmetic, the resolution and the
write decision live in it. Two consumers, one each: `restoreLastScene` in `scene_session.cpp` reads
and resolves; `EditorApp::persistProjectState` decides and writes.

- **(1) `restoreLastScene` runs from `openProjectPath` ONLY, and ONLY after `adoptProject`.** Before
  it, `project.session.root()` still names the OUTGOING project and the restore reads that project's
  state. `createAndOpenProject` deliberately does **not** call it: `createProject` refuses a
  non-empty target, so a fresh project has no `Library/`, no state file and no scenes **by
  construction**, and a call there could only ever be a no-op. It takes a `const ProjectSession&`
  rather than the `ProjectContext&` its caller holds, so **INV-P1's setter is not reachable from it
  at all** — 2.6.2's `PanelContext::project` rule ("the `const` is the enforcement"), one layer down.
- **(2) The write reconcile is the END of `tick()`, never the top.** `applyFileRequests` runs inside
  `drawShellUi` earlier in the same tick, so a scene opened, saved or cleared by this frame's menu
  action is already in `session.path()` by the time the draw walk returns — and the unsaved-changes
  modal's **"Save"** answer performs the save **and** the pending Quit in ONE tick, after which
  `tick()` returns false for ever. A top-of-tick reconcile loses exactly that frame, which is the
  most common quit path there is. The call sits between `endFrame()` and the `quitConfirmed` branch;
  it touches no ImGui and no GPU, so the placement is about the QUIT, not about the draw walk. **No
  test tier can answer that modal** — `FileFlow::choice` has no public `EditorApp` accessor and the
  GPU tier is ImGui-free at source — so this is proven only by a manual pass.
- **(3) A project CHANGE adopts the baseline WITHOUT writing; only a scene change inside one project
  writes.** Writing on a project change would re-record the value just read from that same file, and
  the two produce **byte-identical** files — so no assertion over the file's content can tell them
  apart. `EditorApp::projectStateWriteCount()` is the ONLY thing in the tree that can. **Do not
  delete it**, and do not "simplify" the reconcile to "write whenever the pair differs".
- **(3b) …but the OUTGOING project's own scene change is handed off, because one tick can do both.**
  A guarded Save on an **untitled** scene takes `resolveConfirm`'s `AskWhereToSave` arm, and the
  native panel's answer reaches `applyDialogResult`, which saves the scene **into the outgoing
  project** and performs the pending `OpenProject` **in the same call**. That `(root, scene)` pair is
  invisible at **both ends** of the tick — at the top the scene is still untitled, at the end the
  session already names the new project — so rule (3) alone records neither. `openProjectPath`
  therefore publishes the pair into `ProjectFlow::outgoingState{Root,Scene}` immediately **before**
  `adoptProject` (never inside it — its five statements are byte-identical to 2.6.1's, and `set()` is
  what makes `root()` name the new project), and `persistProjectState` **drains it first**, through
  the **same** `projectStateStep` against the **same** baseline, then clears both fields whether or
  not it wrote. **This is a handoff, not a second write site**: `persistProjectState` remains the only
  thing in the tree that writes `editor-state.json`. A non-empty pending root **is** the "pending"
  flag; a separate bool would be a second spelling of the same fact. On a plain open or swap the
  pending pair **equals** the baseline, so nothing is written and (3) still holds — which is what
  `I176`, `I180` and `I185` assert. **No tier can drive the chain itself** (`FileFlow::choice` has no
  `EditorApp` accessor and `DialogChannel` is src-private), so `PJ55` drives it at the flow tier and
  is its only behavioural witness.
- **(4) A failed write advances the baseline anyway.** Otherwise a read-only `Library/` produces a
  write attempt and a WARN on **every frame**, for ever. `asset_database.cpp`'s INV-C11 ("in-memory
  regardless of whether the write succeeded"), one file over. A failure therefore costs exactly one
  WARN per scene change, and the counter above counts only **successes**, which keeps it an
  assertion about the disk rather than about the attempt.
- **(5) `lastScene` ABSENT and `lastScene: ""` are DIFFERENT ANSWERS.** Absent cascades to the
  project's first scene; `""` opens a new one. Collapsing them hands a person who chose
  `File ▸ New Scene` somebody else's scene on the next launch. The writer **omits the key** when
  nothing is recorded and the parser reads "recorded" from the key's **presence** — which is also
  what makes appending a second key to this file a non-breaking change with no version bump.
- **A recorded scene that is MISSING cascades; one that EXISTS and fails to open STOPS.** One
  `fileExists` call tells them apart, it is made by the caller, and `chooseStartupScene` stays pure.
  The decider picks a *candidate*; the caller opens it **once** and never re-enters the decider. D6's
  "stop" is not code — it is the absence of a second attempt, so the way to break it is to ADD one.
- **A stale record is never pruned, and the cascade does not correct it.** A cascade takes the
  AdoptBaseline arm, which writes nothing, so a record naming a deleted scene survives until the user
  next changes scene. That is deliberate — `recent_projects.json`'s rule (docs/09 §4.9) plus the
  write rule above — and the behaviour is right every time the scene is still missing. **Do not add
  a write on the cascade arm**: it would be a second write site, in the TU defined as the one that
  logs and does not write, and it would write on a mere open.
- **`projectRelativeScenePath` is a LEXICAL stand-in, not E.4.2's containment predicate.** No symlink
  resolution, no case folding, no drive-letter rule — and its remainder is gated on
  `isLegalRelativePath`, because the producer must guarantee what the parser requires or this build
  can write a document it cannot read back. Its failure mode is **"forget"**, never "wrong project":
  a false negative records `""` and the user gets a new scene. When E.4.2's predicate lands, this
  function becomes its caller or is deleted in favour of it.
- **EVERY producer of a project-relative path gates on `isLegalRelativePath` — `firstSceneUnder` too.**
  `entry.name` is an **OS-supplied leaf**, and `':'` and `'\'` are legal POSIX filename bytes that
  rule refuses anywhere, so a `boss:arena.scene.json` returned verbatim answers `""` from
  `absoluteScenePath` and reaches `openSceneFile` as an **empty path** — one spurious ERROR naming
  nothing, and the restore's one-and-only attempt spent on it while the loadable scene beside it is
  never tried. Such an entry is **skipped and the scan continues**; abandoning the directory would
  hand the whole project's startup scene to the first bad byte in it.

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

## Scene and project containment

(4) **CLOSED BY E.4.2, and replaced rather than deleted because the residue is deliberate.**
`openSceneFile`/`saveSceneFile` used to perform **zero** containment validation against the project root,
so a scene from another project loaded while the AssetDatabase still resolved GUIDs against the open one.
Today both take a non-defaulted `SceneFileContext` and refuse an out-of-project path **before any I/O**,
with one ERROR and a modal, **lexical first with a canonical rescue that only ever widens** — and four
things are still permitted **on purpose**: the no-project state (`NoProject` permits and logs nothing —
the Welcome window is supported), a symlink **inside** the project pointing out (`CN16` pins it, closing
it is one line plus a cost measurement), `<root>/Library/` as a save destination (`IO19` pins it;
**E.4.3** owns any reserved-path policy), and **the project's own entry points** — `loadProjectFrom` and
`createProject` are untouched, so a scene cannot escape its project while a project can still be opened,
or created, anywhere. Those four are the unowned handoffs, not a gap in the predicate.

**A PROJECT UNDER A SYMLINKED PATH SILENTLY LOSES ITS RECORDED POSITION (E.4.1's macOS pass).** `/tmp`
is a symlink to `private/tmp`, and the editor's native panels return the resolved form while `argv[1]`
keeps whatever was typed — so `projectRelativeScenePath`'s **purely lexical** prefix test declines and
records `""`. That is D8's DESIGNED failure mode (*forget*, never *wrong project*), reached from a
direction D8 did not name: E5 assumed the root and the scene path share one source, and the native
panel is a second. **E.4.2 owns the real predicate including symlinks, and its
validation row 5 has now MEASURED this shape in the product.** ★ **THE CANONICAL RESCUE IS LOAD-BEARING IN
PRODUCTION, NOT ONLY IN TESTS — R2 IS ANSWERED.** With a project opened through `/tmp/aero-symlink-proj`
(the root recorded with that spelling, because `loadProjectFrom` uses `absolute` and NOT
`weakly_canonical`) and the native dialog returning `/private/tmp/aero-symlink-proj/scenes/level1.scene.json`,
the open was **PERMITTED**. The two paths share **no leading segment**, so no lexical comparison could have
accepted it — D4 step 4 is the only thing that did. **Delete the rescue and an ordinary open under `/tmp`
is refused outright.** Validate on a NON-symlinked root unless symlinks are the thing under test.
