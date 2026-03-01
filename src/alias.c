/*
 * meowsh — POSIX-compliant shell
 * alias.c — Alias table and expansion
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "alias.h"
#include "memalloc.h"
#include "mystring.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void
alias_init(void)
{
	memset(sh.aliases, 0, sizeof(sh.aliases));
}

static struct alias_entry *
alias_lookup(const char *name)
{
	unsigned int h = hash_string(name);
	const struct alias_entry *a;

	for (a = sh.aliases[h]; a; a = a->next) {
		if (strcmp(a->name, name) == 0)
			return a;
	}
	return NULL;
}

int
alias_set(const char *name, const char *value)
{
	struct alias_entry *a;
	unsigned int h;

	a = alias_lookup(name);
	if (a) {
		free(a->value);
		a->value = sh_strdup(value);
		return 0;
	}

	a = sh_malloc(sizeof(*a));
	a->name = sh_strdup(name);
	a->value = sh_strdup(value);
	a->in_use = 0;

	h = hash_string(name);
	a->next = sh.aliases[h];
	sh.aliases[h] = a;

	return 0;
}

const char *
alias_get(const char *name)
{
	const struct alias_entry *a = alias_lookup(name);
	return a ? a->value : NULL;
}

int
alias_unset(const char *name)
{
	unsigned int h = hash_string(name);
	struct alias_entry *a, **pp;

	for (pp = &sh.aliases[h]; (a = *pp) != NULL; pp = &a->next) {
		if (strcmp(a->name, name) == 0) {
			*pp = a->next;
			free(a->name);
			free(a->value);
			free(a);
			return 0;
		}
	}
	return -1;
}

void
alias_unset_all(void)
{
	int i;
	struct alias_entry *a, *next;

	for (i = 0; i < HASH_SIZE; i++) {
		for (a = sh.aliases[i]; a; a = next) {
			next = a->next;
			free(a->name);
			free(a->value);
			free(a);
		}
		sh.aliases[i] = NULL;
	}
}

void
alias_print_all(void)
{
	int i;
	struct alias_entry *a;

	for (i = 0; i < HASH_SIZE; i++) {
		for (a = sh.aliases[i]; a; a = a->next) {
			printf("alias %s='%s'\n", a->name, a->value);
		}
	}
}

int
alias_print(const char *name)
{
	struct alias_entry *a = alias_lookup(name);
	if (a) {
		printf("alias %s='%s'\n", a->name, a->value);
		return 0;
	}
	return -1;
}

int
alias_mark_inuse(const char *name, int inuse)
{
	struct alias_entry *a = alias_lookup(name);
	if (a) {
		a->in_use = inuse;
		return 0;
	}
	return -1;
}

int
alias_is_inuse(const char *name)
{
	struct alias_entry *a = alias_lookup(name);
	return a ? a->in_use : 0;
}
