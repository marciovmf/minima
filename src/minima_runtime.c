#include "minima_runtime.h"
#include "minima.h"
#include <stdx_string.h>
#include <stdx_arena.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN64
typedef __int64 ssize_t;
#else
typedef long ssize_t;
#endif

//----------------------------------------------------------
// Forwards
//----------------------------------------------------------

static MiRtBuiltinFn s_find_command(MiRuntime *rt, XSlice name);
static MiRtValue mi_rt_eval_command_expr(MiRuntime *rt, const MiExpr *expr);
static MiRtVar* s_rt_var_find_in_frame(const MiScopeFrame* frame, XSlice name);
static MiRtVar* s_rt_var_find_nearest(MiRuntime* rt, XSlice name, MiScopeFrame** out_owner);
static MiRtValue s_eval_script_scoped(MiRuntime* rt, const MiScript* script);

//----------------------------------------------------------
// Internal helpers
//----------------------------------------------------------

static void* s_realloc(void *ptr, size_t size)
{
  if (size == 0u)
  {
    free(ptr);
    return NULL;
  }

  void *p = realloc(ptr, size);
  if (!p)
  {
    mi_error("minima_runtime: out of memory\n");
    exit(1);
  }
  return p;
}

static void s_rt_scope_init(MiScopeFrame* f, size_t chunk_size, MiScopeFrame* parent)
{
  f->arena = x_arena_create(chunk_size);
  if (!f->arena)
  {
    mi_error("minima_runtime: out of memory\n");
    exit(1);
  }
  f->vars = NULL;
  f->parent = parent;
  f->next_free = NULL;
}

static MiRtVar* s_rt_var_find_in_frame(const MiScopeFrame* frame, XSlice name)
{
  if (!frame)
  {
    return NULL;
  }

  MiRtVar* it = frame->vars;
  while (it)
  {
    if (x_slice_eq(it->name, name))
    {
      return it;
    }
    it = it->next;
  }

  return NULL;
}

static MiRtVar* s_rt_var_find_nearest(MiRuntime* rt, XSlice name, MiScopeFrame** out_owner)
{
  if (!rt)
  {
    return NULL;
  }

  MiScopeFrame* f = rt->current;
  while (f)
  {
    MiRtVar* v = s_rt_var_find_in_frame(f, name);
    if (v)
    {
      if (out_owner)
      {
        *out_owner = f;
      }
      return v;
    }
    f = f->parent;
  }

  return NULL;
}

static MiRtValue s_eval_script_scoped(MiRuntime* rt, const MiScript* script)
{
  mi_rt_scope_push(rt);
  MiRtValue v = mi_rt_eval_script(rt, script);
  mi_rt_scope_pop(rt);
  return v;
}

//----------------------------------------------------------
// Expression evaluation 
//----------------------------------------------------------

static MiRtValue s_eval_binary_string(MiTokenKind op, const MiRtValue *a, const MiRtValue *b)
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
  else {
    mi_error("unsupported string binary operator\n");
    return mi_rt_make_void();
  }

}

static MiRtValue s_eval_binary_numeric(MiTokenKind op, const MiRtValue *a, const MiRtValue *b)
{
  // Promote to float if needed.
  bool is_float = (a->kind == MI_RT_VAL_FLOAT) || (b->kind == MI_RT_VAL_FLOAT);
  if (!is_float &&
      a->kind != MI_RT_VAL_INT &&
      b->kind != MI_RT_VAL_INT)
  {
    mi_error("binary operator: expected numeric operands\n");
    return mi_rt_make_void();
  }

  double da = 0.0;
  double db = 0.0;

  if (is_float)
  {
    da = (a->kind == MI_RT_VAL_FLOAT) ? a->as.f : (double) a->as.i;
    db = (b->kind == MI_RT_VAL_FLOAT) ? b->as.f : (double) b->as.i;
  }
  else
  {
    da = (double) a->as.i;
    db = (double) b->as.i;
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

MiRtValue mi_rt_eval_expr(MiRuntime *rt, const MiExpr *expr)
{
  if (!expr)
  {
    return mi_rt_make_void();
  }

  switch (expr->kind)
  {
    case MI_EXPR_INT_LITERAL:
      return mi_rt_make_int((long long) expr->as.int_lit.value);

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
          MiRtValue name_v = mi_rt_eval_expr(rt, expr->as.var.name_expr);
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
        MiRtValue base  = mi_rt_eval_expr(rt, expr->as.index.target);
        MiRtValue index = mi_rt_eval_expr(rt, expr->as.index.index);

        if (index.kind != MI_RT_VAL_INT)
        {
          mi_error("list index must be integer\n");
          return mi_rt_make_void();
        }

        if (base.kind == MI_RT_VAL_PAIR)// 
        {
          long long i = index.as.i;
          if (i < 0 || i > 1)
          {
            mi_error("pair index out of range\n");
            return mi_rt_make_void();
          }
          return base.as.pair->items[(size_t) i];
        }
        else if (base.kind == MI_RT_VAL_LIST) // || !base.as.list
        {
          long long i = index.as.i;
          if (i < 0 || (size_t) i >= base.as.list->count)
          {
            mi_error("list index out of range\n");
            return mi_rt_make_void();
          }
          return base.as.list->items[(size_t) i];
        }
        else
        {
          mi_error("indexing non-list or pair value\n");
          return mi_rt_make_void();
        }

      }

    case MI_EXPR_UNARY:
      {
        MiRtValue v = mi_rt_eval_expr(rt, expr->as.unary.expr);
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

            mi_error("unary - expects numeric operand\n");
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
            {
              bool b;
              if (v.kind == MI_RT_VAL_BOOL)
              {
                b = v.as.b;
              }
              else
              {
                mi_error("not expects bool operand\n");
                return mi_rt_make_void();
              }
              return mi_rt_make_bool(!b);
            }

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
          MiRtValue left  = mi_rt_eval_expr(rt, expr->as.binary.left);
          MiRtValue right = mi_rt_eval_expr(rt, expr->as.binary.right);

          if (left.kind != MI_RT_VAL_BOOL || right.kind != MI_RT_VAL_BOOL)
          {
            mi_error("and/or expect bool operands\n");
            return mi_rt_make_void();
          }

          if (op == MI_TOK_AND)
          {
            return mi_rt_make_bool(left.as.b && right.as.b);
          }
          else
          {
            return mi_rt_make_bool(left.as.b || right.as.b);
          }
        }

        // Numeric comparators and arithmetic
        MiRtValue left  = mi_rt_eval_expr(rt, expr->as.binary.left);
        MiRtValue right = mi_rt_eval_expr(rt, expr->as.binary.right);
        if (left.kind == MI_RT_VAL_STRING && right.kind == MI_RT_VAL_STRING)
          return s_eval_binary_string(op, &left, &right);
        else
          return s_eval_binary_numeric(op, &left, &right);
      }

    case MI_EXPR_PAIR:
      {
        MiRtPair *pair = mi_rt_pair_create();
        pair->items[0] = mi_rt_eval_expr(rt, expr->as.pair.key);
        pair->items[1] = mi_rt_eval_expr(rt, expr->as.pair.value);
        return mi_rt_make_pair(pair);
      }

    case MI_EXPR_LIST:
      {
        MiRtList *list = mi_rt_list_create(rt);
        MiExprList *it = expr->as.list.items;
        while (it)
        {
          MiRtValue v = mi_rt_eval_expr(rt, it->expr);
          mi_rt_list_push(list, v);
          it = it->next;
        }
        return mi_rt_make_list(list);
      }

    case MI_EXPR_DICT:
      mi_error("dicts and pairs not supported yet\n");
      return mi_rt_make_void();

    case MI_EXPR_BLOCK:
      {
        MiRtValue v;
        v.kind       = MI_RT_VAL_BLOCK;
        v.as.block   = expr->as.block.script;
        return v;
      }

    case MI_EXPR_COMMAND:
      return mi_rt_eval_command_expr(rt, expr);

    default:
      mi_error("unknown expression kind\n");
      return mi_rt_make_void();
  }
}

/* COMMAND-EXPR: head_expr :: arg_expr* */
static MiRtValue mi_rt_eval_command_expr(MiRuntime *rt, const MiExpr *expr)
{
  MiRtValue     head_val;
  XSlice        head_name;
  MiRtBuiltinFn fn;

  if (!expr || expr->kind != MI_EXPR_COMMAND)
  {
    return mi_rt_make_void();
  }

  head_val = mi_rt_eval_expr(rt, expr->as.command.head);
  if (head_val.kind != MI_RT_VAL_STRING)
  {
    mi_error("command head is not a string\n");
    return mi_rt_make_void();
  }

  head_name = head_val.as.s;

  fn = s_find_command(rt, head_name);
  if (!fn)
  {
    mi_error_fmt("unknown command: %.*s\n",
        (int) head_name.length, head_name.ptr);
    return mi_rt_make_void();
  }

  return fn(rt, &head_name, expr->as.command.argc, expr->as.command.args);
}

static MiRtValue mi_rt_eval_command_node(MiRuntime *rt, const MiCommand *cmd)
{
  if (!cmd || !cmd->head)
  {
    return mi_rt_make_void();
  }

  // Wrap command node as MI_EXPR_COMMAND and reuse eval.
  MiExpr expr;
  memset(&expr, 0, sizeof(expr));
  expr.kind             = MI_EXPR_COMMAND;
  expr.token            = cmd->head->token;
  expr.as.command.head  = cmd->head;
  expr.as.command.args  = cmd->args;
  expr.as.command.argc  = cmd->argc;

  return mi_rt_eval_command_expr(rt, &expr);
}


//----------------------------------------------------------
// Variable table (scoped)
//----------------------------------------------------------

bool mi_rt_var_set(MiRuntime *rt, XSlice name, MiRtValue value)
{
  if (!rt || !rt->current)
  {
    return false;
  }

  MiScopeFrame* owner = NULL;
  MiRtVar* existing = s_rt_var_find_nearest(rt, name, &owner);
  if (existing)
  {
    existing->value = value;
    return true;
  }

  MiRtVar* v = (MiRtVar*) x_arena_alloc(rt->current->arena, sizeof(MiRtVar));
  if (!v)
  {
    mi_error("minima_runtime: out of memory\n");
    exit(1);
  }

  v->name = name;
  v->value = value;
  v->next = rt->current->vars;
  rt->current->vars = v;
  return true;
}


//----------------------------------------------------------
// Builtin commands
//----------------------------------------------------------

typedef struct MiRtBuiltin
{
  const char   *name;
  MiRtBuiltinFn fn;
} MiRtBuiltin;

static MiRtValue s_cmd_set(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args);
static MiRtValue s_cmd_print(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args);
static MiRtValue s_cmd_list(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args);
static MiRtValue s_cmd_if(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args);
static MiRtValue s_cmd_while(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args);
static MiRtValue s_cmd_foreach(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args);
static MiRtValue s_cmd_call(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args);
static MiRtValue s_cmd_range(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args);
static MiRtValue s_cmd_typeof(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args);

static const MiRtBuiltin s_builtins[] =
{
  {"set",   s_cmd_set},
  {"print", s_cmd_print},
  {"list",  s_cmd_list},
  {"if",    s_cmd_if},
  {"while", s_cmd_while },
  {"foreach", s_cmd_foreach },
  {"call", s_cmd_call },
  {"range", s_cmd_range },
  {"typeof", s_cmd_typeof }
};

static const size_t s_builtin_count = sizeof(s_builtins) / sizeof(s_builtins[0]);

static MiRtBuiltinFn s_find_command(MiRuntime *rt, XSlice name)
{
  size_t i;

  // User-registered commands first.
  for (i = 0u; i < rt->command_count; ++i)
  {
    if (x_slice_eq(name, rt->commands[i].name))
    {
      return rt->commands[i].fn;
    }
  }

  // Then builtins.
  for (i = 0u; i < s_builtin_count; ++i)
  {
    if (x_slice_eq_cstr(name, s_builtins[i].name))
    {
      return s_builtins[i].fn;
    }
  }

  return NULL;
}

/* set :: name value? */
static MiRtValue s_cmd_set(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args)
{
  (void) head_name;
  (void) argc;

  if (!args || !args->expr)
  {
    mi_error("set: missing variable name\n");
    return mi_rt_make_void();
  }

  // First argument: variable name (must evaluate to string).
  MiRtValue name_val = mi_rt_eval_expr(rt, args->expr);
  if (name_val.kind != MI_RT_VAL_STRING)
  {
    mi_error("set: first argument must be a string (variable name)\n");
    return mi_rt_make_void();
  }

  XSlice name = name_val.as.s;
  MiRtValue value = mi_rt_make_void();

  // Optional second argument: value expression.
  if (args->next && args->next->expr)
  {
    value = mi_rt_eval_expr(rt, args->next->expr);
  }

  mi_rt_var_set(rt, name, value);
  return value;
}

static void s_print_value(FILE *out, const MiRtValue *v)
{
  switch (v->kind)
  {
    case MI_RT_VAL_VOID:
      fprintf(out, "void");
      break;

    case MI_RT_VAL_INT:
      fprintf(out, "%lld", v->as.i);
      break;

    case MI_RT_VAL_FLOAT:
      fprintf(out, "%f", v->as.f);
      break;

    case MI_RT_VAL_BOOL:
      fprintf(out, "%s", v->as.b ? "true" : "false");
      break;

    case MI_RT_VAL_STRING:
      fwrite(v->as.s.ptr, 1u, v->as.s.length, out);
      break;

    case MI_RT_VAL_LIST:
      {
        fprintf(out, "[");
        if (v->as.list)
        {
          size_t i;
          for (i = 0u; i < v->as.list->count; ++i)
          {
            if (i > 0u)
            {
              fprintf(out, ", ");
            }
            s_print_value(out, &v->as.list->items[i]);
          }
        }
        fprintf(out, "]");
        break;
      }

    case MI_RT_VAL_PAIR:

      fprintf(out, "(");
      s_print_value(out, &v->as.pair->items[0]);
      fprintf(out, ":");
      s_print_value(out, &v->as.pair->items[1]);
      fprintf(out, ")");
      break;

    case MI_RT_VAL_BLOCK:
      fprintf(out, "<block>");
      break;

    default:
      fprintf(out, "<unknown>");
      break;
  }
}

static MiRtValue s_cmd_print(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args)
{
  (void) rt;
  (void) head_name;
  (void) argc;

  MiExprList *it   = args;
  bool        first = true;

  while (it && it->expr)
  {
    MiRtValue v = mi_rt_eval_expr(rt, it->expr);
    if (!first)
    {
      fputc(' ', stdout);
    }
    s_print_value(stdout, &v);
    first = false;
    it    = it->next;
  }

  //fputc('\n', stdout);
  return mi_rt_make_void();
}

static MiRtValue s_cmd_if(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args)
{
  (void) argc;
  MiExprList *it = args;
  (void) head_name;

  if (!it)
  {
    mi_error("if: missing condition and block\n");
    return mi_rt_make_void();
  }

  /*
   * Args pattern:
   *   cond1, block1,
   *   ["elseif", condN, blockN, ...]
   *   ["else",   blockElse]
   */
  while (it)
  {
    MiExpr     *cond_expr = it->expr;
    MiRtValue   cond_val;
    MiScript   *then_script;

    it = it->next;
    if (!it || !it->expr || it->expr->kind != MI_EXPR_BLOCK)
    {
      mi_error("if: expected block after condition\n");
      return mi_rt_make_void();
    }

    then_script = it->expr->as.block.script;

    cond_val = mi_rt_eval_expr(rt, cond_expr);
    if (cond_val.kind != MI_RT_VAL_BOOL)
    {
      mi_error("if: condition must be boolean\n");
      return mi_rt_make_void();
    }

    if (cond_val.as.b)
    {
      // Execute then-block and stop.
      return s_eval_script_scoped(rt, then_script);
    }

    // Condition was false, try elseif/else.
    it = it->next;
    if (!it)
    {
      return mi_rt_make_void();
    }

    if (!it->expr)
    {
      return mi_rt_make_void();
    }

    if (it->expr->kind == MI_EXPR_STRING_LITERAL)
    {
      XSlice kw = it->expr->as.string_lit.value;

      if (x_slice_eq_cstr(kw, "else"))
      {
        it = it->next;
        if (!it || !it->expr || it->expr->kind != MI_EXPR_BLOCK)
        {
          mi_error("if: expected block after else\n");
          return mi_rt_make_void();
        }

        return s_eval_script_scoped(rt, it->expr->as.block.script);
      }

      if (x_slice_eq_cstr(kw, "elseif"))
      {
        it = it->next;
        if (!it)
        {
          mi_error("if: missing condition after elseif\n");
          return mi_rt_make_void();
        }

        // Next loop iteration will treat this as a new cond.
        continue;
      }

      mi_error_fmt("if: unknown keyword '%.*s'\n",
          (int) kw.length, kw.ptr);
      return mi_rt_make_void();
    }

    // Not a keyword: stop processing.
    return mi_rt_make_void();
  }

  return mi_rt_make_void();
}

static MiRtValue s_cmd_while(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args) 
{
  (void) argc;
  MiExprList *cond_node = args;
  MiExprList *body_node = NULL;
  MiRtValue   last      = mi_rt_make_void();

  (void) head_name;

  if (!cond_node)
  {
    mi_error("while: missing condition and body\n");
    return mi_rt_make_void();
  }

  body_node = cond_node->next;
  if (!body_node)
  {
    mi_error("while: missing body block\n");
    return mi_rt_make_void();
  }

  if (!body_node->expr || body_node->expr->kind != MI_EXPR_BLOCK)
  {
    mi_error("while: body must be a block\n");
    return mi_rt_make_void();
  }

  {
    MiScript *body_script = body_node->expr->as.block.script;

    if (cond_node->expr && cond_node->expr->kind == MI_EXPR_BLOCK)
    {
      // while :: { cond-script } { body }
      MiScript *cond_script = cond_node->expr->as.block.script;
      for (;;)
      {
        MiRtValue cond_val = s_eval_script_scoped(rt, cond_script);
        if (cond_val.kind != MI_RT_VAL_BOOL)
        {
          mi_error("while: condition block must yield bool\n");
          return mi_rt_make_void();
        }
        if (!cond_val.as.b)
        {
          break;
        }
        last = s_eval_script_scoped(rt, body_script);
      }
    }
    else
    {
      // while :: (cond-expr) { body }
      MiExpr *cond_expr = cond_node->expr;
      for (;;)
      {
        MiRtValue cond_val = mi_rt_eval_expr(rt, cond_expr);
        if (cond_val.kind != MI_RT_VAL_BOOL)
        {
          mi_error("while: condition must be boolean\n");
          return mi_rt_make_void();
        }
        if (!cond_val.as.b)
        {
          break;
        }
        last = s_eval_script_scoped(rt, body_script);
      }
    }
  }

  return last;
}

static MiRtValue s_cmd_foreach(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args) 
{

  (void) head_name;
  if (argc != 3)
  {
    mi_error_fmt("while: Expected 3 arguments but found %d\n", argc);
    return mi_rt_make_void();
  }

  MiRtValue var_name = mi_rt_eval_expr(rt, args->expr);
  MiRtValue iterable = mi_rt_eval_expr(rt, args->next->expr);
  MiRtValue body = mi_rt_eval_expr(rt, args->next->next->expr);

  { // Validations

    if (var_name.kind != MI_RT_VAL_STRING)
    {
      mi_error("while: expected variable name \n");
      return mi_rt_make_void();
    }

    if (iterable.kind != MI_RT_VAL_LIST)
    {
      mi_error("while: expected list\n");
      return mi_rt_make_void();
    }

    if (body.kind != MI_RT_VAL_BLOCK)
    {
      mi_error("while: expected code block\n");
      return mi_rt_make_void();
    }
  }

  MiRtValue last = mi_rt_make_void();
  { // Execution

    mi_rt_scope_push(rt);

    for (unsigned int i = 0; i < iterable.as.list->count; i++)
    {
      /* No shadowing: if the iterator exists in a parent scope, this updates it.
         Otherwise it is created in this foreach scope and will be discarded on pop. */
      mi_rt_var_set(rt, var_name.as.s, iterable.as.list->items[(size_t) i]);
      last = s_eval_script_scoped(rt, body.as.block);
    }

    mi_rt_scope_pop(rt);
  }
  return last;
}

static MiRtValue s_cmd_call(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList   *args)
{
  (void) argc;
  (void) head_name;

  MiRtValue result = mi_rt_make_void();

  if (!args || !args->expr)
  {
    mi_error("call: missing block argument\n");
    return result;
  }

  // Avalia o primeiro argumento
  MiRtValue blk = mi_rt_eval_expr(rt, args->expr);

  if (blk.kind != MI_RT_VAL_BLOCK || blk.as.block == NULL)
  {
    mi_error("call: argument is not a block\n");
    return result;
  }

  // Executa o bloco
  return s_eval_script_scoped(rt, blk.as.block);
}

static MiRtValue s_cmd_range(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList   *args)
{
  (void) head_name;

  if (argc != 2)
  {
    mi_error_fmt("range: Expected 2 arguments but found %d\n", argc);
    return mi_rt_make_void();
  }

  MiRtValue start = mi_rt_eval_expr(rt, args->expr);
  MiRtValue end = mi_rt_eval_expr(rt, args->next->expr);

  if (start.kind != MI_RT_VAL_INT || end.kind != MI_RT_VAL_INT)
  {
    mi_error("range: start and end must be integers\n");
  }

  MiRtList* list = mi_rt_list_create(rt);
  if (start.as.i < end.as.i)
  {
    for (long long i = start.as.i; i <= end.as.i; i++)
    {
      mi_rt_list_push(list, mi_rt_make_int(i));
    }
  }
  if (start.as.i > end.as.i)
  {
    for (long long i = start.as.i; i >= end.as.i; i--)
    {
      mi_rt_list_push(list, mi_rt_make_int(i));
    }
  }

  return mi_rt_make_list(list);
}

static MiRtValue s_cmd_typeof(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList   *args)
{
  (void) head_name;

  if (argc != 1)
  {
    mi_error_fmt("typeof: Expected 1 arguments but found %d\n", argc);
    return mi_rt_make_void();
  }

  MiRtValue value = mi_rt_eval_expr(rt, args->expr);
  switch (value.kind)
  {
    case MI_RT_VAL_INT:
      return mi_rt_make_string_slice(x_slice("int"));
    case MI_RT_VAL_FLOAT:
      return mi_rt_make_string_slice(x_slice("float"));
    case MI_RT_VAL_BOOL:
      return mi_rt_make_string_slice(x_slice("bool"));
    case MI_RT_VAL_STRING:
      return mi_rt_make_string_slice(x_slice("string"));
    case MI_RT_VAL_LIST:
      return mi_rt_make_string_slice(x_slice("list"));
    case MI_RT_VAL_PAIR:
      return mi_rt_make_string_slice(x_slice("pair"));
    case MI_RT_VAL_BLOCK:
      return mi_rt_make_string_slice(x_slice("block"));
    case MI_RT_VAL_VOID:
      return mi_rt_make_string_slice(x_slice("void"));
  }

  return mi_rt_make_string_slice(x_slice("void"));
}

/* list :: (len list) | (append list value) */
static MiRtValue s_cmd_list(MiRuntime *rt, const XSlice *head_name, int argc, MiExprList *args)
{
  (void) rt;
  (void) head_name;
  (void) argc;

  if (!args || !args->expr)
  {
    mi_error("list: expected subcommand 'len' or 'append'\n");
    return mi_rt_make_void();
  }

  // First argument is the subcommand name.
  MiRtValue sub_val = mi_rt_eval_expr(rt, args->expr);
  if (sub_val.kind != MI_RT_VAL_STRING)
  {
    mi_error("list: expected subcommand 'len' or 'append'\n");
    return mi_rt_make_void();
  }

  XSlice sub = sub_val.as.s;

  if (x_slice_eq_cstr(sub, "len"))
  {
    MiExprList *list_arg = args->next;
    if (!list_arg || !list_arg->expr)
    {
      mi_error("list len: expected one list argument\n");
      return mi_rt_make_void();
    }

    MiRtValue list_val = mi_rt_eval_expr(rt, list_arg->expr);
    if (list_val.kind != MI_RT_VAL_LIST || !list_val.as.list)
    {
      mi_error("list len: argument must be a list\n");
      return mi_rt_make_void();
    }

    // MiRtList is opaque here, but we assume it has a count field.
    long long len = (long long) list_val.as.list->count;
    return mi_rt_make_int(len);
  }

  if (x_slice_eq_cstr(sub, "append"))
  {
    MiExprList *list_arg  = args->next;
    MiExprList *value_arg = list_arg ? list_arg->next : NULL;

    if (!list_arg || !list_arg->expr || !value_arg || !value_arg->expr)
    {
      mi_error("list append: expected list and value\n");
      return mi_rt_make_void();
    }

    MiRtValue list_val  = mi_rt_eval_expr(rt, list_arg->expr);
    MiRtValue value_val = mi_rt_eval_expr(rt, value_arg->expr);

    if (list_val.kind != MI_RT_VAL_LIST || !list_val.as.list)
    {
      mi_error("list append: first arg must be a list\n");
      return mi_rt_make_void();
    }

    mi_rt_list_push(list_val.as.list, value_val);
    return mi_rt_make_void();
  }

  mi_error("list: unknown subcommand\n");
  return mi_rt_make_void();
}


//----------------------------------------------------------
// Constant folding
//----------------------------------------------------------

static void s_fold_script(MiRuntime *rt, MiScript *script);
static void s_fold_command(MiRuntime *rt, MiCommand *cmd);
static void s_fold_expr(MiRuntime *rt, MiExpr *expr);

/* Converts a MiRtValue in literal, in-place. */
static void s_fold_replace_with_literal(MiExpr *expr, MiRtValue v)
{
  expr->can_fold = true;

  switch (v.kind)
  {
    case MI_RT_VAL_INT:
      {
        expr->kind = MI_EXPR_INT_LITERAL;
        expr->as.int_lit.value = (int64_t) v.as.i;
      } break;

    case MI_RT_VAL_FLOAT:
      {
        expr->kind = MI_EXPR_FLOAT_LITERAL;
        expr->as.float_lit.value = v.as.f;
      } break;

    case MI_RT_VAL_BOOL:
      {
        expr->kind = MI_EXPR_BOOL_LITERAL;
        expr->as.bool_lit.value = v.as.b;
      } break;

    case MI_RT_VAL_STRING:
      {
        expr->kind = MI_EXPR_STRING_LITERAL;
        expr->as.string_lit.value = v.as.s;
      } break;

    case MI_RT_VAL_VOID:
      {
        expr->kind = MI_EXPR_VOID_LITERAL;
      } break;

    case MI_RT_VAL_LIST:
    case MI_RT_VAL_BLOCK:
    default:
      break;
  }
}

/* Recursive visitor for expressions. */
static void s_fold_expr(MiRuntime *rt, MiExpr *expr)
{
  if (!expr)
  {
    return;
  }

  // Recursively fold each child node, even if this node can not be folded.
  switch (expr->kind)
  {
    case MI_EXPR_INT_LITERAL:
    case MI_EXPR_FLOAT_LITERAL:
    case MI_EXPR_STRING_LITERAL:
    case MI_EXPR_BOOL_LITERAL:
    case MI_EXPR_VOID_LITERAL: // Literals has no children
    case MI_EXPR_VAR:          // Var has no children
      break;

    case MI_EXPR_INDEX:
      {
        s_fold_expr(rt, expr->as.index.target);
        s_fold_expr(rt, expr->as.index.index);
      } break;

    case MI_EXPR_UNARY:
      {
        s_fold_expr(rt, expr->as.unary.expr);
      } break;

    case MI_EXPR_BINARY:
      {
        s_fold_expr(rt, expr->as.binary.left);
        s_fold_expr(rt, expr->as.binary.right);
      } break;

    case MI_EXPR_LIST:
      {
        MiExprList *it = expr->as.list.items;
        while (it)
        {
          s_fold_expr(rt, it->expr);
          it = it->next;
        }
      } break;

    case MI_EXPR_DICT:
      {
        MiExprList *it = expr->as.dict.items;
        while (it)
        {
          /* Cada item de dict é MI_EXPR_PAIR */
          s_fold_expr(rt, it->expr);
          it = it->next;
        }
      } break;

    case MI_EXPR_PAIR:
      {
        s_fold_expr(rt, expr->as.pair.key);
        s_fold_expr(rt, expr->as.pair.value);
      } break;

    case MI_EXPR_BLOCK:
      {
        /* Fold recursivo dentro do bloco (script interno). */
        if (expr->as.block.script)
        {
          s_fold_script(rt, expr->as.block.script);
        }
      } break;

    case MI_EXPR_COMMAND:
      {
        /* Comando dentro de expressão: fold no head e nos args. */
        s_fold_expr(rt, expr->as.command.head);

        MiExprList *it = expr->as.command.args;
        while (it)
        {
          s_fold_expr(rt, it->expr);
          it = it->next;
        }
      } break;

    default:
      {
        /* Caso de segurança; idealmente não cai aqui. */
      } break;
  }

  // If the current node is foldable, try evaluate it and replace it by a litera.
  if (!expr->can_fold)
  {
    return;
  }

  // Literals are alerady the maximum folding level.
  // And commands should not run during folding.
  if (expr->kind == MI_EXPR_INT_LITERAL
      || expr->kind == MI_EXPR_FLOAT_LITERAL
      || expr->kind == MI_EXPR_STRING_LITERAL
      || expr->kind == MI_EXPR_BOOL_LITERAL
      || expr->kind == MI_EXPR_COMMAND
      || expr->kind == MI_EXPR_VOID_LITERAL)
  {
    return;
  }

  MiRtValue v = mi_rt_eval_expr(rt, expr);  // This should be a math/logical expression!
  s_fold_replace_with_literal(expr, v);

  return;
}

/* Constant fold a command (head + args). */
static void s_fold_command(MiRuntime *rt, MiCommand *cmd)
{
  if (!cmd)
  {
    return;
  }

  if (cmd->head)
  {
    s_fold_expr(rt, cmd->head);
  }

  MiExprList *it = cmd->args;
  while (it)
  {
    s_fold_expr(rt, it->expr);
    it = it->next;
  }
}

/* Constant fold a script */
static void s_fold_script(MiRuntime *rt, MiScript *script)
{
  if (!script)
  {
    return;
  }

  MiCommandList *it = script->first;
  while (it)
  {
    s_fold_command(rt, it->command);
    it = it->next;
  }
}

/* Constant fold a script */
static void s_fold_program(const MiScript *script)
{
  if (!script)
  {
    return;
  }

  MiRuntime rt;
  mi_rt_init(&rt);
  s_fold_script(&rt, (MiScript *) script);
  mi_rt_shutdown(&rt);
}


//----------------------------------------------------------
// Public API
//----------------------------------------------------------

void mi_rt_init(MiRuntime *rt)
{
  if (!rt)
  {
    return;
  }

  rt->scope_chunk_size = 16u * 1024u;

  rt->root.arena = x_arena_create(64u * 1024u);
  if (!rt->root.arena)
  {
    mi_error("minima_runtime: out of memory\n");
    exit(1);
  }
  rt->root.vars = NULL;
  rt->root.parent = NULL;
  rt->root.next_free = NULL;
  rt->current = &rt->root;

  rt->free_frames = NULL;

  rt->commands         = NULL;
  rt->command_count    = 0u;
  rt->command_capacity = 0u;
}

void mi_rt_shutdown(MiRuntime *rt)
{
  size_t i;

  if (!rt)
  {
    return;
  }

  while (rt->current && rt->current != &rt->root)
  {
    mi_rt_scope_pop(rt);
  }

  while (rt->free_frames)
  {
    MiScopeFrame* f = rt->free_frames;
    rt->free_frames = f->next_free;

    x_arena_destroy(f->arena);
    free(f);
  }

  x_arena_destroy(rt->root.arena);
  rt->root.arena = NULL;
  rt->root.vars = NULL;
  rt->root.parent = NULL;
  rt->current = NULL;

  if (rt->commands)
  {
    for (i = 0u; i < rt->command_count; ++i)
    {
      if (rt->commands[i].name.ptr)
      {
        free((void*) rt->commands[i].name.ptr);
      }
    }
    free(rt->commands);
  }
  rt->commands         = NULL;
  rt->command_count    = 0u;
  rt->command_capacity = 0u;
}

bool mi_rt_register_command(MiRuntime *rt, const char *name, MiRtBuiltinFn fn)
{
  size_t len;
  size_t i;
  char  *name_copy;

  if (!rt || !name || !fn)
  {
    return false;
  }

  len = strlen(name);

  /* Update existing command if found. */
  for (i = 0u; i < rt->command_count; ++i)
  {
    XSlice s = rt->commands[i].name;
    if (s.length == len &&
        (len == 0u || memcmp(s.ptr, name, len) == 0))
    {
      rt->commands[i].fn = fn;
      return true;
    }
  }

  name_copy = NULL;
  if (len > 0u)
  {
    name_copy = (char*) s_realloc(NULL, len);
    memcpy(name_copy, name, len);
  }

  if (rt->command_count == rt->command_capacity)
  {
    size_t new_cap = (rt->command_capacity == 0u) ? 4u : (rt->command_capacity * 2u);
    MiRtUserCommand *new_cmds = (MiRtUserCommand*) s_realloc(
        rt->commands,
        new_cap * sizeof(MiRtUserCommand)
        );
    rt->commands         = new_cmds;
    rt->command_capacity = new_cap;
  }

  rt->commands[rt->command_count].name.ptr    = name_copy;
  rt->commands[rt->command_count].name.length = len;
  rt->commands[rt->command_count].fn          = fn;
  rt->command_count                            = rt->command_count + 1u;

  return true;
}

MiRtValue mi_rt_eval_script(MiRuntime *rt, const MiScript *script)
{
  if (!script)
  {
    return mi_rt_make_void();
  }

  // constant fold AST
  s_fold_program((MiScript *) script);

  MiRtValue last = mi_rt_make_void();
  const MiCommandList *it = script->first;
  while (it)
  {
    last = mi_rt_eval_command_node(rt, it->command);
    it   = it->next;
  }

  return last;
}

bool mi_rt_var_get(const MiRuntime *rt, XSlice name, MiRtValue *out_value)
{
  if (!rt)
  {
    return false;
  }

  const MiScopeFrame* f = rt->current;
  while (f)
  {
    MiRtVar* v = s_rt_var_find_in_frame(f, name);
    if (v)
    {
      if (out_value)
      {
        *out_value = v->value;
      }
      return true;
    }
    f = f->parent;
  }

  return false;
}

MiRtValue mi_rt_make_void(void)
{
  MiRtValue v;
  v.kind = MI_RT_VAL_VOID;
  return v;
}

MiRtValue mi_rt_make_int(long long v)
{
  MiRtValue out;
  out.kind  = MI_RT_VAL_INT;
  out.as.i  = v;
  return out;
}

MiRtValue mi_rt_make_float(double v)
{
  MiRtValue out;
  out.kind  = MI_RT_VAL_FLOAT;
  out.as.f  = v;
  return out;
}

MiRtValue mi_rt_make_bool(bool v)
{
  MiRtValue out;
  out.kind  = MI_RT_VAL_BOOL;
  out.as.b  = v;
  return out;
}

MiRtValue mi_rt_make_string_slice(XSlice s)
{
  MiRtValue out;
  out.kind  = MI_RT_VAL_STRING;
  out.as.s  = s;
  return out;
}

MiRtValue mi_rt_make_list(MiRtList *list)
{
  MiRtValue v;
  v.kind     = MI_RT_VAL_LIST;
  v.as.list  = list;
  return v;
}

MiRtValue mi_rt_make_pair(MiRtPair *pair)
{
  MiRtValue v;
  v.kind     = MI_RT_VAL_PAIR;
  v.as.pair  = pair;
  return v;
}

MiRtList* mi_rt_list_create(MiRuntime *rt)
{
  if (!rt || !rt->current || !rt->current->arena)
  {
    return NULL;
  }

  MiRtList* list = (MiRtList*) x_arena_alloc_zero(rt->current->arena, sizeof(MiRtList));
  if (!list)
  {
    mi_error("minima_runtime: out of memory\n");
    exit(1);
  }

  list->arena = rt->current->arena;
  list->items = NULL;
  list->count = 0u;
  list->capacity = 0u;
  return list;
}

MiRtPair* mi_rt_pair_create(void)
{
  MiRtPair *pair = (MiRtPair*) s_realloc(NULL, sizeof(MiRtPair));
  pair->items[0]  = mi_rt_make_void();
  pair->items[1]  = mi_rt_make_void();
  return pair;
}

bool mi_rt_list_push(MiRtList *list, MiRtValue value)
{
  if (!list)
  {
    return false;
  }

  if (!list->arena)
  {
    mi_error("minima_runtime: list has no arena\n");
    return false;
  }

  if (list->count == list->capacity)
  {
    size_t new_cap = (list->capacity == 0u) ? 4u : (list->capacity * 2u);
    MiRtValue* new_items = (MiRtValue*) x_arena_alloc(list->arena, new_cap * sizeof(MiRtValue));
    if (!new_items)
    {
      mi_error("minima_runtime: out of memory\n");
      exit(1);
    }

    if (list->items && list->count > 0u)
    {
      memcpy(new_items, list->items, list->count * sizeof(MiRtValue));
    }

    list->items = new_items;
    list->capacity = new_cap;
  }

  list->items[list->count] = value;
  list->count              = list->count + 1u;
  return true;
}

void mi_rt_scope_push(MiRuntime* rt)
{
  if (!rt)
  {
    return;
  }

  MiScopeFrame* f = NULL;
  if (rt->free_frames)
  {
    f = rt->free_frames;
    rt->free_frames = f->next_free;
    f->next_free = NULL;

    x_arena_reset_keep_head(f->arena);
    f->vars = NULL;
    f->parent = rt->current;
  }
  else
  {
    f = (MiScopeFrame*) malloc(sizeof(MiScopeFrame));
    if (!f)
    {
      mi_error("minima_runtime: out of memory\n");
      exit(1);
    }

    s_rt_scope_init(f, rt->scope_chunk_size, rt->current);
  }

  rt->current = f;
}

void mi_rt_scope_pop(MiRuntime* rt)
{
  if (!rt || !rt->current)
  {
    return;
  }

  if (rt->current == &rt->root)
  {
    return;
  }

  MiScopeFrame* f = rt->current;
  rt->current = f->parent;

  x_arena_reset_keep_head(f->arena);
  f->vars = NULL;
  f->parent = NULL;
  f->next_free = rt->free_frames;
  rt->free_frames = f;
}
