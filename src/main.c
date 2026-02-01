#include <stdx_common.h>

#define X_IMPL_STRBUILDER
#define X_IMPL_STRING
#define X_IMPL_ARENA
#define X_IMPL_IO
#define X_IMPL_LOG
#define X_IMPL_FILESYSTEM

#include <stdx_strbuilder.h>
#include <stdx_string.h>
#include <stdx_filesystem.h>
#include <stdx_arena.h>
#include <stdx_io.h>
#include <stdx_log.h>

#include <stdlib.h>
#include <string.h>

#include "minima.h"

//----------------------------------------------------------
// CLI helpers
//----------------------------------------------------------

static void s_usage(const char* exe)
{
  mi_error_fmt(
      "Usage:\n"
      "  %s [--cache-dir <dir>] -c <file.min> [out.mx]   Compile only (default out = file.min.mx)\n"
      "  %s [--cache-dir <dir>] -d <file.mx>             Disassemble MIX file\n"
      "  %s [--cache-dir <dir>] <file.min>               Compile and run\n"
      "  %s [--cache-dir <dir>] <file.mx>                Run MIX file\n",
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

static int s_cmd_run_source(const char* mi_file, const char* cache_dir)
{
  size_t src_len = 0;
  char* src = x_io_read_text(mi_file, &src_len);
  if (!src)
  {
    mi_error_fmt("Failed to read: %s\n", mi_file);
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

  MiVmChunk* ch = mi_compile_vm_script_ex(&vm,
      res.script,
      x_slice_from_cstr("<script>"),
      x_slice_from_cstr(mi_file));

  x_arena_destroy(arena);
  if (!ch)
  {
    mi_error_fmt("Compilation failed: %s\n", mi_file);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    free(src);
    return 1;
  }

  (void)mi_vm_execute(&vm, ch);
  mi_vm_chunk_destroy(ch);

  mi_vm_shutdown(&vm);
  mi_rt_shutdown(&rt);
  free(src);
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

int main(int argc, char** argv)
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
    return s_cmd_disasm(args[1], cache_dir);
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
