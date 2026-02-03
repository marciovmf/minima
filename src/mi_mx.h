#ifndef MI_MX_H
#define MI_MX_H

#include <stdbool.h>
#include <stdx_string.h>
#include "mi_vm.h"
#include "mi_version.h"

//----------------------------------------------------------
// MIX - MInima eXecutable
//----------------------------------------------------------

// On-disk MX format version. Should be compatible with major versions.
#define MI_MX_VERSION MINIMA_VERSION_MAJOR

typedef struct MiMixProgram
{
  MiVmChunk* entry;
  /* Owns all allocations made when loading. */
  MiVmChunk** chunks;
  size_t      chunk_count;
} MiMixProgram;

/**
 * Save a compiled VM chunk to disk as a MX file.
 * Returns false on I/O error or unsupported constant type.
 */
bool mi_mx_save_file(const MiVmChunk* entry, const char* filename);

/**
 * Read MX header and return its version.
 * Returns false if the file is not a valid MX file or can't be read.
 */
bool mi_mx_peek_file_version(const char* filename, uint32_t* out_version);

/**
 * Load a MIX program from disk.
 * - Resolves command functions using vm->commands by name.
 *   - Allocates owned memory; must be freed by mi_mx_program_destroy.
 */
bool mi_mx_load_file(MiVm* vm, const char* filename, MiMixProgram* out_program);

/**
 * Free all memory owned by a loaded MX program.
 */
void mi_mx_program_destroy(MiMixProgram* p);


#endif // MI_MX_H
