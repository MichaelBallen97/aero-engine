# Phase 4 demo, the TypeScript half

This folder is an Aero project, not a CMake target: it is deliberately absent from `samples/CMakeLists.txt`. It holds
two scenes. `scenes/main.scene.json` is two cubes that run one script, `assets/scripts/mover.ts`, and it is the scene
the Story 1 and Story 2 demos of `docs/tasks/phase-4.md` start from. `scenes/game.scene.json` with
`assets/scripts/game.ts` is Story 3's small game.

```
ts/
├── project.json              the manifest (language "ts")
├── tsconfig.json             what the editor writes for a TypeScript project; kept so the bytes stay put
├── assets/scripts/mover.ts   slides its entity along x with a sine; hold Space to freeze it
├── assets/scripts/mover.ts.meta   the script's GUID, which the scene's ScriptComponents name
├── assets/scripts/game.ts    the small game, one script on the player
├── assets/scripts/game.ts.meta    the script's GUID
├── assets/bullet.aeroprefab  a byte copy of ../cpp/assets/bullet.aeroprefab
├── assets/bullet.aeroprefab.meta  the prefab's GUID
├── scenes/main.scene.json    a camera, a sun and two cubes, each with the script attached
└── scenes/game.scene.json    the C++ mini-game's scene: a camera, a sun, a ground, the player and three targets
```

## Open it

Run `aero_editor`, choose File > Open Project and pick this folder. The editor creates `Library/` here (the
transpiled script, its source map, the declarations); it hides itself from git. If `main.scene.json` is not the open
scene, choose File > Open Scene and pick it.

## Story 1, play and stop (about two minutes)

1. Press Play (Cmd/Ctrl+P). Both cubes slide along x, each from its own start, each with its own state.
2. Press Pause, then Step: the cubes hold, and each Step moves them exactly one frame.
3. Hold Space while playing: the cubes freeze and move on from there when it is released.
4. With the viewport focused, press W: the transform tool stays put. After Stop it switches again.
5. Press Stop. The scene is exactly as it was: same entities, same values, the cubes back at -1.5 and 1.5.

## Story 2, edit while playing (about two minutes)

1. Select `Main Camera` and change its Transform position in the Inspector (orbiting the viewport records no edit),
   then press Play: the change is on disk before the first frame.
2. While the cubes move, change `SPEED` at the top of `mover.ts` and save. The cubes follow the new speed from where
   they are, with no jump: `SPEED` is module code, which a reload replaces, while `phase` and `originX` are the
   script's own fields, which a reload keeps. A script's own fields keep their values across a reload, so a field's
   new initialiser shows at the next Play.
3. Add `throw new Error("boom");` to `onUpdate` and save. The Console names `assets/scripts/mover.ts` and its line.
   Remove it and save to bring the cubes back.
4. End the editor mid-play (kill the process) and reopen this project: it offers to recover the work.

## Story 3, the small game (about two minutes)

1. Choose File > Open Scene and pick `scenes/game.scene.json`, press Play and click the viewport.
2. The keys:

   | Key | Effect |
   |---|---|
   | Left, Right, A, D | move along X, clamped to +-8 |
   | Up, Down, W, S | move along Z, clamped to +-6 |
   | Space | fire; held, it fires every 0.21 s |

   Stop ends the game: the C++ game's Escape has no counterpart here, because Stop is the editor's.
3. A bullet that reaches a target destroys both. The player's name reads `player: fired=<n> hits=<n>`.
4. Press Stop. The seven entities are back as saved, and the player is named `player` again.
5. In Edit, drag `bullet.aeroprefab` from the asset browser into the Viewport: a `bullet` appears, scaled 0.2.
6. Open this folder in VS Code once the editor has written `Library/types/`: `engine.` completes in `game.ts`, and a
   misspelt field or key name is flagged.
7. Cook the game's bundle: `aero_cooker script --input Library/Scripts/0c9230e5304dffa96484c38101d407e5.js --output
   game.aeroscript`. The file begins with a NUL and `AEROSCR`.

## The scripted run

Set `SCRIPTED` to `true` at the top of `game.ts`, save, and press Play. After 600 frames the player's name reads
`mini_game: frames=600 fired=8 hits=3 bullets=0 entities=4`, the line the C++ mini-game's `--scripted` prints before
its `drawn` field. The run steps 1/60 s a frame whatever the editor's frame time, and the game stands still after it.
One difference: a bullet's age is a JavaScript double, so a miss expires after 60 flights where the C++ float expires
after 61 (frame 469 against 470); no integer of the line changes. Set it back to `false` afterwards.

## What the tests pin

`tests/editor/play_scripts_test.cpp` (SX9, SX10, SX11) opens this project in place, checks that its scene is byte for
byte what the editor writes, runs it for thirty frames from a copy, and saves a new `SPEED` into that copy mid-play.
SX12 opens the game scene in place, checks that it is the editor's text with the C++ game's values and that the bullet
is the C++ game's file. SX13 drives the interactive game through the game's input: a held Right, Space and its
cooldown. SX14 runs the scripted mode from a copy and reads the summary line.
