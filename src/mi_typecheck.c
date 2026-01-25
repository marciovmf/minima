#include "mi_typecheck.h"
#include "mi_log.h"

#include <string.h>

//----------------------------------------------------------
// Helpers
//----------------------------------------------------------

typedef struct MiTcSig
{
  const MiFuncSig* sig;
} MiTcSig;

static bool s_slice_eq(XSlice a, XSlice b)
{
  if (a.length != b.length)
  {
    return false;
  }
  if (a.length == 0)
  {
    return true;
  }
  return memcmp(a.ptr, b.ptr, a.length) == 0;
}

static void s_tc_error(MiTypecheckError* err, MiToken at, const char* msg)
{
  if (!err)
  {
    return;
  }
  err->line = at.line;
  err->column = at.column;
  err->message = x_slice_init(msg, strlen(msg));
}

static bool s_is_numeric(MiTypeKind t)
{
  return (t == MI_TYPE_INT) || (t == MI_TYPE_FLOAT);
}

static bool s_type_compatible(MiTypeKind got, MiTypeKind expected)
{
  if (expected == MI_TYPE_ANY)
  {
    return true;
  }
  return got == expected;
}

//----------------------------------------------------------
// Signature table (linear scan: fine for scripts)
//----------------------------------------------------------

static const MiFuncSig* s_find_sig(const MiScript* script, XSlice name)
{
  const MiCommandList* it = script ? script->first : NULL;
  while (it)
  {
    const MiCommand* c = it->command;
    if (c && c->func_sig)
    {
      if (s_slice_eq(c->func_sig->name, name))
      {
        return c->func_sig;
      }
    }
    it = it->next;
  }
  return NULL;
}


static bool s_tc_match_func_sig(const MiFuncTypeSig* expected, const MiFuncSig* got)
{
  if (!expected || !got)
  {
    return false;
  }

  if (expected->param_count != got->param_count)
  {
    return false;
  }

  if (expected->ret_type != MI_TYPE_ANY && expected->ret_type != got->ret_type)
  {
    return false;
  }

  for (int i = 0; i < expected->param_count; i++)
  {
    MiTypeKind e = expected->param_types[i];
    MiTypeKind g = got->params[i].type;
    if (e != MI_TYPE_ANY && e != g)
    {
      return false;
    }
  }

  return true;
}

static bool s_tc_is_builtin_print_expr(const MiExpr* e)
{
  if (!e || e->kind != MI_EXPR_VAR || e->as.var.is_indirect)
  {
    return false;
  }
  return s_slice_eq(e->as.var.name, x_slice_init("print", 5));
}

//----------------------------------------------------------
// Expression typing
//----------------------------------------------------------

typedef struct MiTcEnvEntry
{
  XSlice name;
  MiTypeKind type;
} MiTcEnvEntry;

typedef struct MiTcEnv
{
  MiTcEnvEntry entries[256];
  int count;
} MiTcEnv;

static MiTypeKind s_env_get(const MiTcEnv* env, XSlice name)
{
  if (!env)
  {
    return MI_TYPE_ANY;
  }
  for (int i = env->count - 1; i >= 0; i--)
  {
    if (s_slice_eq(env->entries[i].name, name))
    {
      return env->entries[i].type;
    }
  }
  return MI_TYPE_ANY;
}

static void s_env_set(MiTcEnv* env, XSlice name, MiTypeKind type)
{
  if (!env)
  {
    return;
  }
  for (int i = env->count - 1; i >= 0; i--)
  {
    if (s_slice_eq(env->entries[i].name, name))
    {
      env->entries[i].type = type;
      return;
    }
  }
  if (env->count < (int)(sizeof(env->entries) / sizeof(env->entries[0])))
  {
    env->entries[env->count].name = name;
    env->entries[env->count].type = type;
    env->count += 1;
  }
}

static MiTypeKind s_tc_expr(const MiScript* script, const MiExpr* e, MiTcEnv* env, MiTypecheckError* err);

static MiTypeKind s_tc_command_expr(const MiScript* script, const MiExpr* e, MiTcEnv* env, MiTypecheckError* err)
{
  // Default: unknown command => any
  if (!e || e->kind != MI_EXPR_COMMAND)
  {
    return MI_TYPE_ANY;
  }

  const MiExpr* head = e->as.command.head;
  if (head && head->kind == MI_EXPR_STRING_LITERAL)
  {
    XSlice name = head->as.string_lit.value;
    const MiFuncSig* sig = s_find_sig(script, name);
    if (sig)
    {
      // Arg count check.
      if ((int)e->as.command.argc != sig->param_count)
      {
        s_tc_error(err, head->token, "Function call argument count mismatch");
        return MI_TYPE_ANY;
      }

      // Type check args.
      const MiExprList* it = e->as.command.args;
      for (int i = 0; i < sig->param_count; i++)
      {
        const MiExpr* arg = it ? it->expr : NULL;
        MiTypeKind got = s_tc_expr(script, arg, env, err);
        if (err && err->message.length > 0)
        {
          return MI_TYPE_ANY;
        }
        MiTypeKind expected = sig->params[i].type;

MiFuncTypeSig* expected_func_sig = sig->params[i].func_sig;
if (expected == MI_TYPE_FUNC && expected_func_sig != NULL)
{
  if (!s_tc_is_builtin_print_expr(arg))
  {
    const MiFuncSig* got_sig = NULL;

    if (arg && arg->kind == MI_EXPR_VAR && !arg->as.var.is_indirect)
    {
      got_sig = s_find_sig(script, arg->as.var.name);
    }
    else if (arg && arg->kind == MI_EXPR_STRING_LITERAL)
    {
      got_sig = s_find_sig(script, arg->as.string_lit.value);
    }

    if (!got_sig || !s_tc_match_func_sig(expected_func_sig, got_sig))
    {
      s_tc_error(err, arg ? arg->token : head->token, "Callback function signature mismatch");
      return MI_TYPE_ANY;
    }
  }
}

        if (!s_type_compatible(got, expected))
        {
          s_tc_error(err, arg ? arg->token : head->token, "Function argument type mismatch");
          return MI_TYPE_ANY;
        }
        it = it ? it->next : NULL;
      }
      return sig->ret_type;
    }
  }

  return MI_TYPE_ANY;
}

static MiTypeKind s_tc_binary(const MiScript* script, const MiExpr* e, MiTcEnv* env, MiTypecheckError* err)
{
  MiTokenKind op = e->as.binary.op;
  MiTypeKind lt = s_tc_expr(script, e->as.binary.left, env, err);
  if (err && err->message.length > 0) return MI_TYPE_ANY;
  MiTypeKind rt = s_tc_expr(script, e->as.binary.right, env, err);
  if (err && err->message.length > 0) return MI_TYPE_ANY;

  // void rules
  if (lt == MI_TYPE_VOID || rt == MI_TYPE_VOID)
  {
    if (op == MI_TOK_EQEQ || op == MI_TOK_BANGEQ)
    {
      return MI_TYPE_BOOL;
    }
    s_tc_error(err, e->token, "Invalid operator with void");
    return MI_TYPE_ANY;
  }

  // Ordering
  if (op == MI_TOK_GT || op == MI_TOK_GTEQ || op == MI_TOK_LT || op == MI_TOK_LTEQ)
  {
    if (!s_is_numeric(lt) || !s_is_numeric(rt))
    {
      s_tc_error(err, e->token, "Ordering comparison requires numeric operands");
      return MI_TYPE_ANY;
    }
    return MI_TYPE_BOOL;
  }

  // Equality
  if (op == MI_TOK_EQEQ || op == MI_TOK_BANGEQ)
  {
    return MI_TYPE_BOOL;
  }

  // Arithmetic
  if (op == MI_TOK_PLUS || op == MI_TOK_MINUS || op == MI_TOK_STAR || op == MI_TOK_SLASH)
  {
    if (!s_is_numeric(lt) || !s_is_numeric(rt))
    {
      s_tc_error(err, e->token, "Arithmetic requires numeric operands");
      return MI_TYPE_ANY;
    }
    if (lt == MI_TYPE_FLOAT || rt == MI_TYPE_FLOAT)
    {
      return MI_TYPE_FLOAT;
    }
    return MI_TYPE_INT;
  }

  return MI_TYPE_ANY;
}

static MiTypeKind s_tc_expr(const MiScript* script, const MiExpr* e, MiTcEnv* env, MiTypecheckError* err)
{
  if (!e)
  {
    return MI_TYPE_VOID;
  }

  switch (e->kind)
  {
    case MI_EXPR_INT_LITERAL: return MI_TYPE_INT;
    case MI_EXPR_FLOAT_LITERAL: return MI_TYPE_FLOAT;
    case MI_EXPR_STRING_LITERAL: return MI_TYPE_STRING;
    case MI_EXPR_BOOL_LITERAL: return MI_TYPE_BOOL;
    case MI_EXPR_VOID_LITERAL: return MI_TYPE_VOID;
    case MI_EXPR_BLOCK: return MI_TYPE_BLOCK;
    case MI_EXPR_LIST: return MI_TYPE_LIST;
    case MI_EXPR_DICT: return MI_TYPE_DICT;

    
case MI_EXPR_VAR:
  {
    if (e->as.var.is_indirect)
    {
      return MI_TYPE_ANY;
    }

    MiTypeKind t = s_env_get(env, e->as.var.name);
    if (t != MI_TYPE_ANY)
    {
      return t;
    }

    /* Treat known script functions as values of type func. */
    if (s_find_sig(script, e->as.var.name) != NULL)
    {
      return MI_TYPE_FUNC;
    }

    /* A tiny set of builtins can be passed as callbacks. */
    if (s_slice_eq(e->as.var.name, x_slice_init("print", 5)))
    {
      return MI_TYPE_FUNC;
    }

    return MI_TYPE_ANY;
  }

    case MI_EXPR_UNARY:
      {
        MiTypeKind t = s_tc_expr(script, e->as.unary.expr, env, err);
        if (err && err->message.length > 0) return MI_TYPE_ANY;
        if (e->as.unary.op == MI_TOK_NOT)
        {
          return MI_TYPE_BOOL;
        }
        if (e->as.unary.op == MI_TOK_MINUS)
        {
          if (!s_is_numeric(t))
          {
            s_tc_error(err, e->token, "Unary '-' requires numeric operand");
            return MI_TYPE_ANY;
          }
          return t;
        }
        return MI_TYPE_ANY;
      }

    case MI_EXPR_BINARY:
      return s_tc_binary(script, e, env, err);

    case MI_EXPR_INDEX:
      {
        // We can at least validate the container kind.
        MiTypeKind tt = s_tc_expr(script, e->as.index.target, env, err);
        if (err && err->message.length > 0) return MI_TYPE_ANY;
        (void)s_tc_expr(script, e->as.index.index, env, err);
        if (err && err->message.length > 0) return MI_TYPE_ANY;
        if (tt != MI_TYPE_LIST && tt != MI_TYPE_DICT && tt != MI_TYPE_ANY)
        {
          s_tc_error(err, e->token, "Indexing requires list or dict");
          return MI_TYPE_ANY;
        }
        return MI_TYPE_ANY;
      }

    case MI_EXPR_COMMAND:
      return s_tc_command_expr(script, e, env, err);

    case MI_EXPR_PAIR:
      return MI_TYPE_ANY;
  }

  return MI_TYPE_ANY;
}

//----------------------------------------------------------
// Function body checking
//----------------------------------------------------------

static bool s_tc_script_in_func(const MiScript* script, const MiScript* body, const MiFuncSig* sig, MiTypecheckError* err)
{
  MiTcEnv env;
  memset(&env, 0, sizeof(env));

  // Bind parameters.
  for (int i = 0; i < sig->param_count; i++)
  {
    s_env_set(&env, sig->params[i].name, sig->params[i].type);
  }

  bool saw_return = false;

  const MiCommandList* it = body ? body->first : NULL;
  while (it)
  {
    const MiCommand* c = it->command;
    if (!c)
    {
      it = it->next;
      continue;
    }

    // return special form.
    if (c->head && c->head->kind == MI_EXPR_STRING_LITERAL && s_slice_eq(c->head->as.string_lit.value, x_slice_init("return", 6)))
    {
      saw_return = true;
      if (sig->ret_type == MI_TYPE_VOID)
      {
        if (c->argc > 0)
        {
          s_tc_error(err, c->head->token, "Void function cannot return a value");
          return false;
        }
      }
      else
      {
        if (c->argc != 1)
        {
          s_tc_error(err, c->head->token, "Non-void function must return a value");
          return false;
        }
        MiTypeKind rt = s_tc_expr(script, c->args ? c->args->expr : NULL, &env, err);
        if (err && err->message.length > 0) return false;
        if (!s_type_compatible(rt, sig->ret_type))
        {
          s_tc_error(err, c->head->token, "Return type mismatch");
          return false;
        }
      }
      it = it->next;
      continue;
    }

    // Basic assignment inference: x = expr; is parsed as set(x, expr)
    if (c->head && c->head->kind == MI_EXPR_STRING_LITERAL && s_slice_eq(c->head->as.string_lit.value, x_slice_init("set", 3)))
    {
      if (c->argc == 2 && c->args && c->args->expr && c->args->expr->kind == MI_EXPR_STRING_LITERAL)
      {
        XSlice var_name = c->args->expr->as.string_lit.value;
        MiTypeKind rhs_t = s_tc_expr(script, c->args->next ? c->args->next->expr : NULL, &env, err);
        if (err && err->message.length > 0) return false;
        s_env_set(&env, var_name, rhs_t);
      }
    }

    // Typecheck all expressions appearing in this command.
    {
      MiExpr fake;
      memset(&fake, 0, sizeof(fake));
      fake.kind = MI_EXPR_COMMAND;
      fake.token = c->head ? c->head->token : (MiToken){0};
      fake.as.command.head = c->head;
      fake.as.command.args = c->args;
      fake.as.command.argc = (unsigned int)c->argc;
      (void)s_tc_expr(script, &fake, &env, err);
      if (err && err->message.length > 0) return false;
    }

    it = it->next;
  }

  if (sig->ret_type != MI_TYPE_VOID && !saw_return)
  {
    s_tc_error(err, sig->name_tok, "Non-void function is missing return");
    return false;
  }

  return true;
}

//----------------------------------------------------------
// Public entry
//----------------------------------------------------------

bool mi_typecheck_script(const MiScript* script, MiTypecheckError* err)
{
  if (err)
  {
    err->line = 0;
    err->column = 0;
    err->message = x_slice_init(NULL, 0);
  }

  if (!script)
  {
    return true;
  }

  // Validate all function bodies.
  const MiCommandList* it = script->first;
  while (it)
  {
    const MiCommand* c = it->command;
    if (c && c->func_sig)
    {
      // func decl is lowered to cmd(name, params..., block) so last arg is block.
      const MiExprList* al = c->args;
      const MiExprList* last = NULL;
      while (al)
      {
        last = al;
        al = al->next;
      }
      const MiExpr* block = last ? last->expr : NULL;
      if (!block || block->kind != MI_EXPR_BLOCK || !block->as.block.script)
      {
        s_tc_error(err, c->head ? c->head->token : (MiToken){0}, "Malformed function body");
        return false;
      }

      if (!s_tc_script_in_func(script, block->as.block.script, c->func_sig, err))
      {
        return false;
      }
    }
    it = it->next;
  }

  // Typecheck top-level expressions too (mainly operator sanity).
  {
    MiTcEnv env;
    memset(&env, 0, sizeof(env));

    const MiCommandList* jt = script->first;
    while (jt)
    {
      const MiCommand* c = jt->command;
      if (c)
      {
        MiExpr fake;
        memset(&fake, 0, sizeof(fake));
        fake.kind = MI_EXPR_COMMAND;
        fake.token = c->head ? c->head->token : (MiToken){0};
        fake.as.command.head = c->head;
        fake.as.command.args = c->args;
        fake.as.command.argc = (unsigned int)c->argc;
        (void)s_tc_expr(script, &fake, &env, err);
        if (err && err->message.length > 0) return false;
      }
      jt = jt->next;
    }
  }

  return true;
}
