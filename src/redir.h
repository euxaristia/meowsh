/*
 * meowsh — POSIX-compliant shell
 * redir.h — I/O redirection setup/teardown
 */

#ifndef MEOWSH_REDIR_H
#define MEOWSH_REDIR_H

#include "types.h"

/* Saved fd for restoration */
struct saved_fd {
	int orig_fd;
	int saved_fd;
	struct saved_fd *next;
};

/* Apply redirections. Returns saved_fd list for later restore.
 * Returns NULL on error (with errno set). */
struct saved_fd *redir_apply(struct redirect *redirs);

/* Restore saved file descriptors */
void redir_restore(struct saved_fd *saved);

#endif /* MEOWSH_REDIR_H */
