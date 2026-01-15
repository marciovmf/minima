#include "mi_runtime.h"
#include "mi_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdx_log.h>

//----------------------------------------------------------
// Internal helpers
//----------------------------------------------------------

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
    mi_error("mi_runtime: out of memory\n");
    exit(1);
  }
  return p;
}

static void s_scope_init(MiScopeFrame* f, size_t chunk_size, MiScopeFrame* parent)
{
  f->arena = x_arena_create(chunk_size);
  if (!f->arena)
  {
    mi_error("mi_runtime: out of memory\n");
    exit(1);
  }
  f->vars = NULL;
  f->parent = parent;
  f->next_free = NULL;
}

static MiRtVar* s_var_find_in_frame(const MiScopeFrame* frame, XSlice name)
{
  if (!frame)
  {
    return NULL;
  }

  MiRtVar* it = frame->vars;
  while (it)
  {
    if (x_slice_eq(it->name, name))
    {
      return it;
    }
    it = it->next;
  }

  return NULL;
}

//----------------------------------------------------------
// Public API
//----------------------------------------------------------

void mi_rt_init(MiRuntime* rt)
{
  if (!rt)
  {
    return;
  }

  rt->scope_chunk_size = 16u * 1024u;

  rt->root.arena = x_arena_create(64u * 1024u);
  if (!rt->root.arena)
  {
    mi_error("mi_runtime: out of memory\n");
    exit(1);
  }
  rt->root.vars = NULL;
  rt->root.parent = NULL;
  rt->root.next_free = NULL;

  rt->current = &rt->root;
  rt->free_frames = NULL;

  rt->commands = NULL;
  rt->command_count = 0u;
  rt->command_capacity = 0u;

  rt->exec_block = NULL;
}

void mi_rt_shutdown(MiRuntime* rt)
{
  size_t i;

  if (!rt)
  {
    return;
  }

  while (rt->current && rt->current != &rt->root)
  {
    mi_rt_scope_pop(rt);
  }

  while (rt->free_frames)
  {
    MiScopeFrame* f = rt->free_frames;
    rt->free_frames = f->next_free;

    x_arena_destroy(f->arena);
    free(f);
  }

  x_arena_destroy(rt->root.arena);
  rt->root.arena = NULL;
  rt->root.vars = NULL;
  rt->root.parent = NULL;
  rt->current = NULL;

  if (rt->commands)
  {
    for (i = 0u; i < rt->command_count; ++i)
    {
      if (rt->commands[i].name.ptr)
      {
        free((void*)rt->commands[i].name.ptr);
      }
    }
    free(rt->commands);
  }

  rt->commands = NULL;
  rt->command_count = 0u;
  rt->command_capacity = 0u;
  rt->exec_block = NULL;
}

void mi_rt_scope_push(MiRuntime* rt)
{
  mi_rt_scope_push_with_parent(rt, rt ? rt->current : NULL);
}

void mi_rt_scope_push_with_parent(MiRuntime* rt, MiScopeFrame* parent)
{
  if (!rt)
  {
    return;
  }

  MiScopeFrame* f = NULL;
  if (rt->free_frames)
  {
    f = rt->free_frames;
    rt->free_frames = f->next_free;
    f->next_free = NULL;
    f->parent = parent;
    x_arena_reset(f->arena);
    f->vars = NULL;
  }
  else
  {
    f = (MiScopeFrame*)s_realloc(NULL, sizeof(MiScopeFrame));
    s_scope_init(f, rt->scope_chunk_size, parent);
  }

  rt->current = f;
}

void mi_rt_scope_pop(MiRuntime* rt)
{
  if (!rt || !rt->current || rt->current == &rt->root)
  {
    return;
  }

  MiScopeFrame* dead = rt->current;
  rt->current = dead->parent;

  dead->next_free = rt->free_frames;
  rt->free_frames = dead;
}

bool mi_rt_var_get(const MiRuntime* rt, XSlice name, MiRtValue* out_value)
{
  if (!rt)
  {
    return false;
  }

  const MiScopeFrame* f = rt->current;
  while (f)
  {
    MiRtVar* v = s_var_find_in_frame(f, name);
    if (v)
    {
      if (out_value)
      {
        *out_value = v->value;
      }
      return true;
    }
    f = f->parent;
  }

  return false;
}

bool mi_rt_var_set(MiRuntime* rt, XSlice name, MiRtValue value)
{
  if (!rt || !rt->current)
  {
    return false;
  }

  // Update nearest existing.
  MiScopeFrame* f = rt->current;
  while (f)
  {
    MiRtVar* v = s_var_find_in_frame(f, name);
    if (v)
    {
      v->value = value;
      return true;
    }
    f = f->parent;
  }

  // Insert in current.
  MiRtVar* v = (MiRtVar*)x_arena_alloc(rt->current->arena, sizeof(MiRtVar));
  if (!v)
  {
    mi_error("mi_runtime: out of memory\n");
    exit(1);
  }

  v->name = name;
  v->value = value;
  v->next = rt->current->vars;
  rt->current->vars = v;
  return true;
}

void mi_rt_set_exec_block(MiRuntime* rt, MiRtExecBlockFn fn)
{
  if (!rt)
  {
    return;
  }
  rt->exec_block = fn;
}

//----------------------------------------------------------
// Value constructors / containers
//----------------------------------------------------------

MiRtList* mi_rt_list_create(MiRuntime* rt)
{
  if (!rt)
  {
    return NULL;
  }

  MiRtList* list = (MiRtList*)x_arena_alloc(rt->current->arena, sizeof(MiRtList));
  if (!list)
  {
    mi_error("mi_runtime: out of memory\n");
    exit(1);
  }

  list->arena = rt->current->arena;
  list->items = NULL;
  list->count = 0u;
  list->capacity = 0u;

  return list;
}

MiRtPair* mi_rt_pair_create(void)
{
  MiRtPair* p = (MiRtPair*)s_realloc(NULL, sizeof(MiRtPair));
  memset(p, 0, sizeof(MiRtPair));
  return p;
}

MiRtBlock* mi_rt_block_create(MiRuntime* rt)
{
  if (!rt)
  {
    return NULL;
  }

  MiRtBlock* b = (MiRtBlock*)x_arena_alloc(rt->current->arena, sizeof(MiRtBlock));
  if (!b)
  {
    mi_error("mi_runtime: out of memory\n");
    exit(1);
  }

  b->kind = MI_RT_BLOCK_INVALID;
  b->ptr = NULL;
  b->env = NULL;
  b->id = 0u;
  return b;
}

bool mi_rt_list_push(MiRtList* list, MiRtValue v)
{
  if (!list)
  {
    return false;
  }

  if (list->count == list->capacity)
  {
    size_t new_cap = (list->capacity == 0u) ? 8u : (list->capacity * 2u);
    MiRtValue* new_items = (MiRtValue*)x_arena_alloc(list->arena, new_cap * sizeof(MiRtValue));
    if (!new_items)
    {
      return false;
    }
    if (list->items && list->count > 0u)
    {
      memcpy(new_items, list->items, list->count * sizeof(MiRtValue));
    }
    list->items = new_items;
    list->capacity = new_cap;
  }

  list->items[list->count] = v;
  list->count = list->count + 1u;
  return true;
}

MiRtValue mi_rt_make_void(void)
{
  MiRtValue v;
  v.kind = MI_RT_VAL_VOID;
  return v;
}

MiRtValue mi_rt_make_int(long long v)
{
  MiRtValue out;
  out.kind = MI_RT_VAL_INT;
  out.as.i = v;
  return out;
}

MiRtValue mi_rt_make_float(double v)
{
  MiRtValue out;
  out.kind = MI_RT_VAL_FLOAT;
  out.as.f = v;
  return out;
}

MiRtValue mi_rt_make_bool(bool v)
{
  MiRtValue out;
  out.kind = MI_RT_VAL_BOOL;
  out.as.b = v;
  return out;
}

MiRtValue mi_rt_make_string_slice(XSlice s)
{
  MiRtValue out;
  out.kind = MI_RT_VAL_STRING;
  out.as.s = s;
  return out;
}

MiRtValue mi_rt_make_list(MiRtList* list)
{
  MiRtValue out;
  out.kind = MI_RT_VAL_LIST;
  out.as.list = list;
  return out;
}

MiRtValue mi_rt_make_pair(MiRtPair* pair)
{
  MiRtValue out;
  out.kind = MI_RT_VAL_PAIR;
  out.as.pair = pair;
  return out;
}

MiRtValue mi_rt_make_block(MiRtBlock* block)
{
  MiRtValue out;
  out.kind = MI_RT_VAL_BLOCK;
  out.as.block = block;
  return out;
}

