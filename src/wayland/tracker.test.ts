import assert from 'node:assert/strict'
import { afterEach, beforeEach, describe, it, mock } from 'node:test'
import type { ToplevelInfo, WaylandHost, WaylandOutput } from './host'
import { ToplevelTracker, type TrackerEvent, x11BoundsToLogical } from './tracker'

const OUTPUTS: WaylandOutput[] = [
  { name: 'DP-1', description: '', x: 0, y: 0, width: 2560, height: 1440, scale: 1 },
  { name: 'DP-2', description: '', x: 2560, y: 0, width: 1920, height: 1080, scale: 1.5 }
]

/** Implements the subset of WaylandHost the tracker uses. */
class FakeHost {
  outputs = OUTPUTS
  toplevels = new Map<number, ToplevelInfo>()
  activated: number[] = []
  private listeners = new Set<() => void>()

  onToplevel (listener: () => void) {
    this.listeners.add(listener)
    return () => this.listeners.delete(listener)
  }

  onOutputs (listener: () => void) {
    return this.onToplevel(listener)
  }

  onRestart () {
    return () => {}
  }

  activateToplevel (tid: number) {
    this.activated.push(tid)
  }

  set (info: Partial<ToplevelInfo> & { tid: number }) {
    const previous = this.toplevels.get(info.tid)
    this.toplevels.set(info.tid, {
      title: '',
      appId: '',
      activated: false,
      fullscreen: false,
      maximized: false,
      minimized: false,
      outputs: [],
      ...previous,
      ...info
    })
    for (const listener of this.listeners) listener()
  }

  close (tid: number) {
    this.toplevels.delete(tid)
    for (const listener of this.listeners) listener()
  }
}

describe('ToplevelTracker', () => {
  let host: FakeHost
  let events: TrackerEvent[]
  let tracker: ToplevelTracker

  beforeEach(() => {
    mock.timers.enable({ apis: ['setTimeout'] })
    host = new FakeHost()
    events = []
    tracker = new ToplevelTracker(host as unknown as WaylandHost, ['Path of Exile', 'Path of Exile 2'], (e) => events.push(e))
  })

  afterEach(() => {
    tracker.stop()
    mock.timers.reset()
  })

  const types = () => events.map((e) => e.type)

  it('attaches to an activated window with a matching title, sized to its output', () => {
    host.set({ tid: 1, title: 'Path of Exile', activated: false, outputs: ['DP-2'] })
    assert.deepEqual(events, [])

    host.set({ tid: 1, activated: true, fullscreen: true })
    assert.deepEqual(events, [
      { type: 'attach', hasAccess: undefined, isFullscreen: true, titleIndex: 0, x: 2560, y: 0, width: 1920, height: 1080 },
      { type: 'focus' }
    ])
    assert.equal(tracker.targetHasFocus, true)
  })

  it('reports the title index of the matched title', () => {
    host.set({ tid: 1, title: 'Path of Exile 2', activated: true, outputs: ['DP-1'] })
    assert.equal(events[0].type === 'attach' && events[0].titleIndex, 1)
  })

  it('ignores titles that only share a prefix', () => {
    host.set({ tid: 1, title: 'Path of Exile Wiki - Browser', activated: true, outputs: ['DP-1'] })
    assert.deepEqual(events, [])
  })

  it('blurs only after the grace period when the game deactivates', () => {
    host.set({ tid: 1, title: 'Path of Exile', activated: true, outputs: ['DP-1'] })
    host.set({ tid: 1, activated: false })
    assert.deepEqual(types(), ['attach', 'focus'])

    mock.timers.tick(100)
    assert.deepEqual(types(), ['attach', 'focus', 'blur'])
    assert.equal(tracker.targetHasFocus, false)
  })

  it('does not blur when the game is reactivated within the grace period', () => {
    host.set({ tid: 1, title: 'Path of Exile', activated: true, outputs: ['DP-1'] })
    host.set({ tid: 1, activated: false })
    host.set({ tid: 1, activated: true })
    mock.timers.tick(200)
    assert.deepEqual(types(), ['attach', 'focus'])
  })

  it('follows the target after its title changes', () => {
    host.set({ tid: 1, title: 'Path of Exile', activated: true, outputs: ['DP-1'] })
    host.set({ tid: 1, title: 'Loading...' })
    host.set({ tid: 1, activated: false })
    mock.timers.tick(100)
    host.set({ tid: 1, activated: true })
    assert.deepEqual(types(), ['attach', 'focus', 'blur', 'focus'])
  })

  it('emits moveresize when the target moves to another output', () => {
    host.set({ tid: 1, title: 'Path of Exile', activated: true, outputs: ['DP-1'] })
    host.set({ tid: 1, outputs: ['DP-2'] })
    assert.deepEqual(events[events.length - 1], { type: 'moveresize', x: 2560, y: 0, width: 1920, height: 1080 })
  })

  it('uses reported geometry over the output bounds', () => {
    host.set({ tid: 1, title: 'Path of Exile', activated: true, outputs: ['DP-1'], geometry: { x: 100, y: 50, width: 1280, height: 720 } })
    host.set({ tid: 1, geometry: { x: 200, y: 50, width: 1280, height: 720 } })
    assert.deepEqual(events, [
      { type: 'attach', hasAccess: undefined, isFullscreen: false, titleIndex: 0, x: 100, y: 50, width: 1280, height: 720 },
      { type: 'focus' },
      { type: 'moveresize', x: 200, y: 50, width: 1280, height: 720 }
    ])
  })

  it('falls back to the output for empty geometry', () => {
    host.set({ tid: 1, title: 'Path of Exile', activated: true, outputs: ['DP-2'], geometry: { x: 0, y: 0, width: 0, height: 0 } })
    assert.deepEqual(events[0], { type: 'attach', hasAccess: undefined, isFullscreen: false, titleIndex: 0, x: 2560, y: 0, width: 1920, height: 1080 })
  })

  it('emits fullscreen changes', () => {
    host.set({ tid: 1, title: 'Path of Exile', activated: true, outputs: ['DP-1'] })
    host.set({ tid: 1, fullscreen: true })
    assert.deepEqual(events[events.length - 1], { type: 'fullscreen', isFullscreen: true })
  })

  it('blurs and detaches when the target closes', () => {
    host.set({ tid: 1, title: 'Path of Exile', activated: true, outputs: ['DP-1'] })
    host.close(1)
    assert.deepEqual(types(), ['attach', 'focus', 'blur', 'detach'])
  })

  it('detaches and finds the new title after a retarget', () => {
    host.set({ tid: 1, title: 'Path of Exile', activated: true, outputs: ['DP-1'] })
    host.set({ tid: 2, title: 'Path of Exile 2', activated: false, outputs: ['DP-1'] })
    tracker.setTitles(['Path of Exile 2'])
    assert.deepEqual(types(), ['attach', 'focus', 'blur', 'detach'])

    host.set({ tid: 1, activated: false })
    host.set({ tid: 2, activated: true })
    assert.deepEqual(types(), ['attach', 'focus', 'blur', 'detach', 'attach', 'focus'])
  })

  it('only activates the target while the game context is active', () => {
    host.set({ tid: 1, title: 'Path of Exile', activated: true, outputs: ['DP-1'] })
    tracker.focusTarget()
    host.set({ tid: 1, activated: false })
    mock.timers.tick(100)
    tracker.focusTarget()
    assert.deepEqual(host.activated, [1])
  })
})

describe('x11BoundsToLogical', () => {
  it('maps a fullscreen window in logical X11 pixels to its output', () => {
    assert.deepEqual(x11BoundsToLogical({ x: 2560, y: 0, width: 1920, height: 1080 }, OUTPUTS), {
      x: 2560, y: 0, width: 1920, height: 1080
    })
  })

  it('maps a fullscreen window in physical X11 pixels to its output', () => {
    assert.deepEqual(x11BoundsToLogical({ x: 3840, y: 0, width: 2880, height: 1620 }, OUTPUTS), {
      x: 2560, y: 0, width: 1920, height: 1080
    })
  })

  it('matches by size when X11 positions follow another layout', () => {
    assert.deepEqual(x11BoundsToLogical({ x: 0, y: 0, width: 2880, height: 1620 }, OUTPUTS), {
      x: 2560, y: 0, width: 1920, height: 1080
    })
  })

  it('leaves windowed bounds unchanged', () => {
    const windowed = { x: 100, y: 100, width: 1280, height: 720 }
    assert.deepEqual(x11BoundsToLogical(windowed, OUTPUTS), windowed)
  })
})
