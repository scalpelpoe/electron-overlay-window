#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <xcb/xcb.h>
#include "overlay_window.h"

static xcb_connection_t* x_conn;
static xcb_window_t root;
static xcb_atom_t ATOM_NET_ACTIVE_WINDOW;
static xcb_atom_t ATOM_NET_WM_NAME;
static xcb_atom_t ATOM_UTF8_STRING;
static xcb_atom_t ATOM_NET_WM_STATE;
static xcb_atom_t ATOM_NET_WM_STATE_FULLSCREEN;

struct ow_target_window
{
  char** titles;
  size_t titles_count;
  xcb_window_t window_id;
  bool is_focused;
  bool is_destroyed;
  bool is_fullscreen;
};

struct ow_overlay_window
{
  xcb_window_t window_id;
};

static xcb_window_t active_window = XCB_WINDOW_NONE;
static uv_mutex_t target_mutex;
static bool hook_running = false;
// Cleanup also runs when the addon was imported without starting its tracker.
static bool hook_started = false;

static struct ow_target_window target_info = {
  .titles = NULL,
  .titles_count = 0,
  .window_id = XCB_WINDOW_NONE,
  .is_focused = false,
  .is_destroyed = false,
  .is_fullscreen = false
};

static struct ow_overlay_window overlay_info = {
  .window_id = XCB_WINDOW_NONE
};

static xcb_window_t get_active_window() {
  xcb_get_property_reply_t* prop_reply = xcb_get_property_reply(x_conn, xcb_get_property(x_conn, 0, root, ATOM_NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW, 0, 1), NULL);
  if (prop_reply == NULL) {
    return XCB_WINDOW_NONE;
  }
  if (xcb_get_property_value_length(prop_reply) < (int)sizeof(xcb_window_t)) {
    free(prop_reply);
    return XCB_WINDOW_NONE;
  }
  xcb_window_t active_window = *((xcb_window_t*)xcb_get_property_value(prop_reply));
  free(prop_reply);
  return active_window;
}

static bool get_title(xcb_window_t wid, char** title) {
  if (wid == XCB_WINDOW_NONE) {
    *title = NULL;
    return true;
  }
  xcb_get_property_reply_t* prop_reply = xcb_get_property_reply(x_conn, xcb_get_property(x_conn, 0, wid, ATOM_NET_WM_NAME, ATOM_UTF8_STRING, 0, 100000), NULL);
  if (prop_reply == NULL) {
    return false;
  }
  int buffLenUtf8 = xcb_get_property_value_length(prop_reply);
  if (buffLenUtf8 == 0) {
    *title = NULL;
    free(prop_reply);
    return true;
  }
  *title = malloc(buffLenUtf8 + 1);
  memcpy(*title, xcb_get_property_value(prop_reply), buffLenUtf8);
  (*title)[buffLenUtf8] = '\0';
  free(prop_reply);
  return true;
}

static bool get_content_bounds_conn(xcb_connection_t* conn, xcb_window_t root_win, xcb_window_t wid, struct ow_window_bounds* bounds) {
  xcb_get_geometry_reply_t* geometry = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, wid), NULL);
  if (geometry == NULL) {
    return false;
  }
  xcb_translate_coordinates_reply_t* translated = xcb_translate_coordinates_reply(conn, xcb_translate_coordinates(conn, wid, root_win, 0, 0), NULL);
  if (translated == NULL) {
    free(geometry);
    return false;
  }

  bounds->x = translated->dst_x;
  bounds->y = translated->dst_y;
  bounds->width = geometry->width;
  bounds->height = geometry->height;
  free(translated);
  free(geometry);
  return true;
}

static bool get_content_bounds(xcb_window_t wid, struct ow_window_bounds* bounds) {
  return get_content_bounds_conn(x_conn, root, wid, bounds);
}

/** A window's title property, or NULL. The caller frees it. */
static char* query_title(xcb_connection_t* conn, xcb_window_t wid, xcb_atom_t name_atom, xcb_atom_t type_atom) {
  xcb_get_property_reply_t* reply = xcb_get_property_reply(
    conn, xcb_get_property(conn, 0, wid, name_atom, type_atom, 0, 100000), NULL);
  if (reply == NULL || reply->format != 8 || reply->value_len == 0) {
    free(reply);
    return NULL;
  }
  unsigned length = xcb_get_property_value_length(reply);
  char* title = malloc(sizeof(char) * (length + 1));
  if (title == NULL) {
    free(reply);
    return NULL;
  }
  memcpy(title, xcb_get_property_value(reply), length);
  title[length] = '\0';
  free(reply);
  return title;
}

static bool find_in_tree(xcb_connection_t* conn, xcb_window_t root_win, xcb_window_t parent,
                         xcb_atom_t name_atom, xcb_atom_t utf8_atom, const char* target,
                         struct ow_window_bounds* out) {
  xcb_query_tree_reply_t* tree = xcb_query_tree_reply(conn, xcb_query_tree(conn, parent), NULL);
  if (tree == NULL) {
    return false;
  }
  xcb_window_t* children = xcb_query_tree_children(tree);
  int count = xcb_query_tree_children_length(tree);
  for (int i = 0; i < count; ++i) {
    char* title = query_title(conn, children[i], name_atom, utf8_atom);
    if (title == NULL) {
      title = query_title(conn, children[i], XCB_ATOM_WM_NAME, XCB_ATOM_STRING);
    }
    if (title != NULL && strcmp(title, target) == 0) {
      bool found = get_content_bounds_conn(conn, root_win, children[i], out);
      free(title);
      free(tree);
      return found;
    }
    free(title);
    if (find_in_tree(conn, root_win, children[i], name_atom, utf8_atom, target, out)) {
      free(tree);
      return true;
    }
  }
  free(tree);
  return false;
}

/** Looks up a window by title using a private X connection. The tracker thread
 *  owns `x_conn`, so querying from the Node thread needs its own connection.
 *  This is needed on Wayland compositors that do not maintain X11's
 *  `_NET_ACTIVE_WINDOW`, where the event hook never sees an ATTACH. */
bool ow_find_window_bounds(const char* title, struct ow_window_bounds* out) {
  if (title == NULL || out == NULL) {
    return false;
  }
  xcb_connection_t* conn = xcb_connect(NULL, NULL);
  if (conn == NULL || xcb_connection_has_error(conn)) {
    if (conn != NULL) xcb_disconnect(conn);
    return false;
  }
  xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
  if (screen == NULL) {
    xcb_disconnect(conn);
    return false;
  }
  xcb_intern_atom_reply_t* name = xcb_intern_atom_reply(
    conn, xcb_intern_atom(conn, 0, strlen("_NET_WM_NAME"), "_NET_WM_NAME"), NULL);
  xcb_intern_atom_reply_t* utf8 = xcb_intern_atom_reply(
    conn, xcb_intern_atom(conn, 0, strlen("UTF8_STRING"), "UTF8_STRING"), NULL);
  if (name == NULL || utf8 == NULL) {
    free(name);
    free(utf8);
    xcb_disconnect(conn);
    return false;
  }
  bool found = find_in_tree(conn, screen->root, screen->root, name->atom, utf8->atom, title, out);
  free(name);
  free(utf8);
  xcb_disconnect(conn);
  return found;
}

static bool is_fullscreen_window(xcb_window_t wid, bool* is_fullscreen) {
  xcb_get_property_reply_t* prop_reply = xcb_get_property_reply(x_conn, xcb_get_property(x_conn, 0, wid, ATOM_NET_WM_STATE, XCB_ATOM_ATOM, 0, 100000), NULL);
  if (prop_reply == NULL) {
    return false;
  }
  *is_fullscreen = false;
  xcb_atom_t* wm_state = (xcb_atom_t*)xcb_get_property_value(prop_reply);
  for (unsigned i = 0; i < prop_reply->value_len; ++i) {
    if (wm_state[i] == ATOM_NET_WM_STATE_FULLSCREEN) {
      *is_fullscreen = true;
    }
  }
  free(prop_reply);
  return true;
}

static void handle_moveresize_xevent(struct ow_target_window* target_info) {
  struct ow_window_bounds bounds;
  if (get_content_bounds(target_info->window_id, &bounds)) {
    struct ow_event e = {
      .type = OW_MOVERESIZE,
      .data.moveresize = {
        .bounds = bounds
      }
    };
    ow_emit_event(&e);
  }
}

static void handle_fullscreen_xevent(struct ow_target_window* target_info) {
  bool is_fullscreen;
  if (is_fullscreen_window(target_info->window_id, &is_fullscreen)) {
    if (is_fullscreen != target_info->is_fullscreen) {
      target_info->is_fullscreen = is_fullscreen;
      struct ow_event e = {
        .type = OW_FULLSCREEN,
        .data.fullscreen = {
          .is_fullscreen = target_info->is_fullscreen
        }
      };
      ow_emit_event(&e);
    }
  }
}

static void check_and_handle_window(xcb_window_t wid, struct ow_target_window* target_info) {
  if (target_info->window_id != XCB_WINDOW_NONE) {
    if (target_info->window_id != wid) {
      if (target_info->is_focused) {
        target_info->is_focused = false;
        struct ow_event e = { .type = OW_BLUR };
        ow_emit_event(&e);
      }

      if (target_info->is_destroyed) {
        target_info->window_id = XCB_WINDOW_NONE;

        target_info->is_destroyed = false;
        struct ow_event e = { .type = OW_DETACH };
        ow_emit_event(&e);
      }
    }
    else if (target_info->window_id == wid) {
      if (!target_info->is_focused) {
        target_info->is_focused = true;
        struct ow_event e = { .type = OW_FOCUS };
        ow_emit_event(&e);
      }
      return;
    }
  }

  char* title = NULL;
  if (!get_title(wid, &title) || title == NULL) {
    return;
  }
  int matched_index = -1;
  for (size_t i = 0; i < target_info->titles_count; i++) {
    if (target_info->titles[i] && strcmp(title, target_info->titles[i]) == 0) {
      matched_index = (int)i;
      break;
    }
  }
  free(title);
  if (matched_index < 0) {
    return;
  }

  if (target_info->window_id != XCB_WINDOW_NONE) {
    uint32_t mask[] = { XCB_EVENT_MASK_NO_EVENT };
    xcb_change_window_attributes(x_conn, target_info->window_id, XCB_CW_EVENT_MASK, mask);
  }

  target_info->window_id = wid;

  // listen for `_NET_WM_STATE` fullscreen and window move/resize/destroy
  uint32_t mask[] = { XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY };
  xcb_change_window_attributes(x_conn, target_info->window_id, XCB_CW_EVENT_MASK, mask);

  struct ow_event e = {
    .type = OW_ATTACH,
    .data.attach = {
      .has_access = -1,
      .is_fullscreen = -1,
      .title_index = matched_index,
    }
  };
  bool is_fullscreen;
  if (
    is_fullscreen_window(target_info->window_id, &is_fullscreen) &&
    get_content_bounds(target_info->window_id, &e.data.attach.bounds)
  ) {
    if (is_fullscreen != target_info->is_fullscreen) {
      target_info->is_fullscreen = is_fullscreen;
      e.data.attach.is_fullscreen = is_fullscreen;
    }
    // emit OW_ATTACH
    ow_emit_event(&e);

    target_info->is_focused = true;
    e.type = OW_FOCUS;
    ow_emit_event(&e);
  } else {
    // something went wrong, did the target window die right after becoming active?
    target_info->window_id = XCB_WINDOW_NONE;
  }
}

static void hook_proc(xcb_generic_event_t* generic_event) {
  if (generic_event->response_type == XCB_DESTROY_NOTIFY) {
    xcb_destroy_notify_event_t* event = (xcb_destroy_notify_event_t*)generic_event;
    if (event->window == target_info.window_id) {
      target_info.is_destroyed = true;
      check_and_handle_window(XCB_WINDOW_NONE, &target_info);
    }
    return;
  }
  if (generic_event->response_type == XCB_CONFIGURE_NOTIFY) {
    xcb_configure_notify_event_t* event = (xcb_configure_notify_event_t*)generic_event;
    if (event->window == target_info.window_id) {
      handle_moveresize_xevent(&target_info);
    }
    return;
  }
  if (generic_event->response_type == XCB_PROPERTY_NOTIFY) {
    xcb_property_notify_event_t* event = (xcb_property_notify_event_t*)generic_event;
    if (event->window == root && event->atom == ATOM_NET_ACTIVE_WINDOW) {
      xcb_window_t old_active = active_window;
      active_window = get_active_window();

      if (old_active != target_info.window_id) {
        uint32_t mask[] = { XCB_EVENT_MASK_NO_EVENT };
        xcb_change_window_attributes(x_conn, old_active, XCB_CW_EVENT_MASK, mask);
      }
      if (active_window != XCB_WINDOW_NONE && active_window != target_info.window_id) {
        // listen for `_NET_WM_NAME`
        uint32_t mask[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
        xcb_change_window_attributes(x_conn, active_window, XCB_CW_EVENT_MASK, mask);
      }
      check_and_handle_window(active_window, &target_info);
    } else if (event->window == target_info.window_id && event->atom == ATOM_NET_WM_STATE) {
      handle_fullscreen_xevent(&target_info);
    } else if (event->window == active_window && event->atom == ATOM_NET_WM_NAME) {
      check_and_handle_window(active_window, &target_info);
    }
    return;
  }
}

static void hook_thread(void* _arg) {
  x_conn = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(x_conn)) {
    fprintf(stderr, "[electron-overlay-window] xcb_connect failed\n");
    xcb_disconnect(x_conn);
    x_conn = NULL;
    return;
  }
  hook_running = true;
  xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(x_conn)).data;
  root = screen->root;

  xcb_intern_atom_reply_t* atom_reply;
  atom_reply = xcb_intern_atom_reply(x_conn, xcb_intern_atom(x_conn, 0, strlen("_NET_ACTIVE_WINDOW"), "_NET_ACTIVE_WINDOW"), NULL);
  ATOM_NET_ACTIVE_WINDOW = atom_reply->atom;
  free(atom_reply);
  atom_reply = xcb_intern_atom_reply(x_conn, xcb_intern_atom(x_conn, 0, strlen("_NET_WM_NAME"), "_NET_WM_NAME"), NULL);
  ATOM_NET_WM_NAME = atom_reply->atom;
  free(atom_reply);
  atom_reply = xcb_intern_atom_reply(x_conn, xcb_intern_atom(x_conn, 0, strlen("UTF8_STRING"), "UTF8_STRING"), NULL);
  ATOM_UTF8_STRING = atom_reply->atom;
  free(atom_reply);
  atom_reply = xcb_intern_atom_reply(x_conn, xcb_intern_atom(x_conn, 0, strlen("_NET_WM_STATE"), "_NET_WM_STATE"), NULL);
  ATOM_NET_WM_STATE = atom_reply->atom;
  free(atom_reply);
  atom_reply = xcb_intern_atom_reply(x_conn, xcb_intern_atom(x_conn, 0, strlen("_NET_WM_STATE_FULLSCREEN"), "_NET_WM_STATE_FULLSCREEN"), NULL);
  ATOM_NET_WM_STATE_FULLSCREEN = atom_reply->atom;
  free(atom_reply);

  if (overlay_info.window_id != XCB_WINDOW_NONE) {
    // Electron window is created with `show: false`,
    // this override-redirect is being set before window is mapped.
    uint32_t values[] = {1};
    xcb_change_window_attributes(x_conn, overlay_info.window_id, XCB_CW_OVERRIDE_REDIRECT, values);
  }

  // listen for `_NET_ACTIVE_WINDOW` changes
  uint32_t mask[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
  xcb_change_window_attributes(x_conn, root, XCB_CW_EVENT_MASK, mask);

  active_window = get_active_window();
  if (active_window != XCB_WINDOW_NONE) {
    // listen for `_NET_WM_NAME`
    uint32_t mask[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_change_window_attributes(x_conn, active_window, XCB_CW_EVENT_MASK, mask);
    check_and_handle_window(active_window, &target_info);
  }
  xcb_flush(x_conn);

  xcb_generic_event_t* event;
  while (hook_running && (event = xcb_wait_for_event(x_conn))) {
    event->response_type = event->response_type & ~0x80;
    hook_proc(event);
    xcb_flush(x_conn);
    free(event);
  }
  xcb_disconnect(x_conn);
  x_conn = NULL;
}

void ow_start_hook(char** target_window_titles, size_t titles_count, void* overlay_window_id) {
  uv_mutex_init(&target_mutex);
  target_info.titles = target_window_titles;
  target_info.titles_count = titles_count;
  if (overlay_window_id != NULL) {
    overlay_info.window_id = *((xcb_window_t*)overlay_window_id);
  }
  if (uv_thread_create(&hook_tid, hook_thread, NULL) == 0) {
    hook_started = true;
  } else {
    uv_mutex_destroy(&target_mutex);
  }
}

void ow_set_target_titles(char** titles, size_t count) {
  uv_mutex_lock(&target_mutex);

  if (target_info.window_id != XCB_WINDOW_NONE) {
    uint32_t mask[] = { XCB_EVENT_MASK_NO_EVENT };
    xcb_change_window_attributes(x_conn, target_info.window_id, XCB_CW_EVENT_MASK, mask);
    target_info.window_id = XCB_WINDOW_NONE;
    target_info.is_focused = false;
    target_info.is_destroyed = false;

    struct ow_event e = { .type = OW_DETACH };
    ow_emit_event(&e);
  }

  if (target_info.titles) {
    for (size_t i = 0; i < target_info.titles_count; i++) {
      free(target_info.titles[i]);
    }
    free(target_info.titles);
  }
  target_info.titles = titles;
  target_info.titles_count = count;

  uv_mutex_unlock(&target_mutex);

  if (x_conn) {
    xcb_window_t active = get_active_window();
    if (active != XCB_WINDOW_NONE) {
      check_and_handle_window(active, &target_info);
    }
  }
}

void ow_clear_target(void) {
  uv_mutex_lock(&target_mutex);

  if (target_info.window_id != XCB_WINDOW_NONE) {
    uint32_t mask[] = { XCB_EVENT_MASK_NO_EVENT };
    xcb_change_window_attributes(x_conn, target_info.window_id, XCB_CW_EVENT_MASK, mask);
    target_info.window_id = XCB_WINDOW_NONE;
    target_info.is_focused = false;
    target_info.is_destroyed = false;

    struct ow_event e = { .type = OW_DETACH };
    ow_emit_event(&e);
  }

  if (target_info.titles) {
    for (size_t i = 0; i < target_info.titles_count; i++) {
      free(target_info.titles[i]);
    }
    free(target_info.titles);
  }
  target_info.titles = NULL;
  target_info.titles_count = 0;

  uv_mutex_unlock(&target_mutex);
}

void ow_stop_hook(void) {
  if (!hook_started) return;
  hook_started = false;
  hook_running = false;
  if (x_conn) {
    xcb_disconnect(x_conn);
    x_conn = NULL;
  }
  uv_thread_join(&hook_tid);
  uv_mutex_destroy(&target_mutex);
}

void ow_activate_overlay() {
  xcb_set_input_focus(x_conn, XCB_INPUT_FOCUS_PARENT, overlay_info.window_id, XCB_CURRENT_TIME);
  xcb_flush(x_conn);
}

void ow_focus_target() {
  xcb_set_input_focus(x_conn, XCB_INPUT_FOCUS_PARENT, target_info.window_id, XCB_CURRENT_TIME);
  xcb_flush(x_conn);
}
