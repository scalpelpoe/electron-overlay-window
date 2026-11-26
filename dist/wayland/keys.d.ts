import type { InputEvent, KeyboardInputEvent } from 'electron';
import type { KeyMods } from './host';
type Modifier = NonNullable<InputEvent['modifiers']>[number];
/** Electron key code for an xkb keysym, or null when Electron has no name
 *  for it. Letters are lowercase: Electron derives Shift from an uppercase
 *  code, and the modifiers already carry it. */
export declare function electronKeyCode(keysym: string, text: string): string | null;
export declare function electronModifiers(mods: KeyMods): Modifier[];
/** Translates one host key event into the Electron events that type it. */
export declare function keyInputEvents(key: {
    pressed: boolean;
    repeat: boolean;
    keysym: string;
    text: string;
    mods: KeyMods;
}): KeyboardInputEvent[];
export {};
