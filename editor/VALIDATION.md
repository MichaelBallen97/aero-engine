# Editor gate ledger — `aero_editor`

Every editor task ships a **two-part gate**: the mechanical/structural half (build, full `ctest` on
both presets, the `AERO_REQUIRE_GPU=1` rehearsal, tools-OFF configurations, sabotage proofs, the five
architecture guards, clang-format/clang-tidy) and a **human mouse/keyboard pass recorded per OS**.
CI can prove the first half. It cannot drag a gizmo, judge font crispness on a HiDPI display, or
confirm that an *actual restart* restores a layout a person arranged — that half needs someone at the
machine, and it is recorded here, mirroring `samples/phase-0-cube/VALIDATION.md` and
`samples/phase-1-scene/VALIDATION.md`.

**One file per task, under [`editor/validation/`](validation/).** This index used to be a single
187 KB document; it grew past the point where markdown previewers would render it to the end, which
silently hid the newest task's checklist — exactly the section a human needs while testing. Each task
now lives in its own page, short enough to render anywhere.

## Gate status

| Task | Mechanical | macOS | Windows | Linux |
|---|---|---|---|---|
| [2.1.1 — ImGui docking & editor target](validation/2.1.1-imgui-docking-editor-target.md) | ✅ | ✅ PASS | ⏳ | ⏳ |
| [2.1.3 — editor app shell & main loop](validation/2.1.3-editor-app-shell-main-loop.md) | ✅ | ✅ PASS | ⏳ | ⏳ |
| [2.2.1 — hierarchy panel](validation/2.2.1-hierarchy-panel.md) | ✅ | ✅ PASS | ⏳ | ⏳ |
| [2.2.2 — reflection-driven inspector](validation/2.2.2-reflection-driven-inspector.md) | ✅ | ✅ PASS | ⏳ | ⏳ |
| [2.2.3 — viewport panel](validation/2.2.3-viewport-panel.md) | ✅ | ✅ PASS | ⏳ | ⏳ |
| [2.2.4 — asset browser stub](validation/2.2.4-asset-browser-stub.md) | ✅ | ✅ PASS | ⏳ | ⏳ |
| [2.2.5 — log/console panel](validation/2.2.5-log-console-panel.md) | ✅ | ⚠️ **PARTIAL** — 11/15, 4 BLOCKED | ⏳ | ⏳ |
| [2.3.1 — editor camera](validation/2.3.1-editor-camera.md) | ✅ | ✅ PASS 19/19 | ⏳ | ⏳ |
| [2.3.2 — selection & picking](validation/2.3.2-selection-picking.md) | ✅ | ✅ PASS 21/21 | ⏳ | ⏳ |
| [2.3.3 — ImGuizmo transform gizmos](validation/2.3.3-imguizmo-transform-gizmos.md) | ✅ | ✅ PASS 24/24 applicable | ⏳ | ⏳ |
| [2.4.1 — command stack](validation/2.4.1-command-stack.md) | ✅ | ✅ PASS 16/16 | ⏳ | ⏳ |
| [2.4.2 — property-set + structural commands](validation/2.4.2-property-set-structural-commands.md) | ✅ | ✅ PASS 24/24 | ⏳ | ⏳ |

`⏳` = needs a code-free, on-hardware follow-up, the Phase 0/1 precedent (0.5.3 / 1.4.2). No Windows or
Linux human pass has been run for **any** Phase 2 task yet; that is a known, accumulating debt, not an
oversight per task.

## What needs a human next

**Every Phase 2 macOS pass is now recorded — 2.4.2 passed 24/24 on 2026-07-31, so Epic 2.4 is CLOSED in
code *and* macOS-validated.** That pass is what turned this task's two uncovered claims into evidence:
row 6 is the **only** proof anywhere in this tree that the Inspector panel itself applies the D17
merge-chain gate pair correctly (no test target can construct `InspectorPanel` — it is src-private and
ImGui-bound; `field_command_test.cpp`'s P8 pins the *policy* against a bare `CommandStack`, not the
panel, and seeding S9/S10 into all seven arms leaves the suite 94/94), and row 11 is the canonical
sequence spec §0-D2 exists for — move the Cube with the gizmo, delete it, undo twice, and the move
undoes too.

**The whole remaining debt in this ledger is now non-macOS, plus 2.2.5's four rows.**

What is left:

- **[2.2.5 — log/console panel](validation/2.2.5-log-console-panel.md) · 4 rows still BLOCKED.** They
  could not be run because the editor emitted no log record after startup. **Three triggerable sources
  now exist** — 2.3.3's degenerate-transform gizmo WARN and 2.4.1's Debug-build `⌘Z` (both confirmed
  working by human passes), plus 2.4.2's own Debug-build `⌘Z`, which now varies its label
  (`Delete Entity`, `Rename`, `Add MeshRenderer`, …) instead of always reading `Transform`. Nothing
  blocks these four rows any more; they have not been re-run against any of the three sources yet.
  **This is the cheapest open item in the ledger.**
- **[2.4.2 — property-set + structural commands](validation/2.4.2-property-set-structural-commands.md)
  · macOS ✅ 24/24 (2026-07-31); Windows and Linux pending.** Row 22's second clause — re-running 2.2.5's
  four rows — was **not** performed in that pass, so it did not close the item above.
- **Windows and Linux — every task, all twelve.** No non-macOS human pass has been run for any Phase 2
  task. That is accumulating debt across the whole phase, not a per-task gap, and it is worth scheduling
  deliberately rather than discovering at the phase gate. Each task page carries its own "How to
  validate one OS" section; the rows are identical per platform except where a page marks one
  platform-only (2.3.3's row 23 is Linux-only).

## Conventions for these pages

Each task page follows the same shape, and a new one should too:

1. `# Task N — name` with the deliverable, what CI proves automatically, and **what it cannot prove**.
2. `## What was mechanically verified` — measured numbers, never predicted, plus the sabotage table
   and its honest outcomes (a seed that discriminates *nothing* is recorded as such, never given
   invented coverage).
3. `## Known-and-expected, NOT a defect` — so a documented trade-off is never filed as a bug.
4. `## How to validate one OS` — the numbered instructions a human follows.
5. `## Validation records` — the same rows as one-line checkboxes, then `### macOS` / `### Windows` /
   `### Linux` with the result per platform.

**Never record a PASS in the same pass that writes the code.** A row is `⏳ pending` until a person has
actually performed it on that OS.
