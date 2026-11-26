import { EventEmitter } from 'node:events'
import { join } from 'node:path'
import { throttle } from 'throttle-debounce'
import { screen } from 'electron'
import { BrowserWindow, Rectangle, BrowserWindowConstructorOptions, WebPreferences } from 'electron'
import { layerShellUsable, probeWaylandHost, WaylandHost } from './wayland/host'
import { adoptLayerWindow, releaseLayerKeyboards } from './wayland/layer-window'
import { FocusArbiter, ToplevelTracker, type TrackerEvent, x11BoundsToLogical } from './wayland/tracker'
import { captureScreenRegion, listenNativeInput, type NativeInputEvent } from './wayland/native-input'
const lib: AddonExports = require('node-gyp-build')(join(__dirname, '..'))

export { layerShellUsable, probeWaylandHost, type HostFeatures, type WaylandOutput } from './wayland/host'
export {
  adoptLayerWindow,
  getLayerWindow,
  getLayerPointer,
  releaseLayerKeyboards,
  type LayerWindow,
  type LayerWindowOptions
} from './wayland/layer-window'
export { setLayerShortcutHandler, type LayerShortcutKey } from './wayland/layer-window'
export type { NativeInputEvent } from './wayland/native-input'
export { FocusArbiter, type TrackerEvent } from './wayland/tracker'

/** Follows the target for the layer-shell backend. */
export interface LayerTracker {
  setTitles(titles: string[]): void
  clear(): void
  focusTarget(): void
  stop(): void
}

/** Builds an app's own layer-shell tracker, for example one that reads exact
 *  geometry from a compositor IPC. It reports through `emit`. */
export type LayerTrackerFactory = (titles: string[], emit: (event: TrackerEvent) => void) => LayerTracker

interface AddonExports {
  start(
    overlayWindowId: Buffer | undefined,
    targetWindowTitles: string[],
    cb: (e: any) => void
  ): void

  activateOverlay(): void
  focusTarget(): void
  screenshot(): Buffer
  setTargetTitles(titles: string[]): void
  clearTarget(): void
  stopHook(): void
}

enum EventType {
  EVENT_ATTACH = 1,
  EVENT_FOCUS = 2,
  EVENT_BLUR = 3,
  EVENT_DETACH = 4,
  EVENT_FULLSCREEN = 5,
  EVENT_MOVERESIZE = 6,
}

export interface AttachEvent {
  hasAccess: boolean | undefined
  isFullscreen: boolean | undefined
  titleIndex: number
  x: number
  y: number
  width: number
  height: number
}

export interface FullscreenEvent {
  isFullscreen: boolean
}

export interface MoveresizeEvent {
  x: number
  y: number
  width: number
  height: number
}

export interface AttachOptions {
  // Whether the Window has a title bar. We adjust the overlay to not cover it
  hasTitleBarOnMac?: boolean
}

const isMac = process.platform === 'darwin'
const isLinux = process.platform === 'linux'

export const OVERLAY_WINDOW_OPTS: BrowserWindowConstructorOptions = {
  fullscreenable: true,
  skipTaskbar: true,
  frame: false,
  show: false,
  transparent: true,
  resizable: true,
  // disable shadow for Mac OS
  hasShadow: !isMac,
  // float above all windows on Mac OS
  alwaysOnTop: isMac
}

/** Web preferences a window needs before `adoptLayerWindow` can present it
 *  as a layer-shell surface. Merge into the window's own webPreferences. */
export const LAYER_SHELL_WEB_PREFERENCES: WebPreferences = {
  offscreen: true,
  // Hidden offscreen windows would otherwise throttle timers and rAF.
  backgroundThrottling: false
}

/**
 * Input, pointer and capture for the target without any X server, for apps
 * using the layer-shell backend. Each member returns null or false where the
 * compositor or permissions do not allow it.
 */
export const waylandNative = {
  /** Starts reading evdev (needs read access to /dev/input, usually the
   *  `input` group). See `NativeInputEvent` for what is delivered when. */
  listen (listener: (event: NativeInputEvent) => void): (() => void) | null {
    const host = WaylandHost.get()
    if (!host) return null
    return listenNativeInput(host, () => OverlayController.targetHasFocus, listener)
  },
  /** Number of evdev devices being read; 0 usually means no permission. */
  inputDevices (): number {
    return WaylandHost.get()?.evdevDevices ?? 0
  },
  /** Presses or releases an evdev key, delivered to the focused window.
   *  Without a virtual keyboard (KWin) this needs write access to
   *  /dev/uinput, and keys combine with physically held modifiers. */
  key (code: number, pressed: boolean): boolean {
    const host = WaylandHost.get()
    if (!host?.features?.virtualKeyboard && !host?.features?.uinput) return false
    host.virtualKey(code, pressed)
    return true
  },
  warpPointer (point: { x: number; y: number }): boolean {
    const host = WaylandHost.get()
    if (!host?.features?.virtualPointer) return false
    host.warpPointer(point.x, point.y)
    return true
  },
  probePointer (): Promise<{ x: number; y: number } | null> {
    return WaylandHost.get()?.probePointer() ?? Promise.resolve(null)
  },
  canCapture (): boolean {
    return !!WaylandHost.get()?.features?.screencopy
  },
  /** BGRA pixels of a logical screen rectangle, overlays included. */
  capture (rect: Rectangle): Promise<{ width: number; height: number; data: Buffer } | null> {
    const host = WaylandHost.get()
    return host?.features?.screencopy ? captureScreenRegion(host, rect) : Promise.resolve(null)
  }
}

/** Clipboard through the Wayland host's data-control connection. Chromium
 *  only sees the Wayland clipboard while one of its own windows is focused,
 *  which an overlay over a game never is. */
export const waylandClipboard = {
  available (): boolean {
    return !!WaylandHost.get()?.features?.dataControl
  },
  readText (): string {
    return WaylandHost.get()?.clipboardText ?? ''
  },
  readHTML (): string {
    return WaylandHost.get()?.clipboardHtml ?? ''
  },
  writeText (text: string): void {
    WaylandHost.get()?.writeClipboard(text)
  },
  write (data: { text?: string; html?: string }): void {
    WaylandHost.get()?.writeClipboard(data.text ?? '', data.html)
  },
  clear (): void {
    WaylandHost.get()?.clearClipboard()
  }
}

class OverlayControllerGlobal {
  private isInitialized = false
  private electronWindow?: BrowserWindow
  // Exposed so that apps can get the current bounds of the target
  // NOTE: stores screen physical rect on Windows and XWayland
  targetBounds: Rectangle = { x: 0, y: 0, width: 0, height: 0 }
  targetHasFocus = false
  private focusNext: 'overlay' | 'target' | undefined
  // The height of a title bar on a standard window. Only measured on Mac
  private macTitleBarHeight = 0
  private attachOptions: AttachOptions = {}
  private layerShell = false
  private tracker: LayerTracker | null = null
  private createLayerTracker: LayerTrackerFactory | undefined
  private x11Arbiter: FocusArbiter | null = null
  // Target titles until the layer-shell tracker starts.
  private layerTitles: string[] = []

  readonly events = new EventEmitter()

  constructor () {
    this.events.on('attach', (e: AttachEvent) => {
      this.targetHasFocus = true
      if (this.electronWindow) {
        this.electronWindow.setIgnoreMouseEvents(true)
        this.electronWindow.showInactive()
        this.electronWindow.setAlwaysOnTop(true, 'screen-saver')
      }
      if (e.isFullscreen !== undefined) {
        this.handleFullscreen(e.isFullscreen)
      }
      this.targetBounds = e
      this.updateOverlayBounds()
    })

    this.events.on('fullscreen', (e: FullscreenEvent) => {
      this.handleFullscreen(e.isFullscreen)
    })

    this.events.on('detach', () => {
      this.targetHasFocus = false
      this.electronWindow?.hide()
    })

    const dispatchMoveresize = throttle(34 /* 30fps */, this.updateOverlayBounds.bind(this))

    this.events.on('moveresize', (e: MoveresizeEvent) => {
      this.targetBounds = e
      dispatchMoveresize()
    })

    this.events.on('blur', () => {
      this.targetHasFocus = false

      if (this.electronWindow && (isMac ||
        this.focusNext !== 'overlay' && !this.electronWindow.isFocused()
      )) {
        this.electronWindow.hide()
      }
    })

    this.events.on('focus', () => {
      this.focusNext = undefined
      this.targetHasFocus = true

      if (this.electronWindow) {
        this.electronWindow.setIgnoreMouseEvents(true)
        if (!this.electronWindow.isVisible()) {
          this.electronWindow.showInactive()
          this.electronWindow.setAlwaysOnTop(true, 'screen-saver')
        }
      }
    })
  }

  private async handleFullscreen(isFullscreen: boolean) {
    if (!this.electronWindow) return

    if (isMac) {
      // On Mac, only a single app can be fullscreen, so we can't go
      // fullscreen. We get around it by making it display on all workspaces,
      // based on code from:
      // https://github.com/electron/electron/issues/10078#issuecomment-754105005
      this.electronWindow.setVisibleOnAllWorkspaces(isFullscreen, { visibleOnFullScreen: true })
      if (isFullscreen) {
        const display = screen.getPrimaryDisplay()
        this.electronWindow.setBounds(display.bounds)
      } else {
        // Set it back to `lastBounds` as set before fullscreen
        this.updateOverlayBounds();
      }
    }
  }

  private updateOverlayBounds () {
    let lastBounds = this.adjustBoundsForMacTitleBar(this.targetBounds)
    if (lastBounds.width === 0 || lastBounds.height === 0) return
    if (!this.electronWindow) return

    if (this.layerShell) {
      // Layer-shell targets are tracked in logical coordinates already.
    } else if (process.platform === 'win32') {
      lastBounds = screen.screenToDipRect(this.electronWindow, this.targetBounds)
    } else if (isLinux) {
      // XWayland reports physical pixels; convert to DIP (logical) for Electron
      const display = screen.getDisplayNearestPoint({ x: lastBounds.x, y: lastBounds.y })
      const scaleFactor = display.scaleFactor || 1
      lastBounds = {
        x: Math.round(lastBounds.x / scaleFactor),
        y: Math.round(lastBounds.y / scaleFactor),
        width: Math.round(lastBounds.width / scaleFactor),
        height: Math.round(lastBounds.height / scaleFactor)
      }
    }
    this.electronWindow.setBounds(lastBounds)

    // if moved to screen with different DPI, 2nd call to setBounds will correctly resize window
    // dipRect must be recalculated as well
    if (process.platform === 'win32') {
      lastBounds = screen.screenToDipRect(this.electronWindow, this.targetBounds)
      this.electronWindow.setBounds(lastBounds)
    }
  }

  private handler (e: unknown) {
    if (this.x11Arbiter) {
      this.arbitrateX11Event(e as { type: EventType }, this.x11Arbiter)
      return
    }
    try {
      switch ((e as { type: EventType }).type) {
        case EventType.EVENT_ATTACH:
          this.events.emit('attach', e)
          break
        case EventType.EVENT_FOCUS:
          this.events.emit('focus', e)
          break
        case EventType.EVENT_BLUR:
          this.events.emit('blur', e)
          break
        case EventType.EVENT_DETACH:
          this.events.emit('detach', e)
          break
        case EventType.EVENT_FULLSCREEN:
          this.events.emit('fullscreen', e)
          break
        case EventType.EVENT_MOVERESIZE:
          this.events.emit('moveresize', e)
          break
      }
    } catch (err) {
      console.error('[electron-overlay-window] handler error:', err)
    }
  }

  /** X11 target tracking while the overlay is a layer surface: keyboard focus
   *  in the overlay deactivates the X11 target, which must not hide it. */
  private arbitrateX11Event (e: { type: EventType }, arbiter: FocusArbiter) {
    const logical = <T extends Rectangle>(event: T): T =>
      ({ ...event, ...x11BoundsToLogical(event, WaylandHost.get()?.outputs ?? []) })
    switch (e.type) {
      case EventType.EVENT_ATTACH:
        arbiter.reset(false)
        this.events.emit('attach', logical(e as AttachEvent & { type: EventType }))
        break
      case EventType.EVENT_DETACH:
        arbiter.reset(false)
        this.events.emit('detach', e)
        break
      case EventType.EVENT_FOCUS:
        arbiter.setGameFocused(true)
        break
      case EventType.EVENT_BLUR:
        arbiter.setGameFocused(false)
        break
      case EventType.EVENT_FULLSCREEN:
        this.events.emit('fullscreen', e)
        break
      case EventType.EVENT_MOVERESIZE:
        this.events.emit('moveresize', logical(e as MoveresizeEvent & { type: EventType }))
        break
    }
  }

  private emitTrackerEvent (event: TrackerEvent) {
    const { type, ...payload } = event
    try {
      this.events.emit(type, payload)
    } catch (err) {
      console.error('[electron-overlay-window] handler error:', err)
    }
  }

  /**
   * Presents overlays as wlr-layer-shell surfaces through the Wayland host
   * (see src/lib/wayland/README.md). For Electron running with
   * `--ozone-platform=wayland`; call before `attachByTitle`. Overlay windows
   * must be created with `LAYER_SHELL_WEB_PREFERENCES`. Returns false when the
   * compositor lacks layer-shell v4 or the host is not installed.
   * `createTracker` replaces the built-in target tracking.
   */
  useLayerShell (createTracker?: LayerTrackerFactory): boolean {
    if (this.isInitialized) throw new Error('Call useLayerShell before attaching.')
    if (!isLinux || !layerShellUsable(probeWaylandHost())) return false
    this.layerShell = WaylandHost.start() !== null
    this.createLayerTracker = createTracker
    return this.layerShell
  }

  get isLayerShell (): boolean {
    return this.layerShell
  }

  /**
   * Create a dummy window to calculate the title bar height on Mac. We use
   * the title bar height to adjust the size of the overlay to not overlap
   * the title bar. This helps Mac match the behaviour on Windows/Linux.
   */
  private calculateMacTitleBarHeight () {
    const testWindow = new BrowserWindow({
      width: 400,
      height: 300,
      webPreferences: {
        nodeIntegration: true
      },
      show: false,
    })
    const fullHeight = testWindow.getSize()[1]
    const contentHeight = testWindow.getContentSize()[1]
    this.macTitleBarHeight = fullHeight - contentHeight
    testWindow.close()
  }

  /** If we're on a Mac, adjust the bounds to not overlap the title bar */
  private adjustBoundsForMacTitleBar (bounds: Rectangle) {
    if (!isMac || !this.attachOptions.hasTitleBarOnMac) {
      return bounds
    }

    const newBounds: Rectangle = {
      ...bounds,
      y: bounds.y + this.macTitleBarHeight,
      height: bounds.height - this.macTitleBarHeight
    }
    return newBounds
  }

  activateOverlay () {
    if (!this.electronWindow) {
      throw new Error('You are using the library in tracking mode')
    }
    this.focusNext = 'overlay'
    this.electronWindow.setIgnoreMouseEvents(false)
    if (this.layerShell) {
      this.electronWindow.focus()
    } else if (isLinux) {
      lib.activateOverlay()
    } else {
      this.electronWindow.focus()
    }
  }

  focusTarget () {
    this.focusNext = 'target'
    this.electronWindow?.setIgnoreMouseEvents(true)
    if (this.layerShell) {
      releaseLayerKeyboards()
      this.tracker?.focusTarget()
      return
    }
    lib.focusTarget()
  }

  attachByTitle (electronWindow: BrowserWindow | undefined, targetWindowTitle: string, options: AttachOptions = {}) {
    this.attachByTitles(electronWindow, [targetWindowTitle], options)
  }

  attachByTitles (electronWindow: BrowserWindow | undefined, targetWindowTitles: string[], options: AttachOptions = {}) {
    if (this.isInitialized) {
      throw new Error('Library can be initialized only once.')
    } else {
      this.isInitialized = true
    }
    this.electronWindow = electronWindow

    this.electronWindow?.on('blur', () => {
      if (!this.targetHasFocus && this.focusNext !== 'target') {
        this.electronWindow!.hide()
      }
    })

    this.electronWindow?.on('focus', () => {
      this.focusNext = undefined
    })

    this.attachOptions = options
    if (isMac) {
      this.calculateMacTitleBarHeight()
    }

    if (this.layerShell) {
      this.attachLayerShell(targetWindowTitles)
      return
    }

    lib.start(
      this.electronWindow?.getNativeWindowHandle(),
      targetWindowTitles,
      this.handler.bind(this))
  }

  private attachLayerShell (titles: string[]) {
    const host = WaylandHost.get()!
    if (this.electronWindow) adoptLayerWindow(this.electronWindow)
    this.layerTitles = titles
    void host.ready().then(() => {
      titles = this.layerTitles
      if (!host.features) {
        console.error('[electron-overlay-window] the Wayland host did not start; the target is not tracked')
        return
      }
      if (this.createLayerTracker) {
        this.tracker = this.createLayerTracker(titles, (event) => this.emitTrackerEvent(event))
        return
      }
      if (host.features.foreignToplevel) {
        this.tracker = new ToplevelTracker(host, titles, (event) => this.emitTrackerEvent(event))
        return
      }
      // KWin and others without foreign-toplevel management: follow an
      // XWayland target through the X11 backend in tracking-only mode.
      this.x11Arbiter = new FocusArbiter((event) => this.emitTrackerEvent(event))
      lib.start(undefined, titles, this.handler.bind(this))
    })
  }

  /** Whether the layer-shell backend has not chosen its tracker yet. */
  private get layerTrackerPending (): boolean {
    return this.layerShell && !this.tracker && !this.x11Arbiter
  }

  setTargetTitles (titles: string[]) {
    if (this.layerTrackerPending) this.layerTitles = titles
    else if (this.tracker) this.tracker.setTitles(titles)
    else lib.setTargetTitles(titles)
  }

  clearTarget () {
    if (this.layerTrackerPending) this.layerTitles = []
    else if (this.tracker) this.tracker.clear()
    else lib.clearTarget()
  }

  stopHook () {
    if (this.tracker) {
      this.tracker.stop()
      this.tracker = null
      return
    }
    this.x11Arbiter?.stop()
    lib.stopHook()
  }

  // buffer suitable for use in `nativeImage.createFromBitmap`
  screenshot (): Buffer {
    if (process.platform !== 'win32') {
      throw new Error('Not implemented on your platform.')
    }
    return lib.screenshot()
  }
}

export const OverlayController = new OverlayControllerGlobal()
