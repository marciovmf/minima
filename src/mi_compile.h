#ifndef MI_COMPILE_H
#define MI_COMPILE_H

#include <stdx_string.h>

#include "mi_vm.h"

typedef struct MiScript MiScript;

/**
 * Compile an already-parsed AST script into VM bytecode (MiVmChunk).
 * The compiler will deep-copy any strings/constants into chunk-owned memory.
 * The returned chunk must be destroyed with mi_vm_chunk_destroy().
 */
MiVmChunk* mi_compile_vm_script(MiVm* vm, const MiScript* script);

/* Compile with debug name/file attached to the resulting chunk.
 * - dbg_name is shown in disassembly/trace (e.g. "<script>", "<block>").
 * - dbg_file should be the source filename (or module name).
 * Pass empty slices to omit.
 */
MiVmChunk* mi_compile_vm_script_ex(MiVm* vm, const MiScript* script, XSlice dbg_name, XSlice dbg_file);

#endif // MI_COMPILE_H
