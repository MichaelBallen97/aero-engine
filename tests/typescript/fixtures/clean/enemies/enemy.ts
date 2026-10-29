import { speed } from "../helper";

export function chase(e: engine.Entity): void {
    const t = e.get(engine.Transform);
    if (t) {
        t.position.z += speed;
    }
}
