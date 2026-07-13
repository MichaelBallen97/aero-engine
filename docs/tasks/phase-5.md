# Phase 5 — Export & Ship a Game 🎯 · est. 2–3 mo

> **Goal:** the project's validation milestone. Both export models (ADR-008) become real, and ONE small, real 3D game ships to desktop. **If we reach this gate, the engine exists.**
>
> **Gate:** A downloadable, playable 3D game made entirely in Aero Engine (desktop)
>
> **Phase non-goals:** no mobile export (Phase 6); no store-quality polish — "small, real, finished" beats "ambitious, abandoned". The game's design deliberately avoids text-heavy UI (v1 has no UI framework; 5.5.3 provides minimal HUD text only).
>
> **Gate artifact:** the game's project lives under `/samples/phase-5-game/` (source), with public builds published (5.5.4).

---

## Epic 5.1 — Packager · tools

**Goal:** everything a game needs, in one mountable archive: the `.pak`.
**Definition of Done:** a full project cooks and packs into a single `.pak` the runtime mounts through the VFS.

### 5.1.1 `.pak` format + writer + VFS mount · P0 · M · depends: 3.3.1, 3.3.2, 4.3.2
**Goal:** the archive format (docs/03 asset flow's last hop).
**Deliverable:** versioned `.pak` with TOC/offsets/alignment; VFS mounts it transparently.
Subtasks:
- `.pak` writer (TOC, offsets, alignment, version field)
- VFS mount + read path in the runtime

### 5.1.2 Cook-all pipeline · P0 · M · depends: 5.1.1
**Goal:** one command cooks a whole project per platform.
**Deliverable:** project scan → cook every asset (meshes, textures, scenes, scripts→bytecode, shaders) → one `.pak`.
Subtasks:
- Full-project cook orchestration; per-platform output dirs
- Incremental re-cook via the 3.1.2 cache

---

## Epic 5.2 — Per-platform desktop runtimes · runtime

**Goal:** the precompiled runtime players (the Godot model's engine half) plus the purity guard that keeps them lean forever.
**Definition of Done:** CI produces mac/win/linux runtime binaries that boot a `.pak`; the runtime-purity guard is live.

### 5.2.1 Runtime player binary · P0 · L · depends: 5.1.1, 4.7.4, 3.7.2
**Goal:** *(audit D9)* the standalone player is a real component, not a byproduct: entry points, boot, loop, quit.
**Deliverable:** `aero_runtime` per desktop OS — mounts `game.pak`, boots the startup scene, runs the full loop (scene, physics, audio, script), quits cleanly.
Subtasks:
- Entry points per desktop OS; config (startup scene, window)
- Mount pak → boot scene → run loop → clean shutdown

### 5.2.2 Runtime-purity CI guard · P0 · S · depends: 5.2.1
**Goal:** *(audit D9)* the docs/04 guard gets an owner: the runtime must never link editor-side libraries.
**Deliverable:** CI link-scan proving no ImGui/Assimp/libclang in the runtime binary; seeded-violation proven, then cleaned.
Subtasks:
- Link/symbol scan of the runtime binary; wire into CI; prove + clean seed

### 5.2.3 CI runtime artifacts · P0 · M · depends: 5.2.1
**Goal:** instant TS export needs prebuilt runtimes on tap (docs/04: CI archives them).
**Deliverable:** Release runtime builds for macOS/Windows/Linux, archived per tagged commit.
Subtasks:
- Release lanes + artifact upload for the 3 desktop OSes

---

## Epic 5.3 — TS export (the Godot model) · editor

**Goal:** ADR-008's headline: export without toolchains, instantly.
**Definition of Done:** one dialog exports a runnable game folder/zip for any desktop OS from any desktop OS.

### 5.3.1 Export dialog + instant pack · P0 · M · depends: 5.1.2, 5.2.3
**Goal:** cooked assets + prebuilt runtime, staged and zipped.
**Deliverable:** platform-picker dialog → `game.pak` + matching precompiled runtime + icon/name staged into an output folder/zip.
Subtasks:
- Export dialog (platform, output path, app name/icon)
- Stage pak + runtime; smoke-launch the export

---

## Epic 5.4 — C++ export (the Unreal model) · editor · tools

**Goal:** the native pipeline: compile-and-link per platform, locally for your OS, CI recipe for the rest.
**Definition of Done:** a C++ project exports a native desktop build locally with documented steps for the other two OSes.

### 5.4.1 Native build + package pipeline · P0 · L · depends: 4.5.2, 5.1.2
**Goal:** drive the user project's CMake build and package the result next to its cooked `.pak`.
**Deliverable:** local-OS native export end-to-end; documented CI recipe for cross-platform builds.
Subtasks:
- Editor-driven CMake configure/build of the user project
- Package binary + pak; document the CI path for other OSes

---

## Epic 5.5 — The game 🎯 · samples

**Goal:** design, build, and ship ONE small real 3D game — the dogfood that proves the loop and finds the engine's real bugs.
**Definition of Done:** the phase gate: a stranger can download and play it.

### 5.5.1 Game design one-pager · P0 · S
**Goal:** constrain scope before building: one mechanic, 5–15 minutes of play, minimal text.
**Deliverable:** a one-page design the rest of the epic executes against.
Subtasks:
- One-pager: mechanic, loop, win/lose, asset list, HUD needs

### 5.5.2 Build the game · P0 · L · depends: 5.5.1, 4.7.4
**Goal:** make it entirely in-engine; every friction point becomes an engine issue.
**Deliverable:** the finished game project under `/samples/phase-5-game/`, playable in-editor.
Subtasks:
- Build the game in the editor (TS or C++ — pick in the one-pager)
- Log every engine friction/bug found; fix the blockers

### 5.5.3 Minimal HUD text · P1 · M · depends: 5.5.2
**Goal:** *(audit D8)* even "Press Space" needs text, the runtime can't link ImGui, and the real text system is Phase 7 — so: msdf atlas baked in-editor, textured quads at runtime. Formalized later by 7.3.
**Deliverable:** score/prompt text rendering in the shipped game via msdf atlas quads.
Subtasks:
- Editor-side msdf atlas bake (msdf-atlas-gen)
- Runtime quad-text path (engine-internal, minimal API)

### 5.5.4 Ship it · P0 · S · depends: 5.5.2, 5.3.1
**Goal:** *(audit D12)* "downloadable" means a public page, not a zip in a drawer.
**Deliverable:** itch.io or GitHub Releases page with builds for the 3 desktop OSes and a README.
Subtasks:
- Publish page; upload 3-OS builds; write the README/controls

---

**Phase gate:** A downloadable, playable 3D game made entirely in Aero Engine (desktop).
