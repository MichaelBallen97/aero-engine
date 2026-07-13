# 00 — Overview

> Open-source, cross-platform 3D graphics engine with an editor and TypeScript **or** C++ scripting.
> **MIT licensed. Solo development. Start: July 2026.**

---

## 🎯 Objective

Build a graphics engine from scratch, with a graphical editor, capable of creating 3D projects (and 2D later), with scripting support, asset management, and multi-platform export.

**Dual purpose:**

1. **Publish my own games** with it.
2. **Contribute an open-source resource** to the community, the way Godot did. This is not a competition — it is my own free software, built in the open and shared.

**Scope framing:** the goal is *core-workflow parity* with Unity and Godot — the loop of edit → script → play → export — **not** feature parity. Feature parity is hundreds of person-years; core-workflow parity is achievable solo. Everything in [06 — Scope & Non-Goals](./06-scope-and-non-goals.md) exists to protect that line.

---

## 📌 The three project rules

### 1. Golden architecture rule
**The editor depends on the engine. The engine NEVER depends on the editor.**

This is what makes it possible for the runtime to go to 5 platforms and the editor only to 3. If it breaks even once, the exporter turns into hell. It is the rule most hobby engines break. It is verified automatically in CI (see [03 — Architecture](./03-architecture.md)).

### 2. Deliverable rule
**Every phase ends in something playable or usable. A phase without a deliverable is not finished.**

The only real filter against building abstractions nobody uses.

### 3. Boundary rule
**No third-party library type crosses the engine's public API.**

Not `glm::vec3`, not a miniaudio type, not an SDL handle. Everything lives behind our own types. This is what makes the question "is it scalable?" have an answer.

---

## 🖥️ The two platform matrices

Independent. Do not conflate.

| | macOS | Windows | Linux | Android | iOS |
|---|:---:|:---:|:---:|:---:|:---:|
| **Editor** (where the engine runs) | ✅ | ✅ | ✅ | ❌ | ❌ |
| **Runtime** (where the exported game runs) | ✅ | ✅ | ✅ | ✅ | ✅ |

**Design consequence:** the editor never runs on mobile → no touch UI, no adaptive layouts, no mobile memory management in the editor.

**Practical consequence:** the Mac is the only machine that can compile and sign for macOS **and** iOS. Windows and Linux are covered by CI.

---

## ⚡ Stack in one table

| Layer | Choice |
|---|---|
| Core language | **C++20** (→ C++23 where all three compilers allow) |
| Platform | **SDL3** |
| RHI (render) | **SDL3 GPU API** (Vulkan / Metal / D3D12) |
| Shaders | HLSL → SDL_shadercross |
| ECS | **EnTT** |
| Reflection | **Code-gen with libclang** → `entt::meta` |
| Scripting | **TypeScript** on **quickjs-ng**, *or* native **C++** (chosen per project) |
| Physics 3D / 2D | **Jolt** / **Box2D v3** |
| Audio | Own abstraction → **miniaudio** backend |
| Canonical 3D format | **glTF 2.0** |
| Editor UI | **Dear ImGui** |
| Build / CI | CMake + vcpkg / GitHub Actions |

Full detail, alternatives, and licenses in [01 — Tech Stack](./01-tech-stack.md).

---

## ⏱️ Realistic horizon

**20–32 months part-time** to a genuinely usable engine. Planning with that number in view is what prevents abandonment.

The milestone that validates the entire project is **Phase 5**: ship a small, real game made with the engine. If we reach it, the engine exists.

---

## 📚 Documentation index

| # | Document | Contents |
|---|---|---|
| 00 | Overview (this file) | Objective, rules, platform matrices, stack, horizon |
| 01 | [Tech Stack](./01-tech-stack.md) | Choice per layer, justification, licenses |
| 02 | [ADRs](./02-adrs.md) | The 8 architecture decisions, with discarded alternatives |
| 03 | [Architecture](./03-architecture.md) | Layers, repo layout, asset flow, export models |
| 04 | [Conventions & Setup](./04-conventions-setup.md) | C++ style, git, CMake, CI, dev environment |
| 05 | [Roadmap & Phases](./05-roadmap.md) | The 9 phases, deliverable gates, durations |
| 06 | [Scope & Non-Goals](./06-scope-and-non-goals.md) | What v1.0 is — and what it is not |
| 07 | [Tasks (index)](./07-tasks.md) | Legend, conventions, links to the per-phase breakdowns |
| — | [docs/tasks/](./tasks/) | Full per-phase breakdown — epics → tasks → subtasks, all 9 phases |
| 08 | [Risks & Open Decisions](./08-risks.md) | Risk register and open decisions |
| — | [Future Roadmap](./future-roadmap.md) | v2 and v3–v4 deferred features |

---

## 🔓 Resolved & open decisions

**Resolved during planning (July 2026):**

- **Project name** → **Aero Engine**.
- **QuickJS vs quickjs-ng** → **quickjs-ng** (better maintained, CMake build, proper Windows support, sanitizer CI). See [ADR-007](./02-adrs.md#adr-007--scripting-typescript-on-quickjs-ng).
- **Scripting model** → **TypeScript OR C++, chosen per project, never combined.** See [ADR-008](./02-adrs.md#adr-008--per-project-language-choice-and-the-two-export-models).
- **July 2026 re-plan (plan audit)** → third tech-stack pass: **zero swaps**; epics **2.6** (project system), **3.6** (rendering essentials), **3.7** (audio playback v0) and **4.7** (play-in-editor) added; full per-phase breakdown now lives in [docs/tasks/](./tasks/).

**Still open (decide when the phase arrives):**

- **Render architecture: forward+ vs deferred** → Phase 8, with real profiling data.
- **Long-term editor UI: ImGui forever?** → not before Phase 8.
- **Migration to native C++26 `std::meta` reflection** → revisit when MSVC ships support (see [ADR-004](./02-adrs.md#adr-004--reflection-by-code-generation)).
