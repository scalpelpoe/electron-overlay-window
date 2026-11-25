import type { WaylandHost, WaylandOutput } from './host';
export interface Bounds {
    x: number;
    y: number;
    width: number;
    height: number;
}
export type TrackerEvent = {
    type: 'attach';
    titleIndex: number;
    isFullscreen: boolean | undefined;
    hasAccess: undefined;
} & Bounds | {
    type: 'focus';
} | {
    type: 'blur';
} | {
    type: 'detach';
} | {
    type: 'fullscreen';
    isFullscreen: boolean;
} | {
    type: 'moveresize';
} & Bounds;
type Emit = (event: TrackerEvent) => void;
/**
 * Converts the X11 bounds of an XWayland window to logical coordinates.
 *
 * X11 pixels are logical pixels unless the compositor lets X11 clients render
 * at native resolution (KWin's "apply scaling themselves", Hyprland's
 * `force_zero_scaling`), where they are physical. A fullscreen target covers an
 * output exactly in one of the two spaces, which identifies that output.
 * Other bounds are assumed to be logical already.
 */
export declare function x11BoundsToLogical(bounds: Bounds, outputs: readonly WaylandOutput[]): Bounds;
/**
 * Turns raw "the game is focused" changes into focus/blur events, treating
 * keyboard focus in one of the overlay's layer surfaces as still being in the
 * game. This matches the X11 backend, where the overlay takes input focus
 * without the window manager deactivating the game.
 */
export declare class FocusArbiter {
    private readonly emit;
    private gameFocused;
    private active;
    private blurTimer;
    private readonly offLayerFocus;
    constructor(emit: Emit);
    get isActive(): boolean;
    setGameFocused(focused: boolean): void;
    /** Forgets focus without emitting, for attach and detach transitions that
     *  emit their own events. */
    reset(active: boolean): void;
    stop(): void;
    private update;
    private cancelBlur;
}
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
export declare class ToplevelTracker {
    private readonly host;
    private readonly emit;
    private titles;
    private attached;
    private bounds;
    private fullscreen;
    private readonly arbiter;
    private readonly unsubscribe;
    constructor(host: WaylandHost, titles: string[], emit: Emit);
    get targetHasFocus(): boolean;
    setTitles(titles: string[]): void;
    clear(): void;
    stop(): void;
    /** Returns keyboard focus to the target. */
    focusTarget(): void;
    private detach;
    private outputOf;
    private boundsOf;
    private update;
}
export {};
