/*
 * meowsh — POSIX-compliant shell
 * compat.c — Platform portability shims
 */

#define _POSIX_C_SOURCE 200809L

#include "compat.h"
#include "mystring.h"
#include "shell.h"

#include <dirent.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) ||     \
    defined(__APPLE__)
#include <sys/param.h>
#endif

int sh_cloexec(int fd, int set) {
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0)
    return -1;
  if (set)
    flags |= FD_CLOEXEC;
  else
    flags &= ~FD_CLOEXEC;
  return fcntl(fd, F_SETFD, flags);
}

sig_handler_t sh_signal(int sig, sig_handler_t handler) {
  struct sigaction sa, old;

  sa.sa_handler = handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  /* Restart interrupted system calls except for SIGCHLD */
  if (sig != SIGCHLD)
    sa.sa_flags |= SA_RESTART;
  if (sigaction(sig, &sa, &old) < 0)
    return SIG_ERR;
  return old.sa_handler;
}

