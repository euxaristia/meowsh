/*
 * meowsh — POSIX-compliant shell
 * options.h — Shell option flags
 */

#ifndef MEOWSH_OPTIONS_H
#define MEOWSH_OPTIONS_H

/* Parse command-line arguments, returns index of first non-option arg */
int options_parse(int argc, char **argv);

/* Apply a set/unset option string (e.g., "-ex" or "+x") */
int options_set(const char *s);

/* Set a single option by character, returns 0 on success */

/* Get current option string for "set +o" output */
char *options_to_string(void);

/* Print options in re-enterable format (set -o/+o) */
void options_print(int verbose);

/* Check if an option is set */
int option_is_set(unsigned int flag);

#endif /* MEOWSH_OPTIONS_H */
