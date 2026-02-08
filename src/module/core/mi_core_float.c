#include "mi_core_float.h"
#include "mi_runtime.h"
#include "mi_vm.h"
#include "mi_log.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
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

static bool s_parse_double(XSlice s, double* out)
{
  if (!out)
  {
    return false;
  }

  char buf[256];
  (void)s_slice_to_cstr(s, buf, sizeof(buf));

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
  double v = strtod(p, &end);
  if (errno != 0)
  {
    return false;
  }
  if (!end)
  {
    return false;
  }

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

static bool s_to_float(MiRtValue v, double* out)
{
  if (!out)
  {
    return false;
  }

  switch (v.kind)
  {
    case MI_RT_VAL_FLOAT:
      *out = v.as.f;
      return true;
    case MI_RT_VAL_INT:
      *out = (double)v.as.i;
      return true;
    case MI_RT_VAL_BOOL:
      *out = v.as.b ? 1.0 : 0.0;
      return true;
    case MI_RT_VAL_STRING:
      return s_parse_double(v.as.s, out);
    case MI_RT_VAL_VOID:
      *out = 0.0;
      return true;
    default:
      return false;
  }
}

static MiRtValue s_fail_assert_float(MiVm* vm, const MiRtValue got)
{
  (void)got;
  mi_error("assert_float: expected float\n");
  vm->api->vm_trace_print(vm);
  exit(1);
  //return vm->api->rt_make_void();
}

//----------------------------------------------------------
// float / float::
//----------------------------------------------------------

static MiRtValue s_cmd_cast(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;
  if (argc != 1)
  {
    mi_error("float: expected 1 argument\n");
    return vm->api->rt_make_float(0.0);
  }

  double v = 0.0;
  if (!s_to_float(argv[0], &v))
  {
    return vm->api->rt_make_float(0.0);
  }
  return vm->api->rt_make_float(v);
}

static MiRtValue s_cmd_assert(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)user;
  if (argc != 1)
  {
    mi_error("assert_float: expected 1 argument\n");
    return vm->api->rt_make_float(0.0);
  }

  if (argv[0].kind != MI_RT_VAL_FLOAT)
  {
    return s_fail_assert_float(vm, argv[0]);
  }
  return argv[0];
}

static MiRtValue s_cmd_float_try(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;
  if (argc != 2)
  {
    mi_error("float::try: expected 2 arguments\n");
    return vm->api->rt_make_float(0.0);
  }

  double v = 0.0;
  if (!s_to_float(argv[0], &v))
  {
    if (!s_to_float(argv[1], &v))
    {
      v = 0.0;
    }
    return vm->api->rt_make_float(v);
  }
  return vm->api->rt_make_float(v);
}

static MiRtValue s_cmd_float_is(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;
  if (argc != 1)
  {
    mi_error("float::is: expected 1 argument\n");
    return vm->api->rt_make_bool(false);
  }
  return vm->api->rt_make_bool(argv[0].kind == MI_RT_VAL_FLOAT);
}

static MiRtValue s_cmd_float_min(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;
  (void)argv;
  if (argc != 0)
  {
    mi_error("float::min: expected 0 arguments\n");
  }
  return vm->api->rt_make_float(-DBL_MAX);
}

static MiRtValue s_cmd_float_max(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;
  (void)argv;
  if (argc != 0)
  {
    mi_error("float::max: expected 0 arguments\n");
  }
  return vm->api->rt_make_float(DBL_MAX);
}

static MiRtValue s_cmd_float_abs(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;
  if (argc != 1)
  {
    mi_error("float::abs: expected 1 argument\n");
    return vm->api->rt_make_float(0.0);
  }

  double v = 0.0;
  if (!s_to_float(argv[0], &v))
  {
    return vm->api->rt_make_float(0.0);
  }
  return vm->api->rt_make_float(fabs(v));
}

static MiRtValue s_cmd_float_clamp(MiVm* vm, void* user, int argc, const MiRtValue* argv)
{
  (void)vm;
  (void)user;
  if (argc != 3)
  {
    mi_error("float::clamp: expected 3 arguments\n");
    return vm->api->rt_make_float(0.0);
  }

  double x = 0.0;
  double lo = 0.0;
  double hi = 0.0;
  if (!s_to_float(argv[0], &x))
  {
    return vm->api->rt_make_float(0.0);
  }
  if (!s_to_float(argv[1], &lo))
  {
    return vm->api->rt_make_float(0.0);
  }
  if (!s_to_float(argv[2], &hi))
  {
    return vm->api->rt_make_float(0.0);
  }

  if (lo > hi)
  {
    double t = lo;
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
  return vm->api->rt_make_float(x);
}

//----------------------------------------------------------
// Registration
//----------------------------------------------------------

bool mi_lib_float_register(MiVm* vm, MiRtValue ns_block)
{
  if (!vm || !vm->api)
  {
    return false;
  }

  MiRtValue ns = ns_block;
  XSlice doc = x_slice_init(NULL, 0);

  vm->api->vm_namespace_add_native_sigv(vm, ns, "abs",    s_cmd_float_abs,   NULL, doc, MI_TYPE_FLOAT, 1, MI_TYPE_FLOAT);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "assert", s_cmd_assert,      NULL, doc, MI_TYPE_FLOAT, 1, MI_TYPE_ANY);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "cast",   s_cmd_cast,        NULL, doc, MI_TYPE_FLOAT, 1, MI_TYPE_ANY);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "clamp",  s_cmd_float_clamp, NULL, doc, MI_TYPE_FLOAT, 3, MI_TYPE_FLOAT, MI_TYPE_FLOAT, MI_TYPE_FLOAT);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "is",     s_cmd_float_is,    NULL, doc, MI_TYPE_BOOL,  1, MI_TYPE_ANY);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "max",    s_cmd_float_max,   NULL, doc, MI_TYPE_FLOAT, 0);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "min",    s_cmd_float_min,   NULL, doc, MI_TYPE_FLOAT, 0);
  vm->api->vm_namespace_add_native_sigv(vm, ns, "try",    s_cmd_float_try,   NULL, doc, MI_TYPE_FLOAT, 2, MI_TYPE_ANY, MI_TYPE_ANY);

  // Constants
  //vm->api->vm_namespace_add_value(vm, ns, "pi", vm->api->rt_make_float(3.141592653589793));
  return true;
}
