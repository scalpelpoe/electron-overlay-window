import { type ChildProcessWithoutNullStreams, execFileSync, spawn } from 'node:child_process'
import { existsSync } from 'node:fs'
import { join, sep } from 'node:path'
import { createInterface } from 'node:readline'

/** Availability of the compositor globals the host uses. */
export interface HostFeatures {
  layerShell: boolean
  layerShellVersion: number
  foreignToplevel: boolean
  dataControl: boolean
  cursorShape: boolean
  viewporter: boolean
  virtualKeyboard: boolean
  /** /dev/uinput is writable, for typing without a virtual keyboard. */
  uinput: boolean
  virtualPointer: boolean
  screencopy: boolean
  /** Windows are tracked through a KWin script, because the compositor lacks
   *  foreign-toplevel management. Untested on Plasma. */
  kwinScript: boolean
}

/** An output in global logical coordinates. */
export interface WaylandOutput {
  name: string
  description: string
  x: number
  y: number
  width: number
  height: number
  scale: number
}

export interface ToplevelInfo {
  tid: number
  title: string
  appId: string
  activated: boolean
  fullscreen: boolean
  maximized: boolean
  minimized: boolean
  outputs: string[]
  /** Global logical bounds of the window's content. Only KWin reports them;
   *  foreign-toplevel management has no positions. */
  geometry?: { x: number; y: number; width: number; height: number }
}

export interface KeyMods {
  shift: boolean
  ctrl: boolean
  alt: boolean
  meta: boolean
  caps: boolean
  num: boolean
}

export type PointerButton = 'left' | 'right' | 'middle' | 'back' | 'forward'

/** Events addressed to one surface. See src/lib/wayland/README.md. */
export type SurfaceEvent =
  | { ev: 'configure'; sid: number; width: number; height: number }
  | { ev: 'closed'; sid: number }
  | { ev: 'buffers'; sid: number; pool: number; path: string }
  | { ev: 'release'; sid: number; pool: number; index: number }
  | { ev: 'pointer'; sid: number; type: 'enter' | 'leave' | 'motion'; x: number; y: number }
  | { ev: 'button'; sid: number; button: PointerButton; pressed: boolean; x: number; y: number }
  | { ev: 'axis'; sid: number; x: number; y: number; dx: number; dy: number; discreteX: number; discreteY: number }
  | { ev: 'focus'; sid: number; focused: boolean }
  | { ev: 'key'; sid: number; pressed: boolean; repeat: boolean; code: number; keysym: string; sym: number; text: string; mods: KeyMods }

type GlobalEvent =
  | ({ ev: 'ready' } & HostFeatures)
  | { ev: 'outputs'; outputs: WaylandOutput[] }
  | ({ ev: 'toplevel' } & ToplevelInfo)
  | { ev: 'toplevel-closed'; tid: number }
  | { ev: 'clipboard'; text: string | null; html: string | null }
  | { ev: 'error'; message: string }
  | { ev: 'evdev'; devices: number }
  | ({ ev: 'input' } & EvdevEvent)
  | { ev: 'capture'; id: string; width: number; height: number }
  | { ev: 'capture'; id: string; error: string }
  | { ev: 'probe'; id: string; x: number | null; y: number | null }

/** A key, button or wheel event read from evdev. Key and button codes are
 *  evdev codes (`linux/input-event-codes.h`). */
export type EvdevEvent =
  | { kind: 'key'; code: number; value: 0 | 1 | 2 }
  | { kind: 'wheel'; value: number; horizontal: boolean }

type HostEvent = SurfaceEvent | GlobalEvent

type SurfaceListener = (event: SurfaceEvent) => void

export type InputRegion = 'all' | 'none' | ReadonlyArray<{ x: number; y: number; width: number; height: number }>
export type KeyboardMode = 'none' | 'on_demand' | 'exclusive'

const LOG_PREFIX = '[electron-overlay-window] wayland:'
const RESTART_DELAY_MS = 1000
// Probes give up within about a second in the host; a capture waits for the
// compositor, which may never answer.
const REQUEST_TIMEOUT_MS = 3000

/** Locates the host executable, preferring the asar-unpacked copy that a
 *  packaged Electron app can execute. */
export function hostExecutablePath (): string | null {
  const built = join(__dirname, '..', '..', 'build', 'Release', 'overlay-wayland-host')
  const unpacked = built.replace(`${sep}app.asar${sep}`, `${sep}app.asar.unpacked${sep}`)
  if (existsSync(unpacked)) return unpacked
  return existsSync(built) ? built : null
}

// Version 4 adds on_demand keyboard interactivity, without which clicking
// into an overlay would not let the user type in it.
const MIN_LAYER_SHELL_VERSION = 4

/** Whether overlays can be presented as layer surfaces, from a probe result. */
export function layerShellUsable (features: HostFeatures | null): boolean {
  return !!features?.layerShell && features.layerShellVersion >= MIN_LAYER_SHELL_VERSION
}

/** How the host follows the target's focus and bounds, or null when it
 *  cannot. Without one, an XWayland target can still be tracked through X11. */
export function windowTracking (features: HostFeatures | null): 'foreign-toplevel' | 'kwin-script' | null {
  if (features?.foreignToplevel) return 'foreign-toplevel'
  if (features?.kwinScript) return 'kwin-script'
  return null
}

/** Synchronously asks the compositor which globals it offers. Returns null when
 *  the host is missing, no Wayland compositor is reachable, or the probe fails.
 *  Safe to call before Electron's `ready` event, so an app can choose its Ozone
 *  platform from the result. */
export function probeWaylandHost (): HostFeatures | null {
  if (process.platform !== 'linux' || !process.env.WAYLAND_DISPLAY) return null
  const path = hostExecutablePath()
  if (!path) return null
  let stdout: string
  try {
    stdout = execFileSync(path, ['--probe'], { encoding: 'utf8', timeout: 3000, stdio: ['ignore', 'pipe', 'ignore'] })
  } catch (error) {
    // Exit status 1 still prints the feature line: layer-shell is just missing.
    stdout = String((error as { stdout?: unknown }).stdout ?? '')
  }
  const line = stdout.split('\n').find((l) => l.startsWith('{'))
  if (!line) return null
  try {
    const event = JSON.parse(line) as HostEvent
    return event.ev === 'ready' ? event : null
  } catch {
    return null
  }
}

/** Client for the `overlay-wayland-host` process. One per app; created by
 *  `WaylandHost.start()`. Restarts the host after a crash and asks surface
 *  owners to resend their state through `onRestart`. */
export class WaylandHost {
  private static instance: WaylandHost | null = null

  features: HostFeatures | null = null
  outputs: WaylandOutput[] = []
  readonly toplevels = new Map<number, ToplevelInfo>()
  clipboardText: string | null = null
  clipboardHtml: string | null = null

  private child: ChildProcessWithoutNullStreams | null = null
  private stopped = false
  private readonly surfaceListeners = new Map<number, SurfaceListener>()
  private readonly toplevelListeners = new Set<(info: ToplevelInfo | { tid: number; closed: true }) => void>()
  private readonly outputListeners = new Set<() => void>()
  private readonly restartListeners = new Set<() => void>()
  private readyWaiters: Array<() => void> = []
  private readonly evdevListeners = new Set<(event: EvdevEvent) => void>()
  private readonly pendingCaptures = new Map<string, (result: { width: number; height: number } | null) => void>()
  private readonly pendingProbes = new Map<string, (point: { x: number; y: number } | null) => void>()
  /** Keys and buttons reported down, released for listeners when the host goes away. */
  private readonly heldEvdev = new Set<number>()
  private nextRequest = 1
  evdevDevices = 0

  private constructor (private readonly path: string) {}

  static get (): WaylandHost | null {
    return WaylandHost.instance
  }

  /** Spawns the host once. Returns null when the executable is unavailable. */
  static start (): WaylandHost | null {
    if (WaylandHost.instance) return WaylandHost.instance
    const path = hostExecutablePath()
    if (!path) return null
    const host = new WaylandHost(path)
    host.spawn()
    WaylandHost.instance = host
    return host
  }

  /** Resolves after the host reported its features, or failed to start. */
  ready (): Promise<void> {
    if (this.features || this.stopped) return Promise.resolve()
    return new Promise((resolve) => this.readyWaiters.push(resolve))
  }

  private resolveReady (): void {
    for (const resolve of this.readyWaiters) resolve()
    this.readyWaiters = []
  }

  stop (): void {
    this.stopped = true
    this.child?.stdin.end()
    this.child = null
    this.resolveReady()
    this.abandonRequests()
  }

  /** Settles requests and releases keys the current host process can no
   *  longer answer or report. */
  private abandonRequests (): void {
    for (const resolve of this.pendingCaptures.values()) resolve(null)
    for (const resolve of this.pendingProbes.values()) resolve(null)
    this.pendingCaptures.clear()
    this.pendingProbes.clear()
    this.evdevDevices = 0
    const held = [...this.heldEvdev]
    this.heldEvdev.clear()
    for (const code of held) {
      for (const listener of this.evdevListeners) listener({ kind: 'key', code, value: 0 })
    }
  }

  private spawn (): void {
    const child = spawn(this.path, [], { stdio: ['pipe', 'pipe', 'pipe'] })
    this.child = child
    child.stdin.on('error', () => {})
    child.on('error', (error) => {
      // Spawn failures (for example a missing executable bit) would repeat on
      // every restart, so give up instead of looping.
      console.error(LOG_PREFIX, 'cannot run the host:', error.message)
      if (this.child === child) this.child = null
      this.stopped = true
      this.resolveReady()
    })
    child.stderr.on('data', (chunk: Buffer) => {
      for (const line of chunk.toString().split('\n')) {
        if (line) console.warn(LOG_PREFIX, line)
      }
    })
    createInterface({ input: child.stdout }).on('line', (line) => this.handleLine(line))
    // A restarted host starts with evdev off.
    if (this.evdevListeners.size > 0) this.send('evdev on')
    child.on('exit', (code, signal) => {
      if (this.child !== child) return
      this.child = null
      if (this.stopped) return
      console.error(LOG_PREFIX, `host exited (${signal ?? code}); restarting`)
      this.toplevels.clear()
      this.abandonRequests()
      setTimeout(() => {
        if (this.stopped) return
        this.spawn()
        for (const listener of this.restartListeners) listener()
      }, RESTART_DELAY_MS)
    })
  }

  private handleLine (line: string): void {
    let event: HostEvent
    try {
      event = JSON.parse(line) as HostEvent
    } catch {
      console.warn(LOG_PREFIX, 'malformed event', line)
      return
    }
    if ('sid' in event) {
      this.surfaceListeners.get(event.sid)?.(event)
      return
    }
    switch (event.ev) {
      case 'ready': {
        const { ev: _, ...features } = event
        this.features = features
        this.resolveReady()
        break
      }
      case 'outputs':
        this.outputs = event.outputs
        for (const listener of this.outputListeners) listener()
        break
      case 'toplevel': {
        const { ev: _, ...info } = event
        this.toplevels.set(info.tid, info)
        for (const listener of this.toplevelListeners) listener(info)
        break
      }
      case 'toplevel-closed':
        this.toplevels.delete(event.tid)
        for (const listener of this.toplevelListeners) listener({ tid: event.tid, closed: true })
        break
      case 'clipboard':
        this.clipboardText = event.text
        this.clipboardHtml = event.html
        break
      case 'error':
        console.error(LOG_PREFIX, event.message)
        break
      case 'evdev':
        this.evdevDevices = event.devices
        break
      case 'input': {
        const { ev: _, ...input } = event
        if (input.kind === 'key' && input.value === 1) this.heldEvdev.add(input.code)
        else if (input.kind === 'key' && input.value === 0) this.heldEvdev.delete(input.code)
        for (const listener of this.evdevListeners) listener(input)
        break
      }
      case 'capture': {
        const resolve = this.pendingCaptures.get(event.id)
        this.pendingCaptures.delete(event.id)
        if ('error' in event) console.warn(LOG_PREFIX, 'capture failed:', event.error)
        resolve?.('error' in event ? null : { width: event.width, height: event.height })
        break
      }
      case 'probe': {
        const resolve = this.pendingProbes.get(event.id)
        this.pendingProbes.delete(event.id)
        resolve?.(event.x === null || event.y === null ? null : { x: event.x, y: event.y })
        break
      }
    }
  }

  /** Returns false when there is no host to send to. */
  private send (line: string): boolean {
    if (!this.child || this.child.stdin.destroyed) return false
    this.child.stdin.write(line + '\n')
    return true
  }

  private request<T> (pending: Map<string, (result: T | null) => void>, id: string, line: string): Promise<T | null> {
    return new Promise((resolve) => {
      pending.set(id, resolve)
      const timer = setTimeout(() => {
        if (pending.get(id) !== resolve) return
        pending.delete(id)
        resolve(null)
      }, REQUEST_TIMEOUT_MS)
      timer.unref?.()
      if (this.send(line)) return
      pending.delete(id)
      resolve(null)
    })
  }

  onSurface (sid: number, listener: SurfaceListener): () => void {
    this.surfaceListeners.set(sid, listener)
    return () => this.surfaceListeners.delete(sid)
  }

  onToplevel (listener: (info: ToplevelInfo | { tid: number; closed: true }) => void): () => void {
    this.toplevelListeners.add(listener)
    return () => this.toplevelListeners.delete(listener)
  }

  onOutputs (listener: () => void): () => void {
    this.outputListeners.add(listener)
    return () => this.outputListeners.delete(listener)
  }

  onRestart (listener: () => void): () => void {
    this.restartListeners.add(listener)
    return () => this.restartListeners.delete(listener)
  }

  createSurface (sid: number): void {
    this.send(`create ${sid}`)
  }

  destroySurface (sid: number): void {
    this.send(`destroy ${sid}`)
  }

  setGeometry (sid: number, rect: { x: number; y: number; width: number; height: number }): void {
    const r = [rect.x, rect.y, Math.max(1, rect.width), Math.max(1, rect.height)].map(Math.round)
    this.send(`geometry ${sid} ${r.join(' ')}`)
  }

  setKeyboard (sid: number, mode: KeyboardMode): void {
    this.send(`keyboard ${sid} ${mode}`)
  }

  setInput (sid: number, region: InputRegion): void {
    if (typeof region === 'string') {
      this.send(`input ${sid} ${region}`)
      return
    }
    const rects = region
      .map((r) => [r.x, r.y, r.width, r.height].map(Math.round))
      .filter(([, , w, h]) => w > 0 && h > 0)
    this.send(rects.length ? `input ${sid} ${rects.map((r) => r.join(',')).join(' ')}` : `input ${sid} none`)
  }

  setLayer (sid: number, layer: 'background' | 'bottom' | 'top' | 'overlay'): void {
    this.send(`layer ${sid} ${layer}`)
  }

  show (sid: number): void {
    this.send(`show ${sid}`)
  }

  hide (sid: number): void {
    this.send(`hide ${sid}`)
  }

  raise (sid: number): void {
    this.send(`raise ${sid}`)
  }

  setBuffers (sid: number, pool: number, path: string, width: number, height: number): void {
    this.send(`buffers ${sid} ${pool} ${path} ${width} ${height}`)
  }

  commit (sid: number, pool: number, index: number, damage: { x: number; y: number; width: number; height: number }): void {
    this.send(`commit ${sid} ${pool} ${index} ${damage.x} ${damage.y} ${damage.width} ${damage.height}`)
  }

  setCursor (sid: number, name: string): void {
    this.send(`cursor ${sid} ${/^[a-z-]+$/.test(name) ? name : 'default'}`)
  }

  activateToplevel (tid: number): void {
    this.send(`activate ${tid}`)
  }

  /** Owns the selection. The local cache is updated immediately so a
   *  following read returns the written value. */
  writeClipboard (text: string, html?: string): void {
    this.clipboardText = text
    this.clipboardHtml = html ?? null
    const encode = (value: string) => Buffer.from(value, 'utf8').toString('base64')
    this.send(`clipboard-set ${encode(text)} ${html ? encode(html) : '-'}`)
  }

  /** Reads keyboards and mice from evdev while there are listeners. */
  onEvdev (listener: (event: EvdevEvent) => void): () => void {
    this.evdevListeners.add(listener)
    if (this.evdevListeners.size === 1) this.send('evdev on')
    return () => {
      this.evdevListeners.delete(listener)
      if (this.evdevListeners.size === 0) this.send('evdev off')
    }
  }

  /** Presses or releases an evdev key on the host's virtual keyboard, or on
   *  its uinput keyboard where the compositor offers no virtual keyboard. */
  virtualKey (code: number, pressed: boolean): void {
    this.send(`vkey ${code} ${pressed ? 1 : 0}`)
  }

  warpPointer (x: number, y: number): void {
    this.send(`warp ${Math.round(x)} ${Math.round(y)}`)
  }

  /** Captures a logical rectangle into `path` as BGRA. Resolves to the pixel
   *  size, or null on failure. */
  capture (rect: { x: number; y: number; width: number; height: number }, path: string): Promise<{ width: number; height: number } | null> {
    const id = `c${this.nextRequest++}`
    const r = [rect.x, rect.y, rect.width, rect.height].map(Math.round)
    // The path ends the command line.
    if (/[\r\n]/.test(path)) return Promise.resolve(null)
    return this.request(this.pendingCaptures, id, `capture ${id} ${r.join(' ')} ${path}`)
  }

  /** Global logical pointer position, found by briefly mapping input
   *  surfaces over every output. */
  probePointer (): Promise<{ x: number; y: number } | null> {
    const id = `p${this.nextRequest++}`
    return this.request(this.pendingProbes, id, `probe ${id}`)
  }

  clearClipboard (): void {
    this.clipboardText = null
    this.clipboardHtml = null
    this.send('clipboard-clear')
  }

  /** The output containing the rectangle's center, else the most overlapped
   *  one. Mirrors the host's placement rule. */
  outputFor (rect: { x: number; y: number; width: number; height: number }): WaylandOutput | null {
    const cx = rect.x + rect.width / 2
    const cy = rect.y + rect.height / 2
    const containing = this.outputs.find((o) => cx >= o.x && cx < o.x + o.width && cy >= o.y && cy < o.y + o.height)
    if (containing) return containing
    let best: WaylandOutput | null = null
    let bestArea = 0
    for (const o of this.outputs) {
      const w = Math.min(rect.x + rect.width, o.x + o.width) - Math.max(rect.x, o.x)
      const h = Math.min(rect.y + rect.height, o.y + o.height) - Math.max(rect.y, o.y)
      if (w > 0 && h > 0 && w * h > bestArea) {
        best = o
        bestArea = w * h
      }
    }
    return best ?? this.outputs[0] ?? null
  }
}
