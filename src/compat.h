/*
 * meowsh — POSIX-compliant shell
 * compat.h — Platform portability shims
 */

#ifndef MEOWSH_COMPAT_H
#define MEOWSH_COMPAT_H

#include "shell.h"

/* Close all file descriptors >= lowfd */

/* Create a pipe; uses pipe2(O_CLOEXEC) where available */

/* Set or clear close-on-exec flag */
int sh_cloexec(int fd, int set);

/* Set a file descriptor to non-blocking */

/* Safe signal wrapper using sigaction */
typedef void (*sig_handler_t)(int);
sig_handler_t sh_signal(int sig, sig_handler_t handler);

/* strsignal fallback */

#endif /* MEOWSH_COMPAT_H */
