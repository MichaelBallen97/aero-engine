# Phase 2 gate artifact — a scene built entirely in the editor

**Gate (docs/tasks/phase-2.md):** *Create a project; create / move / edit / save entities visually.*

This directory **is** an Aero project, because that is what the gate asks for — Phase 2 shipped the
project system (epic 2.6), so "create a project" is half the sentence being proved. It is not a
sample application: there is no `main.cpp` and no `CMakeLists.txt`, and it is deliberately absent
from `samples/CMakeLists.txt`. The other three samples are compile-proofs of engine code; this one is
a **provenance proof** of the editor, and the only thing it has to demonstrate is that a person drove
the tool and the tool produced a valid, reloadable document.

## What must be here

```
phase-2-editor-scene/
├── project.json            the manifest the editor's New Project flow wrote
├── assets/                 scaffolded by New Project (empty until Phase 3)
└── scenes/
    └── <name>.scene.json   the scene, saved from the editor's File ▸ Save Scene
```

`assets/` and `scenes/` carry a `.gitkeep` only so the scaffold survives an empty commit; delete each
one once real content lands beside it.

## How to produce it

Everything below happens **in `aero_editor`**, with the mouse. That is the entire point — a
hand-written `scene.json` would be byte-identical to a real one and would prove nothing, which is
exactly the property the gate exists to test.

1. `File ▸ New Project…`, location `samples/`, name `phase-2-editor-scene`. The editor scaffolds
   `project.json`, `assets/` and `scenes/`, adopts the project, and resets to a fresh scene.
2. Build a scene with the panels alone — no code, no hand-edited JSON:
   - **create** entities (Hierarchy right-click, or `⌘D` to duplicate),
   - **parent** at least one entity to another, so the saved file exercises the hierarchy,
   - **move / rotate / scale** with the ImGuizmo gizmos in the Viewport,
   - **edit** at least one reflected field in the Inspector that is not a Transform,
   - **rename** at least one entity (double-click or `F2`).
3. `File ▸ Save Scene` into this project's `scenes/`.
4. Confirm the round trip the gate actually claims: `File ▸ New Scene`, then `File ▸ Open Scene` on
   the file you just wrote. The scene must come back identical — same entities, same names, same
   parenting, same transforms.

## Why the round trip is the load-bearing step

Saving proves the writer ran. Only reopening proves the document is **real** — that the editor can
consume its own output. Epic 2.5's Definition of Done is "save → close → reopen → identical scene",
and step 4 is the only step in this list that tests it end to end through the UI rather than through
`scene_serialize`'s unit tests.

## What this artifact is not

It is not a substitute for the per-task human validation in `editor/VALIDATION.md`. Those record that
each *feature* works on each *OS*; this records that the *workflow* produces a document. Both are
owed, and passing this one does not close any row there.
