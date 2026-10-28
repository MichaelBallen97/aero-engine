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
t.position = { x: 1, y: 2, z: 3, set() { return this; }, copy() { return this; } }; // expect TS2741
class Hack extends engine.Transform {} // expect TS2675
const helper: Helper | undefined = undefined;
export { pattern, helper, Hack };
