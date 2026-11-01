export default class Probe extends engine.Script {
    seen = false;
    onInit(): void {
        this.seen = this.entity.alive;
    }
    onUpdate(dt: number): void {
        this.seen = this.entity.alive && dt >= 0;
    }
    onDestroy(): void {
        this.seen = this.entity.alive;
    }
}
