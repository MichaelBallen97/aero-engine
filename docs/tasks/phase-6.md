# Phase 6 — Mobile Runtime & Audio Depth · est. 2–3 mo

> **Goal:** complete the 5-platform runtime matrix (iOS + Android join the desktop three) and deepen audio from "it plays" (3.7) to buses/mixers/effects. Touch input arrives here through the engine input API designed in 0.3.2/4.4.3.
>
> **Gate:** The Phase 5 game running on a phone
>
> **Phase non-goals:** no mobile *editor* — ever (project rule); no store submission/certification (publishing pipelines beyond a runnable device build are the developer's per-game concern); no HRTF/occlusion (Steam Audio layer stays a Phase 5+ open decision, ADR-006).
>
> **Gate artifact:** device-tested builds of the Phase 5 game; capture video committed under `/samples/phase-6-mobile/`.

---

## Epic 6.1 — iOS runtime · runtime · platform

**Goal:** the runtime on iOS: Metal via SDL_GPU, no JIT needed by design (quickjs-ng is an interpreter — ADR-007's iOS bet pays off here).
**Definition of Done:** an iOS device runs an engine `.pak` full-loop with touch input.

### 6.1.1 iOS build (Metal) · P0 · L · depends: 5.2.1
**Goal:** stand up the iOS toolchain — the Mac-only lane (docs/00 practical consequence).
**Deliverable:** iOS runtime target building via CMake/Xcode with SDL3's iOS backend; signing steps documented.
Subtasks:
- CMake toolchain / Xcode project generation for the runtime
- SDL3 iOS backend + Metal via SDL_GPU; app lifecycle basics
- Code-signing walkthrough documented (Mac required)

### 6.1.2 Touch input · P0 · M · depends: 6.1.1, 4.4.3
**Goal:** *(audit D7)* a mobile runtime without touch is decorative — touch joins the engine input API and the script surface (shared design with 6.2.2).
**Deliverable:** touch events (down/move/up, multi-touch) exposed through `engine::input` and to scripts.
Subtasks:
- Touch events in the platform layer → engine input API
- Script exposure; simple virtual-stick helper for the Phase 5 game

---

## Epic 6.2 — Android runtime · runtime · platform

**Goal:** the runtime on Android: Vulkan via SDL_GPU, NDK build, AAudio through miniaudio.
**Definition of Done:** an Android device runs the same `.pak` full-loop with touch and correct lifecycle behavior.

### 6.2.1 Android build (Vulkan/NDK) · P0 · L · depends: 5.2.1
**Goal:** the NDK lane, kept as thin as possible.
**Deliverable:** runtime `.apk` via CMake toolchain + minimal Gradle shell; AAudio path through miniaudio verified.
Subtasks:
- NDK CMake toolchain + minimal Gradle wrapper producing an APK
- SDL3 Android backend + Vulkan via SDL_GPU; AAudio through miniaudio

### 6.2.2 Touch + app lifecycle · P0 · M · depends: 6.2.1, 6.1.2
**Goal:** Android's realities: touch, suspend/resume, back button.
**Deliverable:** touch through the shared input API; app pauses/resumes without losing GPU resources; back button handled.
Subtasks:
- Touch via the shared engine input path
- Suspend/resume (GPU resource survival); back-button handling

---

## Epic 6.3 — Mobile texture path · tools

**Goal:** the cooker learns mobile GPU formats (the KTX2/Basis investment pays out).
**Definition of Done:** per-platform cooker profiles emit ASTC/ETC2 for mobile targets, BCn for desktop, from the same sources.

### 6.3.1 ASTC/ETC2 cooker profiles · P0 · M · depends: 3.3.2, 6.1.1
**Goal:** right format per target, one source of truth.
**Deliverable:** platform profiles in the cooker (desktop = BCn; mobile = ASTC with ETC2 fallback), validated on devices.
Subtasks:
- Profile plumbing in the cooker; ASTC/ETC2 encode paths
- On-device validation of memory/quality

---

## Epic 6.4 — Audio graph depth · audio

**Goal:** ADR-006's own graph grows real: buses, mixers, basic effects over the 3.7 foundation.
**Definition of Done:** game audio routes through named buses with per-bus control and basic effects.

### 6.4.1 Buses & mixers · P0 · M · depends: 3.7.2
**Goal:** the master/music/sfx routing every game needs.
**Deliverable:** named buses with per-bus volume/mute; sources route to buses.
Subtasks:
- Bus graph (master/music/sfx defaults); per-bus volume/mute
- `AudioSource` bus assignment (reflected field)

### 6.4.2 Basic effects · P1 · M · depends: 6.4.1
**Goal:** the minimum effect set with the graph-node shape future effects will reuse.
**Deliverable:** gain, pan, low-pass, and reverb-lite as bus effects.
Subtasks:
- Effect node interface; gain/pan/low-pass/reverb-lite

---

## Epic 6.5 — Re-export & mobile CI · build-ci

**Goal:** *(renamed by the July 2026 audit, D10)* the Phase 5 game reaches devices, and CI covers all 5 runtime platforms from here on.
**Definition of Done:** the phase gate + mobile runtime artifacts produced by CI on every tagged build.

### 6.5.1 Mobile CI lanes · P0 · M · depends: 6.1.1, 6.2.1
**Goal:** *(audit D10)* runtime coverage in CI grows from 3 platforms to 5.
**Deliverable:** Android cross-compile lane (Linux runner) and iOS build lane (macOS runner, unsigned) archiving runtime artifacts.
Subtasks:
- Android NDK lane on the Ubuntu runner
- iOS (unsigned) lane on the macOS runner; artifact upload for both

### 6.5.2 The Phase 5 game on device · P0 · M · depends: 6.5.1, 6.1.2, 6.2.2, 6.3.1
**Goal:** validate the gate on real hardware.
**Deliverable:** the game exported, installed, and played on a physical iPhone and Android phone with the touch-control variant.
Subtasks:
- Export with mobile profiles; install on both device families
- Touch-controls variant validated; capture video for `/samples/phase-6-mobile/`

---

**Phase gate:** The Phase 5 game running on a phone.
