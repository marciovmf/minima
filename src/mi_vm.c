#include "mi_vm.h"
#include "mi_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void s_vm_print_value(const MiRtValue* v)
{
  switch (v->kind)
  {
    case MI_RT_VAL_VOID:   printf("()\n"); break;
    case MI_RT_VAL_INT:    printf("%lld\n", v->as.i); break;
    case MI_RT_VAL_FLOAT:  printf("%g\n", v->as.f); break;
    case MI_RT_VAL_BOOL:   printf("%s\n", v->as.b ? "true" : "false"); break;
    case MI_RT_VAL_STRING: printf("%.*s\n", (int) v->as.s.length, v->as.s.ptr); break;

    case MI_RT_VAL_LIST:
                           {
                             printf("[");
                             for (size_t i = 0; i < v->as.list->count; ++i)
                             {
                               const MiRtValue* it = &v->as.list->items[i];
                               if (i != 0)
                               {
                                 printf(", ");
                               }
                               if (it->kind == MI_RT_VAL_STRING)
                               {
                                 printf("\"%.*s\"", (int) it->as.s.length, it->as.s.ptr);
                               }
                               else if (it->kind == MI_RT_VAL_INT)
                               {
                                 printf("%lld", it->as.i);
                               }
                               else if (it->kind == MI_RT_VAL_FLOAT)
                               {
                                 printf("%g", it->as.f);
                               }
                               else if (it->kind == MI_RT_VAL_BOOL)
                               {
                                 printf("%s", it->as.b ? "true" : "false");
                               }
                               else
                               {
                                 printf("<%d>", (int) it->kind);
                               }
                             }
                             printf("]\n");
                           } break;

    case MI_RT_VAL_BLOCK:  printf("{...}\n"); break;
    case MI_RT_VAL_PAIR:   printf("<pair>\n"); break;
    default:               printf("<unknown>\n"); break;
  }
}

static MiRtValue s_vm_cmd_print(MiRuntime* rt, const XSlice* head_name, int argc, MiExprList* args)
{
  (void) rt;
  (void) head_name;

  (void) argc;
  (void) args;
  return mi_rt_make_void();
}

static MiRtValue s_vm_cmd_set(MiRuntime* rt, const XSlice* head_name, int argc, MiExprList* args)
{
  (void) head_name;

  (void) rt;
  (void) argc;
  (void) args;
  return mi_rt_make_void();
}

//----------------------------------------------------------
// VM init/shutdown
//----------------------------------------------------------

void mi_vm_init(MiVm* vm, MiRuntime* rt)
{
  memset(vm, 0, sizeof(*vm));
  vm->rt = rt;
  vm->arg_top = 0;

  // Minimal builtins to get started.
  (void) mi_vm_register_command(vm, x_slice_from_cstr("print"), s_vm_cmd_print);
  (void) mi_vm_register_command(vm, x_slice_from_cstr("set"),   s_vm_cmd_set);
}

void mi_vm_shutdown(MiVm* vm)
{
  if (!vm)
  {
    return;
  }

  free(vm->commands);
  vm->commands = NULL;
  vm->command_count = 0;
  vm->command_capacity = 0;
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

void mi_vm_chunk_destroy(MiVmChunk* chunk)
{
  if (!chunk)
  {
    return;
  }

  free(chunk->code);
  free(chunk->consts);
  free(chunk->symbols);
  free(chunk->cmd_fns);
  free(chunk->cmd_names);
  free(chunk);
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

static int32_t s_chunk_add_const(MiVmChunk* c, MiRtValue v)
{
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

//----------------------------------------------------------
// Compiler (AST -> bytecode)
//----------------------------------------------------------

typedef struct MiVmBuild
{
  MiVm*       vm;
  MiVmChunk*  chunk;
  uint8_t     next_reg;
} MiVmBuild;

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

static uint8_t s_compile_expr(MiVmBuild* b, const MiExpr* e);

static uint8_t s_compile_command_expr(MiVmBuild* b, const MiExpr* e)
{
  uint8_t dst = s_alloc_reg(b);
  uint8_t argc = 0;

  const MiExprList* it = e->as.command.args;
  while (it)
  {
    uint8_t r = s_compile_expr(b, it->expr);
    s_chunk_emit(b->chunk, MI_VM_OP_PUSH_ARG, r, 0, 0, 0);
    argc++;
    it = it->next;
  }

  const MiExpr* head = e->as.command.head;
  if (head && head->kind == MI_EXPR_STRING_LITERAL)
  {
    XSlice name = head->as.string_lit.value;
    MiVmCommandFn fn = s_vm_find_command_fn(b->vm, name);
    if (!fn)
    {
      mi_error_fmt("mi_vm: unknown command: %.*s\n", (int) name.length, name.ptr);
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, s_chunk_add_const(b->chunk, mi_rt_make_void()));
      return dst;
    }

    int32_t cmd_id = s_chunk_add_cmd_fn(b->chunk, name, fn);
    s_chunk_emit(b->chunk, MI_VM_OP_CALL_CMD, dst, argc, 0, cmd_id);
    return dst;
  }

  // Dynamic head: compute name into a register and do dynamic lookup.
  uint8_t head_reg = s_compile_expr(b, head);
  s_chunk_emit(b->chunk, MI_VM_OP_CALL_CMD_DYN, dst, head_reg, argc, 0);
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
      return s_compile_command_expr(b, e);

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

MiVmChunk* mi_vm_compile_script(MiVm* vm, const MiScript* script, XArena* arena)
{
  (void) arena;

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

    (void) s_compile_command_expr(&b, &fake);
    it = it->next;
  }

  s_chunk_emit(chunk, MI_VM_OP_HALT, 0, 0, 0, 0);

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

  return mi_rt_make_void();
}

MiRtValue mi_vm_execute(MiVm* vm, const MiVmChunk* chunk)
{
  if (!vm || !chunk)
  {
    return mi_rt_make_void();
  }

  vm->arg_top = 0;
  MiRtValue last = mi_rt_make_void();

  size_t pc = 0;
  while (pc < chunk->code_count)
  {
    MiVmIns ins = chunk->code[pc++];
    switch ((MiVmOp) ins.op)
    {
      case MI_VM_OP_NOOP:
        break;

      case MI_VM_OP_LOAD_CONST:
        vm->regs[ins.a] = chunk->consts[ins.imm];
        break;

      case MI_VM_OP_MOV:
        vm->regs[ins.a] = vm->regs[ins.b];
        break;

      case MI_VM_OP_NEG:
        {
          MiRtValue x = vm->regs[ins.b];
          if (x.kind == MI_RT_VAL_INT)
          {
            vm->regs[ins.a] = mi_rt_make_int(-x.as.i);
          }
          else if (x.kind == MI_RT_VAL_FLOAT)
          {
            vm->regs[ins.a] = mi_rt_make_float(-x.as.f);
          }
          else
          {
            vm->regs[ins.a] = mi_rt_make_void();
          }
        } break;

      case MI_VM_OP_NOT:
        {
          MiRtValue x = vm->regs[ins.b];
          if (x.kind == MI_RT_VAL_BOOL)
          {
            vm->regs[ins.a] = mi_rt_make_bool(!x.as.b);
          }
          else
          {
            vm->regs[ins.a] = mi_rt_make_void();
          }
        } break;

      case MI_VM_OP_ADD:
      case MI_VM_OP_SUB:
      case MI_VM_OP_MUL:
      case MI_VM_OP_DIV:
      case MI_VM_OP_MOD:
        {
          vm->regs[ins.a] = s_vm_binary_numeric((MiVmOp) ins.op, &vm->regs[ins.b], &vm->regs[ins.c]);
        } break;

      case MI_VM_OP_EQ:
      case MI_VM_OP_NEQ:
      case MI_VM_OP_LT:
      case MI_VM_OP_LTEQ:
      case MI_VM_OP_GT:
      case MI_VM_OP_GTEQ:
        {
          vm->regs[ins.a] = s_vm_binary_compare((MiVmOp) ins.op, &vm->regs[ins.b], &vm->regs[ins.c]);
        } break;

      case MI_VM_OP_AND:
        {
          MiRtValue x = vm->regs[ins.b];
          MiRtValue y = vm->regs[ins.c];
          if (x.kind == MI_RT_VAL_BOOL && y.kind == MI_RT_VAL_BOOL)
          {
            vm->regs[ins.a] = mi_rt_make_bool(x.as.b && y.as.b);
          }
          else
          {
            vm->regs[ins.a] = mi_rt_make_void();
          }
        } break;

      case MI_VM_OP_OR:
        {
          MiRtValue x = vm->regs[ins.b];
          MiRtValue y = vm->regs[ins.c];
          if (x.kind == MI_RT_VAL_BOOL && y.kind == MI_RT_VAL_BOOL)
          {
            vm->regs[ins.a] = mi_rt_make_bool(x.as.b || y.as.b);
          }
          else
          {
            vm->regs[ins.a] = mi_rt_make_void();
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
          vm->regs[ins.a] = v;
        } break;

      case MI_VM_OP_STORE_VAR:
        {
          XSlice name = chunk->symbols[ins.imm];
          (void) mi_rt_var_set(vm->rt, name, vm->regs[ins.a]);
        } break;

      case MI_VM_OP_LOAD_INDIRECT_VAR:
        {
          MiRtValue n = vm->regs[ins.b];
          if (n.kind != MI_RT_VAL_STRING)
          {
            mi_error("indirect variable name must be string\n");
            vm->regs[ins.a] = mi_rt_make_void();
            break;
          }
          MiRtValue v;
          if (!mi_rt_var_get(vm->rt, n.as.s, &v))
          {
            mi_error_fmt("undefined variable: %.*s\n", (int) n.as.s.length, n.as.s.ptr);
            v = mi_rt_make_void();
          }
          vm->regs[ins.a] = v;
        } break;

      case MI_VM_OP_PUSH_ARG:
        {
          if (vm->arg_top >= 256)
          {
            mi_error("mi_vm: arg stack overflow\n");
            break;
          }
          vm->arg_stack[vm->arg_top++] = vm->regs[ins.a];
        } break;

      case MI_VM_OP_CALL_CMD:
        {
          int argc = (int) ins.b;
          if (argc > vm->arg_top)
          {
            mi_error("mi_vm: arg stack underflow\n");
            vm->regs[ins.a] = mi_rt_make_void();
            break;
          }

          MiRtValue argv[256];
          for (int i = argc - 1; i >= 0; --i)
          {
            argv[i] = vm->arg_stack[--vm->arg_top];
          }

          (void) argv;
          (void) chunk;
          mi_error("mi_vm: CALL_CMD not supported yet\n");
          vm->regs[ins.a] = mi_rt_make_void();
          last = vm->regs[ins.a];
        } break;

      case MI_VM_OP_CALL_CMD_DYN:
        {
          int argc = (int) ins.c;
          if (argc > vm->arg_top)
          {
            mi_error("mi_vm: arg stack underflow\n");
            vm->regs[ins.a] = mi_rt_make_void();
            break;
          }

          MiRtValue head = vm->regs[ins.b];
          if (head.kind != MI_RT_VAL_STRING)
          {
            mi_error("mi_vm: dynamic command head must be string\n");
            vm->regs[ins.a] = mi_rt_make_void();
            break;
          }

          MiRtValue argv[256];
          for (int i = argc - 1; i >= 0; --i)
          {
            argv[i] = vm->arg_stack[--vm->arg_top];
          }

          (void) argv;
          mi_error("mi_vm: CALL_CMD_DYN not supported yet\n");
          vm->regs[ins.a] = mi_rt_make_void();
          last = vm->regs[ins.a];
        } break;

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
    case MI_VM_OP_NOOP: return "NOOP";
    case MI_VM_OP_LOAD_CONST: return "LOAD_CONST";
    case MI_VM_OP_MOV: return "MOV";
    case MI_VM_OP_NEG: return "NEG";
    case MI_VM_OP_NOT: return "NOT";
    case MI_VM_OP_ADD: return "ADD";
    case MI_VM_OP_SUB: return "SUB";
    case MI_VM_OP_MUL: return "MUL";
    case MI_VM_OP_DIV: return "DIV";
    case MI_VM_OP_MOD: return "MOD";
    case MI_VM_OP_EQ: return "EQ";
    case MI_VM_OP_NEQ: return "NEQ";
    case MI_VM_OP_LT: return "LT";
    case MI_VM_OP_LTEQ: return "LTEQ";
    case MI_VM_OP_GT: return "GT";
    case MI_VM_OP_GTEQ: return "GTEQ";
    case MI_VM_OP_AND: return "AND";
    case MI_VM_OP_OR: return "OR";
    case MI_VM_OP_LOAD_VAR: return "LOAD_VAR";
    case MI_VM_OP_STORE_VAR: return "STORE_VAR";
    case MI_VM_OP_LOAD_INDIRECT_VAR: return "LOAD_INDIRECT_VAR";
    case MI_VM_OP_PUSH_ARG: return "PUSH_ARG";
    case MI_VM_OP_CALL_CMD: return "CALL_CMD";
    case MI_VM_OP_CALL_CMD_DYN: return "CALL_CMD_DYN";
    case MI_VM_OP_JUMP: return "JUMP";
    case MI_VM_OP_JUMP_IF_TRUE: return "JUMP_IF_TRUE";
    case MI_VM_OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
    case MI_VM_OP_RETURN: return "RETURN";
    case MI_VM_OP_HALT: return "HALT";
    default: return "?";
  }
}

void mi_vm_disasm(const MiVmChunk* chunk)
{
  if (!chunk)
  {
    return;
  }

  for (size_t i = 0; i < chunk->code_count; ++i)
  {
    MiVmIns ins = chunk->code[i];
    printf("%04zu  %-16s a=%u b=%u c=%u imm=%d\n",
        i,
        s_op_name((MiVmOp) ins.op),
        (unsigned) ins.a,
        (unsigned) ins.b,
        (unsigned) ins.c,
        (int) ins.imm);
  }
}
