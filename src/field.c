/*
 * meowsh — POSIX-compliant shell
 * field.c — IFS-based field splitting
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "field.h"
#include "var.h"
#include "memalloc.h"

#include <string.h>
#include <ctype.h>

static int
is_ifs_white(char c, const char *ifs)
{
	return (c == ' ' || c == '\t' || c == '\n') && strchr(ifs, c);
}

static int
is_ifs_char(char c, const char *ifs)
{
	return strchr(ifs, c) != NULL;
}

char **
field_split(const char *s, int *countp)
{
	const char *ifs;
	char **fields = NULL;
	int count = 0;
	int cap = 0;
	const char *p;

	if (!s || !*s) {
		fields = sh_malloc(sizeof(char *));
		fields[0] = NULL;
		if (countp) *countp = 0;
		return fields;
	}

	ifs = var_get("IFS");
	if (!ifs)
		ifs = " \t\n";

	/* If IFS is empty, no field splitting */
	if (!*ifs) {
		fields = sh_malloc(2 * sizeof(char *));
		fields[0] = sh_strdup(s);
		fields[1] = NULL;
		if (countp) *countp = 1;
		return fields;
	}

	p = s;

	/* Skip leading IFS whitespace */
	while (*p && is_ifs_white(*p, ifs))
		p++;

	while (*p) {
		const char *start = p;

		/* Scan to next IFS character */
		while (*p && !is_ifs_char(*p, ifs))
			p++;

		/* Add field */
		if (count >= cap) {
			cap = cap ? cap * 2 : 16;
			fields = sh_realloc(fields, (cap + 1) * sizeof(char *));
		}
		{
			size_t len = (size_t)(p - start);
			fields[count] = sh_malloc(len + 1);
			memcpy(fields[count], start, len);
			fields[count][len] = '\0';
			count++;
		}

		if (!*p)
			break;

		/* Skip IFS delimiters:
		 * - IFS whitespace is trimmed
		 * - Non-whitespace IFS chars delimit fields
		 * - Adjacent non-ws IFS chars produce empty fields */
		{
			int saw_nonws = 0;
			while (*p && is_ifs_char(*p, ifs)) {
				if (!is_ifs_white(*p, ifs)) {
					if (saw_nonws) {
						/* Two non-ws delimiters: empty field */
						if (count >= cap) {
							cap = cap ? cap * 2 : 16;
							fields = sh_realloc(fields,
							    (cap + 1) * sizeof(char *));
						}
						fields[count++] = sh_strdup("");
					}
					saw_nonws = 1;
				}
				p++;
			}
		}
	}

	if (!fields)
		fields = sh_malloc(sizeof(char *));
	fields[count] = NULL;

	if (countp) *countp = count;
	return fields;
}

void
field_free(char **fields)
{
	int i;

	if (!fields)
		return;
	for (i = 0; fields[i]; i++)
		free(fields[i]);
	free(fields);
}
