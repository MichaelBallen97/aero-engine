# Phase 8 — Rendering Maturity & v1.0 · est. 2–4 mo

> **Goal:** resolve the deferred rendering decision **with data**, raise visual quality onto the 3.6 foundations, optimize with profiler evidence, document everything, and ship v1.0.
>
> **Gate:** Tagged v1.0 release, documented, with sample projects
>
> **Phase non-goals:** no new subsystems — this phase finishes, decides, polishes, and ships. Ray tracing, GI, terrain etc. remain future-roadmap items regardless of how tempting they look now.
>
> **Gate artifact:** the v1.0 tag itself, plus the sample projects shipped with it.

---

## Epic 8.1 — Render architecture decision · render

**Goal:** the forward+ vs deferred decision, deferred since day one (docs/08), gets resolved the only honest way: with Tracy captures of representative scenes.
**Definition of Done:** decision recorded ADR-style with the data attached; the renderer restructured accordingly.

### 8.1.1 Profiling scenes + Tracy captures · P0 · M · depends: 3.6.2, 7.2.1
**Goal:** build the evidence before the argument.
**Deliverable:** representative light/geometry stress scenes with captured Tracy profiles on desktop + mobile.
Subtasks:
- Stress scenes (many lights / heavy geometry / mixed)
- Tracy captures across desktop GPUs + one mobile device

### 8.1.2 Decide + implement · P0 · L · depends: 8.1.1
**Goal:** commit to forward+ or deferred (or tiled variant) and make the renderer match.
**Deliverable:** ADR-009 appended to docs/02 with the data; renderer restructured to the chosen architecture.
Subtasks:
- ADR-009 written from the captures
- Renderer restructure to the decision

---

## Epic 8.2 — Lighting & shadow quality + post stack · render

**Goal:** *(reworded by the July 2026 audit, D3)* the **quality** pass on what 3.6 established: cascades, soft shadows, and a small post stack.
**Definition of Done:** v1.0's visual bar: stable cascaded shadows and tasteful post effects, on by default in samples.

### 8.2.1 Cascaded shadow maps + soft shadows · P0 · L · depends: 3.6.2, 8.1.2
**Goal:** from "one shadow map" to production shadows.
**Deliverable:** CSM (2–4 cascades) with stable splits and PCF/PCSS-lite softening.
Subtasks:
- Cascade splits + stabilization; per-cascade rendering
- Soft filtering; quality settings

### 8.2.2 Post stack: bloom + AA · P1 · M · depends: 3.6.3, 8.1.2
**Goal:** grow 3.6.3's tonemap pass into a small, ordered post stack.
**Deliverable:** bloom and FXAA (TAA stays out of v1) behind per-camera settings.
Subtasks:
- Post-stack ordering scaffold over 3.6.3
- Bloom; FXAA; per-camera toggles

---

## Epic 8.3 — Performance passes · core · render

**Goal:** profiling-driven optimization — Tracy decides what's worked on, nothing else.
**Definition of Done:** measured wins recorded; no regressions; mobile included.

### 8.3.1 CPU pass · P1 · M · depends: 8.1.2
**Goal:** frame-time CPU costs: job usage, culling, uploads.
**Deliverable:** documented before/after Tracy captures for each change.
Subtasks:
- Job-system utilization; culling + upload batching
- Before/after captures committed to the perf log

### 8.3.2 GPU pass · P1 · M · depends: 8.1.2
**Goal:** draw batching, state sorting, bandwidth (mobile especially).
**Deliverable:** same evidence discipline, desktop + mobile.
Subtasks:
- Batching/state-sorting improvements; bandwidth on mobile
- Before/after captures committed to the perf log

---

## Epic 8.4 — Documentation · docs

**Goal:** it is open source — docs are part of the product (docs/03 repo layout says so on purpose).
**Definition of Done:** a stranger ships a tiny game using only the docs.

### 8.4.1 Getting-started tutorial · P0 · M
**Goal:** the golden path: install → project → script → play → export.
**Deliverable:** step-by-step tutorial covering both languages' happy paths.
Subtasks:
- TS-path tutorial; C++-path tutorial; screenshots

### 8.4.2 Scripting API reference · P0 · M · depends: 4.2.2
**Goal:** reference generated from the same source as the autocomplete — the `.d.ts` output.
**Deliverable:** browsable API reference generated from the shipped `.d.ts`.
Subtasks:
- Generation pipeline from `.d.ts`; publish with the docs

### 8.4.3 Architecture/engine docs refresh · P1 · M
**Goal:** the planning docs graduate into as-built documentation.
**Deliverable:** docs/ aligned with shipped reality (deltas from the plan recorded, not hidden).
Subtasks:
- Sweep docs/ for drift vs shipped v1.0; fix or annotate

---

## Epic 8.5 — Editor UX polish · editor

**Goal:** the difference between "works" and "pleasant": shortcuts, defaults, first-run experience.
**Definition of Done:** a week of daily dogfooding without a crash or a rage-quit moment.

### 8.5.1 Polish pass · P1 · M
**Goal:** fix the hundred small things the game and samples surfaced.
**Deliverable:** shortcut map, sane default layout, empty-state screens, first-run flow.
Subtasks:
- Shortcut audit + defaults; layout presets
- Empty states; first-run project-creation flow
- One-week dogfood log; zero crashes

---

## Epic 8.6 — v1.0 release · build-ci

**Goal:** ship it: tag, notes, samples, announcement.
**Definition of Done:** the phase gate — and with it, the project's v1.0 promise.

### 8.6.1 Ship v1.0 · P0 · M · depends: 8.2.1, 8.4.1, 8.5.1
**Goal:** the release ritual, done properly.
**Deliverable:** `v1.0.0` tag; release notes; sample projects packaged; announcement post published.
Subtasks:
- Tag + release notes + packaged samples
- Announcement (blog/README/socials); AUTHORS/credits pass

---

**Phase gate:** Tagged v1.0 release, documented, with sample projects.
