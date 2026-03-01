/*
 * meowsh — POSIX-compliant shell
 * error.c — Diagnostics
 */

#define _POSIX_C_SOURCE 200809L

#include "sh_error.h"
#include "shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void __attribute__((format(printf, 2, 0))) // flawfinder: ignore
sh_vmsg(const char *prefix, const char *fmt, va_list ap) {
  const char *name = sh.argv0 ? sh.argv0 : "meowsh";

  fprintf(stderr, "%s: ", name);
  if (sh.lineno > 0) {
    if (sh.colno > 0)
      fprintf(stderr, "line %d, col %d: ", sh.lineno, sh.colno);
    else
      fprintf(stderr, "line %d: ", sh.lineno);
  }
  if (prefix)
    fprintf(stderr, "%s: ", prefix);
  vfprintf(stderr, fmt, ap);                  // flawfinder: ignore
  if (fmt[0] && fmt[strlen(fmt) - 1] != '\n') // flawfinder: ignore
    fputc('\n', stderr);
}

void __attribute__((format(printf, 1, 2))) // flawfinder: ignore
sh_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  sh_vmsg(NULL, fmt, ap);
  va_end(ap);
}

void __attribute__((format(printf, 1, 2))) // flawfinder: ignore
sh_warn(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  sh_vmsg("warning", fmt, ap);
  va_end(ap);
}

void __attribute__((format(printf, 1, 2))) // flawfinder: ignore
sh_fatal(const char *fmt, ...)

{
  va_list ap;
  va_start(ap, fmt);
  sh_vmsg(NULL, fmt, ap);
  va_end(ap);
  exit(2);
}

void __attribute__((format(printf, 1, 2))) // flawfinder: ignore
sh_syntax(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  sh_vmsg("syntax error", fmt, ap);
  va_end(ap);
  sh.parse_error = 1;
  if (!sh.interactive)
    exit(2);
  sh.last_status = 2;
}

void __attribute__((format(printf, 1, 2))) // flawfinder: ignore
sh_errorf(const char *fmt, ...) {
  va_list ap;
  const char *name = sh.argv0 ? sh.argv0 : "meowsh";
  int saved = errno;

  fprintf(stderr, "%s: ", name);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap); // flawfinder: ignore
  va_end(ap);
  fprintf(stderr, ": %s\n", strerror(saved));
}
