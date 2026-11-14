// Slides the entity along x with a sine, from where it starts. Hold Space to freeze it.
// SPEED is module code, which a reload replaces, so a saved SPEED reaches a running game at once; phase and originX
// are the instance's own fields, which a reload keeps, so the motion carries on from where it is. A field's new
// starting value would reach only the next Play.
const SPEED = 1.5; // radians per second
const AMPLITUDE = 2;

export default class Mover extends engine.Script {
    phase = 0; // radians
    originX = 0;

    onInit(): void {
        const transform = this.entity.get(engine.Transform);
        if (transform !== null) {
            this.originX = transform.position.x;
        }
    }

    onUpdate(dt: number): void {
        if (engine.input.keyDown("Space")) {
            return;
        }
        this.phase += dt * SPEED;
        const transform = this.entity.get(engine.Transform);
        if (transform !== null) {
            transform.position.x = this.originX + Math.sin(this.phase) * AMPLITUDE;
        }
    }
}
