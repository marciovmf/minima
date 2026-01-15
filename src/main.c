#include <stdx_common.h>
#define X_IMPL_STRBUILDER
#define X_IMPL_STRING
#define X_IMPL_ARENA
#define X_IMPL_IO
#define X_IMPL_LOG
#include <stdx_strbuilder.h>
#include <stdx_string.h>
#include <stdx_arena.h>
#include <stdx_io.h>
#include <stdx_log.h>

#include <stdlib.h>
#include <string.h>
#include "minima.h"

int test_compiler(XSlice source)
{
  MiCompiler  c;
  MiProgram   out;
  mi_compiler_init(&c, 4 * 1024);
  bool success = mi_compile_script(&c, source, &out);

  mi_disasm_program(stdout, &out);

  mi_compiler_shutdown(&c);
  return success ? 0 : 1;
}

int test_vm_eval(XSlice source)
{
  XArena* arena = x_arena_create(1024 * 32);
  if (!arena)
  {
    mi_error("Failed to create arena\n");
    return 1;
  }

  //MiParseResult res = mi_parse_program(source.ptr, source.length, arena);
  //if (!res.ok || !res.script)
  //{
  //  x_arena_destroy(arena);
  //  return 1;
  //}

  MiRuntime rt;
  mi_rt_init(&rt);

  MiVm vm;
  mi_vm_init(&vm, &rt);

  MiVmChunk* ch = mi_vm_compile_script(&vm, source);
  mi_vm_disasm(ch);
  (void)mi_vm_execute(&vm, ch);
  mi_vm_chunk_destroy(ch);

  mi_vm_shutdown(&vm);
  mi_rt_shutdown(&rt);
  x_arena_destroy(arena);
  return 0;
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    mi_error_fmt("Usage: %s <script.min>\n", argv[0]);
    return 1;
  }

  const char* filename = argv[1];
  size_t src_len = 0;
  char *src = x_io_read_text(filename, &src_len);
  if (!src) { return 1; }

  XSlice source = x_slice_init(src, src_len);

  int result = 0;
  if (argc >= 3 && strcmp(argv[2], "--vm") == 0)
  {
    result = test_vm_eval(source);
  }
  else
  {
    result = test_compiler(source);
  }

  if (result == 0)
    mi_info("Success\n");
  else
    mi_error("Fail\n");

  free(src);
  return result;
}
