---
paths:
  - ".github/workflows/*.yml"
  - "tests/**/*.cpp"
  - "tests/**/*.hpp"
  - "tests/**/*.cmake"
  - "tests/lsan.supp"
---

# CI portability — the failure classes macOS cannot see

Three lanes (macOS/Metal, Windows/MSVC+D3D12-WARP, Ubuntu/GCC+lavapipe), Debug and
Release. A green local macOS run is **not** evidence CI is green. These have each
reddened a lane at least once.

## Transitive-include breaks (Linux/GCC)

libc++ supplies many headers transitively that libstdc++ does not. Using `std::uint16_t`
with only `<string>` included compiles on macOS and fails on Linux. **Include what you
use** — especially `<cstdint>`. Ninja stops at the first error, so one such break usually
hides several more.

## Raw string literals in macro arguments (Windows/MSVC)

A raw string literal passed **directly as a doctest macro argument** breaks MSVC's legacy
preprocessor when it contains `\"` — the sequence tokenises as an escaped quote
(C2017/C3688/C2661). Hoist the literal into a named local first.

**The discriminator is `\"`, not raw-strings-in-macros.** Raw literals without `\"` sit
inside macros in these tests today and have always passed on MSVC. Do not "fix" those.

## `<ostream>` is not transitively included by MSVC — this one has bitten four times

Streaming into `std::ostream` needs the **complete** type, not the forward declaration `<iosfwd>`
supplies. MS STL defines `operator<<(std::ostream&, std::string_view)` **inline in `<string_view>`**,
against a `std::basic_ostream` that only `<iosfwd>` has declared at that point; libc++ and libstdc++
are self-sufficient here and drag the complete definition in transitively. So a `CHECK` comparing
`std::string_view`s compiles clean on macOS and Linux and **fails the Windows lane alone** — with the
errors pointing **inside the STL headers**, not at the `CHECK` that caused them.

**doctest is why it surfaces at all**: a `CHECK` stringifies its operands, so the instantiation exists
only inside the template doctest expands, invisible until MSVC parses it.

Four occurrences so far: task 0.4.1, `tests/rhi_format_test.cpp`, `tests/scene_format_test.cpp`, and
task 3.3.1 (`tests/cooked_mesh_test.cpp`, `CHECK`ing the magic and `cookedMeshStatusLabel`).

Task 3.3.2's four new TUs (`cooked_texture_test.cpp`, `texture_cook_test.cpp`, `bc_block_test.cpp`,
`editor/texture_cook_source_test.cpp`) each carry the include **preventively**, written when the TU was
created rather than after a Windows lane said so. That is the posture to copy; they are not a fifth
occurrence, because no lane ever reddened for them.

**The fix is always the same: `#include <ostream>` in the test TU.** Not `<iostream>` (heavier, and it
adds a static initializer to every TU that includes it), and never a `static_cast<std::string>` at the
call site, which hides the cause and has to be repeated at every future `CHECK`. If a test compares or
prints a `std::string_view`, include `<ostream>` when you write it rather than after a Windows lane
tells you to.

## An engine `toString(YourEnum)` hijacks doctest's stringifier — every lane, hard error

`DOCTEST_STRINGIFY` expands to an **unqualified** `toString(...)`. So for any enum whose engine header
declares `std::string_view toString(YourEnum)`, **ADL finds ours** — a non-template exact match that
beats doctest's own template — and doctest then feeds the result into its decomposer, which tries
`std::string_view + const char*`. That is a **hard compile error on all three lanes**, reported inside
`doctest.h` rather than at the `CHECK` that caused it, so the message names neither your enum nor your
test.

It bites the moment a test writes `CHECK(someValue == YourEnum::X)` — the comparison doctest must
stringify. It has happened **twice**: `tests/rhi_format_test.cpp` (`engine::rhi::toString`) and task
3.3.2's `tests/cooked_texture_test.cpp` / `texture_cook_test.cpp` (`engine::assets::toString`
over `CookedTextureFormat`). Treat it as a recurring class, not a one-off — every future
`toString(SomeEnum)` on a public engine header inherits it.

**Two fixes, both fine, and the choice is per file:**

- Compare through `toString()` on **both** sides — `CHECK(toString(a) == toString(b))` — which needs a
  case proving `toString` is injective over the enum, or a difference can hide inside it. Task 3.3.2's
  `CT7` is exactly that case.
- Wrap the comparison in a **second pair of parentheses**: `CHECK((a == YourEnum::X))`. That stops
  doctest's expression decomposition entirely, which is what `tests/rhi_format_test.cpp` does.

**A status/label function named something other than `toString` is unaffected** — that is why
`cookedTextureStatusLabel` and `cookedMeshStatusLabel` are named the way they are, and it is worth
keeping in mind when naming the next one.

## clang-tidy (Linux Debug lane only, `--warnings-as-errors='*'`)

clang-format passing locally proves nothing about clang-tidy. Run it before pushing:

```bash
SDKROOT=$(xcrun --sdk macosx15.4 --show-sdk-path) \
  clang-tidy-18 -p build/macos-debug --warnings-as-errors='*' <files>
```

Repeat offenders: `misc-no-recursion` (**write iterative code — no recursive function
anywhere in engine or editor sources**), `performance-enum-size` (give every enum an
explicit `: std::uint8_t`), `bugprone-incorrect-roundings` (use `std::lround`, not
`static_cast<int>(x + 0.5F)`), `modernize-avoid-c-arrays`, `modernize-use-auto`,
`readability-identifier-naming`.

Members are plain `camelBack` with **no trailing underscore**; on an accessor-name
collision, pick a distinct name.

`clang-format-18` Homebrew and Ubuntu builds disagree on ~120-column chains. A local
format PASS never proves CI green — keep statements comfortably under the limit.

Sample `.cpp` files under `samples/` **are** linted in CI.

## Sanitizers

Debug lanes run ASan/UBSan (Windows: ASan only). UBSan catches what macOS-local Release
runs never will — unaligned reads (ImGui drag-drop payloads are `alignas(1)`: `memcpy`
into a local, never a cast) and out-of-range float→int casts.

LSan runs **only on the Linux Debug lane**. `tests/lsan.supp` must never grow without
observed CI evidence, and entries are frame-scoped, not module-scoped — with one
documented exception below.

## The lavapipe leak is SOLVED — do not re-litigate it

CI's software Vulkan ICD leaks a per-thread worker-pool struct at exit, with frames
unresolvable because the loader unmaps its ICDs. The fix that shipped:
`tests/vulkan_stack_pin.cpp` (Linux-only, static-init) `dlopen`s the ICD itself with
`RTLD_NODELETE`, plus two module-scoped `lsan.supp` entries. GPU-draw tests inherit it.

**Proven dead ends — never retry:** `LP_NUM_THREADS=0`, `MESA_SHADER_CACHE_DISABLE`,
`MESA_GLSL_CACHE_DISABLE`, and `LD_PRELOAD` (aborts ASan's "runtime must come first"
check and poisons the shaderc test processes).

## Test inventory

`ctest` totals **diverge by OS** — Windows runs one fewer case
(`golden-rule.include_scan_e2e` is registered on non-Windows only). Never assert an
absolute count without measuring; use `ctest -N`.

GPU tests needing presentation use a **visible** window — hidden-window presentation is
unproven on lavapipe and WARP. `AERO_REQUIRE_GPU=1` turns a missing GPU from a loud skip
into a hard failure; rehearse CI with it set.

Full history: `docs/10-engineering-log.md`.
