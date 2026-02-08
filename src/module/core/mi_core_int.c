#include "mi_core_int.h"
#include "mi_runtime.h"
#include "mi_vm.h"
#include "mi_log.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>


//----------------------------------------------------------
// Helpers
//----------------------------------------------------------

static bool s_slice_to_cstr(XSlice s, char* out, size_t out_cap)
{
  if (!out || out_cap == 0)
  {
    return false;
  }
  if (!s.ptr)
  {
    out[0] = '\0';
    return false;
  }
  size_t n = s.length;
  if (n >= out_cap)
  {
    n = out_cap - 1;
  }
  memcpy(out, s.ptr, n);
  out[n] = '\0';
  return true;
}

static bool s_parse_int64(XSlice s, long long* out)
{
  if (!out)
  {
    return false;
  }

  char buf[256];
  (void)s_slice_to_cstr(s, buf, sizeof(buf));

  // Trim leading spaces.
  char* p = buf;
  while (*p && isspace((unsigned char)*p))
  {
    ++p;
  }

  if (*p == '\0')
  {
    return false;
  }

  errno = 0;
  char* end = NULL;
  long long v = strtoll(p, &end, 10);
  if (errno != 0)
  {
    return false;
  }
  if (!end)
  {
    return false;
  }
  // Skip trailing spaces.
  while (*end && isspace((unsigned char)*end))
  {
    ++end;
  }
  if (*end != '\0')
  {
    return false;
  }

  *out = v;
  return true;
}

static bool s_to_int(MiRtValue v, long long* out)
{
  if (!out)
  {
    return false;
  }

  switch (v.kind)
  {
    case MI_RT_VAL_INT:
      *out = (long long)v.as.i;
      return true;
    case MI_RT_VAL_FLOAT:
      if (v.as.f > (double)LLONG_MAX)
      {
        *out = LLONG_MAX;
        return true;
      }
      if (v.as.f < (double)LLONG_MIN)
      {
        *out = LLONG_MIN;
        return true;
      }
      *out = (long long)v.as.f;
      return true;
    case MI_RT_VAL_BOOL:
      *out = v.as.b ? 1 : 0;
      return true;
    case MI_RT_VAL_STRING:
      return s_parse_int64(v.as.s, out);
    case MI_RT_VAL_VOID:
      *out = 0;
      return true;
    default:
      return false;
  }
}

static MiRtValue s_fail_assert(MiVm* vm, const MiRtValue got)
{
  (void)got;
  mi_error("assert: expected int\n");
  vm->api->vm_trace_print(vm);
  exit(1);
  //return vm->api->rt_make_void();
}

//----------------------------------------------------------
// int / int::
//----------------------------------------------------------

static MiRtValue s_cmd_cast(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;
  if (argc != 1)
  {
    mi_error("int: expected 1 argument\n");
    return vm->api->rt_make_int(0);
  }

  long long v = 0;
  if (!s_to_int(argv[0], &v))
  {
    return vm->api->rt_make_int(0);
  }
  return vm->api->rt_make_int(v);
}

static MiRtValue s_cmd_assert(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
  if (argc != 1)
  {
    mi_error("assert: expected 1 argument\n");
    return vm->api->rt_make_int(0);
  }
  if (argv[0].kind != MI_RT_VAL_INT)
  {
    return s_fail_assert(vm, argv[0]);
  }
  return argv[0];
}

static MiRtValue s_cmd_int_try(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;
  if (argc != 2)
  {
    mi_error("int::try: expected 2 arguments\n");
    return vm->api->rt_make_int(0);
  }

  long long v = 0;
  if (!s_to_int(argv[0], &v))
  {
    // default must be int-ish. We intentionally accept ANY and coerce it via int().
    if (!s_to_int(argv[1], &v))
    {
      v = 0;
    }
    return vm->api->rt_make_int(v);
  }
  return vm->api->rt_make_int(v);
}

static MiRtValue s_cmd_int_is(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;
  if (argc != 1)
  {
    mi_error("int::is: expected 1 argument\n");
    return vm->api->rt_make_bool(false);
  }
  return vm->api->rt_make_bool(argv[0].kind == MI_RT_VAL_INT);
}

static MiRtValue s_cmd_int_min(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;
  (void)argv;
  if (argc != 0)
  {
    mi_error("int::min: expected 0 arguments\n");
  }
  return vm->api->rt_make_int(LLONG_MIN);
}

static MiRtValue s_cmd_int_max(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;
  (void)argv;
  if (argc != 0)
  {
    mi_error("int::max: expected 0 arguments\n");
  }
  return vm->api->rt_make_int(LLONG_MAX);
}

static MiRtValue s_cmd_int_abs(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
  if (argc != 1 || argv[0].kind != MI_RT_VAL_INT)
  {
    mi_error("int::abs: expected 1 int argument\n");
    vm->api->vm_trace_print(vm);
    exit(1);
  }
  long long v = (long long)argv[0].as.i;
  if (v == LLONG_MIN)
  {
    return vm->api->rt_make_int(LLONG_MAX);
  }
  if (v < 0)
  {
    v = -v;
  }
  return vm->api->rt_make_int(v);
}

static MiRtValue s_cmd_int_clamp(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
  if (argc != 3 || argv[0].kind != MI_RT_VAL_INT || argv[1].kind != MI_RT_VAL_INT || argv[2].kind != MI_RT_VAL_INT)
  {
    mi_error("int::clamp: expected 3 int arguments\n");
    vm->api->vm_trace_print(vm);
    exit(1);
  }

  long long x = (long long)argv[0].as.i;
  long long lo = (long long)argv[1].as.i;
  long long hi = (long long)argv[2].as.i;
  if (lo > hi)
  {
    long long t = lo;
    lo = hi;
    hi = t;
  }
  if (x < lo)
  {
    x = lo;
  }
  if (x > hi)
  {
    x = hi;
  }
  return vm->api->rt_make_int(x);
}

//----------------------------------------------------------
// Registration
//----------------------------------------------------------

bool mi_lib_int_register(MiVm* vm, MiRtValue ns_block)
{
  if (!vm || !vm->api)
  {
    return false;
  }

  MiRtValue ns = ns_block;
  XSlice doc = x_slice_init(NULL, 0);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "abs",    s_cmd_int_abs,   NULL, doc, MI_TYPE_INT,  1, MI_TYPE_INT);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "assert", s_cmd_assert,    NULL, doc, MI_TYPE_INT,  1, MI_TYPE_ANY);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "cast",   s_cmd_cast,      NULL, doc, MI_TYPE_INT,  1, MI_TYPE_ANY);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "clamp",  s_cmd_int_clamp, NULL, doc, MI_TYPE_INT,  3, MI_TYPE_INT, MI_TYPE_INT, MI_TYPE_INT);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "is",     s_cmd_int_is,    NULL, doc, MI_TYPE_BOOL, 1, MI_TYPE_ANY);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "max",    s_cmd_int_max,   NULL, doc, MI_TYPE_INT,  0);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "min",    s_cmd_int_min,   NULL, doc, MI_TYPE_INT,  0);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "try",    s_cmd_int_try,   NULL, doc, MI_TYPE_INT,  2, MI_TYPE_ANY, MI_TYPE_ANY);
  return true;
}
