# Engineering Standards: A Verification-First Approach

> This document is the canonical engineering standards reference for the NBS framework.

## Philosophy and Principles

This document defines engineering standards built on a fundamental insight: **safety comes from verbs, not nouns**. Correctness emerges from actions — checking, validating, asserting, testing, monitoring — not from static structures like type systems or design patterns. The act of verification matters more than the classification system.

### Core Tenets

- **Falsifiability as Foundation**: You cannot prove code correct, but you CAN prove it wrong. A single counterexample demolishes a claim. Tests should try to break code, not confirm it works. "All tests pass" provides weak confidence; "I tried hard to break it and failed" provides strong confidence.
- **Verbs Over Nouns**: Correctness comes from actions, not classifications. "This value was validated" matters more than "this has type ValidatedInput". The verb happened or it didn't. The assertion passed or it failed. That's provable.
- **Types Are Hints, Not Guarantees**: Type systems are incomplete — they cannot express "this list is sorted" or "this connection is authenticated". Types constrain design toward what the checker can verify, not what the problem demands. Use types as documentation, but rely on assertions and invariants for correctness.
- **Test the Real System**: Mocks hide integration failures. Real systems have real network latency, real concurrency, real resource contention, real failure modes. Integration-first testing, with targeted unit tests only for complex isolated logic.
- **The Cycle of Verified Construction**: Design, Plan, Deconstruct into testable steps. For each step: write tests FIRST, then write code to make tests pass, then document learnings before moving to next step. This is not optional ceremony — it is the engine of quality.

---

_Prove you understand the problem by defining how you would falsify the solution, then build the solution, then record what you learned._

---

## The Assertion Protocol

Assertions are not optional debugging aids to be disabled in production. They are executable specifications — documentation that verifies itself. A triggered assertion is proof of a bug, not merely a hint.

### The Three-Level Hierarchy

All three levels are required.

- **Level 1: Preconditions (Entry Guards)**
  - Verify assumptions about inputs before processing
  - Fail fast with clear messages when violated
  - Every public function must validate its inputs
  - Document what the function requires to operate correctly
- **Level 2: Postconditions (Exit Guarantees)**
  - Verify promises about outputs before returning
  - Capture relationships between inputs and outputs
  - Detect corruption that occurred during processing
- **Level 3: Invariants (Always-True Properties)**
  - Properties that must hold at all times
  - Checked at key state transitions
  - Represent fundamental system correctness constraints

### Example: All Three Levels

```python
def transfer_funds(from_account, to_account, amount):
    # PRECONDITIONS - what must be true on entry
    assert from_account != to_account, "Cannot transfer to same account"
    assert amount > 0, f"Amount must be positive, got {amount}"
    assert from_account.balance >= amount, \
        f"Insufficient funds: {from_account.balance} < {amount}"

    # Capture state for postcondition
    old_total = from_account.balance + to_account.balance

    # Perform the operation
    from_account.balance -= amount
    to_account.balance += amount

    # POSTCONDITION - what must be true on exit
    assert from_account.balance + to_account.balance == old_total, \
        "Invariant violated: money created or destroyed"

    # INVARIANT - always true for accounts
    assert from_account.balance >= 0, "Account balance went negative"
    assert to_account.balance >= 0, "Account balance went negative"
```

### Assertion Messages

Every assertion message MUST answer three questions:

- **What**: was expected?
- **What**: actually occurred?
- **Why**: does this matter?

```python
# BAD: States the obvious
assert x > 0, "x must be greater than 0"

# GOOD: Provides context and values
assert x > 0, f"Request count must be positive for rate limiting, got {x}"

# BEST: Actionable guidance
assert x > 0, \
    f"Request count must be positive for rate limiting, got {x}. "
    f"Check if the counter was reset incorrectly or input validation failed."
```

Adapt syntax to the project's language — the principle is universal.

## Testing for Falsification

Traditional testing asks "does it work for these examples?" Falsification testing asks "can I find any input that breaks it?" The difference is profound: one confirms, the other challenges.

### Property-Based Testing

Instead of testing specific examples, define properties that must
always hold, then generate many inputs to search for counterexamples:

```python
# Example-based: proves almost nothing
def test_sort_example():
    assert sort([3, 1, 2]) == [1, 2, 3]

# Property-based: actively seeks counterexamples
def test_sort_properties():
    for _ in range(1000):
        data = generate_random_list()
        result = sort(data)

        # Property 1: Output is sorted
        assert all(result[i] <= result[i+1]
                   for i in range(len(result)-1))

        # Property 2: Output is permutation of input
        assert sorted(result) == sorted(data)

        # Property 3: Idempotent
        assert sort(result) == result
```

### Adversarial Input Generation

For any function, systematically generate inputs designed to break it:

- **`Empty inputs`**: empty strings, empty lists, zero, None
- **`Boundary values`**: MAX_INT, MIN_INT, epsilon around boundaries
- **`Type confusion`**: strings where numbers expected, nested structures
- **`Resource exhaustion`**: very large inputs, deeply nested structures
- **`Malformed data`**: invalid UTF-8, truncated data, corruption
- **`Timing attacks`**: race conditions, reordering, delays

### Integration-First Methodology

- **AVOID**
  - Unit test with mocks → Unit test with mocks → Integration test (maybe)
- **PREFER**
  - Integration test (real system) → Targeted unit tests for complex logic

The real system reveals what mocks hide:

- Network latency and timeouts
- Concurrency and race conditions
- Resource contention and deadlocks
- Configuration mismatches
- Real failure modes and error messages

### Decomposition Criterion: Testability

**If you cannot write a test for a step, you haven't decomposed it far enough, or you don't understand it yet.**

- **BAD decomposition** — "Implement authentication" (How do you test this?)
- **GOOD decomposition**:
  - Validate password meets complexity rules
  - Hash password with salt
  - Compare hash against stored hash
  - Generate session token on success
  - Return appropriate error on failure

Each step is independently testable. Each test defines what success means.

## The Development Cycle in Practice

### The Cycle of Verified Construction

1. **DESIGN**
  - Understand the problem before proposing solutions
  - Identify constraints, dependencies, and risks
  - Define what success looks like
2. **PLAN**
  - Structure the approach before writing code
  - Identify the sequence of changes
  - Anticipate integration points and failure modes
3. **DECONSTRUCT into testable steps**
  - Break work into independently verifiable units
  - Each step must have a clear test criterion
  - If you can't test it, decompose further
4. **For each step: TEST FIRST**
  - Write the test before the implementation
  - The test defines what would falsify success
  - Include adversarial cases, not just happy paths
5. **For each step: WRITE CODE**
  - Implement to make tests pass
  - Include assertions for preconditions and postconditions
  - Write clear, maintainable code
6. **For each step: DOCUMENT LEARNINGS**
  - What did you learn? (Often different from expected)
  - What assumptions were validated or invalidated?
  - What edge cases emerged?
  - Do this BEFORE moving to next step
7. **NEXT STEP**
  - Repeat until complete
  - Each step builds on verified foundation

### Phase Entry and Exit Criteria

| Phase     | Entry Criterion          | Exit Criterion               |
|-----------|--------------------------|------------------------------|
| Design    | Problem statement exists | Success criteria defined     |
| Plan      | Success criteria clear   | Testable steps identified    |
| Decompose | Steps identified         | Each step has test criterion |
| Test      | Test criterion defined   | Test code written and fails  |
| Code      | Test fails correctly     | Test passes, assertions hold |
| Document  | Test passes              | Learnings recorded           |

## Runtime Verification

Verification extends beyond tests into production. The same assertions that catch bugs in development can detect corruption in production — if you let them.

### Health Checks and Watchdogs

- **Self-checks**: Periodic verification of internal invariants
- **Dependency checks**: Verify external services remain available
- **Data integrity checks**: Validate critical data hasn't corrupted
- **Watchdog timers**: Detect hung processes or infinite loops

### Graceful Degradation

When a runtime assertion fails in production:

1. **Log**: Capture full context for debugging
2. **Alert**: Notify operators immediately
3. **Contain**: Prevent corruption from spreading
4. **Degrade**: Fall back to safe mode if possible
5. **Recover**: Attempt automatic recovery or await intervention

**Never silently continue after an invariant violation. The data is no longer trustworthy.**

## AI-Accelerated Development

AI processing power transforms verification from impractical ceremony into routine practice. Use AI to handle the verification burden that makes rigorous approaches infeasible for most software.

### AI Capabilities by Phase

| Phase     | AI Contribution                                                |
|-----------|----------------------------------------------------------------|
| Design    | Explore patterns, identify constraints, analyse existing code  |
| Plan      | Verify completeness, identify missing steps, find dependencies |
| Decompose | Suggest testable units, identify implicit assumptions          |
| Test      | Generate adversarial cases, property-based tests, edge cases   |
| Code      | Implement with assertions, verify against tests                |
| Document  | Summarise learnings, trace to requirements                     |

### Context Management Principles

- **Delegate exploration to agents**: Don't read large files directly
- **Parallel analysis**: Launch multiple agents for independent tasks
- **Structured summaries**: Agent analysis beats raw file dumps
- **Preserve context for implementation**: Don't waste tokens on exploration

### AI-Assisted Test Generation

AI can systematically generate tests that humans often miss:

- **Boundary conditions**: Values at edges of valid ranges
- **Null and empty cases**: What happens with nothing?
- **Type coercion edge cases**: String "0" vs integer 0
- **Unicode edge cases**: Emoji, RTL text, zero-width characters
- **Concurrency scenarios**: Race conditions, deadlocks
- **Resource exhaustion**: What breaks under load?

## Language-Specific Patterns

The philosophy is portable across all languages.
These patterns show concrete implementations.

### Python

```python
# Assertions - use liberally
assert precondition, f"Meaningful message with {context}"

# Property-based testing with Hypothesis
from hypothesis import given, strategies as st

@given(st.lists(st.integers()))
def test_sort_properties(data):
    result = sort(data)
    # Properties checked automatically for many inputs
    assert is_sorted(result)
    assert is_permutation(result, data)

# Type hints as documentation (not enforcement)
def process(data: list[int]) -> list[int]:
    """Types hint intent; assertions verify it."""
    assert isinstance(data, list), f"Expected list, got {type(data)}"
    return sorted(data)
```

### Shell/Bash

```bash
#!/bin/bash
# Strict mode - fail fast on errors
set -euo pipefail

# Assertions via guard functions
assert_file_exists() {
    local file="$1"
    [[ -f "$file" ]] || {
        echo "ASSERTION FAILED: File not found: $file" >&2
        exit 1
    }
}

assert_not_empty() {
    local var_name="$1"
    local var_value="$2"
    [[ -n "$var_value" ]] || {
        echo "ASSERTION FAILED: $var_name is empty" >&2
        exit 1
    }
}

# Use assertions
assert_not_empty "CONFIG_PATH" "$CONFIG_PATH"
assert_file_exists "$CONFIG_PATH"
```

### C/Systems Programming

```c
/* Assertions with context */
#define ASSERT_MSG(cond, msg, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "ASSERT FAILED %s:%d: " msg "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        abort(); \
    } \
} while(0)

/* Defensive programming */
void* safe_malloc(size_t size) {
    ASSERT_MSG(size > 0, "Allocation size must be positive: %zu", size);
    ASSERT_MSG(size < MAX_ALLOC, "Allocation too large: %zu", size);

    void* ptr = malloc(size);
    ASSERT_MSG(ptr != NULL, "malloc failed for size %zu", size);

    return ptr;
}

/* Postcondition verification */
int* create_sorted_array(int* input, size_t len) {
    int* result = /* ... sorting logic ... */;

    /* Postcondition: result is sorted */
    for (size_t i = 1; i < len; i++) {
        ASSERT_MSG(result[i-1] <= result[i],
                   "Sort postcondition violated at index %zu", i);
    }

    return result;
}
```

### C/CPython Extensions

C extensions for CPython operate under additional constraints beyond general C programming. Python's reference counting, the GIL, and the CPython API's calling conventions create a distinct set of quality gates. These are not optional extras — without them, C conversion is unsupervised gambling with memory safety.

#### ASan Requirement

Every C extension must compile and pass its full test suite with AddressSanitizer and UndefinedBehaviourSanitizer enabled:

```bash
# Compile with sanitisers
CFLAGS="-fsanitize=address -fsanitize=undefined" python setup.py build_ext --inplace
# or
CFLAGS="-fsanitize=address -fsanitize=undefined" pip install -e .
```

ASan catches heap buffer overflows, use-after-free, double-free, stack buffer overflows, and memory leaks at runtime. It is the C equivalent of Rust's borrow checker — without it, memory safety bugs are invisible. This check is non-negotiable.

#### Leak Analysis

All C extensions must pass leak analysis with zero leaks:

```bash
valgrind --leak-check=full --error-exitcode=1 python -m pytest tests/
```

Memory leaks in C extensions are silent, cumulative, and invisible to correctness tests. Any C extension that leaks memory under the test suite fails the evidence gate, regardless of performance or correctness test results.

#### Refcount Discipline

Every `PyObject*` must have documented ownership semantics (borrowed reference vs new/strong reference). `Py_INCREF`/`Py_DECREF` balance must be verified for every parameter, return value, and local variable holding a `PyObject*` — on every code path, including error paths.

Common failure patterns:
- Missing `Py_DECREF` on error paths (early returns after allocation)
- `Py_DECREF` on borrowed references (double-free)
- Returning a borrowed reference without `Py_INCREF` (use-after-free)

These bugs may not manifest until long after the buggy code runs.

#### Calling Convention Discipline

C extensions exist because Python is too slow. If the extension itself uses slow calling conventions, it has no reason to exist. The following patterns are required:

- **Use `METH_FASTCALL`** for all functions taking positional arguments
- **`PyArg_ParseTuple` must be absent** — it allocates tuple objects and parses format strings at runtime
- **`Py_BuildValue` must be absent** for single return values — use direct API calls instead
- **`PyBool_FromLong` must be absent** — use `Py_RETURN_TRUE`/`Py_RETURN_FALSE` or `Py_NewRef(Py_True)`/`Py_NewRef(Py_False)` instead

These are the patterns most represented in training data and tutorials. They are also the slow patterns. Default to `METH_FASTCALL` and direct object construction.

## Anti-Patterns and Failure Modes

### Anti-Pattern 1: Silent Failure

Catching exceptions and discarding them with no logging, no re-raise, and no signal to the caller. This is the canonical violation. Every `except: pass`, `catch (...) {}`, or equivalent is a finding.

**WRONG:**

```python
try:
    risky_operation()
except Exception:
    pass  # Silently swallow all errors
```

**RIGHT:**

```python
try:
    risky_operation()
except SpecificException as e:
    logger.error("Operation failed: %s. Context: %s", e, context)
    raise OperationError(f"Failed with {e}") from e
```

### Anti-Pattern 2: Unfalsifiable Claims

Docstrings or comments claiming properties (e.g. "Thread-safe", "Idempotent", "Always returns valid X") without any mechanism enforcing them. A claim without a falsifier is bullshit.

**WRONG:**

```python
def process(data):
    """Thread-safe data processor."""
    # No locks, no atomic operations, no assertions verifying thread safety
    shared_state.append(data)
```

**RIGHT:**

```python
def process(data):
    """Thread-safe data processor. Safety enforced by self._lock."""
    with self._lock:
        assert self._invariant_holds(), "State corrupted before processing"
        shared_state.append(data)
        assert self._invariant_holds(), "State corrupted after processing"
```

### Anti-Pattern 3: Unreachable Guards

Validation checks ordered so that earlier checks make later checks unreachable. Example: checking `isinstance(x, int)` before `isinstance(x, bool)` when `bool` is a subclass of `int` — the bool check can never be reached because every bool is also an int.

**WRONG:**

```python
def validate(x):
    if isinstance(x, int):
        return handle_int(x)
    elif isinstance(x, bool):  # UNREACHABLE: bool is a subclass of int
        return handle_bool(x)
```

**RIGHT:**

```python
def validate(x):
    if isinstance(x, bool):   # Check subclass first
        return handle_bool(x)
    elif isinstance(x, int):
        return handle_int(x)
```

### Anti-Pattern 4: Quick Fix Trap

Silently returning a default value or None for unexpected conditions instead of asserting. Comments like "TODO: handle properly" are a signal.

**WRONG:**

```python
# Skip this for now, fix later
if problematic_condition:
    return None  # TODO: handle properly
```

**RIGHT:**

```python
# Understand root cause, fix properly
assert not problematic_condition, \
    f"Unexpected state: {context}. Investigate why this occurs."
```

### Anti-Pattern 5: Type-System False Confidence

Code that relies on type annotations for safety without runtime assertions. "It type-checks, therefore it is correct" substitutes a noun (the type) for the verb (the check). Flag functions whose type signature implies guarantees (e.g. `-> ValidatedOutput`, `-> SafeResult`) without postcondition assertions verifying those guarantees.

**WRONG:**

```python
# "It type-checks, therefore it's correct"
def process(data: ValidatedInput) -> SafeOutput:
    # Types say it's valid, but is it really?
    return transform(data)
```

**RIGHT:**

```python
# Types hint, assertions verify
def process(data: ValidatedInput) -> SafeOutput:
    assert data.is_actually_validated(), \
        "ValidatedInput was not actually validated"
    result = transform(data)
    assert result.meets_safety_criteria(), \
        "Transform produced unsafe output"
    return result
```

### Anti-Pattern 6: Mock-Heavy Testing

Tests where every dependency is mocked prove only that the mock behaves as expected. Flag test files where three or more dependencies are mocked simultaneously — this is a sign that integration tests are missing.

Mocks are acceptable at true system boundaries (external APIs the project does not control) and at **conversion boundaries during porting** — when replacing code piece by piece, mocking the boundary between ported and unported code is the methodology, not a shortcut. The mock proves the ported piece is behaviourally equivalent to the original in isolation before fusing.

Outside these two cases, prefer integration tests against the real system.

**WRONG:**

```python
# Everything mocked - tests pass, integration fails
@mock.patch('database')
@mock.patch('network')
@mock.patch('filesystem')
def test_everything_mocked():
    # This test proves nothing about real behaviour
    pass
```

**RIGHT:**

```python
# Test real integration, mock only external dependencies
def test_with_real_database(test_db):
    # Uses real database, real queries
    result = service.process(test_db)
    assert result.saved_to_db()
```

### Anti-Pattern 7: No Runtime Verification

Long-running processes or services that lack health checks, invariant monitoring, or graceful degradation. Flag services or daemons that do not periodically verify internal state. Flag code that silently continues after an invariant violation rather than logging, alerting, and containing the corruption.

After an invariant violation, the data is no longer trustworthy — the system must:

1. **Log** full context for debugging
2. **Alert** operators immediately
3. **Contain** the corruption — prevent it from spreading
4. **Degrade** to a safe mode if possible
5. **Recover** or await intervention

**WRONG:**

```python
class DataService:
    def run_forever(self):
        while True:
            data = self.fetch()
            self.process(data)  # No invariant checks, no health monitoring
            time.sleep(60)
```

**RIGHT:**

```python
class DataService:
    def run_forever(self):
        while True:
            self._verify_internal_state()
            data = self.fetch()
            self.process(data)
            self._check_data_integrity()
            time.sleep(60)

    def _verify_internal_state(self):
        assert self._cache_consistent(), \
            f"Cache inconsistency detected: {self._cache_diagnosis()}"

    def _check_data_integrity(self):
        assert self._no_orphaned_records(), \
            f"Orphaned records found: {self._orphan_report()}"
```

### Anti-Pattern 8: Missing Dynamic Analysis

Code without dynamic analysis tooling is code whose runtime behaviour is unverified. Static checks (type systems, linters, compilation) are necessary but insufficient — they cannot catch use-after-free, data races, undefined behaviour, or input-dependent failures.

Flag projects that lack build targets or CI stages for dynamic analysis appropriate to their language:

- **C/C++**: AddressSanitizer (ASan), ThreadSanitizer (TSan), UndefinedBehaviourSanitizer (UBSan), Valgrind
- **Rust**: Miri (for unsafe code), sanitiser builds, `cargo test` under ASan/TSan
- **Python**: `pytest` with `-x` (fail fast), `python -X dev` mode, `faulthandler`, Hypothesis for property-based testing
- **Concurrent code in any language**: a thread/race analysis tool (TSan, Go race detector, Helgrind)
- **Code handling untrusted input**: fuzz testing (libFuzzer, AFL, cargo-fuzz, Atheris)

The general principle: every class of runtime bug that static analysis cannot catch must have a corresponding dynamic analysis verb. If the project has no mechanism to detect memory errors, data races, or undefined behaviour at runtime, those bugs are invisible — and invisible bugs are the most dangerous kind.

### Anti-Pattern 5 (Supplementary): Skipping the Cycle

This is a behavioural anti-pattern rather than a code pattern, but it is no less dangerous:

- **WRONG**: "I'll write tests after the code works"
- **WRONG**: "Documentation can wait until the end"
- **WRONG**: "It's a small change, no need to plan"
- **RIGHT**: Follow the cycle every time, adjust scope to match

## Summary and Quick Reference

### The Cycle (Memorise This)

**Design → Plan → Deconstruct → [Test → Code → Document] → Next**

### The Philosophy (Internalise This)

- Safety through verbs, not nouns.
- Falsifiability as the foundation.
- Types as hints, assertions as guarantees.
- Test the real system.
- Document before moving on.

### The Questions (Ask Every Time)

- **Before implementing**: What would falsify this?
- **Before committing**: Did I try to break it?
- **Before moving on**: What did I learn?

---

**_Prove you understand the problem by defining how you would falsify the solution, then build the solution, then record what you learned._**
