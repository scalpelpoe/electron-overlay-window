import assert from 'node:assert/strict'
import { describe, it } from 'node:test'
import { type HostFeatures, windowTracking } from './host'

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
  kwinScript: false
}

const KWIN: HostFeatures = {
  ...WLROOTS,
  layerShellVersion: 5,
  foreignToplevel: false,
  virtualKeyboard: false,
  virtualPointer: false,
  screencopy: false,
  kwinScript: true
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
