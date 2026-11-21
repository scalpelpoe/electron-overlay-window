"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.WaylandHost = void 0;
exports.hostExecutablePath = hostExecutablePath;
exports.layerShellUsable = layerShellUsable;
exports.probeWaylandHost = probeWaylandHost;
const node_child_process_1 = require("node:child_process");
const node_fs_1 = require("node:fs");
const node_path_1 = require("node:path");
const node_readline_1 = require("node:readline");
const LOG_PREFIX = '[electron-overlay-window] wayland:';
const RESTART_DELAY_MS = 1000;
// Probes give up within about a second in the host; a capture waits for the
// compositor, which may never answer.
const REQUEST_TIMEOUT_MS = 3000;
/** Locates the host executable, preferring the asar-unpacked copy that a
 *  packaged Electron app can execute. */
function hostExecutablePath() {
    const built = (0, node_path_1.join)(__dirname, '..', '..', 'build', 'Release', 'overlay-wayland-host');
    const unpacked = built.replace(`${node_path_1.sep}app.asar${node_path_1.sep}`, `${node_path_1.sep}app.asar.unpacked${node_path_1.sep}`);
    if ((0, node_fs_1.existsSync)(unpacked))
        return unpacked;
    return (0, node_fs_1.existsSync)(built) ? built : null;
}
// Version 4 adds on_demand keyboard interactivity, without which clicking
// into an overlay would not let the user type in it.
const MIN_LAYER_SHELL_VERSION = 4;
/** Whether overlays can be presented as layer surfaces, from a probe result. */
function layerShellUsable(features) {
    return !!features?.layerShell && features.layerShellVersion >= MIN_LAYER_SHELL_VERSION;
}
/** Synchronously asks the compositor which globals it offers. Returns null when
 *  the host is missing, no Wayland compositor is reachable, or the probe fails.
 *  Safe to call before Electron's `ready` event, so an app can choose its Ozone
 *  platform from the result. */
function probeWaylandHost() {
    if (process.platform !== 'linux' || !process.env.WAYLAND_DISPLAY)
        return null;
    const path = hostExecutablePath();
    if (!path)
        return null;
    let stdout;
    try {
        stdout = (0, node_child_process_1.execFileSync)(path, ['--probe'], { encoding: 'utf8', timeout: 3000, stdio: ['ignore', 'pipe', 'ignore'] });
    }
    catch (error) {
        // Exit status 1 still prints the feature line: layer-shell is just missing.
        stdout = String(error.stdout ?? '');
    }
    const line = stdout.split('\n').find((l) => l.startsWith('{'));
    if (!line)
        return null;
    try {
        const event = JSON.parse(line);
        return event.ev === 'ready' ? event : null;
    }
    catch {
        return null;
    }
}
/** Client for the `overlay-wayland-host` process. One per app; created by
 *  `WaylandHost.start()`. Restarts the host after a crash and asks surface
 *  owners to resend their state through `onRestart`. */
class WaylandHost {
    constructor(path) {
        this.path = path;
        this.features = null;
        this.outputs = [];
        this.toplevels = new Map();
        this.clipboardText = null;
        this.clipboardHtml = null;
        this.child = null;
        this.stopped = false;
        this.surfaceListeners = new Map();
        this.toplevelListeners = new Set();
        this.outputListeners = new Set();
        this.restartListeners = new Set();
        this.readyWaiters = [];
        this.evdevListeners = new Set();
        this.pendingCaptures = new Map();
        this.pendingProbes = new Map();
        /** Keys and buttons reported down, released for listeners when the host goes away. */
        this.heldEvdev = new Set();
        this.nextRequest = 1;
        this.evdevDevices = 0;
    }
    static get() {
        return WaylandHost.instance;
    }
    /** Spawns the host once. Returns null when the executable is unavailable. */
    static start() {
        if (WaylandHost.instance)
            return WaylandHost.instance;
        const path = hostExecutablePath();
        if (!path)
            return null;
        const host = new WaylandHost(path);
        host.spawn();
        WaylandHost.instance = host;
        return host;
    }
    /** Resolves after the host reported its features, or failed to start. */
    ready() {
        if (this.features || this.stopped)
            return Promise.resolve();
        return new Promise((resolve) => this.readyWaiters.push(resolve));
    }
    resolveReady() {
        for (const resolve of this.readyWaiters)
            resolve();
        this.readyWaiters = [];
    }
    stop() {
        this.stopped = true;
        this.child?.stdin.end();
        this.child = null;
        this.resolveReady();
        this.abandonRequests();
    }
    /** Settles requests and releases keys the current host process can no
     *  longer answer or report. */
    abandonRequests() {
        for (const resolve of this.pendingCaptures.values())
            resolve(null);
        for (const resolve of this.pendingProbes.values())
            resolve(null);
        this.pendingCaptures.clear();
        this.pendingProbes.clear();
        this.evdevDevices = 0;
        const held = [...this.heldEvdev];
        this.heldEvdev.clear();
        for (const code of held) {
            for (const listener of this.evdevListeners)
                listener({ kind: 'key', code, value: 0 });
        }
    }
    spawn() {
        const child = (0, node_child_process_1.spawn)(this.path, [], { stdio: ['pipe', 'pipe', 'pipe'] });
        this.child = child;
        child.stdin.on('error', () => { });
        child.on('error', (error) => {
            // Spawn failures (for example a missing executable bit) would repeat on
            // every restart, so give up instead of looping.
            console.error(LOG_PREFIX, 'cannot run the host:', error.message);
            if (this.child === child)
                this.child = null;
            this.stopped = true;
            this.resolveReady();
        });
        child.stderr.on('data', (chunk) => {
            for (const line of chunk.toString().split('\n')) {
                if (line)
                    console.warn(LOG_PREFIX, line);
            }
        });
        (0, node_readline_1.createInterface)({ input: child.stdout }).on('line', (line) => this.handleLine(line));
        // A restarted host starts with evdev off.
        if (this.evdevListeners.size > 0)
            this.send('evdev on');
        child.on('exit', (code, signal) => {
            if (this.child !== child)
                return;
            this.child = null;
            if (this.stopped)
                return;
            console.error(LOG_PREFIX, `host exited (${signal ?? code}); restarting`);
            this.toplevels.clear();
            this.abandonRequests();
            setTimeout(() => {
                if (this.stopped)
                    return;
                this.spawn();
                for (const listener of this.restartListeners)
                    listener();
            }, RESTART_DELAY_MS);
        });
    }
    handleLine(line) {
        let event;
        try {
            event = JSON.parse(line);
        }
        catch {
            console.warn(LOG_PREFIX, 'malformed event', line);
            return;
        }
        if ('sid' in event) {
            this.surfaceListeners.get(event.sid)?.(event);
            return;
        }
        switch (event.ev) {
            case 'ready': {
                const { ev: _, ...features } = event;
                this.features = features;
                this.resolveReady();
                break;
            }
            case 'outputs':
                this.outputs = event.outputs;
                for (const listener of this.outputListeners)
                    listener();
                break;
            case 'toplevel': {
                const { ev: _, ...info } = event;
                this.toplevels.set(info.tid, info);
                for (const listener of this.toplevelListeners)
                    listener(info);
                break;
            }
            case 'toplevel-closed':
                this.toplevels.delete(event.tid);
                for (const listener of this.toplevelListeners)
                    listener({ tid: event.tid, closed: true });
                break;
            case 'clipboard':
                this.clipboardText = event.text;
                this.clipboardHtml = event.html;
                break;
            case 'error':
                console.error(LOG_PREFIX, event.message);
                break;
            case 'evdev':
                this.evdevDevices = event.devices;
                break;
            case 'input': {
                const { ev: _, ...input } = event;
                if (input.kind === 'key' && input.value === 1)
                    this.heldEvdev.add(input.code);
                else if (input.kind === 'key' && input.value === 0)
                    this.heldEvdev.delete(input.code);
                for (const listener of this.evdevListeners)
                    listener(input);
                break;
            }
            case 'capture': {
                const resolve = this.pendingCaptures.get(event.id);
                this.pendingCaptures.delete(event.id);
                if ('error' in event)
                    console.warn(LOG_PREFIX, 'capture failed:', event.error);
                resolve?.('error' in event ? null : { width: event.width, height: event.height });
                break;
            }
            case 'probe': {
                const resolve = this.pendingProbes.get(event.id);
                this.pendingProbes.delete(event.id);
                resolve?.(event.x === null || event.y === null ? null : { x: event.x, y: event.y });
                break;
            }
        }
    }
    /** Returns false when there is no host to send to. */
    send(line) {
        if (!this.child || this.child.stdin.destroyed)
            return false;
        this.child.stdin.write(line + '\n');
        return true;
    }
    request(pending, id, line) {
        return new Promise((resolve) => {
            pending.set(id, resolve);
            const timer = setTimeout(() => {
                if (pending.get(id) !== resolve)
                    return;
                pending.delete(id);
                resolve(null);
            }, REQUEST_TIMEOUT_MS);
            timer.unref?.();
            if (this.send(line))
                return;
            pending.delete(id);
            resolve(null);
        });
    }
    onSurface(sid, listener) {
        this.surfaceListeners.set(sid, listener);
        return () => this.surfaceListeners.delete(sid);
    }
    onToplevel(listener) {
        this.toplevelListeners.add(listener);
        return () => this.toplevelListeners.delete(listener);
    }
    onOutputs(listener) {
        this.outputListeners.add(listener);
        return () => this.outputListeners.delete(listener);
    }
    onRestart(listener) {
        this.restartListeners.add(listener);
        return () => this.restartListeners.delete(listener);
    }
    createSurface(sid) {
        this.send(`create ${sid}`);
    }
    destroySurface(sid) {
        this.send(`destroy ${sid}`);
    }
    setGeometry(sid, rect) {
        const r = [rect.x, rect.y, Math.max(1, rect.width), Math.max(1, rect.height)].map(Math.round);
        this.send(`geometry ${sid} ${r.join(' ')}`);
    }
    setKeyboard(sid, mode) {
        this.send(`keyboard ${sid} ${mode}`);
    }
    setInput(sid, region) {
        if (typeof region === 'string') {
            this.send(`input ${sid} ${region}`);
            return;
        }
        const rects = region
            .map((r) => [r.x, r.y, r.width, r.height].map(Math.round))
            .filter(([, , w, h]) => w > 0 && h > 0);
        this.send(rects.length ? `input ${sid} ${rects.map((r) => r.join(',')).join(' ')}` : `input ${sid} none`);
    }
    setLayer(sid, layer) {
        this.send(`layer ${sid} ${layer}`);
    }
    show(sid) {
        this.send(`show ${sid}`);
    }
    hide(sid) {
        this.send(`hide ${sid}`);
    }
    raise(sid) {
        this.send(`raise ${sid}`);
    }
    setBuffers(sid, pool, path, width, height) {
        this.send(`buffers ${sid} ${pool} ${path} ${width} ${height}`);
    }
    commit(sid, pool, index, damage) {
        this.send(`commit ${sid} ${pool} ${index} ${damage.x} ${damage.y} ${damage.width} ${damage.height}`);
    }
    setCursor(sid, name) {
        this.send(`cursor ${sid} ${/^[a-z-]+$/.test(name) ? name : 'default'}`);
    }
    activateToplevel(tid) {
        this.send(`activate ${tid}`);
    }
    /** Owns the selection. The local cache is updated immediately so a
     *  following read returns the written value. */
    writeClipboard(text, html) {
        this.clipboardText = text;
        this.clipboardHtml = html ?? null;
        const encode = (value) => Buffer.from(value, 'utf8').toString('base64');
        this.send(`clipboard-set ${encode(text)} ${html ? encode(html) : '-'}`);
    }
    /** Reads keyboards and mice from evdev while there are listeners. */
    onEvdev(listener) {
        this.evdevListeners.add(listener);
        if (this.evdevListeners.size === 1)
            this.send('evdev on');
        return () => {
            this.evdevListeners.delete(listener);
            if (this.evdevListeners.size === 0)
                this.send('evdev off');
        };
    }
    /** Presses or releases an evdev key on the host's virtual keyboard. */
    virtualKey(code, pressed) {
        this.send(`vkey ${code} ${pressed ? 1 : 0}`);
    }
    warpPointer(x, y) {
        this.send(`warp ${Math.round(x)} ${Math.round(y)}`);
    }
    /** Captures a logical rectangle into `path` as BGRA. Resolves to the pixel
     *  size, or null on failure. */
    capture(rect, path) {
        const id = `c${this.nextRequest++}`;
        const r = [rect.x, rect.y, rect.width, rect.height].map(Math.round);
        // The path ends the command line.
        if (/[\r\n]/.test(path))
            return Promise.resolve(null);
        return this.request(this.pendingCaptures, id, `capture ${id} ${r.join(' ')} ${path}`);
    }
    /** Global logical pointer position, found by briefly mapping input
     *  surfaces over every output. */
    probePointer() {
        const id = `p${this.nextRequest++}`;
        return this.request(this.pendingProbes, id, `probe ${id}`);
    }
    clearClipboard() {
        this.clipboardText = null;
        this.clipboardHtml = null;
        this.send('clipboard-clear');
    }
    /** The output containing the rectangle's center, else the most overlapped
     *  one. Mirrors the host's placement rule. */
    outputFor(rect) {
        const cx = rect.x + rect.width / 2;
        const cy = rect.y + rect.height / 2;
        const containing = this.outputs.find((o) => cx >= o.x && cx < o.x + o.width && cy >= o.y && cy < o.y + o.height);
        if (containing)
            return containing;
        let best = null;
        let bestArea = 0;
        for (const o of this.outputs) {
            const w = Math.min(rect.x + rect.width, o.x + o.width) - Math.max(rect.x, o.x);
            const h = Math.min(rect.y + rect.height, o.y + o.height) - Math.max(rect.y, o.y);
            if (w > 0 && h > 0 && w * h > bestArea) {
                best = o;
                bestArea = w * h;
            }
        }
        return best ?? this.outputs[0] ?? null;
    }
}
exports.WaylandHost = WaylandHost;
WaylandHost.instance = null;
//# sourceMappingURL=host.js.map