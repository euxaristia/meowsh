/*
 * meowsh — POSIX-compliant shell
 * var.h — Variable table, environment, special params
 */

#ifndef MEOWSH_VAR_H
#define MEOWSH_VAR_H

struct var_entry {
	char *name;
	char *value;
	unsigned int flags;
	struct var_entry *next;
};

/* Initialize variable table */
void var_init(void);

/* Get variable value (NULL if unset) */
const char *var_get(const char *name);

/* Set variable; returns 0 on success, -1 if readonly */
int var_set(const char *name, const char *value, int export);

/* Unset variable; returns 0 on success, -1 if readonly */
int var_unset(const char *name);

/* Mark variable as exported */
int var_export(const char *name);

/* Mark variable as readonly */
int var_readonly(const char *name, const char *value);

/* Import a "name=value" string from environment */
void var_import(const char *envstr);

/* Build environment array for execve */
char **var_environ(void);

/* Free environment array */
void var_environ_free(char **env);

/* Set positional parameters */
void var_set_posparams(int argc, char **argv);

/* Push/pop positional params (for function calls) */
void var_push_posparams(int argc, char **argv);
void var_pop_posparams(void);

/* Shift positional parameters */
int var_shift(int n);

/* Get special parameter value (returns static/arena string) */
const char *var_special(int c);

/* Lookup variable entry */
struct var_entry *var_lookup(const char *name);

/* Iterate all exported variables */
void var_walk_exports(void (*fn)(const char *name, const char *value));

#endif /* MEOWSH_VAR_H */
