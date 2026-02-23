// JITRT_FastCall wrappers for METH_FASTCALL C extension methods.
//
// These bridge the gap between CinderX's register-based CallStatic calling
// convention and the _PyCFunctionFast calling convention which expects a
// contiguous args array. Each wrapper builds a stack-allocated array from
// individual arguments and calls the C function directly.
//
// Used by TranslateSpecializedCall (generator.cpp) for PyCFunction builtins
// with METH_FASTCALL (e.g. getattr, iter). The function pointer is burned
// into the generated code as an Imm operand.
//
// Naming: JITRT_FastCallN where N is the number of positional args
// (excluding self). For module-level builtins, self is the module.

// --- Add to jit_rt.cpp ---

PyObject* JITRT_FastCall0(void* ml_meth, PyObject* self) {
  auto cfunc = reinterpret_cast<_PyCFunctionFast>(ml_meth);
  return cfunc(self, nullptr, 0);
}

PyObject* JITRT_FastCall1(void* ml_meth, PyObject* self, PyObject* a0) {
  auto cfunc = reinterpret_cast<_PyCFunctionFast>(ml_meth);
  PyObject* args[1] = {a0};
  return cfunc(self, args, 1);
}

PyObject* JITRT_FastCall2(
    void* ml_meth,
    PyObject* self,
    PyObject* a0,
    PyObject* a1) {
  auto cfunc = reinterpret_cast<_PyCFunctionFast>(ml_meth);
  PyObject* args[2] = {a0, a1};
  return cfunc(self, args, 2);
}

PyObject* JITRT_FastCall3(
    void* ml_meth,
    PyObject* self,
    PyObject* a0,
    PyObject* a1,
    PyObject* a2) {
  auto cfunc = reinterpret_cast<_PyCFunctionFast>(ml_meth);
  PyObject* args[3] = {a0, a1, a2};
  return cfunc(self, args, 3);
}

// --- Add to jit_rt.h ---
// After the existing JITRT_Vectorcall declaration (~line 207):

/**
 * METH_FASTCALL wrapper functions.
 *
 * These provide a register-based calling convention bridge for C extension
 * methods that use METH_FASTCALL. The JIT calls these wrappers via
 * TranslateSpecializedCall, passing the original ml_meth function pointer
 * and individual arguments. The wrapper builds the args array on the stack
 * and calls the C function directly, bypassing _PyObject_Vectorcall dispatch.
 *
 * @param ml_meth  The C function pointer (from PyMethodDef::ml_meth)
 * @param self     The self/module object (from PyCFunction_GET_SELF)
 * @param a0..aN   Individual positional arguments
 * @return         The function result, or NULL with exception set
 */
PyObject* JITRT_FastCall0(void* ml_meth, PyObject* self);
PyObject* JITRT_FastCall1(void* ml_meth, PyObject* self, PyObject* a0);
PyObject* JITRT_FastCall2(
    void* ml_meth,
    PyObject* self,
    PyObject* a0,
    PyObject* a1);
PyObject* JITRT_FastCall3(
    void* ml_meth,
    PyObject* self,
    PyObject* a0,
    PyObject* a1,
    PyObject* a2);
