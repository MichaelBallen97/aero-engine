// Slides the entity along x with a sine, from where it starts. Hold Space to freeze it.
// A saved edit to speed or amplitude reloads into the running game and continues from the same time.
export default class Mover extends engine.Script {
    speed = 1.5; // radians per second
    amplitude = 2;
    time = 0;
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
        this.time += dt;
        const transform = this.entity.get(engine.Transform);
        if (transform !== null) {
            transform.position.x = this.originX + Math.sin(this.time * this.speed) * this.amplitude;
        }
    }
}
