/*
 * Session bus connection, polled from the host's main loop. libdbus is used
 * because Electron itself links it, so it adds no runtime dependency.
 */
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "host.h"

static DBusConnection *connection;
static bool connect_failed;

DBusConnection *bus_get(void)
{
	if (connection || connect_failed)
		return connection;
	DBusError error;
	dbus_error_init(&error);
	/* Private, so that closing it on disconnect cannot affect anyone else. */
	connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
	if (!connection) {
		log_msg("dbus: %s", error.message);
		dbus_error_free(&error);
		connect_failed = true;
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(connection, FALSE);
	return connection;
}

const char *bus_unique_name(void)
{
	return connection ? dbus_bus_get_unique_name(connection) : NULL;
}

int bus_fd(void)
{
	int fd;
	if (!connection || !dbus_connection_get_unix_fd(connection, &fd))
		return -1;
	return fd;
}

void bus_dispatch(bool readable)
{
	if (!connection)
		return;
	if (readable)
		dbus_connection_read_write(connection, 0);
	while (dbus_connection_dispatch(connection) == DBUS_DISPATCH_DATA_REMAINS)
		;
	if (dbus_connection_get_is_connected(connection))
		return;
	/* A closed socket would keep the poll loop spinning. */
	emit_error("dbus: the session bus connection was closed");
	dbus_connection_close(connection);
	dbus_connection_unref(connection);
	connection = NULL;
	connect_failed = true;
}

DBusMessage *bus_call_blocking(DBusMessage *call, int timeout_ms)
{
	DBusConnection *conn = bus_get();
	if (!conn) {
		dbus_message_unref(call);
		return NULL;
	}
	DBusError error;
	dbus_error_init(&error);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, timeout_ms, &error);
	if (!reply) {
		log_msg("dbus: %s.%s: %s", dbus_message_get_interface(call),
			dbus_message_get_member(call), error.message);
		dbus_error_free(&error);
	}
	dbus_message_unref(call);
	return reply;
}

char *bus_name_owner(const char *name)
{
	DBusMessage *call = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
							 DBUS_INTERFACE_DBUS, "GetNameOwner");
	if (!call || !dbus_message_append_args(call, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID))
		return NULL;
	DBusConnection *conn = bus_get();
	if (!conn) {
		dbus_message_unref(call);
		return NULL;
	}
	/* A missing owner is an expected answer, not worth logging. */
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, 1000, NULL);
	dbus_message_unref(call);
	if (!reply)
		return NULL;
	const char *owner = NULL;
	char *result = NULL;
	if (dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID))
		result = xstrdup(owner);
	dbus_message_unref(reply);
	return result;
}

struct pending {
	void (*notify)(DBusMessage *reply, void *data);
	void *data;
};

static void pending_done(DBusPendingCall *call, void *user_data)
{
	struct pending *pending = user_data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	if (reply) {
		pending->notify(reply, pending->data);
		dbus_message_unref(reply);
	}
	dbus_pending_call_unref(call);
}

bool bus_call(DBusMessage *call, void (*notify)(DBusMessage *reply, void *data), void *data)
{
	DBusConnection *conn = bus_get();
	if (!conn) {
		dbus_message_unref(call);
		return false;
	}
	if (!notify) {
		dbus_message_set_no_reply(call, TRUE);
		bool sent = dbus_connection_send(conn, call, NULL);
		dbus_message_unref(call);
		dbus_connection_flush(conn);
		return sent;
	}
	DBusPendingCall *pending_call = NULL;
	if (!dbus_connection_send_with_reply(conn, call, &pending_call, DBUS_TIMEOUT_USE_DEFAULT) ||
	    !pending_call) {
		dbus_message_unref(call);
		return false;
	}
	dbus_message_unref(call);
	struct pending *pending = xcalloc(1, sizeof(*pending));
	pending->notify = notify;
	pending->data = data;
	dbus_pending_call_set_notify(pending_call, pending_done, pending, free);
	dbus_connection_flush(conn);
	return true;
}

void bus_send(DBusMessage *message)
{
	DBusConnection *conn = bus_get();
	if (conn) {
		dbus_connection_send(conn, message, NULL);
		dbus_connection_flush(conn);
	}
	dbus_message_unref(message);
}

static void dict_entry(DBusMessageIter *dict, const char *key, int type, const char *signature,
		       const void *value)
{
	DBusMessageIter entry, variant;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant);
	dbus_message_iter_append_basic(&variant, type, value);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(dict, &entry);
}

void bus_dict_string(DBusMessageIter *dict, const char *key, const char *value)
{
	dict_entry(dict, key, DBUS_TYPE_STRING, DBUS_TYPE_STRING_AS_STRING, &value);
}

void bus_dict_u32(DBusMessageIter *dict, const char *key, uint32_t value)
{
	dbus_uint32_t v = value;
	dict_entry(dict, key, DBUS_TYPE_UINT32, DBUS_TYPE_UINT32_AS_STRING, &v);
}

bool bus_dict_find(DBusMessageIter *dict, const char *key, DBusMessageIter *value)
{
	if (dbus_message_iter_get_arg_type(dict) != DBUS_TYPE_ARRAY)
		return false;
	DBusMessageIter entries;
	dbus_message_iter_recurse(dict, &entries);
	for (; dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY;
	     dbus_message_iter_next(&entries)) {
		DBusMessageIter entry;
		dbus_message_iter_recurse(&entries, &entry);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			continue;
		const char *name;
		dbus_message_iter_get_basic(&entry, &name);
		if (strcmp(name, key) != 0 || !dbus_message_iter_next(&entry) ||
		    dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			continue;
		dbus_message_iter_recurse(&entry, value);
		return true;
	}
	return false;
}
