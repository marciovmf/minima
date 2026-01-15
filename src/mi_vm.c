#include "mi_vm.h"
#include "mi_log.h"
#include "mi_fold.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static MiRtValue s_vm_exec_block_value(MiVm* vm, MiRtValue block_value);

static void s_vm_print_value_inline(const MiRtValue* v)
{
  switch (v->kind)
  {
    case MI_RT_VAL_VOID:   printf("()"); break;
    case MI_RT_VAL_INT:    printf("%lld", v->as.i); break;
    case MI_RT_VAL_FLOAT:  printf("%g", v->as.f); break;
    case MI_RT_VAL_BOOL:   printf("%s", v->as.b ? "true" : "false"); break;
    case MI_RT_VAL_STRING: printf("%.*s", (int) v->as.s.length, v->as.s.ptr); break;
    case MI_RT_VAL_LIST:   printf("[list]"); break;
    case MI_RT_VAL_BLOCK:  printf("{...}"); break;
    case MI_RT_VAL_PAIR:   printf("<pair>"); break;
    default:               printf("<unknown>"); break;
  }
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
    case MI_RT_VAL_BLOCK:  (void)snprintf(out, cap, "{...}"); break;
    case MI_RT_VAL_PAIR:   (void)snprintf(out, cap, "<pair>"); break;
    default:               (void)snprintf(out, cap, "<unknown>"); break;
  }
}

static MiRtValue s_vm_cmd_print(MiVm* vm, int argc, const MiRtValue* argv)
{
  (void) vm;

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

static MiRtValue s_vm_cmd_set(MiVm* vm, int argc, const MiRtValue* argv)
{
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

static MiRtValue s_vm_cmd_call(MiVm* vm, int argc, const MiRtValue* argv)
{
  if (argc != 1)
  {
    mi_error("call: expected 1 argument\n");
    return mi_rt_make_void();
  }

  return s_vm_exec_block_value(vm, argv[0]);
}

static MiRtValue s_vm_exec_block_value(MiVm* vm, MiRtValue block_value)
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
  MiScopeFrame* parent = b->env ? b->env : vm->rt->current;

  /* CALL ABI:
     - r0 is return value
     - r1-r7 are VM-reserved and preserved across calls
     - r8+ are caller-saved (clobbered)
     - arg stack is not preserved across calls */
  MiRtValue saved_vm_regs[7];
  for (int i = 0; i < 7; ++i)
  {
    saved_vm_regs[i] = vm->regs[1 + i];
  }

  mi_rt_scope_push_with_parent(vm->rt, parent);
  MiRtValue ret = mi_vm_execute(vm, sub);
  mi_rt_scope_pop(vm->rt);

  for (int i = 0; i < 7; ++i)
  {
    vm->regs[1 + i] = saved_vm_regs[i];
  }
  vm->arg_top = 0;

  return ret;
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
  (void) mi_vm_register_command(vm, x_slice_from_cstr("call"),  s_vm_cmd_call);
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

  if (chunk->subchunks)
  {
    for (size_t i = 0; i < chunk->subchunk_count; ++i)
    {
      mi_vm_chunk_destroy(chunk->subchunks[i]);
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
}

static bool s_expr_is_lit_string(const MiExpr* e, const char* cstr)
{
  if (!e || e->kind != MI_EXPR_STRING_LITERAL)
  {
    return false;
  }

  return s_slice_eq(e->as.string_lit.value, x_slice_from_cstr(cstr));
}

static uint8_t s_compile_command_expr(MiVmBuild* b, const MiExpr* e)
{
  uint8_t dst = s_alloc_reg(b);
  uint8_t argc = 0;

  /* Start a fresh argument list for every call.
     This also makes disassembly easier to read. */
  s_chunk_emit(b->chunk, MI_VM_OP_ARG_CLEAR, 0, 0, 0, 0);

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
      int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_void());
      s_chunk_emit(b->chunk, MI_VM_OP_LOAD_CONST, dst, 0, 0, k);
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

      uint8_t block_reg = s_compile_expr(b, then_block);
      s_chunk_emit(b->chunk, MI_VM_OP_CALL_BLOCK, dst, block_reg, 0, 0);

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

        //uint8_t block_reg = s_compile_expr(b, else_block);
        block_reg = s_compile_expr(b, else_block);
        s_chunk_emit(b->chunk, MI_VM_OP_CALL_BLOCK, dst, block_reg, 0, 0);
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

    size_t loop_start = b->chunk->code_count;

    uint8_t cond_reg = s_compile_expr(b, cond);

    /* JF cond, <to loop end> */
    size_t jf_index = b->chunk->code_count;
    s_chunk_emit(b->chunk, MI_VM_OP_JUMP_IF_FALSE, cond_reg, 0, 0, 0);

    uint8_t block_reg = s_compile_expr(b, body_block);
    s_chunk_emit(b->chunk, MI_VM_OP_CALL_BLOCK, dst, block_reg, 0, 0);

    /* JMP back to loop_start */
    size_t jmp_index = b->chunk->code_count;
    s_chunk_emit(b->chunk, MI_VM_OP_JUMP, 0, 0, 0, 0);
    {
      int32_t rel_back = (int32_t)((int64_t)loop_start - (int64_t)(jmp_index + 1u));
      s_chunk_patch_imm(b->chunk, jmp_index, rel_back);
    }

    /* Patch JF to jump here (loop end). */
    {
      int32_t rel_end = (int32_t)((int64_t)b->chunk->code_count - (int64_t)(jf_index + 1u));
      s_chunk_patch_imm(b->chunk, jf_index, rel_end);
    }

    return dst;
  }

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

    /* Fallback: compute into a register then push. */
    uint8_t r = s_compile_expr(b, arg);
    s_chunk_emit(b->chunk, MI_VM_OP_ARG_PUSH, r, 0, 0, 0);
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

    (void) s_compile_command_expr(&b, &fake);
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
    (void) mi_vm_register_command(vm, x_slice_from_cstr("set"),   s_vm_cmd_set);
    (void) mi_vm_register_command(vm, x_slice_from_cstr("call"),  s_vm_cmd_call);
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

      case MI_VM_OP_LOAD_BLOCK:
        {
          if (ins.imm < 0 || (size_t)ins.imm >= chunk->subchunk_count)
          {
            mi_error("mi_vm: LOAD_BLOCK invalid subchunk index\n");
            vm->regs[ins.a] = mi_rt_make_void();
            break;
          }

          MiRtBlock* b = mi_rt_block_create(vm->rt);
          b->kind = MI_RT_BLOCK_VM_CHUNK;
          b->ptr = (void*)chunk->subchunks[(size_t)ins.imm];
          b->env = vm->rt->current;
          b->id = (uint32_t)ins.imm;
          vm->regs[ins.a] = mi_rt_make_block(b);
        } break;

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

      case MI_VM_OP_ARG_CLEAR:
        vm->arg_top = 0;
        break;

      case MI_VM_OP_ARG_PUSH:
        {
          if (vm->arg_top >= MI_VM_ARG_STACK_COUNT)
          {
            mi_error("mi_vm: arg stack overflow\n");
            break;
          }
          vm->arg_stack[vm->arg_top++] = vm->regs[ins.a];
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
            vm->arg_stack[vm->arg_top++] = mi_rt_make_void();
            break;
          }
          vm->arg_stack[vm->arg_top++] = chunk->consts[ins.imm];
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
            vm->arg_stack[vm->arg_top++] = mi_rt_make_void();
            break;
          }
          XSlice name = chunk->symbols[ins.imm];
          MiRtValue v;
          if (!mi_rt_var_get(vm->rt, name, &v))
          {
            mi_error_fmt("undefined variable: %.*s\n", (int)name.length, name.ptr);
            v = mi_rt_make_void();
          }
          vm->arg_stack[vm->arg_top++] = v;
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

          MiRtValue argv[MI_VM_ARG_STACK_COUNT];
          for (int i = argc - 1; i >= 0; --i)
          {
            argv[i] = vm->arg_stack[--vm->arg_top];
          }

          MiVmCommandFn fn = chunk->cmd_fns[ins.imm];
          if (!fn)
          {
            mi_error("mi_vm: CALL_CMD null function\n");
            vm->regs[ins.a] = mi_rt_make_void();
            break;
          }

          vm->regs[ins.a] = fn(vm, argc, argv);
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

          MiRtValue argv[MI_VM_ARG_STACK_COUNT];
          for (int i = argc - 1; i >= 0; --i)
          {
            argv[i] = vm->arg_stack[--vm->arg_top];
          }

          MiVmCommandFn fn = s_vm_find_command_fn(vm, head.as.s);
          if (!fn)
          {
            mi_error_fmt("mi_vm: unknown command: %.*s\n", (int)head.as.s.length, head.as.s.ptr);
            vm->regs[ins.a] = mi_rt_make_void();
            break;
          }

          vm->regs[ins.a] = fn(vm, argc, argv);
          last = vm->regs[ins.a];
        } break;

      case MI_VM_OP_CALL_BLOCK:
        {
          MiRtValue ret = s_vm_exec_block_value(vm, vm->regs[ins.b]);
          vm->regs[ins.a] = ret;
          last = ret;
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
        return vm->regs[ins.a];

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
    case MI_VM_OP_LOAD_INDIRECT_VAR:  return "LDIV";
    case MI_VM_OP_ARG_CLEAR:          return "ACLR";
    case MI_VM_OP_ARG_PUSH:           return "APR";
    case MI_VM_OP_ARG_PUSH_CONST:     return "APC";
    case MI_VM_OP_ARG_PUSH_VAR_SYM:   return "APV";
    case MI_VM_OP_ARG_PUSH_SYM:       return "APS";
    case MI_VM_OP_CALL_CMD:           return "CALL";
    case MI_VM_OP_CALL_CMD_DYN:       return "DCALL";
    case MI_VM_OP_CALL_BLOCK:         return "BCALL";
    case MI_VM_OP_JUMP:               return "JMP";
    case MI_VM_OP_JUMP_IF_TRUE:       return "JT";
    case MI_VM_OP_JUMP_IF_FALSE:      return "JF";
    case MI_VM_OP_RETURN:             return "RET";
    case MI_VM_OP_HALT:               return "HALT";
    default: mi_error_fmt("Unknown opcode %X\n", op); return "?";
  }
}

void mi_vm_disasm(const MiVmChunk* chunk)
{
  if (!chunk)
  {
    return;
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
            /* comment: const_N <value> */
            char vbuf[96];
            vbuf[0] = '\0';
            /* Print value inline into a buffer by using the same printer via a temp FILE? Not available.
               Keep consistent with previous file: comment shows const_N and value is printed by LOAD_CONST itself only.
               So here we keep the value out; LOAD_CONST already shows const_N. */
            (void)vbuf;
            (void)snprintf(comment, sizeof(comment), "const_%d", (int)ins.imm);
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

      case MI_VM_OP_LOAD_INDIRECT_VAR:
        (void)snprintf(instr, sizeof(instr), "%s r%u, r%u", s_op_name(op), (unsigned)ins.a, (unsigned)ins.b);
        break;

      case MI_VM_OP_ARG_CLEAR:
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

          /* We'll append the value into instr by printing into a temp buffer via value-to-string helper if available.
             If not available in this translation unit, fall back to printing directly like before by using s_vm_print_value_inline
             and then padding. Here we do the safe route: keep instr as "ARG_PUSH_CONST" and print value inline separately. */
          if (ins.imm >= 0 && (size_t)ins.imm < chunk->const_count)
          {
            /* Print "ARG_PUSH_CONST " now, then inline value, then pad+comment. */
            /* We'll handle this as a special case after switch. */
            (void)snprintf(comment, sizeof(comment), "const_%d", (int)ins.imm);
            (void)snprintf(vbuf, sizeof(vbuf), "%d", 0);
          }
          else
          {
            (void)snprintf(comment, sizeof(comment), "<oob>");
          }

          /* Mark as special by storing op name in instr and using comment; actual value printed later. */
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

      case MI_VM_OP_JUMP:
        (void)snprintf(instr, sizeof(instr), "%s %d", s_op_name(op), (int)ins.imm);
        break;

      case MI_VM_OP_JUMP_IF_TRUE:
        (void)snprintf(instr, sizeof(instr), "%s r%u, %d", s_op_name(op), (unsigned)ins.a, (int)ins.imm);
        break;

      case MI_VM_OP_JUMP_IF_FALSE:
        (void)snprintf(instr, sizeof(instr), "%s r%u, %d", s_op_name(op), (unsigned)ins.a, (int)ins.imm);
        break;

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
      /* Print "ARG_PUSH_CONST <value>" into a fixed field, then comment */
      char tmp[96];
      tmp[0] = '\0';

      if (ins.imm >= 0 && (size_t)ins.imm < chunk->const_count)
      {
        /* print into stdout via s_vm_print_value_inline but keep alignment:
           we print into a temp string using existing s_vm_value_to_string if available. */
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

      (void)tmp;
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
      mi_vm_disasm(chunk->subchunks[i]);
    }
  }

  printf("\n");
}
