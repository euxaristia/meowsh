/*
 * meowsh — POSIX-compliant shell
 * jobs.c — Job table, process groups, SIGCHLD
 */

#define _POSIX_C_SOURCE 200809L

#include "jobs.h"
#include "compat.h"
#include "memalloc.h"
#include "mystring.h"
#include "options.h"
#include "sh_error.h"
#include "shell.h"
#include "trap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <ctype.h>

void jobs_init(void) {
  sh.jobs = NULL;
  sh.next_job_id = 1;

  if (sh.interactive && option_is_set(OPT_MONITOR)) {
    sh_signal(SIGCHLD, sigchld_handler);
  }
}

static void jobs_reap(void);
static struct job *job_by_id(int id);
static void job_foreground(struct job *j);
static void job_background(struct job *j);
static int job_last_status(struct job *j);

void job_update_status(pid_t pid, int status) {
  struct job *j;
  struct process *p;

  for (j = sh.jobs; j; j = j->next) {
    for (p = j->procs; p; p = p->next) {
      if (p->pid == pid) {
        p->status = status;
        if (WIFSTOPPED(status))
          p->state = PROC_STOPPED;
        else
          p->state = PROC_DONE;

        /* Update job state */
        {
          int all_done = 1, any_stopped = 0;
          const struct process *pp;
          for (pp = j->procs; pp; pp = pp->next) {
            if (pp->state == PROC_RUNNING)
              all_done = 0;
            if (pp->state == PROC_STOPPED)
              any_stopped = 1;
          }
          if (all_done)
            j->state = JOB_DONE;
          else if (any_stopped)
            j->state = JOB_STOPPED;
        }
        return;
      }
    }
  }
}

int job_wait_fg(struct job *j) {
  int status = 0;

  while (j->state == JOB_RUNNING) {
    int wstatus;
    pid_t pid = waitpid(-j->pgid, &wstatus, WUNTRACED);
    if (pid < 0) {
      if (errno == ECHILD)
        break;
      if (errno == EINTR)
        continue;
      break;
    }
    job_update_status(pid, wstatus);
  }

  if (j->state == JOB_STOPPED) {
    fprintf(stderr, "\n[%d]+  Stopped\t%s\n", j->id,
            j->cmd_text ? j->cmd_text : "");
    j->notified = 1;
  }

  /* Get status from last process in pipeline */
  status = job_last_status(j);

  /* Give terminal back to shell */
  if (sh.terminal_fd >= 0)
    tcsetpgrp(sh.terminal_fd, sh.shell_pgid);

  return status;
}

static void jobs_reap(void) {
  pid_t pid;
  int status;

  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0)
    job_update_status(pid, status);
}

void jobs_notify(void) {
  struct job *j;

  jobs_reap();
  for (j = sh.jobs; j; j = j->next) {
    if ((j->state == JOB_DONE || j->state == JOB_STOPPED) && !j->notified) {
      job_print(j, 0, stderr);
      j->notified = 1;
    }
  }
}

static struct job *job_by_id(int id) {
  struct job *j;

  for (j = sh.jobs; j; j = j->next)
    if (j->id == id)
      return j;
  return NULL;
}

struct job *job_current(void) {
  struct job *j;

  /* Most recent stopped job, or most recent running job */
  for (j = sh.jobs; j; j = j->next)
    if (j->state == JOB_STOPPED)
      return j;
  for (j = sh.jobs; j; j = j->next)
    if (j->state == JOB_RUNNING)
      return j;
  return NULL;
}

int job_continue(struct job *j, int foreground) {
  j->state = JOB_RUNNING;
  j->notified = 0;

  {
    struct process *p;
    for (p = j->procs; p; p = p->next)
      if (p->state == PROC_STOPPED)
        p->state = PROC_RUNNING;
  }

  if (foreground)
    job_foreground(j);
  else
    job_background(j);

  kill(-j->pgid, SIGCONT);

  if (foreground)
    return job_wait_fg(j);
  return 0;
}

static void job_foreground(struct job *j) {
  j->foreground = 1;
  if (sh.terminal_fd >= 0)
    tcsetpgrp(sh.terminal_fd, j->pgid);
}

static void job_background(struct job *j) {
  j->foreground = 0;
  fprintf(stderr, "[%d] %ld\n", j->id, (long)j->pgid);
}

void job_print(struct job *j, int verbose, FILE *out) {
  const char *state;

  switch (j->state) {
  case JOB_RUNNING:
    state = "Running";
    break;
  case JOB_STOPPED:
    state = "Stopped";
    break;
  case JOB_DONE:
    state = "Done";
    break;
  default:
    state = "???";
    break;
  }

  if (verbose) {
    struct process *p;
    fprintf(out, "[%d]  %s", j->id, state);
    for (p = j->procs; p; p = p->next)
      fprintf(out, "\t%ld %s", (long)p->pid, p->cmd ? p->cmd : "");
    fprintf(out, "\t%s\n", j->cmd_text ? j->cmd_text : "");
  } else {
    fprintf(out, "[%d]  %s\t\t%s\n", j->id, state,
            j->cmd_text ? j->cmd_text : "");
  }
}

void jobs_cleanup(void) {
  struct job **pp, *j;

  pp = &sh.jobs;
  while ((j = *pp) != NULL) {
    if (j->state == JOB_DONE && j->notified) {
      *pp = j->next;
      {
        struct process *p, *pnext;
        for (p = j->procs; p; p = pnext) {
          pnext = p->next;
          free(p->cmd);
          free(p);
        }
      }
      free(j->cmd_text);
      free(j);
    } else {
      pp = &j->next;
    }
  }
}

static int job_last_status(struct job *j) {
  struct process *p, *last = NULL;

  for (p = j->procs; p; p = p->next)
    last = p;

  if (!last)
    return 0;

  if (WIFEXITED(last->status))
    return WEXITSTATUS(last->status);
  if (WIFSIGNALED(last->status))
    return 128 + WTERMSIG(last->status);
  return 0;
}

struct job *job_parse_spec(const char *spec) {
  if (!spec)
    return job_current();

  if (spec[0] == '%') {
    spec++;
    if (*spec == '%' || *spec == '+' || *spec == '\0')
      return job_current();
    if (*spec == '-') {
      /* Previous job — second most recent */
      struct job *j, *prev = NULL;
      for (j = sh.jobs; j; j = j->next) {
        if (j != job_current())
          prev = j;
      }
      return prev;
    }
    if (isdigit((unsigned char)*spec)) {
      char *endp;
      int id = (int)sh_strtol(spec, &endp, 10);
      return job_by_id(id);
    }
    /* Match by command prefix */
    {
      struct job *j;
      for (j = sh.jobs; j; j = j->next) {
        if (j->cmd_text &&
            strncmp(j->cmd_text, spec, strlen(spec)) == 0) // flawfinder: ignore
          return j;
      }
    }
  }

  return NULL;
}

struct job *job_alloc(void) {
  struct job *j = sh_malloc(sizeof(*j));
  memset(j, 0, sizeof(*j));
  j->id = sh.next_job_id++;
  j->state = JOB_RUNNING;
  j->next = sh.jobs;
  sh.jobs = j;
  return j;
}

void job_add_proc(struct job *j, pid_t pid, const char *cmd) {
  struct process *p = sh_malloc(sizeof(*p));
  memset(p, 0, sizeof(*p));
  p->pid = pid;
  p->state = PROC_RUNNING;
  p->cmd = sh_strdup(cmd ? cmd : "");
  p->next = j->procs;
  j->procs = p;
}

void job_free(struct job *j) {
  struct job **pp;
  struct process *p, *next;

  for (pp = &sh.jobs; *pp; pp = &(*pp)->next) {
    if (*pp == j) {
      *pp = j->next;
      break;
    }
  }

  for (p = j->procs; p; p = next) {
    next = p->next;
    free(p->cmd);
    free(p);
  }
  free(j->cmd_text);
  free(j);
}
