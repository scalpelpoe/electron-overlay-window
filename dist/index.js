"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.OverlayController = exports.OVERLAY_WINDOW_OPTS = void 0;
const node_events_1 = require("node:events");
const node_path_1 = require("node:path");
const throttle_debounce_1 = require("throttle-debounce");
const electron_1 = require("electron");
const electron_2 = require("electron");
const lib = require('node-gyp-build')((0, node_path_1.join)(__dirname, '..'));
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
    skipTaskbar: !isLinux,
    frame: false,
    show: false,
    transparent: true,
    // let Chromium to accept any size changes from OS
    resizable: !isLinux,
    // disable shadow for Mac OS
    hasShadow: !isMac,
    // float above all windows on Mac OS
    alwaysOnTop: isMac
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
        if (process.platform === 'win32') {
            lastBounds = electron_1.screen.screenToDipRect(this.electronWindow, this.targetBounds);
        }
        else if (isLinux) {
            // The `xcb_get_geometry` can receive physical coords under KDE's XWayland.
            // see https://github.com/SnosMe/electron-overlay-window/pull/50
            // The following code should be a no-op on native X11.
            const tl = electron_1.screen.screenToDipPoint({ x: lastBounds.x, y: lastBounds.y });
            // const br = screen.screenToDipPoint({ x: lastBounds.x + lastBounds.width, y: lastBounds.y + lastBounds.height })
            // lastBounds = { x: tl.x, y: tl.y, width: br.x - tl.x, height: br.y - tl.y }
            const logicalSize = electron_1.screen.screenToDipPoint({ x: lastBounds.width, y: lastBounds.height });
            lastBounds = { x: tl.x, y: tl.y, width: logicalSize.x, height: logicalSize.y };
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
        if (isLinux) {
            lib.activateOverlay();
        }
        else {
            this.electronWindow.focus();
        }
    }
    focusTarget() {
        this.focusNext = 'target';
        this.electronWindow?.setIgnoreMouseEvents(true);
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
        lib.start(this.electronWindow?.getNativeWindowHandle(), targetWindowTitles, this.handler.bind(this));
    }
    setTargetTitles(titles) {
        lib.setTargetTitles(titles);
    }
    clearTarget() {
        lib.clearTarget();
    }
    stopHook() {
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