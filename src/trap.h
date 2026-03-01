/*
 * meowsh — POSIX-compliant shell
 * trap.h — Signal handling, trap builtin
 */

#ifndef MEOWSH_TRAP_H
#define MEOWSH_TRAP_H

/* Initialize signal handlers */
void trap_init(void);

/* Set a trap action for a signal */
int trap_set(int sig, const char *action);

/* Check and execute pending traps */
void trap_check(void);

/* Clear all traps (for subshell) */
void trap_clear(void);

/* Reset traps to default (for exec'd process) */
void trap_reset(void);

/* Signal name to number, returns -1 on failure */
int trap_signum(const char *name);

/* Signal number to name */
const char *trap_signame(int sig);

/* Print all traps (for trap with no args) */
void trap_print(void);

/* Signal handler for SIGCHLD */
void sigchld_handler(int sig);

/* Block/unblock signals during critical sections */

#endif /* MEOWSH_TRAP_H */
