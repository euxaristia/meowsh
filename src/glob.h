/*
 * meowsh — POSIX-compliant shell
 * glob.h — Pathname expansion
 */

#ifndef MEOWSH_GLOB_H
#define MEOWSH_GLOB_H

/* Check if a string contains unquoted glob characters */
int has_glob_chars(const char *s);

/* Perform pathname expansion on a word.
 * Returns NULL-terminated array, sets *countp.
 * If no match, returns array with original word. */
char **glob_expand(const char *pattern, int *countp);

/* Free glob result */
void glob_free(char **results);

#endif /* MEOWSH_GLOB_H */
