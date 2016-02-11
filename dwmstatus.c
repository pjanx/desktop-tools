/*
 * dwmstatus.c: simple PulseAudio-enabled dwmstatus
 *
 * Copyright (c) 2015 - 2016, Přemysl Janouch <p.janouch@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
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

#define _GNU_SOURCE // openat

#include "config.h"
#undef PROGRAM_NAME
#define PROGRAM_NAME "dwmstatus"
#include "liberty/liberty.c"

#include <dirent.h>
#include <sys/un.h>
#include <spawn.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <X11/XKBlib.h>

#include <pulse/mainloop.h>
#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/subscribe.h>

// --- Utilities ---------------------------------------------------------------

static void
log_message_custom (void *user_data, const char *quote, const char *fmt,
	va_list ap)
{
	(void) user_data;
	FILE *stream = stdout;

	fprintf (stream, PROGRAM_NAME ": ");
	fputs (quote, stream);
	vfprintf (stream, fmt, ap);
	fputs ("\n", stream);
}

static void
set_dwm_status (Display *dpy, const char *str)
{
	print_debug ("setting status to: %s", str);
	XStoreName (dpy, DefaultRootWindow (dpy), str);
	XSync (dpy, False);
}

// --- Simple network I/O ------------------------------------------------------

enum socket_io_result
{
	SOCKET_IO_OK = 0,                   ///< Completed successfully
	SOCKET_IO_EOF,                      ///< Connection shut down by peer
	SOCKET_IO_ERROR                     ///< Connection error
};

static enum socket_io_result
socket_io_try_read (int socket_fd, struct str *rb)
{
	ssize_t n_read;
	while (true)
	{
		str_ensure_space (rb, 512);
		n_read = recv (socket_fd, rb->str + rb->len,
			rb->alloc - rb->len - 1 /* null byte */, 0);

		if (n_read > 0)
		{
			rb->str[rb->len += n_read] = '\0';
			continue;
		}
		if (n_read == 0)
			return SOCKET_IO_EOF;

		if (errno == EAGAIN)
			return SOCKET_IO_OK;
		if (errno == EINTR)
			continue;

		LOG_LIBC_FAILURE ("recv");
		return SOCKET_IO_ERROR;
	}
}

static enum socket_io_result
socket_io_try_write (int socket_fd, struct str *wb)
{
	ssize_t n_written;
	while (wb->len)
	{
		n_written = send (socket_fd, wb->str, wb->len, 0);
		if (n_written >= 0)
		{
			str_remove_slice (wb, 0, n_written);
			continue;
		}

		if (errno == EAGAIN)
			return SOCKET_IO_OK;
		if (errno == EINTR)
			continue;

		LOG_LIBC_FAILURE ("send");
		return SOCKET_IO_ERROR;
	}
	return SOCKET_IO_OK;
}

// --- PulseAudio mainloop abstraction -----------------------------------------

struct pa_io_event
{
	LIST_HEADER (pa_io_event)

	pa_mainloop_api *api;               ///< Parent structure
	struct poller_fd fd;                ///< Underlying FD event

	pa_io_event_cb_t dispatch;          ///< Dispatcher
	pa_io_event_destroy_cb_t free;      ///< Destroyer
	void *user_data;                    ///< User data
};

struct pa_time_event
{
	LIST_HEADER (pa_time_event)

	pa_mainloop_api *api;               ///< Parent structure
	struct poller_timer timer;          ///< Underlying timer event

	pa_time_event_cb_t dispatch;        ///< Dispatcher
	pa_time_event_destroy_cb_t free;    ///< Destroyer
	void *user_data;                    ///< User data
};

struct pa_defer_event
{
	LIST_HEADER (pa_defer_event)

	pa_mainloop_api *api;               ///< Parent structure
	struct poller_idle idle;            ///< Underlying idle event

	pa_defer_event_cb_t dispatch;       ///< Dispatcher
	pa_defer_event_destroy_cb_t free;   ///< Destroyer
	void *user_data;                    ///< User data
};

struct poller_pa
{
	struct poller *poller;              ///< The underlying event loop
	int result;                         ///< Result on quit
	bool running;                       ///< Not quitting

	pa_io_event *io_list;               ///< I/O events
	pa_time_event *time_list;           ///< Timer events
	pa_defer_event *defer_list;         ///< Deferred events
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static short
poller_pa_flags_to_events (pa_io_event_flags_t flags)
{
	short result = 0;
	if (flags & PA_IO_EVENT_ERROR)   result |= POLLERR;
	if (flags & PA_IO_EVENT_HANGUP)  result |= POLLHUP;
	if (flags & PA_IO_EVENT_INPUT)   result |= POLLIN;
	if (flags & PA_IO_EVENT_OUTPUT)  result |= POLLOUT;
	return result;
}

static pa_io_event_flags_t
poller_pa_events_to_flags (short events)
{
	pa_io_event_flags_t result = 0;
	if (events & POLLERR)  result |= PA_IO_EVENT_ERROR;
	if (events & POLLHUP)  result |= PA_IO_EVENT_HANGUP;
	if (events & POLLIN)   result |= PA_IO_EVENT_INPUT;
	if (events & POLLOUT)  result |= PA_IO_EVENT_OUTPUT;
	return result;
}

static struct timeval
poller_pa_get_current_time (void)
{
	struct timeval tv;
#ifdef _POSIX_TIMERS
	struct timespec tp;
	hard_assert (clock_gettime (CLOCK_REALTIME, &tp) != -1);
	tv.tv_sec = tp.tv_sec;
	tv.tv_usec = tp.tv_nsec / 1000;
#else
	gettimeofday (&tv, NULL);
#endif
	return tv;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
poller_pa_io_dispatcher (const struct pollfd *pfd, void *user_data)
{
	pa_io_event *self = user_data;
	self->dispatch (self->api, self,
		pfd->fd, poller_pa_events_to_flags (pfd->revents), self->user_data);
}

static void
poller_pa_io_enable (pa_io_event *self, pa_io_event_flags_t events)
{
	struct poller_fd *fd = &self->fd;
	if (events)
		poller_fd_set (fd, poller_pa_flags_to_events (events));
	else
		poller_fd_reset (fd);
}

static pa_io_event *
poller_pa_io_new (pa_mainloop_api *api, int fd_, pa_io_event_flags_t events,
	pa_io_event_cb_t cb, void *userdata)
{
	pa_io_event *self = xcalloc (1, sizeof *self);
	self->api = api;
	self->dispatch = cb;
	self->user_data = userdata;

	struct poller_pa *data = api->userdata;
	struct poller_fd *fd = &self->fd;
	poller_fd_init (fd, data->poller, fd_);
	fd->user_data = self;
	fd->dispatcher = poller_pa_io_dispatcher;

	poller_pa_io_enable (self, events);
	LIST_PREPEND (data->io_list, self);
	return self;
}

static void
poller_pa_io_free (pa_io_event *self)
{
	if (self->free)
		self->free (self->api, self, self->user_data);

	struct poller_pa *data = self->api->userdata;
	poller_fd_reset (&self->fd);
	LIST_UNLINK (data->io_list, self);
	free (self);
}

static void
poller_pa_io_set_destroy (pa_io_event *self, pa_io_event_destroy_cb_t cb)
{
	self->free = cb;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
poller_pa_time_dispatcher (void *user_data)
{
	pa_time_event *self = user_data;
	// XXX: the meaning of the time argument is undocumented,
	//   so let's just put current Unix time in there
	struct timeval now = poller_pa_get_current_time ();
	self->dispatch (self->api, self, &now, self->user_data);
}

static void
poller_pa_time_restart (pa_time_event *self, const struct timeval *tv)
{
	struct poller_timer *timer = &self->timer;
	if (tv)
	{
		struct timeval now = poller_pa_get_current_time ();
		poller_timer_set (timer,
			(tv->tv_sec  - now.tv_sec)  * 1000 +
			(tv->tv_usec - now.tv_usec) / 1000);
	}
	else
		poller_timer_reset (timer);
}

static pa_time_event *
poller_pa_time_new (pa_mainloop_api *api, const struct timeval *tv,
	pa_time_event_cb_t cb, void *userdata)
{
	pa_time_event *self = xcalloc (1, sizeof *self);
	self->api = api;
	self->dispatch = cb;
	self->user_data = userdata;

	struct poller_pa *data = api->userdata;
	struct poller_timer *timer = &self->timer;
	poller_timer_init (timer, data->poller);
	timer->user_data = self;
	timer->dispatcher = poller_pa_time_dispatcher;

	poller_pa_time_restart (self, tv);
	LIST_PREPEND (data->time_list, self);
	return self;
}

static void
poller_pa_time_free (pa_time_event *self)
{
	if (self->free)
		self->free (self->api, self, self->user_data);

	struct poller_pa *data = self->api->userdata;
	poller_timer_reset (&self->timer);
	LIST_UNLINK (data->time_list, self);
	free (self);
}

static void
poller_pa_time_set_destroy (pa_time_event *self, pa_time_event_destroy_cb_t cb)
{
	self->free = cb;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
poller_pa_defer_dispatcher (void *user_data)
{
	pa_defer_event *self = user_data;
	self->dispatch (self->api, self, self->user_data);
}

static pa_defer_event *
poller_pa_defer_new (pa_mainloop_api *api,
	pa_defer_event_cb_t cb, void *userdata)
{
	pa_defer_event *self = xcalloc (1, sizeof *self);
	self->api = api;
	self->dispatch = cb;
	self->user_data = userdata;

	struct poller_pa *data = api->userdata;
	struct poller_idle *idle = &self->idle;
	poller_idle_init (idle, data->poller);
	idle->user_data = self;
	idle->dispatcher = poller_pa_defer_dispatcher;

	poller_idle_set (idle);
	LIST_PREPEND (data->defer_list, self);
	return self;
}

static void
poller_pa_defer_enable (pa_defer_event *self, int enable)
{
	struct poller_idle *idle = &self->idle;
	if (enable)
		poller_idle_set (idle);
	else
		poller_idle_reset (idle);
}

static void
poller_pa_defer_free (pa_defer_event *self)
{
	if (self->free)
		self->free (self->api, self, self->user_data);

	struct poller_pa *data = self->api->userdata;
	poller_idle_reset (&self->idle);
	LIST_UNLINK (data->defer_list, self);
	free (self);
}

static void
poller_pa_defer_set_destroy (pa_defer_event *self,
	pa_defer_event_destroy_cb_t cb)
{
	self->free = cb;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
poller_pa_quit (pa_mainloop_api *api, int retval)
{
	struct poller_pa *data = api->userdata;
	data->result = retval;
	data->running = false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static struct pa_mainloop_api g_poller_pa_template =
{
	.io_new            = poller_pa_io_new,
	.io_enable         = poller_pa_io_enable,
	.io_free           = poller_pa_io_free,
	.io_set_destroy    = poller_pa_io_set_destroy,

	.time_new          = poller_pa_time_new,
	.time_restart      = poller_pa_time_restart,
	.time_free         = poller_pa_time_free,
	.time_set_destroy  = poller_pa_time_set_destroy,

	.defer_new         = poller_pa_defer_new,
	.defer_enable      = poller_pa_defer_enable,
	.defer_free        = poller_pa_defer_free,
	.defer_set_destroy = poller_pa_defer_set_destroy,

	.quit              = poller_pa_quit,
};

static struct pa_mainloop_api *
poller_pa_new (struct poller *self)
{
	struct poller_pa *data = xcalloc (1, sizeof *data);
	data->poller = self;

	struct pa_mainloop_api *api = xmalloc (sizeof *api);
	*api = g_poller_pa_template;
	api->userdata = data;
	return api;
}

static void
poller_pa_destroy (struct pa_mainloop_api *api)
{
	struct poller_pa *data = api->userdata;

	LIST_FOR_EACH (pa_io_event, iter, data->io_list)
		poller_pa_io_free (iter);
	LIST_FOR_EACH (pa_time_event, iter, data->time_list)
		poller_pa_time_free (iter);
	LIST_FOR_EACH (pa_defer_event, iter, data->defer_list)
		poller_pa_defer_free (iter);

	free (data);
	free (api);
}

/// Since our poller API doesn't care much about continuous operation,
/// we need to provide that in the PulseAudio abstraction itself
static int
poller_pa_run (struct pa_mainloop_api *api)
{
	struct poller_pa *data = api->userdata;
	data->running = true;
	while (data->running)
		poller_run (data->poller);
	return data->result;
}

// --- MPD client interface ----------------------------------------------------

// This is a rather thin MPD client interface intended for basic tasks

#define MPD_SUBSYSTEM_TABLE(XX)                 \
	XX (DATABASE,         0, "database")        \
	XX (UPDATE,           1, "update")          \
	XX (STORED_PLAYLIST,  2, "stored_playlist") \
	XX (PLAYLIST,         3, "playlist")        \
	XX (PLAYER,           4, "player")          \
	XX (MIXER,            5, "mixer")           \
	XX (OUTPUT,           6, "output")          \
	XX (OPTIONS,          7, "options")         \
	XX (STICKER,          8, "sticker")         \
	XX (SUBSCRIPTION,     9, "subscription")    \
	XX (MESSAGE,         10, "message")

enum mpd_subsystem
{
#define XX(a, b, c) MPD_SUBSYSTEM_ ## a = (1 << b),
	MPD_SUBSYSTEM_TABLE (XX)
#undef XX
	MPD_SUBSYSTEM_MAX
};

static const char *mpd_subsystem_names[] =
{
#define XX(a, b, c) [b] = c,
	MPD_SUBSYSTEM_TABLE (XX)
#undef XX
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

enum mpd_client_state
{
	MPD_DISCONNECTED,                   ///< Not connected
	MPD_CONNECTING,                     ///< Currently connecting
	MPD_CONNECTED                       ///< Connected
};

struct mpd_response
{
	bool success;                       ///< OK or ACK

	// ACK-only fields:

	int error;                          ///< Numeric error value (ack.h)
	int list_offset;                    ///< Offset of command in list
	char *current_command;              ///< Name of the erroring command
	char *message_text;                 ///< Error message
};

/// Task completion callback
typedef void (*mpd_client_task_cb) (const struct mpd_response *response,
	const struct str_vector *data, void *user_data);

struct mpd_client_task
{
	LIST_HEADER (struct mpd_client_task)

	mpd_client_task_cb callback;        ///< Callback on completion
	void *user_data;                    ///< User data
};

struct mpd_client
{
	struct poller *poller;              ///< Poller

	// Connection:

	enum mpd_client_state state;        ///< Connection state
	struct connector *connector;        ///< Connection establisher

	int socket;                         ///< MPD socket
	struct str read_buffer;             ///< Input yet to be processed
	struct str write_buffer;            ///< Outut yet to be be sent out
	struct poller_fd socket_event;      ///< We can read from the socket

	struct poller_timer timeout_timer;  ///< Connection seems to be dead

	// Protocol:

	bool got_hello;                     ///< Got the OK MPD hello message

	bool idling;                        ///< Sent idle as the last command
	unsigned idling_subsystems;         ///< Subsystems we're idling for
	bool in_list;                       ///< We're inside a command list

	struct mpd_client_task *tasks;      ///< Task queue
	struct mpd_client_task *tasks_tail; ///< Tail of task queue
	struct str_vector data;             ///< Data from last command

	// User configuration:

	void *user_data;                    ///< User data for callbacks

	/// Callback after connection has been successfully established
	void (*on_connected) (void *user_data);

	/// Callback for general failures or even normal disconnection;
	/// the interface is reinitialized
	void (*on_failure) (void *user_data);

	/// Callback to receive "idle" updates.
	/// Remember to restart the idle if needed.
	void (*on_event) (unsigned subsystems, void *user_data);
};

static void mpd_client_reset (struct mpd_client *self);
static void mpd_client_destroy_connector (struct mpd_client *self);

static void
mpd_client_init (struct mpd_client *self, struct poller *poller)
{
	memset (self, 0, sizeof *self);

	self->poller = poller;
	self->socket = -1;

	str_init (&self->read_buffer);
	str_init (&self->write_buffer);

	str_vector_init (&self->data);

	poller_fd_init (&self->socket_event, poller, -1);
	poller_timer_init (&self->timeout_timer, poller);
}

static void
mpd_client_free (struct mpd_client *self)
{
	// So that we don't have to repeat most of the stuff
	mpd_client_reset (self);

	str_free (&self->read_buffer);
	str_free (&self->write_buffer);

	str_vector_free (&self->data);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Reinitialize the interface so that you can reconnect anew
static void
mpd_client_reset (struct mpd_client *self)
{
	if (self->state == MPD_CONNECTING)
		mpd_client_destroy_connector (self);

	if (self->socket != -1)
		xclose (self->socket);
	self->socket = -1;

	self->socket_event.closed = true;
	poller_fd_reset (&self->socket_event);
	poller_timer_reset (&self->timeout_timer);

	str_reset (&self->read_buffer);
	str_reset (&self->write_buffer);

	str_vector_reset (&self->data);

	self->got_hello = false;
	self->idling = false;
	self->idling_subsystems = 0;
	self->in_list = false;

	LIST_FOR_EACH (struct mpd_client_task, iter, self->tasks)
		free (iter);
	self->tasks = self->tasks_tail = NULL;

	self->state = MPD_DISCONNECTED;
}

static void
mpd_client_fail (struct mpd_client *self)
{
	mpd_client_reset (self);
	if (self->on_failure)
		self->on_failure (self->user_data);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
mpd_client_parse_response (const char *p, struct mpd_response *response)
{
	if (!strcmp (p, "OK"))
		return response->success = true;
	if (!strcmp (p, "list_OK"))
		// TODO: either implement this or fail the connection properly
		hard_assert (!"command_list_ok_begin not implemented");

	char *end = NULL;
	if (*p++ != 'A' || *p++ != 'C' || *p++ != 'K' || *p++ != ' ' || *p++ != '[')
		return false;

	errno = 0;
	response->error = strtoul (p, &end, 10);
	if (errno != 0 || end == p)
		return false;
	p = end;
	if (*p++ != '@')
		return false;

	errno = 0;
	response->list_offset = strtoul (p, &end, 10);
	if (errno != 0 || end == p)
		return false;
	p = end;
	if (*p++ != ']' || *p++ != ' ' || *p++ != '{' || !(end = strchr (p, '}')))
		return false;

	response->current_command = xstrndup (p, end - p);
	p = end + 1;

	if (*p++ != ' ')
		return false;

	response->message_text = xstrdup (p);
	response->success = false;
	return true;
}

static void
mpd_client_dispatch (struct mpd_client *self, struct mpd_response *response)
{
	struct mpd_client_task *task;
	if (!(task = self->tasks))
		return;

	if (task->callback)
		task->callback (response, &self->data, task->user_data);
	str_vector_reset (&self->data);

	LIST_UNLINK_WITH_TAIL (self->tasks, self->tasks_tail, task);
	free (task);
}

static bool
mpd_client_parse_hello (struct mpd_client *self, const char *line)
{
	const char hello[] = "OK MPD ";
	if (strncmp (line, hello, sizeof hello - 1))
	{
		print_debug ("invalid MPD hello message");
		return false;
	}

	// TODO: call "on_connected" now.  We should however also set up a timer
	//   so that we don't wait on this message forever.
	return self->got_hello = true;
}

static bool
mpd_client_parse_line (struct mpd_client *self, const char *line)
{
	print_debug ("MPD >> %s", line);

	if (!self->got_hello)
		return mpd_client_parse_hello (self, line);

	struct mpd_response response;
	memset (&response, 0, sizeof response);
	if (mpd_client_parse_response (line, &response))
	{
		mpd_client_dispatch (self, &response);
		free (response.current_command);
		free (response.message_text);
	}
	else
		str_vector_add (&self->data, line);
	return true;
}

/// All output from MPD commands seems to be in a trivial "key: value" format
static char *
mpd_client_parse_kv (char *line, char **value)
{
	char *sep;
	if (!(sep = strstr (line, ": ")))
		return NULL;

	*sep = 0;
	*value = sep + 2;
	return line;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_client_update_poller (struct mpd_client *self)
{
	poller_fd_set (&self->socket_event,
		self->write_buffer.len ? (POLLIN | POLLOUT) : POLLIN);
}

static bool
mpd_client_process_input (struct mpd_client *self)
{
	// Split socket input at newlines and process them separately
	struct str *rb = &self->read_buffer;
	char *start = rb->str, *end = start + rb->len;
	for (char *p = start; p < end; p++)
	{
		if (*p != '\n')
			continue;

		*p = 0;
		if (!mpd_client_parse_line (self, start))
			return false;
		start = p + 1;
	}

	str_remove_slice (rb, 0, start - rb->str);
	return true;
}

static void
mpd_client_on_ready (const struct pollfd *pfd, void *user_data)
{
	(void) pfd;

	struct mpd_client *self = user_data;
	if (socket_io_try_read  (self->socket, &self->read_buffer)  != SOCKET_IO_OK
	 || !mpd_client_process_input (self)
	 || socket_io_try_write (self->socket, &self->write_buffer) != SOCKET_IO_OK)
		mpd_client_fail (self);
	else
		mpd_client_update_poller (self);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
mpd_client_must_quote_char (char c)
{
	return (unsigned char) c <= ' ' || c == '"' || c == '\'';
}

static bool
mpd_client_must_quote (const char *s)
{
	if (!*s)
		return true;
	for (; *s; s++)
		if (mpd_client_must_quote_char (*s))
			return true;
	return false;
}

static void
mpd_client_quote (const char *s, struct str *output)
{
	str_append_c (output, '"');
	for (; *s; s++)
	{
		if (mpd_client_must_quote_char (*s))
			str_append_c (output, '\\');
		str_append_c (output, *s);
	}
	str_append_c (output, '"');
}

/// Beware that delivery of the event isn't deferred and you musn't make
/// changes to the interface while processing the event!
static void
mpd_client_add_task
	(struct mpd_client *self, mpd_client_task_cb cb, void *user_data)
{
	// This only has meaning with command_list_ok_begin, and then it requires
	// special handling (all in-list tasks need to be specially marked and
	// later flushed if an early ACK or OK arrives).
	hard_assert (!self->in_list);

	struct mpd_client_task *task = xcalloc (1, sizeof *self);
	task->callback = cb;
	task->user_data = user_data;
	LIST_APPEND_WITH_TAIL (self->tasks, self->tasks_tail, task);
}

/// Send a command.  Remember to call mpd_client_add_task() to handle responses,
/// unless the command is being sent in a list.
static void mpd_client_send_command
	(struct mpd_client *self, const char *command, ...) ATTRIBUTE_SENTINEL;

static void
mpd_client_send_commandv (struct mpd_client *self, char **commands)
{
	// Automatically interrupt idle mode
	if (self->idling)
	{
		poller_timer_reset (&self->timeout_timer);

		self->idling = false;
		self->idling_subsystems = 0;
		mpd_client_send_command (self, "noidle", NULL);
	}

	struct str line;
	str_init (&line);

	for (; *commands; commands++)
	{
		if (line.len)
			str_append_c (&line, ' ');

		if (mpd_client_must_quote (*commands))
			mpd_client_quote (*commands, &line);
		else
			str_append (&line, *commands);
	}

	print_debug ("MPD << %s", line.str);
	str_append_c (&line, '\n');
	str_append_str (&self->write_buffer, &line);
	str_free (&line);

	mpd_client_update_poller (self);
}

static void
mpd_client_send_command (struct mpd_client *self, const char *command, ...)
{
	struct str_vector v;
	str_vector_init (&v);

	va_list ap;
	va_start (ap, command);
	for (; command; command = va_arg (ap, const char *))
		str_vector_add (&v, command);
	va_end (ap);

	mpd_client_send_commandv (self, v.vector);
	str_vector_free (&v);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_client_list_begin (struct mpd_client *self)
{
	hard_assert (!self->in_list);
	mpd_client_send_command (self, "command_list_begin", NULL);
	self->in_list = true;
}

/// End a list of commands.  Remember to call mpd_client_add_task()
/// to handle the summary response.
static void
mpd_client_list_end (struct mpd_client *self)
{
	hard_assert (self->in_list);
	mpd_client_send_command (self, "command_list_end", NULL);
	self->in_list = false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
mpd_resolve_subsystem (const char *name, unsigned *output)
{
	for (size_t i = 0; i < N_ELEMENTS (mpd_subsystem_names); i++)
		if (!strcasecmp_ascii (name, mpd_subsystem_names[i]))
		{
			*output |= 1 << i;
			return true;
		}
	return false;
}

static void
mpd_client_on_idle_return (const struct mpd_response *response,
	const struct str_vector *data, void *user_data)
{
	(void) response;

	struct mpd_client *self = user_data;
	unsigned subsystems = 0;
	for (size_t i = 0; i < data->len; i++)
	{
		char *value, *key;
		if (!(key = mpd_client_parse_kv (data->vector[i], &value)))
			print_debug ("%s: %s", "erroneous MPD output", data->vector[i]);
		else if (strcasecmp_ascii (key, "changed"))
			print_debug ("%s: %s", "unexpected idle key", key);
		else if (!mpd_resolve_subsystem (value, &subsystems))
			print_debug ("%s: %s", "unknown subsystem", value);
	}

	// Not resetting "idling" here, we may send an extra "noidle" no problem
	if (self->on_event && subsystems)
		self->on_event (subsystems, self->user_data);
}

static void mpd_client_idle (struct mpd_client *self, unsigned subsystems);

static void
mpd_client_on_timeout (void *user_data)
{
	struct mpd_client *self = user_data;
	unsigned subsystems = self->idling_subsystems;

	// Just sending this out should bring a dead connection down over TCP
	// TODO: set another timer to make sure the ping reply arrives
	mpd_client_send_command (self, "ping", NULL);
	mpd_client_add_task (self, NULL, NULL);

	// Restore the incriminating idle immediately
	mpd_client_idle (self, subsystems);
}

/// When not expecting to send any further commands, you should call this
/// in order to keep the connection alive.  Or to receive updates.
static void
mpd_client_idle (struct mpd_client *self, unsigned subsystems)
{
	hard_assert (!self->in_list);

	struct str_vector v;
	str_vector_init (&v);

	str_vector_add (&v, "idle");
	for (size_t i = 0; i < N_ELEMENTS (mpd_subsystem_names); i++)
		if (subsystems & (1 << i))
			str_vector_add (&v, mpd_subsystem_names[i]);

	mpd_client_send_commandv (self, v.vector);
	str_vector_free (&v);

	self->timeout_timer.dispatcher = mpd_client_on_timeout;
	self->timeout_timer.user_data = self;
	poller_timer_set (&self->timeout_timer, 5 * 60 * 1000);

	mpd_client_add_task (self, mpd_client_on_idle_return, self);
	self->idling = true;
	self->idling_subsystems = subsystems;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_client_finish_connection (struct mpd_client *self, int socket)
{
	set_blocking (socket, false);
	self->socket = socket;
	self->state = MPD_CONNECTED;

	poller_fd_init (&self->socket_event, self->poller, self->socket);
	self->socket_event.dispatcher = mpd_client_on_ready;
	self->socket_event.user_data = self;

	mpd_client_update_poller (self);

	if (self->on_connected)
		self->on_connected (self->user_data);
}

static void
mpd_client_destroy_connector (struct mpd_client *self)
{
	if (self->connector)
		connector_free (self->connector);
	free (self->connector);
	self->connector = NULL;

	// Not connecting anymore
	self->state = MPD_DISCONNECTED;
}

static void
mpd_client_on_connector_failure (void *user_data)
{
	struct mpd_client *self = user_data;
	mpd_client_destroy_connector (self);
	mpd_client_fail (self);
}

static void
mpd_client_on_connector_connected
	(void *user_data, int socket, const char *host)
{
	(void) host;

	struct mpd_client *self = user_data;
	mpd_client_destroy_connector (self);
	mpd_client_finish_connection (self, socket);
}

static bool
mpd_client_connect_unix (struct mpd_client *self, const char *address,
	struct error **e)
{
	int fd = socket (AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
	{
		error_set (e, "%s: %s", "socket", strerror (errno));
		return false;
	}

	// Expand tilde if needed
	char *expanded = resolve_filename (address, xstrdup);

	struct sockaddr_un sun;
	sun.sun_family = AF_UNIX;
	strncpy (sun.sun_path, expanded, sizeof sun.sun_path);
	sun.sun_path[sizeof sun.sun_path - 1] = 0;

	free (expanded);

	if (connect (fd, (struct sockaddr *) &sun, sizeof sun))
	{
		error_set (e, "%s: %s", "connect", strerror (errno));
		return false;
	}

	mpd_client_finish_connection (self, fd);
	return true;
}

static bool
mpd_client_connect (struct mpd_client *self, const char *address,
	const char *service, struct error **e)
{
	hard_assert (self->state == MPD_DISCONNECTED);

	// If it looks like a path, assume it's a UNIX socket
	if (strchr (address, '/'))
		return mpd_client_connect_unix (self, address, e);

	struct connector *connector = xmalloc (sizeof *connector);
	connector_init (connector, self->poller);
	self->connector = connector;

	connector->user_data    = self;
	connector->on_connected = mpd_client_on_connector_connected;
	connector->on_failure   = mpd_client_on_connector_failure;

	connector_add_target (connector, address, service);
	self->state = MPD_CONNECTING;
	return true;
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

	struct str_vector fields;           ///< Line fields
};

static void
nut_parser_init (struct nut_parser *self)
{
	self->state = NUT_STATE_START_LINE;
	str_init (&self->current_field);
	str_vector_init (&self->fields);
}

static void
nut_parser_free (struct nut_parser *self)
{
	str_free (&self->current_field);
	str_vector_free (&self->fields);
}

static int
nut_parser_end_field (struct nut_parser *self, char c)
{
	str_vector_add (&self->fields, self->current_field.str);
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
		str_vector_reset (&self->fields);
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

	struct str_vector fields;           ///< Parsed fields from the line
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

	str_init (&self->read_buffer);
	str_init (&self->write_buffer);

	nut_parser_init (&self->parser);

	poller_fd_init (&self->socket_event, poller, -1);
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
		str_vector_free (&iter->fields);
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
	struct str reconstructed;
	str_init (&reconstructed);
	nut_client_serialize (self->parser.fields.vector, &reconstructed);
	print_debug ("NUT >> %s", reconstructed.str);
	str_free (&reconstructed);

	struct str_vector *fields = &self->parser.fields;
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
		str_vector_init (&line->fields);
		str_vector_add_vector (&line->fields, fields->vector);
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
	struct str line;
	str_init (&line);
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
	struct str_vector v;
	str_vector_init (&v);

	va_list ap;
	va_start (ap, command);
	for (; command; command = va_arg (ap, const char *))
		str_vector_add (&v, command);
	va_end (ap);

	nut_client_send_commandv (self, v.vector);
	str_vector_free (&v);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
nut_client_finish_connection (struct nut_client *self, int socket)
{
	set_blocking (socket, false);
	self->socket = socket;
	self->state = NUT_CONNECTED;

	poller_fd_init (&self->socket_event, self->poller, self->socket);
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

// --- Configuration -----------------------------------------------------------

static struct simple_config_item g_config_table[] =
{
	{ "mpd_address",     "localhost",       "MPD host or socket"             },
	{ "mpd_service",     "6600",            "MPD service name or port"       },
	{ "mpd_password",    NULL,              "MPD password"                   },

	{ "nut_enabled",     "off",             "NUT UPS status reading enabled" },
	{ "nut_load_thld",   "50",              "NUT threshold for load display" },

	{ NULL,              NULL,              NULL                             }
};

// --- Application -------------------------------------------------------------

struct app_context
{
	struct str_map config;              ///< Program configuration

	Display *dpy;                       ///< X display handle
	int xkb_base_event_code;            ///< Xkb base event code
	const char *prefix;                 ///< User-defined prefix

	struct poller poller;               ///< Poller
	struct poller_timer time_changed;   ///< Time change timer
	struct poller_timer make_context;   ///< Start PulseAudio communication
	struct poller_timer refresh_rest;   ///< Refresh unpollable information

	// Hotkeys:

	struct poller_fd x_event;           ///< X11 event

	// MPD:

	struct poller_timer mpd_reconnect;  ///< Start MPD communication
	struct mpd_client mpd_client;       ///< MPD client

	char *mpd_song;                     ///< MPD current song
	char *mpd_status;                   ///< MPD status (overrides song)

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

	pa_cvolume sink_volume;             ///< Current volume
	bool sink_muted;                    ///< Currently muted?

	bool source_muted;                  ///< Currently muted?
};

static void
str_map_destroy (void *self)
{
	str_map_free (self);
	free (self);
}

static void
app_context_init (struct app_context *self)
{
	memset (self, 0, sizeof *self);

	str_map_init (&self->config);
	self->config.free = free;
	simple_config_load_defaults (&self->config, g_config_table);

	if (!(self->dpy = XkbOpenDisplay
		(NULL, &self->xkb_base_event_code, NULL, NULL, NULL, NULL)))
		exit_fatal ("cannot open display");

	poller_init (&self->poller);
	self->api = poller_pa_new (&self->poller);

	poller_fd_init (&self->x_event, &self->poller,
		ConnectionNumber (self->dpy));

	mpd_client_init (&self->mpd_client, &self->poller);

	nut_client_init (&self->nut_client, &self->poller);
	str_map_init (&self->nut_ups_info);
	self->nut_ups_info.free = str_map_destroy;
}

static void
app_context_free (struct app_context *self)
{
	str_map_free (&self->config);

	poller_fd_reset (&self->x_event);

	if (self->context)  pa_context_unref (self->context);
	if (self->dpy)      XCloseDisplay (self->dpy);

	poller_pa_destroy (self->api);
	poller_free (&self->poller);

	mpd_client_free (&self->mpd_client);
	free (self->mpd_song);
	free (self->mpd_status);

	nut_client_free (&self->nut_client);
	str_map_free (&self->nut_ups_info);
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

	struct str s;
	str_init (&s);
	bool success = read_line (fp, &s);
	fclose (fp);

	if (!success)
	{
		error_set (e, "%s: %s", filename, "read failed");
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
		error_set (e, "%s: %s", filename, "doesn't contain an unsigned number");
	free (value);
	return number;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static char *
read_battery_status (int dir, struct error **e)
{
	char *result = NULL;
	struct error *error = NULL;

	char *status;
	double charge_now;
	double charge_full;

	if ((status      = read_value  (dir, "status",      &error), error)
	 || (charge_now  = read_number (dir, "charge_now",  &error), error)
	 || (charge_full = read_number (dir, "charge_full", &error), error))
		error_propagate (e, error);
	else
		result = xstrdup_printf ("%s (%u%%)",
			status, (unsigned) (charge_now / charge_full * 100 + 0.5));

	free (status);
	return result;
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

	bool is_relevant =
		!strcmp (type, "Battery") ||
		!strcmp (type, "UPS");

	char *result = NULL;
	if (is_relevant)
	{
		char *status = read_battery_status (dir, &error);
		if (error)
			error_propagate (e, error);
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
	char *status = NULL;
	while (!status && (entry = readdir (power_supply)))
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
		status = try_power_supply (dir, &error);
		close (dir);
		if (error)
		{
			print_error ("%s: %s", device_name, error->message);
			error_free (error);
		}
	}
	closedir (power_supply);
	return status;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static char *
make_time_status (char *fmt)
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

	struct str s;
	str_init (&s);
	str_append_printf (&s, "%u%%", VOLUME_PERCENT (ctx->sink_volume.values[0]));
	if (!pa_cvolume_channels_equal_to (&ctx->sink_volume, ctx->sink_volume.values[0]))
		for (size_t i = 1; i < ctx->sink_volume.channels; i++)
			str_append_printf (&s, " / %u%%",
				VOLUME_PERCENT (ctx->sink_volume.values[i]));
	return str_steal (&s);
}

static void
refresh_status (struct app_context *ctx)
{
	struct str status;
	str_init (&status);

	if (ctx->prefix)
		str_append_printf (&status, "%s   ", ctx->prefix);

	if (ctx->mpd_status)
		str_append_printf (&status, "%s   ", ctx->mpd_status);
	else if (ctx->mpd_song)
		str_append_printf (&status, "%s   ", ctx->mpd_song);

	if (ctx->failed)
		str_append_printf (&status, "%s   ", "PA failure");
	else
	{
		char *volumes = make_volume_status (ctx);
		str_append_printf (&status, "%s %s   ",
			ctx->sink_muted ? "Muted" : "Volume", volumes);
		free (volumes);
	}

	char *battery = make_battery_status ();
	if (battery)
		str_append_printf (&status, "%s   ", battery);
	free (battery);

	if (ctx->nut_status)
		str_append_printf (&status, "%s   ", ctx->nut_status);

	char *times = make_time_status ("Week %V, %a %d %b %Y %H:%M %Z");
	str_append (&status, times);
	free (times);

	set_dwm_status (ctx->dpy, status.str);
	str_free (&status);
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

// Sometimes it's not that easy and there can be repeating entries
static void
mpd_vector_to_map (const struct str_vector *data, struct str_map *map)
{
	str_map_init (map);
	map->key_xfrm = tolower_ascii_strxfrm;
	map->free = free;

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
	const struct str_vector *data, void *user_data)
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

	free (ctx->mpd_status);
	ctx->mpd_status = NULL;

	const char *value;
	if ((value = str_map_find (&map, "state")))
	{
		if (!strcmp (value, "stop"))
			ctx->mpd_status = xstrdup ("MPD stopped");
		if (!strcmp (value, "pause"))
			ctx->mpd_status = xstrdup ("MPD paused");
	}

	struct str s;
	str_init (&s);

	str_append (&s, "Playing: ");
	if ((value = str_map_find (&map, "title"))
	 || (value = str_map_find (&map, "name"))
	 || (value = str_map_find (&map, "file")))
		str_append_printf (&s, "\"%s\"", value);
	if ((value = str_map_find (&map, "artist")))
		str_append_printf (&s, " by \"%s\"", value);
	if ((value = str_map_find (&map, "album")))
		str_append_printf (&s, " from \"%s\"", value);

	free (ctx->mpd_song);
	ctx->mpd_song = str_steal (&s);

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
	const struct str_vector *data, void *user_data)
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

	const char *password = str_map_find (&ctx->config, "mpd_password");
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
}

static void
on_mpd_reconnect (void *user_data)
{
	struct app_context *ctx = user_data;

	struct mpd_client *c = &ctx->mpd_client;
	c->user_data    = ctx;
	c->on_failure   = mpd_on_failure;
	c->on_connected = mpd_on_connected;
	c->on_event     = mpd_on_events;

	struct error *e = NULL;
	if (!mpd_client_connect (&ctx->mpd_client,
		str_map_find (&ctx->config, "mpd_address"),
		str_map_find (&ctx->config, "mpd_service"), &e))
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
nut_translate_status (const char *status, struct str_vector *out)
{
	// https://github.com/networkupstools/nut/blob/master/clients/status.h
	if (!strcmp (status, "OL"))      str_vector_add (out, "on-line");
	if (!strcmp (status, "OB"))      str_vector_add (out, "on battery");
	if (!strcmp (status, "LB"))      str_vector_add (out, "low battery");
	if (!strcmp (status, "RB"))      str_vector_add (out, "replace battery");
	if (!strcmp (status, "CHRG"))    str_vector_add (out, "charging");
	if (!strcmp (status, "DISCHRG")) str_vector_add (out, "discharging");
	if (!strcmp (status, "OVER"))    str_vector_add (out, "overload");
	if (!strcmp (status, "OFF"))     str_vector_add (out, "off");
	if (!strcmp (status, "TRIM"))    str_vector_add (out, "voltage trim");
	if (!strcmp (status, "BOOST"))   str_vector_add (out, "voltage boost");
	if (!strcmp (status, "BYPASS"))  str_vector_add (out, "bypass");
}

static char *
interval_string (unsigned long seconds)
{
	unsigned long hours = seconds / 3600; seconds %= 3600;
	unsigned long mins  = seconds /   60; seconds %=   60;
	return xstrdup_printf ("%lu:%02lu:%02lu", hours, mins, seconds);
}

static void
nut_process_ups (struct app_context *ctx, struct str_vector *ups_list,
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

	if (!soft_assert (status && charge && runtime))
		return;

	unsigned long runtime_sec;
	if (!soft_assert (xstrtoul (&runtime_sec, runtime, 10)))
		return;

	struct str_vector items;
	str_vector_init (&items);

	bool running_on_batteries = false;

	struct str_vector v;
	str_vector_init (&v);
	cstr_split_ignore_empty (status, ' ', &v);
	for (size_t i = 0; i < v.len; i++)
	{
		const char *status = v.vector[i];
		nut_translate_status (status, &items);

		if (!strcmp (status, "OB"))
			running_on_batteries = true;
	}
	str_vector_free (&v);

	if (running_on_batteries || strcmp (charge, "100"))
		str_vector_add_owned (&items, xstrdup_printf ("%s%%", charge));
	if (running_on_batteries)
		str_vector_add_owned (&items, interval_string (runtime_sec));

	// Only show load if it's higher than the threshold so as to not distract
	const char *threshold = str_map_find (&ctx->config, "nut_load_thld");
	unsigned long load_n, threshold_n;
	if (load
	 && xstrtoul (&load_n,      load,      10)
	 && xstrtoul (&threshold_n, threshold, 10)
	 && load_n >= threshold_n)
		str_vector_add_owned (&items, xstrdup_printf ("load %s%%", load));

	struct str result;
	str_init (&result);
	str_append (&result, "UPS: ");

	for (size_t i = 0; i < items.len; i++)
	{
		if (i) str_append (&result, "; ");
		str_append (&result, items.vector[i]);
	}
	str_vector_free (&items);
	str_vector_add_owned (ups_list, str_steal (&result));
}

static void
nut_on_logout_response (const struct nut_response *response, void *user_data)
{
	if (!nut_common_handler (response))
		return;

	struct app_context *ctx = user_data;
	struct str_vector ups_list;
	str_vector_init (&ups_list);

	struct str_map_iter iter;
	str_map_iter_init (&iter, &ctx->nut_ups_info);
	struct str_map *dict;
	while ((dict = str_map_iter_next (&iter)))
		nut_process_ups (ctx, &ups_list, iter.link->key, dict);

	free (ctx->nut_status);
	ctx->nut_status = NULL;

	if (ups_list.len)
	{
		struct str status;
		str_init (&status);
		str_append (&status, ups_list.vector[0]);
		for (size_t i = 1; i < ups_list.len; i++)
			str_append_printf (&status, "   %s", ups_list.vector[0]);
		ctx->nut_status = str_steal (&status);
	}

	ctx->nut_success = true;
	str_vector_free (&ups_list);
	refresh_status (ctx);
}

static void
nut_store_var (struct app_context *ctx,
	const char *ups_name, const char *key, const char *value)
{
	struct str_map *map;
	if (!(map = str_map_find (&ctx->nut_ups_info, ups_name)))
	{
		str_map_init ((map = xcalloc (1, sizeof *map)));
		map->free = free;
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
		const struct str_vector *fields = &iter->fields;
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
		const struct str_vector *fields = &iter->fields;
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
	free (ctx->nut_status);
	ctx->nut_status = xstrdup ("NUT failure");

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

	bool want_nut = false;
	if (!set_boolean_if_valid (&want_nut,
		str_map_find (&ctx->config, "nut_enabled")))
		print_error ("invalid configuration value for `%s'", "nut_enabled");
	if (!want_nut)
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
		ctx->sink_volume = info->volume;
		ctx->sink_muted = !!info->mute;
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
spawn (struct app_context *ctx, char *argv[])
{
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init (&actions);

	posix_spawn_file_actions_addclose (&actions, ConnectionNumber (ctx->dpy));
	if (ctx->mpd_client.socket != -1)
		posix_spawn_file_actions_addclose (&actions, ctx->mpd_client.socket);
	if (ctx->nut_client.socket != -1)
		posix_spawn_file_actions_addclose (&actions, ctx->nut_client.socket);

	posix_spawnattr_t attr;
	posix_spawnattr_init (&attr);
	posix_spawnattr_setpgroup (&attr, 0);

	posix_spawnp (NULL, argv[0], &actions, &attr, argv, environ);

	posix_spawn_file_actions_destroy (&actions);
	posix_spawnattr_destroy (&attr);
}

#define MPD_SIMPLE(name, ...)                              \
	static void                                            \
	on_mpd_ ## name (struct app_context *ctx, int arg)     \
	{                                                      \
		(void) arg;                                        \
		struct mpd_client *c = &ctx->mpd_client;           \
		if (c->state != MPD_CONNECTED)                     \
			return;                                        \
		mpd_client_send_command (c, __VA_ARGS__);          \
		mpd_client_add_task (c, NULL, NULL);               \
		mpd_client_idle (c, 0);                            \
	}

// XXX: pause without argument is deprecated, we can watch play state
//   if we want to have the toggle pause/play functionality
MPD_SIMPLE (play,     "pause",    NULL)
MPD_SIMPLE (stop,     "stop",     NULL)
MPD_SIMPLE (prev,     "previous", NULL)
MPD_SIMPLE (next,     "next",     NULL)
MPD_SIMPLE (forward,  "seekcur", "+10", NULL)
MPD_SIMPLE (backward, "seekcur", "-10", NULL)

static void
on_volume_finish (pa_context *context, int success, void *userdata)
{
	(void) context;
	(void) success;
	(void) userdata;

	// Just like... whatever, man
}

static void
on_volume_mic_mute (struct app_context *ctx, int arg)
{
	(void) arg;

	if (!ctx->context)
		return;

	pa_operation_unref (pa_context_set_source_mute_by_name (ctx->context,
		DEFAULT_SOURCE, !ctx->source_muted, on_volume_finish, ctx));
}

static void
on_volume_mute (struct app_context *ctx, int arg)
{
	(void) arg;

	if (!ctx->context)
		return;

	pa_operation_unref (pa_context_set_sink_mute_by_name (ctx->context,
		DEFAULT_SINK, !ctx->sink_muted, on_volume_finish, ctx));
}

static void
on_volume_set (struct app_context *ctx, int arg)
{
	if (!ctx->context)
		return;

	pa_cvolume volume = ctx->sink_volume;
	if (arg > 0)
		pa_cvolume_inc (&volume, (pa_volume_t)  arg * PA_VOLUME_NORM / 100);
	else
		pa_cvolume_dec (&volume, (pa_volume_t) -arg * PA_VOLUME_NORM / 100);
	pa_operation_unref (pa_context_set_sink_volume_by_name (ctx->context,
		DEFAULT_SINK, &volume, on_volume_finish, ctx));
}

static void
on_lock (struct app_context *ctx, int arg)
{
	(void) arg;

	// One of these will work
	char *argv_gdm[] = { "gdm-switch-user", NULL };
	spawn (ctx, argv_gdm);
	char *argv_ldm[] = { "dm-tool", "lock", NULL };
	spawn (ctx, argv_ldm);
}

static void
on_brightness (struct app_context *ctx, int arg)
{
	char *value = xstrdup_printf ("%d", arg);
	char *argv[] = { "brightness", value, NULL };
	spawn (ctx, argv);
	free (value);
}

struct
{
	unsigned mod;
	KeySym keysym;
	void (*handler) (struct app_context *ctx, int arg);
	int arg;
}
g_keys[] =
{
	// This key should be labeled L on normal Qwert[yz] layouts
	{ Mod4Mask,            XK_n,         on_lock,              0 },

	// MPD
	{ Mod4Mask,            XK_Up,        on_mpd_play,          0 },
	{ Mod4Mask,            XK_Down,      on_mpd_stop,          0 },
	{ Mod4Mask,            XK_Left,      on_mpd_prev,          0 },
	{ Mod4Mask,            XK_Right,     on_mpd_next,          0 },
	/* xmodmap | grep -e Alt_R -e Meta_R -e ISO_Level3_Shift -e Mode_switch */
	{ Mod4Mask | Mod5Mask, XK_Left,      on_mpd_backward,      0 },
	{ Mod4Mask | Mod5Mask, XK_Right,     on_mpd_forward,       0 },

	// Brightness
	{ Mod4Mask,            XK_Home,      on_brightness,       10 },
	{ Mod4Mask,            XK_End,       on_brightness,      -10 },
	{ 0, XF86XK_MonBrightnessUp,         on_brightness,       10 },
	{ 0, XF86XK_MonBrightnessDown,       on_brightness,      -10 },

	// Volume
	{ Mod4Mask,            XK_Delete,    on_volume_mute,       0 },
	{ Mod4Mask,            XK_Page_Up,   on_volume_set,       10 },
	{ Mod4Mask | Mod5Mask, XK_Page_Up,   on_volume_set,        1 },
	{ Mod4Mask,            XK_Page_Down, on_volume_set,      -10 },
	{ Mod4Mask | Mod5Mask, XK_Page_Down, on_volume_set,       -1 },
	{ 0, XF86XK_AudioMicMute,            on_volume_mic_mute,   0 },
	{ 0, XF86XK_AudioMute,               on_volume_mute,       0 },
	{ 0, XF86XK_AudioRaiseVolume,        on_volume_set,       10 },
	{ 0, XF86XK_AudioLowerVolume,        on_volume_set,      -10 },
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
on_x_keypress (struct app_context *ctx, XEvent *e)
{
	XKeyEvent *ev = &e->xkey;
	KeySym keysym = XkbKeycodeToKeysym (ctx->dpy, (KeyCode) ev->keycode,
		0 /* XXX: current group? */, !!(ev->state & ShiftMask));
	for (size_t i = 0; i < N_ELEMENTS (g_keys); i++)
		if (keysym == g_keys[i].keysym
		 && g_keys[i].mod == ev->state
		 && g_keys[i].handler)
			g_keys[i].handler (ctx, g_keys[i].arg);
}

static void
on_x_ready (const struct pollfd *pfd, void *user_data)
{
	(void) pfd;
	struct app_context *ctx = user_data;

	XEvent ev;
	while (XPending (ctx->dpy))
	{
		if (XNextEvent (ctx->dpy, &ev))
			exit_fatal ("XNextEvent returned non-zero");
		if (ev.type == KeyPress)
			on_x_keypress (ctx, &ev);
	}
}

static void
grab_keys (struct app_context *ctx)
{
	unsigned ignored_locks =
		LockMask | XkbKeysymToModifiers (ctx->dpy, XK_Num_Lock);
	hard_assert (XkbSetIgnoreLockMods
		(ctx->dpy, XkbUseCoreKbd, ignored_locks, ignored_locks, 0, 0));

	KeyCode code;
	Window root = DefaultRootWindow (ctx->dpy);
	for (size_t i = 0; i < N_ELEMENTS (g_keys); i++)
		if ((code = XKeysymToKeycode (ctx->dpy, g_keys[i].keysym)))
			XGrabKey (ctx->dpy, code, g_keys[i].mod, root,
				 False /* ? */, GrabModeAsync, GrabModeAsync);

	XSelectInput (ctx->dpy, root, KeyPressMask);
	XSync (ctx->dpy, False);

	ctx->x_event.dispatcher = on_x_ready;
	ctx->x_event.user_data = ctx;
	poller_fd_set (&ctx->x_event, POLLIN);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
poller_timer_init_and_set (struct poller_timer *self, struct poller *poller,
	poller_timer_fn cb, void *user_data)
{
	poller_timer_init (self, poller);
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
		{ 'w', "write-default-cfg", "FILENAME",
		  OPT_OPTIONAL_ARG | OPT_LONG_ONLY,
		  "write a default configuration file and exit" },
		{ 0, NULL, NULL, 0, NULL }
	};

	struct opt_handler oh;
	opt_handler_init (&oh, argc, argv, opts, NULL, "Set root window name.");

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
	case 'w':
		call_simple_config_write_default (optarg, g_config_table);
		exit (EXIT_SUCCESS);
	default:
		print_error ("wrong options");
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}

	argc -= optind;
	argv += optind;

	opt_handler_free (&oh);

	// We don't need to retrieve exit statuses of anything, avoid zombies
	struct sigaction sa;
	sa.sa_flags = SA_RESTART | SA_NOCLDWAIT;
	sigemptyset (&sa.sa_mask);
	sa.sa_handler = SIG_IGN;
	if (sigaction (SIGCHLD, &sa, NULL) == -1)
		print_error ("%s: %s", "sigaction", strerror (errno));

	struct app_context ctx;
	app_context_init (&ctx);
	ctx.prefix = argc > 1 ? argv[1] : NULL;

	struct error *e = NULL;
	if (!simple_config_update_from_file (&ctx.config, &e))
		exit_fatal ("%s", e->message);

	poller_timer_init_and_set (&ctx.time_changed, &ctx.poller,
		on_time_changed, &ctx);
	poller_timer_init_and_set (&ctx.make_context, &ctx.poller,
		on_make_context, &ctx);
	poller_timer_init_and_set (&ctx.refresh_rest, &ctx.poller,
		on_refresh_rest, &ctx);
	poller_timer_init_and_set (&ctx.mpd_reconnect, &ctx.poller,
		on_mpd_reconnect, &ctx);
	poller_timer_init_and_set (&ctx.nut_reconnect, &ctx.poller,
		on_nut_reconnect, &ctx);

	grab_keys (&ctx);

	poller_pa_run (ctx.api);
	app_context_free (&ctx);

	return 0;
}
