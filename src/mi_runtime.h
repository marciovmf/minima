#ifndef MI_RUNTIME_H
#define MI_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdx_common.h>
#include <stdx_arena.h>
#include <stdx_string.h>

//----------------------------------------------------------
// Runtime Values
//----------------------------------------------------------

typedef struct MiRuntime MiRuntime;
typedef struct MiRtValue MiRtValue;
typedef struct MiRtList MiRtList;
typedef struct MiRtPair MiRtPair;
typedef struct MiRtBlock MiRtBlock;

typedef enum MiRtValueKind
{
  MI_RT_VAL_VOID = 0,
  MI_RT_VAL_INT,
  MI_RT_VAL_FLOAT,
  MI_RT_VAL_BOOL,
  MI_RT_VAL_STRING,
  MI_RT_VAL_LIST,
  MI_RT_VAL_BLOCK,
  MI_RT_VAL_PAIR
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
    MiRtBlock* block;
  } as;
};

struct MiRtPair
{
  MiRtValue items[2];
};

struct MiRtList
{
  XArena*    arena;
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
  uint32_t      id;       // VM chunk id or user id
};

//----------------------------------------------------------
// Runtime / Scopes
//----------------------------------------------------------

typedef struct MiRtVar
{
  XSlice            name;
  MiRtValue         value;
  struct MiRtVar*   next;
} MiRtVar;

typedef struct MiScopeFrame
{
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
  MiScopeFrame      root;
  MiScopeFrame*     current;
  MiScopeFrame*     free_frames;
  size_t            scope_chunk_size;

  MiRtUserCommand*  commands;
  size_t            command_count;
  size_t            command_capacity;

  MiRtExecBlockFn   exec_block;
};

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
 * Create a new runtime pair.
 * @return Newly created pair.
 */
MiRtPair* mi_rt_pair_create(void);

/**
 * Create a new runtime block.
 * @param rt Runtime instance.
 * @return   Newly created block.
 */
MiRtBlock* mi_rt_block_create(MiRuntime* rt);

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


#endif // MI_RUNTIME_H
