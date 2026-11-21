"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.listenNativeInput = listenNativeInput;
exports.captureScreenRegion = captureScreenRegion;
const node_fs_1 = require("node:fs");
const node_os_1 = require("node:os");
const node_path_1 = require("node:path");
const layer_window_1 = require("./layer-window");
// BTN_MISC up to KEY_OK: mouse, joystick and gamepad buttons.
const BUTTON_FIRST = 0x100;
const BUTTON_LAST = 0x15f;
function isButton(code) {
    return code >= BUTTON_FIRST && code <= BUTTON_LAST;
}
/**
 * Delivers evdev input while it reaches the target, mirroring what a hook
 * inside the target's display server would see: presses only while the target
 * has keyboard focus and, for mouse input, while the pointer is not over an
 * overlay surface. Releases are always delivered so held-key tracking cannot
 * get stuck.
 */
function listenNativeInput(host, targetActive, listener) {
    return host.onEvdev((event) => {
        if (event.kind === 'wheel') {
            if (targetActive() && !(0, layer_window_1.getLayerPointer)())
                listener({ type: 'wheel', delta: event.value, horizontal: event.horizontal });
            return;
        }
        const pressed = event.value !== 0;
        if (isButton(event.code)) {
            if (event.value === 2)
                return;
            if (!pressed)
                listener({ type: 'mouseup', code: event.code });
            else if (targetActive() && !(0, layer_window_1.getLayerPointer)())
                listener({ type: 'mousedown', code: event.code });
            return;
        }
        if (!pressed)
            listener({ type: 'keyup', code: event.code, repeat: false });
        else if (targetActive() && !(0, layer_window_1.hasFocusedLayerWindow)())
            listener({ type: 'keydown', code: event.code, repeat: event.value === 2 });
    });
}
let nextCapture = 1;
/** Captures a logical rectangle of the screen as BGRA pixels. */
async function captureScreenRegion(host, rect) {
    // The pool prefix lets the stale-file sweep remove captures of killed processes.
    const path = (0, node_path_1.join)(process.env.XDG_RUNTIME_DIR || (0, node_os_1.tmpdir)(), `electron-overlay-window-${process.pid}-capture-${nextCapture++}`);
    try {
        const size = await host.capture(rect, path);
        if (!size)
            return null;
        const data = (0, node_fs_1.readFileSync)(path);
        return data.length === size.width * size.height * 4 ? { ...size, data } : null;
    }
    catch {
        return null;
    }
    finally {
        try {
            (0, node_fs_1.unlinkSync)(path);
        }
        catch { }
    }
}
//# sourceMappingURL=native-input.js.map