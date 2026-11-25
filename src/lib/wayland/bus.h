/*
 * Session bus helpers for the D-Bus based modules. Calls are asynchronous
 * except where noted, so a slow service never stalls the Wayland loop.
 */
#ifndef OVERLAY_WAYLAND_BUS_H
#define OVERLAY_WAYLAND_BUS_H

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stdint.h>

/* Opens the connection on first use. NULL without a session bus. */
DBusConnection *bus_get(void);
const char *bus_unique_name(void);

/* Blocking. The unique name owning `name`, or NULL; free with free(). */
char *bus_name_owner(const char *name);
/* Blocking. Takes ownership of `call`. Returns the reply, or NULL after
 * logging the error. */
DBusMessage *bus_call_blocking(DBusMessage *call, int timeout_ms);
/* Takes ownership of `call`. `notify` receives the reply or error message,
 * which it must not unref; it may be NULL to ignore the reply. */
bool bus_call(DBusMessage *call, void (*notify)(DBusMessage *reply, void *data), void *data);
/* Sends a message that needs no reply, such as a method return. */
void bus_send(DBusMessage *message);

/* Appends `{key: <string>}` or `{key: <uint32>}` to an open a{sv} array. */
void bus_dict_string(DBusMessageIter *dict, const char *key, const char *value);
void bus_dict_u32(DBusMessageIter *dict, const char *key, uint32_t value);
/* Looks up a key of an a{sv} iterator and returns its variant's contents. */
bool bus_dict_find(DBusMessageIter *dict, const char *key, DBusMessageIter *value);

#endif
