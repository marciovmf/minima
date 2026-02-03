#include <stdx_common.h>
#include <stdx_io.h>
#include <stdlib.h>
#include <string.h>
#include "minima.h"
#include "mi_cli.h"

//----------------------------------------------------------
// CLI helpers
//----------------------------------------------------------

static void s_usage(const char* exe)
{

  mi_error_fmt(
      "minima v%d.%d.%d\n"
      "Usage:\n"
      "  %s [--cache-dir <dir>] -c <file.min> [out.mx]   Compile only (default out = file.min.mx)\n"
      "  %s [--cache-dir <dir>] -d <file.mi|file.mx>      Disassemble (compile if needed)\n"
      "  %s [--cache-dir <dir>] <file.min>               Compile and run\n"
      "  %s [--cache-dir <dir>] <file.mx>                Run MIX file\n",
      MINIMA_VERSION_MAJOR,
      MINIMA_VERSION_PATCH,
      MINIMA_VERSION_MINOR,
      exe, exe, exe, exe);
}

static bool s_has_ext(const char* path, const char* ext)
{
  size_t lp = strlen(path);
  size_t le = strlen(ext);
  if (lp < le)
  {
    return false;
  }
  return strcmp(path + (lp - le), ext) == 0;
}

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

static int s_cmd_compile_only(const char* in_file, const char* out_file, const char* cache_dir)
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

static int s_cmd_disasm(const char* mx_file, const char* cache_dir)
{
  MiRuntime rt;
  mi_rt_init(&rt);

  MiVm vm;
  mi_vm_init(&vm, &rt);
  mi_vm_set_cache_dir(&vm, cache_dir);

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

static int s_cmd_disasm_mi(const char* mi_file, const char* cache_dir)
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
    int rc = s_cmd_compile_only(mi_file, cached_mx.buf, cache_dir);
    if (rc != 0)
    {
      return rc;
    }
  }

  return s_cmd_disasm(cached_mx.buf, cache_dir);
}

static int s_cmd_run_source(const char* mi_file, const char* cache_dir)
{
  MiRuntime rt;
  mi_rt_init(&rt);

  MiVm vm;
  mi_vm_init(&vm, &rt);
  mi_vm_set_cache_dir(&vm, cache_dir);

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

  (void)mi_vm_execute(&vm, ch);
  mi_vm_chunk_destroy(ch);

  mi_vm_shutdown(&vm);
  mi_rt_shutdown(&rt);
  return 0;
}

static int s_cmd_run_mix(const char* mx_file, const char* cache_dir)
{
  MiRuntime rt;
  mi_rt_init(&rt);

  MiVm vm;
  mi_vm_init(&vm, &rt);
  mi_vm_set_cache_dir(&vm, cache_dir);

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

//----------------------------------------------------------
// main
//----------------------------------------------------------

int minima_cli_main(int argc, char** argv)
{
  if (argc < 2)
  {
    s_usage(argv[0]);
    return 1;
  }

  const char* cache_dir = NULL;
  int argi = 1;
  if (argi + 2 <= argc && strcmp(argv[argi], "--cache-dir") == 0)
  {
    cache_dir = argv[argi + 1];
    argi += 2;
  }

  int rem = argc - argi;
  char** args = &argv[argi];
  if (rem < 1)
  {
    s_usage(argv[0]);
    return 1;
  }

  // Compile only
  if (strcmp(args[0], "-c") == 0)
  {
    if (rem != 2 && rem != 3)
    {
      s_usage(argv[0]);
      return 1;
    }

    const char* in_file = args[1];
    XFSPath out_file;

    if (rem == 3)
    {
      x_fs_path(&out_file, args[2]);
    }
    else
    {
      x_fs_path(&out_file, in_file);
      x_fs_path_change_extension(&out_file, ".mx");
    }

    int r = s_cmd_compile_only(in_file, out_file.buf, cache_dir);
    return r;
  }

  // Disassemble
  if (strcmp(args[0], "-d") == 0)
  {
    if (rem != 2)
    {
      s_usage(argv[0]);
      return 1;
    }

    const char* in_file = args[1];
    if (s_has_ext(in_file, ".mi"))
    {
      return s_cmd_disasm_mi(in_file, cache_dir);
    }
    return s_cmd_disasm(in_file, cache_dir);
  }

  // Single arg: compile+run or run MIX
  if (rem == 1)
  {
    const char* path = args[0];
    if (s_has_ext(path, ".mx"))
    {
      return s_cmd_run_mix(path, cache_dir);
    }
    return s_cmd_run_source(path, cache_dir);
  }

  s_usage(argv[0]);
  return 1;
}
