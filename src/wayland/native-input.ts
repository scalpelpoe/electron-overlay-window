import { readFileSync, unlinkSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import type { EvdevEvent, WaylandHost } from './host'
import { getLayerPointer, hasFocusedLayerWindow } from './layer-window'

/** Input aimed at the target, from evdev. Codes are evdev codes; mouse
 *  buttons are the `BTN_*` codes. Wheel deltas are detents, positive meaning
 *  up or right. */
export type NativeInputEvent =
  | { type: 'keydown' | 'keyup'; code: number; repeat: boolean }
  | { type: 'mousedown' | 'mouseup'; code: number }
  | { type: 'wheel'; delta: number; horizontal: boolean }

// BTN_MISC up to KEY_OK: mouse, joystick and gamepad buttons.
const BUTTON_FIRST = 0x100
const BUTTON_LAST = 0x15f

function isButton (code: number): boolean {
  return code >= BUTTON_FIRST && code <= BUTTON_LAST
}

/**
 * Delivers evdev input while it reaches the target, mirroring what a hook
 * inside the target's display server would see: presses only while the target
 * has keyboard focus and, for mouse input, while the pointer is not over an
 * overlay surface. Releases are always delivered so held-key tracking cannot
 * get stuck.
 */
export function listenNativeInput (
  host: WaylandHost,
  targetActive: () => boolean,
  listener: (event: NativeInputEvent) => void
): () => void {
  return host.onEvdev((event: EvdevEvent) => {
    if (event.kind === 'wheel') {
      if (targetActive() && !getLayerPointer()) listener({ type: 'wheel', delta: event.value, horizontal: event.horizontal })
      return
    }
    const pressed = event.value !== 0
    if (isButton(event.code)) {
      if (event.value === 2) return
      if (!pressed) listener({ type: 'mouseup', code: event.code })
      else if (targetActive() && !getLayerPointer()) listener({ type: 'mousedown', code: event.code })
      return
    }
    if (!pressed) listener({ type: 'keyup', code: event.code, repeat: false })
    else if (targetActive() && !hasFocusedLayerWindow()) listener({ type: 'keydown', code: event.code, repeat: event.value === 2 })
  })
}

let nextCapture = 1

/** Captures a logical rectangle of the screen as BGRA pixels. */
export async function captureScreenRegion (
  host: WaylandHost,
  rect: { x: number; y: number; width: number; height: number }
): Promise<{ width: number; height: number; data: Buffer } | null> {
  // The pool prefix lets the stale-file sweep remove captures of killed processes.
  const path = join(process.env.XDG_RUNTIME_DIR || tmpdir(), `electron-overlay-window-${process.pid}-capture-${nextCapture++}`)
  try {
    const size = await host.capture(rect, path)
    if (!size) return null
    const data = readFileSync(path)
    return data.length === size.width * size.height * 4 ? { ...size, data } : null
  } catch {
    return null
  } finally {
    try {
      unlinkSync(path)
    } catch {}
  }
}
