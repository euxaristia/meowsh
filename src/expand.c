/*
 * meowsh — POSIX-compliant shell
 * expand.c — All 7 POSIX expansion steps:
 *   1. Tilde expansion
 *   2. Parameter expansion
 *   3. Command substitution
 *   4. Arithmetic expansion
 *   5. Field splitting
 *   6. Pathname expansion (globbing)
 *   7. Quote removal
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "expand.h"
#include "var.h"
#include "arith.h"
#include "field.h"
#include "sh_glob.h"
#include "sh_error.h"
#include "memalloc.h"
#include "mystring.h"
#include "options.h"
#include "input.h"
#include "parser.h"
#include "exec.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/wait.h>
#include <pwd.h>
#include <fnmatch.h>

/* Internal expand state */
struct expand_ctx {
	struct strbuf sb;
	int quoted;     /* inside double quotes */
};

static char *expand_param(const char *s, int quoted);
static char *expand_cmdsub(const char *cmd);
static char *expand_arith_expr(const char *expr);
static void expand_tilde(struct strbuf *sb, const char **sp);

/* Tilde expansion */
static void
expand_tilde(struct strbuf *sb, const char **sp)
{
	const char *s = *sp;
	const char *end;
	const char *home;

	if (*s != '~') {
		return;
	}
	s++;

	/* Find end of tilde prefix */
	end = s;
	while (*end && *end != '/' && *end != ':')
		end++;

	if (end == s) {
		/* ~/... -> $HOME/... */
		home = var_get("HOME");
		if (home)
			strbuf_addstr(sb, home);
		else
			strbuf_addch(sb, '~');
	} else {
		/* ~user/... -> lookup user's home */
		char *user = sh_malloc((size_t)(end - s) + 1);
		struct passwd *pw;

		memcpy(user, s, (size_t)(end - s));
		user[end - s] = '\0';

		/* Use getpwnam if available */
		pw = getpwnam(user);
		if (pw)
			strbuf_addstr(sb, pw->pw_dir);
		else {
			strbuf_addch(sb, '~');
			strbuf_addstr(sb, user);
		}
		free(user);
	}

	*sp = end;
}

/* Parameter expansion: ${var}, ${var:-default}, ${#var}, etc. */
static char *
expand_param(const char *s, int quoted)
{
	struct strbuf sb = STRBUF_INIT;
	const char *val;
	char name[256];
	size_t nlen = 0;
	int special = 0;

	/* Skip $ */
	if (*s == '$')
		s++;

	/* ${...} form */
	if (*s == '{') {
		const char *p = s + 1;
		const char *end;
		int op = 0;
		int colon = 0;
		int hash_prefix = 0;

		/* ${#var} — string length */
		if (*p == '#' && p[1] != '}') {
			hash_prefix = 1;
			p++;
		}

		/* Read variable name */
		end = p;
		while (*end && *end != '}' && *end != ':' && *end != '-' &&
		       *end != '=' && *end != '?' && *end != '+' &&
		       *end != '%' && *end != '#') {
			if (nlen < sizeof(name) - 1)
				name[nlen++] = *end;
			end++;
		}
		name[nlen] = '\0';

		if (hash_prefix && *end == '}') {
			/* ${#var} */
			val = var_get(name);
			if (!val)
				val = var_special(name[0]);
			{
				char buf[32];
				snprintf(buf, sizeof(buf), "%zu",
				    val ? strlen(val) : 0);
				strbuf_addstr(&sb, buf);
			}
			return strbuf_detach(&sb);
		}

		/* Check for operators */
		if (*end == ':') {
			colon = 1;
			end++;
		}
		if (*end == '-' || *end == '=' || *end == '?' || *end == '+') {
			op = *end++;
		} else if (*end == '#' && !hash_prefix) {
			op = '#';
			end++;
			if (*end == '#') {
				op = 'H'; /* ## */
				end++;
			}
		} else if (*end == '%') {
			op = '%';
			end++;
			if (*end == '%') {
				op = 'P'; /* %% */
				end++;
			}
		}

		/* Get the word after the operator */
		{
			const char *word_start = end;
			int depth = 1;
			while (*end && depth > 0) {
				if (*end == '{') depth++;
				else if (*end == '}') depth--;
				if (depth > 0) end++;
			}

			{
				size_t wlen = (size_t)(end - word_start);
				char *word = sh_malloc(wlen + 1);
				memcpy(word, word_start, wlen);
				word[wlen] = '\0';

				val = var_get(name);
				if (!val && name[0] && !name[1])
					val = var_special(name[0]);

				switch (op) {
				case '-':
					if ((!val) || (colon && val[0] == '\0'))
						strbuf_addstr(&sb, word);
					else
						strbuf_addstr(&sb, val);
					break;
				case '=':
					if ((!val) || (colon && val[0] == '\0')) {
						var_set(name, word, 0);
						strbuf_addstr(&sb, word);
					} else {
						strbuf_addstr(&sb, val);
					}
					break;
				case '?':
					if ((!val) || (colon && val[0] == '\0')) {
						if (word[0])
							sh_error("%s: %s", name, word);
						else
							sh_error("%s: parameter not set", name);
						free(word);
						strbuf_free(&sb);
						sh.last_status = 1;
						return sh_strdup("");
					}
					strbuf_addstr(&sb, val);
					break;
				case '+':
					if (val && !(colon && val[0] == '\0'))
						strbuf_addstr(&sb, word);
					break;
				case '#': /* ${var#pattern} — shortest prefix */
				case 'H': /* ${var##pattern} — longest prefix */
					if (val) {
						size_t vlen = strlen(val);
						size_t i;
						int found = 0;
						if (op == 'H') {
							for (i = vlen; i > 0; i--) {
								char *sub = sh_malloc(i + 1);
								memcpy(sub, val, i);
								sub[i] = '\0';
								if (fnmatch(word, sub, 0) == 0) {
									strbuf_addstr(&sb, val + i);
									found = 1;
									free(sub);
									break;
								}
								free(sub);
							}
						} else {
							for (i = 1; i <= vlen; i++) {
								char *sub = sh_malloc(i + 1);
								memcpy(sub, val, i);
								sub[i] = '\0';
								if (fnmatch(word, sub, 0) == 0) {
									strbuf_addstr(&sb, val + i);
									found = 1;
									free(sub);
									break;
								}
								free(sub);
							}
						}
						if (!found)
							strbuf_addstr(&sb, val);
					}
					break;
				case '%': /* ${var%pattern} — shortest suffix */
				case 'P': /* ${var%%pattern} — longest suffix */
					if (val) {
						size_t vlen = strlen(val);
						size_t i;
						int found = 0;
						if (op == 'P') {
							for (i = 0; i < vlen; i++) {
								if (fnmatch(word, val + i, 0) == 0) {
									strbuf_addmem(&sb, val, i);
									found = 1;
									break;
								}
							}
						} else {
							for (i = vlen; i > 0; i--) {
								if (fnmatch(word, val + i, 0) == 0) {
									strbuf_addmem(&sb, val, i);
									found = 1;
									break;
								}
							}
						}
						if (!found)
							strbuf_addstr(&sb, val);
					}
					break;
				default:
					/* No operator, just ${var} */
					if (val)
						strbuf_addstr(&sb, val);
					break;
				}
				free(word);
			}
		}

		return strbuf_detach(&sb);
	}

	/* Simple $var or $N or $special */
	if (*s == '?' || *s == '$' || *s == '!' || *s == '#' || *s == '-' ||
	    *s == '0' || *s == '*' || *s == '@') {
		val = var_special(*s);
		if (val)
			strbuf_addstr(&sb, val);
		return strbuf_detach(&sb);
	}

	if (isdigit((unsigned char)*s)) {
		val = var_special(*s);
		if (val)
			strbuf_addstr(&sb, val);
		return strbuf_detach(&sb);
	}

	/* Variable name */
	while (is_name_char(*s))
		name[nlen++] = *s++;
	name[nlen] = '\0';

	val = var_get(name);
	if (val)
		strbuf_addstr(&sb, val);
	else if (option_is_set(OPT_NOUNSET)) {
		sh_error("%s: parameter not set", name);
		sh.last_status = 1;
	}

	(void)quoted;
	(void)special;
	return strbuf_detach(&sb);
}

/* Command substitution: execute command, capture stdout */
static char *
expand_cmdsub(const char *cmd)
{
	int pipefd[2];
	pid_t pid;
	struct strbuf sb = STRBUF_INIT;

	if (pipe(pipefd) < 0) {
		sh_errorf("pipe");
		return sh_strdup("");
	}

	pid = fork();
	if (pid < 0) {
		sh_errorf("fork");
		close(pipefd[0]);
		close(pipefd[1]);
		return sh_strdup("");
	}

	if (pid == 0) {
		/* Child */
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(1);
		close(pipefd[1]);

		/* Parse and execute the command */
		input_push_string(cmd);
		{
			struct node *tree = parse_command(NULL, NULL);
			if (tree)
				exec_node(tree, 0);
		}
		_exit(sh.last_status);
	}

	/* Parent */
	close(pipefd[1]);
	{
		char buf[1024];
		ssize_t n;
		while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
			strbuf_addmem(&sb, buf, (size_t)n);
	}
	close(pipefd[0]);

	{
		int status;
		waitpid(pid, &status, 0);
	}

	/* Remove trailing newlines */
	while (sb.len > 0 && sb.buf[sb.len - 1] == '\n')
		sb.buf[--sb.len] = '\0';

	return strbuf_detach(&sb);
}

/* Arithmetic expansion */
static char *
expand_arith_expr(const char *expr)
{
	int err = 0;
	long result;
	char buf[32];

	result = arith_eval(expr, &err);
	if (err) {
		sh_error("arithmetic error: %s", expr);
		return sh_strdup("0");
	}
	snprintf(buf, sizeof(buf), "%ld", result);
	return sh_strdup(buf);
}

/* Expand a raw word string through all expansion steps.
 * This handles the raw lexer output which contains literal quotes,
 * $var, ${...}, $(...), $((...)), `...`, ~, etc. */
static char *
expand_raw(const char *s, int quoted, int *was_quoted)
{
	struct strbuf sb = STRBUF_INIT;
	const char *p = s;
	int any_quoted = 0;

	/* Tilde expansion (only at start or after : in assignments) */
	if (!quoted && *p == '~')
		expand_tilde(&sb, &p);

	while (*p) {
		switch (*p) {
		case '\\':
			if (quoted) {
				/* In double quotes: only \\, \$, \`, \", \newline */
				if (p[1] == '\\' || p[1] == '$' || p[1] == '`' ||
				    p[1] == '"' || p[1] == '\n') {
					if (p[1] != '\n')
						strbuf_addch(&sb, p[1]);
					p += 2;
				} else {
					strbuf_addch(&sb, *p++);
				}
			} else {
				if (p[1] == '\n') {
					p += 2; /* line continuation */
				} else if (p[1]) {
					any_quoted = 1;
					strbuf_addch(&sb, p[1]);
					p += 2;
				} else {
					strbuf_addch(&sb, *p++);
				}
			}
			break;

		case '\'':
			if (quoted) {
				strbuf_addch(&sb, *p++);
			} else {
				any_quoted = 1;
				p++;
				while (*p && *p != '\'')
					strbuf_addch(&sb, *p++);
				if (*p) p++; /* skip closing quote */
			}
			break;

		case '"':
			if (!quoted) {
				/* Enter double-quote context, expand inner */
				any_quoted = 1;
				p++;
				while (*p && *p != '"') {
					if (*p == '\\') {
						if (p[1] == '\\' || p[1] == '$' ||
						    p[1] == '`' || p[1] == '"') {
							strbuf_addch(&sb, p[1]);
							p += 2;
						} else if (p[1] == '\n') {
							p += 2;
						} else {
							strbuf_addch(&sb, *p++);
						}
					} else if (*p == '$') {
						char *expanded = expand_param(p, 1);
						strbuf_addstr(&sb, expanded);
						free(expanded);
						/* Skip past the parameter */
						p++;
						if (*p == '{') {
							int d = 1;
							p++;
							while (*p && d > 0) {
								if (*p == '{') d++;
								else if (*p == '}') d--;
								p++;
							}
						} else if (*p == '(') {
							if (p[1] == '(') {
								/* $(()) */
								p += 2;
								{
									const char *start = p;
									int d = 2;
									while (*p && d > 0) {
										if (*p == '(') d++;
										else if (*p == ')') d--;
										p++;
									}
									(void)start;
								}
							} else {
								p++;
								{
									int d = 1;
									while (*p && d > 0) {
										if (*p == '(') d++;
										else if (*p == ')') d--;
										p++;
									}
								}
							}
						} else {
							while (*p && is_name_char(*p))
								p++;
							if (*p && !is_name_char(*p) &&
							    *p != '?' && *p != '$' &&
							    *p != '!' && *p != '#' &&
							    *p != '-' && *p != '@' &&
							    *p != '*')
								; /* name ended */
							else if (*p == '?' || *p == '$' ||
							         *p == '!' || *p == '#' ||
							         *p == '-' || *p == '@' ||
							         *p == '*' ||
							         isdigit((unsigned char)*p))
								p++;
						}
					} else if (*p == '`') {
						/* Backtick in dquote */
						p++;
						{
							struct strbuf cmd = STRBUF_INIT;
							while (*p && *p != '`') {
								if (*p == '\\' && p[1]) {
									strbuf_addch(&cmd, p[1]);
									p += 2;
								} else {
									strbuf_addch(&cmd, *p++);
								}
							}
							if (*p) p++;
							{
								char *res = expand_cmdsub(cmd.buf);
								strbuf_addstr(&sb, res);
								free(res);
							}
							strbuf_free(&cmd);
						}
					} else {
						strbuf_addch(&sb, *p++);
					}
				}
				if (*p == '"') p++;
			} else {
				strbuf_addch(&sb, *p++);
			}
			break;

		case '$':
			if (p[1] == '(' && p[2] == '(') {
				/* $(( expr )) */
				struct strbuf expr = STRBUF_INIT;
				int depth = 2;
				p += 3;
				while (*p && depth > 0) {
					if (*p == '(') depth++;
					else if (*p == ')') depth--;
					if (depth > 0)
						strbuf_addch(&expr, *p);
					p++;
				}
				{
					char *res = expand_arith_expr(expr.buf);
					strbuf_addstr(&sb, res);
					free(res);
				}
				strbuf_free(&expr);
			} else if (p[1] == '(') {
				/* $( cmd ) */
				struct strbuf cmd = STRBUF_INIT;
				int depth = 1;
				p += 2;
				while (*p && depth > 0) {
					if (*p == '(') depth++;
					else if (*p == ')') depth--;
					if (depth > 0)
						strbuf_addch(&cmd, *p);
					p++;
				}
				{
					char *res = expand_cmdsub(cmd.buf);
					strbuf_addstr(&sb, res);
					free(res);
				}
				strbuf_free(&cmd);
			} else {
				char *expanded = expand_param(p, quoted);
				strbuf_addstr(&sb, expanded);
				free(expanded);
				/* Skip past */
				p++;
				if (*p == '{') {
					int d = 1;
					p++;
					while (*p && d > 0) {
						if (*p == '{') d++;
						else if (*p == '}') d--;
						p++;
					}
				} else if (*p == '?' || *p == '$' || *p == '!' ||
				           *p == '#' || *p == '-' || *p == '@' ||
				           *p == '*' || *p == '0') {
					p++;
				} else if (isdigit((unsigned char)*p)) {
					p++;
				} else {
					while (*p && is_name_char(*p))
						p++;
				}
			}
			break;

		case '`':
			/* Backtick command substitution */
			{
				struct strbuf cmd = STRBUF_INIT;
				p++;
				while (*p && *p != '`') {
					if (*p == '\\' && p[1]) {
						if (p[1] == '$' || p[1] == '`' || p[1] == '\\')
							strbuf_addch(&cmd, p[1]);
						else {
							strbuf_addch(&cmd, *p);
							strbuf_addch(&cmd, p[1]);
						}
						p += 2;
					} else {
						strbuf_addch(&cmd, *p++);
					}
				}
				if (*p) p++;
				{
					char *res = expand_cmdsub(cmd.buf);
					strbuf_addstr(&sb, res);
					free(res);
				}
				strbuf_free(&cmd);
			}
			break;

		default:
			strbuf_addch(&sb, *p++);
			break;
		}
	}

	if (was_quoted)
		*was_quoted = any_quoted || quoted;
	return strbuf_detach(&sb);
}

char *
expand_word(struct word *w, int quoted)
{
	struct strbuf sb = STRBUF_INIT;
	struct wordpart *p;

	if (!w)
		return sh_strdup("");

	for (p = w->parts; p; p = p->next) {
		char *expanded;

		switch (p->type) {
		case WPART_LITERAL:
			expanded = expand_raw(p->data, quoted, NULL);
			strbuf_addstr(&sb, expanded);
			free(expanded);
			break;
		case WPART_SQUOTE:
			strbuf_addstr(&sb, p->data);
			break;
		case WPART_PARAM:
			expanded = expand_param(p->data, quoted);
			strbuf_addstr(&sb, expanded);
			free(expanded);
			break;
		case WPART_CMDSUB:
			expanded = expand_cmdsub(p->data);
			strbuf_addstr(&sb, expanded);
			free(expanded);
			break;
		case WPART_ARITH:
			expanded = expand_arith_expr(p->data);
			strbuf_addstr(&sb, expanded);
			free(expanded);
			break;
		default:
			if (p->data)
				strbuf_addstr(&sb, p->data);
			break;
		}
	}

	return strbuf_detach(&sb);
}

char **
expand_words(struct word *words, int *countp)
{
	struct word *w;
	char **argv = NULL;
	int argc = 0;
	int cap = 0;

	for (w = words; w; w = w->next) {
		char *expanded;
		int was_quoted = 0;

		expanded = expand_raw(w->parts ? w->parts->data : "", 0,
		    &was_quoted);

		if (!was_quoted && !option_is_set(OPT_NOGLOB) &&
		    has_glob_chars(expanded)) {
			/* Field split then glob */
			int fc, gc;
			char **fields = field_split(expanded, &fc);
			int fi;
			free(expanded);

			for (fi = 0; fi < fc; fi++) {
				char **globs = glob_expand(fields[fi], &gc);
				int gi;
				for (gi = 0; gi < gc; gi++) {
					if (argc >= cap) {
						cap = cap ? cap * 2 : 16;
						argv = sh_realloc(argv,
						    (cap + 1) * sizeof(char *));
					}
					argv[argc++] = sh_strdup(globs[gi]);
				}
				glob_free(globs);
			}
			field_free(fields);
		} else if (!was_quoted) {
			/* Field split only */
			int fc;
			char **fields = field_split(expanded, &fc);
			int fi;
			free(expanded);

			for (fi = 0; fi < fc; fi++) {
				if (argc >= cap) {
					cap = cap ? cap * 2 : 16;
					argv = sh_realloc(argv,
					    (cap + 1) * sizeof(char *));
				}
				argv[argc++] = sh_strdup(fields[fi]);
			}
			field_free(fields);
		} else {
			/* Quoted: no splitting or globbing */
			if (argc >= cap) {
				cap = cap ? cap * 2 : 16;
				argv = sh_realloc(argv,
				    (cap + 1) * sizeof(char *));
			}
			argv[argc++] = expanded;
		}
	}

	if (!argv) {
		argv = sh_malloc(sizeof(char *));
	}
	argv[argc] = NULL;

	if (countp)
		*countp = argc;
	return argv;
}

char *
expand_assignment(struct word *w)
{
	return expand_word(w, 0);
}

void
expand_free(char **argv)
{
	int i;

	if (!argv)
		return;
	for (i = 0; argv[i]; i++)
		free(argv[i]);
	free(argv);
}

char *
expand_heredoc(const char *body)
{
	return expand_raw(body, 0, NULL);
}
