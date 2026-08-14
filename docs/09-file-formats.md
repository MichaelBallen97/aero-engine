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
the scene is instantiated. Asset-grade, cross-file-stable identity is task 3.1.1's `.meta` GUID
system's job (section 5) — upgrading scene entity ids to something GUID-like, if that is ever needed,
is exactly the kind of breaking change the version field exists to gate.

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
`.meta` GUID system (§2.2, §5). **Answered inline rather than deferred further (task 3.1.1):** it
stays **unowned**. Phase 3's `AssetDatabase` (§5) gives *asset* files a stable cross-machine identity;
it does not touch scene entity ids, and this task deliberately does not adopt that open question just
because it landed in the same phase (2.2.5's lesson: do not attach an unrelated open item to a task
that already has its own full surface). It remains scoped for whoever picks it up next.

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

## 5. Asset meta format v1

> Enforced in code by `editor/src/asset_meta.cpp` (the pure parse/write/lifecycle half) and
> `editor/src/asset_database.cpp` (the `<filesystem>`-and-disk half, task 3.1.1);
> `tests/editor/asset_meta_test.cpp` and `tests/editor/asset_database_test.cpp` are its
> machine-checkable form, including a three-fixture golden battery (§5.7, task 3.2.1's
> `importer-settings.meta` the third) mirroring §4.8's design.

### 5.1 Envelope

One `.meta` per source asset, named `<full source file name>.meta`, in the same directory. Up to three
root keys, in this exact order on save.

| Key | Kind | Required | Rule | Error |
|---|---|---|---|---|
| `version` | number, integral | yes | must equal `1` | §5.4 |
| `guid` | string | yes | exactly 32 hex digits, any case; must not be nil | §5.4 |
| `importer` | object | no | additive at v1, task 3.2.1 -- see §5.9 | §5.4 |

`version` is validated first. Unknown root keys are WARNed (by the caller) and ignored, collected in
document order.

### 5.2 Identity

The GUID is 128 random bits, minted once when the asset is first discovered and never changed. It is
**not** an RFC 4122 UUID and carries no version or variant bits. The all-zero value is the reserved
none sentinel and is never valid inside a `.meta`. Canonical text is 32 lowercase hex digits, high 64
bits first; readers accept any case, writers always emit lowercase.

### 5.3 Lifecycle

> A `.meta` is written in exactly two situations: when an asset has none, and when two assets claim
> one GUID. A valid `.meta` is never rewritten. An invalid one is never overwritten. A `.meta` whose
> asset is gone is never deleted. Identity is preserved across a move or rename by the sidecar
> travelling with the asset; an editor operation that moves an asset must move its sidecar in the same
> operation.

### 5.4 Error catalog

Mirrors §4.7's shape; JSON-stage errors pass through with position, every other stage carries zeros.

| Stage | Message |
|---|---|
| JSON | *(verbatim from the JSON parser, with position)* |
| Envelope | `asset meta root must be a JSON object (found <kind>)` |
| Envelope | `missing required key "version"` |
| Envelope | `"version" must be an integer (found <kind-or-lexeme>)` |
| Envelope | `unsupported asset meta format version <N> (this build reads version 1)` |
| Envelope | `missing required key "guid"` |
| Envelope | `"guid" must be a string (found <kind>)` |
| Envelope | `"guid" must be 32 hexadecimal digits (found "<value>")` |
| Envelope | `"guid" must not be the nil GUID` |

Success-only warnings (document order, caller-emitted; the parser never logs):

| Warning |
|---|
| `editor: asset meta '<relative path>' -- ignoring unknown key "<k>"` |

The same warning covers a `k` nested inside the `importer` block (§5.9): a key the parser did not
recognise there is reported with its DOTTED PATH, e.g. `"importer.foo"` or `"importer.settings.foo"`,
through this identical mechanism -- never a second warning shape.

**Importer-block messages (task 3.2.1, §5.9) — non-fatal, returned directly by `parseMeta`, never
touching `error`/`guid`/`unknownKeys` above.** First violation wins, in the order shown; a JSON kind is
`<kind>`, and a number that fails a *form* rule (non-integer, non-finite) instead shows its own
quoted lexeme, exactly like `<kind-or-lexeme>` above.

| Message |
|---|
| `"importer" must be a JSON object (found <kind>)` |
| `missing required key "importer.name"` |
| `"importer.name" must be a string (found <kind>)` |
| `missing required key "importer.version"` |
| `"importer.version" must be an integer (found <kind-or-lexeme>)` |
| `missing required key "importer.settings"` |
| `"importer.settings" must be a JSON object (found <kind>)` |
| `missing required key "importer.settings.scale"` |
| `"importer.settings.scale" must be a finite number (found <kind-or-lexeme>)` |
| `missing required key "importer.settings.importMaterials"` |
| `"importer.settings.importMaterials" must be a boolean (found <kind>)` |
| `missing required key "importer.settings.importAnimations"` |
| `"importer.settings.importAnimations" must be a boolean (found <kind>)` |
| `missing required key "importer.settings.importSkins"` |
| `"importer.settings.importSkins" must be a boolean (found <kind>)` |

### 5.5 Worked example

The exact **65 bytes** a creation writes:

```json
{
  "version": 1,
  "guid": "a3f1c07e5b8d42198e6f0c3d7a2b4b92"
}
```

### 5.6 Canonicalization

§1's policy, unchanged. Key order fixed, never sorted by content.

### 5.7 Golden fixtures

`tests/fixtures/assets/` — content pins with **no regeneration path whatsoever** (§2.7/§4.8's design,
third application), plus §4.8's standing warning restated: bytes are necessary and never sufficient,
so every byte-fixpoint case is paired with a semantic case that reads the fixture without re-deriving
it. `importer-settings.meta` (task 3.2.1) pins the §5.9 envelope with a **non-default** block, byte for
byte, alongside `minimal.meta` (the two-key envelope) and `unknown-keys.meta` (the unrecognised-key
case, whose own `importer` value is a bare string — MALFORMED, not a §5.9 block at all, so it stays an
ordinary unknown key exactly as it did before task 3.2.1).

### 5.8 Scope of v1

Directories carry no `.meta`. Content hashes and dependency records did NOT arrive here: task 3.1.2
put them in §6's machine-local import cache instead, and this prediction was wrong. A hash in a
committed file would need a third write path (repealing §5.3's never-rewrite rule), would dirty two
files on every save, and would put unresolvable merge conflicts on derived data. **Import settings
arrived at task 3.2.1**, as §5.9's additive, optional `importer` block — user intent, committed to git,
surviving a machine change, exactly as this section previously predicted; §6's `metaHash` already
invalidates an import when they change.

### 5.9 The optional `importer` block (task 3.2.1)

An OPTIONAL third root key, additive at format version 1 — **never** a version bump. §5.3's
never-rewrite rule protects a *valid* `.meta`, but a version bump would still nil every *other* asset's
identity on an older build the moment it reads any v2 sidecar (`parseMeta` rejects an unrecognised
`version` outright, and §5.3 then forbids repairing the result) — an additive optional key degrades
instead: an older build reads the GUID correctly, warns once, and loses only the settings, which it
could not have honoured anyway. Present only when an asset's import settings differ from its
importer's defaults; omitted entirely otherwise, so a freshly-created sidecar is unaffected and §5.5's
65-byte worked example stays exact.

| Key | Kind | Required (once `importer` is present) | Rule |
|---|---|---|---|
| `importer.name` | string | yes | the importer's registered identity, e.g. `"gltf"` or `"fbx"` |
| `importer.version` | number, integral | yes | the importer's own format version at write time |
| `importer.settings` | object | yes | four keys, below |
| `importer.settings.scale` | number | yes | a user-intent multiplier; honoured even if zero or negative — NEVER clamped |
| `importer.settings.importMaterials` | boolean | yes | |
| `importer.settings.importAnimations` | boolean | yes | |
| `importer.settings.importSkins` | boolean | yes | |

**Engagement, not validity, is the signal.** A present-but-malformed block (a missing key, a wrong
type, a non-finite `scale`) never invalidates the asset's identity — `version` and `guid` are
unaffected and the parse still succeeds. It surfaces only through two fields a caller reads together:
`importer` disengaged (no value) and `importerMessage` non-empty (§5.4's importer-block catalog). This
is the one asymmetry in the whole format: everywhere else a malformed root key is fatal; here it costs
a user their settings, never their asset.

**A malformed `importer` key is reported exactly like any other unrecognised root key** (§5.1's
unknown-key rule) — it is not a key this build actually understood, so it is not exempted from that
list. Only a block that *engages* (every required field present and well-typed) is excluded from it;
an unknown key nested inside the block, or inside `settings`, is reported as a DOTTED PATH through the
same success-only warning §5.4 defines at the root, and does not itself stop the block from engaging.

**Forward compatibility.** A `.meta` carrying this block, read by an older build with no notion of
`importer` at all, still reads the GUID correctly and reports `importer` as a single unknown-key
warning — §5.1's additive-evolution guarantee, applied to this key.

## 6. Asset import cache index v1

> Enforced in code by `editor/src/asset_cache.cpp` (the pure parse/write/change-detection half, task
> 3.1.2); `tests/editor/asset_cache_test.cpp` is its machine-checkable form, including a three-fixture
> golden battery (§6.7) mirroring §5.7's design.

### 6.1 Nature

Derived, machine-local, disposable. One file per project at `<projectRoot>/Library/asset-cache.json`,
never committed, never merged, never compared across machines. **This section's strictness policy is
deliberately the INVERSE of §5's** — §6.3 states the contrast in full.

### 6.2 Envelope

Three root keys, in this exact order on save.

| Key | Kind | Required | Rule |
|---|---|---|---|
| `version` | number, integral | yes | must equal `1` |
| `hashAlgorithm` | string | yes | must equal `"murmur3-x64-128"` |
| `entries` | array of objects | yes | see below; sorted by `guid` on write |

`version` is validated first, then `hashAlgorithm`, then `entries`. Ten entry keys, in this exact
order on save.

| Key | Kind | Required | Rule |
|---|---|---|---|
| `guid` | string | yes | exactly 32 hex digits, any case; **must not be nil** (the only field this format forbids nil for) |
| `path` | string | yes | project-relative, informational; a move updates it without invalidating the entry |
| `size` | number, integral | yes | bytes observed at the scan that produced this entry |
| `mtime` | number, integral | yes | OPAQUE, machine-local `file_time_type` ticks — see §6.5 |
| `contentHash` | string | yes | exactly 32 hex digits, any case; **MAY be all-zero** — the empty file's digest is a legitimate value, never rejected |
| `metaHash` | string | yes | same rule as `contentHash`, over the whole sidecar's bytes |
| `importer` | string | no, default `""` | "" until 3.2 registers an importer |
| `importerVersion` | number, integral | no, default `0` | |
| `dependencies` | array of strings | no, default `[]` | each element exactly 32 hex digits |
| `missing` | number, integral | no, default `0` | consecutive scans this entry's asset was absent (§6.6's grace) |

Canonical form is §1's, unchanged: UTF-8, no BOM, LF, 2-space indent, one trailing newline. Optional
keys are **always written**, never omitted when defaulted, so a reader never has to distinguish
absent from default.

### 6.3 Strictness

The envelope (`version`/`hashAlgorithm`/`entries`) is checked exactly like §5's: the first violation
discards the WHOLE document, and the next successful scan overwrites the file atomically — **nothing
in this format is ever deleted from disk: a discard means do not carry the entries forward.** Below
that, the policy inverts: a malformed **entry** is dropped silently (counted, never named) and every
other entry survives, and an unknown key at **either** level — root or entry — is ignored with no
warning of any kind, because a newer build wrote it and this build cannot act on it. §5's `.meta`
takes the opposite position on unknown keys (WARN + ignore) because a `.meta` is a committed,
human-visible file a user might want to know about; this index is neither.

### 6.4 Identity vs. content

The `guid` is §5's identity: stable, committed, path-independent. The `contentHash` is this section's
fingerprint: unstable by design, machine-derivable, never committed. Two different 128-bit values
with different lifetimes; never substitute one for the other.

### 6.5 The mtime field, and why it is a number

`mtime` is an opaque `file_time_type` tick count, never a calendar date and never comparable across
machines or filesystems. It is a JSON **number**, not a string, because this tree's JSON DOM
(`engine::JsonValue`) stores a number's validated lexeme verbatim and converts at access time, so a
19-significant-digit tick count (`> 2^53`) round-trips through `std::to_chars`/`std::from_chars`
exactly — a future parser that pre-parses numbers to `double` would silently corrupt every value past
`2^53`. This is F1's dependency, made explicit: the minimal golden fixture (§6.7) pins its `mtime` at
19 digits on purpose, so that regression is caught the moment it is introduced.

### 6.6 Canonicalization

Entries sorted by `guid`; key order fixed at both levels; never sorted by content. Determinism is
load-bearing — the index is written only when its text differs from the text it was read from (task
3.1.2's D15), so a non-deterministic writer would reintroduce a per-scan write.

### 6.7 Golden fixtures

`tests/fixtures/assets/cache-minimal.json`, `cache-dependencies.json` and `cache-damaged.json` —
content pins with **no regeneration path whatsoever** (§5.7's design, a second application), plus the
standing warning restated: bytes are necessary and never sufficient, so every byte-fixpoint case is
paired with a semantic case that reads the fixture without re-deriving it.

### 6.8 Scope of v1

No cooked artifacts (a future task adds an `artifacts` key, additively). No per-importer settings
(§5's `.meta` owns user intent once 3.2 defines it). No cross-project sharing, no shared cache server,
no artifact deduplication by hash.

### 6.9 Error catalog

Envelope errors discard the whole document.

| Stage | Message |
|---|---|
| JSON | *(verbatim from the JSON parser, with position)* |
| Envelope | `asset cache root must be a JSON object (found <kind>)` |
| Envelope | `missing required key "version"` |
| Envelope | `"version" must be an integer (found <kind-or-lexeme>)` |
| Envelope | `unsupported asset cache format version <N> (this build reads version 1)` |
| Envelope | `missing required key "hashAlgorithm"` |
| Envelope | `"hashAlgorithm" must be a string (found <kind>)` |
| Envelope | `unsupported hash algorithm "<X>" (this build writes "murmur3-x64-128")` |
| Envelope | `missing required key "entries"` |
| Envelope | `"entries" must be an array (found <kind>)` |

A per-**entry** failure is silent and counted (`droppedEntries`/`droppedDependencies`), never a
message — §6.3's inversion of §5's policy.

## 7. Blender export provenance record v1

> Enforced in code by `editor/src/blender_tool.cpp` (`parseExportProvenance` /
> `writeExportProvenanceText` / `provenanceMatches`, task 3.2.4);
> `tests/editor/blender_tool_test.cpp` is its machine-checkable form, and
> `tests/editor/blender_service_test.cpp` exercises it end to end through `ModelImportSession`.

### 7.1 Nature

Derived, machine-local, disposable. One file per converted `.blend`, at
`<projectRoot>/Library/BlenderExports/<guid>.json`, never committed, never merged, never compared
across machines. **Its strictness policy follows §6's, not §5's:** a wrong `version`, a missing key or
unparseable text is a **miss** — the artifact is re-converted — and never a repair. Losing it costs one
re-conversion, not an identity.

**The path is shared with the run's own STATUS document, and that is deliberate.** Blender's export
script writes `<guid>.json` as its status report; on a successful import the provenance record
*overwrites* it. A half-finished run therefore leaves a document `parseExportProvenance` rejects, which
is exactly the "no valid cache entry" answer the next selection needs. Two paths would add a fifth file
per asset for no gain.

### 7.2 Envelope

Nine root keys, in this exact order on save.

| Key | Kind | Required | Rule |
|---|---|---|---|
| `version` | number, integral | yes | must equal `1`; validated first |
| `guid` | string | yes | exactly 32 hex digits — the `.blend`'s own asset GUID; **informational** |
| `sourcePath` | string | yes | project-relative path of the `.blend`; **informational** |
| `sourceHash` | string | yes | exactly 32 hex digits — the `.blend`'s content hash |
| `blenderPath` | string | yes | the binary that produced the artifact; **informational** |
| `blenderVersion` | string | yes | as Blender prints it (`"5.2.0 LTS"`) |
| `scriptVersion` | number, integral | yes | `BLENDER_SCRIPT_VERSION` at the time of export |
| `settingsFingerprint` | string | yes | exactly 32 hex digits — see §7.4 |
| `artifactBytes` | number, integral | yes | size of the `<guid>.glb` produced; **informational** |

### 7.3 Identity vs content — what is COMPARED

Only **four** fields decide staleness, and one of them conditionally:

| Field | Compared |
|---|---|
| `sourceHash` | always |
| `scriptVersion` | always |
| `settingsFingerprint` | always |
| `blenderVersion` | **only when a version is known** — see below |
| `guid`, `sourcePath`, `blenderPath`, `artifactBytes` | **never** |

`sourcePath` and `blenderPath` being informational is what makes the record survive a move: the cache is
keyed by GUID, never by path (3.1.2 D11), so relocating a `.blend` together with its sidecar does not
invalidate its conversion.

**The conditional version rule.** A pure cache hit has probed nothing, so the *expected* Blender version
is empty and the field is not compared at all — which is what makes "selecting an unchanged `.blend`
spawns zero processes" literally true, since comparing a version you have not probed would require
probing, and a probe is a process. The recorded value is still *displayed*. Once anything in the session
has probed a version, every later evaluation compares it, so an upgrade is caught within a session.
**The accepted cost, stated rather than discovered later:** convert with one Blender, upgrade, restart
the editor, select the `.blend` — the artifact is served from cache and the panel names the old version.
It is not re-converted until something else triggers a probe. Zero processes on a hit is worth more than
catching an upgrade one selection early, the artifact is a valid glTF either way, and `Re-import` is one
click away.

### 7.4 Canonicalization

Written by `writeTextFileAtomic` only, from `ModelImportSession` only, and **only after the artifact has
been read back and imported successfully**. A killed, timed-out, failed or unusable run leaves any
previous record untouched. Key order is fixed and the document ends in exactly one `\n`, so a re-write
of unchanged data is byte-identical.

`settingsFingerprint` is `formatContentHash(hashBytes(writeMetaText(Guid{}, settings)))` — a **pure
function of `ImportSettings` alone**, since it uses a nil GUID and the default importer identity. Reusing
`.meta`'s own serializer is the point: a future `ImportSettings` field enters the fingerprint
automatically. Strictly it is not required for correctness today (what is cached is the GLB, and `scale`
is applied during *import*); it exists so that a future Blender-**side** option cannot be added without
the invalidation already being in place.

### 7.5 Machine-local, derived, disposable, never committed

`Library/` already carries a `.gitignore` containing `*`, and is excluded from both the asset scan and
the file watcher by the project root's canonical path — so this directory is invisible to both walks
with no code change, and an artifact written there cannot start a rescan loop. A re-export overwrites
the same paths, so the directory holds at most one artifact set per `.blend` in the project, forever,
with **no deletion anywhere in the task**.

---

## 8. Editor tool preferences v1

> Enforced in code by `editor/src/blender_tool.cpp` (`parseToolPrefs` / `writeToolPrefsText`, task
> 3.2.4); `tests/editor/blender_tool_test.cpp` is its machine-checkable form.

### 8.1 Nature

Machine-local, derived from a user's own choice, disposable. **One file per MACHINE**, not per project,
at `SDL_GetPrefPath("AeroEngine", "AeroEditor") + "editor_tools.json"` — beside `recent_projects.json`,
and for the identical reason: an installed tool's path is a property of a machine, not of a project.
Putting it in `project.json` would hand every teammate a path that does not exist on their machine.

### 8.2 Envelope

Two root keys, in this exact order on save.

| Key | Kind | Required | Rule |
|---|---|---|---|
| `version` | number, integral | yes | must equal `1`; validated first |
| `blenderPath` | string | no | absolute path to a Blender binary; `""` (or absent) means "detect automatically" |

A **missing file** is empty preferences, **silently** — that is the normal state on a machine where the
user has never used `Locate…`. A file that **exists but does not parse**, or carries a wrong `version`,
is empty preferences **plus one warning**, emitted from `editor_app.cpp` and nowhere else. A non-string
`blenderPath` is a parse failure, not a coerced empty value.

### 8.3 Identity vs content

There is no identity here at all: the file is a single preference. Losing it costs one re-detection.
Written **only** when the user picks a path with `Locate…`, or clears it with `Re-detect` (which writes
`""`). It is in no project, is never touched by the asset scan, and never appears in a repository.

### 8.4 Canonicalization

`writeTextFileAtomic`, fixed key order, exactly one trailing `\n`; re-parses equal.

---

## 9. Cooked mesh container v1 (`.aeromesh`)

> Enforced in code by `engine/assets` (`cooked_mesh.{hpp,cpp}` = the format and its parser,
> `mesh_cook.{hpp,cpp}` = the producer, task 3.3.1); the doctest batteries in
> `tests/cooked_mesh_test.cpp` and `tests/mesh_cook_test.cpp` are its machine-checkable form, and
> `tests/cooked_mesh_golden.hpp` holds three byte-level goldens. Produced by `aero_cooker mesh`.

### 9.0 Scope, and what does and does not carry over from section 1

**This is the first BINARY format in this document.** Section 1's canonical-**text** rules — UTF-8, no
BOM, LF newlines, 2-space indent, key order, member omission, and the two round-trip guarantees — **do
not apply to it**. There is no text to canonicalize and no key to order; the equivalent guarantees are
restated for bytes in 9.10.

Section 1's **versioning** policy and `docs/04:51` **do** apply, unchanged: the format carries a
version field from day one, and it may break without migration until v1.0. Section 1's strictness
table also does not carry over, and the inversion is deliberate: a JSON envelope WARNs on an unknown
key so old readers keep loading newer files, while this format **refuses** a non-zero reserved field
outright (9.11 says why).

**What v1 stores:** geometry only — interleaved vertex blobs at GPU stride, one index buffer, an
axis-aligned box per submesh and one for the model, plus each submesh's source coordinates and
material index. **What it does not store:** node hierarchy, materials, images, skeletons, inverse bind
matrices and animation. A consumer that instantiates a cooked mesh with no hierarchy therefore puts
every submesh at the origin. **That gap is named, not owned** — a cooked model/prefab container
carrying the node tree is the right answer and belongs to whoever owns instantiation; task 3.1.5 is
the first task that will hit it.

The container is **platform-independent**: there is no platform field and no `--platform` flag. That
changes for textures at task 3.3.2, where BCn/ASTC/ETC2 diverge.

### 9.1 Conventions

- **Little-endian, DECLARED rather than "native".** All three target hosts are little-endian; the
  writer and the reader do explicit byte assembly through eight `constexpr` primitives
  (`putU16`/`putU32`/`putU64`/`putF32` and their `get*` inverses) whose endianness is a
  `static_assert`, not a test. A big-endian mistake is a **build failure**.
- **Floats are IEEE-754 binary32**, moved bit for bit via `std::bit_cast` and a little-endian store.
  The cook performs no floating-point *arithmetic* on vertex data at all: bounds folding is
  comparison-and-select.
- **Offsets and byte lengths are `u64`; counts are `u32`.**
- **Alignment.** Every **stored** offset — each `CookedSection::vertexDataOffset` and the header's
  `indexDataOffset` — is a multiple of **16**, as is `totalBytes`, and every gap between regions is
  zero-filled. **The three tables are packed with no padding, at implicit 8-byte-aligned starts the
  reader derives**, so with an odd `attributeCount` the section table legitimately begins at 104 and
  the submesh table at 136. Nothing is lost by that: no record is ever `memcpy`'d or cast, every field
  goes through the `get*` primitives, so table alignment has no correctness meaning. The two **bulk**
  regions are the ones a consumer hands to an upload call, and those are the two that are 16-aligned.
- **`sizeof` is never taken of an on-disk record.** The four `COOKED_MESH_*_BYTES` constants (96 / 8 /
  32 / 64) are the only sizes: a struct's size is a compiler's opinion and a format's is not.

### 9.2 The header — 96 bytes at offset 0

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 8 | `char[8]` | `magic` | `AEROMESH`, ASCII, **no NUL terminator** |
| 8 | 4 | `u32` | `formatVersion` | must equal `1`; a different value is refused |
| 12 | 4 | `u32` | `cookerVersion` | the producing cooker's version. **Informational** — never gates a parse |
| 16 | 8 | `u64` | `sourceGuid.hi` | the source asset's GUID, high half first |
| 24 | 8 | `u64` | `sourceGuid.lo` | the low half. The nil GUID (both zero) is legal |
| 32 | 4 | `u32` | `reservedFlags` | **must be 0** — a non-zero value is a refusal (9.11) |
| 36 | 4 | `u32` | `sectionCount` | ≤ `MAX_COOKED_SECTIONS` |
| 40 | 4 | `u32` | `submeshCount` | ≤ `MAX_COOKED_SUBMESHES` |
| 44 | 4 | `u32` | `indexCount` | total indices in the file's single index buffer; ≤ `MAX_COOKED_INDICES` |
| 48 | 4 | `u32` | `indexType` | `0` = `Uint16`, `1` = `Uint32`. Any other value is refused |
| 52 | 4 | `u32` | `attributeCount` | ≤ `MAX_COOKED_ATTRIBUTES` |
| 56 | 8 | `u64` | `totalBytes` | **must equal the buffer's own size** |
| 64 | 12 | `f32[3]` | `boundsMin` | model-space minimum, x/y/z |
| 76 | 12 | `f32[3]` | `boundsMax` | model-space maximum, x/y/z |
| 88 | 8 | `u64` | `indexDataOffset` | absolute, a multiple of 16 |

`indexDataOffset` is **stored rather than derived** for two reasons: "the end of the last section's
padded region" has no answer when `sectionCount == 0`, and every other region in this file states its
own position, so deriving one would be the odd case out.

A file with **zero sections and zero submeshes** is legal: the header alone, 96 bytes, `indexType`
`Uint16` (vacuously), `indexDataOffset` 96, `totalBytes` 96, and a **point box at the origin** rather
than an inverted sentinel whose centre would be NaN. That is what a model whose every primitive was
dropped produces — an asset that exists must have an artifact.

### 9.3 The attribute table — 8 bytes per record, at offset 96

`attributeCount` records, packed. A section names a contiguous slice of this one table.

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 2 | `u16` | `semantic` | a `CookedVertexSemantic` code (9.6). `>= 8` is refused |
| 2 | 2 | `u16` | `format` | a `CookedVertexFormat` code (9.6). `> 3` is refused |
| 4 | 4 | `u32` | `offset` | bytes from the start of a vertex; must lie wholly inside the stride |

### 9.4 The section table — 32 bytes per record

`sectionCount` records, packed, immediately after the attribute table (so at `96 + 8 ×
attributeCount`). One section per distinct attribute set.

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 4 | `u32` | `firstAttribute` | index into the attribute table |
| 4 | 4 | `u32` | `attributeCount` | **must be ≥ 1**; `[firstAttribute, +attributeCount)` must lie inside the table |
| 8 | 4 | `u32` | `vertexStride` | **must be non-zero and a multiple of 4** |
| 12 | 4 | `u32` | `vertexCount` | ≤ `MAX_COOKED_VERTICES` |
| 16 | 8 | `u64` | `vertexDataOffset` | absolute, a multiple of 16 |
| 24 | 8 | `u64` | `vertexDataBytes` | **must equal `vertexCount × vertexStride`** |

Within one section, **no semantic may appear twice**, and every attribute must fit inside the stride.

### 9.5 The submesh table — 64 bytes per record

`submeshCount` records, packed, immediately after the section table. One submesh per cooked source
primitive.

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 4 | `u32` | `sectionIndex` | `< sectionCount` |
| 4 | 4 | `u32` | `firstIndex` | in index **units**, into the file's single index buffer |
| 8 | 4 | `u32` | `indexCount` | `[firstIndex, +indexCount)` must lie inside the header's `indexCount` |
| 12 | 4 | `u32` | `materialIndex` | preserved verbatim from the source; `0xFFFFFFFF` means none |
| 16 | 4 | `u32` | `sourceMeshIndex` | the **position** in `ImportedModel::meshes` — see below |
| 20 | 4 | `u32` | `sourcePrimitiveIndex` | the position in that mesh's `primitives` |
| 24 | 8 | `u64` | `reserved0` | **must be 0** |
| 32 | 12 | `f32[3]` | `boundsMin` | this submesh's own box, model space |
| 44 | 12 | `f32[3]` | `boundsMax` | |
| 56 | 8 | `u64` | `reserved1` | **must be 0** |

**`sourceMeshIndex` is the POSITION in `ImportedModel::meshes`, not `ImportedMesh::localId`.** The two
coincide for glTF and OBJ and **differ for FBX**, where `localId` is a raw ufbx `typed_id` rather than
a dense index. The position is what a consumer can actually resolve — it is the same value
`ImportedNode::meshIndex` holds.

**Indices are SECTION-RELATIVE.** A submesh's indices address vertices within its own section, not
within the file, which is what lets one index buffer serve every section and what makes the width
decision (9.7) depend on the largest *section* rather than on the file total.

### 9.6 The frozen enums

**These values are part of the format and never change.** They are deliberately independent of
`rhi::VertexFormat`, whose fifteen enumerators have implicit values: reordering that enum is a legal,
invisible change today, and writing its values into a file would make every previously-cooked artifact
silently decode wrong afterwards.

`CookedVertexSemantic` (`u16`) — the codes mirror `editor::VertexAttribute`'s **bit positions**
exactly, so "bit *n* set" means "semantic *n* present":

| Code | Semantic | Format | Bytes | Source in `ImportedPrimitive` |
|---|---|---|---|---|
| 0 | `Position` | `Float3` | 12 | `positions` — **mandatory**; a primitive without it is dropped |
| 1 | `Normal` | `Float3` | 12 | `normals` |
| 2 | `Tangent` | `Float4` | 16 | `tangents`; `.w` is the bitangent sign, preserved, never normalized |
| 3 | `TexCoord0` | `Float2` | 8 | `uv0` |
| 4 | `TexCoord1` | `Float2` | 8 | `uv1` |
| 5 | `Color0` | `Float4` | 16 | `colors`, linear RGBA |
| 6 | `Joints0` | `Uint4` | 16 | `joints`, widened `u16 → u32` losslessly |
| 7 | `Weights0` | `Float4` | 16 | `weights` |

`CookedVertexFormat` (`u16`): `0 = Float2` (8 bytes), `1 = Float3` (12), `2 = Float4` (16),
`3 = Uint4` (16).

`CookedIndexType` (`u32`): `0 = Uint16` (2 bytes), `1 = Uint32` (4).

The semantic → format mapping above is **total and frozen**: minimum stride 12 (position only),
maximum stride 104 (all eight). `Joints0` is `Uint4` rather than a `u16 × 4` because `rhi::VertexFormat`
has no unsigned-16×4 enumerator and `UByte4Norm` is *normalized* and therefore wrong for an index — so
a skinned vertex pays 8 bytes it does not need. The fix is `CookedVertexFormat::UShort4` beside a new
`rhi::VertexFormat::UShort4`, and per 9.11 that is a **`formatVersion` bump**.

### 9.7 Layout, ordering and regions

**Ordering rules, all three normative:**

1. **Submeshes are emitted in ascending `(attributeMask, sourceMeshIndex, sourcePrimitiveIndex)`
   order**, never in the caller's input order. The mask is the *surviving* attribute set (9.8).
2. **Sections are emitted in ascending mask order**, which falls out of rule 1: equal masks are
   adjacent after the sort, so one linear pass builds every section.
3. **Within a section, attributes are laid out in ascending semantic code**, with offsets accumulated
   in exactly that order and **no padding between attributes** — every v1 format's size is a multiple
   of 4, so the stride is one too.

**The index width is a file-level decision:** `Uint16` if every section has `vertexCount <= 65536`,
otherwise `Uint32`. The bound is `<=`, not `<`: a section with exactly 65536 vertices has a maximum
index of 65535, which `Uint16` represents.

**Regions, in file order:** header, attribute table, section table, submesh table, then **one vertex
region per section in section order** — each starting at the next multiple of 16, each exactly
`vertexCount × vertexStride` bytes — then the **index region**, again at the next multiple of 16,
`indexCount × indexTypeBytes` bytes. `totalBytes` is that cursor rounded up to the next multiple of
16. **Every gap is zero-filled.** Inter-region padding is *computed*, not unconditional: a region that
already ends on a multiple of 16 is followed by no padding at all.

### 9.8 What the cook drops and what it demotes

The producer drops **whole primitives** and demotes **whole attributes**, never anything partial. Five
arms, each producing one capped warning that names its source coordinates:

| Condition | Action |
|---|---|
| `positions` empty, `indices` empty, `indices.size() % 3 != 0`, or any index ≥ `positions.size()` | the primitive is dropped whole |
| any position component non-finite | the primitive is dropped whole |
| an optional array is non-empty but its length ≠ `positions.size()` | that attribute is treated as absent; the primitive still cooks |
| exactly one of `joints`/`weights` survives the rule above | **both** are treated as absent — half a skin is worse than none |
| two primitives share the ordering key `(mask, sourceMeshIndex, sourcePrimitiveIndex)` | one warning; **nothing is dropped** (9.10) |

Non-finite values in attributes *other than* positions are copied through verbatim: they participate
in no computation, their bits are moved rather than derived, and refusing them would drop geometry
over a shading artifact.

**Bounds** are folded with `std::min`/`std::max`, **accumulator first**, over every position
**written** — not over the subset an index reaches — so a submesh's box equals its source
`ImportedPrimitive::bounds` bit for bit. The model box is the union of the **emitted** submeshes'
boxes, so a dropped or cap-refused primitive contributes nothing to it.

### 9.9 The caps

Seven constants, enforced by **both** the writer and the parser.

| Constant | Value | Why |
|---|---|---|
| `MAX_COOKED_SECTIONS` | 128 | exactly the number of representable layouts: `Position` is mandatory, so a mask has at most 2⁷ values. Exists **for the parser** — well-formed input can *reach* it and can never *exceed* it, so the cook's own arm for it is unreachable defence in depth |
| `MAX_COOKED_ATTRIBUTES` | 1024 | 128 sections × 8 semantics; unreachable from the cook for the same reason |
| `MAX_COOKED_SUBMESHES` | 65536 | one per cooked source primitive |
| `MAX_COOKED_VERTICES` | 8 000 000 | mirrors the importer's `MAX_VERTICES_PER_MODEL` |
| `MAX_COOKED_INDICES` | 24 000 000 | mirrors the importer's `MAX_INDICES_PER_MODEL` |
| `MAX_COOKED_MESH_BYTES` | 2 GiB | a cheap parser early-out. It can never refuse a legitimately cooked artifact: the two caps above bound the largest legal file at `8000000 × 104 + 24000000 × 4` ≈ 928 MB |
| `MAX_COOK_WARNINGS` | 20 | mirrors `MAX_IMPORT_WARNINGS`; the *list* is capped, the *total* is not |

Exceeding a vertex, index or submesh cap produces a **truncated but complete and parseable** file: the
producer walks the sorted order, stops accepting at the first primitive that would violate any cap,
and reports one message per violated cap joined with `"; "`. It never emits a hole.

### 9.10 Determinism

The same input cooks to the same bytes across two runs, three toolchains and any ordering of the
input's own primitives. That is **structural**, not aspirational — six sources of divergence are closed
by construction:

1. **No struct `memcpy`, ever**, and no `reinterpret_cast` of a record pointer and no packed-struct
   pragma, so compiler padding, member reordering and alignment choices cannot reach the file.
2. **No hash container anywhere in `engine/assets`** — no `std::map`, `std::unordered_map`, `std::set`
   or `std::unordered_set` — so there is no iteration order for the output to depend on. Grouping is a
   sorted vector.
3. **All padding is explicitly zero**, because the output buffer is allocated zero-initialized.
4. **No timestamp, no path, no hostname, no user name, no build id.** The only provenance fields are
   `cookerVersion` (a compile-time constant) and the caller-supplied `sourceGuid`.
5. **Floats are re-emitted bit for bit.**
6. **Order-independence**, per 9.7's rule 1.

**Its one stated limit.** The ordering key `(mask, sourceMeshIndex, sourcePrimitiveIndex)` is unique
for everything the editor's own adapter produces — one entry per `(meshIndex, primitiveIndex)` — but
`cookMesh` is a public API over a caller-supplied span, and a caller *can* hand it two primitives with
an identical key. The sort is stable, so tied entries keep the input's relative order: **byte-identity
across a reordering holds exactly when the keys are distinct.** A repeated key produces one warning
naming the first collision and the total count; nothing is dropped, because dropping would lose
geometry over a caller's bookkeeping.

**One caveat on the model box, and it is a caveat about agreement, not about determinism.** The model
box is the union of the **emitted** submesh boxes folded in **emission** order — the sorted order of
9.7's rule 1 — and that cannot change: the model box is written into the header, so folding in the
caller's input order would make a shuffled input produce different header bytes, which is exactly what
order-independence forbids. An importer folds its own model box in **source** order instead.
`std::min` and `std::max` are order-independent for every pair of floats **except** `(+0.0f, -0.0f)`,
so the two folds can disagree in **the sign of a zero and nowhere else**. No box is wrong either way —
a ±0 bound is the same box — and no committed fixture in this repository carries a signed zero, which
is why the cooked and imported model boxes are currently bit-equal as well as geometrically equal.

Determinism *across platforms* is asserted by three committed byte goldens, the cook's own round-trip
cases on all three CI lanes, and the frozen manifest `tests/cooker/determinism.sha256`, which the
`cooker.golden_manifest` case checks in every build configuration on every lane and the dedicated
`cook-determinism` CI job re-checks against the three lanes' actually-produced artifacts (task 3.3.3).

### 9.11 Versioning and evolution

Two version fields, and they mean different things:

- **`formatVersion`** — bumped when an older **reader** can no longer read the file. The parser
  refuses any value it does not equal.
- **`cookerVersion`** — bumped when **the same input now cooks to different bytes**. It is a
  cache-invalidation signal and nothing else; it never gates a parse.

**Reserved space is a BREAKING extension point, not an additive one.** A non-zero `reservedFlags`,
`reserved0` or `reserved1` is a **parse refusal**, deliberately, as the inverse of section 1's
unknown-key WARN rule — and the consequence follows directly: **occupying a reserved field requires a
`formatVersion` bump**, not merely a `cookerVersion` one, because every v1 reader will refuse the
whole file. That refusal is the *intended* behaviour: it is exactly what stops a v1 reader silently
misreading a v2 file.

**Adding a semantic or format code is a `formatVersion` bump too**, for the same reason: the parser
refuses unknown codes with `BadLayout`.

**Appending a whole new region past `totalBytes` is not possible**, because `totalBytes` must equal the
buffer's own size.

The contrast with section 1 is deliberate, not an inconsistency. A JSON envelope can tolerate an
unknown key because the key has a *name* a reader can warn about and skip. A binary reserved field has
no name, no length prefix and no way to be skipped — and tolerating it forfeits the ability to ever
use it, because a v1 reader that ignored `reserved0` would keep happily misreading every v2 file.

### 9.12 The writer/reader asymmetry

The **writer** always zeroes every pad byte. The **reader** does not check that it did: a file whose
trailing padding is non-zero parses `Ok`. That is deliberate, so a future writer that pads differently
is not locked out of a format whose meaningful content it reproduced exactly.

Two further things the parser deliberately **does not** check, stated so their absence is not read as
an oversight:

1. **Individual index VALUES against their section's `vertexCount`.** That is O(`indexCount`) work on
   up to 24 million entries on every load, and the consumer that uploads to the GPU is where an
   out-of-range index is a driver concern. First-party cooked files are always in range — the cook
   validates every index it writes. The Phase 5 answer is an opt-in `parseCookedMeshStrict`, chosen by
   the caller, never forced on every load.
2. **Whether the vertex regions and the index region overlap each other or the tables.** Every read
   goes through `sectionVertexBytes`/`indexBytes`, both bounds-checked against the buffer, so an
   overlap is a wrong *picture*, never a memory error; refusing it needs an interval sort over up to
   129 ranges and buys nothing an attacker can use.

Everything the parser *does* check is a **subtraction** against a known-good size
(`length <= size && offset <= size - length`), never an addition that can wrap, and **nothing is
allocated before the count it is allocating for has been checked against a frozen cap**.

### 9.13 Error catalog

`parseCookedMesh` never throws, never reads a file and never logs. It returns one of ten statuses with
a human-readable message; the message is empty **iff** the status is `Ok`.

| Status | Cause |
|---|---|
| `Ok` | the buffer is a valid v1 container |
| `TooSmall` | fewer than 96 bytes — shorter than the header |
| `BadMagic` | the first eight bytes are not `AEROMESH` |
| `UnsupportedVersion` | `formatVersion` is not 1 |
| `ReservedNotZero` | `reservedFlags`, or a submesh's `reserved0`/`reserved1`, is non-zero |
| `SizeMismatch` | `totalBytes` does not equal the buffer's own size |
| `CapExceeded` | the buffer is over `MAX_COOKED_MESH_BYTES`, or a declared count is over its cap |
| `BadTable` | the three tables do not fit in the buffer; a section names attributes outside the table; or `indexType` is neither 0 nor 1 |
| `BadRange` | an offset/length pair leaves the buffer; a stored offset is not 16-aligned; `vertexDataBytes ≠ vertexCount × vertexStride`; a submesh names a section or an index range that does not exist |
| `BadLayout` | an unknown semantic or format code; a section with zero attributes, a zero stride, or a stride that is not a multiple of 4; an attribute outside its stride; or a semantic declared twice in one section |

### 9.14 Golden fixtures

Three byte-level goldens live in `tests/cooked_mesh_golden.hpp` as annotated in-source arrays, shared
by `aero_tests` and `aero_editor_shell_test` so there is one copy and no drift:

- **`COOKED_GOLDEN_EMPTY`, 96 bytes** — zero primitives. Simultaneously the valid empty file and the
  smallest buffer that is not `TooSmall`.
- **`COOKED_GOLDEN_TRIANGLE`, 272 bytes** — one position-only primitive. This is exactly
  `tests/fixtures/assets/triangle.gltf` imported at `Full` depth, so one golden pins the editor's
  adapter as well as the cook. Its section table starts at **104** and its submesh table at **136**,
  neither 16-aligned, which is 9.1's packed-table rule in bytes.
- **`COOKED_GOLDEN_MIXED`, 480 bytes** — two primitives supplied in **reverse** key order, so the
  array is simultaneously the golden and the order-independence proof. It pins ascending-mask section
  order, ascending-semantic attribute order, a non-nil GUID in both header halves, and the fact that
  inter-region padding is computed rather than unconditional.

**They are frozen.** A change to any layout rule, offset, ordering rule or padding rule fails them by
construction. If a golden has to change, the format changed — and that is a `formatVersion` decision,
not a test edit.

---

## 10. Cooked texture container v1 (`.ktx2`)

> Enforced in code by `engine/assets` (`cooked_texture.{hpp,cpp}` = the format and its parser,
> `texture_cook.{hpp,cpp}` = the producer, `bc_block.{hpp,cpp}` = the two block encoders, task 3.3.2);
> the doctest batteries in `tests/cooked_texture_test.cpp`, `tests/texture_cook_test.cpp` and
> `tests/bc_block_test.cpp` are its machine-checkable form, and `tests/cooked_texture_golden.hpp` holds
> four byte-level goldens. Produced by `aero_cooker texture`.

### 10.0 Scope — the first THIRD-PARTY format in this document

Sections 1–9 describe formats this project invented. **This one it did not.** The container is a strict
subset of Khronos **KTX2**, and a real one: `ktx info`, `ktx validate` and RenderDoc open these files.
Every field below is therefore a **fact extracted from `ktxspec.adoc`**, not a decision taken here, and
where our subset is narrower than the specification that narrowing is a **refusal** rather than a
reinterpretation.

Section 1's canonical-**text** rules do not apply, for the same reason they do not apply to section 9.
Section 9's byte rules do, and 10.10 restates the ones that differ.

**Section 9.0's forward reference is answered here.** It says the mesh container is
platform-independent and that "that changes for textures at task 3.3.2, where BCn/ASTC/ETC2 diverge".
The answer for v1 is: **there is still no platform field and no `--platform` flag**, because v1 emits
exactly one profile — desktop BCn. A flag with one legal value would be a promise the cooker cannot
keep. ASTC, ETC2 and the flag itself arrive together at task **6.3.1**, with the second profile.

**What v1 stores:** one 2D image, its full mip chain or its base level alone, in one of eight Vulkan
formats, plus the source asset's GUID, an orientation declaration and a writer id.
**What it does not store:** cubemaps, array layers, 3D/volume textures, incomplete mip chains,
supercompression of any kind (Basis/UASTC/ETC1S, Zstd, ZLIB), BC7, BC6H, and any per-texture cook
setting. Each is a refusal with a named owner in the task's own record, not an omission.

### 10.1 Conventions

- **Little-endian, DECLARED rather than "native"**, and formed through **section 9's eight `constexpr`
  primitives** (`putU16`/`putU32`/`putU64`/`putF32` and their `get*` inverses, in `cooked_mesh.hpp`).
  There is deliberately **no second set** in this subsystem: a second set is a second place for a
  big-endian mistake to hide, and the `static_assert` only protects the set it is attached to.
  `putF32`/`getF32` go unused here — this format has no float field — and that is not a reason to
  split the header.
- **NO FLOATING POINT ANYWHERE IN THESE FILES.** Not in the container, not in the cook, not in the
  encoders, not in the mip filter: no `float`, no `double`, no `<cmath>`, no runtime table generation.
  This is not a style rule. A float here is a **byte-identity hazard on three CI lanes** — FMA
  contraction differs between clang and MSVC, and libm differs between three C libraries — and task
  3.3.3 turns cross-platform byte-identity into a CI job for **both** cook kinds. (`<numeric>`'s
  `std::lcm` is integer-only and is the one exception the rule does not reach.)
- **Offsets and byte lengths inside the header are `u32`; the level index's three fields are `u64`**,
  because that is what the specification says, not because either width was chosen here.
- **Alignment: exactly one padding site per file** (10.4).
- **`sizeof` is never taken of an on-disk record.** `KTX2_HEADER_BYTES` (80), `KTX2_LEVEL_RECORD_BYTES`
  (24) and `KTX2_KVD_BYTES` (120) are the only sizes.

### 10.2 The header and Index — 80 bytes at offset 0

| Offset | Size | Type | Field | Value in v1 |
|---|---|---|---|---|
| 0 | 12 | `u8[12]` | `identifier` | `AB 4B 54 58 20 32 30 BB 0D 0A 1A 0A` — «KTX 20» wrapped in bytes that make a text-mode write corrupt the file at byte 0 rather than silently deeper in |
| 12 | 4 | `u32` | `vkFormat` | one of the eight of 10.7 and **nothing else** |
| 16 | 4 | `u32` | `typeSize` | **1**. The specification requires 1 for every `_BLOCK` format; the two RGBA8 formats have 1-byte channels, so 1 is right for all eight |
| 20 | 4 | `u32` | `pixelWidth` | 1 … `MAX_TEXTURE_DIMENSION` |
| 24 | 4 | `u32` | `pixelHeight` | 1 … `MAX_TEXTURE_DIMENSION` |
| 28 | 4 | `u32` | `pixelDepth` | **0** — 2D images only |
| 32 | 4 | `u32` | `layerCount` | **0** — no texture arrays |
| 36 | 4 | `u32` | `faceCount` | **1** — no cubemaps |
| 40 | 4 | `u32` | `levelCount` | the full chain or **1**; **never 0** (10.8) |
| 44 | 4 | `u32` | `supercompressionScheme` | **0**, always. A non-zero value is a parse refusal with its own status |
| 48 | 4 | `u32` | `dfdByteOffset` | exactly `80 + 24 × levelCount` |
| 52 | 4 | `u32` | `dfdByteLength` | 44, 60 or 92, decided by the format (10.5) |
| 56 | 4 | `u32` | `kvdByteOffset` | exactly `dfdByteOffset + dfdByteLength` |
| 60 | 4 | `u32` | `kvdByteLength` | **120**, always (10.6) |
| 64 | 8 | `u64` | `sgdByteOffset` | **0** |
| 72 | 8 | `u64` | `sgdByteLength` | **0** |

The parser refuses a `dfdByteOffset` or `kvdByteOffset` that is not *exactly* where the layout puts it.
The specification permits more freedom; a first-party cooked-asset reader does not need it, and a file
that disagrees with our own arithmetic is either corrupt or our own bug.

### 10.3 The level index and the mip level array — the ORDERING INVERSION

`levelCount` records of 24 bytes, packed, at offset 80:

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 8 | `u64` | `byteOffset` — absolute into the file |
| 8 | 8 | `u64` | `byteLength` |
| 16 | 8 | `u64` | `uncompressedByteLength` — **equal to `byteLength`**, since the scheme is 0 |

**The index and the data run in opposite directions, and this is the single most likely place in the
whole format for an off-by-one.** `ktxspec.adoc` states both halves:

> *"The array is ordered starting with level_base (the level with the largest size images) at index 0"*

> *"Mip level data is ordered from the level with the smallest size images, level_p to that with the
> largest size images, level_base"*

So the record at index 0 describes the **largest** level, and it holds the **numerically largest**
`byteOffset` in the file. The offsets are **strictly decreasing** in index order; the last record's
offset is the smallest and equals the aligned end of the key/value data; the first record's
`byteOffset + byteLength` equals the file's total size. The writer computes the offsets in **reverse**
and stores them **forward**.

A level's dimensions are `max(1, pixelWidth >> level)` by `max(1, pixelHeight >> level)`, and its
`byteLength` is `ceil(w / blockWidth) × ceil(h / blockHeight) × blockBytes`. The parser recomputes both
and refuses a record that disagrees.

### 10.4 Alignment and the single padding site

Every level's `byteOffset` is a multiple of `lcm(blockBytes, 4)` — KTX2's own `mipPadding` rule, quoted
verbatim as *"between 0 and lcm(texel_block_size, 4) - 1 bytes of value 0x00"*, required exactly
because `supercompressionScheme` is 0.

**There is exactly ONE padding site in the whole file**: between the end of the key/value data and the
start of the *smallest* level, which is the first level written. Every level's `byteLength` is itself a
multiple of that alignment, so once the first written level is aligned every subsequent level start is
aligned automatically, and no gap can exist between two levels. The pad is `(A - kvdEnd % A) % A` bytes
of `0x00`, and it is zero for a file whose key/value data happens to end aligned (golden B).

**The parser checks the alignment of every level anyway**, not only the first: a hostile file is not
obliged to share our arithmetic.

### 10.5 The eight frozen Data Format Descriptors

Each format carries a DFD that **must byte-match the frozen table** for its `vkFormat`. Layout: 4 bytes
of `dfdTotalSize`, then six basic-block words, then 16 bytes per sample.

| Word | Composition |
|---|---|
| 0 | `vendorId` (0) \| `descriptorType` (0) << 17 |
| 1 | `versionNumber` (**2**) \| `descriptorBlockSize` << 16 |
| 2 | `colorModel` \| `primaries` (1, BT709) << 8 \| `transferFunction` << 16 \| `flags` (0, straight alpha) << 24 |
| 3 | `(blockW-1)` \| `(blockH-1)` << 8 \| `(blockD-1)` << 16 \| `(blockE-1)` << 24 |
| 4 | `bytesPlane0` (the block byte size), planes 1–3 above it |
| 5 | `bytesPlane4..7`, all zero |
| sample | `bitOffset` \| `(bitLength-1)` << 16 \| `channelId` << 24 ; `samplePosition` ; `sampleLower` ; `sampleUpper` |

`versionNumber` 2 is `KHR_DF_VERSIONNUMBER_1_4`, which `khr_df.h` notes *did not bump the block version
number* from 1.3 — so the byte is the same either way, and the label matters only to whoever reads a
future dfdutils that does bump it.

**Every table was DERIVED from `dfdutils`' own `createdfd.c`** (`createDFDCompressed`,
`createDFDUnpacked`, `writeHeader`, `writeSample`) with the enumerator values read out of `khr_df.h`,
then regenerated by compiling that file and dumping its output byte for byte. Deriving them by hand was
tried and is exactly how the two corrections below were nearly missed.

**They are explicit byte literals and never shift-and-or expressions**, for two reasons. A table that
is *computed* can be computed wrongly in a way its own reader agrees with (10.12's circularity). And
`(uint32_t)channel << 24` shifts into a sign bit for the channel values 128–132 if the value is ever
handled as a signed type — `createdfd.c` carries its own comment about that. A literal cannot.

```
131 BC1_RGB_UNORM   (44 bytes)
2C 00 00 00  00 00 00 00  02 00 28 00  80 01 01 00  03 03 00 00  08 00 00 00
00 00 00 00  00 00 3F 00  00 00 00 00  00 00 00 00  FF FF FF FF

132 BC1_RGB_SRGB    (44 bytes)  — 131 with byte 14: 01 -> 02, and NOTHING ELSE

137 BC3_UNORM       (60 bytes)
3C 00 00 00  00 00 00 00  02 00 38 00  82 01 01 00  03 03 00 00  10 00 00 00
00 00 00 00  00 00 3F 0F  00 00 00 00  00 00 00 00  FF FF FF FF
40 00 3F 00  00 00 00 00  00 00 00 00  FF FF FF FF

138 BC3_SRGB        (60 bytes)  — 137 with byte 14: 01 -> 02 AND byte 31: 0F -> 1F

139 BC4_UNORM       (44 bytes)
2C 00 00 00  00 00 00 00  02 00 28 00  83 01 01 00  03 03 00 00  08 00 00 00
00 00 00 00  00 00 3F 00  00 00 00 00  00 00 00 00  FF FF FF FF

141 BC5_UNORM       (60 bytes)
3C 00 00 00  00 00 00 00  02 00 38 00  84 01 01 00  03 03 00 00  10 00 00 00
00 00 00 00  00 00 3F 00  00 00 00 00  00 00 00 00  FF FF FF FF
40 00 3F 01  00 00 00 00  00 00 00 00  FF FF FF FF

37  R8G8B8A8_UNORM  (92 bytes)
5C 00 00 00  00 00 00 00  02 00 58 00  01 01 01 00  00 00 00 00  04 00 00 00
00 00 00 00  00 00 07 00  00 00 00 00  00 00 00 00  FF 00 00 00
08 00 07 01  00 00 00 00  00 00 00 00  FF 00 00 00
10 00 07 02  00 00 00 00  00 00 00 00  FF 00 00 00
18 00 07 0F  00 00 00 00  00 00 00 00  FF 00 00 00

43  R8G8B8A8_SRGB   (92 bytes)  — 37 with byte 14: 01 -> 02 AND byte 79: 0F -> 1F
```

**THE ALPHA QUALIFIER, AND WHY TWO OF THE THREE sRGB TABLES ARE NOT ONE-BYTE EDITS OF THEIR SIBLINGS.**
This paragraph is the most important prose in this section. `createdfd.c`'s `setChannelFlags` sets
`KHR_DF_SAMPLE_DATATYPE_LINEAR` (`0x10`) on a sample whose channel id equals
`KHR_DF_CHANNEL_RGBSDA_ALPHA` (15) when the suffix is sRGB. `KHR_DF_CHANNEL_BC3_ALPHA` is **also 15**,
and `writeSample` maps a channel-3 request to `KHR_DF_CHANNEL_RGBSDA_ALPHA` before applying the same
rule — so **BC3_SRGB's first sample and R8G8B8A8_SRGB's fourth carry channel byte `1F`, not `0F`**. The
comparison is numeric, not semantic, which is exactly why the library's rule reaches BC3 at all.

KTX2 makes it a **`must`**, not a preference:

> *"If transferFunction is not KHR_DF_TRANSFER_LINEAR or KHR_DF_TRANSFER_UNSPECIFIED and an alpha
> channel exists, KHR_DF_SAMPLE_DATATYPE_LINEAR must be set in the qualifiers field of the alpha
> sample."*

So of the three UNORM/SRGB pairs, exactly **one** differs in a single byte: 131/132, whose only sample
is channel 0 (`KHR_DF_CHANNEL_BC1A_COLOR`) and which the alpha rule cannot reach. 137/138 and 37/43
differ in **two**. BC4 and BC5 have no sRGB sibling at any Vulkan value.

**Do not "simplify" the sRGB tables into byte patches of their siblings.** One of them is; two of them
are not; and the two that are not are precisely the ones an external validator rejects — while our own
parser, which compares against the same table our writer emits, accepts them happily.

### 10.6 Key/value data — three keys, 120 bytes, always

Each record is `4` bytes of `keyAndValueByteLength`, then the NUL-terminated key, then the
NUL-terminated value, then `valuePadding` zeroes to the next 4-byte boundary.

| Key | Value | key+value | Record |
|---|---|---|---|
| `AeroSourceGuid` | 32 lowercase hex characters + NUL | 15 + 33 | 52 |
| `KTXorientation` | `rd` + NUL — right and down, i.e. a **TOP-LEFT origin** | 15 + 3 | 24 |
| `KTXwriter` | `Aero Engine texture cook 1` + NUL | 10 + 27 | 44 |

**`KTX2_KVD_BYTES = 120` is a closed form, not a magic number**, and a `static_assert` recomputes it
from the three records — so a `COOKED_TEXTURE_COOKER_VERSION` bump that lengthens the writer string is a
**build failure** rather than a silently wrong `kvdByteLength` and every offset after it.

**The records are sorted by Unicode code point**, which the specification requires. Every key here is
ASCII, so a byte-wise compare *is* a code-point compare. The writer constructs them in a **deliberately
wrong order** and sorts — otherwise the sort would be load-bearing in a way no test could see.

**`AeroSourceGuid` is written unconditionally, including for the nil GUID** (32 `'0'` characters), so
the file's layout never depends on whether a GUID was supplied.

**The reader tolerates unknown keys, and that does NOT contradict section 9.11's reserved-field
refusal.** A KTX2 key is *named* and *length-prefixed*, so it can genuinely be skipped with every one of
its bytes accounted for; a binary reserved field has no name, no length and no way to be skipped. A
missing or malformed `AeroSourceGuid` yields the **nil** GUID and is not a refusal either: a broken
provenance key is not a corrupt image.

### 10.7 The eight formats

| `vkFormat` | Name | Block | Bytes/block | Alignment | sRGB sibling |
|---|---|---|---|---|---|
| 37 | `R8G8B8A8_UNORM` | 1×1 | 4 | 4 | 43 |
| 43 | `R8G8B8A8_SRGB` | 1×1 | 4 | 4 | — |
| 131 | `BC1_RGB_UNORM_BLOCK` | 4×4 | 8 | 8 | 132 |
| 132 | `BC1_RGB_SRGB_BLOCK` | 4×4 | 8 | 8 | — |
| 137 | `BC3_UNORM_BLOCK` | 4×4 | 16 | 16 | 138 |
| 138 | `BC3_SRGB_BLOCK` | 4×4 | 16 | 16 | — |
| 139 | `BC4_UNORM_BLOCK` | 4×4 | 8 | 8 | **none exists** |
| 141 | `BC5_UNORM_BLOCK` | 4×4 | 16 | 16 | **none exists** |

**133/134 (`BC1_RGBA_UNORM`/`_SRGB`) are deliberately absent**: those are the punch-through-alpha
variants, and this cook's BC1 encoder always emits opaque four-colour blocks. A value in this table is a
promise the encoder must keep.

**There is no `BC4_SRGB` and no `BC5_SRGB` at any Vulkan value** — the enumeration runs 139 `BC4_UNORM`,
140 `BC4_SNORM`, 141 `BC5_UNORM`, 142 `BC5_SNORM` — which is what makes "an sRGB normal map"
**unspellable** rather than merely rejected. The colour space is carried by the format enumerator and
by nothing else: there is no `bool srgb` anywhere in the subsystem, so the invalid combination is not a
state to validate, log about and eventually get wrong.

### 10.8 The mip chain

`levelCount` is either `floor(log2(max(width, height))) + 1` or **1**. There is no partial pyramid: KTX2
permits one and this container refuses it, because a partial chain's only effect is to make a
consumer's sampler configuration depend on the file. `levelCount == 0` is forbidden by the
specification itself for block-compressed formats and is refused for every format here.

**The refusal is the parser's, not merely the cook's**, and the two statuses differ: a count *between*
1 and the full chain is `UnsupportedShape` (nothing is over a cap — 2 is inside 1…4 for an 8×8 image),
while 0 and a count *past* the chain are `CapExceeded`. For a 1×1 image the two accepted answers
coincide, so the round trip of the smallest legal file is unaffected.

Level `p` is filtered **from level `p-1`**, never resampled from level 0.

**The filter is an integer polyphase box**, per axis, and it is the reason an NPOT texture's small mips
do not walk:

| Source extent `S` | Taps | Weights | Denominator | Source indices |
|---|---|---|---|---|
| `S == 1` | 1 | `{1}` | 1 | `{0}` |
| even, `S == 2D` | 2 | `{1, 1}` | 2 | `{2i, 2i+1}` |
| odd, `S == 2D+1` | 3 | `{D-i, D, i+1}` | `2D+1` | `{2i, 2i+1, 2i+2}` |

The odd weights sum to `2D+1` for every `i`, which is what makes the filter energy-preserving; the naive
alternatives (drop the last row/column, or clamp the second tap) shift the image by a fraction of a
texel **per level**, so the shift compounds down the chain. The two axes are combined into **one fused
2D weighted sum with ONE rounding step**, round-half-up — two separable passes would round twice, the
second time over already-rounded values.

**The filter is gamma-correct.** For an `*Srgb` format each colour sample is decoded to linear light,
averaged there, and re-encoded; for a `*Unorm` format the stored values already *are* linear and are
averaged as they stand. **Alpha is averaged as stored in BOTH cases**: alpha is coverage, never a
gamma-encoded colour.

**The two gamma tables are committed literals**, `SRGB_TO_LINEAR[256]` (16-bit fixed point) and the 255
midpoint thresholds of its inverse. The sRGB transfer function is a `pow`, so it is evaluated exactly
**zero** times at runtime; a table generated at startup would put a libm implementation into the output
bytes, which is worse than the FMA hazard the first-party encoders exist to avoid. Three
`static_assert`s hold them: the forward table spans 0…65535 exactly, it is strictly increasing, and the
threshold table **is** the midpoint sequence derived from it — so only one of the two arrays can be
independently wrong.

### 10.9 The block encoders — output-byte decisions, stated normatively

There are **two** encoders, and BC3 and BC5 have none of their own:

- **BC3** = `encodeBc4Block(alpha)` into bytes 0–7, then `encodeBc1Block(rgb)` into bytes 8–15.
- **BC5** = `encodeBc4Block(red)` into bytes 0–7, then `encodeBc4Block(green)` into bytes 8–15.

Both composition **orders** are part of the format: swapping either produces a plausible image rather
than an obviously broken one.

**BC1.** Bounding box of the 16 texels → RGB565 endpoints (round to nearest; no exact half exists, so
the tie rule is moot) → dequantize by **bit replication**, which is what a hardware decoder does → build
the four-colour palette with the format's frozen `(2a+b+1)/3` rounding → assign each texel the index
minimising the weighted squared error with the **frozen weights 3/6/1** → **exactly two** least-squares
refinement iterations, integer, Cramer's rule, keeping the current endpoints when the system is
degenerate → order the endpoints so `c0 > c1` (four-colour mode), remapping every index with `^ 1`,
which is exact → if the quantized endpoints coincide, all indices are zero. Ties take the **lower**
index. Texel `(x, y)`'s 2 bits sit at bit `2 × (4y + x)` of the little-endian `u32` at byte 4. Alpha is
ignored: `VK_FORMAT_BC1_RGB_*` carries none.

**BC4.** `r0 = max`, `r1 = min` (the eight-value mode), the six interpolants
`((7-k)·r0 + k·r1 + 3) / 7` for `k = 1…6`, nearest value with ties taking the **lower** index, and 3
bits per texel at bit `3 × (4y + x)` of the 48-bit little-endian field at byte 2. Equal endpoints make
the block a constant, which index 0 reproduces exactly.

**The iteration count and the error weights are frozen quality decisions, not tunables.** Moving either
is a `COOKED_TEXTURE_COOKER_VERSION` bump. An error-driven stopping rule would be a second determinism
surface — one whose behaviour depends on an epsilon.

**Partial edge blocks CLAMP the sample coordinate; they never zero-fill.** A 5×3 image's second block
column covers columns 4–7, of which 5–7 are replicated copies of column 4. Zero-fill would drag the
block's endpoints toward black and visibly darken the right and bottom edges. The gather is the block
loop's job precisely so the encoders always see sixteen valid texels.

### 10.10 Determinism

The same input cooks to the same bytes across two runs and three toolchains. Section 9.10's six
structural sources carry over unchanged; **two more are new here and are the whole reason the encoders
are first-party**:

7. **No floating point anywhere in the subsystem.** FMA contraction differs between clang on arm64 and
   MSVC under `/fp:precise`, so a float endpoint search can produce different bytes on two of this
   project's three lanes. `stb_dxt.h` — already installed, and providing exactly these four formats —
   finds BC1's principal axis by float power iteration, and that is why it is not used. The argument is
   determinism, not quality.
8. **No runtime table generation.** The gamma tables are committed literals rather than `std::pow` at
   startup, because libm differs between three C libraries.

There is **no order-dependence to close** here: the cook takes one image, not a set of primitives, so
section 9.10's ordering caveat has no analogue.

Determinism *across platforms* is asserted by four committed byte goldens, the cook's own round-trip
cases on all three CI lanes, and the frozen manifest `tests/cooker/determinism.sha256`, which the
`cooker.texture_golden_manifest` case checks in every build configuration on every lane and the
dedicated `cook-determinism` CI job re-checks against the three lanes' actually-produced artifacts —
including a pinned Khronos `ktx validate` over every cooked `.ktx2` (task 3.3.3).

### 10.11 The caps

| Constant | Value | Why |
|---|---|---|
| `MAX_TEXTURE_DIMENSION` | 16384 | per axis. A `static_assert` ties it to the level cap: `16384 == 2^(15-1)` |
| `MAX_TEXTURE_LEVELS` | 15 | `floor(log2(16384)) + 1` — the full chain of the largest legal image |
| `MAX_COOKED_TEXTURE_BYTES` | 512 MiB | a cheap parser early-out **and** a real cook refusal |
| `MAX_TEXTURE_FILE_BYTES` | 64 MiB | the **compressed source file** the editor's adapter will read, matching `MAX_THUMBNAIL_SOURCE_BYTES` rather than `MAX_MODEL_FILE_BYTES`'s 256 MiB. The decoded pixel count is bounded separately and per axis |

The consequence of the third is stated rather than discovered: a 16384² **RGBA8** cook needs 1.07 GB for
level 0 alone and **is refused**. The maximum dimension is reachable for BC1/BC4 (~179 MB with the full
chain) and BC3/BC5 (~358 MB) and not for RGBA8 — deliberately, because the uncompressed path is an
escape hatch for small textures rather than a way to ship a 1 GB artifact.

Unlike section 9's caps, **a texture cap never truncates**: a cook either produces the whole image or
refuses it. There is no "some of the image", which is why the cook's status is binary.

### 10.12 The writer/reader asymmetry, and interop

**Interop is ONE-DIRECTIONAL, and that is a decision rather than a limitation to be fixed later.** Our
files open in any conforming KTX2 reader. Arbitrary third-party KTX2 files do **not** open in ours: a
valid file from another tool may legitimately differ in its DFD (the specification explicitly permits a
sample's `KHR_DF_SAMPLE_DATATYPE_LINEAR` qualifier to differ), carry key/value data we do not write, or
use a `vkFormat` outside our eight. This is a first-party cooked-asset reader, not a general loader. A
general KTX2 **importer** — dragging a third-party `.ktx2` into a project — is a different feature with
a different owner.

**The DFD byte-match is where the reader is deliberately stricter than the specification.** It is also
where the reader is **circular**: it compares the descriptor against the same table the writer emits, so
every test in this repository passes with a wrong table. Only an external validator can break that
circle, which is why `ktx validate` is a mandatory row on this task's validation page and why the two
corrections in 10.5 were derived from `createdfd.c` rather than copied.

Three things the parser deliberately does **not** check:

1. **That levels do not overlap each other, the tables or the key/value data.** Every read goes through
   the bounds-checked `levelBytes(level)`, so an overlap is a wrong *picture*, never a memory error —
   the same reasoning and the same Phase 5 trigger as section 9.12's second residual.
2. **That there are no trailing bytes after the last level.** The writer emits none; the reader
   tolerates them, exactly as the mesh reader tolerates non-zero trailing padding.
3. **The block CONTENTS.** There is no such thing as an invalid BCn block: every 8- or 16-byte pattern
   decodes to something.

Everything it *does* check is a **subtraction** against a known-good size
(`length <= size && offset <= size - length`), never an addition that can wrap, and **nothing is
allocated before the count it is allocating for has been checked against a frozen cap**.

**THE NAMED, UNOWNED GAP: nothing in this tree can upload one of these files.** `rhi::TextureFormat`
carries no block formats, and adding them is a **contract change, not an enumerator addition** —
`rhi::texelBlockSize` is documented as bytes per *texel* and `uploadTexture`'s precondition is
`data.size() == texelBlockSize(format) × mipWidth × mipHeight`, which is not expressible for a
block-compressed format. That belongs to task **3.4.1**, which depends on this one. The deliverable here
is a file, proven as a file.

### 10.13 Error catalog

`parseCookedTexture` never throws, never reads a file and never logs. It returns one of ten statuses
with a human-readable message; the message is empty **iff** the status is `Ok`. The ladder's order is
part of the contract — a file that is wrong in two ways gets the status of the **first** check it fails.

| Status | Cause |
|---|---|
| `TooSmall` | fewer than 80 bytes, or the level index does not fit |
| `CapExceeded` | the buffer is over `MAX_COOKED_TEXTURE_BYTES` (checked **before any field is read**); or a dimension, or `levelCount`, is outside its range — including a `levelCount` that is legal in itself but impossible for the declared dimensions |
| `BadIdentifier` | the first 12 bytes are not the KTX2 identifier |
| `Supercompressed` | `supercompressionScheme != 0`. A **distinct** status, checked **before** `vkFormat`, because a Basis file's `vkFormat` is legitimately `VK_FORMAT_UNDEFINED` and "unsupported format 0" would be the wrong diagnosis |
| `UnsupportedFormat` | `vkFormat` outside the eight, or `typeSize != 1` |
| `UnsupportedShape` | `pixelDepth != 0`, `layerCount != 0`, `faceCount != 1`, or a **partial mip pyramid** (a `levelCount` strictly between 1 and the image's full chain, 10.8) — the message names which. A count that is *over* the chain or zero is `CapExceeded` instead: those are range violations and a partial chain is not, since 2 is inside 1…4 for an 8×8 image |
| `BadTable` | the DFD or key/value region is misplaced, mis-sized or does not tile exactly; a record declares length 0 (the infinite-loop guard) or overruns; or the global-data pair is non-zero with scheme 0 |
| `BadDescriptor` | the DFD does not byte-match the frozen table for the declared `vkFormat` |
| `BadRange` | a level's `byteLength` is wrong for its dimensions, its `uncompressedByteLength` disagrees, its offset is misaligned, or its range leaves the buffer |
| `Ok` | the buffer is a valid v1 container |

### 10.14 Golden fixtures

Four byte-level goldens live in `tests/cooked_texture_golden.hpp` as annotated in-source arrays, each
carrying the exact RGBA8 texels it was cooked from — so every case **re-cooks** that input and compares
the whole file byte for byte. A golden that is only a captured blob proves nothing about the transform
that produced it.

- **Golden A, `COOKED_TEXTURE_GOLDEN_BC1_4X4`, 344 bytes** — 4×4 BC1-sRGB, three levels, nil GUID.
  `80 + 24×3 = 152`, `+44` DFD `= 196`, `+120` KVD `= 316`, aligned to 8 → 4 pad bytes, three 8-byte
  levels at 320/328/336.
- **Golden B, `COOKED_TEXTURE_GOLDEN_RGBA8_1X1`, 320 bytes** — the smallest legal file: 1×1
  `Rgba8Unorm`, one level, and **zero padding**, which is the point of it. `80 + 24 = 104`, `+92 = 196`,
  `+120 = 316`, already 4-aligned, one 4-byte level at 316.
- **Golden C, `COOKED_TEXTURE_GOLDEN_BC5_5X3`, 400 bytes** — 5×3 BC5, odd in **both** axes, three
  levels, a non-nil GUID. The polyphase filter's asymmetric weights and the 16-byte alignment are both
  visible in its bytes.
- **Golden D, `COOKED_TEXTURE_GOLDEN_BC3_SRGB_2X2`, 352 bytes** — 2×2 BC3-sRGB, two levels. It exists
  because none of the other three carries the corrected `1F` alpha-qualifier byte of 10.5, and it also
  pins BC3's alpha-then-colour composition in bytes.

**They are frozen.** A change to any layout, ordering, padding, filter or encoder rule fails them by
construction. If one has to change, the cook changed — and that is a `COOKED_TEXTURE_COOKER_VERSION`
decision, not a test edit.

---

## 11. Reserved for future formats

- **Cooked / `.pak` binary formats** — Phase 3+, owned by the cooker; own version field, docs/04:51
  applies unchanged. Section appends here. Still unowned: the `.pak` container itself and cooked
  scenes. The cooked **mesh** container is section 9 and the cooked **texture** container is
  section 10.
