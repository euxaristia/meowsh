/*
 * meowsh — POSIX-compliant shell
 * completion.c — Tab completion logic
 */

#define _POSIX_C_SOURCE 200809L

#include "completion.h"
#include "alias.h"
#include "builtin.h"
#include "memalloc.h"
#include "mystring.h"
#include "shell.h"
#include "var.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void add_match(struct completion_result *cr, const char *match,
                      enum completion_type type) {
  size_t i;

  for (i = 0; i < cr->count; i++) {
    if (strcmp(cr->matches[i], match) == 0)
      return;
  }

  cr->matches = sh_realloc(cr->matches, (cr->count + 1) * sizeof(char *));
  cr->types = sh_realloc(cr->types, (cr->count + 1) * sizeof(enum completion_type));
  cr->matches[cr->count] = strdup(match);
  cr->types[cr->count] = type;
  cr->count++;
}

void completion_free(struct completion_result *cr) {
  size_t i;
  if (!cr)
    return;
  for (i = 0; i < cr->count; i++)
    free(cr->matches[i]);
  free(cr->matches);
  free(cr->types);
  free(cr);
}

static size_t get_common_prefix(char **matches, size_t count) {
  size_t i, j;
  if (count == 0)
    return 0;
  if (count == 1)
    return strlen(matches[0]); // flawfinder: ignore // flawfinder: ignore

  for (j = 0; matches[0][j]; j++) {
    for (i = 1; i < count; i++) {
      if (matches[i][j] != matches[0][j])
        return j;
    }
  }
  return j;
}

static void sort_matches(struct completion_result *cr) {
  size_t i, j;
  for (i = 0; i < cr->count; i++) {
    for (j = i + 1; j < cr->count; j++) {
      if (strcmp(cr->matches[i], cr->matches[j]) > 0) {
        char *tmp_m = cr->matches[i];
        enum completion_type tmp_t = cr->types[i];
        cr->matches[i] = cr->matches[j];
        cr->types[i] = cr->types[j];
        cr->matches[j] = tmp_m;
        cr->types[j] = tmp_t;
      }
    }
  }
}

static int is_command_pos(const char *line, int pos) {
  int i = pos;

  /* Back up to start of current word */
  while (i > 0 && line[i - 1] != ' ' && line[i - 1] != '\t' &&
         line[i - 1] != '|' && line[i - 1] != ';' && line[i - 1] != '&' &&
         line[i - 1] != '<' && line[i - 1] != '>') {
    i--;
  }

  /* Back up past whitespace */
  while (i > 0 && (line[i - 1] == ' ' || line[i - 1] == '\t'))
    i--;

  if (i == 0)
    return 1;
  if (line[i - 1] == '|' || line[i - 1] == ';' || line[i - 1] == '&')
    return 1;
  return 0;
}

static int is_token_delim(char c) {
  return c == ' ' || c == '\t' || c == '|' || c == ';' || c == '&' ||
         c == '<' || c == '>';
}

static int is_cd_argument_pos(const char *line, int start) {
  int seg_start = start;
  int i;
  int token_index = 0;
  char cmd[64]; // flawfinder: ignore

  while (seg_start > 0) {
    char c = line[seg_start - 1];
    if (c == '|' || c == ';' || c == '&')
      break;
    seg_start--;
  }

  cmd[0] = '\0';
  i = seg_start;
  while (i < start) {
    int ts;
    int tl;

    while (i < start && (line[i] == ' ' || line[i] == '\t'))
      i++;
    if (i >= start)
      break;
    if (line[i] == '|' || line[i] == ';' || line[i] == '&')
      break;
    if (line[i] == '<' || line[i] == '>') {
      i++;
      continue;
    }

    ts = i;
    while (i < start && !is_token_delim(line[i]))
      i++;
    tl = i - ts;
    if (tl <= 0)
      continue;

    if (token_index == 0) {
      size_t n = (size_t)tl;
      if (n >= sizeof(cmd))
        n = sizeof(cmd) - 1;
      memcpy(cmd, line + ts, n); // flawfinder: ignore
      cmd[n] = '\0';
    }
    token_index++;
  }

  return token_index == 1 && strcmp(cmd, "cd") == 0;
}

static void complete_path(struct completion_result *cr, const char *pfx,
                          int dirs_only) {
  char *dir_path;
  const char *base_pfx;
  char *word_prefix;
  DIR *dir;
  const struct dirent *de;
  const char *slash = strrchr(pfx, '/');
  int show_dotfiles;

  if (slash) {
    dir_path = strndup(pfx, (size_t)(slash - pfx + 1));
    if (dir_path[0] == '\0') {
      free(dir_path);
      dir_path = strdup("./");
    }
    base_pfx = slash + 1;
    word_prefix = strndup(pfx, (size_t)(slash - pfx + 1));
  } else {
    dir_path = strdup("./");
    base_pfx = pfx;
    word_prefix = strdup("");
  }

  dir = opendir(dir_path);
  if (!dir) {
    free(dir_path);
    free(word_prefix);
    return;
  }

  show_dotfiles = (base_pfx[0] == '.');

  while ((de = readdir(dir)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    if (!show_dotfiles && de->d_name[0] == '.')
      continue;
    if (prefix(de->d_name, base_pfx)) {
      char full[PATH_MAX]; // flawfinder: ignore
      struct stat st;
      size_t len =
          strlen(word_prefix) + strlen(de->d_name) + 2; // flawfinder: ignore
      char *m;

      snprintf(full, sizeof(full), "%s%s",
               strcmp(dir_path, "./") == 0 ? "" : dir_path, de->d_name);

      m = malloc(len);
      if (!m)
        continue;
      snprintf(m, len, "%s%s", word_prefix, de->d_name);

      if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t mlen = strlen(m); // flawfinder: ignore
        m[mlen] = '/';
        m[mlen + 1] = '\0';
        add_match(cr, m, COMP_TYPE_DIR);
      } else if (!dirs_only) {
        enum completion_type type = COMP_TYPE_DEFAULT;
        if (stat(full, &st) == 0 && (st.st_mode & S_IXUSR))
          type = COMP_TYPE_EXE;
        add_match(cr, m, type);
      }
      free(m);
    }
  }

  closedir(dir);
  free(dir_path);
  free(word_prefix);
}

static void complete_exe_in_dir(struct completion_result *cr,
                                const char *dir_path, const char *pfx) {
  DIR *dir = opendir(dir_path);
  const struct dirent *de;
  if (!dir)
    return;

  while ((de = readdir(dir)) != NULL) {
    if (prefix(de->d_name, pfx)) {
      char full[PATH_MAX]; // flawfinder: ignore
      struct stat st;
      snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
      if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR)) {
        add_match(cr, de->d_name, COMP_TYPE_CMD);
      }
    }
  }
  closedir(dir);
}

static void complete_command(struct completion_result *cr, const char *pfx) {
  /* Builtins */
  const struct builtin_entry *b;
  for (b = builtin_get_all(); b->name; b++) {
    if (prefix(b->name, pfx))
      add_match(cr, b->name, COMP_TYPE_CMD);
  }

  /* Aliases */
  int i;
  for (i = 0; i < HASH_SIZE; i++) {
    const struct alias_entry *ae = sh.aliases[i];
    while (ae) {
      if (prefix(ae->name, pfx))
        add_match(cr, ae->name, COMP_TYPE_CMD);
      ae = ae->next;
    }
  }

  /* Functions */
  for (i = 0; i < HASH_SIZE; i++) {
    const struct func_entry *fe = sh.functions[i];
    while (fe) {
      if (prefix(fe->name, pfx))
        add_match(cr, fe->name, COMP_TYPE_CMD);
      fe = fe->next;
    }
  }

  /* PATH */
  const char *path = var_get("PATH");
  if (path) {
    char *p = strdup(path);
    char *saveptr;
    const char *dir = strtok_r(p, ":", &saveptr);
    while (dir) {
      complete_exe_in_dir(cr, dir, pfx);
      dir = strtok_r(NULL, ":", &saveptr);
    }
    free(p);
  }
}

struct completion_result *completion_get(const char *line, int pos) {
  struct completion_result *cr = calloc(1, sizeof(*cr));
  int start = pos;
  while (start > 0 && line[start - 1] != ' ' && line[start - 1] != '	' &&
         line[start - 1] != '|' && line[start - 1] != ';' &&
         line[start - 1] != '&' && line[start - 1] != '<' &&
         line[start - 1] != '>') {
    start--;
  }

  char *pfx = strndup(line + start, (size_t)(pos - start));

  if (strchr(pfx, '/') || !is_command_pos(line, start)) {
    complete_path(cr, pfx, is_cd_argument_pos(line, start));
  } else {
    complete_command(cr, pfx);
    /* Also complete builtins - I'll need to expose them */
  }

  free(pfx);

  if (cr->count > 0) {
    sort_matches(cr);
    cr->common_len = get_common_prefix(cr->matches, cr->count);
  }

  return cr;
}
