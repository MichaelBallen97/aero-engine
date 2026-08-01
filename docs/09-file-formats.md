# 09 — File Formats

> Normative. Scope: the formats Aero Engine reads and writes on disk. Section 2 (scene v1) is
> enforced in code by `engine/reflect`'s scene_format layer (task 1.2.3); the doctest battery in
> `tests/scene_format_test.cpp` is its machine-checkable form.

## 1. Scope & policy

docs/04:51 — *"scene/asset formats may break without migration until v1.0; every format carries a
version field from day one (so post-1.0 migrations are possible without archaeology)."*

Every format defined in this document follows a two-layer strictness split:

| Key class | Policy | Rationale |
|---|---|---|
| Envelope structure (version, entities, id, name, parent, components shapes) | **REJECT**, fail-fast, one deterministic first error, no partial document | A structurally broken file has no meaningful partial load — which entities are even real? |
| Unknown envelope keys | **WARN + ignore** (the additive-evolution path) | Old readers keep loading newer, additively-extended files without a version bump |
| Component payload fields | The task-1.2.2 reader policy, applied at LOAD time: missing key silent, unknown key WARNed, present-but-bad WARNed + best-effort continue | Components evolve independently of the envelope; old scene files must keep loading after a component gains a field |
| Unresolvable component NAME (load time, task 1.4.2) | **WARN + skip** the component, continue the load | The same WARN-and-tolerate philosophy, one level up, applied to names the loader does not (yet) recognize |

**Canonical form.** UTF-8, no BOM, LF newlines, pretty 2-space indent, one trailing newline. Key
order and member-omission rules are per-format (section 2.4 for scenes). Numbers are canonicalized
per section 2.4; strings are escaped by `engine::JsonWriter`'s rules.

**The two round-trip guarantees**, which every format in this document satisfies:

1. Canonical text is **byte-stable** through parse → write: writing a document parsed from
   already-canonical text reproduces it byte-for-byte.
2. For **any** successfully-parsed text (canonical or not), writing is **idempotent**: write →
   parse → write reproduces the first write's bytes exactly.

Guarantee (2) is the one git-mergeable text actually needs day to day: a load → save cycle in the
editor is stable, so diffs stay meaningful and hand edits survive a round-trip.

## 2. Scene format v1

### 2.1 Envelope

| Key | Kind | Required | Range / rule | Default | Error on violation |
|---|---|---|---|---|---|
| `version` (root) | number, integral | yes | must equal `1` | — | missing / wrong kind / wrong value: see section 2.6 |
| `entities` (root) | array | yes | may be empty | — | missing / wrong kind: see section 2.6 |
| `id` (entity) | number, integral | yes | `1 <= id <= 2^64-1`, unique within the file | — | see section 2.6 |
| `name` (entity) | string | no | informational only, duplicates allowed | `""` (≡ absent) | wrong kind: see section 2.6 |
| `parent` (entity) | number, integral | no | must equal some `id` in the same file | `0` (≡ root, no parent) | wrong kind/value, unresolved, or cyclic: see section 2.6 |
| `components` (entity) | object | no | member keys are component type names, member values are payload objects | `{}` (≡ absent) | wrong kind, bad key, bad payload: see section 2.6 |

Unknown keys at root or entity level are **tolerated**: WARNed and ignored on load, and **stripped**
on the next canonical save (a load → save cycle loses them, with a load-time WARN naming each one).

One scene per file. Recommended extension `*.scene.json` — a convention only; loaders never sniff
file names or extensions.

### 2.2 Identity & hierarchy

Entity ids are `u64`, file-scoped, `>= 1`. **`0` is the reserved null/none sentinel** and is never a
valid id — it is the value `parent` takes by omission, never by being written explicitly as `0`.

`parent` references are **forward-reference legal**: an entity may name a parent that is declared
later in the `entities` array. Resolution happens after every id in the file is known. The parent
graph must be a **forest**: self-parenting and any cycle (of any length, including an entity hanging
off a cycle it is not itself part of) are hard errors.

Ids carry **no cross-file or session stability promise** in v1: they are not GUIDs and not runtime
handles. Task 1.4.2's loader maps `id -> live entity` in a load-time table and discards the table once
the scene is instantiated. Asset-grade, cross-file-stable identity is the future `.meta` GUID system's
job (Phase 2+) — upgrading scene entity ids to something GUID-like, if that is ever needed, is exactly
the kind of breaking change the version field exists to gate.

The **save** direction (task 1.4.2's `saveWorld`) assigns file ids `1..N` in the `World`'s entity
iteration order. Entity `name`s **do** persist (task 2.2.1): the `World` stores an optional,
entity-level, purely informational name (`World::setName` / `World::name`) — entity-level exactly
like `parent`, and deliberately **not** a component, so the runtime model matches this document's.
A save emits `name` for every entity that has one and omits the key for every entity that does not,
since `""` ≡ absent (see the table above). Names are never validated, never made unique, and never
interpreted.

### 2.3 Components

The member **key** of a `"components"` object is the reflected type's fully-qualified C++ name,
exactly as `reflect-gen` registers it — the same string `--emit-meta` uses for `.type()` and the
generated `aeroReadJson` readers use in their own WARN messages.

The member **value** is the payload object `aeroWriteJson` emits for that component: one key per
supported field, in declaration order, with unsupported fields simply absent. This document defines
the payload shape **by reference** to the generated serializers (tasks 1.2.1/1.2.2) — it does not
restate field-level shapes. Field kinds reachable through the generated serializers are the
reflectable subset — JSON numbers for primitives, `{x,y,z}`/`{x,y,z,w}` for `Vec3`/`Quat`, and
**from task 2.2.2 a JSON string for `std::string`**, escaped by the writer's rules with non-UTF-8
bytes passed through; purely additive to scene v1 (envelope unchanged, every existing scene byte
still reads and re-emits identically). The field-level tolerance policy those generated readers
apply, restated here for convenience:

| Field condition (at load) | Behavior |
|---|---|
| Key missing from the payload | Silent — the target field is left untouched (schema evolution) |
| Payload has an extra, unrecognized key | WARN, ignored |
| Key present but unreadable (wrong kind/shape) | WARN, that field left untouched, load continues with every other field applied |

Member order is preserved on load and re-emitted unchanged — this keeps hand-edit diffs minimal and
makes task 1.4.2's instantiation order deterministic (file order).

Duplicate component keys in raw text (the same type name appearing twice in one `"components"`
object) collapse **last-wins**, at the JSON layer, before the scene layer ever sees them — inherited
JSON-parser tolerance (documented, not a bug; the scene layer has no way to detect this after the
fact). A **hand-built DOM** (`parseScene(const JsonValue&)` on a root the JSON parser did not
produce) is the one place two members can still share a key — there `parseScene` rejects the
duplicate outright (section 2.6), so no path yields two records of one type on one entity.

### 2.4 Canonicalization notes

**Key order on save**, entity-level: `id`, then `name` **iff non-empty**, then `parent` **iff
non-zero**, then `components` **iff non-empty**. Root-level: `version`, then `entities`. Entities
within `"entities"` and components within a `"components"` object are emitted in **document order**
— the writer never sorts them.

**Numbers.** An integral-form lexeme (no `.`, `e`, or `E`) re-emits exactly, through the exact `i64`
or `u64` value it names; `-0` keeps its sign. Everything else re-emits as the shortest round-trip
double. Two documented lossy corners: an integral-form lexeme beyond the 64-bit range goes through
the double path (may lose precision), and a value that rounds to `+/-inf` (e.g. `1e999`) becomes
`null` — a later typed read then maps that `null` to NaN for a `float`/`double` field, so the chain
stays coherent end to end.

**Strings.** A `\uXXXX`-escaped input string normalizes to raw UTF-8 output on the first write.

### 2.5 Worked examples

Minimal (an empty scene):

```json
{
  "version": 1,
  "entities": []
}
```

Full (`engine::Transform` is now the real generated payload shape, task 1.3.2 — the example's bytes are
already correct; `engine::Camera`/`demo::Marker` stay illustrative until task 1.3.3; the envelope never
resolves any of them):

```json
{
  "version": 1,
  "entities": [
    {
      "id": 1,
      "name": "camera",
      "components": {
        "engine::Transform": {
          "position": {
            "x": 0,
            "y": 2.5,
            "z": -10
          },
          "rotation": {
            "x": 0,
            "y": 0,
            "z": 0,
            "w": 1
          }
        },
        "engine::Camera": {
          "fovDegrees": 60,
          "nearPlane": 0.1,
          "farPlane": 100
        }
      }
    },
    {
      "id": 2,
      "name": "crate",
      "components": {
        "engine::Transform": {
          "position": {
            "x": 0,
            "y": 0,
            "z": 0
          }
        }
      }
    },
    {
      "id": 3,
      "name": "lamp",
      "parent": 2,
      "components": {
        "demo::Marker": {}
      }
    },
    {
      "id": 4
    }
  ]
}
```

(Both examples end in one trailing newline in their canonical, on-disk form.) Entity 3 shows a
`parent` reference; entity 4 is a minimal, componentless entity; the `demo::Marker` payload shows a
tag component's empty-object form. Both examples are byte-pinned by `tests/scene_format_test.cpp`.

### 2.6 Error catalog

Checked in this order; the first violation wins (fail-fast, one error, no partial document). `line`/
`column`/`offset` are non-zero **only** for JSON-stage errors (malformed JSON text itself); every
scene-stage error carries zeros and puts its context (`entities[<i>]`, ids) directly in the message
text.

| Stage | Message |
|---|---|
| JSON | *(passed through verbatim from the JSON parser, with position)* |
| Envelope | `scene root must be a JSON object (found <kind>)` |
| Envelope | `missing required key "version"` |
| Envelope | `"version" must be an integer (found <kind-or-lexeme>)` |
| Envelope | `unsupported scene format version <N> (this build reads version 1)` |
| Envelope | `missing required key "entities"` |
| Envelope | `"entities" must be an array (found <kind>)` |
| Entity | `entities[<i>] must be an object (found <kind>)` |
| Entity | `entities[<i>]: missing required key "id"` |
| Entity | `entities[<i>]: "id" must be an integer >= 1 (found <kind-or-lexeme>)` |
| Entity | `entities[<i>]: duplicate entity id <id> (first used by entities[<j>])` |
| Entity | `entities[<i>] (id <id>): "name" must be a string (found <kind>)` |
| Entity | `entities[<i>] (id <id>): "parent" must be an integer >= 1 (found <kind-or-lexeme>)` |
| Entity | `entities[<i>] (id <id>): "components" must be an object (found <kind>)` |
| Component | `entities[<i>] (id <id>): component name must be a non-empty string` |
| Component | `entities[<i>] (id <id>): component "<type>" payload must be an object (found <kind>)` |
| Component | `entities[<i>] (id <id>): duplicate component type "<type>"` (see note) |
| Hierarchy | `entities[<i>] (id <id>): parent <p> does not reference any entity id in this scene` |
| Hierarchy | `entities[<i>] (id <id>): parent chain is cyclic` |

**Note (duplicate component type):** text input can never trip this line in `parseScene` — the JSON
object form of `"components"` already collapses a duplicate key (last-wins, section 2.3) before the
scene layer sees the document. It fires in exactly two places: `parseScene(const JsonValue&)` over a
**hand-built DOM** whose `"components"` object carries two members with the same key (the JSON
parser cannot produce that shape), and `validateScene` over a hand-built (not-yet-serialized)
`SceneDocument` — e.g. a future editor pre-save hook — whose `std::vector` carries two records of
one type. Both paths enforce the same ≤1-component-per-type invariant.

Success-only warnings (document order, `"scene: "`-prefixed):

| Warning |
|---|
| `scene: ignoring unknown key "<k>"` (root) |
| `scene: entities[<i>] (id <id>): ignoring unknown key "<k>"` (entity) |

### 2.7 Golden fixtures

Three scene files under `tests/fixtures/scenes/` are **content pins** for this format:

| File | Pins |
|---|---|
| `empty.scene.json` | the degenerate document — an empty `entities` array |
| `full.scene.json` | 8 entities, 10 components: all five built-in component types, a **forward** `parent` reference, a three-level chain, an entity with no `name` key, an entity with no `components` key, and two entities sharing one name |
| `edge.scene.json` | 4 entities, 3 components: the lexical corners — `-0`, two `null` payloads, both exponent signs, a full-precision decimal, all five string-escape classes, and 2-, 3- and 4-byte raw UTF-8 |

Each is an exact fixpoint of the writer — `saveWorldText(load(bytes)) == bytes`, and again on a
second cycle. **Nothing regenerates them.** There is no environment variable, CMake option, target or
script that can update one; a deliberate format change is a hand edit, reviewed in a diff, landed in
the same commit as the change to this document. The doctest batteries in
`tests/scene_serialize_test.cpp` (engine: text → `World` → text) and
`tests/editor/scene_golden_test.cpp` (editor: file → `World` → file, through the editor's own atomic
read/write pair) are their machine-checkable form, and they assert structure and semantics alongside
the bytes: bytes-in == bytes-out is necessary, never sufficient — a load/save pair that *both* stopped
handling a key would agree with itself.

**Non-finite floats, at the component layer.** §2.4 states the rule for JSON *lexemes*; this is its
typed consequence, and it is asymmetric. A `float` field holding NaN **or ±infinity** writes as
`null`, and reading `null` back yields **NaN**. So an infinity is lossy exactly once and byte-stable
from the first write onward, while a negative zero survives intact as the lexeme `-0`.
`edge.scene.json` pins all three cases end to end through a real component.

**String escaping, exhaustively.** A quote becomes `\"`, a backslash `\\`, a tab `\t`, a newline
`\n`; every other C0 control becomes `\uXXXX`. Multi-byte UTF-8 is emitted **raw**, never
re-escaped, and a `\uXXXX`-escaped input normalizes to raw UTF-8 on the first write (§2.4). Names are
never validated and never interpreted (§2.2), so a control character in a name is legal and
round-trips.

**Entity order is storage order, and a delete reorders the file.** §2.2 says a save assigns file ids
`1..N` in "the `World`'s entity iteration order". That order is the entity storage's **packed** order,
and destroying an entity is swap-and-pop: the last live entity moves into the destroyed entity's
slot. Consequently a save taken after deleting one entity **renumbers every later id and reorders the
entities**, and a `parent` reference that was forward can become backward. A one-entity edit
therefore produces a whole-file diff. This is the behaviour today and it is pinned by a test.
Stabilizing entity order across edits is a format-level decision — a stable sort key, a hierarchy
walk, or persistent per-entity ids — with a migration question attached and an interaction with the
future `.meta` GUID system (§2.2); it is unowned, and should be scoped before Phase 3's asset
database makes scene diffs a daily concern.

## 3. Versioning & evolution

The `"version"` key is validated **first**, before any structural check — so a future-format file
fails with `unsupported scene format version <N> (this build reads version 1)` rather than pages of
bogus structural complaints about a schema it never claimed to follow.

Additive optional keys at root or entity level do **not** bump the version — old readers WARN and
ignore them (the soft-forward path). Any breaking change (semantics, required keys, payload meaning)
bumps it.

Pre-1.0: the format may break freely with a version bump and **no migration** (docs/04:51).
Post-1.0: loaders migrate oldest → newest; the migration machinery itself is deferred until a v2
actually exists — building it speculatively, before there is a second version to migrate from, is
explicitly out of scope.

A load-then-save cycle **strips unknown keys** (with load-time WARNs naming each one) — acceptable
pre-1.0, and safely gated by the version check: a v2 file never successfully reaches a v1 save, since
it would have failed the version gate on load.

## 4. Project format v1

> Enforced in code by `editor/src/project.cpp` (the pure parse/write/validate half) and
> `editor/src/project_file.cpp` (the `<filesystem>`-and-SDL half); `tests/editor/project_test.cpp`
> (57 cases, task 2.6.1 plus its code-review round) is its machine-checkable form, including a
> three-fixture golden battery (§4.8) mirroring section 2.7's design.

### 4.1 Envelope

One `project.json` per project, at the project root. **Five root keys, in this exact order on
save**: `version`, `name`, `engineVersion`, `language`, `paths`; `paths` has exactly two members,
`assets` then `scenes`.

| Key | Kind | Required | Range / rule | Default | Error on violation |
|---|---|---|---|---|---|
| `version` (root) | number, integral | yes | must equal `1` | — | missing / wrong kind / wrong value: see §4.7 |
| `name` (root) | string | yes | validated by §4.3 | — | missing / wrong kind: see §4.7 |
| `engineVersion` (root) | string | yes | informational only, never compared for ordering, never a gate | — | missing / wrong kind: see §4.7 |
| `language` (root) | string | yes | `"ts"` or `"cpp"` | — | missing / wrong kind / unsupported value: see §4.7 |
| `paths` (root) | object | yes | exactly `assets` and `scenes`, both required | — | missing / wrong kind: see §4.7 |
| `paths.assets` (paths) | string | yes | a legal project-relative path (§4.4) | `"assets"` | missing / wrong kind / illegal: see §4.7 |
| `paths.scenes` (paths) | string | yes | a legal project-relative path (§4.4) | `"scenes"` | missing / wrong kind / illegal: see §4.7 |

`version` is validated **first**, before any other key — a file also missing `name` still reports the
version error, mirroring §2.6/§3's "first violation wins" and "future-format files fail fast" rules.

Unknown keys, at root or nested inside `paths`, are **tolerated**: WARNed (one WARN per key, by the
caller — this format's parser never logs, INV-P6) and collected in **true document order**. A nested
`paths.<unknown>` key is collected at the position `paths` itself occupies among the root members, not
after every root-level unknown regardless of where `paths` sits — one depth-first walk, not two
separate sweeps. Stripped on the next canonical save, exactly like §2.1's envelope-level unknowns.

### 4.2 Language

`ProjectLanguage` is `"ts"` (TypeScript) or `"cpp"` (C++), fixed at creation and never mixed
(ADR-008). New Project always writes `"ts"` in this build; `"cpp"` is accepted on read so a
hand-edited or future-tool-authored file opens correctly, and enforcing the C++ project workflow
itself is a later task's job — purely additive over a field that already round-trips. Any other value
is a hard reject, never a silent fallback.

### 4.3 Project name validation

Checked in this exact order, the first violation wins (mirrors §2.6's fail-fast discipline). This
table describes `validateProjectName`'s **actual behavior**, corrected from an earlier draft that
over-stated it in three rows (code-review round, task 2.6.1) — the code was already spec-correct
against D6 ("no trailing space and no trailing `.`"); only the doc was wrong:

| Order | Problem | Rule |
|---|---|---|
| 1 | Empty | zero bytes remain after **trimming** ASCII spaces/tabs from both ends of the name — this trim exists ONLY to decide this one rule; the untrimmed name is what every later rule (including 6) checks |
| 2 | TooLong | more than 64 UTF-8 bytes, measured on the **original, untrimmed** name |
| 3 | Separator | contains `/` or `\` anywhere in the original name |
| 4 | DotName | is exactly `.` or `..` |
| 5 | IllegalChar | contains any of `<>:"\|?*` or a C0 control byte. `/` and `\` are deliberately absent from this set — rule 3 (Separator) already catches both first, so this rule can never fire for either |
| 6 | TrailingSpaceOrDot | the **original, untrimmed** name ends with a space or a `.` — checked on `back()` only. **A leading space is legal and reaches no rule at all**: `" Foo"` is `Ok` and scaffolds a directory literally named `" Foo"`. This is deliberate, not an oversight — D6 only ever asked for a trailing check |
| 7 | ReservedDeviceName | is (ASCII-case-insensitively, stem before the first `.`) one of the 22 reserved DOS device names: `CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9` |

All seven rules apply on **every** OS unconditionally — there is no `#if defined(_WIN32)` anywhere;
a name illegal on Windows is refused even when creating on macOS or Linux, so a project directory
never becomes unusable after being copied to a different platform.

### 4.4 Path fields

`paths.assets`/`paths.scenes` are checked with a **pure string** rule, deliberately never
`std::filesystem::path::is_absolute()` (`is_absolute("/shared")` is `false` on Windows — no root
name — so a POSIX-rooted value would sail through that check): non-empty, no leading `/`, no `\`
anywhere, no `:` anywhere (covers `C:` and `C:/x`), no `..` segment. A `.` segment is legal. Both
default to their own bare name (`"assets"`/`"scenes"`) when creating a new project.

### 4.5 Worked example

The exact bytes of `minimal.project.json` (152 bytes, one trailing newline, `tests/fixtures/projects/`)
— what `createProject`/New Project writes for a fresh TypeScript project:

```json
{
  "version": 1,
  "name": "MyGame",
  "engineVersion": "0.1.0",
  "language": "ts",
  "paths": {
    "assets": "assets",
    "scenes": "scenes"
  }
}
```

Byte-pinned by `tests/editor/project_test.cpp`'s golden battery (PG4, §4.8); regenerating it from a
build that silently changed the writer is exactly what that battery exists to catch.

### 4.6 Canonicalization notes

Same canonical-form policy as §1: UTF-8, no BOM, LF newlines, pretty 2-space indent, one trailing
newline. Key order is fixed (§4.1); members within `paths` are always `assets` then `scenes`, never
sorted or reordered by content. `New Project` writes exactly the same bytes `writeProjectText` would
produce from any other manifest carrying the same field values — there is no second code path.

### 4.7 Error catalog

| Stage | Message |
|---|---|
| JSON | *(passed through verbatim from the JSON parser, with position)* |
| Envelope | `project root must be a JSON object (found <kind>)` |
| Envelope | `missing required key "version"` |
| Envelope | `"version" must be an integer (found <kind-or-lexeme>)` |
| Envelope | `unsupported project format version <N> (this build reads version 1)` |
| Envelope | `missing required key "name"` |
| Envelope | `"name" must be a string (found <kind>)` |
| Envelope | `missing required key "engineVersion"` |
| Envelope | `"engineVersion" must be a string (found <kind>)` |
| Envelope | `missing required key "language"` |
| Envelope | `"language" must be a string (found <kind>)` |
| Envelope | `unsupported language "<value>" (expected "ts" or "cpp")` |
| Envelope | `missing required key "paths"` |
| Envelope | `"paths" must be an object (found <kind>)` |
| Paths | `missing required key "paths.assets"` / `"paths.scenes"` |
| Paths | `"paths.assets"/"paths.scenes" must be a string (found <kind>)` |
| Paths | `"paths.assets"/"paths.scenes" must be a non-empty relative path with no leading '/', no '\', no ':' and no ".." segment (found "<value>")` |

Success-only warnings (document order, caller-emitted — the parser itself never logs):

| Warning |
|---|
| `editor: project '<root>' -- ignoring unknown key "<k>"` |
| `editor: project '<root>' was created with engine version <manifest-version> (this build is <build-version>)` — D14: informational only, never a gate, and never emitted when the caller's own build-version string is empty (a test context, by convention) |

### 4.8 Golden fixtures

Three project files under `tests/fixtures/projects/` are **content pins** for this format, mirroring
§2.7's design exactly — no self-update flag, no environment variable, no target, no regeneration path
whatsoever:

| File | Pins |
|---|---|
| `minimal.project.json` | the exact bytes `createProject`/New Project writes for a fresh TypeScript project |
| `full.project.json` | nested `paths` content (`content/art`, `content/levels`), `language: "cpp"`, a non-ASCII name (`Café Rocket 🚀`) |
| `unknown-keys.project.json` | one root-level unknown key nested inside `paths` (`prefabs`) plus two root-level unknowns (`author`, `editorLayout`), pinning the document-order collection rule of §4.1 |

**Bytes are necessary and never sufficient — the same lesson §2.7 and task 2.5.2's S12 seed
established, re-proven for this format by sabotage seed S9 (task 2.6.1).** A `parseProject`/
`writeProjectText` pair that both stopped handling one key (`paths.scenes`) agreed with itself and
passed every byte comparison the moment the affected fixture was regenerated from the buggy build —
and stayed caught only by the semantic cases that read a fixture or a freshly-created project
directly (`PG5`–`PG7`), never by the cases that re-derive bytes and compare them. The battery
therefore always pairs byte-fixpoint cases with at least one semantic case that reads the fixture (or
a real `createProject` outcome) without re-deriving it — never bytes alone.

### 4.9 The recent-projects envelope

A small, separate versioned format at `recent_projects.json` (under `SDL_GetPrefPath`, not next to
the executable — it is *user* state that must survive rebuilding the editor into a fresh `build/`
directory, unlike `aero_editor.ini`): `{"version": 1, "projects": [<absolute path>, ...]}`. Same
canonical form as §4.6. Capped at 10 entries, newest-first, deduplicated on normalized bytes with no
case folding (two paths differing only in case get two rows — dedup is not the place to guess a
filesystem's case sensitivity). Never auto-pruned: a missing/renamed/unmounted target is left for
"Clear Recent Projects" to remove explicitly, since the destructive interpretation of a transient
`exists() == false` is the irreversible one. A corrupt, wrong-version or malformed file is tolerated —
one WARN, an empty list, the editor keeps running — with one exception: an individual non-string
array element is skipped and the rest of the list is kept, so one corrupt row never costs the whole
file. No golden fixtures; this format has no byte-stability requirement any caller depends on.

## 5. Reserved for future formats

- **`.meta` (asset GUID sidecars)** — Phase 2. Section appends here.
- **Cooked / `.pak` binary formats** — Phase 3+, owned by the cooker; own version field, docs/04:51
  applies unchanged. Section appends here.
