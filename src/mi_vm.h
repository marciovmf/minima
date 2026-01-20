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

typedef struct MiVm MiVm;
typedef struct MiVmChunk MiVmChunk;

#define MI_VM_REG_COUNT 32
#define MI_VM_ARG_STACK_COUNT 256
#define MI_VM_ARG_FRAME_MAX 16

/* Debug call stack (blocks + user commands). */
#define MI_VM_CALL_STACK_MAX 64

typedef enum MiVmCallFrameKind
{
  MI_VM_CALL_FRAME_BLOCK = 1,
  MI_VM_CALL_FRAME_USER_CMD = 2,
} MiVmCallFrameKind;

typedef struct MiVmCallFrame
{
  MiVmCallFrameKind kind;
  XSlice            name; /* For MI_VM_CALL_FRAME_USER_CMD; otherwise empty. */
  const MiVmChunk*  caller_chunk;
  size_t            caller_ip; /* Instruction index in caller chunk (0-based). */
} MiVmCallFrame;

//----------------------------------------------------------
// Minima VM
//----------------------------------------------------------


/* VM command entrypoint.
   - name is the invoked command name (useful for user-defined commands).
   - argv does NOT include the command name; it's only the arguments.
*/
typedef MiRtValue (*MiVmCommandFn)(MiVm* vm, XSlice name, int argc, const MiRtValue* argv);

typedef struct MiVmCommandEntry
{
  XSlice         name;
  MiVmCommandFn  fn;
} MiVmCommandEntry;

typedef struct MiVmUserCommand
{
  XSlice   name;
  uint32_t param_count;
  XSlice*  param_names;
  MiRtValue* defaults;
  MiRtValue body; /* MI_RT_VAL_BLOCK */
} MiVmUserCommand;

typedef enum MiVmOp
{
  MI_VM_OP_NOOP = 0,
  // Constants / moves
  MI_VM_OP_LOAD_CONST,  // a = const[imm]
  MI_VM_OP_LOAD_BLOCK,  // a = new block from subchunk[imm] (captures env)
  MI_VM_OP_MOV,         // a = b
  // Lists
  MI_VM_OP_LIST_NEW,    // a = new list
  MI_VM_OP_LIST_PUSH,   // regs[a].list push regs[b]
  // Dicts
  MI_VM_OP_DICT_NEW,    // a = new dict

  /* Iteration (cursor-based, no heap iterator objects)
     - regs[c] is an int cursor (start at -1)
     - regs[a] receives bool has_next
     - imm low 8 bits contains dst_item register
     - For lists: item = list[cursor]
     - For dicts: item = KVREF (virtual 2-elem view)
  */
  MI_VM_OP_ITER_NEXT,
  // Indexing
  MI_VM_OP_INDEX,       // a = regs[b][regs[c]]
  MI_VM_OP_STORE_INDEX, // regs[a][regs[b]] = regs[c]
  MI_VM_OP_LEN,         // a = len(regs[b])
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
  MI_VM_OP_DEFINE_VAR,        // define $sym[imm] = a in current scope only
  MI_VM_OP_LOAD_INDIRECT_VAR, // a = $( regs[b] )
                              // Args + command calls
  MI_VM_OP_ARG_CLEAR,         // clear arg stack
  MI_VM_OP_ARG_PUSH,          // push regs[a]
  MI_VM_OP_ARG_PUSH_CONST,    // push const[imm]
  MI_VM_OP_ARG_PUSH_VAR_SYM,  // push $sym[imm]
  MI_VM_OP_ARG_PUSH_SYM,
  MI_VM_OP_ARG_SAVE,          // save arg stack to a VM-side frame stack
  MI_VM_OP_ARG_RESTORE,       // restore arg stack from a VM-side frame stack
  MI_VM_OP_CALL_CMD,          // a = call cmd_fn[imm] with argc=b
  MI_VM_OP_CALL_CMD_DYN,      // a = call by name in regs[b], argc=c

  MI_VM_OP_CALL_BLOCK,        // a = call regs[b] (block) with argc=c
  // Scopes (VM-only; used by compiler for inlined control flow)
  MI_VM_OP_SCOPE_PUSH,        // push a new scope frame (parent = current)
  MI_VM_OP_SCOPE_POP,         // pop current scope frame
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

  /* Loaded modules (include:). */
  struct MiMixProgram* modules;
  size_t               module_count;
  size_t               module_capacity;

  MiScopeFrame**       module_envs;
  size_t               module_env_count;
  size_t               module_env_capacity;

  // Working state (execution).
  MiRtValue regs[MI_VM_REG_COUNT];
  MiRtValue arg_stack[MI_VM_ARG_STACK_COUNT];
  int       arg_top;

  // Arg stack save/restore for nested command expressions.
  MiRtValue arg_frames[MI_VM_ARG_FRAME_MAX][MI_VM_ARG_STACK_COUNT];
  int       arg_frame_tops[MI_VM_ARG_FRAME_MAX];
  int       arg_frame_depth;

  // Debug: track current instruction and call stack for trace:.
  const MiVmChunk*  dbg_chunk;
  size_t            dbg_ip; /* last fetched instruction index (0-based). */
  MiVmCallFrame     call_stack[MI_VM_CALL_STACK_MAX];
  int               call_depth;
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
