#include "mi_compile.h"
#include "mi_fold.h"

#include "mi_log.h"
#include <stdlib.h>
#include <string.h>

#include <stdx_strbuilder.h>

//----------------------------------------------------------
// Forward declarations (compiler-local)
//----------------------------------------------------------

static bool      s_chunk_const_eq(MiRtValue a, MiRtValue b);
static void      s_chunk_emit(MiVmChunk* c, MiVmOp op, uint8_t a, uint8_t b, uint8_t c0, int32_t imm);
static MiVmChunk* s_chunk_create(void);
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
    mi_error("mi_compile: out of memory\n");
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

static XSlice s_slice_dup_heap(XSlice s)
{
  if (s.length == 0)
  {
    return x_slice_empty();
  }

  char* p = (char*)malloc(s.length + 1u);
  if (!p)
  {
    mi_error("mi_compile: out of memory\n");
    exit(1);
  }

  memcpy(p, s.ptr, s.length);
  p[s.length] = 0;
  return x_slice_init(p, s.length);
}

static bool s_chunk_const_eq(MiRtValue a, MiRtValue b)
{
  if (a.kind != b.kind)
  {
    return false;
  }

  switch (a.kind)
  {
    case MI_RT_VAL_VOID: return true;
    case MI_RT_VAL_BOOL: return a.as.b == b.as.b;
    case MI_RT_VAL_INT:  return a.as.i == b.as.i;
    case MI_RT_VAL_FLOAT:return a.as.f == b.as.f;
    case MI_RT_VAL_STRING:
    {
      return s_slice_eq(a.as.s, b.as.s);
    }
    case MI_RT_VAL_BLOCK: /* blocks compared by pointer identity */ return a.as.block == b.as.block;
    case MI_RT_VAL_CMD:   return a.as.cmd == b.as.cmd;
    case MI_RT_VAL_LIST:  return a.as.list == b.as.list;
    case MI_RT_VAL_DICT:  return a.as.dict == b.as.dict;
    case MI_RT_VAL_PAIR:  return a.as.pair == b.as.pair;
    case MI_RT_VAL_KVREF:
    {
      return (a.as.kvref.dict == b.as.kvref.dict) && (a.as.kvref.entry_index == b.as.kvref.entry_index);
    }
    default: return false;
  }
}

static void s_chunk_emit(MiVmChunk* c, MiVmOp op, uint8_t a, uint8_t b, uint8_t c0, int32_t imm)
{
  if (c->code_count == c->code_capacity)
  {
    size_t new_cap = c->code_capacity ? c->code_capacity * 2u : 256u;
    c->code = (MiVmIns*)s_realloc(c->code, new_cap * sizeof(*c->code));
    c->code_capacity = new_cap;
  }

  MiVmIns ins;
  ins.op = (uint8_t)op;
  ins.a = a;
  ins.b = b;
  ins.c = c0;
  ins.imm = imm;

  c->code[c->code_count++] = ins;
}

static MiVmChunk* s_chunk_create(void)
{
  MiVmChunk* c = (MiVmChunk*)calloc(1u, sizeof(MiVmChunk));
  if (!c)
  {
    mi_error("mi_compile: out of memory\n");
    exit(1);
  }
  return c;
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

  if (v.kind == MI_RT_VAL_STRING)
  {
    v.as.s = s_slice_dup_heap(v.as.s);
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
  c->symbols[c->symbol_count++] = s_slice_dup_heap(name);
  return idx;
}

/* Command lookup lives in the VM runtime. */

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
  c->cmd_names[c->cmd_count] = s_slice_dup_heap(name);
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

    MiVmCommandFn cmd_fn = mi_vm_find_command_fn(b->vm, x_slice_from_cstr("cmd"));
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
      if (arg->kind == MI_EXPR_STRING_LITERAL) v = mi_rt_make_string_slice(arg->token.lexeme);

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
    MiVmCommandFn fn = mi_vm_find_command_fn(b->vm, name);
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
        /* Use the token lexeme as the source of truth for the slice.
           Some earlier AST transforms may not preserve as.string_lit.value reliably. */
        int32_t k = s_chunk_add_const(b->chunk, mi_rt_make_string_slice(e->token.lexeme));
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

MiVmChunk* mi_compile_vm_script(MiVm* vm, const MiScript* script)
{
  if (!vm || !script)
  {
    return NULL;
  }
  return s_vm_compile_script_ast(vm, script, NULL);
}
