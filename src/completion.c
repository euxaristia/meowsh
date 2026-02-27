/*
 * meowsh — POSIX-compliant shell
 * completion.c — Tab completion logic
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "completion.h"
#include "builtin.h"
#include "alias.h"
#include "var.h"
#include "mystring.h"
#include "memalloc.h"

#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static void
add_match(struct completion_result *cr, const char *match)
{
	size_t i;

	for (i = 0; i < cr->count; i++) {
		if (strcmp(cr->matches[i], match) == 0)
			return;
	}

	cr->matches = sh_realloc(cr->matches, (cr->count + 1) * sizeof(char *));
	cr->matches[cr->count++] = strdup(match);
}

void
completion_free(struct completion_result *cr)
{
	size_t i;
	if (!cr) return;
	for (i = 0; i < cr->count; i++)
		free(cr->matches[i]);
	free(cr->matches);
	free(cr);
}

static size_t
get_common_prefix(char **matches, size_t count)
{
	size_t i, j;
	if (count == 0) return 0;
	if (count == 1) return strlen(matches[0]);

	for (j = 0; matches[0][j]; j++) {
		for (i = 1; i < count; i++) {
			if (matches[i][j] != matches[0][j])
				return j;
		}
	}
	return j;
}

static int
cmp_matches(const void *a, const void *b)
{
	const char *ma = *(const char * const *)a;
	const char *mb = *(const char * const *)b;
	return strcmp(ma, mb);
}

static int
is_command_pos(const char *line, int pos)
{
	int i = pos;
	
	/* Back up to start of current word */
	while (i > 0 && line[i-1] != ' ' && line[i-1] != '\t' && 
	       line[i-1] != '|' && line[i-1] != ';' && line[i-1] != '&' &&
	       line[i-1] != '<' && line[i-1] != '>') {
		i--;
	}

	/* Back up past whitespace */
	while (i > 0 && (line[i-1] == ' ' || line[i-1] == '\t'))
		i--;

	if (i == 0) return 1;
	if (line[i-1] == '|' || line[i-1] == ';' || line[i-1] == '&') return 1;
	return 0;
}

static int
is_token_delim(char c)
{
	return c == ' ' || c == '\t' || c == '|' || c == ';' || c == '&' ||
	    c == '<' || c == '>';
}

static int
is_cd_argument_pos(const char *line, int start)
{
	int seg_start = start;
	int i;
	int token_index = 0;
	char cmd[64];

	while (seg_start > 0) {
		char c = line[seg_start - 1];
		if (c == '|' || c == ';' || c == '&')
			break;
		seg_start--;
	}

	cmd[0] = '\0';
	i = seg_start;
	while (i < start) {
		int ts;
		int tl;

		while (i < start && (line[i] == ' ' || line[i] == '\t'))
			i++;
		if (i >= start)
			break;
		if (line[i] == '|' || line[i] == ';' || line[i] == '&')
			break;
		if (line[i] == '<' || line[i] == '>') {
			i++;
			continue;
		}

		ts = i;
		while (i < start && !is_token_delim(line[i]))
			i++;
		tl = i - ts;
		if (tl <= 0)
			continue;

		if (token_index == 0) {
			size_t n = (size_t)tl;
			if (n >= sizeof(cmd))
				n = sizeof(cmd) - 1;
			memcpy(cmd, line + ts, n);
			cmd[n] = '\0';
		}
		token_index++;
	}

	return token_index == 1 && strcmp(cmd, "cd") == 0;
}

static void
complete_path(struct completion_result *cr, const char *pfx, int dirs_only)
{
	char *dir_path;
	const char *base_pfx;
	char *word_prefix;
	DIR *dir;
	struct dirent *de;
	const char *slash = strrchr(pfx, '/');
	int show_dotfiles;

	if (slash) {
		dir_path = strndup(pfx, (size_t)(slash - pfx + 1));
		if (dir_path[0] == '\0') {
			free(dir_path);
			dir_path = strdup("./");
		}
		base_pfx = slash + 1;
		word_prefix = strndup(pfx, (size_t)(slash - pfx + 1));
	} else {
		dir_path = strdup("./");
		base_pfx = pfx;
		word_prefix = strdup("");
	}

	dir = opendir(dir_path);
	if (!dir) {
		free(dir_path);
		free(word_prefix);
		return;
	}

	show_dotfiles = (base_pfx[0] == '.');

	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (!show_dotfiles && de->d_name[0] == '.')
			continue;
		if (prefix(de->d_name, base_pfx)) {
			char full[PATH_MAX];
			struct stat st;
			size_t len = strlen(word_prefix) + strlen(de->d_name) + 2;
			char *m;

			snprintf(full, sizeof(full), "%s%s", 
			    strcmp(dir_path, "./") == 0 ? "" : dir_path, 
			    de->d_name);

			m = malloc(len);
			if (!m)
				continue;
			snprintf(m, len, "%s%s", word_prefix, de->d_name);

			if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
				size_t mlen = strlen(m);
				m[mlen] = '/';
				m[mlen + 1] = '\0';
				add_match(cr, m);
			} else if (!dirs_only) {
				add_match(cr, m);
			}
			free(m);
		}
	}

	closedir(dir);
	free(dir_path);
	free(word_prefix);
}

static void
complete_exe_in_dir(struct completion_result *cr, const char *dir_path, const char *pfx)
{
	DIR *dir = opendir(dir_path);
	struct dirent *de;
	if (!dir) return;

	while ((de = readdir(dir)) != NULL) {
		if (prefix(de->d_name, pfx)) {
			char full[PATH_MAX];
			snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
			if (access(full, X_OK) == 0) {
				add_match(cr, de->d_name);
			}
		}
	}
	closedir(dir);
}

static void
complete_command(struct completion_result *cr, const char *pfx)
{
	/* Builtins */
	const struct builtin_entry *b;
	for (b = builtin_get_all(); b->name; b++) {
		if (prefix(b->name, pfx))
			add_match(cr, b->name);
	}

	/* Aliases */
	int i;
	for (i = 0; i < HASH_SIZE; i++) {
		struct alias_entry *ae = sh.aliases[i];
		while (ae) {
			if (prefix(ae->name, pfx))
				add_match(cr, ae->name);
			ae = ae->next;
		}
	}

	/* Functions */
	for (i = 0; i < HASH_SIZE; i++) {
		struct func_entry *fe = sh.functions[i];
		while (fe) {
			if (prefix(fe->name, pfx))
				add_match(cr, fe->name);
			fe = fe->next;
		}
	}

	/* PATH */
	const char *path = var_get("PATH");
	if (path) {
		char *p = strdup(path);
		char *saveptr;
		char *dir = strtok_r(p, ":", &saveptr);
		while (dir) {
			complete_exe_in_dir(cr, dir, pfx);
			dir = strtok_r(NULL, ":", &saveptr);
		}
		free(p);
	}
}

struct completion_result *
completion_get(const char *line, int pos)
{
	struct completion_result *cr = calloc(1, sizeof(*cr));
	int start = pos;
	while (start > 0 && line[start - 1] != ' ' && line[start - 1] != '	' && 
	       line[start - 1] != '|' && line[start - 1] != ';' && line[start - 1] != '&' &&
	       line[start - 1] != '<' && line[start - 1] != '>') {
		start--;
	}

	char *pfx = strndup(line + start, (size_t)(pos - start));

	if (strchr(pfx, '/') || !is_command_pos(line, start)) {
		complete_path(cr, pfx, is_cd_argument_pos(line, start));
	} else {
		complete_command(cr, pfx);
		/* Also complete builtins - I'll need to expose them */
	}

	free(pfx);

	if (cr->count > 0) {
		qsort(cr->matches, cr->count, sizeof(cr->matches[0]), cmp_matches);
		cr->common_len = get_common_prefix(cr->matches, cr->count);
	}

	return cr;
}
