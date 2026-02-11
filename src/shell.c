/*
 * meowsh — POSIX-compliant shell
 * shell.c — Global shell state and common utilities
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "var.h"
#include "mystring.h"

/* Global shell state */
struct shell_state sh;

unsigned int
hash_string(const char *s)
{
	unsigned int h = 5381;

	while (*s)
		h = ((h << 5) + h) + (unsigned char)*s++;
	return h % HASH_SIZE;
}

void
shorten_path(char *dst, const char *src, size_t size)
{
	const char *home = var_get("HOME");
	char buf[PATH_MAX];
	const char *p;
	size_t len = 0;

	if (home && home[0] && prefix(src, home)) {
		buf[0] = '~';
		size_t hlen = strlen(home);
		strncpy(buf + 1, src + hlen, sizeof(buf) - 2);
		buf[sizeof(buf) - 1] = '\0';
		p = buf;
	} else {
		p = src;
	}

	while (*p && len < size - 1) {
		if (*p == '/') {
			dst[len++] = *p++;
			if (len >= size - 1) break;
			
			/* Find next slash to see if this is the last component */
			const char *next_slash = strchr(p, '/');
			if (next_slash && *p) {
				/* Not the last component, shorten to one char */
				dst[len++] = *p;
				p = next_slash;
				continue;
			}
		}
		dst[len++] = *p++;
	}
	dst[len] = '\0';
}
