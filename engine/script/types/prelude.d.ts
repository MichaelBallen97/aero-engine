// Aero Engine -- the hand-written half of aero-engine.d.ts (task 4.2.2): Vec3, Quat, Entity and Script (the base class
// a script's class extends, task 4.4.1) as engine/script/src/script_api*.cpp installs them, the namespace functions
// (task 4.4.2), the nested namespace input (task 4.4.3), the members an interface adds to a generated class
// (Transform's helpers), and the component types each generated block merges into.
// The build joins this file first, then one reflect-gen --emit-dts block per built-in component header.
// A change to the script API changes this file in the same commit: DT3-DT7 compare it with a live VM.
// Grammar (the drift reader refuses anything else): one declaration per line, doc comments on one line,
// no at-sign anywhere. A class's constructor line is `constructor(...)`, `private constructor(...)` or, for a base
// class a script extends, `protected constructor(...)`; a method may be optional (`name?(...): type;`), a field never.
// A namespace function is `function name(...): type;`; its last parameter may be a rest parameter. An interface named
// like a class adds members to it. A nested `namespace <name> {` block inside the engine namespace holds only one-line
// string-literal unions (`type Name = "a" | "b";`) and `function` lines.

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
        /** Adds v to this vector and returns it. */
        add(v: Vec3): this;
        /** Subtracts v from this vector and returns it. */
        sub(v: Vec3): this;
        /** Multiplies this vector by s and returns it. */
        scale(s: number): this;
        /** Adds v times s to this vector and returns it. */
        addScaled(v: Vec3, s: number): this;
        /** Negates this vector and returns it. */
        negate(): this;
        /** Scales this vector to unit length (a zero vector stays zero) and returns it. */
        normalize(): this;
        /** Moves this vector the fraction t of the way to v (t is not clamped) and returns it. */
        lerp(v: Vec3, t: number): this;
        /** Sets this vector to the cross product this x v and returns it. */
        cross(v: Vec3): this;
        /** Rotates this vector by q (normalized first) and returns it. */
        applyQuat(q: Quat): this;
        /** A new detached Vec3 holding this vector's values. */
        clone(): Vec3;
        /** The dot product with v. */
        dot(v: Vec3): number;
        /** The length. */
        length(): number;
        /** The squared length. */
        lengthSquared(): number;
        /** The distance to v. */
        distanceTo(v: Vec3): number;
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
        /** Sets this quaternion to this * q (q applied first) and returns it. */
        multiply(q: Quat): this;
        /** Sets this quaternion to q * this (q applied last) and returns it. */
        premultiply(q: Quat): this;
        /** Inverts this quaternion (a zero quaternion becomes the identity) and returns it. */
        invert(): this;
        /** Scales this quaternion to unit length (a zero quaternion becomes the identity) and returns it. */
        normalize(): this;
        /** Moves this rotation the fraction t of the way to q along the shorter arc and returns it. */
        slerp(q: Quat, t: number): this;
        /** Sets this quaternion to a rotation of radians about axis (normalized first; a zero axis gives the identity) and returns it. */
        setFromAxisAngle(axis: Vec3, radians: number): this;
        /** Sets this quaternion from Euler angles in radians, the Inspector's order, and returns it. */
        setFromEuler(x: number, y: number, z: number): this;
        /** A new detached Quat holding this quaternion's values. */
        clone(): Quat;
        /** The Euler angles in radians, the Inspector's order, as a new Vec3. */
        toEuler(): Vec3;
        /** The dot product with q. */
        dot(q: Quat): number;
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
        /** The instance of this entity's script, or null; read it again each frame, since a reload replaces it. */
        readonly script: Script | null;
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

    /** The transform helpers every Transform component object carries; the generated block declares its fields. */
    interface Transform {
        /** The entity's position in world space, as a new Vec3. */
        worldPosition(): Vec3;
        /** The entity's -Z axis in world space as a unit vector (zero when its scale collapses it), as a new Vec3. */
        forward(): Vec3;
        /** The entity's +X axis in world space as a unit vector, as a new Vec3. */
        right(): Vec3;
        /** The entity's +Y axis in world space as a unit vector, as a new Vec3. */
        up(): Vec3;
        /** Moves the entity by v along its own axes and returns this Transform. */
        translate(v: Vec3): this;
        /** Turns the entity by q about its own axes and returns this Transform. */
        rotate(q: Quat): this;
        /** Turns the entity so its -Z axis faces target, a world-space point (up defaults to world +Y), and returns this Transform. */
        lookAt(target: Vec3, up?: Vec3): this;
    }

    /** A new root entity holding an identity Transform and no name; it is alive at once, and its script attaches at the next update. */
    function spawn(): Entity;
    /** Queues the entity for destruction with its subtree at the end of the update; false when it is already queued or not alive. */
    function destroy(entity: Entity): boolean;
    /** The live entity with exactly this name and the lowest entity index, or null. */
    function find(name: string): Entity | null;
    /** Every live entity with exactly this name, in ascending entity index. */
    function findAll(name: string): Entity[];
    /** Every live entity holding all of 1 to 8 component classes, in ascending entity index. */
    function query(...types: ComponentClass<Component>[]): Entity[];

    /** The keyboard, the mouse and up to four gamepads, read from the input bound to the VM; "this frame" means since that input's frame began. */
    namespace input {
        /** A physical key, named by its position on a US keyboard: "W" is the same key on every layout. */
        type Key = "A" | "B" | "C" | "D" | "E" | "F" | "G" | "H" | "I" | "J" | "K" | "L" | "M" | "N" | "O" | "P" | "Q" | "R" | "S" | "T" | "U" | "V" | "W" | "X" | "Y" | "Z" | "Num0" | "Num1" | "Num2" | "Num3" | "Num4" | "Num5" | "Num6" | "Num7" | "Num8" | "Num9" | "F1" | "F2" | "F3" | "F4" | "F5" | "F6" | "F7" | "F8" | "F9" | "F10" | "F11" | "F12" | "Escape" | "Enter" | "Tab" | "Space" | "Backspace" | "Delete" | "Insert" | "Home" | "End" | "PageUp" | "PageDown" | "Left" | "Right" | "Up" | "Down" | "LeftShift" | "RightShift" | "LeftCtrl" | "RightCtrl" | "LeftAlt" | "RightAlt" | "LeftGui" | "RightGui" | "CapsLock" | "Minus" | "Equals" | "LeftBracket" | "RightBracket" | "Backslash" | "Semicolon" | "Apostrophe" | "Grave" | "Comma" | "Period" | "Slash" | "Kp0" | "Kp1" | "Kp2" | "Kp3" | "Kp4" | "Kp5" | "Kp6" | "Kp7" | "Kp8" | "Kp9" | "KpEnter" | "KpPlus" | "KpMinus" | "KpMultiply" | "KpDivide" | "KpPeriod" | "PrintScreen" | "ScrollLock" | "Pause";
        /** A mouse button. */
        type MouseButton = "Left" | "Right" | "Middle" | "X1" | "X2";
        /** A gamepad button, named by position: "South" is Xbox A, PlayStation Cross and Switch B. */
        type GamepadButton = "South" | "East" | "West" | "North" | "Back" | "Guide" | "Start" | "LeftStick" | "RightStick" | "LeftShoulder" | "RightShoulder" | "DpadUp" | "DpadDown" | "DpadLeft" | "DpadRight" | "Misc1" | "RightPaddle1" | "LeftPaddle1" | "RightPaddle2" | "LeftPaddle2" | "Touchpad" | "Misc2" | "Misc3" | "Misc4" | "Misc5" | "Misc6";
        /** A gamepad axis: a stick reads -1 to 1 with up and left negative, a trigger 0 to 1. */
        type GamepadAxis = "LeftX" | "LeftY" | "RightX" | "RightY" | "LeftTrigger" | "RightTrigger";
        /** A gamepad stick. */
        type GamepadStick = "Left" | "Right";
        /** Whether the key is held. */
        function keyDown(key: Key): boolean;
        /** Whether the key went down this frame; an auto-repeat is not a press. */
        function keyPressed(key: Key): boolean;
        /** Whether the key went up this frame. */
        function keyReleased(key: Key): boolean;
        /** Whether the mouse button is held. */
        function mouseButtonDown(button: MouseButton): boolean;
        /** Whether the mouse button went down this frame. */
        function mouseButtonPressed(button: MouseButton): boolean;
        /** Whether the mouse button went up this frame. */
        function mouseButtonReleased(button: MouseButton): boolean;
        /** The pointer's position in points from the game view's top-left corner, y down, as a new Vec3 with z 0. */
        function mousePosition(): Vec3;
        /** This frame's summed pointer motion in points, as a new Vec3 with z 0. */
        function mouseDelta(): Vec3;
        /** This frame's summed scroll, y vertical and positive away from the user, as a new Vec3 with z 0. */
        function mouseWheel(): Vec3;
    }
}
