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

#include "minima.h"

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

  XArena *arena = x_arena_create(1024 * 16);
  if (!arena)
  {
    mi_error("Failed to create arena\n");
    free(src);
    return 1;
  }

  printf("-------------------- AST -----------------------\n");
  MiParseResult res = mi_parse_program(src, src_len, arena);
  mi_ast_debug_print_script(res.script);

  printf("------------------ EXECUTION -------------------\n");
  if (!res.ok)
  {
    x_arena_destroy(arena);
    free(src);
    return 1;
  }

  MiRuntime rt;
  mi_rt_init(&rt);
  mi_cmd_register_builtins(&rt);
  mi_ast_backend_bind(&rt);
  (void)mi_eval_script_ast(&rt, res.script);
  mi_rt_shutdown(&rt);

  x_arena_destroy(arena);
  free(src);
  return 0;
}
