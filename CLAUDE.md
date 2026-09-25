# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Aero Engine — an open-source (MIT), cross-platform 3D game engine with an editor and per-project TypeScript **or** C++ scripting. Solo project, started July 2026. The goal is core-workflow parity with Unity/Godot (edit → script → play → export), explicitly **not** feature parity. 3D-first; 2D arrives in Phase 7.

Two platform matrices, never to be conflated: the **editor** runs on macOS/Windows/Linux only; the **runtime** (exported games) targets those three plus iOS and Android. The editor never runs on mobile — no touch UI, no adaptive layouts.

## Current state — read this first

**Phase E (Editor Experience) is the open front**, executing between Phase 3 and Phase 4. **Sixteen of its
24 tasks are merged: Epics E.1, E.2 and E.3 are all CLOSED IN CODE, E.4.1 and E.4.2 are both MERGED AND
macOS-VALIDATED, and E.4.3 is MERGED AND macOS-VALIDATED — so Epic E.4 stands at three of five, with all three
of its Definition-of-Done clauses discharged in code. E.4.4, E.4.5, E.5 and E.6 are what is left — eight
tasks, planning only.** Phase 3 remains OPEN behind it: all seven of its epics are closed in code,
and what is left is its deliverable gate and the validation debt.

**Phase E is lettered, not fractioned.** `3.5` and `3.5.1`/`3.5.2` are already Phase 3's skeletal-animation
epic and its tasks, and numbering is append-only, so a "Phase 3.5" would collide with referenced numbers.
In Notion its `Phase #` is `3.5` — a sort key, not an identifier.

**THE BUILT-IN COMPONENT COUNT IS TEN**: `Transform`, `Camera`, `DirectionalLight`, `PointLight`,
`MeshRenderer`, `AnimationPlayer`, `AudioSource`, `AudioListener`, `Environment` (E.2.1), `SpotLight`
(E.2.2). Nothing since E.2.2 has added one, so the five-generation-site rule and the component-count sweep
have not fired since — **they still apply in full to the next built-in, whenever one arrives.**

**Next free ids: `I227` at the ImGui tier**, `AA64`, `DR28`. The `MR` prefix is taken (E.3.4's
`MR1`–`MR21`), so is `PJ` (E.4.1's `PJ1`–`PJ57`), and so is `CN` (E.4.2's `CN1`–`CN25`). E.4.1 took
`I176`–`I185`, E.4.2 took `I186`–`I192` and E.4.3 took `I210`–`I226`; the other E.4.2 ceilings are `SS`
54 and `IO` 21. **`I193`–`I209` are FREE and were never claimed by anything that merged** — E.4.3 kept
its plan's `I210` base rather than renumbering a contiguous block into them, because a gap costs nothing
and a renumbering invites exactly the silent collision this project has recorded. **The E.4.4 and E.4.5
specs both claim `I176`+ or `I199`+ — both must RE-MEASURE the ceiling and renumber before they are
implemented.**

**Four facts Phase E was built on, each measured in the tree and each contradicting a plausible guess.**
(1) The directional light **already** derives its direction from the entity's −Z world axis
(`scene_renderer.cpp:191-208`) — "it behaves like a point light" was an *affordance* gap, not a math bug,
because nothing drew the direction and nothing said which of two directional lights the bridge picked.
(2) `buildRenderView`'s **primitive arm never assigns `instance.material`** (`scene_renderer.cpp:99-107`),
so a material dropped on a primitive is written to the component through the undoable command and then
silently discarded at draw time — **a confirmed defect, still open, owned by E.5.1**; E.1.4, E.2.1, E.2.2,
E.2.3 and E.2.4 have each reproduced it rather than fixing it in passing. (3) `rhi::PrimitiveType::LineList`
and `FillMode::Line` existed since 0.4.1 with **no consumer at all** until E.1.1, still the tree's only
`LineList` pipeline set — **`FillMode::Line` remains unexercised and wireframe-of-meshes is an unowned
handoff.** (4) **CLOSED BY E.4.2, and replaced rather than deleted because the residue is deliberate.**
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

> **Per-task history — what each task shipped, what it deliberately left out, every trap and every dead end —
> lives in `docs/10-engineering-log.md`**, which carries a `### <task>` entry for every task through E.4.2,
> each with its own `#### The sentences that govern new work` subsection. **Grep it before re-deriving
> anything.**
>
> **This section is a summary of WHERE THE POSITION IS and of the RULES THAT STILL GOVERN NEW WORK. It is
> REWRITTEN as the position moves, never grown, and per-task narrative does not belong in it.** It reached
> 207 k characters once and 121 k a second time; both times the cause was exactly that.

### Merged-task index

Full detail for every row is in `docs/10`. Validation verdicts are macOS-only — **no Windows or Linux
validation pass exists for any task in any phase.** N-E = not executable, N-R = not run.

| Task | PR | Merge | macOS validation |
|---|---|---|---|
| 3.7.1 Audio clip assets | #88 | `4892e65` | 11 / 11 |
| 3.7.2 Playback API + components | #89 | `b398d17` | 47 of 53 records — **the 6 open ones each need ears or the editor** |
| 3.7.3 Audio-boundary CI guard | #91 | `0530cff` | **no page, deliberately** — the 2.1.2/2.5.2 precedent for pure guard infrastructure |
| E.1.1 Debug line renderer | #92 | `15bf58b` | 8 PASS / 2 partial |
| E.1.2 Grid floor + world axes | #93 | `d91eab1` | 8 PASS / 2 partial / 1 N-E |
| E.1.3 View-axis gizmo | #94, fix #95 | `6fb323c`, `0ab204d` | 11 PASS / 1 partial / 2 N-E / 1 N-R — **the pass found the ortho gizmo-suppression defect** |
| E.1.4 Silhouette selection outline | #96 | `3aadffb` | 10 PASS / 1 N-E |
| E.1.5 Transform-gizmo restyle | #97 | `cdc81ce` | **UNRUN on every platform** |
| E.2.1 `Environment` + sky pass | #98 | `28deab0` | 10 PASS / 3 open |
| E.2.2 Point falloff + `SpotLight` | #99 | `bf363e4` | 12 / 12 |
| E.2.3 Light gizmos + viewport icons | #100 | `00e4c7b` | 12 / 12 |
| E.2.4 Material-preview parity | #101 | `ae817dc` | 12 / 12 |
| E.3.1 Axis-labelled vector fields | #102 | `a8d963d` | 13 PASS / 1 partial |
| E.3.2 Selection-follows-focus router | #103 | `b172198` | **UNRUN on every platform** |
| E.3.3 Asset-reference picker | #104 | `fc77c4b` | 10 PASS / 3 partial / 1 N-E / 1 N-R, nothing failed |
| E.3.4 Material inspector redesign | #105 | `170ad9b` | 12 PASS / 1 partial / 1 N-E, nothing failed |
| E.4.1 Reopen the last scene | #106 | `068c45c` | **12 / 12**, nothing failed — and 26 sabotage seeds / 29 runs with **no coverage hole** |
| E.4.2 Scene/project containment | #107 | `f88079d` | **14 of 16 rows** — 12 outright, 2 as stated variants, 1 partial, **2 NOT EXECUTABLE**. 29 sabotage seeds found **three real coverage holes**; the code-review round found eight findings, one blocking; Windows CI found a ninth after all three passed |
| E.4.3 Asset file operations | #108 | `3dff5ef` | **55 of 57 records PASS, 2 FAIL.** All twelve rows run; all six seed-critical rows (3, 4, 5, 6, 8, 9) pass, so S9, S11, S14, S18, S21, S22 and S28 all have witnesses. **The 2 failures are ONE defect: Enter activates neither modal's default button** (below) |

**E.3.2 landed before E.3.1** — legal, disjointly id-reserved; the reservation is discharged and the
numbering is contiguous.

### The phase table

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE.** Gate reached, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics closed and macOS-validated; Windows/Linux rows pending for every task (`editor/VALIDATION.md`). Gate artifact: `samples/phase-2-editor-scene/` — data, deliberately not `add_subdirectory`'d. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** All seven epics (3.1–3.7) **CLOSED in code**. What is left is the gate below and the validation debt. |
| **Phase 3 gate** | Drop a rigged glTF/FBX in → PBR materials + shadows + a playing animation + **an audible sound**. The audible half exists in code as of 3.7.2 and **has never been heard on any platform.** |
| **Phase E** — Editor Experience | **OPEN.** Epics E.1, E.2 and E.3 **CLOSED in code**; **E.4.1, E.4.2 and E.4.3 merged** — 16 of 24, see the index above. **E.4.4, E.4.5, E.5 and E.6 are the open front: eight tasks, planning only.** TWO validation pages are unrun (E.1.5, E.3.2) and are the whole of this OS's remaining Phase E risk. |
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
* **`/editor`** has gained ten public pairs across Epics E.2 and E.3: `material_inspector_model` (E.3.4 —
  PURE: no ImGui, no GPU, no `<filesystem>`, no logging, so tier 0 can walk the whole panel's shape),
  `thumbnail_service`, `asset_tile`, `asset_picker_model`, `asset_picker` (E.3.3), `context_router`,
  `editor_prefs` (E.3.2), `material_preview_rig` (E.2.4), `viewport_icons`, `viewport_gizmos` (E.2.3) —
  plus `project_state` (E.4.1) and `scene_containment` (E.4.2) in Epic E.4, both PURE and both tier-0
  reachable in all three build configurations.
  **DO NOT record a "pair count" here — it is not reproducible**: E.2.4 measured the tree six ways looking
  for the figure this line used to carry and none of the six was it. The two figures anyone can re-run are
  `git ls-files`: **`editor/src/*.cpp` = 89** and **`editor/include/aero/editor/*.hpp` = 64** at E.4.2.
  `/tools` links `aero::assets` and `aero::editor_core` through `aero_cooker`, which is legal because
  `tools/` is enumerated by neither half of the golden rule.

### Standing invariants that govern new work

Every rule below was earned by a defect, a green-and-wrong test, or a measurement that contradicted the plan.
They are grouped only for navigation — all of them bind. **Each is stated as the rule plus the citation that
proves it; the narrative of how it was found is in `docs/10`.**

#### Boundaries, guards and the build

**THE `find_package` BOUNDARY.** `aero_assets`, `aero_audio` and `aero_scene_audio` link **no vcpkg package
at all**, which makes their `PRIVATE` links a **real compile-time boundary** rather than convention-plus-grep:
vcpkg installs every port into one shared per-triplet `include/` root that lands on the compile line of any
target linking any vcpkg package. So a stray `#include <miniaudio.h>` in `engine/audio` is a **hard compile
error — IN THE PROFILING-OFF CONFIGURATIONS ONLY**: `aero::profiling` is `PRIVATE` on all three and carries
`Tracy::TracyClient` when `AERO_ENABLE_PROFILING=ON`, and a target's own `PRIVATE` usage requirements apply to
its own compile line, so `mixer.cpp` carries `vcpkg_installed` in `macos-release` and not in `macos-debug`.
**Never write "hard compile error" about a vcpkg-free target without saying in which configurations.** Adding
a `find_package` to any of the three voids the boundary silently while CI stays green — guard-enforced since
3.7.3 by `check-audio-boundary.sh` prong A, `tests/audio_boundary_probe.cpp` (the compile-time half that
survives Release) and `audio-boundary.guard_e2e` (the proof it goes red).

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

**THE SEGMENT-WISE PREFIX RULE NOW LIVES IN FIVE PLACES (E.4.3), AND A SIXTH NEEDS A SIXTH CASE.**
`assetOpPathLadder`'s rung 6 (`isInsideOrEqual`), `countRecordsUnder`, `assetOpBlockedByDirtyMaterial`,
`listingHolds`'s case-only carve-out, and `AA45(c)`'s claim — each with its own `("tex", "textures/a")`-shaped
case, because a raw `starts_with` is right on every input except the one that matters. Seed `S8` came back
green against the third of them until `I226` was written for it.

**`beginAssetDragSource`'s `isDirectory` IS NON-DEFAULTED AND THE SOURCE CANNOT DERIVE IT (E.4.3).** It has
only a path, and `classifyAssetKind`'s own `isDirectory` argument was hardcoded `false` there because until
E.4.3 a folder and an extension-less file took the same early return. They now take DIFFERENT arms — a folder
has no sidecar, so its plan has one step and not two — so a default would let a future call site silently
encode a folder as a file, with no error and no failing test. Three call sites; `DR27` pins the one surviving
`/*isDirectory=*/false` to the search-hit site, where it is correct because `searchAssets` never matches a
folder.

**A ONE-SHOT TAKER THAT MOVES OUT OF AN `optional` MUST `.reset()` AFTERWARDS (E.4.3).** A moved-from optional
is still ENGAGED, so the omission leaves the one-shot set and re-runs a moved-from (empty) request on EVERY
tick — one refused operation and one rescan per frame, for ever. **No observable in this tree can see it**;
`I218`'s third arm is a source-text pin over the header that owns the four optional takers. Seed `S13` came
back green until that arm existed.

**A FOURTH vcpkg-free target must add itself to `VCPKG_FREE_CMAKE` in the commit that creates it, and to
nothing else.** Target list and skip test are both DERIVED from that one roster — three parallel lists once
left a target guarded by three prongs and invisible to the fourth. **An unlisted target is silently unguarded.**

**A COMMAND DENYLIST OVER CMAKE CANNOT CONVERGE — INVERT TO AN ALLOWLIST (3.7.3).** Three review rounds, and
each blocking finding sat inside the fix written to teach the previous one (the `aero::` alias vs the raw
name; `EXCLUDE_FROM_ALL` on `add_library` vs `set_target_properties`; property spellings vs plain commands).
**If a guard ever needs a second arm for a second spelling of one predicate, stop and invert it** — ask what a
protected thing may legitimately be named by, measure that the set is small, refuse everything else.

**AND WHERE INVERSION IS IMPOSSIBLE, READ THE BUILD FACT INSTEAD OF PREDICTING IT.** Reaching a target
*without naming it* — via a toolchain file or a preset's cache variables, which appear in no CMakeLists at
all — is bounded by no list. `boundary-probes.probe_compile_line` reads `compile_commands.json` and asserts
the property directly. Its stated limits: a multi-config generator writes no database (self-skips, exit 77),
and a contaminating include root that is not vcpkg's is invisible to it.

**A GUARD'S OWN `.cmake` E2E DRIVER IS INSIDE THE SET IT SWEEPS**, and it bit twice — fixture strings spelling
`target_link_libraries(aero_audio …)` made the guard exit 1 on itself. Compose the command name from a
variable so the scratch file stays byte-identical while no matching literal remains. **An exclusion list is
the wrong fix** — a permanent silent hole in a universal sweep, in the file most likely to acquire a real
CMake snippet later. Expect this whenever a guard's scan set grows to include `*.cmake`.

**FOUR GREPS ARE NOT LITERALLY ZERO AND MUST BE READ RATHER THAN COUNTED:** the `tools/` process-spawn grep
("libsdl-org fork" in `tools/shaderc/README.md`); INV-A1's float grep over the four audio cook/container
files; 3.6.3's `applyOetf|gammaEnabled|linearOutput|skipEncode`; and `#if` over 3.7.2's four new test files,
where every match is the prohibition *sentence* — usable form
`git grep -nE '^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif|else|endif)'`. **It was FIVE until 3.7.3**,
which moved the `find_package` reading into `check-audio-boundary.sh`'s self-test 2, with those prohibition
comments as anti-vacuity canaries (deleting one is a loud exit 2).

**A GREP THAT LOOKS FINE CAN BE SILENTLY VACUOUS.** `git grep -- $F` with a pathspec list in a shell variable
**does not word-split under zsh**: one bogus pathspec, no matches, exit 1 — and a `&& … || echo "none (ok)"`
idiom **reports a clean result on a dirty tree**. Use `${=F}` or literal paths. Same species as the POSIX `\b`
degradation on BSD, and 3.7.2's `echo "exit=$(basename $X) $?"` reading the *command substitution's* status.
**Check every guard grep in BOTH directions: a guard command that cannot fail is worse than no guard command.**

**AND A SOURCE-TEXT TOKEN SCAN IS BLIND TO INTEGER-LITERAL SUFFIXES (E.3.1).** The pin guarding "the panel
restates no axis colour" missed `226U` — digits followed by an identifier character — which is exactly how
`axis_palette.hpp` spells its own bytes.

**clang-tidy NEEDS THE PINNED SDK.** With `SDKROOT=$(xcrun --sdk macosx --show-sdk-path)` it exits 1 with tens
of thousands of errors inside system libc++ headers (`__builtin_clzg` and friends) — **a red verdict on a
clean tree**, because the generic SDK resolves to a newer libc++ than LLVM 18 can parse. Use `macosx15.4`.
**Read the EXIT CODE, never a tail**; `SDKROOT=… git diff | xargs clang-tidy` binds `SDKROOT` to `git diff`.

**`git checkout -- <file>` REVERTS TO HEAD**, so seeding a file to prove a fix **also reverts the fix if it is
not committed**. It ate two closures during 3.7.2. **Commit the fix, then seed it.**

**`git grep -nE '_WIN32|__APPLE__|__linux__'`** over `engine/assets engine/audio engine/scene_audio
engine/render engine/scene tools/cooker` reads **zero lines**; over `editor/src` + `editor/include`, **exactly
three lines in one file** (3.2.4's `currentHostOs()`).

**THE DETERMINISM MANIFEST IS FROZEN** at **20 hash lines across FIVE arms / 40 cross-lane comparisons**. A red
manifest case is `docs/09` §9.11's `cookerVersion` sentence firing; the five constants to bump are
`COOKED_{MESH,TEXTURE,SKELETON,ANIMATION,AUDIO}_COOKER_VERSION`, and the regeneration ritual lives in the
manifest's own header. **Never edit a hash to green a red run.** **mp3 and ogg are deliberately NOT in it and
never may be** (`docs/09` §14.7): their decoders run floating-point transforms whose paths differ by SIMD
availability and FMA contraction policy. `cooker.audio_lossy_digests` prints both digests on every lane and
asserts **no digest value**.

#### Components, reflection and serialization

**`AERO_BUILTIN_COMPONENT_HEADERS` NAMES THE BUILT-IN COMPONENT HEADERS ONCE**, at root scope. It reaches
**four** generation sites and **three of the four are silently optional** — a component added to the editor's
list and not the serializer's is registered, inspectable, editable and **NOT SAVED**, with every test green.
**The site the variable does not reach is the one that matters**:
`engine/scene_serialize/src/scene_serialize.cpp`'s hand-written dispatch table, plus
`builtin_serializers.hpp`'s declarations. All five move together, or the result is a link failure or — far
worse — green and wrong.

**A COMPONENT-COUNT LITERAL IS NOT CONFINED TO THE TESTS THAT ARE ABOUT COMPONENTS.** 3.7.2 found them in
`scene_test.cpp`, `transform_test.cpp`, `editor/hierarchy_test.cpp`, `editor/inspector_test.cpp` and
`scene_serialize_test.cpp` — **every one found by a red test, none by a grep.**

**AND THE GREP ALSO OVER-MATCHES.** E.2.1's sweep came to **21** built-in literals (not the 18 planned) and
**19** default-scene pins, against **five look-alikes that had to stay unchanged**: `scene_test.cpp:224`
/`:1175`, `scene_serialize_test.cpp:596`, `hierarchy_test.cpp:468`, `scene_io_test.cpp:281`. Three of the 21
are reachable by **no grep at all** — a component *tally* rather than a type count, a literal on the line
*after* its `componentTypeCount()` call, and a **TEST-CASE NAME** (`PB13`'s `400-byte Lights block`, with its
arithmetic in prose beneath it). **E.2.2 re-measured the default-scene pins: SEVENTEEN `entityCount() == 4`
lines, not ten** — sixteen real pins plus `hierarchy_test.cpp:635` (duplicate-entity arithmetic). **Carry the
LIST: "unchanged from ten" is a different claim from "unchanged from seventeen". A count is only as good as
the grep behind it — re-run every one, and classify each hit by READING ITS COMMENT, never by its shape.**

**`createEntity` (`entity_ops.cpp:68`) ALWAYS ADDS A `Transform`.** A seeded entity that must not carry one is
built with `world.create()` + `setName` directly — never `createEntity` + `remove<Transform>`. E.2.1's
"Environment" seed is the precedent. **And the assertion it broke is the lesson:** `hierarchy_test`'s
`for (root) CHECK(has<Transform>)` was true only *by accident*, and is now four per-entity statements with
`CHECK_FALSE` on the fourth. **A universal that is true by accident hides the seed that falsifies it.**

**THE VOCABULARY SPLIT (E.3.3): reflect-gen validates a GRAMMAR and the EDITOR owns the VOCABULARY**, because
`AssetKind` is an editor type the engine's reflect layer must not know. **Do not add a kind list to the tool.**
`AERO_ASSET(kind)` is the fourth field annotation; a fifth `DropSurface` with a NON-DEFAULTED fourth parameter
routes the field through 3.1.5's own matrix.

#### The editor — ImGui, ImGuizmo, focus and panels

**THE EDITOR HAS EXACTLY ONE FOCUS SLOT, AND `ImGui::SetWindowFocus` IS CALLED AT MOST ONCE PER FRAME FROM IT
(E.3.2).** It lives in `shell_ui.cpp` immediately before `DockSpaceOverViewport` — dock nodes update inside
it, so the focus lands with no one-frame lag — and resolves both paths in order: explicit `requestPanelFocus`
first (a COMMAND), then the pending context route (an INFERENCE). `I159(a)` pins one file, three calls. **A
fourth caller in another file is a second focus policy with no way to order it against the first.** The reason
is that **`FocusWindow`'s two side effects are not idempotent**: it closes every popup above the focused
window (`imgui.cpp:13740`) and **steals the active widget** (`:13754-13756`, whose own comment at `:13751`
names this very slot) — and a stolen `InputText` edit is **DISCARDED**, not interrupted, because
`MaterialPanel` commits only on `IsItemDeactivatedAfterEdit()` and the panel that lost the tab never draws to
observe the edge.

**RE-READ AT EVERY ImGui / ImGuizmo BUMP:** `imgui.cpp:13740` and `:13754` (focus side effects); `:8848`
(`g.LastItemData = ParentLastItemDataBackup`, so a last-item rect read after a `BeginPopup`/`EndPopup` pair
names the item *before* the popup); `:3418` (`Begin` sets `DisplayStart` to −1); `:8279` (an API-positioned
popup is never clamped); `:3918` (a label truncates at its first `##`); `imgui_widgets.cpp:6802` +
`imgui.cpp:8581` (`StateStorage` is per window); `imgui.cpp:6860` + `:12129` + `:437` (`EndChild` → `ItemSize`
→ the spacing recipe); `:1657` vs `:1703-1708` (1.92.8 removed the "a 1 px `Separator` does not move the
cursor" hack while `SeparatorEx`'s header comment still describes it); `ImGuizmo.cpp:1229-1230`.

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
`axis_palette.hpp` and `viewport_panel.cpp` states no gizmo colour literal at all, because **a restated
literal one byte off is invisible to every automated tier** (E.1.4's sabotage row 20).

**A CHROME WIDGET THAT SUBMITS NO ImGui ITEM IS INVISIBLE TO ImGuizmo'S OWN PROTECTION (E.1.3).**
`CanActivate()` is `IsMouseClicked(0) && !IsAnyItemHovered() && !IsAnyItemActive()`, so a widget drawn with
`ImDrawList` alone is protected only by accident. Widen the `ImGuizmo::Enable` term — never an early return
(it would hide the handles), never `!IsOver()` in the widget's guard (it reads `gContext` before this frame's
`Manipulate`) — guarded by `!IsUsing()` so an in-flight drag survives. **Any future viewport chrome drawn with
`ImDrawList` alone inherits this and must claim its own presses.**

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

**THE STYLE IS DOUBLED ON A RETINA DISPLAY AND THE FONT IS NOT (E.3.4) — E.6.1 OWNS THIS.**
`ScaleAllSizes(SDL_GetWindowDisplayScale(win))` runs unconditionally at `imgui_layer.cpp:87-89`, while
`io.ConfigDpiScaleFonts` only overwrites `FontScaleDpi` when a monitor's DPI **CHANGES** — which never fires
for a window created already on the 2x display. Measured at scale 2.0 in a 320x180 window: `availHeight`
**98** (`windowHeight − 82`), `fontSize` 13, `frameHeight` **25**, `textLineHeight` 13, `itemSpacingY` **8**,
`SeparatorSize` **2** — so fixed chrome costing 71 points at 1x costs **103 against 98**. A layout with one
mode answers that with a zero-height element, **failing on every Retina Mac while the three 1x CI lanes stay
green.** Any panel with fixed chrome needs a MODE, a floor, and a threshold chosen to make the derived height
**continuous** across the boundary.

**A NON-DEFAULTED PARAMETER ON A WIDELY-CALLED EDITOR FUNCTION IS A 57-LINE EDIT, AND IT IS STILL THE RIGHT
CALL.** `buildSelectionOverlay` has **38 call sites, 37 in `selection_overlay_test.cpp`**; `projectToViewport`
7, `clipSegmentToNearPlane` 5, `gizmoOriginBehindCamera` 7, `overlayOwnsPress` 16. A default lets a future
site silently take the wrong arm — a wrong picture with no error and no failing test; non-defaulted makes
every unconverted site a compile error, which is what makes such a change atomic. **Pass a file-local
`constexpr auto` alias, never the full enum spelling** (seven characters per line instead of forty, which
keeps them under the 120-column CI skew). **And a mechanical rewrite must not count commas at bracket depth**
— `std::array<Entity, 1>{cube}` hides one inside a template argument list, which put the new argument one slot
early on 17 of 37 sites at E.1.3.

#### Rendering and the GPU

**A RENDER TARGET'S DEPTH IS ONLY READABLE IF IT WAS STORED, AND NOTHING CAN DETECT THAT IT WAS NOT.**
`RenderTargetConfig::depthStore` and `PostProcessConfig::sceneDepthStore` both default to `Clear` → `DontCare`.
On a tile-based deferred renderer — **every Apple Silicon Mac** — `DontCare` means the tile's depth is never
written back, so a later `LoadOp::Load` reads **GARBAGE, not stale-but-plausible values**: an EMPTY result on
Metal and a CORRECT one on D3D12/Vulkan, the worst failure shape there is. **And a pass attaching an existing
depth with `LoadOp::Load` must spell BOTH load ops**: the rhi cycles a depth target iff ANY load op is not
`Load`, `stencilLoadOp` defaults to `DontCare`, and the combination trips `SDL_BeginGPURenderPass`'s own
assertion and **HANGS the process** rather than failing it.

**A PASS THAT RE-RASTERISES GEOMETRY AND COMPARES AGAINST AN EXISTING DEPTH MUST PAIR WITH THE SAME VERTEX
STAGE, NOT AN EQUIVALENT ONE.** No graphics API guarantees position invariance across two different vertex
shaders; the failure mode is a **speckled mask that reads as a depth-bias bug**. E.1.4's mask pass reuses
`scene.vert` / `scene_skinned.vert` verbatim and pays `GpuPerObject`'s 208 bytes rather than a `Mat4`. It also
uses `CompareOp::LessOrEqual`, never `Less`: `Less` rejects every fragment whose depth is already in the
buffer, and the mask comes out empty.

**A PASS WHOSE TARGET HAS MARGIN MUST SET VIEWPORT AND SCISSOR, AND NO `quantum = 1` TEST CAN SEE THAT IT DID
NOT.** `renderShadowMap` sets neither, correctly — its texture has no margin. `renderSelectionMask`'s does:
with `beginRenderPass`'s default full-target viewport the mask maps across the ALLOCATION while the resolve
maps across the DRAWN rect, so the result is silently rescaled and slides as the panel is resized. Every
convenient test target is `quantum = 1`, where `drawExtent == textureExtent` and the bug is invisible; `OG7`
uses a 200x140 draw inside a 256x192 allocation and is the only case that can catch it.

**A UV THAT ADDRESSES A SUB-RECT NEEDS TWO DIFFERENT FAR BOUNDS (E.1.4).** `tonemapSourceUvMax` returns the
drawn rect's **exclusive** far edge — right for a fullscreen triangle's far corner, **wrong as a clamp**:
under Nearest filtering `floor((drawW / texW) · texW) == drawW` is the **first cleared MARGIN texel**. A clamp
bound is the last drawn texel's **centre**, `(drawExtent − 0.5) / textureExtent`, a DISTINCT quantity with its
own name. **The case that guards it must run on a MARGINED target** — `drawExtent == textureExtent` makes it
unobservable, because the hardware's own `ClampToEdge` answers correctly there.

**THE MASK MIRRORS `draw()`'s FRUSTUM CULL, WITH `draw()`'s OWN RESOLVED FRUSTUM (E.1.4).** "An off-screen
instance writes to no texel" is FALSE as a reason to skip it: `draw()` culls on the **cooked AABB**, so an
instance whose bounds are invalid or smaller than its triangles is dropped from the picture while still
projecting on screen. `draw()` publishes its resolved `(frustum, culling)` pair and `renderSelectionMask`
reads it — a mirror, never a second extraction. **Any future pass that re-draws a subset of the forward pass's
instances inherits this.**

**EVERY CLIP-SPACE PREDICATE IS PROJECTION-AWARE, AND THE PARAMETER IS NON-DEFAULTED (E.1.3).** An ortho
proj's bottom row is `(0,0,0,1)` and the view matrix is affine, so `clip.w` **does not depend on the world
point at all** — which made every "in front of the eye" test vacuous under ortho. `projectToViewport`,
`clipSegmentToNearPlane`, `gizmoOriginBehindCamera` and `viewportRay` all take a **NON-DEFAULTED**
`ProjectionMode` (`CLIP_Z_EPSILON = 1e-6` on `clip.z` in ortho). **The two gates are not equivalent and the
asymmetry is shipped**: perspective's `w > 0` means "in front of the EYE" and admits a point closer than
`nearPlane`; ortho's `z > 0` means "beyond the NEAR PLANE" and rejects it. **A universal `z`-based gate is
2.3.2's contract to change and is an unowned handoff.**

**A THRESHOLD CALIBRATED IN ONE PROJECTION'S DEPTH UNITS MEANS SOMETHING ELSE ENTIRELY IN THE OTHER (E.1.3,
and it shipped as a defect).** ImGuizmo's `0.001` near-band is calibrated against a **view-space** depth;
ortho's `clip.z` is a **normalised** one, so the same constant reached ~1.0 world units and suppressed the
gizmo outright after `focusOn`. The mirror is now gated on `Perspective` (PR #95). **"Stricter" is only safe
against the failure it was written for.**

**THE RHI GREW EXACTLY TWO CALLS AT E.1.1, AND EACH HAS ONE SENTENCE THAT VOIDS SILENTLY.**
`Device::recordBufferUpload` **replaces the WHOLE buffer** — everything past `data.size()` is undefined
afterwards, because the destination is cycled, which is why there is no `dstOffset` and why a partial write
cannot merge with last frame's contents. It refuses a command buffer with **a render pass open, in every
configuration** — that refusal is OURS, not SDL's, which only checks under `debug_mode`.
`Device::readbackTexture` is **BLOCKING and a test-and-tooling path**, and **its `SDL_WaitForGPUFences` call
is what performs the copy on D3D12**, where the download is deferred into `D3D12_INTERNAL_CleanCommandBuffer`.
Map before the wait and you read garbage **on Windows alone**. Release the fence **after** the wait.

**A DEPTH BIAS DOES NOTHING TO A LINE PRIMITIVE.** Measured at E.1.2 on the real `DebugDraw`: D3D12 and Metal
both exclude point and line primitives from rasterizer depth bias by specification, Vulkan permits without
guaranteeing. A 13 x 5 sweep moved a line at no gap down to `1e-5`; the same bias moved a `TriangleList`
billboard predictably, bracketing Metal's unit at `2^-24` for `D32Float`. **`DebugDrawConfig` therefore has no
bias field, and adding one for lines would be inert on two of three backends.** The remaining consumers are
triangle topologies: the shadow pass (`forward_renderer.cpp`, the only live consumer), and **E.2.3 EXAMINED
the handoff and DECLINED it** — every billboard it pushes is `DebugDepth::Overlay`, whose pipeline does not
test depth at all. **The handoff is RE-ISSUED** to whichever task first wants a depth-*tested* billboard.

**THE DEBUG BATCH IS SHARED, SO "THE BATCH IS EMPTY" IS A WHOLE-EDITOR CLAIM — AND THE WALL IS FOUR CASES, NOT
THREE.** Any task adding a producer to `render::DebugDraw` reddens every case that counts the batch exactly:
E.1.2's grid took `I108`/`I109`/`I111`; **E.2.3's icons took `I108`, `I109`, `I112`** (`I111` reads only LINE
quantities and icons are billboards; `I113` uses a `>` bound). **Fix it with the producer's own toggle seam,
never by restating the magnitude** — and `uploadCount()` is a *lifetime* counter, so for `I108`/`I109` the
toggle must precede the warm-up ticks while `I112` may take it after. **THE FOURTH CASE IS INVISIBLE TO THE
COUNTER GREP: `I110`'s roster subcase** walks `editor/src/*.cpp` and asserts exactly which files name
`DebugDraw` at all, so it reddens the moment a new producer TU lands — four commits before that producer has a
call site. **Sweep for the TYPE NAME as well as the accessors. The next producer inherits all four toggles.**

**THE MODES LIVE ONLY ON THE CPU, AND THE GPU RECEIVES DIFFERENCES — NEVER ENDPOINTS, NEVER A `lerp` (E.2.1).**
`resolveSkyGradient` packs `{horizon, sky - horizon, ground - horizon}` and `resolveAmbient` packs
`{mid, halfDelta}`, so **the shaders carry no mode, no branch and no selector**, and a zero delta makes
`x + 0*w` exact on every backend. **Verified against the Khronos registry, not assumed: `GLSL.std.450`'s
`FMix` is specified `x(1-a) + ya` and DXC maps `lerp` to it** (`docs/SPIR-V.rst:2595`), so a `lerp` form
differs from `x` by an ulp when `x == y` and would destroy the exactness that lets Solid + Flat reproduce a
clear and a constant **bit for bit**. **Do not "simplify" the resolvers back to endpoints.** (`-0.0 + 0.0*w`
is `+0.0` — equal, bit-different.)

**THE FULLSCREEN TRIANGLE IS BACK-FACING** under SDL_GPU's normalised convention, measured: `CullMode::Back`
on `SkyPass` deletes the sky entirely. `CullMode::None` there is load-bearing, not stylistic. **And a
depth-write-with-test-off seed is INERT on all three backends by API rule** — SDL Metal computes
`depthWriteEnabled = write && test` (`SDL_gpu_metal.m:1183`), Vulkan skips the depth update when
`depthTestEnable` is false, D3D12 maps test-off to `DepthEnable = FALSE`.

**A "NEGATED" COMPARISON IS ONLY NaN-SAFE IN THE DIRECTION IT WAS WRITTEN FOR (E.2.3).** 2.3.2's A10 is a
REFUSAL — `if (!(d <= radius)) return;` — and a NaN `d` correctly fires it. Translating that into an
ACCEPTANCE gives `d <= radius`; **`!(d > radius)`, which reads like the same thing, is its opposite** and
accepts a NaN. It shipped that way with a comment claiming the safety it did not have, and no case could see
it because `projectToViewport` refuses a non-finite projection.

**`MaterialParams{}` IS NOT THE RENDERER'S DEFAULT MATERIAL (E.1.4).** It defaults `metallicFactor` to glTF's
`1.0`, and *a metal with no environment to reflect renders near-black under analytic lights* — so a test quad
built from `MaterialParams{}` **is** drawn and is byte-identical to the background, which makes any colour
assertion over it vacuous. Start from `DEFAULT_MATERIAL_PARAMS`.

**THE TREE ASSERTS PIXELS NOW** — `RU5` first, then `DG5`–`DG16`, `DG18`, `I108`–`I113`, on Metal, WARP and
lavapipe, on every push. **But a pixel assertion on GENERATED geometry must not name a single pixel**: under a
centred camera an axis lands on a pixel *boundary*, where Metal lights row 95 while the projection names 96,
so `DG18` bounds where the lit **run** starts to **±1** — the width of the genuine fill-rule ambiguity. **A
later visual task that settles for "no backend error" is choosing to, not forced to**: `readbackTexture` plus
a `RenderTarget` under an identity camera makes "which pixel" arithmetic rather than judgement.

**A FLOAT COLOUR CONSTANT CAN COLLIDE WITH `std::numbers` (E.2.3).** `176/255` through the sRGB EOTF is
`0.43415`, 9.4e-05 from `std::numbers::log10e`, and trips `modernize-use-std-numbers` — as **every** accurate
spelling of that colour does; carry a `NOLINTNEXTLINE` with the reason beside it. **And re-derive such
literals rather than trusting a plan's**: E.2.3's plan spelled sRGB byte 32 as `0.0176`, which re-encodes to
**36**; the measured value is `0.0144`.

#### Floating point

**`a + (b − a)` IS NOT `b` (E.1.3): an animation that must land on a boundary must HOLD its endpoint.** And
**`MAX_PITCH` is now exactly `HALF_PI`** — safe here because the composition is yaw-outer / pitch-inner, so
`right()` is independent of pitch, `viewMatrix()` has no `lookAt` and no up vector, and nothing divides by
`cos(pitch)`. Measured pole residual: worst component error 2.384e-07 over 1441 yaw samples; `right().y` is
exactly 0 there.

**`k * fl(1/255)` IS NOT `fl(k / 255)` (E.1.4)** — the reciprocal form is bit-unequal for **126 of the 256**
byte values, first at `k = 3`, and looks identical to six significant digits. **Any packer/unpacker pair that
wants an exact round trip must divide.**

**`fl(1e-4) * fl(1e-4)` IS NOT `fl(1e-8)` (E.2.2)** — one ulp below. A test arm claiming to evaluate "the same
expression the implementation evaluates" while spelling the squared constant as a literal is a **different**
computation that agrees only by rounding. Spell the constant squared.

**`std::clamp(NaN, lo, hi)` RETURNS NaN ON libc++ (3.7.2), AND `std::min(NaN, 48.0F)` RETURNS NaN (E.1.2).**
Every clamp on a value that can be non-finite needs an explicit finiteness arm *first*.

**A FLOOR DOES NOT MAKE A FUNCTION TOTAL WHEN THE NaN IS IN THE *DIFFERENCE* (E.2.2).** The spec's
`resolveSpotCone`, its header comment and the test that pinned it all agreed with one another and all three
were wrong: a NaN inner angle makes `cos(inner) - cos(outer)` NaN, the NaN takes the delta floor, and the
result is a finite **hard-edged cone at full intensity on the axis** — a plausible wrong picture. Test
`std::isfinite` on both cosines **FIRST** and return `{0, 0}`, which makes `saturate(cosAngle*0 + 0)^2`
exactly `+0.0` for every cosine, NaN included, on every backend (`std::cos(±inf)` is NaN, so infinite angles
land there too). **Generalisation: re-run every FORMULA in the precision it will execute in, exactly as
E.2.1's lesson was to re-run every GREP. A spec's own code is a claim, not a proof.**

**THREE FLOAT FACTS FROM E.1.2, EACH MEASURED AND EACH COUNTER-INTUITIVE.** (1) **A float-indexed lattice loop
does not terminate**: at `focus = 1e6, spacing = 0.01` the quotient reaches ~1e8 where `k += 1.0F` is a no-op
— and the naive repair traps, because `inf - inf` is NaN. Use an integer counter and clamp the span.
(2) **Decade lattices NEST in float32, so coordinate divisibility cannot identify a cadence.** (3)
**`pow10(n)` must be computed FRESH from `1.0F`**, never as a running product.

#### Test method

**ASSERT THE EFFECT, NEVER THE INTENTION** — the single most repeated failure in this project's review rounds.

**A SEAM'S OWN ACCESSOR IS A ROUND TRIP, NOT A TEST (E.3.4).** Reading back the flag a seam just wrote reports
what was *requested*, never whether ImGui *obeyed*. Seed `S26` (`ImGuiCond_Once` instead of `Always`, handing
the node back to ImGui's storage so it never opens) walked through **149 green assertions** for that reason,
with five `BeginCombo` calls and a `DragInt` executing on no lane at all. **Assert a consequence the widget
PRODUCED** — `materialSamplerRowsDrawn()` counts disclosures that actually submitted their rows. E.3.4's two
seams are `requestMaterialSlotDetails`/`materialSlotDetailsOpen` (the sampler disclosure) and
`requestMaterialSectionOpen`/`materialSectionOpen` (the eight sections); both read back only what was
REQUESTED. **Any future panel state driven by a seam inherits this.**

**A TEST THAT COMPARES TWO VALUES FROM THE SAME SOURCE ASSERTS NOTHING, AND READING IT WILL NOT TELL YOU.**
E.1.2's sabotage matrix found **two**, each green on a seeded regression plainly visible in the product:
`GR8` computed its own `f → 1` side from the emitter's formula and compared it against the emitter's `f == 0`
side — both halves from one source — and the naive two-set crossfade passed **13.3 million assertions**;
`GR12`/`GR6` missed a snapped disc centre that **jumps ten world units for a 0.2-unit pan**. **The code review
read both and approved both**, correctly: the flaw is invisible to reading, because such a test looks exactly
like one that works. **Only mutating the code and watching the test not care exposes it.** Two rules follow:
read back the value under test **off the thing under test**, on both sides of any identity; and **a case is
only as strong as the pose it samples** — `GR6` was blind because its focus defaults to the origin and
`round(0/s)*s == 0`, so an invariant about following the camera must be asserted at a focus deliberately OFF
the lattice, with an anti-vacuity arm proving it is.

**AN ASSERTION IS ONLY AS STRONG AS THE TOLERANCE IT IS WRITTEN AT (E.2.1).** `SB9` and `SB16` compared the
sky's centre texel **bit-exactly** against a gradient oracle their own sibling arms only claim to 2 half-ulps,
so when the sky genuinely covered the cube the texels were **one half-ulp apart and both arms reported
SUCCESS**. Both now assert a distance **greater than** the tolerance. **And a refusal case proves nothing
about WHICH inputs are refused unless something pins an input the narrower check would accept** —
`packSkyCamera`'s sixteen-element loop reduced to column-0-only left the whole suite green, because `SB3`'s
refusals make *all four* columns non-finite.

**A TEST'S TOLERANCE CAN ALSO BE WIDER THAN THE DEFECT IT GUARDS (E.3.1).** `VF6`'s `Approx(1).epsilon(1e-6)`
could not see a dropped `normalize`, because GLM's euler constructor is **already unit to 5.96e-08** — exactly
`1 - 2^-24`, a one-ulp miss. **And the arm that replaces such a check must not assert that ulp either**: it is
a property of the host libm and of FMA contraction policy, so a bitwise claim there reddens CI on a correct
tree. A `WARN` is the right instrument when a case's own strength depends on a float property.

**A CASE THAT RUNS THE RIGHT CONFIGURATION STILL ASSERTS NOTHING IF THE SAMPLE POINT IS INSENSITIVE (E.2.1).**
The hemispheric ambient had **no pixel cover anywhere** until `SB17`: `DG6` and `OG` run Flat at intensity 1,
where `halfDelta` is exactly zero — blind **by construction** — and `SB16` read a face with `N.y == 0`, where
the term collapses to `mid`. **And at `N.y = +1` the mid/half-delta swap is INVISIBLE** (`mid + halfDelta*1`
and `halfDelta + mid*1` are the same sum), so the case needs an up-facing read, a **down**-facing read and a
Flat control. **A symmetric property needs an asymmetric sample.**

**AND A CLAMP THAT BOUNDS A LOOP DOES NOT NECESSARILY BOUND A COUNT.** `debug_grid.cpp` claimed its span clamp
made `DEBUG_GRID_MAX_LINES` structural; removing the clamp leaves the battery green with an identical
assertion count. The count is bounded by the **disc clip**, not the clamp. Two readings passed over that
sentence before a seed disproved it.

**A SANITISER ADDED INSIDE A SHARED PURE FUNCTION IS INVISIBLE TO A TWO-SIDED A/B (E.2.4).** `PX`'s byte
identity compares the preview rig against `SceneRenderer`; side A **is** the rig, so a defensive clamp added
inside `materialPreviewView` would sanitise side A while side B passes the number through — all four arms stay
green while the identity breaks in the one direction they structurally cannot see. Assert such a case at
tier 0 instead.

**A SET CLAIM NEEDS A SET ASSERTION (E.2.4).** `I127(b)` was "no editor file names `SkyPass`"; it is now an
exact sorted, de-duplicated allowlist — `{material_preview.cpp, material_preview.hpp}` — because the old
per-line string accumulator could not express a set claim at all. A third file naming the type reddens it; widening it to `<= 3` makes the same seed invisible
(both directions proven). Note an `#include <aero/render/sky_pass.hpp>` line is invisible to that sweep by
construction — it spells `sky_pass`, not `SkyPass`.

**`PanelRegistry::noteDrawn` IS WHAT MAKES A TAB ASSERTABLE AT ALL (E.3.2).** `ImGui::Begin` returns false for
a docked window that is not the selected tab and `drawPanels` skips `onDraw` entirely, so "the Inspector
raised" was **unfalsifiable at every automated tier** before it — 3.1.3's log records two attempts that passed
while executing none of the code they named. Every routing claim is a **delta across one tick** with an
anti-vacuity arm. **And MATERIAL is the Right node's default front tab, with every right-node panel drawing
once on frame one**, which makes `panelDrawnCount(x) > before` satisfied with or without a route when `x` is
already drawing — put the target in front with an explicit request and `REQUIRE` it is not drawing first. **A
baseline taken before the two settle ticks reads a layout artefact.**

**NOTHING IN `tests/` CAN SYNTHESISE A CLICK, PRESS A KEY, OR OPEN AN ImGui POPUP.** The backend rewrites
`io.MousePos` every `NewFrame` and ImGuizmo exports no getter for `mbEnable`, so `I118` and `I115` are
source-text pins that say so in their own comments. `I142` exercises `BeginPopupContextItem`'s own
`IM_ASSERT(id != 0)` on both call sites every frame — real cover — **but the popup never OPENS**, so
`EndPopup`, both `BeginDisabled` pairs, `MenuItem`, `Separator` and the whole of `resetField` execute nowhere
in CI. **Every future context menu inherits this hole, and a manual validation pass is its only behavioural
witness.**

**ENTT'S `each` WALKS IN REVERSE CREATION ORDER (E.2.2).** A truncation test seeding nine entities and
expecting the first eight **by creation order** reads the wrong ones: collect the World's own walk into a
vector and compare element for element, never a second `buildRenderView`. **And `buildRenderView` walks
`each<Transform, MeshRenderer>` in EnTT's storage order while `buildSelectionMaskSet` walks the SELECTION** —
comparing the two builders' output BY POSITION asserts that two unrelated orders agree. Match by a key and
`REQUIRE` its uniqueness.

**A `REQUIRE` ON A MID-FLIGHT ANIMATION AFTER ONE REAL FRAME IS A CROSS-LANE FLAKE (E.1.3).**
`PanelContext::deltaSeconds` caps at **0.25 s, exactly `VIEW_SNAP_SECONDS`**, so one slow frame completes the
whole snap. Drive a mid-flight property at the pure tier where the delta is a parameter.

**TWO PLACES MUST NEVER COMPARE THE SAME KEY BY DIFFERENT RULES (3.7.2).** A binding matched on full `Entity`
identity and swept by index alone orphaned a looping voice permanently. One comparator, one place.

**FOUR TEST-FILE RULES**, each earned: **no `#if` of any kind** in a test file (3.6.3 shipped four cases inside
a file-level `#if`, everything green while the one arm that mattered never ran); **`#include <ostream>`** in
any TU that `CHECK`s a `std::string_view` (the 0.4.1 MSVC trap, hit five times); **exact float assertions
where the arithmetic is exact**, and a tolerance **with the epsilon as part of the assertion** where it is
not; and **assert the EFFECT, never the INTENTION.**

**THE `toString` TRAP.** doctest's `DOCTEST_STRINGIFY` expands to an **unqualified** `toString(...)`, so an
engine `toString(SomeEnum)` on a public header is found by ADL, beats doctest's own template, and the
decomposer then tries `std::string_view + const char*` — a **hard compile error on every lane**, reported
inside `doctest.h`. Name label functions anything else (`audioClipLoadStatusLabel` is the precedent). A
scoped-enum comparison inside a `CHECK` needs **double parentheses**.

**AN `operator<<` IN A TEST'S ANONYMOUS NAMESPACE IS INVISIBLE TO doctest WHEN THE TYPE IS NOT (E.2.2).**
doctest's streamability trait calls an **unqualified** `operator<<` from inside `doctest::detail`, so only
namespaces **associated with the arguments** are searched — `engine::render` for a `render::SpotCone`, never
the test file's anonymous namespace. **The sibling helpers that work are not evidence that a new one will**:
`Half4`, `Rgba` and `Size4` each stream a type declared in that same anonymous namespace, which is what makes
them reachable. A printer for an **engine** type must live in that type's own namespace, and nothing warns
when it does not. **And a comparison struct declared inside a `TEST_CASE` cannot carry an `operator<<` at
all** ([class.friend]/6 forbids defining a friend in a local class) — hoist it to the file's anonymous
namespace or every assertion prints `{?} == {?}` on failure. It is not optional where a pixel claim rides on
it: `CHECK((a == b))` prints `CHECK( true )` on a FAILURE as well as a pass.

**`doctest::Approx(x).epsilon(0.0)` NEVER MATCHES** (its comparison is `< 0`) and prints `1 == 1` on failure;
**`CHECK(a && b)` is a hard compile error** ("Expression Too Complex").

**A `-tc=` FILTER IS A GLOB, NOT A REGEX, AND A FILTER THAT MATCHES NOTHING EXITS 0.** `*PK1[35]*` selected
**zero** cases and the binary reported success — which reads as "the seed reverted cleanly" during a sabotage
run on a tree where the seed was still live. **Read doctest's own `test cases:` line, never the exit code.**

**`static_assert(!requires(T v) { v.member; })` ON A NON-DEPENDENT TYPE IS A HARD COMPILE ERROR**, not
`false` — a requires-expression only substitutes when the type is dependent. Route such a check through a
file-local concept and carry a **positive control**, without which a mis-spelled detector makes the negative
assertion vacuously true for every type in the language (`HE17`'s shape).

#### Sabotage method

**SEED THE MISTAKE, NOT THE SYMPTOM, AND A SEED PLACED AFTER THE GUARD THAT REFUSES IT IS INERT (E.3.2).**
E.3.2's `S16` (a `setVisible` inside the `Apply` arm) could not change behaviour at all, because a hidden
target takes the **Drop** four guards earlier. **Mutate an arm the way a careless edit would, not the way a
demolition would, and pin the offending TOKEN rather than the arm's generic sentence** — a coarse mutation is
caught by any assertion and proves nothing about the one under test.

**A CLAUSE-ORDER SEED IS INVISIBLE UNLESS THE LOSING CLAUSE WOULD HAVE WON (E.2.3)**, and a REDUNDANT arm
makes its own seed unobservable. Three of E.2.3's seeds came back green for the second reason and none was a
defect: a redundant `alive()` guard behind `World::has<T>`, a `std::clamp` an unclamped caller would have
skipped anyway, and a faithfully re-derived basis that is bit-identical. **Such an exposure prevents DRIFT
rather than fixing a defect — record it as a non-finding, do not "fix" it.**

**AN ADDRESS COMPARISON CANNOT SEE A DANGLING `c_str()` (E.3.2).** `routedPanelId(x) == routedPanelId(x)`
stayed **green** against a seeded `return std::string("Inspector").c_str();` — the temporary is SSO, so it
lives in the callee's frame and two calls from one caller land at the same address, and ASan's
`detect_stack_use_after_return` is **off by default**. **Pin a lifetime by surviving intervening work, never
by an address.**

**BUT `detect_stack_use_after_scope` IS ON BY DEFAULT, AND THEY ARE DIFFERENT CHECKS (E.3.4).** Inlining a
by-value key into an aggregate whose field is a `string_view` does **not** survive silently: ASan reports
**`stack-use-after-scope`**, a read in the caller's own frame, and the case aborts. The after-*return* check
covers a returned frame; the after-*scope* check covers a temporary that died in the current one — so a
scratch local that outlives the aggregate stays, and has automatic cover on all three Debug lanes.

**A CASE WRITTEN FROM THE SAME SENTENCE AS THE CODE CANNOT FALSIFY THAT SENTENCE (E.3.2).** `RT24` **pinned a
defect while calling it correct** — it re-observed a value produced while the feature was disabled and named
that "a fresh act". **A seed that restores the defect, not a re-reading, is what closes such a finding.**

**VERIFY THE SEED LANDED** before trusting any guard verdict — BSD `sed`/`perl` give silent false PASSes.

### Test inventory — measured, never remembered

**Read totals from doctest's own `filters:` line, never from a `grep -c` of case names, and re-measure on the
tree in front of you.** The moment a branch merges `origin/main`, every whole-tree count on its own page goes
stale, and adding one task's delta to another task's baseline is exactly the arithmetic that produces a
confident wrong number. **Never predict a delta arithmetically** — a `SUBCASE` is not a `TEST_CASE`, and
several tasks have added twenty-plus assertions while moving no total at all.

**AND A BUILD TREE GOES STALE SILENTLY, WHICH `ctest -N` CANNOT SEE.** At E.2.4's branch point
`build/macos-debug` was a build of a pre-E.2.3 commit and reported E.2.2's doctest totals while
`build/macos-release` was current — and `ctest -N` read the same number out of both, because the entry count
is a configure-time property. **Rebuild before you believe any doctest number, and read the totals out of both
presets so a disagreement is visible.** A recorded total goes stale the same way: `origin/main`'s own shell
total was one stale at E.1.4's gate. **Read the binary, never the block.**

**At E.4.3's gate**, measured on both presets out of freshly built trees and agreeing between them, with
both reduced configurations configured fresh:

| Measurement | Value |
|---|---|
| `ctest -N` | **178**, entry set byte-identical between presets AND to E.4.2's; **165** shader-tools-OFF, **93** reflect-tools-OFF; `cooker.*` **70 / 70 / 70** |
| doctest, seven binaries | **1404 / 2084 / 247 / 40 / 59 / 10 / 28** |
| guards | math **525**, platform **92**, rhi **163**, scene **92**, golden-rule **165**, project-no-delete **A=7 B=89**, audio **11-3-55**, probes **6-57** |
| `git ls-files` | `editor/src/*.cpp` **89**, `editor/include/aero/editor/*.hpp` **64** |

The seven doctest binaries, in order: `aero_tests`, `aero_editor_shell_test`, `aero_editor_imgui_test`,
`aero_scene_serialize_test`, `aero_editor_inspector_test`, `aero_reflect_meta_test`, `aero_reflect_json_test`.
**On a task that adds no component, only the binaries it writes cases into may move.** A moved
`aero_scene_serialize_test` or `aero_editor_inspector_test` means a component crept in; the two reflect
binaries generate from a fixture aggregator, not the built-in list. **A third guard moving on a task that adds
no target is a stop-and-find-out** — math and project-no-delete prong B move with new tracked C-family files
(prong B only for `editor/src/*.cpp`), and the other six should not.

**A TASK THAT ADDS A BUILT-IN MOVES `ctest -N` TOO** (a per-header reflect-gen case) **and three of the seven
doctest totals.** A task that adds none may still move `ctest -N`: E.3.3's four additions are `reflect-gen.*`
**process** cases inside `if(AERO_REFLECT_TOOLS)`. **Read the two kinds of move differently** — a `cooker.*`
addition must be **identical in all three** configurations (`aero_cooker` takes no gate flag), a
`reflect-gen.*` addition is **tools-ON only**, and a smaller move in a reduced configuration means the cooker
block accidentally grew a gate, which **no test can report**. 3.7.3 is the inverse pattern and worth knowing
exists: it moved `ctest -N` by +2 while every doctest total stayed put, because its additions are `cmake -P`
drivers and its one new TU is a probe with no `TEST_CASE`.

**COMPARE ENTRY *SETS*, NOT TOTALS**, measured by comparing entry NAMES with the ctest numbering stripped — a
raw `diff` of `ctest -N` output is dominated by the renumbering and shows every later entry as changed.
shader-tools-OFF removes exactly the **13 `shaderc.*`** entries; reflect-tools-OFF removes the
`reflect-gen.*` entries **plus four doctest binaries**; **nothing is added in either**; and all **70
`cooker.*` entries are present in all three**, which is the property that check is actually about. **Both
reduced numbers and the reflect-gen breakdown have each gone stale more than once** — the `159` recorded for
shader-tools-OFF was wrong by arithmetic alone (it is always `total - 13`), and the breakdown moved three
times without the total moving. **Re-measure both, every time.**

**Both reduced configurations must be configured FRESH with `-G Ninja`**, with an explicit
`-DCMAKE_TOOLCHAIN_FILE=<src>/vcpkg/scripts/buildsystems/vcpkg.cmake` — the `base` preset supplies it and a
raw `cmake -S . -B …` does not, which fails at `find_package(spdlog)` in `engine/core/CMakeLists.txt:16`
before reaching anything the configuration is about — and with **`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`**, since
only the presets set it and without it `boundary-probes.probe_compile_line` skips (honestly, exit 77 → ctest
"Skipped", but a skipped case measures nothing). `CMAKE_GENERATOR` enters the shadercross bootstrap's option
hash, so the generator-less form reads the cached toolchain as **cold** and pays a from-source DXC rebuild
that peaked at 7.6 GB here. **Each run must name which binaries it built and ran.** And
**`check-math-boundary.sh` counts `git ls-files`, so it reads a STALE number until new files are `git add`ed**
— stage first, then measure.

**`I136` IS DISPLAY-DEPENDENT, SO THE LOCAL GPU TIER GATES AT EITHER 231 OR 232 OF 232 AND THE RUN MUST SAY
WHICH.** It fails `REQUIRE(drawExtent.width > 4U)` with value **4** on a 2x display — deterministically at
E.3.4's and E.4.1's gates, on an unmodified `HEAD` — and it **PASSED at E.4.2's gate** (30 assertions, a
full 232 / 232) in a session that touches nothing in the DPI story. **A green run is therefore not evidence
it is fixed**; it is pre-existing, it is the DPI story's, and it is handed to E.6.1. **Name it either way;
never let a known failure be quietly counted as green, and never let it hide a new one.**

**COUNTS DIVERGE BY OS, so never assume one.** Windows skips **FOUR** e2e cases —
`golden-rule.include_scan_e2e`, **`project-no-delete.no_delete_e2e`**, `audio-boundary.guard_e2e` and
`boundary-probes.probe_links_e2e`, registered at `tests/CMakeLists.txt:1078, 1110, 1134, 1155`, all
`NOT WIN32` by the same D16 reasoning (the lint job that runs the scripts is ubuntu-only, and the BSD-userland
proof comes from the macOS lane) — plus fourteen whole `BS` cases, one arm of `BS11` and one GPU case
(`I80`). **The no-delete entry is the one every earlier count omitted; it was measured at E.4.3, and the
figure was "three" here for four tasks.** **Those 3.2.4 skips are not a random sample**: together they are the only coverage anywhere of both
Blender timeouts, cancellation, the `Converted` state, `ok: false`, both `ArtifactUnusable` arms and the
refused-by-cap log.

### Committed fixtures and artifacts

Two images at `tests/fixtures/assets/`; six 32×32 PNGs and six `.ktx2` under
`samples/phase-3-materials/textures/`; seven `.aeromat` at `tests/fixtures/materials/`;
`samples/phase-3-skinning/arm.aero{mesh,skel}`; `samples/phase-3-animation/wave.aero{mesh,skel,anim}`;
`tests/fixtures/audio/` (four encodings of one 1.0 s signal, `tone.aerowave`, the corrupt
`tone-lying-length.ogg`, and **`tone.s16le.pcm` — ffmpeg's own decode, the external anchor that ties dr_wav
and dr_flac to libavcodec byte for byte**); and, from 3.7.2, `samples/phase-3-audio/{orbit,beacon}.aerowave` —
mono 48 kHz 0.5 s, **exactly 48 064 B each**, cut at a whole number of cycles so the loop seam is continuous
*by arithmetic*. **Neither audio sample fixture enters the determinism manifest**, and the README says so.

### The validation debt — the whole of the remaining risk

**Validation pages are gitignored, so they enter no commit.** Per-page measurements and method notes are in
`docs/10`; this is the ledger of what is still owed.

**TWO PAGES HAVE NOT BEEN RUN ON ANY PLATFORM: E.1.5's AND E.3.2's.** They are the whole of
Phase E's validation risk on this OS — every other task in E.1, E.2, E.3 and E.4 is macOS-validated (see
the index above). **E.4.1 is fully validated: 12 / 12 macOS, nothing failed**, and its sabotage matrix is
run too — 26 seeds / 29 runs, **no seed green with nothing else catching it** (`docs/10`). An earlier
matrix attempt was abandoned mid-seed and **left a live seed in the working tree** (the deleted
`sceneIoAvailable()` gate), caught by `git status` before anything was committed. **Always `git status`
after an interrupted sabotage run**; an aborted seed looks exactly like a clean tree until it is read.

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

**E.4.2'S SIXTEEN-ROW PAGE IS RUN ON macOS — 14 of 16, with 2 NOT EXECUTABLE and 1 PARTIAL**
(`editor/validation/E.4.2-scene-project-containment.md`). Seven seed-only rows are now **closed**: row 3
(`S15`'s live `%`-format half — its buffer-overrun half is **inert by construction** and produced no ASan
report), row 4 (`S12`'s consequence: the `OK`-only modal, and undo/redo proving the unsaved work survived),
rows 5 and 6 (the symlink and case routes), row 10 (`S10`, closed on **click** evidence — the File menu is
unclickable and un-highlightable while the modal is up) and row 11 (`S16`, closed for **both** buttons).

**THREE RECORDS REMAIN OWED AND EACH HAS A MEASURED REASON.** Row 9's **Escape** third, row 15
(request-hook popup close; `I192` pins only the ordering as source text and **no tier here can read
`g.HoveredWindow`**) and row 16 (the already-open-project guard; cover stays `CN25`). ★ **THE CAUSE OF ALL
THREE IS ONE MEASURED FACT: NO SYNTHETIC KEYBOARD INPUT REACHES THIS EDITOR** — not CGEvent chords, not
`System Events keystroke`, not the native dialog's Go-to-Folder field — because `aero_editor` is a bare
Unix executable and never becomes a key window. Synthetic **mouse** events work fully, including navigating
the native file dialog by its column view. Row 16 additionally cannot be staged at all: `NSOpenPanel`
watches the filesystem and **clears the selection and disables `Open`** the moment the scene's parent
directory is deleted. Its two **Windows** rows are the two cross-platform risks recorded rather than fixed: `path("C:/").parent_path()` possibly yielding `"C:"` and giving the walk one extra
drive-relative probe, and a scene directly at a POSIX filesystem root being a **false refusal** — both
bounded, both in the safe direction.

**WHY A VALIDATION PAGE IS NOT OPTIONAL: for many tasks it is the ONLY cover a declared sabotage seed has
anywhere.** The recurring pattern is that no tier in this tree can type, click, press a key, open an ImGui
popup, read ImGuizmo's global state, judge a colour at a glance, or observe a frame-to-frame layout fact — so
a rect, a colour, a gesture, a wrap or a HiDPI legibility claim has no automated witness. E.3.4's pass alone
closed **eighteen of nineteen** uncovered seeds across ten rows; E.2.4's closed all six of its own; E.2.3's
rows 3 and 4 were the only cover its two seeds had. **E.3.2's six seed-only rows are therefore live debt**
(`editor/validation/E.3.2-selection-follows-focus-router.md`, twelve rows): row 2 for `S17`, rows 5 and 6 for
`S24` (a raise must not discard a half-typed name or move a drop surface mid-drag), row 7 for `S18`'s
behavioural half, and row 8 for the gizmo-drag hold.

**ONE SEED REMAINS UNCOVERED ON AN OTHERWISE-COMPLETE PAGE AND CANNOT BE COVERED**: E.2.4's `S19`, the
`SkyPass::create` failure arm, is unreachable at runtime in any build whose shaders cooked, so the source-text
pin in `I140(b)` is its only witness — as that page itself states.

**STILL-OPEN RECORDS ON PAGES THAT OTHERWISE PASSED.** E.2.1 has three: row 4's normal-mapped arm is **GATED
ON E.5.1** (a material dropped on a *primitive* is silently discarded, so the default Cube cannot carry one —
a dependency nothing recorded before that pass), row 7's `Save Scene` produced **no write and no log line**
under synthetic input while New Scene, Reset Layout and Undo all worked from the same path — **possibly a real
defect, explicitly unresolved** — and rows 10/12 lacked a material asset and a Tracy connection. E.1.3 has
three: row 6's translate/scale-in-ortho arm (blocked by the defect PR #95 fixed, re-runnable now), row 7 (this
scene's only floor is a debug LINE grid, which receives no shadow — a solid plane is **E.5.2's**) and row 13's
cost A/B, which needs a second build at the branch point.

**OUTSTANDING macOS PASSES: 3.5.1's twelve rows, 3.5.2's twelve rows, and 3.7.2's twelve rows.** Each is the
only cover its task's declared seeds have anywhere. **3.4.2's `S26` cannot be covered from macOS at all** —
SDL queues a texture-container free on Metal and performs it immediately on Vulkan and D3D12, so it is
observable only on a Windows or Linux pass.

**3.7.2's SHAPE IS WORTH STATING RATHER THAN A FORMULA.** CI genuinely compiles and runs every
`SP`/`MX`/`SY`/`SA`/`DV` case on all three lanes, so the spatializer, the mixer, the system and the bridge
**are** cross-lane covered — and **no lane produces a sound**, because CI opens the null backend only.
**LSan runs on the Linux Debug lane alone**, which makes that page's Linux row matter more than most: a green
teardown case on macOS proves the teardown is *clean*, not that a leak is *absent*, and unlike 3.7.1 (whose
leak lived in third-party code) everything 3.7.2 allocates is first-party. Its `A23` and `A36` remain
**uncovered by anything at all** — no lane runs TSan and the null-render buffer belongs to miniaudio.

**3.7.3 ADDS NO PAGE AND THAT IS DELIBERATE** — the 2.1.2 / 2.5.2 precedent: pure test/guard infrastructure
needs no ears, eyes, hardware or OS-specific behaviour a row could measure. Its standing evidence is the
hermetic e2e pair, which runs on the macOS and Linux lanes on every push. **Its residual is Windows**: both
e2e cases are `NOT WIN32` and the lint job is ubuntu-only, so nothing about the guards' *behaviour* is
exercised under an MSYS userland. Coverage of the *invariant* is unaffected.

**NO WINDOWS OR LINUX VALIDATION PASS EXISTS FOR ANY TASK IN ANY PHASE** — Phase 0's gate, Phase 1's render
rows, all thirteen Phase 2 tasks, and every Phase 3 and Phase E task.

**METHOD TRAPS FOR DRIVING THE EDITOR, ALL MEASURED.** A pending TCC prompt stalls the editor to ~0.2% CPU
with a window that **NEVER RENDERS** (a black capture); the prompts **CASCADE**, one per bundle identity, and
both kinds accept a synthetic click on the affirmative button derived from the dialog's own **fresh** bounds
(~`x + 0.727w`, `y + 0.844h`). A stale editor survives `pkill -f` and must be `pkill -9`ed; a stalled bundle
identity stays stalled until a fresh `CFBundleIdentifier` clears it. Synthetic mouse MOVES do provoke ImGui
tooltips; **synthetic typing and synthetic drag-and-drop do not work at all** (DND undocks panels instead).
**Bind every capture to its launched PID** — two live `aero_editor` processes once made a window lookup
capture the STALE one and produced three false "0 differing" comparisons, including one that provably
contained an outline. **Let an anti-vacuity control be what catches that.** Note `screencapture` carries the
display's ICC profile, and **there is no `renderFrame` Tracy zone in this tree** — the frame-level zones are
`renderScene` and `render`.

### Next

**E.4.4, E.4.5, E.5 and E.6 are the open front: eight tasks, planning only.** See `docs/tasks/phase-E.md`,
and `docs/tasks/phase-3.md` for what Phase 3 still owes.

**Ownership of the open work.** **E.4.3 IS MERGED, and it did NOT use `directoryWithin` /
`normalizeForContainment` — deliberately, and the reason generalises.** Those answer *"is this ABSOLUTE
path inside that ABSOLUTE root"*, which is the question a native file dialog's answer raises. Every path
E.4.3 handles is **assets-root-relative by construction** — the browser produced it — so the containment
question it actually has is *"is this relative path safe"*, and that is `validateRelativeAssetPath`: no
`..` SEGMENT, no leading `/`, no drive prefix, no backslash. **A future task must pick by the SHAPE OF ITS
INPUT, not by habit**: an absolute path from a dialog takes E.4.2's pair, a relative path from the browser
takes E.4.3's. **And E.4.3 added no reserved-destination policy for `<root>/Library/`** — it writes the
trash INTO it — because `Library/` is excluded from the scan, so the browser cannot show it and no move
INTO it is reachable through the UI. `IO19` and E.4.2's validation row 14 still record today's permissive
behaviour as the "before", and that handoff is **still open**. **E.4.5** (material names & thumbnails) is
unblocked **twice over**: it has
`material_preview_rig.hpp` to call BY NAME — a thumbnail is `materialPreviewCamera(rig, fixedAngle, 1.0F)`
plus `materialPreviewView(...)` with whatever `MaterialPreviewLighting` it wants — and it has E.3.3's
`ThumbnailService`, which is the "a second PRODUCER, not
a second cache" home: a rendered thumbnail marks a key `Ready` through the same ledger, is evicted by the same
LRU and is drawn by the same `drawAssetTileFace`. **`thumbnailKeyForRecord`'s decodable-extension guard is the
ONE line that producer widens**, and it now has tier-0 cover. E.3.4 adds a third piece of evidence: the
material slot's row is a second, row-sized consumer of `drawAssetTileFace`, which says the face's geometry
parameters generalise past a grid tile. **And `assetReferenceFieldWidth` is ONE formula with TWO hosts**, so a
theme or DPI change moves the Inspector's reference row and the material slot's together. **E.5.1** is an S-sized fix for the confirmed `instance.material`
defect and is independent of everything; E.2.4's `PX` battery gains a non-default-material arm the day it
lands. **E.5.2** owns the coplanar-geometry problem, has a ground colour to sit its plane against, and owns
the Create menu's Light entries (Directional / Point / Spot), which now land something **visible** the moment
they are created — the default scene is deliberately unchanged by every Phase E task so far, and its seventeen
`entityCount() == 4` pins are byte-identical. **E.6.1** owns the DPI story E.1.5 deferred, plus the two
measurements E.3.4 handed it (the style/font scale gap and `I136`). **E.6.2** moves `T R S` and `Local/World`
into the main toolbar and leaves `View` on the viewport — it is per-view, not per-shell. **E.6.3** splits
E.2.4's popover into the mock's header dropdowns; the grouping is already the mock's, so it restyles rather
than regroups. **8.2** inherits IBL/HDRI and the after-opaque sky variant (`SB9`/`SB16` are in place to catch
a wrong ordering), plus physical light units, IES profiles and area lights.

**TWO INSPECTOR-ROW GAPS ARE OPEN AND E.3 CLOSED WITHOUT ADDING OR CLOSING ANY** — E.2.1's **enum-aware row**
(`Environment`'s two modes are bare 0/1 drag fields because reflect-gen cannot reflect an enum) and E.2.2's
**unit-aware row** (`SpotLight`'s cone angles are raw radians clamped to `[0, 1.5708]`). Both are a reflect-gen
surface first. E.3.4 adds one gap of its own shape, unowned: a **closed `File` section**, which needs one flag
plus nothing else now that `requestMaterialSectionOpen` exists, the day a manual pass says the diagnostics are
noise.

**ENTER ACTIVATES NEITHER MODAL'S DEFAULT BUTTON, AND THE CAUSE IS THE ONE THE TASK ALREADY WORKED AROUND
(E.4.3's macOS pass — OPEN DEFECT).** In both the Rename and the Delete modal, `Return` does nothing: the
modal stays open and the operation is not performed, while clicking the button from the identical state
commits at once, and Escape cancels correctly. **`SetItemDefaultFocus()` needs keyboard nav, and
`imgui_layer.cpp` never sets `ImGuiConfigFlags_NavEnableKeyboard`** — which is the SAME fact 3.1.3 cites as
the reason Escape had to be hand-bound with `IsKeyPressed`. So the Enter path was never going to fire, on any
modal in this editor, and the orphan modal's own "Enter == Delete" comment is aspirational rather than
measured. **The fix is symmetric with the existing Escape binding**; it is not a blocker, because every
operation is reachable by its button and nothing is performed incorrectly. **Any future modal inherits this:
do not write "Enter commits" without hand-binding it.** No owner; the nearest is whoever next touches modal
input or the key-binding registry E.6.2 would need.

**NINE UNOWNED HANDOFFS.** **Asset-browser keyboard shortcuts (F2, Del)** — E.4.3 dropped both bindings AND
their accelerator text: the gating needs a THIRD condition nobody named (`!io.WantTextInput`, because the
browser's own header carries an `InputText` search box, so `Del` while editing the query would delete the
selected asset), nothing in `tests/` can press a key, and the editor has no key-binding registry, so a third
hand-bound global would be an undeclared policy with no way to order it against the other two. **E.6.2 is the
nearest owner** because it is the task that moves editor-wide controls, but nothing in the roadmap claims it.
**Spot and point shadows** — the shadow pass is directional-only (3.6.2), Phase E's
non-goals name cascaded/soft shadows as 8.2.1's, and no roadmap item owns omni or spot shadow maps at all.
**A camera FRUSTUM gizmo** — E.2.3 draws the camera an icon and no gizmo; a natural fit for 4.7. **A MIP CHAIN
for E.2.3's icon atlas** — a 64-texel cell drawn at 22 points is a 2.91x minification, mitigated by a 4-texel
minimum feature size rather than eliminated; `Device::uploadTexture` already takes a mip level, so it is a
downsampler and three lines. **Muting the point/spot lights the bridge TRUNCATES** — safe the day `RenderView`
can report which lights survived. **A preview carrying the scene's POINT and SPOT lights** — deliberately
excluded at E.2.4, because a lamp lights a sphere at the origin by where it happens to sit, which predicts
nothing about the material. **A FOV control** — `EditorCamera::setFovYRadians` exists and the mock implies one.
**PERSISTING the four viewport toggles and the tonemap params per user** — 3.6.3's, E.1.2's and E.2.3's
handoff unchanged, since E.2.4 moved the controls and not the state; **exposure as a scene or camera property
is still unowned too**. **A `meshIndex`-aware model pick** — a picker choosing `(mesh, meshIndex)` together
needs either a reflectable "sub-asset" annotation or a component-aware row, and both are a design (likely
first customer 4.7 or E.5.x). Plus two smaller ones from E.3.3: **`AERO_ASSET(text)` and a `Text` reference**
— the vocabulary IS the draggable set, so the day a script component wants a `.json` it is ONE edit to
`assetKindIsDraggable` and three tests (`DR10`, `AR5`, `MP1`) — and **a "reveal in browser" verb** from the
picker or the row, which is E.3.2's file-open handoff seen from the other side and needs the same new
`ActionKind`. **And `FillMode::Line` / wireframe-of-meshes** (fact 3 above) remains unowned.

**E.1.1's THICK-LINE HANDOFF IS FIRED, NOT CLEARED AND NOT DEFERRED.** E.2.3's macOS pass measured it: icons
scale exactly 2x and hold 22 points, but **387 of 441 sampled runs across the gizmo are ONE DEVICE PIXEL** —
0.5 points, half their apparent weight at 1x — and **that applies to every `DebugDraw` consumer, the grid and
world axes included.** It waited eight tasks to fire because HiDPI was not a validation row on any of them.

**PHASE 3 REMAINS OPEN BEHIND PHASE E, AND PHASE E DOES NOT CLOSE IT.** What remains is its deliverable gate —
a rigged glTF/FBX in, producing PBR materials, shadows, a playing animation and **an audible sound** — plus
the validation debt above. **The audible half has never been heard on any platform.**

> **Before touching a subsystem, read its entry in `docs/10-engineering-log.md`.** That file is the full
> per-task history: what shipped, what was deliberately left out, the traps found, and the dead ends that must
> never be retried (the lavapipe LSan leak, `LD_PRELOAD`, vcpkg's `sdl3-shadercross` on macOS, …). It is
> deliberately *not* auto-loaded — grep it before re-deriving anything.

**Maintenance:** rewrite this whole section as the position moves. **Per-task history is appended to
`docs/10-engineering-log.md`, never here** — that is what grew this file to 207 k characters once and 121 k a
second time. A new task adds a row to the index, edits the phase table, and adds a rule to the standing
invariants **only if that rule can still be broken by future work**; everything else about it belongs in
`docs/10`.

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
