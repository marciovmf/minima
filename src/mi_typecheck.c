#include "mi_typecheck.h"
#include "mi_log.h"
#include "mi_vm.h"

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


static const MiFuncTypeSig* s_find_runtime_sig(MiVm* vm, XSlice name)
{
  const MiFuncTypeSig* sig = NULL;
  if (!vm)
  {
    return NULL;
  }
  if (mi_vm_find_sig(vm, name, &sig))
  {
    return sig;
  }
  return NULL;
}


static bool s_tc_build_static_name(const MiExpr* e, char* out, size_t cap, size_t* io_len)
{
  if (!e || !out || cap == 0 || !io_len)
  {
    return false;
  }

  if (e->kind == MI_EXPR_STRING_LITERAL)
  {
    const XSlice v = e->as.string_lit.value;
    if (!v.ptr || v.length == 0)
    {
      return false;
    }
    if (*io_len + (size_t)v.length >= cap)
    {
      return false;
    }
    memcpy(out + *io_len, v.ptr, (size_t)v.length);
    *io_len += (size_t)v.length;
    out[*io_len] = '\0';
    return true;
  }

  if (e->kind == MI_EXPR_VAR && !e->as.var.is_indirect)
  {
    const XSlice n = e->as.var.name;
    if (!n.ptr || n.length == 0)
    {
      return false;
    }
    if (*io_len + (size_t)n.length >= cap)
    {
      return false;
    }
    memcpy(out + *io_len, n.ptr, (size_t)n.length);
    *io_len += (size_t)n.length;
    out[*io_len] = '\0';
    return true;
  }

  if (e->kind == MI_EXPR_QUAL)
  {
    if (!s_tc_build_static_name(e->as.qual.target, out, cap, io_len))
    {
      return false;
    }

    const char sep[] = "::";
    if (*io_len + 2 >= cap)
    {
      return false;
    }
    memcpy(out + *io_len, sep, 2);
    *io_len += 2;

    const XSlice m = e->as.qual.member;
    if (!m.ptr || m.length == 0)
    {
      return false;
    }
    if (*io_len + (size_t)m.length >= cap)
    {
      return false;
    }
    memcpy(out + *io_len, m.ptr, (size_t)m.length);
    *io_len += (size_t)m.length;
    out[*io_len] = '\0';
    return true;
  }

  return false;
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
    // A variadic callee can accept any fixed prefix, as long as types are compatible. 
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

static bool s_tc_match_runtime_sig(const MiFuncTypeSig* expected, const MiFuncTypeSig* got)
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
  // Current function signature while typechecking a function body. 
  const MiFuncSig* cur_func_sig;
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

static MiTypeKind s_tc_expr(const MiScript* script, MiVm* vm, const MiExpr* e, MiTcEnv* env, MiTypecheckError* err);


static void s_tc_preload_includes(const MiScript* script, MiVm* vm, XSlice dbg_file)
{
  if (!script || !vm || !vm->rt)
  {
    return;
  }

  // During typecheck we are not executing a chunk, but include() resolves
  // relative module paths using the VM's current script file derived from
  // debug context. Provide a temporary debug chunk so relative includes
  // behave the same as at runtime.
  static MiVmChunk fake_dbg_chunk;
  memset(&fake_dbg_chunk, 0, sizeof(fake_dbg_chunk));
  fake_dbg_chunk.dbg_file = dbg_file;
  const MiVmChunk* prev_dbg_chunk = vm->dbg_chunk;
  if (dbg_file.length)
  {
    vm->dbg_chunk = &fake_dbg_chunk;
  }

  const MiCommandList* it = script->first;
  while (it)
  {
    const MiCommand* c = it->command;
    if (c && c->is_include_stmt)
    {
      const MiExpr* head = c->head;
      const MiExpr* arg0 = (c->args && c->args->expr) ? c->args->expr : NULL;
      if (head && head->kind == MI_EXPR_STRING_LITERAL && arg0 && arg0->kind == MI_EXPR_STRING_LITERAL)
      {
        XSlice cmd_name = head->as.string_lit.value;
        MiRtValue argv[1];
        argv[0] = mi_rt_make_string_slice(arg0->as.string_lit.value);

        MiRtValue mod = mi_vm_call_command(vm, cmd_name, 1, argv);
        // Bind alias into the current runtime scope so qualified lookups work during typecheck. 
        (void)mi_rt_var_set(vm->rt, c->include_alias_tok.lexeme, mod);
        mi_rt_value_release(vm->rt, mod);
      }
    }
    it = it->next;
  }

  vm->dbg_chunk = prev_dbg_chunk;
}

static MiTypeKind s_tc_command_expr(const MiScript* script, MiVm* vm, const MiExpr* e, MiTcEnv* env, MiTypecheckError* err)
{
  // Default: unknown command => any
  if (!e || e->kind != MI_EXPR_COMMAND)
  {
    return MI_TYPE_ANY;
  }

  const MiExpr* head = e->as.command.head;
  XSlice name = x_slice_init(NULL, 0);

  // Support string literal heads, vars, and qualified heads like int::cast. 
  if (head)
  {
    if (head->kind == MI_EXPR_STRING_LITERAL)
    {
      name = head->as.string_lit.value;
    }
    else if (head->kind == MI_EXPR_VAR && !head->as.var.is_indirect)
    {
      name = head->as.var.name;
    }
    else if (head->kind == MI_EXPR_QUAL)
    {
      char buf[256];
      size_t len = 0;
      buf[0] = '\0';
      if (s_tc_build_static_name(head, buf, sizeof(buf), &len))
      {
        name = x_slice_init(buf, (int)len);
      }
    }
  }

  if (name.ptr && name.length > 0)
  {
    // Special-case: set("name", rhs)
    // The parser lowers both assignments (x = rhs) and declarations (let x = rhs)
    // to a command call: set("x", rhs).
    //
    // Runtime updates bindings fine, but the typechecker must also update its local
    // environment, otherwise reads of x later in the same function are typed as ANY
    // and can trigger false "Return type mismatch" errors.
    //
    // Note: we only update the typing env when the lvalue is a *static* string
    // literal. Dynamic names remain typed as ANY.
    
    if (s_slice_eq(name, x_slice_from_cstr("set")))
    {
      if ((int)e->as.command.argc == 2)
      {
        const MiExprList* it = e->as.command.args;
        const MiExpr* lhs = it ? it->expr : NULL;
        const MiExpr* rhs = (it && it->next) ? it->next->expr : NULL;

        MiTypeKind rhs_type = s_tc_expr(script, vm, rhs, env, err);
        if (err && err->message.length > 0)
        {
          return MI_TYPE_ANY;
        }

        if (lhs && lhs->kind == MI_EXPR_STRING_LITERAL)
        {
          s_env_set(env, lhs->as.string_lit.value, rhs_type, NULL);
        }

        return rhs_type;
      }
    }

    const MiFuncSig* sig = s_find_sig(script, name);
    if (sig)
    {
      // Arg count check (supports variadic).
      if (!sig->is_variadic)
      {
        if ((int)e->as.command.argc != sig->param_count)
        {
          s_tc_error(err, head->token, "Function call argument count mismatch");
          return MI_TYPE_ANY;
        }
      }
      else
      {
        if ((int)e->as.command.argc < sig->param_count)
        {
          s_tc_error(err, head->token, "Function call argument count mismatch");
          return MI_TYPE_ANY;
        }
      }

      // Type check args (fixed params first, then variadic tail).
      const MiExprList* it = e->as.command.args;
      for (int i = 0; i < sig->param_count; i++)
      {
        const MiExpr* arg = it ? it->expr : NULL;
        MiTypeKind got = s_tc_expr(script, vm, arg, env, err);
        if (err && err->message.length > 0)
        {
          return MI_TYPE_ANY;
        }
        MiTypeKind expected = sig->params[i].type;

        MiFuncTypeSig* expected_func_sig = sig->params[i].func_sig;
        if (expected == MI_TYPE_FUNC && expected_func_sig != NULL)
        {
          const MiFuncSig* got_sig = NULL;
          const MiFuncTypeSig* got_rsig = NULL;

          if (arg && arg->kind == MI_EXPR_VAR && !arg->as.var.is_indirect)
          {
            got_sig = s_find_sig(script, arg->as.var.name);
            got_rsig = s_find_runtime_sig(vm, arg->as.var.name);
          }
          else if (arg && arg->kind == MI_EXPR_STRING_LITERAL)
          {
            got_sig = s_find_sig(script, arg->as.string_lit.value);
            got_rsig = s_find_runtime_sig(vm, arg->as.string_lit.value);
          }

          if (got_sig)
          {
            if (!s_tc_match_func_sig(expected_func_sig, got_sig))
            {
              s_tc_error(err, arg ? arg->token : head->token, "Callback function signature mismatch");
              return MI_TYPE_ANY;
            }
          }
          else if (got_rsig)
          {
            if (!s_tc_match_runtime_sig(expected_func_sig, got_rsig))
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

      if (sig->is_variadic)
      {
        MiTypeKind vt = sig->variadic_type;
        int extra = (int)e->as.command.argc - sig->param_count;
        for (int j = 0; j < extra; ++j)
        {
          const MiExpr* arg = it ? it->expr : NULL;
          MiTypeKind got = s_tc_expr(script, vm, arg, env, err);
          if (err && err->message.length)
          {
            return MI_TYPE_ANY;
          }
          if (vt != MI_TYPE_ANY)
          {
            if (!s_type_compatible(got, vt))
            {
              s_tc_error(err, arg ? arg->token : head->token, "Function argument type mismatch");
              return MI_TYPE_ANY;
            }
          }
          it = it ? it->next : NULL;
        }
      }

      return sig->ret_type;
    }

    // Runtime calls with known signatures (global commands and qualified members). 
    {
      const MiFuncTypeSig* fs = s_find_runtime_sig(vm, name);
      if (fs)
      {
        // Arg count check (supports variadic). 
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
          MiTypeKind got = s_tc_expr(script, vm, arg, env, err);
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

        // Special-case: arg(i) returns the type of the i-th argument of the current function when known. 
        if (s_slice_eq(name, x_slice_init("arg", 3)))
        {
          const MiFuncSig* cur = env ? env->cur_func_sig : NULL;
          const MiExpr* idx_expr = e->as.command.args ? e->as.command.args->expr : NULL;
          if (cur && idx_expr && idx_expr->kind == MI_EXPR_INT_LITERAL)
          {
            int64_t idx = idx_expr->as.int_lit.value;
            if (idx >= 0)
            {
              if ((int)idx < cur->param_count)
              {
                return cur->params[(int)idx].type;
              }
              if (cur->is_variadic)
              {
                return cur->variadic_type;
              }
            }
          }
          return MI_TYPE_ANY;
        }

        return fs->ret_type;
      }
    }

    // If the head is a variable name whose type is func(..)->.., typecheck as an indirect call. 
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
        MiTypeKind got = s_tc_expr(script, vm, arg, env, err);
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

static MiTypeKind s_tc_binary(const MiScript* script, MiVm* vm, const MiExpr* e, MiTcEnv* env, MiTypecheckError* err)
{
  MiTokenKind op = e->as.binary.op;
  MiTypeKind lt = s_tc_expr(script, vm, e->as.binary.left, env, err);
  if (err && err->message.length > 0) return MI_TYPE_ANY;
  MiTypeKind rt = s_tc_expr(script, vm, e->as.binary.right, env, err);
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

static MiTypeKind s_tc_expr(const MiScript* script, MiVm* vm, const MiExpr* e, MiTcEnv* env, MiTypecheckError* err)
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

                         // Treat known script functions as values of type func. 
                         if (s_find_sig(script, e->as.var.name) != NULL)
                         {
                           return MI_TYPE_FUNC;
                         }

                         // Runtime commands with signatures can be passed as callbacks. 
                         if (s_find_runtime_sig(vm, e->as.var.name) != NULL)
                         {
                           return MI_TYPE_FUNC;
                         }

                         return MI_TYPE_ANY;
                       }

    case MI_EXPR_UNARY:
                       {
  MiTypeKind t = s_tc_expr(script, vm, e->as.unary.expr, env, err);
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
                       return s_tc_binary(script, vm, e, env, err);

    case MI_EXPR_INDEX:
                       {
                         // We can at least validate the container kind.
  MiTypeKind tt = s_tc_expr(script, vm, e->as.index.target, env, err);
                         if (err && err->message.length > 0) return MI_TYPE_ANY;
  (void)s_tc_expr(script, vm, e->as.index.index, env, err);
                         if (err && err->message.length > 0) return MI_TYPE_ANY;
                         if (tt != MI_TYPE_LIST && tt != MI_TYPE_DICT && tt != MI_TYPE_ANY)
                         {
                           s_tc_error(err, e->token, "Indexing requires list or dict");
                           return MI_TYPE_ANY;
                         }
                         return MI_TYPE_ANY;
                       }

    case MI_EXPR_COMMAND:
                       return s_tc_command_expr(script, vm, e, env, err);

    case MI_EXPR_PAIR:
                       return MI_TYPE_ANY;
  }

  return MI_TYPE_ANY;
}

//----------------------------------------------------------
// Function body checking
//----------------------------------------------------------

static MiTypeKind s_tc_typecheck_command_expr(const MiScript* script, MiVm* vm, const MiCommand* c, MiTcEnv* env, MiTypecheckError* err)
{
  MiExpr fake;
  memset(&fake, 0, sizeof(fake));
  fake.kind = MI_EXPR_COMMAND;
  fake.token = c && c->head ? c->head->token : (MiToken){0};
  fake.as.command.head = c ? c->head : NULL;
  fake.as.command.args = c ? c->args : NULL;
  fake.as.command.argc = c ? (unsigned int)c->argc : 0;
  return s_tc_expr(script, vm, &fake, env, err);
}

static bool s_tc_command_definitely_returns(const MiScript* script, MiVm* vm, const MiCommand* c, const MiFuncSig* sig, MiTcEnv* env, MiTypecheckError* err);

static bool s_tc_script_definitely_returns(const MiScript* script, MiVm* vm, const MiScript* body, const MiFuncSig* sig, MiTcEnv* env, MiTypecheckError* err)
{
  const MiCommandList* it = body ? body->first : NULL;
  while (it)
  {
    const MiCommand* c = it->command;
    if (c)
    {
      bool dr = s_tc_command_definitely_returns(script, vm, c, sig, env, err);
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

static bool s_tc_if_definitely_returns(const MiScript* script, MiVm* vm, const MiCommand* c, const MiFuncSig* sig, MiTcEnv* env, MiTypecheckError* err)
{
  // if (cond) {then} else {else} 
  const MiExprList* args = c->args;
  if (!args || !args->expr)
  {
    return false;
  }

  // Typecheck condition 
  (void)s_tc_expr(script, vm, args->expr, env, err);
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
      bool br = s_tc_script_definitely_returns(script, vm, ex->as.block.script, sig, &inner, err);
      if (err && err->message.length > 0)
      {
        return false;
      }
      all_return = all_return && br;
      args = args->next;
      continue;
    }

    // else / elseif are encoded as string literal command heads in args list:
    // we accept 'else' followed by block, or 'elseif' cond block.
    if (ex && ex->kind == MI_EXPR_STRING_LITERAL && s_slice_eq(ex->as.string_lit.value, x_slice_init("else", 4)))
    {
      saw_else = true;
      args = args->next;
      continue;
    }

    if (ex && ex->kind == MI_EXPR_STRING_LITERAL && s_slice_eq(ex->as.string_lit.value, x_slice_init("elseif", 6)))
    {
      args = args->next;
      // cond 
      if (!args)
      {
        break;
      }
      (void)s_tc_expr(script, vm, args->expr, env, err);
      if (err && err->message.length > 0)
      {
        return false;
      }
      args = args->next;
      continue;
    }

    // Unknown piece: typecheck expression and move on. 
    (void)s_tc_expr(script, vm, ex, env, err);
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

static bool s_tc_command_definitely_returns(const MiScript* script, MiVm* vm, const MiCommand* c, const MiFuncSig* sig, MiTcEnv* env, MiTypecheckError* err)
{
  if (!c)
  {
    return false;
  }

  // return special form 
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
      MiTypeKind rt = s_tc_expr(script, vm, c->args ? c->args->expr : NULL, env, err);
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

  // if special form: only used for definite-return; normal typing happens via expr checks below 
  if (c->head && c->head->kind == MI_EXPR_STRING_LITERAL && s_slice_eq(c->head->as.string_lit.value, x_slice_init("if", 2)))
  {
    // We still need to typecheck the full command expression so variable inference stays consistent. 
    (void)s_tc_typecheck_command_expr(script, vm, c, env, err);
    if (err && err->message.length > 0)
    {
      return false;
    }
    return s_tc_if_definitely_returns(script, vm, c, sig, env, err);
  }

  // default: just typecheck as expression 
  (void)s_tc_typecheck_command_expr(script, vm, c, env, err);
  if (err && err->message.length > 0)
  {
    return false;
  }
  return false;
}

static bool s_tc_script_in_func(const MiScript* script, MiVm* vm, const MiScript* body, const MiFuncSig* sig, MiTypecheckError* err)
{
  MiTcEnv env;
  memset(&env, 0, sizeof(env));
  env.cur_func_sig = sig;

  for (int i = 0; i < sig->param_count; ++i)
  {
    s_env_set(&env, sig->params[i].name, sig->params[i].type, sig->params[i].func_sig);
  }

  bool definitely_returns = s_tc_script_definitely_returns(script, vm, body, sig, &env, err);
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

bool mi_typecheck_script(const MiScript* script, MiVm* vm, XSlice dbg_file, MiTypecheckError* err)
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

  // Preload top-level include/import statements into the runtime scope.
  // This allows the typechecker to resolve qualified calls like int::cast
  // via declared signatures on the loaded module namespace members.
  // Note: operators rules are not affected by this.
  s_tc_preload_includes(script, vm, dbg_file);

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

      if (!s_tc_script_in_func(script, vm, block->as.block.script, c->func_sig, err))
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
        (void)s_tc_expr(script, vm, &fake, &env, err);
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

