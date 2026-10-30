---
paths:
  - "**/CMakeLists.txt"
  - "cmake/**"
  - ".github/**"
  - "CMakePresets.json"
  - "vcpkg.json"
  - "tests/*_probe.cpp"
  - "tests/golden-rule/**"
  - "tests/audio-boundary/**"
  - "tests/boundary-probes/**"
  - "tests/project-no-delete/**"
  - "tests/script-boundary/**"
---

# Guards, CMake and the build

Each rule here was earned by a defect, a green-and-wrong test or a measurement that contradicted a plan, and every one binds new work. The story behind each is in `docs/10-engineering-log.md`; grep it for the citation.

## Boundaries, guards and the build

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
**E.6.1 found two more:** `git grep -E` on macOS reads **`\s` as a literal**, exactly as it reads `\b`, so a
guard spelled with it can never fire (use `[[:space:]]`); and **`grep -P` works in this machine's shell only
through its ugrep wrapper** — under BSD `grep` it errors out and prints nothing, which reads as the expected
"nothing". Use `-E`/`-F`, or `command grep` to see what a script will see.
**Check every guard grep in BOTH directions: a guard command that cannot fail is worse than no guard command.**

**`git grep -nE '_WIN32|__APPLE__|__linux__'`** over `engine/assets engine/audio engine/scene_audio
engine/render engine/scene tools/cooker` reads **zero lines**; over `editor/src` + `editor/include`, **exactly
three lines in one file** (3.2.4's `currentHostOs()`).

**A GENERATED TU INCLUDES ITS HEADER BY NAME (E.6.1, `aa54461`).** MSVC builds this tree without `/utf-8` and
decodes source text through the code page, so an absolute `#include "…"` spelling a non-ASCII checkout path
breaks there. `cmake/embed.cmake` writes `#include "editor_font_data.hpp"` and passes the directory through a
source-file `INCLUDE_DIRECTORIES` property scoped to the generated files; any future generator does the same.

**A NEW `cmake/*.cmake` MOVES TWO GUARD FIGURES ON A TASK THAT ADDS NO TARGET (E.6.1).** audio-boundary's and
boundary-probes' last figure count every CMake file they sweep, `*.cmake` included, so `embed.cmake` + `embed_run.cmake` moved them
55 → 57 and 57 → 59. That move is expected; a move in any other guard is still a stop-and-find-out.

**THE ESBUILD FETCH IS A CONFIGURE-TIME STEP, UNGATED (4.3.1, `cmake/esbuild.cmake`).** The root `CMakeLists.txt`
includes it on every configure, including both reduced configurations (`AERO_SHADER_TOOLS=OFF`,
`AERO_REFLECT_TOOLS=OFF`), because the editor's tests need the binary there too. The file holds the host table (npm
platform, member inside the tarball, SHA-512 of the tarball) for `darwin-arm64`, `darwin-x64`, `win32-x64` and
`linux-x64`; any other host is a `FATAL_ERROR` naming the table. The hash table is the trust anchor: a mismatch is a
hard configure error, and a version bump means new hashes read from the registry's `dist.integrity` and re-verified
locally, never a relaxed check. Two presets configuring at once are safe because the work sits behind a `file(LOCK)` on
the version's cache directory; extraction goes through a staging directory, so an interrupted extract never leaves a
truncated binary at the final path; and `file(DOWNLOAD … EXPECTED_HASH)` skips the network when a matching tarball is
already cached. The cache is `<cache root>/aero-engine/esbuild/<version>/<platform>/`, resolved by the same convention
as `tools/shaderc/bootstrap.cmake` (which a reduced configuration does not include, hence the restatement; change the
two together), and `AERO_ESBUILD_ROOT` overrides the root. `aero_editor` gets a POST_BUILD copy beside it; the test
binaries run the cached binary through `AERO_TEST_ESBUILD`. CI caches the directory per OS under the key
`esbuild-<os>-0.28.2`, with no `restore-keys`. A cold-cache run needs `registry.npmjs.org`: a registry outage fails
configure, and the answer is to re-run it.

**`editor/src/script_transpiler.cpp` IS IN `check-project-no-delete.sh`'s CHECK A (4.3.1)**, which moved from 7 files to
8 and Check B from 111 to 113 `editor/src/*.cpp` files. It writes into `Library/Scripts/`, the reason `project_state.cpp`
is listed. Check A's `::copy` also forbids `std::copy` and `std::ranges::copy` there.
