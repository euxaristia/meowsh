/*
 * meowsh — POSIX-compliant shell
 * input.h — Stacked input sources
 */

#ifndef MEOWSH_INPUT_H
#define MEOWSH_INPUT_H

#include <stdio.h>

typedef enum {
	INPUT_FD,
	INPUT_STRING,
	INPUT_FILE,
} input_type_t;

struct input_source {
	input_type_t type;
	union {
		int fd;
		FILE *fp;
		struct {
			const char *str;
			size_t pos;
		} string;
	} u;
	char *buf;
	size_t buflen;
	size_t bufpos;
	int lineno;
	int eof;
	struct input_source *prev;
};

/* Push/pop input sources */
void input_push_fd(int fd);
void input_push_string(const char *s);
void input_push_file(const char *path);
void input_pop(void);

/* Read a character from the current input source */
int input_getc(void);

/* Put a character back */
void input_ungetc(int c);
void input_clear_unget(void);

/* Read a complete line (for interactive mode) */
char *input_readline(const char *prompt);

/* Initialize input system */
void input_init(void);

#endif /* MEOWSH_INPUT_H */
