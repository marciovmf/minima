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

#include "minima.h"

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

    int r = mi_compile_only(in_file, out_file.buf, cache_dir);
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
    if (x_cstr_ends_with(in_file, ".mi"))
    {
      return mi_disasm_mi(in_file, cache_dir);
    }
    return mi_disasm(in_file, cache_dir);
  }

  // Single arg: compile+run or run MIX
  if (rem == 1)
  {
    const char* path = args[0];
    if (x_cstr_ends_with(path, ".mx"))
    {
      return mi_run_mx(path, cache_dir);
    }
    return mi_run_source(path, cache_dir);
  }

  s_usage(argv[0]);
  return 1;
}
