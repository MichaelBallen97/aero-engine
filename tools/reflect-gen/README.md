# tools/reflect-gen

`aero_reflect_gen` — a first-party CLI that links system libclang (the stable C API, LLVM 18),
parses a translation unit given on its command line, and either walks the resulting AST, (task
1.1.2, `--components`) detects `engine::component`-annotated structs/classes and collects their
fields, (task 1.1.3, `--emit-meta`) emits `entt::meta` registration C++ from that detected model,
or (tasks 1.2.1/1.2.2, `--emit-json`) emits a per-component JSON serializer PAIR — `aeroWriteJson`
+ `aeroReadJson` — against the `engine::reflect` runtime. This is Epic 1.1's harness (ADR-004:
reflection is the spine) plus Epic 1.2's full JSON serialization loop (write + read + the
component -> JSON -> component byte-equal round-trip); 1.1.4 makes codegen a first-class incremental
build step.

## Why system-discovered LLVM 18 (and not vcpkg, and not a pinned download)

`aero_reflect_gen` is build-time-only and never shipped, and it uses only libclang's **stable C
API** (see below) — so the byte-for-byte hermeticity of a pinned-prebuilt-download bootstrap (the
`tools/shaderc` pattern, task 0.4.3) buys nothing here, while costing hundreds of MB per OS and an
awkward Windows extraction. vcpkg's `llvm` port is a non-starter: it builds LLVM from source (hours
per cold CI lane, multi-GB) — the same from-source problem that killed the vcpkg route for shaderc,
only larger.

Instead, `tools/reflect-gen/CMakeLists.txt` discovers an LLVM 18 already provisioned by the
platform's own package manager: Homebrew `llvm@18` (macOS, keg-only — not on `PATH`, so discovery
hints the formula's prefix, never bare `find_program`), apt `libclang-18-dev` + `llvm-18-dev`
(Linux), or `choco install llvm --version=18.1.8` (Windows). This is also consistent with existing
precedent: the repo already provisions LLVM 18 from system packages for `clang-format`/`clang-tidy`.

Discovery order: `find_package(Clang 18 CONFIG QUIET)` first (an imported `libclang` target in one
shot, when it resolves), a hand-rolled `find_library`/`find_path` fallback second — both hinted by
the cache variable `AERO_LLVM_ROOT` (per-OS default; overridable via `-DAERO_LLVM_ROOT=...` or the
`AERO_LLVM_ROOT` environment variable). **Expect the fallback to fire on at least some lanes** — for
example, on Homebrew's `llvm@18` (18.1.8), `ClangConfigVersion.cmake`'s compatibility check requires
an *exact* major.minor match against the requested version, so requesting the bare major `18` (as
this project does) never satisfies it, and the fallback always fires there. That is fine: a
C-API-only consumer needs none of LLVM's CMake machinery. Either path keeps a version-drift gate
alive — the fast path via `find_package`'s own version match, the fallback via a light `llvm-config
--version` assertion (skipped, not required, if `llvm-config` is absent).

`-DAERO_REFLECT_TOOLS=OFF` skips the discovery, the tool, and the `reflect-gen` ctest suite
entirely, with a loud `STATUS` explaining what was skipped — the escape hatch for constrained/offline
dev machines, mirroring `AERO_SHADER_TOOLS`. Never set in CI.

## The C-API-only rule (and why)

`aero_reflect_gen` includes **only** `<clang-c/...>` headers — libclang's stable, versioned C API —
**never** a C++ Clang/LLVM header (`libclang-cpp`/libTooling, which is explicitly unstable and
version-locked). This is what makes system-discovery across a whole LLVM major version safe: the C
API's source and binary compatibility guarantees mean any 18.1.x build resolves the same calls, on
any host, from any vendor's package. It is also the seam ADR-004 keeps open for a future migration to
native C++26 `std::meta`: because this tool's own internals (which Clang API it uses) never leak into
what it produces, that migration stays local to this one tool.

## The frozen CLI contract (extended, never broken, by 1.1.2–1.1.4)

```
aero_reflect_gen [--all] [--main-file-only] [--version] [--help] [--emit-json] [--depfile <file>] <input> [-- <clang args>...]
```

- `<input>` — the translation unit to parse (the first non-flag token before `--`; a second is a
  usage error).
- Everything **after `--`** is forwarded verbatim to libclang (`-std=...`, `-I...`, `-isysroot...`,
  `-D...`, ...). `aero_reflect_gen` never derives these itself in this task — see below.
- `--main-file-only` (the **default**) limits the AST walk to cursors physically located in
  `<input>`. `--all` includes cursors from every included header. If both are given, `--all` wins
  (it is not a usage error to pass both).
- `--version` prints the tool version **and** `clang_getClangVersion()`, then exits 0. `--help`
  prints this usage grammar, then exits 0. Both short-circuit before any parse.
- **Output:** an indented AST walk to **stdout**, one line per cursor:
  `<indent><KindSpelling> '<spelling>' @<line>:<col>`. Diagnostics and errors go to **stderr** only.

| Exit code | Meaning |
|---|---|
| `0` | Parsed with **zero** error/fatal diagnostics (warnings — e.g. an unrecognized `[[engine::...]]` attribute — are allowed) |
| `1` | Usage error (unknown flag, missing `<input>`, `<input>` given twice) |
| `2` | Parse failure (a null translation unit, or at least one error/fatal diagnostic) |
| `3` | I/O error (`<input>` does not exist, is not a regular file, or is not readable) |

**What this task deliberately does NOT do** (each is a later, numbered task): derive compile flags
from the build itself (1.1.4 — this task's tests feed a hand-verified, per-OS flag set via
`-- <clang args>`); recognize `[[engine::component]]` as anything other than an unrecognized,
warned-about attribute (1.1.2); emit `entt::meta` registration or any other generated output (1.1.3,
below).

## Component detection (`--components`)

With `--components`, `aero_reflect_gen` **detects** every `struct`/`class` definition carrying the
`engine::component` annotation, **collects** its data members in declaration order, **classifies**
each against ADR-004's minimal subset (primitives + `Vec3`/`Quat`), and reports the result as a
deterministic listing to stdout — instead of the raw AST walk. `--main-file-only`/`--all` still
govern which cursors are considered, exactly as for the walk.

### Why a macro, not the literal `[[engine::component]]`

A bare `[[engine::component]]` is an *unrecognized* attribute: Clang parses it and then **discards**
it — there is no attribute cursor left to find in the AST, only a `-Wunknown-attributes` warning
(this is exactly what the `annotation_visible` test case keys on). So the annotation is authored
through the **`AERO_COMPONENT` macro**:

```cpp
#if defined(AERO_REFLECT_PARSE)
  #define AERO_COMPONENT [[clang::annotate("engine::component")]]
#else
  #define AERO_COMPONENT
#endif
```

Under `aero_reflect_gen`'s own parse, `AERO_REFLECT_PARSE` is **auto-injected** as
`-DAERO_REFLECT_PARSE=1` at the front of every invocation's clang args (no caller needs to pass it),
so `AERO_COMPONENT` expands to `[[clang::annotate("engine::component")]]` — a first-class
`CXCursor_AnnotateAttr` AST node the tool can find. Under the real compiler (where
`AERO_REFLECT_PARSE` is never defined), the same macro expands to **nothing**: zero attribute, zero
warning, zero runtime cost. The macro currently lives at `tests/reflect-gen/fixtures/aero_reflect.hpp`
(a tests-local home, kept out of `engine/` so this task's footprint is tools+tests only); it moves to
a permanent `engine/reflect/` public header when the first real component is authored in engine code
(task 1.3.2).

### Detection rule

A component is a `CXCursor_StructDecl` **or** `CXCursor_ClassDecl` that (a) is a definition, and (b)
has a **direct-child** `CXCursor_AnnotateAttr` whose spelling is exactly `engine::component`. Its
fields are its non-static data members (`CXCursor_FieldDecl`) in declaration order — static members,
methods, and nested types are excluded. Its qualified name is built by walking
`clang_getCursorSemanticParent` (namespaces and enclosing records) and joining with `::` (e.g.
`engine::demo::Light`).

### Field classification

Each field's type is canonicalized (`clang_getCanonicalType`, so a `using`/typedef alias still
resolves) before classification:

| Canonical type | Category |
|---|---|
| `bool`, the char family, `short`/`int`/`long`/`long long` (signed and unsigned), `float`, `double` | `primitive` |
| `engine::Vec3` | `vec3` |
| `engine::Quat` | `quat` |
| anything else (`long double`, `__int128`/`unsigned __int128`, `engine::Vec4`, `engine::Mat4`, `std::string`, a nested struct, a pointer, an array, …) | `unsupported` |

**The primitive whitelist is an explicit 18-kind list (task 1.2.2, D11), not "the whole builtin-arithmetic
range."** The range `[CXType_Bool, CXType_LongDouble]` actually spans 21 `CXTypeKind`s; `classifyField`
tests membership in exactly those 18, deliberately EXCLUDING `long double`, `__int128`, and
`unsigned __int128`. Those three are deliberately unsupported: `engine/reflect/serialize.hpp` has no
viable overload for any of them (`long double` is ambiguous between the `float`/`double` overloads;
`std::is_integral_v<__int128>` is false under strict `-std=c++20`), so a component carrying one used to
generate **non-compiling** `.json.gen.cpp`/`.meta.gen.cpp` code before this fix — the whitelist closes
that hole uniformly across all four reflection consumers (`--components`, `--emit-meta`, `--emit-json`,
and the future quickjs-ng/`.d.ts` consumers). No game component needs 80-bit or 128-bit fields; "expand
on demand" applies if one ever does, with a real per-type design rather than an ambiguous overload.

An `unsupported` field is **not** an error: it is still collected and listed, tagged `[unsupported]`,
and a warning naming it is printed to stderr — the exit code stays **0**. Detection is a lenient
*harness*; codegen policy (task 1.1.3) may later choose to reject, skip, or stub an unsupported field,
but detection itself always surfaces what it saw rather than silently dropping it or aborting the
whole translation unit.

### Output format

One `component <qualified-name> @<line>:<col>` line per detected record, followed by its `field
<name> : <type-spelling> [<category>]` lines (indented two spaces); a component with zero fields
prints only its `component` line. Nothing but the listing goes to stdout — diagnostics and
unsupported-field warnings stay on stderr, matching the tool's existing stream discipline. Example:

```
component Transform @6:24
  field position : engine::Vec3 [vec3]
  field rotation : engine::Quat [quat]
  field mass : float [primitive]
```

## entt::meta codegen (`--emit-meta`)

With `--emit-meta`, `aero_reflect_gen` runs the **same detection walk** as `--components` (spec
D3/D4/D5/D7), then serializes the resulting `Component`/`Field` model as a compilable
`entt::meta` registration translation unit — instead of the AST walk or the `--components` text
listing. `-o <file>` writes it to `<file>` (in **text mode**, so the output is byte-identical to
the stdout form on every OS — Windows' `\r\n` translation applies equally to both); the default
with no `-o` is stdout. Generation only happens for a **clean parse**: on a parse failure
(`maxSeverity >= Error`), the tool falls through to the usual exit-2 verdict *without* running
detection or opening `-o` — so a parse failure never leaves a stale/partial output file behind.

### The generated file's shape

```cpp
// GENERATED by aero_reflect_gen --emit-meta — DO NOT EDIT.
// source: component_basic.hpp
#include <entt/meta/factory.hpp>
#include <entt/core/hashed_string.hpp>

#include "component_basic.hpp"

void aero_reflect_register_component_basic() {
    using namespace entt::literals;
    entt::meta_factory<Transform>{}
        .type("Transform"_hs, "Transform")
        .data<&Transform::position>("position"_hs, "position")
        .data<&Transform::rotation>("rotation"_hs, "rotation")
        .data<&Transform::mass>("mass"_hs, "mass")
        .data<&Transform::hitPoints>("hitPoints"_hs, "hitPoints")
        .data<&Transform::active>("active"_hs, "active");
}
```

- **The register function is explicitly named and caller-invoked — never static-init
  auto-registration.** The name is `aero_reflect_register_<stem>`, where `<stem>` is `<input>`'s
  file stem with every character outside `[A-Za-z0-9_]` mapped to `_` (and a leading `_` inserted
  if the result would otherwise start with a digit) — a deterministic, valid C++ identifier. This
  is a deliberate design choice (approach B): a static-initializer that self-registers at load time
  would be silently dead-code-eliminated by a linker when the component lives in a **static
  library** with nothing else referencing it — exactly 1.3.2's shape (an engine-side static lib
  with no other symbol pulling the TU in). An explicit, named, caller-invoked function has no such
  footgun.
- **`Vec3`/`Quat`-typed fields are registered as plain `.data<>()` members of their owning
  component, never as standalone `entt::meta` types of their own** (D5) — this task does not
  register `engine::Vec3`/`engine::Quat` themselves with `entt::meta`.
- **Unsupported fields are skipped, not fatal**: each becomes a `// skipped: <name> (<type> —
  unsupported)` comment *after* the `.data<>()` chain's terminating `;` (a comment cannot sit
  mid-chain), plus the same stderr warning `--components` already emits. The exit code stays `0`.
- **A zero-field (tag) component** emits `.type(...)` with no `.data<>()` line at all. **Zero
  detected components** emits a register function containing only
  `// no engine::component annotations detected` — still a valid, callable no-op function.
- **Deterministic**: no addresses, timestamps, or absolute paths appear (the `#include` is always
  a file basename) — two runs over the same input+flags produce byte-identical output.

### Depfiles (`--depfile`)

With `--emit-meta` **and** `-o <file>`, `--depfile <file>` additionally writes a **Makefile-format**
depfile listing the parse's full `#include` closure — the standard `-MD` convention, so a build
system (`add_custom_command(... DEPFILE ...)`, task 1.1.4) can trigger a real incremental rebuild the
moment any transitively-included header changes, not just the top-level component header itself.

The depfile is a single rule: `<abs -o path>: <dep> <dep> ...` — the target is the `-o` path
(absolute, forward-slash, lexically normal); the deps are every file `clang_getInclusions()` visited
during the parse (the component header itself, its own includes, and every system/SDK/compiler
resource header transitively pulled in), plus `<input>` itself as a belt-and-suspenders entry, all
absolute + forward-slash + lexically normal, deduplicated and sorted (`std::set`) so two runs over
the same input+flags produce a byte-identical depfile on the same machine. Paths are escaped per the
Makefile dependency-list convention: a literal space becomes `\ `, `#` becomes `\#`, `$` becomes `$$`
(backslashes cannot appear in the paths this tool emits, since they are always `generic_string()`-ed
forward-slash first).

**Machine-local exemption:** unlike the generated `.cpp` (byte-identical across machines, see above),
a depfile is **not** required to be portable across machines — like a compiler's own `.d` output, it
necessarily embeds this machine's absolute SDK/resource-directory paths. Determinism here means "two
runs on the *same* machine with the *same* flags produce the same depfile," not "the depfile is the
same on every OS."

**Validation (D6):** `--depfile` is meaningful only in terms of a make-rule *target*, which is always
the `-o` path — so it requires **both** `--emit-meta` and `-o`; passing `--depfile` without either is
a usage error (exit `1`), checked before any filesystem work. **Gating (D8):** exactly like `-o`
itself, the depfile is written only after a **clean parse** (no error/fatal diagnostics) and only
after the `-o` write has fully succeeded — a parse failure or `-o` I/O failure leaves **no** depfile
behind, never a stale or partial one. A depfile I/O failure (cannot open/write `<file>`) is exit `3`,
mirroring `-o`'s own I/O-error path.

### Build-time generation, not configure-time

`aero_reflect_gen` is itself built by this same build, so it does not exist at CMake **configure**
time — generation is therefore always a build-time `add_custom_command(... DEPENDS
aero_reflect_gen ... DEPFILE ...)`, never a configure-time `execute_process`. `cmake/reflect.cmake`
(task 1.1.4) owns this end-to-end via `aero_reflect_generate(<target> HEADERS <hdr>... [INCLUDE_DIRS
<dir>...] [DEFINES <NAME[=VAL]>...] [AGGREGATOR <name>])`:

- **`HEADERS`** is an explicit list of component headers — no glob, no auto-discovery-by-scan (D3):
  the build graph must be knowable from the call site alone, not from what happens to exist on disk.
- Each header gets its own `add_custom_command(... DEPFILE ...)`, producing one generated
  `<stem>.meta.gen.cpp` (plus its `.d` depfile) under
  `${CMAKE_CURRENT_BINARY_DIR}/reflect-generated/<target>/` (D5) — a directory scoped per *target*, so
  two targets reflecting headers with the same stem never collide.
- **`INCLUDE_DIRS`/`DEFINES`** add extra parse `-I`/`-D` flags the target's own `INCLUDE_DIRECTORIES`/
  `COMPILE_DEFINITIONS` genexes can't reach on their own — most commonly a linked library's `PUBLIC`
  include directory, which propagates to the *compile* but not to this configure-time-evaluated parse
  (D9).
- A machine-generated **aggregator** TU (`<target>.aggregator.gen.cpp`) forward-declares and calls
  every per-header register function, in **`HEADERS`-list order** (D4/D15), inside one explicitly
  named function — `aero_reflect_register_all_<target>` by default, or the `AGGREGATOR` override.
  Never static-init auto-registration (same D4 rationale as the per-header functions themselves): a
  caller must invoke it explicitly.
- Calling `aero_reflect_generate` twice on the same target, or with an empty `HEADERS`, or before the
  target exists, is a configure-time `FATAL_ERROR` (D18/E3/E7) — not a silent no-op.
- **`-DAERO_REFLECT_TOOLS=OFF`**: the function becomes a defined, harmless no-op (a `STATUS` line,
  then `return()`) — no generated sources, no depfile, no aggregator (D11/E15). Callers that need the
  registration to exist must gate themselves.

`tests/CMakeLists.txt`'s `aero_reflect_meta_test` — the first Epic 1.1 doctest target — is the real
consumer: two fixtures (`component_codegen.hpp`, `component_wiring.hpp`) reflected in one
`aero_reflect_generate()` call, with a single TEST_CASE proving the generated aggregator registers
both in one call. `entt` (3.16.0, the pinned vcpkg baseline) is the runtime consumer of the generated
output — `find_package(EnTT CONFIG REQUIRED)` → `EnTT::EnTT`, header-only.

## JSON serializer codegen (`--emit-json`) — writer + reader

With `--emit-json`, `aero_reflect_gen` runs the **same detection walk** as `--components`/
`--emit-meta`, then serializes the resulting `Component`/`Field` model as a compilable JSON
serializer translation unit — instead of the AST walk, the `--components` listing, or `entt::meta`
registration. `-o <file>` writes it to `<file>` (text mode, byte-identical to the stdout form on
every OS); the default with no `-o` is stdout. **`--emit-json` and `--emit-meta` are mutually
exclusive** — one `-o` holds one artifact, and requesting both is a usage error (exit `1`), checked
before any parse. `--depfile` works exactly as it does for `--emit-meta`: it requires `-o` and
**one** of `--emit-meta`/`--emit-json`, and is written only after a clean parse and only after the
`-o` write has fully succeeded.

**Task 1.2.2 extended the emitted TU to hold BOTH halves of the pair** — per component, the writer
`aeroWriteJson` (task 1.2.1, byte-identical) immediately followed by a reader `aeroReadJson` (task
1.2.2, new) — rather than adding a second CLI flag or CMake function. Serialization is one ADR-004
consumer whose two halves must never version-skew; one artifact makes a write-only or stale-read
build unrepresentable.

### The generated file's shape

```cpp
// GENERATED by aero_reflect_gen --emit-json — DO NOT EDIT.
// source: component_basic.hpp
#include <aero/reflect/serialize.hpp>

#include "component_basic.hpp"

void aeroWriteJson(engine::JsonWriter& writer, const Transform& value) {
    writer.beginObject();
    writer.key("position");  engine::reflect::writeJson(writer, value.position);
    writer.key("rotation");  engine::reflect::writeJson(writer, value.rotation);
    writer.key("mass");  engine::reflect::writeJson(writer, value.mass);
    writer.key("hitPoints");  engine::reflect::writeJson(writer, value.hitPoints);
    writer.key("active");  engine::reflect::writeJson(writer, value.active);
    writer.endObject();
}

bool aeroReadJson(const engine::JsonValue& json, Transform& value) {
    if (!engine::reflect::expectObject(json, "Transform")) {
        return false;
    }
    bool ok = true;
    ok = engine::reflect::readField(json, "Transform", "position", value.position) && ok;
    ok = engine::reflect::readField(json, "Transform", "rotation", value.rotation) && ok;
    ok = engine::reflect::readField(json, "Transform", "mass", value.mass) && ok;
    ok = engine::reflect::readField(json, "Transform", "hitPoints", value.hitPoints) && ok;
    ok = engine::reflect::readField(json, "Transform", "active", value.active) && ok;
    engine::reflect::warnUnknownKeys(json, "Transform", {"position", "rotation", "mass", "hitPoints", "active"});
    return ok;
}
```

- **One free function PAIR per detected component**: `void aeroWriteJson(engine::JsonWriter&, const
  T&)` and `bool aeroReadJson(const engine::JsonValue&, T&)`. Every supported field becomes one
  uniform writer line and one uniform `readField` line — overload resolution in `engine/reflect`
  routes primitive/`Vec3`/`Quat` automatically, so the emitter never branches on field category
  except supported-vs-skipped.
- **Namespace-wrapped for ADL**: if the component's qualified name has a namespace (e.g.
  `engine::demo::Light`), BOTH generated functions are wrapped in `namespace engine::demo { ... }` so
  ordinary argument-dependent lookup resolves `aeroWriteJson`/`aeroReadJson` at the call site — a
  caller need only forward-declare them in the same namespace (no registry, no `#include` of the
  generated file required at the call site).
- **No aggregator, no register-at-startup concept** (unlike `--emit-meta`/`aero_reflect_generate()`):
  serialization is called directly by name, per type, wherever it is needed — there is nothing to
  auto-register.
- **The reader's contract (D9)**: a non-object root WARNs and returns `false` (value untouched);
  otherwise every supported field is attempted with NO short-circuit (`ok = readField(...) && ok;` —
  the `&& ok` on the RIGHT), so best-effort application means one bad field never blocks the rest. A
  **missing key** leaves that field untouched and does not fail the read (schema evolution — a
  component gaining a field must not brick year-old scene files); a **present-but-unreadable** field
  logs one WARN naming `<QualifiedName>.<field>` and forces the overall result `false` (already-applied
  fields stay applied). **Unknown keys** each log one WARN (`ignoring unknown key "..."`) but never
  fail the read. `null` reads back as a **quiet NaN** for `float`/`double` targets (the mirror of the
  writer's non-finite -> `null`); `null` into anything else fails.
- **Unsupported fields are skipped in BOTH functions, not fatal**: each becomes a `// skipped: <name>
  (<type> — unsupported)` comment (appearing once per function, so twice per field per file) — but
  only ONE stderr warning per run (the writer pass; the reader pass emits no second warning for the
  same field).
- **A zero-field (tag) component** emits a writer with no `key()` line (`{}`) and a reader with no
  `readField(` line, just `expectObject` + an empty `warnUnknownKeys(json, "Tag", {})`. **Zero
  detected components** emits only a `// no engine::component annotations detected` comment — no
  `aeroWriteJson`/`aeroReadJson` at all.
- **Deterministic**: no addresses, timestamps, or absolute paths appear — two runs over the same
  input+flags produce byte-identical output.

### The `engine::reflect` runtime (`engine/reflect`, `aero::reflect`)

The generated calls target a small, hand-rolled runtime — no third-party JSON library, satisfying
the boundary rule by construction:

- **`engine::JsonWriter`** (`<aero/reflect/json_writer.hpp>`) — a streaming, DOM-free JSON writer:
  `beginObject`/`endObject`/`beginArray`/`endArray`/`key`/`value`/`valueNull`, appending straight
  into an internal `std::string` (`str()`). Numbers are shortest-round-trip via `std::to_chars`
  (locale-independent); non-finite floats/doubles serialize as `null` (JSON has no NaN/Inf).
  `JsonWriterConfig` selects pretty (2-space indent, the default) or compact output.
- **`engine::reflect::writeJson`** (`<aero/reflect/serialize.hpp>`) — leaf overloads for `bool`,
  `float`, `double`, `Vec3` (`{"x":..,"y":..,"z":..}`), `Quat` (`{"x":..,"y":..,"z":..,"w":..}`), and
  a constrained template covering every other integral type (widened by signedness so `to_chars`
  never sees a character type; `bool` is excluded so it binds its own exact overload instead).
- **`engine::JsonValue`/`engine::parseJson`** (`<aero/reflect/json_value.hpp>` /
  `<aero/reflect/json_reader.hpp>`, task 1.2.2) — a strict, iterative, depth-capped RFC 8259 parser
  producing an immutable DOM. Numbers are stored as the **validated lexeme** and converted at access
  time at the target's precision (`asF32`/`asF64`/`asI64`/`asU64`) — never pre-parsed to a `double`,
  which would lose a ULP on some `float` lexemes and cannot hold the full `uint64_t` range. Three
  documented tolerances beyond strict RFC 8259: a single leading UTF-8 BOM is skipped; duplicate
  object keys are last-wins; non-key string content is not UTF-8-validated. `JsonParseResult` carries
  either the parsed `JsonValue` or a `JsonParseError` (message + 1-based line/column + 0-based byte
  offset) — no exceptions, no logging (the caller, e.g. an editor console, owns presentation).
- **`engine::reflect::readJson`** (`<aero/reflect/serialize.hpp>`) — the read-side mirror of
  `writeJson`: leaf overloads for `bool`/`float`/`double`/`Vec3`/`Quat` plus the integral template,
  all silent and pure; `readField`/`expectObject`/`warnUnknownKeys` carry the D9 tolerance policy and
  are the only three things in `engine/reflect` that log (via `AERO_LOG_WARN`).
- `engine/reflect`'s `aero_reflect` STATIC library builds **unconditionally** — it is real engine
  code, independent of `AERO_REFLECT_TOOLS` — so it is usable by hand-written code today, not only by
  generated code.

### Build-time wiring: `aero_reflect_generate_json()`

`cmake/reflect.cmake` also defines `aero_reflect_generate_json(<target> HEADERS <hdr>...
[INCLUDE_DIRS <dir>...] [DEFINES <NAME[=VAL]>...])` — a sibling of `aero_reflect_generate()` with the
same per-header `add_custom_command(... DEPFILE ...)` incremental wiring, but running `--emit-json`
and producing `<stem>.json.gen.cpp` (instead of `--emit-meta`/`<stem>.meta.gen.cpp`), and with **no
aggregator TU** (D7 above — there is nothing to register at startup). It tracks its own
`AERO_REFLECT_JSON_WIRED` target property (distinct from `aero_reflect_generate()`'s
`AERO_REFLECT_WIRED`), so a single target may call **both** generators over the same headers.
`-DAERO_REFLECT_TOOLS=OFF` makes it a defined, harmless no-op, exactly like `aero_reflect_generate()`.

`tests/CMakeLists.txt`'s `aero_reflect_json_test` — a sibling of `aero_reflect_meta_test`, but
linking `aero::reflect` instead of `EnTT::EnTT` (serialization is `entt::meta`-independent) — is the
real consumer: four fixtures (the fourth, `component_limits.hpp`, added at task 1.2.2 to pin 64-bit
integer exactness, narrow-integral range checks, and the D11 whitelist skip) reflected in one
`aero_reflect_generate_json()` call, proving the exact, ordered, round-trippable JSON shape —
component -> JSON -> component, byte-equal — end-to-end (Epic 1.2's Definition of Done).

## Local troubleshooting

- **libclang cannot find the C++ standard library / SDK headers, even with a correct `-isysroot`.**
  Because `aero_reflect_gen` is a standalone executable living outside the LLVM install prefix,
  libclang's internal driver cannot locate its own **resource directory** (the compiler-builtin
  headers such as `stdarg.h`, normally found relative to a real `clang` binary's own path) purely
  from `argv[0]` — every parse needs an explicit `-resource-dir <AERO_LLVM_ROOT>/lib/clang/18` (the
  standard LLVM install-tree layout on every platform) alongside `-isysroot`. The `reflect-gen` ctest
  suite computes this for every case that needs it; a manual invocation needs the same flag.
- **macOS: the default SDK may be too new for clang-18.** If this machine's default SDK (`xcrun
  --show-sdk-path`) is newer than what LLVM 18 was built to understand, a heavier parse (anything
  pulling in libc++, e.g. `<type_traits>`) can fail with libc++-internal parse errors even though a
  trivial header (e.g. one that only includes `<cstdint>`) parses fine. If that happens, pin
  `-isysroot` to an older installed SDK instead (for example `xcrun --sdk macosx15.4
  --show-sdk-path`), the same workaround this repo's local `clang-tidy` invocation already needs.
- **Escape hatch:** `-DAERO_REFLECT_TOOLS=OFF` skips the tool and its tests entirely (see above).
- **Override the discovered LLVM install:** `-DAERO_LLVM_ROOT=/path/to/llvm-18` (CMake cache
  variable) or the `AERO_LLVM_ROOT` environment variable (read only when the cache variable is not
  already set).
