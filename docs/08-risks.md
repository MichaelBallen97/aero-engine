# 08 — Risks & Open Decisions

> A living register. Review at the start of each phase. Likelihood/Impact are Low / Med / High.

---

## Risk register

| # | Risk | L | I | Mitigation |
|---|---|:--:|:--:|---|
| **R1** | **Learning C++ solo** while building an engine — the #1 project risk (ADR-001) | High | High | Phase 0 budgeted as a learning phase; handles-not-pointers; ASan/UBSan from commit #1; never manual `new`/`delete` |
| **R2** | **Reflection generator complexity** (`reflect-gen`, ADR-004) — the #2 technical risk; feeds four systems | Med | High | Start with the minimal subset (plain structs + primitives + `Vec3`/`Quat`); build it in Phase 1 before anything depends on it; native `std::meta` migration path kept open. **Task 1.1.1 (2026-07-21) landed the harness rung**: `tools/reflect-gen`/`aero_reflect_gen` links system libclang 18's stable C API and proves it parses a real, unmodified engine header (`engine/core/include/aero/core/handle.hpp`) to a clean, zero-diagnostic AST on macOS (CI provisioning shipped for all 3 lanes; the C-API-only boundary, spec D5/F3, is what keeps the `std::meta` exit door open — confirmed live, not just asserted). **1.1.2 (2026-07-21) landed the detection rung**: `--components` detects `engine::component`-annotated structs/classes (authored via an `AERO_COMPONENT` macro → `clang::annotate`, since the literal attribute is discarded by Clang) and classifies their fields (primitives + `Vec3`/`Quat` supported; anything else collected, tagged `unsupported`, and warned about — never fatal) into an in-memory `Component`/`Field` model, proven end-to-end by 20 process-boundary ctest cases before 1.1.3's codegen consumes it. **1.1.3 (2026-07-22) landed the codegen rung**: `--emit-meta` emits `entt::meta` registration from the detected model, proven end-to-end (compiles + registers) on a fixture via the new `aero_reflect_meta_test`. **1.1.4 (2026-07-22) closed Epic 1.1's DoD**: codegen is now a first-class, incremental build step — `cmake/reflect.cmake`'s `aero_reflect_generate()` wires `--depfile`-driven `add_custom_command(DEPFILE)` per component header plus a machine-generated aggregator, proven end-to-end by a nested-project incremental-rebuild ctest (`reflect-gen.incremental_e2e`) that regenerates on a *transitively*-included header change and no-ops otherwise. R2 is downgraded from an open risk to a closed rung — the reflection generator's harness → detection → codegen → build-integration chain is complete; only the four downstream *consumers* (inspector, serialization, quickjs-ng bindings, `.d.ts`) remain, each its own later task. **Epic 1.2 (Serialization) opened at task 1.2.1 (2026-07-22): the JSON writer generated** — `--emit-json` emits per-component `aeroWriteJson` serializers against the new `engine::reflect`/`JsonWriter` runtime; the second of the four downstream consumers is under way. **1.2.2 (2026-07-22) closed the serialization consumer's JSON loop** — generated readers (`aeroReadJson`, against a new hand-rolled iterative `engine::parseJson`/`JsonValue` parser+DOM) + byte-equal round-trip proven on 3 OSes, closing Epic 1.2's Definition of Done ("component → JSON → component byte-equal"); also closed a latent 1.2.1 defect (an 18-`CXTypeKind` explicit whitelist replaces the old builtin-range test, excluding `long double`/`__int128`/`unsigned __int128`, which had no viable serializer overload). **1.2.3 (2026-07-22) froze the scene envelope**: schema v1 + version gate + strict-envelope/tolerant-payload policy documented in docs/09 and enforced in `aero_reflect`; the four reflection consumers are unchanged — the format layer treats payloads as opaque `JsonValue` objects by design. **The Epic 1.2 close-out audit (2026-07-23) re-verified the serialization consumer end-to-end and hardened three edges (non-namespace-scoped components now skip in `--emit-json` instead of emitting uncompilable code; `parseScene` rejects duplicate component keys on a hand-built DOM; a negative `JsonWriterConfig::indentWidth` clamps instead of throwing) — zero blockers found; the epic is audited-closed (accepted residual debt: R19 + the deferred WARN-text assertions pending the Phase-2 log sink)** **The `entt::meta` and JSON consumers are now proven end to end over the first real engine component (`engine::Transform`, task 1.3.2), not only over test fixtures.** |
| **R3** | **SDL3 dependency concentration** — it is platform + render + (via shadercross) shaders at once; single point of failure | Med | High | The `engine/rhi` wrapper is the escape hatch — treat it as sacred; boundary CI guards; SDL_GPU is otherwise economical and proven |
| **R4** | **SDL_shadercross is still preview** (v3.0.0-preview2, April 2026; re-verified July 2026, third stack pass — unchanged) | Med | Med | **The isolation seam now exists (task 0.4.3):** the frozen `aero_shaderc` CLI grammar, the JSON artifact schema, and `aero_add_shaders()` are the only contracts anything downstream depends on — never SDL_shadercross's own API/CLI/CMake package. The documented fallback (raw DirectX Shader Compiler + SPIRV-Cross calls swapped in behind that same frozen surface) lives in `tools/shaderc/README.md` |
| **R5** | **Scope creep → solo abandonment** — the classic engine-project death | High | High | The non-goals list ([06](./06-scope-and-non-goals.md)) is load-bearing; project rule #2 (every phase ships something usable); realistic 20–32 month horizon; re-plan per phase |
| **R6** | **Two export pipelines** (TS instant + C++ native, ADR-008) double the export surface | Med | Med | Sequence them: C++ authoring is free/early (dogfooding), TS layer is the dedicated build; consolidate both in Phase 5 |
| **R7** | **Cross-platform build pain** (CMake + vcpkg on 3 OSes) | High | Med | CI from commit #1; CMake presets; pinned vcpkg baseline; `main` always green |
| **R8** | **Mobile signing & platform quirks** (iOS/Android) surface late | Med | Med | Deferred to Phase 6 by design; Mac required for iOS; precompiled-runtime model keeps user-side export toolchain-free for TS projects |
| **R9** | **quickjs-ng scripting performance** (no JIT) becomes a gameplay bottleneck | Low | Med | Move hot logic to a C++ ECS system (ADR-007), not a language change; heavy work already lives in C++ |
| **R10** | **Time estimates slip** — part-time solo | High | Med | Estimates are planning aids, not commitments; the deliverable-gate model surfaces slippage early; re-estimate at each phase boundary |
| **R11** | **In-process play-in-editor** (4.7) — a gameplay crash takes the editor down with it | Med | Med | Autosave-before-play (4.7.3); scene snapshot & exact restore (4.7.2); separate-process play mode deferred to v2 |
| **R12** | **vcpkg's shared per-triplet `include/` directory limits the "private backend" compile-time boundary** (found in task 0.2.2) — every port installs into one flat `vcpkg_installed/<triplet>/include/`, and every vcpkg CONFIG target's `INTERFACE_INCLUDE_DIRECTORIES` is that same shared root, not a per-package directory. `PRIVATE`-linking a backend library (e.g. `aero_core`'s GLM link, ADR-005) makes a stray `#include` a hard compile error **only** for targets that link no vcpkg package directly; any target that does (e.g. `tests/` → `doctest::doctest`, or a future `engine/platform` → SDL3) inherits the whole shared root and can resolve the "private" backend's headers regardless | Med | Low | Engine layers (which link no vcpkg package directly) get the compile-time guarantee for free — verified for ADR-005/task 0.2.2 with a throwaway probe target, now the permanent `tests/math_boundary_probe.cpp` (task 0.2.3). Test targets and any layer linking a vcpkg package directly do not, and must rely on the textual grep guards docs/04 already specifies — **math's mitigation is now in place**: `.github/scripts/check-math-boundary.sh`, run by the `lint` job (task 0.2.3). SDL (0.3.1) and now **miniaudio (task 0.3.3)** are the second and third PRIVATE flat-root backends the probe bites for — miniaudio's is `tests/platform_boundary_probe.cpp` again (extended, not a new probe), because the *device* lives in `engine/platform`; the *separate* `engine/audio` guard for Phase 3.7's mixer/clip layer was owned by task 3.7.3, and **audio's mitigation is now in place**: `.github/scripts/check-audio-boundary.sh` (prong A, the no-vcpkg property of `engine/assets`/`engine/audio`/`engine/scene_audio`'s CMakeLists; prong B, a miniaudio token ban over both audio roots with **sources included**) + `tests/audio_boundary_probe.cpp`, which links `aero::audio` alone and is the **sixth** probe. That task also **measured a sharpening of this very risk that no sentence here had stated**: the three vcpkg-free targets link `aero::profiling` `PRIVATE`, and `cmake/profiling.cmake` makes that an INTERFACE library carrying `Tracy::TracyClient` — and with it this shared root — whenever `AERO_ENABLE_PROFILING=ON`. `PRIVATE` keeps it off *consumers'* compile lines, but a target's own `PRIVATE` usage requirements apply to its **own** line, so `engine/audio/src/mixer.cpp`'s compile line carries `vcpkg_installed` in `macos-release` and does not in `macos-debug` (read from both `compile_commands.json`). **Every "hard compile error" claim about a vcpkg-free target is therefore a profiling-OFF claim, and any future sentence making one must say in which configurations.** The probe, which links no profiling, is the only compile-time enforcement that survives Release. Separately, task 3.7.3 closed the probes' own rot mode — a vcpkg package silently joining a probe's link line, opened at 0.2.3 and renewed at 0.4.5 §7.1 — with `.github/scripts/check-boundary-probes.sh`, which derives the probe set from `tests/CMakeLists.txt` and states one bounded predicate: a probe may be named by exactly two calls — its own `add_library(<probe> OBJECT …)` and its single `target_link_libraries(<probe> PRIVATE aero::x)` — and **any other command, in any tracked CMake file, naming a derived probe is a violation**. That is an allowlist rather than a list of banned commands, and deliberately so: three code-review rounds each closed the spellings they were shown (`target_include_directories`, `target_compile_options`, `set_target_properties`, `set_property`, the CMake ≥ 3.13 cross-directory append) and left the class open, because CMake has too many ways to reach a target for enumeration to converge. The link call's own shape — one `aero::` library, one `PRIVATE`, one call — is checked separately, and the routes that reach a probe *without naming it* — a directory-scoped include/link command, and the `EXCLUDE_FROM_ALL` spellings, in the registry's own file, any ancestor, or any `.cmake` module those `include()` — are refused separately. That arm is a denylist and is **not** claimed complete: `add_compile_options(-I…)`, a `CMAKE_CXX_FLAGS` mutation, a toolchain file and a preset's cache variables reach the same compile line, the last two from no CMakeLists at all. The ctest case `boundary-probes.probe_compile_line` is what closes the class, by reading `compile_commands.json` and asserting the property directly rather than predicting it. **RHI's mitigation is now in place**: `.github/scripts/check-rhi-boundary.sh` + `tests/rhi_boundary_probe.cpp` (task 0.4.5) — SDL_GPU is the fourth PRIVATE flat-root backend the probe bites for (after glm, SDL, miniaudio). **EnTT (task 1.3.1) is the fifth**: `.github/scripts/check-scene-boundary.sh` + `tests/scene_boundary_probe.cpp`. **quickjs-ng (task 4.1.1) is the sixth**, and the named next case is closed for it: `.github/scripts/check-script-boundary.sh` + `tests/script_boundary_probe.cpp`, the **seventh** probe — covered by `check-boundary-probes.sh` and `boundary-probes.probe_compile_line` with no edit to either, because both derive their probe sets. One structural novelty: `aero::scene_internal` **deliberately** carries `EnTT::EnTT` INTERFACE, so linking it into any boundary probe would silently make that probe vacuous — recorded in the probe's own comment block, and **refused by name** by both 3.7.3 guards since. R12 itself stays **open** in general: future private backends still inherit it, and so does the residual the textual guards cannot reach. **That residual was bounded wrongly three times in this task and is no longer bounded by a list**: it was described as "a contamination that never touches `tests/CMakeLists.txt`", and a `link_libraries()` in that very file was then measured escaping. CMake pushes state into a directory scope through many commands and through a toolchain file or preset cache variables that are in no CMakeLists at all. What reads a fact rather than predicting it is the ctest case `boundary-probes.probe_compile_line` — and it reads exactly **one symptom**, vcpkg's shared include root on a *probe's* compile line: it filters to `*_boundary_probe.cpp` entries, so it says nothing about `EXCLUDE_FROM_ALL` (an excluded target is still configured and still has an entry) and nothing about the three vcpkg-free targets, whose own lines legitimately carry that root in Release; what remains beyond it is a per-probe negative compile, which is what the still-deferred `try_compile` harness would add |
| **R13** | **No ThreadSanitizer in CI** (raised by task 0.2.5) — the Debug lanes run ASan/UBSan (0.1.4), neither of which detects data races. `engine/core`'s job system (0.2.5) is the engine's first genuinely concurrent code, and every future subsystem that uses it inherits the gap: the suite can prove a job graph's *results* are correct while a race silently corrupts state on another machine or another day | Med | Med | Accepted for now — the job system's own internals are thin wrappers over enkiTS, which ships its own test suite, and the public API's contract (build single-threaded, submit/wait on the owning thread) keeps first-party concurrent code minimal. Revisit when the first real consumer lands (Phase 3+, likely `render` or `assets`): a TSan lane is a separate task, cheaper to add once there is concurrent first-party code worth pointing it at. TSan cannot share a lane with ASan. Task 2.2.5 added the second genuinely concurrent first-party code path (after 0.2.5's job system): the editor console's `LogSink` is written from arbitrary logging threads and drained from the frame thread. Its coverage is a stress arm under ASan/UBSan, which detects no races — this strengthens the case for a TSan lane. **Still open; the lane was deliberately not opened here.** |
| **R14** | **In-flight log callbacks are not drained on detach** (raised by task 0.2.4) — `setLogCallback({})` and `shutdownLogging()` clear the *stored* callback but do not wait for an invocation already running on another thread, because the callback is invoked outside the internal lock by design. A record that passed the level check just before the detach can still be mid-callback after it returns, so detaching does **not** make the callback's captured state safe to destroy. ASan-proven heap-use-after-free for the Phase 2 editor-console pattern: a panel detaches, destructs, and a worker-thread record invokes the freed lambda | Med | Med | Documented on the public API at `engine/core/include/aero/core/log.hpp:92-101`. No consumer detaches-then-destroys yet, so it is unreachable today: the only callers are tests, which outlive their callbacks. Until a drain exists the contract is the caller's — keep anything the callback captures (e.g. a `this` pointer) alive until independently certain no in-flight record can still be executing, or synchronize externally. A drain/join was deliberately out of 0.2.4's scope; **decide at Phase 2**, when the editor console panel becomes the first real consumer and can pin down whether the drain belongs at the call site or in `core`. **RESOLVED 2026-07-28 (task 2.2.5)** — at the CALL SITE, by shared ownership, with `engine/core` byte-identical. `editor::LogSinkScope` installs a callback that captures a `std::shared_ptr<LogSink>` **by value**; `logWrite` copies the stored callback under its mutex (`log.cpp:123-133`) and invokes it outside (`:153-156`), so the `std::function` — and every object it captures — is alive for the whole invocation no matter when another thread detaches. Detach-then-destroy is structurally impossible. Proven deterministically by `use_count() >= 2` while installed (`tests/editor/console_model_test.cpp`, case 21) and stressed with 4 concurrent producers under ASan/UBSan (case 24). A drain/join in `core` was **rejected**: it would put a blocking wait into a global logging API on behalf of one consumer, need a reader count or a second lock on `logWrite`'s hot path (every record, every thread), and still not save a consumer that captured a raw `this` — the caller's ownership is what actually has to be right. `log.hpp:92-101`'s warning **stays as written**: it is still correct for anyone who captures raw state, and this task is the worked example of doing it right. |
| **R15** | **CI never opens a real on-screen window** (raised by task 0.3.1) — the platform layer's window + event pump are exercised in CI only under SDL's headless `dummy` video driver (no runner has a display). Real-driver behaviour — HiDPI point-vs-pixel scaling, genuine resize/focus/minimize events from a window manager, DPI changes when dragging between monitors — is validated only by running `samples/phase-0-window` manually, and gets its first automated cross-OS exercise at the 0.5.3 spinning-cube gate | Med | Low | Accepted — the dummy driver covers window lifecycle, the pump's drain/translate path (white-box via `SDL_PushEvent`), and RAII/ASan; the translation is a pure mapping a real driver does not change. The residual is driver-specific window behaviour, inherently a with-a-display concern. Revisit if a real-window bug ships: an Xvfb (Linux) + windowed CI lane is a separate task, cheaper once render exists to make a window worth showing |
| **R16** | **CI's GPU-backed test coverage rests on software rasterizers and runner-image GPU sessions** (raised by task 0.4.2) — Linux uses lavapipe (Mesa's software Vulkan ICD, no real GPU), Windows uses D3D12's WARP software adapter, and macOS relies on the hosted runner VM's paravirtualized Metal session. Any of these could stop being available in a future runner image, or a lavapipe/WARP correctness gap could produce a false pass/fail unrelated to the engine | Med | Low | Mitigated by `AERO_REQUIRE_GPU=1` on all three CI lanes: a missing/broken GPU device is a hard test FAILURE, not a silent skip, so a runner-image regression cannot rot into invisible coverage loss — it turns the lane red immediately. If a lane's software path ever proves unavailable, the pre-decided fallback (spec 0.4.2 D7) is to flip that lane's `require_gpu` matrix value to `0` (skip-mode) in the same PR that discovers it, record the outcome here, and re-check promotion at task 0.4.4. Revisit this row whenever the vcpkg SDL baseline bumps — re-verify per task 0.4.2's own H6 checklist: the `MAX_*` limit constants (`SDL_sysgpu.h`), each backend's `PrepareDriver` video-driver gate, the deferred-GPU-reclamation structure (E13), and the SDR swapchain format set |
| **R17** | **The shader toolchain is a from-source bootstrap outside vcpkg's binary cache** (raised by task 0.4.3) — `tools/shaderc/bootstrap.cmake` clones SDL_shadercross and builds vendored DirectXShaderCompiler + SPIRV-Cross from source into a per-user cache; this is a first-build cost per machine/lane (several minutes to ~20 minutes locally, more on a cold CI runner) that vcpkg's own binary cache does not cover. Two narrower sub-risks: the Windows-vendored build configuration is the one build upstream's own CI does **not** exercise (F4) — a pre-decided contingency exists (switch that lane to prebuilt DXC `v1.9.2602` + `SDLSHADERCROSS_VENDORED=OFF` in the same PR if it fails); and DXIL is emitted with **no post-compile validation/signing step** (SDL_shadercross takes DXC's raw output as-is) — bytes-level validity (the `DXBC` fourcc) is all this task can prove, so *acceptance* by a real D3D12 runtime (WARP) is a named deliverable of 0.4.4's Windows lane. **A third, found-during-review correction**: the spec's F6 claim that the built `SDL3_shadercross` shared library is "self-contained (no dxcompiler runtime file)" is **empirically false** at this pin — `libdxcompiler`, `libdxil`, and `libspirv-cross-c-shared` install as real, separate shared objects alongside it in the prefix's `lib/`, and it is dynamically linked against them (confirmed via `otool -L`/inspecting the installed prefix). macOS resolves this for free via `aero_shaderc`'s own executable rpath (dyld resolves a dependent dylib's `@rpath` against the *loading executable's* `LC_RPATH`), but Linux's `DT_RUNPATH` is explicitly **not transitive** the same way, so `libSDL3_shadercross.so` needed its own `$ORIGIN`-relative install rpath (`CMAKE_INSTALL_RPATH=$ORIGIN` on the Linux sub-configure) to find its siblings — caught by code review before any CI run, not by a red Linux lane | Med | Low | The per-lane `actions/cache@v4` step (keyed on the toolchain SHA, no `restore-keys`) means the cost is paid once per machine/lane, not on every CI run; a SHA bump naturally invalidates the cache rather than silently reusing a stale toolchain. The Windows contingency is pre-decided, not improvised, and is upstream's own Windows CI pattern if taken. The DXIL-signing question is deliberately deferred to 0.4.4 (the real verdict is *acceptance* at `createGraphicsPipeline`/PSO creation under WARP — **not** `createShader`, which only checks the `DXBC` fourcc; 0.4.4's F6 correction) rather than guessed at here — if it rejects, the escalation options (verify the fork's embedded-validator behavior; add explicit validation; fall back to prebuilt DXC's signing `dxil.dll`) are pre-listed in the spec, not invented under pressure. **RESOLVED 2026-07-19 (task 0.4.4, PR #18, merged as `a31773d`): SDL_shadercross's unsigned DXIL IS accepted at PSO creation under D3D12/WARP — the Windows CI lane's `createGraphicsPipeline` passed on real WARP, so none of the escalation options were needed. First-party first-bytecode pipeline creation is now proven green on all three backends (WARP/DXIL, lavapipe/SPIR-V, Metal/MSL).** The from-source bootstrap + per-lane cache held on all three lanes; this row stays open only for the standing per-machine first-build cost. |
| **R18** | **`reflect-gen` uses system-discovered libclang 18, not a pinned artifact** (raised by task 1.1.1) — a contributor's machine or a future CI runner image with a different LLVM major installed parses the engine's headers with a different Clang frontend than the one this repo lints/formats against, which could in principle accept or reject different code than intended | Low | Low | Mitigated by the stable C API (libclang's `<clang-c/...>` headers carry source and binary compatibility guarantees across an LLVM major, spec F3) — `aero_reflect_gen` never includes a C++ Clang/LLVM header, so system-discovery across 18.x is safe by construction; the `find_package(Clang 18 CONFIG)` version match (when it fires) plus the fallback `llvm-config --version` assertion (plan C.3) both fail loud, not silent, on a real major-version drift; explicit per-lane provisioning pins LLVM 18 on all three CI hosts (Linux apt, macOS Homebrew, Windows choco); and `-DAERO_REFLECT_TOOLS=OFF` is the escape hatch for a machine that cannot install LLVM 18 at all. A future deliberate bump (e.g. to LLVM 19+) is a tested, recorded change, never an accidental drift |
| **R19** | **`JsonValue`'s compiler-generated special members are mutually recursive** (raised by the Epic 1.2 close-out audit, 2026-07-23) — destroy, copy, and move of a `JsonValue`/`JsonMember` tree all recurse once per nesting level (exactly the implicit members the `misc-no-recursion` NOLINT concession in `json_value.hpp` covers). Every parser-produced DOM is safe by construction (`JsonParseConfig::maxDepth`, default 256), but the `JsonValue::array`/`object` factories impose no cap, so a pathologically deep **hand-built** tree can overflow the stack on destruction (or copy) — the one non-iterative path in an otherwise deliberately iterative subsystem (parser, DOM re-emitter, and both scene walks are loop-based and stay checked) | Low | Low | Accepted and documented rather than fixed: an iterative destructor alone would be incomplete (copy/move recurse identically), and the full fix forces rule-of-five churn plus NOLINT renegotiation in a settled file, for a call pattern no current or planned code has (scene envelopes are shallow; payloads only come from the depth-capped parser). **Trigger condition for reopening**: any future code path that builds or copies unbounded-depth `JsonValue` trees by hand must first add iterative teardown/copy or a construction-depth cap — revisit this row at that point |
| **R20** | **Multi-viewport (OS-level floating ImGui panels) is deferred** (raised by task 2.1.1, 2026-07-25) — `ImGuiConfigFlags_ViewportsEnable` is left off; only `ImGuiConfigFlags_DockingEnable` is set. SDL_GPU's multi-viewport support is experimental, and enabling it would force the RHI to model ImGui-spawned OS windows plus a per-viewport swapchain each — a real architectural expansion `rhi::Device` does not need for anything else in view. Docking alone (undock/redock/split/tab within the one host window) already satisfies the "dockable panels" deliverable | Low | Low | Accepted: docking-only ships in 2.1.1 and is expected to carry the editor through Phase 2's panel work with no gap in day-to-day usability — nothing so far needs a panel to float outside the main window. **Trigger condition for reopening**: a real workflow need for an OS-level floating panel (e.g. a second monitor holding just the viewport) surfaces; at that point, re-evaluate SDL_GPU's multi-viewport maturity and the `rhi::Device` modeling cost together, as a dedicated task, not a drive-by flag flip. Separately: the editor observes the raw backend event stream (for ImGui's SDL3 backend) through a new generic, void-based `engine::platform::Context` seam (`RawEventAccessor`/`native_event.hpp`) rather than anything ImGui-named — the engine still names nothing ImGui, keeping the dependency-placement invariant intact. |
| **R21** | **quickjs-ng absorbs the time budget's interrupt on two paths `engine/script` cannot reach** (raised by task 4.1.1, 2026-10-22) — the interrupt that stops a script past its budget is an error the script cannot catch, and the backend converts it into an ordinary value in a few places. `engine::script::Vm` keeps every call that converts it `Interrupted` and lets no new JavaScript call start in it, but two paths survive that. (1) **A loop around a `using` block can outlast its budget for good.** When a dispose method fails while another exception is pending, the interpreter wraps both into a new, catchable `SuppressedError` (`quickjs.c:18897-18911`), and the interrupt can be raised at that call's entry poll, before the exhausted stack limit refuses the call. The backend consults the interrupt handler once every 10 000 polls, so a loop whose poll count divides 10 000 meets it at the same point every time. Measured under a 100 ms budget on both macOS presets: `for (;;) { try { using r = res; throw 1; } catch (e) {} }` ran until a 20 s cap killed it at two of four poll offsets, and ended in about 100 ms at the other two. The engine thread stays inside that call. (2) **An interrupt that rejects a promise a handler already waits for stays uncatchable.** The backend reports a rejection to the tracker only when no handler is attached (`quickjs.c:54371-54376`), so nothing clears the object's flag. In a later call the handler receives an error no `catch` takes: throwing it again ends that call `Exception`, and a reaction that passes it on is abandoned (`quickjs.c:54315-54317`), so the rest of that promise chain never runs | Low | High | **Recorded, not fixed, and unowned.** The impact is (1)'s, a hung engine thread; (2) costs one wrong status and a stalled chain. No engine code runs a script yet: nothing is script-visible before 4.2.1, and 4.4.1 is the first per-frame caller. The wrapper cannot close either path, because the poll counter is private to the backend and the wrapping happens inside the interpreter. Closing both needs a patched backend: a vcpkg overlay port of quickjs-ng whose conversion sites let an uncatchable error propagate, as `promise_reaction_job` already does. That would be the project's first patched dependency, so it is a decision of its own, and the defect is worth reporting upstream either way. `VM57` pins (2) as it stands and is meant to go red when such a patch lands |
| **R22** | **quickjs-ng 0.15.1 can leave a module reachable by name that points at freed memory, or link one module twice** (raised by task 4.1.2, 2026-10-22) — both measured in standalone programs against the installed port. (1) When a module's compile fails while it resolves its imports, the backend frees that module and nothing it loaded (`quickjs.c:37411-37415`: "should free all the unresolved dependencies"). A dependency that imported it back — a cycle — has already recorded its address and stays listed under its own name, so a later import that finds it links through freed memory: a SIGSEGV, or `Assertion failed: (m->status == JS_MODULE_STATUS_UNLINKED)` in `js_inner_module_linking` (`quickjs.c:30684`), with the port's Debug variant, and silently wrong linking with its Release one. (2) A link that fails after binding one import resets the module to unlinked without undoing the binding (`quickjs.c:30873-30878`), so linking it again counts the binding twice: `Assertion failed: (list_empty(&rt->gc_obj_list))` in `JS_FreeRuntime` (`quickjs.c:2348`) in Debug, one leaked block in Release | Med | High | **Mitigated in `engine/script`, not fixed in the backend.** Every module a failed load or link attempt created is retired under a new generation name (`<path>?gen=N`), so the backend never finds it again; a dynamic `import()` is linked by the engine itself before the backend sees the module's name, so no module is linked twice. ML23 and ML24 reproduce both shapes through the VFS on every lane. An embedder without the rule meets (1) with an ordinary missing file inside a cycle; the defect is worth reporting upstream. 4.3.2 re-measures both shapes on its bytecode path, where the reader resolves imports while reading (`quickjs.c:39136-39144`) |
| **R23** | **quickjs-ng frees a context's modules while objects outside it still hold module values** (raised by task 4.1.2's code-review round, 2026-10-23) — `JS_FreeContext` frees every module of the context unconditionally (`quickjs.c:2714`). When a module's top-level await is still pending at teardown and something still refers to that evaluation — a resolver a global or a module binding keeps, an importer awaiting it, or an `import()` of it — the await's reaction functions hold module values (`quickjs.c:31377-31379`, `:31030-31035`). The collector releases those functions after the module is gone, and each release decrements a count inside the freed module (`:6208`, `:6803`). Measured on both variants of the 0.15.1 port: five shapes, every one an abort at `quickjs.c:6779` once freed memory is poisoned, and silent corruption of whatever reuses the block otherwise. It is bellard/quickjs #280, fixed there on 2025-06-28 by letting a module outlive its context. quickjs-ng 0.17.0 still frees the modules (`quickjs.c:3073`) | Med | High | **Mitigated in `engine/script`, not fixed in the backend.** `Vm::shutdown` makes the VM's allocator hold every block the backend releases, from `JS_FreeContext` until `JS_FreeRuntime` returns, so the late writes land in memory the VM still owns. They are counted in `VmTeardownReport::writtenAfterRelease`. A block the backend hands back a second time — released, or reallocated — is absorbed and counted in `repeatedReleases`. The allocator answers the backend's size query with 0 for a held block, because the backend asks it before every release (`quickjs.c:1666-1669`). None has been measured. The hold costs about 4 ms on a full 64 MiB heap and no memory of its own. ML52 reproduces the shapes on every lane and VM61 pins the hold. Every other case asserts a zero count. A quickjs-ng report citing #280 is owed. If a port bump takes the fix, ML52's counts go to zero and tell |
| **R24** | **quickjs-ng can compile past a bytecode buffer that failed to grow** (raised by task 4.1.2's code-review round 3, 2026-10-25). The bytecode buffer records a refused growth and drops later writes. The only place that reads that record is `get_prev_opcode` (`quickjs.c:23358`), which an assignment's lvalue check calls. A compile whose lost growth no assignment follows therefore succeeds, and `resolve_variables` decodes the truncated buffer (`:34336`, `:34478`): a SIGBUS or SEGV, or `heap-buffer-overflow` with an instrumented backend. It is reached by `evaluateScript`, `evaluateModule`, `import()` and a script's own `eval()` and `new Function()`, under a heap filled to within a few hundred bytes to about 2 KB of its limit. Repro: `f()` compiled after a 1 MiB fill (`exp-review3/review3.cpp`, `compilecrash*`). quickjs-ng 0.17.0 has the same structure (`:23992`, `:37046`) | Low | High | **Recorded, not fixed, and unowned**, as R21 and R23. Script code reaches it through `eval()`, so no engine-side wrapper can close it. The backend fix is one check: refuse to create the function when `dbuf_error(&fd->byte_code)` holds, before `resolve_variables`. That makes the backend the project's first patched dependency, a decision of its own, together with R21 and R23. An upstream report is owed. The tests that sweep compiles near the limit are kept off it by construction: each source's bytecode growths are followed by an assignment (task 4.1.2 §O1) |

---

## Open decisions

Decisions deliberately deferred until there is data or the relevant phase arrives.

| Decision | Decide at | Notes |
|---|---|---|
| **Render architecture: forward+ vs deferred** | Phase 8 | With real Tracy profiling data — not before |
| **Long-term editor UI: ImGui forever?** | Phase 8 | ImGui is right for v1; revisit only with evidence |
| **Migrate to native C++26 `std::meta` reflection** | When MSVC ships support (~2027–2028) | `reflect-gen` output boundary designed for this swap (ADR-004) |
| **Spatial audio: Steam Audio / Resonance** | Phase 5+ | Optional HRTF/occlusion layer over miniaudio (ADR-006) |
| **Math backend: RTM instead of GLM** | If profiling shows GLM cost | Swappable by design (ADR-005); no engine changes needed |
| **Split Phase 3 into 3a/3b** | Start of Phase 3 | It is the largest phase (3–5 mo after the July 2026 additions); natural cut: 3a = 3.1–3.3, 3b = 3.4–3.7 |
| **esbuild vs swc for TS transpilation** | Task 4.3.1 | Default **esbuild** — single Go binary, no Node runtime, invoked externally (the Blender-CLI pattern); swc is the fallback |
| **HUD-text mechanism for the Phase 5 game** | Task 5.5.3 | Planned: editor-baked msdf atlas → engine-internal textured quads; formalized into the real text system at 7.3 |
| **Tilemap import format (JSON vs Tiled subset)** | Task 7.2.2 | Record the choice here when taken |
| **Transform world-matrix caching / dirty tracking** | Phase 2+ (first editor viewport, or 1.4.x profiling with Tracy data) | Requires a change-detection mechanism first (setter discipline, an explicit `Dirty` tag, or entt signals through the seam); until then `worldMatrix` recomputes on demand — the only correct option while components are mutated through bare `get<T>()` pointers |

---

## Unowned handoffs

Work a task found, measured or declined that no task in `docs/tasks/` owns yet. The list moved here from
`CLAUDE.md`'s state block on 2026-10-26, where a pointer to this section remains.

**SEVENTY-SEVEN UNOWNED HANDOFFS — one per bullet below; re-count the bullets, never the headline.** It read
"NINE" over a list that already held thirteen, because two "smaller" items, the exposure clause and
`FillMode::Line` were never counted. Handoffs recorded in their own paragraphs in `CLAUDE.md`'s state block (fact
4's four permitted cases, the inspector-row gap, the closed `File` section, E.1.1's fired thick-line handoff) are
not repeated here.

- **Asset-browser keyboard shortcuts (F2, Del)** — E.4.3 dropped both bindings AND their accelerator text:
  the gating needs a THIRD condition (`!io.WantTextInput`, because the browser's header carries an `InputText`
  search box), nothing in `tests/` can press a key, and there is no key-binding registry. E.6.2, the nearest
  candidate, built no registry (its D23), so nothing in the roadmap claims it.
  E.5.2's Create entries have no shortcut either. **E.6.3 found that condition too weak**
  (`imgui_widgets.cpp:5656-5660`): `WantTextInput` is set only while a field is ACTIVE, and Enter or Escape leaves
  the field holding `NavId` — the opening through which a Backspace after Enter deleted the Hierarchy's selection
  (the code-review round's finding 20). The gate that holds is the Hierarchy's latch (`.claude/rules/editor.md`, "A
  SELECTION SHORTCUT BESIDE A TEXT FIELD…", `I339`).
- **Richer undo labels** (`Move Cube` rather than `Transform`, the mock's wording) — E.6.2 shows the existing labels
  verbatim by decision; a label is the command layer's, and nothing owns renaming them.
- **Spot and point shadows** — the shadow pass is directional-only (3.6.2); Phase E's non-goals name
  cascaded/soft shadows as 8.2.1's, and nothing owns omni or spot shadow maps.
- **A camera FRUSTUM gizmo** — E.2.3 draws the camera an icon and no gizmo; a natural fit for 4.7.
- **A MIP CHAIN for E.2.3's icon atlas** — a 2.91x minification mitigated by a 4-texel minimum feature
  size; `Device::uploadTexture` already takes a mip level, so it is a downsampler and three lines.
- **Muting the point/spot lights the bridge TRUNCATES** — safe the day `RenderView` can report which survived.
- **A preview carrying the scene's POINT and SPOT lights** — excluded at E.2.4: a lamp lights a sphere at the
  origin by where it happens to sit, which predicts nothing about the material.
- **A FOV control** — `EditorCamera::setFovYRadians` exists and the mock implies one.
- **PERSISTING the viewport toggles and the tonemap params per user** — 3.6.3's, E.1.2's and E.2.3's
  handoff unchanged; E.2.4 moved the controls, not the state, and E.6.3's Stats toggle joined them as session state.
- **Exposure as a scene or camera property.**
- **A `meshIndex`-aware model pick** — needs a reflectable "sub-asset" annotation or a component-aware row;
  likely first customer 4.7 or E.5.x.
- **`AERO_ASSET(text)` and a `Text` reference** (E.3.3) — ONE edit to `assetKindIsDraggable` and three tests
  (`DR10`, `AR5`, `MP1`) the day a script component wants a `.json`.
- **A "reveal in browser" verb** from the picker or the row (E.3.3) — E.3.2's file-open handoff from the other
  side, needing the same new `ActionKind`.
- **`FillMode::Line` / wireframe-of-meshes** (fact 3 above), and with it a viewport shading mode (the mock's
  `Shaded`, declined by E.6.3's D13: no shading mode exists to name).
- **A case-only rename done OUTSIDE the editor (measured at E.4.4)** — on a case-insensitive volume the scan
  re-attaches `Wood.png` to the old GUID from `wood.png.meta`, the write of `Wood.png.meta` collides with that
  sidecar, and the record stays `Invalid` with a nil GUID on every scan. Pre-existing 3.1.2 / E.4.3 behaviour;
  only `deleteOrphanMeta`'s existence test keeps that last copy of the GUID from being deleted.
- **A project-level ignore file** (`.aeroignore` or a `project.json` key) — a format, a parse, an error
  catalog and a watcher interaction (E.4.4).
- **A directory roster** — `__MACOSX`, `node_modules`, Blender's `backup/`; `isDirectory` is the first term of
  both the browser predicate and the scan's bucket loop, so it is a traversal decision (E.4.4).
- **vim / emacs swap and auto-save files** — dot-prefixed, so already hidden on macOS and Linux (E.4.4).
- **A bulk "delete all orphaned sidecars" action or a distinct Issues category** for sidecars of now-ignored
  files — E.4.4's row 2 judged 40 of them usable, and tedious to delete one click at a time.
- **A thumbnail key that covers its DEPENDENCIES** (E.4.5) — a texture edit does not refresh a material's
  picture (R4); Reimport All refreshes only a tile NOT drawn in the frame its clear runs.
- **A quiet `parseMaterial` mode** (E.4.5) — a hand-edited file's unknown-key WARNs pass through once per card
  read and once per render; a `reflect` change.
- **A third thumbnail producer** (mesh, scene, audio waveform) — a third `ThumbnailSource` enumerator, a store
  and one `releaseKey` line (E.4.5).
- **A persisted thumbnail store under `Library/`** (E.4.5).
- **An Apply that refreshes the browser without the watcher** — declined at E.4.5: an Apply reaches the
  browser through the watcher, as every asset kind's edit does.
- **The Delete log line's trash path** (found on E.5.1's macOS pass) — it prints `Library/Trash/1` while the
  directory on disk is `0001`: E.4.3's sequence number printed unpadded. Cosmetic; one format specifier.
- **A typed create as a CHILD of the right-clicked row** (E.5.2) — needs a local-transform rule.
- **Placement by a surface raycast** (E.5.2).
- **Click-to-place in the viewport** (E.5.2).
- **Uniquified names** (`Cube (1)`) (E.5.2).
- **`AERO_LABELS` with spaces** (E.5.2).
- **Create ▸ Audio and ▸ Environment** (E.5.2 D8).
- **A Tested debug-line producer off the ground plane** (E.5.2).
- **A sheet macOS puts ON a still-open panel leaves the panel itself un-key** (#118's rows 6 and 12) — its
  "replace?" alert closed with Escape, its Go-to-Folder sheet closed with Return: the panel then ignores keys until
  clicked. No result exists while the panel is open, so the editor cannot act; SDL exposes no panel handle.
- **A held Return past the auto-repeat delay may answer a modal raised after a keyboard-closed panel** (#118 row
  15, unverified) — SDL never saw the press, so the repeat arrives as a fresh one. Hardware-only to measure.
  E.6.4's New Project dialog is a second surface (Browse's picker closing onto the open dialog); its row 25 is N-E
  too — a synthetic autorepeat stand-in never reached the picker, and no project was created in either attempt.
- **Report the keyboard-close case upstream** (libsdl-org/SDL) — #12684's fix reactivates the app but cannot recreate
  a key window that never existed; the editor's gate retires itself if SDL hands the focus back.
- **reflect-gen's `parseRangeToken` hex-`F` quirk** — `AERO_RANGE(0, 0x2F)` reads as 2; latent.
- **`THUMBNAIL_EDGE_TEXELS` 128 → 256** (with the resident budget halved) **if** E.6.1's row 9 judges 128² too
  soft at 2× — a judgement no 1× display can make.
- **`PREVIEW_MAX_EXTENT` 512 → 768 if E.6.1's row 10 does** the same for the Material preview.
- **FreeType hinting at 1×** (E.6.1 D5), on a named legibility complaint — stb_truetype rasterises today.
- **A light theme, a user theme file, a per-user UI-scale preference** (E.6.1) — `editor_prefs` is their home;
  `EDITOR_THEME` is one constexpr value today.
- **A CJK / emoji fallback face merged after Plex** (E.6.1) — user text in those scripts draws `U+FFFD`.
- **Licence notices shipped beside a PACKAGED editor binary** (E.6.1 D24) — Plex's OFL and Lucide's ISC/MIT
  texts live under `editor/third_party/fonts/` only.
- **A theme decision for `ImGuiItemFlags_MixedValue`**, the day a multi-selection edit draws one (E.6.1 D13).
- **ImGuizmo's hit tolerances above a 100 % UI scale** (E.6.1 R7) — library literals, deliberately unscaled.
- **The mock's axis hues, across all four consumers at once** (E.6.3 D3, declined) — the gizmo, the grid's world
  axes, the view-axis widget and the Inspector letters share ONE identity colour; moving it re-opens E.1.2 / E.1.3 /
  E.1.5 / E.3.1's validation rows and moves `TH2`, `AX1`–`AX3` and `I143(c)`.
- **A stacked axis row (label above, under a width threshold) or values at `smallSize`** (E.6.3 D5) — the two ways
  to win back the risen axis thresholds; the Directional Light still clips at the 1280 launch default.
- **The axis row's last digit draws over its unit's 1-point outline in a narrow band** (E.6.3's row 7, UI scale 1,
  this task's own layout) — for `0.000` from the content width where the digit's stroke enters the border (266 / 279
  / 316, Cube / Main Camera / Directional Light) down to where its ink is first cut, 4 points lower (262 / 275 / 312);
  for `123.456` from 310 to 307. Between the two the digit hides the outline, because the frameless drag's value
  region runs to the unit's outer edge (`inspectorAxisLayout`: `valueWidth = unitWidth - valueX`), where
  `RenderTextClipped` clips — the clip every framed ImGui drag uses (`imgui_widgets.cpp:2805`,
  `RenderTextClipped(frame_bb.Min, frame_bb.Max, …)`), so a stock `DragFloat` draws over its own border the same way.
  Ending the value region inside the border closes it, and moves every threshold above.
- **No `Clear` on the Inspector's reference rows in a 640-wide window** (E.6.3's row 17, present at the branch point)
  — E.3.3's `assetReferenceFieldWidth` floors the field at one frame height and `Clear` lands past the panel's clip.
  Nothing overlaps.
- **The Material slot row's thumbnail overlaps its `Clear` at 640 wide** (E.6.3's row 17, present at the branch
  point) — E.3.4's slot row draws the 64-point well at `thumbnailEdge` whatever its ≈ 50-point frame; at 900 and
  above the well fits.
- **Widgets in a dock's tab row** — the mock's header dropdowns; the pinned ImGui has `DockNodeBeginAmendTabBar`
  (`imgui_internal.h:3734-3735`), the route E.6.3 named and did not take (E.2.4 stopped at a popover).
- **Tab icons** (E.6.3 D12) — a label change publishes a new `window->Name` to `keyboardFocusPanelId`, so it needs
  `###` labels and `I159(h)`'s respelling.
- **The Console tab's unread dot** (E.6.3 D7) — needs a "seen" notion, and it changes what the tab's X means.
- **GPU time in the stats card** (E.6.3 D9) — SDL_GPU 3.4.12 has no timestamp queries, so the card shows `—`.
- **An entity enabled flag** (the mock's `off` meta, E.6.3 D6) — engine, schema and bridge.
- **The Hierarchy's footer Create Empty / Create Child buttons** (E.6.3 D6) — the Create menu and both context menus
  already create.
- **Component Copy / Paste / Reset** (E.6.3 D13) — the card's overflow menu holds Remove Component alone.
- **Renaming from the Inspector** (E.6.3 D5) — the entity header's name is read-only.
- **The mock's 38-point menu bar** (E.6.3 D13, declined after E.6.2 handed it on) — it re-arms E.6.2's D22 fixtures.
- **The mock's default layout**, the bottom dock spanning the full width (E.6.3 D13).
- **The viewport's `Cube · Transform` chip** (E.6.3 D13) — the mock gives it no stated meaning.
- **The shell's two modals beyond the theme** (E.6.3 D13).
- **A rounded nav cursor on the asset tiles** (E.6.3 D8) — `RenderNavCursor` draws a square over the `Selectable`'s
  widened box.
- **A missing-folder marker on a recent card** (E.6.4 D9) — an existence check per entry, taken outside the draw walk
  and refreshed like the New Project probe.
- **Removing one recent project from the list** (E.6.4) — a card's overflow or context menu; Open Recent ▸ Clear is
  the only edit today.
- **The manifest name and a last-opened time on a recent card** (E.6.4) — reading each `project.json`, and a timestamp
  the recents file does not store.
- **A typable Location** in the New Project dialog (E.6.4 U3, declined) — `Browse...` is its only writer.
- **Scene/content templates of mock 1e's kind** (`Empty 3D`, `Third-person starter`, …, seeding `assets/` and a first
  scene) (E.6.4 D4). C++ project-template generation is NOT unowned: it is 4.5.2's and 4.6.1's.
- **Scene units and git-init at project creation** (E.6.4 D4) — new project-system scope.
- **A "close project" command** that returns to the Welcome window (E.6.4) — `ProjectSession::close()` has no caller.
- **Enter on the Unsaved Changes modal** (E.6.4) — `shell_ui.cpp:309` comments `SetItemDefaultFocus(); // Enter ==
  Save`, which binds nothing; the containment modal's binding (`shell_ui.cpp:440-443`, `I270`) is the one to copy.
- **A pinned header and footer for the New Project dialog** (E.6.4 D34) — under ≈ 523 points of work height the
  whole dialog scrolls; pinning them is a body `BeginChild`, with the focus arming read in the modal and passed in.
- **A minimum OS window size for the editor** (E.6.4 D34) — nothing stops a window narrower than the 280-point
  dialogs.
- **The mock's heading letter-spacing** (E.6.4 D28) — a glyph-by-glyph draw.
- **A drop shadow for modals** (E.6.4 D22) — ImGui 1.92.8 draws none.
- **A watcher-driven New Project probe** (E.6.4 D12) instead of the 30-tick refresh.
- **A trailing separator on the `creates` path** (E.6.4 D32).
- **A patched quickjs-ng for R21's two paths** (4.1.1) — a vcpkg overlay port whose conversion sites let an
  uncatchable error propagate (the `using` dispose wrap, and a promise rejected with the interrupt), as
  `promise_reaction_job` already does. It would be the project's first patched dependency, and the defect is worth
  reporting upstream either way. VM57 pins R21's second path and goes red when the patch lands. **4.1.2 adds two
  defects the same port would close**: R24 (refuse to create a function while `dbuf_error` holds, one check before
  `resolve_variables`) and R23 (let a module outlive its context, bellard/quickjs #280's fix), whose hold could then
  go. Three upstream reports are owed: R21, R23 (citing #280), R24.

---

## Validation debt

What each task's validation still owes, and why a page is not optional. Validation pages are gitignored, so this section and CLAUDE.md's "Current state" are the tracked record.

**Validation pages are gitignored, so they enter no commit.** Per-page measurements and method notes are in
`docs/10`; this is the ledger of what is still owed.

**ONE PAGE HAS NOT BEEN RUN ON ANY PLATFORM: E.1.5's.** It is the whole of Phase E's validation risk on this OS
— every other task in E.1–E.6 is macOS-validated, as are 4.1.1 and 4.1.2 (see the index above; each run page's
record and measurements are in its task's `docs/10` entry, among them 4.1.1's Debug recursion depth of 56 calls at
512 KiB against Release's 544, which 4.4.1 decides). **Rows still owed on pages that otherwise passed include** the
2× rows of pages run on 1× displays — E.6.1's 2, 9, 10 and 18 (rows 9 and 10 decide two unowned handoffs), E.6.2's
19, E.6.4's 21, E.5.2's 18 and E.4.5's 11 — which need a Retina panel attached, and E.6.4's row 25 (a synthetic
autorepeat stand-in never reached the picker) and #118's row 15, which need a HELD hardware key. The index carries
every page's partial and N-E counts.

**Three method facts from E.5.2's pass:** this machine's `editor_prefs.json` had Focus Follows Selection OFF, which
row 1 needs ON (set through View, restored after); a project under `/private/tmp` makes E.4.1 record `""` because
macOS's panels return `/tmp/…`, so validate from a home-directory project; and a click that closes the Edit menu at
its first item's position IS Edit ▸ Undo — close a menu by re-clicking its header. **And always `git status` after
an interrupted sabotage run** (E.4.1's matrix once left a live seed in the working tree, caught before anything was
committed): an aborted seed looks exactly like a clean tree until it is read.

**E.4.2'S PAGE (14 of 16) OWES THREE RECORDS, EACH WITH A MEASURED REASON.** Row 9's **Escape** third, row 15
(request-hook popup close; `I192` pins only the ordering as source text and **no tier here can read
`g.HoveredWindow`**) and row 16 (the already-open-project guard; cover stays `CN25`). The pass drove the bare
executable, which no synthetic keyboard input reaches (see the method traps below); a signed `.app` takes keys, so
row 9's Escape third is executable now. Synthetic **mouse** events work fully, including navigating the native file
dialog by its column view. Row 16 additionally cannot be staged at all: `NSOpenPanel`
watches the filesystem and **clears the selection and disables `Open`** the moment the scene's parent
directory is deleted. Its two **Windows** rows are the two cross-platform risks recorded rather than fixed: `path("C:/").parent_path()` possibly yielding `"C:"` and giving the walk one extra
drive-relative probe, and a scene directly at a POSIX filesystem root being a **false refusal** — both
bounded, both in the safe direction.

**WHY A VALIDATION PAGE IS NOT OPTIONAL: for many tasks it is the ONLY cover a declared sabotage seed has
anywhere.** The recurring pattern is that no tier in this tree can type, click, press a key, open an ImGui
popup, read ImGuizmo's global state, judge a colour at a glance, or observe a frame-to-frame layout fact — so
a rect, a colour, a gesture, a wrap or a HiDPI legibility claim has no automated witness. E.3.4's pass alone
closed **eighteen of nineteen** uncovered seeds across ten rows; E.2.4's closed all six of its own; E.2.3's
rows 3 and 4 were the only cover its two seeds had. **E.3.2's seed rows are closed where synthetic input reaches**
(row 5, `S24`'s half-typed name, passed — and #116's row 9 re-ran it under the tab-only raise); what stays
uncovered is row 6 (`S24`'s drop half — no synthetic release commits a reparent), row 8 (the gizmo-drag hold — no
command can be issued while the button is held) and row 7's popup-closing half (unreachable by construction behind
the `popupOpen` Hold).

**ONE SEED REMAINS UNCOVERED ON AN OTHERWISE-COMPLETE PAGE AND CANNOT BE COVERED**: E.2.4's `S19`, the
`SkyPass::create` failure arm, is unreachable at runtime in any build whose shaders cooked, so the source-text
pin in `I140(b)` is its only witness — as that page itself states.

**STILL-OPEN RECORDS ON PAGES THAT OTHERWISE PASSED.** E.2.1 has two — its row 4 normal-mapped arm was
closed by E.5.1's pass (the bump reads in the ambient-only shade, the witness its seed 8 lacked): row 7's `Save
Scene` produced **no write and no log line** under synthetic input while New Scene, Reset Layout and Undo all
worked from the same path — **possibly a real defect, explicitly unresolved** — and rows 10/12 lacked a
material asset and a Tracy connection. E.1.3 has
two: row 6's translate/scale-in-ortho arm (blocked by the defect PR #95 fixed, re-runnable now) and row 13's
cost A/B, which needs a second build at the branch point. **Its row 7 PASSED on 2026-09-30** through E.5.2's
row 12: a created Plane receives the directional shadow in ortho, recorded on both pages.

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
rows, all thirteen Phase 2 tasks, and every Phase 3, Phase E and Phase 4 task.

**METHOD TRAPS FOR DRIVING THE EDITOR, ALL MEASURED.** A pending TCC prompt stalls the editor to ~0.2% CPU
with a window that **NEVER RENDERS** (a black capture); the prompts **CASCADE**, one per bundle identity, and
both kinds accept a synthetic click on the affirmative button derived from the dialog's own **fresh** bounds
(~`x + 0.727w`, `y + 0.844h`). A stale editor survives `pkill -f` and must be `pkill -9`ed; a stalled bundle
identity stays stalled until a fresh `CFBundleIdentifier` clears it. Synthetic mouse MOVES do provoke ImGui
tooltips. **Synthetic KEYBOARD input reaches ImGui only when the editor runs as a signed `.app`** (a bare
executable never becomes a key window). It goes to the FRONTMOST app, so confirm that before every key; and
Enter deactivates a single-line `InputText`. **Synthetic drag-and-drop still does not work** (it undocks
panels). **Click window-relative, re-reading the origin every time** — the window can move mid-row.
**Bind every capture to its launched PID** — two live `aero_editor` processes once made a window lookup
capture the STALE one and produced three false "0 differing" comparisons, including one that provably
contained an outline. **Let an anti-vacuity control be what catches that.** **While a native sheet is open, the
first `layer=0 onscreen=1` window of the PID is the SHEET**, so window-relative clicks land elsewhere — click a
panel in absolute coordinates. And before #118, a key sent right after a native panel closed from the keyboard was
dropped by the editor itself, not by the input method. Note `screencapture` carries the
display's ICC profile, and **there is no `renderFrame` Tracy zone in this tree** — the frame-level zones are
`renderScene` and `render`.

---

## Resolved decisions (for the record)

- **Project name** → **Aero Engine** (July 2026)
- **Scripting VM** → **quickjs-ng** over Bellard's QuickJS (July 2026)
- **Scripting model** → **TypeScript OR C++ per project, never combined** (ADR-008, July 2026)
- **"Ship a game" milestone** → moved from Phase 4 to **Phase 5** (needs scripting + export first)
- **2D** → **Phase 7**, after the 3D foundation is proven
- **July 2026 plan audit** → third tech-stack pass: **zero swaps** (all pillars verified active; doctest's maintainership confirmed resumed); epics **2.6** (project system), **3.6** (rendering essentials), **3.7** (audio playback v0), **4.7** (play-in-editor) added; audio & shadows pulled forward to Phase 3; imnodes dropped from the v1 table (v2 graph editors); full-depth breakdown for all phases in `docs/tasks/`; Notion consolidated to the 3 linked DBs (duplicate "Work Breakdown" DB deleted), tasks-as-rows + subtask checklists
- **E13 (SDL_GPU destroy-while-in-flight safety)** → **RESOLVED** at task 0.4.2, verified against the pinned SDL 3.4.12 source rather than assumed from the header's silence: all three backends (Vulkan `SDL_gpu_vulkan.c:10493-10520`/`:10734`/`:10767`/`:10969`; Metal `SDL_gpu_metal.m:659-991`; D3D12 `SDL_gpu_d3d12.c:961-1368`) gate actual reclamation on an atomic per-resource reference count that in-flight command buffers hold, draining pending destroys only once that count reaches zero. The engine's `destroy*()` (SlotMap-remove then `SDL_ReleaseGPU*`) needs no engine-side deferral of its own — SDL's own contract already makes "destroy a resource submitted this frame" safe
- **0.1.2 availability-table correction** (found by task 0.4.3) → the 0.1.2 port-verification pass ("11/11 ports available, zero gaps") checked port **name + version** existence only, not each port's transitive `supports` platform expression — the same species of gap task 0.3.3's miniaudio omission found. Task 0.4.3 hit this directly: the pinned baseline's `sdl3-shadercross` port depends unconditionally on `directx-dxc`, which vcpkg marks unsupported on `arm64-osx` (a real `vcpkg install --dry-run` proves it), so the port is dead on macOS despite existing in the registry. **Lesson for any future port-availability check:** verify transitive `supports` expressions for every dependency in the chain, not just top-level port existence

### The July 2026 audit findings (D1–D12)

The gap-fix codes cited as *(audit Dn)* throughout `docs/tasks/`:

| # | Gap found | Fixed by |
|---|---|---|
| D1 | Play-in-editor was in v1 scope but no phase built it | Epic 4.7 (+ risk R11, autosave-before-play) |
| D2 | Phase 5 game would ship silent — all audio waited for Phase 6 | Epic 3.7 Audio playback v0 (buses/effects stay in 6.4) |
| D3 | Game would ship shadowless, with no culling | Epic 3.6 Rendering essentials (8.2 became the *quality* pass) |
| D4 | Nothing created the *project* concept (ADR-008 and the AssetDatabase both need it) | Epic 2.6 Project system v0 |
| D5 | Scripts couldn't spawn anything at runtime | 4.4.4 prefab-lite (overrides/nesting → v2) |
| D6 | TS runtime errors would point at compiled JS, not the `.ts` source | 4.3.4 source-mapped errors (DAP debugger → v2) |
| D7 | Input under-planned: raw kb/mouse in 0.3.2, then nothing | 4.4.3 script input API; 6.1.2/6.2.2 touch (action-mapping → v2) |
| D8 | Game needs HUD text; text system was Phase 7; runtime can't link ImGui | 5.5.3 minimal msdf HUD text (formalized by 7.3) |
| D9 | Runtime player binary and two CI guards had no owning task | 5.2.1 player, 5.2.2 purity guard, 0.4.5 RHI guard |
| D10 | Mobile CI unstated | 6.5.1 mobile CI lanes (5-platform runtime coverage) |
| D11 | Missing non-goals — scope-creep holes around Phase 5 | 7 new non-goal rows in `docs/06` (each with a future-roadmap entry) |
| D12 | Editor QoL invisible in the plan | 3.1.4 hot-reload watcher, 3.1.3 asset browser v1, 5.5.4 public release page |
