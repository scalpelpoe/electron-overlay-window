import { closeSync, ftruncateSync, openSync, readdirSync, unlinkSync, writeSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import {
  BrowserWindow,
  type MouseInputEvent,
  type MouseWheelInputEvent,
  type NativeImage,
  type Rectangle
} from 'electron'
import { type InputRegion, type KeyboardMode, type KeyMods, type PointerButton, type SurfaceEvent, WaylandHost } from './host'
import { keyInputEvents } from './keys'

type Modifier = NonNullable<MouseInputEvent['modifiers']>[number]

interface Rect {
  x: number
  y: number
  width: number
  height: number
}

/** Pixels of the two shared buffers for one surface. */
interface Pool {
  id: number
  fd: number
  width: number
  height: number
  busy: [boolean, boolean]
  /** Region of each buffer that is older than the latest frame. */
  stale: [Rect | null, Rect | null]
}

type Drag =
  | { kind: 'move'; grabX: number; grabY: number }
  | { kind: 'resize'; edges: Edges; start: Rect; grabX: number; grabY: number }

interface Edges {
  left: boolean
  right: boolean
  top: boolean
  bottom: boolean
}

const RESIZE_BORDER = 6
const DOUBLE_CLICK_MS = 500
const DOUBLE_CLICK_DISTANCE = 4
// Chromium's X11 wheel step, so wheel scrolling matches the XWayland build.
const WHEEL_TICK_PIXELS = 53
const POOL_PREFIX = 'electron-overlay-window'

let nextSid = 1
let nextPool = 1
let mapSequence = 0
const layerWindows = new Map<number, LayerWindow>()
let focusedLayerWindow: LayerWindow | null = null
export type LayerShortcutKey = { code: number; sym: number; mods: KeyMods }
let shortcutHandler: ((key: LayerShortcutKey) => boolean) | null = null
let pointer: { window: LayerWindow; x: number; y: number } | null = null
const focusListeners = new Set<() => void>()

function union (a: Rect | null, b: Rect): Rect {
  if (!a) return b
  const x = Math.min(a.x, b.x)
  const y = Math.min(a.y, b.y)
  return {
    x,
    y,
    width: Math.max(a.x + a.width, b.x + b.width) - x,
    height: Math.max(a.y + a.height, b.y + b.height) - y
  }
}

function clamp (r: Rect, width: number, height: number): Rect | null {
  const x = Math.max(0, Math.floor(r.x))
  const y = Math.max(0, Math.floor(r.y))
  const right = Math.min(width, Math.ceil(r.x + r.width))
  const bottom = Math.min(height, Math.ceil(r.y + r.height))
  return right > x && bottom > y ? { x, y, width: right - x, height: bottom - y } : null
}

function poolDirectory (): string {
  return process.env.XDG_RUNTIME_DIR || tmpdir()
}

/** Removes pool files left behind by processes that were killed. */
function removeStalePools (): void {
  let names: string[]
  try {
    names = readdirSync(poolDirectory())
  } catch {
    return
  }
  for (const name of names) {
    const pid = Number(new RegExp(`^${POOL_PREFIX}-(\\d+)-`).exec(name)?.[1])
    if (!pid || pid === process.pid) continue
    try {
      process.kill(pid, 0)
      continue
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== 'ESRCH') continue
    }
    try {
      unlinkSync(join(poolDirectory(), name))
    } catch {}
  }
}

/** Options for `adoptLayerWindow`. */
export interface LayerWindowOptions {
  /** Frames per second Electron renders offscreen. Default 60. */
  frameRate?: number
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
export class LayerWindow {
  readonly sid = nextSid++
  private bounds: Rect
  private visible = false
  private mappedAt = 0
  private ignoreMouse = false
  private interactiveRects: Rect[] = []
  private focused = false
  private grab = false
  private onTop = true
  private pool: Pool | null = null
  /** Pool files the host has not opened yet, by pool id. */
  private readonly unopenedPools = new Map<number, string>()
  private pendingFrame: { bitmap: Buffer; width: number; height: number } | null = null
  private damageSinceCommit: Rect | null = null
  private pageCursor = 'default'
  private shownCursor = ''
  private buttons = new Set<PointerButton>()
  private lastClick = { button: '' as PointerButton | '', at: 0, x: 0, y: 0, count: 0 }
  private drag: Drag | null = null
  private dragProbe = 0
  private lastReshow = 0
  private readonly unsubscribe: Array<() => void> = []

  private readonly original: {
    setBounds: BrowserWindow['setBounds']
  }

  constructor (readonly win: BrowserWindow, private readonly host: WaylandHost, options: LayerWindowOptions = {}) {
    this.original = { setBounds: win.setBounds.bind(win) }
    const { x, y, width, height } = win.getBounds()
    this.bounds = { x, y, width, height }
    layerWindows.set(this.sid, this)

    this.unsubscribe.push(host.onSurface(this.sid, (event) => this.handleEvent(event)))
    this.unsubscribe.push(host.onRestart(() => this.resendState()))
    this.resendState()

    const wc = win.webContents
    wc.setFrameRate(options.frameRate ?? 60)
    wc.on('paint', (_event, dirty, image) => this.paint(dirty, image))
    wc.on('cursor-changed', (_event, type) => {
      this.pageCursor = type === 'custom' ? 'default' : type
      if (!this.drag) this.updateCursor()
    })
    win.once('closed', () => this.dispose())
    this.patchWindow()
  }

  private resendState (): void {
    this.host.createSurface(this.sid)
    this.host.setGeometry(this.sid, this.bounds)
    this.host.setInput(this.sid, this.inputRegion())
    this.host.setKeyboard(this.sid, this.keyboardMode())
    if (!this.onTop) this.host.setLayer(this.sid, 'bottom')
    // A restarted host has no pool. Recreate it from a fresh file and repaint.
    this.dropPool()
    this.unlinkUnopenedPools()
    if (!this.win.isDestroyed()) this.win.webContents.invalidate()
    if (this.visible) this.host.show(this.sid)
  }

  private patchWindow (): void {
    const win = this.win
    win.show = () => this.show()
    win.showInactive = () => this.show()
    win.hide = () => this.hide()
    win.isVisible = () => this.visible
    win.focus = () => this.focus()
    win.blur = () => this.blur()
    win.isFocused = () => this.focused
    win.moveTop = () => this.raise()
    win.setIgnoreMouseEvents = (ignore: boolean) => {
      this.ignoreMouse = ignore
      this.updateInput()
    }
    win.setBounds = (bounds: Partial<Rectangle>) => this.setBounds({ ...this.bounds, ...bounds })
    win.getBounds = () => ({ ...this.bounds })
    win.getContentBounds = win.getBounds
    win.setContentBounds = win.setBounds
    win.getPosition = () => [this.bounds.x, this.bounds.y]
    win.setPosition = (x: number, y: number) => this.setBounds({ ...this.bounds, x, y })
    win.getSize = () => [this.bounds.width, this.bounds.height]
    win.getContentSize = win.getSize
    win.setSize = (width: number, height: number) => this.setBounds({ ...this.bounds, width, height })
    win.setContentSize = win.setSize
    // Not always on top means below windows, so that a native dialog opened
    // from an overlay is not hidden behind the overlay layer.
    win.setAlwaysOnTop = (flag: boolean) => {
      if (flag === this.onTop) return
      this.onTop = flag
      if (!flag) this.releaseKeyboard()
      this.host.setLayer(this.sid, flag ? 'overlay' : 'bottom')
    }
    win.isAlwaysOnTop = () => this.onTop
    // Layer surfaces never appear in a taskbar or on a single workspace.
    win.setSkipTaskbar = () => {}
    win.setVisibleOnAllWorkspaces = () => {}
  }

  /** Rectangles, in window coordinates, that accept pointer input while the
   *  window ignores mouse events. Lets click-through overlays keep interactive
   *  panels without polling the pointer. */
  setInteractiveRects (rects: Rect[]): void {
    this.interactiveRects = rects.map((r) => ({ ...r }))
    this.updateInput()
  }

  private inputRegion (): InputRegion {
    if (!this.ignoreMouse) return 'all'
    return this.interactiveRects.length ? this.interactiveRects : 'none'
  }

  private keyboardMode (): KeyboardMode {
    if (this.grab) return 'exclusive'
    return this.inputRegion() === 'none' ? 'none' : 'on_demand'
  }

  private updateInput (): void {
    this.host.setInput(this.sid, this.inputRegion())
    this.host.setKeyboard(this.sid, this.keyboardMode())
  }

  private show (): void {
    if (this.win.isDestroyed()) return
    if (!this.visible) {
      this.visible = true
      this.mappedAt = ++mapSequence
      this.host.show(this.sid)
      this.win.webContents.startPainting()
      this.win.webContents.invalidate()
      this.win.emit('show')
    }
  }

  private hide (): void {
    if (!this.visible) return
    this.visible = false
    this.grab = false
    this.drag = null
    this.buttons.clear()
    if (pointer?.window === this) pointer = null
    this.host.setKeyboard(this.sid, this.keyboardMode())
    this.host.hide(this.sid)
    this.setFocused(false)
    if (!this.win.isDestroyed()) {
      this.win.webContents.stopPainting()
      this.win.emit('hide')
    }
  }

  private raise (): void {
    if (!this.visible || this.mappedAt === mapSequence) return
    this.mappedAt = ++mapSequence
    this.host.raise(this.sid)
  }

  /** Grabs the keyboard. Layer-shell has no activation request, so focus is
   *  exclusive until `blur`, `hide` or `releaseLayerKeyboards`. */
  private focus (): void {
    if (!this.visible || this.grab) return
    for (const other of layerWindows.values()) {
      if (other !== this) other.releaseKeyboard()
    }
    this.grab = true
    this.host.setKeyboard(this.sid, 'exclusive')
  }

  private blur (): void {
    this.releaseKeyboard()
  }

  releaseKeyboard (): void {
    if (!this.grab) return
    this.grab = false
    this.host.setKeyboard(this.sid, this.keyboardMode())
  }

  private setBounds (next: Rect): void {
    const rect = {
      x: Math.round(next.x),
      y: Math.round(next.y),
      width: Math.max(1, Math.round(next.width)),
      height: Math.max(1, Math.round(next.height))
    }
    const b = this.bounds
    if (rect.x === b.x && rect.y === b.y && rect.width === b.width && rect.height === b.height) return
    const resized = rect.width !== b.width || rect.height !== b.height
    this.bounds = rect
    // Resizes the offscreen renderer. The native window stays unmapped, so its
    // position is irrelevant.
    if (resized && !this.win.isDestroyed()) this.original.setBounds(rect)
    this.host.setGeometry(this.sid, rect)
  }

  private paint (dirty: Rectangle, image: NativeImage): void {
    if (!this.visible) return
    const { width, height } = image.getSize()
    if (width <= 0 || height <= 0) return
    let pool = this.pool
    if (!pool || pool.width !== width || pool.height !== height) {
      pool = this.createPool(width, height)
      dirty = { x: 0, y: 0, width, height }
    }
    // The dirty rectangle is in pixels, except that Chromium reports a full
    // repaint after a resize in DIP. Repaint everything when it looks like one.
    const full = dirty.x === 0 && dirty.y === 0 && dirty.width === this.bounds.width && dirty.height === this.bounds.height
    const damage = clamp(full ? { x: 0, y: 0, width, height } : dirty, width, height)
    if (!damage) return
    pool.stale = [union(pool.stale[0], damage), union(pool.stale[1], damage)]
    this.damageSinceCommit = union(this.damageSinceCommit, damage)
    const index = !pool.busy[0] ? 0 : !pool.busy[1] ? 1 : null
    if (index === null) {
      // Both buffers are with the compositor. Keep a copy and present it on release.
      this.pendingFrame = { bitmap: image.toBitmap(), width, height }
      return
    }
    this.pendingFrame = null
    this.present(pool, index, image.getBitmap())
  }

  private present (pool: Pool, index: 0 | 1, bitmap: Buffer): void {
    // Busy until the host reports the compositor released it.
    const stale = pool.stale[index]
    const stride = pool.width * 4
    if (stale) {
      const start = stale.y * stride
      const length = stale.height * stride
      writeSync(pool.fd, bitmap, start, length, index * stride * pool.height + start)
      pool.stale[index] = null
    }
    const damage = this.damageSinceCommit ?? { x: 0, y: 0, width: pool.width, height: pool.height }
    this.damageSinceCommit = null
    pool.busy[index] = true
    this.host.commit(this.sid, pool.id, index, damage)
  }

  private createPool (width: number, height: number): Pool {
    // The host keeps its own mapping of the old pool until the compositor
    // releases those buffers, so this side can drop it right away.
    this.dropPool()
    const id = nextPool++
    const path = join(poolDirectory(), `${POOL_PREFIX}-${process.pid}-${this.sid}-${id}`)
    const fd = openSync(path, 'w+', 0o600)
    ftruncateSync(fd, width * height * 4 * 2)
    const pool: Pool = { id, fd, width, height, busy: [false, false], stale: [null, null] }
    this.pool = pool
    // The file must exist until the host has opened it.
    this.unopenedPools.set(id, path)
    this.host.setBuffers(this.sid, id, path, width, height)
    return pool
  }

  /** Forgets the current pool. Its releases are ignored from then on. */
  private dropPool (): void {
    const pool = this.pool
    this.pool = null
    this.pendingFrame = null
    if (!pool) return
    try {
      closeSync(pool.fd)
    } catch {}
  }

  private unlinkUnopenedPools (): void {
    for (const path of this.unopenedPools.values()) {
      try {
        unlinkSync(path)
      } catch {}
    }
    this.unopenedPools.clear()
  }

  private handleEvent (event: SurfaceEvent): void {
    switch (event.ev) {
      case 'buffers': {
        // The host holds its own reference; the name is no longer needed.
        const path = this.unopenedPools.get(event.pool)
        this.unopenedPools.delete(event.pool)
        if (path) {
          try {
            unlinkSync(path)
          } catch {}
        }
        break
      }
      case 'release': {
        const pool = this.pool
        if (!pool || event.pool !== pool.id) break
        if (event.index !== 0 && event.index !== 1) break
        pool.busy[event.index] = false
        const pending = this.pendingFrame
        if (pending && pending.width === pool.width && pending.height === pool.height) {
          this.pendingFrame = null
          this.present(pool, event.index, pending.bitmap)
        }
        break
      }
      case 'closed':
        // The compositor dropped the surface, for example with its output.
        // Map it again on the output the geometry now selects, unless the
        // compositor keeps refusing it.
        if (this.visible && Date.now() - this.lastReshow > 1000) {
          this.lastReshow = Date.now()
          this.host.show(this.sid)
        }
        break
      case 'focus':
        if (!event.focused) this.grab = false
        this.setFocused(event.focused)
        break
      case 'key':
        if (event.pressed && !event.repeat && shortcutHandler?.(event)) break
        for (const input of keyInputEvents(event)) this.win.webContents.sendInputEvent(input)
        break
      case 'pointer':
        this.handlePointer(event.type, event.x, event.y)
        break
      case 'button':
        this.handleButton(event.button, event.pressed, event.x, event.y)
        break
      case 'axis':
        this.handleAxis(event)
        break
      case 'configure': {
        // A recreated layer surface starts empty, and a compositor need not
        // release buffers of the surface it replaced. Repaint, into a fresh
        // pool if both buffers are still held.
        const pool = this.pool
        if (pool && pool.busy[0] && pool.busy[1]) this.dropPool()
        if (this.visible && !this.win.isDestroyed()) this.win.webContents.invalidate()
        break
      }
    }
  }

  private setFocused (focused: boolean): void {
    if (this.focused === focused) return
    this.focused = focused
    if (focused) {
      focusedLayerWindow = this
    } else if (focusedLayerWindow === this) {
      focusedLayerWindow = null
    }
    if (!this.win.isDestroyed()) {
      if (focused) this.win.webContents.focus()
      else this.win.blurWebView()
      this.win.emit(focused ? 'focus' : 'blur')
    }
    for (const listener of focusListeners) listener()
  }

  private modifiers (): Modifier[] {
    const result: Modifier[] = []
    if (this.buttons.has('left')) result.push('leftButtonDown')
    if (this.buttons.has('middle')) result.push('middleButtonDown')
    if (this.buttons.has('right')) result.push('rightButtonDown')
    return result
  }

  private toPage (x: number, y: number): { x: number; y: number } {
    const zoom = this.win.webContents.getZoomFactor() || 1
    return { x: Math.round(x / zoom), y: Math.round(y / zoom) }
  }

  private handlePointer (type: 'enter' | 'leave' | 'motion', x: number, y: number): void {
    const wc = this.win.webContents
    if (type === 'leave') {
      if (pointer?.window === this) pointer = null
      this.shownCursor = ''
      if (!this.drag) wc.sendInputEvent({ type: 'mouseLeave', x: 0, y: 0 })
      return
    }
    pointer = { window: this, x, y }
    if (this.drag) {
      this.continueDrag(x, y)
      return
    }
    if (type === 'enter') {
      this.shownCursor = ''
      wc.sendInputEvent({ type: 'mouseEnter', ...this.toPage(x, y), modifiers: this.modifiers() })
    }
    this.updateCursor()
    wc.sendInputEvent({ type: 'mouseMove', ...this.toPage(x, y), modifiers: this.modifiers() })
  }

  private handleButton (button: PointerButton, pressed: boolean, x: number, y: number): void {
    const wc = this.win.webContents
    if (pressed) this.buttons.add(button)
    else this.buttons.delete(button)

    if (this.drag) {
      if (!pressed && button === 'left') this.endDrag()
      return
    }
    if (button === 'back' || button === 'forward') return

    const page = this.toPage(x, y)
    if (pressed && button === 'left') {
      const edges = this.resizeEdges(x, y)
      if (edges) {
        this.startDrag({ kind: 'resize', edges, start: { ...this.bounds }, grabX: x, grabY: y })
        return
      }
    }
    if (pressed) {
      const now = Date.now()
      const last = this.lastClick
      const repeat = last.button === button && now - last.at < DOUBLE_CLICK_MS &&
        Math.abs(last.x - x) <= DOUBLE_CLICK_DISTANCE && Math.abs(last.y - y) <= DOUBLE_CLICK_DISTANCE
      this.lastClick = { button, at: now, x, y, count: repeat ? last.count + 1 : 1 }
    }
    const event: MouseInputEvent = {
      type: pressed ? 'mouseDown' : 'mouseUp',
      button,
      clickCount: this.lastClick.count,
      ...page,
      modifiers: this.modifiers()
    }
    wc.sendInputEvent(event)
    if (pressed && button === 'left') this.probeDragRegion(x, y)
  }

  private handleAxis (event: Extract<SurfaceEvent, { ev: 'axis' }>): void {
    const wheel: MouseWheelInputEvent = {
      type: 'mouseWheel',
      ...this.toPage(event.x, event.y),
      modifiers: this.modifiers(),
      canScroll: true
    }
    if (event.discreteX || event.discreteY) {
      wheel.wheelTicksX = -event.discreteX
      wheel.wheelTicksY = -event.discreteY
      wheel.deltaX = -event.discreteX * WHEEL_TICK_PIXELS
      wheel.deltaY = -event.discreteY * WHEEL_TICK_PIXELS
    } else {
      wheel.deltaX = -event.dx
      wheel.deltaY = -event.dy
      wheel.hasPreciseScrollingDeltas = true
    }
    this.win.webContents.sendInputEvent(wheel)
  }

  /** Asks the page whether the press landed on a `-webkit-app-region: drag`
   *  area. The press was already delivered; if it was a drag area, the page
   *  gets a release and the window moves with the pointer instead. */
  private probeDragRegion (x: number, y: number): void {
    const probe = ++this.dragProbe
    const page = this.toPage(x, y)
    const script = `(() => {
      let el = document.elementFromPoint(${page.x}, ${page.y})
      while (el) {
        const style = getComputedStyle(el)
        const region = style.getPropertyValue('app-region') || style.getPropertyValue('-webkit-app-region')
        if (region === 'drag') return true
        if (region === 'no-drag') return false
        el = el.parentElement
      }
      return false
    })()`
    this.win.webContents.executeJavaScript(script, false).then((isDrag: unknown) => {
      if (isDrag !== true || probe !== this.dragProbe || !this.buttons.has('left') || this.drag) return
      if (this.win.isDestroyed()) return
      const current = pointer?.window === this ? pointer : { x, y }
      this.win.webContents.sendInputEvent({
        type: 'mouseUp',
        button: 'left',
        clickCount: 1,
        ...this.toPage(current.x, current.y)
      })
      this.startDrag({ kind: 'move', grabX: x, grabY: y })
    }, () => {})
  }

  private resizeEdges (x: number, y: number): Edges | null {
    if (this.win.isDestroyed() || !this.win.isResizable()) return null
    const edges = {
      left: x < RESIZE_BORDER,
      right: x >= this.bounds.width - RESIZE_BORDER,
      top: y < RESIZE_BORDER,
      bottom: y >= this.bounds.height - RESIZE_BORDER
    }
    return edges.left || edges.right || edges.top || edges.bottom ? edges : null
  }

  private startDrag (drag: Drag): void {
    this.drag = drag
    this.win.emit(drag.kind === 'move' ? 'will-move' : 'will-resize')
  }

  private continueDrag (x: number, y: number): void {
    const drag = this.drag
    if (!drag) return
    if (drag.kind === 'move') {
      // Pointer coordinates are relative to where the surface was when the
      // compositor sent the event, so the grab offset stays fixed.
      this.setBounds({ ...this.bounds, x: this.bounds.x + Math.round(x - drag.grabX), y: this.bounds.y + Math.round(y - drag.grabY) })
      this.win.emit('move')
      return
    }
    const [minWidth, minHeight] = this.win.getMinimumSize()
    const [maxWidth, maxHeight] = this.win.getMaximumSize()
    const limit = (value: number, min: number, max: number) => Math.min(Math.max(value, Math.max(min, 1)), max || Infinity)
    // Pointer coordinates are relative to the current origin, which moves
    // while resizing from the left or top edge.
    const pointerX = this.bounds.x + x
    const pointerY = this.bounds.y + y
    const start = drag.start
    let { x: left, y: top, width, height } = start
    if (drag.edges.right) width = limit(pointerX - start.x + (start.width - drag.grabX), minWidth, maxWidth)
    if (drag.edges.bottom) height = limit(pointerY - start.y + (start.height - drag.grabY), minHeight, maxHeight)
    if (drag.edges.left) {
      width = limit(start.x + start.width - (pointerX - drag.grabX), minWidth, maxWidth)
      left = start.x + start.width - width
    }
    if (drag.edges.top) {
      height = limit(start.y + start.height - (pointerY - drag.grabY), minHeight, maxHeight)
      top = start.y + start.height - height
    }
    this.setBounds({ x: left, y: top, width, height })
    this.win.emit('resize')
  }

  private endDrag (): void {
    const drag = this.drag
    this.drag = null
    if (!drag) return
    this.win.emit(drag.kind === 'move' ? 'moved' : 'resized')
    this.updateCursor()
  }

  private updateCursor (): void {
    let cursor = this.pageCursor
    const p = pointer?.window === this ? pointer : null
    const edges = p && !this.drag ? this.resizeEdges(p.x, p.y) : null
    if (edges) {
      const diagonal = (edges.top && edges.left) || (edges.bottom && edges.right)
        ? 'nwse-resize'
        : (edges.top && edges.right) || (edges.bottom && edges.left) ? 'nesw-resize' : null
      cursor = diagonal ?? (edges.left || edges.right ? 'ew-resize' : 'ns-resize')
    }
    if (cursor === this.shownCursor) return
    this.shownCursor = cursor
    this.host.setCursor(this.sid, cursor)
  }

  private dispose (): void {
    // Late paint events must not create pools for the destroyed surface.
    this.visible = false
    for (const off of this.unsubscribe) off()
    this.setFocused(false)
    if (pointer?.window === this) pointer = null
    layerWindows.delete(this.sid)
    this.host.destroySurface(this.sid)
    this.dropPool()
    this.unlinkUnopenedPools()
  }

  /** Global logical bounds of the surface. */
  getBounds (): Rect {
    return { ...this.bounds }
  }

  isVisible (): boolean {
    return this.visible
  }
}

const adopted = new WeakMap<BrowserWindow, LayerWindow>()
let focusedWindowPatched = false

/** Makes `win` a layer-shell surface. The window must have been created with
 *  `webPreferences.offscreen: true` and `show: false`, and must not have been
 *  shown yet. Call before wrapping its methods elsewhere. */
export function adoptLayerWindow (win: BrowserWindow, options: LayerWindowOptions = {}): LayerWindow {
  const existing = adopted.get(win)
  if (existing) return existing
  const host = WaylandHost.get()
  if (!host) throw new Error('The Wayland host is not running')
  if (!focusedWindowPatched) {
    focusedWindowPatched = true
    removeStalePools()
    const getFocusedWindow = BrowserWindow.getFocusedWindow.bind(BrowserWindow)
    BrowserWindow.getFocusedWindow = () => focusedLayerWindow?.win ?? getFocusedWindow()
  }
  const layer = new LayerWindow(win, host, options)
  adopted.set(win, layer)
  return layer
}

export function getLayerWindow (win: BrowserWindow): LayerWindow | undefined {
  return adopted.get(win)
}

export function hasFocusedLayerWindow (): boolean {
  return focusedLayerWindow !== null
}

export function onLayerFocusChange (listener: () => void): () => void {
  focusListeners.add(listener)
  return () => focusListeners.delete(listener)
}

/** Drops every keyboard grab so the compositor returns focus to a window. */
export function releaseLayerKeyboards (): void {
  for (const layer of layerWindows.values()) layer.releaseKeyboard()
}

/** Lets global shortcuts see key presses while a layer surface has keyboard
 *  focus, as X11 root grabs see them while an overlay window has focus. The
 *  handler returns true to swallow the key. */
export function setLayerShortcutHandler (handler: ((key: LayerShortcutKey) => boolean) | null): void {
  shortcutHandler = handler
}

/** Global logical pointer position while it is over a layer surface. */
export function getLayerPointer (): { x: number; y: number } | null {
  if (!pointer) return null
  const b = pointer.window.getBounds()
  return { x: b.x + pointer.x, y: b.y + pointer.y }
}
