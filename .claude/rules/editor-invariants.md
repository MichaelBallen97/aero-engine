---
paths:
  - "editor/**"
  - "tests/editor/**"
---

# Editor invariants

Each rule here was earned by a defect, a green-and-wrong test or a measurement that contradicted a plan, and every one binds new work. The story behind each is in `docs/10-engineering-log.md`; grep it for the citation.

## The editor's public pairs

* **`/editor`** has gained ten public pairs across Epics E.2 and E.3: `material_inspector_model` (E.3.4 —
  PURE: no ImGui, no GPU, no `<filesystem>`, no logging, so tier 0 can walk the whole panel's shape),
  `thumbnail_service`, `asset_tile`, `asset_picker_model`, `asset_picker` (E.3.3), `context_router`,
  `editor_prefs` (E.3.2), `material_preview_rig` (E.2.4), `viewport_icons`, `viewport_gizmos` (E.2.3) —
  plus `project_state` (E.4.1), `scene_containment` (E.4.2) and `material_card` (E.4.5) in Epic E.4, all
  three PURE and all three tier-0 reachable in all three build configurations, and `create_menu` (E.5.2 —
  ImGui-free: the create table, the anchor and the seeds), and E.6.1's `editor_theme` (PURE, constexpr) plus the
  macro-only `editor_glyphs.hpp` / `editor_icons.hpp`; its font bytes are four generated TUs, never tracked.
  E.6.3 added six PURE, ImGui-free pairs — `panel_chrome_model`, `entity_presentation`, `hierarchy_view`,
  `console_view`, `asset_browser_chrome`, `viewport_hud` — and the src-private `panel_chrome_ui`, the panels' one
  shared ImGui TU. E.6.4 added `project_ui_view` (PURE, ImGui-free and filesystem-free: the Welcome window's and the
  New Project dialog's layouts, validation line, summary and record).
  **DO NOT record a "pair count" here — it is not reproducible**: E.2.4 measured the tree six ways looking
  for the figure this line used to carry and none of the six was it. The two figures anyone can re-run are
  `git ls-files`: **`editor/src/*.cpp` = 110** and **`editor/include/aero/editor/*.hpp` = 81** at E.6.4
  (`52a5b68`).
  `/tools` links `aero::assets` and `aero::editor_core` through `aero_cooker`, which is legal because
  `tools/` is enumerated by neither half of the golden rule.


## Destructive file operations and one-shot takers

**`DELETE_RE` MATCHES A SPELLING, WHICH IS WHY CHECK B GAINED AN ALIAS PRONG (E.4.3).**
`namespace fs = std::filesystem; fs::rename(a, b, ec);` is invisible to `(remove_all|std::filesystem::remove|
std::filesystem::rename)` in EVERY file, permitted or not — theoretical until E.4.3, because no editor TU
had any reason to write a `rename`. **The fix is NOT a widened `DELETE_RE`** (a denylist over spellings
cannot converge — 3.7.3's lesson) but the inverted claim: *the only legitimate spelling of the
`std::filesystem` namespace in `editor/src/*.cpp` is the FULLY-QUALIFIED one*, applied to the permitted
files too, since an alias there would blind Check B for them the moment the allowlist shrinks. **A future
destructive call must be fully qualified.** Proved in both directions: deleting the prong makes e2e stage 18
the first to fail, and an `ALIAS_RE` widened to match everything is refused by B-self-test 5 with exit 2
before any stage runs.

**`Delete` IS A `rename` INTO `Library/Trash/<NNNN>/`, AND `remove_all` NEVER ENTERS THE EDITOR (E.4.3).**
Check B **permits** `remove_all` in `asset_actions.cpp` — it is one of the two `PERMITTED_DELETERS` — so the
guard CANNOT make this claim and `AA61`'s source-text pin is the only witness there is. A folder delete is
ONE rename, so everything inside travels by construction and INV-A8 is satisfied structurally rather than by
iteration. **A future *Empty Trash* is a `remove_all` scoped to `Library/Trash/`**, which the guard's own
header already licenses as a deliberate, reviewed relaxation.

**A ONE-SHOT TAKER THAT MOVES OUT OF AN `optional` MUST `.reset()` AFTERWARDS (E.4.3).** A moved-from optional
is still ENGAGED, so the omission leaves the one-shot set and re-runs a moved-from (empty) request on EVERY
tick — one refused operation and one rescan per frame, for ever. **No observable in this tree can see it**;
`I218`'s third arm is a source-text pin over the header that owns the four optional takers. Seed `S13` came
back green until that arm existed.


## The editor — ImGui, ImGuizmo, focus and panels

**ONE CREATE PIPELINE (E.5.2)**: a new entry is one `createMenuEntries()` row plus one `createSeed` arm, and
only `EditorApp::applyCreate` places (at the root), pushes, frames and reveals (`I246`, `I253`); its camera
write runs before the draw walk, never in `renderScene`. **A click-emulating seam lives ONE tick**
(`expireNamedSelection`, `S48`).

**THE EDITOR HAS EXACTLY ONE FOCUS SLOT, AND `ImGui::SetWindowFocus` IS CALLED AT MOST ONCE PER FRAME FROM IT
(E.3.2).** It lives in `shell_ui.cpp` immediately before `DockSpaceOverViewport` — dock nodes update inside
it, so the focus lands with no one-frame lag — and resolves both paths in order: explicit `requestPanelFocus`
first (a COMMAND), then the pending context route (an INFERENCE). `I159(a)` pins one file, three calls. **A
fourth caller in another file is a second focus policy with no way to order it against the first.** The reason
is that **`FocusWindow`'s two side effects are not idempotent**: it closes every popup above the focused
window (`imgui.cpp:13740`) and **steals the active widget** (`:13754-13756`, whose own comment at `:13751`
names this very slot) — and a stolen `InputText` edit is **DISCARDED**, not interrupted, because
`MaterialPanel` commits only on `IsItemDeactivatedAfterEdit()` and the panel that lost the tab never draws to
observe the edge. **An automatic route selects the target's dock TAB and leaves `NavWindow` alone (#116)** —
`TabBarQueueFocus` with the `ImGuiTabItem*` overload, decided by the pure `routeRaise` — and falls back to
`SetWindowFocus` only when the keyboard is in a sibling tab of the target's node (ImGui re-selects that tab every
frame, `imgui.cpp:19611-19613`) or the node has no tab bar yet; the router HOLDS while `NavWindow`'s root is a
closed popup, which ImGui hands back a frame later. `I256` is the witness, and `keyboardFocusPanelId()` is the only
tier that tells "raised" from "took the keyboard".

**RE-READ AT EVERY ImGui / ImGuizmo BUMP:** E.6.1's — `imgui.cpp:1512-1590` (`ImGuiStyle`'s members; `TH5` pins
`sizeof(ThemeMetrics) == 308`, so a new member is theme work) and `:1602-1651` (`ScaleAllSizes` truncates EVERY
size); `imgui.h:1886` (`ImGuiCol_COUNT`, `static_assert`ed at 63); `imgui.h:529-530` (never `PushFont` a
`GetFontSize()`); `imgui.cpp:9743` (`FontScaleDpi` multiplies every font size) and `:16701` (what
`ConfigDpiScaleFonts` would overwrite); `imgui_internal.h:3598-3599` (`BeginMenuEx`/`MenuItemEx`, the Create
menu's icons); `imgui_draw.cpp:3538` (`GlyphRanges` is read only on the legacy preload path — a dynamic face is
restricted with `GlyphExcludeRanges`), `:4819-4820` (a merged `GlyphOffset` is snapped to whole points per baked
size) and `:3743-3744` (the one free of `FontData`, gated on `FontDataOwnedByAtlas`). And: `imgui.cpp:13740` and
`:13754` (focus side effects); `:8848`
(`g.LastItemData = ParentLastItemDataBackup`, so a last-item rect read after a `BeginPopup`/`EndPopup` pair
names the item *before* the popup); `:3418` (`Begin` sets `DisplayStart` to −1); `:8279` (an API-positioned
popup is never clamped); `:3918` (a label truncates at its first `##`); `imgui_widgets.cpp:6802` +
`imgui.cpp:8581` (`StateStorage` is per window); `imgui.cpp:6860` + `:12129` + `:437` (`EndChild` → `ItemSize`
→ the spacing recipe); `:1657` vs `:1703-1708` (1.92.8 removed the "a 1 px `Separator` does not move the
cursor" hack while `SeparatorEx`'s header comment still describes it); `ImGuizmo.cpp:1229-1230`;
`imgui.cpp:19611-19613` and `:19740-19751` (tab reselect / tab focus), `imgui_widgets.cpp:10002-10005` and
`:10384-10389` (the queued tab's consumption, and the `const char*` `TabBarQueueFocus` overload's dock-node assert),
and `imgui.cpp:5949-5950` (`NewFrame` hands the keyboard back from a closed popup). E.6.3's, the Hierarchy search
gate's premises: `imgui.cpp:4863-4866` (`ClearActiveID` is `SetActiveID(0, NULL)` and leaves `NavId` on the field),
`imgui_widgets.cpp:5180` (Enter sets `clear_active_id`), `:5212` (Escape sets it too, reverting the edit) and
`:5452-5453` (`InputTextEx` releases the active id at its end) — so a field closed by either key still holds the
keyboard's focus, which is why the gate reads `IsItemFocused`, never `IsItemActive` alone. The card painter's and
the card clip's citations are in `.claude/rules/editor.md`'s panel-chrome section, the crumb-row fold's in its
fixed-regions section, and E.6.4's — the sideways scroll reach a full-width band gives a clamped window
(`imgui.cpp:8478`, `:11053`) and the active field that owns its text — in its first-contact section.

**★ ImGui's `StateStorage` IS PER WINDOW, SO A BODY THAT IS SOMETIMES A CHILD IS TWO SETS OF WIDGET STATE
(E.3.4).** A child is a distinct window, so the same `CollapsingHeader` is a different entry inside a body
child than in the panel window — which silently re-opened every collapsed section whenever the panel crossed
its layout-mode boundary, a regression against `main`. **One function called from two places keeps the two
modes drawing the same CODE; that is not the same as the same STATE. Anything ImGui keys by window must be a
panel member to survive a mode flip.**

**IMGUIZMO'S TWO VISIBILITY SETTERS ARE CROSSED, AND NO TIER IN THIS TREE CAN READ EITHER VALUE.**
`SetPlaneLimit` hides **axes** and `SetAxisLimit` hides **planes** (`ImGuizmo.cpp:1229-1230` against the
setters at `:2657-2670`); the header's own comments say the opposite because they describe the NAMES. The
crossing is spelled in exactly one place — `viewport_panel.cpp`'s `applyGizmoStyle`, with the citation beside
it — and `I125(c)` pins it as source text. **There is no getter for either member, so a port bump that
un-crosses them is green and wrong.**

**THE GIZMO STYLE IS A PROCESS-WIDE GLOBAL WRITTEN EVERY SUBMITTED FRAME, AND A SECOND WRITER IS AN UNDECLARED
DECISION.** `ImGuizmo::GetStyle()` returns `gContext.mStyle` by reference and `BeginFrame()` does not touch
it, so the viewport's per-frame write is idempotent and self-healing — **so a task that wants a different
gizmo style must change `defaultGizmoStyle()`, never write the global from a second site.** `I125(e)` asserts
`ImGuizmo::GetStyle()` appears exactly **twice** in `viewport_panel.cpp`. Its colours are DERIVED from
`axis_palette.hpp` (aliases into `EDITOR_THEME` since E.6.1) and `viewport_panel.cpp` states no gizmo colour
literal at all, because **a restated literal one byte off is invisible to every automated tier** (E.1.4's
sabotage row 20). Since E.6.1 its LENGTHS pass through the pure `scaledGizmoStyle(style, uiScale)` once, at
that one write.

**A CHROME WIDGET THAT SUBMITS NO ImGui ITEM IS INVISIBLE TO ImGuizmo'S OWN PROTECTION (E.1.3).**
`CanActivate()` is `IsMouseClicked(0) && !IsAnyItemHovered() && !IsAnyItemActive()`, so a widget drawn with
`ImDrawList` alone is protected only by accident. Widen the `ImGuizmo::Enable` term — never an early return
(it would hide the handles), never `!IsOver()` in the widget's guard (it reads `gContext` before this frame's
`Manipulate`) — guarded by `!IsUsing()` so an in-flight drag survives. **Any future viewport chrome drawn with
`ImDrawList` alone inherits this and must claim its own presses** — E.6.3's HUD does, through `hudOwnsPoint`
beside `viewAxisOwnsPoint`, and chrome drawn around a claimed shape grows the ONE claimed extent with it (the
view-axis circle, `viewAxisCircleRect`, `VO14`, `I116`).

**A ROUTE NEVER RE-OPENS A PANEL THE USER CLOSED, AND EVERY DROP IS TESTED BEFORE EVERY HOLD (E.3.2).**
`targetAvailable` is *registered AND visible*, and a hidden target is a **Drop**, not a Hold — closing a panel
is the user's second, coarser off switch. The five terminal conditions can each persist indefinitely, so
holding on one would hold forever; the four transient ones all end on a mouse-up, a click-away or an Escape.
**Never reorder a Drop below a Hold.** And the router **derives nothing** — it spells neither
`isImportableModelName` nor `isBlendFileName`, reading the `SessionState` the import session itself wrote, at
the cost of one extra tick. If that ever has to be one tick, the fix is a settled-state signal ON THE SESSION,
published before `ShellUiState` is built — never a predicate in the router.

**A PREFERENCE THAT SUPPRESSES AN EFFECT MUST NOT SUPPRESS THE OBSERVATION THAT FEEDS IT (E.3.2).**
`ContextRouter`'s three `observe*` functions gate the **LATCH**, never the early return: the baseline advances
on every tick whatever the preference says, so an act performed while routing is off is SEEN AND FORGOTTEN
rather than SKIPPED. Gating the early return leaves the baseline stale, and the first observation after the
preference returns raises a panel for a minutes-old act — a focus steal triggered by ticking a menu item,
which a whole green matrix was blind to. **`observeImportTarget` is the deliberate exception**: `!settled`
stays in its early return, because D4's rule is the SESSION's and is independent of the preference. **General
shape: when a switch turns a REACTION off, the state deciding "is this new?" must keep advancing, or the
switch becomes a delay line.** `.claude/rules/editor.md` carries this too.

**`Selection::prune` MUST NEVER BUMP `Selection::revision` (E.3.2).** `HierarchyPanel::onDraw` prunes every
frame, so a bumping prune is a permanent focus storm; `I159(e)` pins the absence as source text, because a
prune that bumped and un-bumped would satisfy the effect. **And `set`, `toggle` and `setAll` delegate to two
PRIVATE, NON-COUNTING helpers**, never to `add`/`remove`: the counter counts public CALLS, so routing them
through the public mutators makes `set` bump twice and `setAll(n)` bump n + 1, while bumping only in
`add`/`remove` gives `setAll({})` zero. **A delegating mutator cannot carry a per-call counter in its own body.**

**A PROJECT CHANGE ADOPTS THE PER-PROJECT STATE BASELINE WITHOUT WRITING — BUT THE OUTGOING PAIR IS
HANDED OFF FIRST, BECAUSE ONE TICK CAN CHANGE BOTH THE SCENE AND THE PROJECT (E.4.1).** `resolveConfirm`'s
`AskWhereToSave` arm keeps the pending action, and `applyDialogResult` then calls `saveSceneFile` — giving
an untitled scene a path INSIDE the outgoing project — and `performAction(flow.pending)` **in the same
call**, so `openProjectPath` → `adoptProject` → `clearPath()` + `set()` all land before the draw walk
ends. **That pair exists only between those two statements and is invisible at BOTH ends of the tick**, so
a top-of-tick snapshot does NOT recover it — at tick start the scene is still untitled, i.e. `""`, which
equals the baseline. `ProjectFlow::outgoingState{Root,Scene}` is published in `openProjectPath` **above
and outside** `adoptProject` (whose five statements stay byte-identical) and drained by
`EditorApp::persistProjectState`, **still the ONE write site**, which asks the SAME pure `projectStateStep`
about the pending pair rather than spelling a second condition that could drift. **A non-empty pending
root IS the validity flag** — a separate bool would be a second spelling of one fact and an arm no seed
could distinguish. **Any future path that can change the scene and the project in one tick inherits this.**

**EVERY PRODUCER OF A PATH THAT WILL BE REJOINED MUST GUARANTEE WHAT THE PARSER REQUIRES (E.4.1).**
`isLegalRelativePath` rejects **any** `'\'` or `':'`, and both are legal POSIX filename bytes — so
`firstSceneUnder`, which returns an OS-supplied LEAF verbatim, could emit `scenes/boss:arena.scene.json`,
make `absoluteScenePath` return `""`, and produce one spurious ERROR plus **no second attempt** (the
design makes exactly one, ever). `projectRelativeScenePath` was gated and `firstSceneUnder` was not; it
now skips such an entry and keeps scanning. **Both feed the same joiner — gate every producer, not the
one you thought of first.**

**SCENE CONTAINMENT HAS ONE AUTHORITY AND IT IS `ProjectSession::root()` (E.4.2).** `openSceneFile` and
`saveSceneFile` take a **NON-DEFAULTED** `SceneFileContext` — a default would let a future site silently
take the permissive arm, which is a wrong picture with no error and no failing test — and the context is
built **at the call expression, never hoisted**, because `root()` returns a view into the live session and
`adoptProject` replaces it from inside `performAction` (`CN19` pins it; `CN20` asserts `editor/src` spells
`NO_PROJECT_SCENE_CONTEXT` zero times). **NEVER `FileDialogHost::projectRoot`**, which is bound to
`scenesRoot()` and refuses every scene deliberately put outside `<root>/scenes` — and is
**mixed-separator on Windows by design**, so half that defect is invisible on macOS and Linux forever;
`SS51` drives five production sites because `IO18` supplies its own context and cannot see which root the
call sites chose. **The verdict is LEXICAL FIRST and the canonical rescue only ever WIDENS** — reachable
only from `Outside`, producing only `Contained`, which bounds every untested platform to a *false refusal
with a readable ERROR* rather than a false accept. Running it on the permitted path changes **no answer
anywhere**, so `CN13` pins the early return's **position in the source text**; nothing else can see it.
**A refused SAVE never offers a project**, at both the raiser and the modal, because accepting one runs
`adoptProject` → `newScene` → `World::clear()` + `CommandStack::clear()` and would present data loss as
the remedy for a failed save. `restoreLastScene`'s two sites pass a **permanent `nullptr` offer**: they
have no `FileFlow` in scope by design, and a modal there would offer the project just opened.

**A NATIVE PANEL'S ANSWER IS NOT THE NAME THE USER SAW (#117).** macOS's Save panel appends its hidden `.json`
to WHATEVER is typed (`r11.scene.json` → `r11.scene.json.json`), so `normalizeSceneSavePath` strips every
trailing suffix one at a time and appends `.scene.json`; only `applyDialogResult`'s Save arm normalises. The
panel's own "replace?" covered ITS answer, never the corrected name, so the three name refusals — no stem, a
hidden leaf, a corrected name that already exists — run after containment and **before serialization** (below it
they are invisible in reflect-tools-OFF), and a new label for the refusal modal shares `###aero_scene_containment`
(`I270`).

**ON macOS A NATIVE SHEET CLOSED FROM THE KEYBOARD LEAVES NO KEY WINDOW, AND SDL DROPS EVERY KEY UNTIL A CLICK
(#118).** The repair is ONE gated `SDL_RaiseWindow` (`restoreKeyboardFocusAfterDialog`) as the first statement of
`EditorApp::tick()`'s one `dialogChannel->take()` arm: main thread, once per result, above `applyDialogResult`
(which can launch the next sheet in the same call) — never in the dialog callback (another thread), never per
frame. The gate reads SDL's focus when the result is taken, so it is a no-op only where the focus is back by then
(every mouse close in the active editor, measured); Windows and Linux are unmeasured. **A dialog whose result
bypasses `dialogChannel` inherits the defect.** `I276` pins the placement as text.

**EVERY ABANDON PATH IN `scene_session.cpp` CLEARS BOTH `flow.requestedPath` AND
`project.flow.requestedPath` (E.4.2).** A failed write abandons the pending action, so that action's own
target goes with it whichever flow object it lives in — `applyDialogResult`'s Save arm was the one hole in
that roster, and a containment-refused save therefore armed a later `File ▸ Open Project…` to skip its
folder dialog and adopt a project from a refused **open** minutes earlier, through `adoptProject` →
`newScene`, with no dialog and no click (`SS52`). **And any programmatic path that answers a modal must
ENTER the popup to close it**: a request hook records the one-shot without the `CloseCurrentPopup` a
button calls, and ImGui **never GCs** an entry for a popup that simply stops being submitted —
`GetTopMostPopupModal` tests only the `Modal` flag, after which `g.HoveredWindow` is `NULL` for the rest
of the process and every menu, panel and dock tab is unclickable (2.6.1's BLOCKING-1, re-proved from the
other side). **No tier here can read `g.HoveredWindow`**, so `I192` is a source-text pin and a manual pass
is the behavioural witness. Every future request hook that answers a modal inherits both.

**A MODAL'S CLICK-EQUIVALENT SEAM RESOLVES INSIDE THE MODAL'S BODY (E.6.4).** `requestNewProjectAnswer` is
consumed by the same `if (dismiss) … else if (commit) …` the buttons and keys use, so it closes the popup the way
a click does and a GPU case observes the real close (`popupOpenAtEntry`, `I354`, `I355`) — the first behavioural
witness of a modal close in this tree. A seam that sets a form one-shot from outside the body needs `I192`'s
drained arm instead; a new way to answer the dialog joins the one resolution.

**THE NEW PROJECT DIALOG READS THE DISK ONLY THROUGH `NewProjectForm::probe` (E.6.4)**, which `applyFileRequests`
keeps outside the draw walk, and the probe and `createProject` share ONE target-path rule (`newProjectTarget` /
`classifyTarget`; `P86`, `P88`), so the preview and Create cannot disagree. The probe is advisory: `createProject`
keeps every refusal, and a filesystem call in `project_ui.cpp` is a defect (`I358(a)`).

**A SEAM THAT TYPES INTO AN `InputText` RELOADS IT WHEN IT IS ACTIVE (E.6.4)** —
`GetInputTextState(id)->ReloadUserBufAndMoveToEnd()` on the seam's one tick — or the active widget writes its own
buffer back over the bound string the same frame, and the case reads the old text for ever (`I358(h)`; seed `S46`
proves it load-bearing). A case driving such a seam proves the field was active first.

**THE IGNORE ROSTER IS ONE VALUE, EVERY CONSUMER COMPOSES IT, AND A NAME-FREEDOM LISTING NEVER FILTERS BY IT
(E.4.4).** `IGNORED_ASSET_NAMES`, `IGNORED_ASSET_NAME_SUFFIXES` and `BLEND_BACKUP_STEM` in `asset_meta.hpp` are
the whole of "not an asset": add an entry THERE and to `docs/09` §5.10 in the same commit, and update `IX3`,
which pins the exact contents because **a universal over an array cannot see an entry deleted from it**.
Every consumer composes `isIgnoredAssetName` — the scan, the watcher by composition, and the browser through
`isBrowserVisibleName`, whose `isDirectory` term comes FIRST (a folder named `backup.bak` is the user's) and
which carries NO hidden rule (`listDirectory` owns it, behind `Show hidden`). An ignored file still owns its
name on disk, so **New Material, New Folder, Rename and Move list UNFILTERED**; `validateAssetName`'s
`IgnoredName` refusing a roster name as a TARGET is the opposite question and is correct (it refuses a folder
name too — it has no file/folder context). `BV9` pins the exact set of six editor files that may name either
predicate.

**`deleteOrphanMeta`'s `AssetPresent` REFUSAL IS A SCANNABLE NAME ON THE LEAF PLUS ANY EXISTING PATH (E.4.4,
the code-review round's blocking finding)** — `isScannableAssetName(assetLeaf) && fileExists(...)`. Widen it
back to "any file exists" and every pre-E.4.4 Blender project's orphaned `.blend1.meta` is undeletable for
ever, since the ignored backup beside it never goes away (`AA67`, `AD76`). Narrow it to an exact-case pairing
and the sidecar a case-only rename leaves on a case-insensitive volume — the only copy of its GUID — becomes
deletable. Feed it the relative path instead of the leaf and `tex/THUMBS.DB.meta` is refused again, because
an exact roster name stops matching behind a folder prefix (`AA67`'s nested arm).

**★ A CHANGE TO WHAT THE SCAN SEES MUST BE TRACED THROUGH EVERY CONSUMER THAT READS "A FILE EXISTS AT THAT
PATH" AS "A LIVE ASSET" (E.4.4).** The two were one fact until the roster split them, and the consumer that
still conflated them sat in code the task never touched — so the plan, the spec, 27 sabotage seeds and the
full gate all missed it. **A sabotage matrix mutates the code a task WROTE and cannot look anywhere else**;
only reading every consumer of the visible set finds this class.

**`ImGuiListClipper::IncludeItemByIndex` GOES AFTER `Begin()` (E.3.3)** — the constructor `memset`s
`DisplayStart` to 0 and `Begin` is what sets it to −1, so a call above it is an `IM_ASSERT` abort.

**AN API-POSITIONED POPUP IS NEVER CLAMPED (E.3.3)**, and one off the screen has its child **culled**, making
`ImGuiListClipper::Step()` return false immediately. `assetPickerAnchor` clamps both axes itself, and **any
future API-positioned window inherits it.**

**A LABEL IS TRUNCATED AT ITS FIRST `##` AND A `Selectable`'s ITEM BOX IS NOT ITS TILE**
(`imgui_widgets.cpp:7395-7396`) — **neither has automated cover anywhere.**

**A POPOVER'S LATCH REPORTS THE FRAME THAT DREW (E.2.4).** `CloseCurrentPopup` runs *inside* the popup's body,
so on the closing frame the popup **did** draw and the latch is still true; it reads false the frame after —
the same one-frame shape ImGui has for a click outside, which it closes at `EndFrame` after the draw walk.

**`normalize(Quat)` ASSERTS, SO A READ-SIDE CALLER IS A NEW HAZARD (E.3.1).** The per-axis reset path reaches
it **every frame a popup is open**, from the STORED value, so a non-finite rotation would abort the Debug
editor on a mere right-click. Guard with `std::isfinite` **first**, gated on `FieldKind::Quat` — a blanket
guard would kill `Vec3`'s NaN rescue, the one case where a live reset is exactly what the user wants.

**A PER-AXIS RESET IS DECIDED AT THE PRECISION THE ROW DISPLAYS; THE WHOLE-FIELD ONE COMPARES BITS (E.3.1).**
Two comparators on purpose, and the asymmetry is load-bearing: a `Quat`'s per-axis reset goes out through
euler and back through `fromEulerAngles` + `normalize`, which **perturbs the OTHER two axes by ~1e-7 every
time**, so a whole-value comparison leaves the menu entry live FOREVER. The whole-field arm writes the default
verbatim and is exact by construction, so it keeps `==`. **Right-click reset targets the component's
DEFAULT-CONSTRUCTED value** — `Transform::scale` resets to `(1,1,1)`, not zero.

**`AERO_LOG_WARN`'S FIRST ARGUMENT IS THE FORMAT STRING, AND A TWO-ARGUMENT CALL SILENTLY DROPS THE SECOND
(E.2.4).** `AERO_LOG_WARN("scene_render", "…")` logs the literal `scene_render` and discards the rest;
`scene_renderer.cpp:49`'s `"{}"` form is the correct shape.

**THE LOG CALLBACK IS ONE GLOBAL SLOT AND `EditorApp` CLAIMS IT (E.2.3).** A case installing `setLogCallback`
before constructing an `EditorApp` is silently displaced by the Console panel's sink and left with nothing
when the app clears the slot at teardown. **Any case observing a log record around an `EditorApp` lifetime
must install its callback AFTER the last `app.reset()`**, inside the scope that still owns the device.

**ONE UI SCALE SIZES THE STYLE, THE FONTS AND THE VIEWPORT'S CHROME (E.6.1, closing E.3.4's doubled style).**
`resolveUiScale(SDL_GetWindowDisplayScale, SDL_GetWindowPixelDensity, previous)` — quantised to 0.05 **by
division**, clamped to [0.5, 4], **no platform branch** — is 1 on a Retina Mac (the style used to be doubled
there, the font not) and on default Wayland (style doubled too), the desktop scale on Windows and X11.
**`style.FontScaleDpi` is its ONE stored copy; `io.ConfigDpiScaleFonts` stays OFF** (it reads the content scale,
1.0 on Cocoa). The style is **REBUILT, never rescaled**: `buildEditorStyle` (pure) → fresh `ImGuiStyle{}` →
`ScaleAllSizes` → every scaled member with a theme value ≥ 1 floored at 1, because `ScaleAllSizes` truncates and
below scale 1 would zero a border, `TabMinWidthBase` or `MouseCursorScale` (`I280`'s universal). `applyEditorStyle`
is the one writer; the scale is re-resolved **before every `NewFrame`**, rebuilding only on a change (`I282`). **A dp length reaches ImGui units ×
`uiScale` exactly ONCE, at the viewport's boundary**: a pure function consuming a dp constant takes a
**non-defaulted** `float uiScale` (`US9`'s set claim reads every public editor header), and a laid-out widget
carries its scale. **A width that holds text is a font multiple, never a literal.** **Still binding from the 2.0
measurement**: a panel with fixed chrome needs a MODE, a floor, and a threshold that keeps the derived height
**continuous** across the boundary — **and must RESERVE that chrome before sizing its flexible region**
(`assetBrowserLayout`, #111).

**A TUNING CONSTANT WRITTEN IN FONT UNITS IS CALIBRATED TO ONE FACE (E.6.1's validation pass).** E.3.1's label
floor, `5 × fontSize`, was tuned on ProggyClean at 13; in Plex at 16 it bound at 80 px and clipped the
Inspector's `0.000` in a 258-px dock where the branch point showed it whole — with every test green, because a
plan that re-measures what FAILS cannot see a constant nothing asserts. Fixed then as `4 ×` plus a one-point
letter gap; since E.6.3 the letter sits inside its box through the pure `inspectorAxisLayout` and the thresholds
are per entity (`TH11`, `I289`). **A font change re-measures every
font-unit constant that binds (label floors, tile multiples, layout thresholds) against the previous build at
the same dock size** — a branch-point A/B is the only witness.

**THE EDITOR DRAWS IBM PLEX WITH LUCIDE ICONS, AND AN EDITOR LITERAL STAYS 7-BIT (E.6.1, retiring ProggyClean
and #111's remap table).** Body (Plex Sans, ImGui size 16), Strong (SemiBold) and Mono (Plex Mono, no icons),
vendored unmodified (OFL-1.1, RFN "Plex"; `-text`, because the licence ships CRLF) and embedded through span
accessors; Lucide merged into Body and Strong **AFTER Plex** — Lucide maps `-`, digits and lowercase to icon-width
glyphs (`I277`). `editor_fonts.cpp` is the only `AddFont` TU, every add sets `FontDataOwnedByAtlas = false`, a
failed face fails `ImGuiLayer::create` with no fallback (`I286`), and `GetFontSize()` is never passed to
`PushFont`. **MSVC builds without `/utf-8`, so a narrow literal round-trips through CP1252**, where icon bytes are
undefined and a `\u` escape is unrepresentable: every `editor/` literal stays 7-bit and non-ASCII comes ONLY
through the hex-escape macros `AERO_GLYPH_*` (13 marks, all faces) and `AERO_ICON_*` (Body and Strong), spliced
BETWEEN literals — `GL6`'s lexer enforces it; comments are free. **`⌘ ⇧ ⌥ ⏎` do not exist in Plex**; a missing
glyph draws `U+FFFD`. **Never hold an `ImFontGlyph*` across another lookup** (a bake reallocates), and a claim
about where a glyph SITS measures its ink, not its box (`I278`). Full rules: `.claude/rules/editor.md`.

**`EDITOR_THEME` IS THE ONE SOURCE OF EVERY COLOUR, SIZE AND TYPE VALUE (E.6.1)** — a constexpr value in a PURE
public header; all 63 ImGui colour slots are mapped (a slot ImGui adds is a `static_assert`). **A new colour is a
theme token, never a file-local constant** (`I287` pins every panel's role reads). Category A (chrome, UI roles)
follows the mock; **Category B** (the viewport's colours, the axis trio, identity colours, clears) moved in
**byte-identical** (0 / 0 differing viewport pixels) and changes only as a stated decision — E.6.3 adopted the
mock's grey folders (`folder` + a new `folderTab`), retired `ThemeViewport::overlayText` with its last reader and
declined the mock's axis hues. E.6.3's `ThemePanels` holds every panel dp metric and the four panel colours no
palette token matches; `ThemePalette` stays 28 tokens. `TextDisabled` is for disabled widgets; de-emphasis is
`textMuted` (in a panel TU, `mutedText`; `I287` / `I333` ban the other spelling).

**A NON-DEFAULTED PARAMETER ON A WIDELY-CALLED EDITOR FUNCTION IS A 57-LINE EDIT, AND IT IS STILL THE RIGHT
CALL.** `buildSelectionOverlay` has **38 call sites, 37 in `selection_overlay_test.cpp`**; `projectToViewport`
7, `clipSegmentToNearPlane` 5, `gizmoOriginBehindCamera` 7, `overlayOwnsPress` 16. A default lets a future
site silently take the wrong arm — a wrong picture with no error and no failing test; non-defaulted makes
every unconverted site a compile error, which is what makes such a change atomic. **Pass a file-local
`constexpr auto` alias, never the full enum spelling** (seven characters per line instead of forty, which
keeps them under the 120-column CI skew). **And a mechanical rewrite must not count commas at bracket depth**
— `std::array<Entity, 1>{cube}` hides one inside a template argument list, which put the new argument one slot
early on 17 of 37 sites at E.1.3.

**ONE LEDGER, TWO STORES, ONE RELEASE SITE, AND A WALK THAT SEES EVERY ABSENT KEY (E.4.5).**
`ThumbnailService` owns one `ThumbnailLedger` and two backing stores — decoded images and rendered
materials — routed by `thumbnailSourceForName`, and every thumbnail texture is released through
`releaseKey`, which asks both stores. It has FOUR callers: the reimport clear, the superseded sweep, eviction,
and the walk's release of an `Absent` material key not drawn this frame. The produce walk asks for EVERY
`Absent` key and spends each budget as it meets a candidate of its kind: tiles drawn in one frame share
`lastTouched`, so the ledger offers them in GUID order, and asking for the sum of the budgets starves one kind
(`I233`). A material render is spent only on a tile drawn this frame (`I243`), and `residentCount()` /
`loadAttempts()` stay the decode store's. A third producer is a third `ThumbnailSource` enumerator, a third
store and a third line in `releaseKey`; `I242(a)`/`(b)` pin the sites as text.

**A RENAME KEEPS THE THUMBNAIL KEY — NEVER CACHE A NAME-DEPENDENT ANSWER UNDER A CONTENT KEY (E.4.5).** A
`ThumbnailKey` is `{Guid, ContentHash}`; E.4.3's rename carries the sidecar and the import cache is keyed by
GUID, so a renamed file keeps its key, and anything baked from the file NAME under that key describes a name
that no longer exists. The stem suppression runs at draw time against the current leaf through
`materialCardSubtitle`, in every host (`I240`, `I242(d)`).


## Open work in the editor

**Ownership of the open work.** **A containment check is picked by the SHAPE OF ITS INPUT, not by habit (E.4.3):**
an absolute path from a native dialog raises *"is this ABSOLUTE path inside that ABSOLUTE root"* and takes E.4.2's
`directoryWithin` / `normalizeForContainment`; a path the browser produced is assets-root-relative by construction,
so its question is *"is this relative path safe"*, and that is `validateRelativeAssetPath`: no `..` SEGMENT, no
leading `/`, no drive prefix, no backslash. **No reserved-destination policy exists for `<root>/Library/`** — E.4.3
writes the trash INTO it, and `Library/` is excluded from the scan, so the browser cannot show it and no move INTO it
is reachable through the UI; `IO19` and E.4.2's validation row 14 record today's permissive behaviour as the
"before", and that handoff is **still open**. **`assetReferenceFieldWidth` is ONE formula with TWO hosts**, so a
theme or DPI change moves the Inspector's reference row and the material slot's together. The default scene is
deliberately unchanged by every Phase E task, and its seventeen `entityCount() == 4` pins are byte-identical. The
New Project dialog's language control ships disabled for **4.6.1**, whose deliverable says so. **8.2** inherits
IBL/HDRI and the after-opaque sky variant (`SB9`/`SB16` are in place to catch a wrong ordering), plus physical
light units, IES profiles and area lights. What each Phase E task shipped, discharged and left out is in its
`docs/10` entry; the editor rules each one added are in `.claude/rules/editor.md`.

**ONE INSPECTOR-ROW GAP IS OPEN** — E.2.2's **unit-aware row** (`SpotLight`'s cone angles are raw radians
clamped to `[0, 1.5708]`), a reflect-gen surface first. E.2.1's **enum-aware row is CLOSED by E.5.2's
`AERO_LABELS`**. E.3.4 adds one gap of its own shape, unowned: a **closed `File` section**, which needs one flag
plus nothing else now that `requestMaterialSectionOpen` exists, the day a manual pass says the diagnostics are
noise.

**A MODAL'S DEFAULT BUTTON ANSWERS NEITHER KEY BY ITSELF — BIND BOTH, BY HAND (E.4.3's macOS pass, fixed in
#109).** Enter was dead in all three of the asset browser's modals until the pass measured it; **any new modal
inherits this — never write "Enter commits" without binding it.** The three things such a binding must get
right (`KeypadEnter` is a distinct key; `IsKeyPressed` passes `ImGuiKeyOwner_Any`; a commit key shares the
button's disabled predicate), each with its pinned-source citation, are in `.claude/rules/editor.md`.
