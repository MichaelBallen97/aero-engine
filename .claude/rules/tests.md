---
paths:
  - "tests/**"
---

# Writing, measuring and sabotage-testing tests

Each rule here was earned by a defect, a green-and-wrong test or a measurement that contradicted a plan, and every one binds new work. The story behind each is in `docs/10-engineering-log.md`; grep it for the citation.

## Case ids

**Next free ids, measured by a whole-`tests/` token sweep on E.6.4's tree: `I361` at the ImGui tier** (4.3.1 took
`I361`–`I366`, so the next is **`I367`**; 4.3.1's new prefixes are `EB` (next `EB30`) and `ST` (next `ST43`)),
`AA68`, `AD77`, `AH21`, `AM29`, `AV68`, `BD8`, `BR31`, `BV10`, `CC15`, `CD15`, `CR13`, `CS11`, `DD31`, `DG26`, `DR28`,
`EC5`, `EK11`, `EP19`, `FT16`, `G29`, `GL8`, `GR34`, `HK6`, `HY9`, `IO24`, `IR23`, `IX19`, `MB27`, `NP14`, `P96`,
`PC15`, `PX6`, `RF8`, `RT31`, `SQ15`, `SS64`, `TB13`, `TH19`, `TS11`, `US13`, `VM65`, `VO15`, `WL7`, `X32`, plus 4.1.2's
`ML80` and `VN5` (4.1.1 added `VM` and 4.1.2 `ML`, `VN` and `VM61`–`VM64`, so every other id is still the E.6.4
sweep's). The list does not name every taken prefix: `MR` (E.3.4), `PJ` (E.4.1), `CN` (E.4.2) and `MC` (mesh
cook, which is why the card cache took `CC`) are taken too, so a NEW prefix is checked against a fresh sweep,
never against this list. Which ids each task took is recorded in its `docs/10` entry.
**E.6.1 DELETED `I230`** with the ProggyClean remap table it pinned — the id is retired, never reused.
**`I193`–`I209`, `I259`–`I269` and `I271`–`I274` are FREE and were never claimed by anything that merged** (the
last two were reserved by the fix plans and left unused) — a
gap costs nothing, and renumbering a block into one invites exactly the silent collision this project has
recorded.


## Tests that reach the esbuild binary

**Every case that can reach the esbuild path sets it explicitly (4.3.1).** The path comes from `AERO_TEST_ESBUILD`, the
binary `cmake/esbuild.cmake` cached, never the copy beside `aero_editor`, and goes into `ScriptTranspilerConfig` or
`EditorAppConfig::esbuildPath`. Resolution is lazy and starts at the first scan that finds a `.ts`, so a case without a
script never reads `AERO_ESBUILD_PATH`; a case with one and no path would run whatever binary the local machine holds.
Pump the transpiler to idle before a `TempDir` goes out of scope: Windows cannot remove a log the child still holds.

## Source-text pins and seeds

**AND A SOURCE-TEXT TOKEN SCAN IS BLIND TO INTEGER-LITERAL SUFFIXES (E.3.1).** The pin guarding "the panel
restates no axis colour" missed `226U` — digits followed by an identifier character — which is exactly how
`editor_theme.hpp` spells the axis bytes (`axis_palette.hpp`'s names are aliases into `EDITOR_THEME` since E.6.1).

**`git checkout -- <file>` REVERTS TO HEAD**, so seeding a file to prove a fix **also reverts the fix if it is
not committed**. It ate two closures during 3.7.2. **Commit the fix, then seed it.**


## Test method

**ASSERT THE EFFECT, NEVER THE INTENTION** — the single most repeated failure in this project's review rounds.

**AN ANTI-VACUITY ARM MUST NOT DEPEND ON THE MACHINE'S GEOMETRY (#111).** `I231`'s check that the Issues body
grew past one row read `13 > 13` on CI's macOS runner, whose panel was short enough that the layout
**correctly** held the body at its floor. The check measured the display. Compare a layout claim against the
layout's own answer for the metrics the panel **recorded** (`assetBrowserLayoutMetrics()`), and prove the
measurement with the measured value itself. A check that depends on geometry can be a `WARN` at most.

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

**A PIN OVER THE HOSTS A PLAN NAMES CANNOT SEE A HOST IT DID NOT NAME (E.6.1's validation pass).** The Create
menu's icon pins covered the two Hierarchy menus the plan listed, and the menu bar's `Create ▸ Empty` — a third
host, a plain `MenuItem` in `shell_ui.cpp` — drew no icon with every test green. **Find the hosts by sweeping
for the BEHAVIOUR's call (`grep` for every `"Empty"` menu item), never by reading the plan's list**; `I285` now
pins the third.

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
**And a pattern can never contain a comma** — doctest splits on it, and a stray lone `*` selects the whole
binary and still exits 0 — so select an existing `(ID, …)` case with `?` (`*(AM7?*`) and end every new case
name with its id and `)` (E.4.4). **A run whose summary "skips" most of its cases may be a crashed binary**:
`S1b` printed `1913 skipped` after a SIGABRT — read the log for `CRASHED`.

**`static_assert(!requires(T v) { v.member; })` ON A NON-DEPENDENT TYPE IS A HARD COMPILE ERROR**, not
`false` — a requires-expression only substitutes when the type is dependent. Route such a check through a
file-local concept and carry a **positive control**, without which a mis-spelled detector makes the negative
assertion vacuously true for every type in the language (`HE17`'s shape).

**CI'S macOS RUNNER FITS FOUR MEDIUM TILE COLUMNS IN THE ASSETS GRID, SO A GPU CASE THAT NEEDS SEVERAL TILES
IN ONE ROW DRAWS THEM SMALL (E.4.5).** Run `36238862338` failed `I233` alone: the runner's contents width was
at least 362 and under 454.5, so the fifth Medium tile wrapped to a second row its short panel never showed
and was never drawn — `d - decodes == 2U` read 1, twice. A Small tile was 58.5 wide and five needed 324.5;
`EditorApp::requestAssetBrowserTileSize` is the seam, and `I233` and `I243` use it. A case that needs tiles
drawn keeps its row short and ends in a loud final count, so a still-narrower runner fails with a clear value,
never vacuously. **Tile widths are font multiples, so E.6.1's body 16 made five Small tiles need 392 dp against
roughly 324 on the runner** — `I233` now also hides the side panels (green at width 985 with the hide, red
without it). **Re-derive every tile-count fixture at a font change.**


## Sabotage method

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


## Committed fixtures and artifacts

Two images at `tests/fixtures/assets/`; six 32×32 PNGs and six `.ktx2` under
`samples/phase-3-materials/textures/`; seven `.aeromat` at `tests/fixtures/materials/`;
`samples/phase-3-skinning/arm.aero{mesh,skel}`; `samples/phase-3-animation/wave.aero{mesh,skel,anim}`;
`tests/fixtures/audio/` (four encodings of one 1.0 s signal, `tone.aerowave`, the corrupt
`tone-lying-length.ogg`, and **`tone.s16le.pcm` — ffmpeg's own decode, the external anchor that ties dr_wav
and dr_flac to libavcodec byte for byte**); and, from 3.7.2, `samples/phase-3-audio/{orbit,beacon}.aerowave` —
mono 48 kHz 0.5 s, **exactly 48 064 B each**, cut at a whole number of cycles so the loop seam is continuous
*by arithmetic*. **Neither audio sample fixture enters the determinism manifest**, and the README says so.


## Measuring the test inventory

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
`reflect-gen.*` entries **plus four doctest binaries and, since 4.2.2, `typescript.clean` and `typescript.broken`** (the
type-check cases that read the generated declarations; `typescript.prelude_only` and `typescript.driver_e2e` stay);
**nothing is added in either**; and all **70
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

**`I136` IS NO LONGER DISPLAY-DEPENDENT (E.6.1, AC-14).** It failed `REQUIRE(drawExtent.width > 4U)` with value
**4** on a 2x display from E.3.4 to E.4.4's branch point, because the style was doubled on Retina; with one UI
scale (1 on a Retina Mac) the whole GPU tier, `I136` included, passed with the built-in Retina panel attached
at E.6.1's step 5, and on 1× externals at every later gate. **The local GPU tier gates whole whatever is
attached** (E.6.3's full gate at `88a2b8f` ran on the built-in Retina panel alone: 351 of 351 imgui cases) —
still name the display configuration in a gate record, because a 2× run is the only local witness that the UI
scale stays 1 on Retina.

**COUNTS DIVERGE BY OS, so never assume one.** Windows skips **FIVE** e2e cases —
`golden-rule.include_scan_e2e`, **`project-no-delete.no_delete_e2e`**, `audio-boundary.guard_e2e`,
`boundary-probes.probe_links_e2e` and 4.1.1's `script-boundary.guard_e2e`, gated at `tests/CMakeLists.txt:1128, 1160, 1184, 1205, 1267`, all
`NOT WIN32` by the same D16 reasoning (the lint job that runs the scripts is ubuntu-only, and the BSD-userland
proof comes from the macOS lane) — plus fourteen whole `BS` cases, one arm of `BS11` and one GPU case
(`I80`). **The no-delete entry is the one every earlier count omitted; it was measured at E.4.3, and the
figure was "three" here for four tasks.** **Those 3.2.4 skips are not a random sample**: together they are the only coverage anywhere of both
Blender timeouts, cancellation, the `Converted` state, `ok: false`, both `ArtifactUnusable` arms and the
refused-by-cap log.

**THE `typescript.*` CASES NEED NODE AND A PINNED TYPESCRIPT (4.2.2).** `tests/typescript/` holds a `package.json` and
lockfile for TypeScript 5.9.3, installed by `npm ci` inside that folder and never at configure. Without node or the
install the cases skip loudly; `AERO_REQUIRE_TSC=1` makes the skip a failure, and all three CI lanes set it. Run
ctest with it set locally, as with `AERO_REQUIRE_GPU=1`.
