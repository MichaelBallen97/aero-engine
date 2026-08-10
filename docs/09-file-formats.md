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

## 9. Reserved for future formats

- **Cooked / `.pak` binary formats** — Phase 3+, owned by the cooker; own version field, docs/04:51
  applies unchanged. Section appends here.
