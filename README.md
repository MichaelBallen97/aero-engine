# 🎮 Aero Engine

[![CI](https://github.com/MichaelBallen97/aero-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/MichaelBallen97/aero-engine/actions/workflows/ci.yml)

> An open-source, cross-platform 3D graphics engine with an editor and TypeScript **or** C++ scripting.
> **MIT licensed. Solo development. Start: July 2026.**

Aero Engine is a from-scratch game engine built for two purposes:

1. **To ship my own games with it.**
2. **To give an open-source resource back to the community** — the way Godot did. Not a competitor; just free software, built in the open and shared.

The engine targets **core-workflow parity** with Unity and Godot (edit → script → play → export), deliberately **not** feature parity. It is 3D-first, with 2D support arriving once the 3D foundation is solid.

---

## The three project rules

1. **Golden architecture rule** — *The editor depends on the engine. The engine NEVER depends on the editor.* This is what lets the runtime ship to 5 platforms while the editor ships to 3. Break it once and the exporter becomes a nightmare.
2. **Deliverable rule** — *Every phase ends in something playable or usable. A phase without a deliverable is not finished.* The only real filter against building abstractions nobody uses.
3. **Boundary rule** — *No third-party library type crosses the engine's public API.* Not `glm::vec3`, not a miniaudio handle, not an SDL type. Everything lives behind our own types.

---

## Platform matrices (two, independent — do not conflate)

| | macOS | Windows | Linux | Android | iOS |
|---|:---:|:---:|:---:|:---:|:---:|
| **Editor** (where the engine runs) | ✅ | ✅ | ✅ | ❌ | ❌ |
| **Runtime** (where the exported game runs) | ✅ | ✅ | ✅ | ✅ | ✅ |

The editor never runs on mobile → no touch UI, no adaptive layouts, no mobile memory management in the editor. The Mac is the only machine that can compile and sign for macOS **and** iOS; Windows and Linux are covered by CI.

---

## Building from source

Prerequisites: git, CMake ≥ 3.28, Ninja. On Windows, configure from a *Visual Studio Developer Command Prompt* (Ninja needs the MSVC environment).

```bash
git clone --recurse-submodules https://github.com/MichaelBallen97/aero-engine.git
cd aero-engine
cmake --preset macos-debug        # or windows-debug / linux-debug on those hosts
cmake --build --preset macos-debug
```

Already cloned without `--recurse-submodules`? Run `git submodule update --init`.

The first configure bootstraps [vcpkg](https://github.com/microsoft/vcpkg) (a pinned git submodule) and compiles the dependencies from source — expect a few minutes and network access. There are no build targets yet (Phase 0 in progress); a successful configure is the current smoke test.

On Linux, SDL3 needs system dev packages first (plus autotools, which vcpkg uses to build some transitive deps from source):
`sudo apt install libx11-dev libxft-dev libxext-dev libwayland-dev libxkbcommon-dev libegl1-mesa-dev libibus-1.0-dev autoconf autoconf-archive automake libtool`

---

## Documentation

| # | Document | Contents |
|---|---|---|
| 00 | [Overview](./docs/00-overview.md) | Objective, rules, platform matrices, stack table, horizon |
| 01 | [Tech Stack](./docs/01-tech-stack.md) | Choice per layer, justification, licenses, accepted limits |
| 02 | [ADRs](./docs/02-adrs.md) | The architecture decisions, with discarded alternatives |
| 03 | [Architecture](./docs/03-architecture.md) | Layers, repo layout, asset flow, export models |
| 04 | [Conventions & Setup](./docs/04-conventions-setup.md) | C++ style, git, CMake/vcpkg, CI, dev environment |
| 05 | [Roadmap & Phases](./docs/05-roadmap.md) | The 9 phases, deliverable gates, durations |
| 06 | [Scope & Non-Goals](./docs/06-scope-and-non-goals.md) | What v1.0 is — and, crucially, what it is not |
| 07 | [Tasks (index)](./docs/07-tasks.md) | Legend, numbering conventions, links to the per-phase breakdowns |
| — | [docs/tasks/](./docs/tasks/) | Full per-phase breakdown — epics → tasks → subtasks with goals & deliverables, all 9 phases |
| 08 | [Risks & Open Decisions](./docs/08-risks.md) | Risk register and decisions still open |
| — | [Future Roadmap](./docs/future-roadmap.md) | v2 and v3–v4 deferred features |

---

## Status

Planning complete. Phase 0 (Foundations & First Triangle) not yet started.

Live task tracking lives in Notion — [**Aero Engine — Build Tracker**](https://app.notion.com/p/39b120678cf1810dbd89cd87ca594ed2) (Phases → Epics → Tasks/subtasks). These docs are the source of truth for scope and architecture; Notion tracks execution.
