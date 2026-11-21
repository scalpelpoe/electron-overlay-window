"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.OverlayController = exports.waylandClipboard = exports.waylandNative = exports.LAYER_SHELL_WEB_PREFERENCES = exports.OVERLAY_WINDOW_OPTS = exports.setLayerShortcutHandler = exports.releaseLayerKeyboards = exports.getLayerPointer = exports.getLayerWindow = exports.adoptLayerWindow = exports.probeWaylandHost = exports.layerShellUsable = void 0;
const node_events_1 = require("node:events");
const node_path_1 = require("node:path");
const throttle_debounce_1 = require("throttle-debounce");
const electron_1 = require("electron");
const electron_2 = require("electron");
const host_1 = require("./wayland/host");
const layer_window_1 = require("./wayland/layer-window");
const tracker_1 = require("./wayland/tracker");
const native_input_1 = require("./wayland/native-input");
const lib = require('node-gyp-build')((0, node_path_1.join)(__dirname, '..'));
var host_2 = require("./wayland/host");
Object.defineProperty(exports, "layerShellUsable", { enumerable: true, get: function () { return host_2.layerShellUsable; } });
Object.defineProperty(exports, "probeWaylandHost", { enumerable: true, get: function () { return host_2.probeWaylandHost; } });
var layer_window_2 = require("./wayland/layer-window");
Object.defineProperty(exports, "adoptLayerWindow", { enumerable: true, get: function () { return layer_window_2.adoptLayerWindow; } });
Object.defineProperty(exports, "getLayerWindow", { enumerable: true, get: function () { return layer_window_2.getLayerWindow; } });
Object.defineProperty(exports, "getLayerPointer", { enumerable: true, get: function () { return layer_window_2.getLayerPointer; } });
Object.defineProperty(exports, "releaseLayerKeyboards", { enumerable: true, get: function () { return layer_window_2.releaseLayerKeyboards; } });
var layer_window_3 = require("./wayland/layer-window");
Object.defineProperty(exports, "setLayerShortcutHandler", { enumerable: true, get: function () { return layer_window_3.setLayerShortcutHandler; } });
var EventType;
(function (EventType) {
    EventType[EventType["EVENT_ATTACH"] = 1] = "EVENT_ATTACH";
    EventType[EventType["EVENT_FOCUS"] = 2] = "EVENT_FOCUS";
    EventType[EventType["EVENT_BLUR"] = 3] = "EVENT_BLUR";
    EventType[EventType["EVENT_DETACH"] = 4] = "EVENT_DETACH";
    EventType[EventType["EVENT_FULLSCREEN"] = 5] = "EVENT_FULLSCREEN";
    EventType[EventType["EVENT_MOVERESIZE"] = 6] = "EVENT_MOVERESIZE";
})(EventType || (EventType = {}));
const isMac = process.platform === 'darwin';
const isLinux = process.platform === 'linux';
exports.OVERLAY_WINDOW_OPTS = {
    fullscreenable: true,
    skipTaskbar: true,
    frame: false,
    show: false,
    transparent: true,
    resizable: true,
    // disable shadow for Mac OS
    hasShadow: !isMac,
    // float above all windows on Mac OS
    alwaysOnTop: isMac
};
/** Web preferences a window needs before `adoptLayerWindow` can present it
 *  as a layer-shell surface. Merge into the window's own webPreferences. */
exports.LAYER_SHELL_WEB_PREFERENCES = {
    offscreen: true,
    // Hidden offscreen windows would otherwise throttle timers and rAF.
    backgroundThrottling: false
};
/**
 * Input, pointer and capture for the target without any X server, for apps
 * using the layer-shell backend. Each member returns null or false where the
 * compositor or permissions do not allow it.
 */
exports.waylandNative = {
    /** Starts reading evdev (needs read access to /dev/input, usually the
     *  `input` group). See `NativeInputEvent` for what is delivered when. */
    listen(listener) {
        const host = host_1.WaylandHost.get();
        if (!host)
            return null;
        return (0, native_input_1.listenNativeInput)(host, () => exports.OverlayController.targetHasFocus, listener);
    },
    /** Number of evdev devices being read; 0 usually means no permission. */
    inputDevices() {
        return host_1.WaylandHost.get()?.evdevDevices ?? 0;
    },
    /** Presses or releases an evdev key, delivered to the focused window. */
    key(code, pressed) {
        const host = host_1.WaylandHost.get();
        if (!host?.features?.virtualKeyboard)
            return false;
        host.virtualKey(code, pressed);
        return true;
    },
    warpPointer(point) {
        const host = host_1.WaylandHost.get();
        if (!host?.features?.virtualPointer)
            return false;
        host.warpPointer(point.x, point.y);
        return true;
    },
    probePointer() {
        return host_1.WaylandHost.get()?.probePointer() ?? Promise.resolve(null);
    },
    canCapture() {
        return !!host_1.WaylandHost.get()?.features?.screencopy;
    },
    /** BGRA pixels of a logical screen rectangle, overlays included. */
    capture(rect) {
        const host = host_1.WaylandHost.get();
        return host?.features?.screencopy ? (0, native_input_1.captureScreenRegion)(host, rect) : Promise.resolve(null);
    }
};
/** Clipboard through the Wayland host's data-control connection. Chromium
 *  only sees the Wayland clipboard while one of its own windows is focused,
 *  which an overlay over a game never is. */
exports.waylandClipboard = {
    available() {
        return !!host_1.WaylandHost.get()?.features?.dataControl;
    },
    readText() {
        return host_1.WaylandHost.get()?.clipboardText ?? '';
    },
    readHTML() {
        return host_1.WaylandHost.get()?.clipboardHtml ?? '';
    },
    writeText(text) {
        host_1.WaylandHost.get()?.writeClipboard(text);
    },
    write(data) {
        host_1.WaylandHost.get()?.writeClipboard(data.text ?? '', data.html);
    },
    clear() {
        host_1.WaylandHost.get()?.clearClipboard();
    }
};
class OverlayControllerGlobal {
    constructor() {
        this.isInitialized = false;
        // Exposed so that apps can get the current bounds of the target
        // NOTE: stores screen physical rect on Windows and XWayland
        this.targetBounds = { x: 0, y: 0, width: 0, height: 0 };
        this.targetHasFocus = false;
        // The height of a title bar on a standard window. Only measured on Mac
        this.macTitleBarHeight = 0;
        this.attachOptions = {};
        this.layerShell = false;
        this.tracker = null;
        this.x11Arbiter = null;
        // Target titles until the layer-shell tracker starts.
        this.layerTitles = [];
        this.events = new node_events_1.EventEmitter();
        this.events.on('attach', (e) => {
            this.targetHasFocus = true;
            if (this.electronWindow) {
                this.electronWindow.setIgnoreMouseEvents(true);
                this.electronWindow.showInactive();
                this.electronWindow.setAlwaysOnTop(true, 'screen-saver');
            }
            if (e.isFullscreen !== undefined) {
                this.handleFullscreen(e.isFullscreen);
            }
            this.targetBounds = e;
            this.updateOverlayBounds();
        });
        this.events.on('fullscreen', (e) => {
            this.handleFullscreen(e.isFullscreen);
        });
        this.events.on('detach', () => {
            this.targetHasFocus = false;
            this.electronWindow?.hide();
        });
        const dispatchMoveresize = (0, throttle_debounce_1.throttle)(34 /* 30fps */, this.updateOverlayBounds.bind(this));
        this.events.on('moveresize', (e) => {
            this.targetBounds = e;
            dispatchMoveresize();
        });
        this.events.on('blur', () => {
            this.targetHasFocus = false;
            if (this.electronWindow && (isMac ||
                this.focusNext !== 'overlay' && !this.electronWindow.isFocused())) {
                this.electronWindow.hide();
            }
        });
        this.events.on('focus', () => {
            this.focusNext = undefined;
            this.targetHasFocus = true;
            if (this.electronWindow) {
                this.electronWindow.setIgnoreMouseEvents(true);
                if (!this.electronWindow.isVisible()) {
                    this.electronWindow.showInactive();
                    this.electronWindow.setAlwaysOnTop(true, 'screen-saver');
                }
            }
        });
    }
    async handleFullscreen(isFullscreen) {
        if (!this.electronWindow)
            return;
        if (isMac) {
            // On Mac, only a single app can be fullscreen, so we can't go
            // fullscreen. We get around it by making it display on all workspaces,
            // based on code from:
            // https://github.com/electron/electron/issues/10078#issuecomment-754105005
            this.electronWindow.setVisibleOnAllWorkspaces(isFullscreen, { visibleOnFullScreen: true });
            if (isFullscreen) {
                const display = electron_1.screen.getPrimaryDisplay();
                this.electronWindow.setBounds(display.bounds);
            }
            else {
                // Set it back to `lastBounds` as set before fullscreen
                this.updateOverlayBounds();
            }
        }
    }
    updateOverlayBounds() {
        let lastBounds = this.adjustBoundsForMacTitleBar(this.targetBounds);
        if (lastBounds.width === 0 || lastBounds.height === 0)
            return;
        if (!this.electronWindow)
            return;
        if (this.layerShell) {
            // Layer-shell targets are tracked in logical coordinates already.
        }
        else if (process.platform === 'win32') {
            lastBounds = electron_1.screen.screenToDipRect(this.electronWindow, this.targetBounds);
        }
        else if (isLinux) {
            // XWayland reports physical pixels; convert to DIP (logical) for Electron
            const display = electron_1.screen.getDisplayNearestPoint({ x: lastBounds.x, y: lastBounds.y });
            const scaleFactor = display.scaleFactor || 1;
            lastBounds = {
                x: Math.round(lastBounds.x / scaleFactor),
                y: Math.round(lastBounds.y / scaleFactor),
                width: Math.round(lastBounds.width / scaleFactor),
                height: Math.round(lastBounds.height / scaleFactor)
            };
        }
        this.electronWindow.setBounds(lastBounds);
        // if moved to screen with different DPI, 2nd call to setBounds will correctly resize window
        // dipRect must be recalculated as well
        if (process.platform === 'win32') {
            lastBounds = electron_1.screen.screenToDipRect(this.electronWindow, this.targetBounds);
            this.electronWindow.setBounds(lastBounds);
        }
    }
    handler(e) {
        if (this.x11Arbiter) {
            this.arbitrateX11Event(e, this.x11Arbiter);
            return;
        }
        try {
            switch (e.type) {
                case EventType.EVENT_ATTACH:
                    this.events.emit('attach', e);
                    break;
                case EventType.EVENT_FOCUS:
                    this.events.emit('focus', e);
                    break;
                case EventType.EVENT_BLUR:
                    this.events.emit('blur', e);
                    break;
                case EventType.EVENT_DETACH:
                    this.events.emit('detach', e);
                    break;
                case EventType.EVENT_FULLSCREEN:
                    this.events.emit('fullscreen', e);
                    break;
                case EventType.EVENT_MOVERESIZE:
                    this.events.emit('moveresize', e);
                    break;
            }
        }
        catch (err) {
            console.error('[electron-overlay-window] handler error:', err);
        }
    }
    /** X11 target tracking while the overlay is a layer surface: keyboard focus
     *  in the overlay deactivates the X11 target, which must not hide it. */
    arbitrateX11Event(e, arbiter) {
        const logical = (event) => ({ ...event, ...(0, tracker_1.x11BoundsToLogical)(event, host_1.WaylandHost.get()?.outputs ?? []) });
        switch (e.type) {
            case EventType.EVENT_ATTACH:
                arbiter.reset(false);
                this.events.emit('attach', logical(e));
                break;
            case EventType.EVENT_DETACH:
                arbiter.reset(false);
                this.events.emit('detach', e);
                break;
            case EventType.EVENT_FOCUS:
                arbiter.setGameFocused(true);
                break;
            case EventType.EVENT_BLUR:
                arbiter.setGameFocused(false);
                break;
            case EventType.EVENT_FULLSCREEN:
                this.events.emit('fullscreen', e);
                break;
            case EventType.EVENT_MOVERESIZE:
                this.events.emit('moveresize', logical(e));
                break;
        }
    }
    emitTrackerEvent(event) {
        const { type, ...payload } = event;
        try {
            this.events.emit(type, payload);
        }
        catch (err) {
            console.error('[electron-overlay-window] handler error:', err);
        }
    }
    /**
     * Presents overlays as wlr-layer-shell surfaces through the Wayland host
     * (see src/lib/wayland/README.md). For Electron running with
     * `--ozone-platform=wayland`; call before `attachByTitle`. Overlay windows
     * must be created with `LAYER_SHELL_WEB_PREFERENCES`. Returns false when the
     * compositor lacks layer-shell v4 or the host is not installed.
     */
    useLayerShell() {
        if (this.isInitialized)
            throw new Error('Call useLayerShell before attaching.');
        if (!isLinux || !(0, host_1.layerShellUsable)((0, host_1.probeWaylandHost)()))
            return false;
        this.layerShell = host_1.WaylandHost.start() !== null;
        return this.layerShell;
    }
    get isLayerShell() {
        return this.layerShell;
    }
    /**
     * Create a dummy window to calculate the title bar height on Mac. We use
     * the title bar height to adjust the size of the overlay to not overlap
     * the title bar. This helps Mac match the behaviour on Windows/Linux.
     */
    calculateMacTitleBarHeight() {
        const testWindow = new electron_2.BrowserWindow({
            width: 400,
            height: 300,
            webPreferences: {
                nodeIntegration: true
            },
            show: false,
        });
        const fullHeight = testWindow.getSize()[1];
        const contentHeight = testWindow.getContentSize()[1];
        this.macTitleBarHeight = fullHeight - contentHeight;
        testWindow.close();
    }
    /** If we're on a Mac, adjust the bounds to not overlap the title bar */
    adjustBoundsForMacTitleBar(bounds) {
        if (!isMac || !this.attachOptions.hasTitleBarOnMac) {
            return bounds;
        }
        const newBounds = {
            ...bounds,
            y: bounds.y + this.macTitleBarHeight,
            height: bounds.height - this.macTitleBarHeight
        };
        return newBounds;
    }
    activateOverlay() {
        if (!this.electronWindow) {
            throw new Error('You are using the library in tracking mode');
        }
        this.focusNext = 'overlay';
        this.electronWindow.setIgnoreMouseEvents(false);
        if (this.layerShell) {
            this.electronWindow.focus();
        }
        else if (isLinux) {
            lib.activateOverlay();
        }
        else {
            this.electronWindow.focus();
        }
    }
    focusTarget() {
        this.focusNext = 'target';
        this.electronWindow?.setIgnoreMouseEvents(true);
        if (this.layerShell) {
            (0, layer_window_1.releaseLayerKeyboards)();
            this.tracker?.focusTarget();
            return;
        }
        lib.focusTarget();
    }
    attachByTitle(electronWindow, targetWindowTitle, options = {}) {
        this.attachByTitles(electronWindow, [targetWindowTitle], options);
    }
    attachByTitles(electronWindow, targetWindowTitles, options = {}) {
        if (this.isInitialized) {
            throw new Error('Library can be initialized only once.');
        }
        else {
            this.isInitialized = true;
        }
        this.electronWindow = electronWindow;
        this.electronWindow?.on('blur', () => {
            if (!this.targetHasFocus && this.focusNext !== 'target') {
                this.electronWindow.hide();
            }
        });
        this.electronWindow?.on('focus', () => {
            this.focusNext = undefined;
        });
        this.attachOptions = options;
        if (isMac) {
            this.calculateMacTitleBarHeight();
        }
        if (this.layerShell) {
            this.attachLayerShell(targetWindowTitles);
            return;
        }
        lib.start(this.electronWindow?.getNativeWindowHandle(), targetWindowTitles, this.handler.bind(this));
    }
    attachLayerShell(titles) {
        const host = host_1.WaylandHost.get();
        if (this.electronWindow)
            (0, layer_window_1.adoptLayerWindow)(this.electronWindow);
        this.layerTitles = titles;
        void host.ready().then(() => {
            titles = this.layerTitles;
            if (!host.features) {
                console.error('[electron-overlay-window] the Wayland host did not start; the target is not tracked');
                return;
            }
            if (host.features.foreignToplevel) {
                this.tracker = new tracker_1.ToplevelTracker(host, titles, (event) => this.emitTrackerEvent(event));
                return;
            }
            // KWin and others without foreign-toplevel management: follow an
            // XWayland target through the X11 backend in tracking-only mode.
            this.x11Arbiter = new tracker_1.FocusArbiter((event) => this.emitTrackerEvent(event));
            lib.start(undefined, titles, this.handler.bind(this));
        });
    }
    /** Whether the layer-shell backend has not chosen its tracker yet. */
    get layerTrackerPending() {
        return this.layerShell && !this.tracker && !this.x11Arbiter;
    }
    setTargetTitles(titles) {
        if (this.layerTrackerPending)
            this.layerTitles = titles;
        else if (this.tracker)
            this.tracker.setTitles(titles);
        else
            lib.setTargetTitles(titles);
    }
    clearTarget() {
        if (this.layerTrackerPending)
            this.layerTitles = [];
        else if (this.tracker)
            this.tracker.clear();
        else
            lib.clearTarget();
    }
    stopHook() {
        if (this.tracker) {
            this.tracker.stop();
            this.tracker = null;
            return;
        }
        this.x11Arbiter?.stop();
        lib.stopHook();
    }
    // buffer suitable for use in `nativeImage.createFromBitmap`
    screenshot() {
        if (process.platform !== 'win32') {
            throw new Error('Not implemented on your platform.');
        }
        return lib.screenshot();
    }
}
exports.OverlayController = new OverlayControllerGlobal();
//# sourceMappingURL=index.js.map