/*
 * overlay-wayland-host: presents Electron overlay windows as layer-shell
 * surfaces. The wire protocol is documented in README.md next to this file.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1-client-protocol.h"
#include "ext-data-control-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include "host.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define NAMESPACE HOST_NAMESPACE
#define CLIPBOARD_READ_MAX (8u << 20)
#define COMMAND_LINE_MAX ((size_t)256 << 20)
#define REPEAT_BURST_MAX 32
#define SEAT_GLOBALS_MAX 16
#define EVDEV_DIR "/dev/input"
#define UINPUT_PATH "/dev/uinput"
#define PROBE_NAMESPACE NAMESPACE "-probe"
/* How long probe surfaces may take to be configured, and how long the pointer
 * then has to enter one. */
#define PROBE_MAP_TIMEOUT_MS 1000
#define PROBE_ENTER_TIMEOUT_MS 200
#define BITS_PER_LONG (sizeof(unsigned long) * CHAR_BIT)
#define BITS_LONGS(bits) (((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG)

/* Utilities */

void log_msg(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fputs("overlay-wayland-host: ", stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
	va_end(args);
}

void *xrealloc(void *ptr, size_t size)
{
	void *result = realloc(ptr, size ? size : 1);
	if (!result) {
		log_msg("out of memory");
		abort();
	}
	return result;
}

void *xcalloc(size_t count, size_t size)
{
	void *result = calloc(count ? count : 1, size ? size : 1);
	if (!result) {
		log_msg("out of memory");
		abort();
	}
	return result;
}

char *xstrdup(const char *str)
{
	size_t len = strlen(str) + 1;
	char *copy = xrealloc(NULL, len);
	memcpy(copy, str, len);
	return copy;
}

static void replace_string(char **dst, const char *src)
{
	free(*dst);
	*dst = src ? xstrdup(src) : NULL;
}

struct strbuf {
	char *data;
	size_t len, cap;
};

static void sb_reserve(struct strbuf *sb, size_t extra)
{
	if (sb->cap - sb->len >= extra)
		return;
	size_t cap = sb->cap ? sb->cap : 256;
	while (cap - sb->len < extra)
		cap *= 2;
	sb->data = xrealloc(sb->data, cap);
	sb->cap = cap;
}

static void sb_append(struct strbuf *sb, const char *data, size_t len)
{
	sb_reserve(sb, len);
	memcpy(sb->data + sb->len, data, len);
	sb->len += len;
}

static void sb_puts(struct strbuf *sb, const char *str)
{
	sb_append(sb, str, strlen(str));
}

static void sb_putc(struct strbuf *sb, char c)
{
	sb_append(sb, &c, 1);
}

static void sb_printf(struct strbuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void sb_printf(struct strbuf *sb, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int len = vsnprintf(NULL, 0, fmt, args);
	va_end(args);
	if (len < 0)
		return;
	sb_reserve(sb, (size_t)len + 1);
	va_start(args, fmt);
	vsnprintf(sb->data + sb->len, (size_t)len + 1, fmt, args);
	va_end(args);
	sb->len += (size_t)len;
}

/* Returns the length of the valid UTF-8 sequence at str, or 0 if invalid. */
static size_t utf8_sequence_len(const unsigned char *str, size_t avail)
{
	unsigned char lead = str[0];
	size_t len;
	uint32_t cp, min;
	if (lead < 0x80)
		return 1;
	if ((lead & 0xe0) == 0xc0) {
		len = 2, cp = lead & 0x1f, min = 0x80;
	} else if ((lead & 0xf0) == 0xe0) {
		len = 3, cp = lead & 0x0f, min = 0x800;
	} else if ((lead & 0xf8) == 0xf0) {
		len = 4, cp = lead & 0x07, min = 0x10000;
	} else {
		return 0;
	}
	if (avail < len)
		return 0;
	for (size_t i = 1; i < len; i++) {
		if ((str[i] & 0xc0) != 0x80)
			return 0;
		cp = cp << 6 | (str[i] & 0x3f);
	}
	if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
		return 0;
	return len;
}

/* Appends a JSON string literal. Invalid UTF-8 becomes U+FFFD. */
static void json_string(struct strbuf *sb, const char *data, size_t len)
{
	const unsigned char *str = (const unsigned char *)data;
	sb_reserve(sb, len + 2);
	sb_putc(sb, '"');
	size_t run = 0;
	for (size_t i = 0; i < len;) {
		unsigned char c = str[i];
		size_t seq = 1;
		const char *escape = NULL;
		char hex[8];
		if (c == '"') {
			escape = "\\\"";
		} else if (c == '\\') {
			escape = "\\\\";
		} else if (c == '\n') {
			escape = "\\n";
		} else if (c == '\r') {
			escape = "\\r";
		} else if (c == '\t') {
			escape = "\\t";
		} else if (c < 0x20 || c == 0x7f) {
			snprintf(hex, sizeof(hex), "\\u%04x", c);
			escape = hex;
		} else if (c >= 0x80) {
			seq = utf8_sequence_len(str + i, len - i);
			if (seq == 0) {
				seq = 1;
				escape = "\\ufffd";
			}
		}
		if (escape) {
			sb_append(sb, data + run, i - run);
			sb_puts(sb, escape);
			run = i + seq;
		}
		i += seq;
	}
	sb_append(sb, data + run, len - run);
	sb_putc(sb, '"');
}

static int base64_value(unsigned char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

/* Decodes standard base64; padding is optional. */
static bool base64_decode(const char *in, char **out, size_t *out_len)
{
	size_t len = strlen(in);
	while (len > 0 && in[len - 1] == '=')
		len--;
	if (len % 4 == 1)
		return false;
	char *buf = xrealloc(NULL, len / 4 * 3 + 3);
	uint32_t acc = 0;
	int bits = 0;
	size_t pos = 0;
	for (size_t i = 0; i < len; i++) {
		int value = base64_value((unsigned char)in[i]);
		if (value < 0) {
			free(buf);
			return false;
		}
		acc = acc << 6 | (uint32_t)value;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			buf[pos++] = (char)(acc >> bits & 0xff);
			acc &= (1u << bits) - 1;
		}
	}
	*out = buf;
	*out_len = pos;
	return true;
}

bool parse_double(const char *str, double *out)
{
	if (!*str)
		return false;
	char *end;
	errno = 0;
	double value = strtod(str, &end);
	if (*end || errno || !(value >= -1e9 && value <= 1e9))
		return false;
	*out = value;
	return true;
}

static int32_t floor_i32(double value)
{
	int32_t truncated = (int32_t)value;
	return (double)truncated > value ? truncated - 1 : truncated;
}

static int32_t ceil_i32(double value)
{
	int32_t truncated = (int32_t)value;
	return (double)truncated < value ? truncated + 1 : truncated;
}

/* Accepts integers and, leniently, fractional values, which are rounded. */
static bool parse_i32(const char *str, int32_t *out)
{
	double value;
	if (!parse_double(str, &value))
		return false;
	*out = floor_i32(value + 0.5);
	return true;
}

bool parse_u32(const char *str, uint32_t *out)
{
	if (*str < '0' || *str > '9')
		return false;
	char *end;
	errno = 0;
	unsigned long value = strtoul(str, &end, 10);
	if (*end || errno || value > UINT32_MAX)
		return false;
	*out = (uint32_t)value;
	return true;
}

/* State */

struct rect {
	int32_t x, y, width, height;
};

struct output {
	struct wl_list link;
	uint32_t global;
	struct wl_output *wl;
	struct zxdg_output_v1 *xdg;
	char *name, *description;
	char *xdg_name, *xdg_description;
	int32_t geometry_x, geometry_y, transform, scale;
	int32_t mode_width, mode_height;
	struct rect logical;
	bool has_logical_position, has_logical_size;
	bool done;
	/* Virtual pointer mapped to this output, from virtual-pointer v2. */
	struct zwlr_virtual_pointer_v1 *vpointer;
};

enum keyboard_mode { KEYBOARD_NONE, KEYBOARD_ON_DEMAND, KEYBOARD_EXCLUSIVE };
enum input_mode { INPUT_ALL, INPUT_NONE, INPUT_RECTS };

struct surface;

struct buffer {
	struct wl_list link; /* in surface.retired while retired */
	struct surface *owner;
	struct wl_buffer *wl;
	uint32_t pool;
	int index;
	/* Attached to a surface and not yet released by the compositor. */
	bool busy, retired;
};

struct surface {
	struct wl_list link;
	char *sid;
	bool numeric_sid;
	bool has_geometry;
	struct rect geometry;
	enum zwlr_layer_shell_v1_layer layer_kind;
	enum keyboard_mode keyboard;
	enum input_mode input;
	struct rect *rects;
	size_t rect_count;
	char *cursor;
	bool visible;

	/* Present while visible and geometry is known. */
	struct wl_surface *wl;
	struct zwlr_layer_surface_v1 *layer;
	struct wp_viewport *viewport;
	struct output *output;
	bool configured;
	/* niri focuses an on_demand overlay surface when it maps, which would take
	 * keys from the game whenever an overlay appears. on_demand is therefore
	 * only applied after the first frame callback following the map. */
	bool keyboard_armed;
	struct wl_callback *arm_callback;

	/* The current pool. Buffers of replaced pools live in `retired` until
	 * the compositor releases them, and never produce `release` events. */
	struct buffer *buffers[2];
	int32_t buffer_width, buffer_height;
	struct wl_list retired;
	uint32_t pool;
	/* Buffer committed while the surface could not show it, presented on the
	 * next configure. Never attached, so it is not busy. */
	int pending_index;
};

struct toplevel {
	struct wl_list link;
	uint32_t id;
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	char *title, *app_id;
	bool activated, fullscreen, maximized, minimized;
	struct wl_output **outputs;
	size_t output_count;
};

/* Text MIME types in read preference order; each owns one bit in dc_offer. */
static const char *const read_text_mimes[] = {
	"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT",
};
static const char *const source_text_mimes[] = {
	"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT",
};
#define MIME_HTML_BIT (1u << ARRAY_LEN(read_text_mimes))
#define MIME_HTML "text/html"

struct dc_offer {
	struct wl_list link;
	void *proxy;
	unsigned mimes;
};

struct payload {
	size_t refs, len;
	char data[];
};

struct dc_source {
	struct wl_list link;
	void *proxy;
	struct payload *text, *html;
};

struct clip_write {
	struct wl_list link;
	int fd;
	struct payload *payload;
	size_t offset;
};

struct clip_read {
	int fd; /* -1 once finished */
	bool active, failed;
	struct strbuf data;
};

struct seat_global {
	uint32_t name, version;
};

struct evdev_device {
	struct wl_list link;
	int fd;
	dev_t rdev;
	char *name; /* entry in EVDEV_DIR */
	/* Events are discarded from SYN_DROPPED until the next SYN_REPORT. */
	bool dropping;
	/* Keys and buttons currently down, to resync after SYN_DROPPED and to
	 * release them when the device disappears. */
	unsigned long keys[BITS_LONGS(KEY_CNT)];
};

struct capture {
	struct wl_list link;
	char *id, *path;
	struct zwlr_screencopy_frame_v1 *frame;
	struct wl_buffer *buffer;
	void *data;
	size_t size;
	bool has_shm;
	uint32_t format, width, height, stride, flags;
	/* wl_output transform of the captured output, undone when writing. */
	int32_t transform;
};

struct probe_surface {
	struct wl_list link;
	struct output *output;
	struct wl_surface *wl;
	struct zwlr_layer_surface_v1 *layer;
	struct wp_viewport *viewport;
	/* Transparent buffer of the configured size, used without wp_viewporter. */
	struct wl_buffer *buffer;
	bool committed;
};

static struct host {
	bool probe, ready;
	/* Fallbacks for compositors without the wlroots protocols, decided
	 * before `ready`. */
	bool kwin_script, remote_desktop;
	struct wl_display *display;
	struct wl_registry *registry;

	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	uint32_t layer_shell_global;
	struct wp_viewporter *viewporter;
	uint32_t viewporter_global;
	struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
	uint32_t cursor_shape_global;
	struct zxdg_output_manager_v1 *xdg_output_manager;
	uint32_t xdg_output_global;

	/* Bound after `ready` so that their events follow it. */
	uint32_t toplevel_global, toplevel_version;
	struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;
	uint32_t dc_ext_global, dc_ext_version, dc_wlr_global, dc_wlr_version;
	uint32_t dc_bound_global;
	bool dc_ext;
	void *dc_manager, *dc_device;
	struct dc_offer *selection_offer, *primary_offer;
	struct wl_list dc_offers, dc_sources, clip_writes;
	struct clip_read clip_reads[2]; /* text, html */

	struct wl_list outputs, surfaces, toplevels;
	bool outputs_dirty;
	uint32_t next_toplevel_id;

	struct seat_global seat_globals[SEAT_GLOBALS_MAX];
	size_t seat_global_count;
	struct wl_seat *seat;
	uint32_t seat_global;

	struct wl_pointer *pointer;
	struct wp_cursor_shape_device_v1 *cursor_shape_device;
	struct wl_cursor_theme *cursor_theme;
	struct wl_surface *cursor_surface;
	struct surface *pointer_focus;
	uint32_t pointer_serial;
	double pointer_x, pointer_y;
	bool axis_pending;
	double axis_dx, axis_dy;
	int32_t axis_value120_x, axis_value120_y;

	struct wl_keyboard *keyboard;
	struct xkb_context *xkb_context;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	struct surface *keyboard_focus;
	int32_t repeat_rate, repeat_delay;
	uint32_t repeat_key;
	bool repeating;
	int repeat_fd;

	bool evdev_on;
	int evdev_inotify_fd;
	struct wl_list evdev_devices;
	int evdev_reported; /* device count last emitted, -1 for none */
	/* Devices holding each key, so a key held on two keyboards is reported
	 * as one press and one release. */
	uint16_t evdev_key_holders[KEY_CNT];

	struct zwp_virtual_keyboard_manager_v1 *vk_manager;
	uint32_t vk_manager_global;
	struct zwp_virtual_keyboard_v1 *vk;
	struct xkb_state *vk_state;
	char *vk_keymap_text; /* the keymap uploaded to vk */
	bool vk_default_keymap; /* vk was created before the seat sent a keymap */

	/* uinput keyboard, for compositors without a virtual keyboard. */
	int uinput_fd;
	bool uinput_failed;
	char uinput_phys[64]; /* identifies it among the evdev devices */

	struct zwlr_virtual_pointer_manager_v1 *vp_manager;
	uint32_t vp_manager_global;
	/* Pointer without an output, for warps with virtual-pointer v1 and for
	 * probe nudges. */
	struct zwlr_virtual_pointer_v1 *vpointer;

	struct zwlr_screencopy_manager_v1 *screencopy;
	uint32_t screencopy_global;
	struct wl_list captures;

	/* Concurrent probes share one set of surfaces and one result. */
	char **probe_ids;
	size_t probe_id_count;
	struct wl_list probe_surfaces;
	bool probe_mapped;
	int probe_fd;
	struct wl_buffer *probe_buffer; /* 1x1 transparent, shared */

	struct strbuf stdin_buf;
	bool stdin_discarding;
	struct strbuf event;
} host;

static void probe_output_removed(struct output *output);
static bool probe_pointer_enter(struct wl_surface *wl, wl_fixed_t x, wl_fixed_t y);
static void vk_keymap_changed(void);
static void vk_destroy(void);
static bool uinput_ensure(void);
static void vp_destroy_all(void);

/* Events */

static void stdout_write(const char *data, size_t len)
{
	while (len > 0) {
		ssize_t written = write(STDOUT_FILENO, data, len);
		if (written > 0) {
			data += written;
			len -= (size_t)written;
		} else if (written < 0 && errno == EINTR) {
			continue;
		} else if (written < 0 && errno == EAGAIN) {
			struct pollfd pfd = { .fd = STDOUT_FILENO, .events = POLLOUT };
			poll(&pfd, 1, -1);
		} else {
			log_msg("stdout closed, exiting");
			exit(1);
		}
	}
}

void ev_begin(const char *name)
{
	host.event.len = 0;
	sb_puts(&host.event, "{\"ev\":");
	json_string(&host.event, name, strlen(name));
}

void ev_key(const char *key)
{
	sb_putc(&host.event, ',');
	json_string(&host.event, key, strlen(key));
	sb_putc(&host.event, ':');
}

void ev_strn(const char *key, const char *value, size_t len)
{
	ev_key(key);
	if (value)
		json_string(&host.event, value, len);
	else
		sb_puts(&host.event, "null");
}

void ev_str(const char *key, const char *value)
{
	ev_strn(key, value, value ? strlen(value) : 0);
}

void ev_int(const char *key, long long value)
{
	ev_key(key);
	sb_printf(&host.event, "%lld", value);
}

void ev_num(const char *key, double value)
{
	ev_key(key);
	sb_printf(&host.event, "%.10g", value == value ? value : 0.0);
}

void ev_bool(const char *key, bool value)
{
	ev_key(key);
	sb_puts(&host.event, value ? "true" : "false");
}

void ev_strings(const char *key, const char *const *values, size_t count)
{
	ev_key(key);
	sb_putc(&host.event, '[');
	for (size_t i = 0; i < count; i++) {
		if (i > 0)
			sb_putc(&host.event, ',');
		json_string(&host.event, values[i], strlen(values[i]));
	}
	sb_putc(&host.event, ']');
}

void ev_rect(const char *key, int32_t x, int32_t y, int32_t width, int32_t height)
{
	ev_key(key);
	sb_printf(&host.event, "{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}", x, y, width,
		  height);
}

/* Numeric sids are echoed as JSON numbers, anything else as strings. */
static void ev_sid(const struct surface *surface)
{
	ev_key("sid");
	if (surface->numeric_sid)
		sb_puts(&host.event, surface->sid);
	else
		json_string(&host.event, surface->sid, strlen(surface->sid));
}

void ev_end(void)
{
	sb_puts(&host.event, "}\n");
	stdout_write(host.event.data, host.event.len);
}

static void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

static void fatal(const char *fmt, ...)
{
	char message[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);
	log_msg("%s", message);
	ev_begin("error");
	ev_str("message", message);
	ev_end();
	exit(1);
}

/* Non-fatal error the client may be waiting on, such as a failed pool. */
void emit_error(const char *fmt, ...)
{
	char message[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);
	log_msg("%s", message);
	ev_begin("error");
	ev_str("message", message);
	ev_end();
}

static void display_failed(void)
{
	int err = wl_display_get_error(host.display);
	if (err == EPROTO) {
		const struct wl_interface *interface = NULL;
		uint32_t id = 0;
		uint32_t code = wl_display_get_protocol_error(host.display, &interface, &id);
		fatal("Wayland protocol error %u on %s@%u", code,
		      interface ? interface->name : "unknown", id);
	}
	fatal("Wayland connection lost: %s", strerror(err ? err : errno));
}

static void *bind_global(uint32_t name, const struct wl_interface *interface,
			 uint32_t advertised, uint32_t max)
{
	uint32_t version = advertised < max ? advertised : max;
	if (version > (uint32_t)interface->version)
		version = (uint32_t)interface->version;
	return wl_registry_bind(host.registry, name, interface, version);
}

/* Outputs */

static struct rect output_logical(const struct output *output)
{
	struct rect rect = { output->geometry_x, output->geometry_y, 0, 0 };
	if (output->has_logical_position) {
		rect.x = output->logical.x;
		rect.y = output->logical.y;
	}
	if (output->has_logical_size) {
		rect.width = output->logical.width;
		rect.height = output->logical.height;
	} else {
		int32_t scale = output->scale > 0 ? output->scale : 1;
		bool rotated = output->transform & 1;
		rect.width = (rotated ? output->mode_height : output->mode_width) / scale;
		rect.height = (rotated ? output->mode_width : output->mode_height) / scale;
	}
	return rect;
}

/* Physical pixels per logical pixel, fractional under fractional scaling. */
static double output_scale(const struct output *output)
{
	int32_t physical = output->transform & 1 ? output->mode_height : output->mode_width;
	if (output->has_logical_size && output->logical.width > 0 && physical > 0)
		return (double)physical / output->logical.width;
	return output->scale > 0 ? output->scale : 1;
}

static const char *output_name(const struct output *output)
{
	return output->name ? output->name : output->xdg_name ? output->xdg_name : "";
}

static struct output *output_from_wl(struct wl_output *wl)
{
	struct output *output;
	wl_list_for_each(output, &host.outputs, link) {
		if (output->wl == wl)
			return output;
	}
	return NULL;
}

static void emit_outputs(void)
{
	host.outputs_dirty = false;
	ev_begin("outputs");
	ev_key("outputs");
	sb_putc(&host.event, '[');
	bool first = true;
	struct output *output;
	wl_list_for_each(output, &host.outputs, link) {
		if (!output->done)
			continue;
		struct rect rect = output_logical(output);
		const char *description = output->description ? output->description
					: output->xdg_description ? output->xdg_description : "";
		if (!first)
			sb_putc(&host.event, ',');
		first = false;
		sb_puts(&host.event, "{\"name\":");
		json_string(&host.event, output_name(output), strlen(output_name(output)));
		sb_puts(&host.event, ",\"description\":");
		json_string(&host.event, description, strlen(description));
		sb_printf(&host.event, ",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,\"scale\":%.10g}",
			  rect.x, rect.y, rect.width, rect.height, output_scale(output));
	}
	sb_putc(&host.event, ']');
	ev_end();
}

static void output_geometry(void *data, struct wl_output *wl, int32_t x, int32_t y,
			    int32_t physical_width, int32_t physical_height, int32_t subpixel,
			    const char *make, const char *model, int32_t transform)
{
	struct output *output = data;
	output->geometry_x = x;
	output->geometry_y = y;
	output->transform = transform;
}

static void output_mode(void *data, struct wl_output *wl, uint32_t flags, int32_t width,
			int32_t height, int32_t refresh)
{
	struct output *output = data;
	if (!(flags & WL_OUTPUT_MODE_CURRENT))
		return;
	output->mode_width = width;
	output->mode_height = height;
}

static void output_done(void *data, struct wl_output *wl)
{
	struct output *output = data;
	output->done = true;
	host.outputs_dirty = true;
}

static void output_scale_event(void *data, struct wl_output *wl, int32_t factor)
{
	struct output *output = data;
	output->scale = factor;
}

#ifdef WL_OUTPUT_NAME_SINCE_VERSION
static void output_name_event(void *data, struct wl_output *wl, const char *name)
{
	struct output *output = data;
	replace_string(&output->name, name);
}

static void output_description(void *data, struct wl_output *wl, const char *description)
{
	struct output *output = data;
	replace_string(&output->description, description);
}
#endif

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale_event,
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
	.name = output_name_event,
	.description = output_description,
#endif
};

static void xdg_output_logical_position(void *data, struct zxdg_output_v1 *xdg, int32_t x, int32_t y)
{
	struct output *output = data;
	output->logical.x = x;
	output->logical.y = y;
	output->has_logical_position = true;
}

static void xdg_output_logical_size(void *data, struct zxdg_output_v1 *xdg, int32_t width,
				    int32_t height)
{
	struct output *output = data;
	output->logical.width = width;
	output->logical.height = height;
	output->has_logical_size = true;
}

static void xdg_output_done(void *data, struct zxdg_output_v1 *xdg)
{
	host.outputs_dirty = true;
}

static void xdg_output_name(void *data, struct zxdg_output_v1 *xdg, const char *name)
{
	struct output *output = data;
	replace_string(&output->xdg_name, name);
}

static void xdg_output_description(void *data, struct zxdg_output_v1 *xdg, const char *description)
{
	struct output *output = data;
	replace_string(&output->xdg_description, description);
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_logical_position,
	.logical_size = xdg_output_logical_size,
	.done = xdg_output_done,
	.name = xdg_output_name,
	.description = xdg_output_description,
};

static void output_create_xdg(struct output *output)
{
	if (output->xdg || !host.xdg_output_manager)
		return;
	output->xdg = zxdg_output_manager_v1_get_xdg_output(host.xdg_output_manager, output->wl);
	zxdg_output_v1_add_listener(output->xdg, &xdg_output_listener, output);
}

static void output_add(uint32_t name, uint32_t version)
{
	struct output *output = xcalloc(1, sizeof(*output));
	output->global = name;
	output->scale = 1;
	output->wl = bind_global(name, &wl_output_interface, version, 4);
	wl_output_add_listener(output->wl, &output_listener, output);
	wl_list_insert(host.outputs.prev, &output->link);
	output_create_xdg(output);
}

static void output_destroy(struct output *output)
{
	struct surface *surface;
	wl_list_for_each(surface, &host.surfaces, link) {
		if (surface->output == output)
			surface->output = NULL;
	}
	struct toplevel *toplevel;
	wl_list_for_each(toplevel, &host.toplevels, link) {
		for (size_t i = 0; i < toplevel->output_count; i++) {
			if (toplevel->outputs[i] != output->wl)
				continue;
			toplevel->outputs[i] = toplevel->outputs[--toplevel->output_count];
			break;
		}
	}
	probe_output_removed(output);
	if (output->vpointer)
		zwlr_virtual_pointer_v1_destroy(output->vpointer);
	if (output->xdg)
		zxdg_output_v1_destroy(output->xdg);
	if (wl_output_get_version(output->wl) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
		wl_output_release(output->wl);
	else
		wl_output_destroy(output->wl);
	wl_list_remove(&output->link);
	free(output->name);
	free(output->description);
	free(output->xdg_name);
	free(output->xdg_description);
	free(output);
	host.outputs_dirty = true;
}

/* Picks the output containing the rect's center, else the one with the
 * largest intersection, else the first one. */
static struct output *output_for_rect(const struct rect *rect)
{
	struct output *output, *best = NULL, *first = NULL;
	int64_t best_area = 0;
	int64_t cx = (int64_t)rect->x + rect->width / 2;
	int64_t cy = (int64_t)rect->y + rect->height / 2;
	wl_list_for_each(output, &host.outputs, link) {
		if (!output->done)
			continue;
		if (!first)
			first = output;
		struct rect r = output_logical(output);
		if (cx >= r.x && cx < (int64_t)r.x + r.width && cy >= r.y && cy < (int64_t)r.y + r.height)
			return output;
		int64_t left = rect->x > r.x ? rect->x : r.x;
		int64_t top = rect->y > r.y ? rect->y : r.y;
		int64_t right = (int64_t)rect->x + rect->width;
		int64_t bottom = (int64_t)rect->y + rect->height;
		if ((int64_t)r.x + r.width < right)
			right = (int64_t)r.x + r.width;
		if ((int64_t)r.y + r.height < bottom)
			bottom = (int64_t)r.y + r.height;
		int64_t area = right > left && bottom > top ? (right - left) * (bottom - top) : 0;
		if (area > best_area) {
			best_area = area;
			best = output;
		}
	}
	return best ? best : first;
}

/* Surfaces */

static struct surface *surface_find(const char *sid)
{
	struct surface *surface;
	wl_list_for_each(surface, &host.surfaces, link) {
		if (strcmp(surface->sid, sid) == 0)
			return surface;
	}
	return NULL;
}

static struct surface *surface_from_wl(struct wl_surface *wl)
{
	if (!wl)
		return NULL;
	struct surface *surface;
	wl_list_for_each(surface, &host.surfaces, link) {
		if (surface->wl == wl)
			return surface;
	}
	return NULL;
}

static void emit_release(struct surface *surface, uint32_t pool, int index)
{
	ev_begin("release");
	ev_sid(surface);
	ev_int("pool", pool);
	ev_int("index", index);
	ev_end();
}

static void buffer_destroy(struct buffer *buffer)
{
	if (buffer->retired)
		wl_list_remove(&buffer->link);
	wl_buffer_destroy(buffer->wl);
	free(buffer);
}

static void buffer_release(void *data, struct wl_buffer *wl)
{
	struct buffer *buffer = data;
	if (buffer->retired) {
		buffer_destroy(buffer);
		return;
	}
	if (!buffer->busy)
		return;
	buffer->busy = false;
	emit_release(buffer->owner, buffer->pool, buffer->index);
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void surface_destroy_retired(struct surface *surface)
{
	struct buffer *buffer, *tmp;
	wl_list_for_each_safe(buffer, tmp, &surface->retired, link)
		buffer_destroy(buffer);
}

static void keyboard_stop_repeat(void)
{
	host.repeating = false;
	struct itimerspec off = { 0 };
	timerfd_settime(host.repeat_fd, 0, &off, NULL);
}

static void emit_pointer(struct surface *surface, const char *type)
{
	ev_begin("pointer");
	ev_sid(surface);
	ev_str("type", type);
	ev_num("x", host.pointer_x);
	ev_num("y", host.pointer_y);
	ev_end();
}

static void emit_focus(struct surface *surface, bool focused)
{
	ev_begin("focus");
	ev_sid(surface);
	ev_bool("focused", focused);
	ev_end();
}

static void pointer_clear_focus(void)
{
	if (!host.pointer_focus)
		return;
	emit_pointer(host.pointer_focus, "leave");
	host.pointer_focus = NULL;
	host.axis_pending = false;
}

static void keyboard_clear_focus(void)
{
	if (!host.keyboard_focus)
		return;
	emit_focus(host.keyboard_focus, false);
	host.keyboard_focus = NULL;
	keyboard_stop_repeat();
}

/* Destroys the layer surface and wl_surface. Attached buffers stay busy until
 * the compositor releases them. */
static void surface_unmap(struct surface *surface)
{
	if (!surface->wl)
		return;
	if (host.pointer_focus == surface)
		pointer_clear_focus();
	if (host.keyboard_focus == surface)
		keyboard_clear_focus();
	if (surface->arm_callback)
		wl_callback_destroy(surface->arm_callback);
	surface->arm_callback = NULL;
	surface->keyboard_armed = false;
	if (surface->viewport)
		wp_viewport_destroy(surface->viewport);
	if (surface->layer)
		zwlr_layer_surface_v1_destroy(surface->layer);
	wl_surface_destroy(surface->wl);
	surface->viewport = NULL;
	surface->layer = NULL;
	surface->wl = NULL;
	surface->output = NULL;
	surface->configured = false;
}

static void surface_apply_geometry(struct surface *surface)
{
	int32_t top = surface->geometry.y, left = surface->geometry.x;
	if (surface->output) {
		struct rect rect = output_logical(surface->output);
		top -= rect.y;
		left -= rect.x;
	}
	zwlr_layer_surface_v1_set_margin(surface->layer, top, 0, 0, left);
	zwlr_layer_surface_v1_set_size(surface->layer, (uint32_t)surface->geometry.width,
				       (uint32_t)surface->geometry.height);
	if (surface->viewport)
		wp_viewport_set_destination(surface->viewport, surface->geometry.width,
					    surface->geometry.height);
}

static void surface_apply_keyboard(struct surface *surface)
{
	uint32_t mode = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
	if (surface->keyboard == KEYBOARD_EXCLUSIVE)
		mode = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
	else if (surface->keyboard == KEYBOARD_ON_DEMAND && surface->keyboard_armed &&
		 zwlr_layer_surface_v1_get_version(surface->layer) >=
			 ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND_SINCE_VERSION)
		mode = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
	zwlr_layer_surface_v1_set_keyboard_interactivity(surface->layer, mode);
}

static void surface_apply_input(struct surface *surface)
{
	if (surface->input == INPUT_ALL) {
		wl_surface_set_input_region(surface->wl, NULL);
		return;
	}
	struct wl_region *region = wl_compositor_create_region(host.compositor);
	if (surface->input == INPUT_RECTS) {
		for (size_t i = 0; i < surface->rect_count; i++) {
			const struct rect *rect = &surface->rects[i];
			wl_region_add(region, rect->x, rect->y, rect->width, rect->height);
		}
	}
	wl_surface_set_input_region(surface->wl, region);
	wl_region_destroy(region);
}

static void arm_keyboard_done(void *data, struct wl_callback *callback, uint32_t time)
{
	struct surface *surface = data;
	wl_callback_destroy(callback);
	surface->arm_callback = NULL;
	surface->keyboard_armed = true;
	if (surface->keyboard != KEYBOARD_ON_DEMAND)
		return;
	surface_apply_keyboard(surface);
	wl_surface_commit(surface->wl);
}

static const struct wl_callback_listener arm_keyboard_listener = {
	.done = arm_keyboard_done,
};

static void surface_present(struct surface *surface, int index, const struct rect *damage)
{
	struct buffer *buffer = surface->buffers[index];
	if (buffer->busy)
		log_msg("surface %s: presenting buffer %d while it is busy", surface->sid, index);
	wl_surface_attach(surface->wl, buffer->wl, 0, 0);
	if (!surface->viewport) {
		/* Without wp_viewporter only integer scales can be shown sharp. */
		int32_t scale = surface->buffer_width / surface->geometry.width;
		if (scale < 1 || surface->buffer_width != scale * surface->geometry.width ||
		    surface->buffer_height != scale * surface->geometry.height)
			scale = 1;
		wl_surface_set_buffer_scale(surface->wl, scale);
	}
	if (wl_surface_get_version(surface->wl) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) {
		if (damage)
			wl_surface_damage_buffer(surface->wl, damage->x, damage->y, damage->width,
						 damage->height);
		else
			wl_surface_damage_buffer(surface->wl, 0, 0, INT32_MAX, INT32_MAX);
	} else {
		wl_surface_damage(surface->wl, 0, 0, INT32_MAX, INT32_MAX);
	}
	buffer->busy = true;
	if (!surface->keyboard_armed && !surface->arm_callback) {
		surface->arm_callback = wl_surface_frame(surface->wl);
		wl_callback_add_listener(surface->arm_callback, &arm_keyboard_listener, surface);
	}
	wl_surface_commit(surface->wl);
}

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *layer,
				    uint32_t serial, uint32_t width, uint32_t height)
{
	struct surface *surface = data;
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	ev_begin("configure");
	ev_sid(surface);
	ev_int("width", width);
	ev_int("height", height);
	ev_end();
	int pending = surface->configured ? -1 : surface->pending_index;
	surface->configured = true;
	if (pending >= 0 && surface->buffers[pending]) {
		surface->pending_index = -1;
		surface_present(surface, pending, NULL);
	} else {
		wl_surface_commit(surface->wl);
	}
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
	struct surface *surface = data;
	surface_unmap(surface);
	surface->visible = false;
	ev_begin("closed");
	ev_sid(surface);
	ev_end();
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void surface_map(struct surface *surface)
{
	if (surface->wl || !surface->visible)
		return;
	if (!surface->has_geometry) {
		log_msg("surface %s: shown before geometry, waiting for it", surface->sid);
		return;
	}
	if (!host.layer_shell || !host.compositor) {
		log_msg("surface %s: zwlr_layer_shell_v1 is unavailable", surface->sid);
		return;
	}
	surface->output = output_for_rect(&surface->geometry);
	surface->wl = wl_compositor_create_surface(host.compositor);
	surface->layer = zwlr_layer_shell_v1_get_layer_surface(
		host.layer_shell, surface->wl, surface->output ? surface->output->wl : NULL,
		surface->layer_kind, NAMESPACE);
	zwlr_layer_surface_v1_add_listener(surface->layer, &layer_surface_listener, surface);
	zwlr_layer_surface_v1_set_anchor(surface->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
							 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_exclusive_zone(surface->layer, -1);
	if (host.viewporter)
		surface->viewport = wp_viewporter_get_viewport(host.viewporter, surface->wl);
	surface_apply_geometry(surface);
	surface_apply_keyboard(surface);
	surface_apply_input(surface);
	wl_surface_commit(surface->wl);
}

static void surface_buffers_destroy(struct surface *surface)
{
	for (int i = 0; i < 2; i++) {
		if (surface->buffers[i])
			buffer_destroy(surface->buffers[i]);
		surface->buffers[i] = NULL;
	}
	surface_destroy_retired(surface);
}

static void surface_destroy(struct surface *surface)
{
	surface_unmap(surface);
	surface_buffers_destroy(surface);
	wl_list_remove(&surface->link);
	free(surface->sid);
	free(surface->rects);
	free(surface->cursor);
	free(surface);
}

/* Cursor */

static const struct cursor_name {
	const char *css;
	enum wp_cursor_shape_device_v1_shape shape;
	const char *xcursor, *legacy;
} cursor_names[] = {
	{ "default", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT, "default", "left_ptr" },
	{ "pointer", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER, "pointer", "hand2" },
	{ "text", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT, "text", "xterm" },
	{ "crosshair", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR, "crosshair", "cross" },
	{ "move", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE, "move", "fleur" },
	{ "grab", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB, "grab", "openhand" },
	{ "grabbing", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING, "grabbing", "closedhand" },
	{ "wait", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT, "wait", "watch" },
	{ "progress", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS, "progress", "left_ptr_watch" },
	{ "help", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP, "help", "question_arrow" },
	{ "not-allowed", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED, "not-allowed", "crossed_circle" },
	{ "nodrop", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED, "not-allowed", "crossed_circle" },
	{ "no-drop", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP, "no-drop", "crossed_circle" },
	{ "e-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE, "e-resize", "right_side" },
	{ "w-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE, "w-resize", "left_side" },
	{ "ew-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE, "ew-resize", "sb_h_double_arrow" },
	{ "n-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE, "n-resize", "top_side" },
	{ "s-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE, "s-resize", "bottom_side" },
	{ "ns-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE, "ns-resize", "sb_v_double_arrow" },
	{ "ne-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE, "ne-resize", "top_right_corner" },
	{ "sw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE, "sw-resize", "bottom_left_corner" },
	{ "nesw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE, "nesw-resize", "fd_double_arrow" },
	{ "nw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE, "nw-resize", "top_left_corner" },
	{ "se-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE, "se-resize", "bottom_right_corner" },
	{ "nwse-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE, "nwse-resize", "bd_double_arrow" },
	{ "col-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE, "col-resize", "sb_h_double_arrow" },
	{ "row-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE, "row-resize", "sb_v_double_arrow" },
	{ "all-scroll", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL, "all-scroll", "fleur" },
	{ "zoom-in", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN, "zoom-in", "zoom_in" },
	{ "zoom-out", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT, "zoom-out", "zoom_out" },
	{ "copy", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY, "copy", "dnd-copy" },
	{ "alias", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS, "alias", "dnd-link" },
	{ "cell", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL, "cell", "plus" },
	{ "context-menu", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU, "context-menu", "left_ptr" },
	{ "vertical-text", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT, "vertical-text", "xterm" },
};

static const struct cursor_name *cursor_lookup(const char *css)
{
	for (size_t i = 0; i < ARRAY_LEN(cursor_names); i++) {
		if (strcmp(cursor_names[i].css, css) == 0)
			return &cursor_names[i];
	}
	return &cursor_names[0];
}

static void pointer_set_xcursor(const struct cursor_name *entry)
{
	if (!host.cursor_theme) {
		const char *size_env = getenv("XCURSOR_SIZE");
		uint32_t size = 24;
		if (size_env && (!parse_u32(size_env, &size) || size == 0 || size > 512))
			size = 24;
		host.cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), (int)size, host.shm);
		if (!host.cursor_theme) {
			log_msg("failed to load cursor theme");
			return;
		}
	}
	const char *candidates[] = { entry->xcursor, entry->legacy, "default", "left_ptr" };
	struct wl_cursor *cursor = NULL;
	for (size_t i = 0; i < ARRAY_LEN(candidates) && !cursor; i++)
		cursor = wl_cursor_theme_get_cursor(host.cursor_theme, candidates[i]);
	if (!cursor || cursor->image_count == 0)
		return;
	struct wl_cursor_image *image = cursor->images[0];
	struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
	if (!buffer)
		return;
	if (!host.cursor_surface)
		host.cursor_surface = wl_compositor_create_surface(host.compositor);
	wl_pointer_set_cursor(host.pointer, host.pointer_serial, host.cursor_surface,
			      (int32_t)image->hotspot_x, (int32_t)image->hotspot_y);
	wl_surface_attach(host.cursor_surface, buffer, 0, 0);
	wl_surface_damage(host.cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
	wl_surface_commit(host.cursor_surface);
}

static void pointer_apply_cursor(struct surface *surface)
{
	if (!host.pointer)
		return;
	const char *name = surface->cursor ? surface->cursor : "default";
	if (strcmp(name, "none") == 0) {
		wl_pointer_set_cursor(host.pointer, host.pointer_serial, NULL, 0, 0);
		return;
	}
	const struct cursor_name *entry = cursor_lookup(name);
	if (host.cursor_shape_device)
		wp_cursor_shape_device_v1_set_shape(host.cursor_shape_device, host.pointer_serial,
						    entry->shape);
	else
		pointer_set_xcursor(entry);
}

/* Pointer */

static void emit_axis(void)
{
	bool pending = host.axis_pending;
	double dx = host.axis_dx, dy = host.axis_dy;
	int32_t value120_x = host.axis_value120_x, value120_y = host.axis_value120_y;
	host.axis_pending = false;
	host.axis_dx = host.axis_dy = 0;
	host.axis_value120_x = host.axis_value120_y = 0;
	if (!host.pointer_focus || !pending)
		return;
	ev_begin("axis");
	ev_sid(host.pointer_focus);
	ev_num("x", host.pointer_x);
	ev_num("y", host.pointer_y);
	ev_num("dx", dx);
	ev_num("dy", dy);
	ev_num("discreteX", value120_x / 120.0);
	ev_num("discreteY", value120_y / 120.0);
	ev_end();
}

static bool pointer_has_frames(void)
{
	return wl_pointer_get_version(host.pointer) >= WL_POINTER_FRAME_SINCE_VERSION;
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
			  struct wl_surface *wl, wl_fixed_t x, wl_fixed_t y)
{
	host.pointer_serial = serial;
	if (probe_pointer_enter(wl, x, y)) {
		host.pointer_focus = NULL;
		return;
	}
	host.pointer_focus = surface_from_wl(wl);
	host.pointer_x = wl_fixed_to_double(x);
	host.pointer_y = wl_fixed_to_double(y);
	if (!host.pointer_focus)
		return;
	emit_pointer(host.pointer_focus, "enter");
	pointer_apply_cursor(host.pointer_focus);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
			  struct wl_surface *wl)
{
	pointer_clear_focus();
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x,
			   wl_fixed_t y)
{
	host.pointer_x = wl_fixed_to_double(x);
	host.pointer_y = wl_fixed_to_double(y);
	if (host.pointer_focus)
		emit_pointer(host.pointer_focus, "motion");
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
			   uint32_t code, uint32_t state)
{
	if (!host.pointer_focus)
		return;
	const char *button;
	switch (code) {
	case BTN_LEFT: button = "left"; break;
	case BTN_RIGHT: button = "right"; break;
	case BTN_MIDDLE: button = "middle"; break;
	case BTN_SIDE: button = "back"; break;
	case BTN_EXTRA: button = "forward"; break;
	default: return;
	}
	ev_begin("button");
	ev_sid(host.pointer_focus);
	ev_str("button", button);
	ev_bool("pressed", state == WL_POINTER_BUTTON_STATE_PRESSED);
	ev_num("x", host.pointer_x);
	ev_num("y", host.pointer_y);
	ev_end();
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
			 wl_fixed_t value)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		host.axis_dy += wl_fixed_to_double(value);
	else
		host.axis_dx += wl_fixed_to_double(value);
	host.axis_pending = true;
	if (!pointer_has_frames())
		emit_axis();
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
	emit_axis();
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
}

static void pointer_add_value120(uint32_t axis, int32_t value120)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		host.axis_value120_y += value120;
	else
		host.axis_value120_x += value120;
	host.axis_pending = true;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
				  int32_t discrete)
{
	/* Superseded by axis_value120 from version 8; compositors send one or the other. */
	pointer_add_value120(axis, discrete * 120);
}

#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis,
				  int32_t value120)
{
	pointer_add_value120(axis, value120);
}
#endif

#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
static void pointer_axis_relative_direction(void *data, struct wl_pointer *pointer, uint32_t axis,
					    uint32_t direction)
{
}
#endif

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
	.axis_value120 = pointer_axis_value120,
#endif
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
	.axis_relative_direction = pointer_axis_relative_direction,
#endif
};

static void cursor_shape_device_create(void)
{
	if (host.cursor_shape_device || !host.pointer || !host.cursor_shape_manager)
		return;
	host.cursor_shape_device =
		wp_cursor_shape_manager_v1_get_pointer(host.cursor_shape_manager, host.pointer);
}

static void pointer_release(void)
{
	if (!host.pointer)
		return;
	pointer_clear_focus();
	if (host.cursor_shape_device)
		wp_cursor_shape_device_v1_destroy(host.cursor_shape_device);
	host.cursor_shape_device = NULL;
	if (wl_pointer_get_version(host.pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
		wl_pointer_release(host.pointer);
	else
		wl_pointer_destroy(host.pointer);
	host.pointer = NULL;
}

/* Keyboard */

static void emit_key(uint32_t keycode, bool pressed, bool repeat)
{
	struct xkb_state *state = host.xkb_state;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(state, keycode);
	char keysym[64];
	if (xkb_keysym_get_name(sym, keysym, sizeof(keysym)) < 0)
		snprintf(keysym, sizeof(keysym), "NoSymbol");
	char text[64];
	int text_len = xkb_state_key_get_utf8(state, keycode, text, sizeof(text));
	if (text_len < 0 || (size_t)text_len >= sizeof(text))
		text_len = 0;
	if (text_len == 1 && ((unsigned char)text[0] < 0x20 || text[0] == 0x7f))
		text_len = 0;
	text[text_len] = '\0';

	ev_begin("key");
	ev_sid(host.keyboard_focus);
	ev_bool("pressed", pressed);
	ev_bool("repeat", repeat);
	ev_str("keysym", keysym);
	ev_int("sym", sym);
	ev_int("code", (long long)keycode - 8);
	ev_strn("text", text, (size_t)text_len);
	ev_key("mods");
	static const struct {
		const char *key, *xkb;
	} mods[] = {
		{ "shift", XKB_MOD_NAME_SHIFT }, { "ctrl", XKB_MOD_NAME_CTRL },
		{ "alt", XKB_MOD_NAME_ALT },	 { "meta", XKB_MOD_NAME_LOGO },
		{ "caps", XKB_MOD_NAME_CAPS },	 { "num", XKB_MOD_NAME_NUM },
	};
	for (size_t i = 0; i < ARRAY_LEN(mods); i++) {
		bool active = xkb_state_mod_name_is_active(state, mods[i].xkb,
							   XKB_STATE_MODS_EFFECTIVE) > 0;
		sb_printf(&host.event, "%s\"%s\":%s", i ? "," : "{", mods[i].key,
			  active ? "true" : "false");
	}
	sb_putc(&host.event, '}');
	ev_end();
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd,
			    uint32_t size)
{
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		log_msg("failed to map keymap: %s", strerror(errno));
		return;
	}
	struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(
		host.xkb_context, map, strnlen(map, size), XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!keymap) {
		log_msg("failed to compile keymap");
		return;
	}
	struct xkb_state *state = xkb_state_new(keymap);
	if (!state) {
		xkb_keymap_unref(keymap);
		return;
	}
	keyboard_stop_repeat();
	xkb_state_unref(host.xkb_state);
	xkb_keymap_unref(host.keymap);
	host.keymap = keymap;
	host.xkb_state = state;
	vk_keymap_changed();
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
			   struct wl_surface *wl, struct wl_array *keys)
{
	keyboard_clear_focus();
	host.keyboard_focus = surface_from_wl(wl);
	if (host.keyboard_focus)
		emit_focus(host.keyboard_focus, true);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
			   struct wl_surface *wl)
{
	keyboard_clear_focus();
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
			 uint32_t key, uint32_t state)
{
	if (!host.keyboard_focus || !host.xkb_state)
		return;
	uint32_t keycode = key + 8;
	bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	emit_key(keycode, pressed, false);
	if (pressed && host.repeat_rate > 0 && xkb_keymap_key_repeats(host.keymap, keycode)) {
		host.repeat_key = keycode;
		host.repeating = true;
		int32_t delay = host.repeat_delay > 0 ? host.repeat_delay : 1;
		long interval = 1000000000L / host.repeat_rate;
		struct itimerspec spec = {
			.it_value = { delay / 1000, (long)(delay % 1000) * 1000000L },
			.it_interval = { interval / 1000000000L, interval % 1000000000L },
		};
		timerfd_settime(host.repeat_fd, 0, &spec, NULL);
	} else if (!pressed && host.repeating && keycode == host.repeat_key) {
		keyboard_stop_repeat();
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
			       uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	if (host.xkb_state)
		xkb_state_update_mask(host.xkb_state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate,
				 int32_t delay)
{
	host.repeat_rate = rate;
	host.repeat_delay = delay;
	if (rate <= 0)
		keyboard_stop_repeat();
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void keyboard_handle_repeat(void)
{
	uint64_t expirations = 0;
	if (read(host.repeat_fd, &expirations, sizeof(expirations)) != sizeof(expirations))
		return;
	if (!host.repeating || !host.keyboard_focus || !host.xkb_state)
		return;
	if (expirations > REPEAT_BURST_MAX)
		expirations = REPEAT_BURST_MAX;
	for (uint64_t i = 0; i < expirations; i++)
		emit_key(host.repeat_key, true, true);
}

static void keyboard_release(void)
{
	if (!host.keyboard)
		return;
	keyboard_clear_focus();
	keyboard_stop_repeat();
	if (wl_keyboard_get_version(host.keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
		wl_keyboard_release(host.keyboard);
	else
		wl_keyboard_destroy(host.keyboard);
	host.keyboard = NULL;
	xkb_state_unref(host.xkb_state);
	xkb_keymap_unref(host.keymap);
	host.xkb_state = NULL;
	host.keymap = NULL;
}

/* Foreign toplevels */

uint32_t toplevel_next_id(void)
{
	return host.next_toplevel_id++;
}

static void toplevel_emit(struct toplevel *toplevel)
{
	ev_begin("toplevel");
	ev_int("tid", toplevel->id);
	ev_str("title", toplevel->title ? toplevel->title : "");
	ev_str("appId", toplevel->app_id ? toplevel->app_id : "");
	ev_bool("activated", toplevel->activated);
	ev_bool("fullscreen", toplevel->fullscreen);
	ev_bool("maximized", toplevel->maximized);
	ev_bool("minimized", toplevel->minimized);
	ev_key("outputs");
	sb_putc(&host.event, '[');
	bool first = true;
	for (size_t i = 0; i < toplevel->output_count; i++) {
		struct output *output = output_from_wl(toplevel->outputs[i]);
		if (!output)
			continue;
		if (!first)
			sb_putc(&host.event, ',');
		first = false;
		json_string(&host.event, output_name(output), strlen(output_name(output)));
	}
	sb_putc(&host.event, ']');
	ev_end();
}

static void toplevel_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
			   const char *title)
{
	struct toplevel *toplevel = data;
	replace_string(&toplevel->title, title);
}

static void toplevel_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
			    const char *app_id)
{
	struct toplevel *toplevel = data;
	replace_string(&toplevel->app_id, app_id);
}

static void toplevel_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
				  struct wl_output *output)
{
	struct toplevel *toplevel = data;
	if (!output)
		return;
	for (size_t i = 0; i < toplevel->output_count; i++) {
		if (toplevel->outputs[i] == output)
			return;
	}
	toplevel->outputs = xrealloc(toplevel->outputs,
				     (toplevel->output_count + 1) * sizeof(*toplevel->outputs));
	toplevel->outputs[toplevel->output_count++] = output;
}

static void toplevel_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
				  struct wl_output *output)
{
	struct toplevel *toplevel = data;
	for (size_t i = 0; i < toplevel->output_count; i++) {
		if (toplevel->outputs[i] != output)
			continue;
		toplevel->outputs[i] = toplevel->outputs[--toplevel->output_count];
		return;
	}
}

static void toplevel_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
			   struct wl_array *states)
{
	struct toplevel *toplevel = data;
	toplevel->activated = toplevel->fullscreen = false;
	toplevel->maximized = toplevel->minimized = false;
	uint32_t *state;
	wl_array_for_each(state, states) {
		switch (*state) {
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED: toplevel->activated = true; break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN: toplevel->fullscreen = true; break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED: toplevel->maximized = true; break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED: toplevel->minimized = true; break;
		}
	}
}

static void toplevel_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	toplevel_emit(data);
}

static void toplevel_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct toplevel *toplevel = data;
	ev_begin("toplevel-closed");
	ev_int("tid", toplevel->id);
	ev_end();
	zwlr_foreign_toplevel_handle_v1_destroy(handle);
	wl_list_remove(&toplevel->link);
	free(toplevel->title);
	free(toplevel->app_id);
	free(toplevel->outputs);
	free(toplevel);
}

static void toplevel_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
			    struct zwlr_foreign_toplevel_handle_v1 *parent)
{
}

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_listener = {
	.title = toplevel_title,
	.app_id = toplevel_app_id,
	.output_enter = toplevel_output_enter,
	.output_leave = toplevel_output_leave,
	.state = toplevel_state,
	.done = toplevel_done,
	.closed = toplevel_closed,
	.parent = toplevel_parent,
};

static void toplevel_manager_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
				      struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct toplevel *toplevel = xcalloc(1, sizeof(*toplevel));
	toplevel->id = toplevel_next_id();
	toplevel->handle = handle;
	wl_list_insert(host.toplevels.prev, &toplevel->link);
	zwlr_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_listener, toplevel);
}

static void toplevel_manager_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager)
{
	zwlr_foreign_toplevel_manager_v1_destroy(manager);
	if (host.toplevel_manager == manager)
		host.toplevel_manager = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_listener = {
	.toplevel = toplevel_manager_toplevel,
	.finished = toplevel_manager_finished,
};

static void toplevel_manager_bind(void)
{
	if (host.toplevel_manager || !host.toplevel_global)
		return;
	host.toplevel_manager = bind_global(host.toplevel_global,
					    &zwlr_foreign_toplevel_manager_v1_interface,
					    host.toplevel_version, 3);
	zwlr_foreign_toplevel_manager_v1_add_listener(host.toplevel_manager,
						      &toplevel_manager_listener, NULL);
}

/* Data control. ext_data_control_v1 and zwlr_data_control_v1 are identical
 * apart from their names; host.dc_ext selects which one every proxy uses. */

static void dc_offer_free(struct dc_offer *offer)
{
	if (!offer)
		return;
	if (host.selection_offer == offer)
		host.selection_offer = NULL;
	if (host.primary_offer == offer)
		host.primary_offer = NULL;
	if (host.dc_ext)
		ext_data_control_offer_v1_destroy(offer->proxy);
	else
		zwlr_data_control_offer_v1_destroy(offer->proxy);
	wl_list_remove(&offer->link);
	free(offer);
}

static void dc_offer_receive(struct dc_offer *offer, const char *mime, int fd)
{
	if (host.dc_ext)
		ext_data_control_offer_v1_receive(offer->proxy, mime, fd);
	else
		zwlr_data_control_offer_v1_receive(offer->proxy, mime, fd);
}

static struct dc_offer *dc_offer_new(void *proxy)
{
	struct dc_offer *offer = xcalloc(1, sizeof(*offer));
	offer->proxy = proxy;
	wl_list_insert(&host.dc_offers, &offer->link);
	return offer;
}

static void dc_offer_mime(struct dc_offer *offer, const char *mime)
{
	for (size_t i = 0; i < ARRAY_LEN(read_text_mimes); i++) {
		if (strcmp(mime, read_text_mimes[i]) == 0)
			offer->mimes |= 1u << i;
	}
	if (strcmp(mime, MIME_HTML) == 0)
		offer->mimes |= MIME_HTML_BIT;
}

static void payload_unref(struct payload *payload)
{
	if (payload && --payload->refs == 0)
		free(payload);
}

static struct payload *payload_new(const char *data, size_t len)
{
	struct payload *payload = xrealloc(NULL, sizeof(*payload) + len);
	payload->refs = 1;
	payload->len = len;
	memcpy(payload->data, data, len);
	return payload;
}

static void clip_write_free(struct clip_write *job)
{
	close(job->fd);
	payload_unref(job->payload);
	wl_list_remove(&job->link);
	free(job);
}

static void clip_write_progress(struct clip_write *job)
{
	while (job->offset < job->payload->len) {
		ssize_t written = write(job->fd, job->payload->data + job->offset,
					job->payload->len - job->offset);
		if (written > 0) {
			job->offset += (size_t)written;
		} else if (written < 0 && errno == EINTR) {
			continue;
		} else if (written < 0 && errno == EAGAIN) {
			return;
		} else {
			break;
		}
	}
	clip_write_free(job);
}

static void dc_source_send(struct dc_source *source, const char *mime, int fd)
{
	struct payload *payload = NULL;
	if (strcmp(mime, MIME_HTML) == 0) {
		payload = source->html;
	} else {
		for (size_t i = 0; i < ARRAY_LEN(source_text_mimes); i++) {
			if (strcmp(mime, source_text_mimes[i]) == 0)
				payload = source->text;
		}
	}
	if (!payload || fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
		close(fd);
		return;
	}
	struct clip_write *job = xcalloc(1, sizeof(*job));
	job->fd = fd;
	job->payload = payload;
	payload->refs++;
	wl_list_insert(&host.clip_writes, &job->link);
	clip_write_progress(job);
}

static void dc_source_free(struct dc_source *source)
{
	if (host.dc_ext)
		ext_data_control_source_v1_destroy(source->proxy);
	else
		zwlr_data_control_source_v1_destroy(source->proxy);
	payload_unref(source->text);
	payload_unref(source->html);
	wl_list_remove(&source->link);
	free(source);
}

static void clip_read_reset(struct clip_read *clip)
{
	if (clip->fd >= 0)
		close(clip->fd);
	clip->fd = -1;
	clip->active = clip->failed = false;
	clip->data.len = 0;
}

static void clip_reads_abort(void)
{
	for (size_t i = 0; i < ARRAY_LEN(host.clip_reads); i++)
		clip_read_reset(&host.clip_reads[i]);
}

static void emit_clipboard(const struct clip_read *text, const struct clip_read *html)
{
	bool has_text = text && text->active && !text->failed;
	bool has_html = html && html->active && !html->failed;
	ev_begin("clipboard");
	ev_strn("text", has_text ? (text->data.data ? text->data.data : "") : NULL,
		has_text ? text->data.len : 0);
	ev_strn("html", has_html ? (html->data.data ? html->data.data : "") : NULL,
		has_html ? html->data.len : 0);
	ev_end();
}

static void clip_reads_check_done(void)
{
	bool any = false;
	for (size_t i = 0; i < ARRAY_LEN(host.clip_reads); i++) {
		if (host.clip_reads[i].active && host.clip_reads[i].fd >= 0)
			return;
		any |= host.clip_reads[i].active;
	}
	if (!any)
		return;
	emit_clipboard(&host.clip_reads[0], &host.clip_reads[1]);
	clip_reads_abort();
}

static void clip_read_progress(struct clip_read *clip)
{
	for (;;) {
		sb_reserve(&clip->data, 65536);
		ssize_t len = read(clip->fd, clip->data.data + clip->data.len,
				   clip->data.cap - clip->data.len);
		if (len > 0) {
			clip->data.len += (size_t)len;
			if (clip->data.len > CLIPBOARD_READ_MAX) {
				log_msg("clipboard content exceeds %u bytes, ignoring it",
					CLIPBOARD_READ_MAX);
				clip->failed = true;
				break;
			}
		} else if (len == 0) {
			break;
		} else if (errno == EINTR) {
			continue;
		} else if (errno == EAGAIN) {
			return;
		} else {
			clip->failed = true;
			break;
		}
	}
	close(clip->fd);
	clip->fd = -1;
	clip_reads_check_done();
}

static bool clip_read_start(struct clip_read *clip, struct dc_offer *offer, const char *mime)
{
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0) {
		log_msg("pipe2: %s", strerror(errno));
		return false;
	}
	/* Only our end is nonblocking; the source writes to a blocking pipe. */
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	dc_offer_receive(offer, mime, fds[1]);
	close(fds[1]);
	clip->fd = fds[0];
	clip->active = true;
	clip->failed = false;
	clip->data.len = 0;
	return true;
}

static void dc_selection(struct dc_offer *offer)
{
	if (host.selection_offer && host.selection_offer != offer)
		dc_offer_free(host.selection_offer);
	host.selection_offer = offer;
	clip_reads_abort();
	bool started = false;
	if (offer) {
		for (size_t i = 0; i < ARRAY_LEN(read_text_mimes); i++) {
			if (!(offer->mimes & 1u << i))
				continue;
			started |= clip_read_start(&host.clip_reads[0], offer, read_text_mimes[i]);
			break;
		}
		if (offer->mimes & MIME_HTML_BIT)
			started |= clip_read_start(&host.clip_reads[1], offer, MIME_HTML);
	}
	if (!started)
		emit_clipboard(NULL, NULL);
}

static void dc_primary_selection(struct dc_offer *offer)
{
	if (host.primary_offer && host.primary_offer != offer)
		dc_offer_free(host.primary_offer);
	host.primary_offer = offer;
}

static void dc_device_destroy(void);
static void dc_device_create(void);

static void dc_finished(void)
{
	dc_device_destroy();
	dc_device_create();
}

#define DC_LISTENERS(P)                                                                         \
	static void P##_offer_offer(void *data, struct P##_offer_v1 *offer, const char *mime)   \
	{                                                                                       \
		dc_offer_mime(data, mime);                                                      \
	}                                                                                       \
	static const struct P##_offer_v1_listener P##_offer_listener = {                        \
		.offer = P##_offer_offer,                                                       \
	};                                                                                      \
	static void P##_device_data_offer(void *data, struct P##_device_v1 *device,             \
					  struct P##_offer_v1 *proxy)                           \
	{                                                                                       \
		P##_offer_v1_add_listener(proxy, &P##_offer_listener, dc_offer_new(proxy));     \
	}                                                                                       \
	static void P##_device_selection(void *data, struct P##_device_v1 *device,              \
					 struct P##_offer_v1 *proxy)                            \
	{                                                                                       \
		dc_selection(proxy ? P##_offer_v1_get_user_data(proxy) : NULL);                 \
	}                                                                                       \
	static void P##_device_finished(void *data, struct P##_device_v1 *device)               \
	{                                                                                       \
		dc_finished();                                                                  \
	}                                                                                       \
	static void P##_device_primary_selection(void *data, struct P##_device_v1 *device,      \
						 struct P##_offer_v1 *proxy)                    \
	{                                                                                       \
		dc_primary_selection(proxy ? P##_offer_v1_get_user_data(proxy) : NULL);         \
	}                                                                                       \
	static const struct P##_device_v1_listener P##_device_listener = {                      \
		.data_offer = P##_device_data_offer,                                            \
		.selection = P##_device_selection,                                              \
		.finished = P##_device_finished,                                                \
		.primary_selection = P##_device_primary_selection,                              \
	};                                                                                      \
	static void P##_source_send(void *data, struct P##_source_v1 *source, const char *mime, \
				    int32_t fd)                                                 \
	{                                                                                       \
		dc_source_send(data, mime, fd);                                                 \
	}                                                                                       \
	static void P##_source_cancelled(void *data, struct P##_source_v1 *source)              \
	{                                                                                       \
		dc_source_free(data);                                                           \
	}                                                                                       \
	static const struct P##_source_v1_listener P##_source_listener = {                      \
		.send = P##_source_send,                                                        \
		.cancelled = P##_source_cancelled,                                              \
	};

DC_LISTENERS(ext_data_control)
DC_LISTENERS(zwlr_data_control)

static void dc_device_create(void)
{
	if (host.dc_device || !host.dc_manager || !host.seat)
		return;
	if (host.dc_ext) {
		struct ext_data_control_device_v1 *device =
			ext_data_control_manager_v1_get_data_device(host.dc_manager, host.seat);
		ext_data_control_device_v1_add_listener(device, &ext_data_control_device_listener, NULL);
		host.dc_device = device;
	} else {
		struct zwlr_data_control_device_v1 *device =
			zwlr_data_control_manager_v1_get_data_device(host.dc_manager, host.seat);
		zwlr_data_control_device_v1_add_listener(device, &zwlr_data_control_device_listener,
							 NULL);
		host.dc_device = device;
	}
}

static void dc_device_destroy(void)
{
	struct dc_offer *offer, *tmp;
	wl_list_for_each_safe(offer, tmp, &host.dc_offers, link)
		dc_offer_free(offer);
	if (!host.dc_device)
		return;
	if (host.dc_ext)
		ext_data_control_device_v1_destroy(host.dc_device);
	else
		zwlr_data_control_device_v1_destroy(host.dc_device);
	host.dc_device = NULL;
}

static void dc_manager_bind(void)
{
	if (!host.ready)
		return;
	if (!host.dc_manager) {
		if (host.dc_ext_global) {
			host.dc_ext = true;
			host.dc_bound_global = host.dc_ext_global;
			host.dc_manager = bind_global(host.dc_ext_global,
						      &ext_data_control_manager_v1_interface,
						      host.dc_ext_version, 1);
		} else if (host.dc_wlr_global) {
			host.dc_ext = false;
			host.dc_bound_global = host.dc_wlr_global;
			host.dc_manager = bind_global(host.dc_wlr_global,
						      &zwlr_data_control_manager_v1_interface,
						      host.dc_wlr_version, 2);
		}
	}
	dc_device_create();
}

static void dc_manager_destroy(void)
{
	dc_device_destroy();
	struct dc_source *source, *tmp;
	wl_list_for_each_safe(source, tmp, &host.dc_sources, link)
		dc_source_free(source);
	if (host.dc_manager) {
		if (host.dc_ext)
			ext_data_control_manager_v1_destroy(host.dc_manager);
		else
			zwlr_data_control_manager_v1_destroy(host.dc_manager);
	}
	host.dc_manager = NULL;
	host.dc_bound_global = 0;
}

static void dc_set_selection(void *source_proxy)
{
	if (host.dc_ext)
		ext_data_control_device_v1_set_selection(host.dc_device, source_proxy);
	else
		zwlr_data_control_device_v1_set_selection(host.dc_device, source_proxy);
}

static void dc_source_offer(struct dc_source *source, const char *mime)
{
	if (host.dc_ext)
		ext_data_control_source_v1_offer(source->proxy, mime);
	else
		zwlr_data_control_source_v1_offer(source->proxy, mime);
}

static void clipboard_set(const char *text, size_t text_len, const char *html, size_t html_len)
{
	struct dc_source *source = xcalloc(1, sizeof(*source));
	source->text = payload_new(text, text_len);
	if (html)
		source->html = payload_new(html, html_len);
	if (host.dc_ext) {
		struct ext_data_control_source_v1 *proxy =
			ext_data_control_manager_v1_create_data_source(host.dc_manager);
		ext_data_control_source_v1_add_listener(proxy, &ext_data_control_source_listener,
							source);
		source->proxy = proxy;
	} else {
		struct zwlr_data_control_source_v1 *proxy =
			zwlr_data_control_manager_v1_create_data_source(host.dc_manager);
		zwlr_data_control_source_v1_add_listener(proxy, &zwlr_data_control_source_listener,
							 source);
		source->proxy = proxy;
	}
	wl_list_insert(&host.dc_sources, &source->link);
	for (size_t i = 0; i < ARRAY_LEN(source_text_mimes); i++)
		dc_source_offer(source, source_text_mimes[i]);
	if (html)
		dc_source_offer(source, MIME_HTML);
	dc_set_selection(source->proxy);
}

/* Seat */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	bool has_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
	bool has_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (has_pointer && !host.pointer) {
		host.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(host.pointer, &pointer_listener, NULL);
		cursor_shape_device_create();
	} else if (!has_pointer) {
		pointer_release();
	}
	if (has_keyboard && !host.keyboard) {
		host.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(host.keyboard, &keyboard_listener, NULL);
		/* Seats older than version 4 have no repeat_info. */
		host.repeat_rate = 25;
		host.repeat_delay = 600;
	} else if (!has_keyboard) {
		keyboard_release();
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void seat_bind(const struct seat_global *global)
{
	host.seat = bind_global(global->name, &wl_seat_interface, global->version, 8);
	host.seat_global = global->name;
	wl_seat_add_listener(host.seat, &seat_listener, NULL);
	dc_manager_bind();
}

static void seat_release(void)
{
	if (!host.seat)
		return;
	vk_destroy();
	vp_destroy_all();
	pointer_release();
	keyboard_release();
	dc_device_destroy();
	if (wl_seat_get_version(host.seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
		wl_seat_release(host.seat);
	else
		wl_seat_destroy(host.seat);
	host.seat = NULL;
	host.seat_global = 0;
}

/* Registry */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		host.compositor = bind_global(name, &wl_compositor_interface, version, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		host.shm = bind_global(name, &wl_shm_interface, version, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		if (host.seat_global_count == SEAT_GLOBALS_MAX)
			return;
		struct seat_global *global = &host.seat_globals[host.seat_global_count++];
		*global = (struct seat_global){ name, version };
		if (!host.seat)
			seat_bind(global);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		output_add(name, version);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		if (host.xdg_output_manager)
			return;
		host.xdg_output_manager =
			bind_global(name, &zxdg_output_manager_v1_interface, version, 3);
		host.xdg_output_global = name;
		struct output *output;
		wl_list_for_each(output, &host.outputs, link)
			output_create_xdg(output);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		host.layer_shell = bind_global(name, &zwlr_layer_shell_v1_interface, version, 4);
		host.layer_shell_global = name;
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		host.viewporter = bind_global(name, &wp_viewporter_interface, version, 1);
		host.viewporter_global = name;
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		host.cursor_shape_manager =
			bind_global(name, &wp_cursor_shape_manager_v1_interface, version, 1);
		host.cursor_shape_global = name;
		cursor_shape_device_create();
	} else if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
		host.toplevel_global = name;
		host.toplevel_version = version;
		if (host.ready)
			toplevel_manager_bind();
	} else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
		host.dc_ext_global = name;
		host.dc_ext_version = version;
		dc_manager_bind();
	} else if (strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0) {
		host.dc_wlr_global = name;
		host.dc_wlr_version = version;
		dc_manager_bind();
	} else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		if (host.vk_manager)
			return;
		host.vk_manager =
			bind_global(name, &zwp_virtual_keyboard_manager_v1_interface, version, 1);
		host.vk_manager_global = name;
	} else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		if (host.vp_manager)
			return;
		host.vp_manager =
			bind_global(name, &zwlr_virtual_pointer_manager_v1_interface, version, 2);
		host.vp_manager_global = name;
	} else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		if (host.screencopy)
			return;
		host.screencopy = bind_global(name, &zwlr_screencopy_manager_v1_interface, version, 3);
		host.screencopy_global = name;
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	struct output *output;
	wl_list_for_each(output, &host.outputs, link) {
		if (output->global == name) {
			output_destroy(output);
			return;
		}
	}
	for (size_t i = 0; i < host.seat_global_count; i++) {
		if (host.seat_globals[i].name != name)
			continue;
		memmove(&host.seat_globals[i], &host.seat_globals[i + 1],
			(host.seat_global_count - i - 1) * sizeof(host.seat_globals[0]));
		host.seat_global_count--;
		if (host.seat_global == name) {
			seat_release();
			if (host.seat_global_count > 0)
				seat_bind(&host.seat_globals[0]);
		}
		return;
	}
	if (name == host.layer_shell_global && host.layer_shell) {
		if (zwlr_layer_shell_v1_get_version(host.layer_shell) >=
		    ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION)
			zwlr_layer_shell_v1_destroy(host.layer_shell);
		else
			wl_proxy_destroy((struct wl_proxy *)host.layer_shell);
		host.layer_shell = NULL;
	} else if (name == host.viewporter_global && host.viewporter) {
		wp_viewporter_destroy(host.viewporter);
		host.viewporter = NULL;
	} else if (name == host.cursor_shape_global && host.cursor_shape_manager) {
		wp_cursor_shape_manager_v1_destroy(host.cursor_shape_manager);
		host.cursor_shape_manager = NULL;
	} else if (name == host.xdg_output_global && host.xdg_output_manager) {
		zxdg_output_manager_v1_destroy(host.xdg_output_manager);
		host.xdg_output_manager = NULL;
	} else if (name == host.toplevel_global) {
		host.toplevel_global = 0;
		if (host.toplevel_manager)
			zwlr_foreign_toplevel_manager_v1_stop(host.toplevel_manager);
	} else if (name == host.dc_ext_global || name == host.dc_wlr_global) {
		if (name == host.dc_ext_global)
			host.dc_ext_global = 0;
		else
			host.dc_wlr_global = 0;
		if (name == host.dc_bound_global) {
			dc_manager_destroy();
			dc_manager_bind();
		}
	} else if (name == host.vk_manager_global && host.vk_manager) {
		vk_destroy();
		zwp_virtual_keyboard_manager_v1_destroy(host.vk_manager);
		host.vk_manager = NULL;
	} else if (name == host.vp_manager_global && host.vp_manager) {
		vp_destroy_all();
		zwlr_virtual_pointer_manager_v1_destroy(host.vp_manager);
		host.vp_manager = NULL;
	} else if (name == host.screencopy_global && host.screencopy) {
		/* Frames in flight are independent objects and finish or fail. */
		zwlr_screencopy_manager_v1_destroy(host.screencopy);
		host.screencopy = NULL;
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* Shared helpers for input injection, capture and probing */

uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

/* Arms a one-shot timer; 0 disarms it. */
static void timer_arm(int fd, long ms)
{
	struct itimerspec spec = { .it_value = { ms / 1000, ms % 1000 * 1000000L } };
	timerfd_settime(fd, 0, &spec, NULL);
}

bool write_all(int fd, const void *data, size_t len)
{
	const char *ptr = data;
	while (len > 0) {
		ssize_t written = write(fd, ptr, len);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return false;
		ptr += written;
		len -= (size_t)written;
	}
	return true;
}

/* Returns a zero-filled memfd of `size` bytes, or -1. */
static int memfd_new(const char *name, size_t size)
{
	int fd = memfd_create(name, MFD_CLOEXEC);
	if (fd < 0) {
		log_msg("memfd_create: %s", strerror(errno));
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		log_msg("ftruncate: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

/* A fully transparent ARGB8888 buffer. */
static struct wl_buffer *transparent_buffer_new(int32_t width, int32_t height)
{
	if (!host.shm || width < 1 || height < 1 || (int64_t)width * height * 4 > INT32_MAX)
		return NULL;
	int32_t size = width * height * 4;
	int fd = memfd_new("overlay-transparent", (size_t)size);
	if (fd < 0)
		return NULL;
	struct wl_shm_pool *pool = wl_shm_create_pool(host.shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
							     WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

static bool bit_test(const unsigned long *bits, unsigned bit)
{
	return bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG) & 1;
}

static void bit_assign(unsigned long *bits, unsigned bit, bool value)
{
	unsigned long mask = 1UL << (bit % BITS_PER_LONG);
	if (value)
		bits[bit / BITS_PER_LONG] |= mask;
	else
		bits[bit / BITS_PER_LONG] &= ~mask;
}

/* Evdev. Devices are never grabbed: the compositor keeps receiving every
 * event, the host only observes them. */

static void emit_input_key(unsigned code, int32_t value)
{
	ev_begin("input");
	ev_str("kind", "key");
	ev_int("code", code);
	ev_int("value", value);
	ev_end();
}

static void emit_input_wheel(int32_t value, bool horizontal)
{
	ev_begin("input");
	ev_str("kind", "wheel");
	ev_int("value", value);
	ev_bool("horizontal", horizontal);
	ev_end();
}

static void evdev_report_count(bool always)
{
	int count = wl_list_length(&host.evdev_devices);
	if (!always && count == host.evdev_reported)
		return;
	host.evdev_reported = count;
	ev_begin("evdev");
	ev_int("devices", count);
	ev_end();
}

struct evdev_caps {
	unsigned long types[BITS_LONGS(EV_CNT)];
	unsigned long keys[BITS_LONGS(KEY_CNT)];
	unsigned long rels[BITS_LONGS(REL_CNT)];
};

/* Keyboards (letter keys), mice (left button) and wheels. */
static bool evdev_caps_wanted(const struct evdev_caps *caps)
{
	return (bit_test(caps->types, EV_KEY) &&
		(bit_test(caps->keys, KEY_A) || bit_test(caps->keys, BTN_LEFT))) ||
	       (bit_test(caps->types, EV_REL) && bit_test(caps->rels, REL_WHEEL));
}

static bool evdev_is_wanted(int fd)
{
	char phys[sizeof(host.uinput_phys)] = { 0 };
	if (ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), phys) > 0 && strcmp(phys, host.uinput_phys) == 0)
		return false;
	struct evdev_caps caps = { 0 };
	if (ioctl(fd, EVIOCGBIT(0, sizeof(caps.types)), caps.types) < 0)
		return false;
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(caps.keys)), caps.keys);
	ioctl(fd, EVIOCGBIT(EV_REL, sizeof(caps.rels)), caps.rels);
	return evdev_caps_wanted(&caps);
}

/* Reads a sysfs capability bitmap: hex longs separated by spaces, most
 * significant first. */
static bool sysfs_read_bits(const char *name, const char *cap, unsigned long *bits, size_t longs)
{
	char path[PATH_MAX], text[1024];
	snprintf(path, sizeof(path), "/sys/class/input/%s/device/capabilities/%s", name, cap);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	ssize_t len = read(fd, text, sizeof(text) - 1);
	close(fd);
	if (len <= 0)
		return false;
	text[len] = '\0';
	char *words[64], *save = NULL;
	size_t count = 0;
	for (char *word = strtok_r(text, " \n", &save); word && count < ARRAY_LEN(words);
	     word = strtok_r(NULL, " \n", &save))
		words[count++] = word;
	for (size_t i = 0; i < count && i < longs; i++)
		bits[i] = strtoul(words[count - 1 - i], NULL, 16);
	return true;
}

/* Closing an evdev fd waits for an RCU grace period, about 10-30 ms, so
 * devices whose sysfs capabilities rule them out are not opened at all. When
 * sysfs is unavailable the device is opened and checked with ioctls. */
static bool evdev_maybe_wanted(const char *name)
{
	char path[PATH_MAX], phys[sizeof(host.uinput_phys)];
	snprintf(path, sizeof(path), "/sys/class/input/%s/device/phys", name);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t len = read(fd, phys, sizeof(phys) - 1);
		close(fd);
		if (len > 0 && phys[len - 1] == '\n')
			len--;
		if (len > 0) {
			phys[len] = '\0';
			if (strcmp(phys, host.uinput_phys) == 0)
				return false;
		}
	}
	struct evdev_caps caps = { 0 };
	if (!sysfs_read_bits(name, "ev", caps.types, ARRAY_LEN(caps.types)) ||
	    !sysfs_read_bits(name, "key", caps.keys, ARRAY_LEN(caps.keys)) ||
	    !sysfs_read_bits(name, "rel", caps.rels, ARRAY_LEN(caps.rels)))
		return true;
	return evdev_caps_wanted(&caps);
}

static struct evdev_device *evdev_find(const char *name)
{
	struct evdev_device *device;
	wl_list_for_each(device, &host.evdev_devices, link) {
		if (strcmp(device->name, name) == 0)
			return device;
	}
	return NULL;
}

/* Records a device's key state, reporting the first press and the last
 * release across devices. */
static void evdev_set_key(struct evdev_device *device, unsigned code, bool down)
{
	if (down == bit_test(device->keys, code))
		return;
	bit_assign(device->keys, code, down);
	if (down && host.evdev_key_holders[code]++ == 0)
		emit_input_key(code, 1);
	else if (!down && --host.evdev_key_holders[code] == 0)
		emit_input_key(code, 0);
}

static void evdev_close(struct evdev_device *device)
{
	close(device->fd);
	wl_list_remove(&device->link);
	free(device->name);
	free(device);
}

/* Closes a device that went away, releasing its held keys so that none stays
 * pressed for the client. */
static void evdev_drop(struct evdev_device *device)
{
	for (unsigned code = 0; code < KEY_CNT; code++)
		evdev_set_key(device, code, false);
	evdev_close(device);
}

enum evdev_open_result { EVDEV_OPENED, EVDEV_SKIPPED, EVDEV_DENIED };

static enum evdev_open_result evdev_open(const char *name)
{
	if (strncmp(name, "event", 5) != 0 || strchr(name, '/'))
		return EVDEV_SKIPPED;
	char path[PATH_MAX];
	snprintf(path, sizeof(path), EVDEV_DIR "/%s", name);
	struct stat st;
	struct evdev_device *existing = evdev_find(name);
	if (existing && stat(path, &st) == 0 && st.st_rdev == existing->rdev)
		return EVDEV_OPENED;
	if (!evdev_maybe_wanted(name))
		return EVDEV_SKIPPED;
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return errno == EACCES || errno == EPERM ? EVDEV_DENIED : EVDEV_SKIPPED;
	if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode) || !evdev_is_wanted(fd)) {
		close(fd);
		return EVDEV_SKIPPED;
	}
	/* The node was replaced by another device with the same name. */
	if (existing)
		evdev_drop(existing);
	struct evdev_device *device = xcalloc(1, sizeof(*device));
	device->fd = fd;
	device->rdev = st.st_rdev;
	device->name = xstrdup(name);
	/* Keys already down produce a release without a press, never a stuck key. */
	ioctl(fd, EVIOCGKEY(sizeof(device->keys)), device->keys);
	for (unsigned code = 0; code < KEY_CNT; code++)
		host.evdev_key_holders[code] += bit_test(device->keys, code);
	wl_list_insert(host.evdev_devices.prev, &device->link);
	return EVDEV_OPENED;
}

/* Opens every wanted device not open yet. Returns whether any was denied. */
static bool evdev_scan(void)
{
	DIR *dir = opendir(EVDEV_DIR);
	if (!dir) {
		log_msg("evdev: cannot open " EVDEV_DIR ": %s", strerror(errno));
		return false;
	}
	bool denied = false;
	struct dirent *entry;
	while ((entry = readdir(dir)))
		denied |= evdev_open(entry->d_name) == EVDEV_DENIED;
	closedir(dir);
	return denied;
}

/* After SYN_DROPPED the kernel discarded events; the current key state is
 * fetched and the differences reported as presses and releases. */
static void evdev_resync(struct evdev_device *device)
{
	unsigned long keys[BITS_LONGS(KEY_CNT)] = { 0 };
	if (ioctl(device->fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
		return;
	for (unsigned code = 0; code < KEY_CNT; code++)
		evdev_set_key(device, code, bit_test(keys, code));
}

static void evdev_handle(struct evdev_device *device, const struct input_event *event)
{
	if (event->type == EV_SYN) {
		if (event->code == SYN_DROPPED) {
			device->dropping = true;
		} else if (event->code == SYN_REPORT && device->dropping) {
			device->dropping = false;
			evdev_resync(device);
		}
		return;
	}
	if (device->dropping)
		return;
	if (event->type == EV_KEY && event->code < KEY_CNT && event->value >= 0 &&
	    event->value <= 2) {
		if (event->value == 2) {
			if (bit_test(device->keys, event->code))
				emit_input_key(event->code, 2);
		} else {
			evdev_set_key(device, event->code, event->value == 1);
		}
	} else if (event->type == EV_REL && (event->code == REL_WHEEL || event->code == REL_HWHEEL) &&
		   event->value != 0) {
		emit_input_wheel(event->value, event->code == REL_HWHEEL);
	}
}

static void evdev_read(struct evdev_device *device)
{
	struct input_event events[64];
	for (;;) {
		ssize_t len = read(device->fd, events, sizeof(events));
		if (len < 0 && errno == EINTR)
			continue;
		if (len < 0 && errno == EAGAIN)
			return;
		if (len <= 0) {
			if (len < 0 && errno != ENODEV)
				log_msg("evdev %s: %s", device->name, strerror(errno));
			evdev_drop(device);
			evdev_report_count(false);
			return;
		}
		size_t count = (size_t)len / sizeof(events[0]);
		for (size_t i = 0; i < count; i++)
			evdev_handle(device, &events[i]);
	}
}

static void evdev_inotify_read(void)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool rescan = false;
	for (;;) {
		ssize_t len = read(host.evdev_inotify_fd, buf, sizeof(buf));
		if (len < 0 && errno == EINTR)
			continue;
		if (len <= 0)
			break;
		for (char *ptr = buf; ptr < buf + len;) {
			const struct inotify_event *event = (const struct inotify_event *)ptr;
			ptr += sizeof(*event) + event->len;
			if (event->mask & IN_Q_OVERFLOW)
				rescan = true;
			if (event->len == 0)
				continue;
			if (event->mask & IN_DELETE) {
				struct evdev_device *device = evdev_find(event->name);
				if (device)
					evdev_drop(device);
			} else {
				/* udev sets permissions after the node is created, so
				 * IN_ATTRIB retries a device denied on IN_CREATE. */
				evdev_open(event->name);
			}
		}
	}
	if (rescan)
		evdev_scan();
	evdev_report_count(false);
}

static void evdev_stop(void)
{
	struct evdev_device *device, *tmp;
	wl_list_for_each_safe(device, tmp, &host.evdev_devices, link)
		evdev_close(device);
	memset(host.evdev_key_holders, 0, sizeof(host.evdev_key_holders));
	if (host.evdev_inotify_fd >= 0)
		close(host.evdev_inotify_fd);
	host.evdev_inotify_fd = -1;
	host.evdev_on = false;
}

static void cmd_evdev(char **argv, size_t argc)
{
	if (strcmp(argv[1], "off") == 0) {
		bool was_on = host.evdev_on;
		evdev_stop();
		if (was_on)
			evdev_report_count(false);
		return;
	}
	if (strcmp(argv[1], "on") != 0) {
		log_msg("evdev: expected on or off, got %.64s", argv[1]);
		return;
	}
	host.evdev_on = true;
	/* The compositor adds a uinput device asynchronously and drops keys sent
	 * before that, so it is created ahead of the first key. */
	if (!host.vk_manager)
		uinput_ensure();
	if (host.evdev_inotify_fd < 0) {
		host.evdev_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (host.evdev_inotify_fd < 0) {
			log_msg("evdev: inotify_init1: %s; devices plugged in later are missed",
				strerror(errno));
		} else if (inotify_add_watch(host.evdev_inotify_fd, EVDEV_DIR,
					     IN_CREATE | IN_ATTRIB | IN_DELETE) < 0) {
			log_msg("evdev: cannot watch " EVDEV_DIR ": %s", strerror(errno));
			close(host.evdev_inotify_fd);
			host.evdev_inotify_fd = -1;
		}
	}
	bool denied = evdev_scan();
	evdev_report_count(true);
	if (wl_list_empty(&host.evdev_devices))
		emit_error("evdev: cannot read any keyboard or mouse in " EVDEV_DIR "%s; the user "
			   "must be in the input group",
			   denied ? " (permission denied)" : "");
}

/* Virtual keyboard */

/* Uploads the keymap to the virtual keyboard, unless it is already in use,
 * and resets the virtual keyboard's modifier state. */
static bool vk_upload(struct xkb_keymap *keymap)
{
	char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!text) {
		log_msg("vkey: cannot serialize the keymap");
		return false;
	}
	if (host.vk_keymap_text && strcmp(text, host.vk_keymap_text) == 0) {
		free(text);
		return true;
	}
	size_t size = strlen(text) + 1;
	struct xkb_state *state = xkb_state_new(keymap);
	int fd = memfd_new("overlay-keymap", 0);
	if (!state || fd < 0 || !write_all(fd, text, size)) {
		log_msg("vkey: cannot upload the keymap");
		if (fd >= 0)
			close(fd);
		xkb_state_unref(state);
		free(text);
		return false;
	}
	/* libwayland duplicates the fd while sending the request. */
	zwp_virtual_keyboard_v1_keymap(host.vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
				       (uint32_t)size);
	close(fd);
	zwp_virtual_keyboard_v1_modifiers(host.vk, 0, 0, 0, 0);
	xkb_state_unref(host.vk_state);
	host.vk_state = state;
	free(host.vk_keymap_text);
	host.vk_keymap_text = text;
	return true;
}

static void vk_destroy(void)
{
	if (host.vk)
		zwp_virtual_keyboard_v1_destroy(host.vk);
	host.vk = NULL;
	xkb_state_unref(host.vk_state);
	host.vk_state = NULL;
	free(host.vk_keymap_text);
	host.vk_keymap_text = NULL;
	host.vk_default_keymap = false;
}

/* Creates the virtual keyboard with the seat keymap, or the default keymap
 * when the seat has not sent one. The keymap is set before any key, which
 * the protocol requires. */
static bool vk_ensure(void)
{
	if (host.vk)
		return true;
	if (!host.vk_manager) {
		log_msg("vkey: zwp_virtual_keyboard_manager_v1 is unavailable");
		return false;
	}
	if (!host.seat) {
		log_msg("vkey: no seat");
		return false;
	}
	struct xkb_keymap *keymap =
		host.keymap ? xkb_keymap_ref(host.keymap)
			    : xkb_keymap_new_from_names(host.xkb_context, NULL,
							XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		log_msg("vkey: cannot compile a default keymap");
		return false;
	}
	host.vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(host.vk_manager, host.seat);
	host.vk_default_keymap = !host.keymap;
	bool uploaded = vk_upload(keymap);
	xkb_keymap_unref(keymap);
	if (!uploaded)
		vk_destroy();
	return uploaded;
}

/* Only the first seat keymap replaces a default one. Later keymap events are
 * not followed: niri broadcasts a virtual keyboard's keymap to the seat
 * whenever it types, so following them uploads our own keymap back and forth
 * between two serializations without end. */
static void vk_keymap_changed(void)
{
	if (!host.vk || !host.vk_default_keymap || !host.keymap)
		return;
	host.vk_default_keymap = false;
	vk_upload(host.keymap);
}

static void vk_key(uint32_t code, bool pressed)
{
	if (!vk_ensure())
		return;
	zwp_virtual_keyboard_v1_key(host.vk, now_ms(), code,
				    pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
					    : WL_KEYBOARD_KEY_STATE_RELEASED);
	enum xkb_state_component changed =
		xkb_state_update_key(host.vk_state, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
	if (changed)
		zwp_virtual_keyboard_v1_modifiers(
			host.vk, xkb_state_serialize_mods(host.vk_state, XKB_STATE_MODS_DEPRESSED),
			xkb_state_serialize_mods(host.vk_state, XKB_STATE_MODS_LATCHED),
			xkb_state_serialize_mods(host.vk_state, XKB_STATE_MODS_LOCKED),
			xkb_state_serialize_layout(host.vk_state, XKB_STATE_LAYOUT_EFFECTIVE));
	wl_display_flush(host.display);
}

/* uinput keyboard */

/* Creates a kernel keyboard device the compositor reads like a physical one,
 * where it offers no virtual keyboard (KWin). Needs write access to
 * /dev/uinput. Unlike a virtual keyboard, compositors that keep one modifier
 * state for all keyboards (KWin, Sway keyboard groups) combine its keys with
 * physically held modifiers, so a held modifier cannot be released for the
 * target. Not attempted again after a failure. */
static bool uinput_ensure(void)
{
	if (host.uinput_fd >= 0)
		return true;
	if (host.uinput_failed)
		return false;
	host.uinput_failed = true;
	int fd = open(UINPUT_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		log_msg("uinput: cannot open " UINPUT_PATH ": %s", strerror(errno));
		return false;
	}
	/* EV_REP is left out: compositors repeat keys themselves. */
	bool ok = ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0 && ioctl(fd, UI_SET_EVBIT, EV_SYN) == 0;
	for (int code = KEY_ESC; ok && code < BTN_MISC; code++)
		ok = ioctl(fd, UI_SET_KEYBIT, code) == 0;
	struct uinput_setup setup = { .id = { .bustype = BUS_VIRTUAL, .version = 1 } };
	snprintf(setup.name, sizeof(setup.name), "overlay-wayland-host keyboard");
	ok = ok && ioctl(fd, UI_SET_PHYS, host.uinput_phys) == 0 &&
	     ioctl(fd, UI_DEV_SETUP, &setup) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
	if (!ok) {
		log_msg("uinput: cannot create a keyboard: %s", strerror(errno));
		close(fd);
		return false;
	}
	host.uinput_fd = fd;
	host.uinput_failed = false;
	return true;
}

static void uinput_key(uint32_t code, bool pressed)
{
	if (code == 0 || code >= BTN_MISC) {
		log_msg("vkey: key %u is not on the uinput keyboard", code);
		return;
	}
	if (!uinput_ensure())
		return;
	struct input_event events[2] = {
		{ .type = EV_KEY, .code = (uint16_t)code, .value = pressed },
		{ .type = EV_SYN, .code = SYN_REPORT },
	};
	if (!write_all(host.uinput_fd, events, sizeof(events)))
		log_msg("uinput: %s", strerror(errno));
}

static void cmd_vkey(char **argv, size_t argc)
{
	uint32_t code, state;
	if (!parse_u32(argv[1], &code) || code >= KEY_CNT || !parse_u32(argv[2], &state) ||
	    state > 1) {
		log_msg("vkey: invalid arguments");
		return;
	}
	if (host.vk_manager)
		vk_key(code, state == 1);
	else if (uinput_ensure())
		uinput_key(code, state == 1);
	else if (!rd_key(code, state == 1))
		log_msg("vkey: no virtual keyboard, uinput keyboard or remote desktop keyboard");
}

/* Virtual pointer */

static void vp_destroy_all(void)
{
	struct output *output;
	wl_list_for_each(output, &host.outputs, link) {
		if (output->vpointer)
			zwlr_virtual_pointer_v1_destroy(output->vpointer);
		output->vpointer = NULL;
	}
	if (host.vpointer)
		zwlr_virtual_pointer_v1_destroy(host.vpointer);
	host.vpointer = NULL;
}

/* The output containing the point, else the nearest one. */
static struct output *output_at(double x, double y)
{
	struct output *output, *best = NULL;
	double best_distance = 0;
	wl_list_for_each(output, &host.outputs, link) {
		struct rect r = output_logical(output);
		if (!output->done || r.width <= 0 || r.height <= 0)
			continue;
		double right = (double)r.x + r.width, bottom = (double)r.y + r.height;
		double dx = x < r.x ? r.x - x : x >= right ? x - right + 1 : 0;
		double dy = y < r.y ? r.y - y : y >= bottom ? y - bottom + 1 : 0;
		double distance = dx * dx + dy * dy;
		if (!best || distance < best_distance) {
			best = output;
			best_distance = distance;
		}
	}
	return best;
}

static void cmd_warp(char **argv, size_t argc)
{
	double x, y;
	if (!parse_double(argv[1], &x) || !parse_double(argv[2], &y)) {
		log_msg("warp: invalid position");
		return;
	}
	if (!host.vp_manager) {
		if (!rd_warp(x, y))
			log_msg("warp: no virtual pointer and no remote desktop pointer at %g,%g", x, y);
		return;
	}
	if (!host.seat) {
		log_msg("warp: no seat");
		return;
	}
	struct zwlr_virtual_pointer_v1 *pointer;
	struct rect extent = { 0 };
	if (zwlr_virtual_pointer_manager_v1_get_version(host.vp_manager) >=
	    ZWLR_VIRTUAL_POINTER_MANAGER_V1_CREATE_VIRTUAL_POINTER_WITH_OUTPUT_SINCE_VERSION) {
		struct output *output = output_at(x, y);
		if (!output) {
			log_msg("warp: no output");
			return;
		}
		if (!output->vpointer)
			output->vpointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
				host.vp_manager, host.seat, output->wl);
		pointer = output->vpointer;
		extent = output_logical(output);
	} else {
		/* Without an output the extent maps onto the whole layout. */
		bool first = true;
		int64_t right = 0, bottom = 0;
		struct output *output;
		wl_list_for_each(output, &host.outputs, link) {
			struct rect r = output_logical(output);
			if (!output->done || r.width <= 0 || r.height <= 0)
				continue;
			if (first || r.x < extent.x)
				extent.x = r.x;
			if (first || r.y < extent.y)
				extent.y = r.y;
			if (first || (int64_t)r.x + r.width > right)
				right = (int64_t)r.x + r.width;
			if (first || (int64_t)r.y + r.height > bottom)
				bottom = (int64_t)r.y + r.height;
			first = false;
		}
		if (first) {
			log_msg("warp: no output");
			return;
		}
		extent.width = (int32_t)(right - extent.x);
		extent.height = (int32_t)(bottom - extent.y);
		if (!host.vpointer)
			host.vpointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
				host.vp_manager, host.seat);
		pointer = host.vpointer;
	}
	int64_t px = floor_i32(x + 0.5) - (int64_t)extent.x;
	int64_t py = floor_i32(y + 0.5) - (int64_t)extent.y;
	px = px < 0 ? 0 : px >= extent.width ? extent.width - 1 : px;
	py = py < 0 ? 0 : py >= extent.height ? extent.height - 1 : py;
	zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(), (uint32_t)px, (uint32_t)py,
						(uint32_t)extent.width, (uint32_t)extent.height);
	zwlr_virtual_pointer_v1_frame(pointer);
	wl_display_flush(host.display);
}

/* Screencopy */

static void capture_free(struct capture *capture)
{
	if (capture->frame)
		zwlr_screencopy_frame_v1_destroy(capture->frame);
	if (capture->buffer)
		wl_buffer_destroy(capture->buffer);
	if (capture->data)
		munmap(capture->data, capture->size);
	wl_list_remove(&capture->link);
	free(capture->id);
	free(capture->path);
	free(capture);
}

static void emit_capture_error(const char *id, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void emit_capture_error(const char *id, const char *fmt, ...)
{
	char message[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);
	log_msg("capture %s: %s", id, message);
	ev_begin("capture");
	ev_str("id", id);
	ev_str("error", message);
	ev_end();
}

static bool capture_format_supported(uint32_t format)
{
	return format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888 ||
	       format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888;
}

static void capture_copy(struct capture *capture)
{
	if (!capture->has_shm) {
		emit_capture_error(capture->id, "the compositor offers no wl_shm buffer");
		capture_free(capture);
		return;
	}
	if (!capture_format_supported(capture->format)) {
		emit_capture_error(capture->id, "unsupported pixel format 0x%08x", capture->format);
		capture_free(capture);
		return;
	}
	if (!host.shm || capture->width == 0 || capture->height == 0 ||
	    capture->width > INT32_MAX / 4 || capture->stride < capture->width * 4 ||
	    (uint64_t)capture->stride * capture->height > INT32_MAX) {
		emit_capture_error(capture->id, "invalid buffer %ux%u with stride %u",
				   capture->width, capture->height, capture->stride);
		capture_free(capture);
		return;
	}
	capture->size = (size_t)capture->stride * capture->height;
	int fd = memfd_new("overlay-capture", capture->size);
	void *data = fd < 0 ? MAP_FAILED
			    : mmap(NULL, capture->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		if (fd >= 0)
			close(fd);
		emit_capture_error(capture->id, "cannot allocate the buffer");
		capture_free(capture);
		return;
	}
	capture->data = data;
	struct wl_shm_pool *pool = wl_shm_create_pool(host.shm, fd, (int32_t)capture->size);
	capture->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)capture->width,
						    (int32_t)capture->height,
						    (int32_t)capture->stride, capture->format);
	wl_shm_pool_destroy(pool);
	close(fd);
	zwlr_screencopy_frame_v1_copy(capture->frame, capture->buffer);
}

static void capture_frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
				 uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
	struct capture *capture = data;
	capture->has_shm = true;
	capture->format = format;
	capture->width = width;
	capture->height = height;
	capture->stride = stride;
	if (zwlr_screencopy_frame_v1_get_version(frame) <
	    ZWLR_SCREENCOPY_FRAME_V1_BUFFER_DONE_SINCE_VERSION)
		capture_copy(capture);
}

static void capture_frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags)
{
	struct capture *capture = data;
	capture->flags = flags;
}

/* Size of the captured image as shown, which differs from the buffer when
 * the output is rotated. */
static void capture_image_size(const struct capture *capture, uint32_t *width, uint32_t *height)
{
	bool rotated = capture->transform & 1;
	*width = rotated ? capture->height : capture->width;
	*height = rotated ? capture->width : capture->height;
}

/* Writes BGRA rows of the image as shown, top row first, with opaque alpha.
 * Screencopy buffers are in the output's buffer orientation, so the output
 * transform is applied here. */
static bool capture_write(const struct capture *capture)
{
	/* A new file only: an existing path may be a symlink or a FIFO planted in
	 * a shared directory. */
	int fd = open(capture->path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	bool swap = capture->format == WL_SHM_FORMAT_ABGR8888 ||
		    capture->format == WL_SHM_FORMAT_XBGR8888;
	bool invert = capture->flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
	bool flipped = capture->transform & WL_OUTPUT_TRANSFORM_FLIPPED;
	int32_t rotation = capture->transform & 3;
	uint32_t width, height;
	capture_image_size(capture, &width, &height);
	uint32_t max_x = capture->width - 1, max_y = capture->height - 1;
	size_t row_len = (size_t)width * 4;
	unsigned char *row = xrealloc(NULL, row_len);
	bool ok = true;
	for (uint32_t y = 0; y < height && ok; y++) {
		for (uint32_t x = 0; x < width; x++) {
			uint32_t bx, by;
			switch (rotation) {
			case WL_OUTPUT_TRANSFORM_90: bx = y, by = max_y - x; break;
			case WL_OUTPUT_TRANSFORM_180: bx = max_x - x, by = max_y - y; break;
			case WL_OUTPUT_TRANSFORM_270: bx = max_x - y, by = x; break;
			default: bx = x, by = y; break;
			}
			if (flipped)
				bx = max_x - bx;
			if (invert)
				by = max_y - by;
			const unsigned char *src = (const unsigned char *)capture->data +
						   (size_t)by * capture->stride + (size_t)bx * 4;
			/* Little-endian xRGB8888 is B, G, R, x in memory; xBGR8888 is R, G, B, x. */
			unsigned char *dst = row + (size_t)x * 4;
			dst[0] = src[swap ? 2 : 0];
			dst[1] = src[1];
			dst[2] = src[swap ? 0 : 2];
			dst[3] = 0xff;
		}
		ok = write_all(fd, row, row_len);
	}
	free(row);
	return close(fd) == 0 && ok;
}

static void capture_frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
				uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
	struct capture *capture = data;
	if (!capture_write(capture)) {
		emit_capture_error(capture->id, "cannot write %s: %s", capture->path, strerror(errno));
	} else {
		uint32_t width, height;
		capture_image_size(capture, &width, &height);
		ev_begin("capture");
		ev_str("id", capture->id);
		ev_int("width", width);
		ev_int("height", height);
		ev_end();
	}
	capture_free(capture);
}

static void capture_frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
	struct capture *capture = data;
	emit_capture_error(capture->id, "the compositor failed to capture the output");
	capture_free(capture);
}

static void capture_frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t x,
				 uint32_t y, uint32_t width, uint32_t height)
{
}

static void capture_frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
				       uint32_t format, uint32_t width, uint32_t height)
{
}

static void capture_frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
	capture_copy(data);
}

static const struct zwlr_screencopy_frame_v1_listener capture_frame_listener = {
	.buffer = capture_frame_buffer,
	.flags = capture_frame_flags,
	.ready = capture_frame_ready,
	.failed = capture_frame_failed,
	.damage = capture_frame_damage,
	.linux_dmabuf = capture_frame_linux_dmabuf,
	.buffer_done = capture_frame_buffer_done,
};

static void cmd_capture(char **argv, size_t argc)
{
	const char *id = argv[1];
	struct rect rect;
	if (!parse_i32(argv[2], &rect.x) || !parse_i32(argv[3], &rect.y) ||
	    !parse_i32(argv[4], &rect.width) || !parse_i32(argv[5], &rect.height) ||
	    rect.width < 1 || rect.height < 1) {
		emit_capture_error(id, "invalid rectangle");
		return;
	}
	/* The path is the rest of the line and may contain spaces. */
	for (size_t i = 6; i < argc - 1; i++)
		argv[i][strlen(argv[i])] = ' ';
	const char *path = argv[6];
	if (!*path) {
		emit_capture_error(id, "empty path");
		return;
	}
	if (!host.screencopy) {
		emit_capture_error(id, "zwlr_screencopy_manager_v1 is unavailable");
		return;
	}
	struct output *output = output_for_rect(&rect);
	if (!output) {
		emit_capture_error(id, "no output");
		return;
	}
	struct rect out = output_logical(output);
	int64_t left = rect.x, top = rect.y;
	int64_t right = left + rect.width, bottom = top + rect.height;
	/* A clipped capture would not match the requested rectangle. */
	if (left < out.x || top < out.y || right > (int64_t)out.x + out.width ||
	    bottom > (int64_t)out.y + out.height) {
		emit_capture_error(id, "the rectangle is not within one output");
		return;
	}
	struct capture *capture = xcalloc(1, sizeof(*capture));
	capture->id = xstrdup(id);
	capture->path = xstrdup(path);
	capture->transform = output->transform;
	wl_list_insert(&host.captures, &capture->link);
	capture->frame = zwlr_screencopy_manager_v1_capture_output_region(
		host.screencopy, 0, output->wl, (int32_t)(left - out.x), (int32_t)(top - out.y),
		(int32_t)(right - left), (int32_t)(bottom - top));
	zwlr_screencopy_frame_v1_add_listener(capture->frame, &capture_frame_listener, capture);
}

/* Pointer probe. No protocol reports the pointer position to a client whose
 * surface it is not over, so transparent surfaces covering every output are
 * mapped until one receives wl_pointer.enter. */

static void probe_surface_destroy(struct probe_surface *probe)
{
	if (probe->viewport)
		wp_viewport_destroy(probe->viewport);
	zwlr_layer_surface_v1_destroy(probe->layer);
	wl_surface_destroy(probe->wl);
	if (probe->buffer)
		wl_buffer_destroy(probe->buffer);
	wl_list_remove(&probe->link);
	free(probe);
}

/* Answers every pending probe and removes the probe surfaces. */
static void probe_finish(bool found, double x, double y)
{
	struct probe_surface *probe, *tmp;
	wl_list_for_each_safe(probe, tmp, &host.probe_surfaces, link)
		probe_surface_destroy(probe);
	timer_arm(host.probe_fd, 0);
	host.probe_mapped = false;
	char **ids = host.probe_ids;
	size_t count = host.probe_id_count;
	host.probe_ids = NULL;
	host.probe_id_count = 0;
	for (size_t i = 0; i < count; i++) {
		ev_begin("probe");
		ev_str("id", ids[i]);
		if (found) {
			ev_num("x", x);
			ev_num("y", y);
		} else {
			ev_str("x", NULL);
			ev_str("y", NULL);
		}
		ev_end();
		free(ids[i]);
	}
	free(ids);
}

/* niri does not move pointer focus to a surface mapped under a still pointer,
 * so a zero relative motion makes it look up the surface again. The pointer
 * does not move. */
static void probe_nudge(void)
{
	if (!host.vp_manager || !host.seat)
		return;
	if (!host.vpointer)
		host.vpointer =
			zwlr_virtual_pointer_manager_v1_create_virtual_pointer(host.vp_manager, host.seat);
	zwlr_virtual_pointer_v1_motion(host.vpointer, now_ms(), 0, 0);
	zwlr_virtual_pointer_v1_frame(host.vpointer);
}

static void probe_check_mapped(void)
{
	if (host.probe_mapped)
		return;
	struct probe_surface *probe;
	wl_list_for_each(probe, &host.probe_surfaces, link) {
		if (!probe->committed)
			return;
	}
	host.probe_mapped = true;
	timer_arm(host.probe_fd, PROBE_ENTER_TIMEOUT_MS);
	probe_nudge();
}

static void probe_layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
				  uint32_t width, uint32_t height)
{
	struct probe_surface *probe = data;
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	struct rect out = output_logical(probe->output);
	int32_t w = width ? (int32_t)width : out.width;
	int32_t h = height ? (int32_t)height : out.height;
	if (w < 1 || h < 1)
		w = h = 1;
	struct wl_buffer *buffer;
	if (probe->viewport) {
		if (!host.probe_buffer)
			host.probe_buffer = transparent_buffer_new(1, 1);
		buffer = host.probe_buffer;
		wp_viewport_set_destination(probe->viewport, w, h);
	} else {
		/* Without a viewport the buffer sets the surface size, which bounds
		 * the input region. */
		if (probe->buffer)
			wl_buffer_destroy(probe->buffer);
		probe->buffer = transparent_buffer_new(w, h);
		buffer = probe->buffer;
	}
	if (buffer) {
		wl_surface_attach(probe->wl, buffer, 0, 0);
		wl_surface_damage(probe->wl, 0, 0, INT32_MAX, INT32_MAX);
	}
	wl_surface_commit(probe->wl);
	probe->committed = true;
	probe_check_mapped();
}

static void probe_layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
	probe_surface_destroy(data);
	if (wl_list_empty(&host.probe_surfaces))
		probe_finish(false, 0, 0);
	else
		probe_check_mapped();
}

static const struct zwlr_layer_surface_v1_listener probe_layer_listener = {
	.configure = probe_layer_configure,
	.closed = probe_layer_closed,
};

static void probe_begin(void)
{
	if (!host.layer_shell || !host.compositor || !host.shm || !host.pointer) {
		probe_finish(false, 0, 0);
		return;
	}
	struct output *output;
	wl_list_for_each(output, &host.outputs, link) {
		if (!output->done)
			continue;
		struct probe_surface *probe = xcalloc(1, sizeof(*probe));
		probe->output = output;
		probe->wl = wl_compositor_create_surface(host.compositor);
		probe->layer = zwlr_layer_shell_v1_get_layer_surface(
			host.layer_shell, probe->wl, output->wl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
			PROBE_NAMESPACE);
		zwlr_layer_surface_v1_add_listener(probe->layer, &probe_layer_listener, probe);
		zwlr_layer_surface_v1_set_anchor(probe->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
							       ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
							       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
							       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_size(probe->layer, 0, 0);
		zwlr_layer_surface_v1_set_exclusive_zone(probe->layer, -1);
		zwlr_layer_surface_v1_set_keyboard_interactivity(
			probe->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
		if (host.viewporter)
			probe->viewport = wp_viewporter_get_viewport(host.viewporter, probe->wl);
		wl_surface_commit(probe->wl);
		wl_list_insert(host.probe_surfaces.prev, &probe->link);
	}
	if (wl_list_empty(&host.probe_surfaces)) {
		probe_finish(false, 0, 0);
		return;
	}
	timer_arm(host.probe_fd, PROBE_MAP_TIMEOUT_MS);
}

static void probe_handle_timer(void)
{
	uint64_t expirations;
	if (read(host.probe_fd, &expirations, sizeof(expirations)) != sizeof(expirations))
		return;
	if (host.probe_id_count > 0)
		probe_finish(false, 0, 0);
}

static bool probe_pointer_enter(struct wl_surface *wl, wl_fixed_t x, wl_fixed_t y)
{
	if (!wl)
		return false;
	struct probe_surface *probe;
	wl_list_for_each(probe, &host.probe_surfaces, link) {
		if (probe->wl != wl)
			continue;
		struct rect out = output_logical(probe->output);
		probe_finish(true, out.x + wl_fixed_to_double(x), out.y + wl_fixed_to_double(y));
		return true;
	}
	return false;
}

static void probe_output_removed(struct output *output)
{
	struct probe_surface *probe, *tmp;
	bool removed = false;
	wl_list_for_each_safe(probe, tmp, &host.probe_surfaces, link) {
		if (probe->output == output) {
			probe_surface_destroy(probe);
			removed = true;
		}
	}
	if (!removed)
		return;
	if (wl_list_empty(&host.probe_surfaces))
		probe_finish(false, 0, 0);
	else
		probe_check_mapped();
}

static void cmd_probe(char **argv, size_t argc)
{
	host.probe_ids = xrealloc(host.probe_ids, (host.probe_id_count + 1) * sizeof(*host.probe_ids));
	host.probe_ids[host.probe_id_count++] = xstrdup(argv[1]);
	/* A probe in progress answers this one too. */
	if (host.probe_id_count == 1)
		probe_begin();
}

/* Commands */

static struct surface *command_surface(const char *command, const char *sid)
{
	struct surface *surface = surface_find(sid);
	if (!surface)
		log_msg("%s: unknown surface %s", command, sid);
	return surface;
}

static bool sid_is_numeric(const char *sid)
{
	size_t len = strlen(sid);
	if (len == 0 || len > 15 || (sid[0] == '0' && len > 1))
		return false;
	for (size_t i = 0; i < len; i++) {
		if (sid[i] < '0' || sid[i] > '9')
			return false;
	}
	return true;
}

static void cmd_create(char **argv, size_t argc)
{
	if (!*argv[1]) {
		log_msg("create: empty sid");
		return;
	}
	if (surface_find(argv[1])) {
		log_msg("create: surface %s already exists", argv[1]);
		return;
	}
	struct surface *surface = xcalloc(1, sizeof(*surface));
	surface->sid = xstrdup(argv[1]);
	surface->numeric_sid = sid_is_numeric(argv[1]);
	surface->pending_index = -1;
	surface->layer_kind = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	wl_list_init(&surface->retired);
	wl_list_insert(host.surfaces.prev, &surface->link);
}

static void cmd_destroy(char **argv, size_t argc)
{
	struct surface *surface = command_surface(argv[0], argv[1]);
	if (surface)
		surface_destroy(surface);
}

static void cmd_geometry(char **argv, size_t argc)
{
	struct surface *surface = command_surface(argv[0], argv[1]);
	struct rect rect;
	if (!surface)
		return;
	if (!parse_i32(argv[2], &rect.x) || !parse_i32(argv[3], &rect.y) ||
	    !parse_i32(argv[4], &rect.width) || !parse_i32(argv[5], &rect.height) ||
	    rect.width < 1 || rect.height < 1) {
		log_msg("geometry: invalid rectangle");
		return;
	}
	surface->geometry = rect;
	surface->has_geometry = true;
	if (!surface->visible)
		return;
	if (!surface->wl) {
		surface_map(surface);
		return;
	}
	if (output_for_rect(&rect) != surface->output) {
		surface_unmap(surface);
		surface_map(surface);
		return;
	}
	surface_apply_geometry(surface);
	wl_surface_commit(surface->wl);
}

static void cmd_keyboard(char **argv, size_t argc)
{
	struct surface *surface = command_surface(argv[0], argv[1]);
	if (!surface)
		return;
	if (strcmp(argv[2], "none") == 0) {
		surface->keyboard = KEYBOARD_NONE;
	} else if (strcmp(argv[2], "on_demand") == 0) {
		surface->keyboard = KEYBOARD_ON_DEMAND;
	} else if (strcmp(argv[2], "exclusive") == 0) {
		surface->keyboard = KEYBOARD_EXCLUSIVE;
	} else {
		log_msg("keyboard: unknown mode %s", argv[2]);
		return;
	}
	if (surface->layer) {
		surface_apply_keyboard(surface);
		wl_surface_commit(surface->wl);
	}
}

static void cmd_layer(char **argv, size_t argc)
{
	static const struct {
		const char *name;
		enum zwlr_layer_shell_v1_layer layer;
	} layers[] = {
		{ "background", ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND },
		{ "bottom", ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM },
		{ "top", ZWLR_LAYER_SHELL_V1_LAYER_TOP },
		{ "overlay", ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY },
	};
	struct surface *surface = command_surface(argv[0], argv[1]);
	if (!surface)
		return;
	size_t i = 0;
	while (i < ARRAY_LEN(layers) && strcmp(argv[2], layers[i].name) != 0)
		i++;
	if (i == ARRAY_LEN(layers)) {
		log_msg("layer: unknown layer %s", argv[2]);
		return;
	}
	if (surface->layer_kind == layers[i].layer)
		return;
	surface->layer_kind = layers[i].layer;
	if (!surface->layer)
		return;
	if (zwlr_layer_surface_v1_get_version(surface->layer) >=
	    ZWLR_LAYER_SURFACE_V1_SET_LAYER_SINCE_VERSION) {
		zwlr_layer_surface_v1_set_layer(surface->layer, surface->layer_kind);
		wl_surface_commit(surface->wl);
		return;
	}
	surface_unmap(surface);
	surface_map(surface);
}

static bool parse_input_rect(char *token, struct rect *rect)
{
	char *parts[4];
	size_t count = 0;
	for (char *part = token; count < 4; count++) {
		parts[count] = part;
		char *comma = strchr(part, ',');
		if (!comma) {
			count++;
			break;
		}
		*comma = '\0';
		part = comma + 1;
	}
	double x, y, width, height;
	if (count != 4 || !parse_double(parts[0], &x) || !parse_double(parts[1], &y) ||
	    !parse_double(parts[2], &width) || !parse_double(parts[3], &height) ||
	    strchr(parts[3], ','))
		return false;
	/* Round outwards so fractional DOM rectangles stay fully interactive. */
	rect->x = floor_i32(x);
	rect->y = floor_i32(y);
	rect->width = ceil_i32(x + width) - rect->x;
	rect->height = ceil_i32(y + height) - rect->y;
	return width >= 0 && height >= 0;
}

static void cmd_input(char **argv, size_t argc)
{
	struct surface *surface = command_surface(argv[0], argv[1]);
	if (!surface)
		return;
	if (argc == 3 && strcmp(argv[2], "all") == 0) {
		surface->input = INPUT_ALL;
	} else if (argc == 3 && strcmp(argv[2], "none") == 0) {
		surface->input = INPUT_NONE;
	} else {
		struct rect *rects = xcalloc(argc - 2, sizeof(*rects));
		for (size_t i = 2; i < argc; i++) {
			if (!parse_input_rect(argv[i], &rects[i - 2])) {
				log_msg("input: invalid rectangle");
				free(rects);
				return;
			}
		}
		free(surface->rects);
		surface->rects = rects;
		surface->rect_count = argc - 2;
		surface->input = INPUT_RECTS;
	}
	if (surface->wl) {
		surface_apply_input(surface);
		wl_surface_commit(surface->wl);
	}
}

static void cmd_show(char **argv, size_t argc)
{
	struct surface *surface = command_surface(argv[0], argv[1]);
	if (!surface || surface->visible)
		return;
	surface->visible = true;
	surface_map(surface);
}

static void cmd_hide(char **argv, size_t argc)
{
	struct surface *surface = command_surface(argv[0], argv[1]);
	if (!surface)
		return;
	surface->visible = false;
	surface_unmap(surface);
}

static void cmd_raise(char **argv, size_t argc)
{
	struct surface *surface = command_surface(argv[0], argv[1]);
	if (!surface || !surface->wl)
		return;
	surface_unmap(surface);
	surface_map(surface);
}

static void cmd_buffers(char **argv, size_t argc)
{
	struct surface *surface = command_surface(argv[0], argv[1]);
	if (!surface)
		return;
	int32_t pool_id;
	if (!parse_i32(argv[2], &pool_id) || pool_id < 0) {
		log_msg("buffers: invalid pool");
		return;
	}
	/* Fields are split at every space, so rejoining them restores paths
	 * that contain spaces. */
	for (size_t i = 3; i < argc - 3; i++)
		argv[i][strlen(argv[i])] = ' ';
	const char *path = argv[3];
	int32_t width, height;
	if (!parse_i32(argv[argc - 2], &width) || !parse_i32(argv[argc - 1], &height) ||
	    width < 1 || height < 1 || (int64_t)width * height * 8 > INT32_MAX) {
		log_msg("buffers: invalid size");
		return;
	}
	if (!host.shm) {
		emit_error("buffers %s: wl_shm is unavailable", surface->sid);
		return;
	}
	int32_t size = width * height * 4;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		emit_error("buffers %s: cannot open %s: %s", surface->sid, path, strerror(errno));
		return;
	}
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size < (off_t)size * 2) {
		emit_error("buffers %s: %s is smaller than %d bytes", surface->sid, path, size * 2);
		close(fd);
		return;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(host.shm, fd, size * 2);
	close(fd);

	for (int i = 0; i < 2; i++) {
		struct buffer *old = surface->buffers[i];
		if (old && old->busy) {
			old->retired = true;
			wl_list_insert(&surface->retired, &old->link);
		} else if (old) {
			buffer_destroy(old);
		}
		struct buffer *buffer = xcalloc(1, sizeof(*buffer));
		buffer->owner = surface;
		buffer->pool = (uint32_t)pool_id;
		buffer->index = i;
		buffer->wl = wl_shm_pool_create_buffer(pool, i * size, width, height, width * 4,
						       WL_SHM_FORMAT_ARGB8888);
		wl_buffer_add_listener(buffer->wl, &buffer_listener, buffer);
		surface->buffers[i] = buffer;
	}
	wl_shm_pool_destroy(pool);
	surface->buffer_width = width;
	surface->buffer_height = height;
	surface->pool = (uint32_t)pool_id;
	surface->pending_index = -1;

	ev_begin("buffers");
	ev_sid(surface);
	ev_int("pool", pool_id);
	ev_str("path", path);
	ev_end();
}

static void cmd_commit(char **argv, size_t argc)
{
	struct surface *surface = command_surface(argv[0], argv[1]);
	int32_t pool, index;
	struct rect damage;
	if (!surface)
		return;
	if (!parse_i32(argv[2], &pool) || pool < 0 || !parse_i32(argv[3], &index) || index < 0 ||
	    index > 1 || !parse_i32(argv[4], &damage.x) || !parse_i32(argv[5], &damage.y) ||
	    !parse_i32(argv[6], &damage.width) || !parse_i32(argv[7], &damage.height)) {
		log_msg("commit: invalid arguments");
		return;
	}
	if (!surface->buffers[index] || surface->pool != (uint32_t)pool) {
		/* The pool failed to open or was replaced. The buffer was never
		 * attached, so hand it straight back. */
		log_msg("commit %s: pool %d is not current", surface->sid, pool);
		emit_release(surface, (uint32_t)pool, index);
		return;
	}
	if (surface->wl && surface->configured) {
		surface_present(surface, index, &damage);
		return;
	}
	/* Only the newest frame is shown once configured; return the one it
	 * supersedes, which was never attached. */
	int previous = surface->pending_index;
	surface->pending_index = index;
	if (previous >= 0 && previous != index && !surface->buffers[previous]->busy)
		emit_release(surface, surface->pool, previous);
}

static void cmd_cursor(char **argv, size_t argc)
{
	struct surface *surface = command_surface(argv[0], argv[1]);
	if (!surface)
		return;
	replace_string(&surface->cursor, argv[2]);
	if (host.pointer_focus == surface)
		pointer_apply_cursor(surface);
}

static void cmd_activate(char **argv, size_t argc)
{
	uint32_t id;
	if (!parse_u32(argv[1], &id)) {
		log_msg("activate: invalid toplevel id %s", argv[1]);
		return;
	}
	if (kwin_activate(id))
		return;
	if (!host.seat) {
		log_msg("activate: no seat");
		return;
	}
	struct toplevel *toplevel;
	wl_list_for_each(toplevel, &host.toplevels, link) {
		if (toplevel->id == id) {
			zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, host.seat);
			return;
		}
	}
	log_msg("activate: unknown toplevel %u", id);
}

static void cmd_clipboard_set(char **argv, size_t argc)
{
	if (!host.dc_device) {
		log_msg("clipboard-set: data control is unavailable");
		return;
	}
	char *text = NULL, *html = NULL;
	size_t text_len = 0, html_len = 0;
	if (!base64_decode(argv[1], &text, &text_len)) {
		log_msg("clipboard-set: invalid base64 text");
		return;
	}
	if (argc > 2 && strcmp(argv[2], "-") != 0 && !base64_decode(argv[2], &html, &html_len)) {
		log_msg("clipboard-set: invalid base64 html");
		free(text);
		return;
	}
	clipboard_set(text, text_len, html, html_len);
	free(text);
	free(html);
}

static void cmd_clipboard_clear(char **argv, size_t argc)
{
	if (!host.dc_device) {
		log_msg("clipboard-clear: data control is unavailable");
		return;
	}
	dc_set_selection(NULL);
}

static void cmd_remote_desktop(char **argv, size_t argc)
{
	if (strcmp(argv[1], "stop") == 0 && argc == 2) {
		rd_stop();
		return;
	}
	if (strcmp(argv[1], "start") != 0) {
		log_msg("remote-desktop: expected start or stop");
		return;
	}
	if (!host.remote_desktop) {
		ev_begin("remote-desktop");
		ev_str("state", "closed");
		ev_bool("keyboard", false);
		ev_bool("pointer", false);
		ev_str("reason", "unavailable");
		ev_end();
		return;
	}
	rd_start(argc > 2 && strcmp(argv[2], "-") != 0 ? argv[2] : NULL);
}

static void cmd_quit(char **argv, size_t argc)
{
	wl_display_flush(host.display);
	exit(0);
}

static const struct command {
	const char *name;
	size_t min_args, max_args; /* including the command name; 0 = unbounded */
	void (*handler)(char **argv, size_t argc);
} commands[] = {
	{ "create", 2, 2, cmd_create },
	{ "destroy", 2, 2, cmd_destroy },
	{ "geometry", 6, 6, cmd_geometry },
	{ "keyboard", 3, 3, cmd_keyboard },
	{ "layer", 3, 3, cmd_layer },
	{ "input", 3, 0, cmd_input },
	{ "show", 2, 2, cmd_show },
	{ "hide", 2, 2, cmd_hide },
	{ "raise", 2, 2, cmd_raise },
	{ "buffers", 6, 0, cmd_buffers },
	{ "commit", 8, 8, cmd_commit },
	{ "cursor", 3, 3, cmd_cursor },
	{ "activate", 2, 2, cmd_activate },
	{ "clipboard-set", 2, 3, cmd_clipboard_set },
	{ "clipboard-clear", 1, 1, cmd_clipboard_clear },
	{ "evdev", 2, 2, cmd_evdev },
	{ "vkey", 3, 3, cmd_vkey },
	{ "warp", 3, 3, cmd_warp },
	{ "capture", 7, 0, cmd_capture },
	{ "probe", 2, 2, cmd_probe },
	{ "remote-desktop", 2, 3, cmd_remote_desktop },
	{ "quit", 1, 1, cmd_quit },
};

static void handle_command(char *line, size_t len)
{
	if (len > 0 && line[len - 1] == '\r')
		line[--len] = '\0';
	if (len == 0)
		return;

	static char **argv;
	static size_t argv_cap;
	size_t argc = 0;
	char *field = line;
	for (;;) {
		if (argc == argv_cap) {
			argv_cap = argv_cap ? argv_cap * 2 : 16;
			argv = xrealloc(argv, argv_cap * sizeof(*argv));
		}
		argv[argc++] = field;
		char *space = strchr(field, ' ');
		if (!space)
			break;
		*space = '\0';
		field = space + 1;
	}

	for (size_t i = 0; i < ARRAY_LEN(commands); i++) {
		const struct command *command = &commands[i];
		if (strcmp(argv[0], command->name) != 0)
			continue;
		if (argc < command->min_args || (command->max_args && argc > command->max_args)) {
			log_msg("%s: wrong number of arguments", command->name);
			return;
		}
		command->handler(argv, argc);
		return;
	}
	log_msg("unknown command: %.64s", argv[0]);
}

/* Returns false on EOF. */
static bool stdin_read(void)
{
	struct strbuf *buf = &host.stdin_buf;
	sb_reserve(buf, 65536);
	/* One byte stays free for the terminator written on EOF. */
	ssize_t len = read(STDIN_FILENO, buf->data + buf->len, buf->cap - buf->len - 1);
	if (len < 0)
		return errno == EINTR || errno == EAGAIN;
	if (len == 0) {
		if (buf->len > 0 && !host.stdin_discarding) {
			buf->data[buf->len] = '\0';
			handle_command(buf->data, buf->len);
		}
		buf->len = 0;
		return false;
	}

	size_t scan = buf->len, start = 0;
	buf->len += (size_t)len;
	char *newline;
	while ((newline = memchr(buf->data + scan, '\n', buf->len - scan))) {
		*newline = '\0';
		size_t end = (size_t)(newline - buf->data);
		if (host.stdin_discarding)
			host.stdin_discarding = false;
		else
			handle_command(buf->data + start, end - start);
		start = scan = end + 1;
	}
	memmove(buf->data, buf->data + start, buf->len - start);
	buf->len -= start;
	if (buf->len > COMMAND_LINE_MAX) {
		log_msg("command line exceeds %zu bytes, discarding it", COMMAND_LINE_MAX);
		host.stdin_discarding = true;
		buf->len = 0;
	}
	return true;
}

/* Main */

static void emit_ready(void)
{
	ev_begin("ready");
	ev_bool("layerShell", host.layer_shell != NULL);
	ev_int("layerShellVersion",
	       host.layer_shell ? zwlr_layer_shell_v1_get_version(host.layer_shell) : 0);
	ev_bool("foreignToplevel", host.toplevel_global != 0);
	ev_bool("dataControl", host.dc_ext_global != 0 || host.dc_wlr_global != 0);
	ev_bool("cursorShape", host.cursor_shape_manager != NULL);
	ev_bool("viewporter", host.viewporter != NULL);
	ev_bool("virtualKeyboard", host.vk_manager != NULL);
	ev_bool("uinput", access(UINPUT_PATH, W_OK) == 0);
	ev_bool("virtualPointer", host.vp_manager != NULL);
	ev_bool("screencopy", host.screencopy != NULL);
	ev_bool("kwinScript", host.kwin_script);
	ev_bool("remoteDesktop", host.remote_desktop);
	ev_end();
}

enum poll_slot {
	SLOT_DISPLAY,
	SLOT_STDIN,
	SLOT_REPEAT,
	SLOT_PROBE,
	SLOT_INOTIFY,
	SLOT_BUS,
	SLOT_EI,
	SLOT_FIXED_COUNT
};

static void run(void)
{
	struct pollfd *pfds = NULL;
	size_t pfds_cap = 0;
	for (;;) {
		/* Blocking D-Bus calls may have queued messages without the fd
		 * becoming readable again. */
		bus_dispatch(false);
		while (wl_display_prepare_read(host.display) != 0) {
			if (wl_display_dispatch_pending(host.display) < 0)
				display_failed();
		}
		if (host.outputs_dirty)
			emit_outputs();

		short display_events = POLLIN;
		if (wl_display_flush(host.display) < 0) {
			if (errno == EAGAIN)
				display_events |= POLLOUT;
			else if (errno != EPIPE) {
				wl_display_cancel_read(host.display);
				display_failed();
			}
		}

		size_t count = SLOT_FIXED_COUNT + ARRAY_LEN(host.clip_reads) +
			       (size_t)wl_list_length(&host.clip_writes) +
			       (size_t)wl_list_length(&host.evdev_devices);
		if (count > pfds_cap) {
			pfds_cap = count * 2;
			pfds = xrealloc(pfds, pfds_cap * sizeof(*pfds));
		}
		pfds[SLOT_DISPLAY] = (struct pollfd){ wl_display_get_fd(host.display), display_events, 0 };
		pfds[SLOT_STDIN] = (struct pollfd){ STDIN_FILENO, POLLIN, 0 };
		pfds[SLOT_REPEAT] = (struct pollfd){ host.repeat_fd, POLLIN, 0 };
		pfds[SLOT_PROBE] = (struct pollfd){ host.probe_fd, POLLIN, 0 };
		/* poll ignores negative fds, which stand for off. */
		pfds[SLOT_INOTIFY] = (struct pollfd){ host.evdev_inotify_fd, POLLIN, 0 };
		pfds[SLOT_BUS] = (struct pollfd){ bus_fd(), POLLIN, 0 };
		pfds[SLOT_EI] = (struct pollfd){ rd_fd(), POLLIN, 0 };
		size_t n = SLOT_FIXED_COUNT;
		struct evdev_device *device, *device_tmp;
		wl_list_for_each(device, &host.evdev_devices, link)
			pfds[n++] = (struct pollfd){ device->fd, POLLIN, 0 };
		size_t evdev_end = n;
		for (size_t i = 0; i < ARRAY_LEN(host.clip_reads); i++)
			pfds[n++] = (struct pollfd){ host.clip_reads[i].fd, POLLIN, 0 };
		struct clip_write *job, *tmp;
		wl_list_for_each(job, &host.clip_writes, link)
			pfds[n++] = (struct pollfd){ job->fd, POLLOUT, 0 };

		if (poll(pfds, n, -1) < 0) {
			wl_display_cancel_read(host.display);
			if (errno == EINTR)
				continue;
			fatal("poll: %s", strerror(errno));
		}

		if (pfds[SLOT_DISPLAY].revents & (POLLIN | POLLERR | POLLHUP)) {
			if (wl_display_read_events(host.display) < 0)
				display_failed();
		} else {
			wl_display_cancel_read(host.display);
		}
		if (wl_display_dispatch_pending(host.display) < 0)
			display_failed();

		if (pfds[SLOT_REPEAT].revents & POLLIN)
			keyboard_handle_repeat();
		if (pfds[SLOT_PROBE].revents & POLLIN)
			probe_handle_timer();
		if (pfds[SLOT_INOTIFY].revents & POLLIN)
			evdev_inotify_read();
		if (pfds[SLOT_BUS].revents)
			bus_dispatch(true);
		if (pfds[SLOT_EI].revents)
			rd_dispatch();

		/* Dispatching may have replaced devices, reads and writes, so the
		 * poll results are matched to the current ones by fd. Spurious
		 * readiness on a reused fd is harmless because all of them are
		 * nonblocking. */
		for (size_t i = SLOT_FIXED_COUNT; i < evdev_end; i++) {
			if (!pfds[i].revents)
				continue;
			wl_list_for_each_safe(device, device_tmp, &host.evdev_devices, link) {
				if (device->fd == pfds[i].fd) {
					evdev_read(device);
					break;
				}
			}
		}
		for (size_t i = evdev_end; i < n; i++) {
			if (!pfds[i].revents || pfds[i].fd < 0)
				continue;
			for (size_t r = 0; r < ARRAY_LEN(host.clip_reads); r++) {
				if (host.clip_reads[r].fd == pfds[i].fd && pfds[i].events == POLLIN)
					clip_read_progress(&host.clip_reads[r]);
			}
			if (pfds[i].events != POLLOUT)
				continue;
			wl_list_for_each_safe(job, tmp, &host.clip_writes, link) {
				if (job->fd == pfds[i].fd) {
					clip_write_progress(job);
					break;
				}
			}
		}

		if (pfds[SLOT_STDIN].revents & (POLLIN | POLLHUP | POLLERR)) {
			if (!stdin_read())
				break;
		}
	}
	free(pfds);
}

int main(int argc, char **argv)
{
	host.probe = argc > 1 && strcmp(argv[1], "--probe") == 0;
	signal(SIGPIPE, SIG_IGN);
	wl_list_init(&host.outputs);
	wl_list_init(&host.surfaces);
	wl_list_init(&host.toplevels);
	wl_list_init(&host.dc_offers);
	wl_list_init(&host.dc_sources);
	wl_list_init(&host.clip_writes);
	wl_list_init(&host.evdev_devices);
	wl_list_init(&host.captures);
	wl_list_init(&host.probe_surfaces);
	for (size_t i = 0; i < ARRAY_LEN(host.clip_reads); i++)
		host.clip_reads[i].fd = -1;
	host.next_toplevel_id = 1;
	host.repeat_fd = -1;
	host.evdev_inotify_fd = -1;
	host.evdev_reported = -1;
	host.uinput_fd = -1;
	snprintf(host.uinput_phys, sizeof(host.uinput_phys), "overlay-wayland-host/%ld",
		 (long)getpid());

	host.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!host.xkb_context) {
		log_msg("failed to create xkb context");
		return 1;
	}
	host.repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	host.probe_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (host.repeat_fd < 0 || host.probe_fd < 0) {
		log_msg("timerfd_create: %s", strerror(errno));
		return 1;
	}

	host.display = wl_display_connect(NULL);
	if (!host.display) {
		log_msg("cannot connect to a Wayland compositor");
		ev_begin("error");
		ev_str("message", "cannot connect to a Wayland compositor");
		ev_end();
		return 2;
	}
	host.registry = wl_display_get_registry(host.display);
	wl_registry_add_listener(host.registry, &registry_listener, NULL);
	/* The first roundtrip announces globals, the second their initial state. */
	for (int i = 0; i < 2; i++) {
		if (wl_display_roundtrip(host.display) < 0)
			display_failed();
	}
	if (!host.compositor)
		fatal("compositor has no wl_compositor");
	/* The keymap follows the keyboard created in the second roundtrip, and
	 * the virtual keyboard should start with it. */
	if (host.keyboard && !host.keymap && wl_display_roundtrip(host.display) < 0)
		display_failed();

	/* Only looked for where the wlroots protocols are missing, so that
	 * other compositors never wait for the session bus. */
	host.kwin_script = !host.toplevel_global && kwin_available();
	host.remote_desktop = !host.vk_manager && rd_available();
	emit_ready();
	if (host.probe) {
		wl_display_disconnect(host.display);
		return host.layer_shell ? 0 : 1;
	}

	host.ready = true;
	host.outputs_dirty = true;
	toplevel_manager_bind();
	dc_manager_bind();
	if (host.kwin_script)
		kwin_start();
	run();
	wl_display_flush(host.display);
	return 0;
}
