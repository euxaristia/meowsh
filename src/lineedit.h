/*
 * meowsh — POSIX-compliant shell
 * lineedit.h — Basic line editing and history
 */

#ifndef MEOWSH_LINEEDIT_H
#define MEOWSH_LINEEDIT_H

/* Initialize line editor */
void lineedit_init(void);

/* Read a line with editing and history */
char *lineedit_read(const char *prompt);

/* Add a line to history */

/* Print history */
void lineedit_print_history(void);
void history_clear(void);

/* Save/load history to/from file */
void history_load(const char *path);
void history_save(const char *path);

#endif /* MEOWSH_LINEEDIT_H */
