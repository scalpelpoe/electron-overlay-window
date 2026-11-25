/*
 * Shared between host.c and the optional modules built next to it.
 */
#ifndef OVERLAY_WAYLAND_HOST_H
#define OVERLAY_WAYLAND_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HOST_NAMESPACE "electron-overlay-window"

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Logs and sends a non-fatal `error` event. */
void emit_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void *xrealloc(void *ptr, size_t size);
void *xcalloc(size_t count, size_t size);
char *xstrdup(const char *str);
bool write_all(int fd, const void *data, size_t len);
bool parse_u32(const char *str, uint32_t *out);
bool parse_double(const char *str, double *out);
uint32_t now_ms(void);

/* One event at a time: ev_begin, any number of fields, ev_end. */
void ev_begin(const char *name);
void ev_str(const char *key, const char *value);
void ev_strn(const char *key, const char *value, size_t len);
void ev_int(const char *key, long long value);
void ev_num(const char *key, double value);
void ev_bool(const char *key, bool value);
void ev_strings(const char *key, const char *const *values, size_t count);
void ev_rect(const char *key, int32_t x, int32_t y, int32_t width, int32_t height);
void ev_end(void);

/* Toplevel ids are shared by every source of `toplevel` events. */
uint32_t toplevel_next_id(void);

#ifdef HAVE_DBUS
/* Session bus (bus.c). The fd is -1 while no connection is open. */
int bus_fd(void);
/* Reads when the fd was readable, then dispatches queued messages. */
void bus_dispatch(bool readable);

/* Window tracking through a KWin script (kwin.c). */
bool kwin_available(void);
void kwin_start(void);
/* Returns false when `tid` is not a KWin window. */
bool kwin_activate(uint32_t tid);
#else
static inline int bus_fd(void) { return -1; }
static inline void bus_dispatch(bool readable) { (void)readable; }
static inline bool kwin_available(void) { return false; }
static inline void kwin_start(void) {}
static inline bool kwin_activate(uint32_t tid) { (void)tid; return false; }
#endif

#ifdef HAVE_REMOTE_DESKTOP
/* Input injection through the RemoteDesktop portal and libei
 * (remote-desktop.c). */
bool rd_available(void);
/* Asks the portal for a session; `restore_token` may be NULL. Reports
 * progress with `remote-desktop` events. */
void rd_start(const char *restore_token);
void rd_stop(void);
/* Return false when no session device can send the event. */
bool rd_key(uint32_t code, bool pressed);
bool rd_warp(double x, double y);
/* The libei fd, -1 while not connected. */
int rd_fd(void);
void rd_dispatch(void);
#else
static inline bool rd_available(void) { return false; }
static inline void rd_start(const char *restore_token) { (void)restore_token; }
static inline void rd_stop(void) {}
static inline bool rd_key(uint32_t code, bool pressed) { (void)code; (void)pressed; return false; }
static inline bool rd_warp(double x, double y) { (void)x; (void)y; return false; }
static inline int rd_fd(void) { return -1; }
static inline void rd_dispatch(void) {}
#endif

#endif
