/*
 * meowsh — POSIX-compliant shell
 * builtin_regular.c — Regular builtins
 */

#define _POSIX_C_SOURCE 200809L

#include "alias.h"
#include "builtin.h"
#include "exec.h"
#include "jobs.h"
#include "lineedit.h"
#include "memalloc.h"
#include "mystring.h"
#include "options.h"
#include "sh_error.h"
#include "shell.h"
#include "trap.h"
#include "var.h"

#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>

/* alias [-p] [name[=value]...] */
int builtin_alias(int argc, char **argv) {
  int i, status = 0;

  if (argc == 1 || (argc == 2 && strcmp(argv[1], "-p") == 0)) {
    alias_print_all();
    return 0;
  }

  for (i = 1; i < argc; i++) {
    const char *eq = strchr(argv[i], '=');
    if (eq) {
      char *name = sh_malloc((size_t)(eq - argv[i]) + 1);
      memcpy(name, argv[i], (size_t)(eq - argv[i])); // flawfinder: ignore
      name[eq - argv[i]] = '\0';
      alias_set(name, eq + 1);
      free(name);
    } else {
      if (alias_print(argv[i]) < 0) {
        sh_error("alias: %s: not found", argv[i]);
        status = 1;
      }
    }
  }
  return status;
}

/* bg [job_id...] */
int builtin_bg(int argc, char **argv) {
  struct job *j;

  if (argc < 2)
    j = job_current();
  else
    j = job_parse_spec(argv[1]);

  if (!j) {
    sh_error("bg: no current job");
    return 1;
  }

  job_continue(j, 0);
  return 0;
}

/* cd [-L|-P] [directory] */
int builtin_cd(int argc, char **argv) {
  const char *dir = NULL;
  const char *oldpwd;
  char resolved[PATH_MAX]; // flawfinder: ignore
  int i;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-L") == 0)
      ; /* physical = 0 */
    else if (strcmp(argv[i], "-P") == 0)
      ; /* physical = 1 */
    else if (strcmp(argv[i], "-") == 0) {
      dir = var_get("OLDPWD");
      if (!dir) {
        sh_error("cd: OLDPWD not set");
        return 1;
      }
      printf("%s\n", dir);
    } else if (argv[i][0] != '-') {
      dir = argv[i];
    }
  }

  if (!dir) {
    dir = var_get("HOME");
    if (!dir) {
      sh_error("cd: HOME not set");
      return 1;
    }
  }

  oldpwd = var_get("PWD");

  if (chdir(dir) < 0) {
    sh_errorf("cd: %s", dir);
    return 1;
  }

  if (oldpwd)
    var_set("OLDPWD", oldpwd, 1);

  if (getcwd(resolved, sizeof(resolved)))
    var_set("PWD", resolved, 1);

  return 0;
}

/* command [-pVv] command [arguments...] */
int builtin_command(int argc, char **argv) {
  int verbose = 0;
  int type_mode = 0;
  int i;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0)
      verbose = 1;
    else if (strcmp(argv[i], "-V") == 0)
      type_mode = 1;
    else if (strcmp(argv[i], "-p") == 0)
      ; /* use default PATH */
    else
      break;
  }

  if (i >= argc)
    return 0;

  if (verbose || type_mode) {
    struct cmd_entry entry;
    find_command(argv[i], &entry);
    switch (entry.type) {
    case CMD_SPECIAL_BUILTIN:
    case CMD_REGULAR_BUILTIN:
      printf("%s\n", argv[i]);
      break;
    case CMD_FUNCTION:
      if (type_mode)
        printf("%s is a function\n", argv[i]);
      else
        printf("%s\n", argv[i]);
      break;
    case CMD_EXTERNAL:
      printf("%s\n", entry.u.path);
      free(entry.u.path);
      break;
    case CMD_NOT_FOUND:
      return 1;
    }
    return 0;
  }

  /* Execute command bypassing functions */
  {
    const struct builtin_entry *bi = builtin_lookup(argv[i]);
    if (bi)
      return bi->fn(argc - i, argv + i);
  }

  /* External command */
  {
    char **envp = var_environ();
    char *path;

    if (strchr(argv[i], '/'))
      path = sh_strdup(argv[i]);
    else {
      const char *pvar = var_get("PATH");
      const char *p, *end;
      char fp[PATH_MAX]; // flawfinder: ignore

      path = NULL;
      if (pvar) {
        for (p = pvar;; p = end + 1) {
          end = strchr(p, ':');
          if (!end)
            end = p + strlen(p); // flawfinder: ignore
          snprintf(fp, sizeof(fp), "%.*s/%s", (int)(end - p), p, argv[i]);
          int fd = open(fp, O_RDONLY | O_NOFOLLOW); // flawfinder: ignore
          if (fd >= 0) {
            close(fd);
            path = sh_strdup(fp);
            break;
          }
          if (*end == '\0')
            break;
        }
      }
    }

    if (!path) {
      sh_error("%s: not found", argv[i]);
      var_environ_free(envp);
      return 127;
    }

    {
      pid_t pid = fork();
      if (pid == 0) {
        execve(path, argv + i, envp);
        _exit(127);
      } else if (pid > 0) {
        int wstatus;
        waitpid(pid, &wstatus, 0);
        free(path);
        var_environ_free(envp);
        if (WIFEXITED(wstatus))
          return WEXITSTATUS(wstatus);
        if (WIFSIGNALED(wstatus))
          return 128 + WTERMSIG(wstatus);
      } else {
        sh_errorf("fork");
      }
      free(path);
      var_environ_free(envp);
    }
  }
  return 1;
}

/* false */
int builtin_false(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return 1;
}

/* fc — minimal implementation */
int builtin_fc(int argc, char **argv) {
  (void)argc;
  (void)argv;
  sh_error("fc: not implemented (no history support)");
  return 1;
}

/* fg [job_id] */
int builtin_fg(int argc, char **argv) {
  struct job *j;

  if (argc < 2)
    j = job_current();
  else
    j = job_parse_spec(argv[1]);

  if (!j) {
    sh_error("fg: no current job");
    return 1;
  }

  job_continue(j, 1);
  return sh.last_status;
}

/* getopts optstring name [args...] */
int builtin_getopts(int argc, char **argv) {
  const char *optstring;
  const char *varname;
  char **args;
  int nargs;
  int optind_val;
  const char *optind_str;
  char c;
  const char *p;

  if (argc < 3) {
    sh_error("getopts: usage: getopts optstring name [args]");
    return 2;
  }

  optstring = argv[1];
  varname = argv[2];

  if (argc > 3) {
    args = argv + 3;
    nargs = argc - 3;
  } else {
    if (sh.posparams) {
      args = sh.posparams->argv;
      nargs = sh.posparams->argc;
    } else {
      args = NULL;
      nargs = 0;
    }
  }

  optind_str = var_get("OPTIND");
  if (optind_str) {
    char *endp;
    optind_val = (int)sh_strtol(optind_str, &endp, 10);
    if (*endp != '\0')
      optind_val = 1;
  } else {
    optind_val = 1;
  }

  if (optind_val < 1 || optind_val > nargs) {
    var_set(varname, "?", 0);
    return 1;
  }

  {
    const char *arg = args[optind_val - 1];

    if (!arg || arg[0] != '-' || strcmp(arg, "-") == 0) {
      var_set(varname, "?", 0);
      return 1;
    }
    if (strcmp(arg, "--") == 0) {
      {
        char buf[16]; // flawfinder: ignore
        snprintf(buf, sizeof(buf), "%d", optind_val + 1);
        var_set("OPTIND", buf, 0);
      }
      var_set(varname, "?", 0);
      return 1;
    }

    c = arg[1];
    if (!c) {
      var_set(varname, "?", 0);
      return 1;
    }

    p = strchr(optstring, c);
    if (!p) {
      var_set(varname, "?", 0);
      var_set("OPTARG", "", 0);
      {
        char buf[16]; // flawfinder: ignore
        snprintf(buf, sizeof(buf), "%d", optind_val + 1);
        var_set("OPTIND", buf, 0);
      }
      if (optstring[0] != ':')
        sh_error("getopts: illegal option -- %c", c);
      return 0;
    }

    {
      const char val[2] = {c, '\0'}; // flawfinder: ignore
      var_set(varname, val, 0);
    }

    if (p[1] == ':') {
      /* Option requires argument */
      if (arg[2]) {
        var_set("OPTARG", arg + 2, 0);
      } else {
        optind_val++;
        if (optind_val > nargs) {
          if (optstring[0] == ':') {
            var_set(varname, ":", 0);
            char val[2] = {c, '\0'}; // flawfinder: ignore
            var_set("OPTARG", val, 0);
          } else {
            sh_error("getopts: option requires argument -- %c", c);
            var_set(varname, "?", 0);
          }
          {
            char buf[16]; // flawfinder: ignore
            snprintf(buf, sizeof(buf), "%d", optind_val);
            var_set("OPTIND", buf, 0);
          }
          return 0;
        }
        var_set("OPTARG", args[optind_val - 1], 0);
      }
    }

    {
      char buf[16]; // flawfinder: ignore
      snprintf(buf, sizeof(buf), "%d", optind_val + 1);
      var_set("OPTIND", buf, 0);
    }
  }

  return 0;
}

int builtin_history(int argc, char **argv) {
  if (argc == 2 &&
      (strcmp(argv[1], "-c") == 0 || strcmp(argv[1], "--clear") == 0 ||
       strcmp(argv[1], "clear") == 0)) {
    history_clear();
    if (sh.history_file)
      history_save(sh.history_file);
    return 0;
  }
  if (argc != 1) {
    sh_error("history: usage: history [-c|--clear|clear]");
    return 2;
  }

  lineedit_print_history();
  return 0;
}

/* hash [-r] [name...] */
int builtin_hash(int argc, char **argv) {
  int i;

  if (argc >= 2 && strcmp(argv[1], "-r") == 0) {
    /* Clear hash table */
    int j;
    for (j = 0; j < HASH_SIZE; j++) {
      struct hash_entry *he = sh.cmd_hash[j];
      while (he) {
        struct hash_entry *next = he->next;
        free(he->name);
        free(he->path);
        free(he);
        he = next;
      }
      sh.cmd_hash[j] = NULL;
    }
    return 0;
  }

  if (argc == 1) {
    /* Print hash table */
    int j;
    for (j = 0; j < HASH_SIZE; j++) {
      const struct hash_entry *he;
      for (he = sh.cmd_hash[j]; he; he = he->next)
        printf("%s\t%s\n", he->name, he->path);
    }
    return 0;
  }

  /* Hash specific commands */
  for (i = 1; i < argc; i++) {
    struct cmd_entry entry;
    find_command(argv[i], &entry);
    if (entry.type == CMD_EXTERNAL)
      free(entry.u.path);
    else if (entry.type == CMD_NOT_FOUND) {
      sh_error("hash: %s: not found", argv[i]);
      return 1;
    }
  }
  return 0;
}

/* jobs [-l|-p] [job_id...] */
int builtin_jobs(int argc, char **argv) {
  int verbose = 0;
  int pid_only = 0;
  struct job *j;
  int i;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-l") == 0)
      verbose = 1;
    else if (strcmp(argv[i], "-p") == 0)
      pid_only = 1;
    else
      break;
  }

  if (i < argc) {
    for (; i < argc; i++) {
      j = job_parse_spec(argv[i]);
      if (j) {
        if (pid_only)
          printf("%ld\n", (long)j->pgid);
        else
          job_print(j, verbose, stdout);
      } else {
        sh_error("jobs: %s: no such job", argv[i]);
      }
    }
  } else {
    for (j = sh.jobs; j; j = j->next) {
      if (pid_only)
        printf("%ld\n", (long)j->pgid);
      else
        job_print(j, verbose, stdout);
    }
  }
  return 0;
}

/* kill [-s signal | -signal] pid|job... */
int builtin_kill(int argc, char **argv) {
  int sig = SIGTERM;
  int i = 1;

  if (argc < 2) {
    sh_error("kill: usage: kill [-s signal] pid...");
    return 2;
  }

  if (i < argc && strcmp(argv[i], "-l") == 0) {
    /* List signals */
    int s;
    for (s = 1; s < NSIG; s++) {
      const char *name = trap_signame(s);
      if (name)
        printf("%d) %s\n", s, name);
    }
    return 0;
  }

  if (i < argc && strcmp(argv[i], "-s") == 0) {
    i++;
    if (i >= argc) {
      sh_error("kill: -s requires signal name");
      return 2;
    }
    sig = trap_signum(argv[i]);
    if (sig < 0) {
      sh_error("kill: %s: bad signal", argv[i]);
      return 1;
    }
    i++;
  } else if (i < argc && argv[i][0] == '-' &&
             !isdigit((unsigned char)argv[i][1])) {
    sig = trap_signum(argv[i] + 1);
    if (sig < 0) {
      sh_error("kill: %s: bad signal", argv[i] + 1);
      return 1;
    }
    i++;
  }

  for (; i < argc; i++) {
    if (argv[i][0] == '%') {
      const struct job *j = job_parse_spec(argv[i]);
      if (j) {
        if (kill(-j->pgid, sig) < 0)
          sh_errorf("kill");
      } else {
        sh_error("kill: %s: no such job", argv[i]);
      }
    } else {
      char *endp;
      pid_t pid = (pid_t)sh_strtol(argv[i], &endp, 10);
      if (kill(pid, sig) < 0)
        sh_errorf("kill");
    }
  }
  return 0;
}

/* newgrp [group] */
int builtin_newgrp(int argc, char **argv) {
  (void)argc;
  (void)argv;
  sh_error("newgrp: not implemented");
  return 1;
}

/* pwd [-L|-P] */
int builtin_pwd(int argc, char **argv) {
  int physical = 0;
  int i;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-P") == 0)
      physical = 1;
    else if (strcmp(argv[i], "-L") == 0)
      physical = 0;
  }

  if (physical) {
    char cwd[PATH_MAX]; // flawfinder: ignore
    if (getcwd(cwd, sizeof(cwd))) {
      printf("%s\n", cwd);
      return 0;
    }
  } else {
    const char *pwd = var_get("PWD");
    if (pwd) {
      printf("%s\n", pwd);
      return 0;
    }
    {
      char cwd[PATH_MAX]; // flawfinder: ignore
      if (getcwd(cwd, sizeof(cwd))) {
        printf("%s\n", cwd);
        return 0;
      }
    }
  }

  sh_errorf("pwd");
  return 1;
}

/* read [-r] var... */
int builtin_read(int argc, char **argv) {
  int raw = 0;
  int i = 1;
  struct strbuf sb = STRBUF_INIT;
  int c;
  const char *ifs;
  char **vars;
  int nvars;

  if (i < argc && strcmp(argv[i], "-r") == 0) {
    raw = 1;
    i++;
  }

  vars = argv + i;
  nvars = argc - i;
  if (nvars == 0) {
    /* Default variable is REPLY */
    static char *reply_var[] = {"REPLY", NULL};
    vars = reply_var;
    nvars = 1;
  }

  /* Read one line from stdin */
  for (;;) {
    c = fgetc(stdin); // flawfinder: ignore
    if (c == EOF) {
      if (sb.len > 0)
        break;
      strbuf_free(&sb);
      return 1;
    }
    if (c == '\n')
      break;
    if (!raw && c == '\\') {
      c = fgetc(stdin); // flawfinder: ignore
      if (c == '\n')
        continue; /* line continuation */
      if (c == EOF)
        break;
      strbuf_addch(&sb, (char)c);
    } else {
      strbuf_addch(&sb, (char)c);
    }
  }

  /* Split by IFS into variables */
  ifs = var_get("IFS");
  if (!ifs)
    ifs = " \t\n";

  {
    const char *p = sb.buf ? sb.buf : "";
    int vi;

    for (vi = 0; vi < nvars; vi++) {
      /* Skip leading IFS whitespace */
      while (*p && (*p == ' ' || *p == '\t' || *p == '\n') && strchr(ifs, *p))
        p++;

      if (vi == nvars - 1) {
        /* Last variable gets the rest */
        /* Trim trailing IFS whitespace */
        size_t len = strlen(p); // flawfinder: ignore
        while (len > 0 && strchr(ifs, p[len - 1]) &&
               (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\n'))
          len--;
        {
          char *val = sh_malloc(len + 1);
          memcpy(val, p, len); // flawfinder: ignore
          val[len] = '\0';
          var_set(vars[vi], val, 0);
          free(val);
        }
      } else {
        const char *start = p;
        while (*p && !strchr(ifs, *p))
          p++;
        {
          size_t len = (size_t)(p - start);
          char *val = sh_malloc(len + 1);
          memcpy(val, start, len); // flawfinder: ignore
          val[len] = '\0';
          var_set(vars[vi], val, 0);
          free(val);
        }
        /* Skip IFS delimiter */
        if (*p && strchr(ifs, *p))
          p++;
      }
    }
  }

  strbuf_free(&sb);
  return 0;
}

/* true */
int builtin_true(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return 0;
}

/* type name... */
int builtin_type(int argc, char **argv) {
  int i, status = 0;

  for (i = 1; i < argc; i++) {
    struct cmd_entry entry;
    find_command(argv[i], &entry);
    switch (entry.type) {
    case CMD_SPECIAL_BUILTIN:
      printf("%s is a special shell builtin\n", argv[i]);
      break;
    case CMD_REGULAR_BUILTIN:
      printf("%s is a shell builtin\n", argv[i]);
      break;
    case CMD_FUNCTION:
      printf("%s is a function\n", argv[i]);
      break;
    case CMD_EXTERNAL:
      printf("%s is %s\n", argv[i], entry.u.path);
      free(entry.u.path);
      break;
    case CMD_NOT_FOUND:
      sh_error("%s: not found", argv[i]);
      status = 1;
      break;
    }
  }
  return status;
}

/* ulimit [-f] [limit] — simplified */
int builtin_ulimit(int argc, char **argv) {
  int resource_type = RLIMIT_FSIZE;
  struct rlimit rl;
  int i = 1;

  while (i < argc && argv[i][0] == '-') {
    switch (argv[i][1]) {
    case 'f':
      resource_type = RLIMIT_FSIZE;
      break;
    case 'c':
      resource_type = RLIMIT_CORE;
      break;
    case 'd':
      resource_type = RLIMIT_DATA;
      break;
    case 'n':
      resource_type = RLIMIT_NOFILE;
      break;
    case 's':
      resource_type = RLIMIT_STACK;
      break;
    case 't':
      resource_type = RLIMIT_CPU;
      break;
    case 'v':
#ifdef RLIMIT_AS
      resource_type = RLIMIT_AS;
#endif
      break;
    case 'a':
      /* Print all limits */
      {
        static const struct {
          int res;
          const char *name;
        } limits[] = {{RLIMIT_CORE, "core file size"},
                      {RLIMIT_DATA, "data seg size"},
                      {RLIMIT_FSIZE, "file size"},
                      {RLIMIT_NOFILE, "open files"},
                      {RLIMIT_STACK, "stack size"},
                      {RLIMIT_CPU, "cpu time"},
                      {-1, NULL}};
        int j;
        for (j = 0; limits[j].name; j++) {
          if (getrlimit(limits[j].res, &rl) == 0) {
            printf("%-20s ", limits[j].name);
            if (rl.rlim_cur == RLIM_INFINITY)
              printf("unlimited\n");
            else
              printf("%lld\n", (long long)rl.rlim_cur);
          }
        }
      }
      return 0;
    default:
      sh_error("ulimit: bad option: %s", argv[i]);
      return 2;
    }
    i++;
  }

  if (i >= argc) {
    /* Print current limit */
    if (getrlimit(resource_type, &rl) < 0) {
      sh_errorf("getrlimit");
      return 1;
    }
    if (rl.rlim_cur == RLIM_INFINITY)
      printf("unlimited\n");
    else
      printf("%lld\n", (long long)rl.rlim_cur);
    return 0;
  }

  /* Set limit */
  if (strcmp(argv[i], "unlimited") == 0) {
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
  } else {
    rl.rlim_cur = (rlim_t)atoll(argv[i]);
    rl.rlim_max = rl.rlim_cur;
  }

  if (setrlimit(resource_type, &rl) < 0) {
    sh_errorf("setrlimit");
    return 1;
  }
  return 0;
}

/* umask [-S] [mode] */
int builtin_umask(int argc, char **argv) {
  int symbolic = 0;
  int i = 1;
  mode_t mask;

  if (i < argc && strcmp(argv[i], "-S") == 0) {
    symbolic = 1;
    i++;
  }

  if (i >= argc) {
    mask = umask(0); // flawfinder: ignore
    umask(mask);     // flawfinder: ignore
    if (symbolic) {
      printf("u=%s%s%s,g=%s%s%s,o=%s%s%s\n", (mask & S_IRUSR) ? "" : "r",
             (mask & S_IWUSR) ? "" : "w", (mask & S_IXUSR) ? "" : "x",
             (mask & S_IRGRP) ? "" : "r", (mask & S_IWGRP) ? "" : "w",
             (mask & S_IXGRP) ? "" : "x", (mask & S_IROTH) ? "" : "r",
             (mask & S_IWOTH) ? "" : "w", (mask & S_IXOTH) ? "" : "x");
    } else {
      printf("%04o\n", (unsigned)mask);
    }
    return 0;
  }

  mask = (mode_t)strtol(argv[i], NULL, 8);
  umask(mask); // flawfinder: ignore
  return 0;
}

/* unalias [-a] name... */
int builtin_unalias(int argc, char **argv) {
  int i, status = 0;

  if (argc >= 2 && strcmp(argv[1], "-a") == 0) {
    alias_unset_all();
    return 0;
  }

  for (i = 1; i < argc; i++) {
    if (alias_unset(argv[i]) < 0) {
      sh_error("unalias: %s: not found", argv[i]);
      status = 1;
    }
  }
  return status;
}

/* echo [arguments...] */
int builtin_echo(int argc, char **argv) {
  int i;
  int newline = 1;

  i = 1;
  if (i < argc && strcmp(argv[i], "-n") == 0) {
    newline = 0;
    i++;
  }

  for (; i < argc; i++) {
    printf("%s%s", argv[i], (i + 1 < argc) ? " " : "");
  }

  if (newline)
    printf("\n");

  return 0;
}

/* meow */
int builtin_meow(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("  /\\_/\\\n");
  printf(" ( o.o )\n");
  printf("  > ^ <  meow!\n");
  return 0;
}

/* wait [pid|job...] */
int builtin_wait(int argc, char **argv) {
  int i, status = 0;

  if (argc < 2) {
    /* Wait for all children */
    int wstatus;
    pid_t pid;
    while ((pid = waitpid(-1, &wstatus, 0)) > 0) {
      job_update_status(pid, wstatus);
      if (WIFEXITED(wstatus))
        status = WEXITSTATUS(wstatus);
      else if (WIFSIGNALED(wstatus))
        status = 128 + WTERMSIG(wstatus);
    }
    return status;
  }

  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '%') {
      struct job *j = job_parse_spec(argv[i]);
      if (j)
        status = job_wait_fg(j);
      else {
        sh_error("wait: %s: no such job", argv[i]);
        status = 127;
      }
    } else {
      char *endp;
      pid_t pid = (pid_t)sh_strtol(argv[i], &endp, 10);
      int wstatus;
      if (waitpid(pid, &wstatus, 0) < 0) {
        status = 127;
      } else {
        if (WIFEXITED(wstatus))
          status = WEXITSTATUS(wstatus);
        else if (WIFSIGNALED(wstatus))
          status = 128 + WTERMSIG(wstatus);
      }
    }
  }
  return status;
}
