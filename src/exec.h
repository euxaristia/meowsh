/*
 * meowsh — POSIX-compliant shell
 * exec.h — Execution engine
 */

#ifndef MEOWSH_EXEC_H
#define MEOWSH_EXEC_H

#include "types.h"

/* Execute an AST node. flags: EXEC_xxx */
#define EXEC_BG     (1 << 0)   /* run in background */
#define EXEC_PIPE   (1 << 1)   /* part of a pipeline */
#define EXEC_NOFORK (1 << 2)   /* exec in current process (exec builtin) */

int exec_node(struct node *n, int flags);

/* Execute a simple command */

/* Execute a pipeline */

/* Search for command: special builtin, function, regular builtin, PATH */
typedef enum {
	CMD_SPECIAL_BUILTIN,
	CMD_FUNCTION,
	CMD_REGULAR_BUILTIN,
	CMD_EXTERNAL,
	CMD_NOT_FOUND,
} cmd_type_t;

struct cmd_entry {
	cmd_type_t type;
	union {
		int (*builtin)(int argc, char **argv);
		struct node *func;
		char *path;
	} u;
};

void find_command(const char *name, struct cmd_entry *entry);

/* Subshell execution */

#endif /* MEOWSH_EXEC_H */
