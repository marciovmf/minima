
#ifndef MI_HEAP_H
#define MI_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdx_arena.h>

typedef struct MiHeap MiHeap;

typedef enum MiObjKind
{
  MI_OBJ_INVALID = 0,
  MI_OBJ_LIST,
  MI_OBJ_PAIR,
  MI_OBJ_DICT,
  MI_OBJ_BLOCK,
  MI_OBJ_CMD,
  MI_OBJ_BUFFER
} MiObjKind;

typedef enum MiObjFlags
{
  MI_OBJ_FLAG_FREED = 1u << 0u
} MiObjFlags;

typedef struct MiHeapStats
{
  size_t bytes_requested;
  size_t bytes_live;
  size_t alloc_count;
  size_t free_count;
} MiHeapStats;

typedef struct MiObjHeader
{
  MiObjKind          kind;
  uint32_t           flags;
  uint32_t           refcount;
  uint32_t           reserved;
  size_t             payload_size_bytes; /* Payload size (not including header). */
  size_t             total_size_bytes;   /* Header + payload (aligned). */
  struct MiObjHeader* next;
  struct MiObjHeader* next_free;
} MiObjHeader;

typedef void (*MiHeapIterFn)(void* user, const MiObjHeader* h, const void* payload);

struct MiHeap
{
  XArena*      arena;
  MiObjHeader* objects;
  MiObjHeader* free_small[256];
  MiObjHeader* free_large;
  MiHeapStats  stats;
};

/**
 * Initializes a runtime heap.
 * @param h           Heap instance to initialize
 * @param chunk_size  Size in bytes of each backing arena chunk
 * @return            True on success, false on failure
 */
bool mi_heap_init(MiHeap* h, size_t chunk_size);

/**
 * Shuts down a runtime heap and releases all memory.
 * @param h  Heap instance to shut down
 */
void mi_heap_shutdown(MiHeap* h);

/**
 * Allocates a heap-managed object.
 * @param h             Heap instance
 * @param kind          Object kind identifier
 * @param payload_size  Size in bytes of the object payload
 * @return              Pointer to object payload
 */
void* mi_heap_alloc_obj(MiHeap* h, MiObjKind kind, size_t payload_size);

/**
 * Allocates a heap-managed raw buffer.
 * @param h             Heap instance
 * @param payload_size  Size in bytes of the buffer
 * @return              Pointer to allocated buffer
 */
void* mi_heap_alloc_buffer(MiHeap* h, size_t payload_size);

/* Retain/release payload pointers that were returned by mi_heap_alloc_*. */
void  mi_heap_retain_payload(void* payload);

/* Retain/release payload pointers that were returned by mi_heap_alloc_*. */
void  mi_heap_release_payload(MiHeap* h, void* payload);

/* Get the object header for a payload pointer (or NULL). */
MiObjHeader* mi_heap_header_from_payload(const void* payload);

/**
 * Returns heap usage statistics.
 * @param h  Heap instance
 * @return   Current heap statistics
 */
MiHeapStats mi_heap_stats(const MiHeap* h);

/**
 * Iterates over all heap-managed objects.
 * @param h     Heap instance
 * @param fn    Callback invoked for each object
 * @param user  User pointer passed to the callback
 */
void mi_heap_iterate(const MiHeap* h, MiHeapIterFn fn, void* user);

#endif
