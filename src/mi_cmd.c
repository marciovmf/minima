#include <stdx_log.h>
#include "mi_cmd.h"
#include "mi_eval_ast.h"
#include "mi_log.h"
#include "mi_runtime.h"
#include <stdlib.h>

//----------------------------------------------------------
// Internal helpers
//----------------------------------------------------------
static void* s_realloc(void* ptr, size_t size)
{
  if (size == 0u)
  {
    free(ptr);
    return NULL;
  }

  void* p = realloc(ptr, size);
  if (!p)
  {
    mi_error("mi_runtime: out of memory\n");
    exit(1);
  }
  return p;
}

//----------------------------------------------------------
// Builtins (value-based)
//----------------------------------------------------------

static void s_print_value(FILE* out, const MiRtValue* v)
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
      }
      break;
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

static MiRtValue s_cmd_set(MiRuntime* rt, const XSlice* head_name, int argc, MiExprList* args)
{
  (void)head_name;

  if (argc < 1 || !args || !args->expr)
  {
    mi_error("set: missing variable name\n");
    return mi_rt_make_void();
  }

  MiRtValue name_val = mi_eval_expr_ast(rt, args->expr);
  if (name_val.kind != MI_RT_VAL_STRING)
  {
    mi_error("set: first argument must be a string (variable name)\n");
    return mi_rt_make_void();
  }

  MiRtValue value = mi_rt_make_void();
  if (argc >= 2 && args->next && args->next->expr)
  {
    value = mi_eval_expr_ast(rt, args->next->expr);
  }

  (void)mi_rt_var_set(rt, name_val.as.s, value);
  return value;
}

static MiRtValue s_cmd_print(MiRuntime* rt, const XSlice* head_name, int argc, MiExprList* args)
{
  (void)rt;
  (void)head_name;

  int i = 0;
  MiExprList* it = args;
  while (it && i < argc)
  {
    if (i > 0)
    {
      fputc(' ', stdout);
    }
    MiRtValue v = mi_eval_expr_ast(rt, it->expr);
    s_print_value(stdout, &v);
    i += 1;
    it = it->next;
  }
  return mi_rt_make_void();
}

static MiRtValue s_cmd_list(MiRuntime* rt, const XSlice* head_name, int argc, MiExprList* args)
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
  MiRtValue sub_val = mi_eval_expr_ast(rt, args->expr);
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

    MiRtValue list_val = mi_eval_expr_ast(rt, list_arg->expr);
    if (list_val.kind != MI_RT_VAL_LIST || !list_val.as.list)
    {
      mi_error("list len: argument must be a list\n");
      return mi_rt_make_void();
    }

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

    MiRtValue list_val  = mi_eval_expr_ast(rt, list_arg->expr);
    MiRtValue value_val = mi_eval_expr_ast(rt, value_arg->expr);

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

static MiRtValue s_exec_block_scoped(MiRuntime* rt, const MiRtBlock* b)
{
  if (!rt->exec_block)
  {
    mi_error("call: no block executor installed\n");
    return mi_rt_make_void();
  }

  mi_rt_scope_push(rt);
  MiRtValue v = rt->exec_block(rt, b);
  mi_rt_scope_pop(rt);
  return v;
}

static MiRtValue s_cmd_call(MiRuntime* rt, const XSlice* head_name, int argc, MiExprList* args)
{
  (void)head_name;

  if (argc < 1 || !args || !args->expr)
  {
    mi_error("call: missing block\n");
    return mi_rt_make_void();
  }

  MiRtValue b = mi_eval_expr_ast(rt, args->expr);
  if (b.kind != MI_RT_VAL_BLOCK)
  {
    mi_error("call: expected block\n");
    return mi_rt_make_void();
  }

  return s_exec_block_scoped(rt, b.as.block);
}

static MiRtValue s_cmd_if(MiRuntime* rt, const XSlice* head_name, int argc, MiExprList* args)
{
  (void)head_name;

  MiRtValue* argv = NULL;
  if (argc > 0)
  {
    argv = (MiRtValue*)x_arena_alloc(rt->current->arena, (size_t)argc * sizeof(MiRtValue));
    if (!argv)
    {
      mi_error("mi_runtime: out of memory\n");
      exit(1);
    }

    MiExprList* it = args;
    int ai = 0;
    while (it && ai < argc)
    {
      argv[ai] = mi_eval_expr_ast(rt, it->expr);
      ai += 1;
      it = it->next;
    }
  }

  int i = 0;
  while (i < argc)
  {
    if (i + 1 >= argc)
    {
      mi_error("if: expected block after condition\n");
      return mi_rt_make_void();
    }

    MiRtValue cond = argv[i];
    MiRtValue then_v = argv[i + 1];

    if (then_v.kind != MI_RT_VAL_BLOCK)
    {
      mi_error("if: expected block after condition\n");
      return mi_rt_make_void();
    }

    if (cond.kind != MI_RT_VAL_BOOL)
    {
      mi_error("if: condition must be boolean\n");
      return mi_rt_make_void();
    }

    if (cond.as.b)
    {
      return s_exec_block_scoped(rt, then_v.as.block);
    }

    i = i + 2;

    if (i >= argc)
    {
      return mi_rt_make_void();
    }

    if (argv[i].kind == MI_RT_VAL_STRING)
    {
      XSlice kw = argv[i].as.s;

      if (x_slice_eq_cstr(kw, "else"))
      {
        if (i + 1 >= argc || argv[i + 1].kind != MI_RT_VAL_BLOCK)
        {
          mi_error("if: expected block after else\n");
          return mi_rt_make_void();
        }
        return s_exec_block_scoped(rt, argv[i + 1].as.block);
      }

      if (x_slice_eq_cstr(kw, "elseif"))
      {
        i = i + 1;
        continue;
      }

      mi_error("if: unknown keyword\n");
      return mi_rt_make_void();
    }

    return mi_rt_make_void();
  }

  return mi_rt_make_void();
}

static MiRtValue s_cmd_while(MiRuntime* rt, const XSlice* head_name, int argc, MiExprList* args)
{
  (void)head_name;

  if (argc < 2 || !args || !args->expr || !args->next || !args->next->expr)
  {
    mi_error("while: missing condition and body\n");
    return mi_rt_make_void();
  }

  MiRtValue argv0;
  if (args->expr->kind == MI_EXPR_BLOCK)
  {
    argv0 = mi_eval_expr_ast(rt, args->expr);
  }
  else
  {
    MiRtBlock* b = mi_rt_block_create(rt);
    b->kind = MI_RT_BLOCK_AST_EXPR;
    b->ptr = args->expr;
    b->id = 0;
    argv0 = mi_rt_make_block(b);
  }

  MiRtValue argv1 = mi_eval_expr_ast(rt, args->next->expr);
  if (argv1.kind != MI_RT_VAL_BLOCK)
  {
    mi_error("while: body must be a block\n");
    return mi_rt_make_void();
  }

  MiRtValue last = mi_rt_make_void();
  const MiRtBlock* body = argv1.as.block;

  // Prefer condition as block, so the backend can re-evaluate it every iteration.
  if (argv0.kind == MI_RT_VAL_BLOCK)
  {
    const MiRtBlock* cond = argv0.as.block;
    for (;;)
    {
      MiRtValue cond_val = s_exec_block_scoped(rt, cond);
      if (cond_val.kind != MI_RT_VAL_BOOL)
      {
        mi_error("while: condition block must yield bool\n");
        return mi_rt_make_void();
      }
      if (!cond_val.as.b)
      {
        break;
      }
      last = s_exec_block_scoped(rt, body);
    }
  }
  else
  {
    // If condition is passed as bool, it is treated as an initial condition only.
    if (argv0.kind != MI_RT_VAL_BOOL)
    {
      mi_error("while: condition must be bool or block\n");
      return mi_rt_make_void();
    }

    while (argv0.as.b)
    {
      last = s_exec_block_scoped(rt, body);
      break;
    }
  }

  return last;
}

static MiRtValue s_cmd_foreach(MiRuntime* rt, const XSlice* head_name, int argc, MiExprList* args)
{
  (void)head_name;

  if (argc != 3)
  {
    mi_error("foreach: expected 3 arguments\n");
    return mi_rt_make_void();
  }

  if (!args || !args->expr || !args->next || !args->next->expr || !args->next->next || !args->next->next->expr)
  {
    mi_error("foreach: expected 3 arguments\n");
    return mi_rt_make_void();
  }

  MiRtValue argv0 = mi_eval_expr_ast(rt, args->expr);
  MiRtValue argv1 = mi_eval_expr_ast(rt, args->next->expr);
  MiRtValue argv2 = mi_eval_expr_ast(rt, args->next->next->expr);

  if (argv0.kind != MI_RT_VAL_STRING)
  {
    mi_error("foreach: expected variable name\n");
    return mi_rt_make_void();
  }

  if (argv1.kind != MI_RT_VAL_LIST)
  {
    mi_error("foreach: expected list\n");
    return mi_rt_make_void();
  }

  if (argv2.kind != MI_RT_VAL_BLOCK)
  {
    mi_error("foreach: expected block\n");
    return mi_rt_make_void();
  }

  MiRtValue last = mi_rt_make_void();
  MiRtList* list = argv1.as.list;
  const MiRtBlock* body = argv2.as.block;

  size_t i;
  for (i = 0u; i < list->count; ++i)
  {
    mi_rt_scope_push(rt);
    (void)mi_rt_var_set(rt, argv0.as.s, list->items[i]);
    last = rt->exec_block ? rt->exec_block(rt, body) : mi_rt_make_void();
    mi_rt_scope_pop(rt);
  }

  return last;
}

static MiRtValue s_cmd_range(MiRuntime* rt, const XSlice* head_name, int argc, MiExprList* args)
{
  (void)head_name;

  long long start = 0;
  long long end = 0;
  long long step = 1;

  if (argc < 1 || !args || !args->expr)
  {
    mi_error("range: missing end\n");
    return mi_rt_make_void();
  }

  MiRtValue argv0 = mi_eval_expr_ast(rt, args->expr);
  MiRtValue argv1 = mi_rt_make_void();
  MiRtValue argv2 = mi_rt_make_void();
  if (argc >= 2 && args->next && args->next->expr)
  {
    argv1 = mi_eval_expr_ast(rt, args->next->expr);
  }
  if (argc >= 3 && args->next && args->next->next && args->next->next->expr)
  {
    argv2 = mi_eval_expr_ast(rt, args->next->next->expr);
  }

  if (argc == 1)
  {
    if (argv0.kind != MI_RT_VAL_INT)
    {
      mi_error("range: expected int\n");
      return mi_rt_make_void();
    }
    end = argv0.as.i;
  }
  else
  {
    if (argv0.kind != MI_RT_VAL_INT || argv1.kind != MI_RT_VAL_INT)
    {
      mi_error("range: expected ints\n");
      return mi_rt_make_void();
    }
    start = argv0.as.i;
    end = argv1.as.i;

    if (argc >= 3)
    {
      if (argv2.kind != MI_RT_VAL_INT)
      {
        mi_error("range: step must be int\n");
        return mi_rt_make_void();
      }
      step = argv2.as.i;
      if (step == 0)
      {
        mi_error("range: step cannot be 0\n");
        return mi_rt_make_void();
      }
    }
  }

  MiRtList* list = mi_rt_list_create(rt);
  if (step > 0)
  {
    long long i;
    for (i = start; i < end; i += step)
    {
      (void)mi_rt_list_push(list, mi_rt_make_int(i));
    }
  }
  else
  {
    long long i;
    for (i = start; i > end; i += step)
    {
      (void)mi_rt_list_push(list, mi_rt_make_int(i));
    }
  }

  return mi_rt_make_list(list);
}

static MiRtValue s_cmd_typeof(MiRuntime* rt, const XSlice* head_name, int argc, MiExprList* args)
{
  (void)rt;
  (void)head_name;

  if (argc < 1 || !args || !args->expr)
  {
    mi_error("typeof: missing value\n");
    return mi_rt_make_void();
  }

  MiRtValue argv0 = mi_eval_expr_ast(rt, args->expr);

  const char* s = "void";
  switch (argv0.kind)
  {
    case MI_RT_VAL_VOID: s = "void"; break;
    case MI_RT_VAL_INT: s = "int"; break;
    case MI_RT_VAL_FLOAT: s = "float"; break;
    case MI_RT_VAL_BOOL: s = "bool"; break;
    case MI_RT_VAL_STRING: s = "string"; break;
    case MI_RT_VAL_LIST: s = "list"; break;
    case MI_RT_VAL_BLOCK: s = "block"; break;
    case MI_RT_VAL_PAIR: s = "pair"; break;
    default: s = "unknown"; break;
  }

  return mi_rt_make_string_slice(x_slice_from_cstr(s));
}


//----------------------------------------------------------
// Public API
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

bool mi_cmd_register(MiRuntime* rt, const char* name, MiRtCommandFn fn)
{
  size_t len;
  size_t i;
  char*  name_copy;

  if (!rt || !name || !fn)
  {
    return false;
  }

  len = strlen(name);

  for (i = 0u; i < rt->command_count; ++i)
  {
    XSlice s = rt->commands[i].name;
    if (s.length == len && (len == 0u || memcmp(s.ptr, name, len) == 0))
    {
      rt->commands[i].fn = fn;
      return true;
    }
  }

  name_copy = NULL;
  if (len > 0u)
  {
    name_copy = (char*)s_realloc(NULL, len);
    memcpy(name_copy, name, len);
  }

  if (rt->command_count == rt->command_capacity)
  {
    size_t new_cap = (rt->command_capacity == 0u) ? 8u : (rt->command_capacity * 2u);
    MiRtUserCommand* new_cmds = (MiRtUserCommand*)s_realloc(rt->commands, new_cap * sizeof(MiRtUserCommand));
    rt->commands = new_cmds;
    rt->command_capacity = new_cap;
  }

  rt->commands[rt->command_count].name.ptr = name_copy;
  rt->commands[rt->command_count].name.length = len;
  rt->commands[rt->command_count].fn = fn;
  rt->command_count = rt->command_count + 1u;

  return true;
}

void mi_cmd_register_builtins(MiRuntime* rt)
{
  (void)mi_cmd_register(rt, "set", s_cmd_set);
  (void)mi_cmd_register(rt, "print", s_cmd_print);
  (void)mi_cmd_register(rt, "list", s_cmd_list);
  (void)mi_cmd_register(rt, "if", s_cmd_if);
  (void)mi_cmd_register(rt, "while", s_cmd_while);
  (void)mi_cmd_register(rt, "foreach", s_cmd_foreach);
  (void)mi_cmd_register(rt, "call", s_cmd_call);
  (void)mi_cmd_register(rt, "range", s_cmd_range);
  (void)mi_cmd_register(rt, "typeof", s_cmd_typeof);
}
