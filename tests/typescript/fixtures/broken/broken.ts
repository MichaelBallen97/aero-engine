import { Helper } from "./helper"; // expect TS1484
export { Helper as Renamed } from "./helper"; // expect TS1205
declare const e: engine.Entity;
declare const t: engine.Transform;
declare const env: engine.Environment;
declare const light: engine.DirectionalLight;
t.positon; // expect TS2551
t.position = 3; // expect TS2322
t.position.x = "a"; // expect TS2322
light.castsShadows = 1; // expect TS2322
env.backgroundMode = "Sky"; // expect TS2322
new engine.Transform(); // expect TS2673
new engine.Entity(); // expect TS2673
e.get(engine.Vec3); // expect TS2345
e.add({}); // expect TS2345
e.has(engine.Entity); // expect TS2345
e.remove(class {}); // expect TS2345
e.alive = false; // expect TS2540
console.log("x"); // expect TS2584
setTimeout(() => {}, 1); // expect TS2304
const pattern = /\p{L}/v; // expect TS1501
t.position = { x: 1, y: 2, z: 3, set() { return this; }, copy() { return this; } }; // expect TS2740
class Hack extends engine.Transform {} // expect TS2675
declare const audio: engine.AudioSource;
const listener: engine.AudioListener = audio; // expect TS2322
const mustExist: engine.Transform = e.get(engine.Transform); // expect TS2322
class WrongSig extends engine.Script { onUpdate(dt: string): void { void dt; } } // expect TS2416
class Assign extends engine.Script { f(): void { this.entity = this.entity; } } // expect TS2540
class NoBase { f(): void { void this.entity; } } // expect TS2339
class Typo extends engine.Script { override onUpate(): void {} } // expect TS4117
const direct = new engine.Script(); // expect TS2674
t.forwrd(); // expect TS2551
new engine.Vec3().normalise(); // expect TS2551
t.translate(t.rotation); // expect TS2345
t.position.addScaled(t.position, "2"); // expect TS2345
engine.query(engine.Vec3); // expect TS2345
engine.find(3); // expect TS2345
engine.spawn("bullet"); // expect TS2554
e.script = null; // expect TS2540
e.parent = 3; // expect TS2322
engine.input.keyDown("Spcae"); // expect TS2345
engine.input.keyDown("w"); // expect TS2345
engine.input.keyDwon("W"); // expect TS2551
engine.input.mouseButtonDown("Back"); // expect TS2345
engine.input.gamepadButtonDown("A"); // expect TS2345
engine.input.gamepadAxis("LeftZ"); // expect TS2345
engine.input.gamepadStick("Up"); // expect TS2345
engine.input.gamepadConnected("0"); // expect TS2345
const notAKey: engine.input.Key = "Unknown"; // expect TS2322
const helper: Helper | undefined = undefined;
export { pattern, helper, Hack, listener, mustExist, WrongSig, Assign, NoBase, Typo, direct, notAKey };
