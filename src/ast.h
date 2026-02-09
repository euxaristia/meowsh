/*
 * meowsh — POSIX-compliant shell
 * ast.h — AST construction and destruction
 */

#ifndef MEOWSH_AST_H
#define MEOWSH_AST_H

#include "types.h"

/* Create nodes (allocated from parse arena) */
struct node *ast_simple_cmd(void);
struct node *ast_pipeline(struct node **cmds, int ncmds, int bang);
struct node *ast_and_or(struct node *left, struct node *right, connector_t conn);
struct node *ast_list(struct node **items, connector_t *conns, int nitems);
struct node *ast_subshell(struct node *body, struct redirect *redirs);
struct node *ast_brace_group(struct node *body, struct redirect *redirs);
struct node *ast_if(struct node *cond, struct node *then_body,
    struct node *else_body, struct redirect *redirs);
struct node *ast_while(struct node *cond, struct node *body,
    struct redirect *redirs);
struct node *ast_until(struct node *cond, struct node *body,
    struct redirect *redirs);
struct node *ast_for(const char *var, struct word *words, int has_in,
    struct node *body, struct redirect *redirs);
struct node *ast_case(struct word *subject, struct node *items,
    struct redirect *redirs);
struct node *ast_case_item(struct word *patterns, struct node *body);
struct node *ast_func_def(const char *name, struct node *body,
    struct redirect *redirs);

/* Create word/parts (allocated from parse arena) */
struct word *ast_word(void);
struct wordpart *ast_wordpart(wpart_type_t type, const char *data);
void word_append_part(struct word *w, struct wordpart *p);
void word_list_append(struct word **head, struct word *w);

/* Create redirect (allocated from parse arena) */
struct redirect *ast_redirect(redir_type_t type, int fd, struct word *filename);
void redir_list_append(struct redirect **head, struct redirect *r);

/* Create assignment (allocated from parse arena) */
struct assignment *ast_assignment(const char *name, struct word *value);
void assign_list_append(struct assignment **head, struct assignment *a);

#endif /* MEOWSH_AST_H */
