/*
 * wmstatus.c: simple PulseAudio-enabled status setter for dwm and i3/sway
 *
 * Copyright (c) 2015 - 2024, Přemysl Eric Janouch <p@janouch.name>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#define LIBERTY_WANT_POLLER
#define LIBERTY_WANT_ASYNC
#define LIBERTY_WANT_PROTO_MPD

// openat, dirfd
#define _XOPEN_SOURCE 700
#define _ATFILE_SOURCE
#define _GNU_SOURCE

#include "config.h"
#undef PROGRAM_NAME
#define PROGRAM_NAME "wmstatus"
#include "liberty/liberty.c"
#include "liberty/liberty-pulse.c"

#include <dirent.h>
#include <spawn.h>

#ifdef BSD
#include <sys/endian.h>
#endif

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/sync.h>

#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/subscribe.h>
#include <pulse/stream.h>
#include <pulse/sample.h>

#include <dbus/dbus.h>

// --- Utilities ---------------------------------------------------------------

enum { PIPE_READ, PIPE_WRITE };

static void
log_message_custom (void *user_data, const char *quote, const char *fmt,
	va_list ap)
{
	(void) user_data;
	FILE *stream = stderr;

	fprintf (stream, PROGRAM_NAME ": ");
	fputs (quote, stream);
	vfprintf (stream, fmt, ap);
	fputs ("\n", stream);
}

static void
shell_quote (const char *str, struct str *output)
{
	// See SUSv3 Shell and Utilities, 2.2.3 Double-Quotes
	str_append_c (output, '"');
	for (const char *p = str; *p; p++)
	{
		if (strchr ("`$\"\\", *p))
			str_append_c (output, '\\');
		str_append_c (output, *p);
	}
	str_append_c (output, '"');
}

// --- NUT ---------------------------------------------------------------------

// More or less copied and pasted from the MPD client.  This code doesn't even
// deserve much love, the protocol is somehow even worse than MPD's.
//
// http://www.networkupstools.org/docs/developer-guide.chunked/ar01s09.html

// This was written by loosely following the top comment in NUT's parseconf.c.

enum nut_parser_state
{
	NUT_STATE_START_LINE,               ///< Start of a line
	NUT_STATE_BETWEEN,                  ///< Between words, expecting non-WS
	NUT_STATE_UNQUOTED,                 ///< Within unquoted word
	NUT_STATE_UNQUOTED_ESCAPE,          ///< Dtto after a backslash
	NUT_STATE_QUOTED,                   ///< Within a quoted word
	NUT_STATE_QUOTED_ESCAPE,            ///< Dtto after a backslash
	NUT_STATE_QUOTED_END                ///< End of word, expecting WS
};

struct nut_parser
{
	enum nut_parser_state state;        ///< Parser state
	struct str current_field;           ///< Current field

	// Public:

	struct strv fields;                 ///< Line fields
};

static void
nut_parser_init (struct nut_parser *self)
{
	self->state = NUT_STATE_START_LINE;
	self->current_field = str_make ();
	self->fields = strv_make ();
}

static void
nut_parser_free (struct nut_parser *self)
{
	str_free (&self->current_field);
	strv_free (&self->fields);
}

static int
nut_parser_end_field (struct nut_parser *self, char c)
{
	strv_append (&self->fields, self->current_field.str);
	str_reset (&self->current_field);

	if (c == '\n')
	{
		self->state = NUT_STATE_START_LINE;
		return 1;
	}

	self->state = NUT_STATE_BETWEEN;
	return 0;
}

/// Returns 1 if a complete line has been read, -1 on error, 0 otherwise
static int
nut_parser_push (struct nut_parser *self, char c)
{
	switch (self->state)
	{
	case NUT_STATE_START_LINE:
		strv_reset (&self->fields);
		str_reset (&self->current_field);
		self->state = NUT_STATE_BETWEEN;
		// Fall-through

	case NUT_STATE_BETWEEN:
		if (c == '\\')
			self->state = NUT_STATE_UNQUOTED_ESCAPE;
		else if (c == '"')
			self->state = NUT_STATE_QUOTED;
		else if (c == '\n' && self->fields.len)
		{
			self->state = NUT_STATE_START_LINE;
			return 1;
		}
		else if (!isspace_ascii (c))
		{
			str_append_c (&self->current_field, c);
			self->state = NUT_STATE_UNQUOTED;
		}
		return 0;

	case NUT_STATE_UNQUOTED:
		if (c == '\\')
			self->state = NUT_STATE_UNQUOTED_ESCAPE;
		else if (c == '"')
			return -1;
		else if (!isspace_ascii (c))
			str_append_c (&self->current_field, c);
		else
			return nut_parser_end_field (self, c);
		return 0;

	case NUT_STATE_UNQUOTED_ESCAPE:
		str_append_c (&self->current_field, c);
		self->state = NUT_STATE_UNQUOTED;
		return 0;

	case NUT_STATE_QUOTED:
		if (c == '\\')
			self->state = NUT_STATE_QUOTED_ESCAPE;
		else if (c == '"')
			self->state = NUT_STATE_QUOTED_END;
		else
			str_append_c (&self->current_field, c);
		return 0;

	case NUT_STATE_QUOTED_ESCAPE:
		str_append_c (&self->current_field, c);
		self->state = NUT_STATE_QUOTED;
		return 0;

	case NUT_STATE_QUOTED_END:
		if (!isspace_ascii (c))
			return -1;
		return nut_parser_end_field (self, c);
	}

	// Silence the compiler
	hard_assert (!"unhandled NUT parser state");
	return -1;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct nut_line
{
	LIST_HEADER (struct nut_line)

	struct strv fields;                 ///< Parsed fields from the line
};

struct nut_response
{
	struct nut_line *data;              ///< Raw result data

	bool success;                       ///< Whether a general failure occured
	char *message;                      ///< Eventually an error ID string
};

/// Task completion callback
typedef void (*nut_client_task_cb)
	(const struct nut_response *response, void *user_data);

struct nut_client_task
{
	LIST_HEADER (struct nut_client_task)

	nut_client_task_cb callback;        ///< Callback on completion
	void *user_data;                    ///< User data
};

enum nut_client_state
{
	NUT_DISCONNECTED,                   ///< Not connected
	NUT_CONNECTING,                     ///< Currently connecting
	NUT_CONNECTED                       ///< Connected
};

struct nut_client
{
	struct poller *poller;              ///< Poller

	// Connection:

	enum nut_client_state state;        ///< Connection state
	struct connector *connector;        ///< Connection establisher

	int socket;                         ///< MPD socket
	struct str read_buffer;             ///< Input yet to be processed
	struct str write_buffer;            ///< Outut yet to be be sent out
	struct poller_fd socket_event;      ///< We can read from the socket

	// Protocol:

	struct nut_parser parser;           ///< Protocol parser
	struct nut_line *data;              ///< Data from last command
	struct nut_line *data_tail;         ///< Tail of data list
	bool in_list;                       ///< Currently within a list

	struct nut_client_task *tasks;      ///< Task queue
	struct nut_client_task *tasks_tail; ///< Tail of task queue

	// User configuration:

	void *user_data;                    ///< User data for callbacks

	/// Callback after connection has been successfully established
	void (*on_connected) (void *user_data);

	/// Callback for general failures or even normal disconnection;
	/// the interface is reinitialized
	void (*on_failure) (void *user_data);
};

static void nut_client_reset (struct nut_client *self);
static void nut_client_destroy_connector (struct nut_client *self);

static void
nut_client_init (struct nut_client *self, struct poller *poller)
{
	memset (self, 0, sizeof *self);

	self->poller = poller;
	self->socket = -1;

	self->read_buffer = str_make ();
	self->write_buffer = str_make ();

	nut_parser_init (&self->parser);

	self->socket_event = poller_fd_make (poller, -1);
}

static void
nut_client_free (struct nut_client *self)
{
	// So that we don't have to repeat most of the stuff
	nut_client_reset (self);

	str_free (&self->read_buffer);
	str_free (&self->write_buffer);

	nut_parser_free (&self->parser);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
nut_client_flush_data (struct nut_client *self)
{
	LIST_FOR_EACH (struct nut_line, iter, self->data)
	{
		strv_free (&iter->fields);
		free (iter);
	}
	self->data = self->data_tail = NULL;
}

/// Reinitialize the interface so that you can reconnect anew
static void
nut_client_reset (struct nut_client *self)
{
	if (self->state == NUT_CONNECTING)
		nut_client_destroy_connector (self);

	if (self->socket != -1)
		xclose (self->socket);
	self->socket = -1;

	self->socket_event.closed = true;
	poller_fd_reset (&self->socket_event);

	str_reset (&self->read_buffer);
	str_reset (&self->write_buffer);

	self->parser.state = NUT_STATE_START_LINE;
	nut_client_flush_data (self);
	self->in_list = false;

	LIST_FOR_EACH (struct nut_client_task, iter, self->tasks)
		free (iter);
	self->tasks = self->tasks_tail = NULL;

	self->state = NUT_DISCONNECTED;
}

static void
nut_client_fail (struct nut_client *self)
{
	nut_client_reset (self);
	if (self->on_failure)
		self->on_failure (self->user_data);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
nut_client_quote (const char *s, struct str *output)
{
	str_append_c (output, '"');
	for (; *s; s++)
	{
		if (*s == '"' || *s == '\\')
			str_append_c (output, '\\');
		str_append_c (output, *s);
	}
	str_append_c (output, '"');
}

static bool
nut_client_must_quote (const char *s)
{
	if (!*s)
		return true;
	for (; *s; s++)
		if ((unsigned char) *s <= ' ' || *s == '"' || *s == '\\')
			return true;
	return false;
}

static void
nut_client_serialize (char **commands, struct str *line)
{
	for (; *commands; commands++)
	{
		if (line->len)
			str_append_c (line, ' ');

		if (nut_client_must_quote (*commands))
			nut_client_quote (*commands, line);
		else
			str_append (line, *commands);
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
nut_client_dispatch (struct nut_client *self, struct nut_response *response)
{
	struct nut_client_task *task;
	if (!(task = self->tasks))
		return;

	if (task->callback)
		task->callback (response, task->user_data);
	nut_client_flush_data (self);

	LIST_UNLINK_WITH_TAIL (self->tasks, self->tasks_tail, task);
	free (task);
}

static bool
nut_client_parse_line (struct nut_client *self)
{
	struct str reconstructed = str_make ();
	nut_client_serialize (self->parser.fields.vector, &reconstructed);
	print_debug ("NUT >> %s", reconstructed.str);
	str_free (&reconstructed);

	struct strv *fields = &self->parser.fields;
	hard_assert (fields->len != 0);

	// Lists are always dispatched as their innards (and they can be empty)
	if (fields->len >= 2
	 && !strcmp (fields->vector[0], "BEGIN")
	 && !strcmp (fields->vector[1], "LIST"))
		self->in_list = true;
	else if (fields->len >= 2
	 && !strcmp (fields->vector[0], "END")
	 && !strcmp (fields->vector[1], "LIST"))
		self->in_list = false;
	else
	{
		struct nut_line *line = xcalloc (1, sizeof *line);
		line->fields = strv_make ();
		strv_append_vector (&line->fields, fields->vector);
		LIST_APPEND_WITH_TAIL (self->data, self->data_tail, line);
	}

	if (!self->in_list)
	{
		struct nut_response response;
		memset (&response, 0, sizeof response);
		response.success = true;
		response.data = self->data;

		if (!strcmp (fields->vector[0], "ERR"))
		{
			response.success = false;
			if (fields->len < 2)
				return false;
			response.message = xstrdup (fields->vector[1]);
		}

		nut_client_dispatch (self, &response);
		free (response.message);
	}
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
nut_client_update_poller (struct nut_client *self)
{
	poller_fd_set (&self->socket_event,
		self->write_buffer.len ? (POLLIN | POLLOUT) : POLLIN);
}

static bool
nut_client_process_input (struct nut_client *self)
{
	struct str *rb = &self->read_buffer;
	for (size_t i = 0; i < rb->len; i++)
	{
		int res = nut_parser_push (&self->parser, rb->str[i]);
		if (res == -1 || (res == 1 && !nut_client_parse_line (self)))
			return false;
	}

	str_reset (rb);
	return true;
}

static void
nut_client_on_ready (const struct pollfd *pfd, void *user_data)
{
	(void) pfd;

	struct nut_client *self = user_data;
	bool read_succeeded = socket_io_try_read
		(self->socket, &self->read_buffer) == SOCKET_IO_OK;

	// Whether or not the read was successful, we need to process all data
	if (!nut_client_process_input (self) || !read_succeeded
	 || socket_io_try_write (self->socket, &self->write_buffer) != SOCKET_IO_OK)
		nut_client_fail (self);
	else
		nut_client_update_poller (self);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Beware that delivery of the event isn't deferred and you musn't make
/// changes to the interface while processing the event!
static void
nut_client_add_task
	(struct nut_client *self, nut_client_task_cb cb, void *user_data)
{
	struct nut_client_task *task = xcalloc (1, sizeof *self);
	task->callback = cb;
	task->user_data = user_data;
	LIST_APPEND_WITH_TAIL (self->tasks, self->tasks_tail, task);
}

/// Send a command.  Remember to call nut_client_add_task() to handle responses,
/// unless the command generates none.
static void nut_client_send_command
	(struct nut_client *self, const char *command, ...) ATTRIBUTE_SENTINEL;

static void
nut_client_send_commandv (struct nut_client *self, char **commands)
{
	struct str line = str_make ();
	nut_client_serialize (commands, &line);

	print_debug ("NUT << %s", line.str);
	str_append_c (&line, '\n');
	str_append_str (&self->write_buffer, &line);
	str_free (&line);

	nut_client_update_poller (self);
}

static void
nut_client_send_command (struct nut_client *self, const char *command, ...)
{
	struct strv v = strv_make ();

	va_list ap;
	va_start (ap, command);
	for (; command; command = va_arg (ap, const char *))
		strv_append (&v, command);
	va_end (ap);

	nut_client_send_commandv (self, v.vector);
	strv_free (&v);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
nut_client_finish_connection (struct nut_client *self, int socket)
{
	set_blocking (socket, false);
	self->socket = socket;
	self->state = NUT_CONNECTED;

	self->socket_event = poller_fd_make (self->poller, self->socket);
	self->socket_event.dispatcher = nut_client_on_ready;
	self->socket_event.user_data = self;

	nut_client_update_poller (self);

	if (self->on_connected)
		self->on_connected (self->user_data);
}

static void
nut_client_destroy_connector (struct nut_client *self)
{
	if (self->connector)
		connector_free (self->connector);
	free (self->connector);
	self->connector = NULL;

	// Not connecting anymore
	self->state = NUT_DISCONNECTED;
}

static void
nut_client_on_connector_failure (void *user_data)
{
	struct nut_client *self = user_data;
	nut_client_destroy_connector (self);
	nut_client_fail (self);
}

static void
nut_client_on_connector_connected
	(void *user_data, int socket, const char *host)
{
	(void) host;

	struct nut_client *self = user_data;
	nut_client_destroy_connector (self);
	nut_client_finish_connection (self, socket);
}

static void
nut_client_connect
	(struct nut_client *self, const char *address, const char *service)
{
	hard_assert (self->state == NUT_DISCONNECTED);

	struct connector *connector = xmalloc (sizeof *connector);
	connector_init (connector, self->poller);
	self->connector = connector;

	connector->user_data    = self;
	connector->on_connected = nut_client_on_connector_connected;
	connector->on_failure   = nut_client_on_connector_failure;

	connector_add_target (connector, address, service);
	self->state = NUT_CONNECTING;
}

// --- Backends ----------------------------------------------------------------

struct backend
{
	/// Initialization
	void (*start) (struct backend *self);
	/// Deinitialization
	void (*stop) (struct backend *self);
	/// Destroy the backend object
	void (*destroy) (struct backend *self);

	/// Add another entry to the status
	void (*add) (struct backend *self, const char *entry);
	/// Flush the status to the window manager
	void (*flush) (struct backend *self);
};

// --- DWM backend -------------------------------------------------------------

struct backend_dwm
{
	struct backend super;               ///< Parent class
	Display *dpy;                       ///< X11 Display
	struct strv items;                  ///< Items on the current row
};

static void
backend_dwm_destroy (struct backend *b)
{
	struct backend_dwm *self = CONTAINER_OF (b, struct backend_dwm, super);
	strv_free (&self->items);
	free (self);
}

static void
backend_dwm_add (struct backend *b, const char *entry)
{
	struct backend_dwm *self = CONTAINER_OF (b, struct backend_dwm, super);
	strv_append (&self->items, entry);
}

static void
backend_dwm_flush (struct backend *b)
{
	struct backend_dwm *self = CONTAINER_OF (b, struct backend_dwm, super);
	char *str = strv_join (&self->items, "   ");
	strv_reset (&self->items);

	// We don't have formatting, so let's at least quote those spans
	for (char *p = str; *p; p++)
		if (*p == '\001')
			*p = '"';

	print_debug ("setting status to: %s", str);
	XStoreName (self->dpy, DefaultRootWindow (self->dpy), str);
	XSync (self->dpy, False);

	free (str);
}

static struct backend *
backend_dwm_new (Display *dpy)
{
	struct backend_dwm *self = xcalloc (1, sizeof *self);
	self->super.destroy = backend_dwm_destroy;
	self->super.add     = backend_dwm_add;
	self->super.flush   = backend_dwm_flush;

	self->dpy = dpy;
	self->items = strv_make ();
	return &self->super;
}

// --- i3bar backend -----------------------------------------------------------

struct backend_i3
{
	struct backend super;               ///< Parent class
	struct strv items;                  ///< Items on the current row
};

static void
backend_i3_destroy (struct backend *b)
{
	struct backend_dwm *self = CONTAINER_OF (b, struct backend_dwm, super);
	strv_free (&self->items);
	free (self);
}

static void
backend_i3_start (struct backend *b)
{
	(void) b;
	// Start with an empty array so that we can later start with a comma
	// as i3bar's JSON library is quite pedantic
	fputs ("{\"version\":1}\n[[]", stdout);
}

static void
backend_i3_stop (struct backend *b)
{
	(void) b;
	fputc (']', stdout);
}

static void
backend_i3_add (struct backend *b, const char *entry)
{
	struct backend_i3 *self = CONTAINER_OF (b, struct backend_i3, super);
	strv_append (&self->items, entry);
}

static void
backend_i3_flush (struct backend *b)
{
	struct backend_i3 *self = CONTAINER_OF (b, struct backend_i3, super);
	fputs (",[", stdout);
	for (size_t i = 0; i < self->items.len; i++)
	{
		if (i) fputc (',', stdout);

		const char *str = self->items.vector[i];
		size_t len = strlen (str);
		if (!soft_assert (utf8_validate (str, len)))
			continue;

		fputs ("{\"full_text\":\"", stdout);
		bool bold = false;
		for (const char *p = str; *p; p++)
			if      (*p == '"')  fputs ("\\\"",  stdout);
			else if (*p == '\\') fputs ("\\\\",  stdout);
			else if (*p == '<')  fputs ("&lt;",  stdout);
			else if (*p == '>')  fputs ("&gt;",  stdout);
			else if (*p == '&')  fputs ("&amp;", stdout);
			else if (*p == '\001')
				fputs ((bold = !bold)
					? "<span weight='bold'>" : "</span>", stdout);
			else
				fputc (*p, stdout);
		if (bold)
			fputs ("</span>", stdout);
		fputs ("\",\"separator\":false,\"markup\":\"pango\"}", stdout);
	}
	fputs ("]\n", stdout);

	// We need to flush the pipe explicitly to get i3bar to update
	fflush (stdout);
	strv_reset (&self->items);
}

static struct backend *
backend_i3_new (void)
{
	struct backend_i3 *self = xcalloc (1, sizeof *self);
	self->super.start = backend_i3_start;
	self->super.stop  = backend_i3_stop;
	self->super.add   = backend_i3_add;
	self->super.flush = backend_i3_flush;

	self->items = strv_make ();
	return &self->super;
}

// --- Configuration -----------------------------------------------------------

static struct config_schema g_config_general[] =
{
	{ .name      = "command",
	  .comment   = "Command to run for more info",
	  .type      = CONFIG_ITEM_STRING },
	{ .name      = "sleep_timer",
	  .comment   = "Idle seconds to suspend after",
	  .type      = CONFIG_ITEM_INTEGER },
	{}
};

static struct config_schema g_config_mpd[] =
{
	{ .name      = "address",
	  .comment   = "MPD host or socket",
	  .type      = CONFIG_ITEM_STRING,
	  .default_  = "\"localhost\"" },
	{ .name      = "service",
	  .comment   = "MPD service name or port",
	  .type      = CONFIG_ITEM_STRING,
	  .default_  = "\"6600\"" },
	{ .name      = "password",
	  .comment   = "MPD password",
	  .type      = CONFIG_ITEM_STRING },
	{}
};

static struct config_schema g_config_nut[] =
{
	{ .name      = "enabled",
	  .comment   = "NUT UPS status reading enabled",
	  .type      = CONFIG_ITEM_BOOLEAN,
	  .default_  = "off" },
	{ .name      = "load_thld",
	  .comment   = "NUT threshold for load display",
	  .type      = CONFIG_ITEM_INTEGER,
	  .default_  = "50" },

	// This is just a hack because my UPS doesn't report that value; a more
	// proper way of providing this information would be by making use of the
	// enhanced configuration format and allowing arbitrary per-UPS overrides
	{ .name      = "load_power",
	  .comment   = "ups.realpower.nominal fallback",
	  .type      = CONFIG_ITEM_INTEGER },
	{}
};

static void
app_load_config_general (struct config_item *subtree, void *user_data)
{
	config_schema_apply_to_object (g_config_general, subtree, user_data);
}

static void
app_load_config_mpd (struct config_item *subtree, void *user_data)
{
	config_schema_apply_to_object (g_config_mpd, subtree, user_data);
}

static void
app_load_config_nut (struct config_item *subtree, void *user_data)
{
	config_schema_apply_to_object (g_config_nut, subtree, user_data);
}

static struct config
app_make_config (void)
{
	struct config config = config_make ();
	config_register_module (&config, "general", app_load_config_general, NULL);
	config_register_module (&config, "keys",    NULL,                    NULL);
	config_register_module (&config, "mpd",     app_load_config_mpd,     NULL);
	config_register_module (&config, "nut",     app_load_config_nut,     NULL);

	// Bootstrap configuration, so that we can access schema items at all
	config_load (&config, config_item_object ());
	return config;
}

static const char *
get_config_string (struct config_item *root, const char *key)
{
	struct config_item *item = config_item_get (root, key, NULL);
	hard_assert (item);
	if (item->type == CONFIG_ITEM_NULL)
		return NULL;
	hard_assert (config_item_type_is_string (item->type));
	return item->value.string.str;
}

static const int64_t *
get_config_integer (struct config_item *root, const char *key)
{
	struct config_item *item = config_item_get (root, key, NULL);
	hard_assert (item);
	if (item->type == CONFIG_ITEM_NULL)
		return NULL;
	hard_assert (item->type == CONFIG_ITEM_INTEGER);
	return &item->value.integer;
}

static const bool *
get_config_boolean (struct config_item *root, const char *key)
{
	struct config_item *item = config_item_get (root, key, NULL);
	hard_assert (item);
	if (item->type == CONFIG_ITEM_NULL)
		return NULL;
	hard_assert (item->type == CONFIG_ITEM_BOOLEAN);
	return &item->value.boolean;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// This is essentially simplified shell command language syntax,
// without comments or double quotes, and line feeds are whitespace.
static bool
parse_binding (const char *line, struct strv *out)
{
	enum { STA, DEF, ESC, WOR, QUO, STATES };
	enum { TAKE = 1 << 3, PUSH = 1 << 4, STOP = 1 << 5, ERROR = 1 << 6 };
	enum { TWOR = TAKE | WOR };

	// We never transition back to the start state, so it can stay as a no-op
	static char table[STATES][7] =
	{
		// state    NUL          SP, TAB, LF '     \     default
		/* STA */ { STOP,        DEF,        QUO,  ESC,  TWOR },
		/* DEF */ { STOP,        0,          QUO,  ESC,  TWOR },
		/* ESC */ { ERROR,       TWOR,       TWOR, TWOR, TWOR },
		/* WOR */ { STOP | PUSH, DEF | PUSH, QUO,  ESC,  TAKE },
		/* QUO */ { ERROR,       TAKE,       WOR,  TAKE, TAKE },
	};

	strv_reset (out);
	struct str token = str_make ();
	int state = STA, edge = 0, ch = 0;
	while (true)
	{
		switch ((ch = (unsigned char) *line++))
		{
		case 0:    edge = table[state][0]; break;
		case '\t':
		case '\n':
		case ' ':  edge = table[state][1]; break;
		case '\'': edge = table[state][2]; break;
		case '\\': edge = table[state][3]; break;
		default:   edge = table[state][4]; break;
		}
		if (edge & TAKE)
			str_append_c (&token, ch);
		if (edge & PUSH)
		{
			strv_append_owned (out, str_steal (&token));
			token = str_make ();
		}
		if (edge & STOP)
		{
			str_free (&token);
			return true;
		}
		if (edge & ERROR)
		{
			str_free (&token);
			return false;
		}
		if (edge &= 7)
			state = edge;
	}
}

// --- Application -------------------------------------------------------------

struct app_context
{
	struct config config;               ///< Program configuration
	struct backend *backend;            ///< WM backend

	Display *dpy;                       ///< X display handle
	struct poller_fd x_event;           ///< X11 event

	struct poller poller;               ///< Poller
	struct poller_timer time_changed;   ///< Time change timer
	struct poller_timer make_context;   ///< Start PulseAudio communication
	struct poller_timer refresh_rest;   ///< Refresh unpollable information

	// IPC:

	int ipc_fd;                         ///< The IPC datagram socket (file)
	struct poller_fd ipc_event;         ///< IPC event

	// Sleep timer:

	int xsync_base_event_code;          ///< XSync base event code
	XSyncCounter idle_counter;          ///< XSync IDLETIME counter
	XSyncValue idle_timeout;            ///< Sleep timeout

	XSyncAlarm idle_alarm_inactive;     ///< User is inactive
	XSyncAlarm idle_alarm_active;       ///< User is active

	// Command:

	struct poller_timer command_start;  ///< Start the command
	struct strv command_current;        ///< Current output of the command
	pid_t command_pid;                  ///< PID of the command process
	int command_fd;                     ///< I/O socket
	struct poller_fd command_event;     ///< I/O event
	struct str command_buffer;          ///< Unprocessed input

	// Hotkeys:

	struct binding *bindings;           ///< Global bindings
	int xkb_base_event_code;            ///< Xkb base event code
	char *layout;                       ///< Keyboard layout

	// Insomnia:

	DBusConnection *system_bus;         ///< System bus connection
	char *insomnia_info;                ///< Status message (possibly error)
	int insomnia_fd;                    ///< Inhibiting file descriptor

	// MPD:

	struct poller_timer mpd_reconnect;  ///< Start MPD communication
	struct mpd_client mpd_client;       ///< MPD client

	char *mpd_song;                     ///< MPD current song
	bool mpd_stopped;                   ///< MPD stopped (overrides song)

	// NUT:

	struct poller_timer nut_reconnect;  ///< Start NUT communication
	struct nut_client nut_client;       ///< NUT client
	struct str_map nut_ups_info;        ///< Per-UPS information
	bool nut_success;                   ///< Information retrieved successfully

	char *nut_status;                   ///< NUT status

	// PulseAudio:

	pa_mainloop_api *api;               ///< PulseAudio event loop proxy
	pa_context *context;                ///< PulseAudio connection context

	bool failed;                        ///< General PulseAudio failure

	pa_sample_spec sink_sample_spec;    ///< Sink sample spec
	pa_cvolume sink_volume;             ///< Current volume
	bool sink_muted;                    ///< Currently muted?
	struct strv sink_ports;             ///< All sink port names
	char *sink_port_active;             ///< Active sink port

	bool source_muted;                  ///< Currently muted?

	// Noise playback:

	struct poller_timer noise_timer;    ///< Update noise timer display, or stop
	pa_stream *noise_stream;            ///< PulseAudio stream for noise playing
	time_t noise_end_time;              ///< End time of noise production, or 0
	float noise_state[2];               ///< Brownian noise state
	int noise_fadeout_iterator;         ///< Fadeout iterator, in samples
	int noise_fadeout_samples;          ///< Sample count for fadeout
};

static void
str_map_destroy (void *self)
{
	str_map_free (self);
	free (self);
}

static void
app_context_init_xsync (struct app_context *self)
{
	int n;
	if (!XSyncQueryExtension (self->dpy, &self->xsync_base_event_code, &n)
	 || !XSyncInitialize (self->dpy, &n, &n))
	{
		print_error ("cannot initialize XSync");
		return;
	}

	// The idle counter is not guaranteed to exist, only SERVERTIME is
	XSyncSystemCounter *counters = XSyncListSystemCounters (self->dpy, &n);
	while (n--)
	{
		if (!strcmp (counters[n].name, "IDLETIME"))
			self->idle_counter = counters[n].counter;
	}
	if (!self->idle_counter)
		print_error ("idle counter is missing");
	XSyncFreeSystemCounterList (counters);
}

static void
app_context_init (struct app_context *self)
{
	memset (self, 0, sizeof *self);

	self->config = app_make_config ();

	if (!(self->dpy = XkbOpenDisplay
		(NULL, &self->xkb_base_event_code, NULL, NULL, NULL, NULL)))
		exit_fatal ("cannot open display");

	poller_init (&self->poller);
	self->api = poller_pa_new (&self->poller);

	self->ipc_fd = -1;
	self->ipc_event = poller_fd_make (&self->poller, self->ipc_fd);

	self->command_current = strv_make ();
	self->command_pid = -1;
	self->command_fd = -1;
	self->command_event = poller_fd_make (&self->poller, self->command_fd);
	self->command_buffer = str_make ();

	set_cloexec (ConnectionNumber (self->dpy));
	self->x_event =
		poller_fd_make (&self->poller, ConnectionNumber (self->dpy));

	app_context_init_xsync (self);

	// So far we don't necessarily need DBus to function,
	// and we have no desire to process any incoming messages either
	DBusError err = DBUS_ERROR_INIT;
	self->insomnia_fd = -1;
	if (!(self->system_bus = dbus_bus_get (DBUS_BUS_SYSTEM, &err)))
	{
		print_error ("dbus: %s", err.message);
		dbus_error_free (&err);
	}

	self->mpd_client = mpd_client_make (&self->poller);

	nut_client_init (&self->nut_client, &self->poller);
	self->nut_ups_info = str_map_make (str_map_destroy);

	self->sink_ports = strv_make ();
}

static void
app_context_free (struct app_context *self)
{
	config_free (&self->config);
	if (self->backend)	     self->backend->destroy (self->backend);

	poller_fd_reset (&self->x_event);
	cstr_set (&self->layout, NULL);

	if (self->noise_stream)  pa_stream_unref (self->noise_stream);
	if (self->context)       pa_context_unref (self->context);
	if (self->dpy)           XCloseDisplay (self->dpy);

	if (self->ipc_fd != -1)
	{
		poller_fd_reset (&self->ipc_event);
		xclose (self->ipc_fd);
	}

	strv_free (&self->command_current);
	if (self->command_pid != -1)
		(void) kill (self->command_pid, SIGTERM);
	if (self->command_fd != -1)
	{
		poller_fd_reset (&self->command_event);
		xclose (self->command_fd);
	}
	str_free (&self->command_buffer);

	cstr_set (&self->insomnia_info, NULL);
	if (self->insomnia_fd != -1)
		xclose (self->insomnia_fd);

	mpd_client_free (&self->mpd_client);
	cstr_set (&self->mpd_song, NULL);

	nut_client_free (&self->nut_client);
	str_map_free (&self->nut_ups_info);
	cstr_set (&self->nut_status, NULL);

	strv_free (&self->sink_ports);
	cstr_set (&self->sink_port_active, NULL);

	poller_pa_destroy (self->api);
	poller_free (&self->poller);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static char *
read_value (int dir, const char *filename, struct error **e)
{
	int fd = openat (dir, filename, O_RDONLY);
	if (fd < 0)
	{
		error_set (e, "%s: %s: %s", filename, "openat", strerror (errno));
		return NULL;
	}

	FILE *fp = fdopen (fd, "r");
	if (!fp)
	{
		error_set (e, "%s: %s: %s", filename, "fdopen", strerror (errno));
		close (fd);
		return NULL;
	}

	errno = 0;
	struct str s = str_make ();
	bool success = read_line (fp, &s) && !ferror (fp);
	fclose (fp);

	if (!success)
	{
		error_set (e, "%s: %s", filename, errno ? strerror (errno) : "EOF");
		return NULL;
	}
	return str_steal (&s);
}

static unsigned long
read_number (int dir, const char *filename, struct error **e)
{
	char *value;
	if (!(value = read_value (dir, filename, e)))
		return false;

	unsigned long number = 0;
	if (!xstrtoul (&number, value, 10))
		error_set (e, "%s: %s", filename, "doesn't contain a valid number");
	free (value);
	return number;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static int
read_battery_charge (int dir)
{
	struct error *error = NULL;
	double capacity, now, full;
	if ((capacity = read_number (dir, "capacity",    &error), !error))
		return capacity;

	error_free (error);
	if ((now      = read_number (dir, "charge_now",  &error), !error)
	 && (full     = read_number (dir, "charge_full", &error), !error))
		return now / full * 100 + 0.5;

	error_free (error);
	return -1;
}

static char *
read_battery_status (int dir, char **type)
{
	// We present errors to the user, don't fill up the session's log.
	struct error *error = NULL;
	struct str s = str_make ();

	// Dell is being unreasonable and seems to set charge_now
	// to charge_full_design when the battery is fully charged
	int charge = read_battery_charge (dir);
	if (charge >= 0 && charge <= 100)
		str_append_printf (&s, "%u%%", charge);

	char *status = NULL;
	char *model_name = read_value (dir, "model_name", NULL);
	if (model_name)
	{
		model_name[strcspn (model_name, " ")] = 0;
		cstr_set (type, model_name);
	}
	else if ((status = read_value (dir, "status", &error), !error))
	{
		str_append_printf (&s, " (%s)", status);
		free (status);
	}
	else
	{
		str_append_printf (&s, " (%s)", strerror (errno));
		error_free (error);
	}
	return str_steal (&s);
}

static char *
try_power_supply (int dir, struct error **e)
{
	char *type;
	struct error *error = NULL;
	if (!(type = read_value (dir, "type", &error)))
	{
		error_propagate (e, error);
		return NULL;
	}

	bool offline = !read_number (dir, "online", &error);
	if (error)
	{
		error_free (error);
		error = NULL;
	}
	else if (offline)
		return NULL;

	bool is_relevant =
		!strcmp (type, "Battery") ||
		!strcmp (type, "USB") ||
		!strcmp (type, "UPS");

	char *result = NULL;
	if (is_relevant)
	{
		char *status = read_battery_status (dir, &type);
		if (status)
			result = xstrdup_printf ("%s %s", type, status);
		free (status);
	}
	free (type);
	return result;
}

static char *
make_battery_status (void)
{
	DIR *power_supply = opendir ("/sys/class/power_supply");
	if (!power_supply)
	{
		print_debug ("cannot access %s: %s: %s",
			"/sys/class/power_supply", "opendir", strerror (errno));
		return NULL;
	}

	struct dirent *entry;
	struct strv batteries = strv_make ();
	while ((entry = readdir (power_supply)))
	{
		const char *device_name = entry->d_name;
		if (device_name[0] == '.')
			continue;

		int dir = openat (dirfd (power_supply), device_name, O_RDONLY);
		if (dir < 0)
		{
			print_error ("%s: %s: %s", device_name, "openat", strerror (errno));
			continue;
		}

		struct error *error = NULL;
		char *status = try_power_supply (dir, &error);
		close (dir);

		if (status)
			strv_append_owned (&batteries, status);
		if (error)
		{
			print_error ("%s: %s", device_name, error->message);
			error_free (error);
		}
	}
	closedir (power_supply);

	char *result = batteries.len ? strv_join (&batteries, " ") : NULL;
	strv_free (&batteries);
	return result;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static char *
make_time_status (const char *fmt)
{
	char buf[129] = "";
	time_t now = time (NULL);
	struct tm *local = localtime (&now);

	if (local == NULL)
		exit_fatal ("%s: %s", "localtime", strerror (errno));
	if (!strftime (buf, sizeof buf, fmt, local))
		exit_fatal ("strftime == 0");

	return xstrdup (buf);
}

#define VOLUME_PERCENT(x) (((x) * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM)

static char *
make_volume_status (struct app_context *ctx)
{
	if (!ctx->sink_volume.channels)
		return xstrdup ("");

	struct str s = str_make ();
	if (ctx->sink_muted)
		str_append (&s, "Muted ");

	str_append_printf (&s,
		"%u%%", VOLUME_PERCENT (ctx->sink_volume.values[0]));
	if (!pa_cvolume_channels_equal_to
		(&ctx->sink_volume, ctx->sink_volume.values[0]))
	{
		for (size_t i = 1; i < ctx->sink_volume.channels; i++)
			str_append_printf (&s, " / %u%%",
				VOLUME_PERCENT (ctx->sink_volume.values[i]));
	}
	return str_steal (&s);
}

static char *
make_noise_status (struct app_context *ctx)
{
	int diff = difftime (ctx->noise_end_time, time (NULL));
	return xstrdup_printf ("\x01" "Playing noise" "\x01 (%d:%02d)",
		diff / 3600, diff / 60 % 60);
}

static void
refresh_status (struct app_context *ctx)
{
	if (ctx->mpd_stopped)   ctx->backend->add (ctx->backend, "MPD stopped");
	else if (ctx->mpd_song) ctx->backend->add (ctx->backend, ctx->mpd_song);

	if (ctx->noise_end_time)
	{
		char *noise = make_noise_status (ctx);
		ctx->backend->add (ctx->backend, noise);
		free (noise);
	}

	if (ctx->failed)        ctx->backend->add (ctx->backend, "PA failure");
	else
	{
		char *volumes = make_volume_status (ctx);
		ctx->backend->add (ctx->backend, volumes);
		free (volumes);
	}

	char *battery = make_battery_status ();
	if (battery)            ctx->backend->add (ctx->backend, battery);
	free (battery);

	if (ctx->nut_status)    ctx->backend->add (ctx->backend, ctx->nut_status);
	if (ctx->layout)        ctx->backend->add (ctx->backend, ctx->layout);

	if (ctx->insomnia_info)
		ctx->backend->add (ctx->backend, ctx->insomnia_info);

	for (size_t i = 0; i < ctx->command_current.len; i++)
		ctx->backend->add (ctx->backend, ctx->command_current.vector[i]);

	char *times = make_time_status ("Week %V, %a %d %b %Y %H:%M %Z");
	ctx->backend->add (ctx->backend, times);
	free (times);

	ctx->backend->flush (ctx->backend);
}

static void
on_time_changed (void *user_data)
{
	struct app_context *ctx = user_data;
	refresh_status (ctx);

	const time_t now = time (NULL);
	const time_t next = (now / 60 + 1) * 60;
	poller_timer_set (&ctx->time_changed, (next - now) * 1000);
}

static void
on_refresh_rest (void *user_data)
{
	struct app_context *ctx = user_data;

	// We cannot use poll() on most sysfs entries, including battery charge

	refresh_status (ctx);
	poller_timer_set (&ctx->refresh_rest, 5000);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
suspend (struct app_context *ctx)
{
	DBusMessage *msg = dbus_message_new_method_call
		("org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "Suspend");
	hard_assert (msg != NULL);

	dbus_bool_t interactive = false;
	hard_assert (dbus_message_append_args (msg,
		DBUS_TYPE_BOOLEAN, &interactive,
		DBUS_TYPE_INVALID));

	DBusError err = DBUS_ERROR_INIT;
	DBusMessage *reply = dbus_connection_send_with_reply_and_block
		(ctx->system_bus, msg, 1000, &err);
	dbus_message_unref (msg);
	if (!reply)
	{
		print_error ("%s: %s", "suspend", err.message);
		dbus_error_free (&err);
	}
	else
		dbus_message_unref (reply);
}

static void
set_idle_alarm (struct app_context *ctx,
	XSyncAlarm *alarm, XSyncTestType test, XSyncValue value)
{
	XSyncAlarmAttributes attr;
	attr.trigger.counter = ctx->idle_counter;
	attr.trigger.test_type = test;
	attr.trigger.wait_value = value;
	XSyncIntToValue (&attr.delta, 0);

	long flags = XSyncCACounter | XSyncCATestType | XSyncCAValue | XSyncCADelta;
	if (*alarm)
		XSyncChangeAlarm (ctx->dpy, *alarm, flags, &attr);
	else
		*alarm = XSyncCreateAlarm (ctx->dpy, flags, &attr);
}

static void
on_x_alarm_notify (struct app_context *ctx, XSyncAlarmNotifyEvent *ev)
{
	if (ev->alarm == ctx->idle_alarm_inactive)
	{
		// Our own lock doesn't matter, we have to check it ourselves
		if (ctx->system_bus && ctx->insomnia_fd == -1)
			suspend (ctx);

		XSyncValue one, minus_one;
		XSyncIntToValue (&one, 1);

		Bool overflow;
		XSyncValueSubtract (&minus_one, ev->counter_value, one, &overflow);

		// Set an alarm for IDLETIME <= current_idletime - 1
		set_idle_alarm (ctx, &ctx->idle_alarm_active,
			XSyncNegativeComparison, minus_one);
	}
	else if (ev->alarm == ctx->idle_alarm_active)
		// XXX: even though it doesn't seem to run during the time the system
		//   is suspended, I haven't found any place where it is specified
		set_idle_alarm (ctx, &ctx->idle_alarm_inactive,
			XSyncPositiveComparison, ctx->idle_timeout);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
command_queue_start (struct app_context *ctx)
{
	poller_timer_set (&ctx->command_start, 30 * 1000);
}

static void
on_command_ready (const struct pollfd *pfd, void *user_data)
{
	struct app_context *ctx = user_data;
	struct str *buf = &ctx->command_buffer;
	enum socket_io_result result = socket_io_try_read (pfd->fd, buf);
	bool data_have_changed = false;

	size_t end = 0;
	for (size_t i = 0; i + 1 < buf->len; i++)
	{
		if (buf->str[i] != '\n' || buf->str[i + 1] != '\n')
			continue;

		buf->str[i + 1] = '\0';
		strv_reset (&ctx->command_current);
		cstr_split (buf->str + end, "\n", true, &ctx->command_current);
		end = i + 2;
		data_have_changed = true;
	}
	str_remove_slice (buf, 0, end);

	if (result != SOCKET_IO_OK)
	{
		// The pipe may have been closed independently
		if (ctx->command_pid != -1)
			(void) kill (ctx->command_pid, SIGTERM);

		poller_fd_reset (&ctx->command_event);
		xclose (ctx->command_fd);
		ctx->command_fd = -1;
		ctx->command_pid = -1;

		// Make it obvious that something's not right here
		strv_reset (&ctx->command_current);
		data_have_changed = true;

		print_error ("external command failed");
		command_queue_start (ctx);
	}
	if (data_have_changed)
		refresh_status (ctx);
}

static void
on_command_start (void *user_data)
{
	struct app_context *ctx = user_data;
	const char *command =
		get_config_string (ctx->config.root, "general.command");
	if (!command)
		return;

	int output_pipe[2];
	if (pipe (output_pipe))
	{
		print_error ("%s: %s", "pipe", strerror (errno));
		command_queue_start (ctx);
		return;
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init (&actions);
	posix_spawn_file_actions_adddup2
		(&actions, output_pipe[PIPE_WRITE], STDOUT_FILENO);
	posix_spawn_file_actions_addclose (&actions, output_pipe[PIPE_READ]);
	posix_spawn_file_actions_addclose (&actions, output_pipe[PIPE_WRITE]);

	pid_t pid = -1;
	char *argv[] = { "sh", "-c", (char *) command, NULL };
	int result = posix_spawnp (&pid, argv[0], &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy (&actions);

	set_blocking (output_pipe[PIPE_READ], false);
	set_cloexec (output_pipe[PIPE_READ]);
	xclose (output_pipe[PIPE_WRITE]);

	if (result)
	{
		xclose (output_pipe[PIPE_READ]);
		print_error ("%s: %s", "posix_spawnp", strerror (result));
		command_queue_start (ctx);
		return;
	}

	ctx->command_pid = pid;
	str_reset (&ctx->command_buffer);

	ctx->command_event = poller_fd_make (&ctx->poller,
		(ctx->command_fd = output_pipe[PIPE_READ]));
	ctx->command_event.dispatcher = on_command_ready;
	ctx->command_event.user_data = ctx;
	poller_fd_set (&ctx->command_event, POLLIN);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Sometimes it's not that easy and there can be repeating entries
static void
mpd_vector_to_map (const struct strv *data, struct str_map *map)
{
	*map = str_map_make (free);
	map->key_xfrm = tolower_ascii_strxfrm;

	char *key, *value;
	for (size_t i = 0; i < data->len; i++)
	{
		if ((key = mpd_client_parse_kv (data->vector[i], &value)))
			str_map_set (map, key, xstrdup (value));
		else
			print_debug ("%s: %s", "erroneous MPD output", data->vector[i]);
	}
}

static void
mpd_on_info_response (const struct mpd_response *response,
	const struct strv *data, void *user_data)
{
	if (!response->success)
	{
		print_debug ("%s: %s",
			"retrieving MPD info failed", response->message_text);
		return;
	}

	struct app_context *ctx = user_data;
	struct str_map map;
	mpd_vector_to_map (data, &map);

	struct str s = str_make ();
	ctx->mpd_stopped = false;

	const char *value;
	if ((value = str_map_find (&map, "state")))
	{
		// Unicode approximates since in proportional fonts ASCII looks ugly
		// and I don't want to depend on a particular font with player chars
		if (!strcmp (value, "stop"))
			ctx->mpd_stopped = true;
		else if (!strcmp (value, "pause"))
			str_append (&s, "▯▯ " /* "|| " */);
		else
			str_append (&s, "▷ "  /* "|> " */);
	}

	if ((value = str_map_find (&map, "title"))
	 || (value = str_map_find (&map, "name"))
	 || (value = str_map_find (&map, "file")))
		str_append_printf (&s, "\001%s\001", value);
	if ((value = str_map_find (&map, "artist")))
		str_append_printf (&s, " by \001%s\001", value);
	if ((value = str_map_find (&map, "album")))
		str_append_printf (&s, " from \001%s\001", value);

	cstr_set (&ctx->mpd_song, str_steal (&s));

	refresh_status (ctx);
	str_map_free (&map);
}

static void
mpd_request_info (struct app_context *ctx)
{
	struct mpd_client *c = &ctx->mpd_client;

	mpd_client_list_begin (c);
	mpd_client_send_command (c, "currentsong", NULL);
	mpd_client_send_command (c, "status", NULL);
	mpd_client_list_end (c);
	mpd_client_add_task (c, mpd_on_info_response, ctx);

	mpd_client_idle (c, 0);
}

static void
mpd_on_events (unsigned subsystems, void *user_data)
{
	struct app_context *ctx = user_data;
	struct mpd_client *c = &ctx->mpd_client;

	if (subsystems & (MPD_SUBSYSTEM_PLAYER | MPD_SUBSYSTEM_PLAYLIST))
		mpd_request_info (ctx);
	else
		mpd_client_idle (c, 0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_queue_reconnect (struct app_context *ctx)
{
	poller_timer_set (&ctx->mpd_reconnect, 30 * 1000);
}

static void
mpd_on_password_response (const struct mpd_response *response,
	const struct strv *data, void *user_data)
{
	(void) data;
	struct app_context *ctx = user_data;
	struct mpd_client *c = &ctx->mpd_client;

	if (response->success)
		mpd_request_info (ctx);
	else
	{
		print_error ("%s: %s",
			"couldn't authenticate to MPD", response->message_text);
		mpd_client_send_command (c, "close", NULL);
	}
}

static void
mpd_on_connected (void *user_data)
{
	struct app_context *ctx = user_data;
	struct mpd_client *c = &ctx->mpd_client;

	const char *password = get_config_string (ctx->config.root, "mpd.password");
	if (password)
	{
		mpd_client_send_command (c, "password", password, NULL);
		mpd_client_add_task (c, mpd_on_password_response, ctx);
	}
	else
		mpd_request_info (ctx);
}

static void
mpd_on_failure (void *user_data)
{
	// This is also triggered both by a failed connect and a clean disconnect
	struct app_context *ctx = user_data;
	print_error ("connection to MPD failed");
	mpd_queue_reconnect (ctx);

	cstr_set (&ctx->mpd_song, NULL);
	ctx->mpd_stopped = false;
	refresh_status (ctx);
}

static void
mpd_on_io_hook (void *user_data, bool outgoing, const char *line)
{
	(void) user_data;
	if (outgoing)
		print_debug ("MPD << %s", line);
	else
		print_debug ("MPD >> %s", line);
}

static void
on_mpd_reconnect (void *user_data)
{
	// FIXME: the user should be able to disable MPD
	struct app_context *ctx = user_data;

	struct mpd_client *c = &ctx->mpd_client;
	c->user_data    = ctx;
	c->on_failure   = mpd_on_failure;
	c->on_connected = mpd_on_connected;
	c->on_event     = mpd_on_events;
	c->on_io_hook   = mpd_on_io_hook;

	struct error *e = NULL;
	struct config_item *root = ctx->config.root;
	if (!mpd_client_connect (&ctx->mpd_client,
		get_config_string (root, "mpd.address"),
		get_config_string (root, "mpd.service"), &e))
	{
		print_error ("%s: %s", "cannot connect to MPD", e->message);
		error_free (e);
		mpd_queue_reconnect (ctx);
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
nut_common_handler (const struct nut_response *response)
{
	if (response->success)
		return true;

	print_error ("%s: %s", "retrieving NUT info failed", response->message);
	return false;
}

static void
nut_translate_status (const char *status, struct strv *out)
{
	// https://github.com/networkupstools/nut/blob/master/clients/status.h
	if (!strcmp (status, "OL"))      strv_append (out, "on-line");
	if (!strcmp (status, "OB"))      strv_append (out, "on battery");
	if (!strcmp (status, "LB"))      strv_append (out, "low battery");
	if (!strcmp (status, "RB"))      strv_append (out, "replace battery");
	if (!strcmp (status, "CHRG"))    strv_append (out, "charging");
	if (!strcmp (status, "DISCHRG")) strv_append (out, "discharging");
	if (!strcmp (status, "OVER"))    strv_append (out, "overload");
	if (!strcmp (status, "OFF"))     strv_append (out, "off");
	if (!strcmp (status, "TRIM"))    strv_append (out, "voltage trim");
	if (!strcmp (status, "BOOST"))   strv_append (out, "voltage boost");
	if (!strcmp (status, "BYPASS"))  strv_append (out, "bypass");
}

static char *
interval_string (unsigned long seconds)
{
	unsigned long hours = seconds / 3600; seconds %= 3600;
	unsigned long mins  = seconds /   60; seconds %=   60;
	return xstrdup_printf ("%lu:%02lu:%02lu", hours, mins, seconds);
}

static void
nut_process_ups (struct app_context *ctx, struct strv *ups_list,
	const char *ups_name, struct str_map *dict)
{
	// Not currently interested in this kind of information;
	// maybe if someone had more than one UPS installed
	(void) ups_name;

	// http://www.networkupstools.org/docs/developer-guide.chunked/apas01.html
	const char *status  = str_map_find (dict, "ups.status");
	const char *charge  = str_map_find (dict, "battery.charge");
	const char *runtime = str_map_find (dict, "battery.runtime");
	const char *load    = str_map_find (dict, "ups.load");
	const char *power   = str_map_find (dict, "ups.realpower.nominal");

	if (!soft_assert (status && charge && runtime))
		return;

	unsigned long runtime_sec;
	if (!soft_assert (xstrtoul (&runtime_sec, runtime, 10)))
		return;

	struct strv items = strv_make ();
	bool running_on_batteries = false;

	struct strv v = strv_make ();
	cstr_split (status, " ", true, &v);
	for (size_t i = 0; i < v.len; i++)
	{
		const char *status = v.vector[i];
		nut_translate_status (status, &items);

		if (!strcmp (status, "OB"))
			running_on_batteries = true;
	}
	strv_free (&v);

	if (running_on_batteries || strcmp (charge, "100"))
		strv_append_owned (&items, xstrdup_printf ("%s%%", charge));
	if (running_on_batteries)
		strv_append_owned (&items, interval_string (runtime_sec));

	// Only show load if it's higher than the threshold so as to not distract
	struct config_item *root = ctx->config.root;
	const int64_t *threshold = get_config_integer (root, "nut.load_thld");
	const int64_t *fallback  = get_config_integer (root, "nut.load_power");
	unsigned long load_n, power_n;
	if (load
	 && xstrtoul (&load_n, load, 10)
	 && load_n >= (unsigned long) *threshold)
	{
		struct str item = str_make ();
		str_append_printf (&item, "load %s%%", load);

		// Approximation of how much electricity the perpihery actually uses.
		// Use fallback if NUT cannot tell it correctly for whatever reason.
		if (power && xstrtoul (&power_n, power, 10))
			str_append_printf (&item,
				" (~%luW)", power_n * load_n / 100);
		else if (fallback && *fallback >= 0)
			str_append_printf (&item,
				" (~%luW)", (unsigned long) *fallback * load_n / 100);

		strv_append_owned (&items, str_steal (&item));
	}

	struct str result = str_make ();
	str_append (&result, "UPS: ");
	for (size_t i = 0; i < items.len; i++)
	{
		if (i) str_append (&result, "; ");
		str_append (&result, items.vector[i]);
	}
	strv_free (&items);
	strv_append_owned (ups_list, str_steal (&result));
}

static void
nut_on_logout_response (const struct nut_response *response, void *user_data)
{
	if (!nut_common_handler (response))
		return;

	struct app_context *ctx = user_data;
	struct strv ups_list = strv_make ();

	struct str_map_iter iter = str_map_iter_make (&ctx->nut_ups_info);
	struct str_map *dict;
	while ((dict = str_map_iter_next (&iter)))
		nut_process_ups (ctx, &ups_list, iter.link->key, dict);

	cstr_set (&ctx->nut_status, NULL);

	if (ups_list.len)
	{
		struct str status = str_make ();
		str_append (&status, ups_list.vector[0]);
		for (size_t i = 1; i < ups_list.len; i++)
			str_append_printf (&status, "   %s", ups_list.vector[0]);
		ctx->nut_status = str_steal (&status);
	}

	ctx->nut_success = true;
	strv_free (&ups_list);
	refresh_status (ctx);
}

static void
nut_store_var (struct app_context *ctx,
	const char *ups_name, const char *key, const char *value)
{
	struct str_map *map;
	if (!(map = str_map_find (&ctx->nut_ups_info, ups_name)))
	{
		map = xmalloc (sizeof *map);
		*map = str_map_make (free);
		str_map_set (&ctx->nut_ups_info, ups_name, map);
	}

	str_map_set (map, key, xstrdup (value));
}

static void
nut_on_var_response (const struct nut_response *response, void *user_data)
{
	if (!nut_common_handler (response))
		return;

	struct app_context *ctx = user_data;
	LIST_FOR_EACH (struct nut_line, iter, response->data)
	{
		const struct strv *fields = &iter->fields;
		if (!soft_assert (fields->len >= 4
		 && !strcmp (fields->vector[0], "VAR")))
			continue;

		nut_store_var (ctx, fields->vector[1],
			fields->vector[2], fields->vector[3]);
	}
}

static void
nut_on_list_ups_response (const struct nut_response *response, void *user_data)
{
	if (!nut_common_handler (response))
		return;

	struct app_context *ctx = user_data;
	struct nut_client *c = &ctx->nut_client;

	// Then we list all their properties and terminate the connection
	LIST_FOR_EACH (struct nut_line, iter, response->data)
	{
		const struct strv *fields = &iter->fields;
		if (!soft_assert (fields->len >= 2
		 && !strcmp (fields->vector[0], "UPS")))
			continue;

		nut_client_send_command (c, "LIST", "VAR", fields->vector[1], NULL);
		nut_client_add_task (c, nut_on_var_response, ctx);
	}

	nut_client_send_command (c, "LOGOUT", NULL);
	nut_client_add_task (c, nut_on_logout_response, ctx);
}

static void
nut_on_connected (void *user_data)
{
	struct app_context *ctx = user_data;
	struct nut_client *c = &ctx->nut_client;

	// First we list all available UPS devices
	nut_client_send_command (c, "LIST", "UPS", NULL);
	nut_client_add_task (c, nut_on_list_ups_response, ctx);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
nut_indicate_failure (struct app_context *ctx)
{
	cstr_set (&ctx->nut_status, xstrdup ("NUT failure"));

	refresh_status (ctx);
}

static void
nut_on_failure (void *user_data)
{
	struct app_context *ctx = user_data;

	// This is also triggered both by a failed connect and a clean disconnect
	if (!ctx->nut_success)
	{
		print_error ("connection to NUT failed");
		nut_indicate_failure (ctx);
	}
}

static void
on_nut_reconnect (void *user_data)
{
	struct app_context *ctx = user_data;
	if (!*get_config_boolean (ctx->config.root, "nut.enabled"))
		return;

	struct nut_client *c = &ctx->nut_client;
	c->user_data    = ctx;
	c->on_failure   = nut_on_failure;
	c->on_connected = nut_on_connected;

	// So that we don't have to maintain a separate timeout timer,
	// we keep a simple periodic reconnect timer
	if (c->state != NUT_DISCONNECTED)
	{
		print_error ("failed to retrieve NUT status within the interval");
		nut_indicate_failure (ctx);
		nut_client_reset (c);
	}

	str_map_clear (&ctx->nut_ups_info);

	nut_client_connect (&ctx->nut_client, "localhost", "3493");
	ctx->nut_success = false;
	poller_timer_set (&ctx->nut_reconnect, 10 * 1000);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static inline float
noise_next_brownian (float last)
{
	// Leaky integrators have a side effect on the signal, making noise white
	// on the lower end of the spectrum, which can be heard as reduced rumbling
	while (1)
	{
		// 0.9375 is the guaranteed to be safe value, not very pleasant
		float f = last * 0.99 + ((double) rand () / RAND_MAX - 0.5) / 8;
		if (f >= -1 && f <= 1)
			return f;
	}
}

static void
noise_generate_stereo (struct app_context *ctx, int16_t *data, size_t n)
{
	float brown_l = ctx->noise_state[0];
	float brown_r = ctx->noise_state[1];

	for (size_t i = 0; i < n / 2; i++)
	{
		// We do not want to use a linear transition, and a decreasing geometric
		// sequence would have a limit in infinity, so use powers of normalized
		// time deltas--in particular 2 up to 6 are said to work
		float gain = 1;
		if (ctx->noise_fadeout_samples)
		{
			float remaining = (float) (ctx->noise_fadeout_samples
				- ctx->noise_fadeout_iterator++) / ctx->noise_fadeout_samples;
			if (remaining <= 0)
				gain = 0;
			else
				gain = remaining * remaining;
		}

		data[i * 2 + 0] =
			(brown_l = noise_next_brownian (brown_l)) * gain * INT16_MAX;
		data[i * 2 + 1] =
			(brown_r = noise_next_brownian (brown_r)) * gain * INT16_MAX;
	}

	ctx->noise_state[0] = brown_l;
	ctx->noise_state[1] = brown_r;
}

static void
noise_abort (struct app_context *ctx)
{
	ctx->noise_end_time = 0;
	poller_timer_reset (&ctx->noise_timer);

	if (ctx->noise_stream)
	{
		(void) pa_stream_disconnect (ctx->noise_stream);
		pa_stream_unref (ctx->noise_stream);
		ctx->noise_stream = NULL;
	}
}

static void
on_noise_writeable (pa_stream *stream, size_t nbytes, void *userdata)
{
	struct app_context *ctx = userdata;
	int16_t data[nbytes / 2];
	noise_generate_stereo (ctx, data, N_ELEMENTS (data));

	int err;
	if ((err = pa_stream_write (stream,
		data, sizeof data, NULL, 0, PA_SEEK_RELATIVE)))
	{
		print_error ("noise playback failed: %s", pa_strerror (err));
		noise_abort (ctx);
	}
}

static const pa_sample_spec noise_default_spec =
{
	.channels = 2,
	.format = BYTE_ORDER == LITTLE_ENDIAN ? PA_SAMPLE_S16LE : PA_SAMPLE_S16BE,
	.rate = 48000,
};

static bool
noise_start (struct app_context *ctx)
{
	if (!ctx->context)
	{
		print_error ("not playing noise, not connected to PulseAudio");
		return false;
	}

	// Avoid unnecessary, and fairly CPU-intensive resampling
	pa_sample_spec spec = noise_default_spec;
	if (ctx->sink_sample_spec.rate == 44100)
		spec.rate = ctx->sink_sample_spec.rate;

	ctx->noise_stream =
		pa_stream_new (ctx->context, PROGRAM_NAME "/noise", &spec, NULL);
	pa_stream_set_write_callback (ctx->noise_stream, on_noise_writeable, ctx);

	int err;
	if ((err = pa_stream_connect_playback (ctx->noise_stream,
		NULL, NULL, 0, NULL, NULL)))
	{
		print_error ("failed to connect noise playback stream: %s",
			pa_strerror (err));
		noise_abort (ctx);
		return false;
	}

	time (&ctx->noise_end_time);
	ctx->noise_state[0] = ctx->noise_state[1] = 0;
	ctx->noise_fadeout_samples = 0;
	ctx->noise_fadeout_iterator = 0;
	return true;
}

static void
on_noise_timer (void *user_data)
{
	struct app_context *ctx = user_data;
	int diff = difftime (ctx->noise_end_time, time (NULL));
	if (diff <= 0)
		noise_abort (ctx);
	else
	{
		poller_timer_set (&ctx->noise_timer, (diff % 60 + 1) * 1000);

		// XXX: this is inaccurate, since we don't take into account buffering,
		//   however it shouldn't pose a major issue
		if (diff <= 60 && !ctx->noise_fadeout_samples)
			ctx->noise_fadeout_samples =
				diff * pa_stream_get_sample_spec (ctx->noise_stream)->rate;
	}

	refresh_status (ctx);
}

static void
action_noise_adjust (struct app_context *ctx, const struct strv *args)
{
	if (args->len != 1)
	{
		print_error ("usage: noise-adjust +/-HOURS");
		return;
	}

	long arg = strtol (args->vector[0], NULL, 10);
	ctx->noise_fadeout_samples = 0;
	ctx->noise_fadeout_iterator = 0;
	if (!ctx->noise_end_time && (arg < 0 || !noise_start (ctx)))
		return;

	time_t now = time (NULL);
	int diff = difftime (ctx->noise_end_time, now);

	// The granularity of noise playback setting is whole hours.
	enum { SECOND = 1, MINUTE = 60, HOUR = 3600 };
	if (arg > 0)
		// Add a minute to enable stepping up from 0:59 to 2:00.
		diff = (diff + arg * HOUR + MINUTE) / HOUR * HOUR;
	else if (arg++ < 0)
		// Remove a second to enable stepping down from 2:00 to 1:00.
		diff = (diff + arg * HOUR - SECOND) / HOUR * HOUR;

	ctx->noise_end_time = now + diff;
	on_noise_timer (ctx);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#define DEFAULT_SOURCE "@DEFAULT_SOURCE@"
#define DEFAULT_SINK   "@DEFAULT_SINK@"

static void
on_sink_info (pa_context *context, const pa_sink_info *info, int eol,
	void *userdata)
{
	(void) context;

	if (info && !eol)
	{
		struct app_context *ctx = userdata;
		ctx->sink_sample_spec = info->sample_spec;
		ctx->sink_volume = info->volume;
		ctx->sink_muted = !!info->mute;

		strv_reset (&ctx->sink_ports);
		cstr_set (&ctx->sink_port_active, NULL);

		if (info->ports)
			for (struct pa_sink_port_info **iter = info->ports; *iter; iter++)
				strv_append (&ctx->sink_ports, (*iter)->name);
		if (info->active_port)
			ctx->sink_port_active = xstrdup (info->active_port->name);

		refresh_status (ctx);
	}
}

static void
on_source_info (pa_context *context, const pa_source_info *info, int eol,
	void *userdata)
{
	(void) context;

	if (info && !eol)
	{
		struct app_context *ctx = userdata;
		ctx->source_muted = !!info->mute;
	}
}

static void
update_volume (struct app_context *ctx)
{
	pa_operation_unref (pa_context_get_sink_info_by_name
		(ctx->context, DEFAULT_SINK, on_sink_info, ctx));
	pa_operation_unref (pa_context_get_source_info_by_name
		(ctx->context, DEFAULT_SOURCE, on_source_info, ctx));
}

static void
on_event (pa_context *context, pa_subscription_event_type_t event,
	uint32_t index, void *userdata)
{
	(void) context;
	(void) index;

	struct app_context *ctx = userdata;
	if ((event & PA_SUBSCRIPTION_EVENT_TYPE_MASK)
		== PA_SUBSCRIPTION_EVENT_CHANGE)
		update_volume (ctx);
}

static void
on_subscribe_finish (pa_context *context, int success, void *userdata)
{
	(void) context;

	struct app_context *ctx = userdata;
	if (!success)
	{
		ctx->failed = true;
		refresh_status (ctx);
	}
}

static void
on_context_state_change (pa_context *context, void *userdata)
{
	struct app_context *ctx = userdata;
	switch (pa_context_get_state (context))
	{
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
		// The stream depends on the context, and would keep its object alive
		noise_abort (ctx);

		ctx->failed = true;
		refresh_status (ctx);

		pa_context_unref (context);
		ctx->context = NULL;

		// Retry after an arbitrary delay of 5 seconds
		poller_timer_set (&ctx->make_context, 5000);
		return;
	case PA_CONTEXT_READY:
		ctx->failed = false;
		refresh_status (ctx);

		pa_context_set_subscribe_callback (context, on_event, userdata);
		pa_operation_unref (pa_context_subscribe (context,
			PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE,
			on_subscribe_finish, userdata));
		update_volume (ctx);
	default:
		return;
	}
}

static void
on_make_context (void *user_data)
{
	struct app_context *ctx = user_data;
	ctx->context = pa_context_new (ctx->api, PROGRAM_NAME);
	pa_context_set_state_callback (ctx->context, on_context_state_change, ctx);
	pa_context_connect (ctx->context, NULL, PA_CONTEXT_NOFLAGS, NULL);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
spawn (char *argv[])
{
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init (&actions);

	// That would mess up our JSON
	posix_spawn_file_actions_addopen
		(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);

	posix_spawnp (NULL, argv[0], &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy (&actions);
}

static void
action_exec (struct app_context *ctx, const struct strv *args)
{
	(void) ctx;
	spawn (args->vector);
}

static void
action_mpd (struct app_context *ctx, const struct strv *args)
{
	struct mpd_client *c = &ctx->mpd_client;
	if (c->state != MPD_CONNECTED)
		return;
	mpd_client_send_commandv (c, args->vector);
	mpd_client_add_task (c, NULL, NULL);
	mpd_client_idle (c, 0);
}

static void
action_mpd_play_toggle (struct app_context *ctx, const struct strv *args)
{
	(void) args;
	struct mpd_client *c = &ctx->mpd_client;
	if (c->state != MPD_CONNECTED)
		return;
	mpd_client_send_command (c, ctx->mpd_stopped ? "play" : "pause", NULL);
	mpd_client_add_task (c, NULL, NULL);
	mpd_client_idle (c, 0);
}

static void
on_volume_finish (pa_context *context, int success, void *userdata)
{
	(void) context;
	(void) success;
	(void) userdata;

	// Just like... whatever, man
}

static void
action_audio_mic_mute (struct app_context *ctx, const struct strv *args)
{
	(void) args;

	if (!ctx->context)
		return;

	pa_operation_unref (pa_context_set_source_mute_by_name (ctx->context,
		DEFAULT_SOURCE, !ctx->source_muted, on_volume_finish, ctx));
}

static void
action_audio_switch (struct app_context *ctx, const struct strv *args)
{
	(void) args;

	if (!ctx->context || !ctx->sink_port_active || !ctx->sink_ports.len)
		return;

	size_t current = 0;
	for (size_t i = 0; i < ctx->sink_ports.len; i++)
		if (!strcmp (ctx->sink_port_active, ctx->sink_ports.vector[i]))
			current = i;

	pa_operation_unref (pa_context_set_sink_port_by_name (ctx->context,
		DEFAULT_SINK,
		ctx->sink_ports.vector[(current + 1) % ctx->sink_ports.len],
		on_volume_finish, ctx));
}

static void
action_audio_mute (struct app_context *ctx, const struct strv *args)
{
	(void) args;

	if (!ctx->context)
		return;

	pa_operation_unref (pa_context_set_sink_mute_by_name (ctx->context,
		DEFAULT_SINK, !ctx->sink_muted, on_volume_finish, ctx));
}

static void
action_audio_volume (struct app_context *ctx, const struct strv *args)
{
	if (args->len != 1)
	{
		print_error ("usage: audio-volume +/-PERCENT");
		return;
	}
	if (!ctx->context)
		return;

	long arg = strtol (args->vector[0], NULL, 10);
	pa_cvolume volume = ctx->sink_volume;
	if (arg > 0)
		pa_cvolume_inc (&volume, (pa_volume_t) +arg * PA_VOLUME_NORM / 100);
	else
		pa_cvolume_dec (&volume, (pa_volume_t) -arg * PA_VOLUME_NORM / 100);
	pa_operation_unref (pa_context_set_sink_volume_by_name (ctx->context,
		DEFAULT_SINK, &volume, on_volume_finish, ctx));
}

static void
go_insomniac (struct app_context *ctx)
{
	static const char *what = "sleep:idle";
	static const char *who = PROGRAM_NAME;
	static const char *why = "";
	static const char *mode = "block";

	DBusMessage *msg = dbus_message_new_method_call
		("org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "Inhibit");
	hard_assert (msg != NULL);

	hard_assert (dbus_message_append_args (msg,
		DBUS_TYPE_STRING, &what,
		DBUS_TYPE_STRING, &who,
		DBUS_TYPE_STRING, &why,
		DBUS_TYPE_STRING, &mode,
		DBUS_TYPE_INVALID));

	DBusError err = DBUS_ERROR_INIT;
	DBusMessage *reply = dbus_connection_send_with_reply_and_block
		(ctx->system_bus, msg, 1000, &err);
	dbus_message_unref (msg);
	if (!reply)
	{
		ctx->insomnia_info = xstrdup_printf ("%s: %s", "Insomnia", err.message);
		dbus_error_free (&err);
	}
	else if (!dbus_message_get_args (reply, &err,
		DBUS_TYPE_UNIX_FD, &ctx->insomnia_fd, DBUS_TYPE_INVALID))
	{
		dbus_message_unref (reply);
		ctx->insomnia_info = xstrdup_printf ("%s: %s", "Insomnia", err.message);
		dbus_error_free (&err);
	}
	else
	{
		dbus_message_unref (reply);
		ctx->insomnia_info = xstrdup ("Insomniac");
		set_cloexec (ctx->insomnia_fd);
	}
}

static void
action_insomnia (struct app_context *ctx, const struct strv *args)
{
	(void) args;
	cstr_set (&ctx->insomnia_info, NULL);

	// Get rid of the lock if we hold one, establish it otherwise
	if (ctx->insomnia_fd != -1)
	{
		xclose (ctx->insomnia_fd);
		ctx->insomnia_fd = -1;
	}
	else if (ctx->system_bus)
		go_insomniac (ctx);

	refresh_status (ctx);
}

static void
action_xkb_lock_group (struct app_context *ctx, const struct strv *args)
{
	if (args->len != 1)
	{
		print_error ("usage: xkb-lock-group GROUP");
		return;
	}

	long group = strtol (args->vector[0], NULL, 10) - 1;
	if (group < XkbGroup1Index || group > XkbGroup4Index)
		print_warning ("invalid XKB group index: %s", args->vector[0]);
	else
		XkbLockGroup (ctx->dpy, XkbUseCoreKbd, group);
}

static const struct action
{
	const char *name;
	void (*handler) (struct app_context *ctx, const struct strv *args);
}
g_handlers[] =
{
	{ "exec",            action_exec            },
	{ "mpd",             action_mpd             },
	{ "mpd-play-toggle", action_mpd_play_toggle },
	{ "xkb-lock-group",  action_xkb_lock_group  },
	{ "insomnia",        action_insomnia        },
	{ "audio-switch",    action_audio_switch    },
	{ "audio-mute",      action_audio_mute      },
	{ "audio-mic-mute",  action_audio_mic_mute  },
	{ "audio-volume",    action_audio_volume    },
	{ "noise-adjust",    action_noise_adjust    },
};

struct binding
{
	LIST_HEADER (struct binding)

	unsigned mods;                      ///< Modifiers
	KeyCode keycode;                    ///< Key code
	struct action handler;              ///< Handling procedure
	struct strv args;                   ///< Arguments to the handler
};

static struct action
action_by_name (const char *name)
{
	for (size_t i = 0; i < N_ELEMENTS (g_handlers); i++)
		if (!strcmp (g_handlers[i].name, name))
			return g_handlers[i];
	return (struct action) {};
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
on_x_keypress (struct app_context *ctx, XEvent *e)
{
	XKeyEvent *ev = &e->xkey;
	LIST_FOR_EACH (struct binding, iter, ctx->bindings)
		if (iter->keycode == ev->keycode
		 && iter->mods == ev->state
		 && iter->handler.handler)
			iter->handler.handler (ctx, &iter->args);
}

static void
on_xkb_event (struct app_context *ctx, XkbEvent *ev)
{
	int group;
	if (ev->any.xkb_type == XkbStateNotify)
		group = ev->state.group;
	else
	{
		XkbStateRec rec;
		XkbGetState (ctx->dpy, XkbUseCoreKbd, &rec);
		group = rec.group;
	}

	XkbDescPtr desc = XkbAllocKeyboard ();
	XkbGetNames (ctx->dpy, XkbGroupNamesMask, desc);

	cstr_set (&ctx->layout, NULL);

	if (group != 0)
	{
		char *layout = XGetAtomName (ctx->dpy, desc->names->groups[group]);
		ctx->layout = xstrdup (layout);
		XFree (layout);
	}

	XkbFreeKeyboard (desc, 0, True);
	refresh_status (ctx);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
on_x_ready (const struct pollfd *pfd, void *user_data)
{
	(void) pfd;
	struct app_context *ctx = user_data;

	XkbEvent ev;
	while (XPending (ctx->dpy))
	{
		if (XNextEvent (ctx->dpy, &ev.core))
			exit_fatal ("XNextEvent returned non-zero");
		if (ev.type == KeyPress)
			on_x_keypress (ctx, &ev.core);
		else if (ev.type == ctx->xkb_base_event_code)
			on_xkb_event (ctx, &ev);
		else if (ctx->xsync_base_event_code
			&& ev.type == ctx->xsync_base_event_code + XSyncAlarmNotify)
			on_x_alarm_notify (ctx, (XSyncAlarmNotifyEvent *) &ev);
	}
}

static bool
parse_key_modifier (const char *modifier, unsigned *mods)
{
	static const struct
	{
		const char *name;
		unsigned mask;
	}
	modifiers[] =
	{
		{"Shift",   ShiftMask},
		{"Lock",    LockMask},
		{"Control", ControlMask},
		{"Mod1",    Mod1Mask},
		{"Mod2",    Mod2Mask},
		{"Mod3",    Mod3Mask},
		{"Mod4",    Mod4Mask},
		{"Mod5",    Mod5Mask},
	};

	for (size_t k = 0; k < N_ELEMENTS (modifiers); k++)
		if (!strcasecmp_ascii (modifiers[k].name, modifier))
		{
			*mods |= modifiers[k].mask;
			return true;
		}
	return false;
}

static bool
parse_key_vector (const struct strv *keys, unsigned *mods, KeySym *keysym)
{
	for (size_t i = 0; i < keys->len; i++)
	{
		if (parse_key_modifier (keys->vector[i], mods))
			continue;
		if (*keysym)
			return false;
		*keysym = XStringToKeysym (keys->vector[i]);
	}
	return *keysym != 0;
}

static bool
parse_key_combination (const char *combination, unsigned *mods, KeySym *keysym)
{
	struct strv keys = strv_make ();
	bool result = parse_binding (combination, &keys)
		&& parse_key_vector (&keys, mods, keysym);
	strv_free (&keys);
	return result;
}

static const char *
init_grab (struct app_context *ctx, const char *combination, const char *action)
{
	unsigned mods = 0;
	KeySym keysym = 0;
	if (!parse_key_combination (combination, &mods, &keysym))
		return "parsing key combination failed";

	KeyCode keycode = XKeysymToKeycode (ctx->dpy, keysym);
	if (!keycode)
		return "no keycode found";

	struct strv args = strv_make ();
	if (!parse_binding (action, &args) || !args.len)
	{
		strv_free (&args);
		return "parsing the binding failed";
	}

	struct action handler = action_by_name (args.vector[0]);
	free (strv_steal (&args, 0));
	if (!handler.name)
	{
		strv_free (&args);
		return "unknown action";
	}

	XGrabKey (ctx->dpy, keycode, mods, DefaultRootWindow (ctx->dpy),
		 False /* ? */, GrabModeAsync, GrabModeAsync);

	struct binding *binding = xcalloc (1, sizeof *binding);
	binding->mods = mods;
	binding->keycode = keycode;
	binding->handler = handler;
	binding->args = args;
	LIST_PREPEND (ctx->bindings, binding);
	return NULL;
}

static void
init_bindings (struct app_context *ctx)
{
	unsigned ignored_locks =
		LockMask | XkbKeysymToModifiers (ctx->dpy, XK_Num_Lock);
	hard_assert (XkbSetIgnoreLockMods
		(ctx->dpy, XkbUseCoreKbd, ignored_locks, ignored_locks, 0, 0));

	struct str_map *keys =
		&config_item_get (ctx->config.root, "keys", NULL)->value.object;
	struct str_map_iter iter = str_map_iter_make (keys);

	struct config_item *action;
	while ((action = str_map_iter_next (&iter)))
	{
		const char *combination = iter.link->key, *err = NULL;
		if (action->type != CONFIG_ITEM_NULL)
		{
			if (action->type != CONFIG_ITEM_STRING)
				err = "expected a string";
			else
				err = init_grab (ctx, combination, action->value.string.str);
		}
		if (err)
			print_warning ("configuration: key `%s': %s", combination, err);
	}

	XSelectInput (ctx->dpy, DefaultRootWindow (ctx->dpy), KeyPressMask);
}

static void
init_xlib_events (struct app_context *ctx)
{
	const int64_t *sleep_timer =
		get_config_integer (ctx->config.root, "general.sleep_timer");
	if (sleep_timer && ctx->idle_counter)
	{
		if (*sleep_timer <= 0 || *sleep_timer > INT_MAX / 1000)
			exit_fatal ("invalid value for the sleep timer");
		XSyncIntToValue (&ctx->idle_timeout, *sleep_timer * 1000);
		set_idle_alarm (ctx, &ctx->idle_alarm_inactive,
			XSyncPositiveComparison, ctx->idle_timeout);
	}

	init_bindings (ctx);
	XSync (ctx->dpy, False);

	ctx->x_event.dispatcher = on_x_ready;
	ctx->x_event.user_data = ctx;
	poller_fd_set (&ctx->x_event, POLLIN);

	// XXX: XkbMapNotify -> XkbRefreshKeyboardMapping(), ...?
	XkbSelectEventDetails (ctx->dpy, XkbUseCoreKbd, XkbNamesNotify,
		XkbAllNamesMask, XkbGroupNamesMask);
	XkbSelectEventDetails (ctx->dpy, XkbUseCoreKbd, XkbStateNotify,
		XkbAllStateComponentsMask, XkbGroupStateMask);
}

// --- IPC ---------------------------------------------------------------------

#define IPC_SOCKET "ipc.socket"

static void
on_ipc_message (struct app_context *ctx, const char *message, size_t len)
{
	struct action handler = action_by_name (message);
	if (!handler.handler)
	{
		print_error ("ipc: %s: %s", "unknown action", message);
		return;
	}

	struct strv args = strv_make ();
	const char *p = memchr (message, 0, len);
	while (p)
	{
		strv_append (&args, ++p);
		p = memchr (p, 0, len - (p - message));
	}

	handler.handler (ctx, &args);
	strv_free (&args);
}

static void
on_ipc_ready (const struct pollfd *pfd, void *user_data)
{
	struct app_context *ctx = user_data;
	char buf[65536] = {};

	while (true)
	{
		ssize_t len = read (pfd->fd, buf, sizeof buf - 1 /* NUL-terminated */);
		if (len >= 0)
		{
			buf[len] = 0;
			on_ipc_message (ctx, buf, len);
		}
		else if (errno == EAGAIN)
			return;
		else if (errno != EINTR)
			print_warning ("ipc: %s: %s", "read", strerror (errno));

	}
}

static void
app_setup_ipc (struct app_context *ctx)
{
	int fd = socket (AF_UNIX, SOCK_DGRAM, 0);
	if (fd == -1)
	{
		print_error ("ipc: %s: %s", "socket", strerror (errno));
		return;
	}

	set_cloexec (fd);
	char *path = resolve_relative_runtime_filename (IPC_SOCKET);

	// This is unfortunately the only way to prevent EADDRINUSE.
	unlink (path);

	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	strncpy (sa.sun_path, path, sizeof sa.sun_path - 1);
	if (bind (fd, (struct sockaddr *) &sa, sizeof sa))
	{
		print_error ("ipc: %s: %s", path, strerror (errno));
		xclose (fd);
		goto out;
	}

	set_blocking (fd, false);
	ctx->ipc_fd = fd;
	ctx->ipc_event = poller_fd_make (&ctx->poller, fd);
	ctx->ipc_event.dispatcher = on_ipc_ready;
	ctx->ipc_event.user_data = ctx;
	poller_fd_set (&ctx->ipc_event, POLLIN);
out:
	free (path);
}

static int
ipc_send (int argc, char *argv[])
{
	int fd = socket (AF_UNIX, SOCK_DGRAM, 0);
	if (fd == -1)
		print_fatal ("ipc: %s: %s", "socket", strerror (errno));

	struct str message = str_make ();
	for (int i = 0; i < argc; i++)
	{
		if (i > 0)
			str_append_c (&message, 0);
		str_append (&message, argv[i]);
	}

	char *path = resolve_relative_runtime_filename (IPC_SOCKET);
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	strncpy (sa.sun_path, path, sizeof sa.sun_path - 1);

	int result = EXIT_FAILURE;
	ssize_t sent = sendto (fd, message.str, message.len, 0,
		(struct sockaddr *) &sa, sizeof sa);
	if (sent < 0)
		print_error ("ipc: %s: %s", path, strerror (errno));
	else if (sent != (ssize_t) message.len)
		print_error ("ipc: %s: %s", path, "incomplete message sent");
	else
		result = 0;

	free (path);
	str_free (&message);
	return result;
}

// --- Configuration -----------------------------------------------------------

static void
app_load_configuration (struct app_context *ctx)
{
	char *filename = resolve_filename
		(PROGRAM_NAME ".conf", resolve_relative_config_filename);
	if (!filename)
		return;

	struct error *e = NULL;
	struct config_item *root = config_read_from_file (filename, &e);
	free (filename);

	if (e)
		exit_fatal ("error loading configuration: %s", e->message);

	if (root)
	{
		config_load (&ctx->config, root);
		config_schema_call_changed (ctx->config.root);
	}
}

static void
app_save_configuration (struct app_context *ctx, const char *path_hint)
{
	static const char *prolog =
		"# " PROGRAM_NAME " " PROGRAM_VERSION " configuration file\n\n";

	struct str data = str_make ();
	str_append (&data, prolog);
	config_item_write (ctx->config.root, true, &data);

	struct error *e = NULL;
	char *filename = write_configuration_file (path_hint, &data, &e);
	str_free (&data);

	if (!filename)
	{
		print_error ("%s", e->message);
		error_free (e);
		exit (EXIT_FAILURE);
	}
	print_status ("configuration written to `%s'", filename);
	free (filename);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
sway_command_argument_needs_quoting (const char *word)
{
	while (*word)
		if (!isalnum_ascii (*word++))
			return true;
	return false;
}

static void
sway_append_command_argument (struct str *out, const char *word)
{
	if (out->len)
		str_append_c (out, ' ');

	if (!sway_command_argument_needs_quoting (word))
	{
		str_append (out, word);
		return;
	}

	str_append_c (out, '\'');
	for (const char *p = word; *p; p++)
	{
		if (*p == '\'' || *p == '\\')
			str_append_c (out, '\\');
		str_append_c (out, *p);
	}
	str_append_c (out, '\'');
}

static const char *
sway_bindsym (const char *combination, const char *action)
{
	const char *error = NULL;
	struct strv keys = strv_make ();
	struct strv args = strv_make ();
	if (!parse_binding (combination, &keys))
	{
		error = "parsing key combination failed";
		goto out;
	}
	if (!parse_binding (action, &args) || !args.len)
	{
		error = "parsing the binding failed";
		goto out;
	}

	struct action handler = action_by_name (args.vector[0]);
	if (!handler.name)
	{
		error = "unknown action";
		goto out;
	}

	// The i3/Sway quoting is properly fucked up,
	// and its exec command forwards to `sh -c`.
	struct str shell_command = str_make ();
	if (strcmp (handler.name, "exec"))
	{
		// argv[0] would need realpath() applied on it.
		shell_quote (PROGRAM_NAME, &shell_command);
		str_append (&shell_command, " -- ");
		shell_quote (handler.name, &shell_command);
		str_append_c (&shell_command, ' ');
	}
	for (size_t i = 1; i < args.len; i++)
	{
		shell_quote (args.vector[i], &shell_command);
		str_append_c (&shell_command, ' ');
	}
	if (shell_command.len)
		shell_command.str[--shell_command.len] = 0;

	// This command name may not be quoted.
	// Note that i3-msg doesn't accept bindsym at all, only swaymsg does.
	struct str sway_command = str_make ();
	sway_append_command_argument (&sway_command, "bindsym");
	char *recombined = strv_join (&keys, "+");
	sway_append_command_argument (&sway_command, recombined);
	free (recombined);
	sway_append_command_argument (&sway_command, "exec");
	sway_append_command_argument (&sway_command, shell_command.str);
	str_free (&shell_command);

	struct strv argv = strv_make ();
	strv_append (&argv, "swaymsg");
	strv_append_owned (&argv, str_steal (&sway_command));

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init (&actions);
	posix_spawnp (NULL, argv.vector[0], &actions, NULL, argv.vector, environ);
	posix_spawn_file_actions_destroy (&actions);

	strv_free (&argv);
out:
	strv_free (&keys);
	strv_free (&args);
	return error;
}

static void
sway_forward_bindings (void)
{
	// app_context_init() has side-effects.
	struct app_context ctx = { .config = app_make_config () };
	app_load_configuration (&ctx);

	struct str_map *keys =
		&config_item_get (ctx.config.root, "keys", NULL)->value.object;
	struct str_map_iter iter = str_map_iter_make (keys);

	struct config_item *action;
	while ((action = str_map_iter_next (&iter)))
	{
		const char *combination = iter.link->key, *err = NULL;
		if (action->type != CONFIG_ITEM_NULL)
		{
			if (action->type != CONFIG_ITEM_STRING)
				err = "expected a string";
			else
				err = sway_bindsym (combination, action->value.string.str);
		}
		if (err)
			print_warning ("configuration: key `%s': %s", combination, err);
	}
}

// --- Signals -----------------------------------------------------------------

static int g_signal_pipe[2];            ///< A pipe used to signal... signals
static struct poller_fd g_signal_event; ///< Signal pipe is readable

static void
on_sigchld (int sig)
{
	(void) sig;

	int original_errno = errno;
	if (write (g_signal_pipe[PIPE_WRITE], "c", 1) == -1)
		soft_assert (errno == EAGAIN);
	errno = original_errno;
}

static void
on_signal_pipe_readable (const struct pollfd *pfd, struct app_context *ctx)
{
	char dummy;
	(void) read (pfd->fd, &dummy, 1);

	pid_t zombie;
	while ((zombie = waitpid (-1, NULL, WNOHANG)))
	{
		// We want to know when this happens so that we don't accidentally
		// try to kill an unrelated process on cleanup
		if (ctx->command_pid == zombie)
			ctx->command_pid = -1;
		if (zombie == -1 && errno == ECHILD)
			return;
		if (zombie == -1)
			hard_assert (errno == EINTR);
	}
}

static void
setup_signal_handlers (struct app_context *ctx)
{
	if (pipe (g_signal_pipe) == -1)
		exit_fatal ("%s: %s", "pipe", strerror (errno));

	set_cloexec (g_signal_pipe[PIPE_READ]);
	set_cloexec (g_signal_pipe[PIPE_WRITE]);

	// So that the pipe cannot overflow; it would make write() block within
	// the signal handler, which is something we really don't want to happen.
	// The same holds true for read().
	set_blocking (g_signal_pipe[PIPE_READ],  false);
	set_blocking (g_signal_pipe[PIPE_WRITE], false);

	struct sigaction sa;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset (&sa.sa_mask);
	sa.sa_handler = on_sigchld;
	if (sigaction (SIGCHLD, &sa, NULL) == -1)
		print_error ("%s: %s", "sigaction", strerror (errno));

	g_signal_event = poller_fd_make (&ctx->poller, g_signal_pipe[PIPE_READ]);
	g_signal_event.dispatcher = (poller_fd_fn) on_signal_pipe_readable;
	g_signal_event.user_data = ctx;
	poller_fd_set (&g_signal_event, POLLIN);
}

// --- Initialisation, event handling ------------------------------------------

static void
poller_timer_init_and_set (struct poller_timer *self, struct poller *poller,
	poller_timer_fn cb, void *user_data)
{
	*self = poller_timer_make (poller);
	self->dispatcher = cb;
	self->user_data = user_data;

	poller_timer_set (self, 0);
}

int
main (int argc, char *argv[])
{
	g_log_message_real = log_message_custom;

	static const struct opt opts[] =
	{
		{ 'd', "debug", NULL, 0, "run in debug mode" },
		{ 'h', "help", NULL, 0, "display this help and exit" },
		{ 'V', "version", NULL, 0, "output version information and exit" },
		{ '3', "i3bar", NULL, 0, "print output for i3-bar/swaybar instead" },
		{ 's', "bind-sway", NULL, 0, "import bindings over swaymsg" },
		{ 'w', "write-default-cfg", "FILENAME",
		  OPT_OPTIONAL_ARG | OPT_LONG_ONLY,
		  "write a default configuration file and exit" },
		{ 0, NULL, NULL, 0, NULL }
	};

	struct opt_handler oh = opt_handler_make (argc, argv, opts, "[ACTION...]",
		"Set root window name.");
	bool i3bar = false;

	int c;
	while ((c = opt_handler_get (&oh)) != -1)
	switch (c)
	{
	case 'd':
		g_debug_mode = true;
		break;
	case 'h':
		opt_handler_usage (&oh, stdout);
		exit (EXIT_SUCCESS);
	case 'V':
		printf (PROGRAM_NAME " " PROGRAM_VERSION "\n");
		exit (EXIT_SUCCESS);
	case '3':
		i3bar = true;
		break;
	case 's':
		sway_forward_bindings ();
		exit (EXIT_SUCCESS);
	case 'w':
	{
		// app_context_init() has side-effects.
		struct app_context ctx = { .config = app_make_config () };
		app_save_configuration (&ctx, optarg);
		exit (EXIT_SUCCESS);
	}
	default:
		print_error ("wrong options");
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}

	argc -= optind;
	argv += optind;

	opt_handler_free (&oh);
	if (argc > 0)
		return ipc_send (argc, argv);

	struct app_context ctx;
	app_context_init (&ctx);
	app_load_configuration (&ctx);
	app_setup_ipc (&ctx);
	setup_signal_handlers (&ctx);

	poller_timer_init_and_set (&ctx.time_changed, &ctx.poller,
		on_time_changed, &ctx);
	poller_timer_init_and_set (&ctx.make_context, &ctx.poller,
		on_make_context, &ctx);
	poller_timer_init_and_set (&ctx.refresh_rest, &ctx.poller,
		on_refresh_rest, &ctx);
	poller_timer_init_and_set (&ctx.command_start, &ctx.poller,
		on_command_start, &ctx);
	poller_timer_init_and_set (&ctx.mpd_reconnect, &ctx.poller,
		on_mpd_reconnect, &ctx);
	poller_timer_init_and_set (&ctx.nut_reconnect, &ctx.poller,
		on_nut_reconnect, &ctx);
	poller_timer_init_and_set (&ctx.noise_timer, &ctx.poller,
		on_noise_timer, &ctx);

	init_xlib_events (&ctx);

	if (i3bar)
		ctx.backend = backend_i3_new ();
	else
		ctx.backend = backend_dwm_new (ctx.dpy);

	if (ctx.backend->start)
		ctx.backend->start (ctx.backend);
	while (true)
		poller_run (&ctx.poller);
	if (ctx.backend->stop)
		ctx.backend->stop (ctx.backend);

	// We never get here since we don't even handle termination signals
	app_context_free (&ctx);
	return 0;
}
