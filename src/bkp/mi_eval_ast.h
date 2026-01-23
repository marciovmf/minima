#ifndef MI_EVAL_AST_H
#define MI_EVAL_AST_H

#include "mi_parse.h"
#include "mi_runtime.h"
#include "mi_fold.h"

/**
 * Bind the runtime block execution hook to the AST backend.
 * @param rt Runtime instance to bind.
 */
void mi_ast_backend_bind(MiRuntime* rt);

/**
 * Execute a script using the AST interpreter.
 * @param rt     Runtime instance.
 * @param script Script AST to execute.
 * @return       Value of the last evaluated expression, or void.
 */
MiRtValue mi_eval_script_ast(MiRuntime* rt, const MiScript* script);

/**
 * Evaluate a single AST expression.
 * @param rt   Runtime instance.
 * @param expr Expression AST to evaluate.
 * @return     Resulting runtime value.
 */
MiRtValue mi_eval_expr_ast(MiRuntime* rt, const MiExpr* expr);

#endif // MI_EVAL_AST_H
