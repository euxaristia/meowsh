/*
 * meowsh — POSIX-compliant shell
 * lexer.c — Hand-written POSIX tokenizer
 */

#define _POSIX_C_SOURCE 200809L

#include "lexer.h"
#include "alias.h"
#include "builtin.h"
#include "input.h"
#include "memalloc.h"
#include "mystring.h"
#include "sh_error.h"
#include "shell.h"

#include <string.h>

static struct token *peeked;
static const char *ps1_str;
static const char *ps2_str;
static int at_line_start;
static int alias_enabled;

/* Structures for pending heredocs */
struct heredoc_pending {
  struct redirect *redir;
  char *delim;
  int strip_tabs;
  int quoted;
  struct heredoc_pending *next;
};

static struct heredoc_pending *pending_heredocs;

static const struct {
  const char *name;
  token_type_t type;
} reserved_words[] = {{"if", TOK_IF},
                      {"then", TOK_THEN},
                      {"else", TOK_ELSE},
                      {"elif", TOK_ELIF},
                      {"fi", TOK_FI},
                      {"do", TOK_DO},
                      {"done", TOK_DONE},
                      {"case", TOK_CASE},
                      {"esac", TOK_ESAC},
                      {"while", TOK_WHILE},
                      {"until", TOK_UNTIL},
                      {"for", TOK_FOR},
                      {"in", TOK_IN},
                      {"{", TOK_LBRACE},
                      {"}", TOK_RBRACE},
                      {"!", TOK_BANG},
                      {NULL, 0}};

void lexer_init(void) {
  peeked = NULL;
  ps1_str = NULL;
  ps2_str = NULL;
  at_line_start = 1;
  alias_enabled = 1;
  pending_heredocs = NULL;
}

void lexer_set_prompts(const char *ps1, const char *ps2) {
  ps1_str = ps1;
  ps2_str = ps2;
  sh.cur_prompt = ps1;
}

void lexer_set_alias(int enable) { alias_enabled = enable; }

const char *token_name(token_type_t t) {
  static const char *names[] = {
      [TOK_WORD] = "WORD",
      [TOK_ASSIGNMENT] = "ASSIGNMENT",
      [TOK_IO_NUMBER] = "IO_NUMBER",
      [TOK_PIPE] = "|",
      [TOK_AND_IF] = "&&",
      [TOK_OR_IF] = "||",
      [TOK_SEMI] = ";",
      [TOK_AMP] = "&",
      [TOK_DSEMI] = ";;",
      [TOK_LPAREN] = "(",
      [TOK_RPAREN] = ")",
      [TOK_LESS] = "<",
      [TOK_GREAT] = ">",
      [TOK_DLESS] = "<<",
      [TOK_DGREAT] = ">>",
      [TOK_LESSAND] = "<&",
      [TOK_GREATAND] = ">&",
      [TOK_LESSGREAT] = "<>",
      [TOK_CLOBBER] = ">|",
      [TOK_DLESSDASH] = "<<-",
      [TOK_IF] = "if",
      [TOK_THEN] = "then",
      [TOK_ELSE] = "else",
      [TOK_ELIF] = "elif",
      [TOK_FI] = "fi",
      [TOK_DO] = "do",
      [TOK_DONE] = "done",
      [TOK_CASE] = "case",
      [TOK_ESAC] = "esac",
      [TOK_WHILE] = "while",
      [TOK_UNTIL] = "until",
      [TOK_FOR] = "for",
      [TOK_IN] = "in",
      [TOK_LBRACE] = "{",
      [TOK_RBRACE] = "}",
      [TOK_BANG] = "!",
      [TOK_NEWLINE] = "NEWLINE",
      [TOK_EOF] = "EOF",
  };

  if (t < TOK_COUNT)
    return names[t] ? names[t] : "???";
  return "???";
}

token_type_t reserved_word(const char *s) {
  int i;

  for (i = 0; reserved_words[i].name; i++) {
    if (strcmp(s, reserved_words[i].name) == 0)
      return reserved_words[i].type;
  }
  return TOK_WORD;
}

static struct token *make_token(token_type_t type, const char *value) {
  struct token *t;

  t = arena_alloc(&parse_arena, sizeof(*t));
  t->type = type;
  t->value = value ? arena_strdup(&parse_arena, value) : NULL;
  t->lineno = sh.lineno;
  return t;
}

static int nextchar(void) { return input_getc(); }

static void pushback(int c) { input_ungetc(c); }

/* Skip whitespace (spaces and tabs, not newlines) */
static void skip_blanks(void) {
  int c;

  for (;;) {
    c = nextchar();
    if (c != ' ' && c != '\t') {
      pushback(c);
      return;
    }
  }
}

/* Prompt for continuation line */
static void continuation_prompt(void) {
  if (sh.interactive) {
    sh.cur_prompt = ps2_str;
  }
}

/* Read heredoc body after newline */
static void read_pending_heredocs(void) {
  struct heredoc_pending *hd, *next;

  for (hd = pending_heredocs; hd; hd = next) {
    next = hd->next;
    hd->redir->heredoc_body =
        lexer_read_heredoc(hd->delim, hd->strip_tabs, hd->quoted);
    hd->redir->heredoc_quoted = hd->quoted;
    free(hd->delim);
    free(hd);
  }
  pending_heredocs = NULL;
}

/* Queue a heredoc to be read after newline */
void queue_heredoc(struct redirect *redir, const char *delim, int strip_tabs,
                   int quoted) {
  struct heredoc_pending *hd, **pp;

  hd = sh_malloc(sizeof(*hd));
  hd->redir = redir;
  hd->delim = sh_strdup(delim);
  hd->strip_tabs = strip_tabs;
  hd->quoted = quoted;
  hd->next = NULL;

  for (pp = &pending_heredocs; *pp; pp = &(*pp)->next)
    ;
  *pp = hd;
}

void lexer_clear_heredocs(void) {
  struct heredoc_pending *hd, *next;

  for (hd = pending_heredocs; hd; hd = next) {
    next = hd->next;
    free(hd->delim);
    free(hd);
  }
  pending_heredocs = NULL;
}

char *lexer_read_heredoc(const char *delim, int strip_tabs, int quoted) {
  struct strbuf sb = STRBUF_INIT;
  int c;
  size_t dlen = strlen(delim); // flawfinder: ignore // flawfinder: ignore

  (void)quoted; /* quoting affects expansion, not reading */

  for (;;) {
    struct strbuf line = STRBUF_INIT;

    /* Read one line */
    for (;;) {
      c = nextchar();
      if (c < 0) {
        strbuf_free(&line);
        goto done;
      }
      strbuf_addch(&line, (char)c);
      if (c == '\n')
        break;
    }

    /* Check if this line is the delimiter */
    {
      const char *lp = line.buf;
      if (strip_tabs) {
        while (*lp == '\t')
          lp++;
      }
      /* Compare without trailing newline */
      {
        size_t llen = strlen(lp); // flawfinder: ignore // flawfinder: ignore
        if (llen > 0 && lp[llen - 1] == '\n')
          llen--;
        if (llen == dlen && memcmp(lp, delim, dlen) == 0) {
          strbuf_free(&line);
          goto done;
        }
      }
    }

    strbuf_addstr(&sb, line.buf);
    strbuf_free(&line);
  }

done:
  if (sb.len == 0)
    return arena_strdup(&parse_arena, "");
  {
    char *result = arena_strdup(&parse_arena, sb.buf);
    strbuf_free(&sb);
    return result;
  }
}

/* Read a word token (handles quoting, expansions) */
static struct token *read_word(int first_char) {
  struct strbuf sb = STRBUF_INIT;
  int c = first_char;
  int depth;

  for (;;) {
    if (c < 0)
      break;

    switch (c) {
    case '\\':
      /* Backslash escape */
      strbuf_addch(&sb, (char)c);
      c = nextchar();
      if (c < 0)
        break;
      if (c == '\n') {
        /* Line continuation */
        sb.len--; /* remove the backslash */
        if (sb.buf)
          sb.buf[sb.len] = '\0';
        continuation_prompt();
        c = nextchar();
        if (c < 0)
          break;
        continue;
      }
      strbuf_addch(&sb, (char)c);
      break;

    case '\'':
      /* Single-quoted string */
      strbuf_addch(&sb, (char)c);
      for (;;) {
        c = nextchar();
        if (c < 0) {
          sh_syntax("unterminated single quote");
          goto done;
        }
        strbuf_addch(&sb, (char)c);
        if (c == '\'')
          break;
        if (c == '\n')
          continuation_prompt();
      }
      break;

    case '"':
      /* Double-quoted string */
      strbuf_addch(&sb, (char)c);
      for (;;) {
        c = nextchar();
        if (c < 0) {
          sh_syntax("unterminated double quote");
          goto done;
        }
        if (c == '\\') {
          int nc = nextchar();
          strbuf_addch(&sb, (char)c);
          if (nc >= 0)
            strbuf_addch(&sb, (char)nc);
          if (nc == '\n') {
            continuation_prompt();
            c = nextchar();
            if (c < 0)
              goto done;
            continue;
          }
          c = nextchar();
          if (c < 0)
            goto done;
          continue;
        }
        strbuf_addch(&sb, (char)c);
        if (c == '"')
          break;
        if (c == '`') {
          /* Backtick inside dquote */
          for (;;) {
            c = nextchar();
            if (c < 0)
              break;
            strbuf_addch(&sb, (char)c);
            if (c == '\\') {
              c = nextchar();
              if (c >= 0)
                strbuf_addch(&sb, (char)c);
              continue;
            }
            if (c == '`')
              break;
          }
        }
        if (c == '$') {
          int nc = nextchar();
          if (nc == '(') {
            strbuf_addch(&sb, (char)nc);
            /* Check for $(( )) */
            {
              int nnc = nextchar();
              if (nnc == '(') {
                strbuf_addch(&sb, (char)nnc);
                /* Arithmetic inside dquote */
                depth = 2;
                while (depth > 0) {
                  c = nextchar();
                  if (c < 0)
                    break;
                  strbuf_addch(&sb, (char)c);
                  if (c == '(')
                    depth++;
                  else if (c == ')')
                    depth--;
                }
                c = nextchar();
                if (c < 0)
                  goto done;
                continue;
              }
              pushback(nnc);
            }
            /* Command substitution inside dquote */
            depth = 1;
            while (depth > 0) {
              c = nextchar();
              if (c < 0)
                break;
              strbuf_addch(&sb, (char)c);
              if (c == '(')
                depth++;
              else if (c == ')')
                depth--;
              else if (c == '\'') {
                for (;;) {
                  c = nextchar();
                  if (c < 0)
                    break;
                  strbuf_addch(&sb, (char)c);
                  if (c == '\'')
                    break;
                }
              }
            }
            c = nextchar();
            if (c < 0)
              goto done;
            continue;
          } else if (nc == '{') {
            strbuf_addch(&sb, (char)nc);
            for (;;) {
              c = nextchar();
              if (c < 0)
                break;
              strbuf_addch(&sb, (char)c);
              if (c == '}')
                break;
            }
          } else {
            if (nc >= 0)
              pushback(nc);
          }
        }
        if (c == '\n')
          continuation_prompt();
      }
      break;

    case '`':
      /* Backtick command substitution */
      strbuf_addch(&sb, (char)c);
      for (;;) {
        c = nextchar();
        if (c < 0)
          break;
        strbuf_addch(&sb, (char)c);
        if (c == '\\') {
          c = nextchar();
          if (c >= 0)
            strbuf_addch(&sb, (char)c);
          c = nextchar();
          if (c < 0)
            break;
          continue;
        }
        if (c == '`')
          break;
      }
      break;

    case '$':
      strbuf_addch(&sb, (char)c);
      c = nextchar();
      if (c == '(') {
        strbuf_addch(&sb, (char)c);
        /* Check for $(( )) arithmetic */
        {
          int nc = nextchar();
          if (nc == '(') {
            strbuf_addch(&sb, (char)nc);
            depth = 2;
            while (depth > 0) {
              c = nextchar();
              if (c < 0)
                break;
              strbuf_addch(&sb, (char)c);
              if (c == '(')
                depth++;
              else if (c == ')')
                depth--;
            }
            c = nextchar();
            continue;
          }
          pushback(nc);
        }
        /* Command substitution $() */
        depth = 1;
        while (depth > 0) {
          c = nextchar();
          if (c < 0)
            break;
          strbuf_addch(&sb, (char)c);
          if (c == '(')
            depth++;
          else if (c == ')')
            depth--;
          else if (c == '\'') {
            for (;;) {
              c = nextchar();
              if (c < 0)
                break;
              strbuf_addch(&sb, (char)c);
              if (c == '\'')
                break;
            }
          } else if (c == '"') {
            for (;;) {
              c = nextchar();
              if (c < 0)
                break;
              strbuf_addch(&sb, (char)c);
              if (c == '\\') {
                c = nextchar();
                if (c >= 0)
                  strbuf_addch(&sb, (char)c);
                continue;
              }
              if (c == '"')
                break;
            }
          }
        }
        c = nextchar();
        continue;
      } else if (c == '{') {
        strbuf_addch(&sb, (char)c);
        depth = 1;
        for (;;) {
          c = nextchar();
          if (c < 0)
            break;
          strbuf_addch(&sb, (char)c);
          if (c == '{')
            depth++;
          else if (c == '}') {
            depth--;
            if (depth == 0)
              break;
          }
        }
      } else {
        if (c >= 0)
          pushback(c);
      }
      break;

    default:
      /* End of word on metacharacter */
      if (c == ' ' || c == '\t' || c == '\n' || c == '|' || c == '&' ||
          c == ';' || c == '<' || c == '>' || c == '(' || c == ')' ||
          c == '#') {
        pushback(c);
        goto done;
      }
      strbuf_addch(&sb, (char)c);
      break;
    }

    c = nextchar();
  }

done:
  if (sb.len == 0) {
    strbuf_free(&sb);
    return make_token(TOK_EOF, NULL);
  }

  {
    char *word = arena_strdup(&parse_arena, sb.buf);
    struct token *tok;
    strbuf_free(&sb);

    tok = make_token(TOK_WORD, word);

    /* Check for assignment word: NAME=... */
    if (is_assignment(word))
      tok->type = TOK_ASSIGNMENT;

    return tok;
  }
}

/* Read an operator token */
static struct token *read_operator(int c) {
  int nc;

  switch (c) {
  case '|':
    nc = nextchar();
    if (nc == '|')
      return make_token(TOK_OR_IF, "||");
    pushback(nc);
    return make_token(TOK_PIPE, "|");

  case '&':
    nc = nextchar();
    if (nc == '&')
      return make_token(TOK_AND_IF, "&&");
    pushback(nc);
    return make_token(TOK_AMP, "&");

  case ';':
    nc = nextchar();
    if (nc == ';')
      return make_token(TOK_DSEMI, ";;");
    pushback(nc);
    return make_token(TOK_SEMI, ";");

  case '<':
    nc = nextchar();
    if (nc == '<') {
      int nnc = nextchar();
      if (nnc == '-')
        return make_token(TOK_DLESSDASH, "<<-");
      pushback(nnc);
      return make_token(TOK_DLESS, "<<");
    }
    if (nc == '&')
      return make_token(TOK_LESSAND, "<&");
    if (nc == '>')
      return make_token(TOK_LESSGREAT, "<>");
    pushback(nc);
    return make_token(TOK_LESS, "<");

  case '>':
    nc = nextchar();
    if (nc == '>')
      return make_token(TOK_DGREAT, ">>");
    if (nc == '&')
      return make_token(TOK_GREATAND, ">&");
    if (nc == '|')
      return make_token(TOK_CLOBBER, ">|");
    pushback(nc);
    return make_token(TOK_GREAT, ">");

  case '(':
    return make_token(TOK_LPAREN, "(");

  case ')':
    return make_token(TOK_RPAREN, ")");

  default:
    break;
  }

  /* Should not reach here */
  {
    char buf[2] = {(char)c, '\0'}; // flawfinder: ignore
    return make_token(TOK_WORD, buf);
  }
}

static struct token *lexer_read_token(void) {
  int c;

  skip_blanks();
  c = nextchar();

  if (c < 0)
    return make_token(TOK_EOF, NULL);

  /* Comments */
  if (c == '#') {
    do {
      c = nextchar();
    } while (c >= 0 && c != '\n');
    if (c == '\n') {
      pushback(c);
      return lexer_read_token();
    }
    return make_token(TOK_EOF, NULL);
  }

  /* Newline */
  if (c == '\n') {
    at_line_start = 1;
    /* Read any pending heredocs */
    if (pending_heredocs)
      read_pending_heredocs();
    return make_token(TOK_NEWLINE, "\n");
  }

  /* Operators */
  if (c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' ||
      c == ')') {
    /* Check for IO_NUMBER: digit(s) before < or > */
    /* This is handled by the parser after getting a WORD */
    at_line_start = 0;
    return read_operator(c);
  }

  /* IO_NUMBER: check if this is a digit followed by < or > */
  if (isdigit((unsigned char)c)) {
    struct strbuf digits = STRBUF_INIT;
    strbuf_addch(&digits, (char)c);
    for (;;) {
      int nc = nextchar();
      if (nc >= 0 && isdigit((unsigned char)nc)) {
        strbuf_addch(&digits, (char)nc);
      } else if (nc == '<' || nc == '>') {
        /* This is an IO_NUMBER */
        char *val = arena_strdup(&parse_arena, digits.buf);
        struct token *tok;
        strbuf_free(&digits);
        pushback(nc);
        tok = make_token(TOK_IO_NUMBER, val);
        at_line_start = 0;
        return tok;
      } else {
        /* Not IO_NUMBER, push everything back */
        if (nc >= 0)
          pushback(nc);

        /* Push back all digits except the first one */
        for (size_t i = digits.len - 1; i >= 1; i--) {
          pushback((unsigned char)digits.buf[i]);
        }
        c = digits.buf[0];
        strbuf_free(&digits);
        goto read_as_word;
      }
    }
  }

read_as_word:
  at_line_start = 0;
  return read_word(c);
}

/* Check if a WORD token should be recognized as a reserved word.
 * The parser will call this when a reserved word is valid. */
static struct token *classify_word(struct token *tok) {
  token_type_t rw;

  if (tok->type != TOK_WORD)
    return tok;

  rw = reserved_word(tok->value);
  if (rw != TOK_WORD)
    tok->type = rw;

  return tok;
}

/* Try alias expansion on a word token.
 * Returns 1 if alias was expanded (token replaced). */
static int try_alias(struct token *tok) {
  const char *val;

  if (!alias_enabled)
    return 0;
  if (tok->type != TOK_WORD && tok->type < TOK_IF)
    return 0;
  if (!tok->value)
    return 0;
  if (is_special_builtin(tok->value))
    return 0;
  if (alias_is_inuse(tok->value))
    return 0;

  val = alias_get(tok->value);
  if (!val)
    return 0;

  /* Push alias expansion as a new input source */
  alias_mark_inuse(tok->value, 1);
  input_push_string(val);
  return 1;
}

struct token *lexer_next(void) {
  struct token *tok;

  if (peeked) {
    tok = peeked;
    peeked = NULL;
    return tok;
  }

  if (at_line_start)
    sh.cur_prompt = ps1_str;

  tok = lexer_read_token();

  /* Try alias expansion at command position */
  if (tok->type == TOK_WORD && alias_enabled) {
    if (try_alias(tok)) {
      tok = lexer_read_token();
    }
  }

  /* Classify reserved words */
  tok = classify_word(tok);

  return tok;
}

struct token *lexer_peek(void) {
  if (!peeked)
    peeked = lexer_next();
  return peeked;
}

void lexer_consume(void) { peeked = NULL; }
