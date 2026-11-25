/*
 * Window tracking on KWin, which has no foreign-toplevel protocol. A script
 * loaded through KWin's scripting D-Bus interface (the approach kdotool
 * takes) reports normal windows back to this host with callDBus. Unlike
 * org_kde_plasma_window_management, this needs no .desktop file declaring
 * the interface, so it works for AppImages and development runs.
 *
 * Written against the Plasma 6 scripting API. Untested on Plasma.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bus.h"
#include "host.h"

#define KWIN_SERVICE "org.kde.KWin"
#define KWIN_SCRIPTING_PATH "/Scripting"
#define KWIN_SCRIPTING_INTERFACE "org.kde.kwin.Scripting"
#define KWIN_SCRIPT_INTERFACE "org.kde.kwin.Script"
/* Script files and plugin names: <prefix>-<pid>[-<n>]. Not the prefix of the
 * client's pool files, so its stale-file sweep leaves them to the host. */
#define SCRIPT_PREFIX HOST_NAMESPACE "-kwin"
#define WINDOW_FIELDS 11

/* Reports each normal window as one tab-separated line, the caption last so
 * that it may contain tabs. `%1$s` is the host's unique bus name. */
static const char tracking_script[] =
	"const service = '%1$s';\n"
	"const send = (member, text) => callDBus(service, '/', '', member, text);\n"
	"const signals = ['captionChanged', 'activeChanged', 'fullScreenChanged', 'minimizedChanged',\n"
	"  'frameGeometryChanged', 'clientGeometryChanged', 'outputChanged'];\n"
	"function report(w) {\n"
	"  const g = w.clientGeometry || w.frameGeometry;\n"
	"  send('window', [String(w.internalId), w.active ? 1 : 0, w.fullScreen ? 1 : 0,\n"
	"    w.minimized ? 1 : 0, Math.round(g.x), Math.round(g.y), Math.round(g.width),\n"
	"    Math.round(g.height), w.output ? w.output.name : '', w.resourceClass || '',\n"
	"    w.caption || ''].join('\\t'));\n"
	"}\n"
	"function watch(w) {\n"
	"  if (!w.normalWindow) return;\n"
	"  for (const s of signals) if (w[s]) w[s].connect(() => report(w));\n"
	"  report(w);\n"
	"}\n"
	"workspace.windowAdded.connect(watch);\n"
	"workspace.windowRemoved.connect((w) => send('closed', String(w.internalId)));\n"
	"for (const w of workspace.windowList()) watch(w);\n";

/* `%1$s` is the window's internal id, `%2$s` the unique bus name and `%3$s`
 * the plugin name, which the `done` call hands back for unloading. */
static const char activation_script[] =
	"for (const w of workspace.windowList()) {\n"
	"  if (String(w.internalId) === '%1$s') workspace.activeWindow = w;\n"
	"}\n"
	"callDBus('%2$s', '/', '', 'done', '%3$s');\n";

struct window {
	char *uuid;
	uint32_t tid;
};

static struct {
	char *owner; /* KWin's unique name; reports from anyone else are ignored */
	bool started;
	struct window *windows;
	size_t window_count;
	unsigned next_script;
} kwin;

static char *script_directory(void)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	return xstrdup(dir && *dir ? dir : "/tmp");
}

static char *script_path(const char *plugin)
{
	char *dir = script_directory();
	size_t len = strlen(dir) + strlen(plugin) + 5;
	char *path = xrealloc(NULL, len);
	snprintf(path, len, "%s/%s.js", dir, plugin);
	free(dir);
	return path;
}

static void unload_script(const char *plugin)
{
	DBusMessage *call = dbus_message_new_method_call(KWIN_SERVICE, KWIN_SCRIPTING_PATH,
							 KWIN_SCRIPTING_INTERFACE, "unloadScript");
	if (call && dbus_message_append_args(call, DBUS_TYPE_STRING, &plugin, DBUS_TYPE_INVALID))
		bus_call(call, NULL, NULL);
	else if (call)
		dbus_message_unref(call);
}

static void remove_script(const char *plugin)
{
	unload_script(plugin);
	char *path = script_path(plugin);
	unlink(path);
	free(path);
}

/* Unloads the scripts of hosts that were killed before they could. */
static void remove_stale_scripts(void)
{
	char *dir = script_directory();
	DIR *handle = opendir(dir);
	free(dir);
	if (!handle)
		return;
	struct dirent *entry;
	while ((entry = readdir(handle))) {
		size_t prefix_len = strlen(SCRIPT_PREFIX "-");
		if (strncmp(entry->d_name, SCRIPT_PREFIX "-", prefix_len) != 0)
			continue;
		char *end;
		long pid = strtol(entry->d_name + prefix_len, &end, 10);
		size_t len = strlen(entry->d_name);
		if (pid <= 0 || (*end != '.' && *end != '-') || len < 4 ||
		    strcmp(entry->d_name + len - 3, ".js") != 0)
			continue;
		if (kill((pid_t)pid, 0) == 0 || errno != ESRCH)
			continue;
		char *plugin = xstrdup(entry->d_name);
		plugin[len - 3] = '\0';
		remove_script(plugin);
		free(plugin);
	}
	closedir(handle);
}

/* Writes the script and starts loading it. */
static bool load_script(const char *plugin, const char *source);

static void run_reply(DBusMessage *reply, void *data)
{
	char *plugin = data;
	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR)
		emit_error("kwin: cannot run script %s: %s", plugin, dbus_message_get_error_name(reply));
	free(plugin);
}

static void load_reply(DBusMessage *reply, void *data)
{
	char *plugin = data;
	dbus_int32_t id = -1;
	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR ||
	    !dbus_message_get_args(reply, NULL, DBUS_TYPE_INT32, &id, DBUS_TYPE_INVALID) || id < 0) {
		emit_error("kwin: cannot load script %s", plugin);
		free(plugin);
		return;
	}
	char path[64];
	snprintf(path, sizeof(path), "/Scripting/Script%d", (int)id);
	DBusMessage *call =
		dbus_message_new_method_call(KWIN_SERVICE, path, KWIN_SCRIPT_INTERFACE, "run");
	if (!call || !bus_call(call, run_reply, plugin))
		free(plugin);
}

static bool load_script(const char *plugin, const char *source)
{
	char *path = script_path(plugin);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	bool written = fd >= 0 && write_all(fd, source, strlen(source));
	if (fd >= 0)
		close(fd);
	if (!written) {
		emit_error("kwin: cannot write %s: %s", path, strerror(errno));
		unlink(path);
		free(path);
		return false;
	}
	DBusMessage *call = dbus_message_new_method_call(KWIN_SERVICE, KWIN_SCRIPTING_PATH,
							 KWIN_SCRIPTING_INTERFACE, "loadScript");
	const char *path_arg = path;
	bool sent = call &&
		    dbus_message_append_args(call, DBUS_TYPE_STRING, &path_arg, DBUS_TYPE_STRING,
					     &plugin, DBUS_TYPE_INVALID) &&
		    bus_call(call, load_reply, xstrdup(plugin));
	if (!sent)
		emit_error("kwin: cannot load %s", path);
	free(path);
	return sent;
}

static struct window *window_find(const char *uuid)
{
	for (size_t i = 0; i < kwin.window_count; i++) {
		if (strcmp(kwin.windows[i].uuid, uuid) == 0)
			return &kwin.windows[i];
	}
	return NULL;
}

/* KWin's internal ids are braced UUIDs. Anything else could break out of
 * the string literal in the activation script. */
static bool uuid_valid(const char *uuid)
{
	if (!*uuid || strlen(uuid) > 64)
		return false;
	for (const char *c = uuid; *c; c++) {
		if (!strchr("0123456789abcdefABCDEF{}-", *c))
			return false;
	}
	return true;
}

static bool field_i32(const char *str, int32_t *out)
{
	char *end;
	errno = 0;
	long value = strtol(str, &end, 10);
	if (errno || end == str || *end || value < INT32_MIN || value > INT32_MAX)
		return false;
	*out = (int32_t)value;
	return true;
}

static void handle_window(char *line)
{
	char *fields[WINDOW_FIELDS];
	size_t count = 0;
	fields[count++] = line;
	while (count < WINDOW_FIELDS) {
		char *tab = strchr(fields[count - 1], '\t');
		if (!tab)
			break;
		*tab = '\0';
		fields[count++] = tab + 1;
	}
	int32_t rect[4];
	if (count < WINDOW_FIELDS || !uuid_valid(fields[0])) {
		log_msg("kwin: malformed window report");
		return;
	}
	for (size_t i = 0; i < 4; i++) {
		if (!field_i32(fields[4 + i], &rect[i])) {
			log_msg("kwin: malformed window geometry");
			return;
		}
	}
	struct window *window = window_find(fields[0]);
	if (!window) {
		kwin.windows = xrealloc(kwin.windows, (kwin.window_count + 1) * sizeof(*kwin.windows));
		window = &kwin.windows[kwin.window_count++];
		window->uuid = xstrdup(fields[0]);
		window->tid = toplevel_next_id();
	}
	const char *output = fields[8];
	ev_begin("toplevel");
	ev_int("tid", window->tid);
	ev_str("title", fields[10]);
	ev_str("appId", fields[9]);
	ev_bool("activated", strcmp(fields[1], "1") == 0);
	ev_bool("fullscreen", strcmp(fields[2], "1") == 0);
	ev_bool("maximized", false);
	ev_bool("minimized", strcmp(fields[3], "1") == 0);
	ev_strings("outputs", &output, *output ? 1 : 0);
	ev_rect("geometry", rect[0], rect[1], rect[2], rect[3]);
	ev_end();
}

static void handle_closed(const char *uuid)
{
	struct window *window = window_find(uuid);
	if (!window)
		return;
	ev_begin("toplevel-closed");
	ev_int("tid", window->tid);
	ev_end();
	free(window->uuid);
	*window = kwin.windows[--kwin.window_count];
}

static DBusHandlerResult filter(DBusConnection *connection, DBusMessage *message, void *data)
{
	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL ||
	    strcmp(dbus_message_get_path(message) ? dbus_message_get_path(message) : "", "/") != 0)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *member = dbus_message_get_member(message);
	const char *sender = dbus_message_get_sender(message);
	if (!member || !sender || !kwin.owner || strcmp(sender, kwin.owner) != 0)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *arg;
	if (!dbus_message_get_args(message, NULL, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (strcmp(member, "window") == 0) {
		char *line = xstrdup(arg);
		handle_window(line);
		free(line);
	} else if (strcmp(member, "closed") == 0) {
		handle_closed(arg);
	} else if (strcmp(member, "done") == 0 &&
		   strncmp(arg, SCRIPT_PREFIX "-", strlen(SCRIPT_PREFIX "-")) == 0) {
		remove_script(arg);
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	/* Unanswered calls would count against KWin's pending reply limit on
	 * the bus until they time out. */
	if (!dbus_message_get_no_reply(message)) {
		DBusMessage *reply = dbus_message_new_method_return(message);
		if (reply)
			bus_send(reply);
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

static char *plugin_name(bool activation)
{
	char name[96];
	if (activation)
		snprintf(name, sizeof(name), SCRIPT_PREFIX "-%ld-%u", (long)getpid(), ++kwin.next_script);
	else
		snprintf(name, sizeof(name), SCRIPT_PREFIX "-%ld", (long)getpid());
	return xstrdup(name);
}

static void kwin_shutdown(void)
{
	char *plugin = plugin_name(false);
	remove_script(plugin);
	free(plugin);
	/* Activation scripts unload themselves through `done`; any that did
	 * not are found by the stale sweep of the next host. */
}

bool kwin_available(void)
{
	if (kwin.owner)
		return true;
	/* The script targets the Plasma 6 API. */
	const char *session = getenv("KDE_SESSION_VERSION");
	if (session && strcmp(session, "5") == 0)
		return false;
	if (!bus_get())
		return false;
	kwin.owner = bus_name_owner(KWIN_SERVICE);
	return kwin.owner != NULL;
}

void kwin_start(void)
{
	if (kwin.started || !kwin_available())
		return;
	DBusConnection *connection = bus_get();
	const char *unique = bus_unique_name();
	if (!connection || !unique)
		return;
	kwin.started = true;
	dbus_connection_add_filter(connection, filter, NULL, NULL);
	remove_stale_scripts();

	char *plugin = plugin_name(false);
	int len = snprintf(NULL, 0, tracking_script, unique);
	char *source = xrealloc(NULL, (size_t)len + 1);
	snprintf(source, (size_t)len + 1, tracking_script, unique);
	if (load_script(plugin, source))
		atexit(kwin_shutdown);
	free(source);
	free(plugin);
}

bool kwin_activate(uint32_t tid)
{
	const char *uuid = NULL;
	for (size_t i = 0; i < kwin.window_count; i++) {
		if (kwin.windows[i].tid == tid)
			uuid = kwin.windows[i].uuid;
	}
	const char *unique = bus_unique_name();
	if (!uuid || !unique)
		return false;
	char *plugin = plugin_name(true);
	int len = snprintf(NULL, 0, activation_script, uuid, unique, plugin);
	char *source = xrealloc(NULL, (size_t)len + 1);
	snprintf(source, (size_t)len + 1, activation_script, uuid, unique, plugin);
	load_script(plugin, source);
	free(source);
	free(plugin);
	return true;
}
