# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Aero Engine — an open-source (MIT), cross-platform 3D game engine with an editor and per-project TypeScript **or** C++ scripting. Solo project, started July 2026. The goal is core-workflow parity with Unity/Godot (edit → script → play → export), explicitly **not** feature parity. 3D-first; 2D arrives in Phase 7.

Two platform matrices, never to be conflated: the **editor** runs on macOS/Windows/Linux only; the **runtime** (exported games) targets those three plus iOS and Android. The editor never runs on mobile — no touch UI, no adaptive layouts.

## Current state — read this first

**PHASE E (Editor Experience) IS OPEN — it executes between Phase 3 and Phase 4.**
**EPIC E.3 (Inspector & context routing) IS OPEN: E.3.1 (axis-labelled vector fields) AND E.3.2 (selection-follows-focus router) ARE BOTH MERGED; E.3.3 and E.3.4 are planning only.** **E.3.2 LANDED FIRST, which is legal and was not a gap** — the two are independent, they were specced and planned the same day against the same commit, and the case-id blocks were reserved disjointly: E.3.1 takes `I142`–`I148` and E.3.2 `I149`–`I161`. **THE `I142`–`I148` GAP IS NOW FILLED BY E.3.1 AND THE RESERVATION IS DISCHARGED** — the two branches touched `CLAUDE.md`, `docs/10` and `imgui_layer_test.cpp` in the same places and the merge was resolved by hand; a `--union` merge of the test file is NOT safe and produced a `TEST_CASE` nested inside another (one extraneous brace, caught only by the compiler). E.3.2 is fourteen commits — the plan's eight, three the sabotage pass forced and two the code-review round forced — with the full local gate green on both presets and both reduced configurations, the 32-seed sabotage matrix run in full, and a code-review round that found **six** gaps including **a real focus-steal defect** (below). **Its validation page has NOT been run on any platform.**
**EPIC E.1 (Viewport legibility) AND EPIC E.2 (Lighting & environment) ARE BOTH CLOSED IN CODE — E.1's five tasks and E.2's four are all merged.** E.2.1 (`Environment` + sky pass, PR #98, merge commit `28deab0`) and E.2.2 (point falloff + `SpotLight`, PR #99, merge commit `bf363e4`) took the built-in component count to TEN; E.2.3 (light gizmos + viewport icons, PR #100, merge commit `00e4c7b`, seventeen commits) is merged AND macOS-VALIDATED 12 of 12; **E.2.4 (material-preview parity + exposure relocation) CLOSES THE EPIC — nine commits, the full local gate green on both presets and both reduced configurations, the 31-seed sabotage matrix run in full and a code-review round closed. **IT IS macOS-VALIDATED 12 of 12, 2026-09-11, no blockers and no partials.** **THE BUILT-IN COUNT STAYS TEN: neither E.2.3 nor E.2.4 adds a component, so the five-generation-site rule and the component-count sweep do not fire for either of them at all.** E.1.1 (Debug line
renderer, PR #92 `15bf58b`), E.1.2 (Grid floor + world axes, PR #93 `d91eab1`), E.1.3 (View-axis
gizmo, PR #94 `6fb323c`, plus the follow-up PR #95 `0ab204d`) and E.1.4 (Silhouette selection
outline, PR #96 `3aadffb`) are all merged **and macOS-validated**; **E.1.5 (Transform-gizmo restyle)
is merged** — five commits, the full local gate green on both presets and both reduced
configurations. **The other 15 tasks are planning only.** Six epics, 24
tasks, in `docs/tasks/phase-E.md`. It is **lettered, not fractioned**, because `3.5` and `3.5.1`/`3.5.2` are
already Phase 3's Skeletal-animation epic and its tasks — a "Phase 3.5" would collide with referenced
numbers, and numbering is append-only. In Notion its `Phase #` is `3.5`, a sort key, not an
identifier. Four facts it was built on were **measured in the tree, and each contradicts a plausible
guess**: (1) the directional light **already** derives its direction from the entity's −Z world axis
(`scene_renderer.cpp:191-208`) — "it behaves like a point light" is an *affordance* gap, not a math
bug, because nothing draws the direction and nothing says which of two directional lights the bridge
picked; (2) `buildRenderView`'s **primitive arm never assigns `instance.material`**
(`scene_renderer.cpp:99-107`), so a material dropped on a primitive is written to the component
through the undoable command and then silently discarded at draw time — a confirmed defect, owned by
**E.5.1**; (3) `rhi::PrimitiveType::LineList` and `FillMode::Line` existed since 0.4.1 with **no consumer at
all** until **E.1.1**, which is now the tree's first and only `LineList` pipeline set — `FillMode::Line`
is still unexercised and named as such in a comment, and wireframe-of-meshes remains an unowned handoff.
**E.1.2 is the first CONTENT in that pipeline set**, and it is what established that a rasterizer
depth bias does not reach a line primitive at all (see below); (4) `openSceneFile`/`saveSceneFile` perform **zero**
containment validation against the project root, so a scene from another project loads while the
AssetDatabase still resolves GUIDs against the open one. **`Environment` (E.2.1) and `SpotLight` (E.2.2)
took the built-in component count from 8 to TEN, and both sweeps are done** — the
five-generation-site rule and the component-count-literal sweep below still apply in full to the
next built-in, whenever one arrives. **E.1.3 answered (4) for its own
half and left the rest**: it made every clip-space predicate projection-aware, and containment
validation against the project root is still absent. **(2) is still open and still E.5.1's**, and
E.1.4 deliberately reproduced it rather than fixing it in passing.

**Phase 3 (Asset Pipeline & 3D Content) is OPEN, and ALL SEVEN of its epics are now CLOSED IN CODE.**
Epic 3.7 (Audio playback v0 · audio) closes with 3.7.1 MERGED (PR #88, `4892e65`, macOS-validated
✅ 11/11), 3.7.2 MERGED (PR #89, `b398d17`, macOS-validated — 47 of 53 records, the 6 open ones each
needing ears or the editor) and **3.7.3 MERGED (PR #91, merge commit `0530cff`, all six CI jobs green
with `headSha == HEAD` asserted)** — one commit per step, the full local gate green, the S/X/P seed
matrices run as ctest stages, the break-the-guard meta-proof run against them, and
the code-review rounds closed — six of them, and the count is deliberately the last thing this sentence says, because it was renumbered in four consecutive deltas. Two durable outcomes: the guards were **inverted from a command denylist to an allowlist** for everything that NAMES a protected target, and for the direction that cannot be inverted — reaching one WITHOUT naming it — a ctest case now **reads `compile_commands.json` and asserts the property instead of predicting it**.

Epics **3.1** (AssetDatabase), **3.2** (Importers), **3.3** (Cooker v0), **3.4** (PBR materials),
**3.5** (Skeletal animation), **3.6** (Rendering essentials) and **3.7** (Audio playback v0) are all
**CLOSED in code**. What is left of the phase is its deliverable gate and the validation debt.

> **Per-task history — what each task shipped, what it deliberately left out, every trap and every dead
> end — lives in `docs/10-engineering-log.md`. Grep it before re-deriving anything.** This block is a
> summary of *where the position is* and of the rules that still govern new work. It is **rewritten**
> as the position moves, never grown: it reached 207 k characters once and that is what this note
> exists to prevent.

### E.3.1 — Axis-labelled vector fields (MERGED) — macOS-VALIDATED 13 PASS / 1 partial

**You can tell which box is Y, and you can put a field back.** The `Vec3` (non-colour) and `Quat` arms
hand-roll what `DragScalarN` does internally — one explicit `BeginGroup`/`EndGroup` around three
`DragScalar`s, each preceded by a coloured `X`/`Y`/`Z` letter **derived from `axis_palette.hpp`** — and
the gate is read **once, after `EndGroup()`**, which is what keeps a one-axis drag one undo entry. All
eight field arms moved into a two-column table whose label column is measured over the **whole model**,
and every field gained a right-click reset to the component's **default-constructed** value (so
`Transform::scale` resets to `(1,1,1)`, not zero), resolved on demand inside an open popup only. No
component, no reflect-gen change, no new file, no CMake line. Full detail in `docs/10`; the sentences
that govern new work are below.

**1. A PER-AXIS RESET IS DECIDED AT THE PRECISION THE ROW DISPLAYS; THE WHOLE-FIELD ONE COMPARES BITS.**
They are two different comparators on purpose and the asymmetry is load-bearing. A `Quat`'s per-axis
reset goes out through euler and back through `fromEulerAngles` + `normalize`, which **perturbs the
OTHER two axes by ~1e-7 every time** — so a whole-value comparison leaves the entry live FOREVER. The
whole-field arm writes the default verbatim and is exact by construction, so it keeps `==`. **The plan
identified this mechanism, fixed the whole-field arm and missed the per-axis one**; the code even
documented the hazard in the branch that did not have it.

**2. `normalize(Quat)` ASSERTS, AND `axisResetAction` ADDED A READ-SIDE CALLER.** Before E.3.1 that
expression ran only when `edited == true`; the reset path now reaches it **every frame a per-axis popup
is open**, from the STORED value. A non-finite rotation therefore aborts the Debug editor on a mere
right-click. Guarded with `std::isfinite` **first**, gated on `FieldKind::Quat` — a blanket guard would
kill `Vec3`'s NaN rescue, which is the one case where a live reset is exactly what the user wants.

**3. NOTHING IN `tests/` CAN OPEN AN ImGui POPUP, SO `resetField` HAD ZERO RUNTIME COVER ANYWHERE.**
`I142` exercises `BeginPopupContextItem`'s own `IM_ASSERT(id != 0)` on both call sites every frame, which
is real cover — but the popup never OPENS, so `EndPopup`, both `BeginDisabled` pairs, `MenuItem`,
`Separator` and the whole of `resetField` execute nowhere in CI. **Every future context menu inherits
this hole**; the manual pass is its only behavioural witness.

**4. A TEST'S TOLERANCE CAN BE WIDER THAN THE DEFECT IT GUARDS.** `VF6`'s `Approx(1).epsilon(1e-6)` could
not see a dropped `normalize`, because GLM's euler constructor is **already unit to 5.96e-08** — exactly
`1 - 2^-24`, a one-ulp miss. The arm that replaced it must not assert that ulp either: it is a property
of the host libm and of FMA contraction policy, so a bitwise claim there reddens CI on a correct tree.

**5. A SOURCE-TEXT TOKEN SCAN IS BLIND TO INTEGER-LITERAL SUFFIXES.** The pin guarding "the panel
restates no axis colour" missed `226U` — digits followed by an identifier character — and `226U` is
exactly how `axis_palette.hpp` spells its own bytes. Proven in both directions before it was trusted.

### E.3.2 — Selection-follows-focus router (MERGED) — Epic E.3 opens, and NOT with E.3.1

**Selecting a thing raises the panel that edits it.** Four panels share the Right dock node and exactly
one is on screen, so clicking a `.aeromat` reloaded a material *behind a tab you could not see*. E.3.2
is one context router over the focus plumbing that already existed, split three ways: **`EditorApp`
detects**, **`shell_ui.cpp` applies**, **`context_router.hpp` decides**. Two new public pairs
(`context_router`, `editor_prefs`), one `Selection` accessor, one `PanelRegistry` counter, four
`ShellUiState` fields, one View-menu checkbox, one new machine-local JSON file and one `docs/09`
**subsection** (§8.5). **NO component, NO target, NO ctest entry, NO shader, NO engine file, NO
link-line change.** `ctest -N` **174 -> 174**, entry set byte-identical in both presets; doctest
`aero_editor_shell_test` **1842 -> 1886**, `aero_editor_imgui_test` **181 -> 194**, the other five
unmoved. **THE BUILT-IN COUNT STAYS TEN: E.3.2 adds no component, so the five-generation-site rule and
the component-count sweep do not fire for it at all.** Full detail in `docs/10`; the sentences that
govern new work are below.

**1. A DELEGATING MUTATOR CANNOT CARRY A PER-CALL COUNTER IN ITS OWN BODY.** `Selection::set`, `toggle`
and `setAll` all routed through `add`/`remove`, so "bump as the first statement of every mutator" gives
`set` **two** bumps and `setAll(n)` **n + 1**, while bumping only in `add`/`remove` gives `setAll({})`
**zero**. Two private, non-counting helpers are the fix, and it is not a tidy-up: the three arms the
counter exists for — a re-`set` of the same entity, an `add` of a present one, a `remove` of an absent
one — are exactly the arms that would have read 2, 2 and 1. **And `Selection::prune` must NEVER bump**:
`HierarchyPanel::onDraw` prunes every frame, so a bumping prune is a permanent focus storm.

**2. `ImGui::FocusWindow` HAS TWO SIDE EFFECTS AND NEITHER IS IDEMPOTENT.** It closes every popup above
the focused window (`imgui.cpp:13740`) and **steals the active widget** (`:13754-13756`), with ImGui's
own comment at `:13751` naming this very slot. A stolen `InputText` edit is **discarded**, not
interrupted — `MaterialPanel` commits only on `IsItemDeactivatedAfterEdit()` and the panel that lost
the tab never draws to observe the edge. **The editor therefore has exactly ONE focus slot and calls
`SetWindowFocus` at most once per frame; `I159` pins one file and three calls. Re-read `:13740` and
`:13754` at every ImGui bump**, beside `ImGuizmo.cpp:1229-1230` and `imgui.cpp:8848`.

**3. `PanelRegistry::noteDrawn` IS WHAT MAKES A TAB ASSERTABLE AT ALL.** `ImGui::Begin` returns false
for a docked window that is not the selected tab and `drawPanels` skips `onDraw` entirely, so "the
Inspector raised" was **unfalsifiable at every automated tier** before this task — 3.1.3's log records
two attempts that passed while executing none of the code they named. Six lines record `Begin`'s own
answer, and every routing claim is a **delta across one tick** with an anti-vacuity arm.

**4. MATERIAL IS THE RIGHT NODE'S DEFAULT FRONT TAB, AND EVERY RIGHT-NODE PANEL DRAWS ONCE ON FRAME
ONE.** Measured, both of them, and each made an obvious assertion vacuous first: `panelDrawnCount(x) >
before` is satisfied with or without a route when `x` is already the drawing tab, so `I149`/`I150`/`I157`
put the Inspector in front with an explicit request **and `REQUIRE` the target is not drawing**; and a
baseline taken before the two settle ticks reads a layout artefact, which is what reddened `I153` on
its first run. **Any future GPU-tier routing case inherits both.**

**0. A PREFERENCE THAT SUPPRESSES AN EFFECT MUST NOT SUPPRESS THE OBSERVATION THAT FEEDS IT** — the
code-review round's BLOCKING finding, and a real focus steal the whole green matrix was blind to.
Gating the three `observe*` **early returns** on `enabled` left every baseline stale, so ticking
View ▸ Focus Follows Selection back on raised a panel for an act performed minutes earlier. **The gate
belongs in the LATCH condition**; the baseline always advances. `observeImportTarget` keeps `!settled`
in its early return, because that rule is the SESSION's and is independent of the preference. The case
that should have caught it, `RT24`, **pinned the defect while calling it correct** — it re-observed the
value produced while disabled and named that "a fresh act". **A case written from the same sentence as
the code cannot falsify that sentence.** Same round, same species: `readEditorPrefs` conflated a
MISSING file with one that EXISTS and cannot be READ (`readTextFile` disengages for both), so an
ACL-blocked `editor_prefs.json` reset the preference with no WARN; `fileExists` is the discriminator,
and it is `std::filesystem::exists`, so it is **TRUE for a directory** — which is what makes a directory
the portable stand-in for a permission-refused file.

**5. AN ADDRESS COMPARISON CANNOT SEE A DANGLING `c_str()`, AND A SEED PLACED AFTER THE GUARD THAT
REFUSES IT IS INERT.** `routedPanelId(x) == routedPanelId(x)` stayed **green** against a seeded
`return std::string("Inspector").c_str();` — the temporary is SSO, so it lives in the callee's frame
and two calls from one caller land at the same address; ASan's `detect_stack_use_after_return` is
**off by default**. Pin a lifetime by surviving intervening work, never by an address. And the plan's
`S16` (a `setVisible` inside the `Apply` arm) could not change behaviour at all, because a hidden
target takes the **Drop** four guards earlier. **Seed the mistake, not the symptom.**

### E.2.4 — Material-preview parity + exposure relocation — CLOSES Epic E.2. macOS-VALIDATED 12/12

**The material preview predicts the scene now, and "they match" is a measurement.** `buildRenderView`'s
two inline light walks became **`scene_render::resolveDirectionalLight` / `resolveEnvironment`**, which
`buildRenderView` itself calls, so the editor reads the bridge's own answer by construction. The
preview draws a unit sphere at the world origin under the open scene's `Environment` and its **active**
`DirectionalLight` — colour, intensity **and world direction** — with the scene's sky behind it through
a `SkyPass` of its own; a new public, pure `editor/material_preview_rig.hpp` owns the camera and
nothing else. Separately, the viewport strip lost four controls to a `View` popover and became
`T R S | Local/World | View`. **Eight commits. NO component, NO target, NO ctest entry, NO shader, NO
`docs/09` change, NO link-line change anywhere.** `ctest -N` **174 -> 174**, entry set byte-identical;
doctest `aero_tests` **1400 -> 1404**, shell **1829 -> 1842**, imgui **170 -> 181**. Full detail in
`docs/10`; the sentences that govern new work are below.

**1. `ImGui::End()` RESTORES THE PARENT'S LAST-ITEM DATA AT `EndPopup`, so the hazard this task was
designed around does not exist in the pinned tree.** `g.LastItemData = window_stack_data.ParentLastItemDataBackup`
(`imgui.cpp:8848`): reading the last-item rect after a `BeginPopup`/`EndPopup` pair names the item
*before* the popup, not the popup's last item. The plan assumed the opposite and `viewOptionsButtonMax`
was built to defend against it. **The capture stays** — it does not *depend* on that restore, and a bump
that stopped restoring, or a stray item submitted between the popup and step 9b, would silently move the
rect `overlayOwnsPress` reads. `I138`'s rect-equality arm is a **regression guard, not a witness**: seed
`S20` is green and recorded as a redundancy. **Re-read `:8848` at every ImGui bump**, exactly as
`ImGuizmo.cpp:1229-1230` already requires.

**2. A POPOVER'S LATCH REPORTS THE FRAME THAT DREW, AND THE EXTRA FRAME IS THE WHOLE POINT.**
`CloseCurrentPopup` runs *inside* the popup's body, so on the closing frame the popup **did** draw and
`viewOptionsOpen()` is still true; it reads false the frame after. That is the same one-frame shape
ImGui has for a click outside, which it closes at `EndFrame` **after** the draw walk — and it is exactly
why `updateGizmo` reading the latch one step earlier next frame reads the RIGHT frame. Measured, not
predicted: the plan expected one tick and it is two.

**3. `AERO_LOG_WARN`'S FIRST ARGUMENT IS THE FORMAT STRING, AND A TWO-ARGUMENT CALL SILENTLY DROPS THE
SECOND.** `AERO_LOG_WARN("scene_render", "...")` logs the literal `scene_render` and discards the rest;
the macro forwards to a formatter (`scene_renderer.cpp:49`'s own `"{}"` form is the correct shape).
Written that way inside a log-capture **control**, it made the control fail — which is what a control is
for. Without it the case would have shipped asserting emptiness against a capture nothing could fill.

**4. A SANITISER ADDED INSIDE A SHARED PURE FUNCTION IS INVISIBLE TO A TWO-SIDED A/B.** `PX`'s byte
identity compares the rig against `SceneRenderer`; side A **is** the rig, so a defensive clamp added
inside `materialPreviewView` would sanitise side A while side B passes the number through, and all four
`PX` arms stay green while the identity breaks in the one direction they structurally cannot see. That
is why AC-10's non-finite scene colour is asserted at **tier 0** (`PV13`), not by `PX`.

**5. THE EDITOR OWNS EXACTLY ONE SKY PASS NOW, AND `I127(b)` IS AN EXACT SORTED ALLOWLIST.** It was
"no editor file names `SkyPass`"; it is now `{material_preview.cpp, material_preview.hpp}` — a
de-duplicated, sorted **set** of filenames, because the old per-line string accumulator could not express
a set claim at all. A third file naming the type reddens it; widening the assertion to `<= 3` makes the
same seed invisible (both directions proven). The `#include <aero/render/sky_pass.hpp>` line is invisible
to the sweep by construction — it spells `sky_pass`, not `SkyPass`.

### E.2.3 — Light gizmos + viewport icons (MERGED, PR #100 `00e4c7b`) — macOS-VALIDATED 12/12

**A light is something you can see and therefore something you can aim.** Every `DirectionalLight`,
`SpotLight`, `PointLight` and `Camera` entity draws one screen-constant billboard icon through
E.1.1's `Overlay` bucket, from a 256x64 RGBA8 atlas rasterised on the CPU at panel init; a selected
light adds a wire gizmo (a rayed disc for directional, a `wireSphere` at `range` for point, a
two-circle cone for spot). **Twelve commits — the plan's ten plus two the sabotage pass forced.**
`ctest -N` **174 -> 174, entry set byte-identical to the branch point**; doctest `aero_tests`
**1377 -> 1400**, shell **1793 -> 1829**, imgui **163 -> 170** — the shell figure is the one measured
**after** the code-review round's two cases, from a build of this task's final content; the `1827`
recorded here until E.2.4 was the *sabotage*-gate number and was two stale. **NO component, NO target, NO ctest
entry, NO shader, NO `docs/09` change, and `DebugDrawConfig` is byte-identical.** Full detail in
`docs/10`; the sentences that govern new work are below.

**1. THE BILLBOARD HALF NOW HAS A PRODUCTION CONSUMER.** `setBillboardTexture` was called only by
`DG9` until this task; `ViewportPanel` now owns a 256x64 atlas texture and a ClampToEdge sampler and
hands both to `DebugDraw`, which BORROWS them. The atlas path is **deliberately NOT all-or-nothing**
with the panel's five other GPU objects: on any failure both handles stay invalid, `DebugDraw` falls
back to its own 1x1 white texel, and every icon draws as a solid tinted square. `hasBillboardTexture()`
is what tells the two states apart. **The panel's FIRST user-declared destructor exists for that atlas
alone**, and it suppresses nothing, because `Panel` deletes both copy and both move operations.

**2. A "NEGATED" COMPARISON IS ONLY NaN-SAFE IN THE DIRECTION IT WAS WRITTEN FOR.** 2.3.2's A10 is a
REFUSAL — `if (!(d <= radius)) return;` — and a NaN `d` correctly fires it. Translating that rule into
an ACCEPTANCE gives `d <= radius`; **`!(d > radius)`, which reads like the same thing, is its
opposite** and accepts a NaN. It shipped that way, with a comment claiming the safety it did not have,
and no case could see it because `projectToViewport` refuses a non-finite projection. **Found by
seeding the "wrong" spelling and watching nothing redden.**

**3. THE LOG CALLBACK IS ONE GLOBAL SLOT AND `EditorApp` CLAIMS IT.** A case that installs
`setLogCallback` before constructing an `EditorApp` is silently displaced by the Console panel's own
sink and left with nothing when the app clears the slot at teardown. `I134` reads `~Device`'s leak
diagnostics and therefore installs its callback **after the last `app.reset()`**, inside the scope that
still owns the device. **Any future case observing a log record around an `EditorApp` lifetime
inherits this.**

**4. A CLAUSE-ORDER SEED IS INVISIBLE UNLESS THE LOSING CLAUSE WOULD HAVE WON**, and a REDUNDANT arm
makes its own seed unobservable. Three seeds came back green for the second reason and none is a
defect: `viewportIconFor`'s `alive()` guard is redundant with `World::has<T>`, which already tests
liveness; `wireCircle` applies the same `std::clamp` an unclamped caller would have skipped; and a
faithfully re-derived `debugCircleBasis` is bit-identical, so the exposure prevents DRIFT rather than
fixing a defect.

**5. A FLOAT COLOUR CONSTANT CAN COLLIDE WITH `std::numbers`.** `176/255` through the sRGB EOTF is
`0.43415`, 9.4e-05 from `std::numbers::log10e`, and trips `modernize-use-std-numbers` — as **every**
accurate spelling of that colour does. It carries a `NOLINTNEXTLINE` with the reason beside it. And
the plan's own literal for sRGB byte 32 was wrong (`0.0176` re-encodes to **36**; the measured value
is `0.0144`) — caught on `VG13`'s first run, exactly where the plan said to check.

### E.2.2 — Point falloff + `SpotLight` (MERGED, PR #99 `bf363e4`) — the tenth built-in

**A lamp is possible, and every point light falls off like light.** The point falloff is now
inverse-square under the Karis/Frostbite window — `saturate(1 - (d^2/r^2)^2)^2 / max(d^2, 1e-4)`,
`r = max(range, 1e-4)` — so `intensity` means **the irradiance delivered at one world unit** and the
cutoff is a fade, not a ring. `engine::SpotLight` is the **tenth** built-in: five reflected fields,
28 bytes, no `.cpp`, placed at its entity's world translation like `PointLight` and aimed down its
-Z world axis like `DirectionalLight`. Ten commits, `ctest -N` **173 -> 174**, doctest `aero_tests`
**1342 -> 1377**. Full detail in `docs/10`; the sentences that govern new work are below.

**1. A FLOOR DOES NOT MAKE A FUNCTION TOTAL WHEN THE NaN IS IN THE *DIFFERENCE*.** The spec's
`resolveSpotCone`, its header comment and the test that pinned it all agreed with one another and all
three were wrong: a NaN inner angle makes `cos(inner) - cos(outer)` NaN, the NaN takes the delta
floor, and the result is a finite **hard-edged cone at full intensity on the axis** — a plausible
wrong picture. The resolver tests `std::isfinite` on both cosines **FIRST** and returns `{0, 0}`,
which makes `saturate(cosAngle*0 + 0)^2` exactly `+0.0` for every cosine, NaN included, on every
backend. `std::cos(+-inf)` is NaN, so infinite angles land there too. **The generalisation: re-run
every FORMULA in the precision it will execute in, exactly as E.2.1's lesson was to re-run every
GREP.** A spec's own code is a claim, not a proof.

**2. AN `operator<<` IN A TEST'S ANONYMOUS NAMESPACE IS INVISIBLE TO doctest WHEN THE TYPE IS NOT.**
doctest's streamability trait calls an **unqualified** `operator<<` from inside `doctest::detail`, so
only namespaces **associated with the arguments** are searched — `engine::render` for a
`render::SpotCone`, never the test file's anonymous namespace. **The sibling helpers that work are
not evidence that a new one will**: `Half4`, `Rgba` and `Size4` each stream a type declared in the
same anonymous namespace, which is what makes them reachable. A printer for an **engine** type must
live in that type's own namespace, and nothing warns when it does not.

**3. AN OUT-OF-ORDER DESIGNATED INITIALISER COMPILES ON macOS AND FAILS THE OTHER TWO LANES.**
Designators must follow declaration order in C++20; clang accepts a wrong order with
`-Wreorder-init-list`, a *warning*, while GCC and MSVC **reject**. The local gate was green and both
other lanes would have failed to compile. **Found by reading the build log rather than its exit
code — and only after forcing a recompile of every changed file**, because an incremental build
re-emits no warning for a TU it does not rebuild, so a warning sweep on a warm tree measures nothing.

**4. `fl(1e-4) * fl(1e-4)` IS NOT `fl(1e-8)`** — one ulp below. A test arm claiming to evaluate "the
same expression the implementation evaluates" while spelling the squared constant as a literal is a
**different** computation that agrees only by rounding. Spell the constant squared. (E.1.4's
`k * fl(1/255)` measurement is the same species.)

**5. ENTT'S `each` WALKS IN REVERSE CREATION ORDER.** A truncation test that seeds nine entities and
expects the first eight **by creation order** reads the wrong ones. Collect the World's own walk into
a vector and compare element for element — never a second `buildRenderView` (the `GR8` species).

**AND THE COUNT SWEEP, RE-MEASURED AGAIN: the default-scene pins are SEVENTEEN `entityCount() == 4`
lines, not the ten the spec claimed** — sixteen real pins plus `hierarchy_test.cpp:635`, which is
duplicate-entity arithmetic. Carry the LIST: "unchanged from ten" is a different claim from
"unchanged from seventeen". The built-in literals were the same **21** E.2.1 left, plus `PB13`'s
test-case NAME and its prose arithmetic, neither reachable by any count grep.

### E.2.1 — `Environment` component + sky pass (MERGED, PR #98 `28deab0`) — Epic E.2 opens

**A scene has an authored environment.** `engine::Environment` is the **ninth** built-in — eight
reflected fields, 72 bytes, no `.cpp`; `render::SkyPass` draws a fullscreen gradient before the opaque
pass into the open HDR target; and the PBR ambient is **hemispheric** through a light block grown
400 -> 416 (and 416 -> 928 at E.2.2). Full detail in `docs/10`; the sentences below still govern.

**0. THE MODES LIVE ONLY ON THE CPU, AND THE GPU RECEIVES DIFFERENCES — NEVER ENDPOINTS, NEVER A
`lerp`.** `resolveSkyGradient` packs `{horizon, sky - horizon, ground - horizon}` and `resolveAmbient`
packs `{mid, halfDelta}`, so **the shaders carry no mode, no branch and no selector** and a zero delta
makes `x + 0*w` exact on every backend. **Verified against the Khronos registry, not assumed:
`GLSL.std.450`'s `FMix` is specified `x(1-a) + ya` and DXC maps `lerp` to it** (`docs/SPIR-V.rst:2595`),
so a `lerp` form differs from `x` by an ulp when `x == y` and would destroy the exactness. That
exactness is what lets Solid + Flat reproduce a clear and a constant **bit for bit**, and it is why
`DG6` and the `OG` battery survived the change with **unedited** expectations. **Do not "simplify" the
resolvers back to endpoints.** (`-0.0 + 0.0*w` is `+0.0` — equal, bit-different.)

**1. `createEntity` ALWAYS ADDS A `Transform`** (`entity_ops.cpp:68`). A seeded entity that must not
carry one is built with `world.create()` + `setName` directly — never `createEntity` +
`remove<Transform>`, which churns the component store for nothing. E.2.1's "Environment" seed is the
precedent. **And the placeability assertion it broke is the lesson:** `hierarchy_test`'s
`for (root) CHECK(has<Transform>)` was true only *by accident*, and is now four per-entity statements
with `CHECK_FALSE` on the fourth. **A universal that is true by accident hides the seed that falsifies
it.**

**2. AN ASSERTION IS ONLY AS STRONG AS THE TOLERANCE IT IS WRITTEN AT.** The sabotage pass found the
sky's whole depth/ordering contract nearly unfalsifiable: `SB9` and `SB16` compared the centre texel
**bit-exactly** against a gradient oracle their own sibling arms only claim to 2 half-ulps, so when the
sky genuinely covered the cube the texels were **one half-ulp apart and both arms reported SUCCESS**.
Both now assert a distance **greater than** the tolerance. **And a refusal case proves nothing about
WHICH inputs are refused unless something pins an input the narrower check would accept** —
`packSkyCamera`'s sixteen-element loop reduced to column-0-only left the whole suite green, because
`SB3`'s refusals make *all four* columns non-finite; the new subcase's anti-vacuity arm asserts
`inverse(proj*view).columns[0]` **is** finite, which is what makes it a statement about the subset.

**3. A CASE THAT RUNS THE RIGHT CONFIGURATION STILL ASSERTS NOTHING IF THE SAMPLE POINT IS INSENSITIVE.**
The hemispheric ambient had **no pixel cover anywhere** until `SB17`: `DG6` and `OG` run Flat at
intensity 1, where `halfDelta` is exactly zero — blind **by construction** — and `SB16` read a face with
`N.y == 0`, where the term collapses to `mid`. **And at `N.y = +1` the mid/half-delta swap is INVISIBLE**
(`mid + halfDelta*1` and `halfDelta + mid*1` are the same sum), so `SB17` needs an up-facing read, a
**down**-facing read and a Flat control. **A symmetric property needs an asymmetric sample.**

**4. `static_assert(!requires(T v) { v.member; })` ON A NON-DEPENDENT TYPE IS A HARD COMPILE ERROR**, not
`false` — a requires-expression only substitutes when the type is dependent. `HE17` routes `RenderView`
through a file-local concept and carries an `AmbientProbe` **positive control**, without which a
mis-spelled detector makes the negative assertion vacuously true for every type in the language.

**5. THE FULLSCREEN TRIANGLE IS BACK-FACING** under SDL_GPU's normalised convention, measured:
`CullMode::Back` on `SkyPass` deletes the sky entirely (9 cases, 100 assertions). `CullMode::None` there
is load-bearing, not stylistic. **And a depth-write-with-test-off seed is INERT on all three backends by
API rule** — SDL Metal computes `depthWriteEnabled = write && test` (`SDL_gpu_metal.m:1183`), Vulkan
skips the depth update when `depthTestEnable` is false, D3D12 maps test-off to `DepthEnable = FALSE`.

**6. THE SKY'S RAY COMES FROM `far - near`, NEVER FROM THE EYE**, so both projections are one path and
`CameraView::eyePosition` — wrong under a parallel projection — is never read. `SV_Position.w` is 1 at
all three vertices, so perspective-correct interpolation reduces to linear. **The Y flip in
`sky.vert.hlsl` is self-cancelling and unobservable**: `ndc` drives both the position and the ray, so
flipping it moves them together. Only a **ray-only** flip is visible (5 cases, 82 assertions).

### Epic E.1 — what E.1.1 through E.1.4 left behind that still governs new work

Per-task narrative — what each shipped, every trap, every dead end — is in `docs/10`. These are the
sentences a new task can still break.

**THE MASK MIRRORS `draw()`'s FRUSTUM CULL, WITH `draw()`'s OWN RESOLVED FRUSTUM (E.1.4).** "An
off-screen instance writes to no texel" is FALSE as a reason to skip it: `draw()` culls on the
**cooked AABB**, so an instance whose bounds are invalid or smaller than its triangles is dropped
from the picture while still projecting on screen. `draw()` publishes its resolved `(frustum,
culling)` pair and `renderSelectionMask` reads it — a mirror, never a second extraction. **Any future
pass that re-draws a subset of the forward pass's instances inherits this.**

**A UV THAT ADDRESSES A SUB-RECT NEEDS TWO DIFFERENT FAR BOUNDS (E.1.4).**
`tonemapSourceUvMax` returns the drawn rect's **exclusive** far edge, right for a fullscreen
triangle's far corner and **wrong as a clamp**: under Nearest filtering `floor((drawW / texW) · texW)
== drawW` is the **first cleared MARGIN texel**. A clamp bound is the last drawn texel's **centre**,
`(drawExtent − 0.5) / textureExtent`, and it is a DISTINCT quantity with its own name. **The case
that guards it must run on a MARGINED target** — `drawExtent == textureExtent` makes it unobservable,
because the hardware's own `ClampToEdge` answers correctly there.

**AND `k * fl(1/255)` IS NOT `fl(k / 255)`** — measured: the reciprocal form is bit-unequal for
**126 of the 256** byte values, first at `k = 3`, and it looks identical to six significant digits.
Any packer/unpacker pair in this tree that wants an exact round trip must divide.

**EVERY CLIP-SPACE PREDICATE IS PROJECTION-AWARE, AND THE PARAMETER IS NON-DEFAULTED (E.1.3).** An
ortho proj's bottom row is `(0,0,0,1)` and the view matrix is affine, so `clip.w` **does not depend on
the world point at all** — which made every "in front of the eye" test vacuous under ortho.
`projectToViewport`, `clipSegmentToNearPlane`, `gizmoOriginBehindCamera` and `viewportRay` all take a
**NON-DEFAULTED** `ProjectionMode` (`CLIP_Z_EPSILON = 1e-6` on `clip.z` in ortho), which is what makes
an unconverted site a compile error rather than a silent wrong picture. **The two gates are not
equivalent and the asymmetry is shipped**: perspective's `w > 0` means "in front of the EYE" and
admits a point closer than `nearPlane`; ortho's `z > 0` means "beyond the NEAR PLANE" and rejects it.
**A universal `z`-based gate is 2.3.2's contract to change and is an unowned handoff.**

**A THRESHOLD CALIBRATED IN ONE PROJECTION'S DEPTH UNITS MEANS SOMETHING ELSE ENTIRELY IN THE OTHER
(E.1.3, and it shipped as a defect).** ImGuizmo's `0.001` near-band is calibrated against a
**view-space** depth; ortho's `clip.z` is a **normalised** one, so the same constant reached ~1.0
world units and suppressed the gizmo outright after `focusOn`. The mirror is now gated on
`Perspective` (PR #95). **"Stricter" is only safe against the failure it was written for.**

**A CHROME WIDGET THAT SUBMITS NO ImGui ITEM IS INVISIBLE TO ImGuizmo'S OWN PROTECTION (E.1.3).**
`CanActivate()` is `IsMouseClicked(0) && !IsAnyItemHovered() && !IsAnyItemActive()`, so a widget drawn
with `ImDrawList` alone is protected only by accident. The fix widens the `ImGuizmo::Enable` term —
never an early return, which would hide the handles; never `!IsOver()` in the widget's guard, which
reads `gContext` before this frame's `Manipulate` — guarded by `!IsUsing()` so an in-flight drag
survives. **Any future viewport chrome drawn with `ImDrawList` alone inherits this and must claim its
own presses.**

**THREE FLOAT FACTS FROM E.1.2, EACH MEASURED AND EACH COUNTER-INTUITIVE.** (1) **A float-indexed
lattice loop does not terminate**: at `focus = 1e6, spacing = 0.01` the quotient reaches ~1e8 where
`k += 1.0F` is a no-op — and the naive repair traps, because `inf - inf` is NaN and
`std::min(NaN, 48.0F)` **returns NaN**. Use an integer counter and clamp the span. (2) **Decade
lattices NEST in float32, so coordinate divisibility cannot identify a cadence**. (3) **`pow10(n)`
must be computed FRESH from `1.0F`**, never as a running product.

**`a + (b − a)` IS NOT `b` (E.1.3): an animation that must land on a boundary must HOLD its
endpoint.** And **`MAX_PITCH` is now exactly `HALF_PI`** — safe here because the composition is
yaw-outer / pitch-inner, so `right()` is independent of pitch, `viewMatrix()` has no `lookAt` and no
up vector, and nothing divides by `cos(pitch)`. Measured pole residual: worst component error
2.384e-07 over 1441 yaw samples, and `right().y` is exactly 0 there.

**AND `MaterialParams{}` IS NOT THE RENDERER'S DEFAULT MATERIAL (E.1.4).** It defaults
`metallicFactor` to glTF's `1.0`, and *a metal with no environment to reflect renders near-black
under analytic lights* — so a test quad built from `MaterialParams{}` **is** drawn and is
byte-identical to the background, which makes any colour assertion over it vacuous. Start from
`DEFAULT_MATERIAL_PARAMS`.

**A `REQUIRE` ON A MID-FLIGHT ANIMATION AFTER ONE REAL FRAME IS A CROSS-LANE FLAKE (E.1.3).**
`PanelContext::deltaSeconds` caps at **0.25 s, exactly `VIEW_SNAP_SECONDS`**, so one slow frame
completes the whole snap. Drive a mid-flight property at the pure tier where the delta is a parameter.

**AND TWO TEST-TIER TRAPS THAT LOOK LIKE PASSING TESTS (E.1.4).** `buildRenderView` walks
`each<Transform, MeshRenderer>` in **EnTT's storage order** while `buildSelectionMaskSet` walks the
**selection**, so comparing the two builders' output BY POSITION asserts that two unrelated orders
agree — match by a key and `REQUIRE` its uniqueness. And **`doctest::Approx(x).epsilon(0.0)` never
matches** (its comparison is `< 0`) and prints `1 == 1` on failure; `CHECK(a && b)` is a hard compile
error ("Expression Too Complex").

### 3.7.3 — Audio-boundary CI guard (MERGED, PR #91 `0530cff`) — CLOSED Epic 3.7

**Zero engine C++** — two lint scripts, one compile-time probe, two hermetic `cmake -P` ctest drivers.
Full detail, all 35 redden-proofs and every dead end are in `docs/10`. Four things still govern new
work:

**A COMMAND DENYLIST OVER CMAKE CANNOT CONVERGE — INVERT TO AN ALLOWLIST.** Three review rounds, and
each one's blocking finding sat inside the fix written to teach the previous one: an `*_internal`
refusal matching only the `aero::` alias while the raw name passed; `EXCLUDE_FROM_ALL` refused on the
`add_library` line while `set_target_properties(… EXCLUDE_FROM_ALL TRUE)` passed; the property
spellings refused while the **plain** commands passed. **If a guard ever needs a second arm for a
second spelling of one predicate, stop and invert it**: ask what a protected thing may legitimately be
named by, measure that the set is small, and refuse everything else.

**AND WHERE INVERSION IS IMPOSSIBLE, READ THE BUILD FACT INSTEAD OF PREDICTING IT.** Reaching a target
*without naming it* — via a toolchain file or a preset's cache variables, which appear in no
CMakeLists at all — is not bounded by any list. `boundary-probes.probe_compile_line` therefore reads
`compile_commands.json` and asserts directly that no probe's compile line carries vcpkg's shared
include root. Its own limits are stated where it lives: a multi-config generator writes no database
(it self-skips, exit 77), and a contaminating include root that is not vcpkg's is invisible to it.

**THE MEASUREMENT THAT OUTLIVES IT:** `vcpkg_installed` **is** on `engine/audio/src/mixer.cpp`'s
compile line in `macos-release` and **is not** in `macos-debug`, read from both databases — because
`aero::profiling` is `PRIVATE` on all three vcpkg-free targets and carries `Tracy::TracyClient` when
`AERO_ENABLE_PROFILING=ON`, and a target's own `PRIVATE` usage requirements apply to its **own**
compile line. **Never write "a stray include there is a hard compile error" without saying in which
configurations.**

**A GUARD'S OWN `.cmake` E2E DRIVER IS INSIDE THE SET IT SWEEPS, AND IT BIT TWICE.** Fixture strings
spelling `target_link_libraries(aero_audio …)` made the guard exit 1 on its own driver, twice. Both
fixed by composing the command name from a variable so the scratch file stays byte-identical while no
matching literal remains. **An exclusion list was rejected both times** — a permanent silent hole in a
universal sweep, in the file most likely to acquire a real CMake snippet later. Expect this whenever a
guard's scan set grows to include `*.cmake`. Related, and the same species: **mutate an arm the way a
careless edit would, not the way a demolition would, and pin the offending TOKEN rather than the arm's
generic sentence** — a coarse mutation is caught by any assertion and proves nothing about the one
under test.

### 3.7.2 — rules that outlive the task (merged, PR #89 `b398d17`, macOS-validated)

**Two rules still worth carrying; the rest is in `docs/10`.** (1) **`std::clamp(NaN, lo, hi)` returns
NaN on libc++** — every clamp on a value that can be non-finite needs an explicit finiteness arm
*first* (E.1.2 hit the same shape again with `std::min(NaN, 48.0F)`). (2) **Two places must never
compare the same key by different rules** — a binding matched on full `Entity` identity and swept by
index alone orphaned a looping voice permanently. One comparator, one place.

**Nothing in 3.7.2 has ever been HEARD on any lane**: CI compiles and runs every `SP`/`MX`/`SY`/`SA`/`DV`
case on all three lanes — so the spatializer, the mixer, the system and the bridge **are** cross-lane
covered — but CI opens the **null backend only**. **LSan runs on the Linux Debug lane alone**, so a
green `SY20` on macOS proves the teardown is *clean*, not that a leak is *absent*, and unlike 3.7.1
(whose leak lived in third-party code) **everything that task allocates is first-party**. Its `A23`
(a release→relaxed swap on the clip-count store) and `A36` (the device's null-render silence path)
remain **uncovered by anything at all** — no lane runs TSan and the null-render buffer belongs to
miniaudio; `A38` is covered only by validation row 9. Full detail in `docs/10`.

### The phase table

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE.** Gate reached, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics closed and macOS-validated; Windows/Linux rows pending for every task (`editor/VALIDATION.md`). Gate artifact: `samples/phase-2-editor-scene/` — data, deliberately not `add_subdirectory`'d. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** **All seven epics CLOSED in code** — 3.1–3.6, and 3.7 with 3.7.1 + 3.7.2 merged and macOS-validated and **3.7.3 merged (PR #91)**. What is left is the gate below and the validation debt. Per-task detail in `docs/10`. |
| **Phase 3 gate** | Drop a rigged glTF/FBX in → PBR materials + shadows + a playing animation + **an audible sound**. The audible half exists in code as of 3.7.2 and **has not been validated on any platform** — 3.7.2's macOS pass ticked 47 of 53 records and left the 6 that need ears open. |
| **Phase E** — Editor Experience | **OPEN. EPIC E.3 IS OPEN; E.3.1 (axis-labelled vector fields) IS MERGED AND macOS-VALIDATED — 13 rows PASS, 1 row PASS with one sub-case NOT EXECUTABLE, 0 FAIL, 2026-09-12.** Ten commits plus a hand-resolved merge of E.3.2's `main`; `ctest -N` UNMOVED at 174, doctest 1404 / 1886 / 201 / 40 / 55 / 7 / 28, all eight guards unmoved (it adds no tracked file), the 25-row sabotage matrix run in full (four assertions found unfalsifiable and closed) and TWO code-review rounds, the second finding a BLOCKING defect: a per-axis `Quat` reset was never disabled, because `enabled` compared the whole recomposed quaternion while the reset round-trips through euler, which perturbs the OTHER two axes by ~1e-7 every time — from `(0,20,40)` degrees it never converged and every click cost another undo entry. A per-axis reset now asks *does this change the axis it names, at the precision the row displays?*, and the validation pass confirmed it in the product: at a near-gimbal `(12,87,27)` pose one click took X to `-0.000` with Y and Z exactly preserved and the entry **greyed on the very next open**. The axis letters measure **byte-exact** — X `rgb(226,65,73)`, Y `rgb(125,199,61)`, Z `rgb(56,133,226)` — once the capture is converted out of the display's ICC profile, which shifted them and is the method trap that invalidated the first measurement. `resetField` had **zero runtime cover anywhere** before that pass. **E.3.2 (selection-follows-focus router) IS ALSO MERGED — fourteen commits, `ctest -N` UNMOVED at 174 with a byte-identical entry set in both presets, doctest 1404 / 1886 / 194 / 40 / 31 / 7 / 28, guards 506 / 92 / 163 / 92 / 165 / A=6 B=82 / 11-3-55 / 6-57, both reduced configurations 161 and 93 with byte-identical entry sets, the 32-seed sabotage matrix run in full (three real holes found and closed), a code-review round that found SIX more including a real focus-steal defect (a disabled preference left every router baseline STALE, so re-enabling raised a panel for a minutes-old act), and NO existing GPU case adjusted — all 181 passed unedited. E.3.2 LANDED BEFORE E.3.1 and the `I142`–`I148` case-id gap is deliberate. ITS VALIDATION PAGE HAS NOT BEEN RUN ON ANY PLATFORM. E.3.1, E.3.3 and E.3.4 are planning only.** **EPIC E.1 AND EPIC E.2 ARE BOTH CLOSED IN CODE -- E.1's five tasks (all macOS-validated) and E.2's four. E.2.4 (material-preview parity + exposure relocation) CLOSES E.2: nine commits, `ctest -N` UNMOVED at 174 with a byte-identical entry set, doctest 1404 / 1842 / 181 / 40 / 31 / 7 / 28, all eight guards green (math 496 -> 500 and project-no-delete B 79 -> 80, the other six unmoved), both reduced configurations 161 and 93 with byte-identical entry sets, the 31-seed sabotage matrix run in full and a code-review round closed. **IT IS macOS-VALIDATED -- 12 PASS / 12, 2026-09-11, no blockers and no partials**, and the pass closed all six declared seeds. **The deliverable is measured, not impressionistic: the preview's peak sphere luminance and the viewport's are BYTE-IDENTICAL at (192,198,209), delta 0.0 levels**, taken over a 22-frame burst spanning a full orbit with disjoint search boxes; under Solid mode the preview corner, the viewport corner and an independently computed ACES+sRGB oracle all read **(206,93,95) exactly**. **The viewport below the strip row is BIT-IDENTICAL to the `990ee2b` build -- 0 of 1 064 924 pixels -- with the strip row itself as a working control at 5391 of 27 092.** The preview's deliberate change is quantified: sphere peak **230.0 -> 212.4** but picture mean **64.1 -> 134.6**, because a dark void became the scene's sky -- **it reads as RIGHT, and no retune of E.5.2's default light is warranted**. Seed cover: **`S30`** -- at `intensity` **exactly 0.000000** the picture equals the no-sun picture (peak 140.1 both) while the notice stays away (0 amber px), so `hasSun` keys on the RESOLUTION; **`S21`** -- a dismissing click landing ON a translate handle left the sphere at **(503.0,366.0) -> (503.0,366.0)**, and one Undo afterwards reverted the ORIGINAL move, proving no spurious entry; **`S22`** -- Escape closed the popup 35205 -> 0 dark px; **`S24`** -- unchecking Gizmos removed the icon from the picture; **`S25`** -- the popup's top-left **(378,105)** sits at the button's `(min.x,max.y)` **(380,104)**, **17 px from the mouse**. Sun direction: peak-brightness orbit phase **69 deg -> 332 deg** for a 90 deg yaw, **97 deg apart** at 5.2 deg frame resolution. Two Environments: the loser's edit moves **364** background px at max delta 2 while the winner's moves **17212** at delta 33, and the WARN fires **exactly once**. Tracy: **NO new zone in either of two A/B pairs**; the preview's `renderFrame` mean rises **+26.97 us** and **+11.42 us** -- same sign, but the E.2.4 build's own run-to-run spread is **15.54 us**, so **+0.01 to +0.03 ms is a BOUND, not a measurement** (~0.1 % of a frame). HiDPI is deliberately NOT a row (this task draws no line and no icon), so **E.1.1's thick-line handoff stays FIRED and unmoved**. **A METHOD FACT THAT CONTRADICTS WHAT E.2.3's PASS RECORDED, re-measured here: a FILE-ACCESS (TCC) prompt DOES accept a synthetic click** -- the "AeroE24 requests access to your Desktop folder" dialog dismissed on a CGEvent click at its Permitir button, as did the local-network one. **The prompts CASCADE (each new dialog's origin shifts) and come in two heights -- TCC 192, local-network 250 -- so the button offset must be DERIVED from the measured bounds, never hardcoded.** **And the editor throttles hard when it is not frontmost**: a Tracy capture taken while the terminal held focus recorded **2 frames in 20 s**, against **21 894** when the editor was re-activated every 2 s. -- **E.2.3 IS MERGED (PR #100, `00e4c7b`, seventeen commits, all six CI checks green on `f1a39aa` with `headSha == HEAD` asserted) AND macOS-VALIDATED 12 of 12, 2026-09-08, no blockers and no partials.** `ctest -N` UNMOVED at 174 with a byte-identical entry set, doctest 1400 / 1829 / 170 / 40 / 31 / 7 / 28, all eight guards green, both reduced configurations 161 and 93, the sabotage matrix run in full (two real holes found and fixed) and a code-review round that found three more (the decisive one: NOTHING anywhere distinguished the four atlas glyphs, so a mis-wired `glyphAlpha` arm drew a point glyph on every spot light with the whole suite green -- seeding it reddens `VI12` ALONE of 1828 cases). **THE VALIDATION FIRED E.1.1's THICK-LINE HANDOFF, which had been UNFIRED for eight tasks.** Row 4 began NOT EXECUTABLE -- both externals are 1x and NEITHER EXPOSES A SINGLE HiDPI MODE (56 and 87 modes enumerated, zero above 1x) -- then those monitors were disconnected, leaving the built-in **3024x1964 Retina** panel, and the row became executable. Icons scale correctly (**28x19 / 40x40 px at 2x** against **14x9 / 19x20 at 1x**, exactly 2x, still 22 POINTS) but **debug LINES do not: 387 of 441 sampled runs across the directional gizmo are ONE DEVICE PIXEL**, i.e. 0.5 points, half their apparent weight at 1x -- which applies to the grid and the world axes too, not just the gizmos. Row 9 is stronger than the page asked: **0 differing of 1 552 000 with `getbbox()` returning None** -- not one pixel -- against a 267-px control, and the D6 divergence is **outline (255,167,56) vs icon (228,192,53)**, max channel delta 27. Row 12 needed no frame match at all: `material_preview.cpp` is ABSENT from E.2.3's changed-file set and includes nothing from `aero/render` or `aero/scene_render`, so it cannot see `DebugDraw`, `debugCircleBasis` or `activeDirectionalLight` -- byte-identical BY CONSTRUCTION. Row 10 is honest about its limit: **no new Tracy zone in either configuration** (diffed against E.2.2's 32-zone capture) is rigorous, but the frame-cost delta is not -- two independent A/B pairs at 23 icons produced deltas of OPPOSITE SIGN, so the feature costs less than the variance between two runs of the same build. Row 1: sun **19x20 -> 19x20** and camera **14x9 -> 14x9**, delta **0** across an ~87x dolly. Row 3: **0** stray ink around all five icons (seed S28 covered). Row 5: sphere follows range **112/320/542 px**, the inner circle vanishes twice with the outer bbox unchanged, both cone bounds clamp to exactly **1.570796**. Row 7: peak ink **(207,208,212)** active vs **(144,144,146)** muted, delete swaps to **(211,207,209)** on the SAME background and Undo restores **(144,144,146)**. Row 11: **0** leak WARNs, log 14 lines all `[info]`, with a WARN-capture control. **NEW METHOD FACTS, EACH OF WHICH PRODUCED A WRONG ANSWER FIRST:** a pending macOS permission prompt stalls the editor to ~2 frames per 3 minutes while it still logs "shell ready" -- Tracy's port-8086 listener triggers the LOCAL NETWORK prompt; local-network prompts accept synthetic clicks but FILE-ACCESS (TCC) prompts reject both CGEvent and the accessibility API and cannot be dismissed programmatically at all; replacing the executable inside an `.app` invalidates its signature and macOS then grants NO window silently (make it a real file and `codesign --force --sign -`, a symlinked executable cannot be signed at all); a bundle identity that has been full-screen can relaunch onto an INACTIVE Space (`onscreen=false`, accessibility reports zero windows) and a fresh bundle id clears it; synthetic input needs the target app FRONTMOST or clicks silently do nothing; **a synthetic Escape goes to whatever app is frontmost -- including the terminal running the session, which it interrupts -- so close ImGui menus by CLICKING**; ImGuizmo's centre handle correctly claims a press over a coincident icon, so a picking row must move the gizmo away first; and **undo of a delete does not restore the original entity index**, so the active-directional winner can move after an undo. E.2.1 AND E.2.2 ARE MERGED — E.2.1 MERGED (PR #98, `28deab0`) and E.2.2 MERGED (PR #99, `bf363e4`, ten commits, all six CI checks green on `b0d44a2` with `headSha == HEAD` asserted). **E.2.2 IS macOS-VALIDATED — 12 PASS / 12, 2026-09-07, no blockers and no partials.** The falloff A/B was MEASURED against a branch-point build through the real render chain, not predicted: the slab reads **0.049194** against **0.193359** at `d = 2.5` and **0.005703** against **0.051788** at `d = 5` — ratios **0.2544 / 0.1101** against a predicted 0.2543 / 0.1106 — and the range edge is monotone to exactly zero, so **no ring**. The spot's pool measures **2.3274** against `4·tan 30° = 2.3094`, **4.0054** against 4.0000 widened, `inner == outer` collapses the soft band from **0.758** units to **0.000**, and a 20° yaw moves the near edge to **0.7036** against 0.7053, recovering a **19.69°** tilt. Nine spots keep eight with **one latched WARN**; the dropped light moves **0** texels against ~1420 for a kept one; a tenth `PointLight` still draws. All nine degenerate cone and range configurations render **0 non-finite texels**. In the editor `SpotLight` is the **last** Add Component entry, five rows at exact defaults, both cone bounds clamp (**0.000000** / **1.570796**), each drag is **one named undo entry**, and two undos restore the viewport to **0 differing pixels of 2 000 000** with a 15 159-px control; selecting a light entity draws E.1.4's marker plus the gizmo and clicking the marker picks it. Tracy: `draw` **0.0124 ms** over 4 377 frames, **no new zone**, vsync-bound at 15.77 ms of 16.67. `phase-3-shadows` at intensity 56 keeps the ground lit under the casters with a visible falloff — **sabotage seed 28's only cover, now settled**. **THE PASS CHARACTERISED E.2.1's OPEN `Save Scene` OBSERVATION AS A REAL DEFECT: it is a SILENT NO-OP WHILE THE SCENE HAS NO BOUND PATH** — no write, no log line, the dirty marker left set — while `Save Scene As…` in the same menu saves, and once a path is bound `Save Scene` works. **It is not E.2.2's and needs its own task.** **Row 8's blocker was a FIXTURE GAP, not a defect** — importing `tests/fixtures/materials/canonical.aeromat` into the project's `assets/` is all it needed, and that clears E.2.1's row 10 the same way. **The preview ANIMATES**, so its identity is measured as a cross-build frame match: the closest pair ACROSS builds differs by **31 texels of 23 000** against a within-build floor of **771/772** — a 25x margin — and `material_preview.cpp` never assigns `points` or `spots`, so nothing here can reach it. **Row 9's A/B**: both builds pin to **16.674 ms with 0 dropped frames** over 1500 frames; eight spots cost **0.107 ms** of scene render, 0.64 % of a frame. **Row 6's sample half**: the phase-1 lamp is **3.9x dimmer** (peak contribution +21.0 -> +5.4 levels) and **41 % tighter** (1040 -> 613 px), 0 WARN, which independently reproduces row 1's predicted 4.0x. **INPUT METHOD, MEASURED AND NOT TO BE RE-DERIVED:** the screen must be **UNLOCKED** or the window server gives every application **zero windows**; synthetic clicks and drags drive ImGui fully and a drag field is driven by **dragging**; a bare mouse **move does not update the cursor**, so a hover proves nothing; **text entry NEVER arrives** by any encoding while **Backspace and Escape do**; **modifier shortcuts do not arrive**, so commands must come from the menus. Tracy listens on **port 8086 and accepts ONE server per client run**, so a stale process holds it and every later capture fails — and a client that starts while the port is taken opens **no listener at all**, so it can never be profiled. **AND THE TRAP THAT INVALIDATED A FIRST ATTEMPT AT THREE ROWS: the cooked shaders live in ONE directory per preset and `AERO_SHADERS_DIR` is compiled into every binary as that path, so two binaries built from different commits both read whichever shader set was cooked LAST.** A build A/B must snapshot `build/<preset>/shaders` per side and restore the matching set before each run; without that the phase-1 sample reported **byte-identical**, and with it the same comparison moves **412 933 pixels**. An A/B that shares one shader directory silently compares a build against itself.** 15 tasks remain, planning only. Inserted between 3 and 4; six epics, 24 tasks in `docs/tasks/phase-E.md`. Viewport legibility (E.1), lighting & environment (E.2), inspector & context routing (E.3), project/scene/asset management (E.4), content-creation UX (E.5), shell identity (E.6). **E.1.1** 8/10 PASS 2026-09-03 · **E.1.2** 8 PASS / 2 PARTIAL / 1 NOT EXECUTABLE 2026-09-04 · **E.1.3** 11 PASS / 1 PARTIAL / 2 NOT EXECUTABLE / 1 NOT RUN 2026-09-05, and that pass found the ortho gizmo-suppression defect fixed in PR #95 · **E.1.4** 10 PASS / 1 NOT EXECUTABLE 2026-09-05 · **E.1.5** 11 PASS / 1 NOT EXECUTABLE 2026-09-05. **E.2.1 is macOS-validated — 10 PASS / 3 open, 2026-09-06.** Its render rows were measured head­lessly through the real `SceneRenderer` -> `SkyPass` -> `PostProcess` chain with a framebuffer readback (no ICC round trip, so the bytes are exact): **every sky oracle difference is 0**; the unlit cube reads `(85,117,162)` on its top face against `(73,91,124)` on its side — dim, not black; ortho is exactly **one** colour; and **a world with no `Environment` is bit-identical to one with a default component, 0 of 921 600, with a 20 694-px anti-vacuity control**. Confirmed **in the editor**: New Scene seeds **four** entities with `Environment` ninth in Add Component, its eight fields at exact defaults, **no Transform**, E.1.4's marker at the origin and **no gizmo**; the grid stays legible over the ground (**Δlum 52.1**); each field edit is **exactly one** named undo entry and two undos restore the viewport to **0 differing pixels of 1 795 500**; the multi-Environment WARN fires **once and latches**, the loser's edit moves **0** sky pixels and the winner's **302 820**; and **Solid + Flat + intensity 1 is bit-identical to the branch-point build — 0 of 1 795 500, with a 1 792 032-pixel control**. Cost is **below the ~0.7 CPU-s noise floor**. `phase-1-scene`, a pre-task sample, renders under the default sky at 85 fps with zero WARN. **Three rows remain open**: row 4's normal-mapped arm is **GATED ON E.5.1** (a material on a *primitive* is silently discarded, so the default Cube cannot carry one — a dependency nobody had recorded), row 7's `Save Scene` produced no write under synthetic input while every other menu action worked (**possibly a real defect, unresolved**), row 10 has no material asset and row 12's release editor never connected to `tracy-capture`. **THE GUI WAS REACHED BY WRAPPING THE BINARY IN A MINIMAL `.app` BUNDLE**: a bare binary launched from an automation session gets **no window**, but `open`ing an `.app` gives it a Foreground LaunchServices identity, after which window geometry, `CGEvent` input and PID-bound capture all work. **`System Events`' own `click at` does not drive ImGui.** **HiDPI is deliberately NOT a row here** (a fullscreen gradient has no size-dependent feature), so E.1.1's thick-line handoff stays UNFIRED for a sixth task. **A branch-point A/B must be built at the PRIMARY binary path**: a binary elsewhere is a distinct application identity to macOS and receives no window. Windows and Linux unvalidated, as everywhere. |
| **Phase E gate** | Open a project and land in the scene you were last editing, on a lit grid floor under a sky; create a Cube from the menu, drop a material on it and see it shade; aim a spot light with a visible gizmo; rename, move and delete assets without leaving the editor. Gate artifact: `samples/phase-E-editor/`. |

### Engine layers, in dependency order

`core` → `assets` → `audio` → `platform` → `rhi` → `render` → `reflect` → `scene` → `scene_render` →
**`scene_audio`** → `scene_serialize`, plus `/editor` (`aero_editor_core` + `aero_editor`) and
`/tools` (`reflect-gen`, `shaderc`, `cooker`). **`/runtime` is still empty** — it arrives in Phase 5.

* **`engine/assets`** (opened 3.3.1) holds the cooked-asset formats and nothing else — eleven pairs:
  `cooked_mesh`, `mesh_cook`, `cooked_texture`, `texture_cook`, `bc_block`, `cooked_skeleton`,
  `skeleton_cook`, `cooked_animation`, `animation_cook`, `cooked_audio`, `audio_cook`. It links
  `aero::core` + `aero::profiling` and **no vcpkg package at all**.
* **`engine/audio`** (opened 3.7.1) holds the runtime clip and, since 3.7.2, the playback surface —
  four pairs: `clip`, `spatial`, `mixer`, `system`, plus the `audio.hpp` umbrella. It links
  `aero::core` + `aero::assets` PUBLIC, `aero::profiling` PRIVATE, and **NO VCPKG PACKAGE, EVER**.
* **`engine/scene_audio`** (opened 3.7.2) is the **World → audio bridge** and **the only code in the
  tree that sees both `engine::scene` and `engine::audio`**, sitting above both — which is what keeps
  `audio` scene-free and `scene` audio-free. `PUBLIC aero::scene aero::audio` /
  `PRIVATE aero::profiling`, and **never `aero::scene_internal`** (which carries `EnTT::EnTT`
  INTERFACE by design). Folding its walk into `engine/audio` would put **EnTT on the link line of every
  binary that links audio**, including the Phase 5 runtime.
* **`/editor`** gained TWO source pairs at E.3.2 (`context_router`, `editor_prefs`) on top of E.2.4's one (`material_preview_rig`) and E.2.3's two (`viewport_icons`, `viewport_gizmos`). **THE "PAIR COUNT" THIS LINE USED TO CARRY IS DROPPED, BECAUSE IT IS NOT REPRODUCIBLE**: E.2.4 measured the tree six ways looking for the 28/30 figure recorded here since E.1.5 and **none of the six is it** — the metric behind that number is undefined, and it had already been flagged as carried arithmetic rather than a measurement. The two figures anyone can re-run are `git ls-files`: **`editor/src/*.cpp` 80 -> 82** and **`editor/include/aero/editor/*.hpp` 58 -> 60** at E.3.2. Use those; `/tools` links `aero::assets` and `aero::editor_core`
  through `aero_cooker`, which is legal because `tools/` is enumerated by neither half of the golden
  rule.

### Standing invariants that govern new work

**THE EDITOR HAS EXACTLY ONE FOCUS SLOT, AND `ImGui::SetWindowFocus` IS CALLED AT MOST ONCE PER
FRAME FROM IT (E.3.2).** It lives in `shell_ui.cpp` immediately before `DockSpaceOverViewport` — dock
nodes update inside it, so the focus lands with no one-frame lag — and it resolves both paths in order:
an explicit `requestPanelFocus` first (a COMMAND), then the pending context route (an INFERENCE).
`I159(a)` pins that exactly one file names the symbol and calls it exactly three times. **A fourth
caller in another file is a second focus policy with no way to order it against the first.** The reason
is that **`FocusWindow`'s two side effects are not idempotent**: it closes every popup above the focused
window (`imgui.cpp:13740`) and **steals the active widget** (`:13754-13756`, whose own comment at
`:13751` names this very slot), and a stolen `InputText` edit is DISCARDED rather than interrupted.
**Re-read `:13740` and `:13754` at every ImGui bump**, exactly as `ImGuizmo.cpp:1229-1230` and
`imgui.cpp:8848` already require.

**A ROUTE NEVER RE-OPENS A PANEL THE USER CLOSED, AND EVERY DROP IS TESTED BEFORE EVERY HOLD (E.3.2).**
`targetAvailable` is *registered AND visible*, and a hidden target is a **Drop**, not a Hold — closing
a panel is the user's second, coarser off switch. The five terminal conditions can each persist
indefinitely, so holding on one would hold forever; the four transient ones all end on a mouse-up, a
click-away or an Escape. **Never reorder a Drop below a Hold.** And the router **derives nothing**: it
spells neither `isImportableModelName` nor `isBlendFileName`, reading the `SessionState` the import
session itself wrote, which costs one extra tick. If that ever has to be one tick, the fix is a
settled-state signal ON THE SESSION, published before `ShellUiState` is built — never a predicate in
the router.

**A PREFERENCE THAT SUPPRESSES AN EFFECT MUST NOT SUPPRESS THE OBSERVATION THAT FEEDS IT (E.3.2).**
`ContextRouter`'s three `observe*` functions gate the **LATCH**, never the early return: a baseline
advances on every tick whatever the preference says, so an act performed while routing is off is SEEN
AND FORGOTTEN rather than SKIPPED. Gating the early return instead leaves the baseline stale, and the
first observation after the preference comes back on raises a panel for a minutes-old act — a focus
steal triggered by ticking a menu item, which the whole green matrix was blind to. **`observeImportTarget`
is the one exception and it is deliberate**: `!settled` stays in its early return, because D4's rule is
the SESSION's and is independent of the preference. **The general shape: when a switch turns a REACTION
off, the state that decides "is this new?" must keep advancing, or the switch becomes a delay line.**

**`.claude/rules/editor.md` — the same section carries this.** And the case that was supposed to catch
it, `RT24`, **pinned the defect while calling it correct**: it re-observed the value produced while
disabled and named that "a fresh act". **A case written from the same sentence as the code cannot
falsify that sentence** — which is why a seed that restores the defect, not a re-reading, is what
closed it.

**`Selection::prune` MUST NEVER BUMP `Selection::revision` (E.3.2).** `HierarchyPanel::onDraw` prunes
every frame, so a bumping prune is a permanent focus storm; `I159(e)` pins the absence as source text
because a prune that bumped and un-bumped would satisfy the effect. And **`set`, `toggle` and `setAll`
delegate to two PRIVATE, NON-COUNTING helpers**, never to `add`/`remove`: the counter counts public
CALLS, so routing them through the public mutators makes `set` bump twice and `setAll(n)` bump n + 1.

**IMGUIZMO'S TWO VISIBILITY SETTERS ARE CROSSED, AND NO TIER IN THIS TREE CAN READ EITHER VALUE.**
`SetPlaneLimit` hides **axes** and `SetAxisLimit` hides **planes** (`ImGuizmo.cpp:1229-1230` against
the setters at `:2657-2670`); the header's own comments say the opposite because they describe the
NAMES. The crossing is spelled in exactly one place — `viewport_panel.cpp`'s `applyGizmoStyle` — with
the citation beside it, and `I125(c)` pins it as source text. **There is no getter for either member,
so a port bump that un-crosses them is green and wrong. Re-read `:1229-1230` at every bump.**

**THE GIZMO STYLE IS A PROCESS-WIDE GLOBAL WRITTEN EVERY SUBMITTED FRAME, AND A SECOND WRITER IS AN
UNDECLARED DECISION.** `ImGuizmo::GetStyle()` returns `gContext.mStyle` by reference and
`BeginFrame()` does not touch it, so the viewport's per-frame write is idempotent and self-healing —
**which means a task that wants a different gizmo style must change `defaultGizmoStyle()`, never
write the global from a second site.** `I125(e)` asserts `ImGuizmo::GetStyle()` appears exactly
**twice** in `viewport_panel.cpp`. The style's colours are DERIVED from `axis_palette.hpp` and
`viewport_panel.cpp` states no gizmo colour literal at all; **a restated literal one byte off is
invisible to every automated tier** (E.1.4's sabotage row 20), which is why the derivation is
structural rather than reviewed.

**A RENDER TARGET'S DEPTH IS ONLY READABLE IF IT WAS STORED, AND NOTHING CAN DETECT THAT IT WAS NOT.**
E.1.4 added `RenderTargetConfig::depthStore` and `PostProcessConfig::sceneDepthStore`, both defaulting
to today's `Clear` → `DontCare`. On a tile-based deferred renderer — **every Apple Silicon Mac** —
`DontCare` means the tile's depth is never written back, so a later `LoadOp::Load` reads **GARBAGE,
not stale-but-plausible values**: an EMPTY result on Metal and a CORRECT one on D3D12/Vulkan, which is
the worst failure shape there is. **And a pass attaching an existing depth with `LoadOp::Load` must
spell BOTH load ops**: the rhi cycles a depth target iff ANY load op is not `Load`, `stencilLoadOp`
defaults to `DontCare`, and the combination trips `SDL_BeginGPURenderPass`'s own assertion and **HANGS
the process** rather than failing it.

**A PASS THAT RE-RASTERISES GEOMETRY AND COMPARES AGAINST AN EXISTING DEPTH MUST PAIR WITH THE SAME
VERTEX STAGE, NOT AN EQUIVALENT ONE.** No graphics API guarantees position invariance across two
different vertex shaders; the failure mode is a **speckled mask that reads as a depth-bias bug**.
E.1.4's mask pass therefore reuses `scene.vert` / `scene_skinned.vert` verbatim and pays
`GpuPerObject`'s 208 bytes rather than a `Mat4`. It also uses `CompareOp::LessOrEqual`, never `Less`:
`Less` rejects every fragment of geometry whose depth is already in the buffer, and the mask comes out
empty.

**A PASS WHOSE TARGET HAS MARGIN MUST SET VIEWPORT AND SCISSOR, AND NO `quantum = 1` TEST CAN SEE
THAT IT DID NOT.** `renderShadowMap` sets neither, correctly, because its texture has no margin.
`renderSelectionMask`'s does: with `beginRenderPass`'s default full-target viewport the mask maps
across the ALLOCATION while the resolve maps across the DRAWN rect, so the result is silently rescaled
and slides as the panel is resized. Every convenient test target is `quantum = 1`, where
`drawExtent == textureExtent` and the bug is invisible; `OG7` uses a 200x140 draw inside a 256x192
allocation and is the only case that can catch it.

**THE RHI GREW EXACTLY TWO CALLS AT E.1.1, AND EACH HAS ONE SENTENCE THAT VOIDS SILENTLY.**
`Device::recordBufferUpload` **replaces the WHOLE buffer** — everything past `data.size()` is
undefined afterwards, because the destination is cycled, which is why there is no `dstOffset` and why
a partial write cannot merge with last frame's contents. It refuses a command buffer with **a render
pass open, in every configuration**: that refusal is OURS, not SDL's, which only checks under
`debug_mode`. `Device::readbackTexture` is **BLOCKING and a test-and-tooling path** — and **its
`SDL_WaitForGPUFences` call is what performs the copy on D3D12**, where the download is deferred into
`D3D12_INTERNAL_CleanCommandBuffer`. Map before the wait and you read garbage **on Windows alone**.
Release the fence **after** the wait, never before.

**THE TREE ASSERTS PIXELS NOW.** `RU5` was the first; `DG5`–`DG16`, `DG18` and `I108`–`I113`
followed, on Metal, WARP and lavapipe, on every push. **But a pixel assertion on GENERATED geometry
must not name a single pixel**: E.1.2 measured that under a centred camera an axis lands on a pixel
*boundary*, where Metal lights row 95 while the projection names 96, so `DG18` bounds where the lit
**run** starts to **+/-1** — the width of the genuine fill-rule ambiguity. **A comparison struct
declared inside a `TEST_CASE` cannot carry an `operator<<` at all** ([class.friend]/6 forbids defining
a friend in a local class); hoist it to the file's anonymous namespace or every assertion prints
`{?} == {?}` on failure. **A later visual task that settles for "no backend error" is
choosing to, not forced to** — `readbackTexture` plus a `RenderTarget` under an identity camera makes
"which pixel" arithmetic rather than judgement. And a test-local `operator<<` is not optional there:
`CHECK((a == b))` prints `CHECK( true )` on a FAILURE as well as a pass, which makes the assertion
carrying the deliverable unreadable at exactly the wrong moment. An `operator<<` on the test's own
comparison type, never a `toString` (that is the ADL trap that hard-errors inside `doctest.h`).

**A DEPTH BIAS DOES NOTHING TO A LINE PRIMITIVE.** Measured at E.1.2 on the real `DebugDraw`: the
`Tested` line pipelines are `PrimitiveType::LineList`, and D3D12 and Metal both exclude point and line
primitives from rasterizer depth bias by specification while Vulkan permits without guaranteeing. A
13 x 5 sweep moved a line at no gap down to `1e-5`; the same bias moved a `TriangleList` billboard
predictably, bracketing Metal's unit at `2^-24` for `D32Float`. **`DebugDrawConfig` therefore has no
bias field, and adding one for lines would be inert on two of three backends.** The remaining
consumers are triangle topologies: the shadow pass (`forward_renderer.cpp`, the tree's only live
consumer), and **E.2.3 EXAMINED the handoff and DECLINED it** — every billboard it pushes is
`DebugDepth::Overlay`, whose pipeline does not test depth at all, so there is nothing for a bias to
correct. `DebugDrawConfig` is still byte-identical. **The handoff is RE-ISSUED** to whichever task
first wants a depth-*tested* billboard.

**A TEST THAT COMPARES TWO VALUES FROM THE SAME SOURCE ASSERTS NOTHING, AND READING IT WILL NOT TELL
YOU.** E.1.2's sabotage matrix found **two** such cases, each green on a seeded regression that is
plainly visible in the product. `GR8` was named for the cadence crossfade's continuity and computed
its own `f → 1` side from the emitter's formula, then compared it against the emitter's `f == 0`
side — both halves from one source; the naive two-set crossfade passed **13.3 million assertions**.
`GR12`/`GR6` missed a snapped disc centre that **jumps ten world units for a 0.2-unit pan**. **The
code review read both and approved both**, correctly: the flaw is invisible to reading, because such
a test looks exactly like one that works. **Only mutating the code and watching the test not care
exposes it.** Two rules follow: read back the value under test **off the thing under test**, on both
sides of any identity; and remember **a case is only as strong as the pose it samples** — `GR6`'s arm
was blind because its focus defaults to the origin and `round(0/s)*s == 0`, so an invariant about
following the camera has to be asserted at a focus deliberately OFF the lattice, with an
anti-vacuity arm proving it is.

**AND A CLAMP THAT BOUNDS A LOOP DOES NOT NECESSARILY BOUND A COUNT.** `debug_grid.cpp` claimed its
span clamp made `DEBUG_GRID_MAX_LINES` structural; removing the clamp leaves the battery green with
an identical assertion count. The count is bounded by the **disc clip**, not the clamp. Two readings
passed over that sentence before a seed disproved it.

**A NON-DEFAULTED PARAMETER ON A WIDELY-CALLED EDITOR FUNCTION IS A 57-LINE EDIT, AND IT IS STILL THE
RIGHT CALL.** `buildSelectionOverlay` has **38 call sites, 37 of them in
`selection_overlay_test.cpp`**; `projectToViewport` has 7, `clipSegmentToNearPlane` 5,
`gizmoOriginBehindCamera` 7, and `overlayOwnsPress` 16. A default lets a future site silently take
the wrong arm — a wrong picture with no error and no failing test; non-defaulted makes every
unconverted site a compile error, which is what makes such a change atomic. **Pass a file-local
`constexpr auto` alias, never the full enum spelling**: seven characters per line instead of forty,
which is what keeps them under the 120-column CI skew. **And a mechanical rewrite must not count
commas at bracket depth** — `std::array<Entity, 1>{cube}` hides one inside a template argument list,
which put the new argument one slot early on 17 of 37 sites at E.1.3.

**A `-tc=` FILTER IS A GLOB, NOT A REGEX, AND A FILTER THAT MATCHES NOTHING EXITS 0.** `*PK1[35]*`
selected **zero** cases and the binary reported success — which reads as "the seed reverted cleanly"
during a sabotage run on a tree where the seed was still live. **Read doctest's own `test cases:` line,
never the exit code alone**; same species as the vacuous-grep trap below.

**THE DEBUG BATCH IS SHARED, SO "THE BATCH IS EMPTY" IS A WHOLE-EDITOR CLAIM — AND THE WALL IS FOUR
CASES, NOT THREE.** Any task that adds a producer to `render::DebugDraw` reddens every case that
counts the batch exactly. E.1.2's grid took `I108`/`I109`/`I111` red; **E.2.3's icons took `I108`,
`I109` and `I112`** — `I111` stayed green (it reads only LINE quantities and icons are billboards) and
`I113` uses a `>` bound. **Fix it with the producer's own toggle seam, never by restating the
magnitude**, and remember `uploadCount()` is a *lifetime* counter, so for `I108`/`I109` the toggle must
precede the warm-up ticks while `I112` may take it after them (it reads only per-frame counters).
**AND THE FOURTH CASE IS INVISIBLE TO THE COUNTER GREP: `I110`'s roster subcase** walks
`editor/src/*.cpp` and asserts exactly which files name `DebugDraw` at all, so it goes red the moment a
new producer TU lands — four commits before that producer has a call site. Sweep for the TYPE NAME as
well as for the accessors. **The next producer inherits all four toggles.**

**THE `find_package` BOUNDARY.** `aero_assets`, `aero_audio` and `aero_scene_audio` link **no vcpkg
package at all**, which makes their `PRIVATE` links a **real compile-time boundary** rather than
convention-plus-grep: vcpkg installs every port into one shared per-triplet `include/` root that lands
on the compile line of any target linking any vcpkg package, so a stray `#include <miniaudio.h>` in
`engine/audio` is a **hard compile error — IN THE PROFILING-OFF CONFIGURATIONS ONLY**, which 3.7.3
measured and which 3.7.2's "verified in both directions" reading had not distinguished:
`aero::profiling` is `PRIVATE` on all three and carries `Tracy::TracyClient` when
`AERO_ENABLE_PROFILING=ON`, and a target's own `PRIVATE` usage requirements apply to its **own**
compile line, so `mixer.cpp` carries `vcpkg_installed` in `macos-release` and not in `macos-debug`.
**Never write "hard compile error" about a vcpkg-free target without saying in which configurations.**
**Adding a `find_package` to any of those three voids it silently while CI stays green** — and since
3.7.3 that is **guard-enforced for all three files**, not just the audio half:
`.github/scripts/check-audio-boundary.sh` prong A, plus `tests/audio_boundary_probe.cpp` for the
compile-time half that survives Release, plus `audio-boundary.guard_e2e` for the proof it goes red.
**A FOURTH vcpkg-free target must add itself to `VCPKG_FREE_CMAKE` in the commit that creates it, and to nothing else** — the target list and the sweep's skip test are DERIVED from that one roster, after three parallel lists made a target guarded by three prongs and invisible to the fourth —
intent cannot be derived from the tree, so an unlisted target is silently unguarded.

**FOUR GREPS ARE NOT LITERALLY ZERO AND MUST BE READ RATHER THAN COUNTED:**
the `tools/` process-spawn grep ("libsdl-org fork" in `tools/shaderc/README.md`); INV-A1's float grep
over the four audio cook/container files; 3.6.3's `applyOetf|gammaEnabled|linearOutput|skipEncode`;
and **`#if` over 3.7.2's four new test files**, where every match is the prohibition *sentence*. The
usable form for the last is line-anchored:
`git grep -nE '^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif|else|endif)'`.
**It was FIVE until 3.7.3.** The `find_package` reading over `engine/assets`, `engine/audio` and
`engine/scene_audio` is now `check-audio-boundary.sh`'s self-test 2, with those very prohibition
comments as its anti-vacuity canaries — deleting one is a loud exit 2, never a quiet pass.

**AND A GREP THAT LOOKS FINE CAN BE SILENTLY VACUOUS.** `git grep -- $F` with a pathspec list in a
shell variable **does not word-split under zsh**: it searches one bogus pathspec, matches nothing,
exits 1, and a `&& … || echo "none (ok)"` idiom **reports a clean result on a dirty tree**. Use `${=F}`
or literal paths, and **check every guard grep in BOTH directions**. Same species as the `->Data`
prefix trap and the POSIX `\b` degradation on BSD: **a guard command that cannot fail is worse than no
guard command.** A shell corollary found at 3.7.2: `echo "exit=$(basename $X) $?"` reads the *command
substitution's* status, not the one you meant.

**`git checkout -- <file>` REVERTS TO HEAD.** Seeding a file to prove a fix **also reverts the fix if
it is not committed**. It ate two closures during 3.7.2. **Commit the fix, then seed it.**

**clang-tidy NEEDS THE PINNED SDK.** With `SDKROOT=$(xcrun --sdk macosx --show-sdk-path)` it exits 1
with tens of thousands of errors inside system libc++ headers (`__builtin_clzg` and friends), because
the generic SDK resolves to a newer libc++ than LLVM 18 can parse — **a red verdict on a clean tree**.
Use `macosx15.4`. And **read the EXIT CODE, never a tail**; `SDKROOT=… git diff | xargs clang-tidy`
binds `SDKROOT` to `git diff`.

**`AERO_BUILTIN_COMPONENT_HEADERS` NAMES THE BUILT-IN COMPONENT HEADERS ONCE**, at root scope. It
reaches **four** generation sites and **three of the four are silently optional** — a component added
to the editor's list and not the serializer's is registered, inspectable, editable and **NOT SAVED**,
with every test green. **The site the variable does not reach is the one that matters**:
`engine/scene_serialize/src/scene_serialize.cpp`'s hand-written dispatch table, plus
`builtin_serializers.hpp`'s declarations. All five move together or the result is a link failure or,
far worse, green and wrong. **There are TEN built-ins**: `Transform`, `Camera`, `DirectionalLight`,
`PointLight`, `MeshRenderer`, `AnimationPlayer`, `AudioSource`, `AudioListener`, `Environment`
(E.2.1), **`SpotLight`** (E.2.2). **`createEntity` (`entity_ops.cpp:68`) ALWAYS adds a `Transform`**, so a seeded entity that
must not carry one is built with `world.create()` + `setName` directly — E.2.1's "Environment" seed is
the precedent, and its `hierarchy_test` placeability block is four per-entity statements with
`CHECK_FALSE` on the fourth, never a universal loop.

**A COMPONENT-COUNT LITERAL IS NOT CONFINED TO THE TESTS THAT ARE ABOUT COMPONENTS.** 3.7.2 found them
in `tests/scene_test.cpp`, `tests/transform_test.cpp`, `tests/editor/hierarchy_test.cpp`,
`tests/editor/inspector_test.cpp` and `tests/scene_serialize_test.cpp` — **every one found by a red
test, none by a grep.** Adding a built-in means finding all of them.

**AND E.2.1 MEASURED THE OTHER DIRECTION TOO: THE GREP ALSO OVER-MATCHES.** Its sweep came to **21**
built-in literals (not the 18 planned) and **19** default-scene pins, against **five look-alikes that
had to stay unchanged** — `scene_test.cpp:224`/`:1175` (create/destroy and generation counts),
`scene_serialize_test.cpp:596` (the fixture's entity-name roster), `hierarchy_test.cpp:468`
(`destroyEntities` arithmetic) and `scene_io_test.cpp:281` (a hand-written three-entity chain). Three
of the 21 are reachable by **no grep at all**: a component *tally* rather than a type count, a literal
on the line *after* its `componentTypeCount()` call, and **`PB13`'s TEST-CASE NAME**, which carried
`400-byte Lights block` with its arithmetic in prose beneath it. **A count is only as good as the grep
behind it — re-run every one, and classify each hit as a real pin or a look-alike by READING ITS
COMMENT, never by its shape.**

**THE DETERMINISM MANIFEST IS FROZEN** at **20 hash lines across FIVE arms / 40 cross-lane
comparisons**. A red manifest case is `docs/09` §9.11's `cookerVersion` sentence firing; the five
constants to bump are `COOKED_{MESH,TEXTURE,SKELETON,ANIMATION,AUDIO}_COOKER_VERSION`, and the
regeneration ritual lives in the manifest's own header. **Never edit a hash to green a red run.**
**mp3 and ogg are deliberately NOT in it and never may be** (`docs/09` §14.7): their decoders run
floating-point transforms whose paths differ by SIMD availability and FMA contraction policy.
`cooker.audio_lossy_digests` prints both digests on every lane and asserts **no digest value**.

**THE `toString` TRAP.** doctest's `DOCTEST_STRINGIFY` expands to an **unqualified** `toString(...)`,
so an engine `toString(SomeEnum)` on a public header is found by ADL, beats doctest's own template, and
the decomposer then tries `std::string_view + const char*` — a **hard compile error on every lane**,
reported inside `doctest.h`. Name label functions anything else (`audioClipLoadStatusLabel` is the
precedent). A scoped-enum comparison inside a `CHECK` needs **double parentheses**.

**FOUR TEST-FILE RULES**, each earned: **no `#if` of any kind** in a test file (3.6.3 shipped four
cases inside a file-level `#if` with everything green while the one arm that mattered never ran);
**`#include <ostream>`** in any TU that `CHECK`s a `std::string_view` (the 0.4.1 MSVC trap, hit five
times); **exact float assertions where the arithmetic is exact**, and a tolerance **with the epsilon as
part of the assertion** where it is not; and **assert the EFFECT, never the INTENTION** — the single
most repeated failure in this project's review rounds.

`git grep -nE '_WIN32|__APPLE__|__linux__' -- engine/assets engine/audio engine/scene_audio
engine/render engine/scene tools/cooker` reads **zero lines**; the same grep over `editor/src` +
`editor/include` reads **exactly three lines in one file** (3.2.4's `currentHostOs()`).

### Test inventory — measured, never remembered

Read totals from **doctest's own `filters:` line**, never from a `grep -c` of case names, and
**re-measure on the tree in front of you**: the moment a branch merges `origin/main`, every whole-tree
count on its own page goes stale, and adding one task's delta to another task's baseline is exactly the
arithmetic that produces a confident wrong number.

At **E.3.1's** gate — measured on both presets and agreeing between them, on the merge of E.3.1 into
E.3.2's `main`: **`ctest -N` 174**, UNMOVED, and doctest **1404 / 1886 / 201 / 40 / 55 / 7 / 28**. The
arithmetic is the check that the merge lost nothing: `201 = 181 + 7 (E.3.1) + 13 (E.3.2)` and
`55 = 31 + 24 (E.3.1)`. **The four that neither task moved are `aero_tests` (1404),
`aero_scene_serialize_test` (40), `aero_reflect_meta_test` (7) and `aero_reflect_json_test` (28)** —
neither adds a component. Guards **506 / 92 / 163 / 92 / 165 / A=6 B=82 / 11-3-55 / 6-57**, all unmoved
by E.3.1, which adds **no tracked file**. The E.3.2 figures immediately below are its own gate and are
superseded by these.

At **E.3.2's** gate, measured on both presets and agreeing between them: **`ctest -N` 174**, UNMOVED
since E.2.2's, and its entry **SET byte-identical to the branch point's** — none of E.2.3, E.2.4 or
E.3.2 adds a component, a target or a ctest entry. doctest across **seven** binaries
**1404 / 1886 / 194 / 40 / 31 / 7 / 28** (`aero_tests`, `aero_editor_shell_test`,
`aero_editor_imgui_test`, `aero_scene_serialize_test`, `aero_editor_inspector_test`,
`aero_reflect_meta_test`, `aero_reflect_json_test`), up from E.2.4's **1404 / 1842 / 181 / 40 / 31 /
7 / 28**, E.2.3's **1400 / 1829 / 170 / 40 / 31 / 7 / 28** and E.2.2's
**1377 / 1793 / 163 / 40 / 31 / 7 / 28**. **THE `1827` RECORDED HERE UNTIL E.2.4
WAS STALE BY TWO**: it was measured at E.2.3's *sabotage* gate, before that task's code-review round
added two cases, and the phase table's `1829` — measured from a build of E.2.3's branch-point content
— was right all along. E.2.4 re-measured both from a freshly rebuilt tree and they agree. **A task
that adds a BUILT-IN moves `ctest -N` too** (it adds a per-header reflect-gen case) **and three of the
seven doctest totals; a task that adds none moves only the doctest totals of the binaries it writes
cases into.** The four that none of E.2.3, E.2.4 and E.3.2 moved are `aero_scene_serialize_test` (40),
`aero_editor_inspector_test` (31), `aero_reflect_meta_test` (7) and `aero_reflect_json_test` (28):
the last two generate from a fixture aggregator, not the built-in list, and a moved serialize or
inspector total on a no-component task means a component crept in — stop and find it. **A `SUBCASE`
is not a `TEST_CASE`** — E.2.1's last two commits added 21 assertions and moved no total at all,
E.2.2's three review-round commits added assertions to four files and moved no total either, E.2.3's
two sabotage-forced commits added six assertions across two files and moved none, and **E.2.4's
`I140(c)` is a third subcase on an existing case and contributed ZERO**. **E.3.2 added EIGHTEEN
`SUBCASE`s and they moved nothing; its three sabotage-forced commits moved the shell total by exactly
one (`EP12`), and its two code-review commits moved it by one more (`EP13`) and the imgui total by two
(`I160`, `I161`) while `I159(f)` — a sixth subcase on an existing case — moved nothing.** Never predict a delta
arithmetically.

**AND A BUILD TREE GOES STALE SILENTLY TOO, WHICH `ctest -N` CANNOT SEE.** At E.2.4's branch point
`build/macos-debug` was a build of a pre-E.2.3 commit and reported E.2.2's doctest totals, while
`build/macos-release` was current — and `ctest -N` read **174** out of *both*, because the entry count
is a configure-time property. **Rebuild before you believe any doctest number**, and read the totals
out of both presets so a disagreement is visible.

**AND A RECORDED TOTAL GOES STALE SILENTLY: `origin/main`'s own shell total was ONE stale at E.1.4's
gate** — it said `1780`, measured at PR #94's gate, and PR #95 then added `G19` while nobody
re-measured. Read the binary, never the block.

The eight guard counts at **E.3.2's** gate: math **506**, platform **92**, rhi **163**, scene **92**,
golden-rule **165**, project-no-delete **A=6/B=82**, audio **11/3/55**, probes **6/57** — math and
project-no-delete moved by E.3.2's six new tracked C-family files (four editor, two tests), and the
other six are unmoved from E.2.4's **500 / 92 / 163 / 92 / 165 / A=6 B=80 / 11-3-55 / 6-57**. **A THIRD
guard moving on a task of this shape is a stop-and-find-out.** **The two
reduced configurations read `161` (shader-tools-OFF) and `93` (reflect-tools-OFF)**, both re-measured
fresh at that gate. **THE `159` RECORDED HERE UNTIL E.2.2 WAS STALE, AND THE ARITHMETIC SAYS SO**:
shader-tools-OFF removes exactly the 13 `shaderc.*` entries and adds nothing, so it was
`173 - 13 = 160` at E.2.1's gate and is `174 - 13 = 161` now — the number should have moved when E.2.1
added a `reflect-gen.*` entry, which is `AERO_REFLECT_TOOLS`-gated and therefore present in this
configuration. 3.7.3's remembered `80 / 93` is HALF WRONG for the same species of reason. Compare the
entry **SETS**, not the totals — measured by comparing entry NAMES with the ctest numbering stripped,
because a raw `diff` of `ctest -N` output is dominated by the renumbering and shows every later entry
as changed: shader-tools-OFF removes exactly the **13 `shaderc.*`** entries; reflect-tools-OFF removes
the **`reflect-gen.*`** entries **plus four doctest binaries**; **nothing is added in either**; and all
**70 `cooker.*` entries are present in all three**, which is the property that check is actually about.
**THE reflect-gen BREAKDOWN RECORDED HERE WAS ITSELF STALE UNTIL E.2.3 AND IS THE SAME SPECIES AGAIN**:
it said 75 + 4 = 79 removals, measured at E.2.3's gate it is **77 + 4 = 81** (174 - 81 = 93), because
E.2.1 and E.2.2 each added a per-header `reflect-gen.components_*` entry after that sentence was
written. **The TOTAL was right and the breakdown was not — re-measure both, every time.**
**`check-math-boundary.sh` counts
`git ls-files`, so it reads a STALE number until new files are `git add`ed** — stage first, then
measure. A moved `ctest -N` on a task like that means a CMakeLists copied
from the wrong template. Both reduced configurations
must be configured **FRESH with `-G Ninja`**, **with an explicit
`-DCMAKE_TOOLCHAIN_FILE=<src>/vcpkg/scripts/buildsystems/vcpkg.cmake`** — the `base` preset supplies it
and a raw `cmake -S . -B …` does not, which fails at `find_package(spdlog)` in
`engine/core/CMakeLists.txt:16` before it reaches anything this configuration is about — and, since
3.7.3, **with
`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`**: only the presets set it, so a raw `cmake -S . -B …` writes no
`compile_commands.json` and `boundary-probes.probe_compile_line` skips. It reports that skip honestly
(exit 77 → ctest "Skipped") rather than passing, but a skipped case measures nothing — `CMAKE_GENERATOR` enters the shadercross bootstrap's
option hash, so the generator-less form reads the cached toolchain as **cold** and pays a from-source
DXC rebuild that peaked at 7.6 GB here — and each must **name which binaries it built and ran**
(`aero_tests`, `aero_editor_shell_test`, `aero_editor_imgui_test`, `aero_cooker`).

**Read the two kinds of move differently:** a `cooker.*` addition must be **identical in all three**
(`aero_cooker` takes no gate flag), and a `reflect-gen.*` addition must be **tools-ON only**. A smaller
move in a reduced configuration means the cooker block accidentally grew a gate, and **no test can
report that** — which is why it is a gate step. `aero_tests`, `aero_editor_shell_test` and
`aero_editor_imgui_test` each register as a **single** ctest entry, so hundreds of new doctest cases
move `ctest -N` not at all; an unmoved `ctest -N` therefore means "zero C++" only for a task that adds
no binary — check a zero-C++ claim against the **doctest** totals instead. **3.7.3 is the inverse
pattern and worth knowing exists**: it moved `ctest -N` by +2 while every doctest total stayed put,
because its two additions are `cmake -P` drivers and its one new TU is a probe with no `TEST_CASE`.

**Counts diverge by OS**, so never assume one: Windows skips **three** e2e cases —
`golden-rule.include_scan_e2e`, and since 3.7.3 `audio-boundary.guard_e2e` and
`boundary-probes.probe_links_e2e`, all `NOT WIN32` by the same D16 reasoning (the lint job that runs
the scripts is ubuntu-only, and the BSD-userland proof comes from the macOS lane) — plus fourteen
whole `BS` cases, one arm of `BS11` and one GPU case (`I80`). Those 3.2.4 skips are not a random
sample — together they are the only coverage anywhere of both Blender timeouts, cancellation, the
`Converted` state, `ok: false`, both `ArtifactUnusable` arms and the refused-by-cap log.

### Committed fixtures and artifacts

Two images at `tests/fixtures/assets/`; six 32×32 PNGs and six `.ktx2` under
`samples/phase-3-materials/textures/`; seven `.aeromat` at `tests/fixtures/materials/`;
`samples/phase-3-skinning/arm.aero{mesh,skel}`; `samples/phase-3-animation/wave.aero{mesh,skel,anim}`;
`tests/fixtures/audio/` (four encodings of one 1.0 s signal, `tone.aerowave`, the corrupt
`tone-lying-length.ogg`, and **`tone.s16le.pcm` — ffmpeg's own decode, the external anchor that ties
dr_wav and dr_flac to libavcodec byte for byte**); and, from 3.7.2,
`samples/phase-3-audio/{orbit,beacon}.aerowave` — mono 48 kHz 0.5 s, **exactly 48 064 B each**, cut at
a whole number of cycles so the loop seam is continuous *by arithmetic*. **Neither audio sample fixture
enters the determinism manifest**, and the README says so.

### The validation debt — the whole of the remaining risk

**TWO PAGES HAVE NOT BEEN RUN ON ANY PLATFORM: E.1.5's and E.3.2's.** E.3.2's is twelve rows
(`editor/validation/E.3.2-selection-follows-focus-router.md`, gitignored, so it enters no commit), and
**six of them are the ONLY cover a declared sabotage seed has anywhere**: row 2 for `S17`
(`sourceStillValid` hard-coded — nothing in `tests/` can load a scene between the reconcile block and
the focus slot inside one tick), rows 5 and 6 for `S24` (a raise must not discard a half-typed name or
move a drop surface mid-drag — **no tier in this tree can type**), row 7 for `S18`'s behavioural half
(a raise must not close an open menu; `I159(b)` pins only the flag spelling), and row 8 for the
gizmo-drag hold. **macOS is otherwise green.** **E.2.4 IS macOS-VALIDATED — 12 PASS / 12, 2026-09-11, no blockers and no partials**,
and the pass closed **all six of its declared sabotage seeds**: S30 (row 5 — at `intensity` **exactly
0.000000** the picture equals the no-sun picture, peak 140.1 both, while the notice stays away at 0
amber pixels, so `hasSun` keys on the RESOLUTION and never on the value), S22/S24/S25 (row 7 — Escape
closed the popup 35205 → 0 dark px; unchecking Gizmos removed the icon from the PICTURE, so the
checkbox drives the member; and the popup's top-left **(378,105)** sits at the button's
`(min.x,max.y)` **(380,104)** while the mouse was **17 px away**), and **S21 (row 8 — a dismissing
click landing ON a translate handle left the sphere at (503.0,366.0) → (503.0,366.0), and one Undo
afterwards reverted the ORIGINAL move, proving no spurious undo entry was pushed)**. Only **S19's
`SkyPass::create` failure arm remains uncovered, and it is unreachable at runtime** in any build whose
shaders cooked — the source-text pin in `I140(b)` is its only witness, as the page itself states.
**Row 2's by-eye judgement came out byte-exact**: the preview's peak sphere luminance and the
viewport's are **both (192,198,209), delta 0.0 levels**, over a 22-frame burst spanning a full orbit
with disjoint search boxes and each box's corner proven to be background. **Row 11's feared
regression is not one** — the sphere does dim, peak **230.0 → 212.4**, but the picture MEAN goes
**64.1 → 134.6** because a dark void became the scene's sky, so it reads as RIGHT and **no retune of
E.5.2's default light is warranted on this evidence**. **HiDPI is deliberately NOT a row** (this task
draws no line and no icon), so **E.1.1's thick-line handoff stays FIRED and unmoved**. **E.2.3 is
macOS-validated in full — 12 PASS / 12, 2026-09-08, no blockers and no partials.** Its rows 3 and 4 were the ONLY cover its declared seeds had, and **both are now covered**:
row 3 measured **0** stray ink around every on-screen icon (the `AddressMode::Repeat` seed), and row 4
was rescued from NOT EXECUTABLE when the 1x externals were disconnected mid-pass and the built-in
**3024x1964 Retina** panel became the only display (the framebuffer-scale seed). **E.1.1's thick-line
handoff is FIRED, not cleared and not deferred**: the icons scale exactly 2x and hold 22 points, but
**387 of 441 sampled runs across the gizmo are one device pixel** — 0.5 points, half their apparent
weight at 1x — and that applies to every `DebugDraw` consumer, the grid and world axes included. **E.2.2 is macOS-validated in full — 12 PASS / 12, 2026-09-07, no blockers and no
partials.** Seed 28's shadows-sample retune (row 11) and seed 5's declared hole are settled or stated;
the two cone defaults, the new `intensity` meaning and the raw-radian Inspector rows were all judged;
and the material-preview row that E.2.1 left blocked is cleared, because its blocker was only a
missing material asset in the validation project. **No Windows or Linux validation pass exists for
any task in any phase**: Phase 0's gate, Phase 1's render rows, all thirteen Phase 2 tasks, and every
Phase 3 task.

**E.2.1's PAGE IS macOS-VALIDATED — 10 PASS / 3 open, 2026-09-06.** Measured with two instruments: a
headless harness through the real `SceneRenderer` -> `SkyPass` -> `PostProcess` chain (exact bytes, no
ICC round trip) and **the editor driven for real**. Every sky oracle difference is **0**; the unlit
cube reads `(85,117,162)` top against `(73,91,124)` side — dim, not black; ortho is exactly **one**
colour; the grid clears the ground by **52.1** luminance levels; each Inspector edit is **exactly one
named undo entry** and two undos restore **0 differing pixels of 1 795 500**; the multi-Environment
WARN **latches** and the lowest-index rule is confirmed in both directions (loser 0 px, winner
302 820); **Solid + Flat + intensity 1 is bit-identical to the branch-point build, 0 of 1 795 500,
with a 1 792 032-px control**; the feature costs **less than the ~0.7 CPU-s noise floor**; and a
pre-task sample renders under the default sky with zero WARN. **D1's promise is measured too**: a
world with no `Environment` is bit-identical to one with a default component, 0 of 921 600.
**THREE ROWS REMAIN OPEN, each for a stated reason.** Row 4's normal-mapped arm — the only cover
sabotage seed 8 has anywhere — is **GATED ON E.5.1**, because a material dropped on a *primitive* is
silently discarded by `buildRenderView`, so the default Cube cannot carry one; **that dependency was
not recorded anywhere before this pass**. Row 7's `Save Scene` produced **no write and no log line**
under synthetic input while New Scene, Reset Layout and Undo all worked from the same input path —
**possibly a real defect and explicitly unresolved**. Row 10 has no material asset in the test
project; row 12's release editor never connected to `tracy-capture`. **HiDPI is deliberately NOT a
row**, so E.1.1's thick-line handoff stays **UNFIRED for a sixth task**. **The sabotage matrix was run
in full — all 31 rows** — and found two holes, both closed before the merge.

**Outstanding macOS passes: 3.5.1's twelve rows, 3.5.2's twelve rows, and 3.7.2's twelve rows.** Each is
the only cover its task's declared seeds have anywhere. **3.4.2's `S26` remains uncovered by any pass
and cannot be covered from macOS** — SDL queues a texture-container free on Metal and performs it
immediately on Vulkan and D3D12, so it is observable only on a Windows or Linux pass.

**3.7.2 widens the debt by a full task's worth, and the shape is worth stating rather than a formula:**
CI genuinely compiles and runs every `SP`/`MX`/`SY`/`SA`/`DV` case on all three lanes, so the
spatializer, the mixer, the system and the bridge **are** cross-lane covered — and **no lane produces a
sound**, because CI opens the null backend only. **LSan runs on the Linux Debug lane alone**, which
makes that page's Linux row matter more than most.

**E.1.4 IS macOS-VALIDATED — 10 PASS / 1 NOT EXECUTABLE, 2026-09-05.** The deliverable holds on a
REAL asset: a glTF whose cooked AABB is a 2 x 2 box outlines its narrow diagonal band, with **0 amber
pixels of 3600 in each off-diagonal corner** and 428/400 in the two the geometry occupies. The band
measures **exactly 2 px** and **`rgb(255,176,64)` byte-exact** on screen — which is `2*radius`, not
`2r+1`, and is display-space colour, so D7's after-the-tonemap composite is confirmed from the
product side. **The marker and the outline are the same amber byte for byte**, which is the ONLY
defence the two declared colour seeds have anywhere. A fully occluded selection draws **0 outline
pixels**; a partly occluded one stops dead at the occluder. The picture with nothing selected is
**bit-identical (0 of 296 378)** to a build with this task absent, with a working anti-vacuity control
at 1580. Across an actual **768 -> 832** allocation change the band stays 2 px and flush against the
silhouette. **The feature is free at 60 Hz**: four conditions all mean 16.665-16.672 ms, vsync-bound.
**Row 4 is NOT EXECUTABLE — both attached displays are 1x — so E.1.1's thick-line handoff stays
UNFIRED rather than cleared.** Two rows carry a stated shortfall: row 5's mid-import transition was
never observable (the import completes inside one frame) and row 6's 256-entity figure is
**extrapolated from eleven, not measured**, because the editor has no Select All and no Hierarchy
range-select. **A METHOD TRAP WORTH KEEPING: two live `aero_editor` processes made a window lookup
capture the STALE one, and three comparisons reported a false '0 differing' — including one that
provably contained an outline. Bind every capture to its launched PID, and let an anti-vacuity
control be what catches it.** The rows that were its only seed cover: **row 1** (a REAL asset
with a fat cooked AABB — the deliverable's headline claim, judged against something other than a
`Sphere`), **row 3** (the palette and the primary/secondary read at a glance — sabotage row 20 is a
deliberate NON-finding, because restating the two colours as `IM_COL32` literals one byte off is
invisible to every automated tier), **row 4** (HiDPI legibility, and the FIRST time E.1.1's
thick-line handoff can be answered at all, because an edge-detect band is the first overlay in the
tree that can control its own apparent thickness), **row 6** (the depth store-out's cost) and **row
9** (whether the band slides on a quantum-boundary resize, which `OG7` bounds to +/-1 but cannot
judge). Everything else this task claims is CI-covered on all three lanes, **including pixels**:
`OG1`–`OG17` read back band width to the exact integer, both colours to the exact byte, occlusion,
the box-corners-bare proof, D10's margined target, the alpha rule, cull mirroring, the skinned arm
and per-frame clearing.

**3.7.3 adds NO validation page and that is the deliberate 2.1.2 precedent** — 2.1.2 and 2.5.2, the
two page-less tasks, are both pure test/guard infrastructure, and nothing in a guard task needs ears,
eyes, hardware or an OS-specific behaviour a row could measure. Its standing evidence is the hermetic
e2e pair, which runs on the macOS and Linux lanes on every push, plus the one-time seed and meta-proof
log in `docs/10`. **Its own residual is Windows**: both e2e cases are `NOT WIN32`, and the lint job
that runs the two scripts is ubuntu-only, so nothing about the guards' behaviour is exercised under an
MSYS userland at all. Coverage of the *invariant* is unaffected — the guards run on every push.

**E.1.1's page IS macOS-validated — 8 of 10 rows PASS on 2026-09-03, 2 partial for structural
reasons.** (This paragraph said "NOT RUN on any platform" until E.1.2's pass; the phase table above
had been updated at E.1.1's pass and this had not, so the block contradicted itself.)
`editor/validation/E.1.1-debug-line-renderer.md` is gitignored, so it enters no commit, and carries
eleven numbered steps, seven of them with measurement blanks. Its shape is unlike every prior
render task's: **rows 1–3 are already automated on all three lanes** by `DG5`–`DG10` and `DG16`,
because the tree now reads pixels back. What stays hardware-only is legibility on a HiDPI display
(a 1-pixel line has no width control on any backend), the editor's picture being unchanged against a
build at the branch point, the cost A/B, Tracy's zone and plots, and the declared seeds — **`S11`
(depth write on the Tested pipelines) and `S21`'s picture half (a mirrored atlas cell whose centre
still reads the right colour) have their ONLY coverage anywhere in that page's row 6.**

**E.1.3 IS macOS-VALIDATED — 11 PASS / 1 PARTIAL / 2 NOT EXECUTABLE / 1 NOT RUN, 2026-09-05, AND THE
PASS FOUND A DEFECT THAT IS NOW FIXED (PR #95, merge commit `0ab204d`).** The measurements worth
carrying: the pivot sits at the **IDENTICAL pixel** across a projection toggle (dx = dy = 0.00, same
34-px footprint) while **55 113 of 204 800** viewport pixels change — D11's pivot-*plane* continuity
with its own anti-vacuity control; the widget's hide threshold is **exactly 140** (width 139 -> 9 px,
140 -> 128 px, so the predicate is `>=`); a 10-unit pillar snapped to Top shows **0 px** of lateral
streak at ~96 px/world-unit, where the old `MAX_PITCH` would have smeared it ~9.6 px; all four
corners pick in ortho with a 90-px-off probe selecting nothing; and a drag released **inside** the
widget commits **exactly one** undoable edit without snapping the view.

**THE DEFECT, AND WHY NO TIER COULD SEE IT.** `gizmoOriginBehindCamera` kept ImGuizmo's
perspective-only near-band mirror **unconditional**, reasoning it was "a narrower band on the same
normalised axis and is safe". `0.001` was calibrated against ImGuizmo's **view-space**
`camSpacePosition.z`; ortho's `clip.z` is `(-z_view - zNear)/(zFar - zNear)`, a **normalised** depth,
so the same constant spans `0.001 * (zFar - zNear)` — **~1.0 world unit** at the shipped defaults and
~10 at `zFar = 10000`, against the ~0.0002-0.11 the code itself measured for perspective. `focusOn`
frames a small entity at exactly that distance, so **F then orthographic drew no transform gizmo at
all**. `G18` had a subcase **pinning the old behaviour**, so this was a decision whose consequence was
never evaluated, not an oversight — and `G18` is structurally blind to that second test anyway, which
is the same blind spot the code-review round already recorded for seed `S6`. **The lesson that
outlives it: a threshold calibrated in one projection's depth units means something else entirely in
the other, and "stricter" is only safe against the failure it was written for.** The perspective band
sits on the OPPOSITE side of the near plane from ortho's — `clip.z < 0.001` under `perspectiveRH_ZO`
solves to a view depth under ~0.101, i.e. between the eye and the near plane, which test 1 accepts
there because it gates on `clip.w` and not on the near plane at all.

**Its page still has three open records** : row 6's translate/scale-in-ortho arm (blocked by the defect above,
re-runnable now), row 7 (NOT EXECUTABLE — this scene's only floor is E.1.2's debug LINE grid, which
receives no shadow; a solid plane is **E.5.2's**) and row 13 (the cost A/B, which needs a second build
at the branch point). Five of its sixteen rows are the ONLY cover their seed has anywhere: **row 6** (the
rotate ring in ortho — seed `S8`, which no runtime tier can see because nothing here reads ImGuizmo's
global state), **row 10** (the widget's press claim at its boundary — seed `S13`, because **nothing in
`tests/` can inject a camera gesture**, measured, and seed `S18`'s one-point edge), **row 1**
(HiDPI legibility, **NOT EXECUTABLE on 1x hardware**), and the two rows the review round added —
**row 15** (a press the widget owns must not reach ImGuizmo, and the other direction: a drag begun on
a handle and released over the widget must still commit once) and **row 16** (both snap cancels, and
that a wheel-dolly does *not* cancel). **Rows 15 and 16 exist because NOTHING in `tests/` can
synthesise a click or press a key** — the backend rewrites `io.MousePos` every `NewFrame`, and
ImGuizmo exports no getter for `mbEnable`, so `I118` and `I115` are source-text pins that say so in
their own comments. Everything else this task claims is CI-covered on all three lanes, which is the
direct consequence of the pure/ImGui split.

**E.1.2 IS macOS-VALIDATED — 8 PASS / 2 PARTIAL / 1 NOT EXECUTABLE, 2026-09-04.** The render with
the grid off is **bit-identical** to the branch point (0 differing pixels of 319 620, with an
anti-vacuity control showing 42 440 when the grid is on); the grid is **free** at 60 Hz (16.63 ms on
vs 16.68 ms off, both vsync-pinned, `dropped 0` over 464 frames); the axes agree with ImGuizmo to
within **4 degrees**; and unchecking Grid takes the viewport from 599 red / 398 blue pixels to
**exactly zero of each** while leaving a selected entity selected — `S26`'s only cover anywhere.

**THE THREE THAT ARE NOT A CLEAN PASS ARE ALL ENVIRONMENTAL, AND NONE IS A CODE DEFECT.** (1) **HiDPI
legibility is NOT EXECUTABLE here** — the only display is 1x, so **E.1.1's thick-line handoff remains
UNFIRED rather than cleared**, and that distinction matters. (2) **The crossfade's *pacing* is not
judged** — the measurable half is clean (no brightness step; largest single-frame delta 4.6% over
eight frames across a decade boundary) but "does it feel evenly paced" needs a slow continuous dolly,
so **`S12` stays only PARTIALLY covered**. (3) **Row 5's below-plane half is not executed** — the
editor starts a default scene rather than opening one from disk, which is E.4.1's job.

**AND ROW 6 PRODUCED THE NUMBER E.5.2 NEEDS.** See the depth-margin table in `docs/10`. **The sample
binary is `aero_sample_phaseE_debug_draw`** — `phaseE`, no underscore before the E.

### Next

**Phase E is the open front, and EPIC E.3 IS NOW OPEN: E.3.2 (selection-follows-focus router) is
merged — fourteen commits, the full local gate green on both presets and both reduced configurations,
the 32-seed sabotage matrix run in full with three real holes found and closed, a code-review round
that found six more (one of them a REAL focus-steal defect the whole green matrix was blind to), and
NO existing GPU case adjusted. Its validation page is UNRUN on all three platforms.** **E.3.1, E.3.3 and E.3.4 are
planning only, and E.3.1's `I142`–`I148` case-id block stays RESERVED — the gap in
`imgui_layer_test.cpp` is deliberate.** **`editor_prefs.json` NOW EXISTS BY NAME**, with its own
version, codec and writer (docs/09 §8.5), and an absent key is its default — so the standing "persist
the four viewport toggles and the tonemap params per user" handoff (3.6.3 / E.1.2 / E.2.3 / E.2.4) and
**E.6.1's `EditorTheme`** both have a **home** and still have **no implementation**; neither is
discharged. **Two new unowned handoffs**: a **file-open verb** in the Asset Browser (a double-click on
a file records byte-identical `SelectEntry` to a single click today, so whoever wants "double-click
opens" owns a new `ActionKind`, a meaning for "open" per asset kind, and its interaction with this
router), and the **camera-gesture guard** (excluded with a measurement — `updatePick` cannot arm during
a gesture — so any future task whose command writes the `Selection` while a camera gesture is live must
add the term, which needs a `ViewportPanel` accessor and one `ShellUiState` field). **E.3 still
inherits exactly TWO Inspector-row gaps** — E.2.1's enum-aware row and E.2.2's unit-aware row — and
**E.3.2 adds none and closes none**.

**EPIC E.1 AND EPIC E.2 ARE BOTH CLOSED IN CODE — all nine tasks merged.**
E.2.1 (PR #98, `28deab0`), E.2.2 (PR #99, `bf363e4`) and E.2.3 (PR #100, `00e4c7b`) are on `main`;
**E.2.4 closes the epic, nine commits**. E.2.1's page is **macOS-validated: 10 PASS / 3 open**;
**E.2.2's is 12/12**; **E.2.3's is 12/12, 2026-09-08, with no blockers and no partials — and it FIRED
E.1.1's thick-line handoff after eight tasks. **E.2.4 IS macOS-VALIDATED 12 of 12, 2026-09-11, with no blockers
and no partials**, which closes the last of Epic E.2's validation debt and all six of its declared
seeds. **Every task in Epics E.1 and E.2 is now merged AND macOS-validated except E.1.5, whose page
has not been run on any platform** -- that, plus E.3.2's own unrun page, is what is left of Phase E's
validation risk on this OS.

**The spine, as E.3.2 leaves it. Epics E.3 (three tasks left), E.4, E.5 and E.6 are the open front —
fourteen tasks, planning only.** **E.4.5 (thumbnails) is unblocked and now has `material_preview_rig.hpp` to call BY
NAME**: a thumbnail is `materialPreviewCamera(rig, fixedAngle, 1.0F)` plus `materialPreviewView(...)`
with whatever `MaterialPreviewLighting` it wants — the scene's, or a fixed studio lighting, a decision
E.4.5 makes explicitly. **E.3 still inherits exactly TWO Inspector-row gaps** — E.2.1's enum-aware row
(`Environment`'s two modes are bare 0/1 drag fields because reflect-gen cannot reflect an enum) and
E.2.2's unit-aware row (`SpotLight`'s cone angles are raw radians clamped to `[0, 1.5708]`) — **E.2.3,
E.2.4 and E.3.2 add none.** **E.3.1** still inherits the palette key; E.2.3's four gizmo tints join
`axis_palette.hpp` as **E.6.1** `EditorTheme` candidates and **E.2.4 adds no colour at all**. **E.5.1
is an S-sized fix for a confirmed defect and is independent of everything** — E.1.4, E.2.1, E.2.2,
E.2.3 and now E.2.4 have all reproduced it rather than fixing it in passing, and E.2.4's `PX` battery
gains a non-default-material arm the day it lands. **E.5.2 owns the coplanar-geometry problem, has a
GROUND COLOUR to sit its plane against, and owns the Create menu's Light entries** (Directional /
Point / Spot), which now land something **visible** the moment they are created — the default scene is
deliberately unchanged by E.2.3 and E.2.4 too, and its seventeen `entityCount() == 4` pins are
byte-identical. **E.6.2 moves `T R S` and `Local/World` into the main toolbar and leaves `View` on the
viewport** — it is per-view, not per-shell. **E.6.3 splits E.2.4's popover into the mock's header
dropdowns**; the grouping is already the mock's, so it restyles rather than regroups. **8.2 inherits
IBL/HDRI and the after-opaque sky variant**, with `SB9`/`SB16` in place to catch a wrong ordering,
plus physical light units, IES profiles and area lights. **E.6.1 owns the DPI story** E.1.5 deferred.

**THREE HANDOFFS DISCHARGED BY E.2.4.** E.1.2's and E.2.3's *"E.2.4 moves this whole row into a
popover"* — **taken**: the strip is `T R S | Local/World | View` and all four controls live in the
popup. **E.1.3's view-axis visibility toggle** — **taken**, and hiding the widget hides its press claim
in one fact rather than four. **E.1.1's thick-line handoff stays FIRED and unmoved**: E.2.4 draws no
line and no icon, and HiDPI is deliberately not one of its validation rows.

**SIX UNOWNED HANDOFFS, THREE OF THEM NEW.** **Spot and point shadows**: the shadow pass is
directional-only (3.6.2), Phase E's non-goals name cascaded/soft shadows as 8.2.1's, and no roadmap
item owns omni or spot shadow maps at all — recorded by E.2.2 and unchanged. **A camera FRUSTUM
gizmo**: E.2.3 draws the camera an icon and no gizmo; a natural fit for 4.7, the first task where a
scene camera's framing matters. **A MIP CHAIN for E.2.3's icon atlas**: a 64-texel cell drawn at
22 points is a 2.91x minification, mitigated by a 4-texel minimum feature size rather than eliminated,
and `Device::uploadTexture` already takes a mip level, so adding one later is a downsampler and three
lines. **Muting the point/spot lights the bridge TRUNCATES stays unowned** — it becomes safe the
day `RenderView` can report which lights survived. And three from E.2.4: **a preview that carries the
scene's POINT and SPOT lights** (deliberately excluded — a lamp lights a sphere at the origin by where
it happens to sit, which predicts nothing about the material); **a FOV control** (`EditorCamera::setFovYRadians`
exists, the mock implies one, nothing owns it); and **PERSISTING the four viewport toggles and the
tonemap params** per user, which is 3.6.3's, E.1.2's and E.2.3's handoff unchanged — E.2.4 moved the
controls, not the state, so **exposure as a scene or camera property is still unowned too**.

**Phase 3 remains OPEN behind it, on its gate and its validation debt, and Phase E does not close
either.** What remains for Phase 3 is its deliverable gate — a rigged glTF/FBX in, producing PBR
materials, shadows, a playing animation and **an audible sound** — and the validation debt below. The
audible half has never been heard on any platform. **Windows and Linux rows are outstanding for every
task in every phase**, and 3.7.2's Linux row matters more than most: LSan runs on that lane alone. See
`docs/tasks/phase-3.md` and `docs/tasks/phase-E.md`.

> **Before touching a subsystem, read its entry in `docs/10-engineering-log.md`.** That file is the full per-task history: what shipped, what was deliberately left out, the traps found, and the dead ends that must never be retried (the lavapipe LSan leak, `LD_PRELOAD`, vcpkg's `sdl3-shadercross` on macOS, …). It is deliberately *not* auto-loaded — grep it before re-deriving anything.

**Maintenance:** rewrite this section as the position moves. Per-task history is appended to `docs/10-engineering-log.md`, never here — that is what grew this file to 175k characters once already.

## Build, test & lint

A fresh clone needs the vcpkg submodule: `git clone --recurse-submodules`, or `git submodule update --init` after a plain clone.

```bash
cmake --preset macos-debug          # or macos-release; windows-*/linux-* are gated to their host
cmake --build --preset macos-debug
ctest --preset macos-debug          # prefix AERO_REQUIRE_GPU=1 to rehearse the CI ratchet
```

Six presets — `{macos,windows,linux}-{debug,release}` — each gated to its host OS by preset conditions, building into `build/<preset>/`.

- **`*-debug`** — ASan/UBSan via `AERO_ENABLE_SANITIZERS` → `cmake/sanitizers.cmake` (Windows: ASan only, MSVC has no UBSan).
- **`*-release`** — `AERO_ENABLE_PROFILING=ON` links the pinned Tracy 0.13.1 client into `aero::profiling` and defines `AERO_PROFILING_ENABLED`. Tracy is dev-builds-only — never Debug, never the runtime — enforced by default-OFF plus link gating, not convention. Use the `AERO_PROFILE_*` macros from `<aero/core/profiler.hpp>`, which no-op when profiling is off.
- **`AERO_REQUIRE_GPU`** — unset, GPU-gated tests skip loudly; set (as all three CI lanes do), a missing GPU is a hard failure.
- **`-DAERO_SHADER_TOOLS=OFF` / `-DAERO_REFLECT_TOOLS=OFF`** — escape hatches for constrained or offline machines. CI never sets them OFF; both must stay green.

**The first configure is slow and needs network.** It bootstraps vcpkg, builds SDL3 from source, and builds the SDL_shadercross toolchain from source into `~/.cache/aero-engine/shadercross` — once per machine, not per preset or worktree. Later configures are instant and fully offline. `reflect-gen` additionally needs a system LLVM 18 (`brew install llvm@18`, `apt install libclang-18-dev llvm-18-dev`, or `choco install llvm --version=18.1.8`); override discovery with `-DAERO_LLVM_ROOT=…`.

**Pinning invariant:** `builtin-baseline` in `vcpkg.json` and the `/vcpkg` submodule commit are the **same SHA**. Bump them together, never separately — a CI job asserts it.

**Lint locally before pushing.** clang-format alone does not catch what CI's clang-tidy rejects, and a local format pass has been proven not to predict CI:

```bash
clang-format-18 --dry-run --Werror <files>
SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path) \
  clang-tidy-18 -p build/macos-debug --warnings-as-errors='*' <files>
```

CI (GitHub Actions; macOS + Windows + Ubuntu) configures, builds and tests all six presets on every push to `main` and every PR, plus a `lint` job running clang-format, clang-tidy, the vcpkg-baseline guard, and the **eight** architecture guards in `.github/scripts/`: `check-math-boundary.sh`, `check-platform-boundary.sh`, `check-rhi-boundary.sh`, `check-scene-boundary.sh`, `check-golden-rule.sh`, `check-project-no-delete.sh`, `check-audio-boundary.sh`, `check-boundary-probes.sh`. The count is measured, not remembered — `ls .github/scripts/`.

Path-scoped working rules live in `.claude/rules/` and load only when the matching files are opened — boundary guards, reflect-gen, CI portability, editor conventions, and cooked assets.

## The three project rules (non-negotiable)

1. **Golden architecture rule** — the editor depends on the engine; the engine NEVER depends on the editor. Enforced by CI guards: no `#include` under `/engine` or `/runtime` may reference `/editor`, and the runtime binary must never link ImGui, Assimp, or libclang.
2. **Deliverable rule** — every phase ends in something playable or usable; a phase without a deliverable is not finished.
3. **Boundary rule** — no third-party type crosses the engine's public API. Not `glm::vec3`, not an SDL handle, not a miniaudio type. Everything lives behind the engine's own types (e.g. `engine::Vec3` wraps GLM inside `core/math`).

## Architecture (planned — full detail in docs/03)

Layers; each depends only on layers below it, and `core` depends on nothing:

- `/editor` (3 desktop platforms) — Dear ImGui, panels, gizmos, undo/redo, **importers**, exporter
- `/engine` (5 platforms) — subsystems in dependency order: `core` (handles, math, jobs, log, VFS, time) → `platform` (SDL3 wrapper) → `rhi` (SDL_GPU wrapper — the escape hatch for future ray tracing; treat as sacred) → `render`, `scene` (EnTT), `physics` (Jolt 3D / Box2D 2D), `audio` (own graph → miniaudio backend), `assets`, `script` (quickjs-ng), `reflect`
- `/runtime` (5 platforms) — game loop, `.pak` loading, per-platform entry points
- `/tools` — `reflect-gen` (libclang codegen), `shaderc` (HLSL → DXIL/MSL/SPIR-V via SDL_shadercross), `cooker`, `packager`

Load-bearing decisions (rationale in `docs/02-adrs.md` — settled ADRs are not re-litigated):

- **Handles, not pointers** for every resource: `Handle<Tag>` = `{index: u32, generation: u32}`. Never manual `new`/`delete`; RAII everywhere. ASan/UBSan run in CI on every commit.
- **Reflection is the spine (ADR-004).** `tools/reflect-gen` parses `[[engine::component]]` annotations with libclang and generates four consumers: `entt::meta` registration (inspector), JSON/binary serialization (scenes on disk), quickjs-ng bindings (script API), and `.d.ts` files (VSCode autocomplete). Write a component once; all four stay in sync. Built in Phase 1, before anything depends on it. Start with the minimal subset: plain structs + primitives + `Vec3`/`Quat`.
- **Asset flow:** source files (`.blend`/`.fbx`/`.obj`/…) → importer (editor-only: ufbx, tinyobjloader, Assimp, Blender invoked as external CLI) → canonical **glTF 2.0** + `.meta` file (stable GUID, committed to git) → cooker (per-platform binaries: KTX2/Basis textures, GPU buffers, script bytecode, compiled shaders) → packager (`game.pak` + precompiled runtime).
- **Two export models (ADR-008).** TypeScript projects: instant export — cooked assets packed next to a CI-precompiled runtime; user needs no toolchains (the Godot model). C++ projects: native compile + link per platform (the Unreal model). The language is fixed per project at creation and never mixed.
- **Dependency placement is an invariant:** ImGui, ImGuizmo, Assimp, ufbx, tinyobjloader, stb_image are editor/tools-only; libclang is tools-only; Tracy is dev-builds-only. An editor-only dependency linked into `/engine` or `/runtime` is an architecture bug, not an optimization issue.

## Conventions (docs/04)

- **C++20** baseline; C++23 features only where Clang, MSVC, and GCC all support them.
- Naming: `PascalCase` types, `camelCase` functions/variables, `SCREAMING_SNAKE_CASE` compile-time constants, `snake_case` files/directories. Everything under `engine::` (subsystem sub-namespaces like `engine::rhi` as needed).
- Headers: `#pragma once`; public headers expose only engine types; no `using namespace` in headers.
- Errors: no exceptions across public API boundaries — explicit result/status types, asserts in debug; handles return invalid rather than throw.
- Git: trunk-based; short-lived feature branches merged to `main` via PR even solo; `main` is always green. Conventional-commit style (`feat:`, `fix:`, `refactor:`, `docs:`, `build:`, `ci:`, `test:`), imperative mood. Phases and releases are tagged. `.meta` files are committed; cooked/build output is gitignored. **Do not add a `Co-Authored-By` trailer to commits.** **Merge with a MERGE COMMIT (`gh pr merge <n> --merge`), never a squash** — the plans deliberately split a task into one green commit per step, and a squash discards every one of them in favour of a single new GitHub-authored commit, which both loses the bisectable per-step history and drops the contributions (GitHub counts only commits that land on the default branch). PRs #22–#26 used merge commits, #38–#45 were squashed (the regression), and #46 onward restores merge commits.
- CI (GitHub Actions, macOS + Windows + Ubuntu, from commit #1): Debug build with ASan/UBSan + Release build, codegen steps, doctest unit tests, the **eight** architecture guards that exist today — math-boundary (no `<glm/...>` outside `engine/core/src/math/glm_backend.cpp`, the single allowlisted file; **not** the looser "outside `core/math`", which would license GLM in the public math headers), platform-boundary, rhi-boundary, scene-boundary, golden-rule, project-no-delete, and since 3.7.3 audio-boundary (the no-vcpkg property of `engine/assets`/`engine/audio`/`engine/scene_audio`'s CMakeLists + a miniaudio token ban over both audio roots, sources included) and boundary-probes (every `aero_*_boundary_probe` links exactly one `aero::` library, `PRIVATE`, with the probe set derived from `tests/CMakeLists.txt`); each created by its owning task, see `docs/04` — and format/lint checks. *(Runtime-purity is planned for its owning phase (5.2.2) and does **not** exist yet; do not cite it as live.)*

## Scope discipline

`docs/06-scope-and-non-goals.md` is load-bearing. Before anything is added to v1.0 it must pass all three: (1) serves the edit → script → play → export loop; (2) needed by a real shippable game in Phase 5; (3) maintainable solo without derailing the 20–32-month horizon. Explicit v1 non-goals include ray tracing/mesh shaders, Nanite-style geometry, baked GI, terrain, visual scripting, networking, web/WASM export, a mobile editor, and FMOD/Wwise in core. Deferred items live in `docs/future-roadmap.md`.

Some decisions are deliberately deferred (`docs/08-risks.md`): forward+ vs deferred rendering (Phase 8, with Tracy data), ImGui's long-term role, migration to C++26 `std::meta`, GLM → RTM swap. Do not resolve them early.

## Documentation map

- `docs/` is the source of truth for scope and architecture. Execution tracking lives in Notion ("Aero Engine — Build Tracker", linked in README): three linked databases (Phases → Epics → Tasks); phases/epics/tasks are rows, **subtasks are to-do checklists inside their task's page**. On any conflict, the docs win and Notion gets corrected.
- **When a validation status changes, update this file's state block as well as the task's validation page** — the state block is the authoritative summary of where every task's gate stands.

| Doc | Contents |
|---|---|
| `docs/00-overview.md` | Objective, rules, platform matrices, stack table, horizon |
| `docs/01-tech-stack.md` | Choice per layer, licenses (MIT-compatibility is a hard requirement), accepted stack limits |
| `docs/02-adrs.md` | ADR-001…008 with discarded alternatives |
| `docs/03-architecture.md` | Layers, repo layout, handles, asset flow, export models, reflection consumers |
| `docs/04-conventions-setup.md` | C++ style, git, CMake/vcpkg, CI guards, testing strategy |
| `docs/05-roadmap.md` | Phases 0–8 with deliverable gates |
| `docs/06-scope-and-non-goals.md` | What v1.0 is and is not |
| `docs/07-tasks.md` | Task index: legend, numbering conventions, per-phase links |
| `docs/tasks/phase-{0..8}.md`, `docs/tasks/phase-E.md` | Full breakdown per phase: epics → tasks → subtasks, each task with goal + deliverable. **Phase E** (Editor Experience) executes between 3 and 4 and is lettered because `3.5` is already taken — see `docs/07`'s numbering conventions |
| `docs/08-risks.md` | Risk register, open + resolved decisions |
| `docs/09-file-formats.md` | Scene schema v1 (entity/components/version), canonical form, versioning & evolution policy |
| `docs/10-engineering-log.md` | **Per-task build history** — what each task shipped and deliberately did not, traps found, dead ends never to retry, and per-task build/dependency impact. Not normative; not auto-loaded. Read the relevant entry before touching a subsystem. |
| `docs/future-roadmap.md` | v2 / v3–v4 deferred features |
| `.claude/rules/*.md` | Path-scoped working rules, loaded only when matching files are opened (boundary guards, reflect-gen, CI portability, editor, cooked assets) |
