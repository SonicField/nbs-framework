# C++ to C Build Failure Catalogue

A catalogue of build failures that occur when converting C++
codebases to C. Each entry includes the exact compiler/linker error
message, root cause, fix, and a prevention rule you can apply
mechanically.

These failures are general — they arise from fundamental differences
between C and C++ compilation, linking, and initialisation semantics.

Honest type references: see `cpp-to-c-types.md` for
`BuildFailureClass` definitions.

---

## 1. CppHeaderInCFile — C++ Header Included from .c File

**Exact error message:**
```
error: 'utility' file not found
```

Triggered when a `.c` file includes a project header that
transitively includes `<utility>` (or another C++ standard library
header).

**Root cause:** C files are compiled by the C compiler, which cannot
parse C++ standard library headers. If a project header includes
`<utility>`, `<vector>`, `<string>`, etc., any `.c` file that
includes that header will fail to compile. In C++ compilation this
works transparently.

**Fix** (three options):

1. **`#ifdef __cplusplus` guards** around C++ includes in the shared
   header:
   ```c
   #ifdef __cplusplus
   #include <utility>
   #endif
   ```

2. **Separate `_c.h` header** that contains only C-compatible
   declarations. The `.c` file includes `_c.h`; the `.cpp` files
   include the original header.

3. **Forward-declare** only the specific items needed in the `.c`
   file instead of including the problematic header.

**Prevention rule:** `.c` files MUST NOT include any header that
transitively includes C++ standard library headers. Before including
a header from a `.c` file, check its `#include` chain for C++
content.

---

## 2. NameManglingMismatch — C vs C++ Linkage at Link Time

**Exact error message:**
```
undefined reference to 'some_function'
```

The linker searches for the unmangled symbol (C linkage) but only
finds the mangled symbol (C++ linkage).

**Root cause:** A `.c` file forward-declares a function with C
linkage (the default for `.c` files). The actual definition in a
`.cpp` file has C++ linkage (name-mangled). The two symbols have
different names at link time.

**Fix:** Address-passing pattern. The C++ side resolves the mangled
symbol at runtime and passes the raw address to C via an init
function:

```c
/* C side */
static uint64_t func_addr = 0;
void set_func_addr(uint64_t addr) { func_addr = addr; }
```

```cpp
/* C++ side */
set_func_addr(reinterpret_cast<uint64_t>(CppFunction));
```

**Prevention rule:** C files MUST NOT forward-declare functions that
are defined with C++ linkage. Either:
- Add `extern "C"` to the C++ definition (if safe).
- Use the address-passing pattern.
- Include the header via a `_c.h` shim with `extern "C"` guards.

---

## 3. NonConstStaticInit — Extern Addresses in Static Initialisers

**Exact error message:**
```
error: initializer element is not a compile-time constant
```

Pointing to an extern variable address in a static array initialiser:
```c
static const Entry table[] = {
    {"error_type_a", (uint64_t)(uintptr_t)error_type_a},
                                          ^~~~~~~~~~~~~
};
```

**Root cause:** C requires static initialisers to be compile-time
constants. The address of an extern variable is NOT a compile-time
constant in C — it is resolved at load time by the dynamic linker.
C++ allows this (the initialiser runs at static construction time).

Note: function addresses (e.g. `(uint64_t)some_function`) ARE
compile-time constants on most platforms. The issue is specific to
extern *variable* addresses.

**Fix:** Lazy initialisation. Replace the static array with a
function-scoped init:

```c
static Entry table[NUM_ENTRIES];
static int table_initialized = 0;

static void init_table(void) {
    table[0].name = "error_type_a";
    table[0].addr = (uint64_t)(uintptr_t)error_type_a;
    /* ... */
    table_initialized = 1;
}

const uint64_t* lookup(const char *name) {
    if (!table_initialized) init_table();
    /* ... */
}
```

**Prevention rule:** Static arrays MUST NOT contain extern variable
addresses. Use the lazy-init pattern for any table that maps names
to runtime-resolved addresses.

---

## 4. MissingInclude — offsetof Unavailable Due to Header Guards

**Exact error message:**
```
error: call to undeclared function 'offsetof'
```

And separately:
```
error: unexpected type name 'SomeStruct': expected expression
```

**Root cause:** Some framework headers use internal `__need_offsetof`
guards in their header chains. When `stddef.h` is included
transitively (via a framework header), the `__need_offsetof` macro
prevents `offsetof` from being defined. In C++ compilation,
`offsetof` is provided by `<cstddef>` which has different guards.
The result: a `.c` file that includes the framework header but not
`<stddef.h>` explicitly does not have `offsetof` available.

**Fix:** Add explicit `#include <stddef.h>` AFTER the framework
header include:

```c
#include "framework.h"
#include <stddef.h>    /* explicit — framework guards block transitive */
```

The second `<stddef.h>` include bypasses the `__need_offsetof` guard
because the guard was consumed by the first transitive include.

**Prevention rule:** All `.c` files that use `offsetof` MUST include
`<stddef.h>` explicitly after any framework headers. Do not rely on
transitive inclusion through framework header chains.

---

## 5. ArchGuardMissing — Architecture-Conditional Fields Without Guards

**Exact error message:**
```
error: no member named 'flag_field' in 'RuntimeEnv'
error: no member named 'saved_offset' in 'RuntimeEnv'
error: no member named 'saved_ptr' in 'DataFooter'
```

**Root cause:** Struct fields that only exist under an architecture
guard (e.g. `#if defined(__aarch64__)`) were accessed unconditionally
in converted code. Compiling on the other architecture (where the
guard is not defined) caused the struct member error.

**Fix:** Wrap architecture-specific field accesses in the same
`#if defined(...)` guard used in the declaring header, with a
fallback `#else`:

```c
ctx.builder = env.as->impl();
#if defined(__aarch64__)
ctx.flag_field = env.flag_field ? 1 : 0;
ctx.saved_offset = env.saved_offset;
#else
ctx.flag_field = 0;
ctx.saved_offset = 0;
#endif
```

**Prevention rule:** Before accessing ANY struct field in converted
code, check whether the field is architecture-conditional. Search
for `#if defined(...)` guards in the declaring header. If guarded,
wrap the access in the same guard.

---

## 6. ExplicitConstructor — Implicit Conversion Blocked by `explicit`

**Exact error message:**
```
error: no matching constructor for initialization of 'TargetLabel'
note: candidate constructor (the implicit copy constructor) not viable:
      no known conversion from 'SourceLabel' to 'const TargetLabel'
note: explicit constructor is not a candidate
```

**Root cause:** `std::vector::emplace_back` constructs the element
in-place using the provided arguments. When the target type has an
`explicit` constructor from the source type, `emplace_back` cannot
invoke it via implicit conversion. The C++ compiler requires an
explicit cast.

Stale `.o` files from a prior C++ build can mask this error. A clean
rebuild (`make clean && make`) exposes it.

**Fix:** Add explicit conversion at the call site:

```cpp
container.emplace_back(
    TargetType(source_value),    // explicit conversion
    other_arg);
```

**Prevention rule:** When a converted `.c` file produces a type
that C++ code consumes, check whether the consuming constructor is
`explicit`. If so, add an explicit cast at every call site. Search
for `explicit` in the target type's constructor declarations.

---

## 7. ConcurrentBuild — Multiple Builders in the Same Working Tree

**Exact error** (not a compiler error — operational failure):

Builder A (clean build script):
```bash
rm -f output_binary    # deletes binary before rebuild
```

Builder B (running tests against the binary):
```
error: No such file or directory: 'output_binary'
```

A clean build script deletes the output binary at the start. When
two developers run builds concurrently in the same working tree,
one build deletes the other's verified output.

**Root cause:** No file-level or process-level locking on the build
directory. Two builders initiated builds in the same working tree
simultaneously. This is a general risk in any project where:
- The build script deletes output before rebuilding.
- Multiple people share a single checkout.
- CI and local builds overlap.

**Fix:** Designate a single builder. Enforce via project
configuration:
```
ONLY the designated builder may run build commands.
All other contributors: file editing and git only.
No exceptions without explicit handoff.
```

Social protocol ("don't build") is weaker than mechanical
enforcement. If possible, use file locks (`flock`), separate
worktrees, or CI-only builds.

**Prevention rule:** In multi-developer projects, designate a single
builder with exclusive access to build commands. Add this rule to
the project's configuration file. For CI, use build queue
serialisation.

---

## 8. CrossFileDeclarationMismatch — Inconsistent Types Across .c Files

**Exact error messages:**
```
error: conflicting types for 'operand_new'
error: redefinition of 'struct Operand'
error: incompatible pointer types passing 'Instruction *' to parameter of type 'void *'
```

Multiple errors in a single build when several `.c` files
cross-reference shared types.

**Root cause:** When multiple `.c` files share types but each uses
its own forward declarations instead of including a shared header,
type definitions drift — struct layouts, function signatures, and
pointer types become inconsistent across translation units.

Adding more forward declarations to fix the errors makes things
worse by introducing duplicate declarations with mismatched types.

**Fix:** All `.c` files that share types MUST `#include` the same
canonical header and conform to its type signatures exactly. No
local forward declarations for types defined in the shared header.

**Prevention rule:** When multiple `.c` files share types, ALL files
must include the SAME canonical header. Never duplicate type
definitions via local forward declarations — they will drift.

---

## 9. DuplicateImplementation — Duplicate Symbol Definitions

**Exact error message:**
```
error: multiple definition of 'operand_new'
operand.o: first defined here
operand_impl.o: additional definition here
```

**Root cause:** Two people independently created C implementations
for the same module. Both object files define the same function.
When linked into the same binary, the linker reports duplicate
symbols.

This is a coordination failure: in a multi-developer team, two
contributors may independently begin converting the same module
without knowing the other has started.

**Fix:** Delete the redundant file. Keep the canonical version.
Verify no other file references the deleted path.

**Prevention rule:** Before starting a conversion, check whether
another `.c` implementation already exists for the same module.
Search for `<module_name>.c`, `<module_name>_impl.c`, and
`<module_name>_c.c` in the same directory.

---

## 10. ApiNamingMismatch — Inconsistent Function Names

**Exact error message:**
```
error: undefined reference to 'operand_create'
```

The `.c` file called `operand_create()`, but the shared header
declared `operand_new()`.

**Root cause:** Implementation files written by different people or
at different times used different naming conventions for the same
operation. The header declared one name; the implementation used
another. The compiler accepted the `.c` file (it had its own local
forward declaration), but the linker could not find the mismatched
symbol.

**Fix:** Align all implementation function names to match the shared
header declarations exactly. Remove any local forward declarations
that shadow the canonical header.

**Prevention rule:** Every function name in a `.c` file must match
the declaration in the shared header character-for-character. Use
copy-paste from the header, not memory. Run `grep` on the header
to confirm the function exists before implementing it.

---

## 11. AccessorTypeMismatch — Bridge Returns Wrong Type

**Exact error:** No compiler error — silent wrong values at runtime.

A bridge accessor function returned a struct (containing multiple
fields) where the caller expected a scalar (just one field of the
struct). The C compiler accepted the implicit conversion, but the
returned value was the struct's raw bytes reinterpreted as a scalar
— producing garbage values.

**Root cause:** Type mismatch between an intermediate struct
(e.g. `Location` with `int loc` + `size_t bit_size`) and the
C API which declares `int` return type. The bridge function returned
the entire struct instead of extracting the relevant field.

**Fix:** One-line change in the bridge:
```c
/* Before: */ return location;          /* returns struct */
/* After:  */ return location.loc;      /* returns int field */
```

**Prevention rule:** When a bridge accessor function returns a
scalar type (`int`, `size_t`, `uint64_t`), verify it extracts the
correct field from any intermediate struct. Do not return the struct
itself — C will silently truncate or reinterpret the bytes.
