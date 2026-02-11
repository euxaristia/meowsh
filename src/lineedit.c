/*
 * meowsh — POSIX-compliant shell
 * lineedit.c — Basic line editing and history
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "lineedit.h"
#include "completion.h"
#include "builtin.h"
#include "alias.h"
#include "memalloc.h"
#include "mystring.h"
#include "var.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>

#define MAX_HISTORY 100

static char *history[MAX_HISTORY];
static time_t history_time[MAX_HISTORY];
static int history_count = 0;
static int history_max = MAX_HISTORY;

void
lineedit_init(void)
{
	memset(history, 0, sizeof(history));
	memset(history_time, 0, sizeof(history_time));
}

static void
history_add_at(const char *line, time_t when)
{
	if (!line || !*line)
		return;

	if (history_count == history_max) {
		free(history[0]);
		memmove(history, history + 1, (size_t)(history_max - 1) * sizeof(char *));
		memmove(history_time, history_time + 1,
		    (size_t)(history_max - 1) * sizeof(time_t));
		history_count--;
	}

	history[history_count++] = sh_strdup(line);
	history_time[history_count - 1] = when > 0 ? when : time(NULL);
}

void
history_add(const char *line)
{
	history_add_at(line, time(NULL));
}

static int
parse_time_prefix(const char *s, time_t *out)
{
	struct tm tm;
	int year, mon, mday, hour, min, sec;
	char trail;

	if (!s || !out)
		return 0;

	if (sscanf(s, "%4d-%2d-%2d %2d:%2d:%2d%c",
	    &year, &mon, &mday, &hour, &min, &sec, &trail) != 7)
		return 0;
	if (trail != ' ')
		return 0;
	if (year < 1970 || mon < 1 || mon > 12 || mday < 1 || mday > 31 ||
	    hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60)
		return 0;

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = year - 1900;
	tm.tm_mon = mon - 1;
	tm.tm_mday = mday;
	tm.tm_hour = hour;
	tm.tm_min = min;
	tm.tm_sec = sec;
	tm.tm_isdst = -1;

	*out = mktime(&tm);
	return *out != (time_t)-1;
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

static int
command_exists_for_highlight(const char *token)
{
	const char *path;

	if (!token || !*token)
		return 0;

	if (builtin_lookup(token) || alias_get(token))
		return 1;

	if (strchr(token, '/'))
		return access(token, X_OK) == 0;

	path = var_get("PATH");
	if (path && *path) {
		const char *pp, *end;
		size_t token_len = strlen(token);
		char fullpath[PATH_MAX];

		for (pp = path; ; pp = end + 1) {
			end = strchr(pp, ':');
			if (!end)
				end = pp + strlen(pp);

			if (end == pp) {
				if (2 + token_len >= sizeof(fullpath)) {
					if (*end == '\0')
						break;
					continue;
				}
				fullpath[0] = '.';
				fullpath[1] = '/';
				memcpy(fullpath + 2, token, token_len);
				fullpath[2 + token_len] = '\0';
			} else {
				size_t dir_len = (size_t)(end - pp);
				if (dir_len + 1 + token_len >= sizeof(fullpath)) {
					if (*end == '\0')
						break;
					continue;
				}
				memcpy(fullpath, pp, dir_len);
				fullpath[dir_len] = '/';
				memcpy(fullpath + dir_len + 1, token, token_len);
				fullpath[dir_len + 1 + token_len] = '\0';
			}

			if (access(fullpath, X_OK) == 0)
				return 1;
			if (*end == '\0')
				break;
		}
	}

	return 0;
}

static void
refresh_line(int fd, const char *prompt, struct strbuf *sb, int pos, int show_suggestion)
{
	struct strbuf colored = STRBUF_INIT;
	const char *line = sb->buf ? sb->buf : "";
	const char *p = line;
	int in_quote = 0;
	char quote_char = 0;
	const char *suggestion = NULL;
	size_t unknown_start[64];
	size_t unknown_len[64];
	size_t unknown_count = 0;
	size_t unknown_idx = 0;
	size_t i;

	/* Determine unknown command tokens at the start of each command segment. */
	{
		size_t len = strlen(line);
		size_t seg = 0;
		char token[PATH_MAX];

		while (seg < len) {
			size_t j = seg;
			int found_cmd = 0;

			while (j < len && (line[j] == ' ' || line[j] == '\t'))
				j++;
			if (j >= len)
				break;

			for (;;) {
				size_t ts, te, tl;

				while (j < len && (line[j] == ' ' || line[j] == '\t'))
					j++;
				if (j >= len)
					break;

				if (line[j] == ';' || line[j] == '|' || line[j] == '&')
					break;

				ts = j;
				while (j < len &&
				    line[j] != ' ' && line[j] != '\t' &&
				    line[j] != ';' && line[j] != '|' &&
				    line[j] != '&' && line[j] != '<' &&
				    line[j] != '>') {
					j++;
				}
				te = j;
				tl = te - ts;
				if (tl == 0)
					continue;

				if (tl >= sizeof(token)) {
					found_cmd = 1;
					break;
				}
				memcpy(token, line + ts, tl);
				token[tl] = '\0';

				if (strchr(token, '=') && !strchr(token, '/'))
					continue;

				if (!command_exists_for_highlight(token) &&
				    unknown_count < (sizeof(unknown_start) / sizeof(unknown_start[0]))) {
					unknown_start[unknown_count] = ts;
					unknown_len[unknown_count] = tl;
					unknown_count++;
				}
				found_cmd = 1;
				break;
			}

			while (j < len && line[j] != ';' && line[j] != '|' && line[j] != '&')
				j++;
			if (j < len) {
				if ((line[j] == '&' || line[j] == '|') &&
				    j + 1 < len && line[j + 1] == line[j]) {
					j += 2;
				} else {
					j++;
				}
			}
			seg = j;

			if (!found_cmd && seg >= len)
				break;
		}
	}
	
	/* Go to beginning of line, print prompt */
	write(fd, "\r", 1);
	if (prompt) write(fd, prompt, strlen(prompt));

	/* Find suggestion if at end of line */
	if (show_suggestion && pos == (int)sb->len && sb->len > 0) {
		int i;
		for (i = history_count - 1; i >= 0; i--) {
			if (prefix(history[i], sb->buf)) {
				suggestion = history[i] + sb->len;
				break;
			}
		}
	}

	/* Highlight words: builtins in blue, paths in cyan etc. */
	/* For now: simple quotes and comments */
	while (*p) {
		i = (size_t)(p - line);
		if (!in_quote && unknown_idx < unknown_count && i == unknown_start[unknown_idx]) {
			size_t seglen = unknown_len[unknown_idx];
			size_t k;
			strbuf_addstr(&colored, "\x1b[31m"); /* Red */
			for (k = 0; k < seglen && p[k]; k++)
				strbuf_addch(&colored, p[k]);
			strbuf_addstr(&colored, "\x1b[0m");
			p += seglen;
			unknown_idx++;
		} else if (!in_quote && (*p == '\'' || *p == '"')) {
			in_quote = 1;
			quote_char = *p;
			strbuf_addstr(&colored, "\x1b[33m"); /* Yellow */
			strbuf_addch(&colored, *p++);
		} else if (in_quote && *p == quote_char) {
			strbuf_addch(&colored, *p++);
			strbuf_addstr(&colored, "\x1b[0m");
			in_quote = 0;
		} else if (!in_quote && *p == '#') {
			strbuf_addstr(&colored, "\x1b[32m"); /* Green */
			while (*p) strbuf_addch(&colored, *p++);
			strbuf_addstr(&colored, "\x1b[0m");
		} else {
			strbuf_addch(&colored, *p++);
		}
	}
	if (in_quote) strbuf_addstr(&colored, "\x1b[0m");

	/* Ghost suggestion in gray */
	if (suggestion && *suggestion) {
		strbuf_addstr(&colored, "\x1b[90m"); /* Dark gray */
		strbuf_addstr(&colored, suggestion);
		strbuf_addstr(&colored, "\x1b[0m");
	}

	if (colored.buf) write(fd, colored.buf, colored.len);
	write(fd, "\x1b[K", 3); /* Clear to end */

	/* Move cursor back to pos */
	write(fd, "\r", 1);
	if (prompt) write(fd, prompt, strlen(prompt));
	{
		int i;
		for (i = 0; i < pos; i++) {
			if (sb->buf && sb->buf[i] == '\t') write(fd, "    ", 4);
			else write(fd, "\x1b[C", 3);
		}
	}
	strbuf_free(&colored);
}

static const char *
prompt_last_line(const char *prompt)
{
	const char *last = prompt;
	const char *p;

	if (!prompt)
		return NULL;

	for (p = prompt; *p; p++) {
		if (*p == '\n')
			last = p + 1;
	}
	return last;
}

char *
lineedit_read(const char *prompt)
{
	struct strbuf sb = STRBUF_INIT;
	int fd = STDIN_FILENO;
	int pos = 0;
	int history_idx = history_count;
	char *saved_current = NULL;
	const char *display_prompt = prompt;

	if (!isatty(fd)) {
		/* Fallback for non-tty */
		char buf[1024];
		if (fgets(buf, sizeof(buf), stdin)) {
			return sh_strdup(buf);
		}
		return NULL;
	}

	if (prompt && strchr(prompt, '\n')) {
		/* Print multiline prompt once; redraw only the last line. */
		write(fd, prompt, strlen(prompt));
		display_prompt = prompt_last_line(prompt);
	}

	enable_raw_mode(fd);
	{
		int suppress_suggestion = 0;
		refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);

		for (;;) {
		char c;
		if (read(fd, &c, 1) <= 0) break;

		if (c == '\r' || c == '\n') {
			strbuf_addch(&sb, '\n');
			write(fd, "\n", 1);
			break;
		} else if (c == 1) { /* Ctrl-A (Home) */
			pos = 0;
			refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
		} else if (c == 5) { /* Ctrl-E (End) */
			pos = (int)sb.len;
			refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
		} else if (c == '\t') { /* Tab completion */
			struct completion_result *cr = completion_get(sb.buf ? sb.buf : "", pos);
			if (cr && cr->count > 0) {
				int start = pos;
				const char *buf = sb.buf ? sb.buf : "";
				while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t' &&
				       buf[start - 1] != '|' && buf[start - 1] != ';' && buf[start - 1] != '&' &&
				       buf[start - 1] != '<' && buf[start - 1] != '>') {
					start--;
				}
				
				int pfx_len = pos - start;
				if (cr->common_len > (size_t)pfx_len) {
					/* Add common prefix characters */
					size_t to_add = cr->common_len - (size_t)pfx_len;
					strbuf_grow(&sb, to_add);
					if (pos < (int)sb.len) {
						memmove(sb.buf + pos + to_add, sb.buf + pos, sb.len - pos);
					}
					memcpy(sb.buf + pos, cr->matches[0] + pfx_len, to_add);
					sb.len += to_add;
					sb.buf[sb.len] = '\0';
					pos += (int)to_add;
					suppress_suggestion = 0;
					refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
				} else if (cr->count > 1) {
					/* Show matches */
					size_t i;
					write(fd, "\n", 1);
					for (i = 0; i < cr->count; i++) {
						write(fd, cr->matches[i], strlen(cr->matches[i]));
						write(fd, "  ", 2);
					}
					write(fd, "\n", 1);
					refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
				}
			}
			completion_free(cr);
			continue;
		} else if (c == 127 || c == 8) { /* Backspace */
			if (pos > 0) {
				pos--;
				memmove(sb.buf + pos, sb.buf + pos + 1, sb.len - pos - 1);
				sb.len--;
				sb.buf[sb.len] = '\0';
				suppress_suggestion = 1;
				refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
			}
		} else if (c == 4) { /* Ctrl-D */
			if (sb.len == 0) {
				disable_raw_mode(fd);
				strbuf_free(&sb);
				return NULL;
			} else if (pos < (int)sb.len) {
				memmove(sb.buf + pos, sb.buf + pos + 1, sb.len - pos - 1);
				sb.len--;
				sb.buf[sb.len] = '\0';
				suppress_suggestion = 1;
				refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
			}
		} else if (c == 21) { /* Ctrl-U (clear line) */
			pos = 0;
			sb.len = 0;
			if (sb.buf) sb.buf[0] = '\0';
			suppress_suggestion = 1;
			refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
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
						strbuf_free(&sb);
						strbuf_addstr(&sb, history[history_idx]);
						pos = (int)sb.len;
						suppress_suggestion = 0;
						refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
					}
				} else if (seq[1] == 'B') { /* Down arrow */
					if (history_idx < history_count) {
						history_idx++;
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
						pos = (int)sb.len;
						suppress_suggestion = 0;
						refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
					}
				} else if (seq[1] == 'D') { /* Left arrow */
					if (pos > 0) {
						pos--;
						refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
					}
				} else if (seq[1] == 'C') { /* Right arrow */
					if (pos < (int)sb.len) {
						pos++;
						refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
					} else {
						/* Accept suggestion if any */
						int i;
						for (i = history_count - 1; i >= 0; i--) {
							if (prefix(history[i], sb.buf ? sb.buf : "")) {
								strbuf_reset(&sb);
								strbuf_addstr(&sb, history[i]);
								pos = (int)sb.len;
								suppress_suggestion = 0;
								refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
								break;
							}
						}
					}
				} else if (seq[1] == 'H') { /* Home */
					pos = 0;
					refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
				} else if (seq[1] == 'F') { /* End */
					pos = (int)sb.len;
					refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
				}
			}
		} else if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
			strbuf_grow(&sb, 1);
			if (pos < (int)sb.len) {
				memmove(sb.buf + pos + 1, sb.buf + pos, sb.len - pos);
			}
			sb.buf[pos] = c;
			sb.len++;
			sb.buf[sb.len] = '\0';
			pos++;
			suppress_suggestion = 0;
			refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
		}
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
	for (i = history_count - 1; i >= 0; i--) {
		char tsbuf[32];
		struct tm tm;
		time_t when = history_time[i] > 0 ? history_time[i] : time(NULL);
		localtime_r(&when, &tm);
		strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tm);
		printf("%s %s\n", tsbuf, history[i]);
	}
}

void
history_clear(void)
{
	int i;

	for (i = 0; i < history_count; i++) {
		free(history[i]);
		history[i] = NULL;
		history_time[i] = 0;
	}
	history_count = 0;
}

void
history_load(const char *path)
{
	FILE *fp = fopen(path, "r");
	char buf[1024];
	if (!fp) return;
	while (fgets(buf, sizeof(buf), fp)) {
		time_t when;
		const char *cmd = buf;
		char *nl = strchr(buf, '\n');
		if (nl) *nl = '\0';
		if (parse_time_prefix(buf, &when) && strlen(buf) > 20)
			cmd = buf + 20;
		else
			when = time(NULL);
		history_add_at(cmd, when);
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
		char tsbuf[32];
		struct tm tm;
		time_t when = history_time[i] > 0 ? history_time[i] : time(NULL);
		localtime_r(&when, &tm);
		strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tm);
		fprintf(fp, "%s %s\n", tsbuf, history[i]);
	}
	fclose(fp);
}
