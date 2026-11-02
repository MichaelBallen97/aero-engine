export default class Target extends engine.Script {
    hits = 0;
    hit(): boolean {
        this.hits += 1;
        return this.hits >= 2 ? engine.destroy(this.entity) : false;
    }
}
