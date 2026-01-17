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

static void* s_value_payload_ptr(MiRtValue v)
{
  switch (v.kind)
  {
    case MI_RT_VAL_LIST:  return (void*)v.as.list;
    case MI_RT_VAL_DICT:  return (void*)v.as.dict;
    case MI_RT_VAL_PAIR:  return (void*)v.as.pair;
    case MI_RT_VAL_BLOCK: return (void*)v.as.block;
    default:              return NULL;
  }
}

static void s_value_pre_destroy(MiRuntime* rt, MiRtValue v)
{
  if (!rt)
  {
    return;
  }

  if (v.kind == MI_RT_VAL_LIST && v.as.list)
  {
    MiRtList* list = v.as.list;
    for (size_t i = 0u; i < list->count; ++i)
    {
      mi_rt_value_release(rt, list->items[i]);
    }
    if (list->items)
    {
      mi_heap_release_payload(&rt->heap, list->items);
      list->items = NULL;
      list->count = 0u;
      list->capacity = 0u;
    }
  }
  else if (v.kind == MI_RT_VAL_DICT && v.as.dict)
  {
    MiRtDict* d = v.as.dict;

    if (d->entries)
    {
      for (size_t i = 0u; i < d->capacity; ++i)
      {
        MiRtDictEntry* e = &d->entries[i];
        if (e->state == 1u)
        {
          mi_rt_value_release(rt, e->key);
          mi_rt_value_release(rt, e->value);
        }
      }

      mi_heap_release_payload(&rt->heap, d->entries);
      d->entries = NULL;
      d->capacity = 0u;
      d->count = 0u;
      d->tombstones = 0u;
    }
  }
  else if (v.kind == MI_RT_VAL_PAIR && v.as.pair)
  {
    MiRtPair* p = v.as.pair;
    mi_rt_value_release(rt, p->items[0]);
    mi_rt_value_release(rt, p->items[1]);
  }
  else if (v.kind == MI_RT_VAL_BLOCK && v.as.block)
  {
    /* Blocks currently don't own their env/payload memory. */
  }
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

  if (!mi_heap_init(&rt->heap, 256u * 1024u))
  {
    mi_error("mi_runtime: out of memory\n");
    exit(1);
  }

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

  /* Release values stored in the root frame before destroying the heap. */
  {
    MiRtVar* it = rt->root.vars;
    while (it)
    {
      mi_rt_value_release(rt, it->value);
      it = it->next;
    }
  }

  x_arena_destroy(rt->root.arena);

  mi_heap_shutdown(&rt->heap);
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

MiHeapStats mi_rt_heap_stats(const MiRuntime* rt)
{
  if (!rt)
  {
    MiHeapStats s;
    memset(&s, 0, sizeof(s));
    return s;
  }
  return mi_heap_stats(&rt->heap);
}

void mi_rt_value_retain(MiRuntime* rt, MiRtValue v)
{
  (void)rt;
  void* p = s_value_payload_ptr(v);
  if (p)
  {
    mi_heap_retain_payload(p);
  }
}

void mi_rt_value_release(MiRuntime* rt, MiRtValue v)
{
  if (!rt)
  {
    return;
  }

  void* p = s_value_payload_ptr(v);
  if (!p)
  {
    return;
  }

  MiObjHeader* hdr = mi_heap_header_from_payload(p);
  if (!hdr)
  {
    return;
  }

  if (hdr->flags & MI_OBJ_FLAG_FREED)
  {
    return;
  }

  /* If this is the last reference, release children before freeing. */
  if (hdr->refcount == 1u)
  {
    s_value_pre_destroy(rt, v);
  }

  mi_heap_release_payload(&rt->heap, p);
}

void mi_rt_value_assign(MiRuntime* rt, MiRtValue* dst, MiRtValue src)
{
  if (!dst)
  {
    return;
  }

  if (rt)
  {
    mi_rt_value_release(rt, *dst);
    mi_rt_value_retain(rt, src);
  }

  *dst = src;
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

  /* Release all values stored in this frame before the arena is reused. */
  MiRtVar* it = dead->vars;
  while (it)
  {
    mi_rt_value_release(rt, it->value);
    it = it->next;
  }

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
      mi_rt_value_assign(rt, &v->value, value);
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
  v->value = mi_rt_make_void();
  mi_rt_value_assign(rt, &v->value, value);
  v->next = rt->current->vars;
  rt->current->vars = v;
  return true;
}

bool mi_rt_var_define(MiRuntime* rt, XSlice name, MiRtValue value)
{
  if (!rt || !rt->current)
  {
    return false;
  }

  MiRtVar* v = s_var_find_in_frame(rt->current, name);
  if (v)
  {
    mi_rt_value_assign(rt, &v->value, value);
    return true;
  }

  v = (MiRtVar*)x_arena_alloc(rt->current->arena, sizeof(MiRtVar));
  if (!v)
  {
    mi_error("mi_runtime: out of memory\n");
    exit(1);
  }

  v->name = name;
  v->value = mi_rt_make_void();
  mi_rt_value_assign(rt, &v->value, value);
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

  MiRtList* list = (MiRtList*)mi_heap_alloc_obj(&rt->heap, MI_OBJ_LIST, sizeof(MiRtList));
  if (!list)
  {
    mi_error("mi_runtime: out of memory\n");
    exit(1);
  }

  list->heap = &rt->heap;
  list->items = NULL;
  list->count = 0u;
  list->capacity = 0u;

  return list;
}

//----------------------------------------------------------
// Dict implementation
//----------------------------------------------------------

static uint64_t s_hash_u64(uint64_t x)
{
  /* SplitMix64 mix (public domain). */
  x = (x ^ (x >> 30u)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27u)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31u);
}

static uint64_t s_hash_bytes(const void* data, size_t len)
{
  const uint8_t* p = (const uint8_t*)data;
  uint64_t h = 1469598103934665603ULL; /* FNV-1a 64 */
  for (size_t i = 0u; i < len; ++i)
  {
    h = h ^ (uint64_t)p[i];
    h = h * 1099511628211ULL;
  }
  return h;
}

static uint64_t s_hash_value(MiRtValue v)
{
  switch (v.kind)
  {
    case MI_RT_VAL_VOID:
      return 0u;
    case MI_RT_VAL_BOOL:
      return s_hash_u64(v.as.b ? 1u : 0u);
    case MI_RT_VAL_INT:
      return s_hash_u64((uint64_t)v.as.i);
    case MI_RT_VAL_FLOAT:
      {
        uint64_t bits = 0u;
        memcpy(&bits, &v.as.f, sizeof(bits));
        return s_hash_u64(bits);
      }
    case MI_RT_VAL_STRING:
      {
        if (!v.as.s.ptr)
        {
          return 0u;
        }
        uint64_t h = s_hash_bytes(v.as.s.ptr, v.as.s.length);
        return s_hash_u64(h ^ (uint64_t)v.as.s.length);
      }
    case MI_RT_VAL_LIST:
      return s_hash_u64((uint64_t)(uintptr_t)v.as.list);
    case MI_RT_VAL_DICT:
      return s_hash_u64((uint64_t)(uintptr_t)v.as.dict);
    case MI_RT_VAL_KVREF:
      {
        uint64_t a = (uint64_t)(uintptr_t)v.as.kvref.dict;
        uint64_t b = (uint64_t)v.as.kvref.entry_index;
        return s_hash_u64(a ^ s_hash_u64(b + 0x9E3779B97F4A7C15ULL));
      }
    case MI_RT_VAL_BLOCK:
      return s_hash_u64((uint64_t)(uintptr_t)v.as.block);
    case MI_RT_VAL_PAIR:
      return s_hash_u64((uint64_t)(uintptr_t)v.as.pair);
    default:
      return 0u;
  }
}

static bool s_value_key_eq(MiRtValue a, MiRtValue b)
{
  if (a.kind != b.kind)
  {
    return false;
  }

  switch (a.kind)
  {
    case MI_RT_VAL_VOID:
      return true;
    case MI_RT_VAL_BOOL:
      return a.as.b == b.as.b;
    case MI_RT_VAL_INT:
      return a.as.i == b.as.i;
    case MI_RT_VAL_FLOAT:
      {
        uint64_t aa = 0u;
        uint64_t bb = 0u;
        memcpy(&aa, &a.as.f, sizeof(aa));
        memcpy(&bb, &b.as.f, sizeof(bb));
        return aa == bb;
      }
    case MI_RT_VAL_STRING:
      return x_slice_eq(a.as.s, b.as.s);
    case MI_RT_VAL_LIST:
      return a.as.list == b.as.list;
    case MI_RT_VAL_DICT:
      return a.as.dict == b.as.dict;
    case MI_RT_VAL_KVREF:
      return a.as.kvref.dict == b.as.kvref.dict && a.as.kvref.entry_index == b.as.kvref.entry_index;
    case MI_RT_VAL_BLOCK:
      return a.as.block == b.as.block;
    case MI_RT_VAL_PAIR:
      return a.as.pair == b.as.pair;
    default:
      return false;
  }
}

static size_t s_next_pow2(size_t x)
{
  size_t p = 1u;
  while (p < x)
  {
    p = p << 1u;
  }
  return p;
}

static bool s_dict_grow(MiRuntime* rt, MiRtDict* d, size_t new_capacity)
{
  if (!rt || !d)
  {
    return false;
  }

  if (new_capacity < 8u)
  {
    new_capacity = 8u;
  }

  new_capacity = s_next_pow2(new_capacity);

  MiRtDictEntry* new_entries = (MiRtDictEntry*)mi_heap_alloc_buffer(&rt->heap, new_capacity * sizeof(MiRtDictEntry));
  if (!new_entries)
  {
    return false;
  }
  memset(new_entries, 0, new_capacity * sizeof(MiRtDictEntry));

  MiRtDictEntry* old_entries = d->entries;
  size_t old_cap = d->capacity;

  d->entries = new_entries;
  d->capacity = new_capacity;
  d->count = 0u;
  d->tombstones = 0u;

  if (old_entries && old_cap)
  {
    for (size_t i = 0u; i < old_cap; ++i)
    {
      MiRtDictEntry* e = &old_entries[i];
      if (e->state != 1u)
      {
        continue;
      }

      /* Reinsert without retain/release: ownership stays in the dict. */
      uint64_t h = s_hash_value(e->key);
      size_t mask = d->capacity - 1u;
      size_t idx = (size_t)h & mask;
      while (d->entries[idx].state == 1u)
      {
        idx = (idx + 1u) & mask;
      }
      d->entries[idx] = *e;
      d->entries[idx].state = 1u;
      d->count++;
    }

    mi_heap_release_payload(&rt->heap, old_entries);
  }

  return true;
}

MiRtDict* mi_rt_dict_create(MiRuntime* rt)
{
  if (!rt)
  {
    return NULL;
  }

  MiRtDict* d = (MiRtDict*)mi_heap_alloc_obj(&rt->heap, MI_OBJ_DICT, sizeof(MiRtDict));
  if (!d)
  {
    mi_error("mi_runtime: out of memory\n");
    exit(1);
  }

  d->heap = &rt->heap;
  d->entries = NULL;
  d->count = 0u;
  d->tombstones = 0u;
  d->capacity = 0u;

  (void)s_dict_grow(rt, d, 8u);
  return d;
}

bool mi_rt_dict_set(MiRuntime* rt, MiRtDict* d, MiRtValue key, MiRtValue value)
{
  if (!rt || !d)
  {
    return false;
  }

  if (!d->entries || d->capacity == 0u)
  {
    if (!s_dict_grow(rt, d, 8u))
    {
      return false;
    }
  }

  /* Grow if load factor (including tombstones) gets high. */
  size_t used = d->count + d->tombstones;
  if (used * 4u >= d->capacity * 3u)
  {
    (void)s_dict_grow(rt, d, d->capacity ? d->capacity * 2u : 8u);
  }

  uint64_t h = s_hash_value(key);
  size_t mask = d->capacity - 1u;
  size_t idx = (size_t)h & mask;
  size_t first_tomb = (size_t)(~0u);

  while (true)
  {
    MiRtDictEntry* e = &d->entries[idx];
    if (e->state == 0u)
    {
      if (first_tomb != (size_t)(~0u))
      {
        e = &d->entries[first_tomb];
        d->tombstones--;
      }

      e->state = 1u;
      e->key = mi_rt_make_void();
      e->value = mi_rt_make_void();
      mi_rt_value_assign(rt, &e->key, key);
      mi_rt_value_assign(rt, &e->value, value);
      d->count++;
      return true;
    }
    else if (e->state == 2u)
    {
      if (first_tomb == (size_t)(~0u))
      {
        first_tomb = idx;
      }
    }
    else if (e->state == 1u)
    {
      if (s_value_key_eq(e->key, key))
      {
        mi_rt_value_assign(rt, &e->value, value);
        return true;
      }
    }

    idx = (idx + 1u) & mask;
  }
}

bool mi_rt_dict_get(const MiRtDict* d, MiRtValue key, MiRtValue* out_value)
{
  if (!d || !d->entries || d->capacity == 0u)
  {
    return false;
  }

  uint64_t h = s_hash_value(key);
  size_t mask = d->capacity - 1u;
  size_t idx = (size_t)h & mask;

  while (true)
  {
    const MiRtDictEntry* e = &d->entries[idx];
    if (e->state == 0u)
    {
      return false;
    }
    if (e->state == 1u && s_value_key_eq(e->key, key))
    {
      if (out_value)
      {
        *out_value = e->value;
      }
      return true;
    }
    idx = (idx + 1u) & mask;
  }
}

bool mi_rt_dict_remove(MiRuntime* rt, MiRtDict* d, MiRtValue key)
{
  if (!rt || !d || !d->entries || d->capacity == 0u)
  {
    return false;
  }

  uint64_t h = s_hash_value(key);
  size_t mask = d->capacity - 1u;
  size_t idx = (size_t)h & mask;

  while (true)
  {
    MiRtDictEntry* e = &d->entries[idx];
    if (e->state == 0u)
    {
      return false;
    }
    if (e->state == 1u && s_value_key_eq(e->key, key))
    {
      mi_rt_value_release(rt, e->key);
      mi_rt_value_release(rt, e->value);
      e->key = mi_rt_make_void();
      e->value = mi_rt_make_void();
      e->state = 2u;
      d->count--;
      d->tombstones++;
      return true;
    }
    idx = (idx + 1u) & mask;
  }
}

size_t mi_rt_dict_count(const MiRtDict* d)
{
  return d ? d->count : 0u;
}

bool mi_rt_dict_iter_next(const MiRtDict* d, MiRtDictIter* it, MiRtValue* out_key, MiRtValue* out_value)
{
  if (!d || !it || !d->entries)
  {
    return false;
  }

  while (it->index < d->capacity)
  {
    const MiRtDictEntry* e = &d->entries[it->index++];
    if (e->state == 1u)
    {
      if (out_key)
      {
        *out_key = e->key;
      }
      if (out_value)
      {
        *out_value = e->value;
      }
      return true;
    }
  }

  return false;
}

MiRtPair* mi_rt_pair_create(MiRuntime* rt)
{
  if (!rt)
  {
    return NULL;
  }

  MiRtPair* p = (MiRtPair*)mi_heap_alloc_obj(&rt->heap, MI_OBJ_PAIR, sizeof(MiRtPair));
  memset(p, 0, sizeof(MiRtPair));
  p->items[0] = mi_rt_make_void();
  p->items[1] = mi_rt_make_void();
  return p;
}

void mi_rt_pair_set(MiRuntime* rt, MiRtPair* pair, int index, MiRtValue v)
{
  if (!rt || !pair)
  {
    return;
  }
  if (index < 0 || index > 1)
  {
    return;
  }
  mi_rt_value_assign(rt, &pair->items[index], v);
}

MiRtBlock* mi_rt_block_create(MiRuntime* rt)
{
  if (!rt)
  {
    return NULL;
  }

  MiRtBlock* b = (MiRtBlock*)mi_heap_alloc_obj(&rt->heap, MI_OBJ_BLOCK, sizeof(MiRtBlock));
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
    MiRtValue* new_items = (MiRtValue*)mi_heap_alloc_buffer(list->heap, new_cap * sizeof(MiRtValue));
    if (!new_items)
    {
      return false;
    }
    if (list->items && list->count > 0u)
    {
      memcpy(new_items, list->items, list->count * sizeof(MiRtValue));
    }

    if (list->items)
    {
      mi_heap_release_payload(list->heap, list->items);
    }

    list->items = new_items;
    list->capacity = new_cap;
  }

  /* The list owns a reference to v (if it is a ref value). */
  mi_heap_retain_payload(
      (v.kind == MI_RT_VAL_LIST)  ? (void*)v.as.list :
      (v.kind == MI_RT_VAL_PAIR)  ? (void*)v.as.pair :
      (v.kind == MI_RT_VAL_BLOCK) ? (void*)v.as.block :
      NULL);

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

MiRtValue mi_rt_make_dict(MiRtDict* dict)
{
  MiRtValue out;
  out.kind = MI_RT_VAL_DICT;
  out.as.dict = dict;
  return out;
}

MiRtValue mi_rt_make_kvref(MiRtDict* dict, size_t entry_index)
{
  MiRtValue out;
  out.kind = MI_RT_VAL_KVREF;
  out.as.kvref.dict = dict;
  out.as.kvref.entry_index = entry_index;
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

