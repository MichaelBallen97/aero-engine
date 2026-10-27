# Phase 4 gate artifact — the two-language demo

**Gate (docs/tasks/phase-4.md):** *Press Play in the editor: hot-reloaded TS or native C++ drives entities; press
Stop: the scene restores exactly.*

**Artifact:** the same game logic twice — as TypeScript scripts in an Aero project, and as a C++ mini-game built
against the engine's public API — committed in this folder.

## The gate as a scenario

- Given a project whose entities carry game logic written in its one language, when I press Play in the editor, then
  that logic drives the entities and picks up my edits to it while the game runs.
- Given the game is playing, when I press Stop, then the scene is exactly as it was before Play: the same entities and
  component values, with nothing left behind.
- Given the same logic written in C++ against the engine's public API, when I build the mini-game with its documented
  steps and run it, then it plays the same game natively.

The C++ half is read through the gate artifact line: the C++ mini-game is built from the C++ project template and runs
as its own program. Play in the editor runs the TypeScript half.

## How the demo is built up

Phase 4 is cut into four stories, in the `## Stories` section of `docs/tasks/phase-4.md`. Each story makes more of
the scenario run, and each one's demo starts from this folder and takes about two minutes:

1. **Play and Stop.** A TypeScript script moves two cubes on Play; Pause and Step hold and advance them; Stop puts the
   scene back exactly.
2. **Edit while playing.** A saved script change reaches the moving cubes without Stop and Play, a thrown error names
   the `.ts` line, and Play writes unsaved work to disk first.
3. **A small TypeScript game.** Keys move the player and fire bullets spawned from an entity-tree asset, VSCode
   autocompletes the API, and the cooker emits each script as bytecode.
4. **The same game in C++.** A C++ project is created from the New Project dialog, and the C++ mini-game builds
   against the public headers and plays beside the TypeScript version.
