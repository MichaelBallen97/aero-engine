---
paths:
  - "engine/**/*.hpp"
  - "engine/**/*.cpp"
  - "runtime/**/*"
  - ".github/scripts/check-*.sh"
  - "tests/*boundary_probe.cpp"
---

# Boundary rule & its guards

Project rule #3: **no third-party type crosses the engine's public API.** Not `glm::vec3`,
not an SDL handle, not a miniaudio type, not `entt::`. Everything lives behind engine types.

Five backends are linked `PRIVATE` and confined to one allowlisted TU each: **GLM** →
`engine/core/src/math/glm_backend.cpp` · **spdlog/fmt** → `engine/core/src/log.cpp` ·
**enkiTS** → `engine/core/src/jobs.cpp` · **SDL3** → `engine/platform/src/platform.cpp` ·
**SDL_GPU** → `engine/rhi/src/sdl_gpu_backend.cpp` · **miniaudio** →
`engine/platform/src/audio_device.cpp` (+ the one `.c` implementation TU) · **EnTT** →
`engine/scene/src/`.

## Every guard ships in two halves — and you need both

1. A `git grep` script in `.github/scripts/`, run by the CI `lint` job.
2. A **compile-time probe** in `tests/` that links **exactly one** `aero::` library.

Neither is sufficient alone. The script catches identifiers a probe cannot (prose-free
textual leaks, `.c` files); the probe catches what a grep pattern misses.

The audio layer's pair is `.github/scripts/check-audio-boundary.sh` +
`tests/audio_boundary_probe.cpp` (task 3.7.3). It is the one place where the two halves are
**not** interchangeable, and the reason is measured rather than argued: `aero_audio` links
`aero::profiling` PRIVATE, so in the `*-release` presets Tracy puts vcpkg's shared include
root on `aero_audio`'s **own** compile line and a stray `#include <miniaudio.h>` there
compiles clean. The script reddens it in every configuration; the probe, which links
`aero::audio` alone and therefore no profiling, is the only compile-time check that survives
Release. That script also carries a second prong the others do not: it guards the
**CMakeLists** of `engine/assets`, `engine/audio` and `engine/scene_audio` — no dependency
hook, no non-`aero::` link token, no include dir outside the subsystem — because their
linking no vcpkg package is the precondition every other claim on this page rests on.

**Rule 2's "exactly one" is enforced, and the enforcement is an ALLOWLIST.**
`.github/scripts/check-boundary-probes.sh` (task 3.7.3, taking the handoff 0.2.3 opened and
0.4.5 §7.1 routed there by name) states one bounded predicate: **a probe may be named by
exactly two calls — its own `add_library(<probe> OBJECT …)` and its single
`target_link_libraries(<probe> PRIVATE aero::x)`, both in `tests/CMakeLists.txt`. Any other
command, in any tracked CMake file, that names a derived probe is a violation.** That covers
`target_include_directories`, `target_compile_options`, `target_sources`,
`set_target_properties`, `set_property`, a bare `set()` capturing the name, and every
spelling nobody has thought of — with nothing to enumerate and nothing to keep current. The
link line's own shape (one `aero::` library, one `PRIVATE`, one call) is still checked
separately, since that is about the call's *contents* rather than its existence.

Some things reach a probe **without naming it**, and the allowlist structurally cannot see
them. Several are refused directly — a directory-scoped `include_directories()` /
`link_libraries()` / `link_directories()`, and the `EXCLUDE_FROM_ALL` spellings, anywhere in
the registry's own file, its ancestors, or a `.cmake` module they `include()`. **That arm is a
denylist and is not claimed to be complete**: `add_compile_options(-I…)`, `add_definitions`, a
`CMAKE_CXX_FLAGS` mutation, a toolchain file and a preset's cache variables all reach the same
compile line, and the last two are in no CMakeLists at all. **What closes the class is
`boundary-probes.probe_compile_line`**, a ctest case that reads `compile_commands.json` and
asserts the property itself: no probe's compile line carries vcpkg's shared include root.
Prefer that shape whenever a textual guard is predicting a build fact it could instead read. The probe list is **derived** from the registry's
`add_library(… OBJECT …)` lines, never enumerated, so a new probe is covered the moment it
lands. `check-audio-boundary.sh` carries the same inversion one layer over: **no file outside
the three guarded CMakeLists may name `aero_assets`, `aero_audio` or `aero_scene_audio` at
all**, and a guarded CMakeLists may contain **only** `add_library`,
`target_include_directories` and `target_link_libraries`. Both guards are proved
red-on-violation by hermetic ctest cases (`audio-boundary.guard_e2e`,
`boundary-probes.probe_links_e2e`).

**Lessons from 3.7.3's three code-review rounds, which between them found ten silently
evadable predicates and five stages that proved less than they claimed. Read 0 first — it
produced the blocking finding in ALL THREE rounds, each time inside the fix written to teach
the previous one:**

0. **DO NOT ENUMERATE SPELLINGS — INVERT TO AN ALLOWLIST.** This produced a blocking
   finding in three consecutive rounds, each time inside the fix written to teach the
   previous one: an `*_internal` alias but not the raw name; `EXCLUDE_FROM_ALL` on
   `add_library` but not its property spellings; the property spellings but not the plain
   commands (`set_property(TARGET x APPEND PROPERTY INCLUDE_DIRECTORIES …)` refused while
   `target_include_directories(x …)` — the same write, same file, same target — passed).
   A denylist over CMake commands cannot converge; there are too many ways to reach a
   target. **Ask instead what a protected thing may LEGITIMATELY be named by, confirm that
   set is small and stable by measuring the tree, and refuse everything else.** The
   remaining work is then the false-positive direction, which is finite and testable. If
   you find yourself adding a second arm for a second spelling of one predicate, stop.
   Where an allowlist genuinely is impossible, the fallback instinct is still **a predicate
   has more spellings than the one in front of you** — `*_internal` has an alias and a raw
   name; `EXCLUDE_FROM_ALL` has an `add_library` keyword, two target-property spellings and
   two directory-scoped ones; a link line has `target_link_libraries` and `LINK_LIBRARIES`.
   But treat that as the losing position, not the fix: enumerate only what you cannot
   invert, and say in the guard's own comment which half you are in.


1. **Match the predicate, not the spelling in front of you.** An `*_internal` refusal that
   compared against the `aero::` alias let the raw `aero_scene_internal` through — and the
   raw name is what anyone copying from the defining `CMakeLists.txt` would write.
2. **Ask what follows the thing you banned.** `include()` was missing from a banned-command
   list, and *nothing* in that guard read an included file, so two lines relocated a
   `find_package` out of the guard's reach entirely.
3. **If one arm flattens, they all must.** A line-scoped sweep beside flattening siblings
   missed a call wrapped between the parenthesis and the target name.
4. **Mutate narrowly when proving a stage asserts.** A stage passed over an arm mutated to
   admit exactly what it forbids, because its seed also tripped a *different* arm and the
   assertion pinned only the shared message. Pin the offending **token**; and note that a
   self-test cannot pin that a scan *uses* a helper when the helper flattens its own input —
   only an e2e stage can.

**R12 — the limitation that makes probes fragile.** vcpkg installs every port into ONE
shared per-triplet `include/` directory. A `PRIVATE` link therefore makes a stray
`#include` a hard error **only for targets that link no vcpkg package at all**. Inside
`tests/`, `doctest::doctest` drags the whole shared root onto the compile line, so the
identical leak compiles clean there. This is why a probe target's
`target_link_libraries` line must stay at exactly one `aero::` entry — adding a second
can silently void the guard. Never link an `aero::*_internal` target into a probe:
those carry the backend as an INTERFACE dependency by design.

Some rules cannot be probed at all and are held by **file placement** instead (editor
public headers staying ImGui-free and entt-free). Say so in the comment; do not claim
enforcement that does not exist.

## Not violations — do not "fix" these

- `<tracy/Tracy.hpp>` in `engine/core/include/aero/core/profiler.hpp`. Tracy is
  dev-builds-only and gated by linkage; this is not a rule-3 break and needs no guard.
- Prose in `//` comments that cites a real third-party identifier (`SDL_GPU`,
  `entt::type_hash`, `SDL_GetTicks`). The scripts strip `//` comments before matching
  precisely so these pass — that is why `check-platform-boundary.sh` is a script and
  not a bare grep (SDL has no namespace to anchor on).

## Writing guard scripts

- POSIX ERE `\b` **degrades to a literal on BSD/macOS**, so a guard using it passes
  vacuously. Use `(^|[^a-zA-Z0-9_])` instead.
- Every guard needs an **anti-vacuity canary**: prove the scan actually found and
  traversed something, not that it passed because it matched nothing. Check the
  *combined* set is non-empty, never a **hardcoded** per-root count or roster — a root
  that is legitimately empty today (`runtime/`) would fail a correct tree.
- **Non-empty is not the same as complete, and only the second one is the guard's job.**
  A scan set narrowed to one subsystem still passes "combined set is non-empty" while
  every other subsystem silently becomes a blind spot. Measured in the Phase 2 audit:
  narrowing `check-scene-boundary.sh`'s `HEADER_GLOB` to `engine/scene/include/*` took
  the scan from 51 files to 8 and the guard still exited 0 with its usual OK banner —
  the same false-green the 2.1.2 review found in `check-golden-rule.sh`'s `SCAN_ROOTS`,
  reached through the glob instead. So also assert **coverage**, with both sides derived
  from the tree (`check-scene-boundary.sh` / `check-platform-boundary.sh` self-test 1b
  are the pattern): a subsystem that ships nothing never enters the expected set, so
  unlike a hardcoded roster it cannot fail on a correct tree. Never derive the
  expectation from the value under test — that is circular and agrees with any narrowing.
- Scan every declared root. A multi-root scan whose second root is empty today
  (`runtime/`) will rot silently green — seed a violation against each root in tests.

## Sabotage proofs are mandatory, and have a failure mode

Seed a real violation, confirm the guard goes red, revert. Two hard-won rules:

1. **Confirm the seed actually landed** (`git diff`) before trusting a red-or-green
   verdict. BSD `sed`/`perl` in-place edits have produced silent false PASSes.
2. **Breaking the guard is the real test.** Proving a guard reddens on a seeded
   violation does *not* prove the test asserts anything. Break the *guard* and confirm
   the *test* goes red. Task 2.1.2 shipped four latent gaps hidden behind a fully
   green suite this way.

Full history of every guard and probe: `docs/10-engineering-log.md`.
