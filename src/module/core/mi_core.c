/**
 * Entrypoint for the minima's core module
 */

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

#include <mi_vm.h>
#include <mi_runtime.h>
#include <string.h>
#include "mi_core_int.h"
#include "mi_core_float.h"

X_PLAT_EXPORT uint32_t mi_module_count(void)
{
  return 2;
}

X_PLAT_EXPORT const char* mi_module_name(uint32_t index)
{
  static const char* s_names[] = { "int", "float" };
  if (index >= 2)
  {
    return NULL;
  }
  return s_names[index];
}

X_PLAT_EXPORT bool mi_module_register(MiVm* vm, const char* module_name, MiRtValue ns_block)
{
  if (strcmp(module_name, "int") == 0)
  {
    return mi_lib_int_register(vm, ns_block);
  }

  if (strcmp(module_name, "float") == 0)
  {
    return mi_lib_float_register(vm, ns_block);
  }

  return false;
}
