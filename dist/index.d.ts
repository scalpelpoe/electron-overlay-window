import { EventEmitter } from 'node:events';
import { BrowserWindow, Rectangle, BrowserWindowConstructorOptions } from 'electron';
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
declare class OverlayControllerGlobal {
    private isInitialized;
    private electronWindow?;
    targetBounds: Rectangle;
    targetHasFocus: boolean;
    private focusNext;
    private macTitleBarHeight;
    private attachOptions;
    readonly events: EventEmitter<[never]>;
    constructor();
    private handleFullscreen;
    private updateOverlayBounds;
    private handler;
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
    setTargetTitles(titles: string[]): void;
    clearTarget(): void;
    stopHook(): void;
    screenshot(): Buffer;
}
export declare const OverlayController: OverlayControllerGlobal;
export {};
