/*
 * meowsh — POSIX-compliant shell
 * shell.h — Master header, global shell state
 */

#ifndef MEOWSH_SHELL_H
#define MEOWSH_SHELL_H

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <limits.h>
#include <ctype.h>
#include <stdarg.h>

#include "types.h"

/* ---- Forward declarations ---- */
struct job;
struct alias_entry;
struct var_entry;

/* ---- Shell option flags ---- */
#define OPT_ALLEXPORT  (1 << 0)   /* -a */
#define OPT_ERREXIT    (1 << 1)   /* -e */
#define OPT_NOGLOB     (1 << 2)   /* -f */
#define OPT_HASHALL    (1 << 3)   /* -h */
#define OPT_INTERACTIVE (1 << 4)  /* -i */
#define OPT_MONITOR    (1 << 5)   /* -m */
#define OPT_NOEXEC     (1 << 6)   /* -n */
#define OPT_NOUNSET    (1 << 7)   /* -u */
#define OPT_VERBOSE    (1 << 8)   /* -v */
#define OPT_XTRACE     (1 << 9)   /* -x */
#define OPT_NOCLOBBER  (1 << 10)  /* -C */

/* ---- Variable flags ---- */
#define VAR_EXPORT     (1 << 0)
#define VAR_READONLY   (1 << 1)
#define VAR_SPECIAL    (1 << 2)

/* ---- Hash table size ---- */
#define HASH_SIZE 64

/* ---- Positional parameters stack ---- */
struct posparams {
	char **argv;
	int argc;
	struct posparams *prev;
};

/* ---- Global shell state ---- */
struct shell_state {
	/* Options */
	unsigned int opts;

	/* Variable hash table */
	struct var_entry *vars[HASH_SIZE];

	/* Positional parameters */
	struct posparams *posparams;

	/* Special parameters */
	int last_status;        /* $? */
	pid_t shell_pid;        /* $$ */
	pid_t last_bg_pid;      /* $! */
	char *argv0;            /* $0 */

	/* Job control */
	struct job *jobs;
	int next_job_id;
	pid_t shell_pgid;
	int terminal_fd;
	struct termios *saved_termios;

	/* Alias table */
	struct alias_entry *aliases[HASH_SIZE];

	/* Command hash table */
	struct hash_entry {
		char *name;
		char *path;
		struct hash_entry *next;
	} *cmd_hash[HASH_SIZE];

	/* Functions */
	struct func_entry {
		char *name;
		struct node *body;
		struct func_entry *next;
	} *functions[HASH_SIZE];

	/* Trap table — indexed by signal number */
	char *traps[NSIG];
	volatile sig_atomic_t trap_pending[NSIG];
	volatile sig_atomic_t any_trap_pending;

	/* Input state */
	struct input_source *input;

	/* Flags */
	int interactive;
	int login_shell;
	int subshell;
	int loop_depth;         /* for break/continue */
	int break_count;
	int continue_count;
	int func_depth;         /* for return */
	int want_return;
	int dot_depth;          /* for . (source) */
	int errexit_suppressed; /* in condition context */

	/* Line number */
	int lineno;
};

extern struct shell_state sh;

/* ---- Common functions ---- */
unsigned int hash_string(const char *s);

#endif /* MEOWSH_SHELL_H */
