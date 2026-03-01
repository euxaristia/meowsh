/*
 * meowsh — POSIX-compliant shell
 * builtin_test.c — test and [ builtins
 */

#define _POSIX_C_SOURCE 200809L

#include "builtin.h"
#include "sh_error.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int test_unary(const char *op, const char *arg) {
  struct stat st;

  if (strcmp(op, "-z") == 0)
    return strlen(arg) == 0;
  if (strcmp(op, "-n") == 0)
    return strlen(arg) > 0;
  if (strcmp(op, "-d") == 0)
    return stat(arg, &st) == 0 && S_ISDIR(st.st_mode);
  if (strcmp(op, "-f") == 0)
    return stat(arg, &st) == 0 && S_ISREG(st.st_mode);
  if (strcmp(op, "-r") == 0)
    return access(arg, R_OK) == 0;
  if (strcmp(op, "-w") == 0)
    return access(arg, W_OK) == 0;
  if (strcmp(op, "-x") == 0)
    return access(arg, X_OK) == 0;
  if (strcmp(op, "-e") == 0)
    return stat(arg, &st) == 0;
  if (strcmp(op, "-s") == 0)
    return stat(arg, &st) == 0 && st.st_size > 0;
  if (strcmp(op, "-L") == 0)
    return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode);

  return 0; // Unknown or false
}

static int test_binary(const char *arg1, const char *op, const char *arg2) {
  if (strcmp(op, "=") == 0)
    return strcmp(arg1, arg2) == 0;
  if (strcmp(op, "!=") == 0)
    return strcmp(arg1, arg2) != 0;
  // TODO: numeric comparisons if needed
  return 0;
}

int builtin_test(int argc, char **argv) {
  int is_bracket = (strcmp(argv[0], "[") == 0);

  if (is_bracket) {
    if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
      sh_error("[: missing ']'");
      return 2;
    }
    argc--; // ignore ]
  }

  if (argc == 1)
    return 1; // test (empty) -> false

  if (argc == 2) {
    // [ arg ] -> true if arg non-empty
    return argv[1][0] == '\0';
  }

  if (argc == 3) {
    // [ op arg ] or [ arg ] (if op is not unary?)
    if (argv[1][0] == '-') {
      return !test_unary(argv[1], argv[2]);
    }
    return argv[1][0] == '\0';
  }

  if (argc == 4) {
    // [ arg1 op arg2 ]
    return !test_binary(argv[1], argv[2], argv[3]);
  }

  // Fallback for more complex cases (not fully POSIX but enough for basic
  // scripts)
  return 1;
}
