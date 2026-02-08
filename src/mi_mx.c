#include "mi_mx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdx_common.h>

//----------------------------------------------------------
// MIX - on-disk structs
//----------------------------------------------------------

#define MI_MX_MAGIC_0 '<'
#define MI_MX_MAGIC_1 'M'
#define MI_MX_MAGIC_2 'X'
#define MI_MX_MAGIC_3 '>'

// MI_MX_VERSION defined in mi_mx.h 

typedef struct MiMixHeader
{
  uint8_t  magic[4];
  uint32_t version;
  uint32_t chunk_count;
  uint32_t entry_chunk_index;
} MiMixHeader;

bool mi_mx_peek_file_version(const char* filename, uint32_t* out_version)
{
  if (!filename || !out_version)
  {
    return false;
  }

  FILE* f = fopen(filename, "rb");
  if (!f)
  {
    return false;
  }

  MiMixHeader h;
  bool ok = fread(&h, sizeof(h), 1, f) == 1;
  fclose(f);
  if (!ok)
  {
    return false;
  }

  if (h.magic[0] != MI_MX_MAGIC_0 || h.magic[1] != MI_MX_MAGIC_1 || h.magic[2] != MI_MX_MAGIC_2 || h.magic[3] != MI_MX_MAGIC_3)
  {
    return false;
  }

  *out_version = h.version;
  return true;
}

typedef enum MiMixConstKind
{
  MI_MX_CONST_VOID  = 0,
  MI_MX_CONST_INT   = 1,
  MI_MX_CONST_FLOAT = 2,
  MI_MX_CONST_BOOL  = 3,
  MI_MX_CONST_STRING = 4,
} MiMixConstKind;

//----------------------------------------------------------
// Helpers
//----------------------------------------------------------

static int s_x_slice_eq(XSlice a, XSlice b)
{
  if (a.length != b.length)
  {
    return 0;
  }
  if (a.length == 0)
  {
    return 1;
  }
  return memcmp(a.ptr, b.ptr, a.length) == 0;
}

static bool s_write_u32(FILE* f, uint32_t v)
{
  return fwrite(&v, sizeof(v), 1, f) == 1;
}

static bool s_write_i32(FILE* f, int32_t v)
{
  return fwrite(&v, sizeof(v), 1, f) == 1;
}

static bool s_write_u8(FILE* f, uint8_t v)
{
  return fwrite(&v, sizeof(v), 1, f) == 1;
}

static bool s_write_u64(FILE* f, uint64_t v)
{
  return fwrite(&v, sizeof(v), 1, f) == 1;
}

static bool s_write_f64(FILE* f, double v)
{
  return fwrite(&v, sizeof(v), 1, f) == 1;
}

static bool s_write_bytes(FILE* f, const void* p, size_t n)
{
  if (n == 0)
  {
    return true;
  }
  return fwrite(p, 1, n, f) == n;
}

static bool s_read_u32(FILE* f, uint32_t* out)
{
  return fread(out, sizeof(*out), 1, f) == 1;
}

static bool s_read_i32(FILE* f, int32_t* out)
{
  return fread(out, sizeof(*out), 1, f) == 1;
}

static bool s_read_u8(FILE* f, uint8_t* out)
{
  return fread(out, sizeof(*out), 1, f) == 1;
}

static bool s_read_u64(FILE* f, uint64_t* out)
{
  return fread(out, sizeof(*out), 1, f) == 1;
}

static bool s_read_f64(FILE* f, double* out)
{
  return fread(out, sizeof(*out), 1, f) == 1;
}

static bool s_read_bytes(FILE* f, void* p, size_t n)
{
  if (n == 0)
  {
    return true;
  }
  return fread(p, 1, n, f) == n;
}

static bool s_write_slice(FILE* f, XSlice s)
{
  if (s.length > 0xFFFFFFFFu)
  {
    return false;
  }
  if (!s_write_u32(f, (uint32_t)s.length))
  {
    return false;
  }
  return s_write_bytes(f, s.ptr, s.length);
}

static bool s_read_slice(FILE* f, XArena* arena, XSlice* out)
{
  uint32_t n = 0;
  if (!s_read_u32(f, &n))
  {
    return false;
  }
  if (n == 0)
  {
    out->ptr = NULL;
    out->length = 0;
    return true;
  }
  char* p = (char*)x_arena_alloc(arena, (size_t)n);
  if (!p)
  {
    return false;
  }
  if (!s_read_bytes(f, p, (size_t)n))
  {
    return false;
  }
  out->ptr = p;
  out->length = n;
  return true;
}

//----------------------------------------------------------
// Chunk indexing (save)
//----------------------------------------------------------

typedef struct MiMixChunkMap
{
  const MiVmChunk** ptrs;
  uint32_t*         indices;
  size_t            count;
  size_t            capacity;
} MiMixChunkMap;

static bool s_map_push(MiMixChunkMap* m, const MiVmChunk* c, uint32_t idx)
{
  if (m->count == m->capacity)
  {
    size_t new_cap = (m->capacity == 0) ? 16u : (m->capacity * 2u);
    const MiVmChunk** np = (const MiVmChunk**)realloc(m->ptrs, new_cap * sizeof(*np));
    uint32_t* ni = (uint32_t*)realloc(m->indices, new_cap * sizeof(*ni));
    if (!np || !ni)
    {
      free(np);
      free(ni);
      return false;
    }
    m->ptrs = np;
    m->indices = ni;
    m->capacity = new_cap;
  }
  m->ptrs[m->count] = c;
  m->indices[m->count] = idx;
  m->count++;
  return true;
}

static bool s_map_find(const MiMixChunkMap* m, const MiVmChunk* c, uint32_t* out_idx)
{
  for (size_t i = 0; i < m->count; ++i)
  {
    if (m->ptrs[i] == c)
    {
      *out_idx = m->indices[i];
      return true;
    }
  }
  return false;
}

static void s_map_destroy(MiMixChunkMap* m)
{
  free(m->ptrs);
  free(m->indices);
  memset(m, 0, sizeof(*m));
}

typedef struct MiMixChunkList
{
  const MiVmChunk** chunks;
  size_t            count;
  size_t            capacity;
} MiMixChunkList;

static bool s_list_push(MiMixChunkList* l, const MiVmChunk* c)
{
  if (l->count == l->capacity)
  {
    size_t new_cap = (l->capacity == 0) ? 16u : (l->capacity * 2u);
    const MiVmChunk** np = (const MiVmChunk**)realloc(l->chunks, new_cap * sizeof(*np));
    if (!np)
    {
      return false;
    }
    l->chunks = np;
    l->capacity = new_cap;
  }
  l->chunks[l->count++] = c;
  return true;
}

static void s_list_destroy(MiMixChunkList* l)
{
  free(l->chunks);
  memset(l, 0, sizeof(*l));
}

static bool s_collect_chunks_dfs(const MiVmChunk* root, MiMixChunkMap* map, MiMixChunkList* out)
{
  uint32_t existing = 0;
  if (s_map_find(map, root, &existing))
  {
    return true;
  }

  uint32_t idx = (uint32_t)out->count;
  if (!s_list_push(out, root))
  {
    return false;
  }
  if (!s_map_push(map, root, idx))
  {
    return false;
  }

  for (size_t i = 0; i < root->subchunk_count; ++i)
  {
    if (!s_collect_chunks_dfs(root->subchunks[i], map, out))
    {
      return false;
    }
  }
  return true;
}

//----------------------------------------------------------
// Save
//----------------------------------------------------------

static bool s_save_chunk(FILE* f, const MiVmChunk* c, const MiMixChunkMap* map)
{
  // Code
  if (c->code_count > 0xFFFFFFFFu)
  {
    return false;
  }
  if (!s_write_u32(f, (uint32_t)c->code_count))
  {
    return false;
  }
  if (!s_write_bytes(f, c->code, c->code_count * sizeof(MiVmIns)))
  {
    return false;
  }

  // Consts
  if (c->const_count > 0xFFFFFFFFu)
  {
    return false;
  }
  if (!s_write_u32(f, (uint32_t)c->const_count))
  {
    return false;
  }

  for (size_t i = 0; i < c->const_count; ++i)
  {
    MiRtValue v = c->consts[i];
    switch (v.kind)
    {
      case MI_RT_VAL_VOID:
        if (!s_write_u8(f, MI_MX_CONST_VOID)) return false;
        break;
      case MI_RT_VAL_INT:
        if (!s_write_u8(f, MI_MX_CONST_INT)) return false;
        if (!s_write_u64(f, (uint64_t)v.as.i)) return false;
        break;
      case MI_RT_VAL_FLOAT:
        if (!s_write_u8(f, MI_MX_CONST_FLOAT)) return false;
        if (!s_write_f64(f, v.as.f)) return false;
        break;
      case MI_RT_VAL_BOOL:
        if (!s_write_u8(f, MI_MX_CONST_BOOL)) return false;
        if (!s_write_u8(f, (uint8_t)(v.as.b ? 1 : 0))) return false;
        break;
      case MI_RT_VAL_STRING:
        if (!s_write_u8(f, MI_MX_CONST_STRING)) return false;
        if (!s_write_slice(f, v.as.s)) return false;
        break;
      default:
        return false;
    }
  }

  // Symbols
  if (c->symbol_count > 0xFFFFFFFFu)
  {
    return false;
  }
  if (!s_write_u32(f, (uint32_t)c->symbol_count))
  {
    return false;
  }
  for (size_t i = 0; i < c->symbol_count; ++i)
  {
    if (!s_write_slice(f, c->symbols[i]))
    {
      return false;
    }
  }

  // Commands
  if (c->cmd_count > 0xFFFFFFFFu)
  {
    return false;
  }
  if (!s_write_u32(f, (uint32_t)c->cmd_count))
  {
    return false;
  }
  for (size_t i = 0; i < c->cmd_count; ++i)
  {
    XSlice name = {0};
    if (c->cmd_names)
    {
      name = c->cmd_names[i];
    }
    if (!s_write_slice(f, name))
    {
      return false;
    }
  }

  // Subchunks
  if (c->subchunk_count > 0xFFFFFFFFu)
  {
    return false;
  }
  if (!s_write_u32(f, (uint32_t)c->subchunk_count))
  {
    return false;
  }
  for (size_t i = 0; i < c->subchunk_count; ++i)
  {
    uint32_t idx = 0;
    if (!s_map_find(map, c->subchunks[i], &idx))
    {
      return false;
    }
    if (!s_write_u32(f, idx))
    {
      return false;
    }
  }

  // Debug info: optional payload, but the presence byte is always encoded.
  // This keeps the stream layout stable across versions (version is only a
  // compatibility gate: file_version <= MI_MX_VERSION).
  {
    bool has_dbg = (c->dbg_name.length != 0u) || (c->dbg_file.length != 0u) || (c->dbg_lines != NULL) || (c->dbg_cols != NULL);
    if (!s_write_u8(f, (uint8_t)(has_dbg ? 1 : 0)))
    {
      return false;
    }

    if (has_dbg)
    {
      if (!s_write_slice(f, c->dbg_name)) return false;
      if (!s_write_slice(f, c->dbg_file)) return false;

      // Per-instruction line/column mapping. 
      for (size_t i = 0; i < c->code_count; ++i)
      {
        uint32_t line = c->dbg_lines ? c->dbg_lines[i] : 0u;
        if (!s_write_u32(f, line)) return false;
      }
      for (size_t i = 0; i < c->code_count; ++i)
      {
        uint32_t col = c->dbg_cols ? c->dbg_cols[i] : 0u;
        if (!s_write_u32(f, col)) return false;
      }
    }
  }

  return true;
}

bool mi_mx_save_file(const MiVmChunk* entry, const char* filename)
{
  if (!entry || !filename)
  {
    return false;
  }

  MiMixChunkMap map = {0};
  MiMixChunkList list = {0};
  if (!s_collect_chunks_dfs(entry, &map, &list))
  {
    s_map_destroy(&map);
    s_list_destroy(&list);
    return false;
  }

  FILE* f = fopen(filename, "wb");
  if (!f)
  {
    s_map_destroy(&map);
    s_list_destroy(&list);
    return false;
  }

  MiMixHeader h;
  h.magic[0] = (uint8_t)MI_MX_MAGIC_0;
  h.magic[1] = (uint8_t)MI_MX_MAGIC_1;
  h.magic[2] = (uint8_t)MI_MX_MAGIC_2;
  h.magic[3] = (uint8_t)MI_MX_MAGIC_3;
  h.version = MI_MX_VERSION;
  h.chunk_count = (uint32_t)list.count;
  h.entry_chunk_index = 0;

  bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
  if (ok)
  {
    for (size_t i = 0; i < list.count; ++i)
    {
      if (!s_save_chunk(f, list.chunks[i], &map))
      {
        ok = false;
        break;
      }
    }
  }

  fclose(f);
  s_map_destroy(&map);
  s_list_destroy(&list);
  return ok;
}

//----------------------------------------------------------
// Load
//----------------------------------------------------------

static bool s_load_chunk(FILE* f, XArena* arena, MiVmChunk* out, uint32_t version, uint32_t chunk_count, uint32_t** out_subidx)
{
  (void) version;
  memset(out, 0, sizeof(*out));

  // Code
  uint32_t code_n = 0;
  if (!s_read_u32(f, &code_n))
  {
    return false;
  }
  if (code_n)
  {
    out->code = (MiVmIns*)x_arena_alloc(arena, (size_t)code_n * sizeof(MiVmIns));
    if (!out->code)
    {
      return false;
    }
    if (!s_read_bytes(f, out->code, (size_t)code_n * sizeof(MiVmIns)))
    {
      return false;
    }
  }
  out->code_count = code_n;
  out->code_capacity = code_n;

  // Consts
  uint32_t const_n = 0;
  if (!s_read_u32(f, &const_n))
  {
    return false;
  }
  if (const_n)
  {
    out->consts = (MiRtValue*)x_arena_alloc_zero(arena, (size_t)const_n * sizeof(MiRtValue));
    if (!out->consts)
    {
      return false;
    }
  }
  out->const_count = const_n;
  out->const_capacity = const_n;

  for (uint32_t i = 0; i < const_n; ++i)
  {
    uint8_t kind = 0;
    if (!s_read_u8(f, &kind))
    {
      return false;
    }
    switch (kind)
    {
      case MI_MX_CONST_VOID:
        out->consts[i] = mi_rt_make_void();
        break;
      case MI_MX_CONST_INT:
        {
          uint64_t v = 0;
          if (!s_read_u64(f, &v)) return false;
          out->consts[i] = mi_rt_make_int((long long)v);
        } break;
      case MI_MX_CONST_FLOAT:
        {
          double v = 0;
          if (!s_read_f64(f, &v)) return false;
          out->consts[i] = mi_rt_make_float(v);
        } break;
      case MI_MX_CONST_BOOL:
        {
          uint8_t b = 0;
          if (!s_read_u8(f, &b)) return false;
          out->consts[i] = mi_rt_make_bool(b != 0);
        } break;
      case MI_MX_CONST_STRING:
        {
          XSlice s;
          if (!s_read_slice(f, arena, &s)) return false;
          out->consts[i] = mi_rt_make_string_slice(s);
        } break;
      default:
        return false;
    }
  }

  // Symbols
  uint32_t sym_n = 0;
  if (!s_read_u32(f, &sym_n))
  {
    return false;
  }
  if (sym_n)
  {
    out->symbols = (XSlice*)x_arena_alloc_zero(arena, (size_t)sym_n * sizeof(XSlice));
    if (!out->symbols)
    {
      return false;
    }
  }
  out->symbol_count = sym_n;
  out->symbol_capacity = sym_n;

  for (uint32_t i = 0; i < sym_n; ++i)
  {
    if (!s_read_slice(f, arena, &out->symbols[i]))
    {
      return false;
    }
  }

  // Cmd names
  uint32_t cmd_n = 0;
  if (!s_read_u32(f, &cmd_n))
  {
    return false;
  }
  if (cmd_n)
  {
    out->cmd_names = (XSlice*)x_arena_alloc_zero(arena, (size_t)cmd_n * sizeof(XSlice));
    out->cmd_targets = (MiRtCmd**)x_arena_alloc_zero(arena, (size_t)cmd_n * sizeof(MiRtCmd*));
    if (!out->cmd_names || !out->cmd_targets)
    {
      return false;
    }
  }
  out->cmd_count = cmd_n;
  out->cmd_capacity = cmd_n;

  for (uint32_t i = 0; i < cmd_n; ++i)
  {
    if (!s_read_slice(f, arena, &out->cmd_names[i]))
    {
      return false;
    }
  }

  // Subchunks (indices first; pointers fixed later)
  uint32_t sub_n = 0;
  if (!s_read_u32(f, &sub_n))
  {
    return false;
  }
  if (sub_n)
  {
    out->subchunks = (MiVmChunk**)x_arena_alloc_zero(arena, (size_t)sub_n * sizeof(MiVmChunk*));
    if (!out->subchunks)
    {
      return false;
    }
  }
  out->subchunk_count = sub_n;
  out->subchunk_capacity = sub_n;

  uint32_t* idxs = NULL;
  if (sub_n)
  {
    idxs = (uint32_t*)x_arena_alloc_zero(arena, (size_t)sub_n * sizeof(uint32_t));
    if (!idxs)
    {
      return false;
    }
  }

  for (uint32_t i = 0; i < sub_n; ++i)
  {
    if (!s_read_u32(f, &idxs[i]))
    {
      return false;
    }
    if (idxs[i] >= chunk_count)
    {
      return false;
    }
  }

  // Debug info: always encoded as a presence byte + optional payload.
  // Do not gate this on file version; version is only used as a compatibility
  // check (file_version <= MI_MX_VERSION).
  {
    uint8_t has_dbg = 0;
    if (!s_read_u8(f, &has_dbg))
    {
      return false;
    }

    if (has_dbg != 0u && has_dbg != 1u)
    {
      return false;
    }

    if (has_dbg)
    {
      if (!s_read_slice(f, arena, &out->dbg_name)) return false;
      if (!s_read_slice(f, arena, &out->dbg_file)) return false;

      if (out->code_count)
      {
        out->dbg_lines = (uint32_t*)x_arena_alloc_zero(arena, out->code_count * sizeof(uint32_t));
        out->dbg_cols = (uint32_t*)x_arena_alloc_zero(arena, out->code_count * sizeof(uint32_t));
        if (!out->dbg_lines || !out->dbg_cols)
        {
          return false;
        }

        for (size_t i = 0; i < out->code_count; ++i)
        {
          if (!s_read_u32(f, &out->dbg_lines[i])) return false;
        }
        for (size_t i = 0; i < out->code_count; ++i)
        {
          if (!s_read_u32(f, &out->dbg_cols[i])) return false;
        }
        out->dbg_capacity = out->code_count;
      }
    }
  }

  *out_subidx = idxs;
  return true;
}

bool mi_mx_load_file(MiVm* vm, const char* filename, MiMixProgram* out_program)
{
  if (!vm || !filename || !out_program)
  {
    return false;
  }

  memset(out_program, 0, sizeof(*out_program));

  FILE* f = fopen(filename, "rb");
  if (!f)
  {
    return false;
  }

  MiMixHeader h;
  if (fread(&h, sizeof(h), 1, f) != 1)
  {
    fclose(f);
    return false;
  }

  if (h.magic[0] != MI_MX_MAGIC_0 || h.magic[1] != MI_MX_MAGIC_1 || h.magic[2] != MI_MX_MAGIC_2 || h.magic[3] != MI_MX_MAGIC_3)
  {
    fclose(f);
    return false;
  }
  if (h.version < 1u || h.version > MI_MX_VERSION)
  {
    fclose(f);
    return false;
  }
  if (h.chunk_count == 0 || h.entry_chunk_index >= h.chunk_count)
  {
    fclose(f);
    return false;
  }

  XArena* arena = x_arena_create(64u * 1024u);
  if (!arena)
  {
    fclose(f);
    return false;
  }

  MiVmChunk** chunks = (MiVmChunk**)x_arena_alloc_zero(arena, (size_t)h.chunk_count * sizeof(MiVmChunk*));
  uint32_t** subidx = (uint32_t**)x_arena_alloc_zero(arena, (size_t)h.chunk_count * sizeof(uint32_t*));
  if (!chunks || !subidx)
  {
    x_arena_destroy(arena);
    fclose(f);
    return false;
  }

  bool ok = true;
  for (uint32_t i = 0; i < h.chunk_count; ++i)
  {
    chunks[i] = (MiVmChunk*)x_arena_alloc_zero(arena, sizeof(MiVmChunk));
    if (!chunks[i])
    {
      ok = false;
      break;
    }
    if (!s_load_chunk(f, arena, chunks[i], h.version, h.chunk_count, &subidx[i]))
    {
      ok = false;
      break;
    }
  }

  if (ok)
  {
    // Fix subchunk pointers.
    for (uint32_t i = 0; i < h.chunk_count; ++i)
    {
      MiVmChunk* c = chunks[i];
      for (size_t j = 0; j < c->subchunk_count; ++j)
      {
        c->subchunks[j] = chunks[subidx[i][j]];
      }
    }

    // Resolve command callables by name (best-effort for qualified names).
    for (uint32_t i = 0; i < h.chunk_count; ++i)
    {
      MiVmChunk* c = chunks[i];
      if (!mi_vm_link_chunk_commands(vm, c))
      {
        ok = false;
        break;
      }
    }
  }

  fclose(f);

  if (!ok)
  {
    x_arena_destroy(arena);
    return false;
  }

  out_program->arena = arena;
  out_program->chunks = chunks;
  out_program->chunk_count = h.chunk_count;
  out_program->entry = chunks[h.entry_chunk_index];
  return true;
}

void mi_mx_program_destroy(MiMixProgram* p)
{
  if (!p)
  {
    return;
  }

  if (p->arena)
  {
    x_arena_destroy(p->arena);
  }
  memset(p, 0, sizeof(*p));
}
