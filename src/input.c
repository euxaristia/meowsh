/*
 * meowsh — POSIX-compliant shell
 * input.c — Stacked input sources
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "input.h"
#include "sh_error.h"
#include "memalloc.h"
#include "mystring.h"
#include "lineedit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INPUT_BUFSZ 1024

static int unget_char = -1;

void
input_init(void)
{
	sh.input = NULL;
	unget_char = -1;
}

static struct input_source *
input_alloc(void)
{
	struct input_source *is;

	is = sh_malloc(sizeof(*is));
	memset(is, 0, sizeof(*is));
	is->prev = sh.input;
	is->lineno = 1;
	sh.input = is;
	return is;
}

void
input_push_fd(int fd)
{
	struct input_source *is = input_alloc();

	is->type = INPUT_FD;
	is->u.fd = fd;
	is->buf = sh_malloc(INPUT_BUFSZ);
	is->buflen = 0;
	is->bufpos = 0;
}

void
input_push_string(const char *s)
{
	struct input_source *is = input_alloc();

	is->type = INPUT_STRING;
	is->u.string.str = sh_strdup(s);
	is->u.string.pos = 0;
}

void
input_push_file(const char *path)
{
	FILE *fp;

	fp = fopen(path, "r");
	if (!fp) {
		sh_errorf("%s", path);
		return;
	}

	{
		struct input_source *is = input_alloc();

		is->type = INPUT_FILE;
		is->u.fp = fp;
		is->buf = sh_malloc(INPUT_BUFSZ);
		is->buflen = 0;
		is->bufpos = 0;
	}
}

void
input_pop(void)
{
	struct input_source *is = sh.input;

	if (!is)
		return;

	sh.input = is->prev;

	switch (is->type) {
	case INPUT_FD:
		free(is->buf);
		break;
	case INPUT_STRING:
		free((char *)is->u.string.str);
		break;
	case INPUT_FILE:
		fclose(is->u.fp);
		free(is->buf);
		break;
	}
	free(is);
	unget_char = -1;
}

static int
input_refill(struct input_source *is)
{
	ssize_t n;

	switch (is->type) {
	case INPUT_FD:
		if (sh.interactive && is->u.fd == STDIN_FILENO) {
			char *line = lineedit_read(NULL);
			if (!line) {
				is->eof = 1;
				return -1;
			}
			free(is->buf);
			is->buf = line;
			is->buflen = strlen(line);
			is->bufpos = 0;
			return 0;
		}
		n = read(is->u.fd, is->buf, INPUT_BUFSZ);
		if (n <= 0) {
			is->eof = 1;
			return -1;
		}
		is->buflen = (size_t)n;
		is->bufpos = 0;
		return 0;
	case INPUT_FILE:
		n = (ssize_t)fread(is->buf, 1, INPUT_BUFSZ, is->u.fp);
		if (n <= 0) {
			is->eof = 1;
			return -1;
		}
		is->buflen = (size_t)n;
		is->bufpos = 0;
		return 0;
	case INPUT_STRING:
		/* Strings don't need refilling */
		return -1;
	}
	return -1;
}

int
input_getc(void)
{
	struct input_source *is;
	int c;

	if (unget_char >= 0) {
		c = unget_char;
		unget_char = -1;
		return c;
	}

	is = sh.input;
	if (!is)
		return -1;

	if (is->type == INPUT_STRING) {
		c = is->u.string.str[is->u.string.pos];
		if (c == '\0') {
			is->eof = 1;
			return -1;
		}
		is->u.string.pos++;
		if (c == '\n')
			sh.lineno++;
		return c;
	}

	/* FD or FILE */
	if (is->bufpos >= is->buflen) {
		if (input_refill(is) < 0)
			return -1;
	}
	c = (unsigned char)is->buf[is->bufpos++];
	if (c == '\n')
		sh.lineno++;
	return c;
}

void
input_ungetc(int c)
{
	if (c < 0)
		return;
	if (c == '\n')
		sh.lineno--;
	unget_char = c;
}

char *
input_readline(const char *prompt)
{
	if (sh.interactive && sh.input && sh.input->type == INPUT_FD &&
	    sh.input->u.fd == STDIN_FILENO) {
		return lineedit_read(prompt);
	}

	{
		struct strbuf sb = STRBUF_INIT;
		int c;

		if (prompt && sh.interactive) {
			fputs(prompt, stderr);
			fflush(stderr);
		}

		for (;;) {
			c = input_getc();
			if (c < 0) {
				if (sb.len > 0)
					return strbuf_detach(&sb);
				strbuf_free(&sb);
				return NULL;
			}
			strbuf_addch(&sb, (char)c);
			if (c == '\n')
				return strbuf_detach(&sb);
		}
	}
}
