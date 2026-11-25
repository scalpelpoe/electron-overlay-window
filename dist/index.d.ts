import { EventEmitter } from 'node:events';
import { BrowserWindow, Rectangle, BrowserWindowConstructorOptions, WebPreferences } from 'electron';
import { type NativeBackends } from './wayland/host';
import type { RemoteDesktopStatus } from './wayland/remote-desktop';
import { type NativeInputEvent } from './wayland/native-input';
export { layerShellUsable, nativeBackends, nativeModeUsable, probeWaylandHost, windowTracking, type HostFeatures, type NativeBackends, type WaylandOutput } from './wayland/host';
export type { RemoteDesktopStatus } from './wayland/remote-desktop';
export { adoptLayerWindow, getLayerWindow, getLayerPointer, releaseLayerKeyboards, type LayerWindow, type LayerWindowOptions } from './wayland/layer-window';
export { setLayerShortcutHandler, type LayerShortcutKey } from './wayland/layer-window';
export type { NativeInputEvent } from './wayland/native-input';
export interface AttachEvent {
    hasAccess: boolean | undefined;
    isFullscreen: boolean | undefined;
    titleIndex: number;
    x: number;
    y: number;
    width: number;
    height: number;
}
export interface FullscreenEvent {
    isFullscreen: boolean;
}
export interface MoveresizeEvent {
    x: number;
    y: number;
    width: number;
    height: number;
}
export interface AttachOptions {
    hasTitleBarOnMac?: boolean;
}
export declare const OVERLAY_WINDOW_OPTS: BrowserWindowConstructorOptions;
/** Web preferences a window needs before `adoptLayerWindow` can present it
 *  as a layer-shell surface. Merge into the window's own webPreferences. */
export declare const LAYER_SHELL_WEB_PREFERENCES: WebPreferences;
/**
 * Input, pointer and capture for the target without any X server, for apps
 * using the layer-shell backend. Each member returns null or false where the
 * compositor or permissions do not allow it.
 */
export declare const waylandNative: {
    /** Starts reading evdev (needs read access to /dev/input, usually the
     *  `input` group). See `NativeInputEvent` for what is delivered when. */
    listen(listener: (event: NativeInputEvent) => void): (() => void) | null;
    /** Number of evdev devices being read; 0 usually means no permission. */
    inputDevices(): number;
    /** How tracking, keys, pointer warps and capture are provided by the
     *  running host. All null before the host is ready. */
    backends(): NativeBackends;
    /** Presses or releases an evdev key, delivered to the focused window.
     *  Without a virtual keyboard (KWin) this needs write access to
     *  /dev/uinput, where keys combine with physically held modifiers, or else
     *  an active `waylandRemoteDesktop` session. */
    key(code: number, pressed: boolean): boolean;
    /** Without a virtual pointer this needs an active `waylandRemoteDesktop`
     *  session whose pointer covers the point. */
    warpPointer(point: {
        x: number;
        y: number;
    }): boolean;
    probePointer(): Promise<{
        x: number;
        y: number;
    } | null>;
    canCapture(): boolean;
    /** BGRA pixels of a logical screen rectangle, overlays included. */
    capture(rect: Rectangle): Promise<{
        width: number;
        height: number;
        data: Buffer;
    } | null>;
};
/**
 * Input injection through the xdg-desktop-portal RemoteDesktop interface and
 * libei, for compositors without a virtual keyboard (KWin). `waylandNative.key`
 * and `warpPointer` use the session once `status()` reports `keyboard` or
 * `pointer`. Starting a session shows the portal's approval dialog unless a
 * restore token from an earlier session is accepted; store every token
 * `onRestoreToken` reports, since each is valid once. A restarted host resumes
 * the session with the newest token. Untested on Plasma. Members return null
 * or false before `useLayerShell` started the host.
 */
export declare const waylandRemoteDesktop: {
    /** Whether the host can start a session. False until the host is ready. */
    available(): boolean;
    /** Without `restoreToken`, the newest token this process has seen is used. */
    start(restoreToken?: string): boolean;
    stop(): void;
    status(): RemoteDesktopStatus | null;
    onStatus(listener: (status: RemoteDesktopStatus) => void): (() => void) | null;
    onRestoreToken(listener: (token: string) => void): (() => void) | null;
};
/** Clipboard through the Wayland host's data-control connection. Chromium
 *  only sees the Wayland clipboard while one of its own windows is focused,
 *  which an overlay over a game never is. */
export declare const waylandClipboard: {
    available(): boolean;
    readText(): string;
    readHTML(): string;
    writeText(text: string): void;
    write(data: {
        text?: string;
        html?: string;
    }): void;
    clear(): void;
};
declare class OverlayControllerGlobal {
    private isInitialized;
    private electronWindow?;
    targetBounds: Rectangle;
    targetHasFocus: boolean;
    private focusNext;
    private macTitleBarHeight;
    private attachOptions;
    private layerShell;
    private tracker;
    private x11Arbiter;
    private layerTitles;
    readonly events: EventEmitter<[never]>;
    constructor();
    private handleFullscreen;
    private updateOverlayBounds;
    private handler;
    /** X11 target tracking while the overlay is a layer surface: keyboard focus
     *  in the overlay deactivates the X11 target, which must not hide it. */
    private arbitrateX11Event;
    private emitTrackerEvent;
    /**
     * Presents overlays as wlr-layer-shell surfaces through the Wayland host
     * (see src/lib/wayland/README.md). For Electron running with
     * `--ozone-platform=wayland`; call before `attachByTitle`. Overlay windows
     * must be created with `LAYER_SHELL_WEB_PREFERENCES`. Returns false when the
     * compositor lacks layer-shell v4 or the host is not installed.
     */
    useLayerShell(): boolean;
    get isLayerShell(): boolean;
    /**
     * Create a dummy window to calculate the title bar height on Mac. We use
     * the title bar height to adjust the size of the overlay to not overlap
     * the title bar. This helps Mac match the behaviour on Windows/Linux.
     */
    private calculateMacTitleBarHeight;
    /** If we're on a Mac, adjust the bounds to not overlap the title bar */
    private adjustBoundsForMacTitleBar;
    activateOverlay(): void;
    focusTarget(): void;
    attachByTitle(electronWindow: BrowserWindow | undefined, targetWindowTitle: string, options?: AttachOptions): void;
    attachByTitles(electronWindow: BrowserWindow | undefined, targetWindowTitles: string[], options?: AttachOptions): void;
    private attachLayerShell;
    /** Whether the layer-shell backend has not chosen its tracker yet. */
    private get layerTrackerPending();
    setTargetTitles(titles: string[]): void;
    clearTarget(): void;
    stopHook(): void;
    screenshot(): Buffer;
}
export declare const OverlayController: OverlayControllerGlobal;
