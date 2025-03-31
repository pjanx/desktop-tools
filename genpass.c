/*
 * genpass.c: password generator
 *
 * Copyright (c) 2025, Přemysl Eric Janouch <p@janouch.name>
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

#include "config.h"
#undef PROGRAM_NAME
#define PROGRAM_NAME "genpass"
#include "liberty/liberty.c"

static struct str
parse_group (const char *group)
{
	bool present[0x100] = {};
	for (size_t i = 0; group[i]; i++)
	{
		unsigned char c = group[i];
		if (!i || c != '-' || !group[i + 1])
			present[c] = true;
		else if (group[i + 1] < group[i - 1])
			exit_fatal ("character ranges must be increasing");
		else
			for (c = group[i - 1]; ++c <= group[i + 1]; )
				present[c] = true;
	}

	struct str alphabet = str_make ();
	for (size_t i = 1; i < N_ELEMENTS (present); i++)
		if (present[i])
			str_append_c (&alphabet, i);
	if (!alphabet.len)
		exit_fatal ("empty group");
	return alphabet;
}

static void
parse_program_arguments (int argc, char **argv,
	unsigned long *length, struct strv *groups, struct str *alphabet)
{
	static const struct opt opts[] =
	{
		{ 'l', "length", "CHARACTERS", 0, "set password length" },
		{ 'd', "debug", NULL, 0, "run in debug mode" },
		{ 'h', "help", NULL, 0, "display this help and exit" },
		{ 'V', "version", NULL, 0, "output version information and exit" },
		{ 0, NULL, NULL, 0, NULL }
	};

	struct opt_handler oh =
		opt_handler_make (argc, argv, opts, "GROUP...", "Password generator.");

	int c;
	while ((c = opt_handler_get (&oh)) != -1)
	switch (c)
	{
	case 'l':
		if (!xstrtoul (length, optarg, 10) || *length <= 0)
			print_fatal ("invalid length argument");
		break;
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

	for (int i = 0; i < argc; i++)
	{
		struct str alphabet = parse_group (argv[i]);
		strv_append_owned (groups, str_steal (&alphabet));
	}

	bool present[0x100] = {};
	for (size_t i = 0; i < groups->len; i++)
		for (size_t k = 0; groups->vector[i][k]; k++)
		{
			unsigned char c = groups->vector[i][k];
			if (present[c])
				exit_fatal ("groups are not disjunct");
			present[c] = true;
		}
	for (size_t i = 1; i < N_ELEMENTS (present); i++)
		if (present[i])
			str_append_c (alphabet, i);

	if (groups->len > *length)
		exit_fatal ("the requested length is less than the number of groups");
	if (!groups->len)
	{
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}

	opt_handler_free (&oh);
}

int
main (int argc, char *argv[])
{
	unsigned long length = 8;
	struct strv groups = strv_make ();
	struct str alphabet = str_make ();
	parse_program_arguments (argc, argv, &length, &groups, &alphabet);

	unsigned seed = 0;
	if (!random_bytes (&seed, sizeof seed, NULL))
		exit_fatal ("failed to initialize random numbers");
	srand (seed);

	// Select from a joined alphabet, but make sure all groups are represented.
	struct str candidate = str_make ();
	while (true)
	{
restart:
		for (size_t i = length; i--; )
			str_append_c (&candidate, alphabet.str[rand () % alphabet.len]);
		for (size_t i = 0; i < groups.len; i++)
			if (!strpbrk (candidate.str, groups.vector[i]))
			{
				str_reset (&candidate);
				goto restart;
			}

		printf ("%s\n", candidate.str);
		return 0;
	}
}
