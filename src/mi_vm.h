#ifndef MI_VM_H
#define MI_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdx_common.h>
#include <stdx_string.h>
#include <stdx_arena.h>

#include "mi_parse.h"
#include "mi_runtime.h"

#define MI_VM_REG_COUNT 32
#define MI_VM_ARG_STACK_COUNT 256

//----------------------------------------------------------
// Minima VM
//----------------------------------------------------------

typedef struct MiVm MiVm;
typedef struct MiVmChunk MiVmChunk;

typedef MiRtValue (*MiVmCommandFn)(MiVm* vm, int argc, const MiRtValue* argv);

typedef struct MiVmCommandEntry
{
  XSlice         name;
  MiVmCommandFn  fn;
} MiVmCommandEntry;

typedef enum MiVmOp
{
  MI_VM_OP_NOOP = 0,
  // Constants / moves
  MI_VM_OP_LOAD_CONST,  // a = const[imm]
  MI_VM_OP_LOAD_BLOCK,  // a = new block from subchunk[imm] (captures env)
  MI_VM_OP_MOV,         // a = b
                        // Unary
  MI_VM_OP_NEG,
  MI_VM_OP_NOT,
  // Binary
  MI_VM_OP_ADD,
  MI_VM_OP_SUB,
  MI_VM_OP_MUL,
  MI_VM_OP_DIV,
  MI_VM_OP_MOD,
  MI_VM_OP_EQ,
  MI_VM_OP_NEQ,
  MI_VM_OP_LT,
  MI_VM_OP_LTEQ,
  MI_VM_OP_GT,
  MI_VM_OP_GTEQ,
  MI_VM_OP_AND,
  MI_VM_OP_OR,
  // Vars
  MI_VM_OP_LOAD_VAR,          // a = $sym[imm]
  MI_VM_OP_STORE_VAR,         // $sym[imm] = a
  MI_VM_OP_LOAD_INDIRECT_VAR, // a = $( regs[b] )
                              // Args + command calls
  MI_VM_OP_ARG_CLEAR,         // clear arg stack
  MI_VM_OP_ARG_PUSH,          // push regs[a]
  MI_VM_OP_ARG_PUSH_CONST,    // push const[imm]
  MI_VM_OP_ARG_PUSH_VAR_SYM,  // push $sym[imm]
  MI_VM_OP_ARG_PUSH_SYM,
  MI_VM_OP_CALL_CMD,          // a = call cmd_fn[imm] with argc=b
  MI_VM_OP_CALL_CMD_DYN,      // a = call by name in regs[b], argc=c

  MI_VM_OP_CALL_BLOCK,        // a = call regs[b] (block) with argc=c
  // Control
  MI_VM_OP_JUMP,
  MI_VM_OP_JUMP_IF_TRUE,
  MI_VM_OP_JUMP_IF_FALSE,
  MI_VM_OP_RETURN,
  MI_VM_OP_HALT,
} MiVmOp;

typedef struct MiVmIns
{
  uint8_t  op;
  uint8_t  a;
  uint8_t  b;
  uint8_t  c;
  int32_t  imm;
} MiVmIns;

struct MiVmChunk
{
  MiVmIns*   code;
  size_t     code_count;
  size_t     code_capacity;

  MiRtValue* consts;
  size_t     const_count;
  size_t     const_capacity;

  XSlice*    symbols;         // variable names
  size_t     symbol_count;
  size_t     symbol_capacity;

  MiVmCommandFn* cmd_fns;     // resolved command functions
  XSlice*        cmd_names;   // optional: for debug
  size_t         cmd_count;
  size_t         cmd_capacity;

  MiVmChunk**    subchunks;      // block literal payloads
  size_t         subchunk_count;
  size_t         subchunk_capacity;
};

struct MiVm
{
  MiRuntime* rt;

  MiVmCommandEntry* commands;
  size_t            command_count;
  size_t            command_capacity;

  // Working state (execution).
  MiRtValue regs[MI_VM_REG_COUNT];
  MiRtValue arg_stack[MI_VM_ARG_STACK_COUNT];
  int       arg_top;
};

//----------------------------------------------------------
// Public API
//----------------------------------------------------------

void      mi_vm_init(MiVm* vm, MiRuntime* rt);
void      mi_vm_shutdown(MiVm* vm);

bool      mi_vm_register_command(MiVm* vm, XSlice name, MiVmCommandFn fn);

/* Register a VM command (this is separate from mi_rt_register_command). */
bool      mi_vm_register_command(MiVm* vm, XSlice name, MiVmCommandFn fn);

/* Compile a script to bytecode chunk using the provided arena for allocations. */
MiVmChunk* mi_vm_compile_script(MiVm* vm, XSlice source);
/* Destroy a chunk created by mi_vm_compile_script (frees heap allocations). */
void      mi_vm_chunk_destroy(MiVmChunk* chunk);

/* Execute a compiled chunk. Returns last command value. */
MiRtValue mi_vm_execute(MiVm* vm, const MiVmChunk* chunk);

/* Debug: pretty-print bytecode to stdout. */
void      mi_vm_disasm(const MiVmChunk* chunk);

#endif // MI_VM_H
