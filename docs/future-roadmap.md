# Future Roadmap (post-v1.0)

> These features are **deliberately out of v1.0** ([06 — Scope & Non-Goals](./06-scope-and-non-goals.md)). They are recorded here so the intent is preserved and the v1 architecture leaves room for them — not scheduled. Priorities and feasibility get revisited after v1.0 ships.

---

## v2 — depth on the 3D foundation

| Feature | Why deferred | Architectural note |
|---|---|---|
| **Baked global illumination** | Large, self-contained rendering feature not needed for the edit→play→export loop | Builds on the Phase 8 render architecture; needs a lightmapper + UV pipeline |
| **Animation state-machine graph editor** | v1 ships animation *playback*; a visual state-machine editor is a separate, sizeable editor feature | The runtime state machine can exist earlier; the **graph editor** (imnodes — removed from the v1 stack table in the July 2026 audit, it enters here) is the v2 piece |
| **Terrain / vegetation systems** | A whole content subsystem (heightmaps, splatting, instanced foliage, LOD) | Sits on top of the existing renderer + asset pipeline; no engine-core changes required |
| **In-game UI framework** | v1 ships only the minimal msdf HUD-text path (5.5.3 → 7.3); a real UI system is a large subsystem | Builds on the 2D renderer (7.2) and text (7.3); components + layout live behind the same reflection pipeline |
| **Particle system** | A whole subsystem the Phase 5 game doesn't need | Sits on the renderer; cooked as assets like everything else |
| **Input action-mapping assets** | v1 exposes device state (4.4.3, touch in Phase 6); mapping layers are v2 polish | An asset type + indirection over the existing input API — no platform-layer changes |
| **Save-game framework** | v1 games hand-roll saves with the serialization primitives | Helpers over the generated serialization (ADR-004's consumer #2) |
| **TypeScript DAP debugger** | v1 ships source-mapped errors (4.3.4); a full debug adapter is sizeable | quickjs-ng exposes the hooks; source maps already exist from 4.3.1 |
| **Separate-process play mode** | v1 plays in-process (risk R11, mitigated by autosave 4.7.3) | The runtime player (5.2.1) is the natural host; editor ↔ player IPC is the new piece |
| **Crash reporter (minidumps)** | Solo-maintainable v1 relies on ASan/UBSan + logs | Runtime-side minidump write + a symbol pipeline in CI |
| **Prefab overrides & nesting** | v1 ships prefab-lite (4.4.4): instantiate a saved entity-tree | Override storage rides the existing serialization; editor diffing UI is the real cost |
| **Asset hot-reload everywhere** | v1 ships a watcher for common types (3.1.4, P2) | Extends the AssetDatabase invalidation to live GPU/scene state across all types |

---

## v3–v4 — advanced rendering & authoring

| Feature | Why deferred | Architectural note |
|---|---|---|
| **Ray tracing / mesh shaders / Nanite-style virtualized geometry** | **SDL_GPU cannot do these** — it has no RT and no mesh-shader support, by design | Requires dropping to **raw Vulkan/Metal/D3D12 behind the `rhi` wrapper**. This is precisely why ADR-002 keeps `engine/rhi` as a hard boundary — it makes this path possible without a rewrite |
| **Visual / node scripting** | A large authoring subsystem; v1's differentiator is first-class *TypeScript*, not nodes | Compiles down to the same reflection-fed binding layer the TS and C++ paths already use (imnodes for the graph UI) |
| **Console targets (Switch / Xbox / PlayStation)** | NDA territory with per-platform legal + toolchain overhead; meaningless before a shipped game catalog exists | Verified July 2026: SDL3 ships commercial games on **Switch/Switch 2** via an NDA zlib-licensed port; **Xbox One/Series** works via GDK (D3D12 backend); **PS5** has no public path → a custom backend behind `engine/rhi` — the same escape hatch as ray tracing. The boundary rule means the engine above `rhi`/`platform` should not care |

---

## Community-plugin territory (any version, if at all)

Kept out of the core intentionally; the architecture leaves the door open for third parties:

- **FMOD / Wwise** integration — excluded from core by license; viable as a user-integrated plugin (the Godot model), thanks to the `audio/graph` abstraction (ADR-006)
- **Additional scripting languages** (Luau, C#, Rust) — the bindings layer is designed generically (reflection-fed), so a language becomes a plugin, not a core rewrite (ADR-007)
- **Networking / multiplayer** — no v1 plans; would be a subsystem or plugin
- **Web / WASM export** — would argue for a wgpu/Dawn RHI backend; only worth it if web becomes a real target

---

## Scalability ledger

Every deliberate exit door in the v1 architecture, in one place — the answer to "can it scale later?" (third stack pass, July 2026). Each swap is possible *because* of the boundary rule: no third-party type crosses the engine's public API.

| Scaling pressure | Designed exit | Where the door lives |
|---|---|---|
| Ray tracing / mesh shaders / Nanite-style geometry | Raw Vulkan/Metal/D3D12 backends replacing SDL_GPU | `engine/rhi` wrapper (ADR-002 — "treat it as sacred") |
| Math throughput (SIMD) | GLM → **RTM** backend swap | `core/math` public types (ADR-005) |
| Reflection build step | libclang `reflect-gen` → native C++26 `std::meta` (~2027–28, when MSVC ships) | The generator's output boundary (ADR-004) |
| Audio depth (HRTF, occlusion, zone reverb) | Steam Audio / Resonance as an optional spatial layer | `audio/graph` public API (ADR-006) |
| Web / WASM export | wgpu/Dawn RHI backend | Same `engine/rhi` boundary |
| More scripting languages (Luau, C#, Rust) | Additional binding emitters as plugins | The reflection-fed generic bindings layer (ADR-007) |
| Consoles | NDA platform ports (see v3–v4 table) | `engine/rhi` + `engine/platform` |
| Editor UI ceiling | Re-evaluated at Phase 8 with evidence — not before | ImGui is confined to `/editor` by the golden rule |

---

## Guiding principle

Everything here must still pass the scope-discipline test in [06](./06-scope-and-non-goals.md) when its version comes up. Being on this list is a record of intent, **not** a commitment to build.
