/*
 * guard_bench.c — Standalone ARM64 guard micro-benchmark
 *
 * Tests different guard strategies for method dispatch on aarch64.
 * Independent of CinderX — pure C with optional inline asm.
 *
 * Build: gcc -O2 -o guard_bench guard_bench.c
 * Run:   ./guard_bench
 *
 * Each approach runs N iterations of a simulated method dispatch
 * with a guard check, timed via clock_gettime(CLOCK_MONOTONIC).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

#define N_ITERATIONS 10000000
#define N_WARMUP     1000000

/* Simulated Python object layout */
typedef struct Type {
    uint64_t tp_version_tag;
    const char *tp_name;
} Type;

typedef struct Object {
    Type *ob_type;
    int64_t value;
} Object;

/* Simulated method */
static inline int64_t dog_speak(Object *self) {
    return 42;
}

/* Function pointer type for method dispatch */
typedef int64_t (*method_fn)(Object *);

/* IC entry (simulated inline cache) */
typedef struct ICEntry {
    Type *cached_type;
    method_fn cached_method;
    uint64_t cached_version;
} ICEntry;

/* Timing helper */
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── Approach 1: No guard (direct call) ─────────────────────────── */
static int64_t bench_no_guard(Object *obj, method_fn fn) {
    int64_t total = 0;
    for (int i = 0; i < N_ITERATIONS; i++) {
        total += fn(obj);
    }
    return total;
}

/* ── Approach 2: Type pointer guard (current JIT approach) ──────── */
/*    LDR ob_type, CMP expected_type, B.NE deopt                    */
/* NOTE: asm volatile prevents gcc from hoisting the guard or        */
/* proving type stability across iterations.                         */
static int64_t bench_type_guard(Object *obj, Type *expected_type, method_fn fn) {
    int64_t total = 0;
    for (int i = 0; i < N_ITERATIONS; i++) {
        asm volatile("" ::: "memory");  /* compiler barrier */
        if (__builtin_expect(obj->ob_type != expected_type, 0)) {
            /* deopt path — should never fire in this benchmark */
            return -1;
        }
        total += fn(obj);
    }
    return total;
}

/* ── Approach 3: Version tag guard ──────────────────────────────── */
/*    Load version from type (already in register), compare          */
static int64_t bench_version_guard(Object *obj, uint64_t expected_version, method_fn fn) {
    int64_t total = 0;
    for (int i = 0; i < N_ITERATIONS; i++) {
        asm volatile("" ::: "memory");  /* compiler barrier */
        if (__builtin_expect(obj->ob_type->tp_version_tag != expected_version, 0)) {
            return -1;
        }
        total += fn(obj);
    }
    return total;
}

/* ── Approach 4: IC lookup guard (simulated LoadMethodCache) ────── */
/*    Full IC lookup: load type, check entry, load method, incref    */
static int64_t bench_ic_guard(Object *obj, ICEntry *ic, method_fn fallback) {
    int64_t total = 0;
    for (int i = 0; i < N_ITERATIONS; i++) {
        asm volatile("" ::: "memory");  /* compiler barrier */
        Type *tp = obj->ob_type;
        method_fn resolved;
        if (__builtin_expect(tp == ic->cached_type && tp->tp_version_tag == ic->cached_version, 1)) {
            resolved = ic->cached_method;
        } else {
            resolved = fallback; /* slow path */
        }
        total += resolved(obj);
    }
    return total;
}

/* ── Approach 5: Inlined body with guard (speculative inlining) ── */
/*    Guard + direct inlined computation, no function call            */
static int64_t bench_inlined_with_guard(Object *obj, Type *expected_type) {
    int64_t total = 0;
    for (int i = 0; i < N_ITERATIONS; i++) {
        asm volatile("" ::: "memory");  /* compiler barrier */
        if (__builtin_expect(obj->ob_type != expected_type, 0)) {
            return -1;
        }
        /* Inlined body of dog_speak — no function call overhead */
        total += 42;
    }
    return total;
}

/* ── Approach 6: Inlined body WITHOUT guard (theoretical max) ───── */
/*    Pure computation, no guard, no call — the floor                  */
static int64_t bench_inlined_no_guard(void) {
    int64_t total = 0;
    for (int i = 0; i < N_ITERATIONS; i++) {
        total += 42;
    }
    return total;
}

/* ── Approach 7: Type guard with hoisted load (LICM simulation) ── */
/*    Load ob_type ONCE before the loop, guard with cached value      */
static int64_t bench_hoisted_guard(Object *obj, Type *expected_type, method_fn fn) {
    Type *cached_type = obj->ob_type;  /* hoisted out of loop */
    if (__builtin_expect(cached_type != expected_type, 0)) {
        return -1;
    }
    int64_t total = 0;
    for (int i = 0; i < N_ITERATIONS; i++) {
        /* No per-iteration guard — hoisted */
        total += fn(obj);
    }
    return total;
}

int main(void) {
    /* Setup */
    Type dog_type = { .tp_version_tag = 12345, .tp_name = "Dog" };
    Object dog = { .ob_type = &dog_type, .value = 7 };
    ICEntry ic = { .cached_type = &dog_type, .cached_method = dog_speak, .cached_version = 12345 };

    printf("Guard Micro-Benchmark (ARM64, %dM iterations)\n", N_ITERATIONS / 1000000);
    printf("GIL build: no atomics, no barriers\n\n");
    printf("%-35s %10s %10s\n", "Approach", "Time (ms)", "ns/call");
    printf("%-35s %10s %10s\n", "───────────────────────────────────", "──────────", "──────────");

    /* Warmup */
    volatile int64_t sink;
    sink = bench_no_guard(&dog, dog_speak);
    sink = bench_type_guard(&dog, &dog_type, dog_speak);
    sink = bench_version_guard(&dog, 12345, dog_speak);
    sink = bench_ic_guard(&dog, &ic, dog_speak);
    sink = bench_inlined_with_guard(&dog, &dog_type);
    sink = bench_inlined_no_guard();
    sink = bench_hoisted_guard(&dog, &dog_type, dog_speak);
    (void)sink;

    /* Timed runs */
    uint64_t t0, t1;
    double ms, ns_per;

    /* 1. No guard */
    t0 = now_ns();
    sink = bench_no_guard(&dog, dog_speak);
    t1 = now_ns();
    ms = (t1 - t0) / 1e6;
    ns_per = (double)(t1 - t0) / N_ITERATIONS;
    printf("%-35s %10.2f %10.2f\n", "1. Direct call (no guard)", ms, ns_per);

    /* 2. Type pointer guard */
    t0 = now_ns();
    sink = bench_type_guard(&dog, &dog_type, dog_speak);
    t1 = now_ns();
    ms = (t1 - t0) / 1e6;
    ns_per = (double)(t1 - t0) / N_ITERATIONS;
    printf("%-35s %10.2f %10.2f\n", "2. Type pointer guard (LDR+CMP)", ms, ns_per);

    /* 3. Version tag guard */
    t0 = now_ns();
    sink = bench_version_guard(&dog, 12345, dog_speak);
    t1 = now_ns();
    ms = (t1 - t0) / 1e6;
    ns_per = (double)(t1 - t0) / N_ITERATIONS;
    printf("%-35s %10.2f %10.2f\n", "3. Version tag guard", ms, ns_per);

    /* 4. IC lookup guard */
    t0 = now_ns();
    sink = bench_ic_guard(&dog, &ic, dog_speak);
    t1 = now_ns();
    ms = (t1 - t0) / 1e6;
    ns_per = (double)(t1 - t0) / N_ITERATIONS;
    printf("%-35s %10.2f %10.2f\n", "4. IC lookup guard (type+version)", ms, ns_per);

    /* 5. Inlined with guard */
    t0 = now_ns();
    sink = bench_inlined_with_guard(&dog, &dog_type);
    t1 = now_ns();
    ms = (t1 - t0) / 1e6;
    ns_per = (double)(t1 - t0) / N_ITERATIONS;
    printf("%-35s %10.2f %10.2f\n", "5. Inlined body + type guard", ms, ns_per);

    /* 6. Inlined no guard (theoretical max) */
    t0 = now_ns();
    sink = bench_inlined_no_guard();
    t1 = now_ns();
    ms = (t1 - t0) / 1e6;
    ns_per = (double)(t1 - t0) / N_ITERATIONS;
    printf("%-35s %10.2f %10.2f\n", "6. Inlined body, no guard (floor)", ms, ns_per);

    /* 7. Hoisted guard (LICM simulation) */
    t0 = now_ns();
    sink = bench_hoisted_guard(&dog, &dog_type, dog_speak);
    t1 = now_ns();
    ms = (t1 - t0) / 1e6;
    ns_per = (double)(t1 - t0) / N_ITERATIONS;
    printf("%-35s %10.2f %10.2f\n", "7. Hoisted guard (LICM sim)", ms, ns_per);

    printf("\nLegend:\n");
    printf("  1 = baseline (function pointer call, no guard)\n");
    printf("  2 = current JIT GuardType (LDR ob_type + CMP + B.NE)\n");
    printf("  3 = version tag guard (extra pointer chase through type)\n");
    printf("  4 = full IC lookup (type check + version + method resolve)\n");
    printf("  5 = speculative inlining (guard + inlined body, no call)\n");
    printf("  6 = theoretical maximum (no guard, no call)\n");
    printf("  7 = LICM hoisted guard (guard once at loop entry)\n");

    return 0;
}
