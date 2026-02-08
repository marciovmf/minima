#include <stdx_common.h>
#include "mi_vm.h"


static const MiVmApi s_vm_api =
{
  mi_rt_make_int,
  mi_rt_make_float,
  mi_rt_make_bool,
  mi_rt_make_void,
  mi_vm_trace_print,
  mi_vm_register_native,
  mi_vm_namespace_add_native,
  mi_vm_register_native_sigv,
  mi_vm_register_native_sigv_var,
  mi_vm_namespace_add_native_sigv,
  mi_vm_namespace_add_native_sigv_var,
  mi_vm_namespace_add_value,
};

#include "mi_log.h"
#include "mi_mx.h"
#include "mi_compile.h"
#include "stdx_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <stdx_io.h>
#include <stdx_filesystem.h>
#include <stdx_strbuilder.h>

static const char* s_vm_kind_name(MiRtValueKind kind);

static XSlice s_vm_current_script_file(MiVm* vm)
{
  if (!vm)
  {
    return x_slice_empty();
  }

  if (vm->dbg_chunk && vm->dbg_chunk->dbg_file.length)
  {
    return vm->dbg_chunk->dbg_file;
  }

  // Some subchunks may not carry dbg_file; walk the VM call stack to find
  // the nearest caller chunk with a file name.
  for (int i = vm->call_depth - 1; i >= 0; --i)
  {
    const MiVmChunk* c = vm->call_stack[i].caller_chunk;
    if (c && c->dbg_file.length)
    {
      return c->dbg_file;
    }
  }

  return x_slice_empty();
}

static void s_vm_print_source_context_from_file(XSlice file, uint32_t line, uint32_t col)
{
  if (!file.ptr || file.length == 0 || line == 0)
  {
    return;
  }

  XFSPath path;
  x_fs_path_set_slice(&path, file);

  char* src_ptr;
  size_t src_size;

  src_ptr = x_io_read_text(path.buf, &src_size);
  if (!src_ptr)
  {
    mi_error_fmt("Failed to read file '%s'", path.buf);
    return;
  }

  XSlice src = x_slice_init(src_ptr, src_size);

  int cur_line = 1;
  size_t i = 0;
  size_t line_start = 0;
  size_t line_end = src.length;

  while (i < src.length)
  {
    if (cur_line == (int)line)
    {
      line_start = i;
      break;
    }
    if (src.ptr[i] == '\n')
    {
      cur_line += 1;
    }
    i += 1;
  }

  i = line_start;
  while (i < src.length)
  {
    if (src.ptr[i] == '\n')
    {
      line_end = i;
      break;
    }
    i += 1;
  }

  if (line_start < src.length && line_end > line_start)
  {
    XSlice ln = x_slice_init(src.ptr + line_start, line_end - line_start);
    mi_error_fmt("  %.*s\n", (int)ln.length, ln.ptr);

    uint32_t caret_col = col;
    if (caret_col == 0)
    {
      caret_col = 1;
    }

    mi_error("  ");
    for (uint32_t c = 1; c < caret_col; c += 1)
    {
      mi_error(" ");
    }
    mi_error("^\n");
  }

  free(src_ptr);
}

static void s_vm_report_error(MiVm* vm, const char* msg)
{
  if (!vm)
  {
    mi_error_fmt("mi_vm: %s\n", msg ? msg : "error");
    return;
  }

  const MiVmChunk* chunk = vm->dbg_chunk;
  uint32_t line = 0;
  uint32_t col = 0;
  XSlice file = x_slice_empty();

  if (chunk && chunk->dbg_lines && chunk->dbg_cols && vm->dbg_ip < chunk->code_count)
  {
    line = chunk->dbg_lines[vm->dbg_ip];
    col = chunk->dbg_cols[vm->dbg_ip];
  }
  if (chunk)
  {
    file = chunk->dbg_file;
  }

  if (file.ptr && file.length > 0 && line > 0)
  {
    mi_error_fmt("Runtime error: %s (%.*s:%u:%u)\n",
        msg ? msg : "error",
        (int)file.length,
        file.ptr,
        (unsigned)line,
        (unsigned)col);
  }
  else
  {
    mi_error_fmt("Runtime error: %s\n", msg ? msg : "error");
  }

  // Print call stack (most recent last). 
  if (vm->call_depth > 0)
  {
    mi_error("Call stack:\n");
    for (int i = vm->call_depth - 1; i >= 0; i -= 1)
    {
      MiVmCallFrame* fr = &vm->call_stack[i];
      if (fr->caller_chunk && fr->caller_chunk->dbg_file.ptr && fr->caller_chunk->dbg_file.length > 0 &&
          fr->caller_chunk->dbg_lines && fr->caller_ip < fr->caller_chunk->code_count)
      {
        uint32_t fl = fr->caller_chunk->dbg_lines[fr->caller_ip];
        uint32_t fc = fr->caller_chunk->dbg_cols ? fr->caller_chunk->dbg_cols[fr->caller_ip] : 0;
        if (fr->kind == MI_VM_CALL_FRAME_USER_CMD)
        {
          mi_error_fmt("  cmd %.*s at %.*s:%u:%u\n",
              (int)fr->name.length,
              fr->name.ptr,
              (int)fr->caller_chunk->dbg_file.length,
              fr->caller_chunk->dbg_file.ptr,
              (unsigned)fl,
              (unsigned)fc);
        }
        else
        {
          mi_error_fmt("  block at %.*s:%u:%u\n",
              (int)fr->caller_chunk->dbg_file.length,
              fr->caller_chunk->dbg_file.ptr,
              (unsigned)fl,
              (unsigned)fc);
        }
      }
      else
      {
        if (fr->kind == MI_VM_CALL_FRAME_USER_CMD)
        {
          mi_error_fmt("  cmd %.*s\n", (int)fr->name.length, fr->name.ptr);
        }
        else
        {
          mi_error("  block\n");
        }
      }
    }
  }

  if (file.ptr && file.length > 0 && line > 0)
  {
    s_vm_print_source_context_from_file(file, line, col);
  }
}


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
    mi_error("mi_vm: out of memory\n");
    exit(1);
  }
  return p;
}

static void s_vm_reg_set(MiVm* vm, uint8_t r, MiRtValue v)
{
  MI_ASSERT(vm);
  MI_ASSERT(r < MI_VM_REG_COUNT);
  mi_rt_value_assign(vm->rt, &vm->regs[r], v);
}

static uint32_t s_vm_chunk_sym_id(MiVm* vm, MiVmChunk* chunk, int32_t sym_index)
{
  if (!vm || !chunk)
  {
    return 0u;
  }

  if (sym_index < 0 || (size_t)sym_index >= chunk->symbol_count)
  {
    return 0u;
  }

  if (!chunk->symbol_ids)
  {
    chunk->symbol_ids = (uint32_t*)s_realloc(NULL, chunk->symbol_capacity * sizeof(*chunk->symbol_ids));
    for (size_t i = 0; i < chunk->symbol_capacity; i += 1)
    {
      chunk->symbol_ids[i] = UINT32_MAX;
    }
  }

  if (chunk->symbol_ids[sym_index] == UINT32_MAX)
  {
    chunk->symbol_ids[sym_index] = mi_rt_sym_intern(vm->rt, chunk->symbols[sym_index]);
  }

  return chunk->symbol_ids[sym_index];
}


static void s_vm_arg_clear(MiVm* vm)
{
  MI_ASSERT(vm);
  MI_ASSERT(vm->arg_top >= 0);
  MI_ASSERT(vm->arg_top <= MI_VM_ARG_STACK_COUNT);
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

static const char* s_vm_type_name(MiTypeKind t)
{
  switch (t)
  {
    case MI_TYPE_VOID: return "void";
    case MI_TYPE_BOOL: return "bool";
    case MI_TYPE_INT: return "int";
    case MI_TYPE_FLOAT: return "float";
    case MI_TYPE_STRING: return "string";
    case MI_TYPE_LIST: return "list";
    case MI_TYPE_DICT: return "dict";
    case MI_TYPE_BLOCK: return "block";
    case MI_TYPE_FUNC: return "func";
    case MI_TYPE_ANY: return "any";
    default: return "unknown";
  }
}

static bool s_vm_type_matches(MiTypeKind expected, const MiRtValue* v)
{
  if (expected == MI_TYPE_ANY)
  {
    return true;
  }

  switch (expected)
  {
    case MI_TYPE_VOID: return v->kind == MI_RT_VAL_VOID;
    case MI_TYPE_BOOL: return v->kind == MI_RT_VAL_BOOL;
    case MI_TYPE_INT: return v->kind == MI_RT_VAL_INT;
    case MI_TYPE_FLOAT: return v->kind == MI_RT_VAL_FLOAT;
    case MI_TYPE_STRING: return v->kind == MI_RT_VAL_STRING;
    case MI_TYPE_LIST: return v->kind == MI_RT_VAL_LIST;
    case MI_TYPE_DICT: return v->kind == MI_RT_VAL_DICT;
    case MI_TYPE_BLOCK: return v->kind == MI_RT_VAL_BLOCK;
    case MI_TYPE_FUNC: return v->kind == MI_RT_VAL_CMD;
    default: return false;
  }
}

static bool s_vm_check_sig(MiVm* vm, const MiFuncTypeSig* sig, XSlice cmd_name, int argc, const MiRtValue* argv)
{
  (void)vm;
  if (!sig)
  {
    return true;
  }

  if (!sig->is_variadic)
  {
    if (argc != sig->param_count)
    {
      mi_error_fmt("%.*s: expected %d args, got %d\n", (int)cmd_name.length, cmd_name.ptr, sig->param_count, argc);
      return false;
    }
  }
  else
  {
    if (argc < sig->param_count)
    {
      mi_error_fmt("%.*s: expected at least %d args, got %d\n", (int)cmd_name.length, cmd_name.ptr, sig->param_count, argc);
      return false;
    }
  }

  // Fixed params. 
  for (int i = 0; i < sig->param_count && i < argc; ++i)
  {
    MiTypeKind expected = sig->param_types ? sig->param_types[i] : MI_TYPE_ANY;
    if (!s_vm_type_matches(expected, &argv[i]))
    {
      mi_error_fmt("%.*s: arg %d expected %s, got %s\n",
          (int)cmd_name.length,
          cmd_name.ptr,
          i,
          s_vm_type_name(expected),
          s_vm_kind_name(argv[i].kind));
      return false;
    }
  }

  // Variadic tail. 
  if (sig->is_variadic && argc > sig->param_count)
  {
    MiTypeKind vt = sig->variadic_type;
    if (vt != MI_TYPE_ANY)
    {
      for (int i = sig->param_count; i < argc; ++i)
      {
        if (!s_vm_type_matches(vt, &argv[i]))
        {
          mi_error_fmt("%.*s: arg %d expected %s, got %s\n",
              (int)cmd_name.length,
              cmd_name.ptr,
              i,
              s_vm_type_name(vt),
              s_vm_kind_name(argv[i].kind));
          return false;
        }
      }
    }
  }

  return true;
}

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
    // Drop frames instead of crashing; trace becomes less useful but program runs. 
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

static MiRtValue s_vm_cmd_dict(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
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

static MiRtValue s_vm_cmd_list(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;

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

static MiRtValue s_vm_cmd_len(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  if (!vm || !vm->rt)
  {
    return mi_rt_make_void();
  }

  (void)user;

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

static MiRtValue s_vm_cmd_warning(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;

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

static MiRtValue s_vm_cmd_error(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;

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

static MiRtValue s_vm_cmd_fatal(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;

  (void)s_vm_cmd_error(vm, NULL, argc, argv);
  exit(1);
}

static MiRtValue s_vm_cmd_assert(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;

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
      (void)s_vm_cmd_fatal(vm, NULL, 1, &one);
    }
    else
    {
      (void)s_vm_cmd_fatal(vm, NULL, 1, &argv[1]);
    }
  }

  mi_error("assert: failed\n");
  exit(1);
}

static MiRtValue s_vm_cmd_type(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;

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

static MiRtValue s_vm_cmd_typeof(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;

  if (argc != 1)
  {
    mi_error("typeof: expected 1 argument\n");
    return mi_rt_make_void();
  }

  return mi_rt_make_type(argv[0].kind);
}

static MiRtValue s_vm_cmd_print(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void) vm;
  (void) user;

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

  const char* chunk_name = (chunk->dbg_name.ptr && chunk->dbg_name.length) ? (const char*)chunk->dbg_name.ptr : "<chunk>";
  const char* file_name  = (chunk->dbg_file.ptr && chunk->dbg_file.length) ? (const char*)chunk->dbg_file.ptr : "<unknown>";

  uint32_t line = 0;
  uint32_t col  = 0;
  if (chunk->dbg_lines && chunk->dbg_cols && ip < chunk->code_count)
  {
    line = chunk->dbg_lines[ip];
    col  = chunk->dbg_cols[ip];
  }

  if (ip >= chunk->code_count)
  {
    printf("  %s %s %s:%u:%u ip=%zu <out-of-range>\n",
      label ? label : "",
      chunk_name,
      file_name,
      (unsigned)line,
      (unsigned)col,
      ip);
    return;
  }

  const MiVmIns ins = chunk->code[ip];
  const char* opname = s_op_name((MiVmOp)ins.op);

  printf("  %s %s %s:%u:%u ip=%zu %s a=%u b=%u c=%u imm=%d\n",
    label ? label : "",
    chunk_name,
    file_name,
    (unsigned)line,
    (unsigned)col,
    ip,
    opname ? opname : "<?>",
    (unsigned)ins.a,
    (unsigned)ins.b,
    (unsigned)ins.c,
    (int)ins.imm);
}

void mi_vm_trace_print(MiVm* vm)
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

static MiRtValue s_vm_cmd_trace(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
  (void)argv;

  if (argc != 0)
  {
    mi_error("trace: expected 0 arguments\n");
    return mi_rt_make_void();
  }

  mi_vm_trace_print(vm);
  return mi_rt_make_void();
}

static MiRtValue s_vm_cmd_argc(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
  (void)argv;
  if (!vm)
  {
    return mi_rt_make_int(0);
  }
  if (argc != 0)
  {
    mi_error("argc: expected 0 arguments\n");
    return mi_rt_make_int(0);
  }
  return mi_rt_make_int((int64_t)vm->cur_argc);
}

static MiRtValue s_vm_cmd_arg(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
  if (!vm)
  {
    return mi_rt_make_void();
  }
  if (argc != 1 || argv[0].kind != MI_RT_VAL_INT)
  {
    mi_error("arg: expected 1 int argument\n");
    return mi_rt_make_void();
  }
  int64_t i = argv[0].as.i;
  if (i < 0 || i >= (int64_t)vm->cur_argc)
  {
    return mi_rt_make_void();
  }
  return vm->cur_argv ? vm->cur_argv[i] : mi_rt_make_void();
}

static MiRtValue s_vm_cmd_arg_type(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
  if (!vm)
  {
    return mi_rt_make_type(MI_TYPE_VOID);
  }
  if (argc != 1 || argv[0].kind != MI_RT_VAL_INT)
  {
    mi_error("arg_type: expected 1 int argument\n");
    return mi_rt_make_type(MI_TYPE_VOID);
  }
  int64_t i = argv[0].as.i;
  if (i < 0 || i >= (int64_t)vm->cur_argc)
  {
    return mi_rt_make_type(MI_TYPE_VOID);
  }
  MiRtValue v0 = vm->cur_argv ? vm->cur_argv[i] : mi_rt_make_void();
  /* Map runtime kind to type kind (approximate). */
  switch (v0.kind)
  {
    case MI_RT_VAL_VOID:   return mi_rt_make_type(MI_TYPE_VOID);
    case MI_RT_VAL_INT:    return mi_rt_make_type(MI_TYPE_INT);
    case MI_RT_VAL_FLOAT:  return mi_rt_make_type(MI_TYPE_FLOAT);
    case MI_RT_VAL_BOOL:   return mi_rt_make_type(MI_TYPE_BOOL);
    case MI_RT_VAL_STRING: return mi_rt_make_type(MI_TYPE_STRING);
    case MI_RT_VAL_LIST:   return mi_rt_make_type(MI_TYPE_LIST);
    case MI_RT_VAL_DICT:   return mi_rt_make_type(MI_TYPE_DICT);
    case MI_RT_VAL_BLOCK:  return mi_rt_make_type(MI_TYPE_BLOCK);
    case MI_RT_VAL_CMD:    return mi_rt_make_type(MI_TYPE_FUNC);
    case MI_RT_VAL_PAIR:   return mi_rt_make_type(MI_TYPE_ANY);
    case MI_RT_VAL_KVREF:  return mi_rt_make_type(MI_TYPE_ANY);
    case MI_RT_VAL_TYPE:   return mi_rt_make_type(MI_TYPE_ANY);
    default:               return mi_rt_make_type(MI_TYPE_ANY);
  }
}

static MiRtValue s_vm_cmd_arg_name(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
  if (!vm || !vm->cur_cmd)
  {
    return mi_rt_make_string_slice(x_slice_init("", 0));
  }
  if (argc != 1 || argv[0].kind != MI_RT_VAL_INT)
  {
    mi_error("arg_name: expected 1 int argument\n");
    return mi_rt_make_string_slice(x_slice_init("", 0));
  }
  int64_t i = argv[0].as.i;
  if (i < 0 || i >= (int64_t)vm->cur_argc)
  {
    return mi_rt_make_string_slice(x_slice_init("", 0));
  }

  uint32_t fixed = vm->cur_cmd->param_count;
  if (vm->cur_cmd->sig)
  {
    fixed = (uint32_t)vm->cur_cmd->sig->param_count;
  }

  if ((uint32_t)i < fixed)
  {
    if (vm->cur_cmd->param_names && (uint32_t)i < vm->cur_cmd->param_count)
    {
      return mi_rt_make_string_slice(vm->cur_cmd->param_names[(uint32_t)i]);
    }
    return mi_rt_make_string_slice(x_slice_init("", 0));
  }

  /* Variadic tail has no real name; return '...' as a sentinel. */
  return mi_rt_make_string_slice(x_slice_init("...", 3));
}

static MiRtValue s_vm_cmd_set(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
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

static MiRtValue s_vm_cmd_call(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
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

  if (c->is_native)
  {
    /* Enforce declared signature when available.
       Native host commands are expected to provide one.
       Legacy builtins may still have sig == NULL. */
    if (c->sig)
    {
      if (!s_vm_check_sig(vm, c->sig, cmd_name, argc, argv))
      {
        return mi_rt_make_void();
      }
    }

    if (c->native_fn2)
    {
      return c->native_fn2(vm, c->native_user, argc, argv);
    }

    if (c->native_fn)
    {
      return c->native_fn(vm, cmd_name, argc, argv);
    }

    mi_error("mi_vm: native cmd missing function pointer\n");
    return mi_rt_make_void();
  }

  if (c->body.kind != MI_RT_VAL_BLOCK || !c->body.as.block || c->body.as.block->kind != MI_RT_BLOCK_VM_CHUNK)
  {
    mi_error("mi_vm: invalid cmd body\n");
    return mi_rt_make_void();
  }

  /* Enforce declared signature when available. */
  if (c->sig)
  {
    if (!s_vm_check_sig(vm, c->sig, cmd_name, argc, argv))
    {
      return mi_rt_make_void();
    }
  }
  else
  {
    if ((uint32_t)argc != c->param_count)
    {
      mi_error_fmt("%.*s: expected %u args, got %d\n", (int)cmd_name.length, cmd_name.ptr, (unsigned)c->param_count, argc);
      return mi_rt_make_void();
    }
  }

  MiRtBlock* b = c->body.as.block;
  MiVmChunk* sub = (MiVmChunk*)b->ptr;
  MiScopeFrame* caller = vm->rt->current;
  MiScopeFrame* parent = b->env ? b->env : caller;

/* Preserve current call argument context so argc()/arg() inside the user command
   refer to the arguments passed to this command call (not to nested builtins). */
int saved_cur_argc = vm->cur_argc;
const MiRtValue* saved_cur_argv = vm->cur_argv;
vm->cur_argc = argc;
vm->cur_argv = argv;

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

vm->cur_argc = saved_cur_argc;
vm->cur_argv = saved_cur_argv;

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

/* Blocks have their own argument context (empty). Preserve outer context so
   argc()/arg() inside the block do not accidentally observe caller args. */
int saved_cur_argc = vm->cur_argc;
const MiRtValue* saved_cur_argv = vm->cur_argv;
vm->cur_argc = 0;
vm->cur_argv = NULL;

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

  vm->cur_argc = saved_cur_argc;
  vm->cur_argv = saved_cur_argv;

  s_vm_call_stack_pop(vm);

  return ret;
}

/* cmd: <name> <param_name_0> <param_name_1> ... <param_name_n> <block>
   This is emitted by the compiler as a special form so it does not need to build
   a list/dict at runtime for parameters.
   */
static MiRtValue s_vm_cmd_cmd(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
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

  /* Optional typed signature (emitted by 'func' frontend): cmd(name, p1..pN, sig_list, block). */
  MiRtValue sig_arg = mi_rt_make_void();
  bool has_sig = false;
  if (argc >= 3 && argv[argc - 2].kind == MI_RT_VAL_LIST && argv[argc - 2].as.list)
  {
    sig_arg = argv[argc - 2];
    has_sig = true;
  }

  if (body.kind != MI_RT_VAL_BLOCK)
  {
    mi_error("cmd: last argument must be a block\n");
    return mi_rt_make_void();
  }

  uint32_t param_count = 0u;
  if (argc > 2)
  {
    param_count = has_sig ? (uint32_t)(argc - 3) : (uint32_t)(argc - 2);
  }
  XSlice* param_names = NULL;
  XSlice  param_names_local[MI_VM_ARG_STACK_COUNT];
  if (param_count > 0)
  {
    if (param_count > (uint32_t)(sizeof(param_names_local) / sizeof(param_names_local[0])))
    {
      mi_error("cmd: too many parameters\n");
      return mi_rt_make_void();
    }
    param_names = param_names_local;
    memset(param_names, 0, (size_t)param_count * sizeof(*param_names));
  }

  for (uint32_t i = 0; i < param_count; ++i)
  {
    MiRtValue pn = argv[1 + (int)i];
    if (pn.kind != MI_RT_VAL_STRING)
    {
      mi_error("cmd: parameter name must be string\n");
      return mi_rt_make_void();
    }

    param_names[i] = pn.as.s;
  }

  MiRtCmd* c = mi_rt_cmd_create(vm->rt, param_count, param_names, body);
  if (!c)
  {
    mi_error("cmd: out of memory\n");
    return mi_rt_make_void();
  }

  /* Attach signature metadata if provided. */
  if (has_sig)
  {
    MiRtList* lst = sig_arg.as.list;
    if (!lst || lst->count < 3u)
    {
      mi_error("cmd: invalid signature list\n");
      return mi_rt_make_void();
    }

    /* Format: [ret_type, fixed_count, t0..tN-1, variadic_type_or_-1] */
    MiRtValue v_ret = lst->items[0];
    MiRtValue v_fixed = lst->items[1];
    MiRtValue v_var = lst->items[lst->count - 1];
    if (v_ret.kind != MI_RT_VAL_INT || v_fixed.kind != MI_RT_VAL_INT || v_var.kind != MI_RT_VAL_INT)
    {
      mi_error("cmd: signature list must contain ints\n");
      return mi_rt_make_void();
    }

    int fixed_count = (int)v_fixed.as.i;
    if (fixed_count < 0)
    {
      mi_error("cmd: invalid fixed_count in signature\n");
      return mi_rt_make_void();
    }

    size_t expected_count = (size_t)(2 + fixed_count + 1);
    if (lst->count != expected_count)
    {
      mi_error_fmt("cmd: signature list wrong size (expected %u, got %u)\n", (unsigned)expected_count, (unsigned)lst->count);
      return mi_rt_make_void();
    }

    if ((uint32_t)fixed_count != param_count)
    {
      mi_error("cmd: signature fixed_count must match parameter name count\n");
      return mi_rt_make_void();
    }

    MiFuncTypeSig* sig = (MiFuncTypeSig*)x_arena_alloc_zero(vm->perm_arena, sizeof(MiFuncTypeSig));
    if (!sig)
    {
      mi_error("cmd: out of memory\n");
      return mi_rt_make_void();
    }

    sig->ret_type = (MiTypeKind)v_ret.as.i;
    sig->param_count = fixed_count;
    sig->param_types = NULL;
    sig->is_variadic = false;
    sig->variadic_type = MI_TYPE_ANY;

    if (fixed_count > 0)
    {
      sig->param_types = (MiTypeKind*)x_arena_alloc(vm->perm_arena, sizeof(MiTypeKind) * (size_t)fixed_count);
      if (!sig->param_types)
      {
        mi_error("cmd: out of memory\n");
        return mi_rt_make_void();
      }
      for (int i = 0; i < fixed_count; ++i)
      {
        MiRtValue vt = lst->items[2 + (size_t)i];
        if (vt.kind != MI_RT_VAL_INT)
        {
          mi_error("cmd: signature param types must be ints\n");
          return mi_rt_make_void();
        }
        sig->param_types[i] = (MiTypeKind)vt.as.i;
      }
    }

    int64_t var_kind = v_var.as.i;
    if (var_kind >= 0)
    {
      sig->is_variadic = true;
      sig->variadic_type = (MiTypeKind)var_kind;
    }

    c->sig = sig;
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

static bool s_slice_has_double_colon(XSlice s)
{
  return s_slice_find_double_colon(s, 0u, NULL);
}

static bool s_vm_resolve_qualified_cmd(MiVm* vm, XSlice full_name, MiRtCmd** out_cmd)
{
  if (out_cmd)
  {
    *out_cmd = NULL;
  }
  if (!vm || !vm->rt)
  {
    return false;
  }

  size_t i0 = 0u;
  size_t dc = 0u;
  if (!s_slice_find_double_colon(full_name, 0u, &dc))
  {
    return false;
  }

  /* Resolve first segment from current scope chain. */
  XSlice seg = { full_name.ptr, dc };
  MiRtValue cur = mi_rt_make_void();
  if (!mi_rt_var_get(vm->rt, seg, &cur))
  {
    return false;
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
      return false;
    }

    MiRtValue v = mi_rt_make_void();
    if (!mi_rt_var_get_from(cur.as.block->env, name, &v))
    {
      return false;
    }

    if (!has_next)
    {
      if (v.kind != MI_RT_VAL_CMD || !v.as.cmd)
      {
        return false;
      }
      if (out_cmd)
      {
        *out_cmd = v.as.cmd;
      }
      return true;
    }

    /* Continue chaining through nested chunks. */
    cur = v;
    seg = name;
    i0 = next_dc + 2u;
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

bool mi_vm_link_chunk_commands(MiVm* vm, MiVmChunk* chunk)
{
  if (!vm || !vm->rt || !chunk)
  {
    return false;
  }

  if (!chunk->cmd_names || !chunk->cmd_targets)
  {
    return true;
  }

  for (size_t i = 0; i < chunk->cmd_count; ++i)
  {
    if (chunk->cmd_targets[i])
    {
      continue;
    }

    XSlice name = chunk->cmd_names[i];

    /* Qualified names may not be available yet (e.g. util defined after include:).
       Leave them unresolved for lazy caching at first call. */
    if (s_slice_has_double_colon(name))
    {
      MiRtCmd* cmd = NULL;
      if (s_vm_resolve_qualified_cmd(vm, name, &cmd) && cmd)
      {
        chunk->cmd_targets[i] = cmd;
      }
      continue;
    }

    MiRtValue cmd_v = mi_rt_make_void();
    if (!mi_vm_find_command(vm, name, &cmd_v))
    {
      return false;
    }
    if (cmd_v.kind != MI_RT_VAL_CMD || !cmd_v.as.cmd)
    {
      mi_rt_value_release(vm->rt, cmd_v);
      return false;
    }
    chunk->cmd_targets[i] = cmd_v.as.cmd;
    mi_rt_value_release(vm->rt, cmd_v);
  }

  return true;
}

//----------------------------------------------------------
// include: cache helpers
//----------------------------------------------------------

static bool s_vm_get_cache_root(MiVm* vm, XFSPath* out_root);

static uint64_t s_hash_fnv1a64(const void* data, size_t len)
{
  const uint8_t* p = (const uint8_t*)data;
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < len; ++i)
  {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static void s_hex_u64(char out[17], uint64_t v)
{
  static const char* k_hex = "0123456789abcdef";
  for (int i = 15; i >= 0; --i)
  {
    out[i] = k_hex[(int)(v & 0xFULL)];
    v >>= 4ULL;
  }
  out[16] = 0;
}

static char* s_strdup_arena(XArena* arena, const char* s)
{
  if (!arena || !s)
  {
    return NULL;
  }
  size_t n = strlen(s);
  char* p = (char*)x_arena_alloc(arena, n + 1u);
  if (!p)
  {
    return NULL;
  }
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

static bool s_vm_cached_mx_for_mi(MiVm* vm, const char* src_mi, XFSPath* out_mx)
{
  if (!vm || !src_mi || !out_mx)
  {
    return false;
  }

  XFSPath cache_root;
  if (!s_vm_get_cache_root(vm, &cache_root))
  {
    return false;
  }
  (void)x_fs_directory_create_recursive(cache_root.buf);

  uint64_t h = s_hash_fnv1a64(src_mi, strlen(src_mi));
  char hex[17];
  s_hex_u64(hex, h);

  XFSPath cache_dir = cache_root;
  (void)x_fs_path_join(&cache_dir, hex);
  (void)x_fs_directory_create_recursive(cache_dir.buf);

  XFSPath mx_name;
  {
    XSlice base = x_fs_path_basename(src_mi);
    XSmallstr tmp;
    (void)x_smallstr_from_slice(base, &tmp);
    x_fs_path_set(&mx_name, x_smallstr_cstr(&tmp));
  }
  (void)x_fs_path_change_extension(&mx_name, ".mx");

  *out_mx = cache_dir;
  (void)x_fs_path_join(out_mx, mx_name.buf);
  return true;
}

static MiRtValue s_vm_module_cache_get(MiVm* vm, const char* key, bool* out_found)
{
  if (out_found)
  {
    *out_found = false;
  }

  if (!vm || !key)
  {
    return mi_rt_make_void();
  }

  for (size_t i = 0; i < vm->module_cache_count; ++i)
  {
    if (vm->module_cache[i].key && strcmp(vm->module_cache[i].key, key) == 0)
    {
      if (out_found)
      {
        *out_found = true;
      }
      /* include() convention: returned values are owned by the caller.
         When the cache hits, return an extra retained reference so callers
         can release safely without affecting the cache's own ownership. */
      if (vm->rt)
      {
        mi_rt_value_retain(vm->rt, vm->module_cache[i].value);
      }
      return vm->module_cache[i].value;
    }
  }

  return mi_rt_make_void();
}

static void s_vm_module_cache_set(MiVm* vm, const char* key, MiRtValue value)
{
  if (!vm || !key)
  {
    return;
  }

  for (size_t i = 0; i < vm->module_cache_count; ++i)
  {
    if (vm->module_cache[i].key && strcmp(vm->module_cache[i].key, key) == 0)
    {
      /* Replace existing cached value, updating ownership. */
      if (vm->rt)
      {
        mi_rt_value_release(vm->rt, vm->module_cache[i].value);
        mi_rt_value_retain(vm->rt, value);
      }
      vm->module_cache[i].value = value;
      return;
    }
  }

  if (vm->module_cache_count == vm->module_cache_capacity)
  {
    size_t new_cap = vm->module_cache_capacity ? vm->module_cache_capacity * 2u : 8u;
    vm->module_cache = (MiVmModuleCacheEntry*)s_realloc(vm->module_cache, new_cap * sizeof(*vm->module_cache));
    vm->module_cache_capacity = new_cap;
  }

  vm->module_cache[vm->module_cache_count].key = s_strdup_arena(vm->perm_arena, key);
  if (vm->rt)
  {
    mi_rt_value_retain(vm->rt, value);
  }
  vm->module_cache[vm->module_cache_count].value = value;
  vm->module_cache_count += 1u;
}


static MiVmNativeDll* s_vm_native_dll_find(MiVm* vm, const char* path)
{
  if (!vm || !path)
  {
    return NULL;
  }

  for (size_t i = 0; i < vm->native_dll_count; ++i)
  {
    if (vm->native_dlls[i].path && strcmp(vm->native_dlls[i].path, path) == 0)
    {
      return &vm->native_dlls[i];
    }
  }

  return NULL;
}

static MiVmNativeDll* s_vm_native_dll_get_or_load(MiVm* vm, const char* path)
{
  if (!vm || !path || !path[0])
  {
    return NULL;
  }

  MiVmNativeDll* hit = s_vm_native_dll_find(vm, path);
  if (hit)
  {
    return hit;
  }

  void* handle = NULL;
#ifdef _WIN32
  handle = (void*)LoadLibraryA(path);
#else
  handle = dlopen(path, RTLD_NOW);
#endif

  if (!handle)
  {
#ifdef _WIN32
    mi_error_fmt("include: failed to load dll: %s\n", path);
#else
    const char* e = dlerror();
    mi_error_fmt("include: failed to load so: %s (%s)\n", path, e ? e : "unknown");
#endif
    return NULL;
  }

  MiModuleCountFn count_fn = NULL;
  MiModuleNameFn name_fn = NULL;
  MiModuleRegisterFn reg_fn = NULL;

#ifdef _WIN32
  count_fn = (MiModuleCountFn)GetProcAddress((HMODULE)handle, "mi_module_count");
  name_fn = (MiModuleNameFn)GetProcAddress((HMODULE)handle, "mi_module_name");
  reg_fn = (MiModuleRegisterFn)GetProcAddress((HMODULE)handle, "mi_module_register");
#else
  count_fn = (MiModuleCountFn)dlsym(handle, "mi_module_count");
  name_fn = (MiModuleNameFn)dlsym(handle, "mi_module_name");
  reg_fn = (MiModuleRegisterFn)dlsym(handle, "mi_module_register");
#endif

  if (!count_fn || !name_fn || !reg_fn)
  {
    mi_error_fmt("include: native module missing required exports: %s\n", path);
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
    return NULL;
  }

  if (vm->native_dll_count == vm->native_dll_capacity)
  {
    size_t new_cap = vm->native_dll_capacity ? vm->native_dll_capacity * 2u : 8u;
    vm->native_dlls = (MiVmNativeDll*)s_realloc(vm->native_dlls, new_cap * sizeof(*vm->native_dlls));
    vm->native_dll_capacity = new_cap;
  }

  char* pcopy = NULL;
  {
    size_t n = strlen(path);
    pcopy = (char*)x_arena_alloc(vm->perm_arena, n + 1);
    if (!pcopy)
    {
#ifdef _WIN32
      FreeLibrary((HMODULE)handle);
#else
      dlclose(handle);
#endif
      return NULL;
    }
    memcpy(pcopy, path, n + 1);
  }

  MiVmNativeDll* out = &vm->native_dlls[vm->native_dll_count];
  memset(out, 0, sizeof(*out));
  out->path = pcopy;
  out->handle = handle;
  out->module_count = count_fn;
  out->module_name = name_fn;
  out->module_register = reg_fn;
  vm->native_dll_count += 1u;
  return out;
}

static char* s_read_text_file_arena(XArena* arena, const char* path, size_t* out_len)
{
  if (out_len)
  {
    *out_len = 0;
  }

  if (!arena)
  {
    return NULL;
  }

  FILE* f = fopen(path, "rb");
  if (!f)
  {
    return NULL;
  }

  if (fseek(f, 0, SEEK_END) != 0)
  {
    fclose(f);
    return NULL;
  }

  long sz = ftell(f);
  if (sz < 0)
  {
    fclose(f);
    return NULL;
  }

  if (fseek(f, 0, SEEK_SET) != 0)
  {
    fclose(f);
    return NULL;
  }

  char* buf = (char*)x_arena_alloc(arena, (size_t)sz + 1u);
  if (!buf)
  {
    fclose(f);
    return NULL;
  }

  size_t rd = fread(buf, 1u, (size_t)sz, f);
  fclose(f);
  if (rd != (size_t)sz)
  {
    return NULL;
  }
  buf[rd] = 0;

  if (out_len)
  {
    *out_len = rd;
  }
  return buf;
}

static bool s_compile_mi_to_mx(MiVm* vm, const char* mi_file, const char* mx_file)
{
  XArena* arena = x_arena_create(1024 * 64);
  if (!arena)
  {
    mi_error("include: failed to create parser arena\n");
    return false;
  }

  size_t src_len = 0;
  char* src = s_read_text_file_arena(arena, mi_file, &src_len);
  if (!src)
  {
    mi_error_fmt("include: failed to read: %s\n", mi_file);
    x_arena_destroy(arena);
    return false;
  }

  MiParseResult res = mi_parse_program_ex(src, src_len, arena, true);
  if (!res.ok || !res.script)
  {
    mi_parse_print_error(x_slice_init(src, src_len), &res);
    x_arena_destroy(arena);
    return false;
  }

  MiVmChunk* ch = mi_compile_vm_script_ex(vm, res.script, x_slice_from_cstr("<module>"), x_slice_from_cstr(mi_file));
  x_arena_destroy(arena);
  if (!ch)
  {
    mi_error_fmt("include: compilation failed: %s\n", mi_file);
    return false;
  }

  bool ok = mi_mx_save_file(ch, mx_file);
  mi_vm_chunk_destroy(ch);
  if (!ok)
  {
    mi_error_fmt("include: failed to write MIX file: %s\n", mx_file);
    return false;
  }

  return true;
}

static bool s_vm_get_cache_root(MiVm* vm, XFSPath* out_root)
{
  if (!out_root)
  {
    return false;
  }

  x_fs_path_set(out_root, "");

  if (vm && vm->cache_dir_set)
  {
    *out_root = vm->cache_dir;
    return true;
  }

#ifdef _WIN32
  const char* app = getenv("LOCALAPPDATA");
  if (app && app[0])
  {
    (void)x_fs_path_set(out_root, app);
    (void)x_fs_path_join(out_root, "minima");
    return true;
  }
#else
  const char* xdg = getenv("XDG_CACHE_HOME");
  if (xdg && xdg[0])
  {
    (void)x_fs_path_set(out_root, xdg);
    (void)x_fs_path_join(out_root, "minima");
    return true;
  }
  const char* home = getenv("HOME");
  if (home && home[0])
  {
    (void)x_fs_path_set(out_root, home);
    (void)x_fs_path_join(out_root, ".cache", "minima");
    return true;
  }
#endif

  if (!x_fs_get_temp_folder(out_root))
  {
    return false;
  }
  (void)x_fs_path_join(out_root, "minima");
  return true;
}

static bool s_vm_get_modules_dir(MiVm* vm, XFSPath* out_dir)
{
  if (!out_dir)
  {
    return false;
  }

  if (vm && vm->modules_dir_set)
  {
    *out_dir = vm->modules_dir;
    return true;
  }

  x_fs_cwd_get(out_dir);
  return true;
}


static void s_vm_track_detached_env(MiVm* vm, MiScopeFrame* env);
static MiRtValue s_vm_load_native_module(MiVm* vm, XSlice module_path)
{
  if (!vm || !vm->rt || !module_path.ptr || module_path.length == 0)
  {
    return mi_rt_make_void();
  }

  /* Parse: <dll>[/<module>] */
  size_t slash = (size_t)-1;
  for (size_t i = 0; i < module_path.length; ++i)
  {
    if (((const char*)module_path.ptr)[i] == '/')
    {
      slash = i;
      break;
    }
  }

  XSlice dll_name = module_path;
  XSlice mod_name = module_path;
  if (slash != (size_t)-1)
  {
    dll_name = x_slice_init(module_path.ptr, slash);
    mod_name = x_slice_init((const char*)module_path.ptr + (slash + 1), module_path.length - (slash + 1));
  }

  if (!dll_name.length || !mod_name.length)
  {
    return mi_rt_make_void();
  }

  XFSPath modules_dir;
  if (!s_vm_get_modules_dir(vm, &modules_dir))
  {
    return mi_rt_make_void();
  }

  const char* ext =
#ifdef _WIN32
    ".dll";
#elif defined(__APPLE__)
    ".dylib";
#else
    ".so";
#endif

  char dll_file[260];
  if (dll_name.length >= sizeof(dll_file))
  {
    return mi_rt_make_void();
  }
  memcpy(dll_file, dll_name.ptr, dll_name.length);
  dll_file[dll_name.length] = '\0';
  (void)strncat(dll_file, ext, sizeof(dll_file) - 1);

  XFSPath dll_path = modules_dir;
  (void)x_fs_path_join(&dll_path, dll_file);
  (void)x_fs_path_normalize(&dll_path);

  if (!x_fs_path_exists(&dll_path))
  {
    return mi_rt_make_void();
  }

  MiVmNativeDll* dll = s_vm_native_dll_get_or_load(vm, dll_path.buf);
  if (!dll)
  {
    return mi_rt_make_void();
  }

  char key[512];
  key[0] = '\0';
  (void)snprintf(key, sizeof(key), "native:%s::%.*s", dll_path.buf, (int)mod_name.length, (const char*)mod_name.ptr);

  bool found = false;
  MiRtValue cached = s_vm_module_cache_get(vm, key, &found);
  if (found)
  {
    return cached;
  }

  /* Create module env + handle block (namespace container). */
  MiScopeFrame* env = mi_rt_scope_create_detached(vm->rt, NULL);
  if (!env)
  {
    return mi_rt_make_void();
  }
  s_vm_track_detached_env(vm, env);

  MiRtBlock* block = mi_rt_block_create(vm->rt);
  if (!block)
  {
    return mi_rt_make_void();
  }
  block->env = env;

  MiRtValue block_v = mi_rt_make_block(block);

  /* Register module contents into this namespace block. */
  char mod_cstr[260];
  if (mod_name.length >= sizeof(mod_cstr))
  {
    return mi_rt_make_void();
  }
  memcpy(mod_cstr, mod_name.ptr, mod_name.length);
  mod_cstr[mod_name.length] = '\0';

  bool ok = dll->module_register(vm, mod_cstr, block_v);
  if (!ok)
  {
    mi_error_fmt("include: native module register failed: %.*s (%s)\n", (int)mod_name.length, (const char*)mod_name.ptr, dll_path.buf);
    return mi_rt_make_void();
  }

  s_vm_module_cache_set(vm, key, block_v);
  return block_v;
}

static MiRtValue s_vm_cmd_include(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
  if (!vm || !vm->rt)
  {
    return mi_rt_make_void();
  }

  if (argc != 1)
  {
    mi_error("include: expected: include: <module_path>\n");
    return mi_rt_make_void();
  }

  if (argv[0].kind != MI_RT_VAL_STRING)
  {
    mi_error("include: argument must be a string\n");
    return mi_rt_make_void();
  }

  XSlice module = argv[0].as.s;
  if (!module.length)
  {
    mi_error("include: empty path\n");
    return mi_rt_make_void();
  }

  XSlice base_file = s_vm_current_script_file(vm);
  XSlice base_dir_slice = base_file.length ? x_fs_path_dirname((const char*)base_file.ptr) : x_slice_empty();
  XFSPath base_dir = {0};
  //x_fs_path_set(&base_dir, base_dir_slice.length ? (const char*)base_dir_slice.ptr : "");
  x_fs_path_join_slice(&base_dir, &base_dir_slice);

  XFSPath req;
  x_fs_path_set(&req, "");
  {
    XFSPath mod;
    x_fs_path_set_slice(&mod, module);

    if (x_fs_path_is_absolute_cstr(mod.buf))
    {
      req = mod;
    }
    else
    {
      req = base_dir;
      (void)x_fs_path_join(&req, mod.buf);
    }
  }
  (void)x_fs_path_normalize(&req);

  /* Candidate source paths. */
  XFSPath src_mx = req;
  XFSPath src_mi = req;
  {
    XSlice ext = x_fs_path_extension(req.buf);
    if (ext.length)
    {
      if (x_slice_eq_cstr(ext, ".mx"))
      {
        src_mx = req;
        src_mi = req;
        (void)x_fs_path_change_extension(&src_mi, ".mi");
      }
      else if (x_slice_eq_cstr(ext, ".mi"))
      {
        src_mi = req;
        src_mx = req;
        (void)x_fs_path_change_extension(&src_mx, ".mx");
      }
      else
      {
        src_mi = req;
        src_mx = req;
        (void)x_fs_path_change_extension(&src_mi, ".mi");
        (void)x_fs_path_change_extension(&src_mx, ".mx");
      }
    }
    else
    {
      (void)x_fs_path_change_extension(&src_mi, ".mi");
      (void)x_fs_path_change_extension(&src_mx, ".mx");
    }
  }


  /* Fallback search: MI_ROOT/modules for .mi/.mx scripts. */
  if (!x_fs_path_exists(&src_mx) && !x_fs_path_exists(&src_mi))
  {
    XFSPath modules_dir;
    if (s_vm_get_modules_dir(vm, &modules_dir))
    {
      XFSPath mod_req = modules_dir;
      x_fs_path_join_slice(&mod_req, &module);
      (void)x_fs_path_normalize(&mod_req);

      XFSPath mod_mx = mod_req;
      XFSPath mod_mi = mod_req;
      XSlice ext = x_fs_path_extension(mod_req.buf);
      if (ext.length)
      {
        if (x_slice_eq_cstr(ext, ".mx"))
        {
          mod_mx = mod_req;
          mod_mi = mod_req;
          (void)x_fs_path_change_extension(&mod_mi, ".mi");
        }
        else if (x_slice_eq_cstr(ext, ".mi"))
        {
          mod_mi = mod_req;
          mod_mx = mod_req;
          (void)x_fs_path_change_extension(&mod_mx, ".mx");
        }
        else
        {
          mod_mi = mod_req;
          mod_mx = mod_req;
          (void)x_fs_path_change_extension(&mod_mi, ".mi");
          (void)x_fs_path_change_extension(&mod_mx, ".mx");
        }
      }
      else
      {
        (void)x_fs_path_change_extension(&mod_mi, ".mi");
        (void)x_fs_path_change_extension(&mod_mx, ".mx");
      }

      src_mx = mod_mx;
      src_mi = mod_mi;
      req = mod_req;
    }
  }

  XFSPath load_mx;
  bool need_compile = false;

  if (x_fs_path_exists(&src_mx))
  {
    load_mx = src_mx;
  }
  else
  {
    if (!x_fs_path_exists(&src_mi))
    {
      MiRtValue native_v = s_vm_load_native_module(vm, module);
      if (native_v.kind != MI_RT_VAL_VOID)
      {
        return native_v;
      }

      mi_error_fmt("include: module not found: %.*s\n", (int)module.length, (const char*)module.ptr);
      return mi_rt_make_void();
    }

    if (!s_vm_cached_mx_for_mi(vm, src_mi.buf, &load_mx))
    {
      mi_error("include: failed to resolve cache directory\n");
      return mi_rt_make_void();
    }

    time_t mi_time = 0;
    time_t mx_time = 0;
    bool mx_exists = x_fs_path_exists(&load_mx);
    bool have_mi = x_fs_file_modification_time(src_mi.buf, &mi_time);
    bool have_mx = mx_exists && x_fs_file_modification_time(load_mx.buf, &mx_time);

    need_compile = !mx_exists || !have_mi || !have_mx || (mi_time > mx_time);
    if (need_compile)
    {
      if (!s_compile_mi_to_mx(vm, src_mi.buf, load_mx.buf))
      {
        return mi_rt_make_void();
      }
    }
  }

  {
    uint32_t mx_version = 0;
    bool have_ver = mi_mx_peek_file_version(load_mx.buf, &mx_version);
    bool compatible = have_ver && (mx_version >= 1u) && (mx_version <= MI_MX_VERSION);

    if (!compatible && x_fs_path_exists(&src_mi))
    {
      XFSPath cached_mx;
      if (!s_vm_cached_mx_for_mi(vm, src_mi.buf, &cached_mx))
      {
        mi_error("include: failed to resolve cache directory\n");
        return mi_rt_make_void();
      }
      if (!s_compile_mi_to_mx(vm, src_mi.buf, cached_mx.buf))
      {
        return mi_rt_make_void();
      }
      load_mx = cached_mx;
    }
  }

  {
    bool hit = false;
    MiRtValue cached = s_vm_module_cache_get(vm, load_mx.buf, &hit);
    if (hit)
    {
      return cached;
    }
  }

  MiMixProgram prog;
  memset(&prog, 0, sizeof(prog));
  if (!mi_mx_load_file(vm, load_mx.buf, &prog))
  {
    mi_error_fmt("include: failed to load module: %s\n", load_mx.buf);
    return mi_rt_make_void();
  }

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

  s_vm_module_cache_set(vm, load_mx.buf, block_v);
  return block_v;
}


//----------------------------------------------------------
// VM init/shutdown
//----------------------------------------------------------

void mi_vm_set_cache_dir(MiVm* vm, const char* path)
{
  if (!vm)
  {
    return;
  }

  vm->cache_dir_set = false;
  (void)x_fs_path_set(&vm->cache_dir, "");

  if (path && path[0])
  {
    (void)x_fs_path_set(&vm->cache_dir, path);
    (void)x_fs_path_normalize(&vm->cache_dir);
    vm->cache_dir_set = true;
  }
}


void mi_vm_set_modules_dir(MiVm* vm, const char* path)
{
  if (!vm)
  {
    return;
  }

  vm->modules_dir_set = false;
  (void)x_fs_path_set(&vm->modules_dir, "");

  if (path && path[0])
  {
    (void)x_fs_path_set(&vm->modules_dir, path);
    (void)x_fs_path_normalize(&vm->modules_dir);
    vm->modules_dir_set = true;
  }
}

void mi_vm_init(MiVm* vm, MiRuntime* rt)
{
  memset(vm, 0, sizeof(*vm));
  vm->rt = rt;
  vm->api = &s_vm_api;

  vm->perm_arena = x_arena_create(1024 * 64);
  if (!vm->perm_arena)
  {
    mi_error("mi_vm: failed to create arena\n");
    exit(1);
  }
  vm->cache_dir_set = false;
  (void)x_fs_path_set(&vm->cache_dir, "");
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

  // Minimal builtins.
  // All native functions declare a type signature. The only intentional
  // outlier is print(), which is variadic.
  static MiFuncTypeSig s_sig_print = {0};
  s_sig_print.ret_type = MI_TYPE_VOID;
  s_sig_print.param_types = NULL;
  s_sig_print.param_count = 0;
  s_sig_print.is_variadic = true;
  s_sig_print.variadic_type = MI_TYPE_ANY;

  static MiFuncTypeSig s_sig_msg = {0};
  s_sig_msg.ret_type = MI_TYPE_VOID;
  s_sig_msg.param_types = NULL;
  s_sig_msg.param_count = 0;
  s_sig_msg.is_variadic = true;
  s_sig_msg.variadic_type = MI_TYPE_ANY;

  static MiTypeKind s_sig_set_params[] = { MI_TYPE_STRING, MI_TYPE_ANY };
  static MiFuncTypeSig s_sig_set = {0};
  s_sig_set.ret_type = MI_TYPE_ANY;
  s_sig_set.param_types = s_sig_set_params;
  s_sig_set.param_count = 2;

  static MiTypeKind s_sig_call_params[] = { MI_TYPE_BLOCK };
  static MiFuncTypeSig s_sig_call = {0};
  s_sig_call.ret_type = MI_TYPE_ANY;
  s_sig_call.param_types = s_sig_call_params;
  s_sig_call.param_count = 1;

  static MiTypeKind s_sig_list_params[] = { MI_TYPE_LIST };
  static MiFuncTypeSig s_sig_list = {0};
  s_sig_list.ret_type = MI_TYPE_LIST;
  s_sig_list.param_types = s_sig_list_params;
  s_sig_list.param_count = 1;

  static MiTypeKind s_sig_dict_params[] = { MI_TYPE_ANY };
  static MiFuncTypeSig s_sig_dict = {0};
  s_sig_dict.ret_type = MI_TYPE_DICT;
  s_sig_dict.param_types = s_sig_dict_params;
  s_sig_dict.param_count = 1;

  static MiTypeKind s_sig_len_params[] = { MI_TYPE_ANY };
  static MiFuncTypeSig s_sig_len = {0};
  s_sig_len.ret_type = MI_TYPE_INT;
  s_sig_len.param_types = s_sig_len_params;
  s_sig_len.param_count = 1;

  static MiTypeKind s_sig_type_params[] = { MI_TYPE_STRING };
  static MiFuncTypeSig s_sig_type = {0};
  s_sig_type.ret_type = MI_TYPE_ANY;
  s_sig_type.param_types = s_sig_type_params;
  s_sig_type.param_count = 1;

  static MiTypeKind s_sig_typeof_params[] = { MI_TYPE_ANY };
  static MiFuncTypeSig s_sig_typeof = {0};
  s_sig_typeof.ret_type = MI_TYPE_ANY;
  s_sig_typeof.param_types = s_sig_typeof_params;
  s_sig_typeof.param_count = 1;

  static MiTypeKind s_sig_include_params[] = { MI_TYPE_STRING };
  static MiFuncTypeSig s_sig_include = {0};
  s_sig_include.ret_type = MI_TYPE_BLOCK;
  s_sig_include.param_types = s_sig_include_params;
  s_sig_include.param_count = 1;

  static MiFuncTypeSig s_sig_trace = {0};
  s_sig_trace.ret_type = MI_TYPE_VOID;
  s_sig_trace.param_types = NULL;
  s_sig_trace.param_count = 0;

  static MiFuncTypeSig s_sig_argc = {0};
  s_sig_argc.ret_type = MI_TYPE_INT;
  s_sig_argc.param_types = NULL;
  s_sig_argc.param_count = 0;

  static MiTypeKind s_sig_arg_params[] = { MI_TYPE_INT };
  static MiFuncTypeSig s_sig_arg = {0};
  s_sig_arg.ret_type = MI_TYPE_ANY;
  s_sig_arg.param_types = s_sig_arg_params;
  s_sig_arg.param_count = 1;

  static MiTypeKind s_sig_arg_type_params[] = { MI_TYPE_INT };
  static MiFuncTypeSig s_sig_arg_type = {0};
  s_sig_arg_type.ret_type = MI_TYPE_ANY;
  s_sig_arg_type.param_types = s_sig_arg_type_params;
  s_sig_arg_type.param_count = 1;

  static MiTypeKind s_sig_arg_name_params[] = { MI_TYPE_INT };
  static MiFuncTypeSig s_sig_arg_name = {0};
  s_sig_arg_name.ret_type = MI_TYPE_STRING;
  s_sig_arg_name.param_types = s_sig_arg_name_params;
  s_sig_arg_name.param_count = 1;

  static MiTypeKind s_sig_assert_params[] = { MI_TYPE_ANY };
  static MiFuncTypeSig s_sig_assert = {0};
  s_sig_assert.ret_type = MI_TYPE_VOID;
  s_sig_assert.param_types = s_sig_assert_params;
  s_sig_assert.param_count = 1;
  s_sig_assert.is_variadic = true;
  s_sig_assert.variadic_type = MI_TYPE_ANY;

  static MiTypeKind s_sig_cmd_params[] = { MI_TYPE_STRING };
  static MiFuncTypeSig s_sig_cmd = {0};
  s_sig_cmd.ret_type = MI_TYPE_FUNC;
  s_sig_cmd.param_types = s_sig_cmd_params;
  s_sig_cmd.param_count = 1;
  s_sig_cmd.is_variadic = true;
  s_sig_cmd.variadic_type = MI_TYPE_ANY;


  (void)mi_vm_register_native(vm, x_slice_from_cstr("arg"),       &s_sig_arg,       s_vm_cmd_arg,       NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("arg_name"),  &s_sig_arg_name,  s_vm_cmd_arg_name,  NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("arg_type"),  &s_sig_arg_type,  s_vm_cmd_arg_type,  NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("argc"),      &s_sig_argc,      s_vm_cmd_argc,      NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("assert"),    &s_sig_assert,    s_vm_cmd_assert,    NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("call"),      &s_sig_call,      s_vm_cmd_call,      NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("cmd"),       &s_sig_cmd,       s_vm_cmd_cmd,       NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("dict"),      &s_sig_dict,      s_vm_cmd_dict,      NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("error"),     &s_sig_msg,       s_vm_cmd_error,     NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("fatal"),     &s_sig_msg,       s_vm_cmd_fatal,     NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("import"),    &s_sig_include,   s_vm_cmd_include,   NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("include"),   &s_sig_include,   s_vm_cmd_include,   NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("len"),       &s_sig_len,       s_vm_cmd_len,       NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("list"),      &s_sig_list,      s_vm_cmd_list,      NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("print"),     &s_sig_print,     s_vm_cmd_print,     NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("set"),       &s_sig_set,       s_vm_cmd_set,       NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("t"),         &s_sig_type,      s_vm_cmd_type,      NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("trace"),     &s_sig_trace,     s_vm_cmd_trace,     NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("type"),      &s_sig_type,      s_vm_cmd_type,      NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("typeof"),    &s_sig_typeof,    s_vm_cmd_typeof,    NULL, x_slice_init(NULL, 0));
  (void)mi_vm_register_native(vm, x_slice_from_cstr("warning"),   &s_sig_msg,       s_vm_cmd_warning,   NULL, x_slice_init(NULL, 0));

  /* Standard library */
  //mi_lib_int_register(vm);
  //mi_lib_float_register(vm);
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

  if (vm->commands)
  {
    for (size_t i = 0; i < vm->command_count; ++i)
    {
      mi_rt_value_release(vm->rt, vm->commands[i].value);
      vm->commands[i].value = mi_rt_make_void();
    }
    free(vm->commands);
  }
  vm->commands = NULL;
  vm->command_count = 0;
  vm->command_capacity = 0;

  if (vm->modules)
  {
    for (size_t i = 0; i < vm->module_count; ++i)
    {
      mi_mx_program_destroy(&vm->modules[i]);
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

  if (vm->module_cache)
  {
    for (size_t i = 0; i < vm->module_cache_count; ++i)
    {
      if (vm->rt)
      {
        mi_rt_value_release(vm->rt, vm->module_cache[i].value);
      }
      vm->module_cache[i].key = NULL;
      vm->module_cache[i].value = mi_rt_make_void();
    }
    free(vm->module_cache);
  }
  vm->module_cache = NULL;
  vm->module_cache_count = 0u;

  if (vm->native_dlls)
  {
    for (size_t i = 0; i < vm->native_dll_count; ++i)
    {
      if (vm->native_dlls[i].handle)
      {
#ifdef _WIN32
        FreeLibrary((HMODULE)vm->native_dlls[i].handle);
#else
        dlclose(vm->native_dlls[i].handle);
#endif
      }
      vm->native_dlls[i].path = NULL;
      vm->native_dlls[i].handle = NULL;
      vm->native_dlls[i].module_count = NULL;
      vm->native_dlls[i].module_name = NULL;
      vm->native_dlls[i].module_register = NULL;
    }
    free(vm->native_dlls);
  }
  vm->native_dlls = NULL;
  vm->native_dll_count = 0u;
  vm->native_dll_capacity = 0u;

  vm->module_cache_capacity = 0u;

  if (vm->perm_arena)
  {
    x_arena_destroy(vm->perm_arena);
    vm->perm_arena = NULL;
  }
}

static XSlice s_vm_perm_copy_slice(MiVm* vm, XSlice s)
{
  if (!vm || !vm->perm_arena || s.length == 0 || !s.ptr)
  {
    return x_slice_init(NULL, 0);
  }

  char* dst = (char*)x_arena_alloc(vm->perm_arena, s.length + 1);
  if (!dst)
  {
    return x_slice_init(NULL, 0);
  }

  memcpy(dst, s.ptr, s.length);
  dst[s.length] = '\0';
  return x_slice_init(dst, s.length);
}

static const MiFuncTypeSig* s_vm_perm_copy_sig(MiVm* vm, const MiFuncTypeSig* sig)
{
  if (!vm || !vm->perm_arena || !sig)
  {
    return NULL;
  }

  MiFuncTypeSig* out = (MiFuncTypeSig*)x_arena_alloc_zero(vm->perm_arena, sizeof(MiFuncTypeSig));
  if (!out)
  {
    return NULL;
  }

  *out = *sig;
  if (sig->param_count > 0 && sig->param_types)
  {
    MiTypeKind* pts = (MiTypeKind*)x_arena_alloc(vm->perm_arena, (size_t)sig->param_count * sizeof(MiTypeKind));
    if (!pts)
    {
      return NULL;
    }
    memcpy(pts, sig->param_types, (size_t)sig->param_count * sizeof(MiTypeKind));
    out->param_types = pts;
  }
  else
  {
    out->param_types = NULL;
  }

  /* Tokens are front-end only; keep them zeroed in native declarations. */
  out->func_tok = (MiToken){0};
  out->lparen_tok = (MiToken){0};
  out->rparen_tok = (MiToken){0};
  out->ret_tok = (MiToken){0};

  return out;
}

bool mi_vm_register_native(MiVm* vm, XSlice name, const MiFuncTypeSig* sig, MiVmNativeFn fn, void* user, XSlice doc)
{
  if (!vm || !vm->rt || !fn || !sig)
  {
    return false;
  }

  const MiFuncTypeSig* sig_copy = s_vm_perm_copy_sig(vm, sig);
  if (!sig_copy)
  {
    return false;
  }

  XSlice doc_copy = s_vm_perm_copy_slice(vm, doc);

  MiRtCmd* c = mi_rt_cmd_create_native2(vm->rt, (MiRtNativeFn2)fn, user, sig_copy, doc_copy);
  if (!c)
  {
    return false;
  }
  MiRtValue v = mi_rt_make_cmd(c);

  for (size_t i = 0; i < vm->command_count; ++i)
  {
    if (s_slice_eq(vm->commands[i].name, name))
    {
      mi_rt_value_release(vm->rt, vm->commands[i].value);
      vm->commands[i].value = v;
      mi_rt_value_retain(vm->rt, v);
      (void)mi_rt_var_set(vm->rt, name, v);
      return true;
    }
  }

  if (vm->command_count == vm->command_capacity)
  {
    size_t new_cap = vm->command_capacity ? vm->command_capacity * 2u : 32u;
    vm->commands = (MiVmCommandEntry*)s_realloc(vm->commands, new_cap * sizeof(*vm->commands));
    vm->command_capacity = new_cap;
  }

  vm->commands[vm->command_count].name = name;
  vm->commands[vm->command_count].value = v;
  mi_rt_value_retain(vm->rt, v);
  vm->command_count++;

  (void)mi_rt_var_define(vm->rt, name, v);
  return true;
}

static void s_vm_track_detached_env(MiVm* vm, MiScopeFrame* env)
{
  if (!vm || !env)
  {
    return;
  }

  if (vm->module_env_count == vm->module_env_capacity)
  {
    size_t new_cap = vm->module_env_capacity ? vm->module_env_capacity * 2u : 8u;
    vm->module_envs = (MiScopeFrame**)s_realloc(vm->module_envs, new_cap * sizeof(*vm->module_envs));
    vm->module_env_capacity = new_cap;
  }
  vm->module_envs[vm->module_env_count++] = env;
}

MiRtValue mi_vm_namespace_get_or_create(MiVm* vm, XSlice name)
{
  if (!vm || !vm->rt || !name.ptr || name.length == 0)
  {
    return mi_rt_make_void();
  }

  MiRtValue existing = mi_rt_make_void();
  if (mi_rt_var_get(vm->rt, name, &existing))
  {
    if (existing.kind == MI_RT_VAL_BLOCK && existing.as.block && existing.as.block->env)
    {
      return existing;
    }
  }

  MiScopeFrame* env = mi_rt_scope_create_detached(vm->rt, NULL);
  if (!env)
  {
    return mi_rt_make_void();
  }
  s_vm_track_detached_env(vm, env);

  MiRtBlock* b = mi_rt_block_create(vm->rt);
  b->kind = MI_RT_BLOCK_VM_CHUNK;
  b->ptr = NULL;
  b->env = env;

  MiRtValue block_v = mi_rt_make_block(b);
  (void)mi_rt_var_define(vm->rt, name, block_v);
  return block_v;
}

bool mi_vm_namespace_add_native(MiVm* vm, MiRtValue ns_block, XSlice member_name, const MiFuncTypeSig* sig, MiVmNativeFn fn, void* user, XSlice doc)
{
  if (!vm || !vm->rt || !sig || !fn)
  {
    return false;
  }
  if (ns_block.kind != MI_RT_VAL_BLOCK || !ns_block.as.block || !ns_block.as.block->env)
  {
    return false;
  }
  if (!member_name.ptr || member_name.length == 0)
  {
    return false;
  }

  const MiFuncTypeSig* sig_copy = s_vm_perm_copy_sig(vm, sig);
  if (!sig_copy)
  {
    return false;
  }

  XSlice doc_copy = s_vm_perm_copy_slice(vm, doc);

  MiRtCmd* c = mi_rt_cmd_create_native2(vm->rt, (MiRtNativeFn2)fn, user, sig_copy, doc_copy);
  if (!c)
  {
    return false;
  }

  MiRtValue v = mi_rt_make_cmd(c);
  uint32_t sym_id = mi_rt_sym_intern(vm->rt, member_name);
  mi_rt_var_set_from_id(ns_block.as.block->env, sym_id, v);
  return true;
}

static bool s_vm_build_sig_from_varargs(MiFuncTypeSig* out_sig, MiTypeKind ret_type, int param_count, bool is_variadic, MiTypeKind variadic_type, va_list args)
{
  if (!out_sig)
  {
    return false;
  }

  if (param_count < 0)
  {
    return false;
  }

  MiTypeKind* params = NULL;
  if (param_count > 0)
  {
    params = (MiTypeKind*)malloc(sizeof(MiTypeKind) * (size_t)param_count);
    if (!params)
    {
      return false;
    }

    for (int i = 0; i < param_count; ++i)
    {
      /* MiTypeKind is an enum; promoted to int in varargs. */
      params[i] = (MiTypeKind)va_arg(args, int);
    }
  }

  *out_sig = (MiFuncTypeSig){0};
  out_sig->ret_type = ret_type;
  out_sig->param_types = params;
  out_sig->param_count = (uint16_t)param_count;
  out_sig->is_variadic = is_variadic;
  out_sig->variadic_type = variadic_type;

  return true;
}

bool mi_vm_register_native_sigv(MiVm* vm, const char* name_cstr, MiVmNativeFn fn, void* user, XSlice doc, MiTypeKind ret_type, int param_count, ...)
{
  va_list args;
  va_start(args, param_count);

  MiFuncTypeSig sig = {0};
  bool ok = s_vm_build_sig_from_varargs(&sig, ret_type, param_count, false, (MiTypeKind)0, args);

  va_end(args);

  if (!ok)
  {
    return false;
  }

  bool r = mi_vm_register_native(vm, x_slice_from_cstr(name_cstr), &sig, fn, user, doc);

  if (sig.param_types)
  {
    free((void*)sig.param_types);
  }

  return r;
}

bool mi_vm_register_native_sigv_var(MiVm* vm, const char* name_cstr, MiVmNativeFn fn, void* user, XSlice doc, MiTypeKind ret_type, int fixed_param_count, MiTypeKind variadic_type, ...)
{
  va_list args;
  va_start(args, variadic_type);

  MiFuncTypeSig sig = {0};
  bool ok = s_vm_build_sig_from_varargs(&sig, ret_type, fixed_param_count, true, variadic_type, args);

  va_end(args);

  if (!ok)
  {
    return false;
  }

  bool r = mi_vm_register_native(vm, x_slice_from_cstr(name_cstr), &sig, fn, user, doc);

  if (sig.param_types)
  {
    free((void*)sig.param_types);
  }

  return r;
}

bool mi_vm_namespace_add_native_sigv(MiVm* vm, MiRtValue ns_block, const char* member_name_cstr, MiVmNativeFn fn, void* user, XSlice doc, MiTypeKind ret_type, int param_count, ...)
{
  va_list args;
  va_start(args, param_count);

  MiFuncTypeSig sig = {0};
  bool ok = s_vm_build_sig_from_varargs(&sig, ret_type, param_count, false, (MiTypeKind)0, args);

  va_end(args);

  if (!ok)
  {
    return false;
  }

  bool r = mi_vm_namespace_add_native(vm, ns_block, x_slice_from_cstr(member_name_cstr), &sig, fn, user, doc);

  if (sig.param_types)
  {
    free((void*)sig.param_types);
  }

  return r;
}

bool mi_vm_namespace_add_native_sigv_var(MiVm* vm, MiRtValue ns_block, const char* member_name_cstr, MiVmNativeFn fn, void* user, XSlice doc, MiTypeKind ret_type, int fixed_param_count, MiTypeKind variadic_type, ...)
{
  va_list args;
  va_start(args, variadic_type);

  MiFuncTypeSig sig = {0};
  bool ok = s_vm_build_sig_from_varargs(&sig, ret_type, fixed_param_count, true, variadic_type, args);

  va_end(args);

  if (!ok)
  {
    return false;
  }

  bool r = mi_vm_namespace_add_native(vm, ns_block, x_slice_from_cstr(member_name_cstr), &sig, fn, user, doc);

  if (sig.param_types)
  {
    free((void*)sig.param_types);
  }

  return r;
}

bool mi_vm_namespace_add_value(MiVm* vm, MiRtValue ns_block, const char* member_name_cstr, MiRtValue value)
{
  if (!vm || !vm->rt)
  {
    return false;
  }

  if (ns_block.kind != MI_RT_VAL_BLOCK || !ns_block.as.block || !ns_block.as.block->env)
  {
    return false;
  }

  XSlice member_name = x_slice_from_cstr(member_name_cstr);
  if (!member_name.ptr || member_name.length == 0)
  {
    return false;
  }

  uint32_t sym_id = mi_rt_sym_intern(vm->rt, member_name);
  mi_rt_var_set_from_id(ns_block.as.block->env, sym_id, value);
  return true;
}

bool mi_vm_find_command(MiVm* vm, XSlice name, MiRtValue* out_cmd)
{
  if (!vm)
  {
    return false;
  }

  for (size_t i = 0; i < vm->command_count; ++i)
  {
    if (s_slice_eq(vm->commands[i].name, name))
    {
      if (out_cmd)
      {
        *out_cmd = vm->commands[i].value;
        mi_rt_value_retain(vm->rt, *out_cmd);
      }
      return true;
    }
  }

  return false;
}

MiVmCommandFn mi_vm_find_command_fn(MiVm* vm, XSlice name)
{
  MiRtValue v = mi_rt_make_void();
  if (!mi_vm_find_command(vm, name, &v))
  {
    return NULL;
  }

  MiVmCommandFn out = NULL;
  if (v.kind == MI_RT_VAL_CMD && v.as.cmd && v.as.cmd->is_native && v.as.cmd->native_fn)
  {
    out = (MiVmCommandFn)v.as.cmd->native_fn;
  }

  mi_rt_value_release(vm->rt, v);
  return out;
}

bool mi_vm_find_sig(MiVm* vm, XSlice qualified_name, const MiFuncTypeSig** out_sig)
{
  if (!vm || !vm->rt || !qualified_name.ptr || qualified_name.length == 0)
  {
    return false;
  }

  if (out_sig)
  {
    *out_sig = NULL;
  }

  /* Fast path: global command registry. */
  {
    MiRtValue v = mi_rt_make_void();
    if (mi_vm_find_command(vm, qualified_name, &v))
    {
      /* Treat result as borrowed: retain before releasing. */
      mi_rt_value_retain(vm->rt, v);

      bool ok = (v.kind == MI_RT_VAL_CMD && v.as.cmd && v.as.cmd->sig);
      if (ok && out_sig)
      {
        *out_sig = v.as.cmd->sig;
      }

      mi_rt_value_release(vm->rt, v);
      return ok;
    }
  }

  /* If the name is not qualified, try global variables (namespaces may live here). */
  {
    const uint8_t* p = (const uint8_t*)qualified_name.ptr;
    const uint8_t* end = p + qualified_name.length;
    bool has_qual = false;
    for (const uint8_t* it = p; it + 1 < end; ++it)
    {
      if (it[0] == ':' && it[1] == ':')
      {
        has_qual = true;
        break;
      }
    }

    if (!has_qual)
    {
      MiRtValue v = mi_rt_make_void();
      if (mi_rt_var_get(vm->rt, qualified_name, &v))
      {
        /* Borrowed -> retain before release. */
        mi_rt_value_retain(vm->rt, v);

        bool ok = (v.kind == MI_RT_VAL_CMD && v.as.cmd && v.as.cmd->sig);
        if (ok && out_sig)
        {
          *out_sig = v.as.cmd->sig;
        }

        mi_rt_value_release(vm->rt, v);
        return ok;
      }
      return false;
    }
  }

  /* Qualified lookup: a::b::c walks envs and ends in a cmd with a signature. */
  {
    const char* p = (const char*)qualified_name.ptr;
    const char* end = p + qualified_name.length;

    const char* seg = p;
    const char* q = p;
    while (q + 1 < end)
    {
      if (q[0] == ':' && q[1] == ':')
      {
        break;
      }
      q++;
    }

    if (q + 1 >= end)
    {
      return false;
    }

    XSlice first = x_slice_init(seg, (int)(q - seg));

    MiRtValue cur = mi_rt_make_void();
    if (!mi_rt_var_get(vm->rt, first, &cur))
    {
      return false;
    }
    /* Borrowed -> retain so we can safely release as we walk. */
    mi_rt_value_retain(vm->rt, cur);

    q += 2;
    while (q <= end)
    {
      const char* next = q;
      while (next + 1 < end)
      {
        if (next[0] == ':' && next[1] == ':')
        {
          break;
        }
        next++;
      }

      bool is_last = (next + 1 >= end);
      XSlice member = x_slice_init(q, (int)(is_last ? (end - q) : (next - q)));

      if (cur.kind != MI_RT_VAL_BLOCK || !cur.as.block || !cur.as.block->env)
      {
        mi_rt_value_release(vm->rt, cur);
        return false;
      }

      MiRtValue child = mi_rt_make_void();
      if (!mi_rt_var_get_from(cur.as.block->env, member, &child))
      {
        mi_rt_value_release(vm->rt, cur);
        return false;
      }
      // Borrowed -> retain before we drop 'cur'. 
      mi_rt_value_retain(vm->rt, child);

      mi_rt_value_release(vm->rt, cur);
      cur = child;

      if (is_last)
      {
        bool ok = (cur.kind == MI_RT_VAL_CMD && cur.as.cmd && cur.as.cmd->sig);
        if (ok && out_sig)
        {
          *out_sig = cur.as.cmd->sig;
        }

        mi_rt_value_release(vm->rt, cur);
        return ok;
      }

      q = next + 2;
    }

    mi_rt_value_release(vm->rt, cur);
  }

  return false;
}


MiRtValue mi_vm_call_command(MiVm* vm, XSlice cmd_name, int argc, const MiRtValue* argv)
{
  if (!vm || !vm->rt || !cmd_name.ptr || cmd_name.length == 0)
  {
    return mi_rt_make_void();
  }

  MiRtValue cmd_value = mi_rt_make_void();
  if (!mi_vm_find_command(vm, cmd_name, &cmd_value))
  {
    return mi_rt_make_void();
  }

  MiRtValue ret = s_vm_exec_cmd_value(vm, cmd_name, cmd_value, argc, argv);
  mi_rt_value_release(vm->rt, cmd_value);
  return ret;
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
      // Cycle detected; avoid infinite recursion. 
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

  if (chunk->consts)
  {
    for (size_t i = 0; i < chunk->const_count; ++i)
    {
      if (chunk->consts[i].kind == MI_RT_VAL_STRING)
      {
        free((void*)chunk->consts[i].as.s.ptr);
      }
    }
    free(chunk->consts);
  }

  if (chunk->symbols)
  {
    for (size_t i = 0; i < chunk->symbol_count; ++i)
    {
      free((void*)chunk->symbols[i].ptr);
    }
    free(chunk->symbols);
  }

  if (chunk->symbol_ids)
  {
    free(chunk->symbol_ids);
  }

  free(chunk->cmd_targets);

  if (chunk->cmd_names)
  {
    for (size_t i = 0; i < chunk->cmd_count; ++i)
    {
      free((void*)chunk->cmd_names[i].ptr);
    }
    free(chunk->cmd_names);
  }

  if (chunk->dbg_lines)
  {
    free(chunk->dbg_lines);
  }
  if (chunk->dbg_cols)
  {
    free(chunk->dbg_cols);
  }
  if (chunk->dbg_name.ptr)
  {
    free((void*)chunk->dbg_name.ptr);
  }
  if (chunk->dbg_file.ptr)
  {
    free((void*)chunk->dbg_file.ptr);
  }

  free(chunk);
}

void mi_vm_chunk_destroy(MiVmChunk* chunk)
{
  s_vm_chunk_destroy_ex(chunk, NULL, 0);
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

  if (a->kind == MI_RT_VAL_VOID || b->kind == MI_RT_VAL_VOID)
  {
    switch (op)
    {
      case MI_VM_OP_EQ:   return mi_rt_make_bool(a->kind == b->kind);
      case MI_VM_OP_NEQ:  return mi_rt_make_bool(a->kind != b->kind);
      default:            return mi_rt_make_void();
    }
  }


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

    // Debug: remember current instruction location for trace:. 
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
          MI_ASSERT(ins.a < MI_VM_REG_COUNT);
          MI_ASSERT(ins.imm < (i32) chunk->const_count);

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
          MI_ASSERT(ins.a < MI_VM_REG_COUNT);
          MI_ASSERT(ins.b < MI_VM_REG_COUNT);

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
          MI_ASSERT(ins.a < MI_VM_REG_COUNT);
          MI_ASSERT(ins.b < MI_VM_REG_COUNT);

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
          MI_ASSERT(ins.a < MI_VM_REG_COUNT);
          MI_ASSERT(ins.b < MI_VM_REG_COUNT);
          MI_ASSERT(ins.c < MI_VM_REG_COUNT);

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
          MI_ASSERT(ins.a < MI_VM_REG_COUNT);
          MI_ASSERT(ins.b < MI_VM_REG_COUNT);
          MI_ASSERT(ins.c < MI_VM_REG_COUNT);

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
          MI_ASSERT(ins.a < MI_VM_REG_COUNT);
          MI_ASSERT(ins.b < MI_VM_REG_COUNT);
          MI_ASSERT(ins.c < MI_VM_REG_COUNT);

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
          uint32_t sym_id = s_vm_chunk_sym_id(vm, (MiVmChunk*)chunk, ins.imm);
          MiRtValue v;
          if (!mi_rt_var_get_id(vm->rt, sym_id, &v))
          {
            XSlice name = chunk->symbols[ins.imm];
            mi_error_fmt("undefined variable: %.*s", (int)name.length, name.ptr);
            v = mi_rt_make_void();
          }
          s_vm_reg_set(vm, ins.a, v);
        } break;

      case MI_VM_OP_LOAD_MEMBER:
        {
          MiRtValue base = vm->regs[ins.b];
          if (base.kind != MI_RT_VAL_BLOCK || !base.as.block || !base.as.block->env)
          {
            mi_error("member access: base is not a chunk/module\n");
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            break;
          }

          uint32_t sym_id = s_vm_chunk_sym_id(vm, (MiVmChunk*)chunk, ins.imm);
          MiRtValue v;
          if (!mi_rt_var_get_from_id(base.as.block->env, sym_id, &v))
          {
            XSlice mem_name = chunk->symbols[ins.imm];
            mi_error_fmt("unknown member: %.*s\n", (int)mem_name.length, (const char*)mem_name.ptr);
            v = mi_rt_make_void();
          }
          s_vm_reg_set(vm, ins.a, v);
        } break;

      case MI_VM_OP_STORE_MEMBER:
        {
          MiRtValue base = vm->regs[ins.b];
          if (base.kind != MI_RT_VAL_BLOCK || !base.as.block || !base.as.block->env)
          {
            mi_error("member store: base is not a chunk/module\n");
            break;
          }

          uint32_t sym_id = s_vm_chunk_sym_id(vm, (MiVmChunk*)chunk, ins.imm);
          mi_rt_var_set_from_id(base.as.block->env, sym_id, vm->regs[ins.a]);
        } break;

      case MI_VM_OP_STORE_VAR:
        {
          uint32_t sym_id = s_vm_chunk_sym_id(vm, (MiVmChunk*)chunk, ins.imm);
          mi_rt_var_set_id(vm->rt, sym_id, vm->regs[ins.a]);
        } break;

      case MI_VM_OP_DEFINE_VAR:
        {
          uint32_t sym_id = s_vm_chunk_sym_id(vm, (MiVmChunk*)chunk, ins.imm);
          mi_rt_var_define_id(vm->rt, sym_id, vm->regs[ins.a]);
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
          MI_ASSERT(ins.a < MI_VM_REG_COUNT);

          if (vm->arg_top >= MI_VM_ARG_STACK_COUNT)
          {
            s_vm_report_error(vm, "arg stack overflow");
            MI_ASSERT(vm->arg_top < MI_VM_ARG_STACK_COUNT);
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

          uint32_t sym_id = s_vm_chunk_sym_id(vm, (MiVmChunk*)chunk, ins.imm);
          MiRtValue v;
          if (!mi_rt_var_get_id(vm->rt, sym_id, &v))
          {
            XSlice name = chunk->symbols[ins.imm];
            mi_error_fmt("undefined variable: %.*s\n", (int)name.length, name.ptr);
            v = mi_rt_make_void();
          }
          mi_rt_value_assign(vm->rt, &vm->arg_stack[vm->arg_top], v);
          vm->arg_top += 1;
        } break;

      case MI_VM_OP_ARG_PUSH_SYM:
        {
          if (vm->arg_top >= MI_VM_ARG_STACK_COUNT)
          {
            mi_error("mi_vm: arg stack overflow\n");
            break;
          }
          if (ins.imm < 0 || (size_t)ins.imm >= chunk->symbol_count)
          {
            mi_error("mi_vm: ARG_PUSH_SYM invalid symbol index\n");
            mi_rt_value_assign(vm->rt, &vm->arg_stack[vm->arg_top], mi_rt_make_void());
            vm->arg_top += 1;
            break;
          }

          XSlice name = chunk->symbols[(size_t)ins.imm];
          MiRtValue v = mi_rt_make_string_slice(name);
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

          bool is_qualified = s_slice_has_double_colon(cmd_name);

          // Scoped commands: a local var may shadow a builtin (unqualified only). 
          if (!is_qualified)
          {
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
          }

          MiRtCmd* target = NULL;
          if (chunk->cmd_targets && ins.imm >= 0 && (size_t)ins.imm < chunk->cmd_count)
          {
            target = chunk->cmd_targets[ins.imm];
          }
          if (!target && is_qualified)
          {
            // Resolve qualified name lazily and cache it.
            // This avoids per-call qualified lookup; only the first call resolves.
            MiRtCmd* q = NULL;
            if (s_vm_resolve_qualified_cmd(vm, cmd_name, &q) && q)
            {
              chunk->cmd_targets[ins.imm] = q;
              target = q;
            }
          }

          if (!target)
          {
            mi_error("mi_vm: CALL_CMD unresolved command\n");
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            for (int i = 0; i < argc; i += 1)
            {
              mi_rt_value_release(vm->rt, argv[i]);
            }
            break;
          }
          s_vm_reg_set(vm, ins.a, s_vm_exec_cmd_value(vm, cmd_name, mi_rt_make_cmd(target), argc, argv));
          for (int i = 0; i < argc; i += 1)
          {
            mi_rt_value_release(vm->rt, argv[i]);
          }
          mi_rt_value_assign(vm->rt, &last, vm->regs[ins.a]);
        } break;

      case MI_VM_OP_CALL_CMD_FAST:
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

          if (!chunk->cmd_names || ins.imm < 0 || (size_t)ins.imm >= chunk->cmd_count)
          {
            mi_error("mi_vm: CALL_CMD_FAST bad cmd id\n");
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            for (int i = 0; i < argc; i += 1)
            {
              mi_rt_value_release(vm->rt, argv[i]);
            }
            break;
          }

          XSlice cmd_name = chunk->cmd_names[ins.imm];
          MiRtCmd* target = chunk->cmd_targets ? chunk->cmd_targets[ins.imm] : NULL;
          if (!target)
          {
            mi_error("mi_vm: CALL_CMD_FAST unresolved command\n");
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            for (int i = 0; i < argc; i += 1)
            {
              mi_rt_value_release(vm->rt, argv[i]);
            }
            break;
          }

          s_vm_reg_set(vm, ins.a, s_vm_exec_cmd_value(vm, cmd_name, mi_rt_make_cmd(target), argc, argv));
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

          if (head.kind == MI_RT_VAL_CMD)
          {
            s_vm_reg_set(vm, ins.a, s_vm_exec_cmd_value(vm, (XSlice){NULL, 0u}, head, argc, argv));
            for (int i = 0; i < argc; i += 1)
            {
              mi_rt_value_release(vm->rt, argv[i]);
            }
            mi_rt_value_assign(vm->rt, &last, vm->regs[ins.a]);
            break;
          }

          if (head.kind == MI_RT_VAL_BLOCK)
          {
            if (argc != 0)
            {
              mi_error("mi_vm: cannot call block with args (DCALL)\n");
              s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            }
            else
            {
              MiRtValue ret = s_vm_exec_block_value(vm, head, chunk, vm->dbg_ip);
              s_vm_reg_set(vm, ins.a, ret);
            }

            for (int i = 0; i < argc; i += 1)
            {
              mi_rt_value_release(vm->rt, argv[i]);
            }
            mi_rt_value_assign(vm->rt, &last, vm->regs[ins.a]);
            break;
          }

          if (head.kind != MI_RT_VAL_STRING)
          {
            mi_error("mi_vm: dynamic command head must be string/cmd/block\n");
            s_vm_reg_set(vm, ins.a, mi_rt_make_void());
            for (int i = 0; i < argc; i += 1)
            {
              mi_rt_value_release(vm->rt, argv[i]);
            }
            mi_rt_value_assign(vm->rt, &last, vm->regs[ins.a]);
            break;
          }

          // Qualified call for dynamic heads (string).
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

          // First: scoped commands stored as variables.
          MiRtValue scoped = mi_rt_make_void();
          if (mi_rt_var_get(vm->rt, head.as.s, &scoped) && scoped.kind == MI_RT_VAL_CMD)
          {
            s_vm_reg_set(vm, ins.a, s_vm_exec_cmd_value(vm, head.as.s, scoped, argc, argv));
          }
          else
          {
            MiRtValue global_cmd = mi_rt_make_void();
            if (!mi_vm_find_command(vm, head.as.s, &global_cmd) || global_cmd.kind != MI_RT_VAL_CMD)
            {
              mi_error_fmt("mi_vm: unknown command: %.*s\n", (int)head.as.s.length, head.as.s.ptr);
              s_vm_reg_set(vm, ins.a, mi_rt_make_void());
              for (int i = 0; i < argc; i += 1)
              {
                mi_rt_value_release(vm->rt, argv[i]);
              }
              break;
            }

            MiRtValue ret = s_vm_exec_cmd_value(vm, head.as.s, global_cmd, argc, argv);
            mi_rt_value_release(vm->rt, global_cmd);
            s_vm_reg_set(vm, ins.a, ret);
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
          MI_ASSERT(ins.a < MI_VM_REG_COUNT);
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
              s_vm_report_error(vm, "JUMP_IF out of range");
              MI_ASSERT(npc >= 0 && npc <= (int64_t)chunk->code_count);
              return last;
            }
            pc = (size_t)npc;
          }
        } break;

      case MI_VM_OP_RETURN:
        {
          MI_ASSERT(ins.a < MI_VM_REG_COUNT);

          MiRtValue ret = vm->regs[ins.a];
          mi_rt_value_retain(vm->rt, ret);
          return ret;
        }

      case MI_VM_OP_HALT:
        return last;

      default:
        s_vm_report_error(vm, "unhandled opcode");
        mi_error_fmt("  opcode: %u\n", (unsigned) ins.op);
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
    case MI_VM_OP_LOAD_MEMBER:        return "LDM";
    case MI_VM_OP_STORE_MEMBER:       return "STM";
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
    case MI_VM_OP_CALL_CMD_FAST:      return "CALLF";
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

  // Cycle/alias guard: chunks can be shared, and bugs can create cycles.
  // Disassembler must not recurse forever.
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
    // Copy the incoming stack so the function can be called with NULL. 
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

    // Print prefix: pc + raw bytes 
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

    // Build the instruction text (no comment) and an optional comment,
    // then print them with a fixed comment column.
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

      case MI_VM_OP_LOAD_MEMBER:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u, %d", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b, (int)ins.imm);
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

      case MI_VM_OP_STORE_MEMBER:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u, %d", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b, (int)ins.imm);
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
          // IMPORTANT: keep previous behavior: print inline value, comment is const_N 
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

      case MI_VM_OP_CALL_CMD_FAST:
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

    // Special-case: ARG_PUSH_CONST must print value inline, not an index. 
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

    // Print with fixed comment column 
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

