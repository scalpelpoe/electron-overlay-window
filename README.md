# electron-overlay-window

[![](https://img.shields.io/npm/v/electron-overlay-window/latest?color=CC3534&label=electron-overlay-window&logo=npm&labelColor=212121)](https://www.npmjs.com/package/electron-overlay-window)

Library for creating overlay windows, intended to complement Electron.

Responsible for:
  - Finding target window by title
  - Keeping position and size of overlay window with target in sync
  - Emits lifecycle events

![npm run demo:electron](https://i.imgur.com/Ej190zc.gif)

Important notes:
  - You can initialize library only once (Electron window must never die, and title by which target window is searched cannot be changed)
  - You can have only one overlay window
  - Found target window remains "valid" even if its title has changed
  - Correct behavior is guaranteed only for top-level windows *(A top-level window is a window that is not a child window, or has no parent window (which is the same as having the "desktop window" as a parent))*
  - X11: library relies on EWHM, more specifically `_NET_ACTIVE_WINDOW`, `_NET_WM_STATE_FULLSCREEN`, `_NET_WM_NAME`

Supported backends:
  - Windows (7 - 10)
  - Linux (X11)
  - Linux (Wayland with wlr-layer-shell: wlroots compositors, Hyprland, niri, KDE, COSMIC; not GNOME)

## Wayland

Under `--ozone-platform=wayland`, Chromium can only create ordinary toplevel
windows, which a compositor may tile or stack below a fullscreen target. The
layer-shell backend instead renders overlay windows offscreen and presents
them as `zwlr_layer_shell_v1` surfaces through a helper process,
`overlay-wayland-host` (see `src/lib/wayland/README.md`). Building it needs
the `wayland-client`, `wayland-cursor` and `xkbcommon` development packages;
without them only the X11 backend is built.

```ts
import { app, BrowserWindow } from 'electron'
import { LAYER_SHELL_WEB_PREFERENCES, OVERLAY_WINDOW_OPTS, OverlayController, probeWaylandHost } from 'electron-overlay-window'

// Before app start: relaunch with --ozone-platform=wayland if
// probeWaylandHost()?.layerShell, otherwise use X11.
app.whenReady().then(() => {
  OverlayController.useLayerShell()
  const win = new BrowserWindow({ ...OVERLAY_WINDOW_OPTS, webPreferences: LAYER_SHELL_WEB_PREFERENCES })
  OverlayController.attachByTitle(win, 'Game Title')
})
```

- The target is found with `zwlr_foreign_toplevel_manager_v1` and covers its
  output, so it must be fullscreen or borderless fullscreen. On KWin, which
  lacks that protocol, the host follows windows with a KWin script instead and
  the overlay covers the target's own bounds (Plasma 6, built when `dbus-1` is
  found; untested on Plasma). `windowTracking(features)` tells which applies.
  Other compositors without either fall back to tracking an XWayland target
  through X11.
- Further overlay windows are presented with `adoptLayerWindow(win)`. Adopted
  windows keep the BrowserWindow API: bounds are global logical coordinates,
  `setIgnoreMouseEvents(true)` makes the window click-through except for the
  rectangles given to `LayerWindow.setInteractiveRects`, `focus()` grabs the
  keyboard until `blur()`, and `-webkit-app-region: drag` areas and the edges
  of resizable windows move and resize the window.
- Chromium only sees the Wayland clipboard while one of its windows is
  focused. `waylandClipboard` reads and writes it through the host instead.
- Electron's `globalShortcut` does not work on Wayland, and nothing outside
  the compositor sees the target's input. `waylandNative` reads keys, buttons
  and the wheel from evdev while the target has focus (the user needs read
  access to `/dev/input`), types with a virtual keyboard (or, where the
  compositor has none, a uinput keyboard, which needs write access to
  `/dev/uinput`), warps and finds the pointer, and captures screen regions.
  `setLayerShortcutHandler` receives keys typed into an overlay. evdev cannot
  keep a key from the target.
- IME composition, tooltips and native context menus are unavailable in
  offscreen windows.

Recommended dev utils
- Windows: AccEvent (accevent.exe) and Inspect Object (inspect.exe) from Windows SDK
- X11: xwininfo, xprop, xev
