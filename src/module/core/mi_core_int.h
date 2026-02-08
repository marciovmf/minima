#ifndef MI_LIB_INT_H
#define MI_LIB_INT_H

#include "mi_vm.h"

/* Register int library commands (pure helpers).
   This intentionally excludes VM core commands like cmd/include/set. */
bool mi_lib_int_register(MiVm* vm, MiRtValue ns_block);

#endif
