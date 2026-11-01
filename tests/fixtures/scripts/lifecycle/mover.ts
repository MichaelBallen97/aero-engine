// 4.4.1: the committed bundle BN56 and BN57 run (mover.js is esbuild 0.28.2's bundle of this file).
export default class Mover extends engine.Script {
    speed = 2;
    onUpdate(dt: number): void {
        const t = this.entity.get(engine.Transform);
        if (t) t.position.x += this.speed * dt;
    }
}
