export default class Mover extends engine.Script {
    speed = 2;
    onUpdate(dt: number): void {
        const t = this.entity.get(engine.Transform);
        if (t) t.position.x += this.speed * dt;
    }
}
