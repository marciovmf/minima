#include "mi_bytecode.h"
#include "mi_log.h"

#include <stdx_common.h>
#define X_IMPL_ARRAY
#include <stdx_array.h>

#include <stdio.h>
#include <string.h>
#include <stddef.h>

//----------------------------------------------------------
// MiChunk (private)
//----------------------------------------------------------

struct MiChunk
{
  XArray* code;      // MiIns
  XArray* consts;    // MiRtValue
  XArray* symbols;   // XSlice
  XArray* cmd_names; // XSlice
};

//----------------------------------------------------------
// Helpers
//----------------------------------------------------------

static XSlice s_intern_slice(XArena* a, XSlice s)
{
  char* p = x_arena_slicedup(a, s.ptr, s.length, true);
  if (!p)
  {
    return x_slice_init(NULL, 0);
  }
  return x_slice_init(p, s.length);
}

static void s_chunk_init(MiChunk* c)
{
  memset(c, 0, sizeof(*c));
  c->code = x_array_create(sizeof(MiIns), 64);
  c->consts = x_array_create(sizeof(MiRtValue), 64);
  c->symbols = x_array_create(sizeof(XSlice), 64);
  c->cmd_names = x_array_create(sizeof(XSlice), 64);
}

static void s_chunk_shutdown(MiChunk* c)
{
  if (!c)
  {
    return;
  }

  if (c->code)
  {
    x_array_destroy(c->code);
  }

  if (c->consts)
  {
    x_array_destroy(c->consts);
  }

  if (c->symbols)
  {
    x_array_destroy(c->symbols);
  }

  if (c->cmd_names)
  {
    x_array_destroy(c->cmd_names);
  }

  memset(c, 0, sizeof(*c));
}

//----------------------------------------------------------
// Program
//----------------------------------------------------------

void mi_program_init(MiProgram* p, size_t arena_chunk_size)
{
  if (!p)
  {
    return;
  }

  memset(p, 0, sizeof(*p));

  p->arena = x_arena_create(arena_chunk_size);
  if (!p->arena)
  {
    mi_error("mi_bytecode: out of memory\n");
    exit(1);
  }

  p->entry = (MiChunk*)x_arena_alloc(p->arena, sizeof(MiChunk));
  if (!p->entry)
  {
    mi_error("mi_bytecode: out of memory\n");
    exit(1);
  }

  s_chunk_init(p->entry);
}

void mi_program_shutdown(MiProgram* p)
{
  if (!p)
  {
    return;
  }

  if (p->entry)
  {
    s_chunk_shutdown(p->entry);
  }

  if (p->arena)
  {
    x_arena_destroy(p->arena);
  }

  memset(p, 0, sizeof(*p));
}

MiChunk* mi_program_entry(MiProgram* p)
{
  if (!p)
  {
    return NULL;
  }
  return p->entry;
}

//----------------------------------------------------------
// Bytecode API
//----------------------------------------------------------

int mi_bytecode_emit(MiChunk* c, MiIns ins)
{
  if (!c || !c->code)
  {
    return 1;
  }

  x_array_push(c->code, &ins);
  return (int)x_array_count(c->code) - 1;
}

int mi_bytecode_add_const(MiChunk* c, MiRtValue v)
{
  if (!c || !c->consts)
  {
    return -1;
  }

  x_array_push(c->consts, &v);
  return (int)x_array_count(c->consts) - 1;
}

MiSymId mi_bytecode_intern_symbol(MiChunk* c, XSlice name, MiProgram* p)
{
  if (!c || !c->symbols || !p || !p->arena)
  {
    return 0;
  }

  uint32_t n = x_array_count(c->symbols);
  XSlice* data = (XSlice*)x_array_getdata(c->symbols).ptr;

  for (uint32_t i = 0; i < n; ++i)
  {
    if (x_slice_eq(data[i], name))
    {
      return (MiSymId)i;
    }
  }

  XSlice owned = s_intern_slice(p->arena, name);
  x_array_push(c->symbols, &owned);
  return (MiSymId)x_array_count(c->symbols) - 1;
}

MiCmdId mi_bytecode_intern_cmd(MiChunk* c, XSlice name, MiProgram* p)
{
  if (!c || !c->cmd_names || !p || !p->arena)
  {
    return 0;
  }

  uint32_t n = x_array_count(c->cmd_names);
  XSlice* data = (XSlice*)x_array_getdata(c->cmd_names).ptr;

  for (uint32_t i = 0; i < n; ++i)
  {
    if (x_slice_eq(data[i], name))
    {
      return (MiCmdId)i;
    }
  }

  XSlice owned = s_intern_slice(p->arena, name);
  x_array_push(c->cmd_names, &owned);
  return (MiCmdId)x_array_count(c->cmd_names) - 1;
}

//----------------------------------------------------------
// Disassembler
//----------------------------------------------------------

#if defined(MI_ENABLE_LEGACY_BYTECODE_DISASM)

static const char* s_op_name(uint8_t op)
{
  switch ((MiOp)op)
  {
    case MI_OP_NOOP: return "NOOP";
    case MI_OP_LOAD_CONST: return "LOAD_CONST";
    case MI_OP_MOV: return "MOV";
    case MI_OP_NEG: return "NEG";
    case MI_OP_NOT: return "NOT";
    case MI_OP_ADD: return "ADD";
    case MI_OP_SUB: return "SUB";
    case MI_OP_MUL: return "MUL";
    case MI_OP_DIV: return "DIV";
    case MI_OP_MOD: return "MOD";
    case MI_OP_EQ: return "EQ";
    case MI_OP_NEQ: return "NEQ";
    case MI_OP_LT: return "LT";
    case MI_OP_LTEQ: return "LTEQ";
    case MI_OP_GT: return "GT";
    case MI_OP_GTEQ: return "GTEQ";
    case MI_OP_AND: return "AND";
    case MI_OP_OR: return "OR";
    case MI_OP_LOAD_VAR_SYM: return "LOAD_VAR_SYM";
    case MI_OP_STORE_VAR_SYM: return "STORE_VAR_SYM";
    case MI_OP_LOAD_INDIRECT_VAR: return "LOAD_INDIRECT_VAR";
    case MI_OP_INDEX: return "INDEX";
    case MI_OP_ARG_CLEAR: return "ARG_CLEAR";
    case MI_OP_ARG_PUSH: return "ARG_PUSH";
    case MI_OP_ARG_PUSH_CONST: return "ARG_PUSH_CONST";
    case MI_OP_ARG_PUSH_VAR_SYM: return "ARG_PUSH_VAR_SYM";
    case MI_OP_CALL_CMD: return "CALL_CMD";
    case MI_OP_CALL_CMD_DYN: return "CALL_CMD_DYN";
    case MI_OP_JUMP: return "JUMP";
    case MI_OP_JUMP_IF_TRUE: return "JUMP_IF_TRUE";
    case MI_OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
    case MI_OP_RETURN: return "RETURN";
    case MI_OP_HALT: return "HALT";
    default: return "<UNKNOWN>";
  }
}

static void s_hex_bytes(FILE* out, const void* ptr, size_t size)
{
  const uint8_t* b = (const uint8_t*)ptr;
  for (size_t i = 0; i < size; ++i)
  {
    fprintf(out, "%02X", (unsigned)b[i]);
    if (i + 1 < size)
    {
      fputc(' ', out);
    }
  }
}

static void s_append_slice(char* dst, size_t dst_cap, int* io_n, XSlice s)
{
  int n = *io_n;
  if ((size_t)n >= dst_cap)
  {
    return;
  }

  int room = (int)(dst_cap - (size_t)n);
  int want = (int)s.length;
  if (want < 0) want = 0;
  if (want >= room) want = room - 1;
  if (want > 0 && s.ptr)
  {
    memcpy(dst + n, s.ptr, (size_t)want);
    n += want;
    dst[n] = 0;
  }
  *io_n = n;
}

static void s_append_sym(char* dst, size_t dst_cap, int* io_n, const MiChunk* c, uint32_t sym_id)
{
  if (!c || !c->symbols)
  {
    *io_n += snprintf(dst + *io_n, dst_cap - (size_t)*io_n, "sym_%u", sym_id);
    return;
  }

  uint32_t nsyms = x_array_count(c->symbols);
  XSlice* data = (XSlice*)x_array_getdata(c->symbols).ptr;

  if (sym_id >= nsyms)
  {
    *io_n += snprintf(dst + *io_n, dst_cap - (size_t)*io_n, "sym[%u](<oob>)", sym_id);
    return;
  }

  XSlice s = data[sym_id];
  *io_n += snprintf(dst + *io_n, dst_cap - (size_t)*io_n, "sym_%u # ", sym_id);
  s_append_slice(dst, dst_cap, io_n, s);
  *io_n += snprintf(dst + *io_n, dst_cap - (size_t)*io_n, "");
}

static void s_append_cmd(char* dst, size_t dst_cap, int* io_n, const MiChunk* c, uint32_t cmd_id)
{
  if (!c || !c->cmd_names)
  {
    *io_n += snprintf(dst + *io_n, dst_cap - (size_t)*io_n, "cmd_%u", cmd_id);
    return;
  }

  uint32_t ncmd = x_array_count(c->cmd_names);
  XSlice* data = (XSlice*)x_array_getdata(c->cmd_names).ptr;

  if (cmd_id >= ncmd)
  {
    *io_n += snprintf(dst + *io_n, dst_cap - (size_t)*io_n, "cmd[%u](<oob>)", cmd_id);
    return;
  }

  XSlice s = data[cmd_id];
  *io_n += snprintf(dst + *io_n, dst_cap - (size_t)*io_n, "cmd_%u # ", cmd_id);
  s_append_slice(dst, dst_cap, io_n, s);
  //*io_n += snprintf(dst + *io_n, dst_cap - (size_t)*io_n, "\"");
}

static void s_disasm_chunk(FILE* out, const MiChunk* c)
{
  if (!out)
  {
    return;
  }

  if (!c || !c->code)
  {
    fprintf(out, "<empty chunk>\n");
    return;
  }

  uint32_t count = x_array_count(c->code);
  MiIns* code = (MiIns*)x_array_getdata(c->code).ptr;

  enum
  {
    MI_DISASM_ASCII_COL_WIDTH = 56
  };

  for (uint32_t ip = 0; ip < count; ++ip)
  {
    MiIns ins = code[ip];
    uint32_t offset = ip * (uint32_t)sizeof(MiIns);

    char ascii[256];
    int n = 0;
    ascii[0] = 0;

    n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, "%s", s_op_name(ins.op));

    switch ((MiOp)ins.op)
    {
      case MI_OP_LOAD_CONST:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " r%u, const_%d", (unsigned)ins.a, (int)ins.imm);
        break;

      case MI_OP_MOV:
      case MI_OP_NEG:
      case MI_OP_NOT:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " r%u, r%u", (unsigned)ins.a, (unsigned)ins.b);
        break;

      case MI_OP_ADD:
      case MI_OP_SUB:
      case MI_OP_MUL:
      case MI_OP_DIV:
      case MI_OP_MOD:
      case MI_OP_EQ:
      case MI_OP_NEQ:
      case MI_OP_LT:
      case MI_OP_LTEQ:
      case MI_OP_GT:
      case MI_OP_GTEQ:
      case MI_OP_AND:
      case MI_OP_OR:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " r%u, r%u, r%u", (unsigned)ins.a, (unsigned)ins.b, (unsigned)ins.c);
        break;

      case MI_OP_LOAD_VAR_SYM:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " r%u, ", (unsigned)ins.a);
        s_append_sym(ascii, sizeof(ascii), &n, c, (uint32_t)ins.imm);
        break;

      case MI_OP_STORE_VAR_SYM:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " ");
        s_append_sym(ascii, sizeof(ascii), &n, c, (uint32_t)ins.imm);
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, ", r%u", (unsigned)ins.a);
        break;

      case MI_OP_LOAD_INDIRECT_VAR:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " r%u, r%u", (unsigned)ins.a, (unsigned)ins.b);
        break;

      case MI_OP_INDEX:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " r%u, r%u, r%u", (unsigned)ins.a, (unsigned)ins.b, (unsigned)ins.c);
        break;

      case MI_OP_ARG_CLEAR:
        break;

      case MI_OP_ARG_PUSH:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " r%u", (unsigned)ins.a);
        break;

      case MI_OP_ARG_PUSH_CONST:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " const_%d", (int)ins.imm);
        break;

      case MI_OP_ARG_PUSH_VAR_SYM:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " ");
        s_append_sym(ascii, sizeof(ascii), &n, c, (uint32_t)ins.imm);
        break;

      case MI_OP_CALL_CMD:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " r%u, %u, ", (unsigned)ins.a, (unsigned)ins.b);
        s_append_cmd(ascii, sizeof(ascii), &n, c, (uint32_t)ins.imm);
        break;

      case MI_OP_CALL_CMD_DYN:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " r%u, name=r%u, argc=%u", (unsigned)ins.a, (unsigned)ins.b, (unsigned)ins.c);
        break;

      case MI_OP_JUMP:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " %d", (int)ins.imm);
        break;

      case MI_OP_JUMP_IF_TRUE:
      case MI_OP_JUMP_IF_FALSE:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " r%u, %d", (unsigned)ins.a, (int)ins.imm);
        break;

      case MI_OP_RETURN:
        n += snprintf(ascii + n, sizeof(ascii) - (size_t)n, " r%u", (unsigned)ins.a);
        break;

      case MI_OP_HALT:
      case MI_OP_NOOP:
      default:
        break;
    }

    fprintf(out, "0x%08X  %-*.*s  ",
        offset,
        MI_DISASM_ASCII_COL_WIDTH,
        MI_DISASM_ASCII_COL_WIDTH,
        ascii);

    s_hex_bytes(out, &ins, sizeof(ins));
    fputc('\n', out);
  }
}

void mi_disasm_program(FILE* out, const MiProgram* p)
{
  if (!out)
  {
    return;
  }

  if (!p || !p->entry)
  {
    fprintf(out, "<null program>\n");
    return;
  }

  const MiChunk* c = (const MiChunk*)p->entry;

  fprintf(out, ".sym (%u)\n", c->symbols ? x_array_count(c->symbols) : 0);
  if (c->symbols)
  {
    uint32_t n = x_array_count(c->symbols);
    XSlice* data = (XSlice*)x_array_getdata(c->symbols).ptr;
    for (uint32_t i = 0; i < n; ++i)
    {
      XSlice s = data[i];
      fprintf(out, " sym_%u = \"%.*s\"\n", i, (int)s.length, s.ptr ? s.ptr : "");
    }
  }

  fprintf(out, "\n.cmd (%u)\n", c->cmd_names ? x_array_count(c->cmd_names) : 0);
  if (c->cmd_names)
  {
    uint32_t n = x_array_count(c->cmd_names);
    XSlice* data = (XSlice*)x_array_getdata(c->cmd_names).ptr;
    for (uint32_t i = 0; i < n; ++i)
    {
      XSlice s = data[i];
      fprintf(out, " cmd_%u = \"%.*s\"\n", i, (int)s.length, s.ptr ? s.ptr : "");
    }
  }

  fprintf(out, "\n.const (%u)\n", c->consts ? x_array_count(c->consts) : 0);
  if (c->consts)
  {
    uint32_t n = x_array_count(c->consts);
    MiRtValue* values = (MiRtValue*)x_array_getdata(c->consts).ptr;

    for (uint32_t i = 0; i < n; ++i)
    {
      MiRtValue* v = &values[i];
      switch(v->kind)
      {
        case MI_RT_VAL_VOID: 
          fprintf(out, " const_%u = (void)\n", i);
          break;
        case MI_RT_VAL_INT:
          fprintf(out, " const_%u = (int) %lld\n", i, v->as.i);
          break;
        case MI_RT_VAL_FLOAT:
          fprintf(out, " const_%u = (float) %f\n", i, v->as.f);
          break;
        case MI_RT_VAL_BOOL:
          fprintf(out, " const_%u = (bool) \"%s\"\n", i, v->as.b ? "true" : "false");
          break;
        case MI_RT_VAL_STRING:
          fprintf(out, " const_%u = (str) \"%.*s\"\n", i, (u32) v->as.s.length, v->as.s.ptr);
          break;
        case MI_RT_VAL_LIST:
          fprintf(out, " const_%u = (list) <list-0x%p>\n", i, v->as.list);
          break;
        case MI_RT_VAL_BLOCK:
          fprintf(out, " const_%u = (list) <block-0x%p>\n", i, v->as.block);
          break;
        case MI_RT_VAL_PAIR:
          fprintf(out, " const_%u = (list) <pair-0x%p>\n", i, v->as.pair);
          break;
        default:
          fprintf(out, " const_%u = (?) <?>\n", i);
          break;
      }
    }
  }

  fprintf(out, "\n.code\n");
  s_disasm_chunk(out, c);
}

#else

void mi_disasm_program(FILE* out, const MiProgram* p)
{
  (void) p;
  if (!out)
  {
    return;
  }
  fprintf(out, "mi_disasm_program: legacy bytecode disassembler disabled.\n");
  fprintf(out, "Use mi_vm_disasm (mi_vm.h/c) for the canonical VM disassembly.\n");
}

#endif
