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
    /* ❯ is UTF-8: 0xe2 0x9d 0xaf */
    const char *content = "some text\n\xe2\x9d\xaf \n";
    int result = detect_prompt_visible(content);

    TEST_ASSERT(result == 1,
                "expected prompt_visible=1, got %d", result);

    TEST_PASS("prompt_visible positive");
}

/* ---- 11. prompt_visible negative: ❯ not in last 3 lines ---- */

static void test_prompt_visible_negative(void)
{
    const char *content = "\xe2\x9d\xaf\nmany\nlines\nafter";
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

/* ---- main ---- */

int main(void)
{
    printf("test_sidecar_detect_unit\n");

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

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
