#ifndef MI_COMPILE_H
#define MI_COMPILE_H

#include <stdbool.h>
#include <stddef.h>

#include <stdx_arena.h>
#include <stdx_string.h>

#include "mi_bytecode.h" /* Legacy compiler pipeline (MiProgram). */
#include "mi_vm.h"       /* VM chunk format + command table (MiVmChunk). */

//----------------------------------------------------------
// Compiler
//----------------------------------------------------------

typedef struct MiCompiler
{
  XArena*  arena;
  size_t   program_arena_chunk_size;
  uint8_t next_temp_reg;
} MiCompiler;

void mi_compiler_init(MiCompiler* c, size_t arena_chunk_size);
void mi_compiler_shutdown(MiCompiler* c);
bool mi_compile_script(MiCompiler* c, XSlice source, MiProgram* out_prog);

/* Compile an already-parsed AST script into VM bytecode (MiVmChunk).
   The compiler will deep-copy any strings/constants into chunk-owned memory.
   The returned chunk must be destroyed with mi_vm_chunk_destroy(). */
MiVmChunk* mi_compile_vm_script(MiVm* vm, const MiScript* script);

#endif
