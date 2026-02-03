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

//----------------------------------------------------------
// main
//----------------------------------------------------------

int main(int argc, char** argv)
{
  return minima_cli_main(argc, argv);
}
