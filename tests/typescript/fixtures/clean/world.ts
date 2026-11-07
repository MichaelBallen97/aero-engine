import type Target from "./target";

export default class World extends engine.Script {
    onInit(): void {
        const a: engine.Entity = engine.spawn();
        const b = engine.spawn();
        a.name = "alpha";
        b.name = a.name;
        b.name = "";
        b.parent = a;
        const up: engine.Entity | null = b.parent;
        if (up) up.name = "up";
        const kids: engine.Entity[] = a.children();
        const count: number = kids.length;
        b.parent = null;
        const hit: engine.Entity | null = engine.find("alpha");
        const all: engine.Entity[] = engine.findAll("alpha");
        const none: engine.Entity[] = engine.query();
        const one: engine.Entity[] = engine.query(engine.Transform);
        const two: engine.Entity[] = engine.query(engine.Transform, engine.MeshRenderer);
        const queued: boolean = engine.destroy(b);
        void [count, hit, all, none, one, two, queued];

        const asset = "res://assets/prefabs/bullet.aeroprefab";
        const planted: engine.Entity = engine.spawn(asset);
        const placedAt: engine.Entity = engine.spawn(asset, new engine.Vec3(1, 2, 3));
        const turned: engine.Entity = engine.spawn(asset, new engine.Vec3(), new engine.Quat());
        const sized: engine.Entity = engine.spawn(asset, new engine.Vec3(), new engine.Quat(), new engine.Vec3(2, 2, 2));
        const own = this.entity.get(engine.Transform);
        if (own) {
            const copied: engine.Entity = engine.spawn(asset, own);
            void copied;
        }
        void [planted, placedAt, turned, sized];
    }

    onUpdate(dt: number): void {
        const other = engine.find("target");
        const inst = other ? (other.script as Target | null) : null;
        if (inst) inst.hit();
        const plain: engine.Script | null = this.entity.script;
        void plain;

        const v = new engine.Vec3(1, 2, 3);
        const w = v
            .clone()
            .add(v)
            .sub(v)
            .scale(2)
            .addScaled(v, 0.5)
            .negate()
            .normalize()
            .lerp(v, 0.5)
            .cross(v)
            .applyQuat(new engine.Quat());
        const dots: number[] = [v.dot(w), v.length(), v.lengthSquared(), v.distanceTo(w)];

        const q = new engine.Quat();
        q.clone()
            .multiply(q)
            .premultiply(q)
            .invert()
            .normalize()
            .slerp(q, 0.5)
            .setFromAxisAngle(v, dt)
            .setFromEuler(0, 1, 2);
        const euler: engine.Vec3 = q.toEuler();
        const qdot: number = q.dot(q);

        const t = this.entity.get(engine.Transform);
        if (t) {
            const pos: engine.Vec3 = t.worldPosition();
            const axes: engine.Vec3[] = [t.forward(), t.right(), t.up()];
            t.translate(v).rotate(q).lookAt(w).lookAt(w, v);
            t.position.x += pos.x + axes.length + euler.y + qdot + dots.length;
        }
    }
}
