// JITRT_FastCall wrappers for METH_FASTCALL builtin specialisation.
//
// These wrappers bridge CallStatic (individual register args) to the
// PyCFunctionFast calling convention (args array pointer + nargs).
//
// Each wrapper builds a stack-local args array and calls through ml_meth
// with the correct _PyCFunctionFast signature.
//
// Add declarations to jit_rt.h:
//   PyObject* JITRT_FastCall0(void* ml_meth, PyObject* self);
//   PyObject* JITRT_FastCall1(void* ml_meth, PyObject* self, PyObject* a0);
//   PyObject* JITRT_FastCall2(void* ml_meth, PyObject* self, PyObject* a0, PyObject* a1);
//   PyObject* JITRT_FastCall3(void* ml_meth, PyObject* self, PyObject* a0, PyObject* a1, PyObject* a2);
//
// IMPORTANT: ml_meth takes void* (not PyCFunction) because
// appendCallInstruction's template type-matching requires the call-site
// arg types to match the function signature exactly. The call-site passes
// (void*)PyCFunction_GET_FUNCTION(callee) which is void*.
//
// Add to jit_rt.cpp:

PyObject* JITRT_FastCall0(void* ml_meth, PyObject* self) {
  return ((_PyCFunctionFast)ml_meth)(self, nullptr, 0);
}

PyObject* JITRT_FastCall1(
    void* ml_meth, PyObject* self, PyObject* a0) {
  PyObject* args[1] = {a0};
  return ((_PyCFunctionFast)ml_meth)(self, args, 1);
}

PyObject* JITRT_FastCall2(
    void* ml_meth, PyObject* self, PyObject* a0, PyObject* a1) {
  PyObject* args[2] = {a0, a1};
  return ((_PyCFunctionFast)ml_meth)(self, args, 2);
}

PyObject* JITRT_FastCall3(
    void* ml_meth, PyObject* self,
    PyObject* a0, PyObject* a1, PyObject* a2) {
  PyObject* args[3] = {a0, a1, a2};
  return ((_PyCFunctionFast)ml_meth)(self, args, 3);
}

// --- TranslateSpecializedCall extension (generator.cpp) ---
//
// In the existing switch on ml_flags in TranslateSpecializedCall,
// add the METH_FASTCALL case:
//
//   case METH_FASTCALL: {
//     PyCFunction cfunc = PyCFunction_GET_FUNCTION(callable);
//     PyObject* self = PyCFunction_GET_SELF(callable);
//     size_t nargs = hir_instr.numArgs();
//
//     // Select wrapper by arity (0-3 args supported)
//     void* wrapper = nullptr;
//     switch (nargs) {
//       case 0: wrapper = reinterpret_cast<void*>(JITRT_FastCall0); break;
//       case 1: wrapper = reinterpret_cast<void*>(JITRT_FastCall1); break;
//       case 2: wrapper = reinterpret_cast<void*>(JITRT_FastCall2); break;
//       case 3: wrapper = reinterpret_cast<void*>(JITRT_FastCall3); break;
//       default: return false;  // >3 args: fall back to VectorCall
//     }
//
//     // Emit call: wrapper(cfunc, self, arg0, arg1, ...)
//     // cfunc and self are immediate constants (known at compile time)
//     appendCallInstruction(
//         env, instr, wrapper,
//         Imm{reinterpret_cast<uint64_t>(cfunc)},
//         Imm{reinterpret_cast<uint64_t>(self)},
//         /* args from hir_instr operands */);
//     return true;
//   }
//
// --- bytecode.cpp ---
//
// Add to specializedOpcode() switch:
//
//   case CALL_BUILTIN_FAST:
//     return opcode;
//
// --- builder.cpp ---
//
// In emitCall (or equivalent CALL handler), after checking
// getConfig().specialized_opcodes:
//
//   case CALL_BUILTIN_FAST: {
//     // Callee is PyCFunction with METH_FASTCALL flags.
//     // Guard type so TranslateSpecializedCall can fire.
//     Register* callee = /* callable register */;
//     tc.emit<GuardType>(callee, TCFunction, callee, tc.frame);
//     break;
//   }
