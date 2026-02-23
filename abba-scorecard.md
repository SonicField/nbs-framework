# ABBA-Verified Benchmark Scorecard

CinderX JIT specialisation results on aarch64 (build-host verified using
ABBA interleaved methodology (15 blocks × 4 measurements = 60 per builtin).

## Methodology

ABBA interleaving: each block measures A, B, B, A to cancel monotonic drift
from co-located workloads on shared GPU hardware. Significance determined by
IQR of per-block deltas — significant only when entire IQR is on one side of
zero.

Control validation: 0/7 benchmarks show false significance under standard
Python (no CinderX). Methodology produces no false positives from noise alone.

## CALL_BUILTIN_FAST Results

Measures JIT-compiled caller vs interpreter caller calling the same builtin.

| Builtin      | JIT (ns) | Interp (ns) | Improvement | Significant | Notes |
|--------------|----------|-------------|-------------|-------------|-------|
| issubclass   | 28.0     | 36.7        | +23.6%      | YES         |       |
| hasattr      | 33.4     | 41.6        | +19.7%      | YES         |       |
| divmod       | 52.2     | 59.2        | +11.9%      | YES         |       |
| getattr      | 33.8     | 36.7        | +7.9%       | YES         |       |
| next_default | 49.6     | 50.3        | +1.4%       | YES (small) |       |
| isinstance   | 32.0     | 32.0        | 0%          | NO          | Sequential +17% was noise |

## G1 Fast Path Results (JITRT_BuiltinNext)

Measures JIT generator vs interpreter generator, both called from JIT caller.
Isolates the G1 fast path (direct resumeEntry call vs tp_iternext fallback).

| Builtin    | JIT gen (ns) | Interp gen (ns) | Improvement | Significant | Notes |
|------------|--------------|-----------------|-------------|-------------|-------|
| next (G1)  | 34.7         | 51.2            | +32.2%      | YES         | IQR width 0.5 ms, all 15 blocks consistent |

Sequential estimate was +16.5% — underestimated by 2× because it confounded
caller and generator compilation variables.

## Key Findings

1. **isinstance +17% was noise.** ABBA correctly falsified the sequential
   estimate. At 32 ns, isinstance is already at the C-level type check floor;
   JIT dispatch overhead is negligible relative to the comparison work.

2. **next() G1 was underestimated.** Sequential measured +16.5% by comparing
   JIT gen + JIT caller vs interp gen + interp caller (two confounded
   variables). ABBA isolated the generator variable and found +32.2%.

3. **6/7 builtins show verified improvement.** The CALL_BUILTIN_FAST
   specialisation and JITRT_BuiltinNext G1 fast path provide real,
   significant speedups on aarch64.

## Commit Chain

```
2286e368  Bug 7 fix
fc986d02  Bug 8 fix
54aaa20f  Test suite
1e5e54e9  CALL_BUILTIN_FAST specialisation
46de56af  JITRT_BuiltinNext (G1 fast path)
```

## Environment

- Machine: build-host (aarch64)
- Python: /usr/local/internal-toolchain/platform010-aarch64/bin/python3
- Branch: aarch64-jit-generators
- CinderX: cinderjit.auto() + enable_specialized_opcodes()
- Date: 2026-02-23
