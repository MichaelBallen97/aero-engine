# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Aero Engine — an open-source (MIT), cross-platform 3D game engine with an editor and per-project TypeScript **or** C++ scripting. Solo project, started July 2026. The goal is core-workflow parity with Unity/Godot (edit → script → play → export), explicitly **not** feature parity. 3D-first; 2D arrives in Phase 7.

Two platform matrices, never to be conflated: the **editor** runs on macOS/Windows/Linux only; the **runtime** (exported games) targets those three plus iOS and Android. The editor never runs on mobile — no touch UI, no adaptive layouts.

## Current state — read this first

**PHASE E (Editor Experience) IS OPEN — it executes between Phase 3 and Phase 4.**
**EPIC E.1 (Viewport legibility) IS CLOSED IN CODE — all five tasks merged.** E.1.1 (Debug line
renderer, PR #92 `15bf58b`), E.1.2 (Grid floor + world axes, PR #93 `d91eab1`), E.1.3 (View-axis
gizmo, PR #94 `6fb323c`, plus the follow-up PR #95 `0ab204d`) and E.1.4 (Silhouette selection
outline, PR #96 `3aadffb`) are all merged **and macOS-validated**; **E.1.5 (Transform-gizmo restyle)
is merged** — five commits, the full local gate green on both presets and both reduced
configurations. **The other 19 tasks are planning only.** Six epics, 24
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
AssetDatabase still resolves GUIDs against the open one. **`SpotLight` (E.2.2) and `Environment`
(E.2.1) take the built-in component count from 8 to 10** — the five-generation-site rule and the
component-count-literal sweep below both apply in full to each. **E.1.3 answered (4) for its own
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

### E.1.5 — Transform-gizmo restyle (MERGED) — Epic E.1 closes in code

**ImGuizmo stops being at its factory defaults.** A pure value model in a new **public** editor
header (`gizmo_style.hpp`) becomes an `ImGuizmo::Style` in the one TU that names ImGuizmo, written to
the library's **process-wide global once per submitted frame** together with a screen size resolved
from that frame's viewport extent. Five commits, 3 new files / 4 edited source or header files, no
new dependency, no shader, no pipeline, no RHI call, no component, no scene-format change, no
`add_test`. **`ctest -N` unmoved at 172**; `aero_editor_shell_test` **1781 → 1793**,
`aero_editor_imgui_test` **159 → 162**, the other five unmoved. **Six commits after the code-review
round, whose one blocking finding is invariant 0 below.** Full detail in `docs/10`; the
sentences that govern new work are below.

**0. A TABLE INDEXED BY BOTH THE WRITER AND THE READER CANCELS, AND A ROUND TRIP THROUGH IT ASSERTS
NOTHING.** The review round's blocking finding, and the one worth carrying out of this task.
`applyGizmoStyle` wrote `Colors[IMGUIZMO_COLOR_SLOT[i]]` and `imGuizmoStyleReadback` read
`Colors[IMGUIZMO_COLOR_SLOT[i]]`, so the composition was the identity for **any injective permutation
of the table** — and `I124` passed **80 of 80 assertions** with `DIRECTION_X` and `DIRECTION_Z`
swapped, while the X arrow rendered blue. MEASURED, before the fix. It is E.1.2's "both sides from one
source" species wearing a **real round trip through ImGui's own packer** as a disguise, which is why
both a plan review and a first code reading passed over it. **The fix is a `static_assert` that the
table is the IDENTITY** — a compile error beats a red test, and it additionally catches an upstream
`ImGuizmo::COLOR` reorder that leaves `COUNT` unchanged, which **no runtime tier could ever see**
because the read-back would follow the reorder too. **Any future write/read pair that shares one
mapping table inherits this**: assert the mapping, do not test through it.

**1. IMGUIZMO'S TWO VISIBILITY SETTERS ARE CROSSED, AND THE HEADER'S OWN COMMENTS SAY THE OPPOSITE.**
`SetPlaneLimit` feeds `mPlaneLimit`, which `ImGuizmo.cpp:1230` compares against the **AXIS's**
projected clip LENGTH; `SetAxisLimit` feeds `mAxisLimit`, which `:1229` compares against the
**PLANE's** projected clip AREA. The setters are at `:2657-2660` and `:2667-2670`; the header's
comments (`ImGuizmo.h:268`, `:272`) describe the **names**, not the code, and the `below…` flags are
additionally inverted in sense (`true` means *visible*). **The crossing is spelled in exactly ONE
place** — `viewport_panel.cpp`'s `applyGizmoStyle`, with the citation beside it — and the pure model
names both thresholds by MEANING (`axisHideClipLength`, `planeHideClipArea`) so nothing else has to
know. **No runtime tier can read either value; there is no getter.** `I125(c)` pins each line to the
member it must name, and **a port bump that
un-crosses them leaves `I125(c)` green. RE-READ `:1229-1230` AT EVERY PORT BUMP** — but it does NOT
invert the picture silently, and that correction is worth more than the warning was.
**THE OBSERVABLE IS BINARY, NOT AN ORDERING.** `GetParallelogram` returns the parallelogram of the
two FULL axis vectors, so a plane's face-on area is `size²` while `axisHideClipLength` is
`0.2 · size`. Swapped, a plane's hide threshold (`0.2 · size`) EXCEEDS its own maximum possible area
(`size²`) for every `size < 0.2` — which is every viewport this resolver produces — so **the plane
quads can never be drawn AT ALL.** MEASURED in the product at E.1.5's validation: correct wiring
44 / 109 fill px for `PLANE_X` / `PLANE_Z`, swapped **0 / 0**, reverted 44 / 109 exactly, with every
axis arrow unchanged. **Check it in one screenshot on the default startup pose — no oblique rotation,
no angle measurement** — counting the alpha-115 blends `(140,67,75)` and `(63,98,144)`.

**2. THE GIZMO STYLE IS A PROCESS-WIDE GLOBAL WRITTEN EVERY SUBMITTED FRAME, AND THAT WRITE IS
SELF-HEALING.** `ImGuizmo::GetStyle()` returns `gContext.mStyle` by reference and `BeginFrame()` does
not touch it. Whatever a future writer does to the global — E.6.1's theme is the obvious one — the
viewport restores its own style on its next frame, **which also means a task that wants a different
gizmo style must change `defaultGizmoStyle()`, never write the global from a second site.**
`I125(e)` asserts `ImGuizmo::GetStyle()` appears **exactly twice** in `viewport_panel.cpp`; a third
is an undeclared second writer. It also runs whether or not the gizmo is *enabled*: `Enable(false)`
skips only the `Handle*` block, so the greyed handles during a camera gesture or over the corner
widget are drawn with **this** style's thicknesses and `GIZMO_INACTIVE_SRGB`.

**3. `mGizmoSizeClipSpace` IS 5 % OF THE *LARGER* VIEWPORT DIMENSION, AND THE AXES NO LONGER FLIP.**
One screen point is `2 / max(w, h)` of that unit in **both** orientations —
`GetSegmentLengthClipSpace`'s two arms collapse to it — so `resolveGizmoScreenSize` inverts it to a
**points-constant 90 with a knee at a 600-point smaller dimension**, and derives both hide thresholds
from the size using the library's own ratios. `AllowAxisFlip(false)` every frame makes
`DrawHatchedAxis` **unreachable by construction**, so `GIZMO_HATCHED_AXIS_SRGB` and
`GIZMO_HATCHED_AXIS_THICKNESS_POINTS` are **dead slots no tier and no validation row can observe** —
a task that re-enables flip must pick a thickness deliberately rather than inherit a stale zero.
**A portrait wrinkle is recorded and deliberately NOT fixed**: `GetParallelogram` divides `y` by
`mDisplayRatio` unconditionally while `GetSegmentLengthClipSpace` branches on it, so planes hide at a
different relative angle in portrait. That is the library's behaviour under its own defaults too.

**4. THE GIZMO'S COLOURS ARE DERIVED FROM `axis_palette.hpp`, NEVER RESTATED**, so the gizmo, the
grid and the corner widget are the same three bytes — and the hot handle is an **opaque** yellow
`(255, 232, 64)` chosen to stand off E.1.4's amber `(255, 176, 64)` by a **tested** 32-per-channel
floor (measured gap: 56). `viewport_panel.cpp` states no gizmo colour literal at all. **A restated
`IM_COL32` literal one byte off is invisible to every automated tier** — E.1.4's sabotage row 20,
unchanged — which is why the derivation is structural and why validation row 1 reads three consumers
off one screenshot.

**AND FOUR EPSILONS WERE MEASURED RATHER THAN CHOSEN.** `resolveGizmoScreenSize({1800, 900})` is
**bit-exactly `0.1F`**; both hide thresholds land **1 ulp** from the library's constants there;
`GS4`'s inverse property is worst **1 ulp over 300 000** seeded viewports; and **`GS11`'s bound is
tight with equality at a square viewport**, so it is written `2.0F * GIZMO_AXIS_MAX_VIEWPORT_FRACTION`
and **never as `0.3F`**. The seeded sweeps derive their floats arithmetically from `std::mt19937`'s
raw output — **`std::uniform_real_distribution` is not portable across standard libraries**, so
otherwise the three lanes sample different viewports.

**TWO `static_assert`s, NEITHER SUFFICIENT ALONE, AND A THIRD CASE NEITHER SEES.** The header's
catches an enumerator added there and forgotten at the apply site; the apply site's, naming
`ImGuizmo::COLOR::COUNT`, catches one added upstream. **A port bump that adds a `Style` FLOAT is seen
by neither** — the new field keeps the library's default and `I124` still passes. Documented, not
covered.

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
| **Phase E** — Editor Experience | **OPEN. EPIC E.1 (Viewport legibility) IS CLOSED IN CODE — all five tasks merged**: E.1.1 (PR #92 `15bf58b`), E.1.2 (PR #93 `d91eab1`), E.1.3 (PR #94 `6fb323c`, + PR #95 `0ab204d`), E.1.4 (PR #96 `3aadffb`) and **E.1.5**. **19 tasks remain, planning only.** Inserted between 3 and 4; six epics, 24 tasks in `docs/tasks/phase-E.md`. Viewport legibility (E.1), lighting & environment (E.2), inspector & context routing (E.3), project/scene/asset management (E.4), content-creation UX (E.5), shell identity (E.6). **E.1.1 is macOS-validated** — 8 of 10 rows PASS on 2026-09-03, 2 partial for structural reasons (the sample installs no billboard atlas, so `S21`'s picture half is not executable with it; Tracy's CLI exports zones but not plots). **E.1.2 is macOS-validated** — 8 PASS / 2 PARTIAL / 1 NOT EXECUTABLE on 2026-09-04. **E.1.3 is macOS-validated** — 11 PASS / 1 PARTIAL / 2 NOT EXECUTABLE / 1 NOT RUN on 2026-09-05; the pass found the ortho gizmo-suppression defect, fixed in PR #94's follow-up (PR #95, `0ab204d`). **E.1.4 is macOS-validated** — 10 PASS / 1 NOT EXECUTABLE on 2026-09-05, with two of the ten carrying a stated shortfall (row 5's mid-import transition not observed; row 6's 256-entity figure extrapolated from eleven, not measured). **E.1.5 is macOS-validated** — 6 PASS / 3 PARTIAL / 2 NOT RUN / 1 NOT EXECUTABLE on 2026-09-05. Confirmed on screen: the gizmo and the corner widget carry the palette bytes EXACTLY, a screen-parallel axis measures 99 px across three dolly distances (target 90 pt + the 10-pt head), the knee is live at both ends (45 pt in an 835x300 dock, 90 pt in 1655x848), the axes REVERSE between opposite views with no hatched stub, the plane quad is 28x27 px (`0.3 x L`), and one drag is one undo restoring the picture to **0 differing pixels**. **Seed 6 gained a product-level cover the plan did not anticipate** — see `docs/10`. Open: row 5's hide ORDER, row 9's RMB arm, rows 10 and 12 (the latter blocked because a freshly built branch-point binary gets no window from macOS), row 11's pixel control, and row 7 (NOT EXECUTABLE on 1x). Windows and Linux unvalidated, as everywhere. |
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
* **`/editor`** is **28 `.hpp`/`.cpp` pairs** since E.1.5's `gizmo_style` (E.1.4 added none); `/tools` links `aero::assets` and `aero::editor_core`
  through `aero_cooker`, which is legal because `tools/` is enumerated by neither half of the golden
  rule.

### Standing invariants that govern new work

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
consumer) and, when it wants one, E.2.3's billboard icons.

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

**THE DEBUG BATCH IS SHARED, SO "THE BATCH IS EMPTY" IS A WHOLE-EDITOR CLAIM.** Any task that adds a
producer to `render::DebugDraw` reddens every case that counts the batch exactly — E.1.2's grid took
`I108`/`I109`/`I111` red at `2224 == 0`, `2225 == 1`, `2229 == 5`. **Fix it with the producer's own
toggle seam, never by restating the magnitude**, and remember `uploadCount()` is a *lifetime* counter
so the toggle must precede the warm-up ticks. **E.2.3 hits this against `I112` next** — E.1.3, E.1.4
and E.1.5 each added no `DebugDraw` producer, so the wall is untouched for the third consecutive task.

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
far worse, green and wrong. **There are eight built-ins**: `Transform`, `Camera`, `DirectionalLight`,
`PointLight`, `MeshRenderer`, `AnimationPlayer`, **`AudioSource`, `AudioListener`**.

**A COMPONENT-COUNT LITERAL IS NOT CONFINED TO THE TESTS THAT ARE ABOUT COMPONENTS.** 3.7.2 found them
in `tests/scene_test.cpp`, `tests/transform_test.cpp`, `tests/editor/hierarchy_test.cpp`,
`tests/editor/inspector_test.cpp` and `tests/scene_serialize_test.cpp` — **every one found by a red
test, none by a grep.** Adding a built-in means finding all of them.

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

At E.1.5's gate, measured on both presets: **`ctest -N` 172, UNMOVED**; doctest across **seven**
binaries **1294 / 1793 / 162 / 34 / 29 / 7 / 28** (`aero_tests`, `aero_editor_shell_test`,
`aero_editor_imgui_test`, `aero_scene_serialize_test`, `aero_editor_inspector_test`,
`aero_reflect_meta_test`, `aero_reflect_json_test`). **E.1.5 repeats E.1.1's, E.1.2's, E.1.3's and
E.1.4's signature and it is the INVERSE of 3.7.3's: the doctest totals MOVE while `ctest -N` does
NOT** — `aero_editor_shell_test` 1781 → 1793 (+12, `GS1`–`GS12`) and `aero_editor_imgui_test`
159 → 162 (+3, `I124`–`I126`), both MEASUREMENTS off the `filters:` line, and exactly the kind of
delta that must never be predicted arithmetically because `SUBCASE` structure makes the sum a guess.

**AND A RECORDED TOTAL GOES STALE SILENTLY: `origin/main`'s own shell total was ONE stale at E.1.4's
gate** — it said `1780`, measured at PR #94's gate, and PR #95 then added `G19` while nobody
re-measured. Read the binary, never the block.

The eight guard counts at E.1.5's gate: math **472**, platform **86**, rhi **152**, scene **86**,
golden-rule **154**, project-no-delete **A=6/B=77**, audio **11/3/55**, probes **6/57**. **The two
reduced configurations read `159` (shader-tools-OFF) and `93` (reflect-tools-OFF)**, unmoved, and
3.7.3's remembered `80 / 93` is HALF WRONG. Compare the entry **SETS**, not the totals — measured at
E.1.5's gate by comparing entry NAMES with the ctest numbering stripped, because a raw `diff` of
`ctest -N` output is dominated by the renumbering and shows every later entry as changed:
shader-tools-OFF removes exactly the **13 `shaderc.*`** entries; reflect-tools-OFF removes exactly the
**75 `reflect-gen.*`** entries **plus four doctest binaries** (79 removals in total — the older
wording said "79 `reflect-gen.*`", which counted the total as the prefix); **nothing is added in
either**; and all **70 `cooker.*` entries are present in all three**, which is the property that check
is actually about.
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

**macOS is otherwise green EXCEPT FOR E.1.5**, whose page has not been run on any platform, so this
is what is left. **No Windows or Linux validation pass exists for
any task in any phase**: Phase 0's gate, Phase 1's render rows, all thirteen Phase 2 tasks, and every
Phase 3 task.

**E.1.5's PAGE IS NOT YET RUN, AND IT IS THE ONLY JUDGEMENT ITS MAGNITUDES HAVE.** Every value the
task writes is a *tuning constant*, and `gizmo.hpp:66-67`'s rule forbids a tier-0 case from pinning
one — so `GS1`–`GS12` and `I124`–`I126` assert **relationships** (a derivation, a gap floor, an
inverse property, an ordering) and **nothing at all** about whether 90 points is the right length,
whether a 20 x 20-point triangle reads as a cone, whether the opaque yellow stands off the palette's
green on a real display, or whether the plane-hide angle feels right. Four things are additionally
unreadable by ANY tier because ImGuizmo exposes no getter for them: the gizmo size, the flip flag and
the two hide thresholds. **Row 7 (HiDPI) will be NOT EXECUTABLE on 1x hardware** — record it as such
rather than skipping it, the way E.1.2's row 4, E.1.3's row 1 and E.1.4's row 4 all were, because the
distinction between "unfired" and "cleared" is the one this project keeps making. **The sabotage
matrix has not been run either**; its two declared holes are already known — `GIZMO_HATCHED_AXIS_SRGB`
and `GIZMO_HATCHED_AXIS_THICKNESS_POINTS` are unobservable by anything at all while flip is off, and
the apply site's `static_assert` has no witness until a port bump. Separately, **seven ticked rows across four tasks were signed off with their measurement
blanks empty** (3.2.5 rows 3, 8, 9, 11, 13; 3.2.2 row 9; 3.2.4 row 12) — the behaviour passed, the
evidence is absent. Every page from Epic 3.3 onward is written so a blank tick is impossible, which is
the pattern the older four should be brought up to.

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

**Phase E is the open front, and EPIC E.1 IS CLOSED IN CODE — all five tasks merged.** E.1.5
(transform-gizmo restyle) is the last of them: five commits, the full local gate green on both
presets and both reduced configurations, `ctest -N` unmoved at 172 and the two editor doctest totals
moved by their own measured deltas. **Its validation page has not been run on any platform, and that
is where its whole remaining risk sits** — every value it writes is a tuning constant no tier-0 case
may pin, and four of ImGuizmo's knobs have no getter at all. E.1.1, E.1.2, E.1.3 and E.1.4 are all
merged AND macOS-validated; **three records on E.1.3's page and three shortfalls on E.1.4's remain
open** (E.1.4: row 4 needs a 2x display, row 5's mid-import transition was never observable, row 6's
256-entity figure is extrapolated, plus four recorded sabotage holes). **Windows and Linux validation
remain outstanding**, as everywhere.

**The spine, unchanged except where E.1.2, E.1.3, E.1.4 and E.1.5 moved it:** **E.2.1
(`Environment`) blocks E.2.2, E.2.4 and E.4.5**; **E.2.3 (light gizmos) is unblocked** — and it
inherits two things by name, the **shared-batch empty-assertion wall** against `I112` (E.1.3, E.1.4
and E.1.5 each added no `DebugDraw` producer, so that wall is untouched for the third consecutive
task), and a **billboard-pipeline depth bias**, which is the only topology where a bias actually
works. **E.3.1** inherits the palette's KEY (`Axis`, `AXIS_COUNT`, `axisColorSrgbBytes`) for its
`Vec3`/`Quat` rows, unchanged. **E.6.1 now inherits `gizmo_style.hpp`'s constants as well as the
palette trio, and owns the DPI story E.1.5 deferred**: nothing in the viewport reads
`SDL_GetWindowDisplayScale`, so every points-authored viewport constant is correct on macOS and
`1/scale` smaller relative to the scaled UI on a Windows or Linux desktop above 100 %. **E.5.1 is an
S-sized fix for a confirmed defect and is independent of everything**, so it can land at any point —
and E.1.4 deliberately reproduced that defect rather than fixing it in passing, because a mask that
disagreed with the picture is the one direction INV-1 forbids. **E.5.2 owns the coplanar-geometry
problem** — it creates the first `Plane` at `y = 0`, and E.1.2 established that a rasterizer bias
cannot be the answer for lines. **E.2.x inherits ortho specular**: `CameraView::eyePosition` is wrong
under a parallel projection, as it is in Unity. **8.2.1 inherits the alpha-tested mask cutout** from
E.1.4's D6, on the same terms it already inherits the alpha-tested shadow caster: the mask stage has
no UVs and cannot discard, so a `MaterialAlpha::Mask` instance outlines as a solid quad and latches
one WARN. **A first-party gizmo cone overlay is recorded as its own task**, triggered only by E.1.5's
validation row 2, never as a follow-up commit.

**Phase 3 remains OPEN behind it, on its gate and its validation debt, and Phase E does not close
either.** 3.7.1 (PR #88 `4892e65`) and 3.7.2 (PR #89 `b398d17`) are merged and macOS-validated;
3.7.3 is merged (PR #91 `0530cff`), its six CI jobs green on the merged SHA. What remains for that
phase is **its deliverable gate** — a rigged glTF/FBX in, producing PBR materials, shadows, a playing
animation and **an audible sound** — and **the validation debt above**. The audible half has never
been heard on any platform: 3.7.2's macOS pass ticked 47 of 53 records and left open exactly the 6
that need ears or the editor. **Windows and Linux rows are outstanding for every task in every
phase, and 3.7.2's Linux row matters more than most**: LSan runs on that lane alone, so the macOS
row-11 zero proves the teardown is *clean*, not that a leak is *absent*, and unlike 3.7.1 everything
that task allocates is first-party. See `docs/tasks/phase-3.md` and `docs/tasks/phase-E.md`.

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
