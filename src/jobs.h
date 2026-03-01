/*
 * meowsh — POSIX-compliant shell
 * jobs.h — Job table, process groups, SIGCHLD
 */

#ifndef MEOWSH_JOBS_H
#define MEOWSH_JOBS_H

#include <stdio.h>
#include <sys/types.h>

typedef enum {
  PROC_RUNNING,
  PROC_STOPPED,
  PROC_DONE,
} proc_state_t;

typedef enum {
  JOB_RUNNING,
  JOB_STOPPED,
  JOB_DONE,
} job_state_t;

struct process {
  pid_t pid;
  int status;
  proc_state_t state;
  char *cmd;
  struct process *next;
};

struct job {
  int id;
  pid_t pgid;
  struct process *procs;
  job_state_t state;
  int notified;
  int foreground;
  char *cmd_text;
  struct job *next;
};

/* Initialize job control */
void jobs_init(void);

/* Create a new job */
struct job *job_alloc(void);

/* Add a process to a job */
void job_add_proc(struct job *j, pid_t pid, const char *cmd);

/* Free a job */
void job_free(struct job *j);

/* Update process status */
void job_update_status(pid_t pid, int status);

/* Wait for a foreground job */
int job_wait_fg(struct job *j);

/* Reap background jobs */

/* Print status of changed jobs and mark as notified */
void jobs_notify(void);

/* Lookup job by id or pid */
struct job *job_current(void);

/* Continue a stopped job */
int job_continue(struct job *j, int foreground);

/* Move job to foreground */

/* Move job to background */

/* Print job status */
void job_print(struct job *j, int verbose, FILE *out);

/* Remove completed jobs */
void jobs_cleanup(void);

/* Get last status of a job */

/* Parse job spec (%N, %string, etc.). Returns job or NULL. */
struct job *job_parse_spec(const char *spec);

#endif /* MEOWSH_JOBS_H */
