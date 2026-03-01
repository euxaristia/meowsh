/*
 * meowsh — POSIX-compliant shell
 * var.c — Variable table, environment, special params
 */

#define _POSIX_C_SOURCE 200809L

#include "var.h"
#include "memalloc.h"
#include "mystring.h"
#include "options.h"
#include "sh_error.h"
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void var_init(void) {
  char buf[32]; // flawfinder: ignore

  memset(sh.vars, 0, sizeof(sh.vars));
  sh.posparams = NULL;

  /* Set default IFS */
  var_set("IFS", " \t\n", 0);

  /*
   * Keep a usable command search path even if the parent environment is
   * sparse (common for login managers/system shells).
   */
  var_set("PATH",
          "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 0);

  /* Set default PS1 */
  var_set("PS1", "meowsh % ", 0);

  /* Set PPID */
  snprintf(buf, sizeof(buf), "%ld", (long)getppid());
  var_set("PPID", buf, 0);

  /* Set PWD */
  {
    char cwd[PATH_MAX]; // flawfinder: ignore
    if (getcwd(cwd, sizeof(cwd)))
      var_set("PWD", cwd, 1);
  }
}

static struct var_entry *var_lookup(const char *name) {
  unsigned int h = hash_string(name);
  struct var_entry *v;

  for (v = sh.vars[h]; v; v = v->next) {
    if (strcmp(v->name, name) == 0)
      return v;
  }
  return NULL;
}

const char *var_get(const char *name) {
  struct var_entry *v;

  /* Check for special single-char parameters */
  if (name[0] && !name[1]) {
    const char *s = var_special(name[0]);
    if (s)
      return s;
  }

  v = var_lookup(name);
  return v ? v->value : NULL;
}

int var_set(const char *name, const char *value, int export) {
  unsigned int h;
  struct var_entry *v;

  v = var_lookup(name);
  if (v) {
    if (v->flags & VAR_READONLY) {
      sh_error("%s: readonly variable", name);
      return -1;
    }
    free(v->value);
    v->value = value ? sh_strdup(value) : NULL;
    if (export)
      v->flags |= VAR_EXPORT;
    return 0;
  }

  v = sh_malloc(sizeof(*v));
  v->name = sh_strdup(name);
  v->value = value ? sh_strdup(value) : NULL;
  v->flags = export ? VAR_EXPORT : 0;

  h = hash_string(name);
  v->next = sh.vars[h];
  sh.vars[h] = v;

  if (option_is_set(OPT_ALLEXPORT))
    v->flags |= VAR_EXPORT;

  return 0;
}

int var_unset(const char *name) {
  unsigned int h = hash_string(name);
  struct var_entry *v, **pp;

  for (pp = &sh.vars[h]; (v = *pp) != NULL; pp = &v->next) {
    if (strcmp(v->name, name) == 0) {
      if (v->flags & VAR_READONLY) {
        sh_error("%s: readonly variable", name);
        return -1;
      }
      *pp = v->next;
      free(v->name);
      free(v->value);
      free(v);
      return 0;
    }
  }
  return 0;
}

int var_export(const char *name) {
  struct var_entry *v = var_lookup(name);

  if (v) {
    v->flags |= VAR_EXPORT;
    return 0;
  }

  /* Create variable with null value, marked for export */
  return var_set(name, "", 1);
}

int var_readonly(const char *name, const char *value) {
  struct var_entry *v;

  if (value)
    var_set(name, value, 0);

  v = var_lookup(name);
  if (v)
    v->flags |= VAR_READONLY;
  return 0;
}

void var_import(const char *envstr) {
  const char *eq = strchr(envstr, '=');
  char *name;

  if (!eq)
    return;

  name = sh_malloc((size_t)(eq - envstr) + 1);
  memcpy(name, envstr, (size_t)(eq - envstr)); // flawfinder: ignore
  name[eq - envstr] = '\0';

  var_set(name, eq + 1, 1);
  free(name);
}

char **var_environ(void) {
  int count = 0;
  int i;
  const struct var_entry *v;
  char **env;

  /* Count exported variables */
  for (i = 0; i < HASH_SIZE; i++) {
    for (v = sh.vars[i]; v; v = v->next) {
      if ((v->flags & VAR_EXPORT) && v->value)
        count++;
    }
  }

  env = sh_malloc((count + 1) * sizeof(char *));
  count = 0;

  for (i = 0; i < HASH_SIZE; i++) {
    for (v = sh.vars[i]; v; v = v->next) {
      if ((v->flags & VAR_EXPORT) && v->value) {
        size_t nlen = strlen(v->name); // flawfinder: ignore  // flawfinder: ignore
        size_t vlen = strlen(v->value); // flawfinder: ignore // flawfinder: ignore
        char *s = sh_malloc(nlen + 1 + vlen + 1);

        memcpy(s, v->name, nlen); // flawfinder: ignore
        s[nlen] = '=';
        memcpy(s + nlen + 1, v->value, vlen + 1); // flawfinder: ignore
        env[count++] = s;
      }
    }
  }

  env[count] = NULL;
  return env;
}

void var_environ_free(char **env) {
  int i;

  if (!env)
    return;
  for (i = 0; env[i]; i++)
    free(env[i]);
  free(env);
}

void var_set_posparams(int argc, char **argv) {
  struct posparams *pp;
  int i;

  if (sh.posparams) {
    for (i = 0; i < sh.posparams->argc; i++)
      free(sh.posparams->argv[i]);
    free(sh.posparams->argv);
    pp = sh.posparams;
  } else {
    pp = sh_malloc(sizeof(*pp));
    pp->prev = NULL;
    sh.posparams = pp;
  }

  pp->argc = argc;
  pp->argv = sh_malloc((argc + 1) * sizeof(char *));
  for (i = 0; i < argc; i++)
    pp->argv[i] = sh_strdup(argv[i]);
  pp->argv[argc] = NULL;
}

void var_push_posparams(int argc, char **argv) {
  struct posparams *pp;
  int i;

  pp = sh_malloc(sizeof(*pp));
  pp->argc = argc;
  pp->argv = sh_malloc((argc + 1) * sizeof(char *));
  for (i = 0; i < argc; i++)
    pp->argv[i] = sh_strdup(argv[i]);
  pp->argv[argc] = NULL;
  pp->prev = sh.posparams;
  sh.posparams = pp;
}

void var_pop_posparams(void) {
  struct posparams *pp = sh.posparams;
  int i;

  if (!pp)
    return;
  sh.posparams = pp->prev;
  for (i = 0; i < pp->argc; i++)
    free(pp->argv[i]);
  free(pp->argv);
  free(pp);
}

int var_shift(int n) {
  struct posparams *pp = sh.posparams;
  int i;

  if (!pp || n > pp->argc)
    return -1;

  for (i = 0; i < n; i++)
    free(pp->argv[i]);

  memmove(pp->argv, pp->argv + n, (size_t)(pp->argc - n + 1) * sizeof(char *));
  pp->argc -= n;
  return 0;
}

const char *var_special(int c) {
  static char buf[32]; // flawfinder: ignore

  switch (c) {
  case '?':
    snprintf(buf, sizeof(buf), "%d", sh.last_status);
    return buf;
  case '$':
    snprintf(buf, sizeof(buf), "%ld", (long)sh.shell_pid);
    return buf;
  case '!':
    if (sh.last_bg_pid == 0)
      return "";
    snprintf(buf, sizeof(buf), "%ld", (long)sh.last_bg_pid);
    return buf;
  case '#':
    snprintf(buf, sizeof(buf), "%d", sh.posparams ? sh.posparams->argc : 0);
    return buf;
  case '0':
    return sh.argv0 ? sh.argv0 : "meowsh";
  case '-':
    return options_to_string();
  case '@':
  case '*':
    /* These need special handling in expand.c */
    return NULL;
  default:
    /* Positional parameters $1..$9 */
    if (c >= '1' && c <= '9') {
      int idx = c - '1';
      if (sh.posparams && idx < sh.posparams->argc)
        return sh.posparams->argv[idx];
      return NULL;
    }
    return NULL;
  }
}

