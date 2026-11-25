import assert from 'node:assert/strict'
import { describe, it } from 'node:test'
import { type HostFeatures, nativeBackends, nativeModeUsable, windowTracking } from './host'

const WLROOTS: HostFeatures = {
  layerShell: true,
  layerShellVersion: 4,
  foreignToplevel: true,
  dataControl: true,
  cursorShape: true,
  viewporter: true,
  virtualKeyboard: true,
  uinput: false,
  virtualPointer: true,
  screencopy: true,
  kwinScript: false,
  remoteDesktop: false
}

const KWIN: HostFeatures = {
  ...WLROOTS,
  layerShellVersion: 5,
  foreignToplevel: false,
  virtualKeyboard: false,
  virtualPointer: false,
  screencopy: false,
  kwinScript: true,
  remoteDesktop: true
}

describe('windowTracking', () => {
  it('prefers foreign-toplevel management', () => {
    assert.equal(windowTracking({ ...WLROOTS, kwinScript: true }), 'foreign-toplevel')
  })

  it('uses the KWin script without foreign-toplevel management', () => {
    assert.equal(windowTracking(KWIN), 'kwin-script')
  })

  it('reports none without either, or without a host', () => {
    assert.equal(windowTracking({ ...KWIN, kwinScript: false }), null)
    assert.equal(windowTracking(null), null)
  })
})

describe('nativeBackends', () => {
  it('uses the wlroots protocols where available', () => {
    assert.deepEqual(nativeBackends(WLROOTS), {
      tracking: 'foreign-toplevel', keys: 'virtual-keyboard', pointer: 'virtual-pointer', capture: 'screencopy'
    })
  })

  it('falls back to the KWin script and the RemoteDesktop portal', () => {
    assert.deepEqual(nativeBackends(KWIN), {
      tracking: 'kwin-script', keys: 'remote-desktop', pointer: 'remote-desktop', capture: null
    })
  })

  it('types through uinput before the portal, but only without a virtual keyboard', () => {
    assert.equal(nativeBackends({ ...KWIN, uinput: true }).keys, 'uinput')
    assert.equal(nativeBackends({ ...WLROOTS, uinput: true }).keys, 'virtual-keyboard')
  })

  it('reports nothing without a host', () => {
    assert.deepEqual(nativeBackends(null), { tracking: null, keys: null, pointer: null, capture: null })
  })
})

describe('nativeModeUsable', () => {
  it('accepts wlroots compositors and KWin with both fallbacks', () => {
    assert.equal(nativeModeUsable(WLROOTS), true)
    assert.equal(nativeModeUsable(KWIN), true)
  })

  it('needs window tracking and key injection', () => {
    assert.equal(nativeModeUsable({ ...KWIN, kwinScript: false }), false)
    assert.equal(nativeModeUsable({ ...KWIN, remoteDesktop: false }), false)
    assert.equal(nativeModeUsable({ ...KWIN, remoteDesktop: false, uinput: true }), true)
  })

  it('needs layer-shell v4', () => {
    assert.equal(nativeModeUsable({ ...WLROOTS, layerShellVersion: 3 }), false)
    assert.equal(nativeModeUsable(null), false)
  })
})
