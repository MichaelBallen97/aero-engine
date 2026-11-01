// mover.ts
var Mover = class extends engine.Script {
  speed = 2;
  onUpdate(dt) {
    const t = this.entity.get(engine.Transform);
    if (t) t.position.x += this.speed * dt;
  }
};
export {
  Mover as default
};
//# sourceMappingURL=mover.js.map
