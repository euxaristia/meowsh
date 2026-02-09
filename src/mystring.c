/*
 * meowsh — POSIX-compliant shell
 * mystring.c — String helpers
 */

#define _POSIX_C_SOURCE 200809L

#include "mystring.h"
#include "memalloc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void
strbuf_init(struct strbuf *sb)
{
	sb->buf = NULL;
	sb->len = 0;
	sb->cap = 0;
}

void
strbuf_reset(struct strbuf *sb)
{
	sb->len = 0;
	if (sb->buf)
		sb->buf[0] = '\0';
}

void
strbuf_free(struct strbuf *sb)
{
	free(sb->buf);
	sb->buf = NULL;
	sb->len = 0;
	sb->cap = 0;
}

void
strbuf_grow(struct strbuf *sb, size_t need)
{
	size_t newcap;

	if (sb->len + need + 1 <= sb->cap)
		return;
	newcap = sb->cap ? sb->cap * 2 : 64;
	while (newcap < sb->len + need + 1)
		newcap *= 2;
	sb->buf = sh_realloc(sb->buf, newcap);
	sb->cap = newcap;
}

void
strbuf_addch(struct strbuf *sb, char c)
{
	strbuf_grow(sb, 1);
	sb->buf[sb->len++] = c;
	sb->buf[sb->len] = '\0';
}

void
strbuf_addstr(struct strbuf *sb, const char *s)
{
	size_t len;

	if (!s)
		return;
	len = strlen(s);
	strbuf_grow(sb, len);
	memcpy(sb->buf + sb->len, s, len);
	sb->len += len;
	sb->buf[sb->len] = '\0';
}

void
strbuf_addmem(struct strbuf *sb, const char *s, size_t n)
{
	strbuf_grow(sb, n);
	memcpy(sb->buf + sb->len, s, n);
	sb->len += n;
	sb->buf[sb->len] = '\0';
}

char *
strbuf_detach(struct strbuf *sb)
{
	char *s;

	if (!sb->buf) {
		s = sh_malloc(1);
		s[0] = '\0';
	} else {
		s = sb->buf;
	}
	sb->buf = NULL;
	sb->len = 0;
	sb->cap = 0;
	return s;
}

char *
strbuf_to_string(struct strbuf *sb)
{
	if (!sb->buf)
		return sh_strdup("");
	return sh_strdup(sb->buf);
}

int
is_name_char(char c)
{
	return isalpha((unsigned char)c) || c == '_' || isdigit((unsigned char)c);
}

int
is_name(const char *s)
{
	if (!s || !*s)
		return 0;
	if (!isalpha((unsigned char)*s) && *s != '_')
		return 0;
	for (s++; *s; s++) {
		if (!is_name_char(*s))
			return 0;
	}
	return 1;
}

int
is_number(const char *s)
{
	if (!s || !*s)
		return 0;
	for (; *s; s++) {
		if (!isdigit((unsigned char)*s))
			return 0;
	}
	return 1;
}

int
is_assignment(const char *s)
{
	const char *eq;

	if (!s || !*s)
		return 0;
	if (!isalpha((unsigned char)*s) && *s != '_')
		return 0;
	for (eq = s + 1; *eq && *eq != '='; eq++) {
		if (!is_name_char(*eq))
			return 0;
	}
	return *eq == '=';
}

int
prefix(const char *s, const char *pfx)
{
	while (*pfx) {
		if (*s != *pfx)
			return 0;
		s++;
		pfx++;
	}
	return 1;
}

char *
sh_stpcpy(char *dst, const char *src)
{
	while ((*dst = *src) != '\0') {
		dst++;
		src++;
	}
	return dst;
}
