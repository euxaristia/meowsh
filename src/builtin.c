/*
 * meowsh — POSIX-compliant shell
 * builtin.c — Dispatch table
 */

#define _POSIX_C_SOURCE 200809L

#include "builtin.h"

#include <string.h>

static const struct builtin_entry builtins[] = {
	/* Special builtins (POSIX) */
	{ "break",    builtin_break,    1 },
	{ ":",        builtin_colon,    1 },
	{ "continue", builtin_continue, 1 },
	{ ".",        builtin_dot,      1 },
	{ "eval",     builtin_eval,     1 },
	{ "exec",     builtin_exec,     1 },
	{ "exit",     builtin_exit,     1 },
	{ "export",   builtin_export,   1 },
	{ "readonly", builtin_readonly, 1 },
	{ "return",   builtin_return,   1 },
	{ "set",      builtin_set,      1 },
	{ "shift",    builtin_shift,    1 },
	{ "times",    builtin_times,    1 },
	{ "trap",     builtin_trap,     1 },
	{ "unset",    builtin_unset,    1 },

	/* Regular builtins */
	{ "alias",    builtin_alias,    0 },
	{ "bg",       builtin_bg,       0 },
	{ "cd",       builtin_cd,       0 },
	{ "command",  builtin_command,  0 },
	{ "echo",     builtin_echo,     0 },
	{ "false",    builtin_false,    0 },
	{ "fc",       builtin_fc,       0 },
	{ "fg",       builtin_fg,       0 },
	{ "getopts",  builtin_getopts,  0 },
	{ "hash",     builtin_hash,     0 },
	{ "history",  builtin_history,  0 },
	{ "jobs",     builtin_jobs,     0 },
	{ "kill",     builtin_kill,     0 },
	{ "meow",     builtin_meow,     0 },
	{ "newgrp",   builtin_newgrp,   0 },
	{ "pwd",      builtin_pwd,      0 },
	{ "read",     builtin_read,     0 },
	{ "true",     builtin_true,     0 },
	{ "type",     builtin_type,     0 },
	{ "ulimit",   builtin_ulimit,   0 },
	{ "umask",    builtin_umask,    0 },
	{ "unalias",  builtin_unalias,  0 },
	{ "wait",     builtin_wait,     0 },

	/* Sentinel */
	{ NULL, NULL, 0 }
};

const struct builtin_entry *
builtin_get_all(void)
{
	return builtins;
}

const struct builtin_entry *
builtin_lookup(const char *name)
{
	const struct builtin_entry *b;

	for (b = builtins; b->name; b++) {
		if (strcmp(b->name, name) == 0)
			return b;
	}
	return NULL;
}

int
is_special_builtin(const char *name)
{
	const struct builtin_entry *b = builtin_lookup(name);

	return b && b->special;
}
