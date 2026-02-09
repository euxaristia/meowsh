/*
 * meowsh — POSIX-compliant shell
 * compat.h — Platform portability shims
 */

#ifndef MEOWSH_COMPAT_H
#define MEOWSH_COMPAT_H

#include "shell.h"

/* Close all file descriptors >= lowfd */
void sh_closefrom(int lowfd);

/* Create a pipe; uses pipe2(O_CLOEXEC) where available */
int sh_pipe(int fds[2]);

/* Set or clear close-on-exec flag */
int sh_cloexec(int fd, int set);

/* Set a file descriptor to non-blocking */
int sh_nonblock(int fd, int set);

/* Safe signal wrapper using sigaction */
typedef void (*sig_handler_t)(int);
sig_handler_t sh_signal(int sig, sig_handler_t handler);

/* strsignal fallback */
const char *sh_strsignal(int sig);

#endif /* MEOWSH_COMPAT_H */
