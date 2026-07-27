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
  *combined* set is non-empty, never a per-root count or roster.
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
