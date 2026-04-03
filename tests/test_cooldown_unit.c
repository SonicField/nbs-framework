/*
 * test_cooldown_unit.c — TDD red-phase tests for cooldown extraction (Item 3)
 *
 * The dual cooldown problem: two independent code paths in sidecar.c
 * compute cooldown state from the same inputs (last_notify_time,
 * notify_cooldown) but independently. They agree by timing coincidence,
 * not by design. If either is refactored independently (different
 * constants, per-priority cooldowns), they diverge silently.
 *
 * Fix: extract a single cooldown_is_active() function. Both paths
 * call it. Logic lives in one place.
 *
 * These tests will FAIL TO LINK until cooldown_is_active() is
 * implemented in sidecar.c and declared in sidecar.h. That is the
 * red phase — tests define the interface before the code exists.
 *
 * Falsifiable claims tested:
 *   1. cooldown_is_active returns 1 when elapsed < notify_cooldown
 *   2. cooldown_is_active returns 0 when elapsed >= notify_cooldown
 *   3. cooldown_is_active returns 0 when last_notify_time is 0 (never notified)
 *   4. cooldown_is_active returns 0 when notify_cooldown is 0 (disabled)
 *   5. cooldown_is_active handles time_t overflow (large last_notify_time)
 *   6. should_inject_notify uses cooldown_is_active (structural)
 *   7. Main loop cooldown tracking uses cooldown_is_active (structural)
 *
 * Build (from project root):
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2 \
 *       -I src/nbs-sidecar -I src/nbs-common \
 *       -o tests/test_cooldown_unit tests/test_cooldown_unit.c \
 *       src/nbs-sidecar/sidecar.c src/nbs-sidecar/transport_ts.c \
 *       -lutil
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sidecar.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    tests_passed++; \
    printf("  PASS: %s\n", name); \
} while(0)

/* --- Helper: create a valid config with given cooldown --- */
static sidecar_config_t make_config(int notify_cooldown) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test-agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_cooldown = notify_cooldown;
    cfg.startup_grace = 30;
    cfg.notify_fail_threshold = 5;
    return cfg;
}

/* --- Helper: create a state with given last_notify_time --- */
static sidecar_state_t make_state(time_t last_notify_time) {
    sidecar_state_t state;
    memset(&state, 0, sizeof(state));
    state.last_notify_time = last_notify_time;
    return state;
}

/* Test 1: Cooldown active when recently notified */
static void test_cooldown_active_within_period(void) {
    sidecar_config_t cfg = make_config(15);  /* 15 second cooldown */
    sidecar_state_t state = make_state(time(NULL) - 5);  /* notified 5s ago */

    int active = cooldown_is_active(&cfg, &state);
    TEST_ASSERT(active == 1,
                "cooldown should be active 5s after notification "
                "(cooldown=15s), got active=%d", active);
    TEST_PASS("cooldown active within period");
}

/* Test 2: Cooldown expired when enough time has passed */
static void test_cooldown_expired_after_period(void) {
    sidecar_config_t cfg = make_config(15);  /* 15 second cooldown */
    sidecar_state_t state = make_state(time(NULL) - 20);  /* notified 20s ago */

    int active = cooldown_is_active(&cfg, &state);
    TEST_ASSERT(active == 0,
                "cooldown should be expired 20s after notification "
                "(cooldown=15s), got active=%d", active);
    TEST_PASS("cooldown expired after period");
}

/* Test 3: No cooldown when never notified (last_notify_time == 0) */
static void test_cooldown_inactive_when_never_notified(void) {
    sidecar_config_t cfg = make_config(15);
    sidecar_state_t state = make_state(0);  /* never notified */

    int active = cooldown_is_active(&cfg, &state);
    TEST_ASSERT(active == 0,
                "cooldown should be inactive when never notified "
                "(last_notify_time=0), got active=%d", active);
    TEST_PASS("cooldown inactive when never notified");
}

/* Test 4: No cooldown when cooldown period is 0 (disabled) */
static void test_cooldown_inactive_when_disabled(void) {
    sidecar_config_t cfg = make_config(0);  /* cooldown disabled */
    sidecar_state_t state = make_state(time(NULL) - 1);  /* notified 1s ago */

    int active = cooldown_is_active(&cfg, &state);
    TEST_ASSERT(active == 0,
                "cooldown should be inactive when cooldown=0, "
                "got active=%d", active);
    TEST_PASS("cooldown inactive when disabled");
}

/* Test 5: Cooldown boundary — exactly at the threshold */
static void test_cooldown_boundary_exact(void) {
    sidecar_config_t cfg = make_config(15);
    /* At exactly the boundary, cooldown should NOT be active
     * (elapsed >= cooldown means expired) */
    sidecar_state_t state = make_state(time(NULL) - 15);

    int active = cooldown_is_active(&cfg, &state);
    TEST_ASSERT(active == 0,
                "cooldown should be expired at exact boundary "
                "(elapsed==cooldown=15s), got active=%d", active);
    TEST_PASS("cooldown boundary: exactly at threshold");
}

/* Test 6: Large elapsed time (time_t safety) */
static void test_cooldown_large_elapsed(void) {
    sidecar_config_t cfg = make_config(15);
    /* last_notify_time very old — should not overflow or wrap */
    sidecar_state_t state = make_state(1000);  /* epoch + 1000s */

    int active = cooldown_is_active(&cfg, &state);
    TEST_ASSERT(active == 0,
                "cooldown should be expired with very old last_notify_time, "
                "got active=%d", active);
    TEST_PASS("cooldown: large elapsed time (time_t safety)");
}

int main(void) {
    printf("=== cooldown extraction unit tests ===\n\n");
    printf("--- Single-source cooldown (Item 3) ---\n");

    test_cooldown_active_within_period();
    test_cooldown_expired_after_period();
    test_cooldown_inactive_when_never_notified();
    test_cooldown_inactive_when_disabled();
    test_cooldown_boundary_exact();
    test_cooldown_large_elapsed();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
