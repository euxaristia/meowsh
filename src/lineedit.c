/*
 * meowsh — POSIX-compliant shell
 * lineedit.c — Basic line editing and history
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "lineedit.h"
#include "memalloc.h"
#include "mystring.h"
#include "var.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#define MAX_HISTORY 100

static char *history[MAX_HISTORY];
static int history_count = 0;
static int history_max = MAX_HISTORY;

void
lineedit_init(void)
{
	memset(history, 0, sizeof(history));
}

void
history_add(const char *line)
{
	if (!line || !*line)
		return;

	/* Don't add if same as last */
	if (history_count > 0 && strcmp(history[history_count - 1], line) == 0)
		return;

	if (history_count == history_max) {
		free(history[0]);
		memmove(history, history + 1, (size_t)(history_max - 1) * sizeof(char *));
		history_count--;
	}

	history[history_count++] = sh_strdup(line);
}

static struct termios orig_termios;
static int raw_mode = 0;

static void
enable_raw_mode(int fd)
{
	struct termios raw;

	if (tcgetattr(fd, &orig_termios) < 0) return;
	raw = orig_termios;
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_cflag |= (CS8);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSAFLUSH, &raw) < 0) return;
	raw_mode = 1;
}

static void
disable_raw_mode(int fd)
{
	if (raw_mode) {
		tcsetattr(fd, TCSAFLUSH, &orig_termios);
		raw_mode = 0;
	}
}

char *
lineedit_read(const char *prompt)
{
	struct strbuf sb = STRBUF_INIT;
	int fd = STDIN_FILENO;
	int pos = 0;
	int history_idx = history_count;
	char *saved_current = NULL;

	if (!isatty(fd)) {
		/* Fallback for non-tty */
		char buf[1024];
		if (fgets(buf, sizeof(buf), stdin)) {
			return sh_strdup(buf);
		}
		return NULL;
	}

	if (prompt) {
		fputs(prompt, stderr);
		fflush(stderr);
	}

	enable_raw_mode(fd);

	for (;;) {
		char c;
		if (read(fd, &c, 1) <= 0) break;

		if (c == '\r' || c == '\n') {
			strbuf_addch(&sb, '\n');
			write(fd, "\n", 1);
			break;
		} else if (c == 127 || c == 8) { /* Backspace */
			if (pos > 0) {
				pos--;
				sb.len--;
				if (sb.buf) sb.buf[sb.len] = '\0';
				write(fd, "\b \b", 3);
			}
		} else if (c == 4) { /* Ctrl-D */
			if (sb.len == 0) {
				disable_raw_mode(fd);
				strbuf_free(&sb);
				return NULL;
			}
		} else if (c == 21) { /* Ctrl-U (clear line) */
			while (pos > 0) {
				write(fd, "\b \b", 3);
				pos--;
			}
			sb.len = 0;
			if (sb.buf) sb.buf[0] = '\0';
		} else if (c == 27) { /* Escape sequence */
			char seq[3];
			if (read(fd, &seq[0], 1) <= 0) break;
			if (read(fd, &seq[1], 1) <= 0) break;

			if (seq[0] == '[') {
				if (seq[1] == 'A') { /* Up arrow */
					if (history_idx > 0) {
						if (history_idx == history_count) {
							saved_current = sh_strdup(sb.buf ? sb.buf : "");
						}
						history_idx--;
						/* Clear current line */
						while (pos > 0) {
							write(fd, "\b \b", 3);
							pos--;
						}
						strbuf_free(&sb);
						strbuf_addstr(&sb, history[history_idx]);
						if (sb.buf) write(fd, sb.buf, sb.len);
						pos = (int)sb.len;
					}
				} else if (seq[1] == 'B') { /* Down arrow */
					if (history_idx < history_count) {
						history_idx++;
						/* Clear current line */
						while (pos > 0) {
							write(fd, "\b \b", 3);
							pos--;
						}
						strbuf_free(&sb);
						if (history_idx == history_count) {
							if (saved_current) {
								strbuf_addstr(&sb, saved_current);
								free(saved_current);
								saved_current = NULL;
							}
						} else {
							strbuf_addstr(&sb, history[history_idx]);
						}
						if (sb.buf) write(fd, sb.buf, sb.len);
						pos = (int)sb.len;
					}
				}
			}
		} else if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
			strbuf_addch(&sb, c);
			write(fd, &c, 1);
			pos++;
		}
	}

	disable_raw_mode(fd);
	free(saved_current);

	if (sb.len == 0) {
		strbuf_free(&sb);
		return NULL;
	}

	{
		char *res = strbuf_detach(&sb);
		if (res && *res && *res != '\n') {
			char *trimmed = sh_strdup(res);
			char *nl = strchr(trimmed, '\n');
			if (nl) *nl = '\0';
			history_add(trimmed);
			free(trimmed);
		}
		return res;
	}
}

void
lineedit_print_history(void)
{
	int i;
	for (i = 0; i < history_count; i++) {
		printf("%5d  %s\n", i + 1, history[i]);
	}
}

void
history_load(const char *path)
{
	FILE *fp = fopen(path, "r");
	char buf[1024];
	if (!fp) return;
	while (fgets(buf, sizeof(buf), fp)) {
		char *nl = strchr(buf, '\n');
		if (nl) *nl = '\0';
		history_add(buf);
	}
	fclose(fp);
}

void
history_save(const char *path)
{
	FILE *fp = fopen(path, "w");
	int i;
	if (!fp) return;
	for (i = 0; i < history_count; i++) {
		fprintf(fp, "%s\n", history[i]);
	}
	fclose(fp);
}