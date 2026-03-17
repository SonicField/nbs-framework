/*
 * test_sidecar_detect_unit.c — Unit tests for detect.c
 *
 * Tests all detection functions against positive and negative cases.
 * Each test is falsifiable: the assertion names what would prove it wrong.
 *
 * Build:
 *   make -C src/nbs-sidecar detect.o
 *   gcc -Wall -Wextra -Wshadow -Werror -std=c11 \
 *       -I src/nbs-common -I src/nbs-sidecar \
 *       -o tests/test_sidecar_detect_unit \
 *       tests/test_sidecar_detect_unit.c src/nbs-sidecar/detect.o
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "detect.h"

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

/* ---- 1. plan_mode positive ---- */

static void test_plan_mode_positive(void)
{
    const char *content = "Some preamble\nWould you like to proceed?\n1. Yes\n2. No";
    dialogue_response_t resp = {0, 0};
    dialogue_type_t type = detect_blocking_dialogue(content, &resp);

    TEST_ASSERT(type == DIALOGUE_PLAN_MODE,
                "expected DIALOGUE_PLAN_MODE (%d), got %d",
                DIALOGUE_PLAN_MODE, type);
    TEST_ASSERT(resp.option == 2,
                "expected option=2, got %d", resp.option);
    TEST_ASSERT(resp.settle_secs == 5,
                "expected settle=5, got %d", resp.settle_secs);

    TEST_PASS("plan_mode positive");
}

/* ---- 2. plan_mode negative ---- */

static void test_plan_mode_negative(void)
{
    const char *content = "No dialogue here";
    dialogue_response_t resp = {0, 0};
    dialogue_type_t type = detect_blocking_dialogue(content, &resp);

    TEST_ASSERT(type == DIALOGUE_NONE,
                "expected DIALOGUE_NONE (%d), got %d",
                DIALOGUE_NONE, type);

    TEST_PASS("plan_mode negative");
}

/* ---- 3. ask_modal positive ---- */

static void test_ask_modal_positive(void)
{
    const char *content = "Type something.\n? 1. Option A\n  2. Option B";
    dialogue_response_t resp = {0, 0};
    dialogue_type_t type = detect_blocking_dialogue(content, &resp);

    TEST_ASSERT(type == DIALOGUE_ASK_MODAL,
                "expected DIALOGUE_ASK_MODAL (%d), got %d",
                DIALOGUE_ASK_MODAL, type);
    TEST_ASSERT(resp.option == 1,
                "expected option=1, got %d", resp.option);
    TEST_ASSERT(resp.settle_secs == 5,
                "expected settle=5, got %d", resp.settle_secs);

    TEST_PASS("ask_modal positive");
}

/* ---- 4. ask_modal negative (Type something only, no numbers) ---- */

static void test_ask_modal_negative(void)
{
    const char *content = "Type something.\nNo numbered options here";
    dialogue_response_t resp = {0, 0};
    dialogue_type_t type = detect_blocking_dialogue(content, &resp);

    TEST_ASSERT(type == DIALOGUE_NONE,
                "expected DIALOGUE_NONE (%d), got %d",
                DIALOGUE_NONE, type);

    TEST_PASS("ask_modal negative (no numbers)");
}

/* ---- 5. permissions positive ---- */

static void test_permissions_positive(void)
{
    const char *content = "Do you want to proceed?\n"
                          "1. Yes\n"
                          "2. Yes, and don't ask again for bash in /home\n"
                          "3. No";
    dialogue_response_t resp = {0, 0};
    dialogue_type_t type = detect_blocking_dialogue(content, &resp);

    TEST_ASSERT(type == DIALOGUE_PERMISSIONS,
                "expected DIALOGUE_PERMISSIONS (%d), got %d",
                DIALOGUE_PERMISSIONS, type);
    TEST_ASSERT(resp.option == 2,
                "expected option=2, got %d", resp.option);
    TEST_ASSERT(resp.settle_secs == 2,
                "expected settle=2, got %d", resp.settle_secs);

    TEST_PASS("permissions positive");
}

/* ---- 6. proceed positive (no "don't ask again") ---- */

static void test_proceed_positive(void)
{
    const char *content = "Do you want to proceed?\n1. Yes\n2. No";
    dialogue_response_t resp = {0, 0};
    dialogue_type_t type = detect_blocking_dialogue(content, &resp);

    TEST_ASSERT(type == DIALOGUE_PROCEED,
                "expected DIALOGUE_PROCEED (%d), got %d",
                DIALOGUE_PROCEED, type);
    TEST_ASSERT(resp.option == 1,
                "expected option=1, got %d", resp.option);
    TEST_ASSERT(resp.settle_secs == 2,
                "expected settle=2, got %d", resp.settle_secs);

    TEST_PASS("proceed positive");
}

/* ---- 7. priority: plan_mode wins over permissions ---- */

static void test_priority_plan_over_permissions(void)
{
    const char *content = "Would you like to proceed?\n"
                          "Do you want to proceed?\n"
                          "don't ask again";
    dialogue_response_t resp = {0, 0};
    dialogue_type_t type = detect_blocking_dialogue(content, &resp);

    TEST_ASSERT(type == DIALOGUE_PLAN_MODE,
                "expected DIALOGUE_PLAN_MODE (%d) to win priority, got %d",
                DIALOGUE_PLAN_MODE, type);
    TEST_ASSERT(resp.option == 2,
                "expected option=2, got %d", resp.option);
    TEST_ASSERT(resp.settle_secs == 5,
                "expected settle=5, got %d", resp.settle_secs);

    TEST_PASS("priority: plan_mode wins over permissions");
}

/* ---- 8. context_stress: each of the 4 patterns ---- */

static void test_context_stress_positive(void)
{
    const char *patterns[] = {
        "...Compacting conversation...",
        "Error: Conversation too long to continue",
        "Prompt is too long for the model",
        "Error compacting conversation: timeout"
    };
    int n = (int)(sizeof(patterns) / sizeof(patterns[0]));

    for (int i = 0; i < n; i++) {
        int result = detect_context_stress(patterns[i]);
        TEST_ASSERT(result == 1,
                    "expected stress=1 for pattern %d: \"%s\"", i, patterns[i]);
    }

    TEST_PASS("context_stress positive (all 4 patterns)");
}

/* ---- 9. context_stress negative ---- */

static void test_context_stress_negative(void)
{
    const char *content = "This is perfectly normal text with no stress indicators.";
    int result = detect_context_stress(content);

    TEST_ASSERT(result == 0,
                "expected stress=0 for normal text, got %d", result);

    TEST_PASS("context_stress negative");
}

/* ---- 10. prompt_visible positive ---- */

static void test_prompt_visible_positive(void)
{
    /* UTF-8 prompt character */
    const char *content = "some text\n\xe2\x9d\xaf \n";
    int result = detect_prompt_visible(content);

    TEST_ASSERT(result == 1,
                "expected prompt_visible=1, got %d", result);

    TEST_PASS("prompt_visible positive");
}

/* ---- 11. prompt_visible negative: prompt not in last 6 lines ---- */

static void test_prompt_visible_negative(void)
{
    const char *content = "\xe2\x9d\xaf\nmany\nlines\nafter\nmore\nstuff\nhere";
    int result = detect_prompt_visible(content);

    TEST_ASSERT(result == 0,
                "expected prompt_visible=0 (prompt too far up), got %d", result);

    TEST_PASS("prompt_visible negative");
}

/* ---- 12. skill_failure positive ---- */

static void test_skill_failure_positive(void)
{
    const char *content = "Error: Unknown skill '/nbs-notify'";
    int result = detect_skill_failure(content);

    TEST_ASSERT(result == 1,
                "expected skill_failure=1, got %d", result);

    TEST_PASS("skill_failure positive");
}

/* ---- 13. skill_failure negative ---- */

static void test_skill_failure_negative(void)
{
    const char *content = "Known skill loaded successfully";
    int result = detect_skill_failure(content);

    TEST_ASSERT(result == 0,
                "expected skill_failure=0, got %d", result);

    TEST_PASS("skill_failure negative");
}

/* ==== ADVERSARIAL TESTS — targeting audit violations ==== */

/*
 * V2 (BUG): detect_prompt_visible — pointer UB when content is all newlines.
 * The old code decremented p below content, producing undefined behaviour.
 * After fix, index-based loop must handle this without UB.
 */
static void test_prompt_visible_all_newlines(void)
{
    const char *content = "\n\n\n\n\n\n\n\n";
    int result = detect_prompt_visible(content);

    TEST_ASSERT(result == 0,
                "all-newlines: expected prompt_visible=0, got %d", result);

    TEST_PASS("prompt_visible: all newlines (V2 UB boundary)");
}

/*
 * V2 (BUG): detect_prompt_visible — single character, no newlines.
 * The backward scan should not go below index 0.
 */
static void test_prompt_visible_single_char(void)
{
    const char *content = "x";
    int result = detect_prompt_visible(content);

    TEST_ASSERT(result == 0,
                "single char: expected prompt_visible=0, got %d", result);

    TEST_PASS("prompt_visible: single char (V2 UB boundary)");
}

/*
 * V2 (BUG): detect_prompt_visible — empty string.
 * The function has a len==0 early return; verify it works.
 */
static void test_prompt_visible_empty_string(void)
{
    const char *content = "";
    int result = detect_prompt_visible(content);

    TEST_ASSERT(result == 0,
                "empty: expected prompt_visible=0, got %d", result);

    TEST_PASS("prompt_visible: empty string (V2 UB boundary)");
}

/*
 * V2 (BUG): detect_prompt_visible — fewer than 6 lines, prompt in first line.
 * Backward scan exhausts entire content without finding 6 newlines.
 */
static void test_prompt_visible_fewer_than_6_lines_with_prompt(void)
{
    const char *content = "line1\nline2\n\xe2\x9d\xaf\n";
    int result = detect_prompt_visible(content);

    TEST_ASSERT(result == 1,
                "3 lines with prompt: expected prompt_visible=1, got %d", result);

    TEST_PASS("prompt_visible: fewer than 6 lines with prompt (V2 UB boundary)");
}

/*
 * V2 (BUG): detect_prompt_visible — exactly 6 lines, prompt on first line.
 * Boundary: search_start should be set to content (beginning), prompt found.
 */
static void test_prompt_visible_exactly_6_lines_prompt_at_start(void)
{
    const char *content = "\xe2\x9d\xaf\nline2\nline3\nline4\nline5\nline6\n";
    int result = detect_prompt_visible(content);

    TEST_ASSERT(result == 1,
                "6 lines prompt at start: expected 1, got %d", result);

    TEST_PASS("prompt_visible: exactly 6 lines, prompt at start");
}

/*
 * V2 (BUG): detect_prompt_visible — trailing newlines padding (tmux panes).
 * The content ends with many trailing newlines. The skip-trailing-newlines
 * logic must handle this without UB.
 */
static void test_prompt_visible_trailing_newlines(void)
{
    /* Prompt on last real line, followed by padding newlines */
    const char *content = "some text\n\xe2\x9d\xaf\n\n\n\n\n\n\n\n\n";
    int result = detect_prompt_visible(content);

    TEST_ASSERT(result == 1,
                "trailing newlines: expected prompt_visible=1, got %d", result);

    TEST_PASS("prompt_visible: trailing newlines padding");
}

/*
 * V1 (HARDENING): detect_ask_modal — digit at end of string without
 * following '.'. Tests that *(p+1) reading NUL is handled safely.
 */
static void test_ask_modal_digit_at_end(void)
{
    const char *content = "Type something.\n1";
    dialogue_response_t resp = {0, 0};
    dialogue_type_t type = detect_blocking_dialogue(content, &resp);

    TEST_ASSERT(type == DIALOGUE_NONE,
                "digit-at-end: expected DIALOGUE_NONE, got %d", type);

    TEST_PASS("ask_modal: digit at end of string (V1 NUL boundary)");
}

/*
 * HARDENING: detect_blocking_dialogue — response is now a required parameter.
 * Passing a valid response pointer when a dialogue is detected must populate
 * both option and settle_secs. This replaces the old NULL-response test.
 */
static void test_blocking_dialogue_response_required(void)
{
    dialogue_response_t resp = {0, 0};
    dialogue_type_t type = detect_blocking_dialogue(
        "Would you like to proceed?\n1. Yes", &resp);

    TEST_ASSERT(type == DIALOGUE_PLAN_MODE,
                "response required: expected DIALOGUE_PLAN_MODE, got %d", type);
    TEST_ASSERT(resp.option == 2,
                "response required: expected option=2, got %d", resp.option);
    TEST_ASSERT(resp.settle_secs == 5,
                "response required: expected settle=5, got %d", resp.settle_secs);

    TEST_PASS("blocking_dialogue: response is required and populated");
}

/*
 * HARDENING: detect_blocking_dialogue — when no dialogue detected,
 * response fields must remain untouched (caller's values preserved).
 */
static void test_blocking_dialogue_no_detection_preserves_response(void)
{
    dialogue_response_t resp = {42, 99};
    dialogue_type_t type = detect_blocking_dialogue("no dialogue here", &resp);

    TEST_ASSERT(type == DIALOGUE_NONE,
                "no detection: expected DIALOGUE_NONE, got %d", type);
    TEST_ASSERT(resp.option == 42,
                "no detection: expected option preserved as 42, got %d", resp.option);
    TEST_ASSERT(resp.settle_secs == 99,
                "no detection: expected settle preserved as 99, got %d", resp.settle_secs);

    TEST_PASS("blocking_dialogue: no detection preserves response fields");
}

/*
 * V7 (HARDENING): detect_context_stress — empty string returns 0.
 */
static void test_context_stress_empty(void)
{
    int result = detect_context_stress("");

    TEST_ASSERT(result == 0,
                "empty: expected stress=0, got %d", result);

    TEST_PASS("context_stress: empty string");
}

/*
 * V7 (HARDENING): detect_skill_failure — empty string returns 0.
 */
static void test_skill_failure_empty(void)
{
    int result = detect_skill_failure("");

    TEST_ASSERT(result == 0,
                "empty: expected skill_failure=0, got %d", result);

    TEST_PASS("skill_failure: empty string");
}

/*
 * V5 (HARDENING): detect_blocking_dialogue — empty string.
 * No dialogue should be detected.
 */
static void test_blocking_dialogue_empty(void)
{
    dialogue_response_t resp = {0, 0};
    dialogue_type_t type = detect_blocking_dialogue("", &resp);

    TEST_ASSERT(type == DIALOGUE_NONE,
                "empty: expected DIALOGUE_NONE, got %d", type);

    TEST_PASS("blocking_dialogue: empty string");
}

/*
 * ask_modal: number 5 should NOT match (only 1-4 valid).
 */
static void test_ask_modal_number_out_of_range(void)
{
    const char *content = "Type something.\n5. Out of range";
    dialogue_response_t resp = {0, 0};
    dialogue_type_t type = detect_blocking_dialogue(content, &resp);

    TEST_ASSERT(type == DIALOGUE_NONE,
                "number 5: expected DIALOGUE_NONE, got %d", type);

    TEST_PASS("ask_modal: number 5 out of range");
}

/*
 * ask_modal: '>' prefix followed by number.
 */
static void test_ask_modal_gt_prefix(void)
{
    const char *content = "Type something.\n> 2. Option B";
    dialogue_response_t resp = {0, 0};
    dialogue_type_t type = detect_blocking_dialogue(content, &resp);

    TEST_ASSERT(type == DIALOGUE_ASK_MODAL,
                "> prefix: expected DIALOGUE_ASK_MODAL, got %d", type);

    TEST_PASS("ask_modal: > prefix");
}

/* ---- main ---- */

int main(void)
{
    printf("test_sidecar_detect_unit\n");

    /* Original tests */
    test_plan_mode_positive();
    test_plan_mode_negative();
    test_ask_modal_positive();
    test_ask_modal_negative();
    test_permissions_positive();
    test_proceed_positive();
    test_priority_plan_over_permissions();
    test_context_stress_positive();
    test_context_stress_negative();
    test_prompt_visible_positive();
    test_prompt_visible_negative();
    test_skill_failure_positive();
    test_skill_failure_negative();

    /* Adversarial tests — audit violations */
    test_prompt_visible_all_newlines();
    test_prompt_visible_single_char();
    test_prompt_visible_empty_string();
    test_prompt_visible_fewer_than_6_lines_with_prompt();
    test_prompt_visible_exactly_6_lines_prompt_at_start();
    test_prompt_visible_trailing_newlines();
    test_ask_modal_digit_at_end();
    test_blocking_dialogue_response_required();
    test_blocking_dialogue_no_detection_preserves_response();
    test_context_stress_empty();
    test_skill_failure_empty();
    test_blocking_dialogue_empty();
    test_ask_modal_number_out_of_range();
    test_ask_modal_gt_prefix();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
