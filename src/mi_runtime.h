#ifndef MI_RUNTIME_H
#define MI_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdx_common.h>
#include <stdx_arena.h>
#include <stdx_string.h>

#include "mi_heap.h"

//----------------------------------------------------------
// Runtime Values
//----------------------------------------------------------

typedef struct MiRuntime MiRuntime;
typedef struct MiRtValue MiRtValue;
typedef struct MiRtList MiRtList;
typedef struct MiRtPair MiRtPair;
typedef struct MiRtDict MiRtDict;
typedef struct MiRtBlock MiRtBlock;
typedef struct MiRtCmd MiRtCmd;
typedef struct MiScopeFrame MiScopeFrame;

/* Forward decl to avoid circular include with mi_vm.h. */
struct MiVm;

typedef struct MiRtKvRef
{
  MiRtDict* dict;
  size_t    entry_index;
} MiRtKvRef;

typedef enum MiRtValueKind
{
  MI_RT_VAL_VOID = 0,
  MI_RT_VAL_INT,
  MI_RT_VAL_FLOAT,
  MI_RT_VAL_BOOL,
  MI_RT_VAL_STRING,
  MI_RT_VAL_LIST,
  MI_RT_VAL_DICT,
  MI_RT_VAL_KVREF,
  MI_RT_VAL_BLOCK,
  MI_RT_VAL_CMD,
  MI_RT_VAL_PAIR,
  MI_RT_VAL_TYPE
} MiRtValueKind;

struct MiRtValue
{
  MiRtValueKind kind;

  union
  {
    long long  i;
    double     f;
    bool       b;
    XSlice     s;
    MiRtPair*  pair;
    MiRtList*  list;
    MiRtDict*  dict;
    MiRtKvRef   kvref;
    MiRtBlock* block;
    MiRtCmd*   cmd;
  } as;
};

/* Native (C) command signature used by first-class command values. */
typedef MiRtValue (*MiRtNativeFn)(struct MiVm* vm, XSlice cmd_name, int argc, const MiRtValue* argv);

/* Native (C) command signature with userdata (preferred for host APIs). */
typedef MiRtValue (*MiRtNativeFn2)(struct MiVm* vm, void* user, int argc, const MiRtValue* argv);

/* Forward-declared function signature type (owned by VM).
   Declared in mi_parse.h and reused here to avoid duplicating enums/structs. */
typedef struct MiFuncTypeSig MiFuncTypeSig;

struct MiRtCmd
{
  bool       is_native;
  uint32_t   param_count;
  XSlice*    param_names; /* heap buffer owned by cmd */

  /* Optional function signature metadata.
     For native cmds this is required (except transitional variadics).
     Owned by the VM (perm arena) and referenced here. */
  const MiFuncTypeSig* sig;

  /* Optional doc string (owned by VM). */
  XSlice     doc;

  /* If is_native == false, body is MI_RT_VAL_BLOCK (retained). */
  MiRtValue  body;

  /* If is_native == true, call this instead of executing body. */
  MiRtNativeFn native_fn;

  /* Preferred native entrypoint with userdata (may be NULL). */
  MiRtNativeFn2 native_fn2;
  void*         native_user;
};

typedef struct MiRtDictEntry
{
  MiRtValue key;
  MiRtValue value;
  uint8_t   state; /* 0 = empty, 1 = filled, 2 = tombstone */
} MiRtDictEntry;

struct MiRtDict
{
  MiHeap*         heap;
  MiRtDictEntry*  entries;
  size_t          count;
  size_t          tombstones;
  size_t          capacity;
};

struct MiRtPair
{
  MiRtValue items[2];
};

struct MiRtList
{
  MiHeap*    heap;
  MiRtValue* items;
  size_t     count;
  size_t     capacity;
};

typedef enum MiRtBlockKind
{
  MI_RT_BLOCK_INVALID = 0,
  MI_RT_BLOCK_AST_SCRIPT,
  MI_RT_BLOCK_AST_EXPR,
  MI_RT_BLOCK_VM_CHUNK,
} MiRtBlockKind;

struct MiRtBlock
{
  MiRtBlockKind kind;
  void*         ptr;      // AST payloads
  MiScopeFrame* env;      // Defining environment (VM blocks)
  uint32_t      id;       // VM chunk id or user id
};

//----------------------------------------------------------
// Runtime / Scopes
//----------------------------------------------------------

typedef struct MiRtVar
{
  uint32_t          sym_id;
  MiRtValue         value;
  struct MiRtVar*   next;
} MiRtVar;

/* Create/destroy detached scope frames (not tied to rt->current push/pop). */
MiScopeFrame* mi_rt_scope_create(MiRuntime* rt, MiScopeFrame* parent);
void          mi_rt_scope_destroy(MiRuntime* rt, MiScopeFrame* frame);

typedef struct MiScopeFrame
{
  MiRuntime*           rt;
  XArena*              arena;
  MiRtVar*             vars;
  struct MiScopeFrame* parent;
  struct MiScopeFrame* next_free;
} MiScopeFrame;

typedef struct MiExprList MiExprList;

typedef MiRtValue (*MiRtExecBlockFn)(MiRuntime* rt, const MiRtBlock* block);

typedef MiRtValue (*MiRtCommandFn)(
    MiRuntime*        rt,
    const XSlice*     head_name,
    int               argc,
    MiExprList*       args);

typedef struct MiRtUserCommand
{
  XSlice        name;
  MiRtCommandFn fn;
} MiRtUserCommand;

struct MiRuntime
{
  MiHeap           heap;

  MiScopeFrame      root;
  MiScopeFrame*     current;
  MiScopeFrame*     free_frames;
  size_t            scope_chunk_size;

  MiRtUserCommand*  commands;
  size_t            command_count;
  size_t            command_capacity;

  MiRtExecBlockFn   exec_block;

  XSlice*           sym_names;
  size_t            sym_count;
  size_t            sym_capacity;
};

MiHeapStats mi_rt_heap_stats(const MiRuntime* rt);

uint32_t  mi_rt_sym_intern(MiRuntime* rt, XSlice name);
XSlice    mi_rt_sym_name(const MiRuntime* rt, uint32_t sym_id);

bool      mi_rt_var_get_id(MiRuntime* rt, uint32_t sym_id, MiRtValue* out);
bool      mi_rt_var_get_from_id(MiScopeFrame* start, uint32_t sym_id, MiRtValue* out);
void      mi_rt_var_set_from_id(MiScopeFrame* start, uint32_t sym_id, MiRtValue value);
void      mi_rt_var_define_id(MiRuntime* rt, uint32_t sym_id, MiRtValue value);
void      mi_rt_var_set_id(MiRuntime* rt, uint32_t sym_id, MiRtValue value);

//----------------------------------------------------------
// Refcount helpers
//----------------------------------------------------------

/* Retain a value if it owns a heap payload. */
void mi_rt_value_retain(MiRuntime* rt, MiRtValue v);

/* Release a value if it owns a heap payload. */
void mi_rt_value_release(MiRuntime* rt, MiRtValue v);

/* Assign into dst (release old, retain new). */
void mi_rt_value_assign(MiRuntime* rt, MiRtValue* dst, MiRtValue src);

//----------------------------------------------------------
// Public API
//----------------------------------------------------------

/**
 * Initialize a runtime instance.
 * @param rt Runtime to initialize.
 */
void mi_rt_init(MiRuntime* rt);

/**
 * Shut down a runtime instance and release resources.
 * @param rt Runtime to shut down.
 */
void mi_rt_shutdown(MiRuntime* rt);

/**
 * Push a new scope frame.
 * @param rt Runtime instance.
 */
void mi_rt_scope_push(MiRuntime* rt);

/**
 * Push a new scope frame with an explicit parent frame.
 * This is used by the VM to implement lexical scoping for blocks.
 * @param rt     Runtime instance.
 * @param parent Parent frame to chain to (may be NULL for root).
 */
void mi_rt_scope_push_with_parent(MiRuntime* rt, MiScopeFrame* parent);

/**
 * Push a new scope frame with an explicit parent.
 * This is used by VM blocks to implement lexical-ish environment chains.
 * @param rt     Runtime instance.
 * @param parent Parent frame for the new frame.
 */

/**
 * Pop the current scope frame.
 * @param rt Runtime instance.
 */
void mi_rt_scope_pop(MiRuntime* rt);

/**
 * Look up a variable by name.
 * @param rt        Runtime instance.
 * @param name      Variable name.
 * @param out_value Optional output for the variable value.
 * @return          True if the variable was found.
 */
bool mi_rt_var_get(const MiRuntime* rt, XSlice name, MiRtValue* out_value);

/* Look up a variable starting from an explicit scope frame (walks parent chain). */
bool mi_rt_var_get_from(const MiScopeFrame* start, XSlice name, MiRtValue* out_value);

/**
 * Set a variable value in the nearest enclosing scope.
 * If the variable does not exist, it is created in the current scope.
 * @param rt    Runtime instance.
 * @param name  Variable name.
 * @param value Value to assign.
 * @return      True on success.
 */
bool mi_rt_var_set(MiRuntime* rt, XSlice name, MiRtValue value);

/**
 * Define a variable in the current scope only.
 * If the variable already exists in the current scope, it is updated.
 * Unlike mi_rt_var_set(), this function never searches parent scopes.
 * @param rt    Runtime instance.
 * @param name  Variable name.
 * @param value Value to assign.
 * @return      True on success.
 */
bool mi_rt_var_define(MiRuntime* rt, XSlice name, MiRtValue value);

/**
 * Set the backend hook used to execute runtime blocks.
 * @param rt Runtime instance.
 * @param fn Block execution function.
 */
void mi_rt_set_exec_block(MiRuntime* rt, MiRtExecBlockFn fn);

/**
 * Create a new runtime list.
 * @param rt Runtime instance.
 * @return   Newly created list.
 */
MiRtList* mi_rt_list_create(MiRuntime* rt);

/**
 * Create a new runtime dictionary.
 * @param rt Runtime instance.
 * @return   Newly created dictionary.
 */
MiRtDict* mi_rt_dict_create(MiRuntime* rt);

/* Set a key/value pair (retains new key/value, releases overwritten value). */
bool mi_rt_dict_set(MiRuntime* rt, MiRtDict* dict, MiRtValue key, MiRtValue value);

/* Get a value by key. Returns false if not found. */
bool mi_rt_dict_get(const MiRtDict* dict, MiRtValue key, MiRtValue* out_value);

/* Remove a key from the dict (releases key/value). Returns false if not found. */
bool mi_rt_dict_remove(MiRuntime* rt, MiRtDict* dict, MiRtValue key);

/* Number of entries in the dict. */
size_t mi_rt_dict_count(const MiRtDict* dict);

typedef struct MiRtDictIter
{
  size_t index;
} MiRtDictIter;

/* Iterate over dict entries (borrowed keys/values; no allocation). */
bool mi_rt_dict_iter_next(const MiRtDict* dict, MiRtDictIter* it, MiRtValue* out_key, MiRtValue* out_value);

/**
 * Create a new runtime pair.
 * @return Newly created pair.
 */
MiRtPair* mi_rt_pair_create(MiRuntime* rt);

/* Set an element in a pair (retains new, releases old). */
void mi_rt_pair_set(MiRuntime* rt, MiRtPair* pair, int index, MiRtValue v);

/**
 * Create a new runtime block.
 * @param rt Runtime instance.
 * @return   Newly created block.
 */
MiRtBlock* mi_rt_block_create(MiRuntime* rt);
MiRtCmd*   mi_rt_cmd_create(MiRuntime* rt, uint32_t param_count, const XSlice* param_names, MiRtValue body);
MiRtCmd*   mi_rt_cmd_create_native(MiRuntime* rt, MiRtNativeFn native_fn);
MiRtCmd*   mi_rt_cmd_create_native2(MiRuntime* rt, MiRtNativeFn2 native_fn2, void* user, const MiFuncTypeSig* sig, XSlice doc);

/**
 * Append a value to a runtime list.
 * @param list List to modify.
 * @param v    Value to append.
 * @return     True on success.
 */
bool mi_rt_list_push(MiRtList* list, MiRtValue v);

/**
 * Create a void runtime value.
 * @return Void value.
 */
MiRtValue mi_rt_make_void(void);

/**
 * Create an integer runtime value.
 * @param v Integer value.
 * @return  Integer runtime value.
 */
MiRtValue mi_rt_make_int(long long v);

/**
 * Create a floating-point runtime value.
 * @param v Floating-point value.
 * @return  Floating-point runtime value.
 */
MiRtValue mi_rt_make_float(double v);

/**
 * Create a boolean runtime value.
 * @param v Boolean value.
 * @return  Boolean runtime value.
 */
MiRtValue mi_rt_make_bool(bool v);

/**
 * Create a string runtime value from a slice.
 * The slice is not copied.
 * @param s String slice.
 * @return  String runtime value.
 */
MiRtValue mi_rt_make_string_slice(XSlice s);

/**
 * Create a list runtime value.
 * @param list List object.
 * @return     List runtime value.
 */
MiRtValue mi_rt_make_list(MiRtList* list);

/**
 * Create a dict runtime value.
 * @param dict Dict object.
 * @return     Dict runtime value.
 */
MiRtValue mi_rt_make_dict(MiRtDict* dict);

/**
 * Create a KVREF runtime value (virtual 2-element view for dict iteration).
 * This value is not user-constructible from Minima syntax.
 * @param dict        Dict object.
 * @param entry_index Index into dict entries array.
 * @return            KVREF runtime value.
 */
MiRtValue mi_rt_make_kvref(MiRtDict* dict, size_t entry_index);

/**
 * Create a pair runtime value.
 * @param pair Pair object.
 * @return     Pair runtime value.
 */
MiRtValue mi_rt_make_pair(MiRtPair* pair);

/**
 * Create a block runtime value.
 * @param block Block object.
 * @return      Block runtime value.
 */
MiRtValue mi_rt_make_block(MiRtBlock* block);
MiRtValue mi_rt_make_cmd(MiRtCmd* cmd);

/**
 * Create a type-token runtime value.
 * The payload is a MiRtValueKind encoded as an integer.
 * @param kind Runtime value kind.
 * @return     Type-token runtime value.
 */
MiRtValue mi_rt_make_type(MiRtValueKind kind);

/* Create/destroy a detached scope frame.
   Detached scopes are used for module environments: they are not managed by
   mi_rt_scope_push/pop, and must be destroyed explicitly. */
MiScopeFrame* mi_rt_scope_create_detached(MiRuntime* rt, MiScopeFrame* parent);
void          mi_rt_scope_destroy_detached(MiRuntime* rt, MiScopeFrame* frame);


#endif // MI_RUNTIME_H
