# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Aero Engine — an open-source (MIT), cross-platform 3D game engine with an editor and per-project TypeScript **or** C++ scripting. Solo project, started July 2026. The goal is core-workflow parity with Unity/Godot (edit → script → play → export), explicitly **not** feature parity. 3D-first; 2D arrives in Phase 7.

Two platform matrices, never to be conflated: the **editor** runs on macOS/Windows/Linux only; the **runtime** (exported games) targets those three plus iOS and Android. The editor never runs on mobile — no touch UI, no adaptive layouts.

## Current state — read this first

**Phase 3 (Asset Pipeline & 3D Content) is OPEN, and ALL SEVEN of its epics are now CLOSED IN CODE.**
Epic 3.7 (Audio playback v0 · audio) closes with 3.7.1 MERGED (PR #88, `4892e65`, macOS-validated
✅ 11/11), 3.7.2 MERGED (PR #89, `b398d17`, macOS-validated — 47 of 53 records, the 6 open ones each
needing ears or the editor) and **3.7.3 COMPLETE IN CODE on
`feat/3.7.3-audio-boundary-ci-guard`** — twenty-two commits, the full local gate green, the S/X/P seed
matrices run as ctest stages, the break-the-guard meta-proof run against them, and
**four** code-review rounds closed (37 findings, 10 blocking — see below). Two durable outcomes: the guards were **inverted from a command denylist to an allowlist** for everything that NAMES a protected target, and for the direction that cannot be inverted — reaching one WITHOUT naming it — a ctest case now **reads `compile_commands.json` and asserts the property instead of predicting it**.

Epics **3.1** (AssetDatabase), **3.2** (Importers), **3.3** (Cooker v0), **3.4** (PBR materials),
**3.5** (Skeletal animation), **3.6** (Rendering essentials) and **3.7** (Audio playback v0) are all
**CLOSED in code**. What is left of the phase is its deliverable gate and the validation debt.

> **Per-task history — what each task shipped, what it deliberately left out, every trap and every dead
> end — lives in `docs/10-engineering-log.md`. Grep it before re-deriving anything.** This block is a
> summary of *where the position is* and of the rules that still govern new work. It is **rewritten**
> as the position moves, never grown: it reached 207 k characters once and that is what this note
> exists to prevent.

### 3.7.3 — Audio-boundary CI guard (complete in code, NOT yet merged) — CLOSES Epic 3.7

**Zero engine C++.** Two lint-job scripts, one compile-time probe, two hermetic `cmake -P` ctest
drivers, and a docs/comment sweep. `docs/tasks/phase-3.md` records the size as **M rather than S**
and why the shipped guard is broader than its subtask sentence.

**The subtask sentence — "public-header scan for miniaudio tokens" — was already live, and has been
since 0.3.3.** `check-platform-boundary.sh` scans `engine/*/include/*`, which is every subsystem, and
its self-test 1b derives the subsystem set from the tree, so both audio roots joined it automatically.
What had **no owner** was the **no-vcpkg property** of `engine/assets`, `engine/audio` and
`engine/scene_audio`'s CMakeLists — the thing four places in the tree handed 3.7.3 *by name*, and the
thing that voids silently while CI stays green. So what ships is: **prong A** (no dependency-hook
command, no non-`aero::` link token, no include dir outside the subsystem, and no cross-directory
`target_link_libraries` from any other tracked CMake file — all three files, `engine/assets`
included), **prong B** (a miniaudio token ban over both audio roots with **sources included**, which
the header-scoped platform guard cannot reach), **`tests/audio_boundary_probe.cpp`** (the sixth
probe), and **`check-boundary-probes.sh`**, taking the handoff 0.2.3 opened and 0.4.5 §7.1 routed here.

**THE MEASUREMENT THAT OUTLIVES THE TASK, AND THAT NO SENTENCE IN THE TREE HAD MADE:**
`vcpkg_installed` **is** on `engine/audio/src/mixer.cpp`'s compile line in `macos-release` and **is
not** in `macos-debug` — read from both `compile_commands.json`. The cause is by design:
`aero::profiling` is `PRIVATE` on all three vcpkg-free targets and carries `Tracy::TracyClient` when
`AERO_ENABLE_PROFILING=ON`, and a target's own `PRIVATE` usage requirements apply to its **own**
compile line. **Every "a stray `#include <miniaudio.h>` there is a hard compile error" claim in this
tree is therefore a profiling-OFF claim**, and any future sentence making one must say in which
configurations. `aero_audio_boundary_probe` links `aero::audio` alone — hence no profiling — and is
the **only compile-time enforcement the audio layer has that survives Release**; all five older probes
read vcpkg-free in Release too, for the same reason. Proven both ways through the real CMake target:
a seeded `#include <miniaudio.h>` fails `'miniaudio.h' file not found` in **both** presets.

**The gate, re-measured after the fourth code-review round:** 172/172 on both macOS presets with
`AERO_REQUIRE_GPU=1` (Debug 239.32 s, Release 64.59 s); fresh `-G Ninja` reduced configurations
**78** and **91**, each having built and RUN `aero_tests`, `aero_editor_shell_test`,
`aero_editor_imgui_test` and `aero_cooker`; **`ctest -N` 172 / 80 / 93 — `+4` in all three, in
lockstep** (the last two are the compile-line case and its own red-proof, both ungated); doctest **1169 / 1746 / 143 / 34 / 29 / 7 / 28 — UNMOVED in all seven**; **eight** guards
exit 0 (audio **11 / 3 / 54**, probes **6 / 56**, math **450**, platform **83**, scene **83**, rhi 144,
golden-rule 146, project-no-delete **A=6 / B=75**); clang-format and clang-tidy clean **by exit
code**; the manifest **untouched at 20 hash lines**. **This task INVERTS the usual pattern — `ctest
-N` moves while every doctest total stays put** (the probe has no `TEST_CASE`, the drivers are
`cmake -P`). No dependency lands, `vcpkg.json` and `/vcpkg` are untouched, `tests/CMakeLists.txt` is
**65 additions / 0 deletions**, `aero_tests`' link line is byte-unchanged, and the five existing probe
blocks are byte-unchanged. The three `engine/**` edits are **comment-only**, asserted by diff.

**The proofs are ctest STAGES, not plan prose, because `docs/plans/` is gitignored and a proof that
lives only there ceases to exist at merge.** `audio-boundary.guard_e2e` runs 37 stages,
`boundary-probes.probe_links_e2e` 34, and `boundary-probes.compile_line_e2e` 9. Every stage asserts an
**exact** exit code — 77 for skip included — every seed is read back **and** asserted present in the
index before any verdict is trusted, and each driver was run instrumented so every observed code was
seen rather than inferred. **Counts here are measured on the tree in front of you; they have gone
stale twice in this task alone.**

**THE CODE-REVIEW ROUNDS ARE THE PART TO REMEMBER, AND THE LESSON IS ARCHITECTURAL: A COMMAND
DENYLIST OVER CMAKE CANNOT CONVERGE.** Three rounds, and each one's blocking finding sat inside the
fix written to teach the previous one. Round 1: an `*_internal` refusal matching only the `aero::`
alias while the raw `aero_scene_internal` — which exists, and is what anyone copying from the defining
file would write — passed. Round 2: `EXCLUDE_FROM_ALL` refused on the `add_library` line while
`set_target_properties(<probe> PROPERTIES EXCLUDE_FROM_ALL TRUE)` passed. Round 3: the property
spellings refused while the **plain** commands passed —
`set_property(TARGET aero_audio APPEND PROPERTY INCLUDE_DIRECTORIES …)` exit 1,
`target_include_directories(aero_audio SYSTEM PRIVATE …)` exit 0, same write, same file, same target.
Along the way: `include()` missing from a banned list with *nothing in the guard reading an included
file*; a sweep line-scoped while its siblings flattened; a probes guard rejecting `PUBLIC` but never
requiring `PRIVATE`, so CMake's plain transitive signature passed under a banner claiming PRIVATE.

**THE FIX, AND THE RULE THAT OUTLIVES THE TASK: INVERT TO AN ALLOWLIST.** Ask what a protected thing
may LEGITIMATELY be named by, confirm by measuring the tree that the set is small and stable, and
refuse everything else. A boundary probe may be named by exactly two calls — its own `add_library`
and its single `target_link_libraries`. Nothing outside the three guarded CMakeLists may name
`aero_assets`/`aero_audio`/`aero_scene_audio` **at all**. A guarded CMakeLists may contain only the
three commands it has ever used. Ten escape attempts spanning `target_sources`,
`target_compile_definitions`, `target_precompile_headers`, `target_link_options`, `add_dependencies`,
`get_target_property`, `if(TARGET …)` and a bare `set()` are refused with no arm of their own. **If a
guard ever needs a second arm for a second spelling of one predicate, stop and invert it.** And where inversion is impossible — the without-naming direction, where a toolchain file or a preset can contaminate a compile line with no CMake text at all — **stop predicting the build fact and read it**: `boundary-probes.probe_compile_line` asserts directly that no probe's compile line carries vcpkg's shared include root, which is what four rounds of textual arms were approximating. What an
allowlist cannot see is what never names the target — inherited directory properties from an ancestor
(closed by an ancestor check computed from real include() edges). Beyond that, **the residual is not bounded by a list and is not claimed to be**: CMake pushes state into a directory scope through many commands, and through a toolchain file or a preset's cache variables which appear in no CMakeLists at all. That is why the ctest case `boundary-probes.probe_compile_line` reads `compile_commands.json` and asserts the property directly -- no probe's compile line carries vcpkg's shared include root -- which closes every such route at once, including the ones no textual guard can read. What that case in turn does not cover is stated where it lives: a configuration it never runs against (a multi-config generator writes no database, so it self-skips), and a contaminating include root that is not vcpkg's.

**AND THREE E2E STAGES PROVED LESS THAN THEY CLAIMED — the 2.1.2 species inside this task's own
proof.** Stage S5 passed over a *narrowly* mutated Part 1c (allowlisting any `${`-rooted include dir,
the exact rot the arm prevents) because its seed also carried the keyword `SYSTEM`, which was itself
producing a violation, so the generic "reaches outside the subsystem" assertion was satisfied by the
**keyword** and said nothing about the **path**. The original meta-proof had used a coarse mutation
any assertion would have caught. **The rule that outlives this: mutate the arm the way a careless edit
would, not the way a demolition would, and pin the offending TOKEN rather than the arm's generic
sentence.** Every stage is redden-proved alone under a one-line mutation — 35 of them, listed by name in `docs/10` rather than summarised, after the first version of that list silently dropped a stage and claimed completeness anyway. Three had to be redone: a mutation that reddens an EARLIER stage sharing the same arm proves the suite asserts, not that the stage does.

**One structural limit, because it looks like an oversight and is not:** a self-test over a call
extractor can pin that the extractor reads a wrapped call, but can **never** pin that the sweep calls
it rather than grepping line by line — the extractor flattens its own input, so it looks correct
under exactly the mutation that matters. Only an e2e stage pins that; S6b is the one that does.

**Two things found by building it that the plan had not predicted.** (1) **A `.cmake` e2e driver is
inside the very set prong A-d sweeps**, so `guard_e2e.cmake`'s fixture strings — literal
`target_link_libraries(aero_audio …)` — made the guard exit 1 on the tree that ships it, naming six
lines of its own driver. Fixed by composing the command name from a variable, so the scratch file
stays byte-identical while no matching literal remains; **an exclusion list was deliberately rejected**
as a permanent hole in a sweep whose whole value is being universal. (2) **A GitHub Actions step name
containing `aero:: library` does not parse** — the `": "` is a YAML mapping separator and the whole
workflow fails to load. Quote any step name containing a colon-space.

**`target_link_libraries(aero_audio_boundary_probe …)` is the tree's first near-miss for prong A-d,
and this task created it.** Part 1d sweeps for the target NAME as a whole token, and the trailing
`([^a-zA-Z0-9_]|$)` of `target_mention_re` is what keeps `aero_audio_boundary_probe` from reading as
`aero_audio`. Three self-tests pin that regex — it must match a real mention, must match a target on
its own line (the wrapped shape), and must NOT match the probe — using the same function the sweep
uses, so they cannot drift. (An earlier version compared against a three-name list by exact equality;
that helper was deleted when Part 1d became a name sweep, and this paragraph described it for a round
afterwards. The mechanism is the regex boundary.)

**A GUARD'S OWN `.cmake` E2E DRIVER IS INSIDE THE SET IT SWEEPS, AND IT BIT TWICE.** Fixture strings
spelling `target_link_libraries(aero_audio …)` made the audio guard exit 1 on six lines of its own
driver; adding the probes guard's cross-file sweep later did the same on seven lines of *its* driver.
Both fixed by composing the command name from a variable (`_AB_TLL` / `_BP_TLL`), so the scratch files
stay byte-identical while no matching literal remains. **An exclusion list was rejected both times** —
it would put a permanent silent hole in a universal sweep, in the one file most likely to acquire a
real CMake snippet later. Expect this every time a guard's scan set grows to include `*.cmake`.

### 3.7.2 — rules that outlive the task (merged, PR #89 `b398d17`, macOS-validated)

**Four rules.** (1) **`--dump-pcm` runs the SAME code path a speaker
hears** — both modes call the same `SceneAudio::update` and `AudioSystem::render`, which is what makes
the validation page's numbers evidence rather than analogy. (2) **The increment conversion reaches no
libm function**, so the whole path is bit-exact on every lane; contrast `render::sampleAnimation`,
which `docs/09` §13.7 excludes from the determinism contract by name. (3) **`std::clamp(NaN, lo, hi)`
returns NaN on libc++**, which made a pitch conversion `static_cast<std::uint64_t>(NaN)` — UB that
UBSan trapped; every clamp on a value that can be non-finite needs an explicit finiteness arm.
(4) **Two places must never compare the same key by different rules** — the review round's blocking
finding was a binding matched on full `Entity` identity and swept by index alone, so a recycled entity
index orphaned a looping voice permanently and unreachably. One comparator, one place.

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
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** **All seven epics CLOSED in code** — 3.1–3.6, and 3.7 with 3.7.1 + 3.7.2 merged and macOS-validated and **3.7.3 complete in code**. What is left is the gate below and the validation debt. Per-task detail in `docs/10`. |
| **Phase 3 gate** | Drop a rigged glTF/FBX in → PBR materials + shadows + a playing animation + **an audible sound**. The audible half exists in code as of 3.7.2 and **has not been validated on any platform** — 3.7.2's macOS pass ticked 47 of 53 records and left the 6 that need ears open. |

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

At 3.7.3's gate: **`ctest -N` 172 / 80 / 93** across tools-ON, both-tools-OFF and reflect-tools-OFF;
doctest across **seven** binaries **1169 / 1746 / 143 / 34 / 29 / 7 / 28**. Both reduced configurations
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

### Next

**Epic 3.7 is closed in code and Phase 3 has no open epics.** 3.7.1 (PR #88 `4892e65`) and 3.7.2
(PR #89 `b398d17`) are merged and macOS-validated; **3.7.3 is complete in code on
`feat/3.7.3-audio-boundary-ci-guard`** and not yet merged.

What remains for the phase is **its deliverable gate** — a rigged glTF/FBX in, producing PBR
materials, shadows, a playing animation and **an audible sound** — and **the validation debt above**.
The audible half has never been heard on any platform: 3.7.2's macOS pass ticked 47 of 53 records and
left open exactly the 6 that need ears or the editor. **Windows and Linux rows are outstanding for
every task in every phase, and 3.7.2's Linux row matters more than most**: LSan runs on that lane
alone, so the macOS row-11 zero proves the teardown is *clean*, not that a leak is *absent*, and unlike
3.7.1 everything that task allocates is first-party. See `docs/tasks/phase-3.md`.

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
| `docs/tasks/phase-{0..8}.md` | Full breakdown per phase: epics → tasks → subtasks, each task with goal + deliverable |
| `docs/08-risks.md` | Risk register, open + resolved decisions |
| `docs/09-file-formats.md` | Scene schema v1 (entity/components/version), canonical form, versioning & evolution policy |
| `docs/10-engineering-log.md` | **Per-task build history** — what each task shipped and deliberately did not, traps found, dead ends never to retry, and per-task build/dependency impact. Not normative; not auto-loaded. Read the relevant entry before touching a subsystem. |
| `docs/future-roadmap.md` | v2 / v3–v4 deferred features |
| `.claude/rules/*.md` | Path-scoped working rules, loaded only when matching files are opened (boundary guards, reflect-gen, CI portability, editor, cooked assets) |
