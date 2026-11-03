export default class Mover extends engine.Script {
    position = new engine.Vec3();
    held = false;
    pads = 0;
    onUpdate(dt: number): void {
        const key: engine.input.Key = "W";
        const button: engine.input.MouseButton = "Left";
        const padButton: engine.input.GamepadButton = "South";
        const axis: engine.input.GamepadAxis = "LeftX";
        const stick: engine.input.GamepadStick = "Left";
        this.held = engine.input.keyDown(key) || engine.input.keyPressed("Space") || engine.input.keyReleased("Escape");
        this.held = this.held || engine.input.mouseButtonDown(button) || engine.input.mouseButtonPressed("Right");
        this.held = this.held || engine.input.mouseButtonReleased("Middle");
        const where: engine.Vec3 = engine.input.mousePosition();
        const moved: engine.Vec3 = engine.input.mouseDelta();
        const scrolled: engine.Vec3 = engine.input.mouseWheel();
        this.position.x += (where.x + moved.x + scrolled.y) * 0;
        this.pads = 0;
        for (let slot = 0; slot < 4; slot++) {
            if (engine.input.gamepadConnected(slot)) {
                this.pads++;
            }
        }
        this.held = this.held || engine.input.gamepadConnected();
        this.held = this.held || engine.input.gamepadButtonDown(padButton) || engine.input.gamepadButtonDown("East", 1);
        this.held = this.held || engine.input.gamepadButtonPressed("North", 2) || engine.input.gamepadButtonPressed(padButton);
        this.held = this.held || engine.input.gamepadButtonReleased("DpadUp", 3) || engine.input.gamepadButtonReleased(padButton);
        const tilt: number = engine.input.gamepadAxis(axis) + engine.input.gamepadAxis("RightTrigger", 1);
        const aim: engine.Vec3 = engine.input.gamepadStick(stick);
        const aimRight: engine.Vec3 = engine.input.gamepadStick("Right", 1);
        this.position.x += (aim.x + aimRight.x) * dt;
        this.position.y += (aim.y + aimRight.y + tilt) * dt;
    }
}
