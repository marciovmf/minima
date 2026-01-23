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
      "  %s -c <file.min> [out.mx]   Compile only (default out = file.min.mx)\n"
      "  %s -d <file.mx>             Disassemble MIX file\n"
      "  %s <file.min>               Compile and run\n"
      "  %s <file.mx>                Run MIX file\n",
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

static int s_cmd_compile_only(const char* in_file, const char* out_file)
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
    mi_error_fmt("Parse error %d,%d - %.*s\n",
        res.error_line,
        res.error_column,
        (int)res.error_message.length,
        res.error_message.ptr);
    x_arena_destroy(arena);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    free(src);
    return 1;
  }

  MiVmChunk* ch = mi_compile_vm_script(&vm, res.script);
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

static int s_cmd_disasm(const char* mx_file)
{
  MiRuntime rt;
  mi_rt_init(&rt);

  MiVm vm;
  mi_vm_init(&vm, &rt);

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

static int s_cmd_run_source(const char* mi_file)
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
    mi_error_fmt("Parse error %d,%d - %.*s\n",
        res.error_line,
        res.error_column,
        (int)res.error_message.length,
        res.error_message.ptr);
    x_arena_destroy(arena);
    mi_vm_shutdown(&vm);
    mi_rt_shutdown(&rt);
    free(src);
    return 1;
  }

  MiVmChunk* ch = mi_compile_vm_script(&vm, res.script);
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

static int s_cmd_run_mix(const char* mx_file)
{
  MiRuntime rt;
  mi_rt_init(&rt);

  MiVm vm;
  mi_vm_init(&vm, &rt);

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

  // Compile only
  if (strcmp(argv[1], "-c") == 0)
  {
    if (argc != 3 && argc != 4)
    {
      s_usage(argv[0]);
      return 1;
    }

    const char* in_file = argv[2];
    XFSPath out_file;

    if (argc == 4)
    {
      x_fs_path(&out_file, argv[3]);
    }
    else
    {
      x_fs_path(&out_file, in_file);
      x_fs_path_change_extension(&out_file, ".mx");
    }

    int r = s_cmd_compile_only(in_file, out_file.buf);
    return r;
  }

  // Disassemble
  if (strcmp(argv[1], "-d") == 0)
  {
    if (argc != 3)
    {
      s_usage(argv[0]);
      return 1;
    }
    return s_cmd_disasm(argv[2]);
  }

  // Single arg: compile+run or run MIX
  if (argc == 2)
  {
    const char* path = argv[1];
    if (s_has_ext(path, ".mx"))
    {
      return s_cmd_run_mix(path);
    }
    return s_cmd_run_source(path);
  }

  s_usage(argv[0]);
  return 1;
}
