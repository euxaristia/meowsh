/*
 * meowsh — POSIX-compliant shell
 * lexer.h — Hand-written POSIX tokenizer
 */

#ifndef MEOWSH_LEXER_H
#define MEOWSH_LEXER_H

#include "types.h"

/* Initialize the lexer */
void lexer_init(void);

/* Set prompt strings for interactive continuation */
void lexer_set_prompts(const char *ps1, const char *ps2);

/* Get next token (allocates in parse arena) */
struct token *lexer_next(void);

/* Peek at next token without consuming it */
struct token *lexer_peek(void);

/* Consume the peeked token */

/* Check if a word is a reserved word and return its token type */

/* Read a here-document body */

/* Queue a heredoc to be read after newline */
void queue_heredoc(struct redirect *redir, const char *delim, int strip_tabs,
    int quoted);

/* Clear pending heredocs (on syntax error or reset) */
void lexer_clear_heredocs(void);

/* Enable/disable alias expansion in the lexer */

/* Token type name for debug */
const char *token_name(token_type_t t);

#endif /* MEOWSH_LEXER_H */
