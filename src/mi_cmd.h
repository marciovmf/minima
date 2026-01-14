#ifndef MI_CMD_H
#define MI_CMD_H

#include "mi_runtime.h"

/* Register the default builtin commands (set/print/if/while/...). */
void mi_cmd_register_builtins(MiRuntime* rt);
bool mi_cmd_register(MiRuntime* rt, const char* name, MiRtCommandFn fn);
MiRtCommandFn mi_cmd_find(MiRuntime* rt, XSlice name);

#endif //MI_CMD_H

