import type { ToplevelInfo, WaylandHost, WaylandOutput } from './host'
import { hasFocusedLayerWindow, onLayerFocusChange, releaseLayerKeyboards } from './layer-window'

export interface Bounds {
  x: number
  y: number
  width: number
  height: number
}

export type TrackerEvent =
  | { type: 'attach'; titleIndex: number; isFullscreen: boolean | undefined; hasAccess: undefined } & Bounds
  | { type: 'focus' }
  | { type: 'blur' }
  | { type: 'detach' }
  | { type: 'fullscreen'; isFullscreen: boolean }
  | { type: 'moveresize' } & Bounds

type Emit = (event: TrackerEvent) => void

// Keyboard focus moving from the game into an overlay surface arrives as two
// independent changes: the game deactivates and the layer surface gains focus.
// Wait this long before treating a deactivation as leaving the game.
const BLUR_GRACE_MS = 100

/**
 * Converts the X11 bounds of an XWayland window to logical coordinates.
 *
 * X11 pixels are logical pixels unless the compositor lets X11 clients render
 * at native resolution (KWin's "apply scaling themselves", Hyprland's
 * `force_zero_scaling`), where they are physical. A fullscreen target covers an
 * output exactly in one of the two spaces, which identifies that output.
 * Other bounds are assumed to be logical already.
 */
export function x11BoundsToLogical (bounds: Bounds, outputs: readonly WaylandOutput[]): Bounds {
  const near = (a: number, b: number) => Math.abs(a - b) <= 1
  const covers = (o: WaylandOutput, scale: number, withPosition: boolean) =>
    near(bounds.width, o.width * scale) && near(bounds.height, o.height * scale) &&
    (!withPosition || (near(bounds.x, o.x * scale) && near(bounds.y, o.y * scale)))
  const output =
    outputs.find((o) => covers(o, 1, true)) ??
    outputs.find((o) => covers(o, o.scale, true)) ??
    outputs.find((o) => covers(o, 1, false) || covers(o, o.scale, false))
  if (!output) return bounds
  return { x: output.x, y: output.y, width: output.width, height: output.height }
}

/**
 * Turns raw "the game is focused" changes into focus/blur events, treating
 * keyboard focus in one of the overlay's layer surfaces as still being in the
 * game. This matches the X11 backend, where the overlay takes input focus
 * without the window manager deactivating the game.
 */
export class FocusArbiter {
  private gameFocused = false
  private active = false
  private blurTimer: ReturnType<typeof setTimeout> | null = null
  private readonly offLayerFocus: () => void

  constructor (private readonly emit: Emit) {
    this.offLayerFocus = onLayerFocusChange(() => this.update())
  }

  get isActive (): boolean {
    return this.active
  }

  setGameFocused (focused: boolean): void {
    this.gameFocused = focused
    this.update()
  }

  /** Forgets focus without emitting, for attach and detach transitions that
   *  emit their own events. */
  reset (active: boolean): void {
    this.cancelBlur()
    this.gameFocused = active
    this.active = active
  }

  stop (): void {
    this.cancelBlur()
    this.offLayerFocus()
  }

  private update (): void {
    const wanted = this.gameFocused || hasFocusedLayerWindow()
    if (wanted) {
      this.cancelBlur()
      if (!this.active) {
        this.active = true
        this.emit({ type: 'focus' })
      }
      return
    }
    if (!this.active || this.blurTimer) return
    this.blurTimer = setTimeout(() => {
      this.blurTimer = null
      if (this.gameFocused || hasFocusedLayerWindow() || !this.active) return
      this.active = false
      this.emit({ type: 'blur' })
    }, BLUR_GRACE_MS)
  }

  private cancelBlur (): void {
    if (this.blurTimer) clearTimeout(this.blurTimer)
    this.blurTimer = null
  }
}

/**
 * Finds the target among the compositor's toplevels
 * (`zwlr_foreign_toplevel_manager_v1`) by exact title. Like the X11 backend,
 * it attaches when a matching window becomes active and keeps following it
 * if its title changes afterwards.
 *
 * The protocol reports a toplevel's outputs but not its position, so the
 * target's bounds are the logical bounds of its output. That is exact for
 * fullscreen and borderless-fullscreen windows.
 */
export class ToplevelTracker {
  private titles: string[]
  private attached: number | null = null
  private bounds: Bounds | null = null
  private fullscreen = false
  private readonly arbiter: FocusArbiter
  private readonly unsubscribe: Array<() => void>

  constructor (private readonly host: WaylandHost, titles: string[], private readonly emit: Emit) {
    this.titles = titles
    this.arbiter = new FocusArbiter(emit)
    this.unsubscribe = [
      host.onToplevel(() => this.update()),
      host.onOutputs(() => this.update()),
      host.onRestart(() => this.update())
    ]
    this.update()
  }

  get targetHasFocus (): boolean {
    return this.arbiter.isActive
  }

  setTitles (titles: string[]): void {
    this.detach()
    this.titles = titles
    this.update()
  }

  clear (): void {
    this.detach()
    this.titles = []
  }

  stop (): void {
    for (const off of this.unsubscribe) off()
    this.arbiter.stop()
  }

  /** Returns keyboard focus to the target. */
  focusTarget (): void {
    releaseLayerKeyboards()
    // Activating while another application is focused would steal focus.
    if (this.attached !== null && this.arbiter.isActive) this.host.activateToplevel(this.attached)
  }

  private detach (): void {
    if (this.attached === null) return
    if (this.arbiter.isActive) this.emit({ type: 'blur' })
    this.arbiter.reset(false)
    this.attached = null
    this.bounds = null
    this.emit({ type: 'detach' })
  }

  private outputOf (toplevel: ToplevelInfo): WaylandOutput | null {
    const outputs = this.host.outputs
    return outputs.find((o) => toplevel.outputs.includes(o.name)) ??
      (outputs.length === 1 ? outputs[0] : null)
  }

  private boundsOf (toplevel: ToplevelInfo): Bounds | null {
    const output = this.outputOf(toplevel)
    if (output) return { x: output.x, y: output.y, width: output.width, height: output.height }
    return this.bounds ?? (this.host.outputs[0] ? { ...this.host.outputs[0] } : null)
  }

  private update (): void {
    const toplevels = this.host.toplevels
    if (this.attached !== null && !toplevels.has(this.attached)) this.detach()

    const active = [...toplevels.values()].find((t) => t.activated && this.titles.includes(t.title))
    if (active && active.tid !== this.attached) {
      const bounds = this.boundsOf(active)
      if (!bounds) return
      this.detach()
      this.attached = active.tid
      this.bounds = bounds
      this.fullscreen = active.fullscreen
      this.arbiter.reset(true)
      this.emit({
        type: 'attach',
        hasAccess: undefined,
        isFullscreen: active.fullscreen,
        titleIndex: this.titles.indexOf(active.title),
        ...bounds
      })
      this.emit({ type: 'focus' })
      return
    }

    const target = this.attached === null ? undefined : toplevels.get(this.attached)
    if (!target) return
    const bounds = this.boundsOf(target)
    if (bounds && JSON.stringify(bounds) !== JSON.stringify(this.bounds)) {
      this.bounds = bounds
      this.emit({ type: 'moveresize', ...bounds })
    }
    if (target.fullscreen !== this.fullscreen) {
      this.fullscreen = target.fullscreen
      this.emit({ type: 'fullscreen', isFullscreen: target.fullscreen })
    }
    this.arbiter.setGameFocused(target.activated)
  }
}
