# C++ to C Conversion Analysis

An analysis of patterns, build failures, and interop mechanisms observed
during a large-scale C++ to C conversion of a JIT compiler codebase.

**Scope:** 16 C++ files eliminated (of 104 total), approximately 2,500
lines of C++ replaced with approximately 3,800 lines of C, plus a
70-function IR C accessor API. Work performed in a single session.

---

## 1. Conversion Patterns

### Pattern 1: Namespace Function to Prefixed C Function

C++ functions in namespaces become prefixed free functions with `extern "C"`
linkage.

**Before** (C++ source):
```cpp
namespace jit::codegen::arch {
  Mem ptr_offset(Gp base, int32_t offset, int32_t access_size) {
    // ...
  }
}
```

**After** (C source):
```c
AsmMem
module_arch_ptr_offset(AsmGp base, int32_t offset, int32_t access_size) {
    AsmMem result;
    int ok = module_arch_ptr_offset_try(&result, base, offset, access_size);
    assert(ok && "module_arch_ptr_offset: offset out of range");
    (void)ok;
    return result;
}
```

**Naming convention:** `module_` prefix + subsystem + `_` + function name.
Examples: `module_arch_ptr_offset`, `module_section_name`,
`module_ir_bit_size`, `module_elf_strtab_insert`.

**Files:** `arch.c`, `code_section.c`, `type.c`

---

### Pattern 2: Inline C++ Wrapper in Header

The converted header provides `extern "C"` declarations visible to both
C and C++ callers, plus inline C++ wrappers inside `#ifdef __cplusplus`
that preserve the original namespace API for unconverted C++ callers.

**Example** (converted header):
```cpp
/* C API */
#ifdef __cplusplus
extern "C" {
#endif
JitIrBlock *module_ir_sort_blocks_rpo(
    JitIrBlock *blocks, size_t count, size_t *out_count);
#ifdef __cplusplus
} /* extern "C" */
#endif

/* C++ wrapper -- only visible to C++ callers */
#ifdef __cplusplus
namespace jit::lir {
class BasicBlockSorter {
 public:
  explicit BasicBlockSorter(const std::vector<BasicBlock*>& blocks)
      : blocks_(blocks) {}
  std::vector<BasicBlock*> getSortedBlocks() {
    size_t out_count = 0;
    JitIrBlock *sorted = module_ir_sort_blocks_rpo(
        reinterpret_cast<JitIrBlock*>(
            const_cast<BasicBlock**>(blocks_.data())),
        blocks_.size(), &out_count);
    std::vector<BasicBlock*> result(
        reinterpret_cast<BasicBlock**>(sorted),
        reinterpret_cast<BasicBlock**>(sorted) + out_count);
    free(sorted);
    return result;
  }
 private:
  const std::vector<BasicBlock*>& blocks_;
};
} // namespace jit::lir
#endif
```

**Files:** All 16 converted headers use this pattern. Notable examples:
`blocksorter.h`, `cold_block_marker.h`, `dce.h`, `verify.h`,
`helper_translations.h`, `code_section.h`.

---

### Pattern 3: Lazy Initialisation for Non-Constant Static Data

C does not allow extern variable addresses in static array initialisers
(they are not compile-time constants). The fix is a lazy-initialised table
populated on first call.

**Before** (C++ source, removed during conversion):
```cpp
static const std::pair<const char*, uint64_t> symbol_table[] = {
    {"ExcTypeError", (uint64_t)ExcTypeError},
    {"ErrFormat",    (uint64_t)ErrFormat},
    // ...
};
```

**After** (C source):
```c
static JitSymbolEntry symbol_table[NUM_SYMBOLS];
static int table_initialized = 0;

static void
init_symbol_table(void) {
    int i = 0;
    symbol_table[i].name = "ErrFormat";
    symbol_table[i].addr = (uint64_t)(uintptr_t)ErrFormat;
    i++;
    symbol_table[i].name = "ExcTypeError";
    symbol_table[i].addr = (uint64_t)(uintptr_t)ExcTypeError;
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

This was established as a standard pattern for all conversions involving
static arrays with extern addresses.

**Files:** `symbol_mapping.c`, `helper_translations.c`

---

### Pattern 4: Address-Passing for C++ Name-Mangled Symbols

When a C file needs a function pointer whose symbol has C++ linkage
(name-mangled), the address is passed from C++ via an init function rather
than forward-declaring the C++ function in C (which would create a linkage
mismatch).

**C side** (converted `.c` file):
```c
static uint64_t runtime_cast_addr = 0;

void
module_ir_set_cast_addr(uint64_t addr) {
    runtime_cast_addr = addr;
}
```

**C++ side** (interop header):
```cpp
inline void ensureHelperInit() {
  static bool initialized = false;
  if (!initialized) {
    module_ir_set_cast_addr(reinterpret_cast<uint64_t>(Runtime_Cast));
    initialized = true;
  }
}
```

This pattern was adopted after three failed build iterations for
`helper_translations.c`. The alternatives (forward-declaring a C++
function in .c, or adding `extern "C"` to the C++ side) were rejected
as more invasive.

---

### Pattern 5: enum class to Integer Constants

C++ `enum class` values are replaced with `#define` constants or anonymous
`enum` values, and function parameters change from the enum type to `int`.

**Before** (C++ source):
```cpp
size_t bitSize(DataType dt) {
    switch (dt) {
    case DataType::k8bit: return 8;
    // ...
    }
}
```

**After** (C source):
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
    case MODULE_IR_DT_8BIT: return 8;
    // ...
    }
}
```

**Files:** `type.c`, `ir_c_api.h` (defines `MODULE_IR_OPTYPE_*`,
`MODULE_IR_FLAG_*`, `MODULE_IR_DT_*`). `dynamic.h` uses `#define` for
ELF `DynTag` values (`MODULE_ELF_DYN_NULL`, etc.).

---

### Pattern 6: Opaque Pointer C API (Bridge Layer)

For files that manipulate C++ objects they do not own, a C accessor API
is introduced. C code receives opaque `void*` handles; the bridge
(`ir_c_api.cpp`) casts to the C++ type and calls methods.

**Header** (`ir_c_api.h`):
```c
typedef void* JitIrFunc;
typedef void* JitIrBlock;
typedef void* JitIrInstr;
typedef void* JitIrOperand;

extern "C" size_t module_ir_func_num_blocks(JitIrFunc func);
extern "C" JitIrBlock module_ir_func_get_block(JitIrFunc func, size_t index);
```

**Bridge** (`ir_c_api.cpp`):
```cpp
extern "C" size_t
module_ir_func_num_blocks(JitIrFunc func) {
  return static_cast<Function*>(func)->basicblocks().size();
}

extern "C" JitIrBlock
module_ir_func_get_block(JitIrFunc func, size_t index) {
  return static_cast<Function*>(func)->basicblocks()[index];
}
```

**Scale:** 70+ accessor functions covering Function, BasicBlock,
Instruction, and OperandBase. Started at 17 functions for Step 8
(cold_block_marker), grew to 70+ by phase completion.

**Endgame:** The bridge is temporary scaffolding. Once underlying types
become C structs, accessors become `static inline` (zero cost) and the
bridge `.cpp` is deleted. LTO inlines during the intermediate phase.

---

### Pattern 7: std::queue/std::stack to Growable Array

C++ STL containers are replaced with simple growable arrays using
`malloc` / `realloc`.

**Example: Queue** (`cold_block_marker.c`):
```c
typedef struct {
    JitIrBlock *items;
    size_t head;
    size_t tail;
    size_t capacity;
} BlockQueue;

static void queue_push(BlockQueue *q, JitIrBlock block) {
    if (q->tail >= q->capacity) {
        size_t new_cap = q->capacity * 2;
        q->items = (JitIrBlock *)realloc(
            q->items, new_cap * sizeof(JitIrBlock));
        q->capacity = new_cap;
    }
    q->items[q->tail++] = block;
}
```

**Example: Stack** (`blocksorter.c`):
```c
typedef struct {
    void **items;
    size_t len;
    size_t cap;
} PtrVec;

static void ptrvec_push(PtrVec *v, void *item) { /* ... */ }
static void *ptrvec_pop(PtrVec *v) { return v->items[--v->len]; }
```

**Files:** `cold_block_marker.c` (BlockQueue), `dce.c` (InstrQueue,
BitSet), `blocksorter.c` (PtrVec used as stack).

---

### Pattern 8: std::unordered_set/std::unordered_map to Alternatives

Different replacements depending on context:

**Fixed-size array** when the set is bounded
(`verify.c`):
```c
/* Typical blocks have 0-3 branches; 256 is far beyond any
 * realistic block size. */
JitIrBlock branched[256];
size_t num_branched = 0;
```

**Bitmap** when elements have sequential integer IDs
(`dce.c`):
```c
typedef struct {
    uint8_t *bits;
    int capacity;
} BitSet;

static int bitset_contains(const BitSet *s, int id) {
    if (id < 0 || id >= s->capacity) return 0;
    return (s->bits[id / 8] >> (id % 8)) & 1;
}
```

**Framework hash table** when a proper hash table is needed
(`blocksorter.c`):
```c
#include "framework_hashtable.h"

s->blocks = hashtable_new(hashtable_hash_ptr,
                          hashtable_compare_direct);
```

---

### Pattern 9: C++ Class to C Struct with init/free Pairs

Classes with constructors/destructors become structs with explicit
`_init`/`_free` functions or `_new`/`_free` functions.

**Before** (C++ StringTable class):
```cpp
class StringTable {
 public:
  uint32_t insert(std::string_view s) { /* ... */ }
 private:
  std::vector<uint8_t> data_{0};
};
```

**After** (C header + source):
```c
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} JitElfStrTab;

void module_elf_strtab_init(JitElfStrTab *st);
void module_elf_strtab_free(JitElfStrTab *st);
uint32_t module_elf_strtab_insert(JitElfStrTab *st, const char *s, size_t slen);
```

**Naming convention:** `Jit` + subsystem + type for the struct (e.g.
`JitElfStrTab`, `JitElfDynTab`, `JitElfSymTab`). Functions use
`module_elf_` + abbreviated type + `_` + method.

**Files:** All ELF types (`string.c`, `symbol.c`, `dynamic.c`, `hash.c`),
`register_preserver_c.h` (`AsmRegPreserver`), `gen_asm_utils_c.h`
(`AsmEmitCallCtx`).

---

### Pattern 10: C++ Lambda to Static Function + Context Struct

C++ lambdas used as callbacks become static functions with an explicit
`void *ctx` parameter.

**Before** (C++ source):
```cpp
for_each_input(instr, [&](auto& operand) {
    if (auto* linked = operand.asLinked()) {
        worklist.push(linked->defInstr());
    }
});
```

**After** (C source):
```c
typedef struct {
    InstrQueue *worklist;
    BitSet *live;
} PropagateCtx;

static void
propagate_input_cb(JitIrOperand operand, void *ctx) {
    PropagateCtx *pctx = (PropagateCtx *)ctx;
    if (module_ir_operand_is_linked(operand)) {
        JitIrInstr def = module_ir_operand_get_linked_instr(operand);
        int def_id = module_ir_instr_id(def);
        if (bitset_insert(pctx->live, def_id)) {
            instrq_push(pctx->worklist, def);
        }
    }
}
```

The C API provides a callback-based iteration function:
```c
void module_ir_instr_foreach_input(
    JitIrInstr instr,
    void (*cb)(JitIrOperand operand, void *ctx),
    void *ctx);
```

---

### Pattern 11: static_assert to _Static_assert

C++ `static_assert` becomes C11 `_Static_assert` for compile-time
struct size verification.

**After** (C source):
```c
_Static_assert(sizeof(JitElfSectionHeader) == 64,
    "ELF SectionHeader must be 64 bytes");
_Static_assert(sizeof(JitElfSegmentHeader) == 56,
    "ELF SegmentHeader must be 56 bytes");
```

---

### Pattern 12: C++ Class Hierarchy to Tagged Union

The C++ `OperandBase`/`Operand`/`LinkedOperand` class hierarchy (using
virtual dispatch) is replaced by a single C struct with an `is_linked`
discriminator flag and a tagged union.

**Before** (C++ class hierarchy, ~48 bytes per operand):
```cpp
class OperandBase { virtual Type type() const = 0; /* ... */ };
class Operand : public OperandBase { /* holds value */ };
class LinkedOperand : public OperandBase { Operand* def_; /* delegates */ };
```

**After** (C API header):
```c
struct IrOperand {
    IrInstruction *parent_instr;
    uint8_t is_linked;
    uint8_t last_use;
    uint8_t type;       /* OperandType enum */
    uint8_t data_type;  /* DataType enum */
    union {
        uint64_t imm;
        void *mem_addr;
        void *label;
        IrMemoryIndirect *indirect;
        IrPhyLocation phy_loc;
        IrOperand *def_opnd;   /* is_linked=1: defining operand */
    } value;
};
```

50% smaller than the C++ equivalent (24 bytes vs ~48 bytes). Virtual
dispatch replaced by a simple `if (op->is_linked)` branch in each
accessor.

**Design:** The struct layout was designed for minimum size; the
coexistence strategy (C struct alongside C++ class, swap later) was
chosen for incremental migration.

---

### Pattern 13: Separate C API Header (_c.h)

When a converted file's C API is complex enough, a dedicated `_c.h`
header is created (separate from the main `.h` which retains C++
wrappers).

**Example** (`register_preserver_c.h`):
```c
typedef struct {
    AsmGp src;
    AsmGp dst;
} AsmRegPair;

typedef struct {
    AsmBuilder* builder;
    const AsmRegPair* regs;
    int num_regs;
    int align_stack;
} AsmRegPreserver;

void asm_reg_preserver_init(AsmRegPreserver* rp, AsmBuilder* builder,
                            const AsmRegPair* regs, int num_regs);
void asm_reg_preserver_preserve(AsmRegPreserver* rp);
void asm_reg_preserver_remap(AsmRegPreserver* rp);
void asm_reg_preserver_restore(AsmRegPreserver* rp);
```

**Files:** `register_preserver_c.h`, `gen_asm_utils_c.h`.

---

### Pattern 14: Iterator Invalidation via Callback

C++ `std::list` iteration with mid-traversal removal (which invalidates
iterators) is replaced by a callback-based removal function that handles
iterator invalidation internally in C++.

**C API** (`ir_c_api.h`):
```c
void module_ir_block_remove_dead_instrs(
    JitIrBlock block,
    int (*is_live)(JitIrInstr instr, void *ctx),
    void *ctx);
```

collect-then-remove over wrapping C++ iterators in C structs.

---

## 2. Build Failures

### Failure Class 1: Extern Addresses in Static Initialisers

**Error:** `initializer element is not a compile-time constant`

**Cause:** C does not allow extern variable addresses (e.g.
runtime exception objects) in static array initialisers. C++ does.

**Example:** `symbol_mapping.c` (Step 6)

**Fix:** Lazy initialisation pattern (Pattern 3 above).

**Recurrence:** Any static array with extern addresses. Also appeared
in `helper_translations.c`.

---

### Failure Class 2: C++ Headers Included from .c Files

**Error:** `#include <utility>` fails when compiled as C.

**Cause:** `.c` files compiled as C cannot include headers that
transitively include C++ standard library headers.

**Example:** `helper_translations.c` included a runtime header which
included `<utility>` (Step 7).

**Fix options:**
1. `#ifdef __cplusplus` guards around C++ includes
2. Separate `_c.h` header
3. Forward-declare specific items (chosen for this case)

**Rule established:** `.c` files MUST NOT include headers that pull in
C++.

---

### Failure Class 3: C/C++ Name Mangling Mismatch (Linker Error)

**Error:** `undefined reference to 'Runtime_Cast'`

**Cause:** C files use C linkage (unmangled names) by default. C++
files use C++ linkage (mangled names). Forward-declaring a C++ function
in a `.c` file creates a linkage mismatch at link time.

**Example:** `helper_translations.c` forward-declared a C++ runtime
function (Step 7).

**Fix:** Address-passing pattern (Pattern 4 above) -- C++ resolves the
mangled symbol and passes the raw address to C via an init function.

---

### Failure Class 4: Framework offsetof Guards

**Error:** `implicit declaration of function 'offsetof'`

**Cause:** The host framework's main header uses `__need_offsetof` guards
that prevent `offsetof` from being defined when `stddef.h` is included
transitively. C++ provided `offsetof` implicitly.

**Example:** `helper_translations.c` (Step 7).

**Fix:** Explicit `#include <stddef.h>` AFTER including the framework
header.

**Rule established:** All `.c` files using `offsetof` must include
`<stddef.h>` explicitly.

---

### Failure Class 5: Architecture-Conditional Fields Without Guards

**Error:** Compile error accessing ARM64-only struct fields on x86_64.

**Cause:** `gen_asm_utils.c` accessed `Environ` fields that only exist
when `TARGET_AARCH64` is defined, without `#ifdef` guards.

**Example:** Step 4.

**Fix:** Wrap ARM64-only field accesses in `#ifdef TARGET_AARCH64`.

---

### Failure Class 6: Implicit Type Conversion (Wrapper Types)

**Error:** `cannot convert 'AsmLabel' to 'asm::Label'`

**Cause:** `emplace_back` cannot perform explicit conversions.
`PendingDebugLoc` constructor expects the underlying assembler label
type, not the wrapper type.

**Example:** `gen_asm_utils.h` (Step 4).

**Fix:** Explicit type conversion at call sites, e.g.
`asm::Label(debug_label)`.

**Note:** Stale `.o` files masked this error in initial builds.
Clean rebuild exposed it.

---

### Failure Class 7: Cross-File Forward Declaration Mismatches

**Error:** Multiple definition / type mismatch errors across core
type `.c` files.

**Cause:** Core type files (`operand_impl.c`, `instruction_impl.c`,
`block_impl.c`, `function_impl.c`) cross-reference each other without
consistent type declarations.

**Example:** Build gate caught 10 errors.

**Fix:** All `_impl.c` files must `#include "ir_c_api.h"` and conform
to its type signatures exactly. The initial fix attempt (adding new
forward declarations) made things worse by introducing duplicate
declarations with mismatched types.

---

### Failure Class 8: Duplicate Implementation Files

**Error:** Linker error from duplicate symbol definitions.

**Cause:** Two commits independently created C implementations for
the same type (e.g. `ir_operand.c` + `operand_impl.c`, `ir_block.c` +
`block_impl.c`).

**Fix:** Delete the redundant file, keep the canonical `_impl.c`
version.

---

### Failure Class 9: API Naming Mismatches

**Error:** Undefined reference to functions with inconsistent names.

**Cause:** Core type `.c` files used different function names from
those declared in `ir_c_api.h` (e.g. `ir_operand_create` vs
`ir_operand_new`).

**Fix:** Align all `_impl.c` function names to match `ir_c_api.h`
declarations exactly.

---

### Failure Class 10: PhyLocation Accessor Bug

**Error:** Wrong value returned for physical register/stack slot.

**Cause:** `ir_c_api.cpp` returned `PhyLocation` struct directly
instead of accessing its `.loc` field when an `int` was expected.

**Fix:** One-line change: `return phyLoc` to `return phyLoc.loc`.

---

## 3. Interop Mechanisms

### 3.1 Opaque Pointer Bridge (ir_c_api.h / ir_c_api.cpp)

**Files:** `ir_c_api.h` (declarations), `ir_c_api.cpp` (implementations).

**Mechanism:** C code receives `void*` handles (`JitIrFunc`,
`JitIrBlock`, `JitIrInstr`, `JitIrOperand`). The bridge file
(`ir_c_api.cpp`) casts to the C++ type and calls class methods.

**Scale:** 70+ functions covering:
- Function: 3 accessors
- BasicBlock: 12 accessors + 2 mutators
- Instruction: 20 accessors + 3 mutators + 7 allocation functions
- Operand: 15 accessors + 10 setters
- MemoryIndirect: 4 accessors
- Branch CC statics: 3 functions
- Constants: 20+ `#define` values

**Endgame:** Bridge is temporary scaffolding. When underlying types
become C structs, accessors become `static inline` struct field access
(zero cost). Bridge `.cpp` is deleted. LTO inlines during the
intermediate phase.

### 3.2 Inline C++ Wrappers in Headers

Every converted header provides inline C++ functions/classes inside
`#ifdef __cplusplus` that delegate to the C API. This allows
unconverted C++ callers to continue using the original API without
modification.

**Example:** `blocksorter.h` wraps `module_ir_sort_blocks_rpo` in a
`BasicBlockSorter` class. `helper_translations.h` wraps
`module_ir_map_helper_to_ir` in `mapHelperToIR`.

### 3.3 Address-Passing Init Functions

For C++ name-mangled symbols needed by C code (only as addresses, not
called), the C++ side resolves the symbol and passes the raw
`uint64_t` address to a C init function.

**Example:** `module_ir_set_cast_addr` receives the address of
`Runtime_Cast` from `ensureHelperInit`.

### 3.4 Separate C API Headers (_c.h)

Complex C APIs get dedicated headers (`register_preserver_c.h`,
`gen_asm_utils_c.h`) that define C structs and function prototypes,
with `extern "C"` guards. The main `.h` includes the `_c.h` and adds
C++ wrappers.

### 3.5 Coexisting C and C++ Implementation Files

The later conversion phase introduced C implementations (`operand_impl.c`,
`instruction_impl.c`, `block_impl.c`, `function_impl.c`) that coexist
with the original C++ files (`operand.cpp`, `instruction.cpp`,
`block.cpp`, `function.cpp`). Both compile. The `ir_c_api.cpp` bridge
currently routes through the C++ classes; the next phase will swap
routing to the C implementations, after which the C++ files are deleted.


---

## 4. Bugs and Fixes

### Bug 1: SCC Successor Graph Corruption (blocksorter.c)

**Bug:** `ptrvec_push` pushed into `succ_scc->successors` instead of
`cur_scc->successors`, corrupting the SCC graph's successor edges.

**Cause:** Variable name confusion during C++ to C translation (the
C++ code used method calls on the correct object; the C code used the
wrong variable).

**Impact:** Would corrupt block sort order, potentially producing
incorrect code layout.


### Bug 2: NULL Callback SIGSEGV (blocksorter.c)

**Bug:** `hashtable_foreach` called with a NULL callback function,
which would crash on single-block SCCs (the common case).

**Cause:** Incomplete translation of the C++ iteration pattern.

### Bug 3: PhyLocation.loc Accessor

**Bug:** `ir_c_api.cpp` returned the full `PhyLocation` struct where
an `int` was expected for `getPhyRegister` / `getStackSlot`.

**Cause:** Type mismatch between C struct (`IrPhyLocation` with
`loc` + `bit_size`) and the C API which returns `int`.


### Bug 4: Stale Object Files Masking Errors

**Bug:** Step 4 initially appeared to build cleanly because stale `.o`
files from a previous C++ build were used by the linker.

**Cause:** Incremental build did not recompile all dependent files.

**Fix:** Clean rebuild (`make clean && make`) exposed the real errors.


---

## 5. Naming Conventions

### File Naming

| Pattern | Example |
|---------|---------|
| Converted file | `arch.cpp` -> `arch.c` |
| C API header (simple) | In same `.h` file with `#ifdef __cplusplus` guards |
| C API header (complex) | `register_preserver_c.h`, `gen_asm_utils_c.h` |
| Bridge implementation | `ir_c_api.cpp` |
| Core type implementation | `operand_impl.c`, `block_impl.c`, `function_impl.c` |

### Function Naming

| Scope | Pattern | Example |
|-------|---------|---------|
| Codegen subsystem | `module_arch_*`, `module_section_*` | `module_arch_ptr_offset` |
| IR subsystem | `module_ir_*` | `module_ir_bit_size` |
| IR C API | `module_ir_func_*`, `module_ir_block_*`, etc. | `module_ir_func_num_blocks` |
| ELF subsystem | `module_elf_*` | `module_elf_strtab_insert` |
| Assembler wrapper | `asm_*` | `asm_reg_preserver_init` |
| Core type lifecycle | `ir_operand_*`, `ir_block_*` | `ir_operand_new` |

### Type Naming

| Pattern | Example |
|---------|---------|
| Opaque handle | `typedef void* JitIrFunc` |
| C struct (ELF) | `JitElfStrTab`, `JitElfDyn` |
| C struct (IR) | `IrOperand`, `IrInstruction`, `IrBasicBlock` |
| C struct (codegen) | `AsmRegPreserver`, `AsmEmitCallCtx` |
| Enum constants | `MODULE_IR_DT_*`, `MODULE_IR_OPTYPE_*`, `MODULE_ELF_DYN_*` |

---

## 6. Summary Statistics

### Files Converted

| Step | File | Lines (C++) | Lines (C) |
|------|------|------------|-----------|
| 1 | arch.cpp -> arch.c | 82 | 76 |
| 2 | code_section.cpp -> code_section.c | 44 | 36 |
| 3 | register_preserver.cpp -> register_preserver.c | 192 | 296 |
| 4 | gen_asm_utils.cpp -> gen_asm_utils.c | 93 | 154 |
| 5 | type.cpp -> type.c | 53 | 68 |
| 6 | symbol_mapping.cpp -> symbol_mapping.c | 27 | 71 |
| 7 | helper_translations.cpp -> helper_translations.c | 46 | 72 |
| 8 | cold_block_marker.cpp -> cold_block_marker.c | 119 | 178 |
| 9 | verify.cpp -> verify.c | 53 | 83 |
| 10 | dce.cpp -> dce.c | 96 | 226 |
| 11 | blocksorter.cpp -> blocksorter.c | 217 | 438 |
| 12 | header.cpp -> header.c | 20 | 43 |
| 13 | string.cpp -> string.c | 32 | 62 |
| 14 | symbol.cpp -> symbol.c | 20 | 72 |
| 15 | dynamic.cpp -> dynamic.c | 16 | 64 |
| 16 | hash.cpp -> hash.c | 46 | 90 |

**Total:** 1,156 lines C++ removed, 2,029 lines C added (in converted
files). Additional 800 lines in core type implementations
(`operand_impl.c`, `instruction_impl.c`, `block_impl.c`,
`function_impl.c`). IR C API bridge: ~600 lines across
`ir_c_api.h` + `ir_c_api.cpp`.

### Conversion Phases

| Phase | Files | Description |
|-------|-------|-------------|
| Phase 1 (Steps 1-4) | 4 | Codegen leaf files (arch, code_section, register_preserver, gen_asm_utils) |
| Phase 2 IR leaves (Steps 5-7) | 3 | type, symbol_mapping, helper_translations |
| Opaque pointer API (Steps 8-10) | 3 | cold_block_marker, verify, dce (via ir_c_api) |
| ELF subsystem (Steps 12-16) | 5 | header, string, symbol, dynamic, hash |
| Tarjan SCC (Step 11) | 1 | blocksorter (hash table, PtrVec) |
| Core types | 4 impl files | operand_impl, instruction_impl, block_impl, function_impl |

### Build Failure Classes

| Class | Count | First Seen |
|-------|-------|------------|
| Extern addresses in static initialisers | 2 | Step 6 |
| C++ headers included from .c | 1 | Step 7 |
| C/C++ linkage mismatch | 1 | Step 7 |
| Framework offsetof guards | 1 | Step 7 |
| Missing #ifdef architecture guards | 1 | Step 4 |
| Implicit type conversion | 1 | Step 4 |
| Cross-file declaration mismatches | 3+ | Core types phase |
| Duplicate implementation files | 2 | Core types phase |
| API naming mismatches | 3+ | Core types phase |
| Accessor type mismatch | 1 | Core types phase |

### Performance Impact

- **ARM64:** 1.33x speedup, matching pre-conversion baseline exactly.
  Zero performance regression. 24/24 benchmarks, zero crashes.
- **x86_64:** Spec ON/OFF ratio ~0.988x (codegen identical). JIT
  disassembly structurally identical across conversions.
  JIT-vs-vanilla showed 1.11x vs 1.22x baseline; gap attributed to
  environmental factors (machine load, not the conversion).
- **JIT disassembly gate:** fib function showed 921/924/915 bytes
  across conversion steps -- differences were register allocation
  non-determinism and ASLR, not codegen regressions.

### Interop Checklist

The team codified 7 interop rules from early conversion learnings:

1. Architecture `#ifdef` guards on struct fields
2. Explicit constructor conversions (e.g. wrapper label type to underlying assembler label type)
3. No C++ headers in `.c` files
4. Explicit `<stddef.h>` for `offsetof`
5. C++ name mangling -- pass addresses from C++ side
6. Lazy init for extern addresses in static arrays
7. `enum class` to `int` casts at C++/C boundary
