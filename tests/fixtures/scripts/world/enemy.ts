// 4.4.2: the committed bundle LC50 runs (enemy.js is esbuild 0.28.2's bundle of this file).
export default class Enemy extends engine.Script {
    hits = 0;
    hit(): boolean {
        this.hits += 1;
        return this.hits >= 2 ? engine.destroy(this.entity) : false;
    }
    onDestroy(): void {
        const wreck = engine.spawn();
        wreck.name = "Wreck";
        const t = this.entity.get(engine.Transform);
        const w = wreck.get(engine.Transform);
        if (t && w) w.position.copy(t.worldPosition());
    }
}
