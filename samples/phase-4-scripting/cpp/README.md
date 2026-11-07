# The C++ mini-game

A player cube on an arena, three target cubes at the far end, and a bullet that destroys the target it reaches. The
TypeScript half of the Phase 4 demo is written against the rules below, so both play the same game.

It is written against the installed SDK alone: its sources include `<aero/...>` headers and C++ standard headers only,
and it reads its shaders and its one asset from beside the executable.

## Build and run against a prefix

Configure the engine with an SDK preset, build and install it, then build this folder against the prefix. No
toolchain file, no package manager, no engine source tree is involved on the consumer's side.

```bash
cmake --preset macos-sdk                    # or windows-sdk / linux-sdk
cmake --build --preset macos-sdk
cmake --install build/macos-sdk             # into build/sdk/macos-sdk

cmake -S samples/phase-4-scripting/cpp -B build/mini-game -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$PWD/build/sdk/macos-sdk"
cmake --build build/mini-game
build/mini-game/aero_mini_game              # interactive
build/mini-game/aero_mini_game --scripted   # the 600-frame run, then one summary line
```

The build copies `shaders/` and `assets/` next to the executable. Copy those two folders and the executable (and, on
Windows, the DLLs beside it) anywhere, and the game runs from there.

## Keys

| Key | Does |
|---|---|
| Left, Right, A, D | move along X |
| Up, Down, W, S | move along Z |
| Space | fire (hold to repeat) |
| Escape | quit |

`--scripted` ignores the keyboard and plays the key table below instead. A non-zero exit says why: 1 is a failure
(its message starts `mini_game:`), 2 an unknown argument, 3 no window or GPU device.

## The rules

These are the reference. Every constant is a named `constexpr` in `main.cpp`.

| Constant | Value | Per frame at 60 Hz |
|---|---|---|
| `FIXED_DT` | 1/60 s | |
| `ARENA_HALF_WIDTH`, `ARENA_HALF_DEPTH` | 8, 6 | X clamped to +-8, Z to +-6 |
| `PLAYER_START_Z`, `PLAYER_Y` | 5, 0.5 | |
| `PLAYER_SPEED` | 6 units/s | 0.1 |
| `FIRE_COOLDOWN` | 0.21 s | a held Space fires every 13 frames |
| `BULLET_SPEED` | 14 units/s along -Z | 0.2333 |
| `BULLET_LIFE` | 1.0 s | 60 frames, range 14 |
| `TARGET_X`, `TARGET_Z` | -4, 0, 4 at -5 | |
| `HIT_RADIUS` | 0.6, in the XZ plane | |

`FIRE_COOLDOWN` is 0.21 and never a multiple of `FIXED_DT`. On a multiple, the test `cooldown <= 0` sits exactly on an
accumulated float sum, and one unit in the last place, which a compiler's fused multiply-add can change, would move an
integer in the summary line.

Frame order, the same in both modes:

1. the keys for this frame;
2. the player moves and is clamped, the cooldown counts down, and a held Space with no cooldown fires;
3. every bullet flies by `BULLET_SPEED * dt` and ages;
4. the hit test: a bullet within `HIT_RADIUS` of a target in the XZ plane destroys both and counts one hit;
5. a bullet at `BULLET_LIFE` is destroyed;
6. draw.

A bullet is a copy of `assets/bullet.aeroprefab` (a sphere scaled 0.2) placed at the player, plus a `Velocity` and a
`Bullet` the code adds, since a prefab file cannot carry a game component. Entities are destroyed after the walk over
them, as `World::each` asks.

## The scripted run

`--scripted` plays 600 frames at `FIXED_DT`, in a 640 x 360 window, from this table of keys (frame, down or up):

| Frame | Keys | Result |
|---|---|---|
| 10, 11 | Space down, up | one shot at x = 0 hits the middle target about frame 51: fired 1, hits 1 |
| 60, 100 | Left down, up | x = -4.0 (40 frames of 0.1) |
| 110, 171 | Space down, up | shots at 110, 123, 136, 149 and 162; the first hits the left target, the other four fly through and expire: fired 6, hits 2 |
| 200, 400 | Right down, up | 200 frames of 0.1 from -4 stop at the clamp, x = 8.0 |
| 410, 411 | Space down, up | a miss at x = 8, which expires at frame 470: fired 7 |
| 420, 460 | Left down, up | x = 4.0 |
| 470, 471 | Space down, up | the shot hits the right target: fired 8, hits 3 |

It prints one line on stdout and exits 0:

```text
mini_game: frames=600 fired=8 hits=3 bullets=0 entities=4 drawn=<n>
```

Each integer answers for one rule: no clamp misses the right target, no cooldown inflates `fired`, no expiry leaves
`bullets` above 0, no hit test leaves `hits` at 0, and a game component that was never registered yields `fired=0`.
`entities` is the camera, the sun, the ground and the player. `drawn` counts the frames the renderer yielded; it is at
least 1 and is not pinned, since a frame can fail to acquire on a CI lane.
