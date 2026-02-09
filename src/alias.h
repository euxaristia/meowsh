/*
 * meowsh — POSIX-compliant shell
 * alias.h — Alias table and expansion
 */

#ifndef MEOWSH_ALIAS_H
#define MEOWSH_ALIAS_H

struct alias_entry {
	char *name;
	char *value;
	int in_use;     /* recursion guard */
	struct alias_entry *next;
};

/* Initialize alias table */
void alias_init(void);

/* Define an alias */
int alias_set(const char *name, const char *value);

/* Lookup an alias (returns value or NULL) */
const char *alias_get(const char *name);

/* Remove an alias */
int alias_unset(const char *name);

/* Remove all aliases */
void alias_unset_all(void);

/* Print all aliases */
void alias_print_all(void);

/* Print a single alias */
int alias_print(const char *name);

/* Mark alias as in-use (for recursion prevention) */
int alias_mark_inuse(const char *name, int inuse);

/* Check if alias is currently being expanded */
int alias_is_inuse(const char *name);

#endif /* MEOWSH_ALIAS_H */
