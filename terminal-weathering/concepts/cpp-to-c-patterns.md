# C++ to C Conversion Patterns Reference

A comprehensive reference for converting C++ codebases to pure C.
Each pattern explains what to do, why, and what goes wrong if you
get it wrong.

Every pattern is illustrated with generic before/after code
examples. The patterns are general -- applicable to any C++ to C
conversion.

Honest type references (e.g. `CppFeature.Namespace`) refer to
definitions in `cpp-to-c-types.md`.

---

## Pattern 1: Namespace Functions -- Prefixed Free Functions

**C++ feature:** `CppFeature.Namespace` -- C++ namespaces scope functions
and types, preventing name collisions.

**C pattern:** `CPattern.PrefixedFreeFunction` -- Free functions with a
subsystem prefix replace namespace scoping.

**Risk:** `ConversionRisk.Safe` -- Mechanical renaming with no semantic
change.

**Before** (C++):
```cpp
namespace module::subsystem::backend {

static std::optional<MemRef>
subsystem_try_offset(const RegBase& base, int32_t offset, AccessSize access_size) {
  if (offset >= -256 && offset < 256) {
    return make_memref(base, offset);
  }
  // ...
  return std::nullopt;
}

MemRef subsystem_offset(const RegBase& base, int32_t offset,
                        AccessSize access_size) {
  auto opt = subsystem_try_offset(base, offset, access_size);
  CHECK(opt.has_value(), "offset out of range");
  return opt.value();
}

} // namespace module::subsystem::backend
```

**After** (C):
```c
static int
module_subsystem_try_offset(MemResult *out, RegHandle base, int32_t offset,
                            int32_t access_size) {
    if (offset >= -256 && offset < 256) {
        *out = make_memref(base, offset);
        return 1;
    }
    // ...
    return 0;
}

MemResult
module_subsystem_offset(RegHandle base, int32_t offset, int32_t access_size) {
    MemResult result;
    int ok = module_subsystem_try_offset(&result, base, offset, access_size);
    assert(ok && "module_subsystem_offset: offset out of range");
    (void)ok;
    return result;
}
```

**Naming convention:** `module_` + subsystem + `_` + function name.
Examples: `module_subsystem_offset`, `module_section_name`,
`module_ir_bit_size`, `module_strtab_insert`.

**Pitfalls:**
- Forgetting to update all callers of the namespace function.
- The inline C++ wrapper in the header must match the original
  signature exactly for unconverted callers.

**Interop:** The header retains the namespace API via inline C++
wrappers that delegate to the C functions:
```cpp
#ifdef __cplusplus
namespace module::subsystem::backend {
inline MemRef subsystem_offset(const RegBase& base, int32_t offset,
                               AccessSize access_size) {
    return module_subsystem_offset(base, offset, (int32_t)access_size);
}
} // namespace
#endif
```

---

## Pattern 2: enum class -- Plain Enum with Int Cast

**C++ feature:** `CppFeature.EnumClass` -- Scoped enumerations that
prevent implicit integer conversion.

**C pattern:** `CPattern.EnumToInt` -- Anonymous enum with prefixed
constants; functions take `int` parameters.

**Risk:** `ConversionRisk.Safe` -- Direct mapping, no logic change.

**Before** (C++):
```cpp
size_t bitSize(DataType dt) {
    switch (dt) {
    case DataType::k8bit:   return 8;
    case DataType::k16bit:  return 16;
    case DataType::k32bit:  return 32;
    case DataType::k64bit:
    case DataType::kDouble:
    case DataType::kObject: return 64;
    }
    throw std::runtime_error{fmt::format("Unrecognised DataType: {}", dt)};
}
```

**After** (C):
```c
enum {
    MODULE_IR_DT_8BIT = 0,
    MODULE_IR_DT_16BIT,
    MODULE_IR_DT_32BIT,
    MODULE_IR_DT_64BIT,
    MODULE_IR_DT_DOUBLE,
    MODULE_IR_DT_OBJECT,
};

size_t
module_ir_bit_size(int dt) {
    switch (dt) {
    case MODULE_IR_DT_8BIT:    return 8;
    case MODULE_IR_DT_16BIT:   return 16;
    case MODULE_IR_DT_32BIT:   return 32;
    case MODULE_IR_DT_64BIT:
    case MODULE_IR_DT_DOUBLE:
    case MODULE_IR_DT_OBJECT:  return 64;
    }
    fprintf(stderr, "module: %s:%d -- Unrecognised DataType: %d\n",
            __FILE__, __LINE__, dt);
    abort();
}
```

**Pitfalls:**
- Enum values MUST match the C++ `enum class` ordinals exactly.
  `DataType::k8bit = 0`, `DataType::k16bit = 1`, etc.
- At the C/C++ boundary, the C++ caller casts:
  `module_ir_bit_size(static_cast<int>(dt))`.

**Interop:** C++ wrapper in the header casts enum class to int:
```cpp
inline size_t bitSize(DataType dt) {
    return module_ir_bit_size(static_cast<int>(dt));
}
```

---

## Pattern 3: Class Methods -- Explicit Struct Pointer

**C++ feature:** `CppFeature.ClassMethod` -- Member functions operating
on `this`.

**C pattern:** `CPattern.ExplicitStructPointer` -- C struct with
explicit pointer parameter replaces implicit `this`.

**Risk:** `ConversionRisk.Safe` -- Mechanical transformation.

**Before** (C++ `StringTable` class):
```cpp
class StringTable {
 public:
  uint32_t insert(std::string_view s) { /* ... */ }
 private:
  std::vector<uint8_t> data_{0};
};
```

**After** (C):
```c
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} ModuleStrTab;

void module_strtab_init(ModuleStrTab *st);
void module_strtab_free(ModuleStrTab *st);
uint32_t module_strtab_insert(ModuleStrTab *st, const char *s, size_t slen);
```

**Pitfalls:**
- Every method call `obj.method(arg)` becomes `prefix_method(&obj, arg)`.
- Constructor logic moves to `_init`, destructor logic to `_free`.
- Must not forget to call `_free` -- see Pattern 10 (RAII).

**Interop:** C++ class in the header wraps the C struct:
```cpp
#ifdef __cplusplus
class StringTable {
 public:
  uint32_t insert(std::string_view s) {
      return module_strtab_insert(&impl_, s.data(), s.size());
  }
 private:
  ModuleStrTab impl_;
};
#endif
```

---

## Pattern 4: std::vector -- Growable Array

**C++ feature:** `CppFeature.StdVector` -- Dynamically-sized array with
amortised O(1) append.

**C pattern:** `CPattern.GrowableArray` -- `malloc`/`realloc` array with
explicit length and capacity.

**Risk:** `ConversionRisk.Moderate` -- Design choice between PtrVec,
fixed-size array, or project-specific growable array type.

**Before** (C++ uses `std::vector<BasicBlock*>`):
```cpp
std::vector<BasicBlock*> result;
result.reserve(basic_blocks_.size());
for (auto& sccblock : scc_blocks_) {
    result.emplace_back(*(sccblock->basic_blocks.begin()));
}
```

**After** (C):
```c
typedef struct {
    void **items;
    size_t len;
    size_t cap;
} PtrVec;

static void ptrvec_push(PtrVec *v, void *item) {
    if (v->len >= v->cap) {
        size_t new_cap = v->cap ? v->cap * 2 : 8;
        v->items = (void **)realloc(v->items,
            new_cap * sizeof(void *));
        v->cap = new_cap;
    }
    v->items[v->len++] = item;
}
```

**Pitfalls:**
- Must call `ptrvec_free` on every exit path (no RAII).
- Doubling strategy uses `realloc` -- check for NULL if the allocator
  does not abort on failure.
- `emplace_back` with constructor arguments has no direct equivalent --
  construct separately then push.

**Interop:** The converted `.c` file uses `PtrVec` internally.
The header's C++ wrapper returns `std::vector` by converting:
```cpp
std::vector<BasicBlock*> result(
    reinterpret_cast<BasicBlock**>(sorted),
    reinterpret_cast<BasicBlock**>(sorted) + out_count);
free(sorted);
return result;
```

---

## Pattern 5: std::unordered_map/set -- Hash Table or Alternatives

**C++ feature:** `CppFeature.StdMap` / `CppFeature.StdSet` -- Hash-based
associative containers.

**C pattern:** `CPattern.HashTable` or `CPattern.BitSet` -- depends on
the use case.

**Risk:** `ConversionRisk.Moderate` -- Multiple valid approaches;
choice depends on element count and type.

Three common replacements:

**5a. Hash table** for pointer-keyed maps:
```c
#include "hashtable.h"

s->blocks = hashtable_new(hashtable_hash_ptr,
                          hashtable_compare_direct);
hashtable_set(s->blocks, block, (void*)1);

// Lookup:
void *val;
int found = hashtable_get(s->blocks, key, &val);
```

**5b. Bitmap** for integer-keyed sets:
```c
typedef struct {
    uint8_t *bits;
    int capacity;
} BitSet;

static int bitset_contains(const BitSet *s, int id) {
    return (s->bits[id / 8] >> (id % 8)) & 1;
}
```

**5c. Fixed-size array** for bounded sets:
```c
IrBlock branched[256];
size_t num_branched = 0;
// Linear search -- acceptable for <= 256 entries
```

**Pitfalls:**
- If using a platform-specific hash table API, include the correct
  internal header (not a public header).
- Bitmap requires knowing the maximum element ID at allocation time.
- Linear scan degrades for large N; profile before choosing.

**Interop:** All three approaches are C-only. No C++ wrapper needed.

---

## Pattern 6: std::optional -- Out-Parameter with Int Return

**C++ feature:** `CppFeature.StdOptional` -- Value-or-nothing wrapper.

**C pattern:** `CPattern.OutParamReturn` -- Function returns int
(1=success, 0=failure), writes result to output pointer.

**Risk:** `ConversionRisk.Safe` -- Direct translation.

**Before** (C++):
```cpp
static std::optional<MemRef>
subsystem_try_offset(const RegBase& base, int32_t offset, AccessSize sz) {
    if (offset >= -256 && offset < 256) {
        return make_memref(base, offset);
    }
    return std::nullopt;
}
```

**After** (C):
```c
static int
module_subsystem_try_offset(MemResult *out, RegHandle base, int32_t offset,
                            int32_t access_size) {
    if (offset >= -256 && offset < 256) {
        *out = make_memref(base, offset);
        return 1;
    }
    return 0;
}
```

**Pitfalls:**
- Caller must check return value before using `*out`.
- `std::optional::value_or(default)` has no direct equivalent --
  check and set default manually.

**Interop:** C++ wrapper returns `std::optional`:
```cpp
inline std::optional<MemRef> subsystem_try_offset(...) {
    MemResult out;
    if (module_subsystem_try_offset(&out, base, offset, sz))
        return MemRef(out);
    return std::nullopt;
}
```

---

## Pattern 7: std::variant -- Tagged Union

**C++ feature:** `CppFeature.StdVariant` -- Type-safe discriminated
union.

**C pattern:** `CPattern.TaggedUnion` -- Struct with type tag (uint8_t)
and explicit union.

**Risk:** `ConversionRisk.Moderate` -- Requires identifying all active
members and their discriminator values.

**Before** (C++ OperandBase class hierarchy, ~48 bytes per operand):
```cpp
class OperandBase {
    virtual Type type() const = 0;    // vtable dispatch
    virtual uint64_t getConstant() const = 0;
    // ... 14 virtual methods
};
class Operand : public OperandBase {
    std::variant<uint64_t, void*, BasicBlock*,
                 unique_ptr<MemoryIndirect>, PhyLocation> value_;
};
class LinkedOperand : public OperandBase {
    Operand* def_opnd_;               // delegates all getters
};
```

**After** (C):
```c
struct IrOperand {
    IrInstruction *parent_instr;
    uint8_t is_linked;           /* replaces vtable */
    uint8_t last_use;
    uint8_t type;                /* OperandType enum */
    uint8_t data_type;           /* DataType enum */
    union {
        uint64_t imm;
        void *mem_addr;
        void *label;             /* BasicBlock* */
        IrMemoryIndirect *indirect;
        IrPhyLocation phy_loc;
        IrOperand *def_opnd;     /* when is_linked=1 */
    } value;
};
```

50% smaller (24 bytes vs ~48 bytes). Virtual dispatch replaced by
`if (op->is_linked) { return op->value.def_opnd->...; }`.

**Pitfalls:**
- The `type` field MUST be set before accessing the union.
- `is_linked` must be checked in EVERY accessor.
- `unique_ptr<MemoryIndirect>` ownership becomes manual -- see
  Pattern 10.

**Interop:** C struct coexists with C++ class. A bridge translation
unit casts between them during the transition. Once all callers are
converted, the C++ class is deleted.

---

## Pattern 8: std::queue/deque -- Ring Buffer or Growable Array

**C++ feature:** `CppFeature.StdQueue` / `CppFeature.StdDeque` -- FIFO
queue backed by deque.

**C pattern:** `CPattern.GrowableArray` -- Linear array used as queue
with head/tail indices.

**Risk:** `ConversionRisk.Moderate` -- Must handle growth correctly.

**Before** (C++):
```cpp
std::queue<BasicBlock*> worklist;
worklist.push(succ);
BasicBlock* block = worklist.front();
worklist.pop();
```

**After** (C):
```c
typedef struct {
    IrBlock *items;
    size_t head;
    size_t tail;
    size_t capacity;
} BlockQueue;

static void queue_push(BlockQueue *q, IrBlock block) {
    if (q->tail >= q->capacity) {
        size_t new_cap = q->capacity * 2;
        q->items = (IrBlock *)realloc(
            q->items, new_cap * sizeof(IrBlock));
        q->capacity = new_cap;
    }
    q->items[q->tail++] = block;
}

static IrBlock queue_pop(BlockQueue *q) {
    return q->items[q->head++];
}
```

**Pitfalls:**
- This is a non-circular queue -- memory before `head` is wasted.
  Acceptable when total pushes are bounded (BFS over a finite graph).
- For long-lived queues, use a circular buffer or compact on pop.
- Must call `queue_free` on every exit path.

**Interop:** Internal data structure. No C++ wrapper needed.

---

## Pattern 9: Virtual Dispatch -- Bool Flag + Switch

**C++ feature:** `CppFeature.VirtualDispatch` -- Dynamic dispatch via
vtable pointer.

**C pattern:** `CPattern.FunctionPointerTable` -- When few subclasses
exist, replace vtable with a discriminator flag and switch/if.

**Risk:** `ConversionRisk.Moderate` -- Must identify all virtual call
sites and ensure the switch covers all cases.

**Before** (C++ Operand class hierarchy with 14 virtual methods):
```cpp
class OperandBase {
    virtual Type type() const = 0;
    virtual Operand* getDefine() = 0;
    // ... 12 more virtual methods
};
// Only 2 concrete subclasses: Operand and LinkedOperand
```

**After** (C):
```c
struct IrOperand {
    uint8_t is_linked;    /* 0=Operand, 1=LinkedOperand */
    // ...
};

/* Every 'virtual' method becomes: */
static inline uint8_t
ir_operand_type(const IrOperand *op) {
    if (op->is_linked) return op->value.def_opnd->type;
    return op->type;
}
```

**Pitfalls:**
- Only viable when the number of subclasses is small (2-3).
- For larger hierarchies, use a function pointer table.
- Every accessor must check `is_linked` -- easy to forget.

**Interop:** The C API bridge translation unit bridges between the C++
virtual dispatch and the C flag-based dispatch during the transition.

---

## Pattern 10: RAII / Destructors -- Explicit `_init`/`_free` Pairs

**C++ feature:** `CppFeature.RAII` -- Resource cleanup tied to object
lifetime via destructors.

**C pattern:** `CPattern.ExplicitCleanup` -- Explicit `_init`/`_free`
function pairs. `goto cleanup` pattern for error handling.

**Risk:** `ConversionRisk.Dangerous` -- Every exit path must call
`_free`. Missing a path causes a memory leak; calling twice causes
double-free.

**Before** (C++ RegisterPreserver with RAII):
```cpp
class RegisterPreserver {
 public:
  RegisterPreserver(Builder* as, const std::vector<std::pair<Reg,Reg>>& regs)
      : num_regs_(regs.size()) { /* ... */ }
  void preserve();
  void restore();
  // destructor: implicit cleanup
};
```

**After** (C):
```c
typedef struct {
    AsmBuilder* builder;
    const RegPair* regs;
    int num_regs;
    int align_stack;
} RegPreserver;

void reg_preserver_init(RegPreserver* rp, AsmBuilder* builder,
                        const RegPair* regs, int num_regs);
void reg_preserver_preserve(RegPreserver* rp);
void reg_preserver_restore(RegPreserver* rp);
/* No _free needed: RegPreserver owns no heap memory */
```

For types that own heap memory (e.g. `ModuleStrTab`, `PtrVec`):
```c
ModuleStrTab st;
module_strtab_init(&st);
/* ... use st ... */
module_strtab_free(&st);   /* MUST be called */
```

**Pitfalls:**
- Every `_init` MUST have a paired `_free` on ALL exit paths.
- Use `goto cleanup` pattern when a function has multiple early
  returns:
  ```c
  int do_work(void) {
      PtrVec vec;
      ptrvec_init(&vec, 16);
      if (error_condition) goto cleanup;
      /* ... */
  cleanup:
      ptrvec_free(&vec);
      return result;
  }
  ```
- The `goto cleanup` pattern is the standard C idiom for
  multi-resource cleanup with multiple exit paths.

**Interop:** C++ header provides RAII wrapper:
```cpp
#ifdef __cplusplus
class StringTable {
 public:
  StringTable() { module_strtab_init(&impl_); }
  ~StringTable() { module_strtab_free(&impl_); }
  // ...
 private:
  ModuleStrTab impl_;
};
#endif
```

---

## Pattern 11: std::unique_ptr -- Manual Ownership with Explicit Free

**C++ feature:** `CppFeature.SmartPointer` -- Automatic ownership
transfer and destruction.

**C pattern:** `CPattern.ExplicitCleanup` -- Raw pointers with explicit
free calls and clear ownership rules.

**Risk:** `ConversionRisk.Dangerous` -- Ownership transfer is implicit
in C++; in C it must be documented and enforced manually.

**Before** (C++ instruction inputs use `unique_ptr`):
```cpp
std::vector<std::unique_ptr<OperandBase>> inputs_;
// Ownership clear: instruction owns its input operands.
// Destructor automatically frees all inputs.
```

**After** (C):
```c
struct IrInstruction {
    IrOperand **inputs;        /* owned array of input operand pointers */
    size_t num_inputs;
    size_t inputs_capacity;
    /* ... */
};
```

Lifecycle:
```c
IrOperand *op = ir_operand_new();
ir_instr_add_input(instr, op);  /* ownership transferred to instr */
/* Do NOT free op after this point */

/* When instruction is freed: */
void ir_instr_free(IrInstruction *instr) {
    for (size_t i = 0; i < instr->num_inputs; i++) {
        ir_operand_free(instr->inputs[i]);
    }
    free(instr->inputs);
    /* ... */
}
```

**Pitfalls:**
- Double-free if ownership is not transferred cleanly via
  `removeInput`/`appendInput`.
- Dangling pointer if operand is freed while instruction still
  references it.
- Verify that operand ownership transfers are well-disciplined
  (always through container methods, never ad-hoc pointer swaps).

**Interop:** The bridge translation unit manages ownership during
the transition. Once all callers use C, ownership is explicit.

---

## Pattern 12: CRTP (Code Generator) -- Three-Layer Bridge

**C++ feature:** `CppFeature.CRTP` -- Curiously Recurring Template
Pattern for static polymorphism.

**C pattern:** Three-layer architecture: C functions -- C++ template
-- namespace shim.

**Risk:** `ConversionRisk.Dangerous` -- The CRTP pattern is deeply
embedded in code generator sources (potentially thousands of lines
with many template uses). The bridge must maintain instruction
encoding correctness.

**Architecture:**
```
Layer 3 (C):     asm_mov_rr(AsmBuilder*, RegHandle, RegHandle)
                        |
Layer 2 (C++):   EmitterExplicitT<Builder>::mov(const Gp& d, const Gp& s) {
                     asm_mov_rr(b_(), d, s);
                 }
                        | CRTP downcast via b_()
Layer 1 (C++):   Builder : EmitterExplicitT<Builder> {
                     AsmBuilder* impl() const { return impl_; }
                 };
```

**CRTP mechanism:**
```cpp
template <typename CRTP>
class EmitterExplicitT {
 protected:
  AsmBuilder* b_() { return static_cast<CRTP*>(this)->impl(); }
 public:
  Error mov(const Gp& d, const Gp& s) {
      asm_mov_rr(b_(), d, s);
      return kErrorOk;
  }
  // ~200+ instruction methods
};
```

**How the code generator uses it:**
```cpp
template <typename... Args>
struct InstructionType {
    using type = Error (EmitterExplicitT<Builder>::*)(
        typename Args::asm_type...);
};
// Creates member function pointers to template methods
```

**Pitfalls:**
- The wrapper layer is where encoding bugs concentrate. Two common
  classes of bugs: a register factory omitting a flag bit, and a
  memory operation dropping a mode bit for certain load types. Both
  cause silent wrong code generation.
- The wrapper is deleted last. Until then, converted C files call
  Layer 3 directly; unconverted C++ files go through Layers 1-2.

**Interop:** The three-layer bridge IS the interop mechanism. Files
are converted bottom-up: leaf files first (call Layer 3 directly),
wrapper files last (delete Layers 1-2). Each intermediate state compiles.

---

## Pattern 13: Templates (Variadic) -- Individual Typed Functions

**C++ feature:** `CppFeature.TemplateVariadic` -- Variadic templates
with parameter pack expansion.

**C pattern:** Individual C functions for each operand type. More
verbose but each is trivial.

**Risk:** `ConversionRisk.Moderate` -- Many call sites to update, but
each change is mechanical.

**Before** (C++):
```cpp
template <typename FirstT, typename... T>
Instruction* addOperands(FirstT&& first_arg, T&&... args) {
    using FT = std::decay_t<FirstT>;
    if constexpr (std::is_same_v<FT, PhyReg>) {
        allocatePhyRegisterInput(first_arg.value)
            ->setDataType(first_arg.data_type);
    } else if constexpr (std::is_same_v<FT, Imm>) {
        allocateImmediateInput(first_arg.value)
            ->setDataType(first_arg.data_type);
    }
    // ... 6 more constexpr-if branches
    return addOperands(std::forward<T>(args)...);  // recursive
}
```

**After** (C API):
```c
void ir_instr_add_phyreg_input(IrInstruction *instr, int loc, int dt);
void ir_instr_add_imm_input(IrInstruction *instr, uint64_t val, int dt);
void ir_instr_add_stack_input(IrInstruction *instr, int loc, int dt);
void ir_instr_add_vreg_input(IrInstruction *instr, IrOperand *def);
void ir_instr_add_indirect_input(IrInstruction *instr,
    IrOperand *base, IrOperand *index, uint8_t mult, int32_t off);
void ir_instr_set_output_phyreg(IrInstruction *instr, int loc, int dt);
void ir_instr_set_output_vreg(IrInstruction *instr, int dt);
```

**Pitfalls:**
- A single `addOperands(OutPhyReg(R0), GP(R1), Imm(42))` call becomes
  three separate C calls. Must not reorder or omit any.
- The `static_assert(output must be first)` compile-time check
  has no C equivalent -- must be enforced by convention.

**Interop:** C++ callers continue using `addOperands<>()`. The template
internally calls the C functions during the transition.

---

## Pattern 14: std::function / Lambdas -- Function Pointer + void* Userdata

**C++ feature:** `CppFeature.Lambda` / `CppFeature.StdFunction` --
Anonymous closures that capture local state.

**C pattern:** Static callback function with explicit `void *ctx`
parameter for captured state.

**Risk:** `ConversionRisk.Moderate` -- Must identify all captured
variables and package them into a context struct.

**Before** (C++):
```cpp
for_each_input(instr, [&](auto& operand) {
    if (auto* linked = operand.asLinked()) {
        worklist.push(linked->defInstr());
    }
});
```

**After** (C):
```c
/* Context struct replaces lambda capture */
typedef struct {
    BitSet *live_set;
    InstrQueue *worklist;
} DceCtx;

/* Static function replaces lambda body */
static void
trace_input_cb(IrOperandHandle operand, void *vctx) {
    DceCtx *ctx = (DceCtx *)vctx;
    if (ir_operand_is_linked(operand)) {
        IrInstrHandle def = ir_operand_get_linked_instr(operand);
        mark_live(ctx, def);
    }
}

/* C API uses function pointer + void* */
ir_instr_foreach_input(instr, trace_input_cb, &ctx);
```

**C API signature:**
```c
void ir_instr_foreach_input(
    IrInstrHandle instr,
    void (*cb)(IrOperandHandle operand, void *ctx),
    void *ctx);
```

**Pitfalls:**
- Mutable captures (`[&]`) require the context struct to contain
  pointers, not copies.
- Lifetime: context struct must outlive the callback invocation.
- Type safety lost -- `void *ctx` cast must match the struct type.

**Interop:** The callback-based C API is also usable from C++. No
wrapper needed -- C++ callers can pass lambdas to the C++ bridge
(which internally uses the C callback API).

---

## Pattern 15: std::ostream / fmt::format -- fprintf(stderr, ...)

**C++ feature:** `CppFeature.StdOstream` -- Stream-based output with
`operator<<` overloading.

**C pattern:** `fprintf(stderr, ...)` for error messages, static
`const char*` return for name functions.

**Risk:** `ConversionRisk.Safe` -- Direct mapping.

**Before** (C++):
```cpp
std::ostream& operator<<(std::ostream& os, OperandType ty) {
    switch (ty) {
    case OperandType::kNone: return os << "None";
    case OperandType::kVreg: return os << "Vreg";
    // ...
    }
}
```

**After** (C):
```c
const char*
ir_operand_type_name(int ty) {
    switch (ty) {
    case 0: return "None";
    case 1: return "Vreg";
    // ...
    }
    return "<unknown OperandType>";
}
```

The C++ wrapper forwards `operator<<` to the C name function:
```cpp
inline std::ostream& operator<<(std::ostream& os, OperandType ty) {
    return os << ir_operand_type_name(static_cast<int>(ty));
}
```

**For error messages** (C):
```c
fprintf(stderr, "module: %s:%d -- Bad code section %d\n",
        __FILE__, __LINE__, section);
abort();
```
This replaces `ABORT("Bad code section {}", ...)` and
`throw std::runtime_error{fmt::format(...)}`.

**Pitfalls:**
- `fmt::format` uses `{}` placeholders; `fprintf` uses `%d`, `%s` etc.
- `operator<<` chaining has no C equivalent -- use `fprintf` with
  multiple format specifiers in one call.

**Interop:** C++ wrappers in headers delegate `operator<<` to C name
functions.

---

## Pattern 16: static_assert -- _Static_assert

**C++ feature:** `CppFeature.StaticAssert` -- Compile-time assertion.

**C pattern:** `CPattern.StaticAssertLayout` -- C11 `_Static_assert`.

**Risk:** `ConversionRisk.Safe` -- Direct translation. Drop any
`static_assert` that uses C++ type traits (e.g. `is_standard_layout`).

**Before** (C++):
```cpp
static_assert(sizeof(SectionHeader) == 64,
    "SectionHeader must be 64 bytes");
```

**After** (C):
```c
_Static_assert(sizeof(ModuleSectionHeader) == 64,
    "SectionHeader must be 64 bytes");
```

**Pitfalls:**
- C++ `static_assert(is_standard_layout_v<T>)` has no C equivalent.
  Drop these -- C structs are standard layout by definition.
- `_Static_assert` requires a string message in C11. C23 makes it
  optional.

**Interop:** No interop needed -- compile-time only.

---

## Pattern 17: Exception Handling -- fprintf + abort or Error Codes

**C++ feature:** `CppFeature.ExceptionHandling` -- `try`/`catch`/`throw`
for error propagation.

**C pattern:** `fprintf(stderr, ...)` + `abort()` for unrecoverable
errors; int return codes for recoverable errors.

**Risk:** `ConversionRisk.Moderate` -- Must identify which exceptions
are truly unrecoverable (abort) vs recoverable (error code).

**Before** (C++):
```cpp
throw std::runtime_error{
    fmt::format("Unrecognised DataType: {}", dt)};
```

**After** (C):
```c
fprintf(stderr, "module: %s:%d -- Unrecognised DataType: %d\n",
        __FILE__, __LINE__, dt);
abort();
```

**Before** (C++ with macro):
```cpp
ABORT("Bad code section {}", static_cast<int>(section));
```

**After** (C):
```c
fprintf(stderr, "module: %s:%d -- Bad code section %d\n",
        __FILE__, __LINE__, section);
abort();
```

**Pitfalls:**
- `abort()` terminates the process. Only use for genuinely
  unrecoverable errors (corrupt state, logic errors).
- For recoverable errors, return an int error code and check it at
  every call site.
- `__FILE__` and `__LINE__` macros help locate the error source.

**Interop:** No interop needed -- error handling is internal.

---

## Pattern 18: C++ Name Mangling at Boundary -- extern "C" or Address-Passing

**C++ feature:** C++ symbols have mangled names. C files cannot link
against them.

**C pattern:** `CPattern.ExternCBlock` for declarations;
`CPattern.AddressPassingInit` when `extern "C"` is not possible.

**Risk:** `ConversionRisk.Dangerous` -- Linker errors are cryptic.
Easy to miss a mangling mismatch until link time.

**18a. extern "C" block** (every converted header):
```c
#ifdef __cplusplus
extern "C" {
#endif

MemResult module_subsystem_offset(RegHandle base, int32_t offset,
                                  int32_t access_size);

#ifdef __cplusplus
} /* extern "C" */
#endif
```

**18b. Address-passing** when the C++ symbol cannot have `extern "C"`
linkage:

```c
/* C side: receives address at runtime */
static uint64_t runtime_cast_addr = 0;

void module_ir_set_cast_addr(uint64_t addr) {
    runtime_cast_addr = addr;
}
```

```cpp
/* C++ side: resolves mangled symbol, passes address */
inline void ensureHelperInit() {
    static bool initialized = false;
    if (!initialized) {
        module_ir_set_cast_addr(
            reinterpret_cast<uint64_t>(Runtime_Cast));
        initialized = true;
    }
}
```

**Pitfalls:**
- `extern "C"` changes the calling convention on some platforms.
  Function signatures must match exactly.
- Address-passing adds a runtime init step -- must be called before
  the address is used.
- A common symptom is a linker error like
  `undefined reference to 'SomeFunction'` because the C declaration
  used unmangled linkage while the C++ definition was mangled.

**Interop:** `extern "C"` blocks are the primary interop mechanism.
Address-passing is a fallback for symbols that cannot be re-declared.

---

## Pattern 19: Static Initialisers with Extern Addresses -- Lazy Initialisation

**C++ feature:** C++ allows extern variable addresses in static
initialisers (evaluated at load time).

**C pattern:** `CPattern.LazyInitTable` -- Lazy initialisation on
first call.

**Risk:** `ConversionRisk.Moderate` -- Must ensure init is called
before any access. Thread safety may be needed.

**Before** (C++):
```cpp
static const std::unordered_map<std::string_view, uint64_t> mapping = {
    {"ExceptionType",   reinterpret_cast<uint64_t>(ExceptionType)},
    {"ErrorFormat",     reinterpret_cast<uint64_t>(ErrorFormat)},
    // ...
};
```

**After** (C):
```c
static SymbolEntry symbol_table[NUM_SYMBOLS];
static int table_initialized = 0;

static void
init_symbol_table(void) {
    int i = 0;
    symbol_table[i].name = "ErrorFormat";
    symbol_table[i].addr = (uint64_t)(uintptr_t)ErrorFormat;
    i++;
    symbol_table[i].name = "ExceptionType";
    symbol_table[i].addr = (uint64_t)(uintptr_t)ExceptionType;
    i++;
    // ...
    table_initialized = 1;
}

const uint64_t*
module_ir_function_from_name(const char *name) {
    if (!table_initialized) {
        init_symbol_table();
    }
    // ...
}
```

**Pitfalls:**
- An extern variable whose address is NOT a compile-time constant
  in C will cause a compiler error:
  `initializer element is not a compile-time constant`.
- Thread safety: the `table_initialized` flag is not atomic. If
  initialisation happens single-threaded, this is acceptable.
  For multi-threaded contexts, use `pthread_once` or an atomic flag.
- The same pattern applies to any static array containing extern
  addresses (function pointers are fine -- only variable addresses
  cause issues on some platforms).

**Interop:** No interop needed. The lazy-init table is internal to
the C implementation. The C++ wrapper in the header calls the C
function directly.
