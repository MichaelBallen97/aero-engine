// The small game of Phase 4's Story 3, the TypeScript half of the two-language demo: the game
// samples/phase-4-scripting/cpp plays, as one script on the player. Every constant and the frame order are
// cpp/main.cpp's, and cpp/README.md states the rules both halves follow.
//
// One frame, main.cpp's stepGame: the keys; the player moves and is clamped, the cooldown counts down, and a held
// Space with no cooldown fires; every bullet flies and ages; the hit test; the expiry. engine.destroy only queues, and
// the queue is destroyed at the end of the update, so an entity this script destroys leaves its lists at once.
//
// The constants are module code, which a reload replaces, so a saved change reaches a running game; the game's state
// is the instance's own fields, which a reload keeps. The player's name shows the score.
//
// SCRIPTED replays the C++ game's --scripted run: its key table by frame, FIXED_DT whatever Play's dt, 600 frames,
// then main.cpp's summary line (without its drawn field) in the player's name, after which the game stands still.
// Set it before Play: a reload that flips it carries the frame count over.

const SCRIPTED: boolean = false; // true: the C++ game's scripted run
const ARENA_HALF_WIDTH = 8; // X is clamped to +-8
const ARENA_HALF_DEPTH = 6; // Z is clamped to +-6
const PLAYER_Y = 0.5;
const PLAYER_SPEED = 6; // units per second
const FIRE_COOLDOWN = 0.21; // seconds; never a multiple of 1/60, see cpp/README.md
const BULLET_SPEED = 14; // units per second along -Z
const BULLET_LIFE = 1; // seconds
const HIT_RADIUS = 0.6; // in the XZ plane
const FIXED_DT = 1 / 60; // seconds per frame of the scripted run
const SCRIPT_FRAMES = 600; // frames of the scripted run
const BULLET_PREFAB = "res://assets/bullet.aeroprefab";

type Key = engine.input.Key;

// main.cpp's SCRIPT: at `frame`, `key` goes down or up. cpp/README.md derives the run by hand.
const SCRIPT: readonly { frame: number; down: boolean; key: Key }[] = [
    { frame: 10, down: true, key: "Space" },
    { frame: 11, down: false, key: "Space" },
    { frame: 60, down: true, key: "Left" },
    { frame: 100, down: false, key: "Left" },
    { frame: 110, down: true, key: "Space" },
    { frame: 171, down: false, key: "Space" },
    { frame: 200, down: true, key: "Right" },
    { frame: 400, down: false, key: "Right" },
    { frame: 410, down: true, key: "Space" },
    { frame: 411, down: false, key: "Space" },
    { frame: 420, down: true, key: "Left" },
    { frame: 460, down: false, key: "Left" },
    { frame: 470, down: true, key: "Space" },
    { frame: 471, down: false, key: "Space" },
];

// Whether `key` is held at `frame` of the scripted run: the last entry for it at or before that frame, the state the
// C++ game's InputState holds after folding that frame's events. The table is in frame order.
function scriptedDown(key: Key, frame: number): boolean {
    let down = false;
    for (const entry of SCRIPT) {
        if (entry.key === key && entry.frame <= frame) {
            down = entry.down;
        }
    }
    return down;
}

// A live bullet: what main.cpp keeps in its Velocity and Bullet components.
interface Bullet {
    entity: engine.Entity;
    age: number; // seconds
}

function clamp(value: number, low: number, high: number): number {
    return Math.min(Math.max(value, low), high);
}

export default class Game extends engine.Script {
    targets: engine.Entity[] = []; // main.cpp's Target components, found by name when the game starts
    bullets: Bullet[] = [];
    cooldown = 0; // seconds until Space fires again
    fired = 0;
    hits = 0;
    frame = 0; // frames stepped since Play, main.cpp's frames

    onInit(): void {
        this.targets = engine.findAll("target");
    }

    onUpdate(dt: number): void {
        if (SCRIPTED) {
            if (this.frame > SCRIPT_FRAMES) {
                return; // the run is over: the game stands still
            }
            if (this.frame === SCRIPT_FRAMES) {
                // The 600th frame's destroys were destroyed at the end of the previous update, so the World now holds
                // what main.cpp counts. Every entity of this game holds a Transform, so the query counts them all.
                const entities = engine.query(engine.Transform).length;
                this.entity.name = `mini_game: frames=${this.frame} fired=${this.fired} hits=${this.hits} bullets=${this.bullets.length} entities=${entities}`;
                this.frame += 1;
                return;
            }
            const frame = this.frame;
            this.step(FIXED_DT, (key) => scriptedDown(key, frame));
        } else {
            this.step(dt, (key) => engine.input.keyDown(key));
        }
        this.frame += 1;
        this.showScore();
    }

    // One frame of the game; `down` says whether a key is held this frame.
    private step(dt: number, down: (key: Key) => boolean): void {
        // An entity something else destroyed leaves the lists first: a component read on a dead entity throws.
        this.targets = this.targets.filter((target) => target.alive);
        this.bullets = this.bullets.filter((bullet) => bullet.entity.alive);

        // The keys; the player moves and is clamped, the cooldown counts down, and a held Space with no cooldown fires.
        let moveX = 0;
        let moveZ = 0;
        moveX -= (down("Left") || down("A")) ? 1 : 0;
        moveX += (down("Right") || down("D")) ? 1 : 0;
        moveZ -= (down("Up") || down("W")) ? 1 : 0;
        moveZ += (down("Down") || down("S")) ? 1 : 0;
        const wantsFire = down("Space");

        const player = this.entity.get(engine.Transform);
        if (player !== null) {
            const position = player.position; // a live view: each write lands in the component
            position.x = clamp(position.x + moveX * PLAYER_SPEED * dt, -ARENA_HALF_WIDTH, ARENA_HALF_WIDTH);
            position.z = clamp(position.z + moveZ * PLAYER_SPEED * dt, -ARENA_HALF_DEPTH, ARENA_HALF_DEPTH);
            this.cooldown = Math.max(0, this.cooldown - dt);
            if (wantsFire && this.cooldown <= 0) {
                this.cooldown = FIRE_COOLDOWN;
                // Read back after the writes: the muzzle is the stored (float) position, as in main.cpp.
                const bullet = engine.spawn(BULLET_PREFAB, new engine.Vec3(position.x, PLAYER_Y, position.z));
                this.bullets.push({ entity: bullet, age: 0 }); // alive at once, so it flies this frame
                this.fired += 1;
            }
        }

        // Every bullet flies and ages; one at BULLET_LIFE is spent.
        const spent = new Set<engine.Entity>();
        for (const bullet of this.bullets) {
            const at = bullet.entity.get(engine.Transform);
            if (at !== null) {
                at.position.z -= BULLET_SPEED * dt;
            }
            bullet.age += dt;
            if (bullet.age >= BULLET_LIFE) {
                spent.add(bullet.entity);
            }
        }

        // The hit test, over every bullet still in the list, a spent one included: within HIT_RADIUS of a target in
        // the XZ plane, the bullet is spent and the target struck.
        const struck: engine.Entity[] = [];
        for (const bullet of this.bullets) {
            const at = bullet.entity.get(engine.Transform);
            if (at === null) {
                continue;
            }
            const bx = at.position.x;
            const bz = at.position.z;
            for (const target of this.targets) {
                const where = target.get(engine.Transform);
                if (where === null) {
                    continue;
                }
                const dx = bx - where.position.x;
                const dz = bz - where.position.z;
                if (dx * dx + dz * dz <= HIT_RADIUS * HIT_RADIUS) {
                    struck.push(target);
                    spent.add(bullet.entity); // a bullet that hits is spent
                }
            }
        }

        // The expiry and the hits. engine.destroy answers false for an entity already queued, so a target two
        // bullets reach in one frame counts one hit, as World::destroy's false does in main.cpp.
        for (const target of struck) {
            if (engine.destroy(target)) {
                this.hits += 1;
            }
        }
        for (const entity of spent) {
            engine.destroy(entity);
        }
        this.targets = this.targets.filter((target) => !struck.includes(target));
        this.bullets = this.bullets.filter((bullet) => !spent.has(bullet.entity));
    }

    // The score, in the player's name, written only when it changes.
    private showScore(): void {
        const score = `player: fired=${this.fired} hits=${this.hits}`;
        if (this.entity.name !== score) {
            this.entity.name = score;
        }
    }
}
