# Wayland layer-shell host

`overlay-wayland-host` presents Electron overlay windows as
`zwlr_layer_shell_v1` surfaces on any compositor that implements layer-shell
(wlroots-based compositors, Hyprland, niri, KWin, COSMIC, ...). GNOME does not.

Chromium only creates `xdg_toplevel` windows, which compositors may tile, move
behind a fullscreen game, or refuse to position. Electron also links its own
copy of libwayland-client into the executable and exports its symbols, so
Wayland code inside the Node addon would be bound to that copy. The host is
therefore a separate process with its own Wayland connection:

- Electron renders overlay windows offscreen (`webPreferences.offscreen`).
- `src/wayland/` copies each painted frame into a shared memory file and
  forwards pointer and keyboard events back into the page with
  `webContents.sendInputEvent`.
- The host tracks toplevels (`zwlr_foreign_toplevel_manager_v1`) to find the
  target window, and mirrors the clipboard (`ext_data_control_v1` or
  `zwlr_data_control_v1`) so Electron can read and write it without focus.
- For input directed at the target, none of which goes through XWayland: the
  host reads keys and buttons from evdev (`/dev/input`, which needs the
  `input` group, since no Wayland protocol exposes another client's input),
  types with `zwp_virtual_keyboard_v1`, moves the pointer with
  `zwlr_virtual_pointer_v1`, captures with `zwlr_screencopy_manager_v1`, and
  finds the pointer by briefly mapping an input-only surface over every output.

`protocols/` holds the protocol XML, copied unchanged from wayland-protocols,
wlr-protocols and wlroots. The build generates the C glue from it with
`wayland-scanner`, so the host needs `wayland-scanner` in addition to the
`wayland-client`, `wayland-cursor` and `xkbcommon` development files.

## Wire protocol

The host reads commands from stdin and writes events to stdout, one per line.
stdin EOF terminates the host. Diagnostics go to stderr.

`overlay-wayland-host --probe` prints the `ready` event and exits with status 0
when layer-shell is available, 1 when it is not, and 2 when no Wayland
compositor could be reached.

### Coordinates

- Surface geometry uses global logical coordinates, the space of
  `zxdg_output_v1` logical positions. Electron's `screen` uses the same space
  under `--ozone-platform=wayland`.
- Pointer positions and input regions are surface-local logical coordinates.
- Buffers and damage are in buffer pixels. The host sets the
  `wp_viewport` destination to the logical size, so a buffer rendered at the
  output scale is shown sharp.

### Commands

Fields are separated by one space. `<sid>` is a surface id chosen by the
client, `<tid>` a toplevel id chosen by the host. Text payloads are base64.

| Command | Meaning |
| --- | --- |
| `create <sid>` | Register a surface. It starts hidden. |
| `destroy <sid>` | Unmap and forget a surface and its buffers. |
| `geometry <sid> <x> <y> <w> <h>` | Logical rectangle. A visible surface moves to the output containing the rectangle's center, recreating its layer surface if that output changed. |
| `keyboard <sid> none\|on_demand\|exclusive` | Keyboard interactivity. Below layer-shell v4, `on_demand` maps to `none`. |
| `input <sid> all` | Whole surface accepts pointer input. |
| `input <sid> none` | Surface is click-through. |
| `input <sid> <x>,<y>,<w>,<h> ...` | Only these rectangles accept pointer input. |
| `layer <sid> background\|bottom\|top\|overlay` | Layer of the surface, `overlay` by default. Applied with `set_layer` from layer-shell v2, else by recreating a visible surface. |
| `show <sid>` | Create the layer surface on the current layer. A buffer committed before the first configure is presented then; otherwise the surface stays empty until the next commit. |
| `hide <sid>` | Destroy the layer surface and `wl_surface`. Attached buffers stay busy until the compositor releases them. |
| `raise <sid>` | Recreate a visible surface so it stacks above the other layer surfaces. |
| `buffers <sid> <pool> <path> <w> <h>` | Replace the buffer pool with pool id `pool`. The file holds two ARGB8888 premultiplied buffers of `w*h*4` bytes with stride `w*4`; buffer `i` starts at `i*w*h*4`. |
| `commit <sid> <pool> <index> <x> <y> <w> <h>` | Present buffer `index` of pool `pool` with this damage rectangle. Before the first configure it is held and presented then; a newer commit replaces it and releases the held buffer. A commit to a pool that is not current is released immediately. |
| `cursor <sid> <name>` | CSS cursor name, as reported by Electron's `cursor-changed` event. `none` hides the cursor. |
| `activate <tid>` | Activate a toplevel with the host's seat. |
| `clipboard-set <text> <html\|->` | Own the selection with text, and optionally HTML. |
| `clipboard-clear` | Clear the selection. |
| `evdev on\|off` | Start or stop reading keyboards and mice from `/dev/input`. Devices that appear later are opened too. |
| `vkey <code> <0\|1>` | Release or press evdev key `code` on a virtual keyboard that uses the seat's keymap. The host keeps its modifier state. |
| `warp <x> <y>` | Move the pointer to a global logical position with a virtual pointer. |
| `capture <id> <x> <y> <w> <h> <path>` | Capture a global logical rectangle, which must lie within one output, without the cursor, into a new file at `path` (the rest of the line). |
| `probe <id>` | Find the pointer: map a transparent input surface over every output on the overlay layer until one receives `wl_pointer.enter`, then remove them. |

### Events

JSON objects with an `ev` field.

| Event | Fields |
| --- | --- |
| `ready` | `layerShell`, `layerShellVersion`, `foreignToplevel`, `dataControl`, `cursorShape`, `viewporter`, `virtualKeyboard`, `virtualPointer`, `screencopy`: availability of each global. Sent once after the initial roundtrips. |
| `outputs` | `outputs`: array of `{ name, description, x, y, width, height, scale }` in logical coordinates. Sent after `ready` and whenever an output changes. |
| `configure` | `sid`, `width`, `height`: the size the compositor assigned. Sent for every new layer surface, which starts empty unless a buffer was held for it, and when the size changes. |
| `closed` | `sid`: the compositor closed the layer surface, for example because its output disappeared. The surface is hidden. |
| `buffers` | `sid`, `pool`, `path`: the pool was created and the file may be unlinked. |
| `release` | `sid`, `pool`, `index`: the buffer may be written again. Sent when the compositor releases an attached buffer, or when the host returns one it never attached. |
| `pointer` | `sid`, `type` (`enter`, `leave`, `motion`), `x`, `y`. |
| `button` | `sid`, `button` (`left`, `right`, `middle`, `back`, `forward`), `pressed`, `x`, `y`. |
| `axis` | `sid`, `x`, `y`, `dx`, `dy`, `discreteX`, `discreteY`. Sent on `wl_pointer.frame`. Deltas are in surface units, positive meaning down and right; discrete steps are wheel clicks (`value120 / 120`), 0 for continuous scrolling. |
| `focus` | `sid`, `focused`: keyboard focus entered or left. |
| `key` | `sid`, `pressed`, `repeat`, `code` (evdev key code), `keysym` (xkb keysym name), `sym` (its numeric value), `text` (UTF-8 produced by the key, empty for none), `mods` (`{ shift, ctrl, alt, meta, caps, num }`). Repeats are generated by the host from `wl_keyboard.repeat_info`. |
| `toplevel` | `tid`, `title`, `appId`, `activated`, `fullscreen`, `maximized`, `minimized`, `outputs` (output names). Sent on every `done`. |
| `toplevel-closed` | `tid` |
| `clipboard` | `text`, `html`: the new selection, `null` when it has no such content or was cleared. |
| `evdev` | `devices`: number of input devices being read. Sent after `evdev on` and whenever that number changes. 0 usually means no permission for `/dev/input`. |
| `input` | `kind` `key`, `code` (evdev code, including `BTN_*` mouse buttons), `value` (0 release, 1 press, 2 autorepeat); or `kind` `wheel`, `value` (detents, positive is up or right), `horizontal`. |
| `capture` | `id`, `width`, `height`: the file holds BGRA pixels with stride `width*4`, top row first, alpha 255. Or `id`, `error`. |
| `probe` | `id`, `x`, `y`: global logical pointer position, both `null` when no surface was entered within 200 ms. |
| `error` | `message`. Fatal errors are followed by exit. |
