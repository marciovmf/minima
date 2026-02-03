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
// Builtin signatures
//----------------------------------------------------------

typedef struct MiBuiltinSig
{
  const char* name;
  MiTypeKind  ret_type;
  const MiTypeKind* param_types;
  int         param_count;
  bool        is_variadic;
  MiTypeKind  variadic_type;
} MiBuiltinSig;

static const MiTypeKind s_sig_any_params_1[] = { MI_TYPE_ANY };

static const MiBuiltinSig s_builtin_sigs[] =
{
  { "print",  MI_TYPE_VOID,   NULL,               0, true,  MI_TYPE_ANY },
  { "len",    MI_TYPE_INT,    s_sig_any_params_1, 1, false, MI_TYPE_ANY },
  { "trace",  MI_TYPE_VOID,   NULL,               0, false, MI_TYPE_ANY },
  { "typeof", MI_TYPE_STRING, s_sig_any_params_1, 1, false, MI_TYPE_ANY },
  { "type",   MI_TYPE_STRING, s_sig_any_params_1, 1, false, MI_TYPE_ANY },
  { "t",      MI_TYPE_STRING, s_sig_any_params_1, 1, false, MI_TYPE_ANY },
};

static const MiBuiltinSig* s_find_builtin_sig(XSlice name)
{
  for (int i = 0; i < (int)(sizeof(s_builtin_sigs) / sizeof(s_builtin_sigs[0])); ++i)
  {
    XSlice bn = x_slice_from_cstr(s_builtin_sigs[i].name);
    if (s_slice_eq(name, bn))
    {
      return &s_builtin_sigs[i];
    }
  }
  return NULL;
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


static bool s_tc_match_params(const MiFuncTypeSig* expected,
    MiTypeKind got_ret,
    const MiTypeKind* got_params,
    int got_param_count,
    bool got_is_variadic,
    MiTypeKind got_variadic_type)
{
  if (!expected)
  {
    return false;
  }

  if (expected->ret_type != MI_TYPE_ANY && expected->ret_type != got_ret)
  {
    return false;
  }

  if (got_is_variadic)
  {
    /* A variadic callee can accept any fixed prefix, as long as types are compatible. */
    for (int i = 0; i < expected->param_count; ++i)
    {
      MiTypeKind e = expected->param_types[i];
      if (e == MI_TYPE_ANY)
      {
        continue;
      }
      if (got_variadic_type != MI_TYPE_ANY && e != got_variadic_type)
      {
        return false;
      }
    }
    return true;
  }

  if (expected->param_count != got_param_count)
  {
    return false;
  }

  for (int i = 0; i < expected->param_count; ++i)
  {
    MiTypeKind e = expected->param_types[i];
    MiTypeKind g = got_params[i];
    if (e != MI_TYPE_ANY && e != g)
    {
      return false;
    }
  }

  return true;
}

static bool s_tc_match_func_sig(const MiFuncTypeSig* expected, const MiFuncSig* got)
{
  if (!expected || !got)
  {
    return false;
  }

  MiTypeKind params[64];
  int count = got->param_count;
  if (count > (int)(sizeof(params) / sizeof(params[0])))
  {
    return false;
  }

  for (int i = 0; i < count; ++i)
  {
    params[i] = got->params[i].type;
  }

  return s_tc_match_params(expected, got->ret_type, params, count, false, MI_TYPE_ANY);
}

static bool s_tc_match_builtin_sig(const MiFuncTypeSig* expected, const MiBuiltinSig* got)
{
  if (!expected || !got)
  {
    return false;
  }

  return s_tc_match_params(expected,
      got->ret_type,
      got->param_types,
      got->param_count,
      got->is_variadic,
      got->variadic_type);
}

//----------------------------------------------------------
// Expression typing
//----------------------------------------------------------

typedef struct MiTcEnvEntry
{
  XSlice name;
  MiTypeKind type;
  const MiFuncTypeSig* func_sig;
} MiTcEnvEntry;

typedef struct MiTcEnv
{
  MiTcEnvEntry entries[256];
  int count;
} MiTcEnv;

static const MiTcEnvEntry* s_env_get_entry(const MiTcEnv* env, XSlice name)
{
  if (!env)
  {
    return NULL;
  }
  for (int i = env->count - 1; i >= 0; i--)
  {
    if (s_slice_eq(env->entries[i].name, name))
    {
      return &env->entries[i];
    }
  }
  return NULL;
}

static MiTypeKind s_env_get(const MiTcEnv* env, XSlice name)
{
  const MiTcEnvEntry* e = s_env_get_entry(env, name);
  return e ? e->type : MI_TYPE_ANY;
}

static void s_env_set(MiTcEnv* env, XSlice name, MiTypeKind type, const MiFuncTypeSig* func_sig)
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
      env->entries[i].func_sig = func_sig;
      return;
    }
  }
  if (env->count < (int)(sizeof(env->entries) / sizeof(env->entries[0])))
  {
    env->entries[env->count].name = name;
    env->entries[env->count].type = type;
    env->entries[env->count].func_sig = func_sig;
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
          const MiFuncSig* got_sig = NULL;
          const MiBuiltinSig* got_bsig = NULL;

          if (arg && arg->kind == MI_EXPR_VAR && !arg->as.var.is_indirect)
          {
            got_sig = s_find_sig(script, arg->as.var.name);
            got_bsig = s_find_builtin_sig(arg->as.var.name);
          }
          else if (arg && arg->kind == MI_EXPR_STRING_LITERAL)
          {
            got_sig = s_find_sig(script, arg->as.string_lit.value);
            got_bsig = s_find_builtin_sig(arg->as.string_lit.value);
          }

          if (got_sig)
          {
            if (!s_tc_match_func_sig(expected_func_sig, got_sig))
            {
              s_tc_error(err, arg ? arg->token : head->token, "Callback function signature mismatch");
              return MI_TYPE_ANY;
            }
          }
          else if (got_bsig)
          {
            if (!s_tc_match_builtin_sig(expected_func_sig, got_bsig))
            {
              s_tc_error(err, arg ? arg->token : head->token, "Callback function signature mismatch");
              return MI_TYPE_ANY;
            }
          }
          else
          {
            s_tc_error(err, arg ? arg->token : head->token, "Callback function signature mismatch");
            return MI_TYPE_ANY;
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

    /* If the head is a variable name whose type is func(..)->.., typecheck as an indirect call. */
    const MiTcEnvEntry* ve = s_env_get_entry(env, name);
    if (ve && ve->type == MI_TYPE_FUNC && ve->func_sig)
    {
      const MiFuncTypeSig* fs = ve->func_sig;

      if (!fs->is_variadic)
      {
        if ((int)e->as.command.argc != fs->param_count)
        {
          s_tc_error(err, head->token, "Function call argument count mismatch");
          return MI_TYPE_ANY;
        }
      }
      else
      {
        if ((int)e->as.command.argc < fs->param_count)
        {
          s_tc_error(err, head->token, "Function call argument count mismatch");
          return MI_TYPE_ANY;
        }
      }

      const MiExprList* it = e->as.command.args;
      for (int i = 0; i < (int)e->as.command.argc; i++)
      {
        const MiExpr* arg = it ? it->expr : NULL;
        MiTypeKind got = s_tc_expr(script, arg, env, err);
        if (err && err->message.length > 0)
        {
          return MI_TYPE_ANY;
        }

        MiTypeKind expected = MI_TYPE_ANY;
        if (i < fs->param_count)
        {
          expected = fs->param_types[i];
        }
        else
        {
          expected = fs->variadic_type;
        }

        if (!s_type_compatible(got, expected))
        {
          s_tc_error(err, arg ? arg->token : head->token, "Function argument type mismatch");
          return MI_TYPE_ANY;
        }

        it = it ? it->next : NULL;
      }

      return fs->ret_type;
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

                         /* Builtins with known signatures can be passed as callbacks. */
                         if (s_find_builtin_sig(e->as.var.name) != NULL)
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

static MiTypeKind s_tc_typecheck_command_expr(const MiScript* script, const MiCommand* c, MiTcEnv* env, MiTypecheckError* err)
{
  MiExpr fake;
  memset(&fake, 0, sizeof(fake));
  fake.kind = MI_EXPR_COMMAND;
  fake.token = c && c->head ? c->head->token : (MiToken){0};
  fake.as.command.head = c ? c->head : NULL;
  fake.as.command.args = c ? c->args : NULL;
  fake.as.command.argc = c ? (unsigned int)c->argc : 0;
  return s_tc_expr(script, &fake, env, err);
}

static bool s_tc_command_definitely_returns(const MiScript* script, const MiCommand* c, const MiFuncSig* sig, MiTcEnv* env, MiTypecheckError* err);

static bool s_tc_script_definitely_returns(const MiScript* script, const MiScript* body, const MiFuncSig* sig, MiTcEnv* env, MiTypecheckError* err)
{
  const MiCommandList* it = body ? body->first : NULL;
  while (it)
  {
    const MiCommand* c = it->command;
    if (c)
    {
      bool dr = s_tc_command_definitely_returns(script, c, sig, env, err);
      if (err && err->message.length > 0)
      {
        return false;
      }
      if (dr)
      {
        return true;
      }
    }
    it = it->next;
  }
  return false;
}

static bool s_tc_if_definitely_returns(const MiScript* script, const MiCommand* c, const MiFuncSig* sig, MiTcEnv* env, MiTypecheckError* err)
{
  /* if (cond) {then} else {else} */
  const MiExprList* args = c->args;
  if (!args || !args->expr)
  {
    return false;
  }

  /* Typecheck condition */
  (void)s_tc_expr(script, args->expr, env, err);
  if (err && err->message.length > 0)
  {
    return false;
  }
  args = args->next;

  bool saw_else = false;
  bool all_return = true;

  while (args)
  {
    const MiExpr* ex = args->expr;
    if (ex && ex->kind == MI_EXPR_BLOCK)
    {
      MiTcEnv inner = *env;
      bool br = s_tc_script_definitely_returns(script, ex->as.block.script, sig, &inner, err);
      if (err && err->message.length > 0)
      {
        return false;
      }
      all_return = all_return && br;
      args = args->next;
      continue;
    }

    /* else / elseif are encoded as string literal command heads in args list:
       we accept 'else' followed by block, or 'elseif' cond block. */
    if (ex && ex->kind == MI_EXPR_STRING_LITERAL && s_slice_eq(ex->as.string_lit.value, x_slice_init("else", 4)))
    {
      saw_else = true;
      args = args->next;
      continue;
    }

    if (ex && ex->kind == MI_EXPR_STRING_LITERAL && s_slice_eq(ex->as.string_lit.value, x_slice_init("elseif", 6)))
    {
      args = args->next;
      /* cond */
      if (!args)
      {
        break;
      }
      (void)s_tc_expr(script, args->expr, env, err);
      if (err && err->message.length > 0)
      {
        return false;
      }
      args = args->next;
      continue;
    }

    /* Unknown piece: typecheck expression and move on. */
    (void)s_tc_expr(script, ex, env, err);
    if (err && err->message.length > 0)
    {
      return false;
    }
    args = args->next;
  }

  if (!saw_else)
  {
    return false;
  }

  return all_return;
}

static bool s_tc_command_definitely_returns(const MiScript* script, const MiCommand* c, const MiFuncSig* sig, MiTcEnv* env, MiTypecheckError* err)
{
  if (!c)
  {
    return false;
  }

  /* return special form */
  if (c->head && c->head->kind == MI_EXPR_STRING_LITERAL && s_slice_eq(c->head->as.string_lit.value, x_slice_init("return", 6)))
  {
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
        s_tc_error(err, c->head->token, "Non-void function must return exactly one value");
        return false;
      }
      MiTypeKind rt = s_tc_expr(script, c->args ? c->args->expr : NULL, env, err);
      if (err && err->message.length > 0)
      {
        return false;
      }
      if (!s_type_compatible(rt, sig->ret_type))
      {
        s_tc_error(err, c->head->token, "Return type mismatch");
        return false;
      }
    }
    return true;
  }

  /* if special form: only used for definite-return; normal typing happens via expr checks below */
  if (c->head && c->head->kind == MI_EXPR_STRING_LITERAL && s_slice_eq(c->head->as.string_lit.value, x_slice_init("if", 2)))
  {
    /* We still need to typecheck the full command expression so variable inference stays consistent. */
    (void)s_tc_typecheck_command_expr(script, c, env, err);
    if (err && err->message.length > 0)
    {
      return false;
    }
    return s_tc_if_definitely_returns(script, c, sig, env, err);
  }

  /* default: just typecheck as expression */
  (void)s_tc_typecheck_command_expr(script, c, env, err);
  if (err && err->message.length > 0)
  {
    return false;
  }
  return false;
}

static bool s_tc_script_in_func(const MiScript* script, const MiScript* body, const MiFuncSig* sig, MiTypecheckError* err)
{
  MiTcEnv env;
  memset(&env, 0, sizeof(env));

  for (int i = 0; i < sig->param_count; ++i)
  {
    s_env_set(&env, sig->params[i].name, sig->params[i].type, sig->params[i].func_sig);
  }

  bool definitely_returns = s_tc_script_definitely_returns(script, body, sig, &env, err);
  if (err && err->message.length > 0)
  {
    return false;
  }

  if (sig->ret_type != MI_TYPE_VOID && !definitely_returns)
  {
    s_tc_error(err, sig->name_tok, "Non-void function is missing a return");
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



static void s_mi_print_source_line_tc(XSlice source, int line, int column)
{
  if (!source.ptr || source.length == 0 || line <= 0)
  {
    return;
  }

  int cur_line = 1;
  size_t i = 0;
  size_t line_start = 0;
  size_t line_end = source.length;

  while (i < source.length)
  {
    if (cur_line == line)
    {
      line_start = i;
      break;
    }
    if (source.ptr[i] == '\n')
    {
      cur_line += 1;
    }
    i += 1;
  }

  i = line_start;
  while (i < source.length)
  {
    if (source.ptr[i] == '\n')
    {
      line_end = i;
      break;
    }
    i += 1;
  }

  if (line_start >= source.length || line_end <= line_start)
  {
    return;
  }

  XSlice ln = x_slice_init(source.ptr + line_start, line_end - line_start);
  mi_error_fmt("  %.*s\n", (int)ln.length, ln.ptr);

  int caret_col = column;
  if (caret_col <= 0)
  {
    caret_col = 1;
  }

  mi_error("  ");
  for (int c = 1; c < caret_col; c += 1)
  {
    mi_error(" ");
  }
  mi_error("^\n");
}

void mi_typecheck_print_error(XSlice source, const MiTypecheckError* err)
{
  if (!err || err->message.length == 0)
  {
    return;
  }

  mi_error_fmt("Typecheck error %d,%d - %.*s\n",
      err->line,
      err->column,
      (int)err->message.length,
      err->message.ptr);

  s_mi_print_source_line_tc(source, err->line, err->column);
}

