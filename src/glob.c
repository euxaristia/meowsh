/*
 * meowsh — POSIX-compliant shell
 * glob.c — Pathname expansion (wraps POSIX glob(3))
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "sh_glob.h"
#include "memalloc.h"

#include <glob.h>
#include <string.h>

int
has_glob_chars(const char *s)
{
	int in_bracket = 0;

	if (!s)
		return 0;

	for (; *s; s++) {
		switch (*s) {
		case '\\':
			if (s[1]) s++;
			break;
		case '*':
		case '?':
			return 1;
		case '[':
			in_bracket = 1;
			break;
		case ']':
			if (in_bracket)
				return 1;
			break;
		}
	}
	return 0;
}

char **
glob_expand(const char *pattern, int *countp)
{
	glob_t gl;
	char **result;
	int ret;
	size_t i;

	memset(&gl, 0, sizeof(gl));
	ret = glob(pattern, GLOB_NOSORT | GLOB_NOCHECK, NULL, &gl);

	if (ret != 0 || gl.gl_pathc == 0) {
		/* No matches: return the pattern itself */
		if (ret == 0)
			globfree(&gl);
		result = sh_malloc(2 * sizeof(char *));
		result[0] = sh_strdup(pattern);
		result[1] = NULL;
		if (countp) *countp = 1;
		return result;
	}

	result = sh_malloc((gl.gl_pathc + 1) * sizeof(char *));
	for (i = 0; i < gl.gl_pathc; i++)
		result[i] = sh_strdup(gl.gl_pathv[i]);
	result[gl.gl_pathc] = NULL;

	if (countp)
		*countp = (int)gl.gl_pathc;

	globfree(&gl);
	return result;
}

void
glob_free(char **results)
{
	int i;

	if (!results)
		return;
	for (i = 0; results[i]; i++)
		free(results[i]);
	free(results);
}
