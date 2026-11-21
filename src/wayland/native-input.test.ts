import assert from 'node:assert/strict'
import { beforeEach, describe, it } from 'node:test'
import type { EvdevEvent, WaylandHost } from './host'
import { listenNativeInput, type NativeInputEvent } from './native-input'

const KEY_C = 46
const BTN_LEFT = 0x110

describe('listenNativeInput', () => {
  let send: (event: EvdevEvent) => void
  let active: boolean
  let events: NativeInputEvent[]

  beforeEach(() => {
    active = true
    events = []
    const host = {
      onEvdev (listener: (event: EvdevEvent) => void) {
        send = listener
        return () => {}
      }
    }
    listenNativeInput(host as unknown as WaylandHost, () => active, (e) => events.push(e))
  })

  it('delivers presses and autorepeat while the target is active', () => {
    send({ kind: 'key', code: KEY_C, value: 1 })
    send({ kind: 'key', code: KEY_C, value: 2 })
    assert.deepEqual(events, [
      { type: 'keydown', code: KEY_C, repeat: false },
      { type: 'keydown', code: KEY_C, repeat: true }
    ])
  })

  it('drops presses but keeps releases while the target is inactive', () => {
    active = false
    send({ kind: 'key', code: KEY_C, value: 1 })
    send({ kind: 'key', code: BTN_LEFT, value: 1 })
    send({ kind: 'wheel', value: 1, horizontal: false })
    send({ kind: 'key', code: KEY_C, value: 0 })
    send({ kind: 'key', code: BTN_LEFT, value: 0 })
    assert.deepEqual(events, [
      { type: 'keyup', code: KEY_C, repeat: false },
      { type: 'mouseup', code: BTN_LEFT }
    ])
  })

  it('reports buttons as mouse events without autorepeat', () => {
    send({ kind: 'key', code: BTN_LEFT, value: 1 })
    send({ kind: 'key', code: BTN_LEFT, value: 2 })
    send({ kind: 'wheel', value: -1, horizontal: true })
    assert.deepEqual(events, [
      { type: 'mousedown', code: BTN_LEFT },
      { type: 'wheel', delta: -1, horizontal: true }
    ])
  })
})
