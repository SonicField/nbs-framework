// Mutual-exclusion assertions for JIT calling-convention paths.
//
// PROBLEM: Two independent paths handle JIT-compiled builtin calls:
//
//   PATH A (simplify.cpp): simplifyVectorCall — hand-coded HIR
//   simplifications for specific builtins (isinstance, hasattr,
//   getattr, next). These DELETE the VectorCall instruction.
//
//   PATH B (generator.cpp): TranslateSpecializedCall METH_FASTCALL —
//   generic wrapper routing through JITRT_FastCall0-3. Only fires on
//   VectorCall instructions that SURVIVE to LIR generation.
//
// Currently mutually exclusive by construction: Path A deletes the
// instruction before Path B can see it. But this is IMPLICIT. If
// someone modifies Path A to transform without deleting, Path B fires
// on the surviving instruction — double-call or wrong-path bug.
//
// These assertions make the exclusion EXPLICIT and debuggable.


// =====================================================================
// ASSERTION 1: In TranslateSpecializedCall, METH_FASTCALL case
// (generator.cpp)
//
// Insert at the top of the METH_FASTCALL case, after extracting cfunc.
// Guards that no known fast-path builtin leaks to the generic path.
// =====================================================================

// In the METH_FASTCALL case of TranslateSpecializedCall, after:
//   auto cfunc = PyCFunction_GET_FUNCTION(callee);
// Add:

#ifndef NDEBUG
    {
      // Mutual-exclusion assertion: if this builtin has a hand-coded
      // simplifyVectorCall path, it should have been handled there.
      // Reaching the generic JITRT_FastCall path means simplification
      // either failed silently or was incorrectly bypassed.
      //
      // Known fast-path builtins (Path A handles these):
      //   - isinstance  (simplifyIsInstance)
      //   - hasattr     (simplifyHasAttr)
      //   - getattr     (simplifyGetAttr)
      //   - next        (simplifyBuiltinNext → JITRT_BuiltinNext)
      //
      // Uses name-based checks via ml_name for portability across
      // CPython versions. The exact C symbol names for builtin
      // function pointers (builtin_isinstance, builtin_isinstance_impl,
      // etc.) vary between versions and would cause link errors.
      // ml_name is stable: it comes from the PyMethodDef table.
      //
      // NOTE: This list must be updated when new simplifyVectorCall
      // entries are added. Forgetting to update causes a silent
      // correctness issue, not a crash — the assertion is strictly
      // a debuggability aid.
      const char* ml_name = PyCFunction_GET_METHODDEF(callee)->ml_name;
      if (ml_name != nullptr) {
        // Names of builtins with hand-coded simplifyVectorCall paths.
        // If any of these reach the generic METH_FASTCALL path, the
        // simplification pass failed to fire.
        static const char* const fast_path_builtins[] = {
            "isinstance", "hasattr", "getattr", "next"
        };
        for (const char* blocked : fast_path_builtins) {
          JIT_DCHECK(
              strcmp(ml_name, blocked) != 0,
              "builtin %s() reached TranslateSpecializedCall "
              "METH_FASTCALL generic path but has a hand-coded "
              "simplifyVectorCall path. simplify.cpp should have "
              "handled this instruction.",
              ml_name);
        }
      }
    }
#endif


// =====================================================================
// ASSERTION 2: In simplifyVectorCall (simplify.cpp)
//
// After each successful simplification of a known METH_FASTCALL
// builtin, assert the VectorCall instruction was replaced/deleted.
// This catches the case where someone adds a fast path that
// falls through without deleting the original instruction.
// =====================================================================

// In simplifyVectorCall, at the end of each specific builtin handler
// (simplifyIsInstance, simplifyHasAttr, etc.), after the handler
// returns, verify the original instruction is no longer live.
//
// The existing pattern in simplify.cpp is:
//
//   if (callee == &builtin_isinstance && nargs == 2) {
//     return simplifyIsInstance(env, instr);
//   }
//
// The return value of simplify*() indicates whether simplification
// succeeded. If it returns true, the original VectorCall should have
// been deleted. Add this verification wrapper:

// Replace direct calls like:
//   return simplifyIsInstance(env, instr);
// With:
//   bool simplified = simplifyIsInstance(env, instr);
//   JIT_DCHECK(
//       !simplified || instr.isDeleted(),
//       "simplifyIsInstance returned true but VectorCall instruction "
//       "was not deleted. This breaks mutual exclusion with "
//       "TranslateSpecializedCall METH_FASTCALL path.");
//   return simplified;
//
// Apply this pattern to all simplify*() calls:
//   - simplifyIsInstance
//   - simplifyHasAttr
//   - simplifyGetAttr
//   - simplifyBuiltinNext (after JITRT_BuiltinNext fix)


// =====================================================================
// TESTING
//
// testkeeper has written test_call_path_mutual_exclusion.py (11 tests)
// that verify from the Python side. These assertions verify from the
// C++ side. Together they provide two layers of defence:
//
// 1. Python tests: call each known builtin under JIT, verify correct
//    results and correct path (no double-call, correct wrapper).
//
// 2. C++ assertions: fire immediately in debug builds if a known
//    builtin reaches the wrong path, with a diagnostic message
//    identifying the exact violation.
//
// To test the assertions themselves:
//   - Build CinderX in debug mode (NDEBUG not defined)
//   - Run the test suite — assertions should NOT fire
//   - Artificially break mutual exclusion (e.g., comment out one
//     simplifyVectorCall case) — assertions SHOULD fire
// =====================================================================
