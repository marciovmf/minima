#ifndef MI_VM_H
#define MI_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdx_common.h>
#include <stdx_string.h>
#include <stdx_arena.h>
#include <stdx_filesystem.h>

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


/**
 * VM command entrypoint.
 * - name is the invoked command name (useful for user-defined commands).
 * - argv does NOT include the command name; it's only the arguments.
 */
typedef MiRtValue (*MiVmCommandFn)(MiVm* vm, XSlice name, int argc, const MiRtValue* argv);

/* Native function entrypoint used by host-defined commands.
   Native functions are first-class cmd values and may carry a required type signature. */
typedef MiRtValue (*MiVmNativeFn)(MiVm* vm, void* user, int argc, const MiRtValue* argv);

typedef struct MiVmCommandEntry
{
  XSlice         name;
  MiRtValue      value; /* MI_RT_VAL_CMD (retained by VM) */
} MiVmCommandEntry;

typedef struct MiVmModuleCacheEntry
{
  char*     key;   /* resolved mx path (heap string) */
  MiRtValue value; /* module handle (block capturing env) */
} MiVmModuleCacheEntry;

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
  /* Qualified member load: a = regs[b].env[$sym[imm]]
     regs[b] must be a VM chunk/module block (MI_RT_VAL_BLOCK with env). */
  MI_VM_OP_LOAD_MEMBER,
  MI_VM_OP_STORE_MEMBER,       // regs[b].env[$sym[imm]] = regs[a]
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

  /* Fast command call: invoke cached chunk->cmd_targets[imm] (cmd value).
     No qualified-name resolution, and no scoped shadowing lookup.
     Compiler should emit this only for known, unqualified command heads. */
  MI_VM_OP_CALL_CMD_FAST,
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
  uint32_t*  symbol_ids;      // runtime-global symbol ids (lazy-interned; UINT32_MAX = unresolved)
  size_t     symbol_count;
  size_t     symbol_capacity;

  MiRtCmd**      cmd_targets; // resolved command callables (cached)
  XSlice*        cmd_names;   // optional: for debug
  size_t         cmd_count;
  size_t         cmd_capacity;

  MiVmChunk**    subchunks;      // block literal payloads
  size_t         subchunk_count;
  size_t         subchunk_capacity;


  // Debug source mapping (optional; may be NULL for chunks loaded without debug info)
  XSlice     dbg_name;        // e.g. function name, "<script>", "<block>"
  XSlice     dbg_file;        // e.g. filename or module name
  uint32_t*  dbg_lines;       // per-instruction line number (1-based); 0 if unknown
  uint32_t*  dbg_cols;        // per-instruction column (1-based); 0 if unknown
  size_t     dbg_capacity;    // capacity of dbg_* arrays (tracks code_capacity)
};
struct MiVm
{
  MiRuntime* rt;

  /* VM-owned allocations that live for the lifetime of the VM.
     Used for module cache keys and other long-lived strings.
     (This is NOT for runtime values; those are owned by MiRuntime.) */
  XArena* perm_arena;

  /* Module cache directory for include:.
     If cache_dir_set is false, include: selects a platform default.
     */
  XFSPath cache_dir;
  bool    cache_dir_set;

  MiVmCommandEntry* commands;
  size_t            command_count;
  size_t            command_capacity;

  /* Loaded modules (include:). */
  struct MiMixProgram* modules;
  size_t               module_count;
  size_t               module_capacity;

  /* Module instance cache (key = resolved mx path). */
  MiVmModuleCacheEntry* module_cache;
  size_t                module_cache_count;
  size_t                module_cache_capacity;

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

  // Current call context (for argc()/arg()/arg_type()/arg_name()).
  int                cur_argc;
  const MiRtValue*    cur_argv;
  const MiRtCmd*      cur_cmd;
  XSlice              cur_cmd_name;

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

/**
 * Configure where include: should store compiled .mx cache entries.
 * If path is NULL or empty, include: uses a platform default.
 */
void      mi_vm_set_cache_dir(MiVm* vm, const char* path);

/* Register a command implemented in C as a first-class cmd value.
   This is the ONLY command registration mechanism: commands are values.
   The VM retains the created cmd value for its lifetime.
*/
/* Register a native command implemented in C.
   sig is required for all commands. Variadics can be expressed via MiFuncTypeSig.
   doc may be empty.
   The VM retains the created cmd value for its lifetime.
*/
bool      mi_vm_register_native(MiVm* vm, XSlice name, const MiFuncTypeSig* sig, MiVmNativeFn fn, void* user, XSlice doc);

/* Deprecated: legacy registration without signature/userdata.
   Prefer mi_vm_register_native. */
bool      mi_vm_register_command(MiVm* vm, XSlice name, MiVmCommandFn fn);

/* Find a registered command value by name. Returns false if not found. */
bool      mi_vm_find_command(MiVm* vm, XSlice name, MiRtValue* out_cmd);

/* Legacy helper used by compiler fast-path linking (native-only). */
MiVmCommandFn mi_vm_find_command_fn(MiVm* vm, XSlice name);


/* Compile a script to bytecode chunk using the provided arena for allocations. */
/* Destroy a compiled chunk (frees heap allocations). */
void      mi_vm_chunk_destroy(MiVmChunk* chunk);

/**
 * Execute a compiled chunk. Returns last command value.
 */
MiRtValue mi_vm_execute(MiVm* vm, const MiVmChunk* chunk);

/* Link a chunk's command ids to resolved command callables.
   This fills chunk->cmd_targets[i] when possible.

   Unqualified names must resolve via mi_vm_find_command().
   Qualified names (with ::) are best-effort: if their namespace is not
   available yet, they remain unresolved and can be lazily resolved and cached
   at first call.

   Returns false only if an unqualified command cannot be resolved. */
bool mi_vm_link_chunk_commands(MiVm* vm, MiVmChunk* chunk);

/**
 * Debug: pretty-print bytecode to stdout.
 */
void      mi_vm_disasm(const MiVmChunk* chunk);

#endif // MI_VM_H
