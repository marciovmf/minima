#ifndef MI_MIX_H
#define MI_MIX_H

#include <stdbool.h>

#include <stdx_string.h>

#include "mi_vm.h"

//----------------------------------------------------------
// MIX - MInima eXecutable
//----------------------------------------------------------

typedef struct MiMixProgram
{
  MiVmChunk* entry;

  /* Owns all allocations made when loading. */
  MiVmChunk** chunks;
  size_t      chunk_count;
} MiMixProgram;

/* Save a compiled VM chunk to disk as a MIX file.
   Returns false on I/O error or unsupported constant type. */
bool mi_mix_save_file(const MiVmChunk* entry, const char* filename);

/* Load a MIX program from disk.
   - Resolves command functions using vm->commands by name.
   - Allocates owned memory; must be freed by mi_mix_program_destroy.
*/
bool mi_mix_load_file(MiVm* vm, const char* filename, MiMixProgram* out_program);

/* Free all memory owned by a loaded MIX program. */
void mi_mix_program_destroy(MiMixProgram* p);


#endif // MI_MIX_H
