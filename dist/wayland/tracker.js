"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.ToplevelTracker = exports.FocusArbiter = void 0;
exports.x11BoundsToLogical = x11BoundsToLogical;
const layer_window_1 = require("./layer-window");
// Keyboard focus moving from the game into an overlay surface arrives as two
// independent changes: the game deactivates and the layer surface gains focus.
// Wait this long before treating a deactivation as leaving the game.
const BLUR_GRACE_MS = 100;
/**
 * Converts the X11 bounds of an XWayland window to logical coordinates.
 *
 * X11 pixels are logical pixels unless the compositor lets X11 clients render
 * at native resolution (KWin's "apply scaling themselves", Hyprland's
 * `force_zero_scaling`), where they are physical. A fullscreen target covers an
 * output exactly in one of the two spaces, which identifies that output.
 * Other bounds are assumed to be logical already.
 */
function x11BoundsToLogical(bounds, outputs) {
    const near = (a, b) => Math.abs(a - b) <= 1;
    const covers = (o, scale, withPosition) => near(bounds.width, o.width * scale) && near(bounds.height, o.height * scale) &&
        (!withPosition || (near(bounds.x, o.x * scale) && near(bounds.y, o.y * scale)));
    const output = outputs.find((o) => covers(o, 1, true)) ??
        outputs.find((o) => covers(o, o.scale, true)) ??
        outputs.find((o) => covers(o, 1, false) || covers(o, o.scale, false));
    if (!output)
        return bounds;
    return { x: output.x, y: output.y, width: output.width, height: output.height };
}
/**
 * Turns raw "the game is focused" changes into focus/blur events, treating
 * keyboard focus in one of the overlay's layer surfaces as still being in the
 * game. This matches the X11 backend, where the overlay takes input focus
 * without the window manager deactivating the game.
 */
class FocusArbiter {
    constructor(emit) {
        this.emit = emit;
        this.gameFocused = false;
        this.active = false;
        this.blurTimer = null;
        this.offLayerFocus = (0, layer_window_1.onLayerFocusChange)(() => this.update());
    }
    get isActive() {
        return this.active;
    }
    setGameFocused(focused) {
        this.gameFocused = focused;
        this.update();
    }
    /** Forgets focus without emitting, for attach and detach transitions that
     *  emit their own events. */
    reset(active) {
        this.cancelBlur();
        this.gameFocused = active;
        this.active = active;
    }
    stop() {
        this.cancelBlur();
        this.offLayerFocus();
    }
    update() {
        const wanted = this.gameFocused || (0, layer_window_1.hasFocusedLayerWindow)();
        if (wanted) {
            this.cancelBlur();
            if (!this.active) {
                this.active = true;
                this.emit({ type: 'focus' });
            }
            return;
        }
        if (!this.active || this.blurTimer)
            return;
        this.blurTimer = setTimeout(() => {
            this.blurTimer = null;
            if (this.gameFocused || (0, layer_window_1.hasFocusedLayerWindow)() || !this.active)
                return;
            this.active = false;
            this.emit({ type: 'blur' });
        }, BLUR_GRACE_MS);
    }
    cancelBlur() {
        if (this.blurTimer)
            clearTimeout(this.blurTimer);
        this.blurTimer = null;
    }
}
exports.FocusArbiter = FocusArbiter;
/**
 * Finds the target among the compositor's toplevels
 * (`zwlr_foreign_toplevel_manager_v1`, or the host's KWin script) by exact
 * title. Like the X11 backend, it attaches when a matching window becomes
 * active and keeps following it if its title changes afterwards.
 *
 * The target's bounds are its reported geometry. Foreign-toplevel management
 * reports outputs but no positions, so there the bounds are those of the
 * target's output, which is exact for fullscreen and borderless-fullscreen
 * windows.
 */
class ToplevelTracker {
    constructor(host, titles, emit) {
        this.host = host;
        this.emit = emit;
        this.attached = null;
        this.bounds = null;
        this.fullscreen = false;
        this.titles = titles;
        this.arbiter = new FocusArbiter(emit);
        this.unsubscribe = [
            host.onToplevel(() => this.update()),
            host.onOutputs(() => this.update()),
            host.onRestart(() => this.update())
        ];
        this.update();
    }
    get targetHasFocus() {
        return this.arbiter.isActive;
    }
    setTitles(titles) {
        this.detach();
        this.titles = titles;
        this.update();
    }
    clear() {
        this.detach();
        this.titles = [];
    }
    stop() {
        for (const off of this.unsubscribe)
            off();
        this.arbiter.stop();
    }
    /** Returns keyboard focus to the target. */
    focusTarget() {
        (0, layer_window_1.releaseLayerKeyboards)();
        // Activating while another application is focused would steal focus.
        if (this.attached !== null && this.arbiter.isActive)
            this.host.activateToplevel(this.attached);
    }
    detach() {
        if (this.attached === null)
            return;
        if (this.arbiter.isActive)
            this.emit({ type: 'blur' });
        this.arbiter.reset(false);
        this.attached = null;
        this.bounds = null;
        this.emit({ type: 'detach' });
    }
    outputOf(toplevel) {
        const outputs = this.host.outputs;
        return outputs.find((o) => toplevel.outputs.includes(o.name)) ??
            (outputs.length === 1 ? outputs[0] : null);
    }
    boundsOf(toplevel) {
        const geometry = toplevel.geometry;
        if (geometry && geometry.width > 0 && geometry.height > 0)
            return { ...geometry };
        const output = this.outputOf(toplevel);
        if (output)
            return { x: output.x, y: output.y, width: output.width, height: output.height };
        return this.bounds ?? (this.host.outputs[0] ? { ...this.host.outputs[0] } : null);
    }
    update() {
        const toplevels = this.host.toplevels;
        if (this.attached !== null && !toplevels.has(this.attached))
            this.detach();
        const active = [...toplevels.values()].find((t) => t.activated && this.titles.includes(t.title));
        if (active && active.tid !== this.attached) {
            const bounds = this.boundsOf(active);
            if (!bounds)
                return;
            this.detach();
            this.attached = active.tid;
            this.bounds = bounds;
            this.fullscreen = active.fullscreen;
            this.arbiter.reset(true);
            this.emit({
                type: 'attach',
                hasAccess: undefined,
                isFullscreen: active.fullscreen,
                titleIndex: this.titles.indexOf(active.title),
                ...bounds
            });
            this.emit({ type: 'focus' });
            return;
        }
        const target = this.attached === null ? undefined : toplevels.get(this.attached);
        if (!target)
            return;
        const bounds = this.boundsOf(target);
        if (bounds && JSON.stringify(bounds) !== JSON.stringify(this.bounds)) {
            this.bounds = bounds;
            this.emit({ type: 'moveresize', ...bounds });
        }
        if (target.fullscreen !== this.fullscreen) {
            this.fullscreen = target.fullscreen;
            this.emit({ type: 'fullscreen', isFullscreen: target.fullscreen });
        }
        this.arbiter.setGameFocused(target.activated);
    }
}
exports.ToplevelTracker = ToplevelTracker;
//# sourceMappingURL=tracker.js.map