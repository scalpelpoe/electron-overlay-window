import type { InputEvent, KeyboardInputEvent } from 'electron'
import type { KeyMods } from './host'

type Modifier = NonNullable<InputEvent['modifiers']>[number]

/** xkb keysym names that do not map to a printable character, as Electron
 *  accelerator key codes accepted by `webContents.sendInputEvent`. */
const NAMED_KEYS: Record<string, string> = {
  Return: 'Enter',
  KP_Enter: 'Enter',
  BackSpace: 'Backspace',
  Tab: 'Tab',
  ISO_Left_Tab: 'Tab',
  Escape: 'Escape',
  space: 'Space',
  Delete: 'Delete',
  KP_Delete: 'Delete',
  Insert: 'Insert',
  KP_Insert: 'Insert',
  Home: 'Home',
  KP_Home: 'Home',
  End: 'End',
  KP_End: 'End',
  Prior: 'PageUp',
  KP_Prior: 'PageUp',
  Next: 'PageDown',
  KP_Next: 'PageDown',
  Left: 'Left',
  KP_Left: 'Left',
  Right: 'Right',
  KP_Right: 'Right',
  Up: 'Up',
  KP_Up: 'Up',
  Down: 'Down',
  KP_Down: 'Down',
  Shift_L: 'Shift',
  Shift_R: 'Shift',
  Control_L: 'Control',
  Control_R: 'Control',
  Alt_L: 'Alt',
  Alt_R: 'Alt',
  ISO_Level3_Shift: 'AltGr',
  Super_L: 'Super',
  Super_R: 'Super',
  Meta_L: 'Meta',
  Meta_R: 'Meta',
  Caps_Lock: 'Capslock',
  Num_Lock: 'Numlock',
  Scroll_Lock: 'Scrolllock',
  Print: 'PrintScreen',
  KP_Add: 'numadd',
  KP_Subtract: 'numsub',
  KP_Multiply: 'nummult',
  KP_Divide: 'numdiv',
  KP_Decimal: 'numdec',
  KP_Separator: 'numdec',
  plus: 'Plus',
  minus: '-',
  equal: '=',
  comma: ',',
  period: '.',
  slash: '/',
  backslash: '\\',
  semicolon: ';',
  apostrophe: "'",
  grave: '`',
  bracketleft: '[',
  bracketright: ']',
  XF86AudioPlay: 'MediaPlayPause',
  XF86AudioStop: 'MediaStop',
  XF86AudioNext: 'MediaNextTrack',
  XF86AudioPrev: 'MediaPreviousTrack',
  XF86AudioRaiseVolume: 'VolumeUp',
  XF86AudioLowerVolume: 'VolumeDown',
  XF86AudioMute: 'VolumeMute'
}

/** Electron key code for an xkb keysym, or null when Electron has no name
 *  for it. Letters are lowercase: Electron derives Shift from an uppercase
 *  code, and the modifiers already carry it. */
export function electronKeyCode (keysym: string, text: string): string | null {
  const named = NAMED_KEYS[keysym]
  if (named) return named
  if (/^F([1-9]|1\d|2[0-4])$/.test(keysym)) return keysym
  if (/^KP_\d$/.test(keysym)) return `num${keysym.slice(3)}`
  if (/^[a-zA-Z0-9]$/.test(keysym)) return keysym.toLowerCase()
  // Shifted punctuation (exclam, at, ...) is named after the character it types.
  if (text.length === 1 && text >= ' ' && text <= '~') return text.toLowerCase()
  return null
}

export function electronModifiers (mods: KeyMods): Modifier[] {
  const result: Modifier[] = []
  if (mods.shift) result.push('shift')
  if (mods.ctrl) result.push('control')
  if (mods.alt) result.push('alt')
  if (mods.meta) result.push('meta')
  if (mods.caps) result.push('capsLock')
  if (mods.num) result.push('numLock')
  return result
}

/** Translates one host key event into the Electron events that type it. */
export function keyInputEvents (
  key: { pressed: boolean; repeat: boolean; keysym: string; text: string; mods: KeyMods }
): KeyboardInputEvent[] {
  const modifiers = electronModifiers(key.mods)
  if (key.repeat) modifiers.push('isAutoRepeat')
  const keyCode = electronKeyCode(key.keysym, key.text)
  const events: KeyboardInputEvent[] = []
  if (keyCode) events.push({ type: key.pressed ? 'keyDown' : 'keyUp', keyCode, modifiers })
  // Text is only produced for plain or shifted keys; Ctrl/Alt/Meta chords are
  // shortcuts handled from keyDown.
  const typesText = key.pressed && key.text && !key.mods.ctrl && !key.mods.alt && !key.mods.meta
  if (typesText) {
    for (const ch of key.text) {
      if (ch >= ' ' && ch !== '\x7f') events.push({ type: 'char', keyCode: ch, modifiers })
    }
  }
  return events
}
