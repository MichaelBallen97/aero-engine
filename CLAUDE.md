# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Aero Engine — an open-source (MIT), cross-platform 3D game engine with an editor and per-project TypeScript **or** C++ scripting. Solo project, started July 2026. The goal is core-workflow parity with Unity/Godot (edit → script → play → export), explicitly **not** feature parity. 3D-first; 2D arrives in Phase 7.

Two platform matrices, never to be conflated: the **editor** runs on macOS/Windows/Linux only; the **runtime** (exported games) targets those three plus iOS and Android. The editor never runs on mobile — no touch UI, no adaptive layouts.

## Current state — read this first

**PHASE E (Editor Experience) IS OPEN — it executes between Phase 3 and Phase 4.**
**E.1.1 (Debug line renderer) is MERGED (PR #92, merge commit `15bf58b`) and E.1.2 (Grid floor +
world axes) is MERGED (PR #93, merge commit `d91eab1`, ten commits, all six CI jobs green with
`headSha == HEAD` asserted); the other 22 tasks are planning only.** Six epics, 24
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
component-count-literal sweep below both apply in full to each.

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

### E.1.2 — Grid floor + world axes (MERGED, PR #93 `d91eab1`) — and a MEASURED NEGATIVE RESULT

**The viewport has a floor.** A pure `render::emitDebugGrid` turns a camera pose into world-space
lines for a ground grid on the XZ plane, drawn through E.1.1's batch behind a `Grid` checkbox that
defaults on and is never persisted. Six commits, no new shader, no new pipeline, no dependency, no
component, no scene-format change, `docs/09` untouched. Full detail in `docs/10`; what governs new
work is below.

**A RASTERIZER DEPTH BIAS DOES NOT APPLY TO LINE PRIMITIVES, AND THIS IS THE ANSWER TO E.1.1'S
HANDOFF.** E.1.1 assigned this task "a depth-bias field on the debug pipelines" by name. It was
built, then **measured inert**: the debug `Tested` pipelines are `PrimitiveType::LineList`, and D3D12
("Bias is not applied to any point or line primitives, except for lines drawn in wireframe mode") and
Metal (bias "only influences triangle primitives") both exclude lines outright, while Vulkan permits
without guaranteeing. A sweep of 13 line depths x 5 bias magnitudes moved a line at **no** gap down to
`1e-5`; the identical sweep with a `TriangleList` billboard moved predictably and bracketed Metal's
unit at **`2^-24`** for a `D32Float` target. SDL is not dropping the state — its Metal backend calls
`setDepthBias:slopeScale:clamp:` unconditionally. **So `DebugDrawConfig` did NOT grow a bias field and
`debug_draw.{hpp,cpp}` are byte-identical after this task.** Coplanar geometry at `y = 0` passes to
**E.5.2** (the task that first creates a `Plane` there); a bias for the **billboard** pipeline, where
it demonstrably works, passes to **E.2.3**. **Never add a depth bias to a line pipeline in this tree
and expect it to do anything.**

**ADDING CONTENT TO THE SHARED BATCH REDDENS EVERY TEST THAT ASSERTS THE BATCH IS EMPTY.** `I108`,
`I109` and `I111` each counted an empty batch *exactly* — and all three went red at `2224 == 0`,
`2225 == 1`, `2229 == 5` the moment the grid existed. They were fixed with
`requestGridEnabled(false)` before their first tick, **every assertion byte-unchanged**; restating the
magnitudes would have baked a grid line count into three E.1.1 cases. `uploadCount()` is a
**lifetime** counter, so the toggle must precede the warm-up ticks. **`E.2.3` shares this batch and
hits the identical wall** against `I112`'s `lastFrameDrawCalls() == 1U` and
`lastFrameBillboards() == 0U`, which this task wrote. A shared per-frame batch makes "the batch is
empty" a claim about the whole editor, not about the subsystem under test.

**THREE FLOAT FACTS, EACH MEASURED AND EACH COUNTER-INTUITIVE.** (1) **A float-indexed lattice loop
does not terminate**: at `focus.x = 1e38` both ends are `+inf`, and at the ordinary pose
`focus = 1e6, spacing = 0.01` the quotient reaches ~1e8 where `k += 1.0F` is a no-op — and the naive
repair traps, because `inf - inf` is NaN and `std::min(NaN, 48.0F)` **returns NaN**. Use an integer
counter and clamp the span. (2) **Decade lattices NEST in float32, so coordinate divisibility cannot
identify a cadence**: `round(-23F / 0.1F) * 0.1F` is exactly `-23F` (error 3.427e-07 under the
9.537e-07 half-ulp) and `10.0` is an exact multiple of all three spacings, defeating finest-first and
coarsest-first at once. `GR24` derives the cadence from **position in the sequence**. (3) **`pow10(n)`
must be computed FRESH from `1.0F`, never as `10 * pow10(n-1)`** — a running product corrupts the
lattice in the last bits.

**AND THE PROJECTION DISAGREES WITH THE RASTERIZER BY ONE TEXEL.** Under a centred top-down camera an
axis lands on a **pixel boundary**, not a centre: Metal lights row 95 / column 127 where the
projection names 96 / 128, with exactly one lit row and one lit column. `DG18` asserts a **+/-1 bound
on where the lit run starts** — that is the width of the genuine fill-rule ambiguity, not slack, and a
single-pixel probe there is a coin flip. **A test-local comparison struct cannot carry an
`operator<<` if it is declared inside the `TEST_CASE`** ([class.friend]/6 forbids defining a friend in
a local class), and without one every texel assertion prints `{?} == {?}` on failure.

### E.1.1 — Debug line renderer (MERGED, PR #92 `15bf58b`) — the first Phase E task

**The engine can draw a world-space line, which it never could**, and **the tree now asserts
pixels**. Six commits: two additive RHI calls, a pure `render::DebugDrawBatch`, a
`render::DebugDraw` owning four pipelines from two shader pairs, four HLSL stages, the viewport slot,
and `samples/phase-E-debug-draw`. Full detail — every trap, every rejected alternative, the handoff
table — is in `docs/10`; what belongs here is what governs new work.

**The two RHI calls, and the sentence that matters about each.**
`Device::recordBufferUpload(cmd, buffer, data)` is the streaming path `device.hpp`'s D14 comment
deferred: it records on a **caller-supplied** command buffer and returns **without waiting**, it
**REPLACES THE WHOLE BUFFER** (bytes past `data.size()` are undefined, which is why it takes no
`dstOffset`), and it refuses a `cmd` with **a render pass open in every configuration** because SDL
only checks that under `debug_mode`. `Device::readbackTexture(texture, mip, out)` is **blocking**, a
test-and-tooling path, never per frame — and **its fence wait is what PERFORMS the copy on D3D12**
(`D3D12_DownloadFromTexture` caches the download; `D3D12_WaitForFences` →
`D3D12_INTERNAL_CleanCommandBuffer` does the realignment copy). Mapping before waiting reads garbage
on that backend **and only that backend**. Never "optimise" the wait away.

**THE TREE NOW ASSERTS PIXELS, on all three lanes, on every push.** `RU5` is the first; `DG5`–`DG16`
and `I108`–`I111` are the rest. **A later visual task that settles for "no backend error" is
CHOOSING to, not forced to.** The mechanism is `readbackTexture` plus a `RenderTarget` under an
identity camera, so world coordinates are NDC and a row/column is arithmetic.

**And `k * fl(1/255)` is NOT `fl(k / 255)`** — measured, not reasoned: the reciprocal form is
bit-unequal for **126 of the 256** byte values, first at k = 3, and it looks identical to six
significant digits. `unpackDebugColor` divides. Any packer/unpacker pair in this tree that wants an
exact round trip must too.

**The editor gained the slot and draws NOTHING new.** `ViewportPanel::debugDraw()` is the seam E.1.2
and E.2.3 write into; the flush sits between `sceneRenderer->render` and `post->endScene`, and
**that ordering is pinned as SOURCE TEXT (`I110`) because it is invisible at runtime** — a flush
moved after `endScene` records into a closed pass, which is a logged no-op, so `I109` stays green
while the picture silently loses every line.

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
| **Phase E** — Editor Experience | **OPEN. E.1.1 merged (PR #92 `15bf58b`) and E.1.2 merged (PR #93 `d91eab1`); 22 tasks remain, planning only.** Inserted between 3 and 4; six epics, 24 tasks in `docs/tasks/phase-E.md`. Viewport legibility (E.1), lighting & environment (E.2), inspector & context routing (E.3), project/scene/asset management (E.4), content-creation UX (E.5), shell identity (E.6). **E.1.1 is macOS-validated** — 8 of 10 rows PASS on 2026-09-03, 2 partial for structural reasons (the sample installs no billboard atlas, so `S21`'s picture half is not executable with it; Tracy's CLI exports zones but not plots). **E.1.2's page is written and NOT RUN on any platform.** Windows and Linux unvalidated, as everywhere. |
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
* **`/editor`** is **26 `.hpp`/`.cpp` pairs**; `/tools` links `aero::assets` and `aero::editor_core`
  through `aero_cooker`, which is legal because `tools/` is enumerated by neither half of the golden
  rule.

### Standing invariants that govern new work

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

**THE DEBUG BATCH IS SHARED, SO "THE BATCH IS EMPTY" IS A WHOLE-EDITOR CLAIM.** Any task that adds a
producer to `render::DebugDraw` reddens every case that counts the batch exactly — E.1.2's grid took
`I108`/`I109`/`I111` red at `2224 == 0`, `2225 == 1`, `2229 == 5`. **Fix it with the producer's own
toggle seam, never by restating the magnitude**, and remember `uploadCount()` is a *lifetime* counter
so the toggle must precede the warm-up ticks. **E.2.3 hits this against `I112` next.**

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

At E.1.2's gate: **`ctest -N` 172** (tools-ON, measured on both presets; the two reduced
configurations' 80 / 93 remain 3.7.3's numbers and were re-measured at neither E.1.1 nor E.1.2);
doctest across **seven** binaries **1250 / 1748 / 149 / 34 / 29 / 7 / 28**. **E.1.2 repeats E.1.1's
signature and it is the INVERSE of 3.7.3's: the doctest totals MOVE while `ctest -N` does NOT** —
`aero_tests` 1223 → 1250 (`GR1`–`GR26` + `DG18`), `aero_editor_shell_test` 1746 → 1748 (`AX1`, `AX2`)
and `aero_editor_imgui_test` 147 → 149 (`I112`, `I113`; `I110` gained a subcase, which moves no
total), because new TUs ride existing binaries and a sample registers no ctest entry. The eight guard
counts at that gate: math **461**, platform **85**, rhi **149**, scene **85**, golden-rule **151**,
project-no-delete **A=6/B=75**, audio **11/3/55**, probes **6/57**. **`check-math-boundary.sh` counts
`git ls-files`, so it reads a STALE number until new files are `git add`ed** — stage first, then
measure. A moved `ctest -N` on a task like that means a CMakeLists copied
from the wrong template. Both reduced configurations
must be configured **FRESH with `-G Ninja`** — and, since 3.7.3, **with
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

**macOS is otherwise green**, so this is what is left. **No Windows or Linux validation pass exists for
any task in any phase**: Phase 0's gate, Phase 1's render rows, all thirteen Phase 2 tasks, and every
Phase 3 task. Separately, **seven ticked rows across four tasks were signed off with their measurement
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

**3.7.3 adds NO validation page and that is the deliberate 2.1.2 precedent** — 2.1.2 and 2.5.2, the
two page-less tasks, are both pure test/guard infrastructure, and nothing in a guard task needs ears,
eyes, hardware or an OS-specific behaviour a row could measure. Its standing evidence is the hermetic
e2e pair, which runs on the macOS and Linux lanes on every push, plus the one-time seed and meta-proof
log in `docs/10`. **Its own residual is Windows**: both e2e cases are `NOT WIN32`, and the lint job
that runs the two scripts is ubuntu-only, so nothing about the guards' behaviour is exercised under an
MSYS userland at all. Coverage of the *invariant* is unaffected — the guards run on every push.

**E.1.1 adds a full page and it is NOT RUN on any platform.**
`editor/validation/E.1.1-debug-line-renderer.md` is written (gitignored, so it enters no commit) with
eleven numbered steps, seven of them carrying measurement blanks. Its shape is unlike every prior
render task's: **rows 1–3 are already automated on all three lanes** by `DG5`–`DG10` and `DG16`,
because the tree now reads pixels back. What stays hardware-only is legibility on a HiDPI display
(a 1-pixel line has no width control on any backend), the editor's picture being unchanged against a
build at the branch point, the cost A/B, Tracy's zone and plots, and the declared seeds — **`S11`
(depth write on the Tested pipelines) and `S21`'s picture half (a mirrored atlas cell whose centre
still reads the right colour) have their ONLY coverage anywhere in that page's row 6.**

**E.1.2 adds a full page and it is NOT RUN on any platform.**
`editor/validation/E.1.2-grid-floor-world-axes.md` — twelve numbered steps, nine measurement blanks,
gitignored. **Rows 1, 3 and 5 are partly automated** by `GR7`, `GR17`, `DG18` and `I112`; what stays
hardware-only is HiDPI legibility (still one-pixel lines — E.1.1's thick-line handoff and this is its
stated trigger), whether the crossfade *paces* well with a linear fraction, the cost A/B in Tracy, the
picture being unchanged against a branch-point build, and that the grid axes agree with ImGuizmo's
handles. **Row 6 is deliberately a MEASUREMENT, not a pass/fail**: no `Plane` primitive exists until
E.5.2, and a shimmer where geometry is coplanar with the grid is **expected**, because debug lines
carry no depth bias and cannot. **The sample binary is `aero_sample_phaseE_debug_draw`** — `phaseE`,
no underscore before the E.

### Next

**Phase E is the open front. E.1.2 is merged and its validation page is written and UNRUN on every
platform** — the first Phase E task whose CI passed on all three lanes from the first push, Windows
and Linux included. The epic's remaining tasks are **E.1.3**
(view-axis gizmo — it owns "which way is up", which E.1.2 deliberately does not answer), **E.1.4**
and **E.1.5** (the gizmo restyle, which adopts `AXIS_{X,Y,Z}` from the palette E.1.2 created).

**The spine, unchanged except where E.1.2 moved it:** **E.2.1 (`Environment`) blocks E.2.2, E.2.4 and
E.4.5**; **E.2.3 (light gizmos) is unblocked** — and it now inherits two things by name, the
**shared-batch empty-assertion wall** against `I112`, and a **billboard-pipeline depth bias**, which
is the only topology where a bias actually works. **E.3.1** adopts the axis palette on its `Vec3`
rows. **E.5.1 is an S-sized fix for a confirmed defect and is independent of everything**, so it can
land at any point. **E.5.2 now owns the coplanar-geometry problem** — it creates the first `Plane` at
`y = 0`, and E.1.2 established that a rasterizer bias cannot be the answer for lines.

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
