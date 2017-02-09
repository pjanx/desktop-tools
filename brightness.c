/*
 * brightness.c: set display brightness via DDC/CI - Linux only
 *
 * Copyright (c) 2015, Přemysl Janouch <p.janouch@gmail.com>
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

// This makes openat() available even though I set _POSIX_C_SOURCE and
// _XOPEN_SOURCE to a version of POSIX older than 2008
#define _GNU_SOURCE

#include "config.h"
#undef PROGRAM_NAME
#define PROGRAM_NAME "brightness"
#include "liberty/liberty.c"

#include "ddc-ci.c"
#include <dirent.h>

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
xstrtol (const char *s, long *out)
{
	char *end;
	errno = 0;
	*out = strtol (s, &end, 10);
	return errno == 0 && !*end && end != s;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
set_brightness (int fd, long diff, struct error **e)
{
	struct vcp_feature_readout readout = {};
	if (!vcp_get_feature (fd, VCP_BRIGHTNESS, &readout, e))
		return false;

	int16_t req = (readout.cur * 100 + diff * readout.max + 50) / 100;
	req = MIN (req, readout.max);
	req = MAX (req, 0);

	uint8_t set_req[] = { VCP_BRIGHTNESS, req >> 8, req };
	if (!ddc_send (fd, DDC_SET_VCP_FEATURE, set_req, sizeof set_req, e))
		return false;

	wait_ms (50);

	printf ("brightness set to %.2f%%\n", 100. * req / readout.max);
	return true;
}

static void
i2c (long diff)
{
	DIR *dev = opendir ("/dev");
	if (!dev)
	{
		print_error ("cannot access %s: %s: %s",
			"/dev", "opendir", strerror (errno));
		return;
	}

	struct dirent *entry;
	while ((entry = readdir (dev)))
	{
		if (strncmp (entry->d_name, "i2c-", 4))
			continue;

		printf ("Trying %s... ", entry->d_name);
		int fd = openat (dirfd (dev), entry->d_name, O_RDONLY);
		if (fd < 0)
		{
			print_error ("%s: %s", "openat", strerror (errno));
			continue;
		}

		struct error *e = NULL;
		if (!is_a_display (fd, &e)
		 || !set_brightness (fd, diff, &e))
		{
			printf ("%s\n", e->message);
			error_free (e);
		}

		close (fd);
	}
	closedir (dev);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static long
read_value (int dir, const char *filename, struct error **e)
{
	int fd = openat (dir, filename, O_RDONLY);
	if (fd < 0)
	{
		error_set (e, "%s: %s: %s", filename, "openat", strerror (errno));
		return -1;
	}

	FILE *fp = fdopen (fd, "r");
	if (!fp)
	{
		error_set (e, "%s: %s: %s", filename, "fdopen", strerror (errno));
		close (fd);
		return -1;
	}

	struct str s;
	str_init (&s);

	long value;
	if (!read_line (fp, &s)
	 || !xstrtol (s.str, &value))
	{
		value = -1;
		error_set (e, "%s: %s", filename, "failed reading an integer value");
	}

	str_free (&s);
	fclose (fp);
	return value;
}

static bool
set_backlight (int dir, long diff, struct error **e)
{
	long cur, max;
	struct error *error = NULL;
	if ((cur = read_value (dir, "brightness", &error), error)
	 || (max = read_value (dir, "max_brightness", &error), error))
	{
		error_propagate (e, error);
		return false;
	}

	if (cur < 0 || max < 0)
		return error_set (e, "invalid range or current value");

	long req = (cur * 100 + diff * max + 50) / 100;
	if (req > max) req = max;
	if (req < 0)   req = 0;

	int fd = openat (dir, "brightness", O_WRONLY);
	if (fd < 0)
	{
		return error_set (e,
			"%s: %s: %s", "brightness", "openat", strerror (errno));
	}

	struct str s;
	str_init (&s);
	str_append_printf (&s, "%ld", req);
	bool result = write (fd, s.str, s.len) == (ssize_t) s.len;
	str_free (&s);

	if (!result)
		error_set (e, "%s: %s: %s", "brightness", "write", strerror (errno));

	close (fd);
	printf ("brightness set to %.2f%%\n", 100. * req / max);
	return result;
}

static void
backlight (long diff)
{
	DIR *backlight = opendir ("/sys/class/backlight");
	if (!backlight)
	{
		print_error ("cannot access %s: %s: %s",
			"/sys/class/backlight", "opendir", strerror (errno));
		return;
	}

	struct dirent *entry;
	while ((entry = readdir (backlight)))
	{
		const char *device_name = entry->d_name;
		if (device_name[0] == '.')
			continue;

		printf ("Trying %s... ", entry->d_name);
		int dir = openat (dirfd (backlight), entry->d_name, O_RDONLY);
		if (dir < 0)
		{
			print_error ("%s: %s", "openat", strerror (errno));
			continue;
		}

		struct error *e = NULL;
		if (!set_backlight (dir, diff, &e))
		{
			printf ("%s\n", e->message);
			error_free (e);
		}

		close (dir);
	}
	closedir (backlight);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

int
main (int argc, char *argv[])
{
	g_log_message_real = log_message_custom;

	long diff = 0;
	if (argc > 1 && !xstrtol (argv[1], &diff))
	{
		printf ("Usage: %s <percentage diff>\n", argv[0]);
		exit (EXIT_FAILURE);
	}

	i2c (diff);
	backlight (diff);
	return 0;
}

