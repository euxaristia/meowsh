/*
 * meowsh — POSIX-compliant shell
 * expand.h — All 7 POSIX expansion steps
 */

#ifndef MEOWSH_EXPAND_H
#define MEOWSH_EXPAND_H

#include "types.h"

/* Expand a word list into a string list (full expansion pipeline).
 * Returns NULL-terminated argv-style array. */
char **expand_words(struct word *words, int *countp);

/* Expand a single word to a single string (no field splitting/glob).
 * Used for assignments, here-doc bodies, etc. */
char *expand_word(struct word *w, int quoted);

/* Expand assignment value */

/* Free an expanded argv array */
void expand_free(char **argv);

/* Expand a here-document body string */
char *expand_heredoc(const char *body);

#endif /* MEOWSH_EXPAND_H */
