#ifndef MI_FOLD_H
#define MI_FOLD_H

#include <stdbool.h>

/* Forward declarations to avoid include cycles. */
struct MiRuntime;
struct MiScript;

/* Perform constant folding on a script AST.
   This is a pure simplification pass: it does not execute variables or commands. */
void mi_fold_constants_ast(struct MiRuntime* rt, const struct MiScript* script);

#endif // MI_FOLD_H
