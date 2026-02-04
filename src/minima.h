#ifndef MINIMA_H
#define MINIMA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "mi_version.h"
#include "mi_parse.h"
#include "mi_runtime.h"
#include "mi_fold.h"
#include "mi_vm.h"
#include "mi_mx.h"
#include "mi_log.h"
#include "mi_compile.h"

  int mi_compile_only(const char* in_file, const char* out_file, const char* cache_dir);
  int mi_disasm(const char* mx_file, const char* cache_dir);
  int mi_disasm_mi(const char* mi_file, const char* cache_dir);
  int mi_run_source(const char* mi_file, const char* cache_dir);
  int mi_run_mx(const char* mx_file, const char* cache_dir);

#ifdef __cplusplus
}
#endif

#endif //MINIMA_H
