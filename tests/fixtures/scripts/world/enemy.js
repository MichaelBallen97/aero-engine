// enemy.ts
var Enemy = class extends engine.Script {
  hits = 0;
  hit() {
    this.hits += 1;
    return this.hits >= 2 ? engine.destroy(this.entity) : false;
  }
  onDestroy() {
    const wreck = engine.spawn();
    wreck.name = "Wreck";
    const t = this.entity.get(engine.Transform);
    const w = wreck.get(engine.Transform);
    if (t && w) w.position.copy(t.worldPosition());
  }
};
export {
  Enemy as default
};
//# sourceMappingURL=enemy.js.map
