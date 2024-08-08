/*
 * fancontrol-ng.c: clone of fancontrol from lm_sensors
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
#define PROGRAM_NAME "fancontrol-ng"
#include "liberty/liberty.c"

// --- Main program ------------------------------------------------------------

struct device
{
	LIST_HEADER (struct device)

	struct app_context *ctx;            ///< Application context
	struct config_item *config;         ///< Configuration root for the device
	char *path;                         ///< Base path

	struct poller_timer timer;          ///< Refresh timer
};

struct app_context
{
	struct poller poller;               ///< Poller
	bool polling;                       ///< The event loop is running

	struct config_item *config;         ///< Program configuration
	struct device *devices;             ///< All devices
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
log_message_custom (void *user_data, const char *quote, const char *fmt,
	va_list ap)
{
	(void) user_data;
	FILE *stream = stdout;

	// TODO: sd-daemon.h log level prefixes?
	fputs (quote, stream);
	vfprintf (stream, fmt, ap);
	fputs ("\n", stream);
}

static char *
read_file_cstr (const char *path, struct error **e)
{
	struct str s = str_make ();
	if (read_file (path, &s, e))
		return str_steal (&s);
	str_free (&s);
	return NULL;
}

static int64_t
read_file_unsigned (const char *path, struct error **e)
{
	char *s, *end;
	if (!(s = read_file_cstr (path, e)))
		return -1;
	if ((end = strpbrk (s, "\r\n")))
		*end = 0;

	unsigned long num;
	bool ok = xstrtoul (&num, s, 10);
	free (s);

	if (!ok || num > INT64_MAX)
	{
		error_set (e, "error reading `%s': %s", path, "invalid integer value");
		return -1;
	}
	return num;
}

static bool
write_file_printf (const char *path, struct error **e, const char *format, ...)
	ATTRIBUTE_PRINTF (3, 4);

static bool
write_file_printf (const char *path, struct error **e, const char *format, ...)
{
	va_list ap;
	va_start (ap, format);
	struct str s = str_make ();
	str_append_vprintf (&s, format, ap);
	va_end (ap);

	bool success = write_file (path, s.str, s.len, e);
	str_free (&s);
	return success;
}

// --- Configuration -----------------------------------------------------------

static bool
config_validate_nonnegative (const struct config_item *item, struct error **e)
{
	if (item->type == CONFIG_ITEM_NULL)
		return true;

	hard_assert (item->type == CONFIG_ITEM_INTEGER);
	if (item->value.integer >= 0)
		return true;

	return error_set (e, "must be non-negative");
}

static const struct config_schema g_config_device[] =
{
	{ .name      = "name",
	  .comment   = "Device identifier",
	  .type      = CONFIG_ITEM_STRING },
	{ .name      = "interval",
	  .comment   = "Temperature checking interval",
	  .type      = CONFIG_ITEM_INTEGER,
	  .validate  = config_validate_nonnegative,
	  .default_  = "5" },
	{}
};

static const struct config_schema g_config_pwm[] =
{
	{ .name      = "temp",
	  .comment   = "Path to temperature sensor output",
	  .type      = CONFIG_ITEM_STRING },
	{ .name      = "min_temp",
	  .comment   = "Temperature for no fan operation",
	  .type      = CONFIG_ITEM_INTEGER,
	  .validate  = config_validate_nonnegative,
	  .default_  = "40" },
	{ .name      = "max_temp",
	  .comment   = "Temperature for maximum fan operation",
	  .type      = CONFIG_ITEM_INTEGER,
	  .validate  = config_validate_nonnegative,
	  .default_  = "80" },
	{ .name      = "min_start",
	  .comment   = "Minimum value for the fan to start spinning",
	  .type      = CONFIG_ITEM_INTEGER,
	  .validate  = config_validate_nonnegative,
	  .default_  = "0" },
	{ .name      = "min_stop",
	  .comment   = "Mimimum value for the fan to stop spinning",
	  .type      = CONFIG_ITEM_INTEGER,
	  .validate  = config_validate_nonnegative,
	  .default_  = "0" },
	{ .name      = "pwm_min",
	  .comment   = "Minimum PWM value to use",
	  .type      = CONFIG_ITEM_INTEGER,
	  .validate  = config_validate_nonnegative },
	{ .name      = "pwm_max",
	  .comment   = "Maximum PWM value to use",
	  .type      = CONFIG_ITEM_INTEGER,
	  .validate  = config_validate_nonnegative },
	{}
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static int64_t
get_config_integer (struct config_item *root, const char *key)
{
	struct config_item *item = config_item_get (root, key, NULL);
	hard_assert (item && item->type == CONFIG_ITEM_INTEGER);
	return item->value.integer;
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

// --- Fan control -------------------------------------------------------------

// Consider this a failed attempt to avoid creating special PWM objects
// based on the configuration.  The complexity just moved somewhere else.

struct paths
{
	char *temp;                         ///< Current temperature
	char *pwm;                          ///< Current PWM value
	char *pwm_enable;                   ///< PWM control state
	char *pwm_min;                      ///< Minimum PWM value
	char *pwm_max;                      ///< Maximum PWM value
};

static struct paths *
paths_new (const char *device_path, const char *path, struct config_item *pwm)
{
	struct paths *self = xcalloc (1, sizeof *self);
	self->temp = xstrdup_printf
		("%s/%s", device_path, get_config_string (pwm, "temp"));

	self->pwm        = xstrdup_printf ("%s/%s",        device_path, path);
	self->pwm_enable = xstrdup_printf ("%s/%s_enable", device_path, path);
	self->pwm_min    = xstrdup_printf ("%s/%s_min",    device_path, path);
	self->pwm_max    = xstrdup_printf ("%s/%s_max",    device_path, path);
	return self;
}

static void
paths_destroy (struct paths *self)
{
	cstr_set (&self->temp, NULL);

	cstr_set (&self->pwm, NULL);
	cstr_set (&self->pwm_enable, NULL);
	cstr_set (&self->pwm_min, NULL);
	cstr_set (&self->pwm_max, NULL);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
pwm_update (struct paths *paths, struct config_item *pwm, struct error **e)
{
	int64_t cur_enable, cur_temp, cur_pwm, pwm_min, pwm_max;
	if ((cur_enable = read_file_unsigned (paths->pwm_enable, e)) < 0
	 || (cur_temp   = read_file_unsigned (paths->temp,       e)) < 0
	 || (cur_pwm    = read_file_unsigned (paths->pwm,        e)) < 0)
		return false;

	struct config_item *pwm_min_item = config_item_get (pwm, "pwm_min", NULL);
	if (pwm_min_item->type == CONFIG_ITEM_INTEGER)
		pwm_min = pwm_min_item->value.integer;
	else if ((pwm_min = read_file_unsigned (paths->pwm_min, NULL)) < 0)
		pwm_min = 0;

	struct config_item *pwm_max_item = config_item_get (pwm, "pwm_max", NULL);
	if (pwm_max_item->type == CONFIG_ITEM_INTEGER)
		pwm_max = pwm_max_item->value.integer;
	else if ((pwm_max = read_file_unsigned (paths->pwm_max, NULL)) < 0)
		pwm_max = 255;

	int64_t min_temp  = get_config_integer (pwm, "min_temp");
	int64_t max_temp  = get_config_integer (pwm, "max_temp");
	int64_t min_start = get_config_integer (pwm, "min_start");
	int64_t min_stop  = get_config_integer (pwm, "min_stop");

#define FAIL(...) error_set (e, __VA_ARGS__)
	if (min_temp >= max_temp)  FAIL ("min_temp must be less than max_temp");
	if (pwm_max > 255)         FAIL ("pwm_max must be at most 255");
	if (min_stop >= pwm_max)   FAIL ("min_stop must be less than pwm_max");
	if (min_stop < pwm_min)    FAIL ("min_stop must be at least pwm_min");
#undef FAIL

	// I'm not sure if this strangely complicated computation is justifiable
	double where
		= ((double) cur_temp / 1000 - min_temp)
		/ ((double) max_temp        - min_temp);

	int64_t new_pwm;
	if      (where <= 0) new_pwm = pwm_min;
	else if (where >= 1) new_pwm = pwm_max;
	else
	{
		new_pwm = min_stop + where * (pwm_max - min_stop);

		// If needed, we start the fan until next iteration
		if (cur_pwm <= min_stop)
			new_pwm = MAX (new_pwm, min_start);
	}

	new_pwm = MAX (new_pwm, pwm_min);
	new_pwm = MIN (new_pwm, pwm_max);

	if (cur_enable != 1 && !write_file_printf (paths->pwm_enable, e, "1"))
		return false;
	if (!write_file_printf (paths->pwm, e, "%" PRId64, new_pwm))
		return false;
	return true;
}

static bool
pwm_set_enable (struct paths *paths, char value)
{
	struct error *e = NULL;
	if (write_file (paths->pwm_enable, &value, 1, &e))
		return true;

	print_error ("failed to change PWM mode to %c: %s",
		value, e->message);
	error_free (e);
	return false;
}

static bool
pwm_give_up (struct paths *paths)
{
	// Try automatic control, and if that fails, go full speed
	return pwm_set_enable (paths, '2') || pwm_set_enable (paths, '0');
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct pwm_iter
{
	struct str_map_iter object_iter;    ///< Configuration iterator
	struct device *device;              ///< Device

	struct config_item *pwm;            ///< PWM
	const char *pwm_path;               ///< PWM path
	struct paths *paths;                ///< Paths
};

static void
pwm_iter_init (struct pwm_iter *self, struct device *device)
{
	self->object_iter = str_map_iter_make
		(&config_item_get (device->config, "pwms", NULL)->value.object);
	self->device = device;
	self->paths = NULL;
}

static void
pwm_iter_free (struct pwm_iter *self)
{
	if (self->paths)
	{
		paths_destroy (self->paths);
		self->paths = NULL;
	}
}

static bool
pwm_iter_next (struct pwm_iter *self)
{
	pwm_iter_free (self);
	if (!(self->pwm = str_map_iter_next (&self->object_iter)))
		return false;

	self->pwm_path = self->object_iter.link->key;
	self->paths = paths_new (self->device->path, self->pwm_path, self->pwm);
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
device_run (struct device *self)
{
	struct pwm_iter iter;
	pwm_iter_init (&iter, self);

	while (pwm_iter_next (&iter))
	{
		struct error *e = NULL;
		if (pwm_update (iter.paths, iter.pwm, &e))
			continue;

		print_error ("pwm `%s': %s", iter.pwm_path, e->message);
		error_free (e);
		pwm_give_up (iter.paths);
	}

	pwm_iter_free (&iter);

	poller_timer_set (&self->timer,
		1000 * get_config_integer (self->config, "interval"));
}

static void
device_stop (struct device *self)
{
	struct pwm_iter iter;
	pwm_iter_init (&iter, self);

	while (pwm_iter_next (&iter))
		pwm_give_up (iter.paths);

	pwm_iter_free (&iter);
}

static void
device_create (struct app_context *ctx, const char *path,
	struct config_item *root)
{
	struct device *self = xcalloc (1, sizeof *self);
	self->config = root;
	self->path = xstrdup (path);

	self->timer = poller_timer_make (&ctx->poller);
	self->timer.dispatcher = (poller_timer_fn) device_run;
	self->timer.user_data = self;

	LIST_PREPEND (ctx->devices, self);
}

// --- Configuration -----------------------------------------------------------

// There is no room for errors in the configuration, everything must be valid.
// Thus the reset to defaults on invalid values is effectively disabled here.
static bool
apply_schema (const struct config_schema *schema, struct config_item *object,
	struct error **e)
{
	struct error *warning = NULL, *error = NULL;
	config_schema_initialize_item (schema, object, NULL, &warning, &error);

	if (error && warning)
	{
		error_free (warning);
		error_propagate (e, error);
		return false;
	}
	if (error)
	{
		error_propagate (e, error);
		return false;
	}
	if (warning)
	{
		// The standard warning is inappropriate here
		error_free (warning);
		return error_set (e, "invalid item `%s'", schema->name);
	}
	return true;
}

static bool
check_device_configuration (struct config_item *subtree, struct error **e)
{
	// Check regular fields in the device object
	for (const struct config_schema *s = g_config_device; s->name; s++)
		if (!apply_schema (s, subtree, e))
			return false;

	// Check for a subobject with PWMs to control
	struct config_item *pwms;
	if (!(pwms = config_item_get (subtree, "pwms", e)))
		return false;
	if (pwms->type != CONFIG_ITEM_OBJECT)
		return error_set (e, "`%s' is not an object", "pwms");
	if (!pwms->value.object.len)
		return error_set (e, "no PWMs defined");

	// Check regular fields in all PWM subobjects
	struct str_map_iter iter = str_map_iter_make (&pwms->value.object);
	struct config_item *pwm;
	struct error *error = NULL;
	while ((pwm = str_map_iter_next (&iter)))
	{
		const char *subpath = iter.link->key;
		for (const struct config_schema *s = g_config_pwm; s->name; s++)
			if (!apply_schema (s, pwm, &error))
			{
				error_set (e, "PWM `%s': %s", subpath, error->message);
				error_free (error);
				return false;
			}
		if (!get_config_string (pwm, "temp"))
		{
			return error_set (e,
				"PWM `%s': %s", subpath, "`temp' cannot be null");
		}
	}
	return true;
}

static void
load_configuration (struct app_context *ctx, const char *config_path)
{
	struct error *e = NULL;
	struct config_item *root = config_read_from_file (config_path, &e);

	if (e)
	{
		print_error ("error loading configuration: %s", e->message);
		error_free (e);
		exit (EXIT_FAILURE);
	}

	struct str_map_iter iter =
		str_map_iter_make (&(ctx->config = root)->value.object);

	struct config_item *subtree;
	while ((subtree = str_map_iter_next (&iter)))
	{
		const char *path = iter.link->key;
		if (subtree->type != CONFIG_ITEM_OBJECT)
			exit_fatal ("device `%s' in configuration is not an object", path);
		else if (!check_device_configuration (subtree, &e))
			exit_fatal ("device `%s': %s", path, e->message);
		else
			device_create (ctx, path, subtree);
	}
}

// --- Signals -----------------------------------------------------------------

static int g_signal_pipe[2];            ///< A pipe used to signal... signals

static void
sigterm_handler (int signum)
{
	(void) signum;

	int original_errno = errno;
	if (write (g_signal_pipe[1], "", 1) == -1)
		soft_assert (errno == EAGAIN);
	errno = original_errno;
}

static void
setup_signal_handlers (void)
{
	if (pipe (g_signal_pipe) == -1)
		exit_fatal ("%s: %s", "pipe", strerror (errno));

	set_cloexec (g_signal_pipe[0]);
	set_cloexec (g_signal_pipe[1]);

	// So that the pipe cannot overflow; it would make write() block within
	// the signal handler, which is something we really don't want to happen.
	// The same holds true for read().
	set_blocking (g_signal_pipe[0], false);
	set_blocking (g_signal_pipe[1], false);

	(void) signal (SIGPIPE, SIG_IGN);

	struct sigaction sa;
	sa.sa_flags = SA_RESTART;
	sa.sa_handler = sigterm_handler;
	sigemptyset (&sa.sa_mask);

	if (sigaction (SIGINT,  &sa, NULL) == -1
	 || sigaction (SIGTERM, &sa, NULL) == -1)
		exit_fatal ("sigaction: %s", strerror (errno));
}

// --- Main program ------------------------------------------------------------

static void
on_signal_pipe_readable (const struct pollfd *fd, struct app_context *ctx)
{
	char id = 0;
	(void) read (fd->fd, &id, 1);

	ctx->polling = false;
}

static const char *
parse_program_arguments (int argc, char **argv)
{
	static const struct opt opts[] =
	{
		{ 'd', "debug", NULL, 0, "run in debug mode" },
		{ 'h', "help", NULL, 0, "display this help and exit" },
		{ 'V', "version", NULL, 0, "output version information and exit" },
		{ 0, NULL, NULL, 0, NULL }
	};

	struct opt_handler oh =
		opt_handler_make (argc, argv, opts, "CONFIG", "Fan controller.");

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

	if (argc != 1)
	{
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}

	opt_handler_free (&oh);
	return argv[0];
}

int
main (int argc, char *argv[])
{
	g_log_message_real = log_message_custom;
	const char *config_path = parse_program_arguments (argc, argv);

	struct app_context ctx;
	memset (&ctx, 0, sizeof ctx);
	poller_init (&ctx.poller);

	setup_signal_handlers ();

	struct poller_fd signal_event;
	signal_event = poller_fd_make (&ctx.poller, g_signal_pipe[0]);
	signal_event.dispatcher = (poller_fd_fn) on_signal_pipe_readable;
	signal_event.user_data = &ctx;
	poller_fd_set (&signal_event, POLLIN);

	load_configuration (&ctx, config_path);

	if (!ctx.devices)
		exit_fatal ("no devices present in configuration");
	LIST_FOR_EACH (struct device, iter, ctx.devices)
		device_run (iter);

	ctx.polling = true;
	while (ctx.polling)
		poller_run (&ctx.poller);

	LIST_FOR_EACH (struct device, iter, ctx.devices)
		device_stop (iter);

	config_item_destroy (ctx.config);
	poller_free (&ctx.poller);
	return EXIT_SUCCESS;
}
