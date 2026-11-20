import assert from 'node:assert/strict'
import { describe, it } from 'node:test'
import { electronKeyCode, keyInputEvents } from './keys'

const NO_MODS = { shift: false, ctrl: false, alt: false, meta: false, caps: false, num: false }

describe('electronKeyCode', () => {
  it('names special keys the way Electron accelerators do', () => {
    assert.equal(electronKeyCode('Return', '\r'), 'Enter')
    assert.equal(electronKeyCode('Prior', ''), 'PageUp')
    assert.equal(electronKeyCode('KP_7', '7'), 'num7')
    assert.equal(electronKeyCode('F12', ''), 'F12')
  })

  it('uses lowercase letters regardless of Shift', () => {
    assert.equal(electronKeyCode('A', 'A'), 'a')
  })

  it('names shifted punctuation after its character', () => {
    assert.equal(electronKeyCode('exclam', '!'), '!')
  })

  it('returns null for keys Electron cannot name', () => {
    assert.equal(electronKeyCode('adiaeresis', 'ä'), null)
  })
})

describe('keyInputEvents', () => {
  it('types text after the key press', () => {
    assert.deepEqual(keyInputEvents({ pressed: true, repeat: false, keysym: 'A', text: 'A', mods: { ...NO_MODS, shift: true } }), [
      { type: 'keyDown', keyCode: 'a', modifiers: ['shift'] },
      { type: 'char', keyCode: 'A', modifiers: ['shift'] }
    ])
  })

  it('types characters that have no key code', () => {
    assert.deepEqual(keyInputEvents({ pressed: true, repeat: false, keysym: 'adiaeresis', text: 'ä', mods: NO_MODS }), [
      { type: 'char', keyCode: 'ä', modifiers: [] }
    ])
  })

  it('does not type text for shortcuts', () => {
    assert.deepEqual(keyInputEvents({ pressed: true, repeat: false, keysym: 'c', text: 'c', mods: { ...NO_MODS, ctrl: true } }), [
      { type: 'keyDown', keyCode: 'c', modifiers: ['control'] }
    ])
  })

  it('does not type control characters', () => {
    assert.deepEqual(keyInputEvents({ pressed: true, repeat: false, keysym: 'BackSpace', text: '\b', mods: NO_MODS }), [
      { type: 'keyDown', keyCode: 'Backspace', modifiers: [] }
    ])
  })

  it('marks repeats and sends no text on release', () => {
    assert.deepEqual(keyInputEvents({ pressed: false, repeat: true, keysym: 'x', text: 'x', mods: NO_MODS }), [
      { type: 'keyUp', keyCode: 'x', modifiers: ['isAutoRepeat'] }
    ])
  })
})
