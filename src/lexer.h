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
void lexer_consume(void);

/* Check if a word is a reserved word and return its token type */
token_type_t reserved_word(const char *s);

/* Read a here-document body */
char *lexer_read_heredoc(const char *delim, int strip_tabs, int quoted);

/* Enable/disable alias expansion in the lexer */
void lexer_set_alias(int enable);

/* Token type name for debug */
const char *token_name(token_type_t t);

#endif /* MEOWSH_LEXER_H */
