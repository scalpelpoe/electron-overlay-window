import type { WaylandHost } from './host';
/** Input aimed at the target, from evdev. Codes are evdev codes; mouse
 *  buttons are the `BTN_*` codes. Wheel deltas are detents, positive meaning
 *  up or right. */
export type NativeInputEvent = {
    type: 'keydown' | 'keyup';
    code: number;
    repeat: boolean;
} | {
    type: 'mousedown' | 'mouseup';
    code: number;
} | {
    type: 'wheel';
    delta: number;
    horizontal: boolean;
};
/**
 * Delivers evdev input while it reaches the target, mirroring what a hook
 * inside the target's display server would see: presses only while the target
 * has keyboard focus and, for mouse input, while the pointer is not over an
 * overlay surface. Releases are always delivered so held-key tracking cannot
 * get stuck.
 */
export declare function listenNativeInput(host: WaylandHost, targetActive: () => boolean, listener: (event: NativeInputEvent) => void): () => void;
/** Captures a logical rectangle of the screen as BGRA pixels. */
export declare function captureScreenRegion(host: WaylandHost, rect: {
    x: number;
    y: number;
    width: number;
    height: number;
}): Promise<{
    width: number;
    height: number;
    data: Buffer;
} | null>;
