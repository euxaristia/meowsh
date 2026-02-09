/*
 * meowsh — POSIX-compliant shell
 * parser.h — Recursive-descent parser
 */

#ifndef MEOWSH_PARSER_H
#define MEOWSH_PARSER_H

#include "types.h"

/* Parse one complete command from current input.
 * ps1/ps2 are prompt strings (NULL for non-interactive).
 * Returns NULL on EOF. */
struct node *parse_command(const char *ps1, const char *ps2);

#endif /* MEOWSH_PARSER_H */
