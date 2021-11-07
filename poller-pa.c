/*
 * pa.c: PulseAudio mainloop abstraction
 *
 * Copyright (c) 2016 - 2017, Přemysl Eric Janouch <p@janouch.name>
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

#include <pulse/mainloop.h>

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
	self->fd = poller_fd_make (data->poller, fd_);
	self->fd.user_data = self;
	self->fd.dispatcher = poller_pa_io_dispatcher;

	// FIXME: under x2go PA tries to register twice for the same FD,
	//   which fails with our curent poller implementation;
	//   we could maintain a list of { poller_fd, listeners } structures;
	//   or maybe we're doing something wrong, which is yet to be determined
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
	self->timer = poller_timer_make (data->poller);
	self->timer.user_data = self;
	self->timer.dispatcher = poller_pa_time_dispatcher;

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
	self->idle = poller_idle_make (data->poller);
	self->idle.user_data = self;
	self->idle.dispatcher = poller_pa_defer_dispatcher;

	poller_idle_set (&self->idle);
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
	(void) api;
	(void) retval;

	// This is not called from within libpulse
	hard_assert (!"quitting the libpulse event loop is unimplemented");
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
