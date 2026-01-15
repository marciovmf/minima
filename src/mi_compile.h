#ifndef MI_COMPILE_H
#define MI_COMPILE_H

#include <stdbool.h>
#include <stddef.h>

#include <stdx_arena.h>
#include <stdx_string.h>

#include "mi_bytecode.h"

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

#endif
