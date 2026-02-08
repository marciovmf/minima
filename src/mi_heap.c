#include "mi_heap.h"

#include <string.h>
#ifdef MI_HEAP_DEBUG
#include <stdio.h>
#endif

#define MI_HEAP_SMALL_GRANULARITY 16u
#define MI_HEAP_SMALL_MAX_BYTES   (MI_HEAP_SMALL_GRANULARITY * 256u)

static size_t s_mi_heap_align_up(size_t n, size_t a)
{
  size_t mask = a - 1u;
  return (n + mask) & ~mask;
}

static size_t s_mi_heap_header_size(void)
{
  return s_mi_heap_align_up(sizeof(MiObjHeader), sizeof(void*));
}

static uint32_t s_mi_heap_small_class_index(size_t total_bytes)
{
  if (total_bytes == 0u)
  {
    return 0u;
  }

  size_t rounded = s_mi_heap_align_up(total_bytes, MI_HEAP_SMALL_GRANULARITY);
  size_t idx = (rounded / MI_HEAP_SMALL_GRANULARITY);
  if (idx == 0u)
  {
    idx = 1u;
  }
  if (idx > 255u)
  {
    idx = 255u;
  }
  return (uint32_t)idx;
}

MiObjHeader* mi_heap_header_from_payload(const void* payload)
{
  if (!payload)
  {
    return NULL;
  }

  const uint8_t* p = (const uint8_t*)payload;
  size_t header_size = s_mi_heap_header_size();
  return (MiObjHeader*)(void*)(p - header_size);
}

bool mi_heap_init(MiHeap* h, size_t chunk_size)
{
  if (!h)
  {
    return false;
  }

  memset(h, 0, sizeof(*h));
  h->arena = x_arena_create(chunk_size);
  if (!h->arena)
  {
    return false;
  }

  h->objects = NULL;
  memset(h->free_small, 0, sizeof(h->free_small));
  h->free_large = NULL;
  memset(&h->stats, 0, sizeof(h->stats));
  return true;
}

void mi_heap_shutdown(MiHeap* h)
{
  if (!h)
  {
    return;
  }

  if (h->arena)
  {
    x_arena_destroy(h->arena);
    h->arena = NULL;
  }

  h->objects = NULL;
  memset(h->free_small, 0, sizeof(h->free_small));
  h->free_large = NULL;
  memset(&h->stats, 0, sizeof(h->stats));
}

static MiObjHeader* s_mi_heap_pop_small(MiHeap* h, uint32_t class_index)
{
  if (!h || class_index == 0u || class_index > 255u)
  {
    return NULL;
  }

  MiObjHeader* hdr = h->free_small[class_index];
  if (hdr)
  {
    h->free_small[class_index] = hdr->next_free;
    hdr->next_free = NULL;
  }
  return hdr;
}

static MiObjHeader* s_mi_heap_pop_large(MiHeap* h, size_t min_total_bytes)
{
  if (!h)
  {
    return NULL;
  }

  MiObjHeader* prev = NULL;
  MiObjHeader* it = h->free_large;
  while (it)
  {
    if (it->total_size_bytes >= min_total_bytes)
    {
      if (prev)
      {
        prev->next_free = it->next_free;
      }
      else
      {
        h->free_large = it->next_free;
      }
      it->next_free = NULL;
      return it;
    }
    prev = it;
    it = it->next_free;
  }

  return NULL;
}

static void s_mi_heap_push_free(MiHeap* h, MiObjHeader* hdr)
{
  if (!h || !hdr)
  {
    return;
  }

  size_t total = hdr->total_size_bytes;
  if (total <= (size_t)MI_HEAP_SMALL_MAX_BYTES)
  {
    uint32_t idx = s_mi_heap_small_class_index(total);
    hdr->next_free = h->free_small[idx];
    h->free_small[idx] = hdr;
  }
  else
  {
    hdr->next_free = h->free_large;
    h->free_large = hdr;
  }
}

static void* s_mi_heap_alloc_internal(MiHeap* h, MiObjKind kind, size_t payload_size)
{
  if (!h || !h->arena)
  {
    return NULL;
  }

  size_t header_size = s_mi_heap_header_size();
  size_t total = header_size + payload_size;
  total = s_mi_heap_align_up(total, sizeof(void*));

  MiObjHeader* hdr = NULL;
  if (total <= (size_t)MI_HEAP_SMALL_MAX_BYTES)
  {
    uint32_t idx = s_mi_heap_small_class_index(total);
    hdr = s_mi_heap_pop_small(h, idx);
  }
  if (!hdr)
  {
    hdr = s_mi_heap_pop_large(h, total);
  }

  if (!hdr)
  {
    uint8_t* mem = (uint8_t*)x_arena_alloc_zero(h->arena, total);
    if (!mem)
    {
      return NULL;
    }
    hdr = (MiObjHeader*)mem;
    hdr->next = h->objects;
    h->objects = hdr;
    h->stats.bytes_requested += total;
  }
  else
  {
    // Reuse a freed block; clear payload bytes. 
    uint8_t* mem = (uint8_t*)hdr;
    if (hdr->total_size_bytes > header_size)
    {
      memset(mem + header_size, 0, hdr->total_size_bytes - header_size);
    }
    hdr->flags &= ~MI_OBJ_FLAG_FREED;
  }

  hdr->kind = kind;
  hdr->flags &= ~MI_OBJ_FLAG_FREED;
  hdr->refcount = 1u;
  hdr->reserved = 0u;
  hdr->payload_size_bytes = payload_size;
  hdr->total_size_bytes = total;
  hdr->next_free = NULL;
  h->stats.bytes_live += total;
  h->stats.alloc_count += 1u;

  return (void*)((uint8_t*)hdr + header_size);
}

void* mi_heap_alloc_obj(MiHeap* h, MiObjKind kind, size_t payload_size)
{
  return s_mi_heap_alloc_internal(h, kind, payload_size);
}

void* mi_heap_alloc_buffer(MiHeap* h, size_t payload_size)
{
  return s_mi_heap_alloc_internal(h, MI_OBJ_BUFFER, payload_size);
}

void mi_heap_retain_payload(void* payload)
{
  MiObjHeader* hdr = mi_heap_header_from_payload(payload);
  if (!hdr)
  {
    return;
  }
  if (hdr->flags & MI_OBJ_FLAG_FREED)
  {
    return;
  }
  hdr->refcount += 1u;
 
#ifdef MI_HEAP_DEBUG
  printf("RETAIN %p kind = %d, count = %d, flags = %x\n",
      payload, hdr->kind, hdr->refcount, hdr->flags);
#endif

}

void mi_heap_release_payload(MiHeap* h, void* payload)
{
  if (!h || !payload)
  {
    return;
  }

  MiObjHeader* hdr = mi_heap_header_from_payload(payload);
  if (!hdr)
  {
    return;
  }
  if (hdr->flags & MI_OBJ_FLAG_FREED)
  {
    return;
  }
  if (hdr->refcount == 0u)
  {
    return;
  }

  hdr->refcount -= 1u;
  if (hdr->refcount != 0u)
  {
    return;
  }

  hdr->flags |= MI_OBJ_FLAG_FREED;
  h->stats.bytes_live -= hdr->total_size_bytes;
  h->stats.free_count += 1u;


#ifdef MI_HEAP_DEBUG
  printf("RELEASE %p kind = %d, count = %d, flags = %x\n",
      payload, hdr->kind, hdr->refcount, hdr->flags);
#endif

  s_mi_heap_push_free(h, hdr);
}

MiHeapStats mi_heap_stats(const MiHeap* h)
{
  MiHeapStats s;
  memset(&s, 0, sizeof(s));
  if (!h)
  {
    return s;
  }
  return h->stats;
}

void mi_heap_iterate(const MiHeap* h, MiHeapIterFn fn, void* user)
{
  if (!h || !fn)
  {
    return;
  }

  size_t header_size = s_mi_heap_header_size();
  const MiObjHeader* it = h->objects;
  while (it)
  {
    const uint8_t* mem = (const uint8_t*)it;
    const void* payload = (const void*)(mem + header_size);
    fn(user, it, payload);
    it = it->next;
  }
}
