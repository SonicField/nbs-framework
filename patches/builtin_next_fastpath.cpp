// JITRT_BuiltinNext: Fast-path wrapper for builtin next().
//
// Replaces Ci_Builtin_Next_Core in the builtinNext special case of
// TranslateSpecializedCall (generator.cpp). Routes through
// JITRT_InvokeIterNext which has a G1 fast path for JIT generators
// (direct resumeEntry call, skipping tp_iternext dispatch).
//
// Converts the JITRT_IterDoneSentinel return to next() semantics:
//   - No default: raise StopIteration
//   - With default: return default value
//
// --- Add to jit_rt.cpp, after JITRT_InvokeIterNext ---

PyObject* JITRT_BuiltinNext(PyObject* it, PyObject* def) {
  PyObject* result = JITRT_InvokeIterNext(it);
  if (result == nullptr) {
    // Error propagation (non-StopIteration exception)
    return nullptr;
  }
  if (result != &JITRT_IterDoneSentinel) {
    // Normal yielded/returned value
    return result;
  }
  // Iterator exhausted - sentinel returned.
  // JITRT_InvokeIterNext increfs the sentinel, so release it.
  Py_DECREF(result);
  if (def != nullptr) {
    return Py_NewRef(def);
  }
  PyErr_SetNone(PyExc_StopIteration);
  return nullptr;
}

// --- Add to jit_rt.h, after JITRT_InvokeIterNext declaration ---

// Fast-path builtin next() wrapper. Routes through JITRT_InvokeIterNext
// (G1 fast path for JIT generators), converts sentinel to next() semantics.
PyObject* JITRT_BuiltinNext(PyObject* it, PyObject* def);

// --- In generator.cpp, replace Ci_Builtin_Next_Core with JITRT_BuiltinNext ---
// (both the 1-arg and 2-arg cases in the builtinNext() check)

// ROOT CAUSE:
// next() was going through Ci_Builtin_Next_Core -> builtin_next_impl ->
// tp_iternext (generic path), while FOR_ITER uses JITRT_InvokeIterNext
// which has a G1 fast path for JIT generators that directly resumes via
// gen_footer->resumeEntry, skipping the tp_iternext virtual dispatch.
//
// JITRT_BuiltinNext bridges this gap by calling JITRT_InvokeIterNext
// and converting the JITRT_IterDoneSentinel return value to the correct
// next() semantics (raise StopIteration or return default).
