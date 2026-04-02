# C++ to C Porting Checklist

A mechanical checklist for converting C++ source files to C within a
mixed C/C++ codebase. Run this before compiling a converted file for
the first time.

Every item is a yes/no question. Each includes a one-sentence
explanation of what fails if the check is skipped, citing the relevant
`BuildFailureClass` from `cpp-to-c-types.md`.

---

## 1. Pre-conversion

These checks confirm the conversion is safe to begin.

- [ ] **Has the build lock been acquired?**
  Two developers building simultaneously corrupt each other's output
  and produce misleading pass/fail results. (`ConcurrentBuild`)

- [ ] **Is the target file a leaf in the dependency graph?**
  Converting a file that other unconverted C++ files depend on forces
  simultaneous changes across multiple files and multiplies failure
  modes. Leaf-first ordering avoids this.

- [ ] **Has a clean build been verified before starting?**
  Stale `.o` files from previous C++ compilation mask real errors in
  the converted `.c` file, causing false-positive builds that break
  later.

- [ ] **Has the build system been updated to compile the new `.c` file?**
  Renaming `.cpp` to `.c` without updating build rules means the file
  is silently skipped, and stale object files link instead.

---

## 2. During conversion

These checks apply while writing the C replacement code.

- [ ] **Does the `.c` file avoid all C++ headers?**
  A `.c` file compiled as C cannot include `<utility>`, `<vector>`,
  `<string>`, or any header that transitively pulls them in.
  (`CppHeaderInCFile`)

- [ ] **Does every `#include` resolve to a C-safe header?**
  Check each included header for transitive C++ includes. If a
  project header includes C++ standard headers, either guard the
  include with `#ifdef __cplusplus`, create a separate `_c.h` header,
  or forward-declare the needed items.

- [ ] **Are all `enum class` values replaced with `int` constants?**
  C has no `enum class`. Replace with anonymous `enum` or `#define`
  constants and change function parameters to `int`.

- [ ] **Are architecture guards (`#ifdef`) present on all arch-specific fields?**
  Accessing struct fields that only exist under architecture guards
  (e.g. `#ifdef __aarch64__`) without guards causes compile errors on
  the other architecture. (`ArchGuardMissing`)

- [ ] **Does `offsetof` have an explicit `#include <stddef.h>`?**
  Some framework headers use include guards that prevent `offsetof`
  from being defined via transitive includes. The explicit include
  must appear AFTER the framework header. (`MissingInclude`)

- [ ] **Are static initialisers free of extern variable addresses?**
  C does not allow extern variable addresses as compile-time constants
  in static array initialisers. Use a lazy initialisation function
  instead. (`NonConstStaticInit`)

- [ ] **Are all references to C++ name-mangled symbols handled via address-passing?**
  A `.c` file cannot forward-declare a C++ function because the
  linker will look for the unmangled C symbol. Instead, the C++ side
  must resolve the symbol and pass its address to C via an init
  function. (`NameManglingMismatch`)

- [ ] **Does every `_init` have a paired `_free`?**
  C++ RAII destructors run automatically; C cleanup does not. Every
  struct with an `_init` function must have a corresponding `_free`,
  and every code path that calls `_init` must reach `_free`.

- [ ] **Are explicit constructors handled at implicit conversion sites?**
  C++ `emplace_back` and similar do not perform `explicit`
  constructor conversions. Wrapper types must be explicitly converted
  to the expected type at call sites. (`ExplicitConstructor`)

- [ ] **Are C++ lambdas replaced with static functions and context structs?**
  C has no closures. Each lambda becomes a static callback function
  with an explicit `void *ctx` parameter, plus a small struct to
  carry the captured state.

---

## 3. Pre-compilation

These checks apply after writing the `.c` file but before the first
compile attempt.

- [ ] **Does the header have `extern "C"` guards?**
  Without `extern "C"` blocks around C function declarations, C++
  callers will look for name-mangled symbols that do not exist.
  (`NameManglingMismatch`)

- [ ] **Do C++ wrapper functions exist inside `#ifdef __cplusplus`?**
  Unconverted C++ callers still expect the original namespace API.
  Inline C++ wrappers in the header delegate to the C API without
  requiring changes to callers.

- [ ] **Are all function names consistent between declaration and definition?**
  Using `operand_create` in the `.c` file while the header declares
  `operand_new` produces undefined-reference linker errors. Match
  declarations exactly. (`ApiNamingMismatch`)

- [ ] **Is there a `_Static_assert` on struct sizes where layout compatibility matters?**
  When C structs must match a binary layout (serialised data,
  shared-memory structures), a `_Static_assert` on `sizeof` catches
  silent layout drift at compile time.

- [ ] **Have forward declarations been removed in favour of the canonical header?**
  Adding local forward declarations risks type mismatches with the
  canonical header. All implementation files should `#include` the
  shared header and conform to its signatures.
  (`CrossFileDeclarationMismatch`)

---

## 4. Post-compilation

These checks apply after the first successful compile.

- [ ] **Was the build a clean build (`make clean && make`)?**
  Incremental builds can link stale `.o` files from the old C++
  version, producing a binary that appears correct but contains the
  old code.

- [ ] **Is there zero regression in the test suite?**
  Compare test results against the pre-conversion baseline. The
  count must match or improve. New failures indicate conversion bugs.

- [ ] **Does generated output match the baseline?**
  For compiler conversions, compare generated code before and after.
  Small differences (register allocation, ASLR) are acceptable;
  structural differences indicate a conversion bug.

- [ ] **Are there duplicate implementation files to clean up?**
  Parallel work streams can independently create C implementations
  for the same module. Delete the redundant file before merging.
  (`DuplicateImplementation`)

- [ ] **Has the conversion been verified on all target architectures?**
  Architecture-guarded fields mean the code compiles differently per
  platform. A file that builds on x86_64 may fail on ARM64 or vice
  versa. (`ArchGuardMissing`)

---

## 5. Interop

These checks verify correctness at the C/C++ boundary.

- [ ] **Are all `enum class` values cast to `int` at the C/C++ boundary?**
  C functions take `int`; C++ callers hold `enum class` values.
  The C++ wrapper must cast explicitly. Implicit conversion from
  `enum class` to `int` is not allowed in C++.

- [ ] **Do opaque pointer typedefs (`void*`) have accessor functions in the bridge?**
  C code must not cast `void*` handles to C++ types. All access goes
  through the bridge layer, which performs the cast and calls the
  class method.

- [ ] **Does the bridge return correct types (not entire structs where scalars are expected)?**
  Returning a struct where the caller expects an `int` produces wrong
  values silently. (`AccessorTypeMismatch`)

- [ ] **Is iterator invalidation handled by the C++ side?**
  C code must not hold raw C++ iterators. For mid-traversal removal,
  expose a callback-based function where C++ manages the iterator
  internally.

- [ ] **Do coexisting C and C++ files avoid duplicate symbol definitions?**
  When a `.c` implementation coexists with the original `.cpp` during
  migration, both compile into the same binary. Ensure the bridge
  routes through one implementation, not both.
  (`DuplicateImplementation`)
