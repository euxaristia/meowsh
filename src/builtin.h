/*
 * meowsh — POSIX-compliant shell
 * builtin.h — Builtin dispatch table
 */

#ifndef MEOWSH_BUILTIN_H
#define MEOWSH_BUILTIN_H

typedef int (*builtin_fn)(int argc, char **argv);

struct builtin_entry {
	const char *name;
	builtin_fn fn;
	int special;  /* 1 = special builtin per POSIX */
};

/* Find a builtin by name. Returns NULL if not found. */
const struct builtin_entry *builtin_lookup(const char *name);

/* Check if name is a special builtin */
int is_special_builtin(const char *name);

/* Special builtins (builtin_special.c) */
int builtin_break(int argc, char **argv);
int builtin_colon(int argc, char **argv);
int builtin_continue(int argc, char **argv);
int builtin_dot(int argc, char **argv);
int builtin_eval(int argc, char **argv);
int builtin_exec(int argc, char **argv);
int builtin_exit(int argc, char **argv);
int builtin_export(int argc, char **argv);
int builtin_readonly(int argc, char **argv);
int builtin_return(int argc, char **argv);
int builtin_set(int argc, char **argv);
int builtin_shift(int argc, char **argv);
int builtin_times(int argc, char **argv);
int builtin_trap(int argc, char **argv);
int builtin_unset(int argc, char **argv);

/* Regular builtins (builtin_regular.c) */
int builtin_alias(int argc, char **argv);
int builtin_bg(int argc, char **argv);
int builtin_cd(int argc, char **argv);
int builtin_command(int argc, char **argv);
int builtin_false(int argc, char **argv);
int builtin_fc(int argc, char **argv);
int builtin_fg(int argc, char **argv);
int builtin_getopts(int argc, char **argv);
int builtin_hash(int argc, char **argv);
int builtin_history(int argc, char **argv);
int builtin_jobs(int argc, char **argv);
int builtin_kill(int argc, char **argv);
int builtin_newgrp(int argc, char **argv);
int builtin_pwd(int argc, char **argv);
int builtin_read(int argc, char **argv);
int builtin_true(int argc, char **argv);
int builtin_type(int argc, char **argv);
int builtin_ulimit(int argc, char **argv);
int builtin_umask(int argc, char **argv);
int builtin_unalias(int argc, char **argv);
int builtin_wait(int argc, char **argv);

#endif /* MEOWSH_BUILTIN_H */
