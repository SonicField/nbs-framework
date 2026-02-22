/*
 * test_sidecar_hash_unit.c — Unit tests for FNV-1a hash.
 *
 * Falsifiable claims tested:
 *   1. Determinism: same input always produces same hash.
 *   2. Discrimination: different inputs produce different hashes.
 *   3. Empty string returns the FNV offset basis.
 *   4. Single byte 'a' matches known FNV-1a 64-bit vector.
 *   5. "foobar" matches known FNV-1a 64-bit vector.
 *   6. NULL data with len=0 does not crash (precondition allows it).
 */

#include "../src/nbs-sidecar/hash.h"
#include <stdio.h>
#include <string.h>

static int tests = 0, fails = 0;

#define CHECK(label, cond) do { \
    tests++; \
    if (!(cond)) { \
        fails++; \
        printf("   FAIL: %s\n", label); \
    } else { \
        printf("   PASS: %s\n", label); \
    } \
} while(0)

int main(void) {
    printf("test_sidecar_hash_unit\n");

    /* 1. Determinism: same input, same hash */
    {
        const char *msg = "hello world";
        uint64_t h1 = fnv1a_hash(msg, strlen(msg));
        uint64_t h2 = fnv1a_hash(msg, strlen(msg));
        CHECK("same input produces same hash", h1 == h2);
    }

    /* 2. Discrimination: different input, different hash */
    {
        const char *a = "hello";
        const char *b = "world";
        uint64_t ha = fnv1a_hash(a, strlen(a));
        uint64_t hb = fnv1a_hash(b, strlen(b));
        CHECK("different input produces different hash", ha != hb);
    }

    /* 3. Empty string returns FNV offset basis */
    {
        uint64_t h = fnv1a_hash("", 0);
        CHECK("empty string returns offset basis",
              h == 14695981039346656037ULL);
    }

    /* 4. Known vector: "a" */
    {
        uint64_t h = fnv1a_hash("a", 1);
        CHECK("hash of 'a' matches FNV-1a spec (0xaf63dc4c8601ec8c)",
              h == 0xaf63dc4c8601ec8cULL);
    }

    /* 5. Known vector: "foobar" */
    {
        uint64_t h = fnv1a_hash("foobar", 6);
        CHECK("hash of 'foobar' matches FNV-1a spec (0x85944171f73967e8)",
              h == 0x85944171f73967e8ULL);
    }

    /* 6. NULL data with len=0 does not crash */
    {
        uint64_t h = fnv1a_hash(NULL, 0);
        CHECK("NULL data with len=0 does not crash",
              h == 14695981039346656037ULL);
    }

    printf("%d/%d passed\n", tests - fails, tests);
    return fails;
}
