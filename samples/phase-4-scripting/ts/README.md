# Phase 4 demo, the TypeScript half

This folder is an Aero project, not a CMake target: it is deliberately absent from `samples/CMakeLists.txt`. It holds
two cubes that run one script, `assets/scripts/mover.ts`, and it is the project the Story 1 and Story 2 demos of
`docs/tasks/phase-4.md` start from. The small game of Story 3 grows from it.

```
ts/
├── project.json              the manifest (language "ts")
├── tsconfig.json             what the editor writes for a TypeScript project; kept so the bytes stay put
├── assets/scripts/mover.ts   slides its entity along x with a sine; hold Space to freeze it
├── assets/scripts/mover.ts.meta   the script's GUID, which the scene's ScriptComponents name
└── scenes/main.scene.json    a camera, a sun and two cubes, each with the script attached
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
2. While the cubes move, change `speed` in `mover.ts` and save. The cubes follow the new speed from where they are,
   with no jump, because `time` and `originX` keep their values across the reload.
3. Add `throw new Error("boom");` to `onUpdate` and save. The Console names `assets/scripts/mover.ts` and its line.
   Remove it and save to bring the cubes back.
4. End the editor mid-play (kill the process) and reopen this project: it offers to recover the work.

## What the tests pin

`tests/editor/play_scripts_test.cpp` (SX9, SX10) opens this project in place, checks that its scene is byte for byte
what the editor writes, and runs it for thirty frames from a copy.
