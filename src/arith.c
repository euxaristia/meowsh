/*
 * meowsh — POSIX-compliant shell
 * arith.c — $((expr)) arithmetic evaluator
 *
 * Full POSIX arithmetic with C operator precedence:
 *   Ternary ?: , assignment, logical or/and, bitwise or/xor/and,
 *   equality, relational, shift, add/sub, mul/div/mod, unary, parens
 */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "arith.h"
#include "var.h"
#include "sh_error.h"
#include "mystring.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct arith_state {
	const char *p;
	int error;
};

static long a_expr(struct arith_state *st);
static long a_ternary(struct arith_state *st);
static long a_or(struct arith_state *st);
static long a_and(struct arith_state *st);
static long a_bitor(struct arith_state *st);
static long a_bitxor(struct arith_state *st);
static long a_bitand(struct arith_state *st);
static long a_eq(struct arith_state *st);
static long a_rel(struct arith_state *st);
static long a_shift(struct arith_state *st);
static long a_add(struct arith_state *st);
static long a_mul(struct arith_state *st);
static long a_unary(struct arith_state *st);
static long a_primary(struct arith_state *st);

static void
skip_ws(struct arith_state *st)
{
	while (*st->p && (*st->p == ' ' || *st->p == '\t' || *st->p == '\n'))
		st->p++;
}

static long
a_primary(struct arith_state *st)
{
	long val;

	skip_ws(st);

	if (*st->p == '(') {
		st->p++;
		val = a_expr(st);
		skip_ws(st);
		if (*st->p == ')')
			st->p++;
		else
			st->error = 1;
		return val;
	}

	/* Number */
	if (isdigit((unsigned char)*st->p)) {
		char *end;
		val = strtol(st->p, &end, 0);
		st->p = end;
		return val;
	}

	/* Variable name */
	if (isalpha((unsigned char)*st->p) || *st->p == '_') {
		char name[256];
		size_t n = 0;
		const char *v;

		while (is_name_char(*st->p) && n < sizeof(name) - 1)
			name[n++] = *st->p++;
		name[n] = '\0';

		v = var_get(name);
		if (v && *v) {
			/* Recursively evaluate variable value */
			int err2 = 0;
			val = arith_eval(v, &err2);
			if (err2) st->error = 1;
			return val;
		}
		return 0;
	}

	st->error = 1;
	return 0;
}

static long
a_unary(struct arith_state *st)
{
	skip_ws(st);

	if (*st->p == '+' && st->p[1] != '+') {
		st->p++;
		return a_unary(st);
	}
	if (*st->p == '-' && st->p[1] != '-') {
		st->p++;
		return -a_unary(st);
	}
	if (*st->p == '~') {
		st->p++;
		return ~a_unary(st);
	}
	if (*st->p == '!') {
		st->p++;
		return !a_unary(st);
	}
	return a_primary(st);
}

static long
a_mul(struct arith_state *st)
{
	long left = a_unary(st);

	for (;;) {
		skip_ws(st);
		if (*st->p == '*') {
			st->p++;
			left *= a_unary(st);
		} else if (*st->p == '/' && st->p[1] != '/') {
			long right;
			st->p++;
			right = a_unary(st);
			if (right == 0) {
				sh_error("division by zero");
				st->error = 1;
				return 0;
			}
			left /= right;
		} else if (*st->p == '%') {
			long right;
			st->p++;
			right = a_unary(st);
			if (right == 0) {
				sh_error("division by zero");
				st->error = 1;
				return 0;
			}
			left %= right;
		} else {
			break;
		}
	}
	return left;
}

static long
a_add(struct arith_state *st)
{
	long left = a_mul(st);

	for (;;) {
		skip_ws(st);
		if (*st->p == '+' && st->p[1] != '+' && st->p[1] != '=') {
			st->p++;
			left += a_mul(st);
		} else if (*st->p == '-' && st->p[1] != '-' && st->p[1] != '=') {
			st->p++;
			left -= a_mul(st);
		} else {
			break;
		}
	}
	return left;
}

static long
a_shift(struct arith_state *st)
{
	long left = a_add(st);

	for (;;) {
		skip_ws(st);
		if (*st->p == '<' && st->p[1] == '<' && st->p[2] != '=') {
			st->p += 2;
			left <<= a_add(st);
		} else if (*st->p == '>' && st->p[1] == '>' && st->p[2] != '=') {
			st->p += 2;
			left >>= a_add(st);
		} else {
			break;
		}
	}
	return left;
}

static long
a_rel(struct arith_state *st)
{
	long left = a_shift(st);

	for (;;) {
		skip_ws(st);
		if (*st->p == '<' && st->p[1] == '=') {
			st->p += 2;
			left = left <= a_shift(st);
		} else if (*st->p == '>' && st->p[1] == '=') {
			st->p += 2;
			left = left >= a_shift(st);
		} else if (*st->p == '<' && st->p[1] != '<') {
			st->p++;
			left = left < a_shift(st);
		} else if (*st->p == '>' && st->p[1] != '>') {
			st->p++;
			left = left > a_shift(st);
		} else {
			break;
		}
	}
	return left;
}

static long
a_eq(struct arith_state *st)
{
	long left = a_rel(st);

	for (;;) {
		skip_ws(st);
		if (*st->p == '=' && st->p[1] == '=') {
			st->p += 2;
			left = left == a_rel(st);
		} else if (*st->p == '!' && st->p[1] == '=') {
			st->p += 2;
			left = left != a_rel(st);
		} else {
			break;
		}
	}
	return left;
}

static long
a_bitand(struct arith_state *st)
{
	long left = a_eq(st);

	for (;;) {
		skip_ws(st);
		if (*st->p == '&' && st->p[1] != '&') {
			st->p++;
			left &= a_eq(st);
		} else {
			break;
		}
	}
	return left;
}

static long
a_bitxor(struct arith_state *st)
{
	long left = a_bitand(st);

	for (;;) {
		skip_ws(st);
		if (*st->p == '^') {
			st->p++;
			left ^= a_bitand(st);
		} else {
			break;
		}
	}
	return left;
}

static long
a_bitor(struct arith_state *st)
{
	long left = a_bitxor(st);

	for (;;) {
		skip_ws(st);
		if (*st->p == '|' && st->p[1] != '|') {
			st->p++;
			left |= a_bitxor(st);
		} else {
			break;
		}
	}
	return left;
}

static long
a_and(struct arith_state *st)
{
	long left = a_bitor(st);

	for (;;) {
		skip_ws(st);
		if (*st->p == '&' && st->p[1] == '&') {
			st->p += 2;
			{
				long right = a_bitor(st);
				left = left && right;
			}
		} else {
			break;
		}
	}
	return left;
}

static long
a_or(struct arith_state *st)
{
	long left = a_and(st);

	for (;;) {
		skip_ws(st);
		if (*st->p == '|' && st->p[1] == '|') {
			st->p += 2;
			{
				long right = a_and(st);
				left = left || right;
			}
		} else {
			break;
		}
	}
	return left;
}

static long
a_ternary(struct arith_state *st)
{
	long cond = a_or(st);

	skip_ws(st);
	if (*st->p == '?') {
		long if_true, if_false;
		st->p++;
		if_true = a_expr(st);
		skip_ws(st);
		if (*st->p == ':')
			st->p++;
		else
			st->error = 1;
		if_false = a_ternary(st);
		return cond ? if_true : if_false;
	}
	return cond;
}

static long
a_expr(struct arith_state *st)
{
	return a_ternary(st);
}

long
arith_eval(const char *expr, int *errp)
{
	struct arith_state st;
	long val;

	if (!expr || !*expr) {
		if (errp) *errp = 0;
		return 0;
	}

	st.p = expr;
	st.error = 0;

	val = a_expr(&st);

	skip_ws(&st);
	if (*st.p != '\0')
		st.error = 1;

	if (errp)
		*errp = st.error;
	return val;
}
