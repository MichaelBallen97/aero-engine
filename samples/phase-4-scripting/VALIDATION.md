# Phase 4 gate ledger — `samples/phase-4-scripting`

The Phase 4 gate (`docs/tasks/phase-4.md`) reads: *"Press Play in the editor: hot-reloaded TS or native C++ drives
entities; press Stop: the scene restores exactly."* Its reading of "or native C++": Play runs TypeScript only, and the
native half is the C++ mini-game built against the installed SDK. The artifact is this folder's two-language demo:
`ts/` (the two-cube scene with `mover.ts`, and the small game, `scenes/game.scene.json` with `assets/scripts/game.ts`)
and `cpp/` (the C++ mini-game). `README.md`'s "The gate as a scenario" states the gate as three lines.

## What CI proves automatically

- `tests/editor/play_scripts_test.cpp`: SX9–SX11 open `ts/` in place, check its scene is the editor's own text, run it
  thirty frames from a copy and save a new `SPEED` into the copy mid-play; SX12 checks `game.scene.json` and the bullet
  prefab against the C++ game's; SX13 drives the interactive game through the game's input; SX14 runs the scripted
  mode from a copy and reads `mini_game: frames=600 fired=8 hits=3 bullets=0 entities=4`.
- `tests/sdk/mini_game.cmake`'s SDK19: the C++ mini-game builds against an installed prefix and its `--scripted` run
  prints the same five integers.
- `typescript.sample`: the demo's scripts type-check under the editor's `tsconfig.json`.

## What it cannot prove

1. That the editor, driven by keyboard and mouse, plays the small game and that Stop gives the scene back as the eye
   and the Inspector see it (lines A1, A2, D1, D2).
2. That a drop from the asset browser, VS Code's completion and the cooker's output work on a real project (A3–A5).
3. That the C++ mini-game, built with its documented steps, plays the same game beside the TypeScript one (B1, B2, D3).
4. Whether the unexplained short hold of 4.4.5's row 2 (`docs/08-risks.md`) reproduces from a fresh launch (C1–C3).

## How to validate one OS

A Release build of `main` run as a signed `.app`, on a scratch copy of `ts/` under the home directory; the C++
mini-game built per `cpp/README.md` against an SDK installed into a private prefix.

**A. Story 3's demo** (one row per clause of its "Demo:" sentence)

1. Open the copy, File ▸ Open Scene `scenes/game.scene.json`, Play, click the viewport, move with the arrows and fire
   with Space: **the player moves, bullets leave it along −Z, and its name reads `player: fired=<n> hits=<n>`**.
2. Press Stop: **the seven entities of the scene are back, the player named `player` at 0, 0.5, 5**, matching the
   pre-Play captures.
3. In Edit, drag `bullet.aeroprefab` from the asset browser onto the viewport: **a `bullet` appears at scale 0.2**.
4. Open the copy in VS Code (a private profile) and type in `game.ts`: **`engine.` completes the API**.
5. Run `aero_cooker script` on each published bundle (`mover.ts`'s and `game.ts`'s): **exit 0, each file begins
   `00 41 45 52 4f 53 43 52` (`.AEROSCR`)**.

**B. Story 4's lines that waited on the small game**

1. Set `SCRIPTED = true` in the copy's `game.ts` and Play: **the player's name reads the five integers the C++
   `--scripted` run prints**.
2. Build the C++ mini-game with `cpp/README.md`'s steps and play it beside the editor's game: **the same arena, player
   and three targets**.

**C. The short-hold reproduction attempt** (`docs/08-risks.md`, 4.4.5's row 2), three times from a fresh launch

1–3. Open the game scene, Play, click the viewport, hold Right 3 s, Pause: **x 8.000 and `player: fired=0 hits=0`**.

**D. The gate as a scenario** (`README.md`)

1. Play: **the TypeScript logic drives the entities, and a constant saved in `game.ts` reaches the running game without
   Stop**.
2. Stop: **the scene is exactly as before Play** — the viewport, Hierarchy and Inspector against pre-Play captures, and
   the scene saved after Stop byte-identical to the file.
3. The C++ mini-game, built with its documented steps against the installed SDK, **runs and plays the same game
   natively**, and `--scripted` prints **`frames=600 fired=8 hits=3 bullets=0 entities=4`**.

## Validation records

- [x] A1. the small game plays with the keyboard and fires bullets — PASS: the copy opened on `game.scene.json`
  (`7 entities, 15 components (0 skipped, 0 failed)`), the player at X 0.000, Y 0.500, Z 5.000. Play, a click on the
  viewport, Left held 1.5 s (to the −8 clamp), three Space taps 1.2 s apart, Right held 1.0 s, Pause: the player's
  name reads **`player: fired=3 hits=0`** and its Inspector **X −2.059**, Y 0.500, Z 5.000 (−8 + ≈ 1 s at 6 units/s).
  A capture series during the taps (14 captures, 20:41:19.08–21.27) shows a yellow sphere in five captures, its
  centroid climbing the image toward the targets, (822, 732) → (862, 647) → (895, 577) → (921, 522) → (945, 470)
  capture px (along −Z), gone in the next, then the next shot at (802, 773)
- [x] A2. Stop shows the scene as before Play — PASS: Stop, the player selected again: against the pre-Play capture
  the Hierarchy (**camera, sun, ground, player, target, target, target**) differs in **0 px**, the Inspector (player,
  **X 0.000, Y 0.500, Z 5.000**) in **0 px**, the viewport image in **0 px** outside the HUD's frame-time value; the
  rest is the status bar's fps figure and the dock tab bar's focus line (a different panel focused)
- [x] A3. the bullet asset drops into the scene from the asset browser — PASS: in Edit, the Assets tab in List view
  (the bottom dock raised by a splitter drag), `bullet.aeroprefab  778 B` dragged onto the viewport with the slow
  shape (45 steps, 32 ms apart): a **`bullet`** row appears selected (search badge 8, HUD `Entities 8`, Undo labelled
  `Instantiate Prefab`), Inspector Position 3.063 / 0.000 / 1.601 (the drop point), Scale **0.200, 0.200, 0.200**,
  Mesh Renderer Sphere, Color 1.000 / 0.850 / 0.200. Cmd+Z: seven rows, HUD `Entities 7`, the title without the
  unsaved marker, and the copy's `game.scene.json` `cmp`-identical to the tree's
- [x] A4. typing in a script in VS Code shows autocomplete — PASS: VS Code 1.113.0 on the copy with a private
  `--user-data-dir` and an empty extensions folder (built-in TypeScript only, workspace trust off), after the editor
  wrote `Library/types/aero-engine.d.ts` (20 569 B): 0 problems at open. `engine.` typed on line 30 of `game.ts`: the
  completion list opens on the classes (AnimationPlayer, AudioListener, AudioSource, Camera, DirectionalLight, Entity,
  Environment, MeshRenderer, PointLight, Quat, Script, ScriptComponent …, the first `class engine.AnimationPlayer`);
  `sp` narrows it to **`spawn` — `function engine.spawn(): engine.Entity (+2 overloads)`**, SpotLight, Script,
  ScriptComponent. Escape and three Cmd+Z: no unsaved marker, 0 problems, nothing saved (the copy's `game.ts`
  `cmp`-identical to the tree's)
- [x] A5. the cooker emits each script's bytecode — PASS: the editor's publish at open (`scripts: 2 transpiled, 0
  failed, 0 up to date`), `Library/Scripts/0364eb133a9bb87c48e6ba1b38862722.js` (`mover.ts`, 677 B) and
  `0c9230e5304dffa96484c38101d407e5.js` (`game.ts`, 4 994 B), each GUID its `.ts.meta`'s; `build/macos-release/tools/
  cooker/aero_cooker script --input <bundle> --output <val folder>/cook/<guid>.aeroscript` **exit 0** for both, the
  files **734 B** and **3 815 B**, and `xxd -l 8` of each prints **`00000000: 0041 4552 4f53 4352  .AEROSCR`**
- [x] B1. the C++ `--scripted` line matches the TypeScript scripted run — PASS: `const SCRIPTED: boolean = true;`
  written in the copy in Edit (`scripts: 1 transpiled, 0 failed` 20:46:26.285), the Hierarchy widened by a splitter
  drag, Play at 20:46:35, captures at 15 s and 18 s: the player's row reads **`mini_game: frames=600 fired=8 hits=3
  bullets=0 entities=4`**, the Hierarchy holds **4 rows** (camera, sun, ground, the player; badge 4), the player at
  X 4.000, Y 0.500, Z 5.000, and the viewport image differs between the two captures in **0 px** (the game stands
  still). The C++ mini-game (B2's build): `aero_mini_game --scripted` **exit 0**, stdout **`mini_game: frames=600
  fired=8 hits=3 bullets=0 entities=4 drawn=600`** — the same five integers. Stop: seven rows, the player named
  `player` at 0, 0.5, 5; `SCRIPTED` set back to `false` (the copy's `game.ts` `cmp`-identical to the tree's,
  transpiled 20:47:20.093)
- [x] B2. the C++ mini-game builds with its documented steps and plays beside the TypeScript version — PASS: the
  steps of `cpp/README.md` on `main`: `cmake --preset macos-sdk` (exit 0), the `macos-sdk` build (exit 0, guard peak
  538 MB), `cmake --install build/macos-sdk --prefix <val folder>/sdk-prefix` (exit 0), then `cmake -S
  samples/phase-4-scripting/cpp -B <val folder>/mini-game -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<prefix>`
  (exit 0, `AeroEngine_DIR` the prefix's `lib/cmake/AeroEngine`, the only include `-isystem <prefix>/include`, every
  `aero` library linked from `<prefix>/lib`) and its build (exit 0); `shaders/` and `assets/` beside the executable.
  Run interactively beside the editor's game (the executable and those two folders wrapped in a signed throwaway
  bundle, so that keys reach it): the editor in Play, a Space tap from X 0 struck the middle target (**`player:
  fired=1 hits=1`**, six rows); in the C++ window (`Aero mini-game (C++)`, 640 × 360) a Space tap from the start
  position sent a bullet seen at the middle target, and the middle target was gone in the next capture (946 px
  changed in its place). The same arena: outer-target spacing ÷ the ground's near-edge width **267 / 479 = 0.557** in
  the C++ window and **310 / 559 = 0.555** in the editor's viewport. Escape quit the C++ game (no process left).
  Observation outside the assertion: the C++ window draws the scene much darker than the editor's Play view
  (`docs/08-risks.md`, 4.4.5's row 7)
- [x] C1. fresh launch 1: a held Right reaches x 8.000 with nothing fired — PASS: the editor quit (Cmd+Q, no
  `aero_editor` left), the bundle re-signed with a fresh id (`dev.aero.p4gate.run2`, so the launch's permission
  prompts come up as they did in 4.4.5's launch), launched 20:50:08.234; the watcher answered the Desktop-folder
  prompt naming the bundle at 20:50:10, 12 s before Play; the copy opened on `game.scene.json` (`7 entities, 15
  components`) at 20:50:09.750. Play 20:50:22.245, viewport click 20:50:24.687, Right held 3 s by one driver process
  posting both edges (20:50:27.768–30.942), the editor's PID frontmost before Play, before and after the hold, Pause
  20:50:31.552: **X 8.000**, Y 0.500, Z 5.000, the name **`player: fired=0 hits=0`**, seven rows. A 22-capture series
  over the hold: the player's blue centroid moves from capture x 1228 to the clamp at 1706 between 20:50:27.93 and
  20:50:29.30 (≈ 1.3 s for 8 units, 6 units/s) and **no yellow pixel in any capture**. Stop: the Hierarchy, the
  Inspector and the viewport image (outside the HUD values) 0 px from the pre-Play capture
- [x] C2. fresh launch 2: a held Right reaches x 8.000 with nothing fired — PASS: `dev.aero.p4gate.run3`, launched
  20:53:10.287, the Desktop-folder prompt answered 20:53:12; Play 20:53:24.285, viewport click 20:53:26.721, Right held
  20:53:29.752–32.944, the editor frontmost throughout, Pause 20:53:33.527: **X 8.000**, **`player: fired=0 hits=0`**,
  seven rows; the series shows the player moving 20:53:29.97–31.42 to the clamp and no yellow pixel; Stop 0 px
- [x] C3. fresh launch 3: a held Right reaches x 8.000 with nothing fired — PASS: `dev.aero.p4gate.run4`, launched
  20:56:01.896, the Desktop-folder prompt answered 20:56:04; Play 20:56:15.884, viewport click 20:56:18.392, Right held
  20:56:21.469–24.648, the editor frontmost throughout, Pause 20:56:25.252: **X 8.000**, **`player: fired=0 hits=0`**,
  seven rows; the series shows the player moving 20:56:21.63–23.02 to the clamp and no yellow pixel. No permission
  prompt or focus change came within 10 s of any of the three holds; the short hold of 4.4.5's row 2 did not
  reproduce
- [x] D1. Play: the TypeScript logic drives the entities and picks up a saved edit — PASS: in C3's launch, after its
  Stop, Play at 20:59:09.706 and a viewport click; a Space tap from X 0 struck the middle target, Left held 4 s (to
  the −8 clamp) and Right held 1.000 s, Pause: **`player: fired=1 hits=1`**, six rows, **X −2.019** (6 units/s).
  Resumed (the status bar `Playing · frame 1888`), then `const PLAYER_SPEED = 6;` rewritten to `2` in the copy's
  `game.ts` at 20:59:42.659: `assets: watcher detected 1 change(s) (first: 'scripts/game.ts')` 20:59:45.329,
  `scripts: 1 transpiled, 0 failed, 1 up to date` 20:59:45.361, no Stop. The same holds again (Left 4 s, Right 1.000
  s), Pause: **X −6.000** (2 units/s: the saved constant reached the running game) and the score **carried on,
  `player: fired=1 hits=1`**, six rows. `6` restored in the copy (`cmp`-identical to the tree's, transpiled
  21:00:07.702)
- [x] D2. Stop: the scene restores exactly — PASS: Stop at 21:00:10, the player selected again: against the pre-Play
  capture of the same launch the Hierarchy (**camera, sun, ground, player, target, target, target**) differs in
  **0 px**, the Inspector (player, **X 0.000, Y 0.500, Z 5.000**) in **0 px**, the title, menu and toolbar in **0 px**,
  and the viewport image in **0 px** outside the HUD's frame-time value; the rest is the status bar's fps figure and
  the Console's new lines. Then Cmd+S: the copy's `game.scene.json` was rewritten (mtime 20:38:01 → 21:00:31,
  5 147 B) and is **`cmp`-identical to the tree's** (SHA-256 `25e3d8fe…98b8c8` both): the same entities and component
  values, nothing left behind
- [x] D3. the C++ mini-game plays the same game natively — PASS: B2's build (`cpp/README.md`'s steps against the
  SDK installed into the private prefix). `aero_mini_game --scripted` again at 21:00:45: **exit 0**, **`mini_game:
  frames=600 fired=8 hits=3 bullets=0 entities=4 drawn=600`**. Interactive (the signed throwaway bundle, the C++
  window frontmost before each key): target pixels left / middle / right **1515 / 1514 / 1515** at the start; a Space
  tap from X 0 → **1515 / 0 / 1515** (the middle target destroyed); Right held 0.667 s moved the player's centroid
  from capture x 640 to 845 (≈ 4 units); a Space tap → **1515 / 0 / 0** (the right target destroyed); Left held 3 s
  (18 units' worth) left the centroid at capture x **229**, i.e. 411 px = **≈ 8.0 units** left of the start at
  51.25 px per unit: the −8 clamp, past the ground's edge, as the editor's game stands at +8 (C1–C3). Escape quit
  the game (no process left)

## Validation table

| OS | Status | Date | Machine / GPU | Story 3 demo (A) | Story 4 lines (B) | Short-hold attempts (C) | Gate lines (D) | notes |
|----|--------|------|---------------|------------------|-------------------|-------------------------|----------------|-------|
| macOS | ✅ validated | 2026-11-15 | MacBook Pro (Apple M1 Pro), Metal backend | PASS 5 / 5 | PASS 2 / 2 | PASS 3 / 3, not reproduced | PASS 3 / 3 | `main` at `4cd7bfe`; see below |
| Windows | ⏳ not run | — | — | — | — | — | — | needs this OS's editor, SDK build and cooker |
| Linux | ⏳ not run | — | — | — | — | — | — | needs this OS's editor, SDK build and cooker |

### macOS

✅ **13 / 13 executable rows PASS** (2026-11-15); the gate scenario **3 / 3 executable lines PASS**; no row NOT
EXECUTABLE.

1. **A — Story 3's demo, 5 / 5.** The small game plays with the arrow keys and Space (the player at X −2.059 after
   the clamp and a 1 s hold, `player: fired=3 hits=0`, each bullet seen climbing toward the targets); Stop brings
   back the seven rows, the player at 0, 0.5, 5, 0 px from the pre-Play capture; the prefab drops as a `bullet` at
   scale 0.2; VS Code completes `engine.` and `engine.sp` → `spawn`; the cooker turns both published bundles into
   `.aeroscript` files that begin `.AEROSCR`.
2. **B — Story 4's waiting lines, 2 / 2.** The TypeScript scripted run's name and the C++ `--scripted` line carry the
   same five integers, `frames=600 fired=8 hits=3 bullets=0 entities=4`; the C++ mini-game, built with
   `cpp/README.md`'s steps against the installed prefix, plays beside the editor's game with the same arena
   proportions (0.557 against 0.555) and the same middle-target hit.
3. **C — the short-hold reproduction, 3 / 3, not reproduced.** Three fresh launches, each with a fresh bundle id and
   its Desktop-folder prompt answered about 12 s before Play: each 3 s Right hold reached X 8.000 with
   `player: fired=0 hits=0` and no bullet pixel in a 22-capture series; the editor stayed frontmost before, during
   and after every hold. The `docs/08-risks.md` entry stays open: nothing here explains the 4.4.5 event, and it did
   not recur.
4. **D — the gate, 3 / 3.** (1) The TypeScript logic moves the player, fires and destroys a target, and a
   `PLAYER_SPEED` of 2 saved into the running game cuts the reach of a 1 s hold to a third (6 units → 2: X −2.019,
   then −6.000) with no Stop and the score kept; (2) Stop gives back the Hierarchy, the Inspector and the viewport
   0 px from the pre-Play capture, and the scene saved after Stop is byte-identical to the tree's `game.scene.json`;
   (3) the C++ mini-game
   built against the installed SDK plays the same game interactively (the middle and right targets destroyed, the
   player clamped) and prints `mini_game: frames=600 fired=8 hits=3 bullets=0 entities=4 drawn=600`.

Build: `main` at `4cd7bfe` (`655f014` plus one docs-only commit). The editor and the cooker: `macos-release`
(`AERO_ENABLE_PROFILING=ON`; the build at HEAD exit 0, guard peak 284 MB), run from a signed throwaway bundle with
the build tree's esbuild 0.28.2 in `Contents/Resources/`, on a copy of `ts/` under the home directory (the tree's
`ts/` untouched; every file the pass edited in the copy was put back `cmp`-identical). The C++ mini-game: the
`macos-sdk` preset configured, built (exit 0) and installed into a private prefix, the mini-game configured and built
against that prefix alone (exit 0), outside the source tree; for keyboard input its executable, `shaders/` and
`assets/` ran from a second signed throwaway bundle. macOS 26.6.2; one display attached, the built-in Retina panel
(1512 × 982 points, 2×). Keys were synthetic CGEvents sent only after the target app's PID was confirmed frontmost;
a held key was one driver process posting both edges; a chord's modifier was held through flags-changed events.
Permission prompts: four, each naming the editor's bundle and answered by the watcher — the local-network prompt at
the first launch (20:40:22) and the Desktop-folder prompt at each of C's three launches (20:50:10, 20:53:12,
20:56:04); no prompt for another application was seen. Colours were measured after the sRGB conversion.

### Windows

⏳ pending.

### Linux

⏳ pending.
