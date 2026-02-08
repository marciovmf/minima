#include <stdx_common.h>
#include <stdx_io.h>
#include <stdlib.h>
#include <string.h>
#include "minima.h"
#include "mi_vm.h"
#include "stdx_filesystem.h"

//----------------------------------------------------------
// CLI helpers
//----------------------------------------------------------

static bool s_get_cache_root(const char* cache_dir_opt, XFSPath* out_root)
{
  if (!out_root)
  {
    return false;
  }

  (void)x_fs_path_set(out_root, "");

  if (cache_dir_opt && cache_dir_opt[0])
  {
    (void)x_fs_path_set(out_root, cache_dir_opt);
    (void)x_fs_path_normalize(out_root);
    return true;
  }

#ifdef _WIN32
  {
    const char* app = getenv("LOCALAPPDATA");
    if (app && app[0])
    {
      (void)x_fs_path_set(out_root, app);
      (void)x_fs_path_join(out_root, "minima");
      return true;
    }
  }
#else
  {
    const char* xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0])
    {
      (void)x_fs_path_set(out_root, xdg);
      (void)x_fs_path_join(out_root, "minima");
      return true;
    }

    const char* home = getenv("HOME");
    if (home && home[0])
    {
      (void)x_fs_path_set(out_root, home);
      (void)x_fs_path_join(out_root, ".cache", "minima");
      return true;
    }
  }
#endif

  if (!x_fs_get_temp_folder(out_root))
  {
    return false;
  }
  (void)x_fs_path_join(out_root, "minima");
  return true;
}

static void s_hex_u64(char out[17], uint64_t v)
{
  static const char* k_hex = "0123456789abcdef";
  for (int i = 0; i < 16; ++i)
  {
    int shift = (15 - i) * 4;
    out[i] = k_hex[(v >> shift) & 0xFULL];
  }
  out[16] = 0;
}

static bool s_cached_mx_for_mi(const char* cache_dir_opt, const char* src_mi, XFSPath* out_mx)
{
  if (!src_mi || !out_mx)
  {
    return false;
  }

  XFSPath cache_root;
  if (!s_get_cache_root(cache_dir_opt, &cache_root))
  {
    return false;
  }

  (void)x_fs_directory_create_recursive(cache_root.buf);

  uint64_t h = x_cstr_hash(src_mi);
  char hex[17];
  s_hex_u64(hex, h);

  XFSPath cache_dir = cache_root;
  (void)x_fs_path_join(&cache_dir, hex);
  (void)x_fs_directory_create_recursive(cache_dir.buf);

  XFSPath mx_name;
  {
    XSlice base = x_fs_path_basename(src_mi);
    XSmallstr tmp;
    (void)x_smallstr_from_slice(base, &tmp);
    x_fs_path_set(&mx_name, x_smallstr_cstr(&tmp));
  }
  (void)x_fs_path_change_extension(&mx_name, ".mx");

  *out_mx = cache_dir;
  (void)x_fs_path_join(out_mx, mx_name.buf);
  return true;
}


//----------------------------------------------------------
// Actions
//----------------------------------------------------------

static const char* s_get_modules_dir()
{
  static XFSPath s_modules_dir = {0};
  if (s_modules_dir.length == 0)
  {
    XFSPath exe_path = {0};
    x_fs_path_from_executable(&exe_path);
    x_fs_path_from_slice(x_fs_path_dirname(exe_path.buf), &s_modules_dir);
    x_fs_path_join(&s_modules_dir, "module");
  }
  return s_modules_dir.buf;
}

int mi_compile_only(const char* in_file, const char* out_file, const char* cache_dir)
{
  size_t src_len = 0;
  char* src = x_io_read_text(in_file, &src_len);
  if (!src)
  {
    mi_error_fmt("Failed to read: %s\n", in_file);
    return 1;
  }

  XSlice source = x_slice_init(src, src_len);

  MiRuntime rt;
  mi_rt_init(&rt);

  MiVm vm;
  mi_vm_init(&vm, &rt);
  mi_vm_set_cache_dir(&vm, cache_dir);
  mi_vm_set_modules_dir(&vm, s_get_modules_dir());

  XArena* arena = x_arena_create(1024 * 64);
  if (!arena)
  {
    mi_error("Failed to create parser arena\n");
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    free(src);
    return 1;
  }

  MiParseResult res = mi_parse_program_ex(source.ptr, source.length, arena, true);
  if (!res.ok || !res.script)
  {
    mi_parse_print_error(source, &res);
    x_arena_destroy(arena);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    free(src);
    return 1;
  }

  MiVmChunk* ch = mi_compile_vm_script_ex(&vm, res.script, x_slice_from_cstr("<script>"), x_slice_from_cstr(in_file));
  x_arena_destroy(arena);
  if (!ch)
  {
    mi_error_fmt("Compilation failed: %s\n", in_file);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    free(src);
    return 1;
  }

  if (!mi_mx_save_file(ch, out_file))
  {
    mi_error_fmt("Failed to write MIX file: %s\n", out_file);
    mi_vm_chunk_destroy(ch);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    free(src);
    return 1;
  }

  mi_vm_chunk_destroy(ch);
  mi_vm_shutdown(&vm);
  mi_rt_shutdown(&rt);
  free(src);
  return 0;
}

int mi_disasm(const char* mx_file, const char* cache_dir)
{
  MiRuntime rt;
  mi_rt_init(&rt);

  MiVm vm;
  mi_vm_init(&vm, &rt);
  mi_vm_set_cache_dir(&vm, cache_dir);
  mi_vm_set_modules_dir(&vm, s_get_modules_dir());

  MiMixProgram p;
  if (!mi_mx_load_file(&vm, mx_file, &p))
  {
    mi_error_fmt("Failed to load MIX file: %s\n", mx_file);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    return 1;
  }

  mi_vm_disasm(p.entry);

  mi_mx_program_destroy(&p);
  mi_vm_shutdown(&vm);
  mi_rt_shutdown(&rt);
  return 0;
}

int mi_disasm_mi(const char* mi_file, const char* cache_dir)
{
  XFSPath cached_mx;
  if (!s_cached_mx_for_mi(cache_dir, mi_file, &cached_mx))
  {
    mi_error_fmt("Failed to resolve cache path for: %s\n", mi_file);
    return 1;
  }

  bool mx_exists = x_fs_is_file(cached_mx.buf);

  time_t mi_time = 0;
  time_t mx_time = 0;
  bool have_mi = x_fs_file_modification_time(mi_file, &mi_time);
  bool have_mx = mx_exists && x_fs_file_modification_time(cached_mx.buf, &mx_time);

  if (!have_mi)
  {
    mi_error_fmt("Failed to stat: %s\n", mi_file);
    return 1;
  }

  if (!have_mx || mi_time > mx_time)
  {
    int rc = mi_compile_only(mi_file, cached_mx.buf, cache_dir);
    if (rc != 0)
    {
      return rc;
    }
  }

  return mi_disasm(cached_mx.buf, cache_dir);
}

int mi_run_source(const char* mi_file, const char* cache_dir)
{
  MiRuntime rt;
  mi_rt_init(&rt);

  MiVm vm;
  mi_vm_init(&vm, &rt);
  mi_vm_set_cache_dir(&vm, cache_dir);
  mi_vm_set_modules_dir(&vm, s_get_modules_dir());

  // Try cached .mx first
  XFSPath cached_mx;
  bool have_cached_path = s_cached_mx_for_mi(cache_dir, mi_file, &cached_mx);

  if (have_cached_path && x_fs_is_file(cached_mx.buf))
  {
    time_t mi_time = 0;
    time_t mx_time = 0;

    bool have_mi_time = x_fs_file_modification_time(mi_file, &mi_time);
    bool have_mx_time = x_fs_file_modification_time(cached_mx.buf, &mx_time);

    uint32_t mx_version = 0;
    bool have_ver = mi_mx_peek_file_version(cached_mx.buf, &mx_version);
    bool compatible = have_ver && (mx_version >= 1u) && (mx_version <= MI_MX_VERSION);

    bool mx_is_fresh = have_mi_time && have_mx_time && (mx_time >= mi_time);

    if (compatible && mx_is_fresh)
    {
      MiMixProgram p;
      memset(&p, 0, sizeof(p));
      if (mi_mx_load_file(&vm, cached_mx.buf, &p))
      {
        (void)mi_vm_execute(&vm, p.entry);
        mi_mx_program_destroy(&p);

        mi_vm_shutdown(&vm);
        mi_rt_shutdown(&rt);
        return 0;
      }
    }
  }

  // Fall back to compile source
  size_t src_len = 0;
  char* src = x_io_read_text(mi_file, &src_len);
  if (!src)
  {
    mi_error_fmt("Failed to read: %s\n", mi_file);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    return 1;
  }

  XSlice source = x_slice_init(src, src_len);

  XArena* arena = x_arena_create(1024 * 64);
  if (!arena)
  {
    mi_error("Failed to create parser arena\n");
    free(src);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    return 1;
  }

  MiParseResult res = mi_parse_program_ex(source.ptr, source.length, arena, true);
  if (!res.ok || !res.script)
  {
    mi_parse_print_error(source, &res);
    x_arena_destroy(arena);
    free(src);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    return 1;
  }

  MiVmChunk* ch = mi_compile_vm_script_ex(&vm,
      res.script,
      x_slice_from_cstr("<script>"),
      x_slice_from_cstr(mi_file));

  x_arena_destroy(arena);
  free(src);

  if (!ch)
  {
    mi_error_fmt("Compilation failed: %s\n", mi_file);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    return 1;
  }

  // Save to cache (best-effort)
  if (have_cached_path)
  {
    (void)mi_mx_save_file(ch, cached_mx.buf);
  }

  if (!mi_vm_link_chunk_commands(&vm, ch))
  {
    mi_error("Link failed: unresolved command(s)\n");
    mi_vm_chunk_destroy(ch);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    return 1;
  }

  (void)mi_vm_execute(&vm, ch);
  mi_vm_chunk_destroy(ch);

  mi_vm_shutdown(&vm);
  mi_rt_shutdown(&rt);
  return 0;
}

int mi_run_mx(const char* mx_file, const char* cache_dir)
{
  MiRuntime rt;
  mi_rt_init(&rt);

  MiVm vm;
  mi_vm_init(&vm, &rt);
  mi_vm_set_cache_dir(&vm, cache_dir);

  mi_vm_set_modules_dir(&vm, s_get_modules_dir());

  MiMixProgram p;
  if (!mi_mx_load_file(&vm, mx_file, &p))
  {
    mi_error_fmt("Failed to load MIX file: %s\n", mx_file);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    return 1;
  }

  (void)mi_vm_execute(&vm, p.entry);

  mi_mx_program_destroy(&p);
  mi_vm_shutdown(&vm);
  mi_rt_shutdown(&rt);
  return 0;
}
