/*
 * paswitch.c: simple PulseAudio device switcher
 *
 * module-switch-on-connect functionality without the on-connect part.
 *
 * Copyright (c) 2015 - 2018, Přemysl Eric Janouch <p@janouch.name>
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

#define _GNU_SOURCE

#include "config.h"
#undef PROGRAM_NAME
#define PROGRAM_NAME "paswitch"
#include "liberty/liberty.c"
#include "liberty/liberty-pulse.c"

#include <locale.h>
#include <wchar.h>

#include <langinfo.h>
#include <termios.h>
#include <sys/ioctl.h>

#include <pulse/mainloop.h>
#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/subscribe.h>

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
	fputs ("\r\n", stream);
}

// --- Application -------------------------------------------------------------

struct port
{
	LIST_HEADER (struct port)

	char *name;                         ///< Name of the port
	char *description;                  ///< Description of the port
	pa_port_available_t available;      ///< Availability
};

static void
port_free (struct port *self)
{
	cstr_set (&self->name, NULL);
	cstr_set (&self->description, NULL);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct sink
{
	LIST_HEADER (struct sink)

	char *name;                         ///< Name of the sink
	char *description;                  ///< Description of the sink
	uint32_t index;                     ///< Index of the sink
	bool muted;                         ///< Currently muted?
	pa_cvolume volume;                  ///< Current volume
	struct port *ports;                 ///< All sink ports
	size_t ports_len;                   ///< The number of ports
	char *port_active;                  ///< Active sink port
};

static struct sink *
sink_new (void)
{
	struct sink *self = xcalloc (1, sizeof *self);
	return self;
}

static void
sink_destroy (struct sink *self)
{
	cstr_set (&self->name, NULL);
	cstr_set (&self->description, NULL);

	for (size_t i = 0; i < self->ports_len; i++)
		port_free (self->ports + i);
	free (self->ports);

	cstr_set (&self->port_active, NULL);
	free (self);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct sink_input
{
	LIST_HEADER (struct sink_input)

	uint32_t index;                     ///< Index of the sink input
	uint32_t sink;                      ///< Index of the connected sink
};

static struct sink_input *
sink_input_new (void)
{
	struct sink_input *self = xcalloc (1, sizeof *self);
	self->index = self->sink = PA_INVALID_INDEX;
	return self;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct app_context
{
	struct poller poller;               ///< Poller
	struct poller_idle redraw_event;    ///< Redraw the terminal
	struct poller_timer make_context;   ///< Start PulseAudio communication

	struct poller_fd tty_event;         ///< Terminal input event
	struct poller_timer tty_timer;      ///< Terminal input timeout
	struct str tty_input_buffer;        ///< Buffered terminal input

	bool quitting;                      ///< Quitting requested
	pa_mainloop_api *api;               ///< PulseAudio event loop proxy
	pa_context *context;                ///< PulseAudio connection context

	bool failed;                        ///< General PulseAudio failure
	bool reset_sinks;                   ///< Flag for info callback
	bool reset_inputs;                  ///< Flag for info callback

	char *default_sink;                 ///< Name of the default sink
	struct sink *sinks;                 ///< PulseAudio sinks
	struct sink *sinks_tail;            ///< Tail of PulseAudio sinks
	struct sink_input *inputs;          ///< PulseAudio sink inputs
	struct sink_input *inputs_tail;     ///< Tail of PulseAudio sink inputs

	uint32_t selected_sink;             ///< Selected sink index (PA)
	ssize_t selected_port;              ///< Selected port index (local)
};

static void
app_context_init (struct app_context *self)
{
	memset (self, 0, sizeof *self);

	poller_init (&self->poller);
	self->tty_input_buffer = str_make ();
	self->api = poller_pa_new (&self->poller);
	self->selected_sink = PA_INVALID_INDEX;
	self->selected_port = -1;
}

static void
app_context_free (struct app_context *self)
{
	if (self->context)
		pa_context_unref (self->context);

	cstr_set (&self->default_sink, NULL);
	LIST_FOR_EACH (struct sink, iter, self->sinks)
		sink_destroy (iter);
	LIST_FOR_EACH (struct sink_input, iter, self->inputs)
		free (iter);

	poller_pa_destroy (self->api);
	str_free (&self->tty_input_buffer);
	poller_free (&self->poller);
}

static struct sink *
current_sink (struct app_context *ctx)
{
	LIST_FOR_EACH (struct sink, iter, ctx->sinks)
		if (iter->index == ctx->selected_sink)
			return iter;
	return NULL;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#define VOLUME_PERCENT(x) (((x) * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM)

static char *
make_volume_status (struct sink *sink)
{
	if (!sink->volume.channels)
		return xstrdup ("");

	struct str s = str_make ();
	if (sink->muted)
		str_append (&s, "Muted ");

	str_append_printf (&s,
		"%u%%", VOLUME_PERCENT (sink->volume.values[0]));
	if (!pa_cvolume_channels_equal_to
		(&sink->volume, sink->volume.values[0]))
	{
		for (size_t i = 1; i < sink->volume.channels; i++)
			str_append_printf (&s, " / %u%%",
				VOLUME_PERCENT (sink->volume.values[i]));
	}
	return str_steal (&s);
}

static char *
make_inputs_status (struct app_context *ctx, struct sink *sink)
{
	int n = 0;
	LIST_FOR_EACH (struct sink_input, input, ctx->inputs)
		if (input->sink == sink->index)
			n++;

	if (n == 0)  return NULL;
	if (n == 1)  return xstrdup_printf ("1 input");
	return xstrdup_printf ("%d inputs", n);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
forget_sinks (struct app_context *ctx)
{
	LIST_FOR_EACH (struct sink, iter, ctx->sinks)
		sink_destroy (iter);
	ctx->sinks = ctx->sinks_tail = NULL;
}

static void
on_sink_info (pa_context *context, const pa_sink_info *info, int eol,
	void *userdata)
{
	(void) context;
	struct app_context *ctx = userdata;

	// Assuming replies cannot overlap
	if (ctx->reset_sinks)
	{
		forget_sinks (ctx);
		ctx->reset_sinks = false;
	}
	if (!info || eol)
	{
		struct sink *sink = current_sink (ctx);
		if (!sink && ctx->sinks)
		{
			ctx->selected_sink = ctx->sinks->index;
			ctx->selected_port = -1;
		}
		else if (sink && ctx->selected_port >= (ssize_t) sink->ports_len)
			ctx->selected_port = -1;

		poller_idle_set (&ctx->redraw_event);
		ctx->reset_sinks = true;
		return;
	}

	struct sink *sink = sink_new ();
	sink->name = xstrdup (info->name);
	sink->description = xstrdup (info->description);
	sink->index = info->index;
	sink->muted = !!info->mute;
	sink->volume = info->volume;

	if (info->ports)
	{
		for (struct pa_sink_port_info **iter = info->ports; *iter; iter++)
			sink->ports_len++;

		struct port *port = sink->ports =
			xcalloc (sink->ports_len, sizeof *sink->ports);
		for (struct pa_sink_port_info **iter = info->ports; *iter; iter++)
		{
			port->name = xstrdup ((*iter)->name);
			port->description = xstrdup ((*iter)->description);
			port->available = (*iter)->available;
			port++;
		}
	}
	if (info->active_port)
		sink->port_active = xstrdup (info->active_port->name);

	LIST_APPEND_WITH_TAIL (ctx->sinks, ctx->sinks_tail, sink);
}

static void
update_sinks (struct app_context *ctx)
{
	pa_operation_unref (pa_context_get_sink_info_list
		(ctx->context, on_sink_info, ctx));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
forget_sink_inputs (struct app_context *ctx)
{
	LIST_FOR_EACH (struct sink_input, iter, ctx->inputs)
		free (iter);
	ctx->inputs = ctx->inputs_tail = NULL;
}

static void
on_sink_input_info (pa_context *context, const struct pa_sink_input_info *info,
	int eol, void *userdata)
{
	(void) context;
	struct app_context *ctx = userdata;

	// Assuming replies cannot overlap
	if (ctx->reset_inputs)
	{
		forget_sink_inputs (ctx);
		ctx->reset_inputs = false;
	}
	if (!info || eol)
	{
		poller_idle_set (&ctx->redraw_event);
		ctx->reset_inputs = true;
		return;
	}

	struct sink_input *input = sink_input_new ();
	input->sink = info->sink;
	input->index = info->index;
	LIST_APPEND_WITH_TAIL (ctx->inputs, ctx->inputs_tail, input);
}

static void
update_sink_inputs (struct app_context *ctx)
{
	pa_operation_unref (pa_context_get_sink_input_info_list
		(ctx->context, on_sink_input_info, ctx));
}

static void
on_server_info (pa_context *context, const struct pa_server_info *info,
	void *userdata)
{
	(void) context;

	struct app_context *ctx = userdata;
	if (info->default_sink_name)
		ctx->default_sink = xstrdup (info->default_sink_name);
	else
		cstr_set (&ctx->default_sink, NULL);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
update_server_info (struct app_context *ctx)
{
	pa_operation_unref (pa_context_get_server_info (ctx->context,
		on_server_info, ctx));
}

static void
on_event (pa_context *context, pa_subscription_event_type_t event,
	uint32_t index, void *userdata)
{
	(void) context;
	(void) index;

	struct app_context *ctx = userdata;
	switch (event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK)
	{
	case PA_SUBSCRIPTION_EVENT_SINK:
		update_sinks (ctx);
		break;
	case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
		update_sink_inputs (ctx);
		break;
	case PA_SUBSCRIPTION_EVENT_SERVER:
		update_server_info (ctx);
	}
}

static void
on_subscribe_finish (pa_context *context, int success, void *userdata)
{
	(void) context;

	struct app_context *ctx = userdata;
	if (!success)
	{
		ctx->failed = true;
		poller_idle_set (&ctx->redraw_event);
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
		ctx->failed = true;
		poller_idle_set (&ctx->redraw_event);

		pa_context_unref (context);
		ctx->context = NULL;

		forget_sinks (ctx);
		forget_sink_inputs (ctx);
		cstr_set (&ctx->default_sink, NULL);

		// Retry after an arbitrary delay of 5 seconds
		poller_timer_set (&ctx->make_context, 5000);
		return;

	case PA_CONTEXT_READY:
		ctx->failed = false;
		poller_idle_set (&ctx->redraw_event);

		pa_context_set_subscribe_callback (context, on_event, userdata);
		pa_operation_unref (pa_context_subscribe (context,
			PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SINK_INPUT |
			PA_SUBSCRIPTION_MASK_SERVER, on_subscribe_finish, userdata));

		ctx->reset_sinks = true;
		ctx->reset_inputs = true;

		update_sinks (ctx);
		update_sink_inputs (ctx);
		update_server_info (ctx);
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
on_pa_finish (pa_context *context, int success, void *userdata)
{
	(void) context;
	(void) success;
	(void) userdata;

	// Just like... whatever, man
}

static void
sink_switch_port (struct app_context *ctx, struct sink *sink, size_t i)
{
	if (!ctx->context || !sink->port_active || !sink->ports)
		return;

	size_t current = 0;
	for (size_t i = 0; i < sink->ports_len; i++)
		if (!strcmp (sink->port_active, sink->ports[i].name))
			current = i;

	if (current != i)
	{
		pa_operation_unref (pa_context_set_sink_port_by_name (ctx->context,
			sink->name, sink->ports[(current + 1) % sink->ports_len].name,
			on_pa_finish, ctx));
	}
}

static void
sink_mute (struct app_context *ctx, struct sink *sink)
{
	if (!ctx->context)
		return;

	pa_operation_unref (pa_context_set_sink_mute_by_name (ctx->context,
		sink->name, !sink->muted, on_pa_finish, ctx));
}

static void
sink_set_volume (struct app_context *ctx, struct sink *sink, int diff)
{
	if (!ctx->context)
		return;

	pa_cvolume volume = sink->volume;
	if (diff > 0)
		pa_cvolume_inc (&volume, (pa_volume_t)  diff * PA_VOLUME_NORM / 100);
	else
		pa_cvolume_dec (&volume, (pa_volume_t) -diff * PA_VOLUME_NORM / 100);
	pa_operation_unref (pa_context_set_sink_volume_by_name (ctx->context,
		sink->name, &volume, on_pa_finish, ctx));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static int g_terminal_lines;
static int g_terminal_columns;

static void
update_screen_size (void)
{
	struct winsize size;
	if (!ioctl (STDOUT_FILENO, TIOCGWINSZ, (char *) &size))
	{
		char *row = getenv ("LINES");
		char *col = getenv ("COLUMNS");
		unsigned long tmp;
		g_terminal_lines =
			(row && xstrtoul (&tmp, row, 10)) ? tmp : size.ws_row;
		g_terminal_columns =
			(col && xstrtoul (&tmp, col, 10)) ? tmp : size.ws_col;
	}
}

static void
on_redraw (struct app_context *ctx)
{
	poller_idle_reset (&ctx->redraw_event);
	update_screen_size ();

	printf ("\x1b[H");   // Cursor to home
	printf ("\x1b[2J");  // Clear the whole screen

	// TODO: see if we can reduce flickering in rxvt-unicode.
	//   Buffering doesn't help, we have to do something more sophisticated.
	// TODO: try not to write more lines than g_terminal_lines for starters
	if (ctx->failed)
	{
		printf ("PulseAudio connection failed, reconnect in progress.\r\n");
		return;
	}

	LIST_FOR_EACH (struct sink, sink, ctx->sinks)
	{
		if (ctx->default_sink && !strcmp (sink->name, ctx->default_sink))
			printf ("\x1b[1m");
		if (sink->index == ctx->selected_sink && ctx->selected_port < 0)
			printf ("\x1b[7m");
		// TODO: erase until end of line with current attributes?

		char *volume = make_volume_status (sink);
		printf ("%s (%s", sink->description, volume);
		free (volume);

		char *inputs = make_inputs_status (ctx, sink);
		if (inputs)  printf (", %s", inputs);
		free (inputs);

		printf (")\x1b[m\r\n");

		for (size_t i = 0; i < sink->ports_len; i++)
		{
			struct port *port = sink->ports + i;
			printf ("  ");

			if (!strcmp (port->name, sink->port_active))
				printf ("\x1b[1m");
			if (sink->index == ctx->selected_sink
			 && ctx->selected_port == (ssize_t) i)
				printf ("\x1b[7m");
			// TODO: erase until end of line with current attributes?

			printf ("%s", port->description);
			if (port->available == PA_PORT_AVAILABLE_YES)
				printf (" (plugged in)");
			else if (port->available == PA_PORT_AVAILABLE_NO)
				printf (" (unplugged)");

			printf ("\x1b[m\r\n");
		}
	}
	fflush (stdout);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

enum action
{
	ACTION_NONE, ACTION_UP, ACTION_DOWN, ACTION_SELECT,
	ACTION_VOLUP, ACTION_VOLDOWN, ACTION_MUTE, ACTION_QUIT
};

static void
on_action (struct app_context *ctx, enum action action)
{
	poller_idle_set (&ctx->redraw_event);

	struct sink *sink = current_sink (ctx);
	switch (action)
	{
	case ACTION_UP:
		if (!sink)
			break;

		if (ctx->selected_port >= 0)
			ctx->selected_port--;
		else if (sink->prev)
		{
			ctx->selected_sink = sink->prev->index;
			ctx->selected_port = sink->prev->ports_len - 1;
		}
		else if (ctx->sinks_tail)
		{
			ctx->selected_sink = ctx->sinks_tail->index;
			ctx->selected_port = ctx->sinks_tail->ports_len - 1;
		}
		break;
	case ACTION_DOWN:
		if (!sink)
			break;

		if (ctx->selected_port + 1 < (ssize_t) sink->ports_len)
			ctx->selected_port++;
		else if (sink->next)
		{
			ctx->selected_sink = sink->next->index;
			ctx->selected_port = -1;
		}
		else if (ctx->sinks)
		{
			ctx->selected_sink = ctx->sinks->index;
			ctx->selected_port = -1;
		}
		break;
	case ACTION_SELECT:
		if (!sink)
			break;

		if (ctx->selected_port != -1)
			sink_switch_port (ctx, sink, ctx->selected_port);

		if (strcmp (ctx->default_sink, sink->name))
		{
			pa_operation_unref (pa_context_set_default_sink (ctx->context,
				sink->name, on_pa_finish, ctx));
		}
		LIST_FOR_EACH (struct sink_input, input, ctx->inputs)
		{
			if (input->sink == sink->index)
				continue;
			pa_operation_unref (pa_context_move_sink_input_by_index
				(ctx->context, input->index, sink->index, on_pa_finish, ctx));
		}
		break;

	case ACTION_VOLUP:
		if (sink)
			sink_set_volume (ctx, sink, 5);
		break;
	case ACTION_VOLDOWN:
		if (sink)
			sink_set_volume (ctx, sink, -5);
		break;
	case ACTION_MUTE:
		if (sink)
			sink_mute (ctx, sink);
		break;

	case ACTION_QUIT:
		ctx->quitting = true;
	case ACTION_NONE:
		break;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static struct key_handler
{
	const char *keyseq;
	enum action action;
}
g_key_handlers[] =
{
	// In local mode, xterm, st, rxvt-unicode and VTE all use these,
	// which copy ANSI/ISO/ECMA codes for cursor movement;
	// we don't enable keypad mode which would break that
	{ "\x1b[A",  ACTION_UP       },
	{ "\x1b[B",  ACTION_DOWN     },

	{ "k",       ACTION_UP       },
	{ "j",       ACTION_DOWN     },
	{ "\x10",    ACTION_UP       },
	{ "\x0e",    ACTION_DOWN     },
	{ "\r",      ACTION_SELECT   },
	{ "+",       ACTION_VOLUP    },
	{ "-",       ACTION_VOLDOWN  },
	{ "\x1b[5~", ACTION_VOLUP    },
	{ "\x1b[6~", ACTION_VOLDOWN  },
	{ "m",       ACTION_MUTE     },
	{ "q",       ACTION_QUIT     },
	{ "\x1b",    ACTION_QUIT     },
	{ NULL,      ACTION_NONE     },
};

static void
handle_key (struct app_context *ctx, const char *keyseq, size_t len)
{
	for (const struct key_handler *i = g_key_handlers; i->keyseq; i++)
		if (strlen (i->keyseq) == len && memcmp (i->keyseq, keyseq, len) == 0)
		{
			on_action (ctx, i->action);
			return;
		}

#if 0
	// Development tool
	for (size_t i = 0; i < len; i++)
	{
		if ((unsigned char) keyseq[i] < 32 || keyseq[i] == 127)
			printf ("^%c", '@' + keyseq[i]);
		else
			putchar (keyseq[i]);
	}
	printf ("\r\n");
#endif
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Match a terminal key sequence roughly following the ABNF syntax below and
/// return its length on a full, unambigious match.  Partial, ambiguous matches
/// are returned as negative numbers.  Returns zero iff "len" is zero.
///
///   match   = alt-key / key
///   alt-key = ESC key
///   key     = csi-seq / ss3-seq / multibyte-character / OCTET
///   csi-seq = ESC '[' *%x30-3F (%x00-2F / %x40-FF)
///   ss3-seq = ESC 'O' OCTET
static int
read_key_sequence (const char *buf, size_t len)
{
	const char *p = buf, *end = buf + len;
	if (p < end && *p == 27)
		p++;
	if (p < end && *p == 27)
		p++;

	int escapes = p - buf;
	if (p == end)
		return -escapes;

	// CSI and SS3 escape sequences are accepted in a very generic format
	// because they don't need to follow ECMA-48 and e.g. urxvt ends shifted
	// keys with $ (an intermediate character) -- best effort
	if (escapes)
	{
		if (*p == '[')
		{
			while (++p < end)
				if (*p < 0x30 || *p > 0x3F)
					return ++p - buf;
			return -escapes;
		}
		if (*p == 'O')
		{
			if (++p < end)
				return ++p - buf;
			return -escapes;
		}
		// We don't know this sequence, so just return M-Esc
		if (escapes == 2)
			return escapes;
	}

	// Shift state encodings aren't going to work, though anything else should
	mbstate_t mb = {};
	int length = mbrlen (p, end - p, &mb);
	if (length == -2)
		return -escapes - 1;
	if (length == -1 || !length)
		return escapes + 1;
	return escapes + length;
}

static void
tty_process_buffer (struct app_context *ctx)
{
	struct str *buf = &ctx->tty_input_buffer;
	const char *p = buf->str, *end = p + buf->len;
	for (int res = 0; (res = read_key_sequence (p, end - p)) > 0; p += res)
		handle_key (ctx, p, res);
	str_remove_slice (buf, 0, p - buf->str);

	poller_timer_reset (&ctx->tty_timer);
	if (buf->len)
		poller_timer_set (&ctx->tty_timer, 100);
}

static void
on_tty_timeout (struct app_context *ctx)
{
	struct str *buf = &ctx->tty_input_buffer;
	int res = abs (read_key_sequence (buf->str, buf->len));
	if (res)
	{
		handle_key (ctx, buf->str, res);
		str_remove_slice (buf, 0, res);
	}

	// The ambiguous sequence may explode into several other sequences
	tty_process_buffer (ctx);
}

static void
on_tty_readable (const struct pollfd *fd, struct app_context *ctx)
{
	if (fd->revents & ~(POLLIN | POLLHUP | POLLERR))
		print_debug ("fd %d: unexpected revents: %d", fd->fd, fd->revents);

	struct str *buf = &ctx->tty_input_buffer;
	str_reserve (buf, 1);

	int res = read (fd->fd, buf->str + buf->len, buf->alloc - buf->len - 1);
	if (res > 0)
	{
		buf->str[buf->len += res] = '\0';
		tty_process_buffer (ctx);
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static struct termios g_saved_termios;

static void
tty_reset (void)
{
	printf ("\x1b[?1049l");  // Exit CA mode (alternate screen)
	printf ("\x1b[?25h");    // Show cursor
	fflush (stdout);

	tcsetattr (STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
}

static bool
tty_start (void)
{
	if (tcgetattr (STDIN_FILENO, &g_saved_termios) < 0)
		return false;

	struct termios request = g_saved_termios, result = {};
	request.c_cc[VMIN] = request.c_cc[VTIME] = 0;
	request.c_lflag &= ~(ECHO | ICANON);
	request.c_iflag &= ~(ICRNL);
	request.c_oflag &= ~(OPOST);

	atexit (tty_reset);
	if (tcsetattr (STDIN_FILENO, TCSAFLUSH, &request) < 0
	 || tcgetattr (STDIN_FILENO, &result) < 0
	 || memcmp (request.c_cc, result.c_cc, sizeof request.c_cc)
	 || request.c_lflag != result.c_lflag
	 || request.c_iflag != result.c_iflag
	 || request.c_oflag != result.c_oflag)
		return false;

	printf ("\x1b[?1049h");  // Enter CA mode (alternate screen)
	printf ("\x1b[?25l");    // Hide cursor
	fflush (stdout);
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static int g_signal_pipe[2];            ///< A pipe used to signal... signals
static struct poller_fd g_signal_event; ///< Signal pipe is readable

static void
on_signal (int sig)
{
	char id = sig;

	// Assuming that the pipe won't normally overflow (16 pages on Linux)
	int original_errno = errno;
	if (write (g_signal_pipe[PIPE_WRITE], &id, 1) == -1)
		soft_assert (errno == EAGAIN);
	errno = original_errno;
}

static void
on_signal_pipe_readable (const struct pollfd *pfd, struct app_context *ctx)
{
	char id = 0;
	(void) read (pfd->fd, &id, 1);

	if (id == SIGINT || id == SIGTERM || id == SIGHUP)
		ctx->quitting = true;
	else if (id == SIGWINCH)
		poller_idle_set (&ctx->redraw_event);
	else
		hard_assert (!"unhandled signal");
}

static void
setup_signal_handlers (struct app_context *ctx)
{
	(void) signal (SIGPIPE, SIG_IGN);

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
	sa.sa_flags = SA_RESTART;
	sigemptyset (&sa.sa_mask);
	sa.sa_handler = on_signal;
	if (sigaction (SIGINT,   &sa, NULL) == -1
	 || sigaction (SIGTERM,  &sa, NULL) == -1
	 || sigaction (SIGHUP,   &sa, NULL) == -1
	 || sigaction (SIGWINCH, &sa, NULL) == -1)
		print_error ("%s: %s", "sigaction", strerror (errno));

	g_signal_event = poller_fd_make (&ctx->poller, g_signal_pipe[PIPE_READ]);
	g_signal_event.dispatcher = (poller_fd_fn) on_signal_pipe_readable;
	g_signal_event.user_data = ctx;
	poller_fd_set (&g_signal_event, POLLIN);
}

static void
poller_timer_init_and_set (struct poller_timer *self, struct poller *poller,
	poller_timer_fn cb, void *user_data)
{
	*self = poller_timer_make (poller);
	self->dispatcher = cb;
	self->user_data = user_data;

	poller_timer_set (self, 0);
}

#ifdef TESTING
static void
test_read_key_sequence (void)
{
	static struct
	{
		const char *buffer;             ///< Terminal input buffer
		int expected;                   ///< Expected parse result
	}
	cases[] =
	{
		{ "",  0 }, { "\x1b[A_", 3 }, { "\x1b\x1b[", -2 },
		{ "Ř", 2 }, { "\x1bOA_", 3 }, { "\x1b\x1bO", -2 },
	};

	setlocale (LC_CTYPE, "");
	for (size_t i = 0; i < N_ELEMENTS (cases); i++)
		hard_assert (read_key_sequence (cases[i].buffer,
			strlen (cases[i].buffer)) == cases[i].expected);
}

int
main (int argc, char *argv[])
{
	struct test test;
	test_init (&test, argc, argv);
	test_add_simple (&test, "/read-key-sequence", NULL, test_read_key_sequence);
	return test_run (&test);
}

#define main main_shadowed
#endif  // TESTING

int
main (int argc, char *argv[])
{
	static const struct opt opts[] =
	{
		{ 'd', "debug", NULL, 0, "run in debug mode" },
		{ 'h', "help", NULL, 0, "display this help and exit" },
		{ 'V', "version", NULL, 0, "output version information and exit" },
		{ 0, NULL, NULL, 0, NULL }
	};

	struct opt_handler oh =
		opt_handler_make (argc, argv, opts, NULL, "Switch PA outputs.");

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
	default:
		print_error ("wrong options");
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}

	argc -= optind;
	argv += optind;

	opt_handler_free (&oh);

	if (!isatty (STDIN_FILENO))
		exit_fatal ("input is not a terminal");
	if (!isatty (STDOUT_FILENO))
		exit_fatal ("output is not a terminal");

	setlocale (LC_CTYPE, "");
	// PulseAudio uses UTF-8, let's avoid encoding conversions
	if (strcasecmp (nl_langinfo (CODESET), "UTF-8"))
		exit_fatal ("UTF-8 encoding required");
	if (setvbuf (stdout, NULL, _IOLBF, 0) || !tty_start ())
		exit_fatal ("terminal initialization failed");

	// TODO: we will need a logging function aware of our rendering
	g_log_message_real = log_message_custom;

	struct app_context ctx;
	app_context_init (&ctx);
	setup_signal_handlers (&ctx);

	ctx.redraw_event = poller_idle_make (&ctx.poller);
	ctx.redraw_event.dispatcher = (poller_idle_fn) on_redraw;
	ctx.redraw_event.user_data = &ctx;
	poller_idle_set (&ctx.redraw_event);

	ctx.tty_event = poller_fd_make (&ctx.poller, STDIN_FILENO);
	ctx.tty_event.dispatcher = (poller_fd_fn) on_tty_readable;
	ctx.tty_event.user_data = &ctx;
	poller_fd_set (&ctx.tty_event, POLLIN);

	ctx.tty_timer = poller_timer_make (&ctx.poller);
	ctx.tty_timer.dispatcher = (poller_timer_fn) on_tty_timeout;
	ctx.tty_timer.user_data = &ctx;

	poller_timer_init_and_set (&ctx.make_context, &ctx.poller,
		on_make_context, &ctx);

	while (!ctx.quitting)
		poller_run (&ctx.poller);

	app_context_free (&ctx);
	return 0;
}
