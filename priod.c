/*
 * priod.c: process reprioritizing daemon
 *
 * Thanks to http://netsplit.com/the-proc-connector-and-socket-filters
 * for showing the way around the proc connector and BPF.
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

#include <dirent.h>

#include <linux/cn_proc.h>
#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/filter.h>

#include <sys/resource.h>
#include <sys/syscall.h>

// --- Main program ------------------------------------------------------------

#define RULE_UNSET INT_MIN

struct rule
{
	char *program_name;                 ///< Program name to match against

	int oom_score_adj;                  ///< For /proc/%/oom_score_adj
	int prio;                           ///< For setpriority()
	int ioprio;                         ///< For SYS_ioprio_set
};

struct app_context
{
	struct poller poller;               ///< Poller
	bool polling;                       ///< The event loop is running

	int proc_fd;                        ///< Proc connector FD
	struct poller_fd proc_event;        ///< Proc connector read event

	struct rule *rules;                 ///< Rules
	size_t rules_len;                   ///< Number of rules
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

// --- Configuration -----------------------------------------------------------

static bool
load_integer (struct str_map *root, const char *key, int min, int max,
	int *value, struct error **e)
{
	*value = RULE_UNSET;

	struct config_item *item;
	if (!(item = str_map_find (root, key)))
		return true;

	if (item->type != CONFIG_ITEM_INTEGER
	 || item->value.integer < min || item->value.integer > max)
		return error_set (e, "%s: must be an integer (%d..%d)", key, min, max);

	*value = item->value.integer;
	return true;
}

static bool
load_rule (const char *name, struct str_map *m, struct rule *r,
	struct error **e)
{
	r->program_name = xstrdup (name);
	if (!load_integer (m, "oom_score_adj", -1000, 1000, &r->oom_score_adj, e)
	 || !load_integer (m, "prio",            -20,   19, &r->prio,          e)
	 || !load_integer (m, "ioprio",            0,    7, &r->ioprio,        e))
		return false;
	return true;
}

static struct rule *
find_rule (struct app_context *ctx, const char *program_name)
{
	for (size_t i = 0; i < ctx->rules_len; i++)
		if (!strcmp (ctx->rules[i].program_name, program_name))
			return ctx->rules + i;
	return NULL;
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

	struct str_map_iter iter = str_map_iter_make (&root->value.object);
	ctx->rules = xcalloc (iter.map->len, sizeof *ctx->rules);
	ctx->rules_len = 0;

	struct config_item *subtree;
	while ((subtree = str_map_iter_next (&iter)))
	{
		const char *path = iter.link->key;
		if (subtree->type != CONFIG_ITEM_OBJECT)
			exit_fatal ("rule `%s' in configuration is not an object", path);
		if (!load_rule (path, &subtree->value.object,
			&ctx->rules[ctx->rules_len++], &e))
			exit_fatal ("rule `%s': %s", path, e->message);
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
adj_oom_score (int pid, const char *program_name, int score)
{
	char buf[16]; snprintf (buf, sizeof buf, "%d\n", score);
	char *path = xstrdup_printf ("/proc/%d/oom_score_adj", pid);
	struct error *e = NULL;
	if (!write_file (path, buf, strlen (buf), &e))
	{
		print_error ("%d (%s): %s", pid, program_name, e->message);
		error_free (e);
	}
	free (path);
}

static bool
reprioritize (int pid, const char *program_name, DIR *dir, struct rule *rule,
	struct str_map *set)
{
	size_t not_previously_visited = 0;
	struct dirent *iter;
	while ((errno = 0, iter = readdir (dir)))
	{
		int tid = atoi (iter->d_name);
		if (!tid || str_map_find (set, iter->d_name))
			continue;

		print_debug (" - thread %d", tid);
		str_map_set (set, iter->d_name, (void *) ++not_previously_visited);

		if (RULE_UNSET != rule->prio
		 && setpriority (PRIO_PROCESS, pid, rule->prio))
			print_error ("%d (%s): thread %d: setpriority: %s",
				pid, program_name, tid, strerror (errno));
		if (RULE_UNSET != rule->ioprio
		 && syscall (SYS_ioprio_set, IOPRIO_WHO_PROCESS, tid,
				IOPRIO_CLASS_BE << IOPRIO_CLASS_SHIFT | rule->ioprio))
			print_error ("%d (%s): thread %d: ioprio_set: %s",
				pid, program_name, tid, strerror (errno));
	}
	if (errno)
	{
		print_error ("%d (%s): readdir: %s",
			pid, program_name, strerror (errno));
	}
	return not_previously_visited == 0;
}

static void
on_exec_name (struct app_context *ctx, int pid, const char *program_name)
{
	// TODO: we might want to at least provide more criteria to match on,
	//   so as to not blindly trust everything, despite these priorities being
	//   relatively harmless if you overlook possible "denial of service"
	struct rule *rule = find_rule (ctx, program_name);
	const char *slash = strrchr (program_name, '/');
	if (!rule && (!slash || !(rule = find_rule (ctx, slash + 1))))
		return;

	print_debug ("%d (%s) matched", pid, program_name);
	if (RULE_UNSET != rule->oom_score_adj)
		adj_oom_score (pid, program_name, rule->oom_score_adj);

	// Priority APIs are strictly per-thread (i.e. Linux "task"), so we must
	// iterate through all tasks within a thread group
	char *path = xstrdup_printf ("/proc/%d/task", pid);
	DIR *dir = opendir (path);
	free (path);
	if (!dir)
	{
		print_error ("%d (%s): opendir: %s",
			pid, program_name, strerror (errno));
		return;
	}

	// This has an inherent race condition, but let's give it a try
	struct str_map set = str_map_make (NULL);
	for (size_t retries = 3; retries--; )
		if (reprioritize (pid, program_name, dir, rule, &set))
			break;

	str_map_free (&set);
	closedir (dir);
}

static void
on_exec (struct app_context *ctx, int pid)
{
	// This is inherently racy but there seems to be no better way to do it
	char *path = xstrdup_printf ("/proc/%d/cmdline", pid);
	struct str cmdline = str_make ();

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
preapply_rules (struct app_context *ctx)
{
	DIR *dir = opendir ("/proc");
	if (!dir)
	{
		print_error ("opendir: %s: %s", "/proc", strerror (errno));
		return;
	}

	// We don't care about processes deleted or created during this loop
	struct dirent *iter;
	while ((errno = 0, iter = readdir (dir)))
	{
		int pid = atoi (iter->d_name);
		if (pid && (iter->d_type == DT_UNKNOWN || iter->d_type == DT_DIR))
			on_exec (ctx, pid);
	}
	closedir (dir);
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

	struct opt_handler oh = opt_handler_make (argc, argv, opts, "CONFIG",
		"Process reprioritizing daemon.");

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

	struct poller_fd signal_event =
		poller_fd_make (&ctx.poller, g_signal_pipe[0]);
	signal_event.dispatcher = (poller_fd_fn) on_signal_pipe_readable;
	signal_event.user_data = &ctx;
	poller_fd_set (&signal_event, POLLIN);

	load_configuration (&ctx, config_path);

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

	ctx.proc_event = poller_fd_make (&ctx.poller, ctx.proc_fd);
	ctx.proc_event.dispatcher = (poller_fd_fn) on_event;
	ctx.proc_event.user_data = &ctx;
	poller_fd_set (&ctx.proc_event, POLLIN);

	// While new events are being queued, we can apply rules to already
	// existing processes, so that we don't miss anything except for obvious
	// cases when a process re-execs to something else after a match.
	// It would inherit the same values anyway, so it seems to be mostly okay.
	preapply_rules (&ctx);

	ctx.polling = true;
	while (ctx.polling)
		poller_run (&ctx.poller);

	poller_free (&ctx.poller);
	xclose (ctx.proc_fd);
	for (size_t i = 0; i < ctx.rules_len; i++)
		free (ctx.rules[i].program_name);
	free (ctx.rules);
	return EXIT_SUCCESS;
}
