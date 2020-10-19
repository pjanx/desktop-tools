/*
 * big-brother.c: activity tracker
 *
 * Copyright (c) 2016, Přemysl Eric Janouch <p@janouch.name>
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

#include "config.h"
#undef PROGRAM_NAME
#define PROGRAM_NAME "big-brother"
#include "liberty/liberty.c"

#include <locale.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/sync.h>

// --- Utilities ---------------------------------------------------------------

static int64_t
clock_msec (clockid_t clock)
{
	struct timespec tp;
	hard_assert (clock_gettime (clock, &tp) != -1);
	return (int64_t) tp.tv_sec * 1000 + (int64_t) tp.tv_nsec / 1000000;
}

static char *
timestamp (int64_t ts)
{
	char buf[24];
	struct tm tm;
	time_t when = ts / 1000;
	strftime (buf, sizeof buf, "%F %T", gmtime_r (&when, &tm));
	return xstrdup_printf ("%s.%03d", buf, (int) (ts % 1000));
}

static void
log_message_custom (void *user_data, const char *quote, const char *fmt,
	va_list ap)
{
	(void) user_data;
	FILE *stream = stdout;

	char *ts = timestamp (clock_msec (CLOCK_REALTIME));
	fprintf (stream, "%s ", ts);
	free (ts);

	fputs (quote, stream);
	vfprintf (stream, fmt, ap);
	fputs ("\n", stream);
}

// --- Configuration -----------------------------------------------------------

static struct simple_config_item g_config_table[] =
{
	{ "idle_timeout",    "600",             "Timeout for user inactivity (s)" },
	{ NULL,              NULL,              NULL                              }
};

// --- Application -------------------------------------------------------------

struct app_context
{
	struct str_map config;              ///< Program configuration
	struct poller poller;               ///< Poller
	bool running;                       ///< Event loop is running

	Display *dpy;                       ///< X display handle
	struct poller_fd x_event;           ///< X11 event

	Atom net_active_window;             ///< _NET_ACTIVE_WINDOW
	Atom net_wm_name;                   ///< _NET_WM_NAME

	// Window title tracking

	char *current_title;                ///< Current window title or NULL
	Window current_window;              ///< Current window

	// XSync activity tracking

	int xsync_base_event_code;          ///< XSync base event code
	XSyncCounter idle_counter;          ///< XSync IDLETIME counter
	XSyncValue idle_timeout;            ///< Idle timeout

	XSyncAlarm idle_alarm_inactive;     ///< User is inactive
	XSyncAlarm idle_alarm_active;       ///< User is active
};

static void
app_context_init (struct app_context *self)
{
	memset (self, 0, sizeof *self);

	self->config = str_map_make (free);
	simple_config_load_defaults (&self->config, g_config_table);

	if (!(self->dpy = XOpenDisplay (NULL)))
		exit_fatal ("cannot open display");

	poller_init (&self->poller);
	self->x_event = poller_fd_make (&self->poller,
		ConnectionNumber (self->dpy));

	self->net_active_window =
		XInternAtom (self->dpy, "_NET_ACTIVE_WINDOW", true);
	self->net_wm_name =
		XInternAtom (self->dpy, "_NET_WM_NAME", true);

	// TODO: it is possible to employ a fallback mechanism via XScreenSaver
	//   by polling the XScreenSaverInfo::idle field, see
	//   https://www.x.org/releases/X11R7.5/doc/man/man3/Xss.3.html

	int n;
	if (!XSyncQueryExtension (self->dpy, &self->xsync_base_event_code, &n)
	 || !XSyncInitialize (self->dpy, &n, &n))
		exit_fatal ("cannot initialize XSync");

	// The idle counter is not guaranteed to exist, only SERVERTIME is
	XSyncSystemCounter *counters = XSyncListSystemCounters (self->dpy, &n);
	while (n--)
	{
		if (!strcmp (counters[n].name, "IDLETIME"))
			self->idle_counter = counters[n].counter;
	}
	if (!self->idle_counter)
		exit_fatal ("idle counter is missing");
	XSyncFreeSystemCounterList (counters);
}

static void
app_context_free (struct app_context *self)
{
	str_map_free (&self->config);
	cstr_set (&self->current_title, NULL);
	poller_fd_reset (&self->x_event);
	XCloseDisplay (self->dpy);
	poller_free (&self->poller);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static char *
x_text_property_to_utf8 (struct app_context *ctx, XTextProperty *prop)
{
#if ARE_WE_UTF8_YET
	Atom utf8_string = XInternAtom (ctx->dpy, "UTF8_STRING", true);
	if (prop->encoding == utf8_string)
		return xstrdup ((char *) prop->value);
#endif

	int n = 0;
	char **list = NULL;
	if (XmbTextPropertyToTextList (ctx->dpy, prop, &list, &n) >= Success
		&& n > 0 && *list)
	{
		// TODO: convert from locale encoding into UTF-8
		char *result = xstrdup (*list);
		XFreeStringList (list);
		return result;
	}
	return NULL;
}

static char *
x_text_property (struct app_context *ctx, Window window, Atom atom)
{
	XTextProperty name;
	XGetTextProperty (ctx->dpy, window, &name, atom);
	if (!name.value)
		return NULL;

	char *result = x_text_property_to_utf8 (ctx, &name);
	XFree (name.value);
	return result;
}

static char *
x_window_title (struct app_context *ctx, Window window)
{
	char *title;
	if (!(title = x_text_property (ctx, window, ctx->net_wm_name))
	 && !(title = x_text_property (ctx, window, XA_WM_NAME)))
		title = xstrdup ("broken");
	return title;
}

static bool
update_window_title (struct app_context *ctx, char *new_title)
{
	bool changed = !ctx->current_title != !new_title
		|| (new_title && strcmp (ctx->current_title, new_title));
	cstr_set (&ctx->current_title, new_title);
	return changed;
}

static void
update_current_window (struct app_context *ctx)
{
	Window root = DefaultRootWindow (ctx->dpy);

	Atom dummy_type; int dummy_format;
	unsigned long nitems, dummy_bytes;
	unsigned char *p = NULL;
	if (XGetWindowProperty (ctx->dpy, root, ctx->net_active_window,
		0L, 1L, false, XA_WINDOW, &dummy_type, &dummy_format,
		&nitems, &dummy_bytes, &p) != Success)
		return;

	char *new_title = NULL;
	if (nitems)
	{
		Window active_window = *(Window *) p;
		XFree (p);

		if (ctx->current_window != active_window && ctx->current_window)
			XSelectInput (ctx->dpy, ctx->current_window, 0);

		XSelectInput (ctx->dpy, active_window, PropertyChangeMask);
		new_title = x_window_title (ctx, active_window);
		ctx->current_window = active_window;
	}
	if (update_window_title (ctx, new_title))
		print_status ("Window changed: %s",
			ctx->current_title ? ctx->current_title : "(none)");
}

static void
on_x_property_notify (struct app_context *ctx, XPropertyEvent *ev)
{
	// This is from the EWMH specification, set by the window manager
	if (ev->atom == ctx->net_active_window)
		update_current_window (ctx);
	else if (ev->window == ctx->current_window && ev->atom == ctx->net_wm_name)
	{
		if (update_window_title (ctx, x_window_title (ctx, ev->window)))
			print_status ("Title changed: %s", ctx->current_title);
	}
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
		print_status ("User is inactive");

		XSyncValue one, minus_one;
		XSyncIntToValue (&one, 1);

		Bool overflow;
		XSyncValueSubtract (&minus_one, ev->counter_value, one, &overflow);

		// Set an alarm for IDLETIME <= current_idletime - 1
		set_idle_alarm (ctx, &ctx->idle_alarm_active,
			XSyncNegativeComparison, minus_one);
	}
	else if (ev->alarm == ctx->idle_alarm_active)
	{
		print_status ("User is active");
		set_idle_alarm (ctx, &ctx->idle_alarm_inactive,
			XSyncPositiveComparison, ctx->idle_timeout);
	}
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
		else if (ev.type == PropertyNotify)
			on_x_property_notify (ctx, &ev.xproperty);
		else if (ev.type == ctx->xsync_base_event_code + XSyncAlarmNotify)
			on_x_alarm_notify (ctx, (XSyncAlarmNotifyEvent *) &ev);
	}
}

static XErrorHandler g_default_x_error_handler;

static int
on_x_error (Display *dpy, XErrorEvent *ee)
{
	// This just is going to happen since those windows aren't ours
	if (ee->error_code == BadWindow)
		return 0;

	return g_default_x_error_handler (dpy, ee);
}

static void
init_events (struct app_context *ctx)
{
	Window root = DefaultRootWindow (ctx->dpy);
	XSelectInput (ctx->dpy, root, PropertyChangeMask);
	XSync (ctx->dpy, False);

	g_default_x_error_handler = XSetErrorHandler (on_x_error);

	unsigned long n;
	const char *timeout = str_map_find (&ctx->config, "idle_timeout");
	if (!xstrtoul (&n, timeout, 10) || !n || n > INT_MAX / 1000)
		exit_fatal ("invalid value for the idle timeout");
	XSyncIntToValue (&ctx->idle_timeout, n * 1000);

	update_current_window (ctx);
	set_idle_alarm (ctx, &ctx->idle_alarm_inactive,
		XSyncPositiveComparison, ctx->idle_timeout);

	ctx->x_event.dispatcher = on_x_ready;
	ctx->x_event.user_data = ctx;
	poller_fd_set (&ctx->x_event, POLLIN);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

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

	struct opt_handler oh =
		opt_handler_make (argc, argv, opts, NULL, "Activity tracker.");

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

	if (!setlocale (LC_CTYPE, ""))
		exit_fatal ("cannot set locale");
	if (!XSupportsLocale ())
		exit_fatal ("locale not supported by Xlib");

	struct app_context ctx;
	app_context_init (&ctx);

	struct error *e = NULL;
	if (!simple_config_update_from_file (&ctx.config, &e))
		exit_fatal ("%s", e->message);

	init_events (&ctx);

	ctx.running = true;
	while (ctx.running)
		poller_run (&ctx.poller);

	app_context_free (&ctx);
	return 0;
}
