# Phase G — 3D Physics · est. 3–4 mo

> **Goal:** the engine gets the 3D physics its stack table names. A game developer gives an entity a rigid body and a collider in the editor, sees the collider in the viewport, presses Play and gravity takes over; game code in TypeScript or C++ pushes bodies, casts rays and hears collisions and triggers; a character walks, a door swings on its hinge; and Stop restores the scene exactly.
>
> **Gate:** In the editor, give a cube a Rigid Body and a collider and see the collider drawn around it; press Play and it falls, lands on the floor and comes to rest; a ball pushed by a script knocks over a stack of crates; a character walks up a ramp and stops at a wall; a door swings on its hinge when the character pushes it; a script hears a trigger volume and finds a crate with a raycast; press Stop and the scene restores exactly; and the same scene runs as a native C++ game
>
> **Phase non-goals:** no soft bodies, cloth, fluids or buoyancy; no vehicles; no ragdolls and no physics-driven animation; no destruction or fracture; no height-field or terrain collider (terrain is v2, `docs/future-roadmap.md`); no joint kinds beyond the four G.3.6 names and no joint motors; no physics-material asset — friction and restitution are fields on the body; no cross-platform determinism (the pinned port builds Jolt with `CROSS_PLATFORM_DETERMINISTIC` off) and no rollback or networking; no double-precision worlds; no 2D physics — Box2D is Phase 7's, and a scene is 2D or 3D (`docs/tasks/phase-7.md`); no Jolt build for iOS or Android here — the mobile runtimes are Phase 6's (6.1.1, 6.2.1); no physics in the exported runtime's loop here — that loop is 5.2.1's, which now depends on G.2.2; and no native C++ game loaded into the editor's Play, which stays as Phase 4 left it. The editor platform matrix is unchanged — desktop only.
>
> **Why a lettered phase.** Numbering is append-only, and a fractioned phase between 4 and 5 would read as one of Phase 4's own epics — `4.5`, `4.6` and `4.7` are its C++-project, language-choice and play-in-editor epics — the collision that lettered Phases E and F (`docs/07`, numbering conventions). This phase is therefore lettered: **G.1 … G.5**, tasks **G.n.k**. It executes between Phase F and Phase 5; in the Notion Build Tracker its `Phase #` is `4.75`, which is a sort key, not an identifier.
>
> **Why it exists at all.** Jolt is the 3D physics choice in the stack table (`docs/01`), `physics/` is a box in the layer diagram (`docs/03`) with a folder that has existed since Phase 0's skeleton, and "Jolt (3D)" is part of what v1.0 is (`docs/06`) — yet until this phase no task in any phase file built it. Measured on 2026-11-16: `engine/physics/` holds a `.gitkeep` and nothing else, Jolt is not in `vcpkg.json`, none of the eleven built-in components is a body or a collider, the script API has no physics, and the editor's game frame runs the scripts' update and nothing else. Two tasks assumed it anyway: 5.2.1's runtime "runs the full loop (scene, physics, audio, script)", and 7.1.1 wraps Box2D "the way Jolt is wrapped". Phase 4's sample shows what the gap costs: both of its games move their bullets by adding a speed to a position and test a hit by comparing a squared distance with a radius, by hand. Phase 5 builds and ships a real 3D game, so the gap is closed before it — the argument that placed Phase E before Phase 4 and Phase F before Phase 5.
>
> **Gate artifact:** the project the gate is run on is committed under `/samples/phase-G-physics/` — a TypeScript project whose scene holds a floor, a stack of crates, a ball, a ramp and a step, a hinged door, a trigger volume and a character, and the same scene as a C++ game.

---

## Stories

The gate as a scenario: Given the editor and a project, when I give a cube a Rigid Body and a collider, press Play,
push a ball into a stack of crates from a script, walk a character up a ramp and through a door, cross a trigger
volume, cast a ray and press Stop, then the collider is drawn, the cube falls and rests, the crates fall, the character
climbs and is stopped by the wall, the door swings on its hinge, the script hears the trigger and the ray names the
crate, and the scene is exactly as it was before Play — and the same scene runs as a native C++ game. Each story below
makes more of it run; its demo lives in `samples/phase-G-physics/`.

### Story 1 — I give a cube a rigid body and a collider, press Play and it falls onto the floor; Stop puts it back
Tasks: G.1.1, G.2.1, G.2.2, G.4.1
As a game developer, I add a Rigid Body and a collider to an entity in the Inspector, press Play and watch gravity move
it, so that motion and collision no longer have to be written by hand in a script.
- Given a cube above a floor, the cube holding a Rigid Body and a box collider and the floor a box collider alone, when
  I press Play, then the cube falls, lands on the floor and comes to rest on it.
- Given the game is playing, when I press Pause and then Step, then the simulation advances by exactly one fixed step
  for each Step.
- Given the cube has come to rest, when I press Stop, then the cube is back where it was before Play and the scene is
  unchanged.
- Given the scene saved and reopened, when I select the cube, then the Inspector shows its Rigid Body and its collider
  with the values I set.
Demo: `samples/phase-G-physics/` — open the falling-cube scene, press Play and watch the cube land, pause and step it,
press Stop and see it return; about one minute.

### Story 2 — I see my colliders, shape them by hand or to my models, and set the world's rules
Tasks: G.4.3, G.4.2, G.4.4, G.2.3, G.4.5
As a game developer, I see every collider as a wireframe, size it with handles or have it fitted to my mesh, give an
imported model its own shape and set gravity for the project, so that what simulates is what I see.
- Given an entity with a collider, when I select it, then the viewport draws the collider's wireframe at its true size
  and pose, and a trigger draws in its own tint.
- Given View ▸ Viewport ▸ Colliders is on, when nothing is selected, then every collider in the scene is drawn.
- Given a cube with no collider, when I add a box collider to it, then the collider is already the size of the cube's
  mesh, and the Inspector shows a box's dimensions and no radius.
- Given a selected box collider in Edit Collider mode, when I drag one of its face handles, then that face moves, the
  Inspector's size follows, and one Undo puts it back.
- Given an imported model with a convex-hull collider and a Rigid Body, when I press Play, then it falls and rests on
  its own shape rather than on a box; and given a static level model with a mesh collider, then a ball rolls over its
  surface.
- Given Project Settings ▸ Physics, when I set gravity to zero and press Play, then the cube stays where it is.
Demo: `samples/phase-G-physics/` — turn the Colliders overlay on, add a box collider to the bare cube and drag a face
handle, press Play to see the imported model rest on its hull, then set gravity to zero and press Play again; about two
minutes.

### Story 3 — My scripts push bodies, ask the world questions and hear what touches what
Tasks: G.3.1, G.3.2, G.3.3, G.3.4
As a game developer, I apply forces from a script, cast rays, receive collision and trigger events and choose which
layers collide, so that gameplay is written against physics instead of around it.
- Given a ball with a script, when the script applies an impulse in `onFixedUpdate`, then the ball rolls into the stack
  of crates and knocks it over, and two runs of the same build end with the crates in the same places.
- Given a crate in front of the camera, when a script casts a ray at it, then the hit names the crate's entity, the
  point and the surface normal; and a ray aimed at empty space returns no hit.
- Given a trigger volume with a script, when the ball enters and leaves it, then the script's `onTriggerEnter` and
  `onTriggerExit` each run once, naming the ball.
- Given two bodies with scripts, when they collide, then each script's `onCollisionEnter` runs once naming the other,
  and `onCollisionExit` runs once when they part.
- Given two layers set not to collide in Project Settings, when a body on one meets a body on the other, then they pass
  through each other, and a raycast masked to one layer does not report the other.
Demo: `samples/phase-G-physics/` — press Play on the crates scene, push the ball with a key and watch the stack fall,
read the trigger's and the ray's lines in the Console, then untick the two layers' box and watch the ball pass through;
about two minutes.

### Story 4 — A character walks my level, a door swings on its hinge, and the same scene runs as a C++ game
Tasks: G.3.5, G.3.6, G.5.1, G.5.2
As a game developer, I move a character that climbs and is stopped by the world, join bodies with joints, and build
the same scene as a native game, so that a whole small 3D game can stand on the engine's physics in either language.
- Given a character on the floor, when a script moves it toward a ramp, a step and a wall, then it walks up the ramp,
  climbs the step, stops at the wall, and reports that it is grounded.
- Given a door hinged to its frame, when the character walks into it, then the door swings about its hinge and stops
  at its limits.
- Given the C++ SDK installed, when I build the sample's C++ game against it and run its scripted input, then the same
  scene simulates and its summary line reports the same counts — crates toppled, trigger entries, the door opened — as
  the TypeScript scene's scripted run.
- Given the gate's scenario, when I run it from start to end on a build of `main`, then every line of it holds.
Demo: `samples/phase-G-physics/` — press Play, walk the character up the ramp and through the door, press Stop, then
build and run the C++ game and compare the two summary lines; about two minutes.

---

## Epic G.1 — Physics core · physics · build-ci

**Goal:** Jolt inside the engine, behind the engine's own types: a world that steps, and bodies and shapes addressed by
handles. Today there is no physics code at all — the folder is a placeholder from Phase 0 and the dependency was never
added.
**Definition of Done:** a body dropped on a static floor lands and comes to rest in a test that is green on all six
presets, and no Jolt name appears outside `engine/physics/src` — enforced by a guard, not by convention.

### G.1.1 Jolt behind `engine/physics`: the world, bodies and primitive shapes · P0 · L · depends: 0.1.2, 0.2.1, 0.2.2, 0.2.5
_(Measured: `engine/physics/` holds a `.gitkeep` from 0.1.1's folder skeleton and nothing else, and `vcpkg.json` names
no physics library. The pinned baseline's port is `joltphysics` 5.5.0 — static-only, built with
`CROSS_PLATFORM_DETERMINISTIC=OFF`, with three opt-in features, `debugrenderer`, `profiler` and `rtti`. `docs/01`
verified 5.6.0 upstream in July, and upstream has been at 5.7.0 since 2026-11-14. Whether the baseline moves is this
task's decision, and a move takes every other dependency with it: `builtin-baseline` and the submodule are one SHA.)_
_(Read from Jolt's own documentation and headers at v5.5.0, for the spike to confirm on this tree: a physics system
cannot be created without three layer-filter interfaces; a step takes a temp allocator and a job system, and the
library expects an engine to bring its own job system; code that uses Jolt must be compiled with the same `JPH_*`
definitions as the library, and type registration aborts the process on a mismatch; Jolt builds without RTTI and
without exceptions unless asked, and a project built with RTTI can then fail at link on its interfaces' type
information; a step is stable at 60 Hz with one collision step. Jolt becomes the ninth backend confined behind rule #3, and its guard the
tenth in `.github/scripts/`.)_
**Goal:** 3D rigid-body physics inside the engine, with nothing of Jolt visible outside one directory.
**Deliverable:** `aero::physics` — a physics world created and destroyed through engine types, with gravity and a fixed
step; static, kinematic and dynamic bodies addressed by handles and shaped as a box, a sphere, a capsule or a cylinder;
a test in which a sphere dropped on a static box comes to rest at the expected height, green on all six presets; and the
physics-boundary guard with its compile-time probe, proven against a seeded violation and cleaned.
Subtasks:
- A spike first: the port under every preset — the `rtti` feature against the engine's own RTTI setting, the `JPH_*` definitions vcpkg's two builds export against the presets' configuration names, ASan and UBSan on all three hosts, what the archive adds to a linked binary — and whether the baseline moves to a newer Jolt
- The `engine/physics` target: Jolt linked `PRIVATE` and confined to `src/`; the library's process-wide setup (allocator, factory, type registration) owned in one place, with a stated rule for two worlds in one process
- The world: gravity, the layer interfaces the library requires as a fixed two-layer table (static, moving) that G.3.4 widens, and stated caps on bodies and contacts
- Bodies by handle (rule: handles, not pointers): create and destroy; motion type; box, sphere, capsule and cylinder shapes with a local offset and rotation; mass from density or given; friction, restitution, damping, gravity factor and continuous detection
- The step: fixed-size steps only; the job system — the engine's `JobSystem` adapted or the library's own pool — chosen on a measurement; the temp allocator's size stated
- Pose and velocity read and write; a teleport against a kinematic move; sleeping and waking
- Errors as results: an invalid handle returns invalid or false and never throws; a step that overflows a cap reports it once
- The boundary pair: `check-physics-boundary.sh` in the lint job — no Jolt token in a public engine header, none in `engine/` or `runtime/` outside `engine/physics/src` — and `tests/physics_boundary_probe.cpp` linking `aero::physics` alone; seeded, proven, cleaned

---

## Epic G.2 — Bodies & colliders in the scene · scene · physics

**Goal:** physics as components. A body and its shape are data on an entity — saved, loaded, inspected and scripted like
a light or a mesh — and a bridge simulates them on a fixed step. Today none of the eleven built-in components describes
a body, and nothing in the engine advances on a fixed interval.
**Definition of Done:** a scene file holding a rigid body and colliders loads, falls and comes to rest when stepped,
through reflected components only, with no editor in the process; an imported model can collide as its own shape.

### G.2.1 `RigidBody` and collider components · P0 · M · depends: G.1.1, 1.3.2, 4.2.1
_(Measured: the built-in component count is eleven and none is a body or a collider. The reflectable subset is
primitives, `Vec3`, `Quat`, `std::string` and `Guid`; a selector is an integer with `AERO_RANGE` and `AERO_LABELS`, as
`MeshRenderer::primitive` is; there is no enum, no variant, no array and no reference from one entity to another, and
an entity holds at most one component of a type. So what a collider *is* — one component with a shape selector, or one
component per shape — and how one body carries several shapes are this task's first decisions.)_
_(A new built-in moves every site that generates from `AERO_BUILTIN_COMPONENT_HEADERS` together with the hand-written
serializer table, and needs a sweep of component-count literals that no single grep finds — 3.7.2, E.2.1 and 4.4.1 each
found more.)_
**Goal:** a body and its shape are data on an entity, like a light or a mesh.
**Deliverable:** reflected `RigidBody` and collider built-ins — registered, shown by the Inspector, saved in scene v1 as
a purely additive change, bound to scripts and declared in `aero-engine.d.ts` — with `docs/09` §2.3 naming their
payloads. They describe a simulation; G.2.2 runs it.
Subtasks:
- The component shapes decided and written down: one collider component with a shape selector or one per shape, how a body carries more than one shape, and which fields a rigid body holds
- `RigidBody` (reflected): motion type (Static / Kinematic / Dynamic), mass, friction, restitution, linear and angular damping, gravity scale, continuous detection — each with its range and default
- The collider (reflected): box, sphere, capsule and cylinder — dimensions, a centre offset, and the trigger flag
- The pairing rules, stated: a collider with no `RigidBody` is static geometry; a `RigidBody` with no collider simulates nothing and says so once
- Every generation site and the hand-written serializer table together; the component-count sweep; icons and kind dots for the new built-ins
- `docs/09` §2.3: the new payloads, additive to scene v1 with no version bump; the golden fixtures re-measured

### G.2.2 The scene's physics bridge and the fixed step · P0 · L · depends: G.1.1, G.2.1, 4.4.4
_(Measured: nothing in the engine steps on a fixed interval. `FrameClock` hands out a variable delta clamped at 0.25 s
— its comment names "a future fixed-step integrator" as one reason for the clamp — scripts receive that delta in
`onUpdate(dt)`, and the one fixed figure in the tree is the editor's `PLAY_STEP_SECONDS`, 1/60 s, which Step alone
uses. `World` has no hook that reports a component being added or removed, so a system that mirrors components finds
the changes by looking — `ScriptSystem` takes a snapshot of the `ScriptComponent`s on every update for exactly that.)_
_(The shape to copy is `engine/scene_audio`: a bridge target that is the only code seeing both `scene` and the
subsystem under it, so `engine/physics` stays scene-free and a binary can link physics without the ECS.)_
**Goal:** the components simulate: what the scene says is what the physics world holds, and what the physics world
computes is where the entities are.
**Deliverable:** `engine/scene_physics` — a physics scene over a `World` that creates and destroys bodies as components
and entities come and go (a scene loading, `engine.spawn`, `engine.destroy`, a component added or removed, a cloned
`World`), advances on a fixed timestep with an accumulator and a cap, and writes simulated poses back into
`Transform`s; a scene file of a body above a floor falls and rests when stepped, with no editor in the process.
Subtasks:
- `engine/scene_physics`: the bridge target, linking `aero::scene` and `aero::physics` and no vcpkg package, its CMake guarded as `scene_audio`'s is; `aero::engine` gains both layers
- Body lifetime by comparison: a body made when a collider appears, remade when its shape or motion type changes, removed with its component or its entity — and never in the middle of a step
- The fixed step: 1/60 s by default, an accumulator, a cap on steps per frame with a stated rule for the time left over; whether drawing interpolates between steps is decided and written down
- Pose sync: a simulated body writes position and rotation to its `Transform` after the step; a `Transform` changed from outside moves a kinematic body as a kinematic move and a dynamic one as a teleport, by a stated rule that tells the two apart
- Hierarchy and scale: a body on a child entity (world pose in, local pose out), a body parented under another body, and scale — a non-uniform scale on a sphere or a capsule has a stated rule, since the library takes uniform scale only for them
- Pinned as tests: the rest height of a dropped body; two runs of the same thousand steps on one build ending bit-identical

### G.2.3 Model-shaped colliders · P1 · L · depends: G.2.2, G.4.3, 3.1.5, 3.3.1
_(Measured: a loaded model's geometry is not kept on the CPU. The cooked mesh is read through once to make GPU buffers
— in the editor `loadFromImportedModel` cooks into a local buffer that dies with the call — and `AssetBindingTable`
maps a model's GUID to render handles only. F.1.1 measured the same thing from the picking side and decides a triangle
source for the editor; whether the two share one source is this task's first decision.)_
_(From Jolt's documentation: a triangle-mesh shape is meant for static bodies — a moving one must not meet another
mesh, needs its mass supplied by hand and is "fairly expensive to simulate" — while a convex hull serves any motion
type.)_
**Goal:** a model collides as its own shape, not as a box drawn around it.
**Deliverable:** two more collider shapes taken from a model asset — a convex hull for any body, and a triangle mesh for
static geometry — defaulting to the entity's own `MeshRenderer` mesh, drawn by G.4.3's wireframes, with the build cost
recorded on the largest sample model and the route into an exported game stated for 5.1.2.
Subtasks:
- The geometry source: where a model's positions and indices come from at run time and in the editor, shared with F.1.1's or separate, decided on a measurement of memory and load time
- Convex hull: built from the model's points, with a vertex cap and a stated fallback when no hull can be built
- Triangle mesh: static bodies only; a kinematic or dynamic body naming one is refused with one Console line and simulates as nothing
- Where shapes are built and kept: at load or by the cooker, cached by content hash and rebuilt when the model changes
- Multi-part models, the built-in primitives and skinned meshes: each with its stated rule
- The hull and the mesh drawn as wireframes; the cost of both on the largest sample model recorded

---

## Epic G.3 — Physics for gameplay · script · physics

**Goal:** game code — TypeScript or C++ — pushes bodies, asks the world questions and hears about contacts. Today a
script can move a `Transform` and nothing else: it has no fixed update, no force, no ray and no event.
**Definition of Done:** a script applies an impulse on the fixed step, casts a ray, receives collision and trigger
events, moves a character and holds a door on a hinge — and a C++ game does each of those through public headers.

### G.3.1 Forces, velocities and the fixed update · P0 · M · depends: G.2.2, G.4.1, 4.4.1, 4.4.2, 4.2.2
_(Measured: `engine.Script` has three hooks — `onInit`, `onUpdate(dt)` and `onDestroy` — and `ScriptSystem::update` runs
once per game frame. A component reaches a script as its reflected fields; a method on a component, like `Transform`'s
`translate` and `lookAt`, is hand-written beside the generated binding. Every member added to the `engine` namespace
becomes a reserved component script name and moves the VM's measured heap figures (4.4.2, 4.4.3), and the VM reaches
the game's input through a borrowed binding, `Vm::bindInput`.)_
**Goal:** game code moves bodies the way a simulation expects — by forces, impulses and velocities, on the simulation's
own clock.
**Deliverable:** an `onFixedUpdate(dt)` hook that runs once before each physics step; on a `RigidBody`, methods to add
a force, an impulse and a torque, to read and set linear and angular velocity, to move a kinematic body and to
teleport; `engine.physics.gravity`; the same operations for a C++ game through `scene_physics`' public header; all of
it declared in `aero-engine.d.ts`.
Subtasks:
- `onFixedUpdate(dt)`: its place in the frame against `onUpdate`, the step and the deferred `engine.destroy`; a script without the hook costs nothing
- `RigidBody` methods: `addForce`, `addImpulse`, `addTorque`, velocity get and set, `movePosition` / `moveRotation` for kinematic bodies, `teleport`; a call on a body that is not simulating has a stated result
- The VM borrows the physics scene as it borrows input: bound per Play, and a VM with none answers every call by a stated rule
- `engine.physics`: `gravity` read and write; the reserved-name list, the prelude and the namespace's binding cases updated
- The C++ surface: the same operations on the bridge's public header, engine types only
- `.d.ts` and the script type checks; the VM's heap and atom figures re-measured

### G.3.2 Raycasts and overlap queries · P0 · M · depends: G.2.2, G.3.1, 4.4.2
_(Measured: the only ray in the tree is the editor's. `Ray` is declared in `editor/include/aero/editor/picking.hpp`,
whose comment keeps it there until "the first real ENGINE consumer appears" and names Jolt raycasts as that consumer,
to be promoted together with `Aabb`. The editor's pick tests render bounds, never colliders, and a script has no way
to ask what lies along a line or inside a volume.)_
_(From Jolt's documentation: its narrow-phase queries cast rays and shapes and collect overlaps, and the order of
their results is not guaranteed.)_
**Goal:** game code asks the physics world what is there.
**Deliverable:** `engine.physics.raycast` returning the nearest hit — entity, point, normal and distance — or nothing;
`raycastAll`; `overlapSphere` and `overlapBox`; a sphere cast; each with a maximum distance and, once G.3.4 lands, a
layer mask; the same calls for C++; and `Ray` promoted from the editor into `engine/core/math`.
Subtasks:
- `Ray` and `Aabb` moved to `core/math`, as the editor's note asks; the editor's picking uses the engine types
- `raycast` and `raycastAll`: nearest-first, an order the engine imposes since the library's is not guaranteed; a back face and a ray starting inside a shape each have a stated rule
- `overlapSphere`, `overlapBox` and `sphereCast`; whether triggers are reported is a parameter with a stated default
- Results name entities: a hit on a body whose entity was destroyed this frame is dropped
- Script bindings and `.d.ts`; the C++ header; a query issued in the middle of a step is refused or deferred by a stated rule

### G.3.3 Collision and trigger events · P0 · L · depends: G.2.2, G.3.1
_(From Jolt's documentation: contact callbacks arrive from several threads at once in the middle of the step with every
body locked; a callback may read the bodies and may not change physics state, and the callback for a contact that ended
cannot touch the bodies at all. A sensor does not detect static bodies unless told to, and a static sensor loses a
body the moment that body falls asleep.)_
_(Measured: scripts run in one VM per Play, on the thread that runs the game frame, and `engine.destroy` is already
deferred to the end of the update — so an event can only reach a script after the step, out of a buffer.)_
**Goal:** game code hears what touched what.
**Deliverable:** four hooks on a script — `onCollisionEnter(other, contact)`, `onCollisionExit(other)`,
`onTriggerEnter(other)` and `onTriggerExit(other)` — delivered after each step in a stated, repeatable order, with a
collider's trigger flag making it a volume that detects and does not push; and the same events as a list a C++ game
reads.
Subtasks:
- Collecting: contacts recorded inside the step, from the library's threads, into a buffer — with no script and no scene access there
- Delivering: after the step, in an order that is the same on every run; begin and end paired exactly once per pair of entities, however many shapes touch
- Triggers: G.2.1's flag honoured; what a trigger detects — static bodies, sleeping bodies, other triggers — decided against the library's defaults and written down
- The contact a script is handed: the other entity, a point, a normal and how hard — which of them, stated
- An entity destroyed or spawned by a handler, and a body removed while touching: each ends its pairs cleanly
- The C++ event list; `.d.ts`; a script without the hooks costs nothing

### G.3.4 Collision layers · P1 · M · depends: G.3.2, G.3.3, G.4.5
_(Measured: G.1.1 gives the physics world the two layers the library cannot start without, static and moving. A project
has nowhere to name a layer: `project.json` has five root keys and none is about physics until G.4.5 adds its block.)_
**Goal:** decide what collides with what.
**Deliverable:** named collision layers per project with a matrix saying which pairs collide, a layer on every
collider or body, and layer masks on every query G.3.2 added; saved in `project.json` and edited in Project Settings ▸
Physics.
Subtasks:
- The layer model: how many layers, the built-in ones, and whether a layer belongs to the collider or to the body — decided once
- The matrix in `project.json` (`docs/09` §4, additive, an absent key is the default) and its grid in Project Settings ▸ Physics
- The layer field shown by name in the Inspector; a layer that was renamed or removed has a stated fallback
- Masks on `raycast`, the overlaps and the casts; triggers and the character controller obey the matrix
- A change to the matrix while the game plays: applied or refused, stated

### G.3.5 Character controller · P0 · L · depends: G.2.2, G.3.2, G.3.3
_(From Jolt's documentation: the library offers two characters. One is a rigid body restricted to translation and
simulated with the rest. The other is moved by collision queries alone and is not in the world — rigid bodies do not
see it unless it is given an inner body, and it reports contacts through a listener of its own — with wall sliding,
stair stepping and moving platforms built in. Which of the two, and how it then meets G.3.3's events, triggers and
dynamic bodies, is this task's first decision.)_
**Goal:** a player on foot — the thing most 3D games move.
**Deliverable:** a reflected `CharacterController` component — a capsule with a step height and a slope limit — and a
`move` call for scripts and C++ that walks it: it climbs slopes up to the limit and steps up to the height, slides
along walls, falls under gravity, and reports whether it is grounded and on what.
Subtasks:
- The choice between the library's two characters, made on a spike against triggers, events and pushing dynamic bodies
- `CharacterController` (reflected): radius, height, step height, slope limit and mass — a new built-in through G.2.1's same sites
- `move(velocity)`: applied on the fixed step, with gravity by a stated rule; `isGrounded`, the ground's normal and its entity
- Meeting the world: static geometry, kinematic platforms, dynamic bodies pushed or not by a stated rule; a character fires triggers; layers apply
- Its wireframe through G.4.3's path; script bindings, `.d.ts` and the C++ surface

### G.3.6 Joints · P0 · L · depends: G.2.2, G.4.3
_(From Jolt's documentation: the library has thirteen kinds of constraint. This task wraps four.)_
_(Measured: the reflectable subset has no reference from one entity to another, and a scene's entity ids are
file-scoped with "no cross-file or session stability promise" (`docs/09` §2.2). How a joint on one entity names the
other body is this task's first decision.)_
**Goal:** bodies held together — a door on its frame, a lamp on its chain.
**Deliverable:** four joints as reflected components — fixed, hinge, slider and distance — each joining its entity's
body to another body or to the world at an anchor, with limits on the hinge and the slider and a spring on the distance
joint; drawn in the viewport for a selected joint; saved, inspected and scripted like any built-in.
Subtasks:
- The reference: how a joint names its other body — through the hierarchy, a new reflectable reference, or otherwise — decided against scene v1 and written down
- Fixed, hinge, slider and distance joints (reflected): anchor, axis, limits, spring; a joint whose other body is missing joins to the world or does nothing, by a stated rule
- Lifetime: made and removed with its component and with either body, through G.2.2's comparison
- The gizmo: the anchor, the axis and the limits drawn for a selected joint
- A break force past which a joint lets go: included, or left to v2 — stated

---

## Epic G.4 — Physics in the editor · editor

**Goal:** physics is added, seen, shaped and run from the editor. Today the editor has nothing for it: Play runs the
scripts and nothing else, no gizmo draws a shape that is not a light, and no handle in the viewport edits a field.
**Definition of Done:** Play simulates and Stop restores exactly; every collider can be seen as a wireframe and sized
with handles; the Inspector shows only the rows that apply; gravity and the timestep are project settings.

### G.4.1 Physics in Play · P0 · M · depends: G.2.2, 4.7.1, 4.7.2, 4.7.4
_(Measured: the editor's game frame runs the scripts' update and nothing else — `EditorApp` calls
`PlayScripts::update` once for each frame that runs and then ends the game's input frame. Play parks the scene and runs
an identity-exact copy; Stop tears the scripts down and moves the original back. While Playing a game frame takes the
editor's clamped frame delta; a Step takes exactly `PLAY_STEP_SECONDS`, 1/60 s.)_
**Goal:** pressing Play simulates.
**Deliverable:** each Play builds a physics scene over the copy and steps it inside the game frame, in a stated order
with the scripts; Pause freezes it; a Step advances it by exactly one fixed step; Stop destroys it before the scene is
restored, so Stop's exact restore holds with bodies in the scene; and the viewport's stats show the body count and what
the step cost.
Subtasks:
- The physics scene's lifetime: made at Play over the copy, after the park and beside the scripts; destroyed at Stop before the restore
- The game frame's order, stated once: the scripts' `onUpdate`, then the fixed steps the frame's time holds, then the frame's deferred destroys
- Pause and Step: a paused game steps nothing; one Step is one fixed step, whatever the editor's frame rate
- Edit mode simulates nothing: a body moved in the editor is simply where it was put
- Stats: bodies, awake bodies and the step's time in the viewport's stats card
- Stop's exact restore proven again with simulated bodies in the scene; the play recovery record is unaffected

### G.4.2 Physics components in the Inspector and the menus · P0 · M · depends: G.2.1, 2.2.2, E.5.2, E.6.3
_(Measured: the Inspector is reflection-driven, so G.2.1's components already appear with every field as a row, and Add
Component already lists every registered component — all of them in a TypeScript project, and in a C++ project all
that name no script asset. What it cannot do is leave a row out: the reflection vocabulary is `AERO_RANGE`,
`AERO_COLOR`, `AERO_ASSET` and `AERO_LABELS`, and none of them makes one field's row depend on another field's value,
so a collider would show a radius beside a box's size. The Create menu has seven typed entries — three 3D objects,
three lights and a camera — and none carries physics.)_
**Goal:** adding physics to an entity is two clicks and leaves nothing to fix up.
**Deliverable:** the physics components shown with only the rows that apply to the chosen shape and motion type; a
collider added to an entity with a mesh arrives sized to that mesh's bounds; the Inspector says in place when a
combination will not simulate; and the Create menu gains a Physics group.
Subtasks:
- Rows that apply: a collider shows its own shape's dimensions only, a static body hides mass and damping — by a rule in the editor or a new reflection hint, decided once
- Fit on add: a new collider takes the size and centre of the entity's mesh bounds, primitive or imported, in the same undo step as the add
- In-place notes: a dynamic body with no collider, a mesh collider on a moving body, a scale a shape cannot take
- Create ▸ Physics: its entries decided against the seven there today — at least a dynamic cube, a static floor and a trigger volume — each one undo step
- Add Component's list grouped once physics adds several entries to it

### G.4.3 Collider gizmos and the physics overlay · P0 · M · depends: G.2.1, E.1.1, E.2.3, F.2.2
_(Measured: the debug-line batch draws lines, boxes, circles and spheres — `wireBox`, `wireCircle`, `wireSphere` — and
has no capsule and no cylinder. Gizmos are emitted for selected entities only, and a `static_assert` keeps the grid plus
a fully selected scene's light gizmos inside the line budget. After F.2.2 the viewport's overlay toggles live in View ▸
Viewport.)_
**Goal:** see the shape that will collide, where it will collide.
**Deliverable:** a selected entity's colliders drawn as wireframes at their true size, offset and pose — box, sphere,
capsule and cylinder, a trigger in its own tint — and a View ▸ Viewport ▸ Colliders toggle that draws every collider in
the scene, coloured during Play by what its body is doing.
Subtasks:
- A wire capsule and a wire cylinder added to the debug-line batch, with their line counts
- Collider gizmos for the selection, through E.2.3's emit path; the line budget's `static_assert` restated with colliders in it
- The Colliders toggle in View ▸ Viewport, saved with the other viewport options; a cap on what is drawn, with the dropped count reported
- During Play: static, awake, asleep and trigger each in a stated colour
- A collider on an entity with no mesh is still clickable — by its wireframe or an icon, a rule stated against F.1.1's pick

### G.4.4 Collider handles in the viewport · P1 · M · depends: G.4.3, 2.3.3, 2.4.2
_(Measured: the viewport's only manipulator is the transform gizmo — one `ImGuizmo::Manipulate` call — which moves,
turns and scales an entity and knows nothing of a component's fields. No handle in the viewport edits a field today.)_
**Goal:** size a collider by dragging it, not by typing.
**Deliverable:** an Edit Collider mode for the selected collider: handles on a box's six faces, on a sphere's radius,
and on a capsule's or a cylinder's radius and height; a drag edits the collider's fields live and lands as one undo
step; the transform gizmo stands down while the mode is on.
Subtasks:
- The mode: entered from the Inspector's collider card and from a key in the viewport, left by Escape or a change of selection; what the transform gizmo and the tool keys do while it is on
- Handles drawn through the debug-line batch and hit-tested in screen space, with hover and active states
- Dragging: along the handle's axis, with the transform gizmo's snap modifier, the opposite face staying where it is — one property command per drag
- Locked in Play like every scene edit

### G.4.5 Project physics settings · P1 · S · depends: G.2.2, 2.6.2
_(Measured: `project.json` has five root keys — `version`, `name`, `engineVersion`, `language` and `paths` — and an
unknown key is tolerated on load and stripped on save (`docs/09` §4.1). Until this task gravity is the library's
default, (0, −9.81, 0), and the fixed step is G.2.2's 1/60 s, with no way to change either for a project.)_
**Goal:** the world's rules belong to the project.
**Deliverable:** a `physics` block in `project.json` — gravity, the fixed timestep and the cap on steps per frame —
edited in Project Settings ▸ Physics, read at Play and by a C++ game, and specified in `docs/09` §4 as an additive
change.
Subtasks:
- The `physics` block: its keys, ranges and defaults, an absent key being the default; `docs/09` §4.1 and its error catalog
- Project Settings ▸ Physics: gravity as a vector row, the timestep and the cap
- Read at Play and applied to the physics scene; a change made while the game plays applies from the next Play
- A C++ game reads the same block

---

## Epic G.5 — Sample & SDK · samples · build-ci

**Goal:** the phase's proof, in both languages: the installed engine carries physics to a C++ game, and one sample scene
uses everything the phase built.
**Definition of Done:** the phase gate validated on `samples/phase-G-physics/`, and a C++ game built against the SDK
prefix alone simulating the same scene.

### G.5.1 Physics in the C++ SDK · P0 · M · depends: G.2.2, G.3.3, 4.5.1, 4.5.2
_(Measured: the SDK prefix installs the engine's static layers, its public headers and four third-party link files
behind a hand-written CMake package, and six `sdk.*` tests prove it on all three CI lanes (4.5.1). `aero::engine` names
the layers a C++ game links; the physics layers join it in G.2.2, and Jolt's archive becomes a fifth third-party link
file, which a game must link without one Jolt header reaching its compile line. On MSVC the package pins the release
DLL runtime in every configuration.)_
**Goal:** a C++ game gets physics from the installed engine alone.
**Deliverable:** the `<os>-sdk` presets install the physics layers and their link file; a game built against the prefix
with `find_package(AeroEngine)` creates bodies, steps them and reads their events; and a new `sdk.*` test drops a body
in such a game on all three lanes.
Subtasks:
- Install rules: the two layers, their public headers, Jolt's archive and the definitions a consumer has to share with it — none of them putting a Jolt header on the consumer's include path
- The package: `find_package(AeroEngine)` resolves physics with no toolchain file; MSVC's runtime pin and the SDK's sanitizer-free configuration checked against the flags Jolt was built with
- `sdk.physics`: a game that drops a body, steps a fixed number of times and prints where it rests
- The header-compiles-alone check extended to the new public headers; `docs/04`'s SDK section updated

### G.5.2 The physics sample · P0 · M · depends: G.3.1, G.3.2, G.3.3, G.3.5, G.3.6, G.4.1, G.4.3, G.5.1
**Goal:** the gate artifact — one scene that uses everything the phase built, in both languages.
**Deliverable:** `samples/phase-G-physics/` — a TypeScript project whose scene holds a floor, a stack of crates, a ball,
a ramp and a step, a hinged door, a trigger volume and a character; scripts that drive the ball and the character from
the keyboard, or from a scripted run that prints a summary line; and a C++ game of the same scene printing the same
summary.
Subtasks:
- The scene and its scripts; a scripted mode, as in Phase 4's sample, whose summary counts what holds on every run — crates toppled, trigger entries, the door opened, the character grounded
- The C++ game of the same scene, built against the SDK prefix
- The sample's README and its validation page
- **Validate the gate**

---

**Phase gate:** In the editor, give a cube a Rigid Body and a collider and see the collider drawn around it; press Play and it falls, lands on the floor and comes to rest; a ball pushed by a script knocks over a stack of crates; a character walks up a ramp and stops at a wall; a door swings on its hinge when the character pushes it; a script hears a trigger volume and finds a crate with a raycast; press Stop and the scene restores exactly; and the same scene runs as a native C++ game.
