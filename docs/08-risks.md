# 08 — Risks & Open Decisions

> A living register. Review at the start of each phase. Likelihood/Impact are Low / Med / High.

---

## Risk register

| # | Risk | L | I | Mitigation |
|---|---|:--:|:--:|---|
| **R1** | **Learning C++ solo** while building an engine — the #1 project risk (ADR-001) | High | High | Phase 0 budgeted as a learning phase; handles-not-pointers; ASan/UBSan from commit #1; never manual `new`/`delete` |
| **R2** | **Reflection generator complexity** (`reflect-gen`, ADR-004) — the #2 technical risk; feeds four systems | Med | High | Start with the minimal subset (plain structs + primitives + `Vec3`/`Quat`); build it in Phase 1 before anything depends on it; native `std::meta` migration path kept open |
| **R3** | **SDL3 dependency concentration** — it is platform + render + (via shadercross) shaders at once; single point of failure | Med | High | The `engine/rhi` wrapper is the escape hatch — treat it as sacred; boundary CI guards; SDL_GPU is otherwise economical and proven |
| **R4** | **SDL_shadercross is still preview** (v3.0.0-preview2, April 2026; re-verified July 2026, third stack pass — unchanged) | Med | Med | Isolated behind `tools/shaderc`; documented fallback to raw DirectX Shader Compiler + SPIRV-Cross if a preview bug blocks us |
| **R5** | **Scope creep → solo abandonment** — the classic engine-project death | High | High | The non-goals list ([06](./06-scope-and-non-goals.md)) is load-bearing; project rule #2 (every phase ships something usable); realistic 20–32 month horizon; re-plan per phase |
| **R6** | **Two export pipelines** (TS instant + C++ native, ADR-008) double the export surface | Med | Med | Sequence them: C++ authoring is free/early (dogfooding), TS layer is the dedicated build; consolidate both in Phase 5 |
| **R7** | **Cross-platform build pain** (CMake + vcpkg on 3 OSes) | High | Med | CI from commit #1; CMake presets; pinned vcpkg baseline; `main` always green |
| **R8** | **Mobile signing & platform quirks** (iOS/Android) surface late | Med | Med | Deferred to Phase 6 by design; Mac required for iOS; precompiled-runtime model keeps user-side export toolchain-free for TS projects |
| **R9** | **quickjs-ng scripting performance** (no JIT) becomes a gameplay bottleneck | Low | Med | Move hot logic to a C++ ECS system (ADR-007), not a language change; heavy work already lives in C++ |
| **R10** | **Time estimates slip** — part-time solo | High | Med | Estimates are planning aids, not commitments; the deliverable-gate model surfaces slippage early; re-estimate at each phase boundary |
| **R11** | **In-process play-in-editor** (4.7) — a gameplay crash takes the editor down with it | Med | Med | Autosave-before-play (4.7.3); scene snapshot & exact restore (4.7.2); separate-process play mode deferred to v2 |
| **R12** | **vcpkg's shared per-triplet `include/` directory limits the "private backend" compile-time boundary** (found in task 0.2.2) — every port installs into one flat `vcpkg_installed/<triplet>/include/`, and every vcpkg CONFIG target's `INTERFACE_INCLUDE_DIRECTORIES` is that same shared root, not a per-package directory. `PRIVATE`-linking a backend library (e.g. `aero_core`'s GLM link, ADR-005) makes a stray `#include` a hard compile error **only** for targets that link no vcpkg package directly; any target that does (e.g. `tests/` → `doctest::doctest`, or a future `engine/platform` → SDL3) inherits the whole shared root and can resolve the "private" backend's headers regardless | Med | Low | Engine layers (which link no vcpkg package directly) get the compile-time guarantee for free — verified for ADR-005/task 0.2.2 with a throwaway probe target, now the permanent `tests/math_boundary_probe.cpp` (task 0.2.3). Test targets and any layer linking a vcpkg package directly do not, and must rely on the textual grep guards docs/04 already specifies — **math's mitigation is now in place**: `.github/scripts/check-math-boundary.sh`, run by the `lint` job (task 0.2.3). R12 itself stays **open**: audio (3.7.3) and RHI (0.4.5) still inherit this same limitation and need their own grep + probe pair |

---

## Open decisions

Decisions deliberately deferred until there is data or the relevant phase arrives.

| Decision | Decide at | Notes |
|---|---|---|
| **Render architecture: forward+ vs deferred** | Phase 8 | With real Tracy profiling data — not before |
| **Long-term editor UI: ImGui forever?** | Phase 8 | ImGui is right for v1; revisit only with evidence |
| **Migrate to native C++26 `std::meta` reflection** | When MSVC ships support (~2027–2028) | `reflect-gen` output boundary designed for this swap (ADR-004) |
| **Spatial audio: Steam Audio / Resonance** | Phase 5+ | Optional HRTF/occlusion layer over miniaudio (ADR-006) |
| **Math backend: RTM instead of GLM** | If profiling shows GLM cost | Swappable by design (ADR-005); no engine changes needed |
| **Split Phase 3 into 3a/3b** | Start of Phase 3 | It is the largest phase (3–5 mo after the July 2026 additions); natural cut: 3a = 3.1–3.3, 3b = 3.4–3.7 |
| **esbuild vs swc for TS transpilation** | Task 4.3.1 | Default **esbuild** — single Go binary, no Node runtime, invoked externally (the Blender-CLI pattern); swc is the fallback |
| **HUD-text mechanism for the Phase 5 game** | Task 5.5.3 | Planned: editor-baked msdf atlas → engine-internal textured quads; formalized into the real text system at 7.3 |
| **Tilemap import format (JSON vs Tiled subset)** | Task 7.2.2 | Record the choice here when taken |

---

## Resolved decisions (for the record)

- **Project name** → **Aero Engine** (July 2026)
- **Scripting VM** → **quickjs-ng** over Bellard's QuickJS (July 2026)
- **Scripting model** → **TypeScript OR C++ per project, never combined** (ADR-008, July 2026)
- **"Ship a game" milestone** → moved from Phase 4 to **Phase 5** (needs scripting + export first)
- **2D** → **Phase 7**, after the 3D foundation is proven
- **July 2026 plan audit** → third tech-stack pass: **zero swaps** (all pillars verified active; doctest's maintainership confirmed resumed); epics **2.6** (project system), **3.6** (rendering essentials), **3.7** (audio playback v0), **4.7** (play-in-editor) added; audio & shadows pulled forward to Phase 3; imnodes dropped from the v1 table (v2 graph editors); full-depth breakdown for all phases in `docs/tasks/`; Notion consolidated to the 3 linked DBs (duplicate "Work Breakdown" DB deleted), tasks-as-rows + subtask checklists

### The July 2026 audit findings (D1–D12)

The gap-fix codes cited as *(audit Dn)* throughout `docs/tasks/`:

| # | Gap found | Fixed by |
|---|---|---|
| D1 | Play-in-editor was in v1 scope but no phase built it | Epic 4.7 (+ risk R11, autosave-before-play) |
| D2 | Phase 5 game would ship silent — all audio waited for Phase 6 | Epic 3.7 Audio playback v0 (buses/effects stay in 6.4) |
| D3 | Game would ship shadowless, with no culling | Epic 3.6 Rendering essentials (8.2 became the *quality* pass) |
| D4 | Nothing created the *project* concept (ADR-008 and the AssetDatabase both need it) | Epic 2.6 Project system v0 |
| D5 | Scripts couldn't spawn anything at runtime | 4.4.4 prefab-lite (overrides/nesting → v2) |
| D6 | TS runtime errors would point at compiled JS, not the `.ts` source | 4.3.4 source-mapped errors (DAP debugger → v2) |
| D7 | Input under-planned: raw kb/mouse in 0.3.2, then nothing | 4.4.3 script input API; 6.1.2/6.2.2 touch (action-mapping → v2) |
| D8 | Game needs HUD text; text system was Phase 7; runtime can't link ImGui | 5.5.3 minimal msdf HUD text (formalized by 7.3) |
| D9 | Runtime player binary and two CI guards had no owning task | 5.2.1 player, 5.2.2 purity guard, 0.4.5 RHI guard |
| D10 | Mobile CI unstated | 6.5.1 mobile CI lanes (5-platform runtime coverage) |
| D11 | Missing non-goals — scope-creep holes around Phase 5 | 7 new non-goal rows in `docs/06` (each with a future-roadmap entry) |
| D12 | Editor QoL invisible in the plan | 3.1.4 hot-reload watcher, 3.1.3 asset browser v1, 5.5.4 public release page |
