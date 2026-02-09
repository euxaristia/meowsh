/*
 * meowsh — POSIX-compliant shell
 * types.h — Token and AST type definitions
 */

#ifndef MEOWSH_TYPES_H
#define MEOWSH_TYPES_H

/* ---- Token types ---- */
typedef enum {
	/* Literals / words */
	TOK_WORD,           /* unquoted or quoted word */
	TOK_ASSIGNMENT,     /* name=value */
	TOK_IO_NUMBER,      /* digit preceding redirection */

	/* Operators */
	TOK_PIPE,           /* | */
	TOK_AND_IF,         /* && */
	TOK_OR_IF,          /* || */
	TOK_SEMI,           /* ; */
	TOK_AMP,            /* & */
	TOK_DSEMI,          /* ;; */
	TOK_LPAREN,         /* ( */
	TOK_RPAREN,         /* ) */

	/* Redirections */
	TOK_LESS,           /* < */
	TOK_GREAT,          /* > */
	TOK_DLESS,          /* << */
	TOK_DGREAT,         /* >> */
	TOK_LESSAND,        /* <& */
	TOK_GREATAND,       /* >& */
	TOK_LESSGREAT,      /* <> */
	TOK_CLOBBER,        /* >| */
	TOK_DLESSDASH,      /* <<- */

	/* Reserved words */
	TOK_IF,
	TOK_THEN,
	TOK_ELSE,
	TOK_ELIF,
	TOK_FI,
	TOK_DO,
	TOK_DONE,
	TOK_CASE,
	TOK_ESAC,
	TOK_WHILE,
	TOK_UNTIL,
	TOK_FOR,
	TOK_IN,
	TOK_LBRACE,         /* { */
	TOK_RBRACE,         /* } */
	TOK_BANG,           /* ! */

	/* Special */
	TOK_NEWLINE,
	TOK_EOF,

	TOK_COUNT
} token_type_t;

/* ---- AST node types ---- */
typedef enum {
	NODE_SIMPLE_CMD,
	NODE_PIPELINE,
	NODE_AND_OR,
	NODE_LIST,
	NODE_SUBSHELL,
	NODE_BRACE_GROUP,
	NODE_IF,
	NODE_WHILE,
	NODE_UNTIL,
	NODE_FOR,
	NODE_CASE,
	NODE_CASE_ITEM,
	NODE_FUNC_DEF,
	NODE_BANG,
} node_type_t;

/* ---- Redirection types ---- */
typedef enum {
	REDIR_INPUT,        /* < */
	REDIR_OUTPUT,       /* > */
	REDIR_APPEND,       /* >> */
	REDIR_HEREDOC,      /* << */
	REDIR_HEREDOC_STRIP,/* <<- */
	REDIR_DUP_INPUT,    /* <& */
	REDIR_DUP_OUTPUT,   /* >& */
	REDIR_RDWR,         /* <> */
	REDIR_CLOBBER,      /* >| */
} redir_type_t;

/* ---- AND_OR connector ---- */
typedef enum {
	CONN_SEMI,          /* ; or newline */
	CONN_AMP,           /* & */
	CONN_AND,           /* && */
	CONN_OR,            /* || */
	CONN_PIPE,          /* | */
} connector_t;

/* ---- Word part types ---- */
typedef enum {
	WPART_LITERAL,
	WPART_SQUOTE,
	WPART_DQUOTE,
	WPART_PARAM,        /* $var or ${...} */
	WPART_CMDSUB,       /* $(...) or `...` */
	WPART_ARITH,        /* $((...)) */
	WPART_TILDE,
} wpart_type_t;

/* ---- Word part ---- */
struct wordpart {
	wpart_type_t type;
	char *data;             /* raw text for this part */
	struct wordpart *next;
};

/* ---- Word ---- */
struct word {
	struct wordpart *parts;
	struct word *next;      /* for word lists */
};

/* ---- Redirect node ---- */
struct redirect {
	redir_type_t type;
	int fd;                 /* file descriptor number, -1 for default */
	struct word *filename;  /* target word (expanded later) */
	char *heredoc_body;     /* for heredocs: expanded body */
	int heredoc_quoted;     /* 1 if delimiter was quoted */
	struct redirect *next;
};

/* ---- Assignment ---- */
struct assignment {
	char *name;
	struct word *value;
	struct assignment *next;
};

/* ---- AST node ---- */
struct node {
	node_type_t type;
	int lineno;
	union {
		/* NODE_SIMPLE_CMD */
		struct {
			struct word *words;         /* command words */
			struct assignment *assigns; /* prefix assignments */
			struct redirect *redirs;
		} simple;

		/* NODE_PIPELINE */
		struct {
			struct node **cmds;
			int ncmds;
			int bang;                   /* ! prefix */
		} pipeline;

		/* NODE_AND_OR */
		struct {
			struct node *left;
			struct node *right;
			connector_t conn;           /* CONN_AND or CONN_OR */
		} and_or;

		/* NODE_LIST */
		struct {
			struct node **items;
			connector_t *connectors;    /* separator after each item */
			int nitems;
		} list;

		/* NODE_SUBSHELL, NODE_BRACE_GROUP */
		struct {
			struct node *body;
			struct redirect *redirs;
		} group;

		/* NODE_IF */
		struct {
			struct node *cond;
			struct node *then_body;
			struct node *else_body;     /* may be NULL or another if */
			struct redirect *redirs;
		} if_cmd;

		/* NODE_WHILE, NODE_UNTIL */
		struct {
			struct node *cond;
			struct node *body;
			struct redirect *redirs;
		} loop;

		/* NODE_FOR */
		struct {
			char *var;
			struct word *words;         /* NULL means use "$@" */
			int has_in;                 /* 1 if 'in' keyword was present */
			struct node *body;
			struct redirect *redirs;
		} for_cmd;

		/* NODE_CASE */
		struct {
			struct word *subject;
			struct node *items;         /* linked list of CASE_ITEM */
			struct redirect *redirs;
		} case_cmd;

		/* NODE_CASE_ITEM */
		struct {
			struct word *patterns;      /* pattern list */
			struct node *body;          /* may be NULL */
			struct node *next_item;
		} case_item;

		/* NODE_FUNC_DEF */
		struct {
			char *name;
			struct node *body;
			struct redirect *redirs;
		} func;

		/* NODE_BANG */
		struct {
			struct node *cmd;
		} bang;
	} data;
};

/* ---- Token ---- */
struct token {
	token_type_t type;
	char *value;
	int lineno;
};

#endif /* MEOWSH_TYPES_H */
