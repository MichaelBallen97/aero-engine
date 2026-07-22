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

### 2.3 Components

The member **key** of a `"components"` object is the reflected type's fully-qualified C++ name,
exactly as `reflect-gen` registers it — the same string `--emit-meta` uses for `.type()` and the
generated `aeroReadJson` readers use in their own WARN messages.

The member **value** is the payload object `aeroWriteJson` emits for that component: one key per
supported field, in declaration order, with unsupported fields simply absent. This document defines
the payload shape **by reference** to the generated serializers (tasks 1.2.1/1.2.2) — it does not
restate field-level shapes. The field-level tolerance policy those generated readers apply, restated
here for convenience:

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
fact).

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

Full (fictional component names — real ones arrive at task 1.3.2; the envelope never resolves them):

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
| Hierarchy | `entities[<i>] (id <id>): parent <p> does not reference any entity id in this scene` |
| Hierarchy | `entities[<i>] (id <id>): parent chain is cyclic` |

Success-only warnings (document order, `"scene: "`-prefixed):

| Warning |
|---|
| `scene: ignoring unknown key "<k>"` (root) |
| `scene: entities[<i>] (id <id>): ignoring unknown key "<k>"` (entity) |

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

## 4. Reserved for future formats

- **`.meta` (asset GUID sidecars)** — Phase 2. Section appends here.
- **Cooked / `.pak` binary formats** — Phase 3+, owned by the cooker; own version field, docs/04:51
  applies unchanged. Section appends here.
