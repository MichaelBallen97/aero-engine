// Aero Engine -- the hand-written half of aero-engine.d.ts (task 4.2.2): Vec3, Quat, Entity and Script (the base class
// a script's class extends, task 4.4.1) as engine/script/src/script_api*.cpp installs them, and the component types
// each generated block merges into.
// The build joins this file first, then one reflect-gen --emit-dts block per built-in component header.
// A change to the script API changes this file in the same commit: DT3-DT7 compare it with a live VM.
// Grammar (the drift reader refuses anything else): one declaration per line, doc comments on one line,
// no at-sign anywhere. A class's constructor line is `constructor(...)`, `private constructor(...)` or, for a base
// class a script extends, `protected constructor(...)`; a method may be optional (`name?(...): type;`), a field never.

declare namespace engine {
    /** Every component class, keyed by its script name. Each generated block adds its own entries. */
    interface ComponentMap {}

    /** Any component instance. */
    type Component = ComponentMap[keyof ComponentMap];

    /** A component class as entity.has/get/add/remove take it, e.g. engine.Transform. */
    interface ComponentClass<T extends Component> {
        readonly prototype: T;
    }

    /** A 3D vector. A component's Vec3 field reads as a live view: writing x, y or z writes the component. */
    class Vec3 {
        private readonly __brand: "Vec3";
        /** An omitted coordinate is 0. */
        constructor(x?: number, y?: number, z?: number);
        x: number;
        y: number;
        z: number;
        /** Writes all three coordinates and returns this vector. */
        set(x: number, y: number, z: number): this;
        /** Copies another Vec3 into this one and returns this vector. */
        copy(v: Vec3): this;
    }

    /** A rotation quaternion. A component's Quat field reads as a live view. */
    class Quat {
        private readonly __brand: "Quat";
        /** An omitted x, y or z is 0 and an omitted w is 1. */
        constructor(x?: number, y?: number, z?: number, w?: number);
        x: number;
        y: number;
        z: number;
        w: number;
        /** Writes all four values and returns this quaternion. */
        set(x: number, y: number, z: number, w: number): this;
        /** Copies another Quat into this one and returns this quaternion. */
        copy(q: Quat): this;
    }

    /** An entity of the World the VM is bound to. A script cannot construct one. */
    class Entity {
        private constructor();
        private readonly __brand: "Entity";
        /** False once the entity is destroyed or the World is rebound; reading it never throws. */
        readonly alive: boolean;
        /** Whether the entity holds the component. */
        has<T extends Component>(type: ComponentClass<T>): boolean;
        /** The entity's component, or null when it has none. */
        get<T extends Component>(type: ComponentClass<T>): T | null;
        /** Adds the component with its default values (replacing one already present) and returns it. */
        add<T extends Component>(type: ComponentClass<T>): T;
        /** Removes the component; false when the entity had none. */
        remove<T extends Component>(type: ComponentClass<T>): boolean;
        /** The entity's name; "" when it has none, and assigning "" clears it. Names are never unique. */
        name: string;
        /** The parent entity, or null for a root. Assigning keeps the local Transform, so the world position follows the new parent; a cycle throws. */
        parent: Entity | null;
        /** The direct children, in the order they were attached. */
        children(): Entity[];
    }

    /** The base class of a script: a script module's default export extends it, and the engine makes one instance per entity whose ScriptComponent names the script. */
    class Script {
        protected constructor();
        /** The entity this instance is attached to; already set in field initializers and the constructor body. */
        readonly entity: Entity;
        /** Runs once, before the first onUpdate. */
        onInit?(): void;
        /** Runs every frame; dt is the seconds since the previous frame. */
        onUpdate?(dt: number): void;
        /** Runs once, when the instance is detached or the session stops. */
        onDestroy?(): void;
    }
}
