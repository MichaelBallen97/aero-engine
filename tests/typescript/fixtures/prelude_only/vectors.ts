declare const e: engine.Entity;

const v = new engine.Vec3(1, 2, 3).set(4, 5, 6);
v.copy(new engine.Vec3());
const q = new engine.Quat(0, 0, 0, 1).set(0, 0, 0, 1);
q.copy(new engine.Quat());
const alive: boolean = e.alive;
export { v, q, alive };
