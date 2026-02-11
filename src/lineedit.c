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
#include <sys/ioctl.h>
#include <stdarg.h>

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
static FILE *lineedit_debug_fp;
static int lineedit_debug_inited;

static void
lineedit_debugf(const char *fmt, ...)
{
	const char *enabled;
	va_list ap;

	if (!lineedit_debug_inited) {
		lineedit_debug_inited = 1;
		enabled = getenv("MEOWSH_DEBUG_LINEEDIT");
		if (enabled && *enabled)
			lineedit_debug_fp = fopen("/tmp/meowsh-lineedit.log", "a");
	}
	if (!lineedit_debug_fp)
		return;

	va_start(ap, fmt);
	vfprintf(lineedit_debug_fp, fmt, ap);
	va_end(ap);
	fputc('\n', lineedit_debug_fp);
	fflush(lineedit_debug_fp);
}

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
	const char *last;
	const char *p;
	const char *end;

	if (!prompt)
		return NULL;

	end = prompt + strlen(prompt);
	while (end > prompt && end[-1] == '\n')
		end--;

	last = prompt;
	for (p = prompt; p < end; p++) {
		if (*p == '\n')
			last = p + 1;
	}
	return last;
}

static int
word_start_at(const char *buf, int pos)
{
	int start = pos;

	while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t' &&
	       buf[start - 1] != '|' && buf[start - 1] != ';' && buf[start - 1] != '&' &&
	       buf[start - 1] != '<' && buf[start - 1] != '>') {
		start--;
	}
	return start;
}

static void
tab_state_clear(char **last_tab_line, int *last_tab_pos)
{
	free(*last_tab_line);
	*last_tab_line = NULL;
	*last_tab_pos = -1;
}

static void
tab_state_store(char **last_tab_line, int *last_tab_pos, const struct strbuf *sb, int pos)
{
	free(*last_tab_line);
	*last_tab_line = sh_strdup(sb->buf ? sb->buf : "");
	*last_tab_pos = pos;
}

struct menu_state {
	int active;
	char **matches;
	size_t count;
	size_t selected;
	char *base_line;
	int base_pos;
	int rows;
	int cols;
};

static void
menu_state_reset(struct menu_state *ms)
{
	size_t i;

	if (!ms)
		return;
	for (i = 0; i < ms->count; i++)
		free(ms->matches[i]);
	free(ms->matches);
	ms->matches = NULL;
	ms->count = 0;
	ms->selected = 0;
	free(ms->base_line);
	ms->base_line = NULL;
	ms->base_pos = 0;
	ms->active = 0;
	ms->rows = 0;
	ms->cols = 0;
}

static void
menu_clear_block(int fd, int rows)
{
	int i;

	if (rows <= 0)
		return;

	for (i = 0; i < rows; i++) {
		/* Cursor starts on prompt line beneath the menu. Move up and clear each row. */
		write(fd, "\x1b[1A\r\x1b[2K", 10);
	}
}

static void
menu_clear_and_restore_cursor(int fd, int rows)
{
	int i;

	if (rows <= 0)
		return;

	menu_clear_block(fd, rows);
	for (i = 0; i < rows; i++)
		write(fd, "\x1b[1B", 4);
}

static int
lineedit_terminal_width(int fd)
{
	struct winsize ws;

	if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return (int)ws.ws_col;
	return 80;
}

static void
lineedit_print_matches_columns(int fd, struct completion_result *cr, ssize_t selected_idx,
    int *rows_out, int *cols_out)
{
	size_t i;
	size_t max_len = 0;
	size_t col_width;
	size_t cols;
	size_t rows;
	int width;
	int rows_printed = 0;

	if (rows_out)
		*rows_out = 0;
	if (cols_out)
		*cols_out = 0;

	if (!cr || cr->count == 0)
		return;

	for (i = 0; i < cr->count; i++) {
		size_t len = strlen(cr->matches[i]);
		if (len > max_len)
			max_len = len;
	}
	if (max_len == 0)
		max_len = 1;

	width = lineedit_terminal_width(fd);
	col_width = max_len + 2;
	if ((size_t)width < col_width)
		col_width = (size_t)width;
	if (col_width == 0)
		col_width = 1;

	cols = (size_t)width / col_width;
	if (cols == 0)
		cols = 1;
	if (cols > cr->count)
		cols = cr->count;

	rows = (cr->count + cols - 1) / cols;
	for (i = 0; i < rows; i++) {
		size_t c;
		for (c = 0; c < cols; c++) {
			size_t idx = i * cols + c;
			size_t j;
			size_t len;
			size_t pad;
			if (idx >= cr->count)
				break;

			len = strlen(cr->matches[idx]);
			if ((ssize_t)idx == selected_idx) {
				write(fd, "\x1b[7m", 4);
				write(fd, cr->matches[idx], len);
				write(fd, "\x1b[0m", 4);
			} else {
				write(fd, cr->matches[idx], len);
			}

			if (c + 1 < cols && idx + 1 < cr->count) {
				pad = col_width > len ? col_width - len : 1;
				for (j = 0; j < pad; j++)
					write(fd, " ", 1);
			}
		}
		write(fd, "\n", 1);
		rows_printed++;
	}

	if (rows_out)
		*rows_out = rows_printed;
	if (cols_out)
		*cols_out = (int)cols;
}

static void
menu_state_start(struct menu_state *ms, const struct completion_result *cr,
    const struct strbuf *sb, int pos)
{
	size_t i;

	menu_state_reset(ms);

	ms->matches = sh_malloc(cr->count * sizeof(ms->matches[0]));
	ms->count = cr->count;
	for (i = 0; i < cr->count; i++)
		ms->matches[i] = sh_strdup(cr->matches[i]);
	ms->selected = 0;
	ms->base_line = sh_strdup(sb->buf ? sb->buf : "");
	ms->base_pos = pos;
	ms->active = 1;
}

static int
lineedit_apply_completion(struct strbuf *sb, int *pos, const char *match, int append_space);

static void
menu_apply_and_render(int fd, struct menu_state *menu, struct strbuf *sb, int *pos,
    const char *display_prompt, int show_suggestion)
{
	struct completion_result tmp = {0};

	if (!menu->active || menu->count == 0)
		return;

	strbuf_reset(sb);
	strbuf_addstr(sb, menu->base_line);
	*pos = menu->base_pos;
	lineedit_apply_completion(sb, pos, menu->matches[menu->selected], 0);

	menu_clear_block(fd, menu->rows);
	tmp.matches = menu->matches;
	tmp.count = menu->count;
	lineedit_print_matches_columns(fd, &tmp, (ssize_t)menu->selected,
	    &menu->rows, &menu->cols);
	refresh_line(fd, display_prompt, sb, *pos, show_suggestion);
}

static size_t
menu_move_vertical(const struct menu_state *menu, int dir)
{
	size_t idx = menu->selected;
	size_t count = menu->count;
	size_t cols = (size_t)(menu->cols > 0 ? menu->cols : 1);
	size_t col = idx % cols;
	size_t cand;

	if (dir > 0) {
		cand = idx + cols;
		if (cand < count)
			return cand;
		return col < count ? col : idx;
	}

	if (idx >= cols)
		return idx - cols;

	cand = col;
	while (cand + cols < count)
		cand += cols;
	return cand;
}

static int
lineedit_apply_completion(struct strbuf *sb, int *pos, const char *match, int append_space)
{
	int start;
	int pfx_len;
	size_t match_len;
	size_t new_len;
	size_t tail_len;

	if (!match)
		return 0;

	start = word_start_at(sb->buf ? sb->buf : "", *pos);
	pfx_len = *pos - start;
	match_len = strlen(match);

	if ((size_t)pfx_len > match_len)
		return 0;

	new_len = sb->len + (match_len - (size_t)pfx_len);
	if (append_space)
		new_len++;
	strbuf_grow(sb, new_len - sb->len);

	tail_len = sb->len - (size_t)*pos;
	memmove(sb->buf + start + match_len, sb->buf + *pos, tail_len + 1);
	memcpy(sb->buf + start, match, match_len);

	if (append_space) {
		memmove(sb->buf + start + match_len + 1, sb->buf + start + match_len, tail_len + 1);
		sb->buf[start + match_len] = ' ';
		*pos = (int)(start + match_len + 1);
	} else {
		*pos = (int)(start + match_len);
	}

	sb->len = new_len;
	return 1;
}

char *
lineedit_read(const char *prompt)
{
	struct strbuf sb = STRBUF_INIT;
	int fd = STDIN_FILENO;
	int pos = 0;
	int history_idx = history_count;
	char *saved_current = NULL;
	char *last_tab_line = NULL;
	int last_tab_pos = -1;
	struct menu_state menu = {0};
	const char *display_prompt = prompt;
	lineedit_debugf("begin prompt=%s", prompt ? prompt : "(null)");

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
		lineedit_debugf("multiline display_prompt=%s",
		    display_prompt ? display_prompt : "(null)");
	}

	enable_raw_mode(fd);
	{
		int suppress_suggestion = 0;
		refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);

		for (;;) {
		char c;
		if (read(fd, &c, 1) <= 0) break;
		lineedit_debugf("key=%u pos=%d len=%zu", (unsigned)c, pos, sb.len);

			if (c == '\r' || c == '\n') {
				menu_state_reset(&menu);
				tab_state_clear(&last_tab_line, &last_tab_pos);
				strbuf_addch(&sb, '\n');
				write(fd, "\n", 1);
				break;
				} else if (c == 3) { /* Ctrl-C */
					if (menu.active && menu.rows > 0)
						menu_clear_and_restore_cursor(fd, menu.rows);
					menu_state_reset(&menu);
					tab_state_clear(&last_tab_line, &last_tab_pos);
				history_idx = history_count;
					free(saved_current);
					saved_current = NULL;
					strbuf_reset(&sb);
					pos = 0;
					lineedit_debugf("ctrl-c in-place clear");
					suppress_suggestion = 1;
					refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
					continue;
				} else if (c == 1) { /* Ctrl-A (Home) */
				pos = 0;
				refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
			} else if (c == 5) { /* Ctrl-E (End) */
				pos = (int)sb.len;
				refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
			} else if (c == '\t') { /* Tab completion */
				if (sb.len == 0 && pos == 0) {
					menu_state_reset(&menu);
					tab_state_clear(&last_tab_line, &last_tab_pos);
					strbuf_addch(&sb, '\t');
					pos = 1;
					suppress_suggestion = 1;
					refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
					continue;
				}

				struct completion_result *cr = completion_get(sb.buf ? sb.buf : "", pos);
				const char *buf = sb.buf ? sb.buf : "";
			int start = word_start_at(buf, pos);
			int pfx_len = pos - start;
			int repeated_tab = (last_tab_line && last_tab_pos == pos &&
			    strcmp(last_tab_line, buf) == 0);

				if (cr && cr->count > 0) {
					if (menu.active && menu.count > 1) {
						menu.selected = (menu.selected + 1) % menu.count;
						suppress_suggestion = 0;
						menu_apply_and_render(fd, &menu, &sb, &pos, display_prompt,
						    !suppress_suggestion);
					} else if (cr->count == 1) {
						menu_state_reset(&menu);
					size_t mlen = strlen(cr->matches[0]);
					int append_space = 0;

					if (mlen > 0 && cr->matches[0][mlen - 1] != '/') {
						append_space = 1;
					}
					if (pos < (int)sb.len && buf[pos] != ' ' && buf[pos] != '\t' &&
					    buf[pos] != '\n') {
						append_space = 0;
					}
					if (lineedit_apply_completion(&sb, &pos, cr->matches[0], append_space)) {
						suppress_suggestion = 0;
						refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
					}
				} else if (cr->common_len > (size_t)pfx_len) {
					menu_state_reset(&menu);
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
				} else if (repeated_tab) {
					menu_state_start(&menu, cr, &sb, pos);
					if (lineedit_apply_completion(&sb, &pos, menu.matches[0], 0)) {
						suppress_suggestion = 0;
					}
					/* Enter menu mode and show highlighted matches. */
					write(fd, "\n", 1);
						{
							struct completion_result tmp = {0};
							tmp.matches = menu.matches;
							tmp.count = menu.count;
							lineedit_print_matches_columns(fd, &tmp, 0, &menu.rows, &menu.cols);
						}
						refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
					} else {
					menu_state_reset(&menu);
					write(fd, "\a", 1);
				}
				tab_state_store(&last_tab_line, &last_tab_pos, &sb, pos);
			} else {
				menu_state_reset(&menu);
				write(fd, "\a", 1);
				tab_state_store(&last_tab_line, &last_tab_pos, &sb, pos);
			}
			completion_free(cr);
			continue;
		} else if (c == 127 || c == 8) { /* Backspace */
			menu_state_reset(&menu);
			tab_state_clear(&last_tab_line, &last_tab_pos);
			if (pos > 0) {
				pos--;
				memmove(sb.buf + pos, sb.buf + pos + 1, sb.len - pos - 1);
				sb.len--;
				sb.buf[sb.len] = '\0';
				suppress_suggestion = 1;
				refresh_line(fd, display_prompt, &sb, pos, !suppress_suggestion);
			}
		} else if (c == 4) { /* Ctrl-D */
			menu_state_reset(&menu);
			tab_state_clear(&last_tab_line, &last_tab_pos);
			if (sb.len == 0) {
				disable_raw_mode(fd);
				menu_state_reset(&menu);
				free(last_tab_line);
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
			menu_state_reset(&menu);
			tab_state_clear(&last_tab_line, &last_tab_pos);
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
				if (menu.active && menu.count > 1) {
					if (seq[1] == 'Z') { /* Shift-Tab */
						menu.selected = (menu.selected + menu.count - 1) % menu.count;
						suppress_suggestion = 0;
						menu_apply_and_render(fd, &menu, &sb, &pos, display_prompt,
						    !suppress_suggestion);
						continue;
					}
					if (seq[1] == 'A') { /* Up */
						menu.selected = menu_move_vertical(&menu, -1);
						suppress_suggestion = 0;
						menu_apply_and_render(fd, &menu, &sb, &pos, display_prompt,
						    !suppress_suggestion);
						continue;
					}
					if (seq[1] == 'B') { /* Down */
						menu.selected = menu_move_vertical(&menu, 1);
						suppress_suggestion = 0;
						menu_apply_and_render(fd, &menu, &sb, &pos, display_prompt,
						    !suppress_suggestion);
						continue;
					}
					if (seq[1] == 'C') { /* Right */
						menu.selected = (menu.selected + 1) % menu.count;
						suppress_suggestion = 0;
						menu_apply_and_render(fd, &menu, &sb, &pos, display_prompt,
						    !suppress_suggestion);
						continue;
					}
					if (seq[1] == 'D') { /* Left */
						menu.selected = (menu.selected + menu.count - 1) % menu.count;
						suppress_suggestion = 0;
						menu_apply_and_render(fd, &menu, &sb, &pos, display_prompt,
						    !suppress_suggestion);
						continue;
					}
				}

				menu_state_reset(&menu);
				tab_state_clear(&last_tab_line, &last_tab_pos);
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
				} else if (seq[1] == 'Z') { /* Shift-Tab */
					write(fd, "\a", 1);
				}
			}
		} else if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
			menu_state_reset(&menu);
			tab_state_clear(&last_tab_line, &last_tab_pos);
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
	menu_state_reset(&menu);
	free(last_tab_line);
	free(saved_current);

	if (sb.len == 0) {
		lineedit_debugf("return NULL (empty)");
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
		lineedit_debugf("return line len=%zu", strlen(res));
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
