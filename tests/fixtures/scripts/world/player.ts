// 4.4.2: the committed bundle LC50 runs (player.js is esbuild 0.28.2's bundle of this file).
import type Enemy from "./enemy";

export default class Player extends engine.Script {
    foundSelf = false;
    sawBullets = 0;
    queried = 0;
    queuedAlive = false;
    onInit(): void {
        const t = this.entity.get(engine.Transform);
        if (!t) return;
        for (let i = 0; i < 3; i++) {
            const bullet = engine.spawn();
            bullet.name = "Bullet";
            const b = bullet.get(engine.Transform);
            if (b) b.position.copy(t.worldPosition()).addScaled(t.forward(), i + 1);
        }
        this.foundSelf = engine.find("Player") === this.entity;
        this.sawBullets = engine.findAll("Bullet").length;
        this.queried = engine.query(engine.Transform, engine.ScriptComponent).length;
    }
    onUpdate(dt: number): void {
        for (const bullet of engine.findAll("Bullet")) {
            bullet.get(engine.Transform)?.translate(new engine.Vec3(0, 0, -1).scale(dt));
        }
        this.entity
            .get(engine.Transform)
            ?.rotate(new engine.Quat().setFromAxisAngle(new engine.Vec3(0, 1, 0), dt));
        const enemy = engine.find("Enemy");
        if (enemy) {
            if ((enemy.script as Enemy | null)?.hit()) this.queuedAlive = enemy.alive;
        }
    }
}
