/*
 * meowsh — POSIX-compliant shell
 * ast.c — AST construction and destruction
 */

#define _POSIX_C_SOURCE 200809L

#include "ast.h"
#include "memalloc.h"
#include "shell.h"

#include <string.h>

static struct node *
ast_node(node_type_t type)
{
	struct node *n;

	n = arena_alloc(&parse_arena, sizeof(*n));
	memset(n, 0, sizeof(*n));
	n->type = type;
	n->lineno = sh.lineno;
	return n;
}

struct node *
ast_simple_cmd(void)
{
	return ast_node(NODE_SIMPLE_CMD);
}

struct node *
ast_pipeline(struct node **cmds, int ncmds, int bang)
{
	struct node *n = ast_node(NODE_PIPELINE);
	struct node **arr;

	arr = arena_alloc(&parse_arena, ncmds * sizeof(*arr));
	memcpy(arr, cmds, ncmds * sizeof(*arr)); // flawfinder: ignore
	n->data.pipeline.cmds = arr;
	n->data.pipeline.ncmds = ncmds;
	n->data.pipeline.bang = bang;
	return n;
}

struct node *
ast_and_or(struct node *left, struct node *right, connector_t conn)
{
	struct node *n = ast_node(NODE_AND_OR);

	n->data.and_or.left = left;
	n->data.and_or.right = right;
	n->data.and_or.conn = conn;
	return n;
}

struct node *
ast_list(struct node **items, connector_t *conns, int nitems)
{
	struct node *n = ast_node(NODE_LIST);
	struct node **iarr;
	connector_t *carr;

	iarr = arena_alloc(&parse_arena, nitems * sizeof(*iarr));
	memcpy(iarr, items, nitems * sizeof(*iarr)); // flawfinder: ignore
	carr = arena_alloc(&parse_arena, nitems * sizeof(*carr));
	memcpy(carr, conns, nitems * sizeof(*carr)); // flawfinder: ignore
	n->data.list.items = iarr;
	n->data.list.connectors = carr;
	n->data.list.nitems = nitems;
	return n;
}

struct node *
ast_subshell(struct node *body, struct redirect *redirs)
{
	struct node *n = ast_node(NODE_SUBSHELL);

	n->data.group.body = body;
	n->data.group.redirs = redirs;
	return n;
}

struct node *
ast_brace_group(struct node *body, struct redirect *redirs)
{
	struct node *n = ast_node(NODE_BRACE_GROUP);

	n->data.group.body = body;
	n->data.group.redirs = redirs;
	return n;
}

struct node *
ast_if(struct node *cond, struct node *then_body, struct node *else_body,
    struct redirect *redirs)
{
	struct node *n = ast_node(NODE_IF);

	n->data.if_cmd.cond = cond;
	n->data.if_cmd.then_body = then_body;
	n->data.if_cmd.else_body = else_body;
	n->data.if_cmd.redirs = redirs;
	return n;
}

struct node *
ast_while(struct node *cond, struct node *body, struct redirect *redirs)
{
	struct node *n = ast_node(NODE_WHILE);

	n->data.loop.cond = cond;
	n->data.loop.body = body;
	n->data.loop.redirs = redirs;
	return n;
}

struct node *
ast_until(struct node *cond, struct node *body, struct redirect *redirs)
{
	struct node *n = ast_node(NODE_UNTIL);

	n->data.loop.cond = cond;
	n->data.loop.body = body;
	n->data.loop.redirs = redirs;
	return n;
}

struct node *
ast_for(const char *var, struct word *words, int has_in, struct node *body,
    struct redirect *redirs)
{
	struct node *n = ast_node(NODE_FOR);

	n->data.for_cmd.var = arena_strdup(&parse_arena, var);
	n->data.for_cmd.words = words;
	n->data.for_cmd.has_in = has_in;
	n->data.for_cmd.body = body;
	n->data.for_cmd.redirs = redirs;
	return n;
}

struct node *
ast_case(struct word *subject, struct node *items, struct redirect *redirs)
{
	struct node *n = ast_node(NODE_CASE);

	n->data.case_cmd.subject = subject;
	n->data.case_cmd.items = items;
	n->data.case_cmd.redirs = redirs;
	return n;
}

struct node *
ast_case_item(struct word *patterns, struct node *body)
{
	struct node *n = ast_node(NODE_CASE_ITEM);

	n->data.case_item.patterns = patterns;
	n->data.case_item.body = body;
	n->data.case_item.next_item = NULL;
	return n;
}

struct node *
ast_func_def(const char *name, struct node *body, struct redirect *redirs)
{
	struct node *n = ast_node(NODE_FUNC_DEF);

	n->data.func.name = arena_strdup(&parse_arena, name);
	n->data.func.body = body;
	n->data.func.redirs = redirs;
	return n;
}

struct word *
ast_word(void)
{
	struct word *w;

	w = arena_alloc(&parse_arena, sizeof(*w));
	w->parts = NULL;
	w->next = NULL;
	return w;
}

struct wordpart *
ast_wordpart(wpart_type_t type, const char *data)
{
	struct wordpart *p;

	p = arena_alloc(&parse_arena, sizeof(*p));
	p->type = type;
	p->data = arena_strdup(&parse_arena, data);
	p->next = NULL;
	return p;
}

void
word_append_part(struct word *w, struct wordpart *p)
{
	struct wordpart **pp;

	for (pp = &w->parts; *pp; pp = &(*pp)->next)
		;
	*pp = p;
}

void
word_list_append(struct word **head, struct word *w)
{
	struct word **pp;

	for (pp = head; *pp; pp = &(*pp)->next)
		;
	*pp = w;
}

struct redirect *
ast_redirect(redir_type_t type, int fd, struct word *filename)
{
	struct redirect *r;

	r = arena_alloc(&parse_arena, sizeof(*r));
	memset(r, 0, sizeof(*r));
	r->type = type;
	r->fd = fd;
	r->filename = filename;
	return r;
}

void
redir_list_append(struct redirect **head, struct redirect *r)
{
	struct redirect **pp;

	for (pp = head; *pp; pp = &(*pp)->next)
		;
	*pp = r;
}

struct assignment *
ast_assignment(const char *name, struct word *value)
{
	struct assignment *a;

	a = arena_alloc(&parse_arena, sizeof(*a));
	a->name = arena_strdup(&parse_arena, name);
	a->value = value;
	a->next = NULL;
	return a;
}

void
assign_list_append(struct assignment **head, struct assignment *a)
{
	struct assignment **pp;

	for (pp = head; *pp; pp = &(*pp)->next)
		;
	*pp = a;
}
