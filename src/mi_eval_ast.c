#include "mi_eval_ast.h"
#include "mi_log.h"
#include "mi_runtime.h"

#include <stdlib.h>
#include <string.h>

//----------------------------------------------------------
// Forwards
//----------------------------------------------------------

static MiRtValue s_eval_script_scoped(MiRuntime* rt, const MiScript* script);
static MiRtValue s_exec_block_ast(MiRuntime* rt, const MiRtBlock* block);

static MiRtValue s_eval_command_expr(MiRuntime* rt, const MiExpr* expr);
static MiRtValue s_eval_command_node(MiRuntime* rt, const MiCommand* cmd);

static void s_fold_program(const MiScript* script);
static void s_fold_command(MiRuntime* rt, MiCommand* cmd);
static void s_fold_expr(MiRuntime* rt, MiExpr* expr);

//----------------------------------------------------------
// Block execution binding
//----------------------------------------------------------

void mi_ast_backend_bind(MiRuntime* rt)
{
  if (!rt)
  {
    return;
  }
  mi_rt_set_exec_block(rt, s_exec_block_ast);
}

static MiRtValue s_eval_script_scoped(MiRuntime* rt, const MiScript* script)
{
  mi_rt_scope_push(rt);
  MiRtValue v = mi_eval_script_ast(rt, script);
  mi_rt_scope_pop(rt);
  return v;
}

static MiRtValue s_exec_block_ast(MiRuntime* rt, const MiRtBlock* block)
{
  if (!rt || !block)
  {
    return mi_rt_make_void();
  }

  if (block->kind == MI_RT_BLOCK_AST_SCRIPT)
  {
    return s_eval_script_scoped(rt, (const MiScript*)block->ptr);
  }
  if (block->kind == MI_RT_BLOCK_AST_EXPR)
  {
    return mi_eval_expr_ast(rt, (const MiExpr*)block->ptr);
  }

  mi_error("ast backend: unsupported block kind\n");
  return mi_rt_make_void();
}

//----------------------------------------------------------
// Expression evaluation
//----------------------------------------------------------

static MiRtValue s_eval_binary_string(MiTokenKind op, const MiRtValue* a, const MiRtValue* b)
{
  bool is_string = (a->kind == MI_RT_VAL_STRING) || (b->kind == MI_RT_VAL_STRING);
  if (!is_string)
  {
    return mi_rt_make_void();
  }

  if (op == MI_TOK_EQEQ)
  {
    bool equals = x_slice_eq(a->as.s, b->as.s);
    return mi_rt_make_bool(equals);
  }
  else if (op == MI_TOK_BANGEQ)
  {
    bool equals = x_slice_eq(a->as.s, b->as.s);
    return mi_rt_make_bool(!equals);
  }
  else
  {
    mi_error("unsupported string binary operator\n");
    return mi_rt_make_void();
  }
}

static MiRtValue s_eval_binary_numeric(MiTokenKind op, const MiRtValue* a, const MiRtValue* b)
{
  bool is_float = (a->kind == MI_RT_VAL_FLOAT) || (b->kind == MI_RT_VAL_FLOAT);

  if (!is_float &&
      (a->kind != MI_RT_VAL_INT && b->kind != MI_RT_VAL_INT) &&
      (a->kind != MI_RT_VAL_VOID && b->kind != MI_RT_VAL_VOID))
  {
    mi_error("binary operator: expected numeric operands\n");
    return mi_rt_make_void();
  }

  double da = 0.0;
  double db = 0.0;

  if (is_float)
  {
    da = (a->kind == MI_RT_VAL_FLOAT) ? a->as.f : (double)a->as.i;
    db = (b->kind == MI_RT_VAL_FLOAT) ? b->as.f : (double)b->as.i;
  }
  else
  {
    da = (double)a->as.i;
    db = (double)b->as.i;
  }

  switch (op)
  {
    case MI_TOK_PLUS:
      if (!is_float)
      {
        return mi_rt_make_int((long long)(da + db));
      }
      return mi_rt_make_float(da + db);

    case MI_TOK_MINUS:
      if (!is_float)
      {
        return mi_rt_make_int((long long)(da - db));
      }
      return mi_rt_make_float(da - db);

    case MI_TOK_STAR:
      if (!is_float)
      {
        return mi_rt_make_int((long long)(da * db));
      }
      return mi_rt_make_float(da * db);

    case MI_TOK_SLASH:
      if (db == 0.0)
      {
        mi_error("division by zero\n");
        return mi_rt_make_void();
      }
      return mi_rt_make_float(da / db);

    case MI_TOK_EQEQ:
      return mi_rt_make_bool(da == db);
    case MI_TOK_BANGEQ:
      return mi_rt_make_bool(da != db);
    case MI_TOK_LT:
      return mi_rt_make_bool(da < db);
    case MI_TOK_LTEQ:
      return mi_rt_make_bool(da <= db);
    case MI_TOK_GT:
      return mi_rt_make_bool(da > db);
    case MI_TOK_GTEQ:
      return mi_rt_make_bool(da >= db);

    default:
      mi_error("unsupported numeric binary operator\n");
      return mi_rt_make_void();
  }
}

MiRtValue mi_eval_expr_ast(MiRuntime* rt, const MiExpr* expr)
{
  if (!expr)
  {
    return mi_rt_make_void();
  }

  switch (expr->kind)
  {
    case MI_EXPR_INT_LITERAL:
      return mi_rt_make_int((long long)expr->as.int_lit.value);

    case MI_EXPR_FLOAT_LITERAL:
      return mi_rt_make_float(expr->as.float_lit.value);

    case MI_EXPR_STRING_LITERAL:
      return mi_rt_make_string_slice(expr->as.string_lit.value);

    case MI_EXPR_BOOL_LITERAL:
      return mi_rt_make_bool(expr->as.bool_lit.value);

    case MI_EXPR_VOID_LITERAL:
      return mi_rt_make_void();

    case MI_EXPR_VAR:
      {
        MiRtValue v;
        XSlice name = expr->as.var.name;

        if (expr->as.var.is_indirect)
        {
          MiRtValue name_v = mi_eval_expr_ast(rt, expr->as.var.name_expr);
          if (name_v.kind != MI_RT_VAL_STRING)
          {
            mi_error("indirect variable name must evaluate to string\n");
            return mi_rt_make_void();
          }
          if (name_v.as.s.length == 0)
          {
            mi_error("indirect variable name cannot be empty\n");
            return mi_rt_make_void();
          }
          name = name_v.as.s;
        }

        if (!mi_rt_var_get(rt, name, &v))
        {
          mi_error_fmt("undefined variable: %.*s\n", (int)name.length, name.ptr);
          return mi_rt_make_void();
        }

        return v;
      }

    case MI_EXPR_INDEX:
      {
        MiRtValue base = mi_eval_expr_ast(rt, expr->as.index.target);
        MiRtValue index = mi_eval_expr_ast(rt, expr->as.index.index);

        if (index.kind != MI_RT_VAL_INT)
        {
          mi_error("list index must be integer\n");
          return mi_rt_make_void();
        }

        if (base.kind == MI_RT_VAL_PAIR)
        {
          long long i = index.as.i;
          if (i < 0 || i > 1)
          {
            mi_error("pair index out of range\n");
            return mi_rt_make_void();
          }
          return base.as.pair->items[(size_t)i];
        }
        else if (base.kind == MI_RT_VAL_LIST)
        {
          long long i = index.as.i;
          if (i < 0 || (size_t)i >= base.as.list->count)
          {
            mi_error("list index out of range\n");
            return mi_rt_make_void();
          }
          return base.as.list->items[(size_t)i];
        }

        mi_error("indexing non-list or pair value\n");
        return mi_rt_make_void();
      }

    case MI_EXPR_UNARY:
      {
        MiRtValue v = mi_eval_expr_ast(rt, expr->as.unary.expr);
        switch (expr->as.unary.op)
        {
          case MI_TOK_PLUS:
            if (v.kind == MI_RT_VAL_INT)
            {
              return mi_rt_make_int(+v.as.i);
            }
            if (v.kind == MI_RT_VAL_FLOAT)
            {
              return mi_rt_make_float(+v.as.f);
            }
            mi_error("unary + expects numeric operand\n");
            return mi_rt_make_void();

          case MI_TOK_MINUS:
            if (v.kind == MI_RT_VAL_INT)
            {
              return mi_rt_make_int(-v.as.i);
            }
            if (v.kind == MI_RT_VAL_FLOAT)
            {
              return mi_rt_make_float(-v.as.f);
            }
            mi_error("unary - expects numeric operand\n");
            return mi_rt_make_void();

          case MI_TOK_NOT:
            if (v.kind != MI_RT_VAL_BOOL)
            {
               mi_error_fmt("not expects bool operand (%d)\n",  v.kind);
              return mi_rt_make_void();
            }
            return mi_rt_make_bool(!v.as.b);

          default:
            mi_error("unknown unary operator\n");
            return mi_rt_make_void();
        }
      }

    case MI_EXPR_BINARY:
      {
        MiTokenKind op = expr->as.binary.op;

        if (op == MI_TOK_AND || op == MI_TOK_OR)
        {
          MiRtValue left = mi_eval_expr_ast(rt, expr->as.binary.left);
          MiRtValue right = mi_eval_expr_ast(rt, expr->as.binary.right);

          if (left.kind != MI_RT_VAL_BOOL || right.kind != MI_RT_VAL_BOOL)
          {
            mi_error("and/or expect bool operands\n");
            return mi_rt_make_void();
          }

          if (op == MI_TOK_AND)
          {
            return mi_rt_make_bool(left.as.b && right.as.b);
          }
          return mi_rt_make_bool(left.as.b || right.as.b);
        }

        MiRtValue left = mi_eval_expr_ast(rt, expr->as.binary.left);
        MiRtValue right = mi_eval_expr_ast(rt, expr->as.binary.right);

        // voids are equal
        if (left.kind == MI_RT_VAL_VOID && right.kind == MI_RT_VAL_VOID)
          return mi_rt_make_bool(true);

        if (left.kind == MI_RT_VAL_STRING && right.kind == MI_RT_VAL_STRING)
        {
          return s_eval_binary_string(op, &left, &right);
        }
        return s_eval_binary_numeric(op, &left, &right);
      }

    case MI_EXPR_PAIR:
      mi_error("pair literals are only valid inside dictionary literals\n");
      return mi_rt_make_void();

    case MI_EXPR_LIST:
      {
        MiRtList* list = mi_rt_list_create(rt);
        MiExprList* it = expr->as.list.items;
        while (it)
        {
          MiRtValue v = mi_eval_expr_ast(rt, it->expr);
          (void)mi_rt_list_push(list, v);
          it = it->next;
        }
        return mi_rt_make_list(list);
      }

    case MI_EXPR_DICT:
      mi_error("dicts not supported yet\n");
      return mi_rt_make_void();

    case MI_EXPR_BLOCK:
      {
        MiRtBlock* b = mi_rt_block_create(rt);
        b->kind = MI_RT_BLOCK_AST_SCRIPT;
        b->ptr = expr->as.block.script;
        b->id = 0;
        return mi_rt_make_block(b);
      }

    case MI_EXPR_COMMAND:
      return s_eval_command_expr(rt, expr);

    default:
      mi_error("unknown expression kind\n");
      return mi_rt_make_void();
  }
}

//----------------------------------------------------------
// Commands
//----------------------------------------------------------

MiRtCommandFn mi_cmd_find(MiRuntime* rt, XSlice name)
{
  size_t i;

  if (!rt)
  {
    return NULL;
  }

  for (i = 0u; i < rt->command_count; ++i)
  {
    if (x_slice_eq(rt->commands[i].name, name))
    {
      return rt->commands[i].fn;
    }
  }

  return NULL;
}

static MiRtValue s_eval_command_expr(MiRuntime* rt, const MiExpr* expr)
{
  (void) rt;
  (void) expr;
  //if (!expr || expr->kind != MI_EXPR_COMMAND)
  //{
  //  return mi_rt_make_void();
  //}

  //MiRtValue head_val = mi_eval_expr_ast(rt, expr->as.command.head);
  //if (head_val.kind != MI_RT_VAL_STRING)
  //{
  //  mi_error("command head is not a string\n");
  //  return mi_rt_make_void();
  //}

  //XSlice head_name = head_val.as.s;
  //MiRtCommandFn fn = mi_cmd_find(rt, head_name);
  //if (!fn)
  //{
  //  mi_error_fmt("unknown command: %.*s\n", (int)head_name.length, head_name.ptr);
  //  return mi_rt_make_void();
  //}

  //int argc = expr->as.command.argc;
  //if (argc <= 0)
  //{
  //  return fn(rt, &head_name, 0, NULL);
  //}

  //return fn(rt, &head_name, argc, expr->as.command.args);

  return mi_rt_make_void();
}

static MiRtValue s_eval_command_node(MiRuntime* rt, const MiCommand* cmd)
{
  if (!cmd || !cmd->head)
  {
    return mi_rt_make_void();
  }

  MiExpr expr;
  memset(&expr, 0, sizeof(expr));
  expr.kind = MI_EXPR_COMMAND;
  expr.token = cmd->head->token;
  expr.as.command.head = cmd->head;
  expr.as.command.args = cmd->args;
  expr.as.command.argc = cmd->argc;
  return s_eval_command_expr(rt, &expr);
}

MiRtValue mi_eval_script_ast(MiRuntime* rt, const MiScript* script)
{
  if (!script)
  {
    return mi_rt_make_void();
  }

  mi_fold_constants_ast(rt, script);

  MiRtValue last = mi_rt_make_void();
  const MiCommandList* it = script->first;
  while (it)
  {
    last = s_eval_command_node(rt, it->command);
    it = it->next;
  }

  return last;
}

//----------------------------------------------------------
// Constant folding (AST-only)
//----------------------------------------------------------

static void s_fold_replace_with_literal(MiExpr* expr, MiRtValue v)
{
  if (!expr)
  {
    return;
  }

  switch (v.kind)
  {
    case MI_RT_VAL_INT:
      expr->kind = MI_EXPR_INT_LITERAL;
      expr->as.int_lit.value = (long long)v.as.i;
      break;
    case MI_RT_VAL_FLOAT:
      expr->kind = MI_EXPR_FLOAT_LITERAL;
      expr->as.float_lit.value = v.as.f;
      break;
    case MI_RT_VAL_BOOL:
      expr->kind = MI_EXPR_BOOL_LITERAL;
      expr->as.bool_lit.value = v.as.b;
      break;
    case MI_RT_VAL_STRING:
      expr->kind = MI_EXPR_STRING_LITERAL;
      expr->as.string_lit.value = v.as.s;
      break;
    case MI_RT_VAL_VOID:
      expr->kind = MI_EXPR_VOID_LITERAL;
      break;
    default:
      break;
  }
}

static void s_fold_command(MiRuntime* rt, MiCommand* cmd)
{
  if (!cmd)
  {
    return;
  }

  if (cmd->head)
  {
    s_fold_expr(rt, cmd->head);
  }

  MiExprList* it = cmd->args;
  while (it)
  {
    s_fold_expr(rt, it->expr);
    it = it->next;
  }
}

static void s_fold_expr(MiRuntime* rt, MiExpr* expr)
{
  if (!rt || !expr)
  {
    return;
  }

  if (expr->kind == MI_EXPR_INT_LITERAL
      || expr->kind == MI_EXPR_FLOAT_LITERAL
      || expr->kind == MI_EXPR_STRING_LITERAL
      || expr->kind == MI_EXPR_BOOL_LITERAL
      || expr->kind == MI_EXPR_VOID_LITERAL)
  {
    return;
  }

  // Recurse first so we can fold constant sub-expressions even when the parent
  // expression is not fully foldable (e.g. ($x + (1 + 2))).
  if (expr->kind == MI_EXPR_BINARY)
  {
    s_fold_expr(rt, expr->as.binary.left);
    s_fold_expr(rt, expr->as.binary.right);
  }
  else if (expr->kind == MI_EXPR_UNARY)
  {
    s_fold_expr(rt, expr->as.unary.expr);
  }
  else if (expr->kind == MI_EXPR_INDEX)
  {
    s_fold_expr(rt, expr->as.index.target);
    s_fold_expr(rt, expr->as.index.index);
  }
  else if (expr->kind == MI_EXPR_COMMAND)
  {
    // Fold within the command head/arguments, but never evaluate the command
    // itself during constant folding. Commands are runtime.
    if (expr->as.command.head)
    {
      s_fold_expr(rt, expr->as.command.head);
    }

    MiExprList* it = expr->as.command.args;
    while (it)
    {
      s_fold_expr(rt, it->expr);
      it = it->next;
    }

    return;
  }

  else if (expr->kind == MI_EXPR_LIST)
  {
    MiExprList* it = expr->as.list.items;
    while (it)
    {
      s_fold_expr(rt, it->expr);
      it = it->next;
    }
  }
  else if (expr->kind == MI_EXPR_PAIR)
  {
    s_fold_expr(rt, expr->as.pair.key);
    s_fold_expr(rt, expr->as.pair.value);
  }
  else if (expr->kind == MI_EXPR_BLOCK)
  {
    s_fold_program(expr->as.block.script);
    return;
  }
  else if (expr->kind == MI_EXPR_VAR)
  {
    if (expr->as.var.is_indirect)
    {
      s_fold_expr(rt, expr->as.var.name_expr);
    }
  }

  // Only fold expressions that are marked foldable by the parser. This avoids
  // evaluating variables or commands during folding, and keeps folding as a
  // pure constant simplification pass.
  if (!expr->can_fold)
  {
    return;
  }

  // Only scalar-producing expressions are replaced with literals. Folding list/
  // pair construction would allocate containers; we deliberately avoid that for
  // now.
  if (expr->kind != MI_EXPR_BINARY
      && expr->kind != MI_EXPR_UNARY
      && expr->kind != MI_EXPR_INDEX)
  {
    return;
  }

  MiRtValue v = mi_eval_expr_ast(rt, expr);
  s_fold_replace_with_literal(expr, v);
}

static void s_fold_program(const MiScript* script)
{
  if (!script)
  {
    return;
  }

  MiRuntime tmp;
  mi_rt_init(&tmp);
  mi_ast_backend_bind(&tmp);

  // We do not register builtins because we do not actuall execute code
  //mi_cmd_register_builtins(&tmp);

  const MiCommandList* it = script->first;
  while (it)
  {
    s_fold_command(&tmp, it->command);
    it = it->next;
  }

  mi_rt_shutdown(&tmp);
}

void mi_fold_constants_ast(MiRuntime* rt, const MiScript* script)
{
  (void)rt;
  s_fold_program(script);
}
