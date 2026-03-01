/*
 * meowsh — POSIX-compliant shell
 * builtin_special.c — Special builtins per POSIX
 */

#define _POSIX_C_SOURCE 200809L

#include "builtin.h"
#include "exec.h"
#include "input.h"
#include "memalloc.h"
#include "mystring.h"
#include "options.h"
#include "parser.h"
#include "sh_error.h"
#include "shell.h"
#include "trap.h"
#include "var.h"

#include <sys/times.h>
#include <time.h>

/* break [n] */
int builtin_break(int argc, char **argv) {
  int n = 1;

  if (argc > 1) {
    char *endp;
    n = (int)sh_strtol(argv[1], &endp, 10);
    if (n < 1)
      n = 1;
  }

  if (sh.loop_depth == 0) {
    sh_error("break: not in a loop");
    return 0;
  }

  sh.break_count = n;
  return 0;
}

/* : [arguments] */
int builtin_colon(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return 0;
}

/* continue [n] */
int builtin_continue(int argc, char **argv) {
  int n = 1;

  if (argc > 1) {
    char *endp;
    n = (int)sh_strtol(argv[1], &endp, 10);
    if (n < 1)
      n = 1;
  }

  if (sh.loop_depth == 0) {
    sh_error("continue: not in a loop");
    return 0;
  }

  sh.continue_count = n;
  return 0;
}

/* . filename [arguments] */
int builtin_dot(int argc, char **argv) {
  const char *filename;
  char fullpath[PATH_MAX]; // flawfinder: ignore
  int status;

  if (argc < 2) {
    sh_error(".: missing filename");
    return 2;
  }

  filename = argv[1];

  /* If no slash, search PATH */
  if (!strchr(filename, '/')) {
    const char *path = var_get("PATH");
    const char *p, *end;

    if (path) {
      for (p = path;; p = end + 1) {
        end = strchr(p, ':');
        if (!end)
          end = p + strlen(p); // flawfinder: ignore
        if (end == p)
          snprintf(fullpath, sizeof(fullpath), "./%s", filename);
        else
          snprintf(fullpath, sizeof(fullpath), "%.*s/%s", (int)(end - p), p,
                   filename);
        int fd = open(fullpath, O_RDONLY | O_NOFOLLOW); // flawfinder: ignore
        if (fd >= 0) {
          close(fd);
          filename = fullpath;
          goto found;
        }
        if (*end == '\0')
          break;
      }
    }
    /* Not found in PATH; try as-is */
  }

found: {
  int fd = open(filename, O_RDONLY | O_NOFOLLOW); // flawfinder: ignore
  if (fd < 0) {
    sh_errorf("%s", filename);
    return 1;
  }
  close(fd);
}

  /* Push positional params if extra args */
  if (argc > 2)
    var_push_posparams(argc - 2, argv + 2);

  sh.dot_depth++;
  input_push_file(filename);

  /* Parse and execute */
  status = 0;
  for (;;) {
    struct node *tree;
    arena_free(&parse_arena);
    tree = parse_command(NULL, NULL);
    if (!tree) {
      int c = input_getc();
      if (c < 0)
        break;
      input_ungetc(c);
      continue;
    }
    status = exec_node(tree, 0);
    if (sh.want_return)
      break;
  }

  input_pop();
  sh.dot_depth--;

  if (sh.want_return)
    sh.want_return = 0;

  if (argc > 2)
    var_pop_posparams();

  sh.last_status = status;
  return status;
}

/* eval [arguments...] */
int builtin_eval(int argc, char **argv) {
  struct strbuf sb = STRBUF_INIT;
  int i, status = 0;

  if (argc <= 1)
    return 0;

  for (i = 1; i < argc; i++) {
    if (i > 1)
      strbuf_addch(&sb, ' ');
    strbuf_addstr(&sb, argv[i]);
  }

  input_push_string(sb.buf);
  strbuf_free(&sb);

  for (;;) {
    struct node *tree;
    arena_free(&parse_arena);
    tree = parse_command(NULL, NULL);
    if (!tree) {
      int c = input_getc();
      if (c < 0)
        break;
      input_ungetc(c);
      continue;
    }
    status = exec_node(tree, 0);
  }

  input_pop();
  sh.last_status = status;
  return status;
}

/* exec [command [arguments...]] */
int builtin_exec(int argc, char **argv) {
  char *path;
  char **envp;

  if (argc <= 1) {
    /* exec with no args — just apply redirections (handled by caller) */
    return 0;
  }

  path = NULL;
  if (strchr(argv[1], '/')) {
    path = sh_strdup(argv[1]);
  } else {
    const char *pvar = var_get("PATH");
    const char *p, *end;
    char fp[PATH_MAX]; // flawfinder: ignore

    if (pvar) {
      for (p = pvar;; p = end + 1) {
        end = strchr(p, ':');
        if (!end)
          end = p + strlen(p); // flawfinder: ignore // flawfinder: ignore
        snprintf(fp, sizeof(fp), "%.*s/%s", (int)(end - p), p, argv[1]);
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
    sh_error("%s: not found", argv[1]);
    return 127;
  }

  envp = var_environ();
  execve(path, argv + 1, envp);
  sh_errorf("%s", argv[1]);
  free(path);
  var_environ_free(envp);
  return errno == ENOENT ? 127 : 126;
}

/* exit [status] */
int builtin_exit(int argc, char **argv) {
  int status = sh.last_status;

  if (argc > 1) {
    char *endp;
    status = (int)sh_strtol(argv[1], &endp, 10);
  }

  exit(status & 0xff);
  return status; /* not reached */
}

/* export [-p] [name[=value]...] */
int builtin_export(int argc, char **argv) {
  int i;

  if (argc == 1 || (argc == 2 && strcmp(argv[1], "-p") == 0)) {
    /* Print all exports */
    int j;
    const struct var_entry *v;
    for (j = 0; j < HASH_SIZE; j++) {
      for (v = sh.vars[j]; v; v = v->next) {
        if (v->flags & VAR_EXPORT) {
          if (v->value)
            printf("export %s=\"%s\"\n", v->name, v->value);
          else
            printf("export %s\n", v->name);
        }
      }
    }
    return 0;
  }

  for (i = 1; i < argc; i++) {
    const char *eq = strchr(argv[i], '=');
    if (eq) {
      char *name = sh_malloc((size_t)(eq - argv[i]) + 1);
      memcpy(name, argv[i], (size_t)(eq - argv[i])); // flawfinder: ignore
      name[eq - argv[i]] = '\0';
      var_set(name, eq + 1, 1);
      free(name);
    } else {
      var_export(argv[i]);
    }
  }
  return 0;
}

/* readonly [-p] [name[=value]...] */
int builtin_readonly(int argc, char **argv) {
  int i;

  if (argc == 1 || (argc == 2 && strcmp(argv[1], "-p") == 0)) {
    int j;
    const struct var_entry *v;
    for (j = 0; j < HASH_SIZE; j++) {
      for (v = sh.vars[j]; v; v = v->next) {
        if (v->flags & VAR_READONLY) {
          if (v->value)
            printf("readonly %s=\"%s\"\n", v->name, v->value);
          else
            printf("readonly %s\n", v->name);
        }
      }
    }
    return 0;
  }

  for (i = 1; i < argc; i++) {
    const char *eq = strchr(argv[i], '=');
    if (eq) {
      char *name = sh_malloc((size_t)(eq - argv[i]) + 1);
      memcpy(name, argv[i], (size_t)(eq - argv[i])); // flawfinder: ignore
      name[eq - argv[i]] = '\0';
      var_readonly(name, eq + 1);
      free(name);
    } else {
      var_readonly(argv[i], NULL);
    }
  }
  return 0;
}

/* return [n] */
int builtin_return(int argc, char **argv) {
  int status = sh.last_status;

  if (sh.func_depth == 0 && sh.dot_depth == 0) {
    sh_error("return: not in a function or sourced file");
    return 1;
  }

  if (argc > 1) {
    char *endp;
    status = (int)sh_strtol(argv[1], &endp, 10);
  }

  sh.last_status = status & 0xff;
  sh.want_return = 1;
  return sh.last_status;
}

/* set [options] [--] [args...] */
int builtin_set(int argc, char **argv) {
  int i;

  if (argc == 1) {
    /* Print all variables */
    int j;
    const struct var_entry *v;
    for (j = 0; j < HASH_SIZE; j++) {
      for (v = sh.vars[j]; v; v = v->next) {
        if (v->value)
          printf("%s='%s'\n", v->name, v->value);
      }
    }
    return 0;
  }

  /* Check for -o / +o */
  if (argc == 2 && strcmp(argv[1], "-o") == 0) {
    options_print(1);
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "+o") == 0) {
    options_print(1);
    return 0;
  }

  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-' || argv[i][0] == '+') {
      if (strcmp(argv[i], "--") == 0) {
        i++;
        break;
      }
      if (argv[i][1] == 'o') {
        if (i + 1 < argc) {
          i++;
          /* Handle -o name / +o name */
          options_set(argv[i - 1][0] == '-' ? "-" : "+");
        }
        continue;
      }
      options_set(argv[i]);
    } else {
      break;
    }
  }

  /* Remaining args become positional parameters */
  if (i < argc || (i > 1 && i <= argc && strcmp(argv[i - 1], "--") == 0)) {
    var_set_posparams(argc - i, argv + i);
  }

  return 0;
}

/* shift [n] */
int builtin_shift(int argc, char **argv) {
  int n = 1;

  if (argc > 1) {
    char *endp;
    n = (int)sh_strtol(argv[1], &endp, 10);
    if (n < 0) {
      sh_error("shift: bad number");
      return 1;
    }
  }

  if (var_shift(n) < 0) {
    sh_error("shift: can't shift that many");
    return 1;
  }
  return 0;
}

/* times */
int builtin_times(int argc, char **argv) {
  struct tms tms_buf;
  long ticks;

  (void)argc;
  (void)argv;

  ticks = sysconf(_SC_CLK_TCK);
  if (ticks <= 0)
    ticks = 100;

  times(&tms_buf);
  printf("%ldm%ld.%03lds %ldm%ld.%03lds\n",
         (long)(tms_buf.tms_utime / ticks / 60),
         (long)(tms_buf.tms_utime / ticks % 60),
         (long)(tms_buf.tms_utime % ticks * 1000 / ticks),
         (long)(tms_buf.tms_stime / ticks / 60),
         (long)(tms_buf.tms_stime / ticks % 60),
         (long)(tms_buf.tms_stime % ticks * 1000 / ticks));
  printf("%ldm%ld.%03lds %ldm%ld.%03lds\n",
         (long)(tms_buf.tms_cutime / ticks / 60),
         (long)(tms_buf.tms_cutime / ticks % 60),
         (long)(tms_buf.tms_cutime % ticks * 1000 / ticks),
         (long)(tms_buf.tms_cstime / ticks / 60),
         (long)(tms_buf.tms_cstime / ticks % 60),
         (long)(tms_buf.tms_cstime % ticks * 1000 / ticks));
  return 0;
}

/* trap [action signal...] */
int builtin_trap(int argc, char **argv) {

  if (argc == 1) {
    trap_print();
    return 0;
  }

  /* trap '' signals — ignore */
  /* trap - signals — reset to default */
  /* trap action signals */
  if (argc == 2 && strcmp(argv[1], "-") == 0) {
    /* Reset all traps? No, trap - alone is not standard */
    return 0;
  }

  {
    const char *action = argv[1];
    for (int i = 2; i < argc; i++) {
      int sig = trap_signum(argv[i]);
      if (sig < 0) {
        sh_error("trap: %s: bad signal", argv[i]);
        return 1;
      }
      if (strcmp(action, "-") == 0)
        trap_set(sig, NULL);
      else
        trap_set(sig, action);
    }
  }
  return 0;
}

/* unset [-f] [-v] name... */
int builtin_unset(int argc, char **argv) {
  int i;
  int func_mode = 0;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-f") == 0) {
      func_mode = 1;
    } else if (strcmp(argv[i], "-v") == 0) {
      func_mode = 0;
    } else if (argv[i][0] == '-') {
      sh_error("unset: bad option: %s", argv[i]);
      return 2;
    } else {
      break;
    }
  }

  for (; i < argc; i++) {
    if (func_mode) {
      /* Unset function */
      unsigned int h = hash_string(argv[i]);
      struct func_entry *fe, **pp;
      for (pp = &sh.functions[h]; (fe = *pp) != NULL; pp = &fe->next) {
        if (strcmp(fe->name, argv[i]) == 0) {
          *pp = fe->next;
          free(fe->name);
          free(fe);
          break;
        }
      }
    } else {
      if (var_unset(argv[i]) < 0)
        return 1;
    }
  }
  return 0;
}
