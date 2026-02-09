/*
 * meowsh — POSIX-compliant shell
 * trap.c — Signal handling, trap builtin
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "trap.h"
#include "exec.h"
#include "parser.h"
#include "input.h"
#include "compat.h"
#include "sh_error.h"
#include "memalloc.h"

#include <string.h>

static const struct {
	const char *name;
	int num;
} sig_names[] = {
	{ "EXIT",  0 },
	{ "HUP",   SIGHUP },
	{ "INT",   SIGINT },
	{ "QUIT",  SIGQUIT },
	{ "ILL",   SIGILL },
	{ "TRAP",  SIGTRAP },
	{ "ABRT",  SIGABRT },
	{ "FPE",   SIGFPE },
	{ "KILL",  SIGKILL },
	{ "BUS",   SIGBUS },
	{ "SEGV",  SIGSEGV },
	{ "PIPE",  SIGPIPE },
	{ "ALRM",  SIGALRM },
	{ "TERM",  SIGTERM },
	{ "USR1",  SIGUSR1 },
	{ "USR2",  SIGUSR2 },
	{ "CHLD",  SIGCHLD },
	{ "CONT",  SIGCONT },
	{ "STOP",  SIGSTOP },
	{ "TSTP",  SIGTSTP },
	{ "TTIN",  SIGTTIN },
	{ "TTOU",  SIGTTOU },
#ifdef SIGURG
	{ "URG",   SIGURG },
#endif
#ifdef SIGXCPU
	{ "XCPU",  SIGXCPU },
#endif
#ifdef SIGXFSZ
	{ "XFSZ",  SIGXFSZ },
#endif
#ifdef SIGVTALRM
	{ "VTALRM", SIGVTALRM },
#endif
#ifdef SIGPROF
	{ "PROF",  SIGPROF },
#endif
#ifdef SIGWINCH
	{ "WINCH", SIGWINCH },
#endif
#ifdef SIGSYS
	{ "SYS",   SIGSYS },
#endif
	{ NULL, 0 }
};

static void
trap_handler(int sig)
{
	if (sig >= 0 && sig < NSIG) {
		sh.trap_pending[sig] = 1;
		sh.any_trap_pending = 1;
	}
}

void
trap_init(void)
{
	int i;
	memset(sh.traps, 0, sizeof(sh.traps));
	for (i = 0; i < NSIG; i++)
		sh.trap_pending[i] = 0;
	sh.any_trap_pending = 0;

	/* Default SIGINT handling for interactive shells */
	if (sh.interactive) {
		sh_signal(SIGINT, SIG_IGN);
		sh_signal(SIGQUIT, SIG_IGN);
	}
}

int
trap_set(int sig, const char *action)
{
	if (sig < 0 || sig >= NSIG)
		return -1;

	free(sh.traps[sig]);

	if (!action) {
		/* Reset to default */
		sh.traps[sig] = NULL;
		if (sig > 0)
			sh_signal(sig, SIG_DFL);
	} else if (action[0] == '\0') {
		/* Ignore */
		sh.traps[sig] = sh_strdup("");
		if (sig > 0)
			sh_signal(sig, SIG_IGN);
	} else {
		/* Set handler */
		sh.traps[sig] = sh_strdup(action);
		if (sig > 0)
			sh_signal(sig, trap_handler);
	}

	return 0;
}

void
trap_check(void)
{
	int sig;

	if (!sh.any_trap_pending)
		return;

	sh.any_trap_pending = 0;

	for (sig = 0; sig < NSIG; sig++) {
		if (sh.trap_pending[sig]) {
			sh.trap_pending[sig] = 0;
			if (sh.traps[sig] && sh.traps[sig][0]) {
				/* Execute trap action */
				input_push_string(sh.traps[sig]);
				{
					struct node *tree =
					    parse_command(NULL, NULL);
					if (tree)
						exec_node(tree, 0);
				}
				input_pop();
			}
		}
	}

	/* EXIT trap (signal 0) */
	/* Handled by atexit or exit path */
}

void
trap_clear(void)
{
	int sig;

	/* In subshell: traps that are not ignored are reset */
	for (sig = 0; sig < NSIG; sig++) {
		if (sh.traps[sig]) {
			if (sh.traps[sig][0] != '\0') {
				/* Was a command — reset to default */
				free(sh.traps[sig]);
				sh.traps[sig] = NULL;
				if (sig > 0)
					sh_signal(sig, SIG_DFL);
			}
			/* If empty string (ignored), keep it */
		}
	}
}

void
trap_reset(void)
{
	int sig;

	for (sig = 1; sig < NSIG; sig++) {
		if (sh.traps[sig]) {
			free(sh.traps[sig]);
			sh.traps[sig] = NULL;
		}
		sh_signal(sig, SIG_DFL);
	}
}

int
trap_signum(const char *name)
{
	int i;
	char *end;
	long num;

	/* Try as a number */
	num = strtol(name, &end, 10);
	if (*end == '\0' && num >= 0 && num < NSIG)
		return (int)num;

	/* Try as a name, with or without SIG prefix */
	for (i = 0; sig_names[i].name; i++) {
		if (strcasecmp(name, sig_names[i].name) == 0)
			return sig_names[i].num;
		/* Try with SIG prefix */
		if (strncasecmp(name, "SIG", 3) == 0 &&
		    strcasecmp(name + 3, sig_names[i].name) == 0)
			return sig_names[i].num;
	}

	return -1;
}

const char *
trap_signame(int sig)
{
	int i;

	for (i = 0; sig_names[i].name; i++) {
		if (sig_names[i].num == sig)
			return sig_names[i].name;
	}
	return NULL;
}

void
trap_print(void)
{
	int sig;

	for (sig = 0; sig < NSIG; sig++) {
		if (sh.traps[sig]) {
			const char *name = trap_signame(sig);
			if (name)
				printf("trap -- '%s' %s\n",
				    sh.traps[sig], name);
			else
				printf("trap -- '%s' %d\n",
				    sh.traps[sig], sig);
		}
	}
}

void
sigchld_handler(int sig)
{
	(void)sig;
	sh.trap_pending[SIGCHLD] = 1;
	sh.any_trap_pending = 1;
}

void
trap_block(void)
{
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGCHLD);
	sigprocmask(SIG_BLOCK, &set, NULL);
}

void
trap_unblock(void)
{
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGCHLD);
	sigprocmask(SIG_UNBLOCK, &set, NULL);
}
