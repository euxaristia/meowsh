/*
 * meowsh — POSIX-compliant shell
 * mystring.h — String helpers
 */

#ifndef MEOWSH_MYSTRING_H
#define MEOWSH_MYSTRING_H

#include <stddef.h>

/* Growable string buffer */
struct strbuf {
	char *buf;
	size_t len;
	size_t cap;
};

#define STRBUF_INIT { NULL, 0, 0 }

void strbuf_reset(struct strbuf *sb);
void strbuf_free(struct strbuf *sb);
void strbuf_grow(struct strbuf *sb, size_t need);
void strbuf_addch(struct strbuf *sb, char c);
void strbuf_addstr(struct strbuf *sb, const char *s);
void strbuf_addmem(struct strbuf *sb, const char *s, size_t n);
char *strbuf_detach(struct strbuf *sb);

/* Utility functions */
int is_name(const char *s);          /* valid POSIX name */
int is_name_char(char c);
int is_assignment(const char *s);    /* name=... */
int prefix(const char *s, const char *pfx);
long sh_strtol(const char *s, char **endp, int base);

#endif /* MEOWSH_MYSTRING_H */
