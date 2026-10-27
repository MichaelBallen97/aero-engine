---
paths:
  - "engine/scene/**"
  - "engine/scene_serialize/**"
  - "engine/scene_audio/**"
  - "engine/reflect/**"
  - "tools/reflect-gen/**"
  - "cmake/reflect.cmake"
  - "tests/scene_*"
  - "tests/reflect-gen/**"
  - "editor/src/inspector*"
  - "editor/src/editor_reflection*"
  - "editor/src/entity_*"
  - "editor/src/create_menu*"
  - "tests/editor/inspector*"
  - "tests/editor/hierarchy*"
  - "tests/editor/create_menu*"
---

# Components, reflection and serialization

Each rule here was earned by a defect, a green-and-wrong test or a measurement that contradicted a plan, and every one binds new work. The story behind each is in `docs/10-engineering-log.md`; grep it for the citation.

## The built-in components

**THE BUILT-IN COMPONENT COUNT IS TEN**: `Transform`, `Camera`, `DirectionalLight`, `PointLight`,
`MeshRenderer`, `AnimationPlayer`, `AudioSource`, `AudioListener`, `Environment` (E.2.1), `SpotLight`
(E.2.2). Nothing since E.2.2 has added one, so the five-generation-site rule and the component-count sweep
have not fired since — **they still apply in full to the next built-in, whenever one arrives.**


## Components, reflection and serialization

**`AERO_BUILTIN_COMPONENT_HEADERS` NAMES THE BUILT-IN COMPONENT HEADERS ONCE**, at root scope. It reaches
**four** generation sites and **three of the four are silently optional** — a component added to the editor's
list and not the serializer's is registered, inspectable, editable and **NOT SAVED**, with every test green.
**The site the variable does not reach is the one that matters**:
`engine/scene_serialize/src/scene_serialize.cpp`'s hand-written dispatch table, plus
`builtin_serializers.hpp`'s declarations. All five move together, or the result is a link failure or — far
worse — green and wrong.

**A COMPONENT-COUNT LITERAL IS NOT CONFINED TO THE TESTS THAT ARE ABOUT COMPONENTS.** 3.7.2 found them in
`scene_test.cpp`, `transform_test.cpp`, `editor/hierarchy_test.cpp`, `editor/inspector_test.cpp` and
`scene_serialize_test.cpp` — **every one found by a red test, none by a grep.** E.5.2 added three more the
next built-in must update: `RF7`'s hand-written ten-name `BUILTINS` array
(`tests/reflect-gen/meta_test.cpp:534-537`) and `IR21`'s two `== 10` pins
(`tests/editor/inspector_test.cpp:2536`, `:2545`). **E.6.3 added two sites no literal reaches**:
`componentIconTable()` and `entityKindFor` (`entity_presentation.cpp`) — a built-in left out of either draws the
fallback `shapes` icon or an `Empty` kind dot with every test green; `EK5`'s two `== 10U` pins move only when a
row is added.

**AND THE GREP ALSO OVER-MATCHES.** E.2.1's sweep came to **21** built-in literals (not the 18 planned) and
**19** default-scene pins, against **five look-alikes that had to stay unchanged**: `scene_test.cpp:224`
/`:1175`, `scene_serialize_test.cpp:596`, `hierarchy_test.cpp:468`, `scene_io_test.cpp:281`. Three of the 21
are reachable by **no grep at all** — a component *tally* rather than a type count, a literal on the line
*after* its `componentTypeCount()` call, and a **TEST-CASE NAME** (`PB13`'s `400-byte Lights block`, with its
arithmetic in prose beneath it). **E.2.2 re-measured the default-scene pins: SEVENTEEN `entityCount() == 4`
lines, not ten** — sixteen real pins plus `hierarchy_test.cpp:635` (duplicate-entity arithmetic). **Carry the
LIST: "unchanged from ten" is a different claim from "unchanged from seventeen". A count is only as good as
the grep behind it — re-run every one, and classify each hit by READING ITS COMMENT, never by its shape.**

**`createEntity` (`entity_ops.cpp:68`) ALWAYS ADDS A `Transform`.** A seeded entity that must not carry one is
built with `world.create()` + `setName` directly — never `createEntity` + `remove<Transform>`. E.2.1's
"Environment" seed is the precedent. **And the assertion it broke is the lesson:** `hierarchy_test`'s
`for (root) CHECK(has<Transform>)` was true only *by accident*, and is now four per-entity statements with
`CHECK_FALSE` on the fourth. **A universal that is true by accident hides the seed that falsifies it.**

**THE VOCABULARY SPLIT (E.3.3): reflect-gen validates a GRAMMAR and the EDITOR owns the VOCABULARY**, because
`AssetKind` is an editor type the engine's reflect layer must not know. **Do not add a kind list to the tool.**
`AERO_ASSET(kind)` is the fourth field annotation; a fifth `DropSurface` with a NON-DEFAULTED fourth parameter
routes the field through 3.1.5's own matrix.

**`AERO_LABELS` NEEDS `AERO_RANGE(0, N-1)` ON THE SAME FIELD (E.5.2)**, and the editor's `namedSelectorRow`
re-checks it, falling back to the drag. **A new reflect-gen annotation arm goes ABOVE the unknown-`engine::`
catch-all** (`S24`), and a new `FieldUiMeta` member is appended LAST, so older customs stay byte-identical.
