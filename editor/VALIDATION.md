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
| [**2.4.1 — command stack**](validation/2.4.1-command-stack.md) | ✅ | **⏳ pending — 16 rows** | ⏳ | ⏳ |

`⏳` = needs a code-free, on-hardware follow-up, the Phase 0/1 precedent (0.5.3 / 1.4.2). No Windows or
Linux human pass has been run for **any** Phase 2 task yet; that is a known, accumulating debt, not an
oversight per task.

## What needs a human next

- **[2.4.1 — command stack](validation/2.4.1-command-stack.md) · 16 rows, none run.** Its
  `## Validation records` section is a tickable checklist. **Rows 3 and 5 are the priority**: they are
  the *only* guard on the code-review round's blocking AC-16 fix — one continuous gizmo drag must undo
  in a single step. No test in either tier can reach that ordering (tier-0 cannot construct the panel,
  the GPU-gated target cannot inject mouse input), so if it regresses, CI stays green. Row 9 (rename
  field focused + `Ctrl/⌘+Z` must undo the *text*, not move the scene) is the other row with no
  mechanical cover.
- **[2.2.5 — log/console panel](validation/2.2.5-log-console-panel.md) · 4 rows BLOCKED.** They could
  not be run because the editor emitted no log record after startup. Two triggerable sources exist
  now — 2.3.3's degenerate-transform gizmo WARN, and (as of 2.4.1) a Debug-build `⌘Z`, which logs one
  `DEBUG editor: undo 'Transform' (…)` per undo — but the four rows have not been re-run against
  either.
- **Windows and Linux, every task.** Each task page carries its own "How to validate one OS" section;
  the rows are the same on every platform except where a page marks one platform-only.

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
