/*
 * Input injection through the xdg-desktop-portal RemoteDesktop interface
 * (version 2: ConnectToEIS) and libei, for compositors without
 * zwp_virtual_keyboard_v1 and zwlr_virtual_pointer_v1, such as KWin since
 * Plasma 6.1. The portal asks the user for approval; a restore token from an
 * earlier session lets it skip the dialog where the portal backend supports
 * persistence.
 *
 * libei is loaded at run time, so a host built with it still starts where it
 * is not installed. Untested on Plasma.
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libei.h>

#include "bus.h"
#include "host.h"

#define PORTAL_SERVICE "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define RD_INTERFACE "org.freedesktop.portal.RemoteDesktop"
#define REQUEST_INTERFACE "org.freedesktop.portal.Request"
#define SESSION_INTERFACE "org.freedesktop.portal.Session"
#define REQUEST_PATH_PREFIX PORTAL_PATH "/request/"
#define DEVICE_KEYBOARD 1u
#define DEVICE_POINTER 2u
#define PERSIST_UNTIL_REVOKED 2u
#define RESPONSE_CANCELLED 1u
#define PORTAL_PROBE_TIMEOUT_MS 1000

#define EI_FUNCTIONS(X)                                                                            \
	X(ei_new_sender)                                                                           \
	X(ei_unref)                                                                                \
	X(ei_configure_name)                                                                       \
	X(ei_log_set_priority)                                                                     \
	X(ei_setup_backend_fd)                                                                     \
	X(ei_get_fd)                                                                               \
	X(ei_dispatch)                                                                             \
	X(ei_get_event)                                                                            \
	X(ei_now)                                                                                  \
	X(ei_event_get_type)                                                                       \
	X(ei_event_get_seat)                                                                       \
	X(ei_event_get_device)                                                                     \
	X(ei_event_unref)                                                                          \
	X(ei_seat_bind_capabilities)                                                               \
	X(ei_device_ref)                                                                           \
	X(ei_device_unref)                                                                         \
	X(ei_device_has_capability)                                                                \
	X(ei_device_start_emulating)                                                               \
	X(ei_device_get_region)                                                                    \
	X(ei_region_contains)                                                                      \
	X(ei_device_keyboard_key)                                                                  \
	X(ei_device_pointer_motion_absolute)                                                       \
	X(ei_device_frame)

#define EI_DECLARE(name) static __typeof__(name) *p_##name;
EI_FUNCTIONS(EI_DECLARE)

enum step { STEP_IDLE, STEP_CREATE, STEP_SELECT, STEP_START, STEP_CONNECT, STEP_ACTIVE };

struct device {
	struct ei_device *device;
	bool resumed;
};

static struct {
	bool subscribed;
	enum step step;
	char *request_path; /* the Request whose Response is awaited */
	char *session;
	char *restore_token;
	bool persist;
	unsigned next_token;

	struct ei *ei;
	struct device *devices;
	size_t device_count;
	uint32_t sequence;
	/* Last reported availability, -1 before the first report. */
	int reported_keyboard, reported_pointer;
} rd = { .reported_keyboard = -1, .reported_pointer = -1 };

static bool libei_load(void)
{
	static int loaded = -1;
	if (loaded >= 0)
		return loaded;
	loaded = 0;
	void *lib = dlopen("libei.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		log_msg("remote-desktop: %s", dlerror());
		return false;
	}
#define EI_LOAD(name)                                                                              \
	if (!(p_##name = (__typeof__(name) *)dlsym(lib, #name))) {                                 \
		log_msg("remote-desktop: libei lacks " #name);                                     \
		return false;                                                                      \
	}
	EI_FUNCTIONS(EI_LOAD)
#undef EI_LOAD
	loaded = 1;
	return true;
}

/* Events */

static struct ei_device *device_with(enum ei_device_capability capability, double x, double y)
{
	for (size_t i = 0; i < rd.device_count; i++) {
		struct device *device = &rd.devices[i];
		if (!device->resumed || !p_ei_device_has_capability(device->device, capability))
			continue;
		if (capability != EI_DEVICE_CAP_POINTER_ABSOLUTE)
			return device->device;
		struct ei_region *region;
		for (size_t r = 0; (region = p_ei_device_get_region(device->device, r)); r++) {
			if (p_ei_region_contains(region, x, y))
				return device->device;
		}
	}
	return NULL;
}

static bool has_resumed(enum ei_device_capability capability)
{
	for (size_t i = 0; i < rd.device_count; i++) {
		if (rd.devices[i].resumed &&
		    p_ei_device_has_capability(rd.devices[i].device, capability))
			return true;
	}
	return false;
}

static void emit_state(const char *state, const char *reason)
{
	bool keyboard = rd.ei && has_resumed(EI_DEVICE_CAP_KEYBOARD);
	bool pointer = rd.ei && has_resumed(EI_DEVICE_CAP_POINTER_ABSOLUTE);
	rd.reported_keyboard = keyboard;
	rd.reported_pointer = pointer;
	ev_begin("remote-desktop");
	ev_str("state", state);
	ev_bool("keyboard", keyboard);
	ev_bool("pointer", pointer);
	if (reason)
		ev_str("reason", reason);
	ev_end();
}

/* Reports an active session again when its devices changed. */
static void emit_devices_changed(void)
{
	if (rd.step != STEP_ACTIVE)
		return;
	if (rd.reported_keyboard != has_resumed(EI_DEVICE_CAP_KEYBOARD) ||
	    rd.reported_pointer != has_resumed(EI_DEVICE_CAP_POINTER_ABSOLUTE))
		emit_state("active", NULL);
}

/* Session */

static void session_close_request(void)
{
	if (!rd.session)
		return;
	DBusMessage *call =
		dbus_message_new_method_call(PORTAL_SERVICE, rd.session, SESSION_INTERFACE, "Close");
	if (call)
		bus_call(call, NULL, NULL);
}

static void ei_teardown(void)
{
	for (size_t i = 0; i < rd.device_count; i++)
		p_ei_device_unref(rd.devices[i].device);
	free(rd.devices);
	rd.devices = NULL;
	rd.device_count = 0;
	if (rd.ei)
		p_ei_unref(rd.ei);
	rd.ei = NULL;
}

/* Ends the session. `closed_by_portal` skips asking the portal to close it. */
static void session_end(const char *reason, bool closed_by_portal)
{
	if (rd.step == STEP_IDLE)
		return;
	ei_teardown();
	if (!closed_by_portal)
		session_close_request();
	free(rd.session);
	rd.session = NULL;
	free(rd.request_path);
	rd.request_path = NULL;
	rd.step = STEP_IDLE;
	emit_state("closed", reason);
}

static void session_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void session_fail(const char *fmt, ...)
{
	char message[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);
	log_msg("remote-desktop: %s", message);
	session_end(message, false);
}

/* Portal requests */

/* Returns a new handle token and expects the Response on its request path. */
static const char *request_token(void)
{
	static char token[32];
	snprintf(token, sizeof(token), "eow%ld_%u", (long)getpid(), ++rd.next_token);
	const char *unique = bus_unique_name();
	char sender[128];
	snprintf(sender, sizeof(sender), "%s", unique && unique[0] == ':' ? unique + 1 : "");
	for (char *c = sender; *c; c++) {
		if (*c == '.')
			*c = '_';
	}
	size_t len = strlen(REQUEST_PATH_PREFIX) + strlen(sender) + strlen(token) + 2;
	free(rd.request_path);
	rd.request_path = xrealloc(NULL, len);
	snprintf(rd.request_path, len, REQUEST_PATH_PREFIX "%s/%s", sender, token);
	return token;
}

static void select_devices(void);

/* Older portals may not derive the request path from the token. */
static void request_reply(DBusMessage *reply, void *data)
{
	enum step step = (enum step)(intptr_t)data;
	if (step != rd.step)
		return;
	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		const char *name = dbus_message_get_error_name(reply);
		const char *message = NULL;
		dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &message, DBUS_TYPE_INVALID);
		if (step == STEP_SELECT && rd.persist) {
			log_msg("remote-desktop: the portal rejected persistence (%s), retrying "
				"without it",
				message ? message : name);
			rd.persist = false;
			select_devices();
			return;
		}
		session_fail("%s failed: %s", step == STEP_CREATE   ? "CreateSession"
					      : step == STEP_SELECT ? "SelectDevices"
								    : "Start",
			     message ? message : name);
		return;
	}
	const char *path = NULL;
	if (dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID) &&
	    path && (!rd.request_path || strcmp(path, rd.request_path) != 0)) {
		free(rd.request_path);
		rd.request_path = xstrdup(path);
	}
}

/* Sends a request whose arguments end with an options dict. `session` and
 * `parent` are prepended when not NULL. */
static void portal_request(const char *method, enum step step, bool with_session,
			   const char *parent, void (*options)(DBusMessageIter *dict))
{
	DBusMessage *call =
		dbus_message_new_method_call(PORTAL_SERVICE, PORTAL_PATH, RD_INTERFACE, method);
	if (!call) {
		session_fail("out of memory");
		return;
	}
	DBusMessageIter args, dict;
	dbus_message_iter_init_append(call, &args);
	if (with_session) {
		const char *session = rd.session;
		dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &session);
	}
	if (parent)
		dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
	dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
	bus_dict_string(&dict, "handle_token", request_token());
	if (options)
		options(&dict);
	dbus_message_iter_close_container(&args, &dict);
	rd.step = step;
	if (!bus_call(call, request_reply, (void *)(intptr_t)step))
		session_fail("cannot call %s", method);
}

static void create_options(DBusMessageIter *dict)
{
	char token[32];
	snprintf(token, sizeof(token), "eow%ld_s%u", (long)getpid(), rd.next_token);
	bus_dict_string(dict, "session_handle_token", token);
}

static void select_options(DBusMessageIter *dict)
{
	bus_dict_u32(dict, "types", DEVICE_KEYBOARD | DEVICE_POINTER);
	if (!rd.persist)
		return;
	bus_dict_u32(dict, "persist_mode", PERSIST_UNTIL_REVOKED);
	if (rd.restore_token)
		bus_dict_string(dict, "restore_token", rd.restore_token);
}

static void select_devices(void)
{
	portal_request("SelectDevices", STEP_SELECT, true, NULL, select_options);
}

static void connect_reply(DBusMessage *reply, void *data)
{
	if (rd.step != STEP_CONNECT || rd.ei)
		return;
	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		const char *message = NULL;
		dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &message, DBUS_TYPE_INVALID);
		session_fail("ConnectToEIS failed: %s",
			     message ? message : dbus_message_get_error_name(reply));
		return;
	}
	int fd = -1;
	if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID)) {
		session_fail("ConnectToEIS returned no fd");
		return;
	}
	rd.ei = p_ei_new_sender(NULL);
	if (!rd.ei) {
		close(fd);
		session_fail("cannot create a libei context");
		return;
	}
	p_ei_log_set_priority(rd.ei, EI_LOG_PRIORITY_WARNING);
	p_ei_configure_name(rd.ei, HOST_NAMESPACE);
	int result = p_ei_setup_backend_fd(rd.ei, fd);
	if (result < 0)
		session_fail("cannot connect to EIS: %s", strerror(-result));
}

static void connect_eis(void)
{
	DBusMessage *call =
		dbus_message_new_method_call(PORTAL_SERVICE, PORTAL_PATH, RD_INTERFACE, "ConnectToEIS");
	DBusMessageIter args, dict;
	if (!call) {
		session_fail("out of memory");
		return;
	}
	dbus_message_iter_init_append(call, &args);
	const char *session = rd.session;
	dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &session);
	dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
	dbus_message_iter_close_container(&args, &dict);
	rd.step = STEP_CONNECT;
	if (!bus_call(call, connect_reply, NULL))
		session_fail("cannot call ConnectToEIS");
}

static bool variant_string(DBusMessageIter *variant, const char **out)
{
	int type = dbus_message_iter_get_arg_type(variant);
	if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH)
		return false;
	dbus_message_iter_get_basic(variant, out);
	return true;
}

static void handle_response(DBusMessage *message)
{
	DBusMessageIter args, value;
	dbus_uint32_t response;
	if (!dbus_message_iter_init(message, &args) ||
	    dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_UINT32)
		return;
	dbus_message_iter_get_basic(&args, &response);
	dbus_message_iter_next(&args);
	free(rd.request_path);
	rd.request_path = NULL;

	if (response != 0) {
		if (rd.step == STEP_SELECT && rd.persist && response != RESPONSE_CANCELLED) {
			log_msg("remote-desktop: SelectDevices failed with persistence, retrying "
				"without it");
			rd.persist = false;
			select_devices();
			return;
		}
		session_end(response == RESPONSE_CANCELLED ? "cancelled" : "failed", false);
		return;
	}
	const char *text;
	switch (rd.step) {
	case STEP_CREATE:
		if (!bus_dict_find(&args, "session_handle", &value) || !variant_string(&value, &text)) {
			session_fail("CreateSession returned no session handle");
			return;
		}
		rd.session = xstrdup(text);
		select_devices();
		break;
	case STEP_SELECT:
		portal_request("Start", STEP_START, true, "", NULL);
		break;
	case STEP_START:
		if (bus_dict_find(&args, "restore_token", &value) && variant_string(&value, &text) &&
		    *text) {
			ev_begin("remote-desktop-token");
			ev_str("token", text);
			ev_end();
		}
		connect_eis();
		break;
	default:
		break;
	}
}

static DBusHandlerResult filter(DBusConnection *connection, DBusMessage *message, void *data)
{
	const char *path = dbus_message_get_path(message);
	if (!path || dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (rd.request_path && strcmp(path, rd.request_path) == 0 &&
	    dbus_message_is_signal(message, REQUEST_INTERFACE, "Response")) {
		handle_response(message);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (rd.session && strcmp(path, rd.session) == 0 &&
	    dbus_message_is_signal(message, SESSION_INTERFACE, "Closed")) {
		session_end("closed", true);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static bool subscribe(void)
{
	if (rd.subscribed)
		return true;
	DBusConnection *connection = bus_get();
	if (!connection)
		return false;
	/* Added before any request, so that no Response can be missed. */
	dbus_bus_add_match(connection,
			   "type='signal',sender='" PORTAL_SERVICE "',interface='" REQUEST_INTERFACE
			   "',member='Response'",
			   NULL);
	dbus_bus_add_match(connection,
			   "type='signal',sender='" PORTAL_SERVICE "',interface='" SESSION_INTERFACE
			   "',member='Closed'",
			   NULL);
	dbus_connection_add_filter(connection, filter, NULL, NULL);
	rd.subscribed = true;
	return true;
}

/* Public */

bool rd_available(void)
{
	if (!libei_load() || !bus_get())
		return false;
	DBusMessage *call = dbus_message_new_method_call(PORTAL_SERVICE, PORTAL_PATH,
							 DBUS_INTERFACE_PROPERTIES, "Get");
	const char *interface = RD_INTERFACE, *property = "version";
	if (!call || !dbus_message_append_args(call, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING,
					       &property, DBUS_TYPE_INVALID)) {
		if (call)
			dbus_message_unref(call);
		return false;
	}
	DBusMessage *reply = bus_call_blocking(call, PORTAL_PROBE_TIMEOUT_MS);
	if (!reply)
		return false;
	DBusMessageIter args, variant;
	dbus_uint32_t version = 0;
	if (dbus_message_iter_init(reply, &args) &&
	    dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&args, &variant);
		if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT32)
			dbus_message_iter_get_basic(&variant, &version);
	}
	dbus_message_unref(reply);
	/* ConnectToEIS and persistence arrived in version 2. */
	return version >= 2;
}

void rd_start(const char *restore_token)
{
	if (rd.step != STEP_IDLE) {
		log_msg("remote-desktop: a session is already %s",
			rd.step == STEP_ACTIVE ? "active" : "starting");
		return;
	}
	if (!subscribe()) {
		emit_state("closed", "no session bus");
		return;
	}
	free(rd.restore_token);
	rd.restore_token = restore_token && *restore_token ? xstrdup(restore_token) : NULL;
	rd.persist = true;
	rd.reported_keyboard = rd.reported_pointer = -1;
	emit_state("starting", NULL);
	portal_request("CreateSession", STEP_CREATE, false, NULL, create_options);
}

void rd_stop(void)
{
	session_end("stopped", false);
}

int rd_fd(void)
{
	return rd.ei ? p_ei_get_fd(rd.ei) : -1;
}

static struct device *device_find(struct ei_device *device)
{
	for (size_t i = 0; i < rd.device_count; i++) {
		if (rd.devices[i].device == device)
			return &rd.devices[i];
	}
	return NULL;
}

void rd_dispatch(void)
{
	if (!rd.ei)
		return;
	p_ei_dispatch(rd.ei);
	struct ei_event *event;
	bool disconnected = false;
	while (!disconnected && (event = p_ei_get_event(rd.ei))) {
		struct ei_device *ei_device = p_ei_event_get_device(event);
		struct device *device = ei_device ? device_find(ei_device) : NULL;
		switch (p_ei_event_get_type(event)) {
		case EI_EVENT_CONNECT:
			rd.step = STEP_ACTIVE;
			emit_state("active", NULL);
			break;
		case EI_EVENT_DISCONNECT:
			disconnected = true;
			break;
		case EI_EVENT_SEAT_ADDED:
			p_ei_seat_bind_capabilities(p_ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD,
						    EI_DEVICE_CAP_POINTER_ABSOLUTE, NULL);
			break;
		case EI_EVENT_DEVICE_ADDED:
			if (device)
				break;
			rd.devices =
				xrealloc(rd.devices, (rd.device_count + 1) * sizeof(*rd.devices));
			rd.devices[rd.device_count++] =
				(struct device){ p_ei_device_ref(ei_device), false };
			break;
		case EI_EVENT_DEVICE_REMOVED:
			if (!device)
				break;
			p_ei_device_unref(device->device);
			*device = rd.devices[--rd.device_count];
			emit_devices_changed();
			break;
		case EI_EVENT_DEVICE_RESUMED:
			if (!device)
				break;
			p_ei_device_start_emulating(device->device, ++rd.sequence);
			device->resumed = true;
			emit_devices_changed();
			break;
		case EI_EVENT_DEVICE_PAUSED:
			if (!device)
				break;
			device->resumed = false;
			emit_devices_changed();
			break;
		default:
			break;
		}
		p_ei_event_unref(event);
	}
	if (disconnected)
		session_end("disconnected", false);
}

bool rd_key(uint32_t code, bool pressed)
{
	struct ei_device *device = rd.ei ? device_with(EI_DEVICE_CAP_KEYBOARD, 0, 0) : NULL;
	if (!device)
		return false;
	p_ei_device_keyboard_key(device, code, pressed);
	p_ei_device_frame(device, p_ei_now(rd.ei));
	return true;
}

bool rd_warp(double x, double y)
{
	struct ei_device *device = rd.ei ? device_with(EI_DEVICE_CAP_POINTER_ABSOLUTE, x, y) : NULL;
	if (!device)
		return false;
	p_ei_device_pointer_motion_absolute(device, x, y);
	p_ei_device_frame(device, p_ei_now(rd.ei));
	return true;
}
