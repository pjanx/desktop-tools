/*
 * input-switch.c: switches display input via DDC/CI
 *
 * Copyright (c) 2017 - 2022, Přemysl Eric Janouch <p@janouch.name>
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

// This makes openat() available even though I set _POSIX_C_SOURCE and
// _XOPEN_SOURCE to a version of POSIX older than 2008
#define _GNU_SOURCE

#include "config.h"
#undef PROGRAM_NAME
#define PROGRAM_NAME "input-switch"
#include "liberty/liberty.c"

#include "ddc-ci.c"
#include <dirent.h>

// This list is from the MCCS 2.2a specification
struct
{
	int code;                           ///< Input code
	const char *name;                   ///< Input name
	int index;                          ///< Input index
}
g_inputs[] =
{
	{ 0x01, "VGA",       1, },          // Analog video (R/G/B) 1
	{ 0x02, "VGA",       2, },          // Analog video (R/G/B) 2
	{ 0x03, "DVI",       1, },          // Digital video (TMDS) 1 DVI 1
	{ 0x04, "DVI",       2, },          // Digital video (TMDS) 2 DVI 2
	{ 0x05, "composite", 1, },          // Composite video 1
	{ 0x06, "composite", 2, },          // Composite video 2
	{ 0x07, "S-Video",   1, },          // S-video 1
	{ 0x08, "S-Video",   2, },          // S-video 2
	{ 0x09, "tuner",     1, },          // Tuner 1
	{ 0x0A, "tuner",     2, },          // Tuner 2
	{ 0x0B, "tuner",     3, },          // Tuner 3
	{ 0x0C, "component", 1, },          // Component video (YPbPr/YCbCr) 1
	{ 0x0D, "component", 2, },          // Component video (YPbPr/YCbCr) 2
	{ 0x0E, "component", 3, },          // Component video (YPbPr/YCbCr) 3
	{ 0x0F, "DP",        1, },          // DisplayPort 1
	{ 0x10, "DP",        2, },          // DisplayPort 2
	{ 0x11, "HDMI",      1, },          // Digital Video (TMDS) 3 HDMI 1
	{ 0x12, "HDMI",      2, },          // Digital Video (TMDS) 4 HDMI 2
	{ 0x15, "bnq-tb",    1, },          // Thunderbolt on BenQ PD3220U (no spec)
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

typedef bool (*ActionFunc) (int fd, int param, struct error **);

static bool
get_input_source (int fd, int input, struct error **e)
{
	struct vcp_feature_readout readout = {};
	if (!vcp_get_feature (fd, VCP_INPUT_SOURCE, &readout, e))
		return false;

	(void) input;
	for (size_t i = 0; i < N_ELEMENTS (g_inputs); i++)
		if (g_inputs[i].code == readout.cur)
		{
			printf ("input is %s %d\n", g_inputs[i].name, g_inputs[i].index);
			return true;
		}

	printf ("input is %d\n", readout.cur);
	return true;
}

static bool
set_input_source (int fd, int input, struct error **e)
{
	struct vcp_feature_readout readout = {};
	if (!vcp_get_feature (fd, VCP_INPUT_SOURCE, &readout, e))
		return false;
	if (input < 0 || input > readout.max)
		return error_set (e, "input index out of range");

	uint8_t set_req[] = { VCP_INPUT_SOURCE, input >> 8, input };
	if (!ddc_send (fd, DDC_SET_VCP_FEATURE, set_req, sizeof set_req, e))
		return false;

	wait_ms (50);

	printf ("input set from %d to %d of %d\n", readout.cur, input, readout.max);
	return true;
}


static bool
set_bnq_kvm (int fd, int kvm, struct error **e)
{
	// This function does a leap of faith, should check the actual manufacturer
	enum { VCP_BNQ_KVM = 0xE4 };

	struct vcp_feature_readout readout = {};
	if (!vcp_get_feature (fd, VCP_BNQ_KVM, &readout, e))
		return false;
	if (kvm < 0 || kvm > readout.max)
		return error_set (e, "KVM index out of range");

	uint8_t set_req[] = { VCP_BNQ_KVM, kvm >> 8, kvm };
	if (!ddc_send (fd, DDC_SET_VCP_FEATURE, set_req, sizeof set_req, e))
		return false;

	wait_ms (50);

	printf ("KVM set from %d to %d of %d\n", readout.cur, kvm, readout.max);
	return true;
}

static void
i2c (ActionFunc action, int param)
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
		 || !action (fd, param, &e))
		{
			printf ("%s\n", e->message);
			error_free (e);
		}

		close (fd);
	}
	closedir (dev);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

int
main (int argc, char *argv[])
{
	g_log_message_real = log_message_custom;

	if (argc <= 1)
	{
		printf ("Usage: %s {? | INPUT [INDEX]}\n", argv[0]);
		exit (EXIT_FAILURE);
	}
	if (!strcmp (argv[1], "?"))
	{
		i2c (get_input_source, -1);
		exit (EXIT_SUCCESS);
	}

	unsigned long input_source = 0;
	if (xstrtoul (&input_source, argv[1], 10))
	{
		i2c (set_input_source, input_source);
		exit (EXIT_SUCCESS);
	}

	unsigned long index = 1;
	if (argc > 2 && !xstrtoul (&index, argv[2], 10))
		exit_fatal ("given index is not a number: %s", argv[2]);

	// Manufacturer-specific, argument currently necessary, but we could rotate
	if (argc > 2 && !strcasecmp (argv[1], "bnq-kvm"))
	{
		i2c (set_bnq_kvm, index);
		exit (EXIT_SUCCESS);
	}

	for (size_t i = 0; i < N_ELEMENTS (g_inputs); i++)
		if (!strcasecmp_ascii (g_inputs[i].name, argv[1])
		 && g_inputs[i].index == (int) index)
			input_source = g_inputs[i].code;
	if (!input_source)
		exit_fatal ("unknown input source: %s %lu", argv[1], index);

	i2c (set_input_source, input_source);
	return 0;
}
