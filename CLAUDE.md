# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Aero Engine — an open-source (MIT), cross-platform 3D game engine with an editor and per-project TypeScript **or** C++ scripting. Solo project, started July 2026. The goal is core-workflow parity with Unity/Godot (edit → script → play → export), explicitly **not** feature parity. 3D-first; 2D arrives in Phase 7.

Two platform matrices, never to be conflated: the **editor** runs on macOS/Windows/Linux only; the **runtime** (exported games) targets those three plus iOS and Android. The editor never runs on mobile — no touch UI, no adaptive layouts.

## Current state — read this first

**Phase 3 (Asset Pipeline & 3D Content) is OPEN. Epic 3.7 (Audio playback v0 · audio) is open with
3.7.1 MERGED (PR #88, `4892e65`, macOS-validated ✅ 11/11) and 3.7.2 COMPLETE IN CODE on
`feat/3.7.2-playback-api-components`** — twelve commits, the full local gate green, the 44-seed
sabotage matrix run to completion, and a code-review round closed (seven findings, one blocking).
**3.7.3 (the audio-boundary CI guard) is next**, and closing it closes the epic.

Epics **3.1** (AssetDatabase), **3.2** (Importers), **3.3** (Cooker v0), **3.4** (PBR materials),
**3.5** (Skeletal animation) and **3.6** (Rendering essentials) are all **CLOSED in code**.

> **Per-task history — what each task shipped, what it deliberately left out, every trap and every dead
> end — lives in `docs/10-engineering-log.md`. Grep it before re-deriving anything.** This block is a
> summary of *where the position is* and of the rules that still govern new work. It is **rewritten**
> as the position moves, never grown: it reached 207 k characters once and that is what this note
> exists to prevent.

### 3.7.2 — Playback API + components (complete in code, NOT yet merged, NOT yet validated)

The first noise this engine has ever made. Four layers move: a realtime `AudioRenderFn` seam on
`platform::AudioDevice` expressed entirely in engine types; a pure spatializer, a lock-free mixer and
`AudioSystem` over two SPSC rings in `engine/audio`; `AudioSource` and `AudioListener` as the **seventh
and eighth built-ins**; and **`engine/scene_audio`, the twelfth engine subsystem**. Plus
`samples/phase-3-audio` — the first sample in the tree with **no window, no `rhi::Device`, no shader
and no `AERO_SHADER_TOOLS` gate at all**.

**The gate, measured on the tree at `e2da6f0`:** 168/168 on both macOS presets with
`AERO_REQUIRE_GPU=1` (Debug 331.55 s, Release 104.45 s); fresh `-G Ninja` reduced configurations
**76/76** and **89/89**, each having built and RUN `aero_tests`, `aero_editor_shell_test`,
`aero_editor_imgui_test` and `aero_cooker`; **`ctest -N` 168 / 76 / 89 — UNMOVED in all three**;
doctest **1169 / 1746 / 143 / 34 / 29 / 7 / 28** (`aero_tests` **+81** = SP 14 · MX 22 · SY 20 ·
SA 21 · DV 4; `aero_scene_serialize_test` +5; `aero_editor_inspector_test` +2; the other four +0), and
**the tools-OFF reading moves by the same delta**; six guards exit 0 (math **449**, platform **83**,
scene **83**, rhi 144, golden-rule 146, project-no-delete **A=6 / B=75 unmoved**); clang-format and
clang-tidy clean **by exit code**; the manifest **untouched at 20 hash lines**. Diff: **20 added /
22 modified / 0 deleted**. **No dependency lands**, and **the only link-line change anywhere is
`aero::scene_audio` joining `aero_tests`** — a test target.

**Its twelve-row validation page is RUN on macOS (2026-08-23): 47 of 53 records measured and ticked,
the 6 open ones every one a row needing ears or the editor opened, and no row failed**
(`editor/validation/3.7.2-playback-api-components.md`). The headline readings: the rolloff matches the
sample's own printed gains to **0.0001 dB**, the pan mirrors to **0.000001 dB**, the loop seam's max
step **equals** the in-cycle max exactly, two dumps are **byte-identical**, and `--no-spatialize`
leaves **0 of 192 000** frames differing against 187 659 for its spatialized twin. **`A38` is
witnessed in fact for the first time** — a real `tracy-capture 0.13.1` session exports a plot named
exactly `audio.voices` with 801 samples matching the sample's own callback count. **`A23` remains
uncovered by anything, as predicted.** The pass found **three defects in the page's own procedures and
one in a test, and none in the shipped code**: row 8 omits the `render()` pass without which
`activeVoices` cannot reach 64; **R4's mix-cost prediction is refuted** — "a few µs at 64 voices"
measured **213.10 µs**, ~50× the prediction, though still only **2.0 % of a 10.667 ms realtime block**
and scaling cleanly at 3.336 µs/voice; row 4 cannot be measured by band filtering at 480/240 Hz
(that method read 4.76 dB against a true 17.50 dB) and needs power subtraction; and **the two audio
inspector cases never called `registerEditorReflection()`**, passing only because a neighbouring case
did — run alone they read 0 fields against an expected 8 and 1. **Nothing in this task has ever been
HEARD on any lane**: CI compiles and runs every `SP`/`MX`/`SY`/`SA`/`DV` case on all three lanes — so the
spatializer, the mixer, the system and the bridge **are** cross-lane covered — but CI opens the **null
backend only**. **LSan runs on the Linux Debug lane alone**, so a green `SY20` on macOS proves the
teardown is *clean*, not that a leak is *absent*, and unlike 3.7.1 (whose leak lived in third-party
code) **everything this task allocates is first-party**.

**Three declared sabotage seeds, as MEASURED rather than predicted:** `A23` (a release→relaxed swap on
the clip-count store) and `A36` (the device's null-render silence path) have **no coverage anywhere at
all** — not a test, not a row, not a lane, because no lane runs TSan and the null-render buffer belongs
to miniaudio; `A38` (the Tracy plot's name literal) is covered only by validation row 9.

**Four rules from it that outlive the task.** (1) **`--dump-pcm` runs the SAME code path a speaker
hears** — both modes call the same `SceneAudio::update` and `AudioSystem::render`, which is what makes
the validation page's numbers evidence rather than analogy. (2) **The increment conversion reaches no
libm function**, so the whole path is bit-exact on every lane; contrast `render::sampleAnimation`,
which `docs/09` §13.7 excludes from the determinism contract by name. (3) **`std::clamp(NaN, lo, hi)`
returns NaN on libc++**, which made a pitch conversion `static_cast<std::uint64_t>(NaN)` — UB that
UBSan trapped; every clamp on a value that can be non-finite needs an explicit finiteness arm.
(4) **Two places must never compare the same key by different rules** — the review round's blocking
finding was a binding matched on full `Entity` identity and swept by index alone, so a recycled entity
index orphaned a looping voice permanently and unreachably. One comparator, one place.

### The phase table

| | State |
|---|---|
| **Phase 0** — Foundations & First Triangle | Complete in code. Gate **macOS-PASS**, held **OPEN** pending Windows/Linux 60 fps sign-off (`samples/phase-0-cube/VALIDATION.md`). |
| **Phase 1** — Reflection, ECS & Serialization | **COMPLETE.** Gate reached, macOS-validated; Windows/Linux render rows pending (`samples/phase-1-scene/VALIDATION.md`). |
| **Phase 2** — Editor | **COMPLETE, gate met 2026-08-02.** All six epics closed and macOS-validated; Windows/Linux rows pending for every task (`editor/VALIDATION.md`). Gate artifact: `samples/phase-2-editor-scene/` — data, deliberately not `add_subdirectory`'d. |
| **Phase 3** — Asset Pipeline & 3D Content | **OPEN.** Epics 3.1–3.6 CLOSED in code; **3.7 open**, 3.7.1 merged and validated, **3.7.2 complete in code**, 3.7.3 next. Per-task detail in `docs/10`. |
| **Phase 3 gate** | Drop a rigged glTF/FBX in → PBR materials + shadows + a playing animation + **an audible sound**. The audible half exists in code as of 3.7.2 and has not been validated on any platform. |

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
`engine/audio` is a **hard compile error** (verified in both directions at 3.7.2's gate).
**Adding a `find_package` to any of those three voids it silently while CI stays green.** 3.7.3 exists
to make the audio half permanent.

**FIVE GREPS ARE NOT LITERALLY ZERO AND MUST BE READ RATHER THAN COUNTED:**
`find_package` over `engine/assets`, `engine/audio` and `engine/scene_audio` (prohibition comments);
the `tools/` process-spawn grep ("libsdl-org fork" in `tools/shaderc/README.md`); INV-A1's float grep
over the four audio cook/container files; 3.6.3's `applyOetf|gammaEnabled|linearOutput|skipEncode`;
and **`#if` over 3.7.2's four new test files**, where every match is the prohibition *sentence*. The
usable form for the last is line-anchored:
`git grep -nE '^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif|else|endif)'`.

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

At 3.7.2's gate: **`ctest -N` 168 / 76 / 89** across tools-ON, both-tools-OFF and reflect-tools-OFF;
doctest across **seven** binaries **1169 / 1746 / 143 / 34 / 29 / 7 / 28**. Both reduced configurations
must be configured **FRESH with `-G Ninja`** — `CMAKE_GENERATOR` enters the shadercross bootstrap's
option hash, so the generator-less form reads the cached toolchain as **cold** and pays a from-source
DXC rebuild that peaked at 7.6 GB here — and each must **name which binaries it built and ran**
(`aero_tests`, `aero_editor_shell_test`, `aero_editor_imgui_test`, `aero_cooker`).

**Read the two kinds of move differently:** a `cooker.*` addition must be **identical in all three**
(`aero_cooker` takes no gate flag), and a `reflect-gen.*` addition must be **tools-ON only**. A smaller
move in a reduced configuration means the cooker block accidentally grew a gate, and **no test can
report that** — which is why it is a gate step. `aero_tests`, `aero_editor_shell_test` and
`aero_editor_imgui_test` each register as a **single** ctest entry, so hundreds of new doctest cases
move `ctest -N` not at all; an unmoved `ctest -N` therefore means "zero C++" only for a task that adds
no binary — check a zero-C++ claim against the **doctest** totals instead.

**Counts diverge by OS**, so never assume one: Windows skips `golden-rule.include_scan_e2e`, fourteen
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

### Next

**3.7.2 is MERGED (PR #89, merge commit `b398d17`, fourteen commits, all six checks green with
`headSha == HEAD` asserted) and its twelve-row macOS pass is RUN** — 47 of 53 records measured, the 6
open ones needing ears or the editor. **Windows and Linux rows are outstanding, and the Linux one
matters more here than on most pages**: LSan runs on that lane alone, so the macOS row-11 zero proves
the teardown is *clean*, not that a leak is *absent*, and unlike 3.7.1 everything this task allocates
is first-party. **3.7.3 (the
audio-boundary CI guard) is next** and closes Epic 3.7 — note that **the link line makes that guard
green TODAY by construction; a guard proves it STAYS green**, which is a different job. See
`docs/tasks/phase-3.md`.

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

CI (GitHub Actions; macOS + Windows + Ubuntu) configures, builds and tests all six presets on every push to `main` and every PR, plus a `lint` job running clang-format, clang-tidy, the vcpkg-baseline guard, and the **six** architecture guards in `.github/scripts/`: `check-math-boundary.sh`, `check-platform-boundary.sh`, `check-rhi-boundary.sh`, `check-scene-boundary.sh`, `check-golden-rule.sh`, `check-project-no-delete.sh`. The count is measured, not remembered — `ls .github/scripts/`.

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
- CI (GitHub Actions, macOS + Windows + Ubuntu, from commit #1): Debug build with ASan/UBSan + Release build, codegen steps, doctest unit tests, the **six** architecture guards that exist today — math-boundary (no `<glm/...>` outside `engine/core/src/math/glm_backend.cpp`, the single allowlisted file; **not** the looser "outside `core/math`", which would license GLM in the public math headers), platform-boundary, rhi-boundary, scene-boundary, golden-rule, project-no-delete; each created by its owning task, see `docs/04` — and format/lint checks. *(Audio-boundary and runtime-purity are planned for their owning phases and do **not** exist yet; do not cite them as live.)*

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
