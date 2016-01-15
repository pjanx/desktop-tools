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

// Sources: ddcciv1r1.pdf, i2c-dev.c in Linux, ddccontrol source code,
//   http://www.boichat.ch/nicolas/ddcci/specs.html was also helpful

// This makes openat() available even though I set _POSIX_C_SOURCE and
// _XOPEN_SOURCE to a version of POSIX older than 2008
#define _GNU_SOURCE

// Undo some dwm Makefile damage and import my everything-library
#include "config.h"
#undef PROGRAM_NAME
#define PROGRAM_NAME "brightness"
#include "liberty/liberty.c"

#include <sys/ioctl.h>
#include <dirent.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

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

#define FAIL(...)                                                              \
	BLOCK_START                                                                \
		error_set (e, __VA_ARGS__);                                            \
		return false;                                                          \
	BLOCK_END

static void
wait_ms (long ms)
{
	struct timespec ts = { 0, ms * 1000 * 1000 };
	nanosleep (&ts, NULL);
}

static bool
xstrtol (const char *s, long *out)
{
	char *end;
	errno = 0;
	*out = strtol (s, &end, 10);
	return errno == 0 && !*end && end != s;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#define DDC_LENGTH_XOR 0x80

enum
{
	DDC_ADDRESS_HOST          = 0x50,   ///< Bus master's base address
	DDC_ADDRESS_DISPLAY       = 0x6E    ///< The display's base address
};

enum { I2C_WRITE, I2C_READ };

enum
{
	DDC_GET_VCP_FEATURE       = 0x01,   ///< Request info about a feature
	DDC_GET_VCP_FEATURE_REPLY = 0x02,   ///< Feature info response
	DDC_SET_VCP_FEATURE       = 0x03    ///< Set or activate a feature
};

enum
{
	VCP_BRIGHTNESS            = 0x10,   ///< Standard VCP opcode for brightness
	VCP_CONTRAST              = 0x12    ///< Standard VCP opcode for contrast
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
check_edid (int fd, struct error **e)
{
	uint8_t edid_req[] = { 0x00 };
	uint8_t buf[8] = "";
	struct i2c_msg bufs[] =
	{
		{ .addr = 0x50, .flags = 0,
		  .len = 1, .buf = edid_req },
		{ .addr = 0x50, .flags = I2C_M_RD,
		  .len = sizeof buf, .buf = buf },
	};

	struct i2c_rdwr_ioctl_data data;
	data.msgs = bufs;
	data.nmsgs = 2;

	if (ioctl (fd, I2C_RDWR, &data) < 0)
		FAIL ("%s: %s", "ioctl", strerror (errno));
	if (memcmp ("\x00\xFF\xFF\xFF\xFF\xFF\xFF\x00", buf, sizeof buf))
		FAIL ("invalid EDID");
	return true;
}

static bool
is_a_display (int fd, struct error **e)
{
	struct stat st;
	if (fstat (fd, &st) < 0)
		FAIL ("%s: %s", "fstat", strerror (errno));

	unsigned long funcs;
	if (!(st.st_mode & S_IFCHR)
	 || ioctl (fd, I2C_FUNCS, &funcs) < 0
	 || !(funcs & I2C_FUNC_I2C))
		FAIL ("not an I2C device");

	return check_edid (fd, e);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
ddc_send (int fd, unsigned command, void *args, size_t args_len,
	struct error **e)
{
	struct str buf;
	str_init (&buf);
	str_pack_u8 (&buf, DDC_ADDRESS_HOST | I2C_READ);
	str_pack_u8 (&buf, DDC_LENGTH_XOR | (args_len + 1));
	str_pack_u8 (&buf, command);
	str_append_data (&buf, args, args_len);

	unsigned xor = DDC_ADDRESS_DISPLAY;
	for (size_t i = 0; i < buf.len; i++)
		xor ^= buf.str[i];
	str_pack_u8 (&buf, xor);

	struct i2c_msg msg =
	{
		// The driver unshifts it back
		.addr = DDC_ADDRESS_DISPLAY >> 1, .flags = 0,
		.len = buf.len, .buf = (uint8_t *) buf.str,
	};

	struct i2c_rdwr_ioctl_data data;
	data.msgs = &msg;
	data.nmsgs = 1;

	bool failed = ioctl (fd, I2C_RDWR, &data) < 0;
	str_free (&buf);
	if (failed)
		FAIL ("%s: %s", "ioctl", strerror (errno));
	return true;
}

static bool
ddc_read (int fd, unsigned *command, void *out_buf, size_t *n_read,
	struct error **e)
{
	uint8_t buf[128] = "";
	struct i2c_msg msg =
	{
		// The driver unshifts it back
		.addr = DDC_ADDRESS_DISPLAY >> 1, .flags = I2C_M_RD,
		.len = sizeof buf, .buf = buf,
	};

	struct i2c_rdwr_ioctl_data data;
	data.msgs = &msg;
	data.nmsgs = 1;

	if (ioctl (fd, I2C_RDWR, &data) < 0)
		FAIL ("%s: %s", "ioctl", strerror (errno));

	struct msg_unpacker unpacker;
	msg_unpacker_init (&unpacker, buf, sizeof buf);

	uint8_t sender, length, cmd;
	(void) msg_unpacker_u8 (&unpacker, &sender);
	(void) msg_unpacker_u8 (&unpacker, &length);
	(void) msg_unpacker_u8 (&unpacker, &cmd);

	if (sender != (DDC_ADDRESS_DISPLAY | I2C_WRITE) || !(length & 0x80))
		FAIL ("invalid response");
	if (!(length ^= 0x80))
		FAIL ("NULL response");

	// TODO: also check the checksum

	*command = cmd;
	memcpy (out_buf, unpacker.data + unpacker.offset, (*n_read = length - 1));
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
set_brightness (int fd, long diff, struct error **e)
{
	uint8_t get_req[] = { VCP_BRIGHTNESS };
	if (!ddc_send (fd, DDC_GET_VCP_FEATURE, get_req, sizeof get_req, e))
		return false;

	wait_ms (40);

	unsigned command = 0;
	uint8_t buf[128] = "";
	size_t len = 0;
	if (!ddc_read (fd, &command, buf, &len, e))
		return false;

	if (command != DDC_GET_VCP_FEATURE_REPLY || len != 7)
		FAIL ("invalid response");

	struct msg_unpacker unpacker;
	msg_unpacker_init (&unpacker, buf, len);

	uint8_t result;     msg_unpacker_u8  (&unpacker, &result);
	uint8_t vcp_opcode; msg_unpacker_u8  (&unpacker, &vcp_opcode);
	uint8_t type;       msg_unpacker_u8  (&unpacker, &type);
	int16_t max;        msg_unpacker_i16 (&unpacker, &max);
	int16_t cur;        msg_unpacker_i16 (&unpacker, &cur);

	if (result == 0x01)
		FAIL ("error reported by monitor");

	if (result != 0x00
	 || vcp_opcode != VCP_BRIGHTNESS)
		FAIL ("invalid response");

	// These are unsigned but usually just one byte long
	if (max < 0 || cur < 0)
		FAIL ("capability range overflow");

	int16_t req = (cur * 100 + diff * max + 50) / 100;
	if (req > max) req = max;
	if (req < 0)   req = 0;

	uint8_t set_req[] = { VCP_BRIGHTNESS, req >> 8, req };
	if (!ddc_send (fd, DDC_SET_VCP_FEATURE, set_req, sizeof set_req, e))
		return false;

	wait_ms (50);

	printf ("brightness set to %.2f%%\n", 100. * req / max);
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
	{
		error_set (e, "invalid range or current value");
		return false;
	}

	long req = (cur * 100 + diff * max + 50) / 100;
	if (req > max) req = max;
	if (req < 0)   req = 0;

	int fd = openat (dir, "brightness", O_WRONLY);
	if (fd < 0)
	{
		error_set (e, "%s: %s: %s", "brightness", "openat", strerror (errno));
		return false;
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

