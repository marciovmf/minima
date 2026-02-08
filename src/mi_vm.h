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

// Debug call stack (blocks + user commands).
#ifndef MI_VM_CALL_STACK_MAX
#define MI_VM_CALL_STACK_MAX 64
#endif

typedef enum MiVmCallFrameKind
{
  MI_VM_CALL_FRAME_BLOCK = 1,
  MI_VM_CALL_FRAME_USER_CMD = 2,
} MiVmCallFrameKind;

typedef struct MiVmCallFrame
{
  MiVmCallFrameKind kind;
  XSlice            name;       // For MI_VM_CALL_FRAME_USER_CMD; otherwise empty.
  const MiVmChunk*  caller_chunk;
  size_t            caller_ip;  // Instruction index in caller chunk (0-based).
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

/**
 * Native function entrypoint used by host-defined commands.
 * Native functions are first-class cmd values and may carry a required type signature.
 */
typedef MiRtValue (*MiVmNativeFn)(MiVm* vm, void* user, int argc, const MiRtValue* argv);
//----------------------------------------------------------
// VM host API table for native modules (.dll/.so)
//----------------------------------------------------------

typedef struct MiVmApi
{
  /* Runtime value constructors */
  MiRtValue (*rt_make_int)(long long v);
  MiRtValue (*rt_make_float)(double v);
  MiRtValue (*rt_make_bool)(bool v);
  MiRtValue (*rt_make_void)(void);

  /* Debug */
  void (*vm_trace_print)(MiVm* vm);

  /* Command/namespace registration */
  bool (*vm_register_native)(
    MiVm* vm,
    XSlice name,
    const MiFuncTypeSig* sig,
    MiVmNativeFn fn,
    void* user,
    XSlice doc
  );

  bool (*vm_namespace_add_native)(
    MiVm* vm,
    MiRtValue ns_block,
    XSlice member_name,
    const MiFuncTypeSig* sig,
    MiVmNativeFn fn,
    void* user,
    XSlice doc
  );


/* Convenience signature builders (variadic C helpers) */
bool (*vm_register_native_sigv)(
  MiVm* vm,
  const char* name_cstr,
  MiVmNativeFn fn,
  void* user,
  XSlice doc,
  MiTypeKind ret_type,
  int param_count,
  ...
);

bool (*vm_register_native_sigv_var)(
  MiVm* vm,
  const char* name_cstr,
  MiVmNativeFn fn,
  void* user,
  XSlice doc,
  MiTypeKind ret_type,
  int fixed_param_count,
  MiTypeKind variadic_type,
  ...
);

bool (*vm_namespace_add_native_sigv)(
  MiVm* vm,
  MiRtValue ns_block,
  const char* member_name_cstr,
  MiVmNativeFn fn,
  void* user,
  XSlice doc,
  MiTypeKind ret_type,
  int param_count,
  ...
);

bool (*vm_namespace_add_native_sigv_var)(
  MiVm* vm,
  MiRtValue ns_block,
  const char* member_name_cstr,
  MiVmNativeFn fn,
  void* user,
  XSlice doc,
  MiTypeKind ret_type,
  int fixed_param_count,
  MiTypeKind variadic_type,
  ...
);

/* Namespace/module values */
bool (*vm_namespace_add_value)(
  MiVm* vm,
  MiRtValue ns_block,
  const char* member_name_cstr,
  MiRtValue value
);
} MiVmApi;
//----------------------------------------------------------
// Convenience registration helpers
//----------------------------------------------------------

/* Register a global native command with a signature built from varargs.
   Pass param_count followed by MiTypeKind params. */
bool mi_vm_register_native_sigv(
  MiVm* vm,
  const char* name_cstr,
  MiVmNativeFn fn,
  void* user,
  XSlice doc,
  MiTypeKind ret_type,
  int param_count,
  ...
);

/* Register a global native command with a variadic signature.
   Pass fixed_param_count followed by MiTypeKind fixed params. */
bool mi_vm_register_native_sigv_var(
  MiVm* vm,
  const char* name_cstr,
  MiVmNativeFn fn,
  void* user,
  XSlice doc,
  MiTypeKind ret_type,
  int fixed_param_count,
  MiTypeKind variadic_type,
  ...
);

/* Register a native member on a namespace block with a signature built from varargs. */
bool mi_vm_namespace_add_native_sigv(
  MiVm* vm,
  MiRtValue ns_block,
  const char* member_name_cstr,
  MiVmNativeFn fn,
  void* user,
  XSlice doc,
  MiTypeKind ret_type,
  int param_count,
  ...
);

/* Register a variadic native member on a namespace block. */
bool mi_vm_namespace_add_native_sigv_var(
  MiVm* vm,
  MiRtValue ns_block,
  const char* member_name_cstr,
  MiVmNativeFn fn,
  void* user,
  XSlice doc,
  MiTypeKind ret_type,
  int fixed_param_count,
  MiTypeKind variadic_type,
  ...
);

/* Define a value (non-callable) inside a namespace block env. */
bool mi_vm_namespace_add_value(
  MiVm* vm,
  MiRtValue ns_block,
  const char* member_name_cstr,
  MiRtValue value
);











/* Print a stack trace (most recent call first) to stdout.
   Intended for stdlib assert_* helpers and host debugging. */
void mi_vm_trace_print(MiVm* vm);

typedef struct MiVmCommandEntry
{
  XSlice         name;
  MiRtValue      value; // MI_RT_VAL_CMD (retained by VM)
} MiVmCommandEntry;

typedef struct MiVmModuleCacheEntry
{
  char*     key;        // resolved mx path (heap string)
  MiRtValue value;      // module handle (block capturing env)
} MiVmModuleCacheEntry;


typedef uint32_t (*MiModuleCountFn)(void);
typedef const char* (*MiModuleNameFn)(uint32_t index);
typedef bool (*MiModuleRegisterFn)(MiVm* vm, const char* module_name, MiRtValue ns_block);

typedef struct MiVmNativeDll
{
  char*              path; /* resolved full path */
  void*              handle;
  MiModuleCountFn    module_count;
  MiModuleNameFn     module_name;
  MiModuleRegisterFn module_register;
} MiVmNativeDll;

typedef struct MiVmUserCommand
{
  XSlice   name;
  uint32_t param_count;
  XSlice*  param_names;
  MiRtValue* defaults;
  MiRtValue body;       // MI_RT_VAL_BLOCK
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

  MI_VM_OP_ITER_NEXT, /* Iteration (cursor-based, no heap iterator objects)
                         - regs[c] is an int cursor (start at -1)
                         - regs[a] receives bool has_next
                         - imm low 8 bits contains dst_item register
                         - For lists: item = list[cursor]
                         - For dicts: item = KVREF (virtual 2-elem view) */
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
  MI_VM_OP_LOAD_MEMBER,       // Qualified member load: a = regs[b].env[$sym[imm]]; regs[b] must be a VM chunk/module block (MI_RT_VAL_BLOCK with env)
  MI_VM_OP_STORE_MEMBER,      // regs[b].env[$sym[imm]] = regs[a]
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
  const MiVmApi* api;

  // VM-owned allocations that live for the lifetime of the VM.
  // Used for module cache keys and other long-lived strings.
  // (This is NOT for runtime values; those are owned by MiRuntime.)
  XArena* perm_arena;

  // Module cache directory for include:.
  // If cache_dir_set is false, include: selects a platform default.
  XFSPath cache_dir;
  bool    cache_dir_set;

  // MI_ROOT/modules directory for native modules (.dll/.so).
  // If modules_dir_set is false, mi_vm_init() tries MI_ROOT or MINIMA_ROOT env vars.
  XFSPath modules_dir;
  bool    modules_dir_set;

  MiVmCommandEntry* commands;
  size_t            command_count;
  size_t            command_capacity;

  // Loaded modules (include:)
  struct MiMixProgram* modules;
  size_t               module_count;
  size_t               module_capacity;

  // Module instance cache (key = resolved mx path).
  MiVmModuleCacheEntry* module_cache;
  size_t                module_cache_count;
  size_t                module_cache_capacity;

  MiVmNativeDll* native_dlls;
  size_t         native_dll_count;
  size_t         native_dll_capacity;

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
  size_t            dbg_ip; // last fetched instruction index (0-based).
  MiVmCallFrame     call_stack[MI_VM_CALL_STACK_MAX];
  int               call_depth;
};

//----------------------------------------------------------
// Public API
//----------------------------------------------------------

/**
 * Initialize a VM instance.
 *
 * This associates the VM with an existing runtime and prepares all
 * internal state required for compilation, execution, and command
 * registration. The VM does not take ownership of the runtime.
 *
 * @param vm Pointer to the VM instance to initialize.
 * @param rt Pointer to an already initialized runtime.
 */
void mi_vm_init(MiVm* vm, MiRuntime* rt);

/**
 * Shut down a VM instance.
 *
 * Releases all VM-owned resources, including command registrations,
 * cached include data, and internal arenas. The associated runtime
 * is not destroyed.
 *
 * @param vm Pointer to the VM instance to shut down.
 */
void mi_vm_shutdown(MiVm* vm);

/**
 * Configure where include: should store compiled .mx cache entries.
 *
 * If path is NULL or empty, include: uses a platform-specific default
 * cache directory. The VM does not take ownership of the string.
 *
 * @param vm   Pointer to the VM instance.
 * @param path Filesystem path for the include cache, or NULL.
 */
void mi_vm_set_cache_dir(MiVm* vm, const char* path);

/* Set MI_ROOT/modules directory for native module loading. */
void mi_vm_set_modules_dir(MiVm* vm, const char* path);

/**
 * Register a native command implemented in C.
 *
 * A native command is registered as a callable runtime value with a
 * required type signature. Variadic commands must be declared via
 * MiFuncTypeSig (e.g. is_variadic = true).
 *
 * The VM retains the created command value for its entire lifetime.
 *
 * @param vm   Pointer to the VM instance.
 * @param name Command name.
 * @param sig  Required function signature metadata.
 * @param fn   Native function entry point.
 * @param user User-defined pointer passed to the native function.
 * @param doc  Optional documentation string (may be empty).
 *
 * @return true on success, false on failure.
 */
bool mi_vm_register_native(
  MiVm* vm,
  XSlice name,
  const MiFuncTypeSig* sig,
  MiVmNativeFn fn,
  void* user,
  XSlice doc
);

/**
 * Find a registered command value by name.
 *
 * On success, out_cmd receives a callable command value.
 *
 * @param vm      Pointer to the VM instance.
 * @param name    Command name.
 * @param out_cmd Output location for the command value.
 *
 * @return true if the command was found, false otherwise.
 */
bool mi_vm_find_command(MiVm* vm, XSlice name, MiRtValue* out_cmd);

/* Lookup a callable signature by name.
   Supports global commands (e.g. "print") and qualified members (e.g. "int::cast"). */
bool mi_vm_find_sig(MiVm* vm, XSlice qualified_name, const MiFuncTypeSig** out_sig);

/* Call a command by name (global registry or qualified a::b::c).
   Convenience wrapper for tooling (e.g. typechecker preloading include statements).
   The returned value is owned by the caller (retain/release as usual). */
MiRtValue mi_vm_call_command(MiVm* vm, XSlice cmd_name, int argc, const MiRtValue* argv);

/* Create (or get) a namespace block with a detached environment, bound to a variable in the root scope.
   Intended for stdlib libraries like int:: and float::. */
MiRtValue mi_vm_namespace_get_or_create(MiVm* vm, XSlice name);

/* Add a native command as a member of a namespace block created by mi_vm_namespace_get_or_create.
   The member name is stored in the namespace environment (not in the global command table). */
bool mi_vm_namespace_add_native(
  MiVm* vm,
  MiRtValue ns_block,
  XSlice member_name,
  const MiFuncTypeSig* sig,
  MiVmNativeFn fn,
  void* user,
  XSlice doc
);

/**
 * Legacy helper used by the compiler fast-path for native-only commands.
 *
 * This returns the raw native function pointer if the command exists
 * and is implemented in C. Commands defined in Minima return NULL.
 *
 * @param vm   Pointer to the VM instance.
 * @param name Command name.
 *
 * @return Native function pointer, or NULL if not found or not native.
 */
MiVmCommandFn mi_vm_find_command_fn(MiVm* vm, XSlice name);

/**
 * Destroy a compiled bytecode chunk.
 *
 * Frees any heap allocations owned by the chunk. Does not affect
 * the VM or runtime.
 *
 * @param chunk Pointer to the chunk to destroy.
 */
void mi_vm_chunk_destroy(MiVmChunk* chunk);

/**
 * Execute a compiled bytecode chunk.
 *
 * The chunk must have been successfully compiled and, if necessary,
 * linked via mi_vm_link_chunk_commands().
 *
 * @param vm    Pointer to the VM instance.
 * @param chunk Pointer to the compiled chunk.
 *
 * @return The value produced by the last executed command.
 */
MiRtValue mi_vm_execute(MiVm* vm, const MiVmChunk* chunk);

/**
 * Link a chunk's command ids to resolved command callables.
 *
 * This function attempts to resolve and cache command targets for
 * each command id used by the chunk.
 *
 * - Unqualified command names must resolve immediately via
 *   mi_vm_find_command(); failure is fatal.
 * - Qualified names (containing ::) are resolved on a best-effort
 *   basis. If their namespace is not available yet, they remain
 *   unresolved and may be lazily resolved and cached on first use.
 *
 * @param vm    Pointer to the VM instance.
 * @param chunk Pointer to the chunk to link.
 *
 * @return false only if an unqualified command cannot be resolved.
 */
bool mi_vm_link_chunk_commands(MiVm* vm, MiVmChunk* chunk);

/**
 * Debug helper: pretty-print bytecode instructions to stdout.
 *
 * This function is intended for diagnostics and debugging only.
 *
 * @param chunk Pointer to the chunk to disassemble.
 */
void mi_vm_disasm(const MiVmChunk* chunk);

#endif  // MI_VM_H
