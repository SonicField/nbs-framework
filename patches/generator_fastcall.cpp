// TranslateSpecializedCall METH_FASTCALL extension for generator.cpp
//
// Add this case to the switch statement in TranslateSpecializedCall
// (generator.cpp ~line 430, after the METH_O case and before the closing
// brace of the switch):
//
// The switch line is:
//   switch (PyCFunction_GET_FLAGS(callee) &
//       (METH_VARARGS | METH_FASTCALL | METH_NOARGS | METH_O | METH_KEYWORDS))

    case METH_FASTCALL: {
      // METH_FASTCALL C functions expect (self, args_array, nargs).
      // We use arity-specific JITRT wrappers that build the stack array
      // from individual register arguments.
      auto cfunc = PyCFunction_GET_FUNCTION(callee);
      auto self = PyCFunction_GET_SELF(callee);
      switch (hir_instr.numArgs()) {
        case 0:
          bbb.appendCallInstruction(
              hir_instr.output(),
              JITRT_FastCall0,
              reinterpret_cast<void*>(cfunc),
              self);
          return true;
        case 1:
          bbb.appendCallInstruction(
              hir_instr.output(),
              JITRT_FastCall1,
              reinterpret_cast<void*>(cfunc),
              self,
              hir_instr.arg(0));
          return true;
        case 2:
          bbb.appendCallInstruction(
              hir_instr.output(),
              JITRT_FastCall2,
              reinterpret_cast<void*>(cfunc),
              self,
              hir_instr.arg(0),
              hir_instr.arg(1));
          return true;
        case 3:
          bbb.appendCallInstruction(
              hir_instr.output(),
              JITRT_FastCall3,
              reinterpret_cast<void*>(cfunc),
              self,
              hir_instr.arg(0),
              hir_instr.arg(1),
              hir_instr.arg(2));
          return true;
        default:
          // Too many args — fall back to generic VectorCall
          break;
      }
      break;
    }

// NOTE: PyCFunction_GET_FUNCTION returns PyCFunction (typedef for
// PyObject*(*)(PyObject*, PyObject*)), but the actual function is
// _PyCFunctionFast. The JITRT wrappers accept void* and cast internally.
//
// reinterpret_cast<void*> is needed because C++ does not allow
// static_cast between function pointers and void*. PyCFunction is a
// function pointer type, so reinterpret_cast is the correct cast.
// The appendCallInstruction template would fail to match if we
// pass a PyCFunction directly since the wrapper expects void*.
