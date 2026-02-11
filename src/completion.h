/*
 * meowsh — POSIX-compliant shell
 * completion.h — Tab completion
 */

#ifndef MEOWSH_COMPLETION_H
#define MEOWSH_COMPLETION_H

#include <stddef.h>

struct completion_result {
	char **matches;
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
