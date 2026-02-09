/*
 * meowsh — POSIX-compliant shell
 * arith.h — $((expr)) arithmetic evaluator
 */

#ifndef MEOWSH_ARITH_H
#define MEOWSH_ARITH_H

/* Evaluate an arithmetic expression string. Returns the result.
 * Sets *errp to non-zero on error. */
long arith_eval(const char *expr, int *errp);

#endif /* MEOWSH_ARITH_H */
