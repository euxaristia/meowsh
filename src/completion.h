/*
 * meowsh — POSIX-compliant shell
 * completion.h — Tab completion
 */

#ifndef MEOWSH_COMPLETION_H
#define MEOWSH_COMPLETION_H

#include <stddef.h>

enum completion_type {
	COMP_TYPE_DEFAULT = 0,
	COMP_TYPE_DIR,
	COMP_TYPE_EXE,
	COMP_TYPE_CMD, /* Builtin, Function, Alias */
};

struct completion_result {
	char **matches;
	enum completion_type *types;
	size_t count;
	size_t common_len;
};

void completion_free(struct completion_result *cr);

/*
 * Get completions for the line at the given position.
 * Returns a result with matches.
 */
struct completion_result *completion_get(const char *line, int pos);

#endif /* MEOWSH_COMPLETION_H */
