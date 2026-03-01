/*
 * meowsh — POSIX-compliant shell
 * exec.c — Execution engine (fork/exec, pipelines, lists)
 */

#define _POSIX_C_SOURCE 200809L

#include "exec.h"
#include "builtin.h"
#include "compat.h"
#include "expand.h"
#include "input.h"
#include "jobs.h"
#include "memalloc.h"
#include "mystring.h"
#include "options.h"
#include "parser.h"
#include "redir.h"
#include "sh_error.h"
#include "shell.h"
#include "trap.h"
#include "var.h"

#include <fnmatch.h>
#include <string.h>
#include <sys/stat.h>

/* Command hash table lookup/insert */
static const char *hash_lookup(const char *name) {
  unsigned int h = hash_string(name);
  struct hash_entry *he;

  for (he = sh.cmd_hash[h]; he; he = he->next) {
    if (strcmp(he->name, name) == 0)
      return he->path;
  }
  return NULL;
}

static void hash_insert(const char *name, const char *path) {
  unsigned int h = hash_string(name);
  struct hash_entry *he;

  for (he = sh.cmd_hash[h]; he; he = he->next) {
    if (strcmp(he->name, name) == 0) {
      free(he->path);
      he->path = sh_strdup(path);
      return;
    }
  }

  he = sh_malloc(sizeof(*he));
  he->name = sh_strdup(name);
  he->path = sh_strdup(path);
  he->next = sh.cmd_hash[h];
  sh.cmd_hash[h] = he;
}

/* Search PATH for an executable */
static char *search_path(const char *name) {
  const char *path_var;
  const char *p, *end;
  char fullpath[PATH_MAX]; // flawfinder: ignore
  struct stat st;

  if (strchr(name, '/')) {
    if (access(name, X_OK) == 0 && stat(name, &st) == 0 && S_ISREG(st.st_mode)) {
      return sh_strdup(name);
    }
    return NULL;
  }

  /* Check hash cache */
  {
    const char *cached = hash_lookup(name);
    if (cached) {
      if (access(cached, X_OK) == 0 && stat(cached, &st) == 0 &&
          S_ISREG(st.st_mode)) {
        return sh_strdup(cached);
      }
      /* Stale cache entry — fall through */
    }
  }

  path_var = var_get("PATH");
  if (!path_var)
    path_var = "/usr/bin:/bin";

  for (p = path_var;; p = end + 1) {
    end = strchr(p, ':');
    if (!end)
      end = p + strlen(p); // flawfinder: ignore // flawfinder: ignore

    if (end == p) {
      /* Empty component = current directory */
      snprintf(fullpath, sizeof(fullpath), "./%s", name);
    } else {
      snprintf(fullpath, sizeof(fullpath), "%.*s/%s", (int)(end - p), p, name);
    }

    if (access(fullpath, X_OK) == 0 && stat(fullpath, &st) == 0 &&
        S_ISREG(st.st_mode)) {
      if (option_is_set(OPT_HASHALL))
        hash_insert(name, fullpath);
      return sh_strdup(fullpath);
    }

    if (*end == '\0')
      break;
  }

  return NULL;
}

/* Function lookup */
static struct node *func_lookup(const char *name) {
  unsigned int h = hash_string(name);
  struct func_entry *fe;

  for (fe = sh.functions[h]; fe; fe = fe->next) {
    if (strcmp(fe->name, name) == 0)
      return fe->body;
  }
  return NULL;
}

void find_command(const char *name, struct cmd_entry *entry) {
  const struct builtin_entry *bi;
  struct node *func;

  /* 1. Special builtins */
  bi = builtin_lookup(name);
  if (bi && bi->special) {
    entry->type = CMD_SPECIAL_BUILTIN;
    entry->u.builtin = bi->fn;
    return;
  }

  /* 2. Functions */
  func = func_lookup(name);
  if (func) {
    entry->type = CMD_FUNCTION;
    entry->u.func = func;
    return;
  }

  /* 3. Regular builtins */
  if (bi) {
    entry->type = CMD_REGULAR_BUILTIN;
    entry->u.builtin = bi->fn;
    return;
  }

  /* 4. PATH search */
  {
    char *path = search_path(name);
    if (path) {
      entry->type = CMD_EXTERNAL;
      entry->u.path = path;
      return;
    }
  }

  entry->type = CMD_NOT_FOUND;
}

/* Apply pre-command assignments */
static void apply_assigns(struct assignment *assigns, int persist) {
  struct assignment *a;

  for (a = assigns; a; a = a->next) {
    char *value = a->value ? expand_word(a->value, 0) : sh_strdup("");
    var_set(a->name, value, 0);
    if (option_is_set(OPT_ALLEXPORT))
      var_export(a->name);
    free(value);
  }
}

/* Save and set temporary assignments, return save list */
struct saved_assign {
  char *name;
  char *old_value;
  int had_value;
  struct saved_assign *next;
};

static struct saved_assign *temp_assigns(struct assignment *assigns) {
  struct saved_assign *saved = NULL;
  struct assignment *a;

  for (a = assigns; a; a = a->next) {
    struct saved_assign *sa = sh_malloc(sizeof(*sa));
    const char *old = var_get(a->name);

    sa->name = sh_strdup(a->name);
    sa->old_value = old ? sh_strdup(old) : NULL;
    sa->had_value = (old != NULL);
    sa->next = saved;
    saved = sa;

    {
      char *value = a->value ? expand_word(a->value, 0) : sh_strdup("");
      var_set(a->name, value, 0);
      var_export(a->name);
      free(value);
    }
  }
  return saved;
}

static void restore_assigns(struct saved_assign *saved) {
  struct saved_assign *sa, *next;

  for (sa = saved; sa; sa = next) {
    next = sa->next;
    if (sa->had_value)
      var_set(sa->name, sa->old_value, 0);
    else
      var_unset(sa->name);
    free(sa->name);
    free(sa->old_value);
    free(sa);
  }
}

/* Execute function call */
static int exec_function(struct node *func_body, int argc, char **argv,
                         struct redirect *redirs) {
  struct saved_fd *saved_redir = NULL;
  int status;

  {
    saved_redir = redir_apply(redirs);
    if (!saved_redir)
      return 1;
  }

  var_push_posparams(argc - 1, argv + 1);
  sh.func_depth++;

  status = exec_node(func_body, 0);

  if (sh.want_return) {
    sh.want_return = 0;
    status = sh.last_status;
  }

  sh.func_depth--;
  var_pop_posparams();

  redir_restore(saved_redir);

  return status;
}

/* Register a function definition */
static void register_function(const char *name, struct node *body) {
  unsigned int h = hash_string(name);
  struct func_entry *fe;

  for (fe = sh.functions[h]; fe; fe = fe->next) {
    if (strcmp(fe->name, name) == 0) {
      fe->body = body;
      return;
    }
  }

  fe = sh_malloc(sizeof(*fe));
  fe->name = sh_strdup(name);
  fe->body = body;
  fe->next = sh.functions[h];
  sh.functions[h] = fe;
}

/* Xtrace: print command before execution */
static void xtrace(int argc, char **argv) {
  int i;
  const char *ps4;

  if (!option_is_set(OPT_XTRACE))
    return;

  ps4 = var_get("PS4");
  if (!ps4)
    ps4 = "+ ";
  fputs(ps4, stderr);
  for (i = 0; i < argc; i++) {
    if (i > 0)
      fputc(' ', stderr);
    fputs(argv[i], stderr);
  }
  fputc('\n', stderr);
}

/* Execute a simple command */
static int exec_simple_cmd(struct node *n, int flags) {
  char **argv;
  int argc;
  struct cmd_entry entry;
  struct saved_fd *saved_redir = NULL;
  int status = 0;

  if (!n)
    return 0;

  /* Assignment-only command */
  if (!n->data.simple.words) {
    if (n->data.simple.assigns) {
      /* Apply assignments persistently */
      apply_assigns(n->data.simple.assigns, 1);

      /* Apply redirections (for side effects) */
      if (n->data.simple.redirs) {
        saved_redir = redir_apply(n->data.simple.redirs);
        if (saved_redir)
          redir_restore(saved_redir);
      }
    }
    return 0;
  }

  /* Expand words */
  argv = expand_words(n->data.simple.words, &argc);
  if (argc == 0) {
    /* Expansion produced nothing */
    if (n->data.simple.assigns)
      apply_assigns(n->data.simple.assigns, 1);
    expand_free(argv);
    return 0;
  }

  xtrace(argc, argv);

  /* Find command */
  find_command(argv[0], &entry);

  switch (entry.type) {
  case CMD_SPECIAL_BUILTIN:
    /* Assignments persist for special builtins */
    if (n->data.simple.assigns)
      apply_assigns(n->data.simple.assigns, 1);

    if (n->data.simple.redirs) {
      saved_redir = redir_apply(n->data.simple.redirs);
      if (!saved_redir && n->data.simple.redirs) {
        status = 1;
        break;
      }
    }

    status = entry.u.builtin(argc, argv);

    if (saved_redir)
      redir_restore(saved_redir);
    break;

  case CMD_FUNCTION: {
    struct saved_assign *sa = NULL;
    if (n->data.simple.assigns)
      sa = temp_assigns(n->data.simple.assigns);

    status = exec_function(entry.u.func, argc, argv, n->data.simple.redirs);

    if (sa)
      restore_assigns(sa);
  } break;

  case CMD_REGULAR_BUILTIN: {
    struct saved_assign *sa = NULL;
    if (n->data.simple.assigns)
      sa = temp_assigns(n->data.simple.assigns);

    if (n->data.simple.redirs) {
      saved_redir = redir_apply(n->data.simple.redirs);
      if (!saved_redir && n->data.simple.redirs) {
        if (sa)
          restore_assigns(sa);
        status = 1;
        break;
      }
    }

    status = entry.u.builtin(argc, argv);

    if (saved_redir)
      redir_restore(saved_redir);
    if (sa)
      restore_assigns(sa);
  } break;

  case CMD_EXTERNAL: {
    /* DEBUG: Trace execution */
    /* fprintf(stderr, "[DEBUG] Executing external: %s\n", entry.u.path); */

    pid_t pid = fork();
    if (pid < 0) {
      sh_errorf("fork");
      status = 1;
    } else if (pid == 0) {
      /* Child process */
      trap_reset();

      if (sh.interactive && option_is_set(OPT_MONITOR)) {
        pid_t cpid = getpid();
        setpgid(cpid, cpid);
        if (!(flags & EXEC_BG)) {
          if (sh.terminal_fd >= 0) {
            /* Try multiple times or check for backgrounding if needed */
            tcsetpgrp(sh.terminal_fd, cpid);
          }
        }
        /* Reset all signals to default for the child */
        for (int i = 1; i < NSIG; i++) {
          if (i == SIGKILL || i == SIGSTOP) continue;
          signal(i, SIG_DFL);
        }
      }

      /* Apply assignments to environment */
      if (n->data.simple.assigns)
        apply_assigns(n->data.simple.assigns, 1);

      /* Apply redirections */
      if (n->data.simple.redirs) {
        if (!redir_apply(n->data.simple.redirs))
          if (n->data.simple.redirs)
            _exit(1);
      }

      {
        char **envp = var_environ();
        /* fprintf(stderr, "[DEBUG] Child execve: %s\n", entry.u.path); */
        execve(entry.u.path, argv, envp);
        sh_errorf("%s", argv[0]);
        _exit(errno == ENOENT ? 127 : 126);
      }
    } else {
      /* Parent */
      if (sh.interactive && option_is_set(OPT_MONITOR)) {
        setpgid(pid, pid);
        if (!(flags & EXEC_BG)) {
          if (sh.terminal_fd >= 0)
            tcsetpgrp(sh.terminal_fd, pid);
        }
      }

      /* fprintf(stderr, "[DEBUG] Parent waiting for pid %d\n", pid); */
      if (flags & EXEC_BG) {
        sh.last_bg_pid = pid;
        status = 0;
      } else {
        int wstatus;
        while (waitpid(pid, &wstatus, WUNTRACED) < 0) {
          if (errno != EINTR)
            break;
        }

        if (sh.interactive && option_is_set(OPT_MONITOR)) {
          if (sh.terminal_fd >= 0) {
            /* Hand terminal back to shell */
            tcsetpgrp(sh.terminal_fd, sh.shell_pgid);
          }
        }

        if (WIFEXITED(wstatus)) {
          status = WEXITSTATUS(wstatus);
        } else if (WIFSIGNALED(wstatus)) {
          status = 128 + WTERMSIG(wstatus);
        } else if (WIFSTOPPED(wstatus)) {
          status = 128 + WSTOPSIG(wstatus);
          fprintf(stderr, "\n[Stopped] %d\n", pid);
          /* We should really register this as a job */
        }
        /* fprintf(stderr, "[DEBUG] Child finished, status=%d\n", status); */
      }
    }
    free(entry.u.path);
  } break;

  case CMD_NOT_FOUND:
    sh_error("app or command %s: not found", argv[0]);
    status = 127;
    break;
  }

  expand_free(argv);
  sh.last_status = status;

  /* Check errexit */
  if (option_is_set(OPT_ERREXIT) && status != 0 && !sh.errexit_suppressed) {
    if (!sh.interactive)
      exit(status);
  }

  return status;
}

/* Execute a pipeline */
static int exec_pipeline(struct node *n) {
  int ncmds = n->data.pipeline.ncmds;
  struct node **cmds = n->data.pipeline.cmds;
  int i;
  int prev_fd = -1;
  pid_t *pids;
  int status = 0;

  if (ncmds == 1) {
    status = exec_node(cmds[0], 0);
    if (n->data.pipeline.bang)
      status = !status;
    sh.last_status = status;
    return status;
  }

  pids = sh_malloc(ncmds * sizeof(pid_t));

  for (i = 0; i < ncmds; i++) {
    int pipefd[2] = {-1, -1};

    if (i < ncmds - 1) {
      if (pipe(pipefd) < 0) {
        sh_errorf("pipe");
        break;
      }
    }

    pids[i] = fork();
    if (pids[i] < 0) {
      sh_errorf("fork");
      if (pipefd[0] >= 0)
        close(pipefd[0]);
      if (pipefd[1] >= 0)
        close(pipefd[1]);
      break;
    }

    if (pids[i] == 0) {
      /* Child */
      trap_reset();

      if (prev_fd >= 0) {
        dup2(prev_fd, STDIN_FILENO);
        close(prev_fd);
      }
      if (pipefd[1] >= 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
      }
      if (pipefd[0] >= 0)
        close(pipefd[0]);

      exec_node(cmds[i], 0);
      fflush(NULL);
      _exit(sh.last_status);
    }

    /* Parent */
    if (prev_fd >= 0)
      close(prev_fd);
    if (pipefd[1] >= 0)
      close(pipefd[1]);
    prev_fd = pipefd[0];
  }

  if (prev_fd >= 0)
    close(prev_fd);

  /* Wait for all children */
  for (i = 0; i < ncmds; i++) {
    int wstatus;
    if (pids[i] > 0) {
      while (waitpid(pids[i], &wstatus, 0) < 0) {
        if (errno != EINTR)
          break;
      }
      if (i == ncmds - 1) {
        if (WIFEXITED(wstatus))
          status = WEXITSTATUS(wstatus);
        else if (WIFSIGNALED(wstatus))
          status = 128 + WTERMSIG(wstatus);
      }
    }
  }

  free(pids);

  if (n->data.pipeline.bang)
    status = !status;

  sh.last_status = status;
  return status;
}

static void exec_subshell(struct node *body, struct redirect *redirs,
                          int flags) {
  pid_t pid;

  pid = fork();
  if (pid < 0) {
    sh_errorf("fork");
    sh.last_status = 1;
    return;
  }

  if (pid == 0) {
    /* Child */
    sh.subshell = 1;
    trap_clear();

    {
      if (!redir_apply(redirs))
        _exit(1);
    }

    exec_node(body, 0);
    fflush(NULL);
    _exit(sh.last_status);
  }

  /* Parent */
  if (flags & EXEC_BG) {
    sh.last_bg_pid = pid;
    sh.last_status = 0;
  } else {
    int wstatus;
    while (waitpid(pid, &wstatus, 0) < 0) {
      if (errno != EINTR)
        break;
    }
    if (WIFEXITED(wstatus))
      sh.last_status = WEXITSTATUS(wstatus);
    else if (WIFSIGNALED(wstatus))
      sh.last_status = 128 + WTERMSIG(wstatus);
  }
}

/* Main dispatch */
int exec_node(struct node *n, int flags) {
  int status = 0;

  if (!n)
    return 0;

  sh.lineno = n->lineno;

  switch (n->type) {
  case NODE_SIMPLE_CMD:
    status = exec_simple_cmd(n, flags);
    break;

  case NODE_PIPELINE:
    status = exec_pipeline(n);
    break;

  case NODE_AND_OR:
    if (!n->data.and_or.left || !n->data.and_or.right) {
      status = 1;
      break;
    }
    status = exec_node(n->data.and_or.left, 0);
    if (n->data.and_or.conn == CONN_AND) {
      if (status == 0) {
        sh.errexit_suppressed++;
        status = exec_node(n->data.and_or.right, 0);
        sh.errexit_suppressed--;
      }
    } else { /* CONN_OR */
      if (status != 0) {
        sh.errexit_suppressed++;
        status = exec_node(n->data.and_or.right, 0);
        sh.errexit_suppressed--;
      }
    }
    sh.last_status = status;
    break;

  case NODE_LIST: {
    int i;
    for (i = 0; i < n->data.list.nitems; i++) {
      if (n->data.list.connectors[i] == CONN_AMP) {
        exec_node(n->data.list.items[i], EXEC_BG);
      } else {
        status = exec_node(n->data.list.items[i], 0);
      }

      /* Check for break/continue */
      if (sh.break_count > 0 || sh.continue_count > 0)
        break;
      if (sh.want_return)
        break;
    }
  } break;

  case NODE_SUBSHELL:
    exec_subshell(n->data.group.body, n->data.group.redirs, flags);
    status = sh.last_status;
    break;

  case NODE_BRACE_GROUP: {
    struct saved_fd *saved = NULL;

    if (n->data.group.redirs) {
      saved = redir_apply(n->data.group.redirs);
      if (!saved && n->data.group.redirs) {
        sh.last_status = 1;
        return 1;
      }
    }

    status = exec_node(n->data.group.body, 0);

    if (saved)
      redir_restore(saved);
    sh.last_status = status;
  } break;

  case NODE_IF: {
    int old_suppress = sh.errexit_suppressed;
    if (!n->data.if_cmd.cond || !n->data.if_cmd.then_body) {
      sh.last_status = 1;
      return 1;
    }
    sh.errexit_suppressed = 1;
    status = exec_node(n->data.if_cmd.cond, 0);
    sh.errexit_suppressed = old_suppress;

    if (status == 0)
      status = exec_node(n->data.if_cmd.then_body, 0);
    else if (n->data.if_cmd.else_body)
      status = exec_node(n->data.if_cmd.else_body, 0);
    else
      status = 0;

    sh.last_status = status;
  } break;

  case NODE_WHILE: {
    struct saved_fd *saved = NULL;
    if (!n->data.loop.cond || !n->data.loop.body) {
      sh.last_status = 1;
      return 1;
    }
    if (n->data.loop.redirs) {
      saved = redir_apply(n->data.loop.redirs);
      if (!saved && n->data.loop.redirs) {
        sh.last_status = 1;
        return 1;
      }
    }

    sh.loop_depth++;
    status = 0;
    for (;;) {
      int cond;
      int old_suppress = sh.errexit_suppressed;
      sh.errexit_suppressed = 1;
      cond = exec_node(n->data.loop.cond, 0);
      sh.errexit_suppressed = old_suppress;

      if (cond != 0)
        break;

      status = exec_node(n->data.loop.body, 0);

      if (sh.break_count > 0) {
        sh.break_count--;
        break;
      }
      if (sh.continue_count > 0) {
        sh.continue_count--;
        continue;
      }
      if (sh.want_return)
        break;
    }
    sh.loop_depth--;

    if (saved)
      redir_restore(saved);
    sh.last_status = status;
  } break;

  case NODE_UNTIL: {
    struct saved_fd *saved = NULL;
    if (!n->data.loop.cond || !n->data.loop.body) {
      sh.last_status = 1;
      return 1;
    }
    if (n->data.loop.redirs) {
      saved = redir_apply(n->data.loop.redirs);
      if (!saved && n->data.loop.redirs) {
        sh.last_status = 1;
        return 1;
      }
    }

    sh.loop_depth++;
    status = 0;
    for (;;) {
      int cond;
      int old_suppress = sh.errexit_suppressed;
      sh.errexit_suppressed = 1;
      cond = exec_node(n->data.loop.cond, 0);
      sh.errexit_suppressed = old_suppress;

      if (cond == 0)
        break;

      status = exec_node(n->data.loop.body, 0);

      if (sh.break_count > 0) {
        sh.break_count--;
        break;
      }
      if (sh.continue_count > 0) {
        sh.continue_count--;
        continue;
      }
      if (sh.want_return)
        break;
    }
    sh.loop_depth--;

    if (saved)
      redir_restore(saved);
    sh.last_status = status;
  } break;

  case NODE_FOR: {
    struct saved_fd *saved = NULL;
    char **words;
    int wc, wi;

    if (!n->data.for_cmd.body) {
      sh.last_status = 1;
      return 1;
    }

    if (n->data.for_cmd.redirs) {
      saved = redir_apply(n->data.for_cmd.redirs);
      if (!saved && n->data.for_cmd.redirs) {
        sh.last_status = 1;
        return 1;
      }
    }

    if (n->data.for_cmd.has_in && n->data.for_cmd.words) {
      words = expand_words(n->data.for_cmd.words, &wc);
    } else if (!n->data.for_cmd.has_in) {
      /* for var; do ... — use "$@" */
      if (sh.posparams) {
        wc = sh.posparams->argc;
        words = sh_malloc((wc + 1) * sizeof(char *));
        for (wi = 0; wi < wc; wi++)
          words[wi] = sh_strdup(sh.posparams->argv[wi]);
        words[wc] = NULL;
      } else {
        wc = 0;
        words = sh_malloc(sizeof(char *));
        words[0] = NULL;
      }
    } else {
      /* for var in; do ... — empty list */
      wc = 0;
      words = sh_malloc(sizeof(char *));
      words[0] = NULL;
    }

    sh.loop_depth++;
    status = 0;
    for (wi = 0; wi < wc; wi++) {
      var_set(n->data.for_cmd.var, words[wi], 0);
      status = exec_node(n->data.for_cmd.body, 0);

      if (sh.break_count > 0) {
        sh.break_count--;
        break;
      }
      if (sh.continue_count > 0) {
        sh.continue_count--;
        continue;
      }
      if (sh.want_return)
        break;
    }
    sh.loop_depth--;
    expand_free(words);

    if (saved)
      redir_restore(saved);
    sh.last_status = status;
  } break;

  case NODE_CASE: {
    char *subject;
    struct node *item;
    struct saved_fd *saved = NULL;

    if (n->data.case_cmd.redirs) {
      saved = redir_apply(n->data.case_cmd.redirs);
      if (!saved && n->data.case_cmd.redirs) {
        sh.last_status = 1;
        return 1;
      }
    }

    subject = expand_word(n->data.case_cmd.subject, 0);
    status = 0;

    for (item = n->data.case_cmd.items; item;
         item = item->data.case_item.next_item) {
      const struct word *pat;
      int matched = 0;

      for (pat = item->data.case_item.patterns; pat; pat = pat->next) {
        char *pattern = expand_word(pat, 0);
        if (fnmatch(pattern, subject, 0) == 0)
          matched = 1;
        free(pattern);
        if (matched)
          break;
      }

      if (matched) {
        if (item->data.case_item.body)
          status = exec_node(item->data.case_item.body, 0);
        break;
      }
    }

    free(subject);
    if (saved)
      redir_restore(saved);
    sh.last_status = status;
  } break;

  case NODE_FUNC_DEF:
    register_function(n->data.func.name, n->data.func.body);
    status = 0;
    sh.last_status = 0;
    break;

  case NODE_BANG:
    status = exec_node(n->data.bang.cmd, 0);
    status = !status;
    sh.last_status = status;
    break;

  default:
    sh_error("internal: unknown node type %d", n->type);
    status = 2;
    break;
  }

  return status;
}
