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

void sh_closefrom(int lowfd) {
#if defined(__linux__) && defined(SYS_close_range)
  if (syscall(SYS_close_range, (unsigned int)lowfd, ~0U, 0) == 0)
    return;
#endif
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  closefrom(lowfd);
  return;
#else
  /* Fallback: iterate through file descriptors */
  {
    int maxfd;
    DIR *d;
    struct dirent *ent;

    d = opendir("/proc/self/fd");
    if (d) {
      while ((ent = readdir(d)) != NULL) {
        int fd;
        if (ent->d_name[0] == '.')
          continue;
        char *endp;
        fd = (int)sh_strtol(ent->d_name, &endp, 10);
        if (*endp != '\0')
          continue;
        if (fd >= lowfd && fd != dirfd(d))
          close(fd);
      }
      closedir(d);
      return;
    }
    maxfd = (int)sysconf(_SC_OPEN_MAX);
    if (maxfd < 0)
      maxfd = 1024;
    for (; lowfd < maxfd; lowfd++)
      close(lowfd);
  }
#endif
}

int sh_pipe(int fds[2]) {
#if defined(__linux__) && defined(O_CLOEXEC)
  if (pipe2(fds, O_CLOEXEC) == 0)
    return 0;
#endif
  if (pipe(fds) < 0)
    return -1;
  sh_cloexec(fds[0], 1);
  sh_cloexec(fds[1], 1);
  return 0;
}

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

int sh_nonblock(int fd, int set) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0)
    return -1;
  if (set)
    flags |= O_NONBLOCK;
  else
    flags &= ~O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags);
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

const char *sh_strsignal(int sig) {
  /* POSIX guarantees strsignal since Issue 7 (2008) */
  return strsignal(sig);
}
