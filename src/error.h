/*
 * meowsh — POSIX-compliant shell
 * error.h — Diagnostics
 */

#ifndef MEOWSH_ERROR_H
#define MEOWSH_ERROR_H

/* Print error and return */
void sh_error(const char *fmt, ...);

/* Print warning */
void sh_warn(const char *fmt, ...);

/* Print error and exit with status 2 */
void sh_fatal(const char *fmt, ...) __attribute__((noreturn));

/* Print syntax error and exit with status 2 */
void sh_syntax(const char *fmt, ...);

/* perror-style error */
void sh_errorf(const char *fmt, ...);

#endif /* MEOWSH_ERROR_H */
