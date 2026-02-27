/*
 * meowsh — POSIX-compliant shell
 * parser.c — Recursive-descent parser (full POSIX grammar)
 *
 * Grammar:
 *   complete_command : list separator_op?
 *   list             : and_or (separator_op and_or)*
 *   and_or           : pipeline (('&&'|'||') linebreak pipeline)*
 *   pipeline         : '!'? command ('|' linebreak command)*
 *   command          : simple_command | compound_command redirect_list?
 *                    | function_definition
 *   compound_command : brace_group | subshell | for_clause | case_clause
 *                    | if_clause | while_clause | until_clause
 *   simple_command   : cmd_prefix cmd_word cmd_suffix?
 *                    | cmd_prefix
 *                    | cmd_name cmd_suffix?
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "parser.h"
#include "lexer.h"
#include "ast.h"
#include "sh_error.h"
#include "memalloc.h"
#include "mystring.h"

#include <string.h>

#define MAX_PIPELINE 256
#define MAX_LIST 256

static struct token *curtok;

static struct node *p_list(int top_level);
static struct node *p_and_or(void);
static struct node *p_pipeline(void);
static struct node *p_command(void);
static struct node *p_simple_command(void);
static struct node *p_compound_command(void);
static struct redirect *p_redirect(void);
static struct redirect *p_redirect_list(void);
static struct word *p_word_token(struct token *tok);
static void token_desc(const struct token *tok, char *buf, size_t size);

static void
next_token(void)
{
	curtok = lexer_next();
}

static int
peek_type(void)
{
	return lexer_peek()->type;
}

static int
accept(token_type_t type)
{
	if ((token_type_t)peek_type() == type) {
		next_token();
		return 1;
	}
	return 0;
}

static void
expect(token_type_t type)
{
	if (sh.parse_error)
		return;
	if (!accept(type)) {
		char got[128];
		token_desc(lexer_peek(), got, sizeof(got));
		sh_syntax("expected '%s', got %s",
		    token_name(type),
		    got);
	}
}

static void
skip_newlines(void)
{
	while (peek_type() == TOK_NEWLINE && !sh.parse_error)
		next_token();
}

static int
is_command_start(token_type_t t)
{
	switch (t) {
	case TOK_WORD:
	case TOK_ASSIGNMENT:
	case TOK_IO_NUMBER:
	case TOK_LESS:
	case TOK_GREAT:
	case TOK_DLESS:
	case TOK_DGREAT:
	case TOK_LESSAND:
	case TOK_GREATAND:
	case TOK_LESSGREAT:
	case TOK_CLOBBER:
	case TOK_DLESSDASH:
	case TOK_IF:
	case TOK_WHILE:
	case TOK_UNTIL:
	case TOK_FOR:
	case TOK_CASE:
	case TOK_LBRACE:
	case TOK_LPAREN:
	case TOK_BANG:
		return 1;
	default:
		return 0;
	}
}

static int
is_redirect(token_type_t t)
{
	switch (t) {
	case TOK_LESS:
	case TOK_GREAT:
	case TOK_DLESS:
	case TOK_DGREAT:
	case TOK_LESSAND:
	case TOK_GREATAND:
	case TOK_LESSGREAT:
	case TOK_CLOBBER:
	case TOK_DLESSDASH:
	case TOK_IO_NUMBER:
		return 1;
	default:
		return 0;
	}
}

static void
token_desc(const struct token *tok, char *buf, size_t size)
{
	const char *name;
	const char *val;

	if (!buf || size == 0)
		return;

	if (!tok) {
		snprintf(buf, size, "'%s'", "EOF");
		return;
	}

	name = token_name(tok->type);
	val = tok->value;

	if (val && *val) {
		snprintf(buf, size, "'%s' (%s)", val, name);
	} else {
		snprintf(buf, size, "'%s'", name);
	}
}

static struct word *
p_word_token(struct token *tok)
{
	struct word *w = ast_word();
	struct wordpart *p = ast_wordpart(WPART_LITERAL, tok->value);

	word_append_part(w, p);
	return w;
}

static struct redirect *
p_redirect(void)
{
	int fd = -1;
	token_type_t op;
	redir_type_t rtype;
	struct word *target;
	struct redirect *r;

	if (peek_type() == TOK_IO_NUMBER) {
		next_token();
		fd = atoi(curtok->value);
	}

	next_token();
	op = curtok->type;

	switch (op) {
	case TOK_LESS:      rtype = REDIR_INPUT; if (fd < 0) fd = 0; break;
	case TOK_GREAT:     rtype = REDIR_OUTPUT; if (fd < 0) fd = 1; break;
	case TOK_DGREAT:    rtype = REDIR_APPEND; if (fd < 0) fd = 1; break;
	case TOK_LESSAND:   rtype = REDIR_DUP_INPUT; if (fd < 0) fd = 0; break;
	case TOK_GREATAND:  rtype = REDIR_DUP_OUTPUT; if (fd < 0) fd = 1; break;
	case TOK_LESSGREAT: rtype = REDIR_RDWR; if (fd < 0) fd = 0; break;
	case TOK_CLOBBER:   rtype = REDIR_CLOBBER; if (fd < 0) fd = 1; break;
	case TOK_DLESS:     rtype = REDIR_HEREDOC; if (fd < 0) fd = 0; break;
	case TOK_DLESSDASH: rtype = REDIR_HEREDOC_STRIP; if (fd < 0) fd = 0; break;
	default:
		{
			char got[128];
			token_desc(lexer_peek(), got, sizeof(got));
			sh_syntax("unexpected token in redirection: %s", got);
		}
		return NULL;
	}

	expect(TOK_WORD);
	target = p_word_token(curtok);
	r = ast_redirect(rtype, fd, target);

	if (rtype == REDIR_HEREDOC || rtype == REDIR_HEREDOC_STRIP) {
		char *delim = sh_strdup(curtok->value);
		int strip = (rtype == REDIR_HEREDOC_STRIP);
		int quoted = 0;
		const char *p;

		for (p = delim; *p; p++) {
			if (*p == '\'' || *p == '"' || *p == '\\') {
				quoted = 1;
				break;
			}
		}

		if (quoted) {
			struct strbuf sb = STRBUF_INIT;
			p = delim;
			while (*p) {
				if (*p == '\\' && *(p + 1)) {
					p++;
					strbuf_addch(&sb, *p++);
				} else if (*p == '\'' || *p == '"') {
					char q = *p++;
					while (*p && *p != q)
						strbuf_addch(&sb, *p++);
					if (*p) p++;
				} else {
					strbuf_addch(&sb, *p++);
				}
			}
			free(delim);
			delim = strbuf_detach(&sb);
		}

		queue_heredoc(r, delim, strip, quoted);
		free(delim);
	}

	return r;
}

static struct redirect *
p_redirect_list(void)
{
	struct redirect *head = NULL;

	while (is_redirect(peek_type())) {
		struct redirect *r = p_redirect();
		if (r)
			redir_list_append(&head, r);
	}
	return head;
}

static struct node *
p_simple_command(void)
{
	struct node *n = ast_simple_cmd();
	struct word *words_head = NULL;
	struct assignment *assigns_head = NULL;
	struct redirect *redirs_head = NULL;
	int have_word = 0;

	for (;;) {
		token_type_t t = peek_type();

		if (t == TOK_ASSIGNMENT && !have_word) {
			char *eq, *name;
			struct word *value = NULL;

			next_token();
			eq = strchr(curtok->value, '=');
			name = arena_strndup(&parse_arena, curtok->value,
			    (size_t)(eq - curtok->value));
			if (eq[1]) {
				value = ast_word();
				word_append_part(value,
				    ast_wordpart(WPART_LITERAL, eq + 1));
			}
			assign_list_append(&assigns_head,
			    ast_assignment(name, value));
		} else if (is_redirect(t)) {
			struct redirect *r = p_redirect();
			if (r)
				redir_list_append(&redirs_head, r);
		} else if (t == TOK_WORD) {
			next_token();
			have_word = 1;
			word_list_append(&words_head, p_word_token(curtok));
		} else {
			break;
		}
	}

	n->data.simple.words = words_head;
	n->data.simple.assigns = assigns_head;
	n->data.simple.redirs = redirs_head;

	if (!words_head && !assigns_head && !redirs_head)
		return NULL;
	return n;
}

static struct node *
p_if_clause(void)
{
	struct node *cond, *then_body, *else_body = NULL;
	struct redirect *redirs;

	skip_newlines();
	cond = p_list(0);
	expect(TOK_THEN);
	skip_newlines();
	then_body = p_list(0);

	if (accept(TOK_ELIF)) {
		else_body = p_if_clause();
		return ast_if(cond, then_body, else_body, NULL);
	}

	if (accept(TOK_ELSE)) {
		skip_newlines();
		else_body = p_list(0);
	}

	expect(TOK_FI);
	redirs = p_redirect_list();
	return ast_if(cond, then_body, else_body, redirs);
}

static struct node *
p_while_clause(void)
{
	struct node *cond, *body;
	struct redirect *redirs;

	skip_newlines();
	cond = p_list(0);
	expect(TOK_DO);
	skip_newlines();
	body = p_list(0);
	expect(TOK_DONE);
	redirs = p_redirect_list();
	return ast_while(cond, body, redirs);
}

static struct node *
p_until_clause(void)
{
	struct node *cond, *body;
	struct redirect *redirs;

	skip_newlines();
	cond = p_list(0);
	expect(TOK_DO);
	skip_newlines();
	body = p_list(0);
	expect(TOK_DONE);
	redirs = p_redirect_list();
	return ast_until(cond, body, redirs);
}

static struct node *
p_for_clause(void)
{
	char *var;
	struct word *words = NULL;
	struct node *body;
	struct redirect *redirs;
	int has_in = 0;

	expect(TOK_WORD);
	var = arena_strdup(&parse_arena, curtok->value);

	skip_newlines();
	if (accept(TOK_IN)) {
		has_in = 1;
		while (peek_type() == TOK_WORD) {
			next_token();
			word_list_append(&words, p_word_token(curtok));
		}
		accept(TOK_SEMI);
		skip_newlines();
	} else if (accept(TOK_SEMI)) {
		skip_newlines();
	}

	expect(TOK_DO);
	skip_newlines();
	body = p_list(0);
	expect(TOK_DONE);
	redirs = p_redirect_list();
	return ast_for(var, words, has_in, body, redirs);
}

static struct node *
p_case_clause(void)
{
	struct word *subject;
	struct node *items = NULL, *last_item = NULL;

	expect(TOK_WORD);
	subject = p_word_token(curtok);
	skip_newlines();
	expect(TOK_IN);
	skip_newlines();

	while (peek_type() != TOK_ESAC) {
		struct word *patterns = NULL;
		struct node *body = NULL;
		struct node *item;

		accept(TOK_LPAREN);
		for (;;) {
			expect(TOK_WORD);
			word_list_append(&patterns, p_word_token(curtok));
			if (!accept(TOK_PIPE))
				break;
		}
		expect(TOK_RPAREN);
		skip_newlines();

		if (peek_type() != TOK_DSEMI && peek_type() != TOK_ESAC)
			body = p_list(0);

		item = ast_case_item(patterns, body);
		if (last_item)
			last_item->data.case_item.next_item = item;
		else
			items = item;
		last_item = item;

		if (accept(TOK_DSEMI))
			skip_newlines();
		else
			break;
	}

	expect(TOK_ESAC);
	return ast_case(subject, items, p_redirect_list());
}

static struct node *
p_brace_group(void)
{
	struct node *body;
	struct redirect *redirs;

	skip_newlines();
	body = p_list(0);
	expect(TOK_RBRACE);
	redirs = p_redirect_list();
	return ast_brace_group(body, redirs);
}

static struct node *
p_subshell(void)
{
	struct node *body;
	struct redirect *redirs;

	skip_newlines();
	body = p_list(0);
	expect(TOK_RPAREN);
	redirs = p_redirect_list();
	return ast_subshell(body, redirs);
}

static struct node *
p_compound_command(void)
{
	token_type_t t = peek_type();

	switch (t) {
	case TOK_IF:     next_token(); return p_if_clause();
	case TOK_WHILE:  next_token(); return p_while_clause();
	case TOK_UNTIL:  next_token(); return p_until_clause();
	case TOK_FOR:    next_token(); return p_for_clause();
	case TOK_CASE:   next_token(); return p_case_clause();
	case TOK_LBRACE: next_token(); return p_brace_group();
	case TOK_LPAREN: next_token(); return p_subshell();
	default:         return NULL;
	}
}

static struct node *
try_function_def(void)
{
	struct token *tok = lexer_peek();
	const char *name;

	if (tok->type != TOK_WORD || !is_name(tok->value))
		return NULL;

	next_token();
	name = curtok->value;

	if (peek_type() != TOK_LPAREN)
		return NULL; /* caller handles already-consumed word */

	next_token(); /* ( */
	expect(TOK_RPAREN);
	skip_newlines();

		{
			struct node *body = p_compound_command();
			if (!body) {
				{
					char got[128];
					token_desc(lexer_peek(), got, sizeof(got));
					sh_syntax("expected compound command in function definition, got %s",
					    got);
				}
				return NULL;
			}
			return ast_func_def(name, body, p_redirect_list());
		}
}

static struct node *
p_command(void)
{
	token_type_t t = peek_type();

	if (t == TOK_IF || t == TOK_WHILE || t == TOK_UNTIL ||
	    t == TOK_FOR || t == TOK_CASE || t == TOK_LBRACE ||
	    t == TOK_LPAREN)
		return p_compound_command();

	if (t == TOK_WORD) {
		struct node *func = try_function_def();
		if (func)
			return func;
		/* try_function_def consumed one WORD token. Build simple cmd. */
		if (curtok && curtok->type == TOK_WORD) {
			struct node *n = ast_simple_cmd();
			struct word *wh = NULL;
			struct redirect *rh = NULL;

			word_list_append(&wh, p_word_token(curtok));
			for (;;) {
				t = peek_type();
				if (t == TOK_WORD || t == TOK_ASSIGNMENT) {
					next_token();
					word_list_append(&wh, p_word_token(curtok));
				} else if (is_redirect(t)) {
					struct redirect *r = p_redirect();
					if (r) redir_list_append(&rh, r);
				} else {
					break;
				}
			}
			n->data.simple.words = wh;
			n->data.simple.redirs = rh;
			return n;
		}
	}

	return p_simple_command();
}

static struct node *
p_pipeline(void)
{
	struct node *cmds[MAX_PIPELINE];
	int ncmds = 0;
	int bang = 0;
	struct node *cmd;

	if (accept(TOK_BANG))
		bang = 1;

	cmd = p_command();
	if (!cmd)
		return NULL;
	cmds[ncmds++] = cmd;

		while (accept(TOK_PIPE)) {
			skip_newlines();
			cmd = p_command();
			if (!cmd) {
				{
					char got[128];
					token_desc(lexer_peek(), got, sizeof(got));
					sh_syntax("expected command after '|', got %s", got);
				}
				break;
			}
		if (ncmds >= MAX_PIPELINE) {
			sh_error("pipeline too long");
			break;
		}
		cmds[ncmds++] = cmd;
	}

	if (ncmds == 1 && !bang)
		return cmds[0];
	return ast_pipeline(cmds, ncmds, bang);
}

static struct node *
p_and_or(void)
{
	struct node *left = p_pipeline();
	if (!left)
		return NULL;

	for (;;) {
		connector_t conn;
		struct node *right;

		if (accept(TOK_AND_IF))
			conn = CONN_AND;
		else if (accept(TOK_OR_IF))
			conn = CONN_OR;
		else
			break;

		skip_newlines();
			right = p_pipeline();
			if (!right) {
				{
					char got[128];
					token_desc(lexer_peek(), got, sizeof(got));
					sh_syntax("expected command after '%s', got %s",
					    conn == CONN_AND ? "&&" : "||",
					    got);
				}
				return left;
			}
		left = ast_and_or(left, right, conn);
	}
	return left;
}

static struct node *
p_list(int top_level)
{
	struct node *items[MAX_LIST];
	connector_t conns[MAX_LIST];
	int nitems = 0;
	struct node *item;

	item = p_and_or();
	if (!item)
		return NULL;

	items[nitems] = item;
	conns[nitems] = CONN_SEMI;
	nitems++;

	for (;;) {
		connector_t sep;

		if (accept(TOK_SEMI))
			sep = CONN_SEMI;
		else if (accept(TOK_AMP))
			sep = CONN_AMP;
		else if (accept(TOK_NEWLINE)) {
			if (top_level) break;
			sep = CONN_SEMI;
			skip_newlines();
			if (!is_command_start(peek_type()))
				break;
			goto next_item;
		} else {
			break;
		}

		conns[nitems - 1] = sep;
		skip_newlines();

		if (!is_command_start(peek_type()))
			break;

	next_item:
		item = p_and_or();
		if (!item)
			break;
		if (nitems >= MAX_LIST) {
			sh_error("command list too long");
			break;
		}
		items[nitems] = item;
		conns[nitems] = CONN_SEMI;
		nitems++;
	}

	if (nitems == 1 && conns[0] == CONN_SEMI)
		return items[0];
	return ast_list(items, conns, nitems);
}

/* Public API */
struct node *
parse_command(const char *ps1, const char *ps2)
{
	struct node *n;

	sh.parse_error = 0;
	lexer_init();
	lexer_set_prompts(ps1, ps2);

	if (peek_type() == TOK_NEWLINE) {
		next_token();
		return NULL;
	}
	if (peek_type() == TOK_EOF)
		return NULL;

	n = p_list(1);
	if (sh.parse_error)
		return NULL;
	return n;
}
