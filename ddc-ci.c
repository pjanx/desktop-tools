/*
 * ddc-ci.c: DDC-CI utilities, Linux-only
 *
 * Copyright (c) 2015 - 2017, Přemysl Janouch <p.janouch@gmail.com>
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

#include <sys/ioctl.h>

#include <linux/i2c-dev.h>
#ifndef I2C_FUNC_I2C
// Fuck you, openSUSE, for fucking up the previous file, see e.g.
// https://github.com/solettaproject/soletta/commit/427f47f
#include <linux/i2c.h>
#endif

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

static void
wait_ms (long ms)
{
	struct timespec ts = { 0, ms * 1000 * 1000 };
	nanosleep (&ts, NULL);
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
	VCP_CONTRAST              = 0x12,   ///< Standard VCP opcode for contrast
	VCP_INPUT_SOURCE          = 0x60    ///< Standard VCP opcode for input
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
		return error_set (e, "%s: %s", "ioctl", strerror (errno));
	if (memcmp ("\x00\xFF\xFF\xFF\xFF\xFF\xFF\x00", buf, sizeof buf))
		return error_set (e, "invalid EDID");
	return true;
}

static bool
is_a_display (int fd, struct error **e)
{
	struct stat st;
	if (fstat (fd, &st) < 0)
		return error_set (e, "%s: %s", "fstat", strerror (errno));

	unsigned long funcs;
	if (!(st.st_mode & S_IFCHR)
	 || ioctl (fd, I2C_FUNCS, &funcs) < 0
	 || !(funcs & I2C_FUNC_I2C))
		return error_set (e, "not an I2C device");

	return check_edid (fd, e);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
ddc_send (int fd, unsigned command, void *args, size_t args_len,
	struct error **e)
{
	struct str buf = str_make ();
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
		return error_set (e, "%s: %s", "ioctl", strerror (errno));
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
		return error_set (e, "%s: %s", "ioctl", strerror (errno));

	struct msg_unpacker unpacker = msg_unpacker_make (buf, sizeof buf);

	uint8_t sender, length, cmd;
	(void) msg_unpacker_u8 (&unpacker, &sender);
	(void) msg_unpacker_u8 (&unpacker, &length);
	(void) msg_unpacker_u8 (&unpacker, &cmd);

	if (sender != (DDC_ADDRESS_DISPLAY | I2C_WRITE) || !(length & 0x80))
		return error_set (e, "invalid response");
	if (!(length ^= 0x80))
		return error_set (e, "NULL response");

	// TODO: also check the checksum

	*command = cmd;
	memcpy (out_buf, unpacker.data + unpacker.offset, (*n_read = length - 1));
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct vcp_feature_readout
{
	uint8_t type;                       ///< Type of the value
	int16_t max;                        ///< Maximum value
	int16_t cur;                        ///< Current value
};

static bool
vcp_get_feature (int fd, uint8_t feature, struct vcp_feature_readout *out,
	struct error **e)
{
	uint8_t get_req[] = { feature };
	if (!ddc_send (fd, DDC_GET_VCP_FEATURE, get_req, sizeof get_req, e))
		return false;

	wait_ms (40);

	unsigned command = 0;
	uint8_t buf[128] = "";
	size_t len = 0;
	if (!ddc_read (fd, &command, buf, &len, e))
		return false;

	if (command != DDC_GET_VCP_FEATURE_REPLY || len != 7)
		return error_set (e, "invalid response");

	struct msg_unpacker unpacker = msg_unpacker_make (buf, len);

	uint8_t result;     msg_unpacker_u8  (&unpacker, &result);
	uint8_t vcp_opcode; msg_unpacker_u8  (&unpacker, &vcp_opcode);
	msg_unpacker_u8  (&unpacker, &out->type);
	msg_unpacker_i16 (&unpacker, &out->max);
	msg_unpacker_i16 (&unpacker, &out->cur);

	if (result == 0x01)
		return error_set (e, "error reported by monitor");

	if (result != 0x00
	 || vcp_opcode != feature)
		return error_set (e, "invalid response");

	// These are unsigned but usually just one byte long
	if (out->max < 0 || out->cur < 0)
		return error_set (e, "capability range overflow");
	return true;
}

