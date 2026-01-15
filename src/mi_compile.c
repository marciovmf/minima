#include "mi_compile.h"
#include "mi_parse.h"
#include "mi_log.h"

#include <string.h>
#include <stdlib.h>

void mi_program_init(MiProgram* p, size_t arena_chunk_size);
void mi_program_shutdown(MiProgram* p);

//----------------------------------------------------------
// Register allocation
//----------------------------------------------------------

enum { MI_REG_COUNT = 32, MI_REG_FIRST_TEMP = 8 };

static uint8_t s_alloc_temp(MiCompiler* c)
{
  if (c->next_temp_reg < MI_REG_FIRST_TEMP)
  {
    c->next_temp_reg = MI_REG_FIRST_TEMP;
  }

  if (c->next_temp_reg >= MI_REG_COUNT)
  {
    mi_error_fmt("compile: out of registers (max %d, temps start at r%d) ", MI_REG_COUNT - 1, MI_REG_FIRST_TEMP);
    exit(1);
  }

  return c->next_temp_reg++;
}

static void s_reset_temps(MiCompiler* c)
{
  c->next_temp_reg = MI_REG_FIRST_TEMP;
}

//----------------------------------------------------------
// Emit helpers
//----------------------------------------------------------

static int s_emit0(MiChunk* ch, MiOp op)
{
  MiIns ins;
  memset(&ins, 0, sizeof(ins));
  ins.op = (uint8_t)op;
  return mi_bytecode_emit(ch, ins);
}

static int s_emit_a(MiChunk* ch, MiOp op, uint8_t a)
{
  MiIns ins;
  memset(&ins, 0, sizeof(ins));
  ins.op = (uint8_t)op;
  ins.a = a;
  return mi_bytecode_emit(ch, ins);
}

static int s_emit_ab(MiChunk* ch, MiOp op, uint8_t a, uint8_t b)
{
  MiIns ins;
  memset(&ins, 0, sizeof(ins));
  ins.op = (uint8_t)op;
  ins.a = a;
  ins.b = b;
  return mi_bytecode_emit(ch, ins);
}

static int s_emit_abc(MiChunk* ch, MiOp op, uint8_t a, uint8_t b, uint8_t c)
{
  MiIns ins;
  memset(&ins, 0, sizeof(ins));
  ins.op = (uint8_t)op;
  ins.a = a;
  ins.b = b;
  ins.c = c;
  return mi_bytecode_emit(ch, ins);
}

static int s_emit_a_imm(MiChunk* ch, MiOp op, uint8_t a, int32_t imm)
{
  MiIns ins;
  memset(&ins, 0, sizeof(ins));
  ins.op = (uint8_t)op;
  ins.a = a;
  ins.imm = imm;
  return mi_bytecode_emit(ch, ins);
}

static int s_emit_ab_imm(MiChunk* ch, MiOp op, uint8_t a, uint8_t b, int32_t imm)
{
  MiIns ins;
  memset(&ins, 0, sizeof(ins));
  ins.op = (uint8_t)op;
  ins.a = a;
  ins.b = b;
  ins.imm = imm;
  return mi_bytecode_emit(ch, ins);
}

//----------------------------------------------------------
// AST compilation
//----------------------------------------------------------

static bool s_compile_expr(MiCompiler* c, MiProgram* p, MiChunk* ch, const MiExpr* e, uint8_t dst);

static bool s_const_index_for_literal(MiCompiler* c, MiProgram* p, MiChunk* ch, const MiExpr* e, int* out_idx)
{
  (void)c;

  if (!p || !p->arena || !ch || !e || !out_idx)
  {
    return false;
  }

  switch (e->kind)
  {
  case MI_EXPR_INT_LITERAL:
    *out_idx = mi_bytecode_add_const(ch, mi_rt_make_int(e->as.int_lit.value));
    return *out_idx >= 0;

  case MI_EXPR_FLOAT_LITERAL:
    *out_idx = mi_bytecode_add_const(ch, mi_rt_make_float(e->as.float_lit.value));
    return *out_idx >= 0;

  case MI_EXPR_STRING_LITERAL:
  {
    XSlice s = e->as.string_lit.value;
    char* owned = x_arena_slicedup(p->arena, s.ptr, s.length, true);
    if (!owned)
    {
      mi_error("compile: out of memory");
      exit(1);
    }

    *out_idx = mi_bytecode_add_const(ch, mi_rt_make_string_slice(x_slice_init(owned, s.length)));
    return *out_idx >= 0;
  }

  case MI_EXPR_BOOL_LITERAL:
    *out_idx = mi_bytecode_add_const(ch, mi_rt_make_bool(e->as.bool_lit.value));
    return *out_idx >= 0;

  case MI_EXPR_VOID_LITERAL:
    *out_idx = mi_bytecode_add_const(ch, mi_rt_make_void());
    return *out_idx >= 0;

  default:
    return false;
  }
}

static bool s_emit_arg_value(MiCompiler* c, MiProgram* p, MiChunk* ch, const MiExpr* e)
{
  if (!e)
  {
    return true;
  }

  /* Direct argument pushes avoid staging into a register. */
  {
    int const_idx = -1;
    if (s_const_index_for_literal(c, p, ch, e, &const_idx))
    {
      return s_emit_a_imm(ch, MI_OP_ARG_PUSH_CONST, 0, (int32_t)const_idx) >= 0;
    }
  }

  if (e->kind == MI_EXPR_VAR && !e->as.var.is_indirect)
  {
    MiSymId sym = mi_bytecode_intern_symbol(ch, e->as.var.name, p);
    return s_emit_a_imm(ch, MI_OP_ARG_PUSH_VAR_SYM, 0, (int32_t)sym) >= 0;
  }

  /* Fallback: compute into a temp register then push. */
  uint8_t a = s_alloc_temp(c);
  if (!s_compile_expr(c, p, ch, e, a))
  {
    return false;
  }

  return s_emit_a(ch, MI_OP_ARG_PUSH, a) >= 0;
}

static MiOp s_binop_to_op(MiTokenKind op)
{
  switch (op)
  {
    case MI_TOK_PLUS: return MI_OP_ADD;
    case MI_TOK_MINUS: return MI_OP_SUB;
    case MI_TOK_STAR: return MI_OP_MUL;
    case MI_TOK_SLASH: return MI_OP_DIV;
    //case MI_TOK_PERCENT: return MI_OP_MOD;
    case MI_TOK_EQEQ: return MI_OP_EQ;
    case MI_TOK_BANGEQ: return MI_OP_NEQ;
    case MI_TOK_LT: return MI_OP_LT;
    case MI_TOK_LTEQ: return MI_OP_LTEQ;
    case MI_TOK_GT: return MI_OP_GT;
    case MI_TOK_GTEQ: return MI_OP_GTEQ;
    case MI_TOK_AND: return MI_OP_AND;
    case MI_TOK_OR: return MI_OP_OR;
    default: break;
  }

  return MI_OP_NOOP;
}

static bool s_compile_list(MiCompiler* c, MiProgram* p, MiChunk* ch, const MiExprList* items, uint8_t dst)
{
  /* Future: list literals could be compiled to const pool objects or built via builtin commands.
     For now, keep it as a constant-folded literal at the AST level, or treat as runtime call. */
  (void)c;
  (void)p;
  (void)ch;
  (void)items;
  (void)dst;
  mi_error("compile: list literals not implemented\n");
  return false;
}

static bool s_compile_expr(MiCompiler* c, MiProgram* p, MiChunk* ch, const MiExpr* e, uint8_t dst)
{
  if (!c || !ch || !e)
  {
    return false;
  }

  switch (e->kind)
  {
    case MI_EXPR_INT_LITERAL:
    {
      int idx = mi_bytecode_add_const(ch, mi_rt_make_int((long long)e->as.int_lit.value));
      if (idx < 0)
      {
        return false;
      }
      return s_emit_a_imm(ch, MI_OP_LOAD_CONST, dst, (int32_t)idx) >= 0;
    }

    case MI_EXPR_FLOAT_LITERAL:
    {
      int idx = mi_bytecode_add_const(ch, mi_rt_make_float(e->as.float_lit.value));
      if (idx < 0)
      {
        return false;
      }
      return s_emit_a_imm(ch, MI_OP_LOAD_CONST, dst, (int32_t)idx) >= 0;
    }

    case MI_EXPR_STRING_LITERAL:
    {
      if (!p || !p->arena)
      {
        return false;
      }

      XSlice s = e->as.string_lit.value;
      char* owned = x_arena_slicedup(p->arena, s.ptr, s.length, true);
      if (!owned)
      {
        return false;
      }

      int idx = mi_bytecode_add_const(ch, mi_rt_make_string_slice(x_slice_init(owned, s.length)));
      if (idx < 0)
      {
        return false;
      }
      return s_emit_a_imm(ch, MI_OP_LOAD_CONST, dst, (int32_t)idx) >= 0;
    }

    case MI_EXPR_BOOL_LITERAL:
    {
      int idx = mi_bytecode_add_const(ch, mi_rt_make_bool(e->as.bool_lit.value));
      if (idx < 0)
      {
        return false;
      }
      return s_emit_a_imm(ch, MI_OP_LOAD_CONST, dst, (int32_t)idx) >= 0;
    }

    case MI_EXPR_VOID_LITERAL:
    {
      int idx = mi_bytecode_add_const(ch, mi_rt_make_void());
      if (idx < 0)
      {
        return false;
      }
      return s_emit_a_imm(ch, MI_OP_LOAD_CONST, dst, (int32_t)idx) >= 0;
    }

    case MI_EXPR_VAR:
    {
      if (!e->as.var.is_indirect)
      {
        MiSymId sym = mi_bytecode_intern_symbol(ch, e->as.var.name, p);
        return s_emit_ab_imm(ch, MI_OP_LOAD_VAR_SYM, dst, 0, (int32_t)sym) >= 0;
      }

      uint8_t name_reg = s_alloc_temp(c);
      if (!s_compile_expr(c, p, ch, e->as.var.name_expr, name_reg))
      {
        return false;
      }

      return s_emit_abc(ch, MI_OP_LOAD_INDIRECT_VAR, dst, name_reg, 0) >= 0;
    }

    case MI_EXPR_INDEX:
    {
      uint8_t target_reg = s_alloc_temp(c);
      uint8_t index_reg  = s_alloc_temp(c);

      if (!s_compile_expr(c, p, ch, e->as.index.target, target_reg))
      {
        return false;
      }

      if (!s_compile_expr(c, p, ch, e->as.index.index, index_reg))
      {
        return false;
      }

      return s_emit_abc(ch, MI_OP_INDEX, dst, target_reg, index_reg) >= 0;
    }

    case MI_EXPR_UNARY:
    {
      uint8_t src = s_alloc_temp(c);
      if (!s_compile_expr(c, p, ch, e->as.unary.expr, src))
      {
        return false;
      }

      if (e->as.unary.op == MI_TOK_MINUS)
      {
        return s_emit_ab(ch, MI_OP_NEG, dst, src) >= 0;
      }

      if (e->as.unary.op == MI_TOK_NOT)
      {
        return s_emit_ab(ch, MI_OP_NOT, dst, src) >= 0;
      }

      mi_error("compile: unsupported unary op\n");
      return false;
    }

    case MI_EXPR_BINARY:
    {
      MiOp op = s_binop_to_op(e->as.binary.op);
      if (op == MI_OP_NOOP)
      {
        mi_error("compile: unsupported binary op\n");
        return false;
      }

      uint8_t l = s_alloc_temp(c);
      uint8_t r = s_alloc_temp(c);

      if (!s_compile_expr(c, p, ch, e->as.binary.left, l))
      {
        return false;
      }

      if (!s_compile_expr(c, p, ch, e->as.binary.right, r))
      {
        return false;
      }

      return s_emit_abc(ch, op, dst, l, r) >= 0;
    }

    case MI_EXPR_LIST:
      return s_compile_list(c, p, ch, e->as.list.items, dst);

    case MI_EXPR_COMMAND:
    {
      /* Evaluate args to values and call.
         For now, we emit ARG_CLEAR + ARG_PUSH* + CALL_CMD/CALL_CMD_DYN.
         r0 is conventional return register; dst can be r0 or temp. */

      if (s_emit0(ch, MI_OP_ARG_CLEAR) < 0)
      {
        return false;
      }

      const MiExprList* it = e->as.command.args;
      while (it)
      {
        if (!s_emit_arg_value(c, p, ch, it->expr))
        {
          return false;
        }
        it = it->next;
      }

      if (e->as.command.head && e->as.command.head->kind == MI_EXPR_STRING_LITERAL)
      {
        MiCmdId cmd = mi_bytecode_intern_cmd(ch, e->as.command.head->as.string_lit.value, p);
        return s_emit_ab_imm(ch, MI_OP_CALL_CMD, dst, (uint8_t)e->as.command.argc, (int32_t)cmd) >= 0;
      }

      uint8_t name_reg = s_alloc_temp(c);
      if (!s_compile_expr(c, p, ch, e->as.command.head, name_reg))
      {
        return false;
      }

      return s_emit_abc(ch, MI_OP_CALL_CMD_DYN, dst, name_reg, (uint8_t)e->as.command.argc) >= 0;
    }

    case MI_EXPR_DICT:
    case MI_EXPR_PAIR:
    case MI_EXPR_BLOCK:
    {
      mi_error("compile: dict/pair/block not implemented\n");
      return false;
    }
  }

  return false;
}

static bool s_compile_command_stmt(MiCompiler* c, MiProgram* p, MiChunk* ch, const MiCommand* cmd)
{
  if (!cmd)
  {
    return true;
  }

  if (s_emit0(ch, MI_OP_ARG_CLEAR) < 0)
  {
    return false;
  }

  const MiExprList* it = cmd->args;
  while (it)
  {
    if (!s_emit_arg_value(c, p, ch, it->expr))
    {
      return false;
    }
    it = it->next;
  }

  if (cmd->head && cmd->head->kind == MI_EXPR_STRING_LITERAL)
  {
    MiCmdId cmd_id = mi_bytecode_intern_cmd(ch, cmd->head->as.string_lit.value, p);
    return s_emit_ab_imm(ch, MI_OP_CALL_CMD, 0, (uint8_t)cmd->argc, (int32_t)cmd_id) >= 0;
  }

  uint8_t name_reg = s_alloc_temp(c);
  if (!s_compile_expr(c, p, ch, cmd->head, name_reg))
  {
    return false;
  }

  return s_emit_abc(ch, MI_OP_CALL_CMD_DYN, 0, name_reg, (uint8_t)cmd->argc) >= 0;
}

//----------------------------------------------------------
// Public API
//----------------------------------------------------------

void mi_compiler_init(MiCompiler* c, size_t arena_chunk_size)
{
  if (!c)
  {
    return;
  }

  memset(c, 0, sizeof(*c));
  c->arena = x_arena_create(arena_chunk_size);
  if (!c->arena)
  {
    mi_error("mi_compile: out of memory\n");
    exit(1);
  }

  c->program_arena_chunk_size = arena_chunk_size;
  c->next_temp_reg = MI_REG_FIRST_TEMP;
}

void mi_compiler_shutdown(MiCompiler* c)
{
  if (!c)
  {
    return;
  }

  if (c->arena)
  {
    x_arena_destroy(c->arena);
  }

  memset(c, 0, sizeof(*c));
}

bool mi_compile_script(MiCompiler* c, XSlice source, MiProgram* out_prog)
{
  if (!c || !out_prog)
  {
    return false;
  }

  x_arena_reset_keep_head(c->arena);

  MiParseResult pr = mi_parse_program(source.ptr, source.length, c->arena);
  if (!pr.ok || !pr.script)
  {
    return false;
  }

  mi_program_init(out_prog, c->program_arena_chunk_size);
  s_reset_temps(c);

  const MiCommandList* it = pr.script->first;
  while (it)
  {
    s_reset_temps(c);

    if (!s_compile_command_stmt(c, out_prog, out_prog->entry, it->command))
    {
      mi_program_shutdown(out_prog);
      return false;
    }
    it = it->next;
  }

  (void)s_emit_a(out_prog->entry, MI_OP_HALT, 0);
  return true;
}
