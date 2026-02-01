#ifndef MINIMA_TYPECHECK_H
#define MINIMA_TYPECHECK_H

#include "mi_parse.h"

typedef struct MiTypecheckError
{
  int line;
  int column;
  XSlice message;
} MiTypecheckError;

bool mi_typecheck_script(const MiScript* script, MiTypecheckError* err);

void mi_typecheck_print_error(XSlice source, const MiTypecheckError* err);

#endif
