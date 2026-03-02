/*
 * test_watchdog_unit.c — Unit tests for the watchdog state machine
 *
 * Tests the pure decision logic in watchdog.c without threads or I/O.
 * Written BEFORE implementation (TDD).
 *
 * Build:
 *   gcc -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L \
 *       -O2 -I ../src/nbs-chat \
 *       -o test_watchdog_unit test_watchdog_unit.c ../src/nbs-chat/watchdog.c
 *
 * 26 tests across 7 groups:
 *   A: Initialisation (3)
 *   B: Team alive (3)
 *   C: Team dead (6)
 *   D: Rate limiting (5)
 *   E: Cooldown (3)
 *   F: Enable/disable (4)
 *   G: Adversarial (2)
 */

#include "watchdog.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL %s:%d: " fmt "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define TEST_PASS(name) do { \
    printf("  PASS: %s\n", (name)); \
    tests_passed++; \
} while (0)

/* Helper: run a function in a child process and check if it aborts */
static int expect_abort(void (*fn)(void)) {
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        _exit(0); /* did not abort */
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

/* ================================================================
 * Group A: Initialisation invariants
 * ================================================================ */

static void test_init_sets_enabled(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/chat.chat", "/tmp/project");
    TEST_ASSERT(watchdog_is_enabled(&ws) == 1,
                "expected enabled=1 after init, got %d",
                watchdog_is_enabled(&ws));
    TEST_ASSERT(ws.restart_count == 0,
                "expected restart_count=0 after init, got %d",
                ws.restart_count);
    TEST_PASS("init sets enabled=1, restart_count=0");
}

static void test_init_copies_paths(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/test.chat", "/home/user/proj");
    TEST_ASSERT(strcmp(ws.chat_path, "/tmp/test.chat") == 0,
                "chat_path mismatch: '%s'", ws.chat_path);
    TEST_ASSERT(strcmp(ws.project_root, "/home/user/proj") == 0,
                "project_root mismatch: '%s'", ws.project_root);
    TEST_PASS("init copies paths correctly");
}

static void child_init_null_chat(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, NULL, "/tmp/project");
}

static void child_init_null_root(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/chat.chat", NULL);
}

static void test_init_rejects_null(void) {
    TEST_ASSERT(expect_abort(child_init_null_chat),
                "expected abort on NULL chat_path");
    TEST_ASSERT(expect_abort(child_init_null_root),
                "expected abort on NULL project_root");
    TEST_PASS("init rejects NULL paths with abort");
}

/* ================================================================
 * Group B: Team alive
 * ================================================================ */

static void test_alive_returns_no_action(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");
    watchdog_decision_t d = watchdog_evaluate(&ws, 6, 1000);
    TEST_ASSERT(d == WATCHDOG_NO_ACTION,
                "expected NO_ACTION for 6 alive, got %d", d);
    TEST_PASS("6 alive → NO_ACTION");
}

static void test_alive_at_boundary(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");
    watchdog_decision_t d = watchdog_evaluate(&ws, 3, 1000);
    TEST_ASSERT(d == WATCHDOG_NO_ACTION,
                "expected NO_ACTION for exactly 3 alive, got %d", d);
    TEST_PASS("exactly 3 alive → NO_ACTION (boundary)");
}

static void test_alive_resets_rate_after_window(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");

    /* Simulate 4 restarts */
    time_t t = 10000;
    for (int i = 0; i < 4; i++) {
        watchdog_evaluate(&ws, 0, t + i * 200);
    }
    TEST_ASSERT(ws.restart_count == 4,
                "expected 4 restarts, got %d", ws.restart_count);

    /* Team alive for over an hour */
    watchdog_decision_t d = watchdog_evaluate(&ws, 6, t + 4000);
    TEST_ASSERT(d == WATCHDOG_NO_ACTION, "alive → NO_ACTION");
    TEST_ASSERT(ws.restart_count == 0,
                "expected rate counter reset to 0, got %d", ws.restart_count);
    TEST_PASS("alive after window elapsed resets rate counter");
}

/* ================================================================
 * Group C: Team dead
 * ================================================================ */

static void test_dead_returns_restart(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");
    watchdog_decision_t d = watchdog_evaluate(&ws, 2, 1000);
    TEST_ASSERT(d == WATCHDOG_RESTART,
                "expected RESTART for 2 alive, got %d", d);
    TEST_PASS("2 alive → RESTART");
}

static void test_dead_at_zero(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");
    watchdog_decision_t d = watchdog_evaluate(&ws, 0, 1000);
    TEST_ASSERT(d == WATCHDOG_RESTART,
                "expected RESTART for 0 alive, got %d", d);
    TEST_PASS("0 alive → RESTART");
}

static void test_dead_increments_count(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");
    watchdog_evaluate(&ws, 0, 1000);
    TEST_ASSERT(ws.restart_count == 1,
                "expected restart_count=1, got %d", ws.restart_count);
    /* Second restart after cooldown */
    watchdog_evaluate(&ws, 0, 1000 + WATCHDOG_COOLDOWN_S);
    TEST_ASSERT(ws.restart_count == 2,
                "expected restart_count=2, got %d", ws.restart_count);
    TEST_PASS("dead increments restart_count");
}

static void test_dead_sets_last_restart(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");
    watchdog_evaluate(&ws, 1, 5000);
    TEST_ASSERT(ws.last_restart == 5000,
                "expected last_restart=5000, got %ld", (long)ws.last_restart);
    TEST_PASS("dead sets last_restart to now");
}

static void test_dead_opens_window(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");
    TEST_ASSERT(ws.window_start == 0, "window_start should be 0 before first restart");
    watchdog_evaluate(&ws, 0, 2000);
    TEST_ASSERT(ws.window_start == 2000,
                "expected window_start=2000, got %ld", (long)ws.window_start);
    TEST_PASS("first restart opens rate window");
}

static void test_dead_boundary_at_2(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");
    watchdog_decision_t d = watchdog_evaluate(&ws, 2, 1000);
    TEST_ASSERT(d == WATCHDOG_RESTART,
                "expected RESTART for 2 (below 3), got %d", d);
    TEST_PASS("2 alive (below threshold 3) → RESTART");
}

/* ================================================================
 * Group D: Rate limiting
 * ================================================================ */

static void test_rate_limit_after_5(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");

    time_t t = 10000;
    /* 5 restarts, each after cooldown */
    for (int i = 0; i < 5; i++) {
        watchdog_decision_t d = watchdog_evaluate(&ws, 0, t);
        TEST_ASSERT(d == WATCHDOG_RESTART,
                    "restart %d should succeed, got %d", i + 1, d);
        t += WATCHDOG_COOLDOWN_S;
    }
    TEST_ASSERT(ws.restart_count == 5,
                "expected 5 restarts, got %d", ws.restart_count);

    /* 6th should be rate limited */
    watchdog_decision_t d = watchdog_evaluate(&ws, 0, t);
    TEST_ASSERT(d == WATCHDOG_RATE_LIMITED,
                "6th restart should be RATE_LIMITED, got %d", d);
    TEST_PASS("6th restart in same window → RATE_LIMITED");
}

static void test_rate_limit_disables(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");

    time_t t = 10000;
    for (int i = 0; i < 5; i++) {
        watchdog_evaluate(&ws, 0, t);
        t += WATCHDOG_COOLDOWN_S;
    }
    watchdog_evaluate(&ws, 0, t); /* triggers RATE_LIMITED */

    TEST_ASSERT(watchdog_is_enabled(&ws) == 0,
                "watchdog should be disabled after rate limit");
    TEST_PASS("rate limit disables watchdog");
}

static void test_rate_resets_after_window(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");

    time_t t = 10000;
    /* 4 restarts */
    for (int i = 0; i < 4; i++) {
        watchdog_evaluate(&ws, 0, t);
        t += WATCHDOG_COOLDOWN_S;
    }
    TEST_ASSERT(ws.restart_count == 4, "expected 4 restarts");

    /* Wait for window to expire, then restart */
    t = 10000 + WATCHDOG_RATE_WINDOW_S + 1;
    watchdog_decision_t d = watchdog_evaluate(&ws, 0, t);
    TEST_ASSERT(d == WATCHDOG_RESTART,
                "after window reset, should RESTART, got %d", d);
    TEST_ASSERT(ws.restart_count == 1,
                "counter should reset to 1, got %d", ws.restart_count);
    TEST_PASS("rate counter resets after window expires");
}

static void test_rate_5th_succeeds(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");

    time_t t = 10000;
    for (int i = 0; i < 4; i++) {
        watchdog_evaluate(&ws, 0, t);
        t += WATCHDOG_COOLDOWN_S;
    }
    watchdog_decision_t d = watchdog_evaluate(&ws, 0, t);
    TEST_ASSERT(d == WATCHDOG_RESTART,
                "5th restart should succeed, got %d", d);
    TEST_ASSERT(ws.restart_count == 5,
                "expected restart_count=5, got %d", ws.restart_count);
    TEST_PASS("5th restart succeeds (limit is >5, not >=5)");
}

static void test_rate_window_boundary(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");

    time_t t = 10000;
    for (int i = 0; i < 5; i++) {
        watchdog_evaluate(&ws, 0, t);
        t += WATCHDOG_COOLDOWN_S;
    }

    /* At exactly window_start + RATE_WINDOW_S, the window should reset */
    time_t boundary = 10000 + WATCHDOG_RATE_WINDOW_S;
    watchdog_decision_t d = watchdog_evaluate(&ws, 0, boundary);
    TEST_ASSERT(d == WATCHDOG_RESTART,
                "at exact window boundary, should RESTART (window resets), got %d", d);
    TEST_PASS("exact window boundary resets rate counter");
}

/* ================================================================
 * Group E: Cooldown
 * ================================================================ */

static void test_cooldown_blocks(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");

    watchdog_evaluate(&ws, 0, 1000); /* first restart */
    /* 60s later — still in cooldown */
    watchdog_decision_t d = watchdog_evaluate(&ws, 0, 1060);
    TEST_ASSERT(d == WATCHDOG_NO_ACTION,
                "during cooldown should be NO_ACTION, got %d", d);
    TEST_PASS("cooldown blocks restart within 120s");
}

static void test_cooldown_boundary(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");

    watchdog_evaluate(&ws, 0, 1000); /* first restart at t=1000 */
    /* Exactly at cooldown boundary */
    watchdog_decision_t d = watchdog_evaluate(&ws, 0, 1000 + WATCHDOG_COOLDOWN_S);
    TEST_ASSERT(d == WATCHDOG_RESTART,
                "at exact cooldown boundary, should RESTART, got %d", d);
    TEST_PASS("restart allowed at exact cooldown boundary");
}

static void test_cooldown_no_increment(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");

    watchdog_evaluate(&ws, 0, 1000); /* first restart */
    TEST_ASSERT(ws.restart_count == 1, "expected 1 restart");

    /* Cooldown NO_ACTION should NOT increment */
    watchdog_evaluate(&ws, 0, 1060);
    TEST_ASSERT(ws.restart_count == 1,
                "cooldown should not increment count, got %d", ws.restart_count);
    TEST_PASS("cooldown NO_ACTION does not increment restart_count");
}

/* ================================================================
 * Group F: Enable/disable
 * ================================================================ */

static void test_disable_returns_disabled(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");
    watchdog_disable(&ws);
    watchdog_decision_t d = watchdog_evaluate(&ws, 0, 1000);
    TEST_ASSERT(d == WATCHDOG_DISABLED,
                "disabled watchdog should return DISABLED, got %d", d);
    TEST_PASS("disabled → DISABLED even with dead team");
}

static void test_enable_after_disable(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");
    watchdog_disable(&ws);
    TEST_ASSERT(watchdog_is_enabled(&ws) == 0, "should be disabled");
    watchdog_enable(&ws);
    TEST_ASSERT(watchdog_is_enabled(&ws) == 1, "should be re-enabled");
    watchdog_decision_t d = watchdog_evaluate(&ws, 0, 1000);
    TEST_ASSERT(d == WATCHDOG_RESTART,
                "re-enabled watchdog should RESTART on dead team, got %d", d);
    TEST_PASS("enable after disable re-activates watchdog");
}

static void test_disabled_ignores_dead(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");
    watchdog_disable(&ws);

    /* Multiple dead evaluations — none should trigger */
    for (int i = 0; i < 10; i++) {
        watchdog_decision_t d = watchdog_evaluate(&ws, 0, 1000 + i * 200);
        TEST_ASSERT(d == WATCHDOG_DISABLED,
                    "disabled: iteration %d should be DISABLED, got %d", i, d);
    }
    TEST_ASSERT(ws.restart_count == 0,
                "disabled: restart_count should stay 0, got %d", ws.restart_count);
    TEST_PASS("disabled watchdog ignores dead team completely");
}

static void test_enable_preserves_rate(void) {
    watchdog_state_t ws;
    watchdog_init(&ws, "/tmp/c.chat", "/tmp/p");

    /* 3 restarts */
    time_t t = 10000;
    for (int i = 0; i < 3; i++) {
        watchdog_evaluate(&ws, 0, t);
        t += WATCHDOG_COOLDOWN_S;
    }
    TEST_ASSERT(ws.restart_count == 3, "expected 3 restarts");

    watchdog_disable(&ws);
    watchdog_enable(&ws);

    TEST_ASSERT(ws.restart_count == 3,
                "enable should preserve restart_count, got %d", ws.restart_count);
    TEST_PASS("enable preserves rate state (restart_count unchanged)");
}

/* ================================================================
 * Group G: Adversarial inputs
 * ================================================================ */

static watchdog_state_t g_adv_ws;

static void child_negative_alive(void) {
    watchdog_init(&g_adv_ws, "/tmp/c.chat", "/tmp/p");
    watchdog_evaluate(&g_adv_ws, -1, 1000);
}

static void test_negative_alive_aborts(void) {
    TEST_ASSERT(expect_abort(child_negative_alive),
                "negative alive_count should abort");
    TEST_PASS("negative alive_count triggers assertion");
}

static void child_time_zero(void) {
    watchdog_init(&g_adv_ws, "/tmp/c.chat", "/tmp/p");
    watchdog_evaluate(&g_adv_ws, 3, 0);
}

static void test_time_zero_aborts(void) {
    TEST_ASSERT(expect_abort(child_time_zero),
                "time=0 should abort");
    TEST_PASS("time=0 triggers assertion");
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void) {
    printf("=== watchdog unit tests ===\n\n");

    /* Group A: Initialisation */
    test_init_sets_enabled();
    test_init_copies_paths();
    test_init_rejects_null();

    /* Group B: Team alive */
    test_alive_returns_no_action();
    test_alive_at_boundary();
    test_alive_resets_rate_after_window();

    /* Group C: Team dead */
    test_dead_returns_restart();
    test_dead_at_zero();
    test_dead_increments_count();
    test_dead_sets_last_restart();
    test_dead_opens_window();
    test_dead_boundary_at_2();

    /* Group D: Rate limiting */
    test_rate_limit_after_5();
    test_rate_limit_disables();
    test_rate_resets_after_window();
    test_rate_5th_succeeds();
    test_rate_window_boundary();

    /* Group E: Cooldown */
    test_cooldown_blocks();
    test_cooldown_boundary();
    test_cooldown_no_increment();

    /* Group F: Enable/disable */
    test_disable_returns_disabled();
    test_enable_after_disable();
    test_disabled_ignores_dead();
    test_enable_preserves_rate();

    /* Group G: Adversarial */
    test_negative_alive_aborts();
    test_time_zero_aborts();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
