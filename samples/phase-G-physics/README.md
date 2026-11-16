# Phase G gate artifact — the physics project

**Gate (docs/tasks/phase-G.md):** *In the editor, give a cube a Rigid Body and a collider and see the collider drawn
around it; press Play and it falls, lands on the floor and comes to rest; a ball pushed by a script knocks over a stack
of crates; a character walks up a ramp and stops at a wall; a door swings on its hinge when the character pushes it; a
script hears a trigger volume and finds a crate with a raycast; press Stop and the scene restores exactly; and the same
scene runs as a native C++ game.*

**Artifact:** an Aero project committed in this folder — a TypeScript project whose scene holds a floor, a stack of
crates, a ball, a ramp and a step, a hinged door, a trigger volume and a character — and the same scene as a C++ game
built against the SDK prefix.

The project itself arrives with the phase's tasks; this page states what it has to show.

## The gate as a scenario

- Given a cube above a floor, when I give the cube a Rigid Body and a collider, then the viewport draws the collider
  around it.
- Given that scene, when I press Play, then the cube falls, lands on the floor and comes to rest.
- Given a ball with a script and a stack of crates, when the script pushes the ball, then the ball knocks the stack
  over.
- Given a character, a ramp and a wall, when a script walks the character forward, then it climbs the ramp and stops at
  the wall.
- Given a door hinged to its frame, when the character walks into it, then the door swings on its hinge.
- Given a trigger volume and a crate, when the ball crosses the volume and a script casts a ray at the crate, then the
  script hears the trigger and the ray names the crate.
- Given the game has run, when I press Stop, then the scene is exactly as it was before Play.
- Given the C++ SDK, when I build and run the C++ game of the same scene, then it simulates the same scene and reports
  the same summary.

## How the demo is built up

Phase G is cut into four stories, in the `## Stories` section of `docs/tasks/phase-G.md`. Each story makes more of
the scenario run, and each one's demo starts from this folder and takes about two minutes:

1. **Add, Play, fall, Stop.** A cube with a Rigid Body and a collider falls onto a floor under Play, steps one fixed
   step at a time under Step, and returns under Stop.
2. **See and shape.** Every collider draws as a wireframe, a new collider fits its mesh, handles resize it, an imported
   model collides as its own shape, and gravity is a project setting.
3. **Push, ask, hear.** A script applies forces on the fixed update, casts rays, receives collision and trigger events,
   and layers decide what collides.
4. **Walk, hinge, C++.** A character climbs a ramp and is stopped by a wall, a door swings on a hinge, and the same
   scene runs as a C++ game.
