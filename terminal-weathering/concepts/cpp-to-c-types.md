# C++ to C Conversion: Type Definitions

Type definitions for the C++ to C porting reference, written in
[Honest](../lib/honest/specs/honest-spec.md) — a Pascal-based
data definition language used throughout the NBS framework.

Honest declarations are self-describing: they carry their own type
definitions, so a reader encountering these types for the first time
can understand them without external documentation. Code blocks
marked `pascal` are Honest — they are authoritative. Comments in
braces `{ }` explain intent.

These types are the vocabulary for the companion documents
(`cpp-to-c-patterns.md`, `cpp-to-c-build-failures.md`,
`cpp-to-c-checklist.md`). They define the domain of C++ to C
conversion in general — not specific to any one project. Where
examples are cited, they come from real-world JIT compiler
conversions. See `cpp-to-c-analysis.md` for the full catalogue
of code examples.

---

## 1. C++ Features and C Replacement Patterns

```pascal
type
  CppFeature = (
    { Language features }
    Namespace,            { namespace X { ... } scoping }
    EnumClass,            { enum class X { ... } with scoped enumerators }
    ClassMethod,          { member functions with implicit this pointer }
    VirtualDispatch,      { virtual functions and vtable-based polymorphism }
    TemplateGeneric,      { template<typename T> for generic programming }
    TemplateVariadic,     { template<typename... Args> variadic templates }
    CRTP,                 { Curiously Recurring Template Pattern: class X : Base<X> }
    OperatorOverload,     { operator+, operator<<, etc. }
    RAII,                 { Resource Acquisition Is Initialisation — constructor/destructor pairs }
    SmartPointer,         { std::unique_ptr, std::shared_ptr }
    ExceptionHandling,    { try/catch/throw }
    ReferenceParam,       { T& and const T& function parameters }
    ForwardDecl,          { forward declarations with C++ linkage }
    StaticAssert,         { static_assert(expr, msg) }
    ConstexprIf,          { if constexpr (expr) { ... } compile-time branching }
    Lambda,               { [captures](params){ body } anonymous functions }

    { Standard library containers }
    StdVector,            { std::vector<T> dynamic array }
    StdMap,               { std::map<K,V> and std::unordered_map<K,V> }
    StdSet,               { std::set<T> and std::unordered_set<T> }
    StdQueue,             { std::queue<T> FIFO container adaptor }
    StdDeque,             { std::deque<T> double-ended queue }

    { Standard library types }
    StdString,            { std::string and std::string_view }
    StdOptional,          { std::optional<T> nullable value }
    StdVariant,           { std::variant<Ts...> type-safe union }
    StdFunction,          { std::function<R(Args...)> type-erased callable }

    { Standard library I/O and concurrency }
    StdOstream,           { std::ostream, operator<<, fmt::format }
    StdMutex,             { std::mutex, std::lock_guard }
    StdAtomic             { std::atomic<T> lock-free concurrency }
  );
```

```pascal
type
  CPattern = (
    { Naming and scoping }
    PrefixedFreeFunction,   { namespace::func() becomes prefix_func() }
    ExternCBlock,           { extern "C" { ... } linkage specification }

    { Struct and method patterns }
    ExplicitStructPointer,  { class methods become func(Struct *self, ...) }
    TaggedUnion,            { std::variant or class hierarchy becomes struct with discriminator + union }
    OpaquePointer,          { void* handle with C accessor functions in a bridge layer }
    InlineWrapper,          { #ifdef __cplusplus inline functions forwarding to extern C API }

    { Function patterns }
    FunctionPointerTable,   { virtual dispatch becomes struct of function pointers or enum + switch }
    CallbackWithContext,    { lambda becomes static function + void *ctx parameter }

    { Container replacements }
    GrowableArray,          { std::vector/queue/stack becomes malloc+realloc array }
    FixedSizeArray,         { bounded collections become fixed-size stack arrays }
    BitSet,                 { std::unordered_set with sequential integer keys becomes bit array }
    HashTable,              { std::unordered_map/set becomes hash table (e.g. open-addressing or chaining implementation) }
    IntrusiveList,          { std::list becomes embedded next/prev pointers in struct }

    { Memory and lifecycle }
    ArenaAllocator,         { bulk allocation with single free }
    ExplicitCleanup,        { RAII becomes explicit _init/_free or _new/_free pairs }
    OutParamReturn,         { std::optional becomes int return code + output parameter }

    { Type and initialisation patterns }
    EnumToInt,              { enum class becomes plain enum or #define constants + int params }
    LazyInitTable,          { static arrays with extern addresses become lazy-init on first call }
    StaticAssertLayout,     { static_assert becomes _Static_assert for struct layout checks }
    AddressPassingInit,     { C++ mangled symbols passed as uint64_t addresses via init function }
    MemcpyTypePun,          { type punning via memcpy for strict aliasing safety }
    PreprocessorConditional { if constexpr becomes #if preprocessor or runtime conditional }
  );
```

---

## 2. Risk Classification

```pascal
type
  ConversionRisk = (Safe, Moderate, Dangerous);
  { Safe:      mechanical transformation, no semantic change.
    A correct converter produces correct output every time.
    Examples: namespace removal, enum class to int, static_assert. }
  { Moderate:  requires design decisions, multiple valid approaches.
    The converter must choose; wrong choice compiles but may produce
    subtle bugs or unnecessary complexity.
    Examples: container replacement strategy, interop mechanism choice. }
  { Dangerous: easy to introduce bugs, needs careful testing.
    Memory safety, ownership, or dispatch semantics change during
    conversion. Manual review and testing are non-negotiable.
    Examples: RAII to manual cleanup, virtual dispatch elimination,
    CRTP bridge construction, iterator invalidation patterns. }
```

---

## 3. Conversion Entries

```pascal
type
  FilePath = String;
  Pitfall = String;
  Pitfalls = sequence of Pitfall;

  ConversionEntry = record
    cpp_feature    : CppFeature;
    c_pattern      : CPattern;
    risk           : ConversionRisk;
    description    : String;
    before_file    : FilePath;  { path to original C++ file }
    after_file     : FilePath;  { path to converted C file }
    pitfalls       : Pitfalls;
  end;
```

Each `ConversionEntry` links one C++ feature to one C replacement
pattern with the risk level, real file paths demonstrating the
conversion, and a list of pitfalls discovered during conversion.
See `cpp-to-c-patterns.md` for the full set of entries.

---

## 4. Interop Mechanisms

```pascal
type
  InteropMechanism = (
    InlineCppWrapper,
    { C++ header provides inline functions forwarding to extern C.
      Preserves the original C++ API for unconverted callers while
      the implementation moves to C. }
    OpaqueCApi,
    { void* handles with C accessor functions in a bridge .cpp file.
      C code receives opaque handles; the bridge casts to C++ types
      and calls class methods. Temporary scaffolding — deleted when
      underlying types become C structs. }
    AddressPassing,
    { C++ side resolves name-mangled symbol and passes raw address
      to C via an init function. Needed when C code requires a
      function pointer to a C++ function it cannot forward-declare. }
    EnumCasting,
    { enum class values cast to int at the C/C++ boundary.
      C functions take int parameters; C++ wrappers perform the cast. }
    SeparateCApiHeader,
    { Dedicated _c.h header for complex C APIs, separate from the
      main .h which retains C++ wrappers. Prevents C++ leakage
      into .c translation units. }
    CoexistingImplementation
    { C and C++ implementation files coexist during phased conversion.
      Both compile. A bridge routes callers to one or the other.
      After migration completes, the C++ file is deleted. }
  );
```

---

## 5. Build Failure Classification

```pascal
type
  BuildFailureClass = (
    { C/C++ language incompatibilities }
    CppHeaderInCFile,
    { .c file includes a header that transitively includes C++ standard
      library headers (e.g. <utility>, <vector>). The C compiler
      cannot parse these. }
    NameManglingMismatch,
    { C file forward-declares a function with C linkage; the definition
      has C++ linkage (name-mangled). The linker cannot resolve the
      symbol. }
    NonConstStaticInit,
    { Static array initialiser contains extern variable addresses.
      Valid in C++ (addresses resolved at load time). Invalid in C
      (initialiser elements MUST be compile-time constants). }
    MissingInclude,
    { A macro or function (e.g. offsetof) is available implicitly in
      C++ but requires an explicit #include in C. Build fails with
      implicit declaration error. }
    ArchGuardMissing,
    { Platform-conditional struct fields or code paths accessed without
      #ifdef guards. Compiles on one architecture, fails on another. }
    ExplicitConstructor,
    { C++ explicit keyword prevents implicit type conversion at call
      sites. C has no equivalent — the conversion must be written
      explicitly. }

    { Multi-file coordination failures }
    CrossFileDeclarationMismatch,
    { Multiple .c files cross-reference each other with inconsistent
      type declarations or function signatures. Typically occurs when
      converted files share types but use separate forward declarations
      instead of a common header. }
    DuplicateImplementation,
    { Two agents or two commits independently create a C implementation
      for the same module, producing duplicate symbol definitions at
      link time. }
    ApiNamingMismatch,
    { Function names in .c implementation files differ from the
      declarations in the API header (e.g. _create vs _new). Produces
      undefined reference errors at link time. }
    AccessorTypeMismatch,
    { A bridge accessor function returns the wrong type — e.g. returns
      a struct where a scalar field of that struct was expected. Compiles
      in C++ (implicit conversion) but produces wrong values in C. }

    { Process failures }
    ConcurrentBuild
    { Two agents or developers building simultaneously, corrupting
      build state or deleting each other's output binaries. }
  );
```

---

## 6. Conversion Process Metadata

```pascal
type
  { ConversionPhase classifies the structural complexity of each
    stage in a phased conversion. Ordered by increasing difficulty. }
  ConversionPhase = (
    LeafSwap,        { self-contained files with no cross-file C++ dependencies }
    OpaqueApi,       { files requiring opaque pointer accessors to C++ objects }
    CoreType,        { core type definitions: C structs coexisting with C++ classes }
    CallerMigration, { bulk migration of callers from C++ API to C API }
    BridgeDeletion   { removal of bridge .cpp files once all callers migrated }
  );

  { GateResult records the verification evidence for a conversion step.
    MUST be defined before ConversionStep (topological ordering). }
  GateResult = record
    test_suite_pass    : Integer;  { tests passing after conversion }
    test_suite_total   : Integer;  { total tests in suite }
    jit_regressions    : Integer;  { MUST be 0 for gate to pass }
    correctness_pass   : Integer;  { benchmark correctness checks passing }
    correctness_total  : Integer;  { total correctness checks }
    asan_clean         : Boolean;  { AddressSanitiser reports no errors }
    arm64_verified     : Boolean;  { verified on ARM64 as well as x86_64 }
  end;

  FailuresEncountered = sequence of BuildFailureClass;

  PatternsUsed = sequence of CPattern;

  ConversionStep = record
    step_number    : Integer;
    source_file    : String;    { original .cpp path }
    target_file    : String;    { converted .c path }
    lines_cpp      : Integer;   { lines of C++ removed }
    lines_c        : Integer;   { lines of C added }
    commit         : String;    { git commit hash }
    phase          : ConversionPhase;
    failures       : FailuresEncountered;
    patterns_used  : PatternsUsed;
    risk           : ConversionRisk;
    gate_result    : GateResult;
  end;
```

---

## 7. Naming Conventions

```pascal
type
  NamingScope = (Subsystem, Type, Function, Constant);

  NamingConvention = record
    scope          : NamingScope;
    pattern        : String;    { e.g. 'prefix_{subsystem}_{function}' }
    example        : String;    { e.g. 'jit_elf_strtab_insert' }
  end;
```

General naming rules for C conversions:

| Element | Convention | Rationale |
|---------|-----------|-----------|
| Functions | `prefix_subsystem_verb` (snake_case) | Replaces namespace scoping; prefix prevents collisions |
| Types (struct) | `PrefixSubsystemType` (PascalCase) | Distinct from functions; matches C convention |
| Types (opaque) | `typedef void* PrefixType` | Hides C++ implementation from C callers |
| Constants | `PREFIX_SUBSYSTEM_NAME` (UPPER_SNAKE) | Replaces enum class scoped values |
| Lifecycle | `prefix_type_init` / `prefix_type_free` | Replaces constructor/destructor pairs |

---

## 8. Combining Type Blocks

The Honest fragments above are split across multiple code blocks for
readability. To combine them into a single `.honest` file, merge all
declarations under one `type` keyword in topological order:

1. `CppFeature` (no dependencies)
2. `CPattern` (no dependencies)
3. `ConversionRisk` (no dependencies)
4. `FilePath`, `Pitfall` (alias `String`)
5. `Pitfalls` (depends on `Pitfall`)
6. `ConversionEntry` (depends on `CppFeature`, `CPattern`, `ConversionRisk`, `FilePath`, `Pitfalls`)
7. `InteropMechanism` (no dependencies)
8. `BuildFailureClass` (no dependencies)
9. `ConversionPhase` (no dependencies)
10. `NamingScope` (no dependencies)
11. `NamingConvention` (depends on `NamingScope`)
12. `GateResult` (no dependencies)
13. `FailuresEncountered` (depends on `BuildFailureClass`)
14. `PatternsUsed` (depends on `CPattern`)
15. `ConversionStep` (depends on `ConversionPhase`, `FailuresEncountered`, `PatternsUsed`, `ConversionRisk`, `GateResult`)

No forward references. No cycles. Single-pass parseable.

---

## 9. What Would Falsify These Types?

These type definitions are wrong IF:

1. **A real C++ to C conversion encounters a C++ feature not in
   `CppFeature`.** Test: attempt conversion of a C++ codebase using
   C++20 modules, coroutines, or concepts. These are absent because
   no conversion evidence exists for them yet.

2. **A real conversion requires a C pattern not in `CPattern`.**
   Test: convert a C++ codebase with deep inheritance hierarchies
   (3+ levels), multiple inheritance, or heavy use of `dynamic_cast`.
   The current patterns assume shallow hierarchies with few subclasses.

3. **A build failure occurs that does not fit any `BuildFailureClass`.**
   Test: convert code that uses C++ exceptions caught across the
   C/C++ boundary, or code that uses C++ thread_local variables
   accessed from C. These may produce failure classes not yet catalogued.

4. **The `ConversionPhase` ordering does not match reality.** Test:
   attempt a conversion that skips `LeafSwap` and starts with
   `CoreType`. If this succeeds without additional failures, the
   phase ordering is descriptive rather than prescriptive.
