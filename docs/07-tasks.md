# 07 — Tasks (index)

> The full work breakdown lives in **one file per phase** under [`docs/tasks/`](./tasks/) — every phase decomposed to epic → task → subtask depth, each task with a **Goal**, a **Deliverable**, a priority, an estimate, and dependencies. This replaced the original "detailed near, coarse far" approach in the July 2026 re-plan: all nine phases are now planned at full depth up front, and refined (not re-invented) as each phase begins.

---

## Legend

- **Estimates:** **S** ≈ a few days · **M** ≈ 1–2 weeks · **L** ≈ 2–4 weeks. Phase estimates are part-time solo months.
- **Priorities:** **P0** blocks the phase gate · **P1** needed for the phase · **P2** nice-to-have.

## Numbering conventions

- `N.M` = epic `M` of phase `N` · `N.M.K` = task `K` of that epic. Subtasks are listed as plain bullets under their task — they define scope, they do not track progress.
- **Numbering is append-only.** New work gets the next free number in its phase (that is how the July 2026 audit added 2.6, 3.6, 3.7, 4.7); existing numbers are never reused or shuffled, so references in docs, Notion, and commits stay valid forever.

## The phases

| # | Phase file | Est. | Deliverable gate |
|---|---|:--:|---|
| 0 | [Foundations & First Triangle](./tasks/phase-0.md) | 2–3 mo | Spinning textured cube @60fps on mac/win/linux; CI green (ASan/UBSan) from commit #1 |
| 1 | [Reflection, ECS & Serialization](./tasks/phase-1.md) | 2–3 mo | Define a component → save/load a scene as JSON → it renders |
| 2 | [The Editor Shell](./tasks/phase-2.md) | 2–3 mo | Create a project; create / move / edit / save entities visually |
| 3 | [Asset Pipeline & 3D Content](./tasks/phase-3.md) | 3–5 mo | Drop a rigged glTF/FBX in → PBR materials + shadows + a playing animation + an audible sound |
| 4 | [Scripting (the language fork)](./tasks/phase-4.md) | 3–5 mo | Press Play in the editor: hot-reloaded TS or native C++ drives entities; press Stop: the scene restores exactly |
| 5 | [Export & Ship a Game 🎯](./tasks/phase-5.md) | 2–3 mo | A downloadable, playable 3D game made entirely in Aero Engine (desktop) |
| 6 | [Mobile Runtime & Audio Depth](./tasks/phase-6.md) | 2–3 mo | The Phase 5 game running on a phone |
| 7 | [2D Support](./tasks/phase-7.md) | 2–3 mo | A working 2D scene: Box2D physics + sprites + text |
| 8 | [Rendering Maturity & v1.0](./tasks/phase-8.md) | 2–4 mo | Tagged v1.0 release, documented, with sample projects |

**Census:** 50 epics · 130 tasks across the nine phases (July 2026 re-plan).

## Conventions

- **Gate artifacts:** every phase gate produces something committed under `/samples` (e.g. `/samples/phase-0-cube/`). A gate that left nothing behind didn't happen (project rule #2).
- **Notion mirroring:** execution tracking lives in the Notion Build Tracker's three linked databases — **Phases**, **Epics / Deliverables**, and **Tasks**. Phases, epics, and tasks are rows; **subtasks are to-do checklists inside their task's page**. These docs are the source of truth for scope and content; Notion tracks status. On any conflict, the docs win and Notion gets corrected. **No progress state lives in these files** — the subtask bullets are deliberately not checkboxes; the only tickable checklist for a task is the one in its Notion page.
- **Re-planning:** at each phase boundary the next phase's file is reviewed and refined (estimates, subtask detail) before work starts — refinement is expected, wholesale restructuring is a smell worth investigating.
