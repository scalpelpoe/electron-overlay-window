/** Availability of the compositor globals the host uses. */
export interface HostFeatures {
    layerShell: boolean;
    layerShellVersion: number;
    foreignToplevel: boolean;
    dataControl: boolean;
    cursorShape: boolean;
    viewporter: boolean;
}
/** An output in global logical coordinates. */
export interface WaylandOutput {
    name: string;
    description: string;
    x: number;
    y: number;
    width: number;
    height: number;
    scale: number;
}
export interface ToplevelInfo {
    tid: number;
    title: string;
    appId: string;
    activated: boolean;
    fullscreen: boolean;
    maximized: boolean;
    minimized: boolean;
    outputs: string[];
}
export interface KeyMods {
    shift: boolean;
    ctrl: boolean;
    alt: boolean;
    meta: boolean;
    caps: boolean;
    num: boolean;
}
export type PointerButton = 'left' | 'right' | 'middle' | 'back' | 'forward';
/** Events addressed to one surface. See src/lib/wayland/README.md. */
export type SurfaceEvent = {
    ev: 'configure';
    sid: number;
    width: number;
    height: number;
} | {
    ev: 'closed';
    sid: number;
} | {
    ev: 'buffers';
    sid: number;
    pool: number;
    path: string;
} | {
    ev: 'release';
    sid: number;
    pool: number;
    index: number;
} | {
    ev: 'pointer';
    sid: number;
    type: 'enter' | 'leave' | 'motion';
    x: number;
    y: number;
} | {
    ev: 'button';
    sid: number;
    button: PointerButton;
    pressed: boolean;
    x: number;
    y: number;
} | {
    ev: 'axis';
    sid: number;
    x: number;
    y: number;
    dx: number;
    dy: number;
    discreteX: number;
    discreteY: number;
} | {
    ev: 'focus';
    sid: number;
    focused: boolean;
} | {
    ev: 'key';
    sid: number;
    pressed: boolean;
    repeat: boolean;
    keysym: string;
    sym: number;
    text: string;
    mods: KeyMods;
};
type SurfaceListener = (event: SurfaceEvent) => void;
export type InputRegion = 'all' | 'none' | ReadonlyArray<{
    x: number;
    y: number;
    width: number;
    height: number;
}>;
export type KeyboardMode = 'none' | 'on_demand' | 'exclusive';
/** Locates the host executable, preferring the asar-unpacked copy that a
 *  packaged Electron app can execute. */
export declare function hostExecutablePath(): string | null;
/** Whether overlays can be presented as layer surfaces, from a probe result. */
export declare function layerShellUsable(features: HostFeatures | null): boolean;
/** Synchronously asks the compositor which globals it offers. Returns null when
 *  the host is missing, no Wayland compositor is reachable, or the probe fails.
 *  Safe to call before Electron's `ready` event, so an app can choose its Ozone
 *  platform from the result. */
export declare function probeWaylandHost(): HostFeatures | null;
/** Client for the `overlay-wayland-host` process. One per app; created by
 *  `WaylandHost.start()`. Restarts the host after a crash and asks surface
 *  owners to resend their state through `onRestart`. */
export declare class WaylandHost {
    private readonly path;
    private static instance;
    features: HostFeatures | null;
    outputs: WaylandOutput[];
    readonly toplevels: Map<number, ToplevelInfo>;
    clipboardText: string | null;
    clipboardHtml: string | null;
    private child;
    private stopped;
    private readonly surfaceListeners;
    private readonly toplevelListeners;
    private readonly outputListeners;
    private readonly restartListeners;
    private readyWaiters;
    private constructor();
    static get(): WaylandHost | null;
    /** Spawns the host once. Returns null when the executable is unavailable. */
    static start(): WaylandHost | null;
    /** Resolves after the host reported its features, or failed to start. */
    ready(): Promise<void>;
    private resolveReady;
    stop(): void;
    private spawn;
    private handleLine;
    private send;
    onSurface(sid: number, listener: SurfaceListener): () => void;
    onToplevel(listener: (info: ToplevelInfo | {
        tid: number;
        closed: true;
    }) => void): () => void;
    onOutputs(listener: () => void): () => void;
    onRestart(listener: () => void): () => void;
    createSurface(sid: number): void;
    destroySurface(sid: number): void;
    setGeometry(sid: number, rect: {
        x: number;
        y: number;
        width: number;
        height: number;
    }): void;
    setKeyboard(sid: number, mode: KeyboardMode): void;
    setInput(sid: number, region: InputRegion): void;
    setLayer(sid: number, layer: 'background' | 'bottom' | 'top' | 'overlay'): void;
    show(sid: number): void;
    hide(sid: number): void;
    raise(sid: number): void;
    setBuffers(sid: number, pool: number, path: string, width: number, height: number): void;
    commit(sid: number, pool: number, index: number, damage: {
        x: number;
        y: number;
        width: number;
        height: number;
    }): void;
    setCursor(sid: number, name: string): void;
    activateToplevel(tid: number): void;
    /** Owns the selection. The local cache is updated immediately so a
     *  following read returns the written value. */
    writeClipboard(text: string, html?: string): void;
    clearClipboard(): void;
    /** The output containing the rectangle's center, else the most overlapped
     *  one. Mirrors the host's placement rule. */
    outputFor(rect: {
        x: number;
        y: number;
        width: number;
        height: number;
    }): WaylandOutput | null;
}
export {};
