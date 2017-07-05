/*
 * priod.c: process reprioritizing daemon
 *
 * Thanks to http://netsplit.com/the-proc-connector-and-socket-filters
 *
 * Copyright (c) 2017, Přemysl Janouch <p.janouch@gmail.com>
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

#define _GNU_SOURCE
#define LIBERTY_WANT_POLLER

#include "config.h"
#undef PROGRAM_NAME
#define PROGRAM_NAME "priod"
#include "liberty/liberty.c"

#include <linux/cn_proc.h>
#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/filter.h>

#include <sys/resource.h>
#include <sys/syscall.h>

// --- Main program ------------------------------------------------------------

struct app_context
{
	struct poller poller;               ///< Poller
	bool polling;                       ///< The event loop is running

	int proc_fd;                        ///< Proc connector FD
	struct poller_fd proc_event;        ///< Proc connector read event
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

// IO priorities are a sort-of-private kernel API with no proper headers

enum
{
	IOPRIO_CLASS_NONE,
	IOPRIO_CLASS_RT,
	IOPRIO_CLASS_BE,
	IOPRIO_CLASS_IDLE,
};

enum
{
	IOPRIO_WHO_PROCESS = 1,
	IOPRIO_WHO_PGRP,
	IOPRIO_WHO_USER,
};

#define IOPRIO_CLASS_SHIFT 13

static void
on_exec_name (struct app_context *ctx, int pid, const char *name)
{
	print_status ("exec %d %s", pid, name);

	if (true)
		return;

	setpriority (PRIO_PROCESS, pid, 0 /* TODO -20..20 */);

	// TODO: this is per thread, and there's an inherent race condition;
	//   keep going through /proc/%d/task and reprioritize all threads;
	//   stop trying after N-th try
	syscall (SYS_ioprio_set, IOPRIO_WHO_PROCESS,
		IOPRIO_CLASS_BE << IOPRIO_CLASS_SHIFT | 0 /* TODO 0..7 */);

	char *path = xstrdup_printf ("/proc/%d/oom_score_adj", pid);
	struct error *e = NULL;
	// TODO: figure out the contents
	if (!write_file (path, "", 0, &e))
	{
		print_error ("%s", e->message);
		error_free (e);
	}
	free (path);
}

static void
on_exec (struct app_context *ctx, int pid)
{
	char *path = xstrdup_printf ("/proc/%d/cmdline", pid);
	struct str cmdline;
	str_init (&cmdline);

	struct error *e = NULL;
	if (read_file (path, &cmdline, &e))
		on_exec_name (ctx, pid, cmdline.str);
	else
	{
		print_debug ("%s", e->message);
		error_free (e);
	}

	free (path);
	str_free (&cmdline);
}

static void
on_netlink_message (struct app_context *ctx, struct nlmsghdr *mh)
{
	// In practice the kernel connector never sends multipart messages
	if (!soft_assert (mh->nlmsg_type != 0)
	 || !soft_assert (mh->nlmsg_flags == 0)
	 || mh->nlmsg_type != NLMSG_DONE)
		return;

	struct cn_msg *m = NLMSG_DATA (mh);
	if (m->id.idx != CN_IDX_PROC
	 || m->id.val != CN_VAL_PROC)
		return;

	// XXX: potential alignment issues
	struct proc_event *e = (struct proc_event *) m->data;
	if (e->what == PROC_EVENT_EXEC)
		on_exec (ctx, e->event_data.exit.process_tgid);
}

static void
on_event (const struct pollfd *pfd, struct app_context *ctx)
{
	char buf[sysconf (_SC_PAGESIZE)];
	struct sockaddr_nl addr;
	while (true)
	{
		socklen_t addr_len = sizeof addr;
		ssize_t len = recvfrom (pfd->fd, buf, sizeof buf, 0,
			(struct sockaddr *) &addr, &addr_len);
		if (len == 0)
			exit_fatal ("socket closed");
		if (len < 0 && errno == EAGAIN)
			return;
		if (len < 0)
			exit_fatal ("recvfrom: %s", strerror (errno));

		// Make sure it comes from the kernel
		if (addr.nl_pid)
			continue;

		// In practice the kernel connector always sends one per dgram
		for (struct nlmsghdr *mh = (struct nlmsghdr *) buf;
			NLMSG_OK (mh, len); mh = NLMSG_NEXT (mh, len))
			on_netlink_message (ctx, mh);
	}
}

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

	struct opt_handler oh;
	opt_handler_init (&oh, argc, argv, opts, "CONFIG", "Fan controller.");

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

/// Sets up a filter so that we're only woken up by the kernel on exec() events
static void
setup_exec_filter (int fd)
{
	struct incoming
	{
		union { struct nlmsghdr netlink; char align[NLMSG_HDRLEN]; };
		struct cn_msg connector;
		struct proc_event event;
	}
	__attribute__ ((packed));

	// Byteswapping is needed because the netlink protocol is host-endian
	struct sock_filter filter[] =
	{
		// Only continue filtering dgrams with one "proc_event" message in them
		BPF_STMT (BPF_LD | BPF_W | BPF_LEN, 0),
		BPF_JUMP (BPF_JMP | BPF_JEQ | BPF_K, sizeof (struct incoming), 0, 9),
		BPF_STMT (BPF_LD | BPF_H | BPF_ABS,
			offsetof (struct incoming, netlink.nlmsg_type)),
		BPF_JUMP (BPF_JMP | BPF_JEQ | BPF_K, htons (NLMSG_DONE), 0, 7),
		BPF_STMT (BPF_LD | BPF_W | BPF_ABS,
			offsetof (struct incoming, connector.id.idx)),
		BPF_JUMP (BPF_JMP | BPF_JEQ | BPF_K, htonl (CN_IDX_PROC), 0, 5),
		BPF_STMT (BPF_LD | BPF_W | BPF_ABS,
			offsetof (struct incoming, connector.id.val)),
		BPF_JUMP (BPF_JMP | BPF_JEQ | BPF_K, htonl (CN_VAL_PROC), 0, 3),

		BPF_STMT (BPF_LD | BPF_W | BPF_ABS,
			offsetof (struct incoming, event.what)),
		BPF_JUMP (BPF_JMP | BPF_JEQ | BPF_K, htonl (PROC_EVENT_EXEC), 1, 0),
		BPF_STMT (BPF_RET | BPF_K, 0),
		BPF_STMT (BPF_RET | BPF_K, 0xffffffff),
	};

	struct sock_fprog fprog = { .filter = filter, .len = N_ELEMENTS (filter) };
	const int yes = 1;
	if (setsockopt (fd, SOL_SOCKET, SO_ATTACH_FILTER, &fprog, sizeof fprog) < 0
	 || setsockopt (fd, SOL_NETLINK, NETLINK_NO_ENOBUFS, &yes, sizeof yes) < 0)
		print_error ("setsockopt: %s", strerror (errno));
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
	poller_fd_init (&signal_event, &ctx.poller, g_signal_pipe[0]);
	signal_event.dispatcher = (poller_fd_fn) on_signal_pipe_readable;
	signal_event.user_data = &ctx;
	poller_fd_set (&signal_event, POLLIN);

	// TODO: load configuration so that we know what to do with the events

	ctx.proc_fd = socket (PF_NETLINK,
		SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_CONNECTOR);
	if (ctx.proc_fd < 0)
		exit_fatal ("cannot make a proc connector: %s", strerror (errno));

	setup_exec_filter (ctx.proc_fd);

	struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_pid = getpid (),
		.nl_groups = CN_IDX_PROC };
	if (bind (ctx.proc_fd, (struct sockaddr *) &addr, sizeof addr) < 0)
		exit_fatal ("cannot make a proc connector: %s", strerror (errno));

	struct
	{
		union { struct nlmsghdr netlink; char align[NLMSG_HDRLEN]; };
		struct cn_msg connector;
		enum proc_cn_mcast_op op;
	}
	__attribute__ ((packed)) subscription =
	{
		.netlink.nlmsg_len = sizeof subscription,
		.netlink.nlmsg_type = NLMSG_DONE,
		.netlink.nlmsg_pid = getpid (),
		.connector.id.idx = CN_IDX_PROC,
		.connector.id.val = CN_VAL_PROC,
		.connector.len = sizeof subscription.op,
		.op = PROC_CN_MCAST_LISTEN,
	};

	if (write (ctx.proc_fd, &subscription, sizeof subscription) < 0)
		exit_fatal ("failed to subscribe for events: %s", strerror (errno));

	poller_fd_init (&ctx.proc_event, &ctx.poller, ctx.proc_fd);
	ctx.proc_event.dispatcher = (poller_fd_fn) on_event;
	ctx.proc_event.user_data = &ctx;
	poller_fd_set (&ctx.proc_event, POLLIN);

	ctx.polling = true;
	while (ctx.polling)
		poller_run (&ctx.poller);

	poller_free (&ctx.poller);
	xclose (ctx.proc_fd);
	return EXIT_SUCCESS;
}
