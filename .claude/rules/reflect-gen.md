---
paths:
  - "tools/reflect-gen/**"
  - "cmake/reflect.cmake"
  - "engine/reflect/**"
  - "engine/scene_serialize/**"
  - "tests/reflect-gen/**"
---

# reflect-gen — the reflection pipeline

`tools/reflect-gen` parses `AERO_COMPONENT`-annotated structs with **libclang 18's C API
only** (`<clang-c/Index.h>` — never a C++ Clang/LLVM header) and emits generated code.
One TU: `tools/reflect-gen/src/main.cpp`. Every mode so far has been additive to it.

CLI dispatch precedence: `--emit-json` > `--emit-meta` > `--components` > raw AST walk.
`--emit-json` and `--emit-meta` are mutually exclusive (one `-o` holds one artifact).
`--depfile` requires `-o` **and** one of the two emit modes.

## The annotation

A bare `[[engine::component]]` is **discarded by Clang** — no attribute cursor survives,
only a `-Wunknown-attributes` warning. So annotations go through macros in
`<aero/reflect/annotations.hpp>` (`AERO_COMPONENT`, `AERO_RANGE(min,max)`, `AERO_COLOR`)
which expand to `[[clang::annotate(...)]]` **only** when `AERO_REFLECT_PARSE` is defined,
and to nothing under the real compiler. The tool auto-injects `-DAERO_REFLECT_PARSE=1`;
no caller manages the marker.

## The reflectable subset — extend it only by exact match

Field classification runs on the **canonical** type: an explicit **18-`CXTypeKind`
whitelist** for primitives, plus `engine::Vec3`, `engine::Quat`, `std::string` and —
since task 3.1.5 — `engine::Guid`. Deliberately excluded: `long double`, `__int128`,
`unsigned __int128` (no viable `serialize.hpp` overload — they used to generate
non-compiling TUs), and enums.

**Match canonical spellings exactly, never by prefix.** `std::string` is compared against
three exact spellings (`std::string`, `std::basic_string<char>`, the fully-qualified
default-allocator form) because a prefix match would also accept an allocator-customized
`basic_string` that has no overload. A fourth unknown spelling then fails **loud and
safe** — `[unsupported]`, one warning, exit 0 — never a miscompile.

`engine::Guid` needs none of that: it is a plain struct at namespace scope with no
template parameters, no inline-namespace ambiguity and no preferred-name alias, so it has
exactly **one** spelling on all three hosts. Do not add a second one pre-emptively.

**Neither emitter gains a branch when the subset grows.** `--emit-meta` writes the same
`.data<&T::member>(...)` line and `--emit-json` the same
`writeJson(writer, value.member)` / `readField(json, ..., value.member)` lines whatever
the category is — C++ overload resolution routes them, so extending the subset is one
`classifyField` arm, one `categoryTag` arm, and one `serialize.{hpp,cpp}` overload pair.
The three "not in the reflectable subset" warning strings must be updated **together**;
`git grep -c 'Vec3/Quat/Guid/std::string' -- tools/reflect-gen/src/main.cpp` reads **3**.

Unsupported fields are lenient, not fatal: collected, tagged `[unsupported]`, warned on
stderr, exit stays 0.

**`--components` prints the AS-WRITTEN type spelling**, not the canonical one
(`std::uint32_t`, not `unsigned int`; `Vec3`, not `engine::Vec3`, inside `namespace
engine`). Only the classification tag is canonical. Pin test expectations accordingly.

`--emit-json` **skips** any component not at namespace scope — wrapping a class name in
`namespace` produces an uncompilable TU. `--emit-meta` is nest-safe by construction.

## Generated code

- Never committed. It lands under `build/<preset>/**/reflect-generated/` and is outside
  every `git ls-files` and lint glob.
- Generation is **build-time**, never configure-time — `aero_reflect_gen` does not exist
  until the same build produces it.
- Registration is an **explicitly named, caller-invoked** function
  (`aero_reflect_register_<stem>`), never static-init auto-registration: a static lib
  would dead-code-eliminate it.
- That function name is a **frozen snake_case cross-boundary contract**, so its forward
  declarations need `// NOLINTNEXTLINE(readability-identifier-naming)`. These NOLINTs are
  correct — do not "fix" them.
- Use `aero_reflect_generate()` / `aero_reflect_generate_json()` from
  `cmake/reflect.cmake`. Both `FATAL_ERROR` at configure if the tool target does not yet
  exist, which is why `add_subdirectory(tools)` precedes `engine/` in the root CMakeLists.
- Everything is gated on `AERO_REFLECT_TOOLS`; `OFF` must stay green, with consumers
  self-skipping.

Serialization's two halves (`aeroWriteJson` / `aeroReadJson`) live in the **same**
generated file on purpose — they must never version-skew.

Full history: `docs/10-engineering-log.md`, Epic 1.1 / 1.2 entries.
