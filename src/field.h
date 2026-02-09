/*
 * meowsh — POSIX-compliant shell
 * field.h — IFS-based field splitting
 */

#ifndef MEOWSH_FIELD_H
#define MEOWSH_FIELD_H

/* Split a string by IFS into fields.
 * Returns NULL-terminated array, sets *countp. */
char **field_split(const char *s, int *countp);

/* Free field-split result */
void field_free(char **fields);

#endif /* MEOWSH_FIELD_H */
