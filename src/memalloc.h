/*
 * meowsh — POSIX-compliant shell
 * memalloc.h — Arena/stack allocator for parse trees
 */

#ifndef MEOWSH_MEMALLOC_H
#define MEOWSH_MEMALLOC_H

#include <stddef.h>

/* Arena block */
struct arena_block {
	struct arena_block *next;
	size_t size;
	size_t used;
	char data[]; // flawfinder: ignore
};

/* Arena */
struct arena {
	struct arena_block *head;
	struct arena_block *current;
};

/* Arena operations */
void arena_init(struct arena *a);
void *arena_alloc(struct arena *a, size_t size);
char *arena_strdup(struct arena *a, const char *s);
char *arena_strndup(struct arena *a, const char *s, size_t n);
void arena_free(struct arena *a);

/* Global parse arena */
extern struct arena parse_arena;

/* Safe malloc wrappers (die on failure) */
void *sh_malloc(size_t size);
void *sh_realloc(void *ptr, size_t size);
char *sh_strdup(const char *s);

#endif /* MEOWSH_MEMALLOC_H */
