import { speed } from "./helper";
import type { Tuning } from "./helper";

type Equals<A, B> = (<X>() => X extends A ? 1 : 2) extends (<X>() => X extends B ? 1 : 2) ? true : false;

declare const e: engine.Entity;
declare const env: engine.Environment;
declare const light: engine.DirectionalLight;
declare const mesh: engine.MeshRenderer;

const found = e.get(engine.Transform);
const exact: Equals<typeof found, engine.Transform | null> = true;
e.add(engine.Transform).position.x += speed;
new engine.Vec3(1, 2, 3).set(0, 0, 0).copy(new engine.Vec3());
const t = e.add(engine.Transform);
t.rotation = new engine.Quat();
env.backgroundMode = 1;
env.skyColor.set(0.2, 0.4, 0.8);
const maybeLight = e.get(engine.DirectionalLight);
if (maybeLight) {
    maybeLight.castsShadows = true;
    maybeLight.intensity = 2;
}
mesh.material = mesh.mesh;
const removed: boolean = e.remove(engine.Camera) && e.has(engine.AudioSource);
const alive: boolean = e.alive;

class Handle {
    [Symbol.dispose](): void {}
}
{
    using handle = new Handle();
    void handle;
}
const last = [1, 2, 3].findLast((n) => n > 1);
const tuning: Tuning = { gain: speed };
export { exact, removed, alive, last, tuning };
