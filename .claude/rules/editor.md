---
paths:
  - "editor/**"
  - "tests/editor/**"
---

# Editor conventions

Split on 2026-10-27 so that no editor rules file is cut at the 150 000-character load limit: scene I/O and projects are in `editor-projects.md`, the asset pipeline in `editor-assets.md`, the importers in `editor-importers.md`, and the standing invariants in `editor-invariants.md`. A reference to a section may point into one of those files.

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

- `Begin`/`End`, `TreeNodeEx`/`TreePop`, `PushID`/`PopID` and **`BeginChild`/`EndChild`** are
  **1:1** — call `End*()` regardless of what `Begin*()` returned. `console_panel.cpp:107` states the
  contrast in its own words: *"EndCombo ONLY when BeginCombo returned true. The OPPOSITE of EndChild
  below."* The 1:1 rule is **"`EndChild` exactly once per `BeginChild`"**, not "always call
  `EndChild`" — a branch that calls **neither** satisfies it, which is what lets a panel put the pair
  inside one arm of a mode branch (task E.3.4).
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
- **A MODAL'S DEFAULT BUTTON ANSWERS NEITHER `Enter` NOR `Escape` BY ITSELF — BIND BOTH, BY HAND
  (task E.4.3's macOS pass).** `BeginPopupModal` always sets `ImGuiWindowFlags_Modal`
  (`imgui.cpp:13232`), `NavUpdateCancelRequest`'s popup branch excludes it (`imgui.cpp:15032`), and this
  editor never sets `ImGuiConfigFlags_NavEnableKeyboard` — so **`SetItemDefaultFocus()` carries
  nothing**. Escape has been hand-bound since 2.5.1; **Enter was NOT**, and all three of the asset
  browser's modals shipped an `Enter == X` comment that was never true until a validation pass measured
  it. **Never write "Enter commits" without an `IsKeyPressed` beside it.** Three things such a binding
  must get right: **`ImGuiKey_KeypadEnter` is a DISTINCT key** (`imgui.h:1678`) and is tested
  separately; **`IsKeyPressed(key, bool)` passes `ImGuiKeyOwner_Any`** (`imgui.cpp:10478-10481`), so it
  fires even though an active `InputText` holds `Shortcut(ImGuiKey_Enter, …, id)`
  (`imgui_widgets.cpp:5136`) — which is why the Escape binding already worked with a field focused; and
  **a commit key shares the button's own disabled predicate**, never a second copy of it, or the key
  commits what the button refuses. Dismiss wins over commit in a frame carrying both. **No tier can
  press a key on a modal**, so this has no automated cover anywhere — a validation row is its only
  witness.
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

## Fonts, icons and the theme (task E.6.1)

- **Three faces, embedded, IBM Plex** (`editor_fonts.{hpp,cpp}`, data under `editor/third_party/fonts/`):
  **Body** (Plex Sans Regular, 16) is ImGui's default font; **Strong** (Plex Sans SemiBold, 16) is for
  headings; **Mono** (Plex Mono Regular, 16) is for log text, paths and numbers and carries **no icons**.
  Lucide's icons are merged into Body and Strong at 11/16 of the face. Nothing else is added — ProggyClean
  and its Windows-1252 remap table are retired.
- **Plex is added BEFORE Lucide in every merge.** 1.92 asks a merge list in order, and Lucide maps `-`, the
  digits and the lowercase letters to glyphs of zero or icon width, so a Lucide-first merge draws text as
  icons or nothing. `I277`'s merge-order arm compares every printable ASCII glyph against the same face with
  no icons merged, exactly.
- **Every add sets `FontDataOwnedByAtlas = false`** — the data is static and ImGui must never free it
  (`ImFontConfig` defaults it to `true`). `I286` counts one per `AddFontFromMemoryTTF(`.
- **`editor_fonts.cpp` is the ONLY editor file that adds a font** (`I286`, a set claim), and `addEditorFonts`
  failing fails `ImGuiLayer::create` with one ERROR naming the face — **no fallback**: a silent fallback
  would ship broken fonts with every test green.
- **`PushFont(face, 0.0F)` keeps the current size; `PushFont(face, EDITOR_THEME.type.<x>Size)` sets an
  UNSCALED size; `GetFontSize()` (or a multiple of it) is NEVER passed to `PushFont`** — it is already
  scaled, so the UI scale would apply twice (`imgui.h:529-530`).
- **A glyph's quad is sized from the font's glyph BOX, which need not be tight, so a claim about where a
  glyph SITS measures its INK.** Every Lucide 1.49.0 glyph box reaches down to the baseline (`glyf`
  `yMin = 0`) whatever its outline does, so a quad centre tracks only an icon's top edge — a 2-point spread
  across the roster at body 16 that no glyph offset fits inside I278's per-icon bound. The harness reports
  the ink rows of the baked bitmap (`MeasuredGlyph::inkY0`/`inkY1`); `LUCIDE_GLYPH_OFFSET_Y = 1.0F` is
  measured on them, and its comment records the numbers.
- **Never hold an `ImFontGlyph*` across another glyph lookup.** A lookup can bake a glyph, which
  `push_back`s into `ImFontBaked::Glyphs` and may reallocate it — an earlier probe that kept the pointer
  read freed memory (ASan: heap-use-after-free) and reported three glyphs as "different" that are
  identical. Copy the values out immediately, as `measureEditorFonts` does.

### The theme and the UI scale

- **`EDITOR_THEME` (`editor_theme.hpp`) is the ONE source of every colour, size and type value.** A NEW
  colour is a theme token, never a file-local constant. Category A (the ImGui chrome and the UI roles)
  follows the mock; Category B (the viewport's 3D and overlay colours, identity colours, clears) moved in
  byte-identical and changes only as a stated visual decision. `TextDisabled` is for DISABLED widgets;
  de-emphasised enabled text is the `textMuted` role.
- **`buildEditorStyle` is the only style builder and `applyEditorStyle` the only style WRITER**
  (`editor_theme_ui.cpp`, `I286`). It starts from a fresh `ImGuiStyle{}`, assigns every size member and all
  63 colour slots from the theme, then `ScaleAllSizes(uiScale)` — so a scale change REBUILDS the style.
  **Never `ScaleAllSizes` a live style**: it truncates, so applying it twice compounds (`I280`'s
  idempotence arm). ImGui gaining or losing a colour slot is a `static_assert` failure at the table, never a
  silently default-coloured widget. **No member `ScaleAllSizes` scales whose theme value is ≥ 1 is ever
  truncated below 1**: it truncates, so at any UI scale below 1 a theme value in [1, 2) would become 0 — a
  1-dp border or separator vanishes, a 0 `MouseCursorScale` puts every tooltip and drag preview under the
  cursor, a 0 `TabMinWidthBase` drops the minimum tab width. The floored members are one table in
  `editor_theme_ui.cpp`, floored at 1 after the scaling (a theme value of 0 stays 0; a scale ≥ 1 is
  untouched). A scaled member whose theme value moves into [1, 2) joins it — `I280`'s sub-1 arm is a
  universal over every scaled member, not a list, so it is red until that member does.
- **The UI scale is `resolveUiScale(SDL_GetWindowDisplayScale, SDL_GetWindowPixelDensity, previous)`** —
  quantised to 0.05 by division, clamped to [0.5, 4], with no platform branch. It is 1 on a Retina Mac and
  on default Wayland, the desktop scale on Windows and X11. **`style.FontScaleDpi` is its ONE stored copy**
  (`currentUiScale()`), so nothing caches it. `ImGuiLayer::beginFrame` re-resolves it BEFORE every
  `NewFrame` and rebuilds the style only when it changes, with one INFO line (`I281`, `I282`).
- **`io.ConfigDpiScaleFonts` stays OFF**: ImGui's own path reads the display's CONTENT scale, which is
  1.0 on Cocoa and on default Wayland while the style would be doubled. `I286` pins that exactly one
  file writes it.
- **The viewport's chrome (D10): a length authored in dp reaches ImGui-unit space multiplied by `uiScale`
  exactly once, at the viewport's boundary; a length that reaches device-pixel space is additionally
  multiplied by the framebuffer scale.** A pure function that consumes a dp constant takes a
  **non-defaulted** `float uiScale` (`resolveGizmoScreenSize`, `scaledGizmoStyle`, `viewAxisLayout`,
  `viewAxisRect`, `buildSelectionOverlay` -- `US9` pins the set as source text), and a laid-out widget
  carries its scale (`ViewAxisLayout::uiScale`) so its hit test cannot use another. The panel captures
  `lastUiScale` in `onDraw` beside `lastFramebufferScale`, and nothing in `renderScene` calls ImGui. A width
  that holds text is a **font multiple**, never a literal. ImGuizmo's internal hit tolerances are library
  literals and stay unscaled (R7) -- `GIZMO_CENTER_HIT_HALF_EXTENT_POINTS` names one and is not scaled.

### The string-literal policy

The editor's fonts decide what it can **draw**; what it can safely **spell in source** is a different
question, answered by the compiler, not the font. **MSVC compiles this tree without `/utf-8`**, so it
decodes a BOM-less source file as the system code page (CP1252) and encodes narrow literals back to it. A
raw UTF-8 `…` or `—` survives that round trip **by accident** — every byte of `E2 80 A6` and
`E2 80 94` is a defined CP1252 character. **Icons do not survive it**: CP1252 leaves `0x81`, `0x8D`,
`0x8F`, `0x90` and `0x9D` undefined, and thirteen roster icons' UTF-8 contains one (`box` is
`EE 81 A1`, `save` `EE 85 8D`). A raw icon character in source is mojibake on Windows alone. **A `\u`
escape is no better**: in a narrow literal it is encoded to the *execution* character set, CP1252 on
MSVC, where a Private Use Area code point is unrepresentable. The policy:

1. **Editor source files stay 7-bit in every string and character literal.** Comments are unaffected — the
   tree's comments use `—`, `§`, `★` and `⌘` freely and keep doing so.
2. **A non-ASCII character in an editor string is spelled ONLY through a named macro** from one of two
   headers, each a string literal of **hex escapes**, so literal concatenation works
   (`AERO_ICON_FOLDER " Assets"`):
   * `editor/include/aero/editor/editor_glyphs.hpp` — `AERO_GLYPH_*`: the typographic marks the editor
     uses (ellipsis, em dash, en dash, middle dot, multiplication sign, degree sign, bullet, the four
     arrows, check mark, minus sign), each one Plex Sans **and** Plex Mono draw natively;
   * `editor/include/aero/editor/editor_icons.hpp` — `AERO_ICON_*`: the Lucide roster.
3. **Which characters exist**, exactly as `I279` asserts per face: all three faces draw Latin-1
   (U+00A0–U+00FF), Latin Extended-A (U+0100–U+017F), basic Cyrillic (U+0400–U+045F) and every
   `AERO_GLYPH_*` mark; Body and Strong draw every Greek letter (U+0391–U+03A9, U+03B1–U+03C9), and
   Mono only `π` (U+03C0) — a `Δ` or an `α` in Mono draws `U+FFFD`. **Icons only in Body and
   Strong** (`I278`). A glyph outside the loaded faces draws `U+FFFD` (`�`), not `?` — **user data**
   (file names, log text from the engine) is drawn as given and may hit it; editor-authored strings
   may not.
4. **`⌘`, `⇧`, `⌥` and `⏎` are NOT available** — Plex has none of them. A future shortcut hint spells
   `Cmd+`, `Shift+` in ASCII, or uses an icon (Lucide has `command`), and that is E.6.2's decision.

- **`GL6` is the enforcement**: a tier-0 lexer over every `editor/src/*.{cpp,hpp}` and
  `editor/include/aero/editor/*.hpp` that walks string, character and raw-string literals, skipping
  comments, and fails on a byte ≥ `0x80` inside a literal, a `\x` or octal escape ≥ `0x80` outside the two
  macro headers, any `\u`/`\U` escape, and an unterminated literal (so a lexer that mis-reads a quote is
  loud, never silent). Its self-tests carry the tree's own traps: a digit separator (`1'000'000`), a quote
  character (`'"'`) and a raw string holding a quote (`asset_actions.cpp`'s `R"(*?"<>|:)"`).
- **A hex escape is greedy — splice a macro BETWEEN two literals, never inside one.** `"\xA6and"` reads
  `\xA6a`; `AERO_GLYPH_ELLIPSIS "and "` is two literals the compiler joins after escape processing.
- **`AERO_GLYPH_*` for typographic marks Plex draws in all three faces, `AERO_ICON_*` for Lucide glyphs,
  which draw in Body and Strong only.** An icon is named by its UPSTREAM Lucide name; what it means is the
  consumer's own table (`createKindIcon` is the first). A new icon is one macro and one `EDITOR_ICONS` row
  — `GL2`, `GL3` and `GL4` check the bytes, the name and the upstream code point.

## Undo/redo

- **`CommandStack::push()` APPLIES the command** (task 2.4.1 D5). A caller must NOT have already
  written the edit — there is exactly one write path and it runs inside `Command::redo()`. Wrapping an
  existing direct write means the write **moves into** the command, not that a command is pushed
  beside it.
- **The merge chain** is open only between one continuous gesture's start and end. It is broken by
  `undo()`, `redo()`, `clear()`, `setClean()` and the explicit `breakMergeChain()`, and opened by any
  push that records a new entry. A panel driving a continuous edit breaks the chain at **both**
  boundaries and at **every** path that abandons the gesture — `ViewportPanel::updateGizmo`'s three
  early returns are the precedent, and they deliver no end edge at all. **But a return that runs while
  no drag of its own is in flight must leave the chain alone** (task E.6.2's `fix(editor):`): the returns
  run on EVERY Viewport frame while the primary has no `Transform` or sits behind the camera (and, since
  E.6.2, under Select), so an unconditional break there split every Inspector drag into one undo entry per
  frame. Each return breaks only `if (gizmoWasUsing)`, read before the latch is cleared (`I290`, `I291`).
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

**A labelled integer is a named selector, chosen by metadata alone (task E.5.2).** `namedSelectorRow`
(`inspector_model.hpp`) re-checks at runtime every condition reflect-gen enforces for `AERO_LABELS` —
Int/UInt, 1..64 valid distinct identifiers, `AERO_RANGE(0, N-1)` exactly — and answers nothing otherwise,
so the drag stays the fallback and a hand-built or skewed `FieldUiMeta` can never draw a lying dropdown.
The panel's Int and UInt arms ask it first; the row is never chosen by a component or field name
(`I253(e)`). **A pick is a discrete write through `resetField`**, and the current value and an
out-of-range index push nothing. The test seam (`requestNamedSelection`) lives exactly one tick —
`EditorApp` expires it after `drawShellUi` — because a panel that did not draw would otherwise apply a
stale request the first time it is shown.

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

  | kind \ surface | HierarchyRow | HierarchyVoid | Viewport | MaterialSlot | AssetField |
  |---|---|---|---|---|---|
  | Model | Instantiate | Instantiate | Instantiate | None | AssignRef* |
  | Material | Assign **iff** the target has a `MeshRenderer` | None | Assign **iff** the hit has one | None | AssignRef* |
  | Texture | None | None | None | BindTextureSlot | AssignRef* |
  | Audio | None | None | None | None | AssignRef* |
  | Folder / Text / Unknown | None | None | None | None | AssignRef* |

  **(*) `AssignAssetReference` iff `assetKindIsDraggable(kind) && (!fieldKind || *fieldKind == kind)`**
  — ONE expression, written into every kind's arm, never short-circuited for the kinds that happen to
  refuse today. Folder/Text/Unknown reach it too and are refused BY IT, which is what makes widening
  `assetKindIsDraggable` (for a future `Text` reference) ONE edit that widens the drag source, the
  token vocabulary and this surface together. `fieldKind` is the kind the field's `AERO_ASSET` names,
  `nullopt` for an unannotated field AND for every surface that is not a field, and it is
  **NON-DEFAULTED** (E.1.3's rule): a default would let a future `AssetField` site forget it and
  silently become unconstrained, while non-defaulted makes every unconverted site a compile error.
  There are **six** production call sites, and two of them are the drop drains in `editor_app.cpp`,
  which recompute the action from the live `World` at drain time.

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

## The asset-reference field (task E.3.3)

**A FIELD HAS EXACTLY ONE ASSIGNMENT SURFACE AND IT IS `drawAssetReferenceField`'s.** A panel that wants
a reference field calls the widget; it does NOT write its own `BeginDragDropTarget`. 3.1.5's D14 ("no
InputText, no drop target on a `Guid` row") is **superseded by an affordance, not contradicted** — there
is still exactly ONE accept rule rather than a fourth surface with rules of its own, and
`inspector_panel.cpp` still names no ImGui drop API at all, which `IR8` clause (a) keeps pinning. The
peek rule is the widget's, once, for both hosts, and `IR8` clause (b) pins it as an **ORDERING** —
classify BETWEEN the begin and the accept — because a membership pin cannot see which side of the accept
the decision fell on (3.4.2's `I96` lesson).

**THE KINDS A FIELD MAY NAME ARE EXACTLY THE KINDS A DRAG MAY CARRY.** `assetReferenceKindFromToken` is
DERIVED from `ASSET_KIND_FILTER_OPTIONS` ∩ `assetKindIsDraggable`, never a list of its own, so a kind
added to one set and not the other is a red test rather than a silent asymmetry.

**THE WIDGET HOLDS NO DECISION.** Every predicate is `asset_picker_model`'s, and `asset_picker.cpp`
states no `AssetKind::` literal at all — the kinds come from the rules, which come from the annotation,
which comes from the component. That is the mechanical form of ADR-004's genericity claim for this
widget, and `I166(c)` pins it.

**THE OBSERVABLES AND THE FOUR LIVE ONE-SHOTS ARE OWNER-KEYED, A LIVE ONE-SHOT IS DROPPED WHEN NOTHING
IS OPEN, AND A TICK IN WHICH NO FIELD CLAIMED THE POPUP RESETS THEM FROM OUTSIDE THE WIDGET.**
`MeshRenderer` draws TWO `Guid` rows every frame, so a per-field write has the second clobber the first
in the same frame. And a one-shot that merely waits for a popup is a **delay line**: it fires on the
frame the next popup appears. An unmoved undo count cannot see that — the discriminator is that the next
open **stays open**.

**The third clause is what owner-keying costs.** The owning row stops being drawn the moment the
selection moves to an entity without that component, or its panel is tabbed away; ImGui closes the popup
because its window goes unsubmitted, and **a widget that is not running cannot write the observable that
says so**. The widget sets a seen-this-tick stamp and `EditorApp::tick()`'s post-draw slot consumes it —
only a slot that runs after the WHOLE draw walk can know nobody claimed it. Without that reset,
`assetPickerOpen()` stays frozen at the last frame that DID draw and `openFieldKey` never empties, so the
one-shot drop never runs again. **`MeshRenderer` hides the second half of this**: its `material` row
performs the "nothing is open" clear on the `mesh` row's behalf one tick later whatever the post-draw
slot does, so a case written over it decides nothing — `I169`'s arm (b) uses a single-`Guid`-field
component for that reason.

**A LABEL IS TRUNCATED AT ITS FIRST `##`, AND A `Selectable`'s ITEM BOX IS NOT ITS TILE.** Two draw-list
rules, and **neither has automated cover anywhere** — nothing in this tree reads rendered text or measures
a widget rect, and both seeds come back green. (a) A value sentence goes through
`ImDrawList::AddText`, clipped to the widget's own rect, and the `Button`'s label is the `###` id ALONE:
`FindRenderedTextEnd` stops at the first `##` (`imgui.cpp:3918`), so `readme##v2.png` renders as `readme`
and `##notes.png` renders as nothing at all. An empty visible label still measures one line high
(`imgui.cpp:6447-6448`), so a `ImVec2(w, 0)` button keeps its frame height, and `AddText` submits no ImGui
item, so the last-item data still names the button for `BeginDragDropTarget` and the anchor.
`asset_browser_panel.cpp:635-637` has carried the same rule for its tile caption since 3.1.3 (E19). (b) A
tile's origin is `GetCursorScreenPos()` taken **before** the `Selectable`, never `GetItemRectMin()` after
it — a `Selectable` extends its item box on the MIN side by half the item spacing
(`imgui_widgets.cpp:7395-7396`), which is half a spacing of drift in both axes.

**AN API-POSITIONED POPUP IS NEVER CLAMPED BY ImGui, AND ONE OFF THE SCREEN HAS ITS CHILD CULLED.**
`Begin`'s visibility clamp runs only `if (!window_pos_set_by_api && …)` (`imgui.cpp:8279`), and a culled
child makes `ImGuiListClipper::Step()` return **false immediately** — no tile submitted, no key noted, no
error anywhere. `assetPickerAnchor` therefore clamps BOTH axes itself, applied to the window's effective
top so the `(0,1)` flip pivot is honoured. **Any future window this tree positions by API inherits this.**

**`ImGuiListClipper::IncludeItemByIndex` GOES AFTER `Begin()`, NEVER BEFORE IT.** The constructor
`memset`s `DisplayStart` to 0 and `Begin` is what sets it to −1, so a call placed above `Begin` trips
`IM_ASSERT(DisplayStart < 0)` (`imgui.cpp:3418`) — an **abort** in the Debug build — and dereferences a
null `TempData`. The header's "before the first call to `Step()`" reads as if above `Begin` were safe.

**SHOWING A HIDDEN PANEL CLOSES AN OPEN POPUP**, because ImGui focuses a window that becomes visible and
`FocusWindow` closes every popup above it (`imgui.cpp:13740`). A case that re-shows a panel and then
measures a popup is measuring one that is no longer there.

Full history: `docs/10-engineering-log.md`, Epic 2.1 / 2.2 / 2.5 / 2.6 entries, and tasks 3.1.1, 3.1.2,
3.1.3, 3.1.4, 3.1.5, 3.2.1, 3.2.2, 3.2.3, 3.2.4, 3.2.5 and 3.4.2's entries under Phase 3.

## Context routing (task E.3.2)

**The editor has ONE focus slot**, in `shell_ui.cpp`, immediately before `DockSpaceOverViewport` —
dock nodes update inside it, so the focus lands with no one-frame lag. Both paths resolve there: an
explicit `requestPanelFocus` first, then the pending context route. `ImGui::SetWindowFocus` is called
**at most once per frame** from that block, and `I159` pins that exactly one file names it and calls
it exactly three times. **A fourth caller elsewhere is a second focus policy with no way to order it
against the first.** **An automatic route selects the target's dock TAB and leaves `NavWindow` alone**
(`TabBarQueueFocus`, decided by the pure `routeRaise`); it moves the keyboard only when the keyboard is
in a sibling tab of the target's node, which ImGui re-selects every frame (`imgui.cpp:19611-19613`), or
before the node's tab bar exists. The explicit path keeps `SetWindowFocus`. `I256` is the witness;
`I159(g)` pins that `TabBarQueueFocus` and `BringWindowToDisplayFront` are each spelled once and
`ImGui::FocusWindow` nowhere.

**`ImGui::SetWindowFocus` is not free and not idempotent.** `FocusWindow` closes every popup above the
focused window (`imgui.cpp:13740`) and **steals the active widget** (`imgui.cpp:13754-13756`), with
ImGui's own comment at `:13751` naming exactly our slot: *"Focus a window while an InputText in
another window is active, if focus happens before the old InputText can run."* The edit is **lost**,
not interrupted — `MaterialPanel`'s name field commits only on `IsItemDeactivatedAfterEdit()`, and the
panel that lost the tab never draws to observe the edge. **Re-read `:13740` and `:13754` at every ImGui
bump**, beside the existing `ImGuizmo.cpp:1229-1230` and `imgui.cpp:8848` rules. **Hiding a tab has the
same cost for that tab's active widget**: it stops being submitted and `NewFrame` clears it
(`imgui.cpp:5795-5798`), which is why `textInputActive` stays a Hold.

- **Only the `ImGuiTabItem*` overload of `TabBarQueueFocus`** — the `const char*` one asserts on a dock
  node's tab bar (`imgui_widgets.cpp:10384-10389`).
- **`EditorApp::keyboardFocusPanelId()`** is the only automated window onto `NavWindow`; any claim that a
  panel keeps or loses the keyboard is a delta on it across one tick, with the explicit path as its
  anti-vacuity control (`I258`).

**Drops are tested before Holds, and the order is the contract.** The five terminal conditions —
nothing pending, the preference off, the source gone, the target hidden or unregistered, an explicit
focus this frame — can each persist indefinitely, so holding on one would hold forever. The four
transient ones — a text field has the keyboard, a drag payload is live, an ImGuizmo drag is in flight,
a popup is open — all end on a mouse-up, a click-away or an Escape. Never reorder a Drop below a Hold.
**"A popup is open" also covers the keyboard still sitting in a popup that has just CLOSED**
(`NavWindow`'s `RootWindow` carries `ImGuiWindowFlags_Popup` — the root, so a child window of the popup
counts, and a docked panel is its own root): a menu click closes the menu, but `NavWindow` moves off it
only in the next frame's `NewFrame` (`imgui.cpp:5949-5950`), so a route decided in between selects the tab
while the keyboard is about to move, and a Material tab that gets the keyboard back takes the node
straight back (validation row 8, measured). ImGui ends that Hold by its own rule one frame later;
`I159(b)` pins the term.

**A route never re-opens a panel the user closed.** `targetAvailable` is *registered AND visible*, and
a hidden target is a **Drop**, not a Hold. The explicit path may `setVisible(true)`; the automatic one
may not. Closing a panel is the user's second, coarser off switch.

**A preference that suppresses an EFFECT must not suppress the OBSERVATION that feeds it.** The three
`observe*` functions gate the **latch**, never the early return: a baseline advances every tick whatever
the preference says, so an act performed while routing is off is seen and forgotten rather than skipped.
Gating the early return leaves the baseline stale, and the first observation after the preference comes
back on raises a panel for a minutes-old act. **`observeImportTarget` keeps `!settled` in its early
return** — that rule is the session's and is independent of the preference.

**`readEditorPrefs` must distinguish "missing" from "exists and cannot be read".** `readTextFile`
disengages its `text` for a missing file, a directory and a permission-refused file alike, so the read
alone cannot tell the normal state on a fresh machine from a file the OS refused. `fileExists` is the
discriminator — and it is `std::filesystem::exists`, so it is **true for a directory**, which is what
makes a directory the portable stand-in for an unreadable file in a test.

**`Selection::prune` must never bump `Selection::revision`.** `HierarchyPanel::onDraw` prunes every
frame, so a bumping prune is a permanent focus storm. And **`set`, `toggle` and `setAll` delegate to
two private, non-counting helpers**, not to `add`/`remove`: the counter counts public CALLS, so
routing them through the public mutators would make `set` bump twice and `setAll(n)` bump n+1 times.

**The router derives nothing.** It never spells `isImportableModelName` or `isBlendFileName`; it reads
the `SessionState` the import session itself wrote, which costs one extra tick and buys a router with
no second copy of a predicate. If the import route ever has to be one tick, the fix is a settled-state
signal on the session, published before `ShellUiState` is built — never a predicate here.

**`RouteSource` gets no `toString`** (doctest's `DOCTEST_STRINGIFY` expands to an unqualified
`toString`, which ADL would find), and its **numeric order IS its priority**, so a new source is
**inserted at its position**, never appended.

**`editor_prefs.json` is where a per-user editor taste goes** — not `project.json` (committed and
shared) and not `editor_tools.json` (written whole from a fresh struct by `setOverridePath`, so a
second key there is clobbered by every `Locate…`). An absent key is its default, so appending one is a
key and a line with no version bump. Its path resolves **only when `persistLayout` is true**; an empty
path means read nothing and write nothing.

**Two measurements any GPU-tier routing case inherits.** `Material` is the Right dock node's **default
front tab**, so `panelDrawnCount("Material") > before` is satisfied with or without a route — put
something else in front with an explicit request first, and `REQUIRE` that the target is not drawing.
And **every Right-node panel draws once on the very first frame**, before the dock node has selected a
tab, so take every baseline **after** the two settle ticks.


## A disclosure a seam must be able to drive (task E.3.4)

`ImGui::SetNextItemOpen(open, ImGuiCond_Always)` + `ImGui::IsItemToggledOpen()`, with the **panel**
owning the flag — never ImGui's storage. `TreeNodeUpdateNextOpen` takes `is_open` straight from
`NextItemData.OpenVal` under `ImGuiCond_Always` (`imgui_widgets.cpp:6817-6823`) and the click path
still runs, flipping it and raising `ImGuiItemStatusFlags_ToggledOpen` (`:7073-7078`), which
`IsItemToggledOpen` reads (`imgui.cpp:6590-6594`). **A forced-open node is still clickable.**

Why it matters: **no tier in this tree can click a disclosure**, so a closed node's branches execute on
no lane at all. If a closed-by-default node hides widgets that need CI coverage, the seam is not a
convenience — it is the price of closing the node.

**AND THE SEAM'S OWN ACCESSOR CANNOT BE THE TEST.** Reading back the flag the seam just wrote is a
round trip: it reports what was *requested*, never whether ImGui *obeyed*. Sabotage seed `S26`
(`ImGuiCond_Once` instead of `Always`, which hands the decision back to ImGui's storage so the node
never opens) walked through **149 green assertions** for exactly that reason. Assert a **consequence the
widget produced** — `MaterialPanel::samplerRowsDrawn()` counts disclosures that actually submitted their
rows, because a node that did not open submits none.

## A panel with fixed regions (task E.3.4)

A fixed region's height is a **pure function** with a tier-0 battery, never arithmetic inside the draw
walk. The scrolling body is `BeginChild(id, ImVec2(0, -footerHeight))` — a **negative** height, so
`CalcItemSize` resolves it as `ImMax(4.0f, avail.y + size.y)` (`imgui.cpp:12344-12345`) and ImGui's own
remainder is authoritative: a one-pixel error costs the child a pixel instead of clipping a button off
the bottom. (The Asset Browser's panes are floored at a positive explicit height for the **opposite**
requirement — two side-by-side panes sharing one explicit height — and both are right. Since E.4.4's
validation fix that floor lives in `assetBrowserLayout` (`asset_view.hpp`), which reserves the footer AND
the Issues region before sizing the panes and caps the Issues body in a scrollable child — this section's
second instance. Since E.6.3's code-review round the floor is ImGui's 4-point child — see the fold below.)
**A fixed row drawn INSIDE a flexible region's height is chrome too**: E.6.3's crumb row, its rule and their
spacing sit inside the contents pane above a child, so a pane shorter than them grew the row and pushed
the Issues region and the footer off the panel while the budget still claimed a fit. Record such a row's
height as a metric the budget reads, so the budget knows the shortest region that holds it: that chrome +
the child's minimum (`AV66`, `I231`).
**And below that height the row FOLDS, rather than the region being floored at it** (E.6.3's code-review
round, rounds 2 and 3): flooring the Assets panes at chrome + 4 left the editor's own 1280 x 720 launch scrolling
with the Issues list open and no fit anywhere in 640-776-point windows. `assetBrowserLayout` answers
`crumbRowShown == (R* >= paneChromeHeight + frameHeight)` and `paneHeight == max(R, 4)` in BOTH modes, `R` the
remainder after the footer and the Issues region and `R*` the remainder the Issues list would leave OPEN at its
one-row floor, **whatever its state**. So **the row never shows over less than one frame height of contents**,
**the mode is the same for both Issues states**, and closing the list never shrinks the asset list. One frame is
a height, not a whole entry: in List view the `##contents` child opens with its frozen column-header row
(`FontSize + 2 x CellPadding.y`, `imgui_tables.cpp:3118`, 22 at 1x), so at the threshold it shows the headers and
the top 2 points of the first entry; in Grid view, the top of the first tile row. The frame term is
`max(frameHeight, 4)`, ImGui's child, so a frame that reads 0 never shows the row over a group taller than its
pane (`AV66`).
A threshold of the floor itself (round 2's `R >= chrome + 4`, read off each state's own `R`) showed the row over a
4-point list and, at the launch, over a 12-point list with the Issues list closed where the open list folded it
over 27. The mode moves no height in the budget, so the pane height is **continuous** across the fold; folded,
the panel submits no crumb, no crumb drop target and no rule, the `##contents` child takes the whole pane, and the
tree still navigates. The Issues body yields to the same `max(ASSET_PANES_MIN_FONT x fontSize, chrome +
frameHeight)` in both modes, since a body that grew on the fold would take the row's height straight back
(`AV67`). **Only the pane is continuous**: the `##contents` child is the pane less the chrome while the row shows
and the whole pane once it folds, so it steps by the chrome at the threshold — by construction, at any threshold
(24 → 60 points at 1x with the list open, `AV67`).
`assetContentsPaneLayout`'s `crumbRowShown` is non-defaulted. The row shows from 143 points at 1x with Issues, open
or closed, and from 91 without; the panel fits from 34 / 64 / 86 (no Issues / closed / open) — the pre-E.6.3
bounds + 3 (`AV67`, `AH18`, `I231`).
**And that minimum is ImGui's, not the one you asked for**: `CalcWindowMinSize` (`imgui.cpp:7007-7029`,
applied at `:8219`) holds a child window WITHOUT `ImGuiChildFlags_ResizeY` to at least
`IMGUI_WINDOW_HARD_MIN_SIZE` (`imgui_internal.h:2813`), 4 points at every scale, and to at least the
rounding term `max(0, style.WindowRounding - 1)` (`:7027` — the WINDOW rounding, even for a child;
`BeginChild` always sets `NoTitleBar` (`imgui.cpp:6744`); a child given `ImGuiWindowFlags_MenuBar` adds its
`MenuBarHeight` to the minimum too (`:8134`)), so for a child with no menu bar the 4 holds while the scaled
`WindowRounding` is at most 5 (the theme's is 0; `asset_browser_panel.cpp` static_asserts the macro and the
term at `UI_SCALE_MAX`). **A `ResizeY` child's minimum is `style.WindowMinSize.y`** (32 dp) instead
(`:7017`) — chrome reserved above one is floored at that, not at 4. So a child asked for 1 is drawn 4 tall
— a floor of chrome + 1 left the group 3 points taller than its pane, which only a drawn comparison at the
floor could see (`I231`'s 640 subcase).
**Never pass a zero height meaning "nothing"**: `CalcItemSize` reads zero as *use the whole remaining
region*.

**Nothing that wraps may live in a fixed-height region.** A wrapped line makes the region's height a
function of the panel's *width*, which makes every derived size a function of the panel's width — and if
one of those sizes is a render target's, dragging the dock divider sideways reallocates it. Draw one
always-reserved line instead, empty when there is nothing to say.

**`Separator()`'s layout height is `max(style.SeparatorSize, 1)` and must be READ, not stated.** 1.92.8
removed the "a 1 px separator does not move the cursor" hack (`imgui_widgets.cpp:1703-1708`) while
`SeparatorEx`'s own header comment at `:1657` still describes it, and `ScaleAllSizes` scales the value —
measured at **2** on a Retina display, where this editor calls it unconditionally
(`imgui_layer.cpp:87-89`). **A footer that follows a child needs THREE `ItemSpacing` terms**, because
`EndChild` calls `ItemSize(child_size)` (`imgui.cpp:6860`) and `ItemSize` advances by
`line_height + ItemSpacing.y` (`:12129`); `imgui.cpp:437` spells the recipe.

**★ AND A BODY THAT IS SOMETIMES A CHILD IS TWO SETS OF WIDGET STATE.** `TreeNodeSetOpen` writes through
`g.CurrentWindow->DC.StateStorage` (`imgui_widgets.cpp:6802`) and `DC.StateStorage` is
`&window->StateStorage` (`imgui.cpp:8581`) — **per window**, and a child is a distinct window. So the
same `CollapsingHeader` is a different entry inside the child than it is in the parent, and crossing a
mode boundary restores every collapsed section to `DefaultOpen`. One function called from two places
keeps the two modes drawing the same **code**; that is not the same as the same **state**. **Anything
ImGui keys by window must be a panel member if it is to survive the flip.**

## The Create menu (task E.5.2)

**One pipeline: the hosts RECORD a `CreateKind`, and only `EditorApp::applyCreate` places, pushes, frames
and reveals.** The menu bar (`ShellUiState::createRequest`, copied after `drawShellUi`) and both Hierarchy
context menus (`pendingCreate`) write a one-shot; the reconcile block drains both, after the drop drain and
**before** the context router, so the Inspector rises in the tick a create lands. A new entry is **one row
in `createMenuEntries()` plus one arm in `createSeed()`** — `create_menu_ui.cpp` walks the table and names
no kind. A create is the **seeded `CreateEntityCommand`**, one undo step with typed writes inside its first
`redo()`; never a panel write, never `AddComponentCommand` + `SetFieldCommand` (which need meta and would
split the undo). Every typed create is at the **root**; Create Child is the one parenting create.

**Framing: a box is fitted exactly as `F` fits it; a point is recentred, never zoomed**
(`EditorCamera::frameCreated`). `F` itself is unchanged. `ViewportPanel::frameCreatedEntity` is the editor
camera's **second writer** and runs from the reconcile block, **before the draw walk** — never from
`renderScene` (INV-3). Undo does not move the camera back and redo does not re-frame: the camera is tool
state.

## Shell chrome (task E.6.2)

- **The toolbar and the status bar are viewport side bars**, `ImGui::BeginViewportSideBar` on the main viewport
  (`"##AeroToolbar"`, `ImGuiDir_Up`; `"##AeroStatusBar"`, `ImGuiDir_Down`), drawn every frame from
  `shell_chrome_ui.cpp` — the chrome's ONE ImGui TU. They reserve their space through the work area exactly as the
  menu bar does, with the same one-frame settle; never "fix" it with a manual offset. **`BeginViewportSideBar` does
  not call `End()`**: the caller ends the window unconditionally (only `BeginMainMenuBar` wraps it). Neither is a
  `Panel` (no View-menu entry, no ini key; the `panels().count() == 8` pins stand).
- **Six flags on both bars, and each has a reason**: `NoSavedSettings` (not forced by the side bar — without it a bar
  enters `aero_editor.ini`), `NoScrollbar`, `NoScrollWithMouse`, `NoFocusOnAppearing`, `NoNavFocus` (Ctrl+Tab runs
  even with `NavEnableKeyboard` off and would list a bar as "(Untitled)") and `NoNavInputs` (Tab-to-focus is always
  on). `I307(d)` pins the set.
- **Order in `drawShellUi`**: the hoisted `fileEnabled`, then `drawMenuBar` (which draws the breadcrumb after the View
  menu), then `drawToolbar` — after the menu bar (Up bars stack in submission order), before `applyHistoryRequests`
  (an Undo click applies this frame) and before `drawPanels` (a tool click reaches `updateGizmo` this frame) — then
  `drawStatusBar` (its position is a convention: the inset lands next frame either way). `I308` pins it.
- **Bar heights are whole points**: `shellBarHeight(heightDp, uiScale)` rounds, because the side bar insets the raw
  float while `Begin` truncates `Pos`/`SizeFull`. The heights are DERIVED in dp (`toolbarHeightDp`, `statusBarHeightDp`
  — 44 and 26); each bar's 1-dp rule is drawn inside it.
- **A toolbar button never takes the keyboard**: every enabled button is wrapped in
  `PushItemFlag(ImGuiItemFlags_NoFocus, true)` / `PopItemFlag()`, 1:1, so the Hierarchy's focus-scoped Delete /
  Cmd+D / F2 survive a tool click. Stated residuals: the snap field (`DragScalar` focuses), a disabled button, empty bar
  space and the whole status bar DO focus their bar. A click on an enabled button while another widget is active (a
  Hierarchy rename) ends that widget AND acts, and the keyboard stays in the panel -- MEASURED on macOS (E.6.2's
  validation row 3, three tools, three times), against a reading of ImGui's hover rule that predicted a second click
  would be needed: trust the measurement, re-measure at an ImGui bump. No hand-back call exists, because it would be
  a second focus writer against E.3.2's one slot.
- **THE TRANSFORM-TOOL STATE HAS ONE HOME AND TWO RECONCILED POINTERS.** `EditorApp::transformTools`
  (`TransformToolState`: the tool, the space, the snap settings) is the only value; the Viewport's `toolStatePtr` is
  re-handed every tick above `layer.beginFrame()` and `ShellUiState::tools` is rebuilt in the designated initializer —
  never bound once in `create()`, because the app is moved out of `create()`'s optional. `PanelContext` is untouched
  (2.6.2's append bar). Writers: the toolbar, the Viewport's keys, and `create()`'s snap restore. `I292(d)` and
  `I307(i)` pin who may NAME each piece.
- **Select draws no gizmo**, through a third early return in `updateGizmo`, shaped like the no-target one; the keys
  are inert mid-drag (`!ImGuizmo::IsUsing()`) and under Cmd/Ctrl or Super (`GizmoModeInput::commandModifierHeld`,
  decided in the pure model so `G21` can witness it).
- **A REQUEST SEAM OBEYS ITS CONTROL.** Every toolbar control is written in one shape — the enabled predicate as a
  named local, the request read on its own statement, the widget ALWAYS submitted, then
  `pressed || (requested && enabled && noModal)` with `noModal = ImGui::GetTopMostPopupModal() == nullptr`. Never
  `enabled && (Button(…) || request)`: the short-circuit skips the widget. A refused request is DROPPED, never
  deferred. The pending members live on `EditorApp` and are cleared right after the `ShellUiState` build — clearing
  `ui`'s copy would make nothing one-shot (`I300`).
- **The snap commit rule is pure** (`snapStepUpdate`): a live drag frame changes the step and NEVER commits; the field's
  `IsItemDeactivatedAfterEdit()` or a request commits; the chrome calls it once per frame. A commit marks
  `editor_prefs.json` dirty and the NEXT tick's flush writes it, once (`editorPrefsWriteCount()`).
- **One chord spelling**: every hint for an `ImGuiMod_Ctrl` chord comes from `chordHint(currentHostOs(), …)` —
  `Cmd+Z` on macOS, `Ctrl+Z` elsewhere — and `modifierName` for prose. `shortcut_hint.cpp` is the ONLY editor source
  whose literals say `Ctrl` or `Cmd` (`HK4`), and no call site spells a `HostOs::` value (`HK5`).
- **The breadcrumb is one frame behind a save, an undo or a scene swap**: the menu bar draws before `applyFileRequests`
  and `applyHistoryRequests`. A GPU case asserting it ticks twice after the change.
- **User strings in the chrome are drawn format-safe** — `TextUnformatted`, `ImDrawList::AddText`, or a `"%s"` format
  (`I307(h)` runs `I242(e)`'s parser over the chrome) — and elided only through the pure layouts, on code-point
  boundaries (`FT15`).
- **The chrome reads every palette role once, in `chromeColors()`**, and never `textDisabled`; an active segment or
  tool pushes all THREE button slots plus the text slot (onAccent on `hover` is 1.34:1). `I287` pins the reads.
- **`US9`'s map** now includes `breadcrumb.hpp` (`P`), `status_bar.hpp` (`P`) and `toolbar_model.hpp` (`PP`): a new
  pure layout that consumes a dp metric takes a non-defaulted `float uiScale` and joins the map.

## Panel chrome (task E.6.3)

- **ONE CARD PAINTER, `panel_chrome_ui.cpp`, and its four rules.** A card's header item is a framed `TreeNodeEx`
  submitted under `Header*` and `Text` cleared through `clearOver` and `FrameBorderSize 0` (a framed node strokes
  `ImGuiCol_Border` otherwise, `imgui.cpp:4063-4074`); the painter draws its own chevron, icon and title from ONE pure
  `cardHeaderLayout`, never from ImGui's arrow box. Every fill, rule and outline is drawn from the x span recorded at
  `begin` -- never `GetItemRectMin/Max`, which includes the framed overhang `IM_TRUNC(WindowPadding.x x 0.5)`
  (`imgui_widgets.cpp:6906-6911`). The vertical layout is NET of `ItemSpacing.y` (`imgui.cpp:12129`): the first body
  item is placed `cardPaddingY` below the strip and the next card is reached by ONE `Dummy` of the net height, never a
  bare `SetCursorScreenPos` (`:8839-8840`). The body is narrowed on BOTH edges -- `WorkRect.Min.x`, `WorkRect.Max.x`,
  `ContentRegionRect.Max.x` -- and all three are restored at `end`: the TU's one internal write. The body is also
  CLIPPED to that width, because a line or a button row that does not wrap ignores all three and drew through the
  padding and over the outline (V4, `I348`): `ImGui::PushClipRect` -- never the list's own, since only it also sets
  `window->ClipRect`, which items cull and hover against and a body's table takes as its host clip -- pushed on the
  content channel and popped FIRST at `end`, before any channel switch: a clip is the list's, and `SetCurrentChannel`
  carries it into the channel it makes current (`imgui_draw.cpp:2270-2277`). No `BeginChild` and no
  `BeginGroup` per card (E.3.4's per-window `StateStorage` rule). A host answers a header's hover or right-click only
  through `CardHeaderResult` -- after `begin`, the last item may be the overflow button. **The chevron has ONE face
  size for both states**, sized off the OPEN chevron's ink (the mock's 3.5 dp); the glyph drawn is measured only to
  centre its ink in that column. Lucide's two chevrons are one stroke rotated, so a size per glyph draws the closed one
  larger, stroke and all (`I336`, `I331`). **A glyph read at a fractional size is read as `RenderText` draws it**: off
  the bake at `IM_ROUND(size)` (`imgui_draw.cpp:5415`), scaled by size / that bake's `Size` (`:5802`), from a pen the
  painter rounds to a whole point because `RenderText` truncates it (`:5791`) -- read raw, the chevron's record sat
  0.2 right of the quad it drew and 6.25 % narrow at body 16. `I336` holds the record's box to the quad the list
  received, off `AddText`'s four vertices. **The pen is the pure `cardChevronPenX`'s answer** (`PC11`; `I336` pins the
  call): at a whole-point pen a centred and a left-aligned ink often draw the same pixels (measured at body 16, scale
  1: both states on a 1x display, the open one on a 2x display), so only tier 0 witnesses the centring on every
  display -- keep the arithmetic there.
- **A panel header that must stay ImGui-free holds the painter by `std::unique_ptr<CardPainter>`**, with the class
  forward-declared and the host's constructor and destructor defined out of line.
- **EVERY CHROME COLOUR A PANEL DRAWS IS A THEME TOKEN, never a literal.** A PALETTE colour is reached as a
  `ChromeRole` through `chromeRoleColor` (`panel_chrome_model.cpp` reads each palette role ONCE); a colour outside the
  palette is its own theme member, read directly -- `ThemePanels`' `hatchStripe` and `addComponentDash`
  (`panel_chrome_ui.cpp`), its `consoleWarnRowTint` / `consoleErrorRowTint` (`console_view.cpp`), and the other
  `EditorTheme` colour groups' members (`axis`, `assetKind`, `swatchLabel`, `viewport`, `clear`, ...). The direct
  `EDITOR_THEME.palette.*` reads that remain -- `material_panel.cpp`, `import_details_panel.cpp`, `project_ui.cpp`,
  `shell_chrome_ui.cpp` -- are pinned at exact per-file counts by `I287` (the role map at one read per role, every
  other panel TU it lists at 0), so a new one, or one converted to a role, is a pin edit. A transparent fill is
  `clearOver(surface)` -- the ONE alpha spelling (`I333` counts it). Never `ImVec4()` / `ImVec4{}` (`I287`'s k = 5),
  `IM_COL32_BLACK_TRANS` (`I287`'s k = 3, by substring) or `withAlpha(` in a panel TU; `I333` refuses all four. A
  document's own colours -- a material's swatch, a thumbnail -- are content, not chrome.
- **De-emphasis is `mutedText` / `mutedTextWrapped`** -- string-only, so format-safe by construction;
  `ImGui::TextDisabled` and `ImGuiCol_TextDisabled` are banned in every panel TU (`project_ui.cpp` too, since
  E.6.4). A disabled widget greys through `BeginDisabled`.
- **Every NEW face switch is `ScopedFace`** (`panel_chrome_ui.cpp` spells its `PushFont`). The one deliberate panel
  exception is the Console's message push, `PushFont(mono, 0.0F)` in `console_panel.cpp`: it keeps the CURRENT size,
  so the row the `Selectable` sized and the clipper's step stay exact, and `I283` pins it as that file's one push,
  scoped to its row. `I286` pins the set of `editor/src` TUs spelling `PushFont` as `{console_panel.cpp,
  panel_chrome_ui.cpp, shell_chrome_ui.cpp}` (the last is E.6.2's shell chrome). A draw-list text takes
  `panelFaceFont` + `panelFacePoints` -- the FINAL size, `faceSizePoints(dp, s) = floor(dp x s + 0.5)`, ImGui's own
  rounding. Measure with `faceTextWidth` (exact advances), never `ImGui::CalcTextSize` for a column of advances.
- **A tile `Selectable` and the Assets tree's node paint no ImGui fill (D8)**: both tile hosts submit their
  `Selectable("##tile"` inside a `ClearHeaderStyle` that wraps that one call, and the face draws every state from the
  `Selectable`'s answers; the Assets tree's `TreeNodeEx("##dir"` rows sit inside one too and draw their own rounded
  `selection` / `hover` fill. **The Hierarchy is the stated exception (D6)**: its `TreeNodeEx("##row"` keeps ImGui's
  own square `Header` = `selection` fill and draws none of its own, so it never takes a `ClearHeaderStyle` or a
  hand-pushed `Header` slot (`I312`).
- **THE VIEWPORT HUD CLAIMS SCREEN-SPACE PRESSES** through the free `hudOwnsPoint(latchedHudLayout, p)`, on exactly two
  lines -- the pick's `overlayOwnsPress` and ImGuizmo's `widgetOwnsCursor` -- beside `viewAxisOwnsPoint`. A case driving
  `overlayOwnsPress` with a zero or image-space rect asserts its probes are outside every recorded HUD rect first: a
  zero image rect switches off only the view-axis arm.
- **THE VIEW-AXIS WIDGET CLAIMS ITS DIVIDER CIRCLE'S BOUNDING SQUARE** -- `viewAxisCircleRect`, `viewAxisRect` grown
  by `viewAxisCircleGap`, degenerate exactly when it is -- and the HUD's keep test and `viewAxisOwnsPoint` (the pick
  and ImGuizmo's `Enable` term) both read it: the circle reaches five past the half-extent rect and draws last.
  Chrome drawn around a claimed shape grows the ONE claimed extent with it (`VO14`, `I116`). The box is the circle's
  NOMINAL extent: the stroke's nominal outer edge is the box's edge at and below scale 1, and 0.5 x (s - 1) points
  past it above (the `ruleHeight()` stroke is s points); at EVERY scale the AA fringe (up to one point,
  `imgui_draw.cpp:887`) lies past that -- neither claimed nor kept clear, as every `roleOutline`'s is.
- **A request seam obeys its control** (the shell chrome's rule, again): the Hierarchy's search query, search-field
  focus and rename request (F2's gate, the `shortcutsLive` verdict), the Inspector's component menu and card header
  toggle, the Assets navigation, the Viewport's stats toggle and the Console's log scroll are each drained on ONE line
  naming the control's enabled predicate and `noModal`, computed once from
  `GetTopMostPopupModal()` (`I337`). A control with no disabled state -- the card header, the component menu's Remove
  Component item, the log child's scrollbar -- is gated by `noModal` (and, for the two Inspector seams, the card's own
  name) alone. The log scroll's value is bounded to the scrollbar's last-drawn `ScrollMax.x` before
  ImGui's 64-bit round can see it; the navigation and search-query seams pass their value through unvalidated, as test
  seams (`applyPending` writes the requested folder into `currentDir` as given, existing or not). Two control terms
  are witnessed by their own refusal: the stats toggle's `statsControlReachable` (`I338` cuts the image until the
  chip is not laid out) and the search query's `searchEnabled` (`I344`'s rename arm refuses a query while the rename
  disables the field). The Assets navigation's `crumbsEnabled`, the search-field focus's `searchEnabled` (the same
  term, read again on its own drain line) and the rename request's `shortcutsLive` have only `I337`'s source pin.
  `noModal` is refused behaviourally for three seams alone -- the Assets navigation, the stats toggle and the card
  header (`I338`); the Hierarchy's three seams, the component menu and the log scroll have only `I337`'s pin for it.
  **A request HELD ON A PANEL lives one tick too**, by its own mechanism, since the
  shell chrome's clear after the `ShellUiState` build reaches only `EditorApp`'s members: the panel resets it on the
  first statement after its drain's closing brace -- that statement and nothing else on its line, so no condition can
  ride along -- whichever way the gate answered (`I337`; the Inspector's two seams
  instead move theirs into a per-frame copy at the top of `onDraw`, E.5.2's shape), and gives it a `noexcept`
  `expire...()` that `EditorApp::tick` calls after `drawShellUi`, beside `expireComponentMenuAction()`, so a request
  issued while the panel does not draw at all is dropped as well (`I338`). A new panel seam adds BOTH, an `I337` arm
  and an `I338`-shaped hidden-panel arm: no source pin sees a missing expire call, and the deferral it leaves is
  invisible until the panel is hidden.
- **THE HIERARCHY'S SHORTCUTS RUN BEHIND ONE GATE**, the ONE `hierarchyShortcutsLive(renaming.valid(),
  searchOwnsKeys)` call (`HY7` is its truth table). An ACTIVE `InputText` owns only Left, Right, Delete, Backspace,
  Home and End (`imgui_widgets.cpp:4923-4925`), so without the gate Cmd/Ctrl+D and F2 would act on the selection
  while the user types. `searchOwnsKeys` is read in `drawHeaderRow()`, right after the field, and the header row runs
  BEFORE the shortcut phase on the same frame -- a read one frame late is the defect (`I312`'s order arm). The verdict
  is recorded, the next line's `if (shortcutsLive) {` reads it, and every `ImGui::Shortcut(` the panel spells sits
  inside that block, which closes before the `##tree` child opens (`I312` counts their three lines). **A new
  Hierarchy shortcut goes INSIDE that block** and moves `I312`'s count; what the field term reads is the next
  bullet's.
- **A SELECTION SHORTCUT BESIDE A TEXT FIELD IS GATED ON THE FIELD'S KEYBOARD, NOT ITS ACTIVITY** -- the Hierarchy's
  Delete, Backspace, Duplicate and Rename, which act on the panel's selection rather than on the field's own list. The
  field owns them while `IsItemActive()`, or while `IsItemFocused()` holds a focus the field's OWN activation gave it:
  a latch set by `IsItemActive()` and released only while the keyboard is in the panel
  (`IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)`, the `##tree` child included) on another item or on none,
  all read right after the field (`I312`, `I339`). Enter and Escape end the activation (`ClearActiveID`,
  `imgui_widgets.cpp:5452-5453`) and leave `NavId` on the field, so a gate on `IsItemActive()` alone -- or on
  `io.WantTextInput`, set only while an InputText is active (`:5656-5660`) -- opens on the very Backspace meant for the
  query, and the Hierarchy's Delete-on-Backspace deleted its selection (the code-review round's finding 20). A tab
  click or a focus command restores the window's last `NavId` (`imgui.cpp:13733`) -- the field's, once it was
  activated -- so after a search the gate holds until a row takes the focus: the safe direction, by decision. **And
  `IsItemFocused()` without the latch over-closes**: ImGui hands a field `NavId` with no click -- a tab or title-bar
  click that restores no `NavId` runs `NavInitWindow` (`imgui.cpp:19750-19752`), so does a docked window reappearing
  with no modal up (`:8616-8619`, re-initialised by `:14402-14409`), and NavInit picks the root window's first item,
  the Hierarchy's field since its rows live in the child -- which left Delete dead after a plain tab click until a row
  was clicked. A re-initialised `NavId` starts from 0 with the keyboard in the panel, so it releases the latch; only a
  restore keeps it. **The deliberate exception is the asset picker** (`asset_picker.cpp`): its Left/Right/Home/End and
  Enter move or commit the popup's own cursor over the list its search box filters, and are gated on `IsItemActive()`
  alone -- the box takes `NavId` on the frame the popup opens (`SetKeyboardFocusHere`) and can still hold it once it
  stops editing, which is exactly when those keys are meant to fire.
- **A TREE WALKED INSIDE A CHILD WALKS ONLY WHILE `BeginChild` RETURNED TRUE**, and `EndChild` stays unconditional --
  the Assets tree's C2 and the Hierarchy's `##tree` (`I312`, `I340`). `BeginChild` clips the child with its parent's
  `ClipRect` (`imgui.cpp:8434-8440`); a child clipped away entirely -- a dock cut down to its header row -- is skipped
  (`:8733-8738`), and `TreeNodeBehavior` then returns at its first statement (`imgui_widgets.cpp:6879-6880`), before
  `TreeNodeUpdateNextOpen`: every node reads closed (an unguarded Assets tree would record a collapse for each, 2.2.4's
  C2) and a `SetNextItemOpen` is never applied, so a one-shot the walk consumes -- the Hierarchy's reveal -- was lost
  for good (the code-review round's finding 6). A `Leaf` row drops it too (`TreeNodeUpdateNextOpen` returns before it
  reads it, `imgui_widgets.cpp:6808-6809`), so only the Hierarchy's TREE walk takes the reveal, never the flat match
  list: a query typed while it waits defers it. Anything the panel shows OUTSIDE the child (the badge's `m / n`) is
  computed ahead of the gate.
- **A RENAME WHOSE FIELD STOPS DRAWING ENDS AS THE FIELD WOULD** (E.6.3's code-review round, round 2; `I344`). The
  Hierarchy's `##rename` field is a row's, so it lives in the `##tree` child: a child skipped under an active field (a
  dock cut to its header row -- at `main` the rows lived in the panel window, where `ItemAdd`'s `KeepAliveID` kept the
  field alive, `imgui.cpp:11987`), a renamed row no longer walked, or a panel that does not draw at all (closed, or
  behind another tab -- `main`'s own stuck state) submits no field. `NewFrame` drops its `ActiveId`
  (`imgui.cpp:5795-5799`) and the deactivation recorded then lapses two frames on (`:4807`, `:5818-5819`), so
  `IsItemDeactivated` never fires once the field is back and `renaming` would stay set: the search field disabled and
  Delete / Backspace / Ctrl+D / F2 dead. `renameFieldFrame` is the ImGui frame the field was last submitted (written
  beside its `inputTextString` call, and by the BeginRename arm with the frame that arm runs in), and phase 1 --
  after the E24 drop, before `searchEnabled` and the shortcut verdict -- ends a rename whose field was not submitted
  on the previous frame: committed when `renameBuffer != renameOriginal` -- the name the BeginRename arm captured, its
  only writer, so an edit -- cancelled otherwise, through `commitRename` / `cancelRename`, the ONE commit and ONE
  cancel path the two action arms call too. **Never against the World's CURRENT name**: something else can rename the
  entity while the field is gone (an Edit > Undo of an earlier rename), and an unedited buffer would then commit,
  re-applying the rename the undo removed and dropping the redo branch (`I344 (d)`). **Never re-armed**:
  a fresh `SetKeyboardFocusHere` would steal the keyboard back from wherever it went meanwhile. A row merely scrolled
  out of the child is still submitted, and `ItemAdd` keeps an active item alive before its clip test
  (`:12025-12029`), so it keeps its rename (`I344 (e)`, sixty roots and a reveal of the last): a clipper added over the
  tree must keep submitting the renamed row, or a wheel scroll ends the rename. A new place the field is drawn writes
  the frame beside it; a third writer elsewhere would keep a field that no longer draws looking fresh (`I344` counts
  two).
- **The Inspector's axis thresholds are per entity and stated before any layout change**: the Cube's `"0.000"` is whole
  from a content width of 266, the Main Camera's from 279, the Directional Light's from 316 (`I289`, geometry-
  independent); the default right split is 0.30. A label-column, letter-inset or split change re-measures all three.
- **`componentIconTable()` and `entityKindFor` are two more sites the next built-in sweeps**, beside `RF7` and `IR21`:
  `EK5` pins the ten rows and `EK1` / `EK2` the kinds.

## First-contact windows (task E.6.4)

- **The Welcome window and the New Project dialog draw ONE frame** (`project_ui.cpp`'s `DialogFrameStyle`,
  `DialogBodyStyle`, `drawDialogHeader`): no title bar, `NoMove` + `ImGuiCond_Always` centring on the work area,
  `NoSavedSettings`, the palette's `dialog` token read ONCE in `dialogFill()` (`I287`'s row), the `active` outline,
  `ThemeDialogs` for every dp. A third first-contact window takes the same frame. The four frame pushes are popped on
  the statement after `Begin` / `BeginPopupModal` -- a modal reads them inside `Begin`, and a tooltip must keep the
  theme's popup look. A full-width band reserves its HEIGHT only (`Dummy(ImVec2(0, h))`): an item as wide as the
  window makes the content that wide, and once a short work area clamps the window its vertical scrollbar turns the
  difference into a `ScrollMax.x` a horizontal wheel scrolls with no `HorizontalScrollbar` flag (`imgui.cpp:8478`,
  `:11053`; `I359`). No flag closes that reach and keeps D34: the wheel's only gates, `NoScrollWithMouse` /
  `NoMouseInputs` (`:11051`), also stop its vertical scroll, and `NoScrollbar` removes the reach only by removing the
  scrollbar D34 draws (`:8401`). Both windows also pin their x scroll at 0 in their own block, directly above `Begin`
  -- a pin anywhere else feeds the next `Begin` -- for the Welcome's floor-width button row (`I358(f)`).
- **The dialog reads the disk ONLY through `NewProjectForm::probe`**, which `applyFileRequests`' step 0e keeps --
  re-taken when the inputs move, after a Create, and every `NEW_PROJECT_PROBE_REFRESH_TICKS`. `probeNewProjectTarget`
  and `createProject` share `newProjectTarget` / `classifyTarget`, so the preview and Create cannot disagree; the probe
  is advisory and `createProject` keeps every refusal. A filesystem call in `project_ui.cpp` is a defect, and so is
  a direct `createProject` / `promoteRecent` / `adoptProject` / `session.set` (`I358(a)`).
- **Create, Enter and the GPU seam share `newProjectCreateEnabled`**; the dialog resolves ONCE --
  `if (dismiss) {…} else if (commit) {…}` -- and every closing path calls `CloseCurrentPopup` on the line after its
  one-shot (`PU1`, arm-local). A new way to answer the dialog joins that resolution; it never sets a form one-shot
  from outside it.
- **A seam that types into a field must reload the field when it is active**: an active `InputText` owns its text and
  writes it back over the bound string every frame (`requestNewProjectInput` → `ReloadUserBufAndMoveToEnd` on its one
  tick, `I353`/`I354`, `I358(h)`). A case driving such a seam proves the field was active first.
- **Heights are stated, not assumed**: the Welcome window folds its list, its empty well and its wrapped blurb to the
  work area (`welcomeLayout`); the New Project dialog's natural height is 517 dp, and under ≈ 523 points of work
  height it is clamped and scrolls whole (D34) -- a row added to it moves that number.
- **A user string is never an ImGui label** in either window: a card is `InvisibleButton("##…")` with its text on the
  draw list (`I358(d)`), because a label hides everything after a `##`.
- **The scripting-language control stays disabled until 4.6.1**: its selected card is `NEW_PROJECT_LANGUAGE`, the
  constant `createProject` writes. Enabling it means a language parameter on `createProject` and a form field -- 4.6.1's
  design, not a toggle of the `BeginDisabled()` pair.
- **The validation line is ONE row in every state** and the dialog's height never changes after it appears; a state
  that needs a second row breaks the stillness `ImGuiCond_Always` centring relies on.
- **A click-equivalent seam on a modal resolves INSIDE the modal's body**, by the click's own predicate, so it closes
  the popup the way a click does (`requestNewProjectAnswer`); a seam that sets a modal's one-shot from outside needs
  I192's drained arm instead.
