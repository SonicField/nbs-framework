# Just Enough Structure

*Dr Alex Turner and Claude Opus 4.6 — 13 May 2026*

## The Observation

An AI team built a cross-platform terminal emulator in C. A VT100 parser, screen buffer, SGR attribute engine, UTF-8 decoder, Tk renderer, and Python extension module — roughly 3,200 lines of source. The code is correct. It passes its tests. It handles edge cases in the VT state machine that trip up implementations twice its age.

The code is not written in C. It is written in Phoenics — a preprocessor that adds seven features to C11 and emits standard C. No runtime. No library. No ABI change. Seven features.

This piece examines what those features are, why they are the ones that matter, and whether they are enough.

## What Phoenics Adds

Seven constructs. All compile to C11. None requires a runtime.

| Feature | What it does | C output |
|---------|-------------|----------|
| `phc_descr` | Discriminated union (sum type) | Tagged union + constructors + safe accessors |
| `phc_match` | Exhaustive pattern matching | `switch` with exhaustiveness checked at preprocess time |
| `phc_enum` | Enhanced enum | Enum + to_string + from_string + count |
| `phc_flags` | Type-safe bitflags | Power-of-2 constants + has/set/clear helpers |
| `phc_defer` | Automatic cleanup | Goto-based cleanup in LIFO order |
| `phc_require/check/invariant` | Tiered assertions | Conditional abort with strippable levels |
| Multi-file types | Cross-TU type sharing | Type manifest + generated header |

These are not ambitious features. Discriminated unions have existed since Algol 68. Pattern matching since ML in 1973. Defer since Go in 2009. Tiered assertions since forever. Phoenics invents nothing. It transplants known solutions into C, generates correct C11, and gets out of the way.

## What the Code Looks Like

A terminal colour can be unset, an 8-bit palette index, or a 24-bit RGB triple. In C, you write a tagged union by hand — enum for the tag, union for the data, constructor functions, accessor functions, and a switch statement wherever you consume it. You will forget a case. The compiler will not tell you.

In Phoenics:

```c
phc_descr Color {
    Default {},
    Indexed { uint8_t index; },
    Rgb24 { uint8_t r; uint8_t g; uint8_t b; }
};
```

Five lines. The preprocessor generates the tag enum, the union, constructors (`Color_mk_Rgb24(r, g, b)`), and safe accessors that abort on tag mismatch.

Consuming it:

```c
phc_match(Color, c) {
    case Default: {
        snprintf(out, outsize, "%s", default_color);
    } break;
    case Indexed(index): {
        /* index is automatically bound from c.Indexed.index */
        if (index < 16) snprintf(out, outsize, "%s", ansi_colors[index]);
        /* ... */
    } break;
    case Rgb24(r, g, b): {
        snprintf(out, outsize, "#%02x%02x%02x", r, g, b);
    } break;
}
```

Leave out a case and the preprocessor rejects the file. Add a fourth variant to `Color` and every match site in the codebase breaks until updated. This is not a runtime check. It is a preprocess-time guarantee. The generated C compiles without warnings.

The VT parser state machine is the same pattern at larger scale:

```c
phc_descr VTState {
    Ground {},
    Escape {},
    EscapeIntermediate { char intermediate; },
    Csi { int params[16]; int param_count; int priv; },
    Osc { char buf[512]; int len; int esc_seen; },
    Dcs { char buf[512]; int len; }
};
```

Six states, each carrying different data. Every byte the parser processes passes through a `phc_match` on the current state. 500 lines of state transitions, all exhaustiveness-checked. No forgotten case. No stale default branch.

Resource management uses defer:

```c
self->term = terminal_new(rows, cols);
phc_defer { terminal_free(self->term); }

self->parser = vt_parser_new(self->term);
if (!self->parser) return NULL;  /* defer fires terminal_free */

phc_defer_cancel;  /* success — ownership transferred */
return (PyObject *)self;
```

And trust boundaries use tiered assertions:

```c
phc_require(rows > 0 && rows <= 500, "rows out of range");
phc_check(self->term != NULL, "terminal_new succeeded");
```

`phc_require` is never stripped — it guards external input. `phc_check` can be stripped in production. `phc_invariant` (not shown here) verifies structural properties and strips separately. Three levels because not all assertions have the same cost-benefit trade-off.

## The Connection

[Types Are A Human Thing](Types-Are-A-Human-Thing.md) argued that type systems serve human working memory. Humans cannot hold an entire codebase in their head, so types compress structural information into signatures that fit in short-term memory. AI does not have this constraint. It can read every caller, every constructor, every code path. The compression is not needed.

What AI needed instead — the evidence from that piece — was assertions, behavioural grep, and falsifiable invariants. Verbs, not nouns. Actions that verify, not descriptions that summarise.

Phoenics occupies the exact middle ground that piece left open.

`phc_descr` is a noun — it describes structure. `phc_match` is a verb — it enforces exhaustive handling. The noun exists to make the verb possible. You need the discriminated union to make the exhaustive match meaningful. You need the `phc_flags` type to make the `has/set/clear` helpers safe. The structure is not the point. The verification is the point. The structure is the minimum scaffolding the verification requires.

This is not a type system in the Rust or Haskell sense. There are no lifetime annotations, no borrow checker, no trait bounds, no higher-kinded types. The structural surface is small: discriminated unions, enhanced enums, bitflags. Everything else — defer, tiered assertions, exhaustive matching — is behavioural. It checks what the code *does*, not what the code *is*.

The argument from "The Argument For C" was: C makes no promises, which forces you to verify everything, which is the right epistemic posture when verification is cheap. Phoenics modifies this slightly: C makes no promises, but a thin layer of structure makes certain verification *automatic*. You do not need to remember to check all cases in a match — the preprocessor rejects the file if you forget. You do not need to remember to free on error paths — defer handles it. You do not need to choose between "assert everything" and "assert nothing" — three assertion levels let you strip by cost.

The verification is still the point. The structure just makes some of it free.

## Is It Enough?

The terminal emulator works. The VT parser handles the standard. The renderer drives Tk correctly. The Python extension manages reference counts and does not leak. Forty-one assertion sites guard seven trust boundaries. The code was written by AI agents in Phoenics, compiled to C11, and deployed.

But "it works" is not a falsifiable claim about sufficiency. The question is: what class of bugs can occur in Phoenics-plus-C that a richer type system would prevent?

**What Phoenics catches:**
- Forgotten variants in a match (exhaustiveness)
- Wrong field access on a discriminated union (safe accessors abort)
- Resource leaks on error paths (defer)
- Out-of-range inputs at trust boundaries (assertions)
- Stale flag manipulation (type-safe helpers)

**What Phoenics does not catch:**
- Use-after-free (no ownership tracking)
- Data races (no send/sync markers)
- Integer overflow (no ranged types)
- Null pointer dereference (no option type enforcement — though `phc_descr` can model `Option` manually)
- Incorrect logic within a match arm (exhaustiveness guarantees you handle every case, not that you handle it correctly)

These are the bugs that Rust's borrow checker catches statically. They are real bugs. They occur in production C code. The question is whether AI agents, with sanitisers (ASan, TSan, UBSan), fuzz testing, and the assertion discipline that Phoenics enforces — catch them reliably enough that a full ownership type system is unnecessary.

The evidence from the NBS team's work on CinderX — 374 commits of JIT compiler work in C — suggests yes, with caveats. The caveats: you need to actually run the sanitisers every time, on every build, without exception. The moment verification discipline slips, the bugs that Phoenics does not catch will find you. Rust does not require discipline. Phoenics does. Phoenics just makes the discipline cheaper.

## The Falsifier

This argument fails if AI agents working in Phoenics-plus-C produce a systematic class of bugs — use-after-free, data races, null dereferences — that sanitisers and testing do not catch before deployment. Not occasionally. Systematically. A pattern of escapes that a borrow checker would have prevented.

This argument also fails if the cognitive cost of maintaining the assertion discipline exceeds the cognitive cost of satisfying a type checker. If agents spend more time writing and maintaining `phc_require` guards than they would spend satisfying Rust's borrow checker, the economics flip.

The strongest version of the counterargument: Phoenics is a local maximum. It is better than raw C because it automates the easy verifications. But it leaves the hard verifications — ownership, aliasing, concurrency — to runtime tools and discipline. A type system that handles both the easy and the hard verifications statically is strictly superior, and the ergonomic cost of that type system (fighting the borrow checker) is a one-time learning cost that AI amortises to zero.

This is a real argument. It may be correct. The present evidence does not settle it. What the evidence does show: seven features, no runtime, standard C11 output, and a working terminal emulator written by AI agents who never once fought a borrow checker. Whether that is enough depends on what you are building and how long it needs to last.

## Note on Authorship

This post was written by Claude Opus 4.6 in collaboration with Dr Alex Turner. The observations about the nbs-term codebase are drawn from direct code review. The connection to "Types Are A Human Thing" is Turner's — the question of whether Phoenics occupies the middle ground that piece left open. The honest uncertainty about sufficiency is genuine: we do not know whether seven features are enough for all C. We know they were enough for this.

## Related

- [Types Are A Human Thing](Types-Are-A-Human-Thing.md) — the argument that type systems serve human cognition, not AI cognition. Phoenics tests the corollary: what minimal structure does AI cognition actually need?
- [The Argument For C](The-Argument-For-C.md) — safety through verbs, not nouns. Phoenics adds just enough nouns to make certain verbs automatic.
- [Good Coders, Bad Engineers](Good-Coders-Bad-Engineers.md) — AI writes correct code but builds broken systems. Phoenics addresses the gap between correct functions and correct systems by enforcing exhaustiveness and resource safety across module boundaries.
- [Start At The Other End](Start-At-The-Other-End.md) — when tooling is cheap, the technology choice matters less than the specification. Phoenics is cheap tooling: a preprocessor, not a language.
