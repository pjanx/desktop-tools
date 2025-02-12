/*
 * iexec.c: run a program and restart on file change
 *
 * Copyright (c) 2017 - 2023, Přemysl Eric Janouch <p@janouch.name>
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
#define PROGRAM_NAME "iexec"
#include "liberty/liberty.c"

// This can also work on BSD if someone puts in the effort to support kqueue
#include <sys/inotify.h>

static struct
{
	pid_t child;                        ///< Watched child or 0
	bool exits;                         ///< Don't restart child when it exits
	bool respawn;                       ///< Respawn child ASAP
	bool killing;                       ///< Waiting for child to die
	int inotify_fd, inotify_wd;
}
g;

// Note that this program doesn't queue up file-based restarts
static void
handle_inotify_event (const struct inotify_event *e, const char *base)
{
	if (e->wd != g.inotify_wd || strcmp (e->name, base))
		return;

	if (g.child)
	{
		print_debug ("file changed, killing child");
		if (kill (g.child, SIGINT))
			print_error ("kill: %s", strerror (errno));
		g.killing = true;
	}
	else
	{
		print_debug ("file changed, respawning");
		g.respawn = true;
	}
}

static void
handle_file_change (const char *base)
{
	char buf[4096];
	ssize_t len = 0;
	struct inotify_event *e = NULL;
	while ((len = read (g.inotify_fd, buf, sizeof buf)) > 0)
		for (char *ptr = buf; ptr < buf + len; ptr += sizeof *e + e->len)
			handle_inotify_event ((e = (struct inotify_event *) buf), base);
}

static void
spawn (char *argv[])
{
	if ((g.child = fork ()) == -1)
		exit_fatal ("fork: %s", strerror (errno));
	else if (g.child)
		return;

	// A linker can create spurious CLOSE_WRITEs, wait until it's executable
	while (1)
	{
		execvp (argv[0], argv);
		print_error ("execvp: %s", strerror (errno));
		sleep (1);
	}
}

static bool
check_child_death (void)
{
	int status = 0;
	if (waitpid (g.child, &status, WNOHANG) != g.child)
		return true;

	g.child = 0;
	if (!g.killing)
	{
		print_debug ("child died on its own, not respawning");
		return g.exits;
	}

	g.killing = false;
	print_debug ("child died on request, respawning");
	return g.respawn = true;
}

static void
sigchld_handler (int signum)
{
	// We need to have this handler so that pselect() can return EINTR
	(void) signum;
}

int
main (int argc, char *argv[])
{
	const char *target = NULL;
	static const struct opt opts[] =
	{
		{ 'f', "file", "PATH", 0, "watch this path rather than the program" },
		{ 'e', "exits", NULL, 0, "allow the program to exit on its own" },
		{ 'd', "debug", NULL, 0, "run in debug mode" },
		{ 'h', "help", NULL, 0, "display this help and exit" },
		{ 'V', "version", NULL, 0, "output version information and exit" },
		{ 0, NULL, NULL, 0, NULL }
	};

	struct opt_handler oh = opt_handler_make (argc, argv, opts,
		"PROGRAM [ARG...]", "Run a program and restart it when it changes.");

	// We have to turn that off as it causes more trouble than what it's worth
	cstr_set (&oh.opt_string, xstrdup_printf ("+%s", oh.opt_string));

	int c;
	while ((c = opt_handler_get (&oh)) != -1)
	switch (c)
	{
	case 'f':
		target = optarg;
		break;
	case 'e':
		g.exits = true;
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

	if (argc == optind)
	{
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}

	opt_handler_free (&oh);
	argc -= optind;
	argv += optind;

	if (!target)
		target = argv[0];

	(void) signal (SIGPIPE, SIG_IGN);
	struct sigaction sa = { .sa_handler = sigchld_handler };
	sigemptyset (&sa.sa_mask);
	if (sigaction (SIGCHLD, &sa, NULL))
		exit_fatal ("sigaction: %s", strerror (errno));

	sigset_t chld, orig;
	sigemptyset (&chld);
	sigaddset (&chld, SIGCHLD);
	if (sigprocmask (SIG_BLOCK, &chld, &orig))
		exit_fatal ("sigprocmask: %s", strerror (errno));

	char *path = NULL;
	char *dir = dirname ((path = xstrdup (target)));

	if ((g.inotify_fd = inotify_init1 (IN_NONBLOCK)) < 0)
		exit_fatal ("inotify_init1: %s", strerror (errno));
	if ((g.inotify_wd = inotify_add_watch (g.inotify_fd,
		dir, IN_MOVED_TO | IN_CLOSE_WRITE)) < 0)
		exit_fatal ("inotify_add_watch: %s", strerror (errno));

	free (path);
	char *base = basename ((path = xstrdup (target)));
	g.respawn = true;
	do
	{
		if (g.respawn)
		{
			spawn (argv);
			g.respawn = false;
		}

		fd_set r; FD_SET (g.inotify_fd, &r);
		(void) pselect (g.inotify_fd + 1, &r, NULL, NULL, NULL, &orig);
		handle_file_change (base);
	}
	while (check_child_death ());

	free (path);
	xclose (g.inotify_fd);
	return EXIT_SUCCESS;
}
