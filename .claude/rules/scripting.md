---
paths:
  - "engine/script/**"
  - "tests/script_*"
  - "tests/script-boundary/**"
  - ".github/scripts/check-script-boundary.sh"
---

# Scripting

Each rule here was earned by a defect, a green-and-wrong test or a measurement that contradicted a plan, and every one binds new work. The story behind each is in `docs/10-engineering-log.md`; grep it for the citation.

## The `engine/script` subsystem

* **`engine/script`** (opened 4.1.1) holds the JavaScript VM and nothing script-visible: `engine::script::Vm` on
  quickjs-ng 0.15.1, and its backend-free allocator behind the INTERFACE `aero::script_internal`. Since 4.1.2 it loads
  ES modules through a borrowed `VirtualFileSystem` (`evaluateModule`, `import()`, `moduleRecords()`,
  `invalidateModule()`), with the loader's pure half in `module_graph.{hpp,cpp}`, and `engine/core`'s
  `normalizeVirtualPath` is public so a module's key is the VFS's own. It links `aero::core`
  PUBLIC and quickjs-ng's `qjs` PRIVATE, without the `libc` feature. **quickjs lives in `engine/script/src/` alone**:
  the ninth guard, `check-script-boundary.sh`, refuses a `JS…`, `js_…`, `QJS_…` or `QUICKJS…` name anywhere else under
  `engine/` or `runtime/`, and `tests/script_boundary_probe.cpp` is R12's seventh probe.


## Scripting

**A CALL IS `Interrupted` BECAUSE ITS OWN BUDGET FIRED, NEVER BECAUSE OF A VALUE (4.1.1).** quickjs-ng converts the
uncatchable interrupt into an ordinary rejection on several C paths, and an object carrying the uncatchable flag can
outlive its call (R21's second path), so `classify` reads the call's own `interruptFired` and never
`JS_IsUncatchableError`. Once the budget fires, the handler sets `JS_SetMaxStackSize(rt, 1)` — **1, never 0**, which
disables the check — so no new frame or native call starts until `~CallScope` restores it. A host function (4.2.1) or
any new caller classifies the same way, and anything that runs script code after the fire inherits the refusal.

**NO LAZY BUILTIN IS READ, AND NO BACKTRACE HOOK RUNS, FOR THE FIRST TIME UNDER A BUDGET (4.1.1).** `Vm::create` reads
quickjs's three bytecode-built builtins and deletes `Error.prepareStackTrace` / `Error.stackTraceLimit` before it
applies the configured stack limit. A port bump that adds a bytecode-built builtin, or a new hook a backtrace consults,
joins `materializeLazyBuiltins` / `removeStackTraceHooks`; VM45 and VM51 are the witnesses, and VM5 reddens at every
version change so the cited `quickjs.c` lines are re-read.

**EVERY BACKEND VALUE IS HELD THE MOMENT IT IS RECEIVED (4.1.1).** A leaked reference ABORTS the Debug lanes inside
`JS_FreeRuntime`, so the leak detector is a crash there. `HeldValue::release()` exists only for `JS_EvalFunction`, which
consumes its argument.

**R21: quickjs-ng ABSORBS THE INTERRUPT ON TWO PATHS THE WRAPPER CANNOT REACH (4.1.1).** A `using` block's dispose call
wraps an interrupt into a catchable `SuppressedError`, so a catch-all loop around one can run past its budget for good
(measured: two of four poll offsets hung under a 100 ms budget); and an interrupt that rejects a promise a handler
already waits for reaches that handler as an error no catch takes. 4.2.1 exposes the first script surface (the `engine` namespace) and calls no script of its own, so the first task that
calls one (4.3.1's TypeScript scripts, 4.4.1's lifecycle) inherits both; its measured fact is that esbuild lowers
`using` (`es2022`, or `--supported:using=false`), which 4.3.1 pins with a test. The fix is a patched backend — the project's first, an unowned handoff.

**EVERY MODULE IS READ THROUGH THE VM'S BORROWED VFS AND NOTHING ELSE, KEYED BY `normalizeVirtualPath` (4.1.2).** ML40
pins the one read path. A second normalizer would be a second key for one file. The specifier grammar is an ALLOWLIST
— `res://`, `./`, `../` — and `?` stays refused everywhere, because it is what keeps a generation unnameable.

**A FAILED LOAD OR LINK ATTEMPT RETIRES EVERY MODULE IT CREATED (4.1.2, R22).** quickjs-ng frees a module that fails
while resolving and leaves a cycle partner pointing at it, and re-linking a half-linked module leaks a binding. So a
failed attempt's modules move to a new generation name (`<path>?gen=N`, the name pure ASCII, with bytes ≥ 0x80 and `%`
percent-encoded), and the backend never sees them again. A new path into the backend's module machinery (4.3.2's
bytecode reader) inherits the rule or re-measures both shapes. A reload is a new instance; nothing is freed before
teardown, and the full reset is a new VM.

**THE TEARDOWN HOLD CONTAINS R23, AND EVERY CASE BUT TWO ASSERTS ITS COUNTS ARE ZERO (4.1.2).** From `JS_FreeContext`
until `JS_FreeRuntime` returns, the allocator holds every block the backend releases, because a top-level await still
pending at teardown makes the backend write into modules it already freed (bellard/quickjs #280). Three properties
keep it whole:
- a held block's size query answers 0, because `js_free_rt` asks before every release;
- the size word is a full 64 bits on every target;
- the hold threads through block headers, never pointer bits (Android tags pointers).
`writtenAfterRelease` and `repeatedReleases` are non-zero only in ML52 and VM61.

**`classify`'S ORDER IS THE CONTRACT (4.1.2):**
1. the call's own interrupt → `Interrupted`;
2. a load failure → `LoadError`;
3. a refusal plus a heap witness (`null`, the `JS_UNINITIALIZED` sentinel, the out-of-memory error, a message-less
   `InternalError`) → `OutOfMemory`;
4. in a compile, a refused growth → `OutOfMemory`;
5. otherwise `CompileError` / `Exception`.
A new heap witness joins step 3, never a new branch elsewhere.

**R24: A COMPILE WHOSE LOST BYTECODE GROWTH NO ASSIGNMENT FOLLOWS OVER-READS ITS BUFFER (4.1.2), AND NO WRAPPER CAN
CLOSE IT**, because `eval()` reaches it. Every test that compiles near the heap limit ends its source in
`assignmentPad`, so a lost growth meets an lvalue check. **And a heap sweep asserts only what the layout cannot move**:
ML71 predicted a starved `import()`'s parser error from a second VM and was red on Windows Release alone, because
where the heap refuses depends on block sizes and on what ran first.


**THE BINDING RUNTIME IS ONE HAND-WRITTEN FILE SET, FED BY QUICKJS-FREE GENERATED TABLES (4.2.1).** `reflect-gen
--emit-script` writes descriptor tables only (`internal/…/component_binding.hpp`); every quickjs call is made by
`engine/script/src/script_api*.cpp`. Generated code is outside lint, so it never holds a backend call, and a new field
kind is a new descriptor, not a new emitter branch. `aero_script` links `aero::scene` PUBLIC (its header names
`Entity`); the quickjs token ban and the public-header scan still hold. With `AERO_REFLECT_TOOLS=OFF` the table is
empty and the namespace holds `Vec3`, `Quat` and `Entity`: a test that names a built-in class checks the table first.

**COMPONENT OBJECTS ARE LIVE AND RE-RESOLVE THEIR ENTITY ON EVERY ACCESS (4.2.1).** No C++ pointer is cached across a
call. `Vec3` / `Quat` fields are live views (`t.position.x += dt` writes through); every field read makes a new view,
so `t.position === t.position` is false; a field accepts an instance only (`TypeError` otherwise, nothing changed);
and a standalone `new engine.Vec3()` is detached. `has`, `get` and `remove` on a stale entity throw (`entity.alive` is
the probe), `add` on a present component resets it to the defaults like `World::add`, and a wrong value on a dead
entity throws the value's error first. Every `bindWorld` invalidates script-held entities, which 4.4.1 and 4.7.2 must
account for.

**THE `engine` NAMESPACE IS COUNTED AGAINST THE BUDGET (4.2.1).** An empty VM costs 164,969 B in Debug and 152,585 B
in Release with the namespace bound (was 139,916 / 129,340 at step 4), and a fresh VM binds 773 atoms (652 empty). ML68
derives its window from a scan, so it follows the atom count; ML76's `ATOM_PRELUDE` (272 names) is typed in and was
re-measured at three steps, so a task that adds script-visible names re-measures it. BN50 sweeps 512 heap limits for the one that
refuses the vector's opaque block (87 of 512 in Debug, 58 in Release on macOS), asserts only that at least two exist,
and is a layout-dependent arm: Windows and Linux CI are its witnesses.

## What Phase 4's next tasks inherit

**Phase 4 is open; Epic 4.1 is closed (4.1.1, 4.1.2) and 4.2.1, quickjs binding emission, is merged (`docs/tasks/phase-4.md`).**
4.2.2 (`.d.ts` emission) reads the same descriptor tables; 4.3.1 (esbuild on import) is unblocked and inherits R21; 4.3.3 consumes 4.1.2's `moduleRecords()` / `invalidateModule()`, and 4.3.2 re-measures R22 on its bytecode
path. Phase E still owes its gate and E.1.5's unrun page — the gate's artifact,
`samples/phase-E-editor/`, does not exist yet; see `docs/tasks/phase-E.md` for the gate, and `docs/tasks/phase-3.md`
for what Phase 3 still owes.
