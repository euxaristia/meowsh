/*
 * meowsh — POSIX-compliant shell
 * memalloc.c — Arena/stack allocator for parse trees
 */

#define _POSIX_C_SOURCE 200809L

#include "memalloc.h"
#include "sh_error.h"

#include <stdlib.h>
#include <string.h>

#define ARENA_BLOCK_SIZE 4096

struct arena parse_arena;

void arena_init(struct arena *a) {
  a->head = NULL;
  a->current = NULL;
}

static struct arena_block *arena_new_block(size_t min_size) {
  size_t size = ARENA_BLOCK_SIZE;
  struct arena_block *b;

  if (min_size > size)
    size = min_size;
  b = malloc(sizeof(*b) + size);
  if (!b)
    sh_fatal("out of memory");
  b->next = NULL;
  b->size = size;
  b->used = 0;
  return b;
}

void *arena_alloc(struct arena *a, size_t size) {
  struct arena_block *b;
  void *ptr;

  /* Align to pointer size */
  size = (size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);

  b = a->current;
  if (!b || b->used + size > b->size) {
    b = arena_new_block(size);
    b->next = a->head;
    a->head = b;
    a->current = b;
  }
  ptr = b->data + b->used;
  b->used += size;
  return ptr;
}

char *arena_strdup(struct arena *a, const char *s) {
  size_t len;
  char *p;

  if (!s)
    return NULL;
  len = strlen(s) + 1; // flawfinder: ignore
  p = arena_alloc(a, len);
  memcpy(p, s, len);
  return p;
}

char *arena_strndup(struct arena *a, const char *s, size_t n) {
  char *p;

  if (!s)
    return NULL;
  p = arena_alloc(a, n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

void arena_free(struct arena *a) {
  struct arena_block *b, *next;

  for (b = a->head; b; b = next) {
    next = b->next;
    free(b);
  }
  a->head = NULL;
  a->current = NULL;
}

void *sh_malloc(size_t size) {
  void *p = malloc(size);
  if (!p && size)
    sh_fatal("out of memory");
  return p;
}

void *sh_realloc(void *ptr, size_t size) {
  void *p = realloc(ptr, size);
  if (!p && size)
    sh_fatal("out of memory");
  return p;
}

char *sh_strdup(const char *s) {
  char *p;

  if (!s)
    return NULL;
  p = strdup(s);
  if (!p)
    sh_fatal("out of memory");
  return p;
}
