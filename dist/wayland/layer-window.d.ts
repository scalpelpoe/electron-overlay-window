import { BrowserWindow } from 'electron';
import { type KeyMods, WaylandHost } from './host';
interface Rect {
    x: number;
    y: number;
    width: number;
    height: number;
}
export type LayerShortcutKey = {
    code: number;
    sym: number;
    mods: KeyMods;
};
/** Options for `adoptLayerWindow`. */
export interface LayerWindowOptions {
    /** Frames per second Electron renders offscreen. Default 60. */
    frameRate?: number;
}
/**
 * Presents an offscreen-rendered BrowserWindow as a layer-shell surface.
 *
 * The window's own native (xdg_toplevel) window is never shown. Window methods
 * that affect presentation are replaced so existing callers keep working:
 * show/hide/isVisible map and unmap the layer surface, bounds methods position
 * it in global logical coordinates, setIgnoreMouseEvents sets the input
 * region, and focus/blur/isFocused follow layer keyboard focus. Pointer and
 * keyboard input arrives from the host and is replayed with sendInputEvent.
 * `-webkit-app-region: drag` areas and the edges of resizable windows move
 * and resize the surface, emitting the usual move/moved and
 * will-resize/resize/resized events.
 */
export declare class LayerWindow {
    readonly win: BrowserWindow;
    private readonly host;
    readonly sid: number;
    private bounds;
    private visible;
    private mappedAt;
    private ignoreMouse;
    private interactiveRects;
    private focused;
    private grab;
    private onTop;
    private pool;
    /** Pool files the host has not opened yet, by pool id. */
    private readonly unopenedPools;
    private pendingFrame;
    private damageSinceCommit;
    private pageCursor;
    private shownCursor;
    private buttons;
    private lastClick;
    private drag;
    private dragProbe;
    private lastReshow;
    private readonly unsubscribe;
    private readonly original;
    constructor(win: BrowserWindow, host: WaylandHost, options?: LayerWindowOptions);
    private resendState;
    private patchWindow;
    /** Rectangles, in window coordinates, that accept pointer input while the
     *  window ignores mouse events. Lets click-through overlays keep interactive
     *  panels without polling the pointer. */
    setInteractiveRects(rects: Rect[]): void;
    private inputRegion;
    private keyboardMode;
    private updateInput;
    private show;
    private hide;
    private raise;
    /** Grabs the keyboard. Layer-shell has no activation request, so focus is
     *  exclusive until `blur`, `hide` or `releaseLayerKeyboards`. */
    private focus;
    private blur;
    releaseKeyboard(): void;
    private setBounds;
    private paint;
    private present;
    private createPool;
    /** Forgets the current pool. Its releases are ignored from then on. */
    private dropPool;
    private unlinkUnopenedPools;
    private handleEvent;
    private setFocused;
    private modifiers;
    private toPage;
    private handlePointer;
    private handleButton;
    private handleAxis;
    /** Asks the page whether the press landed on a `-webkit-app-region: drag`
     *  area. The press was already delivered; if it was a drag area, the page
     *  gets a release and the window moves with the pointer instead. */
    private probeDragRegion;
    private resizeEdges;
    private startDrag;
    private continueDrag;
    private endDrag;
    private updateCursor;
    private dispose;
    /** Global logical bounds of the surface. */
    getBounds(): Rect;
    isVisible(): boolean;
}
/** Makes `win` a layer-shell surface. The window must have been created with
 *  `webPreferences.offscreen: true` and `show: false`, and must not have been
 *  shown yet. Call before wrapping its methods elsewhere. */
export declare function adoptLayerWindow(win: BrowserWindow, options?: LayerWindowOptions): LayerWindow;
export declare function getLayerWindow(win: BrowserWindow): LayerWindow | undefined;
export declare function hasFocusedLayerWindow(): boolean;
export declare function onLayerFocusChange(listener: () => void): () => void;
/** Drops every keyboard grab so the compositor returns focus to a window. */
export declare function releaseLayerKeyboards(): void;
/** Lets global shortcuts see key presses while a layer surface has keyboard
 *  focus, as X11 root grabs see them while an overlay window has focus. The
 *  handler returns true to swallow the key. */
export declare function setLayerShortcutHandler(handler: ((key: LayerShortcutKey) => boolean) | null): void;
/** Global logical pointer position while it is over a layer surface. */
export declare function getLayerPointer(): {
    x: number;
    y: number;
} | null;
export {};
