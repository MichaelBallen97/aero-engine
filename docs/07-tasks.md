# 07 — Tasks (index)

> The full work breakdown lives in **one file per phase** under [`docs/tasks/`](./tasks/) — every phase decomposed to epic → task → subtask depth, each task with a **Goal**, a **Deliverable**, a priority, an estimate, and dependencies. This replaced the original "detailed near, coarse far" approach in the July 2026 re-plan: all nine phases are now planned at full depth up front, and refined (not re-invented) as each phase begins.

---

## Legend

- **Estimates:** **S** ≈ a few days · **M** ≈ 1–2 weeks · **L** ≈ 2–4 weeks. Phase estimates are part-time solo months.
- **Priorities:** **P0** blocks the phase gate · **P1** needed for the phase · **P2** nice-to-have.

## Numbering conventions

- `N.M` = epic `M` of phase `N` · `N.M.K` = task `K` of that epic. Subtasks are listed as plain bullets under their task — they define scope, they do not track progress.
- **Numbering is append-only.** New work gets the next free number in its phase (that is how the July 2026 audit added 2.6, 3.6, 3.7, 4.7); existing numbers are never reused or shuffled, so references in docs, Notion, and commits stay valid forever.
- **A phase inserted between two existing phases is LETTERED, not fractioned.** Phase E (Editor Experience) executes between Phase 3 and Phase 4, and its epics are `E.1`…`E.6` with tasks `E.n.k`. It could not be "Phase 3.5": `3.5` is already Phase 3's Skeletal-animation epic and `3.5.1`/`3.5.2` are already its tasks, so a fractioned phase would collide head-on with numbers that are referenced from docs, Notion and commit messages. A letter collides with nothing, reads unambiguously in a commit scope (`feat(E.3.2): …`), and keeps the rule above intact — nothing was renumbered to make room. In the Notion Build Tracker the row carries `Phase # = 3.5`, which is a sort key for the timeline view, not an identifier.
- **Phase F (Editor Workflow) follows the same rule.** It executes between Phase 4 and Phase 5, and its epics are `F.1`…`F.4` with tasks `F.n.k`. It could not be "Phase 4.5": `4.5` is Phase 4's C++-project-mode epic and `4.5.1`/`4.5.2` are its tasks. Its Notion row carries `Phase # = 4.5`, again a sort key.
- **Phase G (3D Physics) follows it too.** It executes between Phase F and Phase 5, and its epics are `G.1`…`G.5` with tasks `G.n.k`. A fraction between 4 and 5 would read as one of Phase 4's own epics — `4.5`, `4.6` and `4.7` are all taken. Its Notion row carries `Phase # = 4.75`, a sort key that places it after Phase F's `4.5`.

## The phases

| # | Phase file | Est. | Deliverable gate |
|---|---|:--:|---|
| 0 | [Foundations & First Triangle](./tasks/phase-0.md) | 2–3 mo | Spinning textured cube @60fps on mac/win/linux; CI green (ASan/UBSan) from commit #1 |
| 1 | [Reflection, ECS & Serialization](./tasks/phase-1.md) | 2–3 mo | Define a component → save/load a scene as JSON → it renders |
| 2 | [The Editor Shell](./tasks/phase-2.md) | 2–3 mo | Create a project; create / move / edit / save entities visually |
| 3 | [Asset Pipeline & 3D Content](./tasks/phase-3.md) | 3–5 mo | Drop a rigged glTF/FBX in → PBR materials + shadows + a playing animation + an audible sound |
| E | [Editor Experience](./tasks/phase-E.md) | 3–4 mo | Open a project and land in the scene you were last editing, on a lit grid floor under a sky; create a Cube from the menu, drop a material on it and see it shade; aim a spot light with a visible gizmo; rename, move and delete assets without leaving the editor |
| 4 | [Scripting (the language fork)](./tasks/phase-4.md) | 3–5 mo | Press Play in the editor: hot-reloaded TS or native C++ drives entities; press Stop: the scene restores exactly |
| F | [Editor Workflow](./tasks/phase-F.md) | 2–3 mo | Open the editor and it fills the screen; drag a model in from the file manager and it imports; click the object under the cursor, even one standing behind another object's bounding box, and delete it with the Delete key; select a camera and see what it sees; create a script and a second scene from the browser and switch scenes from the breadcrumb; save a script while the game plays and see the change at once |
| G | [3D Physics](./tasks/phase-G.md) | 3–4 mo | In the editor, give a cube a Rigid Body and a collider and see the collider drawn around it; press Play and it falls, lands on the floor and comes to rest; a ball pushed by a script knocks over a stack of crates; a character walks up a ramp and stops at a wall; a door swings on its hinge when the character pushes it; a script hears a trigger volume and finds a crate with a raycast; press Stop and the scene restores exactly; and the same scene runs as a native C++ game |
| 5 | [Export & Ship a Game 🎯](./tasks/phase-5.md) | 2–3 mo | A downloadable, playable 3D game made entirely in Aero Engine (desktop) |
| 6 | [Mobile Runtime & Audio Depth](./tasks/phase-6.md) | 2–3 mo | The Phase 5 game running on a phone |
| 7 | [2D Support](./tasks/phase-7.md) | 2–3 mo | A working 2D scene: Box2D physics + sprites + text |
| 8 | [Rendering Maturity & v1.0](./tasks/phase-8.md) | 2–4 mo | Tagged v1.0 release, documented, with sample projects |

**Census:** 65 epics · 189 tasks across the twelve phases — 50 epics · 133 tasks for phases 0–8, plus Phase E's 6 · 24, Phase F's 4 · 15 and Phase G's 5 · 17. Measured off `docs/tasks/*.md`, not remembered.

## Conventions

- **Gate artifacts:** every phase gate produces something committed under `/samples` (e.g. `/samples/phase-0-cube/`). A gate that left nothing behind didn't happen (project rule #2).
- **Notion mirroring:** execution tracking lives in the Notion Build Tracker's three linked databases — **Phases**, **Epics / Deliverables**, and **Tasks**. Phases, epics, and tasks are rows; **subtasks are to-do checklists inside their task's page**. These docs are the source of truth for scope and content; Notion tracks status. On any conflict, the docs win and Notion gets corrected. **No progress state lives in these files** — the subtask bullets are deliberately not checkboxes; the only tickable checklist for a task is the one in its Notion page.
- **Re-planning:** at each phase boundary the next phase's file is reviewed and refined (estimates, subtask detail) before work starts — refinement is expected, wholesale restructuring is a smell worth investigating.
