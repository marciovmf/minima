#include "mi_vm.h"
#include "mi_log.h"
#include "mi_fold.h"
#include "mi_mix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdx_strbuilder.h>

//----------------------------------------------------------
// Internal helpers
//----------------------------------------------------------

static MiVmChunk* s_vm_compile_script_ast(MiVm* vm, const MiScript* script, XArena* arena);

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
    mi_error("mi_vm: out of memory\n");
    exit(1);
  }
  return p;
}

static void s_vm_reg_set(MiVm* vm, uint8_t r, MiRtValue v)
{
  mi_rt_value_assign(vm->rt, &vm->regs[r], v);
}

static void s_vm_arg_clear(MiVm* vm)
{
  for (int i = 0; i < vm->arg_top; ++i)
  {
    mi_rt_value_assign(vm->rt, &vm->arg_stack[i], mi_rt_make_void());
  }
  vm->arg_top = 0;
}

static bool s_slice_eq(const XSlice a, const XSlice b)
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

/* Note: the idea is to keep compilation maps as simple linear scans, 
 * avoiding pulling in hash table machinery during early bring-up.
 * Once the instruction set stabilizes, maybe I'll switch to stdx_hashtable.
 */

//----------------------------------------------------------
// VM builtins (minimal starter set)
//----------------------------------------------------------

static MiRtValue s_vm_exec_block_value(MiVm* vm, MiRtValue block_value, const MiVmChunk* caller_chunk, size_t caller_ip);

static void s_vm_call_stack_push(MiVm* vm, MiVmCallFrameKind kind, XSlice name, const MiVmChunk* caller_chunk, size_t caller_ip)
{
  if (!vm)
  {
    return;
  }

  if (vm->call_depth < 0)
  {
    vm->call_depth = 0;
  }
  if (vm->call_depth >= (int)MI_VM_CALL_STACK_MAX)
  {
    /* Drop frames instead of crashing; trace becomes less useful but program runs. */
    return;
  }

  MiVmCallFrame* f = &vm->call_stack[vm->call_depth++];
  f->kind = kind;
  f->name = name;
  f->caller_chunk = caller_chunk;
  f->caller_ip = caller_ip;
}

static void s_vm_call_stack_pop(MiVm* vm)
{
  if (!vm)
  {
    return;
  }
  if (vm->call_depth > 0)
  {
    vm->call_depth -= 1;
  }
}

static const char* s_vm_kind_name(MiRtValueKind kind)
{
  switch (kind)
  {
    case MI_RT_VAL_VOID:   return "()";
    case MI_RT_VAL_INT:    return "int";
    case MI_RT_VAL_FLOAT:  return "float";
    case MI_RT_VAL_BOOL:   return "bool";
    case MI_RT_VAL_STRING: return "string";
    case MI_RT_VAL_LIST:   return "list";
    case MI_RT_VAL_DICT:   return "dict";
    case MI_RT_VAL_BLOCK:  return "block";
    case MI_RT_VAL_CMD:    return "cmd";
    case MI_RT_VAL_KVREF:  return "kvref";
    case MI_RT_VAL_PAIR:   return "pair";
    case MI_RT_VAL_TYPE:   return "type";
    default:               return "unknown";
  }
}

static void s_vm_print_value_inline_depth(const MiRtValue* v, int depth)
{
  if (!v)
  {
    printf("<null>");
    return;
  }

  if (depth > 8)
  {
    printf("...");
    return;
  }

  switch (v->kind)
  {
    case MI_RT_VAL_VOID:   printf("()"); break;
    case MI_RT_VAL_INT:    printf("%lld", v->as.i); break;
    case MI_RT_VAL_FLOAT:  printf("%g", v->as.f); break;
    case MI_RT_VAL_BOOL:   printf("%s", v->as.b ? "true" : "false"); break;
    case MI_RT_VAL_STRING: printf("%.*s", (int)v->as.s.length, v->as.s.ptr); break;
    case MI_RT_VAL_DICT:
    {
      const MiRtDict* d = v->as.dict;
      printf("[dict");
      if (d)
      {
        printf(" %zu", mi_rt_dict_count(d));
      }
      printf("]");
      break;
    }
    case MI_RT_VAL_KVREF:  printf("<kvref>"); break;
    case MI_RT_VAL_BLOCK:  printf("{...}"); break;
    case MI_RT_VAL_PAIR:   printf("<pair>"); break;
    case MI_RT_VAL_TYPE:
    {
      MiRtValueKind k = (MiRtValueKind) v->as.i;
      printf("type:%s", s_vm_kind_name(k));
      break;
    }

    case MI_RT_VAL_LIST:
    {
      const MiRtList* list = v->as.list;
      printf("[");
      if (list)
      {
        for (size_t i = 0u; i < list->count; ++i)
        {
          if (i != 0u)
          {
            printf(" ");
          }
          s_vm_print_value_inline_depth(&list->items[i], depth + 1);
        }
      }
      printf("]");
      break;
    }

    default:
      printf("<unknown>");
      break;
  }
}

static void s_vm_print_value_inline(const MiRtValue* v)
{
  s_vm_print_value_inline_depth(v, 0);
}

static void s_vm_value_to_string(char* out, size_t cap, const MiRtValue* v)
{
  if (!out || cap == 0u)
  {
    return;
  }

  out[0] = '\0';

  if (!v)
  {
    (void)snprintf(out, cap, "<null>");
    return;
  }

  switch (v->kind)
  {
    case MI_RT_VAL_VOID:   (void)snprintf(out, cap, "()"); break;
    case MI_RT_VAL_INT:    (void)snprintf(out, cap, "%lld", v->as.i); break;
    case MI_RT_VAL_FLOAT:  (void)snprintf(out, cap, "%g", v->as.f); break;
    case MI_RT_VAL_BOOL:   (void)snprintf(out, cap, "%s", v->as.b ? "true" : "false"); break;
    case MI_RT_VAL_STRING: (void)snprintf(out, cap, "%.*s", (int)v->as.s.length, v->as.s.ptr); break;
    case MI_RT_VAL_LIST:   (void)snprintf(out, cap, "[list]"); break;
    case MI_RT_VAL_DICT:   (void)snprintf(out, cap, "[dict]"); break;
    case MI_RT_VAL_KVREF:  (void)snprintf(out, cap, "<kvref>"); break;
    case MI_RT_VAL_BLOCK:  (void)snprintf(out, cap, "{...}"); break;
    case MI_RT_VAL_PAIR:   (void)snprintf(out, cap, "<pair>"); break;
    case MI_RT_VAL_TYPE:   (void)snprintf(out, cap, "type:%s", s_vm_kind_name((MiRtValueKind)v->as.i)); break;
    default:               (void)snprintf(out, cap, "<unknown>"); break;
  }
}

static MiRtValue s_vm_cmd_dict(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)name;
  if (!vm || !vm->rt)
  {
    return mi_rt_make_void();
  }

  if (argc != 1)
  {
    mi_error("dict: expected 1 argument\n");
    return mi_rt_make_void();
  }

  /* Identity cast: dict:[k=v, ...] where the literal already produced a dict. */
  if (argv[0].kind == MI_RT_VAL_DICT && argv[0].as.dict)
  {
    return argv[0];
  }

  /* Backwards compatibility: dict: (list of [k, v] entries). */
  if (argv[0].kind == MI_RT_VAL_LIST && argv[0].as.list)
  {
    MiRtDict* d = mi_rt_dict_create(vm->rt);
    if (!d)
    {
      return mi_rt_make_void();
    }

    MiRtList* entries = argv[0].as.list;
    for (size_t i = 0u; i < entries->count; ++i)
    {
      MiRtValue kv = entries->items[i];
      if (kv.kind != MI_RT_VAL_LIST || !kv.as.list || kv.as.list->count != 2u)
      {
        mi_error("dict: each entry must be a 2-element list [k, v]\n");
        continue;
      }
      MiRtValue k = kv.as.list->items[0];
      MiRtValue v = kv.as.list->items[1];
      (void) mi_rt_dict_set(vm->rt, d, k, v);
    }

    return mi_rt_make_dict(d);
  }

  mi_error("dict: argument must be a dict literal or a list of [k, v] entries\n");
  return mi_rt_make_void();
}

static MiRtValue s_vm_cmd_list(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)name;

  if (argc != 1)
  {
    mi_error("list: expected 1 argument\n");
    return mi_rt_make_void();
  }

  if (argv[0].kind != MI_RT_VAL_LIST || !argv[0].as.list)
  {
    mi_error("list: argument must be a list\n");
    return mi_rt_make_void();
  }

  return argv[0];
}

static MiRtValue s_vm_cmd_len(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  if (!vm || !vm->rt)
  {
    return mi_rt_make_void();
  }

  (void)name;

  if (argc != 1)
  {
    mi_error("len: expected 1 argument\n");
    return mi_rt_make_void();
  }

  MiRtValue v = argv[0];

  if (v.kind == MI_RT_VAL_LIST && v.as.list)
  {
    return mi_rt_make_int((int64_t)v.as.list->count);
  }

  if (v.kind == MI_RT_VAL_PAIR && v.as.pair)
  {
    return mi_rt_make_int(2);
  }

  if (v.kind == MI_RT_VAL_DICT && v.as.dict)
  {
    return mi_rt_make_int((int64_t)mi_rt_dict_count(v.as.dict));
  }

  if (v.kind == MI_RT_VAL_KVREF)
  {
    return mi_rt_make_int(2);
  }

  if (v.kind == MI_RT_VAL_STRING && v.as.s.ptr)
  {
    return mi_rt_make_int((int64_t)v.as.s.length);
  }

  mi_error("len: unsupported type\n");
  return mi_rt_make_void();
}

static bool s_vm_is_truthy(const MiRtValue* v)
{
  if (!v)
  {
    return false;
  }

  switch (v->kind)
  {
    case MI_RT_VAL_VOID:  return false;
    case MI_RT_VAL_BOOL:  return v->as.b;
    case MI_RT_VAL_INT:   return v->as.i != 0;
    case MI_RT_VAL_FLOAT: return v->as.f != 0.0;
    default:              return true;
  }
}

static MiRtValue s_vm_cmd_warning(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)name;

  XStrBuilder* sb = x_strbuilder_create();
  if (!sb)
  {
    mi_warning("warning: out of memory\n");
    return mi_rt_make_void();
  }

  for (int i = 0; i < argc; ++i)
  {
    if (i != 0)
    {
      (void)x_strbuilder_append_char(sb, ' ');
    }

    char vbuf[256];
    s_vm_value_to_string(vbuf, sizeof(vbuf), &argv[i]);
    (void)x_strbuilder_append(sb, vbuf);
  }
  (void)x_strbuilder_append_char(sb, '\n');

  mi_warning_fmt("%s", x_strbuilder_to_string(sb));
  x_strbuilder_destroy(sb);
  return mi_rt_make_void();
}

static MiRtValue s_vm_cmd_error(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)name;

  XStrBuilder* sb = x_strbuilder_create();
  if (!sb)
  {
    mi_error("error: out of memory\n");
    return mi_rt_make_void();
  }

  for (int i = 0; i < argc; ++i)
  {
    if (i != 0)
    {
      (void)x_strbuilder_append_char(sb, ' ');
    }

    char vbuf[256];
    s_vm_value_to_string(vbuf, sizeof(vbuf), &argv[i]);
    (void)x_strbuilder_append(sb, vbuf);
  }
  (void)x_strbuilder_append_char(sb, '\n');

  mi_error_fmt("%s", x_strbuilder_to_string(sb));
  x_strbuilder_destroy(sb);
  return mi_rt_make_void();
}

static MiRtValue s_vm_cmd_fatal(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)name;

  (void)s_vm_cmd_error(vm, name, argc, argv);
  exit(1);
}

static MiRtValue s_vm_cmd_assert(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)name;

  if (argc < 1 || argc > 2)
  {
    mi_error("assert: expected 1 or 2 arguments\n");
    return mi_rt_make_void();
  }

  bool ok = s_vm_is_truthy(&argv[0]);
  if (ok)
  {
    return mi_rt_make_void();
  }

  if (argc == 2)
  {
    if (argv[1].kind == MI_RT_VAL_STRING)
    {
      MiRtValue one;
      one.kind = MI_RT_VAL_STRING;
      one.as.s = argv[1].as.s;
      (void)s_vm_cmd_fatal(vm, name, 1, &one);
    }
    else
    {
      (void)s_vm_cmd_fatal(vm, name, 1, &argv[1]);
    }
  }

  mi_error("assert: failed\n");
  exit(1);
}

static MiRtValue s_vm_cmd_type(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)name;

  if (argc != 1)
  {
    mi_error("type: expected 1 argument\n");
    return mi_rt_make_void();
  }

  if (argv[0].kind != MI_RT_VAL_STRING)
  {
    mi_error("type: argument must be a type name string\n");
    return mi_rt_make_void();
  }

  XSlice s = argv[0].as.s;
  if (s_slice_eq(s, x_slice_from_cstr("()")) || s_slice_eq(s, x_slice_from_cstr("void")))
  {
    return mi_rt_make_type(MI_RT_VAL_VOID);
  }
  if (s_slice_eq(s, x_slice_from_cstr("int")))
  {
    return mi_rt_make_type(MI_RT_VAL_INT);
  }
  if (s_slice_eq(s, x_slice_from_cstr("float")))
  {
    return mi_rt_make_type(MI_RT_VAL_FLOAT);
  }
  if (s_slice_eq(s, x_slice_from_cstr("bool")))
  {
    return mi_rt_make_type(MI_RT_VAL_BOOL);
  }
  if (s_slice_eq(s, x_slice_from_cstr("string")))
  {
    return mi_rt_make_type(MI_RT_VAL_STRING);
  }
  if (s_slice_eq(s, x_slice_from_cstr("list")))
  {
    return mi_rt_make_type(MI_RT_VAL_LIST);
  }
  if (s_slice_eq(s, x_slice_from_cstr("dict")))
  {
    return mi_rt_make_type(MI_RT_VAL_DICT);
  }
  if (s_slice_eq(s, x_slice_from_cstr("block")))
  {
    return mi_rt_make_type(MI_RT_VAL_BLOCK);
  }

  mi_error("type: unknown type name\n");
  return mi_rt_make_void();
}

static MiRtValue s_vm_cmd_typeof(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)name;

  if (argc != 1)
  {
    mi_error("typeof: expected 1 argument\n");
    return mi_rt_make_void();
  }

  return mi_rt_make_type(argv[0].kind);
}

static MiRtValue s_vm_cmd_print(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void) vm;
  (void) name;

  for (int i = 0; i < argc; ++i)
  {
    if (i != 0)
    {
      printf(" ");
    }
    s_vm_print_value_inline(&argv[i]);
  }
  printf("\n");
  return mi_rt_make_void();
}

static const char* s_op_name(MiVmOp op);

static void s_vm_trace_print_frame(const MiVmChunk* chunk, size_t ip, const char* label)
{
  if (!chunk)
  {
    printf("  %s <no-chunk>\n", label ? label : "");
    return;
  }

  if (ip >= chunk->code_count)
  {
    printf("  %s ip=%zu <out-of-range>\n", label ? label : "", ip);
    return;
  }

  const MiVmIns ins = chunk->code[ip];
  const char* opname = s_op_name((MiVmOp)ins.op);
  printf("  %s ip=%zu %s a=%u b=%u c=%u imm=%d\n",
    label ? label : "",
    ip,
    opname ? opname : "<?>",
    (unsigned)ins.a,
    (unsigned)ins.b,
    (unsigned)ins.c,
    (int)ins.imm);
}

static void s_vm_trace_print(MiVm* vm)
{
  if (!vm)
  {
    return;
  }

  printf("Stack trace (most recent call first):\n");

  /* Current frame: the instruction that invoked trace:. */
  s_vm_trace_print_frame(vm->dbg_chunk, vm->dbg_ip, "#0");

  int idx = 1;
  for (int i = vm->call_depth - 1; i >= 0; --i)
  {
    const MiVmCallFrame* f = &vm->call_stack[i];
    char label[128];
    if (f->kind == MI_VM_CALL_FRAME_USER_CMD)
    {
      snprintf(label, sizeof(label), "#%d user:%.*s", idx, (int)f->name.length, f->name.ptr);
    }
    else
    {
      snprintf(label, sizeof(label), "#%d call", idx);
    }
    s_vm_trace_print_frame(f->caller_chunk, f->caller_ip, label);
    idx += 1;
  }
}

static MiRtValue s_vm_cmd_trace(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)name;
  (void)argv;

  if (argc != 0)
  {
    mi_error("trace: expected 0 arguments\n");
    return mi_rt_make_void();
  }

  s_vm_trace_print(vm);
  return mi_rt_make_void();
}

static MiRtValue s_vm_cmd_set(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)name;
  if (!vm || !vm->rt)
  {
    return mi_rt_make_void();
  }

  if (argc != 2)
  {
    mi_error("set: expected 2 arguments\n");
    return mi_rt_make_void();
  }

  if (argv[0].kind != MI_RT_VAL_STRING)
  {
    mi_error("set: first argument must be a string variable name\n");
    return mi_rt_make_void();
  }

  (void) mi_rt_var_set(vm->rt, argv[0].as.s, argv[1]);
  return argv[1];
}

static MiRtValue s_vm_cmd_call(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)name;
  if (argc != 1)
  {
    mi_error("call: expected 1 argument\n");
    return mi_rt_make_void();
  }

  return s_vm_exec_block_value(vm, argv[0], vm->dbg_chunk, vm->dbg_ip);
}

static MiRtValue s_vm_exec_cmd_value(MiVm* vm, XSlice cmd_name, MiRtValue cmd_value, int argc, const MiRtValue* argv)
{
  if (!vm || !vm->rt)
  {
    return mi_rt_make_void();
  }

  if (cmd_value.kind != MI_RT_VAL_CMD || !cmd_value.as.cmd)
  {
    return mi_rt_make_void();
  }

  MiRtCmd* c = cmd_value.as.cmd;
  if (c->body.kind != MI_RT_VAL_BLOCK || !c->body.as.block || c->body.as.block->kind != MI_RT_BLOCK_VM_CHUNK)
  {
    mi_error("mi_vm: invalid cmd body\n");
    return mi_rt_make_void();
  }

  if ((uint32_t)argc != c->param_count)
  {
    mi_error_fmt("%.*s: expected %u args, got %d\n", (int)cmd_name.length, cmd_name.ptr, (unsigned)c->param_count, argc);
    return mi_rt_make_void();
  }

  MiRtBlock* b = c->body.as.block;
  MiVmChunk* sub = (MiVmChunk*)b->ptr;
  MiScopeFrame* caller = vm->rt->current;
  MiScopeFrame* parent = b->env ? b->env : caller;

  /* Preserve VM-reserved regs across call (same as call:). */
  MiRtValue saved_vm_regs[7];
  for (int i = 0; i < 7; ++i)
  {
    saved_vm_regs[i] = vm->regs[1 + i];
    mi_rt_value_retain(vm->rt, saved_vm_regs[i]);
  }

  mi_rt_scope_push_with_parent(vm->rt, parent);

  for (uint32_t i = 0; i < c->param_count; ++i)
  {
    (void)mi_rt_var_define(vm->rt, c->param_names[i], argv[(int)i]);
  }

  s_vm_call_stack_push(vm, MI_VM_CALL_FRAME_USER_CMD, cmd_name, vm->dbg_chunk, vm->dbg_ip);
  MiRtValue ret = mi_vm_execute(vm, sub);
  s_vm_call_stack_pop(vm);

  mi_rt_scope_pop(vm->rt);
  vm->rt->current = caller;

  for (int i = 0; i < 7; ++i)
  {
    mi_rt_value_assign(vm->rt, &vm->regs[1 + i], saved_vm_regs[i]);
    mi_rt_value_release(vm->rt, saved_vm_regs[i]);
  }
  s_vm_arg_clear(vm);

  return ret;
}

static MiRtValue s_vm_exec_block_value(MiVm* vm, MiRtValue block_value, const MiVmChunk* caller_chunk, size_t caller_ip)
{
  if (!vm || !vm->rt)
  {
    return mi_rt_make_void();
  }

  if (block_value.kind != MI_RT_VAL_BLOCK || !block_value.as.block)
  {
    mi_error("call: expected block");
    return mi_rt_make_void();
  }

  MiRtBlock* b = block_value.as.block;
  if (b->kind != MI_RT_BLOCK_VM_CHUNK || !b->ptr)
  {
    mi_error("call: expected VM block");
    return mi_rt_make_void();
  }

  MiVmChunk* sub = (MiVmChunk*)b->ptr;
  MiScopeFrame* caller = vm->rt->current;
  MiScopeFrame* parent = b->env ? b->env : caller;

  s_vm_call_stack_push(vm, MI_VM_CALL_FRAME_BLOCK, x_slice_init(NULL, 0), caller_chunk, caller_ip);

  /* CALL ABI:
     - r0 is return value
     - r1-r7 are VM-reserved and preserved across calls
     - r8+ are caller-saved (clobbered)
     - arg stack is not preserved across calls */
  MiRtValue saved_vm_regs[7];
  for (int i = 0; i < 7; ++i)
  {
    saved_vm_regs[i] = vm->regs[1 + i];
    mi_rt_value_retain(vm->rt, saved_vm_regs[i]);
  }

  mi_rt_scope_push_with_parent(vm->rt, parent);
  MiRtValue ret = mi_vm_execute(vm, sub);
  mi_rt_scope_pop(vm->rt);
  /* Restore caller scope even when the block's lexical parent differs. */
  vm->rt->current = caller;

  for (int i = 0; i < 7; ++i)
  {
    mi_rt_value_assign(vm->rt, &vm->regs[1 + i], saved_vm_regs[i]);
    mi_rt_value_release(vm->rt, saved_vm_regs[i]);
  }
  s_vm_arg_clear(vm);

  s_vm_call_stack_pop(vm);

  return ret;
}

/* cmd: <name> <param_name_0> <param_name_1> ... <param_name_n> <block>
   This is emitted by the compiler as a special form so it does not need to build
   a list/dict at runtime for parameters.
   */
static MiRtValue s_vm_cmd_cmd(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)name;
  if (!vm || !vm->rt)
  {
    return mi_rt_make_void();
  }

  if (argc < 2)
  {
    mi_error("cmd: expected at least 2 arguments\n");
    return mi_rt_make_void();
  }

  if (argv[0].kind != MI_RT_VAL_STRING)
  {
    mi_error("cmd: first argument must be command name string\n");
    return mi_rt_make_void();
  }

  MiRtValue body = argv[argc - 1];
  if (body.kind != MI_RT_VAL_BLOCK)
  {
    mi_error("cmd: last argument must be a block\n");
    return mi_rt_make_void();
  }

  uint32_t param_count = (argc > 2) ? (uint32_t)(argc - 2) : 0u;
  XSlice* param_names = NULL;
  if (param_count > 0)
  {
    param_names = (XSlice*)calloc(param_count, sizeof(*param_names));
    if (!param_names)
    {
      free(param_names);
      mi_error("cmd: out of memory\n");
      return mi_rt_make_void();
    }
  }

  for (uint32_t i = 0; i < param_count; ++i)
  {
    MiRtValue pn = argv[1 + (int)i];
    if (pn.kind != MI_RT_VAL_STRING)
    {
      mi_error("cmd: parameter name must be string\n");
      free(param_names);
      return mi_rt_make_void();
    }

    param_names[i] = pn.as.s;
  }

  MiRtCmd* c = mi_rt_cmd_create(vm->rt, param_count, param_names, body);
  free(param_names);
  if (!c)
  {
    mi_error("cmd: out of memory\n");
    return mi_rt_make_void();
  }

  /* Store command object in the current scope. */
  (void)mi_rt_var_set(vm->rt, argv[0].as.s, mi_rt_make_cmd(c));
  return mi_rt_make_void();
}

static bool s_slice_ends_with(XSlice s, const char* suffix)
{
  if (!suffix)
  {
    return false;
  }
  size_t n = strlen(suffix);
  if ((size_t)s.length < n)
  {
    return false;
  }
  const char* p = (const char*)s.ptr;
  return memcmp(p + (s.length - n), suffix, n) == 0;
}

static bool s_slice_find_double_colon(XSlice s, size_t start, size_t* out_index)
{
  const char* p = (const char*)s.ptr;
  size_t n = s.length;
  if (start >= n)
  {
    return false;
  }
  for (size_t i = start; i + 1u < n; ++i)
  {
    if (p[i] == ':' && p[i + 1u] == ':')
    {
      if (out_index)
      {
        *out_index = i;
      }
      return true;
    }
  }
  return false;
}

static MiRtValue s_vm_exec_qualified_cmd(MiVm* vm, XSlice full_name, int argc, const MiRtValue* argv, bool* out_ok)
{
  if (out_ok)
  {
    *out_ok = false;
  }
  if (!vm || !vm->rt)
  {
    return mi_rt_make_void();
  }

  size_t i0 = 0u;
  size_t dc = 0u;
  if (!s_slice_find_double_colon(full_name, 0u, &dc))
  {
    return mi_rt_make_void();
  }

  /* Resolve first segment from current scope chain. */
  XSlice seg = { full_name.ptr, dc };
  MiRtValue cur = mi_rt_make_void();
  if (!mi_rt_var_get(vm->rt, seg, &cur))
  {
    mi_error_fmt("%.*s: unknown module/namespace '%.*s'\n", (int)full_name.length, (const char*)full_name.ptr, (int)seg.length, (const char*)seg.ptr);
    return mi_rt_make_void();
  }

  /* Walk remaining segments inside chunk environments. */
  i0 = dc + 2u;
  while (i0 < full_name.length)
  {
    size_t next_dc = 0u;
    bool has_next = s_slice_find_double_colon(full_name, i0, &next_dc);
    size_t seg_len = has_next ? (next_dc - i0) : (full_name.length - i0);
    XSlice name = { (const char*)full_name.ptr + i0, seg_len };

    if (cur.kind != MI_RT_VAL_BLOCK || !cur.as.block || !cur.as.block->env)
    {
      mi_error_fmt("%.*s: '%.*s' is not a chunk/module\n", (int)full_name.length, (const char*)full_name.ptr, (int)seg.length, (const char*)seg.ptr);
      return mi_rt_make_void();
    }

    MiRtValue v = mi_rt_make_void();
    if (!mi_rt_var_get_from(cur.as.block->env, name, &v))
    {
      mi_error_fmt("%.*s: unknown member '%.*s'\n", (int)full_name.length, (const char*)full_name.ptr, (int)name.length, (const char*)name.ptr);
      return mi_rt_make_void();
    }

    if (!has_next)
    {
      if (v.kind != MI_RT_VAL_CMD)
      {
        mi_error_fmt("%.*s: '%.*s' is not a command\n", (int)full_name.length, (const char*)full_name.ptr, (int)name.length, (const char*)name.ptr);
        return mi_rt_make_void();
      }
      if (out_ok)
      {
        *out_ok = true;
      }
      return s_vm_exec_cmd_value(vm, full_name, v, argc, argv);
    }

    /* Continue chaining through nested chunks. */
    cur = v;
    seg = name;
    i0 = next_dc + 2u;
  }

  return mi_rt_make_void();
}

static MiRtValue s_vm_cmd_include(MiVm* vm, XSlice name, int argc, const MiRtValue* argv)
{
  (void)name;
  if (!vm || !vm->rt)
  {
    return mi_rt_make_void();
  }

  if (argc != 3)
  {
    mi_error("include: expected: include: <module> as <alias>\n");
    return mi_rt_make_void();
  }

  if (argv[0].kind != MI_RT_VAL_STRING || argv[1].kind != MI_RT_VAL_STRING || argv[2].kind != MI_RT_VAL_STRING)
  {
    mi_error("include: arguments must be strings\n");
    return mi_rt_make_void();
  }

  if (!s_slice_eq(argv[1].as.s, x_slice_from_cstr("as")))
  {
    mi_error("include: expected second argument to be 'as'\n");
    return mi_rt_make_void();
  }

  XSlice module = argv[0].as.s;
  XSlice alias = argv[2].as.s;

  char* filename = NULL;
  if (s_slice_ends_with(module, ".mx"))
  {
    filename = (char*)calloc(module.length + 1u, 1u);
    if (!filename)
    {
      mi_error("include: out of memory\n");
      return mi_rt_make_void();
    }
    memcpy(filename, module.ptr, module.length);
  }
  else
  {
    filename = (char*)calloc(module.length + 1u + 3u, 1u);
    if (!filename)
    {
      mi_error("include: out of memory\n");
      return mi_rt_make_void();
    }
    memcpy(filename, module.ptr, module.length);
    memcpy(filename + module.length, ".mx", 3u);
  }

  MiMixProgram prog;
  memset(&prog, 0, sizeof(prog));
  if (!mi_mix_load_file(vm, filename, &prog))
  {
    mi_error_fmt("include: failed to load module: %s\n", filename);
    free(filename);
    return mi_rt_make_void();
  }
  free(filename);

  /* Track loaded module so it can be freed on VM shutdown. */
  if (vm->module_count == vm->module_capacity)
  {
    size_t new_cap = vm->module_capacity ? vm->module_capacity * 2u : 8u;
    vm->modules = (MiMixProgram*)s_realloc(vm->modules, new_cap * sizeof(*vm->modules));
    vm->module_capacity = new_cap;
  }
  vm->modules[vm->module_count] = prog;
  vm->module_count += 1u;

  /* Create a detached environment for the module, run its top-level, then bind alias to a block capturing that env. */
  MiScopeFrame* env = mi_rt_scope_create_detached(vm->rt, NULL);
  if (vm->module_env_count == vm->module_env_capacity)
  {
    size_t new_cap = vm->module_env_capacity ? vm->module_env_capacity * 2u : 8u;
    vm->module_envs = (MiScopeFrame**)s_realloc(vm->module_envs, new_cap * sizeof(*vm->module_envs));
    vm->module_env_capacity = new_cap;
  }
  vm->module_envs[vm->module_env_count++] = env;

  MiScopeFrame* saved = vm->rt->current;
  vm->rt->current = env;
  (void)mi_vm_execute(vm, prog.entry);
  vm->rt->current = saved;

  MiRtBlock* b = mi_rt_block_create(vm->rt);
  b->kind = MI_RT_BLOCK_VM_CHUNK;
  b->ptr = (void*)prog.entry;
  b->env = env;

  MiRtValue block_v = mi_rt_make_block(b);
  (void)mi_rt_var_set(vm->rt, alias, block_v);
  return block_v;
}


//----------------------------------------------------------
// VM init/shutdown
//----------------------------------------------------------

void mi_vm_init(MiVm* vm, MiRuntime* rt)
{
  memset(vm, 0, sizeof(*vm));
  vm->rt = rt;
  vm->arg_top = 0;

  for (int i = 0; i < MI_VM_REG_COUNT; ++i)
  {
    vm->regs[i] = mi_rt_make_void();
  }
  for (int i = 0; i < MI_VM_ARG_STACK_COUNT; ++i)
  {
    vm->arg_stack[i] = mi_rt_make_void();
  }
  for (int f = 0; f < MI_VM_ARG_FRAME_MAX; ++f)
  {
    vm->arg_frame_tops[f] = 0;
    for (int i = 0; i < MI_VM_ARG_STACK_COUNT; ++i)
    {
      vm->arg_frames[f][i] = mi_rt_make_void();
    }
  }
  vm->arg_frame_depth = 0;

  // Minimal builtins to get started.
  (void) mi_vm_register_command(vm, x_slice_from_cstr("print"), s_vm_cmd_print);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("warning"), s_vm_cmd_warning);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("error"),   s_vm_cmd_error);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("fatal"),   s_vm_cmd_fatal);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("assert"),  s_vm_cmd_assert);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("type"),    s_vm_cmd_type);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("t"),       s_vm_cmd_type);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("typeof"),  s_vm_cmd_typeof);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("set"),   s_vm_cmd_set);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("call"),  s_vm_cmd_call);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("list"),  s_vm_cmd_list);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("dict"),  s_vm_cmd_dict);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("len"),   s_vm_cmd_len);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("cmd"),   s_vm_cmd_cmd);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("include"), s_vm_cmd_include);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("trace"), s_vm_cmd_trace);
}

void mi_vm_shutdown(MiVm* vm)
{
  if (!vm)
  {
    return;
  }

  for (int i = 0; i < MI_VM_REG_COUNT; ++i)
  {
    mi_rt_value_release(vm->rt, vm->regs[i]);
    vm->regs[i] = mi_rt_make_void();
  }
  s_vm_arg_clear(vm);

  free(vm->commands);
  vm->commands = NULL;
  vm->command_count = 0;
  vm->command_capacity = 0;

  if (vm->modules)
  {
    for (size_t i = 0; i < vm->module_count; ++i)
    {
      mi_mix_program_destroy(&vm->modules[i]);
    }
    free(vm->modules);
  }
  vm->modules = NULL;
  vm->module_count = 0u;
  vm->module_capacity = 0u;

  if (vm->module_envs)
  {
    for (size_t i = 0; i < vm->module_env_count; ++i)
    {
      mi_rt_scope_destroy_detached(vm->rt, vm->module_envs[i]);
    }
    free(vm->module_envs);
  }
  vm->module_envs = NULL;
  vm->module_env_count = 0u;
  vm->module_env_capacity = 0u;
}

bool mi_vm_register_command(MiVm* vm, XSlice name, MiVmCommandFn fn)
{
  if (!vm || !fn)
  {
    return false;
  }

  for (size_t i = 0; i < vm->command_count; ++i)
  {
    if (s_slice_eq(vm->commands[i].name, name))
    {
      vm->commands[i].fn = fn;
      return true;
    }
  }

  if (vm->command_count == vm->command_capacity)
  {
    size_t new_cap = vm->command_capacity ? vm->command_capacity * 2u : 32u;
    vm->commands = (MiVmCommandEntry*) s_realloc(vm->commands, new_cap * sizeof(*vm->commands));
    vm->command_capacity = new_cap;
  }

  vm->commands[vm->command_count].name = name;
  vm->commands[vm->command_count].fn = fn;
  vm->command_count++;
  return true;
}

//----------------------------------------------------------
// Chunk helpers
//----------------------------------------------------------

static MiVmChunk* s_chunk_create(void)
{
  MiVmChunk* c = (MiVmChunk*) calloc(1, sizeof(*c));
  if (!c)
  {
    mi_error("mi_vm: out of memory\n");
    exit(1);
  }
  return c;
}

static void s_vm_chunk_destroy_ex(MiVmChunk* chunk, MiVmChunk** stack, size_t depth)
{
  if (!chunk)
  {
    return;
  }

  for (size_t i = 0; i < depth; ++i)
  {
    if (stack[i] == chunk)
    {
      /* Cycle detected; avoid infinite recursion. */
      return;
    }
  }

  MiVmChunk* local_stack[128];
  if (depth < (sizeof(local_stack) / sizeof(local_stack[0])))
  {
    for (size_t i = 0; i < depth; ++i)
    {
      local_stack[i] = stack[i];
    }
    local_stack[depth] = chunk;
    stack = local_stack;
    depth += 1;
  }

  if (chunk->subchunks)
  {
    for (size_t i = 0; i < chunk->subchunk_count; ++i)
    {
      s_vm_chunk_destroy_ex(chunk->subchunks[i], stack, depth);
    }
    free(chunk->subchunks);
  }

  free(chunk->code);
  free(chunk->consts);
  free(chunk->symbols);
  free(chunk->cmd_fns);
  free(chunk->cmd_names);
  free(chunk);
}

void mi_vm_chunk_destroy(MiVmChunk* chunk)
{
  s_vm_chunk_destroy_ex(chunk, NULL, 0);
}

static void s_chunk_emit(MiVmChunk* c, MiVmOp op, uint8_t a, uint8_t b, uint8_t d, int32_t imm)
{
  if (c->code_count == c->code_capacity)
  {
    size_t new_cap = c->code_capacity ? c->code_capacity * 2u : 256u;
    c->code = (MiVmIns*) s_realloc(c->code, new_cap * sizeof(*c->code));
    c->code_capacity = new_cap;
  }

  MiVmIns ins;
  ins.op = (uint8_t) op;
  ins.a = a;
  ins.b = b;
  ins.c = d;
  ins.imm = imm;
  c->code[c->code_count++] = ins;
}

static bool s_chunk_const_eq(MiRtValue a, MiRtValue b)
{
  if (a.kind != b.kind)
  {
    return false;
  }

  switch (a.kind)
  {
    case MI_RT_VAL_VOID:   return true;
    case MI_RT_VAL_BOOL:   return a.as.b == b.as.b;
    case MI_RT_VAL_INT:    return a.as.i == b.as.i;
    case MI_RT_VAL_FLOAT:  return a.as.f == b.as.f;
    case MI_RT_VAL_STRING: return s_slice_eq(a.as.s, b.as.s);
    default:               return false;
  }
}

static int32_t s_chunk_add_const(MiVmChunk* c, MiRtValue v)
{
  for (size_t i = 0; i < c->const_count; ++i)
  {
    if (s_chunk_const_eq(c->consts[i], v))
    {
      return (int32_t) i;
    }
  }

  if (c->const_count == c->const_capacity)
  {
    size_t new_cap = c->const_capacity ? c->const_capacity * 2u : 64u;
    c->consts = (MiRtValue*) s_realloc(c->consts, new_cap * sizeof(*c->consts));
    c->const_capacity = new_cap;
  }

  c->consts[c->const_count] = v;
  return (int32_t) c->const_count++;
}

static int32_t s_chunk_add_symbol(MiVmChunk* c, XSlice name)
{
  for (size_t i = 0; i < c->symbol_count; ++i)
  {
    if (s_slice_eq(c->symbols[i], name))
    {
      return (int32_t) i;
    }
  }

  if (c->symbol_count == c->symbol_capacity)
  {
    size_t new_cap = c->symbol_capacity ? c->symbol_capacity * 2u : 64u;
    c->symbols = (XSlice*) s_realloc(c->symbols, new_cap * sizeof(*c->symbols));
    c->symbol_capacity = new_cap;
  }

  int32_t idx = (int32_t) c->symbol_count;
  c->symbols[c->symbol_count++] = name;
  return idx;
}

static MiVmCommandFn s_vm_find_command_fn(MiVm* vm, XSlice name)
{
  for (size_t i = 0; i < vm->command_count; ++i)
  {
    if (s_slice_eq(vm->commands[i].name, name))
    {
      return vm->commands[i].fn;
    }
  }

  return NULL;
}

static int32_t s_chunk_add_cmd_fn(MiVmChunk* c, XSlice name, MiVmCommandFn fn)
{
  for (size_t i = 0; i < c->cmd_count; ++i)
  {
    if (s_slice_eq(c->cmd_names[i], name))
    {
      return (int32_t) i;
    }
  }

  if (c->cmd_count == c->cmd_capacity)
  {
    size_t new_cap = c->cmd_capacity ? c->cmd_capacity * 2u : 32u;
    c->cmd_fns = (MiVmCommandFn*) s_realloc(c->cmd_fns, new_cap * sizeof(*c->cmd_fns));
    c->cmd_names = (XSlice*) s_realloc(c->cmd_names, new_cap * sizeof(*c->cmd_names));
    c->cmd_capacity = new_cap;
  }

  int32_t idx = (int32_t) c->cmd_count;
  c->cmd_fns[c->cmd_count] = fn;
  c->cmd_names[c->cmd_count] = name;
  c->cmd_count++;
  return idx;
}

static int32_t s_chunk_add_subchunk(MiVmChunk* c, MiVmChunk* sub)
{
  if (!c || !sub)
  {
    return -1;
  }

  if (c->subchunk_count == c->subchunk_capacity)
  {
    size_t new_cap = c->subchunk_capacity ? c->subchunk_capacity * 2u : 16u;
    c->subchunks = (MiVmChunk**)s_realloc(c->subchunks, new_cap * sizeof(*c->subchunks));
    c->subchunk_capacity = new_cap;
  }

  int32_t idx = (int32_t)c->subchunk_count;
  c->subchunks[c->subchunk_count++] = sub;
  return idx;
}

//----------------------------------------------------------
// Compiler (AST -> bytecode)
//----------------------------------------------------------

typedef struct MiVmBuild
{
  MiVm*       vm;
  MiVmChunk*  chunk;
  uint8_t     next_reg;
  uint8_t     reg_base;

  /* Loop context stack (compiler-only). Needed for break/continue. */
  int         loop_depth;
  struct
  {
    size_t  loop_start_ip;
    size_t  break_jumps[64];
    size_t  break_jump_count;
    int     scope_base_depth; // inline_scope_depth value outside this loop
  } loops[16];

  /* Tracks active inlined scope depth for proper cleanup on `return`.
     This only tracks scopes created by MI_VM_OP_SCOPE_PUSH/POP emitted
     by the compiler (e.g. inlined while bodies). */
  int         inline_scope_depth;

  // When >0, we are compiling an expression that is itself an argument of
  // another command call. Command expressions must preserve the arg stack.
  int         arg_expr_depth;
} MiVmBuild;

static uint8_t s_compile_expr(MiVmBuild* b, const MiExpr* e);

static uint8_t s_alloc_reg(MiVmBuild* b)
{
  if (b->next_reg >= 250u)
  {
    mi_error("mi_vm: ran out of registers\n");
    return 0;
  }
  return b->next_reg++;
}

static MiVmOp s_map_unary(MiTokenKind op)
{
  switch (op)
  {
    case MI_TOK_MINUS: return MI_VM_OP_NEG;
    case MI_TOK_NOT:   return MI_VM_OP_NOT;
    default:           return MI_VM_OP_NOOP;
  }
}

static MiVmOp s_map_binary(MiTokenKind op)
{
  switch (op)
  {
    case MI_TOK_PLUS:  return MI_VM_OP_ADD;
    case MI_TOK_MINUS: return MI_VM_OP_SUB;
    case MI_TOK_STAR:  return MI_VM_OP_MUL;
    case MI_TOK_SLASH: return MI_VM_OP_DIV;
    case MI_TOK_EQEQ:  return MI_VM_OP_EQ;
    case MI_TOK_BANGEQ:return MI_VM_OP_NEQ;
    case MI_TOK_LT:    return MI_VM_OP_LT;
    case MI_TOK_LTEQ:  return MI_VM_OP_LTEQ;
    case MI_TOK_GT:    return MI_VM_OP_GT;
    case MI_TOK_GTEQ:  return MI_VM_OP_GTEQ;
    case MI_TOK_AND:   return MI_VM_OP_AND;
    case MI_TOK_OR:    return MI_VM_OP_OR;
    default:           return MI_VM_OP_NOOP;
  }
}

static void s_chunk_patch_imm(MiVmChunk* c, size_t ins_index, int32_t imm)
{
  if (!c || ins_index >= c->code_count)
  {
    return;
  }
  c->code[ins_index].imm = imm;

  /* Suppress JMP/JT/JF 0: fallthrough is implicit. */
  if (imm == 0)
  {
    MiVmOp op = (MiVmOp)c->code[ins_index].op;
    if (op == MI_VM_OP_JUMP || op == MI_VM_OP_JUMP_IF_TRUE || op == MI_VM_OP_JUMP_IF_FALSE)
    {
      c->code[ins_index].op = (uint8_t)MI_VM_OP_NOOP;
      c->code[ins_index].a = 0;
      c->code[ins_index].b = 0;
      c->code[ins_index].c = 0;
      c->code[ins_index].imm = 0;
    }
  }
}

static bool s_expr_is_lit_string(const MiExpr* e, const char* cstr)
{
  if (!e || e->kind != MI_EXPR_STRING_LITERAL)
  {
    return false;
  }

  return s_slice_eq(e->as.string_lit.value, x_slice_from_cstr(cstr));
}

static void s_emit_scope_pops(MiVmBuild* b, int count)
{
  if (!b)
  {
    return;
  }

  for (int i = 0; i < count; ++i)
  {
    s_chunk_emit(b->chunk, MI_VM_OP_SCOPE_POP, 0, 0, 0, 0);
  }
}

static uint8_t s_compile_command_expr(MiVmBuild* b, const MiExpr* e, bool wants_result);

static void s_compile_script_inline(MiVmBuild* b, const MiScript* script)
{
  if (!b || !script)
  {
    return;
  }

  const MiCommandList* it = script->first;
  while (it)
  {
    const MiCommand* cmd = it->command;
    b->next_reg = b->reg_base;

    MiExpr fake;
    memset(&fake, 0, sizeof(fake));
    fake.kind = MI_EXPR_COMMAND;
    fake.as.command.head = cmd ? cmd->head : NULL;
    fake.as.command.args = cmd ? cmd->args : NULL;
    fake.as.command.argc = cmd ? (unsigned int) cmd->argc : 0;

    (void) s_compile_command_expr(b, &fake, false);
    it = it->next;
  }
}

static void s_compile_script_inline_to_reg(MiVmBuild* b, const MiScript* script, uint8_t dst)
{
  if (!b || !script)
  {
    int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
    s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
    return;
  }

  uint8_t last = dst;
  bool have_last = false;

  const MiCommandList* it = script->first;
  while (it)
  {
    const MiCommand* cmd = it->command;
    b->next_reg = b->reg_base;

    MiExpr fake;
    memset(&fake, 0, sizeof(fake));
    fake.kind = MI_EXPR_COMMAND;
    fake.as.command.head = cmd ? cmd->head : NULL;
    fake.as.command.args = cmd ? cmd->args : NULL;
    fake.as.command.argc = cmd ? (unsigned int)cmd->argc : 0;

    bool wants_result = (it->next == NULL);
    last = s_compile_command_expr(b, &fake, wants_result);
    have_last = true;

    it = it->next;
  }

  if (!have_last)
  {
    int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
    s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
    return;
  }

  if (last != dst)
  {
    s_chunk_emit(b->chunk, MI_VM_OP_MOV, dst, last, 0, 0);
  }
}

static uint8_t s_compile_command_expr(MiVmBuild* b, const MiExpr* e, bool wants_result)
{
  uint8_t dst = s_alloc_reg(b);
  uint8_t argc = 0;

  /* Special form: set :: <lvalue> <value>
     Supports:
       set :: name value
       set :: $name value
       set :: $list[index] value
     Falls back to regular command dispatch for indirect names. */
  if (s_expr_is_lit_string(e->as.command.head, "set") && e->as.command.argc == 2u)
  {
    const MiExprList* it = e->as.command.args;
    const MiExpr* lvalue = it ? it->expr : NULL;
    it = it ? it->next : NULL;
    const MiExpr* rhs = it ? it->expr : NULL;

    if (!lvalue || !rhs)
    {
      mi_error("set: expected lvalue and value\n");
      int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
      return dst;
    }

    /* lvalue: bare name */
    if (lvalue->kind == MI_EXPR_STRING_LITERAL)
    {
      uint8_t rhs_reg = s_compile_expr(b, rhs);
      int32_t sym = s_chunk_add_symbol(b->chunk, lvalue->as.string_lit.value);
      s_chunk_emit(b->chunk, MI_VM_OP_STORE_VAR, rhs_reg, 0, 0, sym);
      if (wants_result)
      {
        s_chunk_emit(b->chunk, MI_VM_OP_MOV, dst, rhs_reg, 0, 0);
      }
      return dst;
    }

    /* lvalue: $name (direct only) */
    if (lvalue->kind == MI_EXPR_VAR && !lvalue->as.var.is_indirect)
    {
      uint8_t rhs_reg = s_compile_expr(b, rhs);
      int32_t sym = s_chunk_add_symbol(b->chunk, lvalue->as.var.name);
      s_chunk_emit(b->chunk, MI_VM_OP_STORE_VAR, rhs_reg, 0, 0, sym);
      if (wants_result)
      {
        s_chunk_emit(b->chunk, MI_VM_OP_MOV, dst, rhs_reg, 0, 0);
      }
      return dst;
    }

    /* lvalue: $target[index] */
    if (lvalue->kind == MI_EXPR_INDEX)
    {
      uint8_t base_reg = s_compile_expr(b, lvalue->as.index.target);
      uint8_t key_reg = s_compile_expr(b, lvalue->as.index.index);
      uint8_t rhs_reg = s_compile_expr(b, rhs);
      s_chunk_emit(b->chunk, MI_VM_OP_STORE_INDEX, base_reg, key_reg, rhs_reg, 0);
      if (wants_result)
      {
        s_chunk_emit(b->chunk, MI_VM_OP_MOV, dst, rhs_reg, 0, 0);
      }
      return dst;
    }

    /* Indirect or unsupported lvalue: fall back to command call. */
  }

  /* Special form: call :: <block>
     This compiles to a direct CALL_BLOCK, bypassing command dispatch. */
  if (e->as.command.head &&
      e->as.command.head->kind == MI_EXPR_STRING_LITERAL &&
      s_slice_eq(e->as.command.head->as.string_lit.value, x_slice_from_cstr("call")) &&
      e->as.command.argc == 1u)
  {
    const MiExprList* only = e->as.command.args;
    uint8_t block_reg = s_compile_expr(b, only ? only->expr : NULL);
    s_chunk_emit(b->chunk, MI_VM_OP_CALL_BLOCK, dst, block_reg, 0, 0);
    return dst;
  }

  /* Special form: cmd: <name> <param_name_0> ... <param_name_n> { ... }
     Lowered without creating a runtime list/dict for the parameter list.
     Emits: CALL cmd with args = (name, param_name..., body_block).
     NOTE: This is NOT a general variadic handler; it only matches when the last
     argument is a block.
  */
  if (s_expr_is_lit_string(e->as.command.head, "cmd") && e->as.command.argc >= 2u)
  {
    const MiExprList* it = e->as.command.args;
    const MiExpr* cmd_name_expr = it ? it->expr : NULL;
    /* params are everything except first (name) and last (body) */
    const MiExprList* params_it = it ? it->next : NULL;
    const MiExpr* body_expr = NULL;
    if (e->as.command.argc >= 1u)
    {
      const MiExprList* tail = e->as.command.args;
      while (tail && tail->next)
      {
        tail = tail->next;
      }
      body_expr = tail ? tail->expr : NULL;
    }

    if (!cmd_name_expr || !body_expr)
    {
      mi_error("cmd: expected name and body\n");
      int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
      return dst;
    }

    if (body_expr->kind != MI_EXPR_BLOCK)
    {
      /* Not the special form; fall through to normal command compilation. */
      goto cmd_special_form_done;
    }

    MiVmCommandFn cmd_fn = s_vm_find_command_fn(b->vm, x_slice_from_cstr("cmd"));
    if (!cmd_fn)
    {
      mi_error("cmd: builtin not registered\n");
      int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
      return dst;
    }

    /* Evaluate and push arguments on the arg stack. */
    s_chunk_emit(b->chunk, MI_VM_OP_ARG_CLEAR, 0, 0, 0, 0);
    argc = 0;

    uint8_t name_reg = s_compile_expr(b, cmd_name_expr);
    s_chunk_emit(b->chunk, MI_VM_OP_ARG_PUSH, name_reg, 0, 0, 0);
    argc += 1;

    /* Push parameter names (raw expressions) in order. */
    const MiExprList* cur = params_it;
    while (cur && cur->next)
    {
      uint8_t preg = s_compile_expr(b, cur->expr);
      s_chunk_emit(b->chunk, MI_VM_OP_ARG_PUSH, preg, 0, 0, 0);
      argc += 1;
      cur = cur->next;
    }

    uint8_t body_reg = s_compile_expr(b, body_expr);
    s_chunk_emit(b->chunk, MI_VM_OP_ARG_PUSH, body_reg, 0, 0, 0);
    argc += 1;

    int32_t cmd_id = s_chunk_add_cmd_fn(b->chunk, x_slice_from_cstr("cmd"), cmd_fn);
    s_chunk_emit(b->chunk, MI_VM_OP_CALL_CMD, dst, argc, 0, cmd_id);
    return dst;
  }

cmd_special_form_done:

  /* Special form: break ::
     Only valid inside a VM-compiled loop that supports break/continue.
     Emits an unconditional jump to the loop end, plus any required
     scope cleanup. */
  if (s_expr_is_lit_string(e->as.command.head, "break"))
  {
    if (b->loop_depth <= 0)
    {
      mi_error("break: not inside a loop\n");
      if (wants_result)
      {
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
      }
      return dst;
    }

    if (wants_result)
    {
      MiRtValue v = mi_rt_make_void();
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, s_chunk_add_const(b->chunk, v));
    }

    int idx = b->loop_depth - 1;

    /* Pop all compiler-emitted inlined scopes down to the loop's base depth.
       This correctly handles break/continue from inside nested inlined blocks
       (e.g. an `if` body inside a `while` body). */
    {
      int pops = b->inline_scope_depth - b->loops[idx].scope_base_depth;
      if (pops > 0)
      {
        s_emit_scope_pops(b, pops);
      }
    }

    if (b->loops[idx].break_jump_count < (sizeof(b->loops[idx].break_jumps) / sizeof(b->loops[idx].break_jumps[0])))
    {
      b->loops[idx].break_jumps[b->loops[idx].break_jump_count++] = b->chunk->code_count;
    }
    s_chunk_emit(b->chunk, MI_VM_OP_JUMP, 0, 0, 0, 0);
    return dst;
  }

  /* Special form: continue ::
     Jumps to loop start, plus any required scope cleanup. */
  if (s_expr_is_lit_string(e->as.command.head, "continue"))
  {
    if (b->loop_depth <= 0)
    {
      mi_error("continue: not inside a loop\n");
      if (wants_result)
      {
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
      }
      return dst;
    }

    if (wants_result)
    {
      MiRtValue v = mi_rt_make_void();
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, s_chunk_add_const(b->chunk, v));
    }

    int idx = b->loop_depth - 1;

    {
      int pops = b->inline_scope_depth - b->loops[idx].scope_base_depth;
      if (pops > 0)
      {
        s_emit_scope_pops(b, pops);
      }
    }

    size_t jmp_index = b->chunk->code_count;
    s_chunk_emit(b->chunk, MI_VM_OP_JUMP, 0, 0, 0, 0);
    {
      int32_t rel = (int32_t)((int64_t)b->loops[idx].loop_start_ip - (int64_t)(jmp_index + 1u));
      s_chunk_patch_imm(b->chunk, jmp_index, rel);
    }
    return dst;
  }

  /* Special form: return :: <expr>?
     Returns from the current chunk early. This is mainly useful inside
     blocks called via CALL_BLOCK, but also works at top level.
     Any compiler-emitted inlined scopes are cleaned up first. */
  if (s_expr_is_lit_string(e->as.command.head, "return"))
  {
    const MiExprList* it = e->as.command.args;
    const MiExpr* value = it ? it->expr : NULL;
    uint8_t r = 0;

    if (value)
    {
      r = s_compile_expr(b, value);
    }
    else
    {
      int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
      r = dst;
    }

    if (b->inline_scope_depth > 0)
    {
      s_emit_scope_pops(b, b->inline_scope_depth);
    }

    s_chunk_emit(b->chunk, MI_VM_OP_RETURN, r, 0, 0, 0);
    return dst;
  }

  /* Special form: if/elseif/else
     Grammar (command args):
       if :: <cond> <then_block> ("elseif" <cond> <block>)* ("else" <block>)?
     Note: "elseif"/"else" are plain string literal tokens in the AST. */
  if (s_expr_is_lit_string(e->as.command.head, "if"))
  {
    const MiExprList* it = e->as.command.args;

    const MiExpr* cond = it ? it->expr : NULL;
    it = it ? it->next : NULL;
    const MiExpr* then_block = it ? it->expr : NULL;
    it = it ? it->next : NULL;

    if (!cond || !then_block)
    {
      mi_error("if: expected cond and then block\n");
      if (wants_result)
      {
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
      }
      return dst;
    }

    /* We'll patch all jumps to the end once we know where it is. */
    size_t end_jumps[64];
    size_t end_jump_count = 0;

    for (;;)
    {
      uint8_t cond_reg = s_compile_expr(b, cond);

      /* JF cond, <to next branch> */
      size_t jf_index = b->chunk->code_count;
      s_chunk_emit(b->chunk, MI_VM_OP_JUMP_IF_FALSE, cond_reg, 0, 0, 0);

      if (then_block->kind == MI_EXPR_BLOCK && then_block->as.block.script)
      {
        s_chunk_emit(b->chunk, MI_VM_OP_SCOPE_PUSH, 0, 0, 0, 0);
        b->inline_scope_depth += 1;
        if (wants_result)
        {
          s_compile_script_inline_to_reg(b, then_block->as.block.script, dst);
        }
        else
        {
          s_compile_script_inline(b, then_block->as.block.script);
        }
        s_chunk_emit(b->chunk, MI_VM_OP_SCOPE_POP, 0, 0, 0, 0);
        b->inline_scope_depth -= 1;
      }
      else
      {
        uint8_t block_reg = s_compile_expr(b, then_block);
        s_chunk_emit(b->chunk, MI_VM_OP_CALL_BLOCK, dst, block_reg, 0, 0);
      }

      /* JMP <to end> */
      if (end_jump_count < (sizeof(end_jumps) / sizeof(end_jumps[0])))
      {
        end_jumps[end_jump_count++] = b->chunk->code_count;
      }
      s_chunk_emit(b->chunk, MI_VM_OP_JUMP, 0, 0, 0, 0);

      /* Patch JF to jump here (start of the next branch parsing/emit). */
      {
        int32_t rel = (int32_t)((int64_t)b->chunk->code_count - (int64_t)(jf_index + 1u));
        s_chunk_patch_imm(b->chunk, jf_index, rel);
      }

      /* No more tokens: done. */
      if (!it)
      {
        break;
      }

      /* elseif / else markers */
      if (s_expr_is_lit_string(it->expr, "elseif"))
      {
        it = it->next;
        cond = it ? it->expr : NULL;
        it = it ? it->next : NULL;
        then_block = it ? it->expr : NULL;
        it = it ? it->next : NULL;
        if (!cond || !then_block)
        {
          mi_error("if: elseif expects cond and block\n");
          break;
        }
        continue;
      }

      if (s_expr_is_lit_string(it->expr, "else"))
      {
        it = it->next;
        const MiExpr* else_block = it ? it->expr : NULL;
        it = it ? it->next : NULL;
        if (!else_block)
        {
          mi_error("if: else expects block\n");
          break;
        }

        if (else_block->kind == MI_EXPR_BLOCK && else_block->as.block.script)
        {
          s_chunk_emit(b->chunk, MI_VM_OP_SCOPE_PUSH, 0, 0, 0, 0);
          b->inline_scope_depth += 1;
          if (wants_result)
          {
            s_compile_script_inline_to_reg(b, else_block->as.block.script, dst);
          }
          else
          {
            s_compile_script_inline(b, else_block->as.block.script);
          }
          s_chunk_emit(b->chunk, MI_VM_OP_SCOPE_POP, 0, 0, 0, 0);
          b->inline_scope_depth -= 1;
        }
        else
        {
          uint8_t block_reg = s_compile_expr(b, else_block);
          s_chunk_emit(b->chunk, MI_VM_OP_CALL_BLOCK, dst, block_reg, 0, 0);
        }
        break;
      }

      /* Unexpected trailing tokens, ignore. */
      mi_error("if: unexpected tokens after then block\n");
      break;
    }

    /* Patch all end jumps to jump here (end label). */
    for (size_t i = 0; i < end_jump_count; ++i)
    {
      size_t jmp_index = end_jumps[i];
      int32_t rel = (int32_t)((int64_t)b->chunk->code_count - (int64_t)(jmp_index + 1u));
      s_chunk_patch_imm(b->chunk, jmp_index, rel);
    }

    return dst;
  }

  /* Special form: while
     Grammar (command args):
       while :: <cond> <body_block>
     Compiles to:
       loop_start:
         <cond>
         JF cond, loop_end
         CALL_BLOCK body
         JMP loop_start
       loop_end:
  */
  if (s_expr_is_lit_string(e->as.command.head, "while"))
  {
    const MiExprList* it = e->as.command.args;
    const MiExpr* cond = it ? it->expr : NULL;
    it = it ? it->next : NULL;
    const MiExpr* body_block = it ? it->expr : NULL;

    if (!cond || !body_block)
    {
      mi_error("while: expected cond and body block\n");
      int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
      return dst;
    }

    if (body_block->kind != MI_EXPR_BLOCK || !body_block->as.block.script)
    {
      mi_error("while: body must be a literal block\n");
      int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
      return dst;
    }

    size_t loop_start = b->chunk->code_count;

    uint8_t cond_reg = s_compile_expr(b, cond);

    /* JF cond, <to loop end> */
    size_t jf_index = b->chunk->code_count;
    s_chunk_emit(b->chunk, MI_VM_OP_JUMP_IF_FALSE, cond_reg, 0, 0, 0);

    /* Inline the block body so break/continue can be compiled to jumps.
       We still preserve block semantics by creating a fresh scope per
       iteration (variables created inside the body do not leak). */
    int loop_scope_base = b->inline_scope_depth;
    s_chunk_emit(b->chunk, MI_VM_OP_SCOPE_PUSH, 0, 0, 0, 0);
    b->inline_scope_depth += 1;

    int loop_idx = -1;
    if (b->loop_depth < (int)(sizeof(b->loops) / sizeof(b->loops[0])))
    {
      loop_idx = b->loop_depth;
      b->loop_depth += 1;
      b->loops[loop_idx].loop_start_ip = loop_start;
      b->loops[loop_idx].break_jump_count = 0;
      b->loops[loop_idx].scope_base_depth = loop_scope_base;
    }
    else
    {
      mi_error("while: loop nesting too deep\n");
    }

    uint8_t saved_reg_base = b->reg_base;
    b->reg_base = b->next_reg;

    s_compile_script_inline(b, body_block->as.block.script);

    b->reg_base = saved_reg_base;

    s_chunk_emit(b->chunk, MI_VM_OP_SCOPE_POP, 0, 0, 0, 0);
    b->inline_scope_depth -= 1;

    /* JMP back to loop_start */
    size_t jmp_index = b->chunk->code_count;
    s_chunk_emit(b->chunk, MI_VM_OP_JUMP, 0, 0, 0, 0);
    {
      int32_t rel_back = (int32_t)((int64_t)loop_start - (int64_t)(jmp_index + 1u));
      s_chunk_patch_imm(b->chunk, jmp_index, rel_back);
    }

    /* loop_end label is here */
    size_t loop_end = b->chunk->code_count;

    /* Patch JF to jump to loop_end */
    {
      int32_t rel_end = (int32_t)((int64_t)loop_end - (int64_t)(jf_index + 1u));
      s_chunk_patch_imm(b->chunk, jf_index, rel_end);
    }

    /* Patch break jumps to loop_end */
    if (loop_idx >= 0)
    {
      for (size_t bi = 0; bi < b->loops[loop_idx].break_jump_count; ++bi)
      {
        size_t bj = b->loops[loop_idx].break_jumps[bi];
        int32_t rel = (int32_t)((int64_t)loop_end - (int64_t)(bj + 1u));
        s_chunk_patch_imm(b->chunk, bj, rel);
      }
      b->loop_depth -= 1;
    }

    return dst;
  }

  /* Special form: foreach
     Grammar (command args):
       foreach : <varname> <expr_list> <body_block>

     Notes:
       - <varname> must be a literal identifier (string literal).
       - <expr_list> is evaluated once.
       - The body is inlined so break/continue compile to jumps.
       - A fresh scope is created for each iteration.

     Compiles to:
       list = <expr_list>
       len = LEN(list)
       idx = -1
       inc_label:
         idx = idx + 1
         cond = idx < len
         JF cond, end
         SCOPE_PUSH
         <varname> = INDEX(list, idx)
         <body>
         SCOPE_POP
         JMP inc_label
       end:
  */
  if (s_expr_is_lit_string(e->as.command.head, "foreach"))
  {
    const MiExprList* it = e->as.command.args;
    const MiExpr* varname_expr = it ? it->expr : NULL;
    it = it ? it->next : NULL;
    const MiExpr* list_expr = it ? it->expr : NULL;
    it = it ? it->next : NULL;
    const MiExpr* body_block = it ? it->expr : NULL;

    if (!varname_expr || !list_expr || !body_block)
    {
      mi_error("foreach: expected varname, expression, and body block\n");
      if (wants_result)
      {
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
      }
      return dst;
    }

    if (varname_expr->kind != MI_EXPR_STRING_LITERAL)
    {
      mi_error("foreach: varname must be a literal identifier\n");
      if (wants_result)
      {
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
      }
      return dst;
    }

    if (body_block->kind != MI_EXPR_BLOCK || !body_block->as.block.script)
    {
      mi_error("foreach: body must be a literal block\n");
      if (wants_result)
      {
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
      }
      return dst;
    }

    /* Result of foreach is void when used as an expression. */
    if (wants_result)
    {
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, s_chunk_add_const(b->chunk, mi_rt_make_void()));
    }

    int32_t foreach_sym = s_chunk_add_symbol(b->chunk, varname_expr->as.string_lit.value);

    /*
      In Minima, bare identifiers are parsed as string literals.
      For foreach, allow iterating a variable by writing its name
      directly (no '$'):
        foreach : x xs { ... }
      This keeps foreach ergonomic without changing identifier rules
      globally.
    */
    uint8_t container_reg = 0;
    if (list_expr->kind == MI_EXPR_STRING_LITERAL)
    {
      int32_t sym = s_chunk_add_symbol(b->chunk, list_expr->as.string_lit.value);
      container_reg = s_alloc_reg(b);
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_VAR, container_reg, 0, 0, sym);
    }
    else
    {
      container_reg = s_compile_expr(b, list_expr);
    }

    uint8_t idx_reg = s_alloc_reg(b);
    s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, idx_reg, 0, 0, s_chunk_add_const(b->chunk, mi_rt_make_int(-1)));

    size_t loop_label = b->chunk->code_count;

    uint8_t cond_reg = s_alloc_reg(b);
    uint8_t item_reg = s_alloc_reg(b);
    s_chunk_emit(b->chunk, MI_VM_OP_ITER_NEXT, cond_reg, container_reg, idx_reg, (int32_t)item_reg);

    /* JF cond, <to end> */
    size_t jf_index = b->chunk->code_count;
    s_chunk_emit(b->chunk, MI_VM_OP_JUMP_IF_FALSE, cond_reg, 0, 0, 0);

    int loop_scope_base = b->inline_scope_depth;
    s_chunk_emit(b->chunk, MI_VM_OP_SCOPE_PUSH, 0, 0, 0, 0);
    b->inline_scope_depth += 1;

    int loop_idx = -1;
    if (b->loop_depth < (int)(sizeof(b->loops) / sizeof(b->loops[0])))
    {
      loop_idx = b->loop_depth;
      b->loop_depth += 1;
      b->loops[loop_idx].loop_start_ip = loop_label;
      b->loops[loop_idx].break_jump_count = 0;
      b->loops[loop_idx].scope_base_depth = loop_scope_base;
    }
    else
    {
      mi_error("foreach: loop nesting too deep\n");
    }

    /* foreach var = item (local bind) */
    s_chunk_emit(b->chunk, MI_VM_OP_DEFINE_VAR, item_reg, 0, 0, foreach_sym);

    uint8_t saved_reg_base = b->reg_base;
    b->reg_base = b->next_reg;

    s_compile_script_inline(b, body_block->as.block.script);

    b->reg_base = saved_reg_base;

    s_chunk_emit(b->chunk, MI_VM_OP_SCOPE_POP, 0, 0, 0, 0);
    b->inline_scope_depth -= 1;

    /* JMP back to loop_label */
    {
      size_t jmp_index = b->chunk->code_count;
      s_chunk_emit(b->chunk, MI_VM_OP_JUMP, 0, 0, 0, 0);
      int32_t rel_back = (int32_t)((int64_t)loop_label - (int64_t)(jmp_index + 1u));
      s_chunk_patch_imm(b->chunk, jmp_index, rel_back);
    }

    size_t loop_end = b->chunk->code_count;

    /* Patch JF to jump to loop_end */
    {
      int32_t rel_end = (int32_t)((int64_t)loop_end - (int64_t)(jf_index + 1u));
      s_chunk_patch_imm(b->chunk, jf_index, rel_end);
    }

    /* Patch break jumps to loop_end */
    if (loop_idx >= 0)
    {
      for (size_t bi = 0; bi < b->loops[loop_idx].break_jump_count; ++bi)
      {
        size_t bj = b->loops[loop_idx].break_jumps[bi];
        int32_t rel = (int32_t)((int64_t)loop_end - (int64_t)(bj + 1u));
        s_chunk_patch_imm(b->chunk, bj, rel);
      }
      b->loop_depth -= 1;
    }

    return dst;
  }

  /* Regular command call: it uses the argument stack. */
  bool preserve_args = (b->arg_expr_depth > 0);
  if (preserve_args)
  {
    s_chunk_emit(b->chunk, MI_VM_OP_ARG_SAVE, 0, 0, 0, 0);
  }
  s_chunk_emit(b->chunk, MI_VM_OP_ARG_CLEAR, 0, 0, 0, 0);

  const MiExprList* it = e->as.command.args;
  while (it)
  {
    const MiExpr* arg = it->expr;

    /* Fast-path: literal constants can be pushed directly. */
    if (arg && (arg->kind == MI_EXPR_INT_LITERAL ||
          arg->kind == MI_EXPR_FLOAT_LITERAL ||
          arg->kind == MI_EXPR_BOOL_LITERAL ||
          arg->kind == MI_EXPR_VOID_LITERAL ||
          arg->kind == MI_EXPR_STRING_LITERAL))
    {
      MiRtValue v = mi_rt_make_void();
      if (arg->kind == MI_EXPR_INT_LITERAL)   v = mi_rt_make_int(arg->as.int_lit.value);
      if (arg->kind == MI_EXPR_FLOAT_LITERAL) v = mi_rt_make_float(arg->as.float_lit.value);
      if (arg->kind == MI_EXPR_BOOL_LITERAL)  v = mi_rt_make_bool(arg->as.bool_lit.value);
      if (arg->kind == MI_EXPR_VOID_LITERAL)  v = mi_rt_make_void();
      if (arg->kind == MI_EXPR_STRING_LITERAL)v = mi_rt_make_string_slice(arg->as.string_lit.value);

      int32_t k = s_chunk_add_const(b->chunk, v);
      s_chunk_emit(b->chunk, MI_VM_OP_ARG_PUSH_CONST, 0, 0, 0, k);
      argc++;
      it = it->next;
      continue;
    }

    /* Fast-path: direct variable reference (non-indirect) can be pushed without staging. */
    if (arg && arg->kind == MI_EXPR_VAR && !arg->as.var.is_indirect)
    {
      int32_t sym = s_chunk_add_symbol(b->chunk, arg->as.var.name);
      s_chunk_emit(b->chunk, MI_VM_OP_ARG_PUSH_VAR_SYM, 0, 0, 0, sym);
      argc++;
      it = it->next;
      continue;
    }

    /* Fallback: compute into a register then push.
       Note: command expressions nested inside other command argument lists
       must preserve the arg stack (ARG_SAVE/ARG_RESTORE). */
    b->arg_expr_depth += 1;
    uint8_t r = s_compile_expr(b, arg);
    b->arg_expr_depth -= 1;
    s_chunk_emit(b->chunk, MI_VM_OP_ARG_PUSH, r, 0, 0, 0);
    argc++;
    it = it->next;
  }

  const MiExpr* head = e->as.command.head;
  if (head && head->kind == MI_EXPR_STRING_LITERAL)
  {
    XSlice name = head->as.string_lit.value;
    MiVmCommandFn fn = s_vm_find_command_fn(b->vm, name);
    if (fn)
    {
      int32_t cmd_id = s_chunk_add_cmd_fn(b->chunk, name, fn);
      s_chunk_emit(b->chunk, MI_VM_OP_CALL_CMD, dst, argc, 0, cmd_id);
    }
    else
    {
      // Late-bound command: allow user-defined commands created earlier in the script.
      uint8_t head_reg = s_alloc_reg(b);
      int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_string_slice(name));
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, head_reg, 0, 0, k);
      s_chunk_emit(b->chunk, MI_VM_OP_CALL_CMD_DYN, dst, head_reg, argc, 0);
    }

    if (preserve_args)
    {
      s_chunk_emit(b->chunk, MI_VM_OP_ARG_RESTORE, 0, 0, 0, 0);
    }
    return dst;
  }

  // Dynamic head: compute name into a register and do dynamic lookup.
  uint8_t head_reg = s_compile_expr(b, head);
  s_chunk_emit(b->chunk, MI_VM_OP_CALL_CMD_DYN, dst, head_reg, argc, 0);

  if (preserve_args)
  {
    s_chunk_emit(b->chunk, MI_VM_OP_ARG_RESTORE, 0, 0, 0, 0);
  }
  return dst;
}

static uint8_t s_compile_expr(MiVmBuild* b, const MiExpr* e)
{
  if (!e)
  {
    uint8_t r = s_alloc_reg(b);
    int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
    s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, r, 0, 0, k);
    return r;
  }

  switch (e->kind)
  {
    case MI_EXPR_INT_LITERAL:
      {
        uint8_t r = s_alloc_reg(b);
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_int(e->as.int_lit.value));
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, r, 0, 0, k);
        return r;
      }

    case MI_EXPR_FLOAT_LITERAL:
      {
        uint8_t r = s_alloc_reg(b);
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_float(e->as.float_lit.value));
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, r, 0, 0, k);
        return r;
      }

    case MI_EXPR_STRING_LITERAL:
      {
        uint8_t r = s_alloc_reg(b);
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_string_slice(e->as.string_lit.value));
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, r, 0, 0, k);
        return r;
      }

    case MI_EXPR_BOOL_LITERAL:
      {
        uint8_t r = s_alloc_reg(b);
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_bool(e->as.bool_lit.value));
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, r, 0, 0, k);
        return r;
      }

    case MI_EXPR_VOID_LITERAL:
      {
        uint8_t r = s_alloc_reg(b);
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, r, 0, 0, k);
        return r;
      }

    case MI_EXPR_VAR:
      {
        uint8_t r = s_alloc_reg(b);
        if (e->as.var.is_indirect)
        {
          uint8_t name_reg = s_compile_expr(b, e->as.var.name_expr);
          s_chunk_emit(b->chunk, MI_VM_OP_LOAD_INDIRECT_VAR, r, name_reg, 0, 0);
          return r;
        }

        int32_t sym = s_chunk_add_symbol(b->chunk, e->as.var.name);
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_VAR, r, 0, 0, sym);
        return r;
      }

    case MI_EXPR_UNARY:
      {
        uint8_t r = s_alloc_reg(b);
        uint8_t x = s_compile_expr(b, e->as.unary.expr);
        MiVmOp op = s_map_unary(e->as.unary.op);
        s_chunk_emit(b->chunk, op, r, x, 0, 0);
        return r;
      }

    case MI_EXPR_BINARY:
      {
        uint8_t r = s_alloc_reg(b);
        uint8_t a = s_compile_expr(b, e->as.binary.left);
        uint8_t c = s_compile_expr(b, e->as.binary.right);
        MiVmOp op = s_map_binary(e->as.binary.op);
        s_chunk_emit(b->chunk, op, r, a, c, 0);
        return r;
      }

    case MI_EXPR_COMMAND:
      return s_compile_command_expr(b, e, true);

    case MI_EXPR_BLOCK:
      {
        uint8_t r = s_alloc_reg(b);
        MiVmChunk* sub = s_vm_compile_script_ast(b->vm, e->as.block.script, NULL);
        if (!sub)
        {
          int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
          s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, r, 0, 0, k);
          return r;
        }

        int32_t id = s_chunk_add_subchunk(b->chunk, sub);
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_BLOCK, r, 0, 0, id);
        return r;
      }
    case MI_EXPR_LIST:
      {
        uint8_t r = s_alloc_reg(b);
        s_chunk_emit(b->chunk, MI_VM_OP_LIST_NEW, r, 0, 0, 0);

        const MiExprList* it = e->as.list.items;
        while (it)
        {
          uint8_t item_reg = s_compile_expr(b, it->expr);
          s_chunk_emit(b->chunk, MI_VM_OP_LIST_PUSH, r, item_reg, 0, 0);
          it = it->next;
        }

        return r;
      }

    case MI_EXPR_DICT:
      {
        /* Native dict literal.
           The parser produces MI_EXPR_DICT with MI_EXPR_PAIR items.
           We lower directly to a dict allocation followed by STORE_INDEX
           for each entry, avoiding intermediate list construction. */

        uint8_t dict_reg = s_alloc_reg(b);
        s_chunk_emit(b->chunk, MI_VM_OP_DICT_NEW, dict_reg, 0, 0, 0);

        const MiExprList* it = e->as.dict.items;
        while (it)
        {
          const MiExpr* pe = it->expr;
          if (!pe || pe->kind != MI_EXPR_PAIR)
          {
            mi_error("dict literal: expected k = v entries\n");
            break;
          }

          uint8_t k_reg = s_compile_expr(b, pe->as.pair.key);
          uint8_t v_reg = s_compile_expr(b, pe->as.pair.value);

          s_chunk_emit(b->chunk, MI_VM_OP_STORE_INDEX, dict_reg, k_reg, v_reg, 0);
          it = it->next;
        }

        return dict_reg;
      }

    case MI_EXPR_PAIR:

      {
        /* Pairs are not a standalone runtime type. The parser only produces
           MI_EXPR_PAIR inside MI_EXPR_DICT; encountering it here means the AST
           was malformed or built manually. */
        mi_error("pair literal used outside dict literal\n");
        uint8_t r = s_alloc_reg(b);
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, r, 0, 0, k);
        return r;
      }

    case MI_EXPR_INDEX:
      {
        uint8_t r = s_alloc_reg(b);
        uint8_t base_reg = s_compile_expr(b, e->as.index.target);
        uint8_t key_reg = s_compile_expr(b, e->as.index.index);
        s_chunk_emit(b->chunk, MI_VM_OP_INDEX, r, base_reg, key_reg, 0);
        return r;
      }

    default:
      {
        mi_error_fmt("mi_vm: unsupported expr kind: %d\n", (int) e->kind);
        uint8_t r = s_alloc_reg(b);
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
        s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, r, 0, 0, k);
        return r;
      }
  }
}

static MiVmChunk* s_vm_compile_script_ast(MiVm* vm, const MiScript* script, XArena* arena)
{
  (void) arena;

  if (!vm || !script)
  {
    return NULL;
  }

  /* Ensure the VM pipeline gets a folded AST. This is a pure simplification
     pass; it will not execute variables or commands. */
  mi_fold_constants_ast(NULL, script);

  MiVmChunk* chunk = s_chunk_create();

  MiVmBuild b;
  memset(&b, 0, sizeof(b));
  b.vm = vm;
  b.chunk = chunk;
  b.next_reg = 0;

  const MiCommandList* it = script ? script->first : NULL;
  while (it)
  {
    const MiCommand* cmd = it->command;
    b.next_reg = 0;

    // Compile as a command expression: head :: args...
    MiExpr fake;
    memset(&fake, 0, sizeof(fake));
    fake.kind = MI_EXPR_COMMAND;
    fake.as.command.head = cmd ? cmd->head : NULL;
    fake.as.command.args = cmd ? cmd->args : NULL;
    fake.as.command.argc = cmd ? (unsigned int) cmd->argc : 0;

    (void) s_compile_command_expr(&b, &fake, false);
    it = it->next;
  }

  s_chunk_emit(chunk, MI_VM_OP_HALT, 0, 0, 0, 0);

  return chunk;
}

MiVmChunk* mi_vm_compile_script(MiVm* vm, XSlice source)
{
  if (!vm)
  {
    return NULL;
  }

  /* Ensure runtime exists and builtins are registered. We keep this very
     conservative to avoid changing behavior elsewhere. */
  if (!vm->rt)
  {
    static MiRuntime s_rt;
    static bool s_rt_inited = false;
    if (!s_rt_inited)
    {
      mi_rt_init(&s_rt);
      s_rt_inited = true;
    }
    vm->rt = &s_rt;
  }

  if (!vm->commands && vm->command_count == 0)
  {
    /* Register minimal builtins, matching mi_vm_init(). */
    vm->arg_top = 0;
    (void) mi_vm_register_command(vm, x_slice_from_cstr("print"), s_vm_cmd_print);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("warning"), s_vm_cmd_warning);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("error"),   s_vm_cmd_error);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("fatal"),   s_vm_cmd_fatal);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("assert"),  s_vm_cmd_assert);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("type"),    s_vm_cmd_type);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("t"),       s_vm_cmd_type);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("typeof"),  s_vm_cmd_typeof);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("set"),   s_vm_cmd_set);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("call"),  s_vm_cmd_call);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("list"),  s_vm_cmd_list);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("dict"),  s_vm_cmd_dict);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("len"),   s_vm_cmd_len);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("cmd"),   s_vm_cmd_cmd);
  }

  XArena* arena = x_arena_create(1024 * 64);
  if (!arena)
  {
    return NULL;
  }

  MiParseResult res = mi_parse_program_ex(source.ptr, source.length, arena, true);
  if (!res.ok || !res.script)
  {
    x_arena_destroy(arena);
    return NULL;
  }

  MiVmChunk* chunk = s_vm_compile_script_ast(vm, res.script, arena);

  x_arena_destroy(arena);
  return chunk;
}


//----------------------------------------------------------
// Execution
//----------------------------------------------------------

static MiRtValue s_vm_binary_numeric(MiVmOp op, const MiRtValue* a, const MiRtValue* b)
{
  bool a_num = (a->kind == MI_RT_VAL_INT) || (a->kind == MI_RT_VAL_FLOAT);
  bool b_num = (b->kind == MI_RT_VAL_INT) || (b->kind == MI_RT_VAL_FLOAT);
  if (!a_num || !b_num)
  {
    mi_error("mi_vm: numeric op on non-number\n");
    return mi_rt_make_void();
  }

  bool is_float = (a->kind == MI_RT_VAL_FLOAT) || (b->kind == MI_RT_VAL_FLOAT);
  double da = (a->kind == MI_RT_VAL_FLOAT) ? a->as.f : (double) a->as.i;
  double db = (b->kind == MI_RT_VAL_FLOAT) ? b->as.f : (double) b->as.i;

  switch (op)
  {
    case MI_VM_OP_ADD:  return is_float ? mi_rt_make_float(da + db) : mi_rt_make_int((long long) (da + db));
    case MI_VM_OP_SUB:  return is_float ? mi_rt_make_float(da - db) : mi_rt_make_int((long long) (da - db));
    case MI_VM_OP_MUL:  return is_float ? mi_rt_make_float(da * db) : mi_rt_make_int((long long) (da * db));
    case MI_VM_OP_DIV:  return mi_rt_make_float(da / db);
    case MI_VM_OP_MOD:  return mi_rt_make_int((long long) da % (long long) db);
    default:            return mi_rt_make_void();
  }
}

static MiRtValue s_vm_binary_compare(MiVmOp op, const MiRtValue* a, const MiRtValue* b)
{
  if ((a->kind == MI_RT_VAL_INT || a->kind == MI_RT_VAL_FLOAT) &&
      (b->kind == MI_RT_VAL_INT || b->kind == MI_RT_VAL_FLOAT))
  {
    double da = (a->kind == MI_RT_VAL_FLOAT) ? a->as.f : (double) a->as.i;
    double db = (b->kind == MI_RT_VAL_FLOAT) ? b->as.f : (double) b->as.i;
    switch (op)
    {
      case MI_VM_OP_EQ:   return mi_rt_make_bool(da == db);
      case MI_VM_OP_NEQ:  return mi_rt_make_bool(da != db);
      case MI_VM_OP_LT:   return mi_rt_make_bool(da < db);
      case MI_VM_OP_LTEQ: return mi_rt_make_bool(da <= db);
      case MI_VM_OP_GT:   return mi_rt_make_bool(da > db);
      case MI_VM_OP_GTEQ: return mi_rt_make_bool(da >= db);
      default:            return mi_rt_make_void();
    }
  }

  if (a->kind == MI_RT_VAL_BOOL && b->kind == MI_RT_VAL_BOOL)
  {
    switch (op)
    {
      case MI_VM_OP_EQ:  return mi_rt_make_bool(a->as.b == b->as.b);
      case MI_VM_OP_NEQ: return mi_rt_make_bool(a->as.b != b->as.b);
      default:           return mi_rt_make_void();
    }
  }

  if (a->kind == MI_RT_VAL_STRING && b->kind == MI_RT_VAL_STRING)
  {
    bool eq = s_slice_eq(a->as.s, b->as.s);
    switch (op)
    {
      case MI_VM_OP_EQ:  return mi_rt_make_bool(eq);
      case MI_VM_OP_NEQ: return mi_rt_make_bool(!eq);
      default:           return mi_rt_make_void();
    }
  }

  if (a->kind == MI_RT_VAL_TYPE && b->kind == MI_RT_VAL_TYPE)
  {
    bool eq = a->as.i == b->as.i;
    switch (op)
    {
      case MI_VM_OP_EQ:  return mi_rt_make_bool(eq);
      case MI_VM_OP_NEQ: return mi_rt_make_bool(!eq);
      default:           return mi_rt_make_void();
    }
  }

  return mi_rt_make_void();
}

MiRtValue mi_vm_execute(MiVm* vm, const MiVmChunk* chunk)
{
  if (!vm || !chunk)
  {
    return mi_rt_make_void();
  }

  s_vm_arg_clear(vm);
  MiRtValue last = mi_rt_make_void();

  size_t pc = 0;
  while (pc < chunk->code_count)
  {
    MiVmIns ins = chunk->code[pc++];

    /* Debug: remember current instruction location for trace:. */
    vm->dbg_chunk = chunk;
    vm->dbg_ip = (pc == 0) ? 0 : (pc - 1);
    switch ((MiVmOp) ins.op)
    {
      case MI_VM_OP_NOOP:
        break;

      case MI_VM_OP_LOAD_CONST:
        s_vm_reg_set(vm, ins.a, chunk->consts[ins.imm]);
        break;

      case MI_VM_OP_LOAD_BLOCK:
        {
          if (ins.imm < 0 || (size_t)ins.imm >= chunk->subchunk_count)
          {
            mi_error("mi_vm: LOAD_BLOCK invalid subchunk index\n");
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            break;
          }

          MiRtBlock* b = mi_rt_block_create(vm->rt);
          b->kind = MI_RT_BLOCK_VM_CHUNK;
          b->ptr = (void*)chunk->subchunks[(size_t)ins.imm];
          b->env = vm->rt->current;
          b->id = (uint32_t)ins.imm;
          s_vm_reg_set(vm, ins.a, mi_rt_make_block(b));
        } break;

      case MI_VM_OP_MOV:
        s_vm_reg_set(vm, ins.a, vm->regs[ins.b]);
        break;


      case MI_VM_OP_LIST_NEW:
        {
          MiRtList* list = mi_rt_list_create(vm->rt);
          if (!list)
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            break;
          }
          s_vm_reg_set(vm, ins.a, mi_rt_make_list(list));
        } break;

      case MI_VM_OP_LIST_PUSH:
        {
          MiRtValue base = vm->regs[ins.a];
          MiRtValue v = vm->regs[ins.b];
          if (base.kind != MI_RT_VAL_LIST || !base.as.list)
          {
            mi_error("mi_vm: LIST_PUSH base is not a list\n");
            break;
          }
          (void) mi_rt_list_push(base.as.list, v);
        } break;

      case MI_VM_OP_DICT_NEW:
        {
          MiRtDict* dict = mi_rt_dict_create(vm->rt);
          if (!dict)
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            break;
          }
          s_vm_reg_set(vm, ins.a, mi_rt_make_dict(dict));
        } break;

      case MI_VM_OP_ITER_NEXT:
        {
          uint8_t dst_item = (uint8_t)(ins.imm & 0xFF);
          MiRtValue container = vm->regs[ins.b];
          MiRtValue cursor_v = vm->regs[ins.c];

          long long cursor = -1;
          if (cursor_v.kind == MI_RT_VAL_INT)
          {
            cursor = cursor_v.as.i;
          }

          if (container.kind == MI_RT_VAL_LIST && container.as.list)
          {
            MiRtList* list = container.as.list;
            long long next = cursor + 1;
            if (next >= 0 && (uint64_t)next < (uint64_t)list->count)
            {
              s_vm_reg_set(vm, ins.c, mi_rt_make_int(next));
              s_vm_reg_set(vm, dst_item, list->items[(size_t)next]);
              s_vm_reg_set(vm, ins.a, mi_rt_make_bool(true));
            }
            else
            {
              s_vm_reg_set(vm, ins.a, mi_rt_make_bool(false));
            }
            break;
          }

          if (container.kind == MI_RT_VAL_DICT && container.as.dict)
          {
            MiRtDict* dict = container.as.dict;
            size_t i = (cursor < -1) ? 0u : (size_t)(cursor + 1);
            while (i < dict->capacity)
            {
              MiRtDictEntry* e = &dict->entries[i];
              if (e->state == 1)
              {
                s_vm_reg_set(vm, ins.c, mi_rt_make_int((long long)i));
                s_vm_reg_set(vm, dst_item, mi_rt_make_kvref(dict, i));
                s_vm_reg_set(vm, ins.a, mi_rt_make_bool(true));
                break;
              }
              i += 1;
            }

            if (i >= dict->capacity)
            {
              s_vm_reg_set(vm, ins.a, mi_rt_make_bool(false));
            }
            break;
          }

          mi_error("mi_vm: ITER_NEXT unsupported container type\n");
          s_vm_reg_set(vm, ins.a, mi_rt_make_bool(false));
        } break;

      case MI_VM_OP_INDEX:
        {
          MiRtValue base = vm->regs[ins.b];
          MiRtValue key = vm->regs[ins.c];
          if (base.kind == MI_RT_VAL_LIST && base.as.list && key.kind == MI_RT_VAL_INT)
          {
            MiRtList* list = base.as.list;
            int64_t idx = key.as.i;
            if (idx < 0 || (uint64_t)idx >= list->count)
            {
              s_vm_reg_set(vm, ins.a, mi_rt_make_void());
              break;
            }
            s_vm_reg_set(vm, ins.a, list->items[(size_t)idx]);
            break;
          }

          if (base.kind == MI_RT_VAL_PAIR && base.as.pair && key.kind == MI_RT_VAL_INT)
          {
            long long idx = key.as.i;
            if (idx != 0 && idx != 1)
            {
              s_vm_reg_set(vm, ins.a, mi_rt_make_void());
              break;
            }
            s_vm_reg_set(vm, ins.a, base.as.pair->items[(int)idx]);
            break;
          }

          if (base.kind == MI_RT_VAL_KVREF && key.kind == MI_RT_VAL_INT)
          {
            long long idx = key.as.i;
            MiRtDict* dict = base.as.kvref.dict;
            size_t entry_index = base.as.kvref.entry_index;
            if (!dict || entry_index >= dict->capacity)
            {
              s_vm_reg_set(vm, ins.a, mi_rt_make_void());
              break;
            }
            MiRtDictEntry* e = &dict->entries[entry_index];
            if (e->state != 1 || (idx != 0 && idx != 1))
            {
              s_vm_reg_set(vm, ins.a, mi_rt_make_void());
              break;
            }
            s_vm_reg_set(vm, ins.a, (idx == 0) ? e->key : e->value);
            break;
          }

          if (base.kind == MI_RT_VAL_DICT && base.as.dict)
          {
            MiRtValue out;
            if (mi_rt_dict_get(base.as.dict, key, &out))
            {
              s_vm_reg_set(vm, ins.a, out);
            }
            else
            {
              s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            }
            break;
          }

          mi_error("mi_vm: INDEX unsupported types\n");
          s_vm_reg_set(vm, ins.a, mi_rt_make_void());
        } break;

      case MI_VM_OP_STORE_INDEX:
        {
          MiRtValue base = vm->regs[ins.a];
          MiRtValue key = vm->regs[ins.b];
          MiRtValue value = vm->regs[ins.c];

          if (base.kind == MI_RT_VAL_LIST && base.as.list && key.kind == MI_RT_VAL_INT)
          {
            MiRtList* list = base.as.list;
            long long idx = key.as.i;
            if (idx < 0 || (size_t)idx >= list->count)
            {
              mi_error("mi_vm: STORE_INDEX list index out of range\n");
              break;
            }
            mi_rt_value_assign(vm->rt, &list->items[(size_t)idx], value);
            break;
          }

          if (base.kind == MI_RT_VAL_PAIR && base.as.pair && key.kind == MI_RT_VAL_INT)
          {
            long long idx = key.as.i;
            if (idx != 0 && idx != 1)
            {
              mi_error("mi_vm: STORE_INDEX pair index out of range\n");
              break;
            }
            mi_rt_pair_set(vm->rt, base.as.pair, (int)idx, value);
            break;
          }

          if (base.kind == MI_RT_VAL_DICT && base.as.dict)
          {
            (void) mi_rt_dict_set(vm->rt, base.as.dict, key, value);
            break;
          }

          mi_error("mi_vm: STORE_INDEX unsupported types\n");
        } break;

      case MI_VM_OP_LEN:
        {
          MiRtValue v = vm->regs[ins.b];
          if (v.kind == MI_RT_VAL_LIST && v.as.list)
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_int((int64_t)v.as.list->count));
            break;
          }

          if (v.kind == MI_RT_VAL_PAIR && v.as.pair)
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_int(2));
            break;
          }

          if (v.kind == MI_RT_VAL_DICT && v.as.dict)
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_int((int64_t)mi_rt_dict_count(v.as.dict)));
            break;
          }

          if (v.kind == MI_RT_VAL_KVREF)
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_int(2));
            break;
          }

          if (v.kind == MI_RT_VAL_STRING && v.as.s.ptr)
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_int((int64_t)v.as.s.length));
            break;
          }

          mi_error("mi_vm: LEN unsupported type\n");
          s_vm_reg_set(vm, ins.a, mi_rt_make_void());
        } break;

      case MI_VM_OP_NEG:
        {
          MiRtValue x = vm->regs[ins.b];
          if (x.kind == MI_RT_VAL_INT)
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_int(-x.as.i));
          }
          else if (x.kind == MI_RT_VAL_FLOAT)
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_float(-x.as.f));
          }
          else
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
          }
        } break;

      case MI_VM_OP_NOT:
        {
          MiRtValue x = vm->regs[ins.b];
          if (x.kind == MI_RT_VAL_BOOL)
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_bool(!x.as.b));
          }
          else
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
          }
        } break;

      case MI_VM_OP_ADD:
      case MI_VM_OP_SUB:
      case MI_VM_OP_MUL:
      case MI_VM_OP_DIV:
      case MI_VM_OP_MOD:
        {
          s_vm_reg_set(vm, ins.a, s_vm_binary_numeric((MiVmOp) ins.op, &vm->regs[ins.b], &vm->regs[ins.c]));
        } break;

      case MI_VM_OP_EQ:
      case MI_VM_OP_NEQ:
      case MI_VM_OP_LT:
      case MI_VM_OP_LTEQ:
      case MI_VM_OP_GT:
      case MI_VM_OP_GTEQ:
        {
          s_vm_reg_set(vm, ins.a, s_vm_binary_compare((MiVmOp) ins.op, &vm->regs[ins.b], &vm->regs[ins.c]));
        } break;

      case MI_VM_OP_AND:
        {
          MiRtValue x = vm->regs[ins.b];
          MiRtValue y = vm->regs[ins.c];
          if (x.kind == MI_RT_VAL_BOOL && y.kind == MI_RT_VAL_BOOL)
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_bool(x.as.b && y.as.b));
          }
          else
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
          }
        } break;

      case MI_VM_OP_OR:
        {
          MiRtValue x = vm->regs[ins.b];
          MiRtValue y = vm->regs[ins.c];
          if (x.kind == MI_RT_VAL_BOOL && y.kind == MI_RT_VAL_BOOL)
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_bool(x.as.b || y.as.b));
          }
          else
          {
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
          }
        } break;

      case MI_VM_OP_LOAD_VAR:
        {
          XSlice name = chunk->symbols[ins.imm];
          MiRtValue v;
          if (!mi_rt_var_get(vm->rt, name, &v))
          {
            mi_error_fmt("undefined variable: %.*s\n", (int) name.length, name.ptr);
            v = mi_rt_make_void();
          }
          s_vm_reg_set(vm, ins.a, v);
        } break;

      case MI_VM_OP_STORE_VAR:
        {
          XSlice name = chunk->symbols[ins.imm];
          (void) mi_rt_var_set(vm->rt, name, vm->regs[ins.a]);
        } break;

      case MI_VM_OP_DEFINE_VAR:
        {
          XSlice name = chunk->symbols[ins.imm];
          (void) mi_rt_var_define(vm->rt, name, vm->regs[ins.a]);
        } break;

      case MI_VM_OP_LOAD_INDIRECT_VAR:
        {
          MiRtValue n = vm->regs[ins.b];
          if (n.kind != MI_RT_VAL_STRING)
          {
            mi_error("indirect variable name must be string\n");
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            break;
          }
          MiRtValue v;
          if (!mi_rt_var_get(vm->rt, n.as.s, &v))
          {
            mi_error_fmt("undefined variable: %.*s\n", (int) n.as.s.length, n.as.s.ptr);
            v = mi_rt_make_void();
          }
          s_vm_reg_set(vm, ins.a, v);
        } break;

      case MI_VM_OP_ARG_CLEAR:
        s_vm_arg_clear(vm);
        break;

      case MI_VM_OP_ARG_PUSH:
        {
          if (vm->arg_top >= MI_VM_ARG_STACK_COUNT)
          {
            mi_error("mi_vm: arg stack overflow\n");
            break;
          }
          mi_rt_value_assign(vm->rt, &vm->arg_stack[vm->arg_top], vm->regs[ins.a]);
          vm->arg_top += 1;
        } break;

      case MI_VM_OP_ARG_PUSH_CONST:
        {
          if (vm->arg_top >= MI_VM_ARG_STACK_COUNT)
          {
            mi_error("mi_vm: arg stack overflow\n");
            break;
          }
          if (ins.imm < 0 || (size_t)ins.imm >= chunk->const_count)
          {
            mi_error("mi_vm: ARG_PUSH_CONST invalid const index\n");
            mi_rt_value_assign(vm->rt, &vm->arg_stack[vm->arg_top], mi_rt_make_void());
            vm->arg_top += 1;
            break;
          }
          mi_rt_value_assign(vm->rt, &vm->arg_stack[vm->arg_top], chunk->consts[ins.imm]);
          vm->arg_top += 1;
        } break;

      case MI_VM_OP_ARG_PUSH_VAR_SYM:
        {
          if (vm->arg_top >= MI_VM_ARG_STACK_COUNT)
          {
            mi_error("mi_vm: arg stack overflow\n");
            break;
          }
          if (ins.imm < 0 || (size_t)ins.imm >= chunk->symbol_count)
          {
            mi_error("mi_vm: ARG_PUSH_VAR_SYM invalid symbol index\n");
            mi_rt_value_assign(vm->rt, &vm->arg_stack[vm->arg_top], mi_rt_make_void());
            vm->arg_top += 1;
            break;
          }
          XSlice name = chunk->symbols[ins.imm];
          MiRtValue v;
          if (!mi_rt_var_get(vm->rt, name, &v))
          {
            mi_error_fmt("undefined variable: %.*s\n", (int)name.length, name.ptr);
            v = mi_rt_make_void();
          }
          mi_rt_value_assign(vm->rt, &vm->arg_stack[vm->arg_top], v);
          vm->arg_top += 1;
        } break;

      case MI_VM_OP_ARG_SAVE:
        {
          if (vm->arg_frame_depth >= MI_VM_ARG_FRAME_MAX)
          {
            mi_error("mi_vm: arg frame overflow\n");
            break;
          }
          int d = vm->arg_frame_depth;
          vm->arg_frame_tops[d] = vm->arg_top;
          for (int i = 0; i < vm->arg_top; ++i)
          {
            mi_rt_value_assign(vm->rt, &vm->arg_frames[d][i], vm->arg_stack[i]);
            mi_rt_value_assign(vm->rt, &vm->arg_stack[i], mi_rt_make_void());
          }
          vm->arg_top = 0;
          vm->arg_frame_depth += 1;
        } break;

      case MI_VM_OP_ARG_RESTORE:
        {
          if (vm->arg_frame_depth <= 0)
          {
            mi_error("mi_vm: arg frame underflow\n");
            break;
          }
          vm->arg_frame_depth -= 1;
          int d = vm->arg_frame_depth;
          int top = vm->arg_frame_tops[d];
          if (top < 0) top = 0;
          if (top > MI_VM_ARG_STACK_COUNT) top = MI_VM_ARG_STACK_COUNT;
          for (int i = 0; i < top; ++i)
          {
            mi_rt_value_assign(vm->rt, &vm->arg_stack[i], vm->arg_frames[d][i]);
            mi_rt_value_assign(vm->rt, &vm->arg_frames[d][i], mi_rt_make_void());
          }
          vm->arg_top = top;
        } break;

      case MI_VM_OP_CALL_CMD:
        {
          int argc = (int) ins.b;
          if (argc > vm->arg_top)
          {
            mi_error("mi_vm: arg stack underflow\n");
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            break;
          }

          MiRtValue argv[MI_VM_ARG_STACK_COUNT];
          int base = (int)vm->arg_top - argc;
          for (int i = 0; i < argc; i += 1)
          {
            argv[i] = vm->arg_stack[base + i];
            mi_rt_value_retain(vm->rt, argv[i]);
          }
          for (int i = 0; i < argc; i += 1)
          {
            mi_rt_value_assign(vm->rt, &vm->arg_stack[base + i], mi_rt_make_void());
          }
          vm->arg_top = base;

          XSlice cmd_name = chunk->cmd_names[ins.imm];

          /* Qualified call: a::b(:) ... resolved through chunk environments. */
          bool q_ok = false;
          MiRtValue q_ret = s_vm_exec_qualified_cmd(vm, cmd_name, argc, argv, &q_ok);
          if (q_ok)
          {
            s_vm_reg_set(vm, ins.a, q_ret);
            for (int i = 0; i < argc; i += 1)
            {
              mi_rt_value_release(vm->rt, argv[i]);
            }
            mi_rt_value_assign(vm->rt, &last, vm->regs[ins.a]);
            break;
          }

          /* Scoped commands: a local var may shadow a builtin. */
          MiRtValue scoped = mi_rt_make_void();
          if (mi_rt_var_get(vm->rt, cmd_name, &scoped) && scoped.kind == MI_RT_VAL_CMD)
          {
            s_vm_reg_set(vm, ins.a, s_vm_exec_cmd_value(vm, cmd_name, scoped, argc, argv));
            for (int i = 0; i < argc; i += 1)
            {
              mi_rt_value_release(vm->rt, argv[i]);
            }
            mi_rt_value_assign(vm->rt, &last, vm->regs[ins.a]);
            break;
          }

          MiVmCommandFn fn = chunk->cmd_fns[ins.imm];
          if (!fn)
          {
            mi_error("mi_vm: CALL_CMD null function\n");
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            for (int i = 0; i < argc; i += 1)
            {
              mi_rt_value_release(vm->rt, argv[i]);
            }
            break;
          }
          s_vm_reg_set(vm, ins.a, fn(vm, cmd_name, argc, argv));
          for (int i = 0; i < argc; i += 1)
          {
            mi_rt_value_release(vm->rt, argv[i]);
          }
          mi_rt_value_assign(vm->rt, &last, vm->regs[ins.a]);
        } break;

      case MI_VM_OP_CALL_CMD_DYN:
        {
          int argc = (int) ins.c;
          if (argc > vm->arg_top)
          {
            mi_error("mi_vm: arg stack underflow\n");
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            break;
          }

          MiRtValue head = vm->regs[ins.b];
          if (head.kind != MI_RT_VAL_STRING)
          {
            mi_error("mi_vm: dynamic command head must be string\n");
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            break;
          }

          MiRtValue argv[MI_VM_ARG_STACK_COUNT];
          int base = (int)vm->arg_top - argc;
          for (int i = 0; i < argc; i += 1)
          {
            argv[i] = vm->arg_stack[base + i];
            mi_rt_value_retain(vm->rt, argv[i]);
          }
          for (int i = 0; i < argc; i += 1)
          {
            mi_rt_value_assign(vm->rt, &vm->arg_stack[base + i], mi_rt_make_void());
          }
          vm->arg_top = base;

          /* Qualified call for dynamic heads. */
          bool q_ok = false;
          MiRtValue q_ret = s_vm_exec_qualified_cmd(vm, head.as.s, argc, argv, &q_ok);
          if (q_ok)
          {
            s_vm_reg_set(vm, ins.a, q_ret);
            for (int i = 0; i < argc; i += 1)
            {
              mi_rt_value_release(vm->rt, argv[i]);
            }
            mi_rt_value_assign(vm->rt, &last, vm->regs[ins.a]);
            break;
          }

          /* First: scoped commands stored as variables. */
          MiRtValue scoped = mi_rt_make_void();
          if (mi_rt_var_get(vm->rt, head.as.s, &scoped) && scoped.kind == MI_RT_VAL_CMD)
          {
            s_vm_reg_set(vm, ins.a, s_vm_exec_cmd_value(vm, head.as.s, scoped, argc, argv));
          }
          else
          {
            MiVmCommandFn fn = s_vm_find_command_fn(vm, head.as.s);
            if (!fn)
            {
              mi_error_fmt("mi_vm: unknown command: %.*s\n", (int)head.as.s.length, head.as.s.ptr);
              s_vm_reg_set(vm, ins.a, mi_rt_make_void());
              for (int i = 0; i < argc; i += 1)
              {
                mi_rt_value_release(vm->rt, argv[i]);
              }
              break;
            }

            s_vm_reg_set(vm, ins.a, fn(vm, head.as.s, argc, argv));
          }
          for (int i = 0; i < argc; i += 1)
          {
            mi_rt_value_release(vm->rt, argv[i]);
          }
          mi_rt_value_assign(vm->rt, &last, vm->regs[ins.a]);
        } break;

      case MI_VM_OP_CALL_BLOCK:
        {
          MiRtValue ret = s_vm_exec_block_value(vm, vm->regs[ins.b], chunk, vm->dbg_ip);
          s_vm_reg_set(vm, ins.a, ret);
          mi_rt_value_assign(vm->rt, &last, ret);
        } break;

      case MI_VM_OP_SCOPE_PUSH:
        {
          mi_rt_scope_push(vm->rt);
        } break;

      case MI_VM_OP_SCOPE_POP:
        {
          mi_rt_scope_pop(vm->rt);
        } break;

      case MI_VM_OP_JUMP:
        {
          int64_t npc = (int64_t)pc + (int64_t)ins.imm;
          if (npc < 0 || npc > (int64_t)chunk->code_count)
          {
            mi_error("mi_vm: JUMP out of range\n");
            return last;
          }
          pc = (size_t)npc;
        } break;

      case MI_VM_OP_JUMP_IF_TRUE:
      case MI_VM_OP_JUMP_IF_FALSE:
        {
          MiRtValue c = vm->regs[ins.a];
          bool is_true = false;
          if (c.kind == MI_RT_VAL_BOOL)
          {
            is_true = c.as.b;
          }
          else if (c.kind == MI_RT_VAL_INT)
          {
            is_true = (c.as.i != 0);
          }
          else if (c.kind == MI_RT_VAL_FLOAT)
          {
            is_true = (c.as.f != 0.0f);
          }
          else if (c.kind == MI_RT_VAL_STRING)
          {
            is_true = (c.as.s.length != 0);
          }

          bool take = ((MiVmOp)ins.op == MI_VM_OP_JUMP_IF_TRUE) ? is_true : !is_true;
          if (take)
          {
            int64_t npc = (int64_t)pc + (int64_t)ins.imm;
            if (npc < 0 || npc > (int64_t)chunk->code_count)
            {
              mi_error("mi_vm: JUMP_IF out of range\n");
              return last;
            }
            pc = (size_t)npc;
          }
        } break;

      case MI_VM_OP_RETURN:
        {
          MiRtValue ret = vm->regs[ins.a];
          mi_rt_value_retain(vm->rt, ret);
          return ret;
        }

      case MI_VM_OP_HALT:
        return last;

      default:
        mi_error_fmt("mi_vm: unhandled opcode: %u\n", (unsigned) ins.op);
        return last;
    }
  }

  return last;
}

//----------------------------------------------------------
// Disassembler
//----------------------------------------------------------

static const char* s_op_name(MiVmOp op)
{
  switch (op)
  {
    case MI_VM_OP_NOOP:               return "NOP";
    case MI_VM_OP_LOAD_CONST:         return "LDC";
    case MI_VM_OP_LOAD_BLOCK:         return "LDB";
    case MI_VM_OP_MOV:                return "MOV";
    case MI_VM_OP_LIST_NEW:           return "LNEW";
    case MI_VM_OP_DICT_NEW:           return "DNEW";
    case MI_VM_OP_LIST_PUSH:          return "LPUSH";
    case MI_VM_OP_ITER_NEXT:          return "ITNEXT";
    case MI_VM_OP_INDEX:              return "INDEX";
    case MI_VM_OP_STORE_INDEX:        return "STINDEX";
    case MI_VM_OP_LEN:                return "LEN";
    case MI_VM_OP_NEG:                return "NEG";
    case MI_VM_OP_NOT:                return "NOT";
    case MI_VM_OP_ADD:                return "ADD";
    case MI_VM_OP_SUB:                return "SUB";
    case MI_VM_OP_MUL:                return "MUL";
    case MI_VM_OP_DIV:                return "DIV";
    case MI_VM_OP_MOD:                return "MOD";
    case MI_VM_OP_EQ:                 return "EQ";
    case MI_VM_OP_NEQ:                return "NEQ";
    case MI_VM_OP_LT:                 return "LT";
    case MI_VM_OP_LTEQ:               return "LTEQ";
    case MI_VM_OP_GT:                 return "GT";
    case MI_VM_OP_GTEQ:               return "GTEQ";
    case MI_VM_OP_AND:                return "AND";
    case MI_VM_OP_OR:                 return "OR";
    case MI_VM_OP_LOAD_VAR:           return "LDV";
    case MI_VM_OP_STORE_VAR:          return "STV";
    case MI_VM_OP_DEFINE_VAR:         return "DEFV";
    case MI_VM_OP_LOAD_INDIRECT_VAR:  return "LDIV";
    case MI_VM_OP_ARG_CLEAR:          return "ACLR";
    case MI_VM_OP_ARG_PUSH:           return "APR";
    case MI_VM_OP_ARG_PUSH_CONST:     return "APC";
    case MI_VM_OP_ARG_PUSH_VAR_SYM:   return "APV";
    case MI_VM_OP_ARG_PUSH_SYM:       return "APS";
    case MI_VM_OP_ARG_SAVE:           return "ASAVE";
    case MI_VM_OP_ARG_RESTORE:        return "AREST";
    case MI_VM_OP_CALL_CMD:           return "CALL";
    case MI_VM_OP_CALL_CMD_DYN:       return "DCALL";
    case MI_VM_OP_CALL_BLOCK:         return "BCALL";
    case MI_VM_OP_SCOPE_PUSH:         return "SPUSH";
    case MI_VM_OP_SCOPE_POP:          return "SPOP";
    case MI_VM_OP_JUMP:               return "JMP";
    case MI_VM_OP_JUMP_IF_TRUE:       return "JT";
    case MI_VM_OP_JUMP_IF_FALSE:      return "JF";
    case MI_VM_OP_RETURN:             return "RET";
    case MI_VM_OP_HALT:               return "HALT";
    default: mi_error_fmt("Unknown opcode %X\n", op); return "?";
  }
}

static void s_vm_disasm_ex(const MiVmChunk* chunk, const MiVmChunk** stack, size_t depth)
{
  if (!chunk)
  {
    return;
  }

  /* Cycle/alias guard: chunks can be shared, and bugs can create cycles.
     Disassembler must not recurse forever. */
  for (size_t i = 0; i < depth; ++i)
  {
    if (stack[i] == chunk)
    {
      printf("=== VM CHUNK (cycle detected) ===\n");
      return;
    }
  }

  const MiVmChunk* local_stack[128];
  if (depth < (sizeof(local_stack) / sizeof(local_stack[0])))
  {
    /* Copy the incoming stack so the function can be called with NULL. */
    for (size_t i = 0; i < depth; ++i)
    {
      local_stack[i] = stack[i];
    }
    local_stack[depth] = chunk;
    stack = local_stack;
    depth += 1;
  }

  printf("=== VM CHUNK ===\n");
  printf("code:   %zu ins\n", chunk->code_count);
  printf("consts: %zu\n", chunk->const_count);
  printf("syms:   %zu\n", chunk->symbol_count);
  printf("cmds:   %zu\n", chunk->cmd_count);
  printf("subs:   %zu\n", chunk->subchunk_count);

  if (chunk->const_count)
  {
    printf("\n-- const pool --\n");
    for (size_t i = 0; i < chunk->const_count; ++i)
    {
      printf("  const_%zu ", i);
      s_vm_print_value_inline(&chunk->consts[i]);
      printf("\n");
    }
  }

  if (chunk->symbol_count)
  {
    printf("\n-- symbols --\n");
    for (size_t i = 0; i < chunk->symbol_count; ++i)
    {
      XSlice s = chunk->symbols[i];
      printf("  sym_%zu %.*s\n", i, (int)s.length, s.ptr);
    }
  }

  if (chunk->cmd_count)
  {
    printf("\n-- commands --\n");
    for (size_t i = 0; i < chunk->cmd_count; ++i)
    {
      const char* name = "<unnamed>";
      int name_len = 9;
      if (chunk->cmd_names)
      {
        XSlice s = chunk->cmd_names[i];
        name = s.ptr;
        name_len = (int)s.length;
      }
      printf("  cmd_%zu %.*s\n", i, name_len, name);
    }
  }

  printf("\n-- code --\n");
  for (size_t i = 0; i < chunk->code_count; ++i)
  {
    MiVmIns ins = chunk->code[i];
    MiVmOp op = (MiVmOp)ins.op;

    uint8_t bytes[8];
    bytes[0] = ins.op;
    bytes[1] = ins.a;
    bytes[2] = ins.b;
    bytes[3] = ins.c;
    uint32_t uimm = (uint32_t)ins.imm;
    bytes[4] = (uint8_t)(uimm & 0xFFu);
    bytes[5] = (uint8_t)((uimm >> 8) & 0xFFu);
    bytes[6] = (uint8_t)((uimm >> 16) & 0xFFu);
    bytes[7] = (uint8_t)((uimm >> 24) & 0xFFu);

    size_t pc = i * sizeof(MiVmIns);

    /* Print prefix: pc + raw bytes */
    printf("0x%08zx  %02X %02X %02X %02X %02X %02X %02X %02X   ",
        pc,
        (unsigned)bytes[0],
        (unsigned)bytes[1],
        (unsigned)bytes[2],
        (unsigned)bytes[3],
        (unsigned)bytes[4],
        (unsigned)bytes[5],
        (unsigned)bytes[6],
        (unsigned)bytes[7]);

    /* Build the instruction text (no comment) and an optional comment,
       then print them with a fixed comment column. */
    char instr[160];
    char comment[160];
    instr[0] = '\0';
    comment[0] = '\0';

    switch (op)
    {
      case MI_VM_OP_LOAD_CONST:
        {
          (void)snprintf(instr, sizeof(instr), "%s r%u, const_%d", s_op_name(op), (unsigned)ins.a, (int)ins.imm);

          if (ins.imm >= 0 && (size_t)ins.imm < chunk->const_count)
          {
            char vbuf[96];
            vbuf[0] = '\0';
            s_vm_value_to_string(vbuf, sizeof(vbuf), &chunk->consts[(size_t)ins.imm]);
            (void)snprintf(comment, sizeof(comment), "const_%d %s", (int)ins.imm, vbuf);
          }
          else
          {
            (void)snprintf(comment, sizeof(comment), "<oob>");
          }
        } break;

      case MI_VM_OP_LOAD_BLOCK:
        (void)snprintf(instr, sizeof(instr), "%s r%u, %d", s_op_name(op), (unsigned)ins.a, (int)ins.imm);
        (void)snprintf(comment, sizeof(comment), "subchunk[%d]", (int)ins.imm);
        break;

      case MI_VM_OP_MOV:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b);
        break;

      case MI_VM_OP_LIST_NEW:
        (void)snprintf(instr, sizeof(instr), "%s r%u", s_op_name(op), (unsigned)ins.a);
        break;

      case MI_VM_OP_DICT_NEW:
        (void)snprintf(instr, sizeof(instr), "%s r%u", s_op_name(op), (unsigned)ins.a);
        break;

      case MI_VM_OP_LIST_PUSH:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b);
        break;

      case MI_VM_OP_ITER_NEXT:
        {
          unsigned dst_item = (unsigned)((uint32_t)ins.imm & 0xFFu);
          (void)snprintf(instr, sizeof(instr), "%s r%u, r%u, r%u -> r%u", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b, (unsigned)ins.c, dst_item);
        } break;

      case MI_VM_OP_INDEX:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u, r%u", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b, (unsigned)ins.c);
        break;

      case MI_VM_OP_STORE_INDEX:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u, r%u", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b, (unsigned)ins.c);
        break;

      case MI_VM_OP_LEN:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b);
        break;

      case MI_VM_OP_NEG:
      case MI_VM_OP_NOT:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b);
        break;

      case MI_VM_OP_ADD:
      case MI_VM_OP_SUB:
      case MI_VM_OP_MUL:
      case MI_VM_OP_DIV:
      case MI_VM_OP_MOD:
      case MI_VM_OP_EQ:
      case MI_VM_OP_NEQ:
      case MI_VM_OP_LT:
      case MI_VM_OP_LTEQ:
      case MI_VM_OP_GT:
      case MI_VM_OP_GTEQ:
      case MI_VM_OP_AND:
      case MI_VM_OP_OR:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u, r%u", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b, (unsigned)ins.c);
        break;

      case MI_VM_OP_LOAD_VAR:
        (void)snprintf(instr, sizeof(instr), "%s r%u, %d", s_op_name(op), (unsigned)ins.a, (int)ins.imm);
        if (ins.imm >= 0 && (size_t)ins.imm < chunk->symbol_count)
        {
          XSlice s = chunk->symbols[(size_t)ins.imm];
          (void)snprintf(comment, sizeof(comment), "sym_%d %.*s", (int)ins.imm, (int)s.length, s.ptr);
        }
        else
        {
          (void)snprintf(comment, sizeof(comment), "<oob>");
        }
        break;

      case MI_VM_OP_STORE_VAR:
        (void)snprintf(instr, sizeof(instr), "%s %d, r%u", s_op_name(op), (int)ins.imm, (unsigned)ins.a);
        if (ins.imm >= 0 && (size_t)ins.imm < chunk->symbol_count)
        {
          XSlice s = chunk->symbols[(size_t)ins.imm];
          (void)snprintf(comment, sizeof(comment), "sym_%d %.*s", (int)ins.imm, (int)s.length, s.ptr);
        }
        else
        {
          (void)snprintf(comment, sizeof(comment), "<oob>");
        }
        break;

      case MI_VM_OP_DEFINE_VAR:
        (void)snprintf(instr, sizeof(instr), "%s %d, r%u", s_op_name(op), (int)ins.imm, (unsigned)ins.a);
        if (ins.imm >= 0 && (size_t)ins.imm < chunk->symbol_count)
        {
          XSlice s = chunk->symbols[(size_t)ins.imm];
          (void)snprintf(comment, sizeof(comment), "sym_%d %.*s", (int)ins.imm, (int)s.length, s.ptr);
        }
        else
        {
          (void)snprintf(comment, sizeof(comment), "<oob>");
        }
        break;

      case MI_VM_OP_LOAD_INDIRECT_VAR:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b);
        break;

      case MI_VM_OP_ARG_CLEAR:
        (void)snprintf(instr, sizeof(instr), "%s", s_op_name(op));
        break;

      case MI_VM_OP_ARG_SAVE:
      case MI_VM_OP_ARG_RESTORE:
        (void)snprintf(instr, sizeof(instr), "%s", s_op_name(op));
        break;

      case MI_VM_OP_ARG_PUSH:
        (void)snprintf(instr, sizeof(instr), "%s r%u", s_op_name(op), (unsigned)ins.a);
        break;

      case MI_VM_OP_ARG_PUSH_CONST:
        {
          /* IMPORTANT: keep previous behavior: print inline value, comment is const_N */
          char vbuf[96];
          vbuf[0] = '\0';

          (void)snprintf(instr, sizeof(instr), "%s ", s_op_name(op));

          if (ins.imm >= 0 && (size_t)ins.imm < chunk->const_count)
          {
            (void)snprintf(comment, sizeof(comment), "const_%d", (int)ins.imm);
            (void)snprintf(vbuf, sizeof(vbuf), "%d", 0);
          }
          else
          {
            (void)snprintf(comment, sizeof(comment), "<oob>");
          }

          (void)vbuf;
        } break;

      case MI_VM_OP_ARG_PUSH_SYM:
        (void)snprintf(instr, sizeof(instr), "%s %d", s_op_name(op), (int)ins.imm);
        if (ins.imm >= 0 && (size_t)ins.imm < chunk->symbol_count)
        {
          XSlice s = chunk->symbols[(size_t)ins.imm];
          (void)snprintf(comment, sizeof(comment), "sym_%d %.*s", (int)ins.imm, (int)s.length, s.ptr);
        }
        else
        {
          (void)snprintf(comment, sizeof(comment), "<oob>");
        }
        break;

      case MI_VM_OP_ARG_PUSH_VAR_SYM:
        (void)snprintf(instr, sizeof(instr), "%s %d", s_op_name(op), (int)ins.imm);
        if (ins.imm >= 0 && (size_t)ins.imm < chunk->symbol_count)
        {
          XSlice s = chunk->symbols[(size_t)ins.imm];
          (void)snprintf(comment, sizeof(comment), "sym_%d %.*s", (int)ins.imm, (int)s.length, s.ptr);
        }
        else
        {
          (void)snprintf(comment, sizeof(comment), "<oob>");
        }
        break;

      case MI_VM_OP_CALL_CMD:
        {
          const char* name = "cmd";
          int name_len = 3;
          if (chunk->cmd_names && ins.imm >= 0 && (size_t)ins.imm < chunk->cmd_count)
          {
            XSlice s = chunk->cmd_names[(size_t)ins.imm];
            name = s.ptr;
            name_len = (int)s.length;
          }
          (void)snprintf(instr, sizeof(instr), "%s r%u, %u, %.*s", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b, name_len, name);
          (void)snprintf(comment, sizeof(comment), "cmd_%d", (int)ins.imm);
        } break;

      case MI_VM_OP_CALL_CMD_DYN:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u, %u", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b, (unsigned)ins.c);
        break;

      case MI_VM_OP_CALL_BLOCK:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u, argc=%u", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b, (unsigned)ins.c);
        break;

      case MI_VM_OP_SCOPE_PUSH:
      case MI_VM_OP_SCOPE_POP:
        (void)snprintf(instr, sizeof(instr), "%s", s_op_name(op));
        break;

      case MI_VM_OP_JUMP:
        {
          (void)snprintf(instr, sizeof(instr), "%s %d", s_op_name(op), (int)ins.imm);

          int64_t ins_target = (int64_t)i + (int64_t)ins.imm;
          uint64_t pc_target = (uint64_t)pc + ((uint64_t)(int64_t)ins.imm * (uint64_t)sizeof(MiVmIns));
          if (ins_target < 0 || (size_t)ins_target >= chunk->code_count)
          {
            (void)snprintf(comment, sizeof(comment), "-> 0x%08zx (OOB)", (size_t)pc_target);
          }
          else
          {
            (void)snprintf(comment, sizeof(comment), "-> 0x%08zx", (size_t)pc_target);
          }
        } break;

      case MI_VM_OP_JUMP_IF_TRUE:
        {
          (void)snprintf(instr, sizeof(instr), "%s r%u, %d", s_op_name(op), (unsigned)ins.a, (int)ins.imm);

          int64_t ins_target = (int64_t)i + (int64_t)ins.imm;
          uint64_t pc_target = (uint64_t)pc + ((uint64_t)(int64_t)ins.imm * (uint64_t)sizeof(MiVmIns));
          if (ins_target < 0 || (size_t)ins_target >= chunk->code_count)
          {
            (void)snprintf(comment, sizeof(comment), "-> 0x%08zx (OOB)", (size_t)pc_target);
          }
          else
          {
            (void)snprintf(comment, sizeof(comment), "-> 0x%08zx", (size_t)pc_target);
          }
        } break;

      case MI_VM_OP_JUMP_IF_FALSE:
        {
          (void)snprintf(instr, sizeof(instr), "%s r%u, %d", s_op_name(op), (unsigned)ins.a, (int)ins.imm);

          int64_t ins_target = (int64_t)i + (int64_t)ins.imm;
          uint64_t pc_target = (uint64_t)pc + ((uint64_t)(int64_t)ins.imm * (uint64_t)sizeof(MiVmIns));
          if (ins_target < 0 || (size_t)ins_target >= chunk->code_count)
          {
            (void)snprintf(comment, sizeof(comment), "-> 0x%08zx (OOB)", (size_t)pc_target);
          }
          else
          {
            (void)snprintf(comment, sizeof(comment), "-> 0x%08zx", (size_t)pc_target);
          }
        } break;

      case MI_VM_OP_RETURN:
        (void)snprintf(instr, sizeof(instr), "%s r%u", s_op_name(op), (unsigned)ins.a);
        break;

      case MI_VM_OP_HALT:
        (void)snprintf(instr, sizeof(instr), "%s r%u", s_op_name(op), (unsigned)ins.a);
        (void)snprintf(comment, sizeof(comment), "");
        break;

      default:
        (void)snprintf(instr, sizeof(instr), "%s a=%u b=%u c=%u imm=%d", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b, (unsigned)ins.c, (int)ins.imm);
        break;
    }

    /* Special-case: ARG_PUSH_CONST must print value inline, not an index. */
    if (op == MI_VM_OP_ARG_PUSH_CONST)
    {
      if (ins.imm >= 0 && (size_t)ins.imm < chunk->const_count)
      {
        char vbuf[64];
        vbuf[0] = '\0';
        s_vm_value_to_string(vbuf, sizeof(vbuf), &chunk->consts[(size_t)ins.imm]);
        (void)snprintf(instr, sizeof(instr), "%s %s", s_op_name(op), vbuf);
        (void)snprintf(comment, sizeof(comment), "const_%d", (int)ins.imm);
      }
      else
      {
        (void)snprintf(instr, sizeof(instr), "%s <oob>", s_op_name(op));
        (void)snprintf(comment, sizeof(comment), "<oob>");
      }
    }

    /* Print with fixed comment column */
    printf("%-32s", instr);
    if (comment[0] || op == MI_VM_OP_HALT)
    {
      printf("  #  %s", comment);
    }
    printf("\n");
  }

  if (chunk->subchunk_count)
  {
    for (size_t i = 0; i < chunk->subchunk_count; ++i)
    {
      printf("\n=== SUBCHUNK %zu ===\n", i);
      s_vm_disasm_ex(chunk->subchunks[i], stack, depth);
    }
  }

  printf("\n");
}

void mi_vm_disasm(const MiVmChunk* chunk)
{
  s_vm_disasm_ex(chunk, NULL, 0);
}

