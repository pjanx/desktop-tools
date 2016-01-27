// Public domain
#include <gdm-user-switching.h>

int
main (int argc, char *argv[])
{
	(void) argc;
	(void) argv;

	GError *e = NULL;
	if (!gdm_goto_login_session_sync (g_cancellable_new (), &e))
	{
		g_printerr ("%s\n", e->message);
		return 1;
	}
	return 0;
}
