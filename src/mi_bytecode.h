#ifndef MI_BYTECODE_H
#define MI_BYTECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>

#include <stdx_arena.h>
#include <stdx_common.h>
#include <stdx_string.h>

#include "mi_runtime.h"

//----------------------------------------------------------
// Bytecode
//----------------------------------------------------------

typedef uint32_t MiSymId;
typedef uint32_t MiCmdId;

typedef enum MiOp
{
  MI_OP_NOOP = 0,

  // Constants / moves
  MI_OP_LOAD_CONST,       // a = const[imm]
  MI_OP_MOV,              // a = b

  // Unary
  MI_OP_NEG,
  MI_OP_NOT,

  // Binary
  MI_OP_ADD,
  MI_OP_SUB,
  MI_OP_MUL,
  MI_OP_DIV,
  MI_OP_MOD,
  MI_OP_EQ,
  MI_OP_NEQ,
  MI_OP_LT,
  MI_OP_LTEQ,
  MI_OP_GT,
  MI_OP_GTEQ,
  MI_OP_AND,
  MI_OP_OR,

  // Data access
  MI_OP_LOAD_VAR_SYM,         // a = $sym[imm]
  MI_OP_STORE_VAR_SYM,        // $sym[imm] = a
  MI_OP_LOAD_INDIRECT_VAR,    // a = $( regs[b] )
  MI_OP_INDEX,                // a = regs[b][regs[c]]

  // Command arguments + calls
  MI_OP_ARG_CLEAR,            // clear arg stack
  MI_OP_ARG_PUSH,             // push regs[a]
  MI_OP_ARG_PUSH_CONST,       // push const[imm]
  MI_OP_ARG_PUSH_VAR_SYM,     // push $sym[imm]
  MI_OP_CALL_CMD,             // a = call cmd_fn[imm] with argc=b
  MI_OP_CALL_CMD_DYN,         // a = call by name regs[b] with argc=c

  // Control flow
  MI_OP_JUMP,                 // pc += imm
  MI_OP_JUMP_IF_TRUE,         // if regs[a] pc += imm
  MI_OP_JUMP_IF_FALSE,        // if !regs[a] pc += imm
  MI_OP_RETURN,               // return regs[a]
  MI_OP_HALT,
} MiOp;

typedef struct MiIns
{
  uint8_t op;
  uint8_t a;
  uint8_t b;
  uint8_t c;
  int32_t imm;
} MiIns;

typedef struct MiProgram
{
  XArena* arena;
  struct MiChunk* entry;
} MiProgram;

typedef struct MiChunk MiChunk;

/**
 * Emit a single instruction into the chunk.
 * @param c   Target chunk.
 * @param ins Instruction to emit.
 * @return    Instruction index.
 */
int mi_bytecode_emit(MiChunk* c, MiIns ins);

/**
 * Add a constant value to the chunk constant pool.
 * @param c Chunk.
 * @param v Constant value.
 * @return  Constant index.
 */
int mi_bytecode_add_const(MiChunk* c, MiRtValue v);

/**
 * Intern a symbol name used for variable access.
 * @param c    Chunk.
 * @param name Symbol name.
 * @param p    Program owning the intern arena.
 * @return     Symbol identifier.
 */
MiSymId mi_bytecode_intern_symbol(MiChunk* c, XSlice name, MiProgram* p);

/**
 * Intern a command name for fast command dispatch.
 * @param c    Chunk.
 * @param name Command name.
 * @param p    Program owning the intern arena.
 * @return     Command identifier.
 */
MiCmdId mi_bytecode_intern_cmd(MiChunk* c, XSlice name, MiProgram* p);

/**
 * Disassemble a compiled program.
 * @param out Output stream.
 * @param p   Program to disassemble.
 */
/* Legacy bytecode disassembler.
   The canonical disassembler lives in mi_vm.c (mi_vm_disasm).
   Define MI_ENABLE_LEGACY_BYTECODE_DISASM to keep the old program dump. */
void mi_disasm_program(FILE* out, const MiProgram* p);

#endif
