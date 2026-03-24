/*
 * detect.c — Content detection functions for the sidecar.
 *
 * Pure functions: string in, result out, no side effects.
 * Ported from the bash implementation in bin/nbs-claude.
 */

#include "detect.h"
#include "../nbs-common/nbs_assert.h"

#include <string.h>
#include <ctype.h>

/* ---- Individual detectors (static) ---- */

/*
 * detect_plan_mode — checks for "Would you like to proceed?"
 * Returns 1 if found, 0 otherwise.
 *
 * Precondition: content != NULL (enforced by caller).
 */
static int detect_plan_mode(const char *content)
{
    ASSERT_MSG(content != NULL, "detect_plan_mode: content is NULL");
    return strstr(content, "Would you like to proceed?") != NULL;
}

/*
 * detect_ask_modal — checks for "Type something." AND a numbered option
 * pattern matching ^\s*[?>]?\s*[1-4]\. on any line.
 * Returns 1 if both conditions met, 0 otherwise.
 *
 * Precondition: content != NULL (enforced by caller).
 */
static int detect_ask_modal(const char *content)
{
    ASSERT_MSG(content != NULL, "detect_ask_modal: content is NULL");

    if (strstr(content, "Type something.") == NULL)
        return 0;

    /* Scan each line for the numbered option pattern: ^\s*[?>]?\s*[1-4]\. */
    const char *p = content;
    while (*p != '\0') {
        const char *line_start = p;

        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t')
            p++;

        /* Optional [?>] */
        if (*p == '?' || *p == '>')
            p++;

        /* Skip whitespace after optional prefix */
        while (*p == ' ' || *p == '\t')
            p++;

        /* Check for [1-4]\. — safe: if *p is in '1'..'4' then p[1] is at
         * most NUL (loop guard ensures *p != '\0'). NUL != '.' so the
         * overall expression is false when the digit is the last byte. */
        if (*p >= '1' && *p <= '4' && p[1] == '.')
            return 1;

        /* Advance to next line */
        p = line_start;
        while (*p != '\0' && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }

    return 0;
}

/*
 * detect_permissions — checks for BOTH "Do you want to proceed?" AND
 * "don't ask again".
 * Returns 1 if both found, 0 otherwise.
 *
 * Precondition: content != NULL (enforced by caller).
 */
static int detect_permissions(const char *content)
{
    ASSERT_MSG(content != NULL, "detect_permissions: content is NULL");
    return strstr(content, "Do you want to proceed?") != NULL &&
           strstr(content, "don't ask again") != NULL;
}

/*
 * detect_proceed — checks for "Do you want to proceed?" but NOT
 * "don't ask again".
 * Returns 1 if proceed without permissions, 0 otherwise.
 *
 * Precondition: content != NULL (enforced by caller).
 */
static int detect_proceed(const char *content)
{
    ASSERT_MSG(content != NULL, "detect_proceed: content is NULL");
    return strstr(content, "Do you want to proceed?") != NULL &&
           strstr(content, "don't ask again") == NULL;
}

/* ---- Public API ---- */

dialogue_type_t detect_blocking_dialogue(const char *content,
                                          dialogue_response_t *response)
{
    ASSERT_MSG(content != NULL, "detect_blocking_dialogue: content is NULL");
    ASSERT_MSG(response != NULL, "detect_blocking_dialogue: response is NULL (required parameter)");

    dialogue_type_t type = DIALOGUE_NONE;
    int option = 0;
    int settle = 0;

    /* Priority order: plan_mode > ask_modal > permissions > proceed */
    if (detect_plan_mode(content)) {
        type = DIALOGUE_PLAN_MODE;
        option = 2;
        settle = 5;
    } else if (detect_ask_modal(content)) {
        type = DIALOGUE_ASK_MODAL;
        option = 1;
        settle = 5;
    } else if (detect_permissions(content)) {
        type = DIALOGUE_PERMISSIONS;
        option = 2;
        settle = 2;
    } else if (detect_proceed(content)) {
        type = DIALOGUE_PROCEED;
        option = 1;
        settle = 2;
    }

    if (type != DIALOGUE_NONE) {
        response->option = option;
        response->settle_secs = settle;
    }

    /* Postcondition: if dialogue detected, response fields must be set */
    if (type != DIALOGUE_NONE) {
        ASSERT_MSG(response->option > 0,
                   "detect_blocking_dialogue postcondition: option not set for type %d", (int)type);
        ASSERT_MSG(response->settle_secs > 0,
                   "detect_blocking_dialogue postcondition: settle_secs not set for type %d", (int)type);
    }

    return type;
}

int detect_context_stress(const char *content)
{
    ASSERT_MSG(content != NULL, "detect_context_stress: content is NULL");

    int result = 0;

    if (strstr(content, "Compacting conversation") != NULL) result = 1;
    else if (strstr(content, "Conversation too long") != NULL) result = 1;
    else if (strstr(content, "Prompt is too long") != NULL) result = 1;
    else if (strstr(content, "Error compacting conversation") != NULL) result = 1;

    /* Postcondition: return value is boolean */
    ASSERT_MSG(result == 0 || result == 1,
               "detect_context_stress postcondition: result %d is not 0 or 1", result);

    return result;
}

int detect_prompt_visible(const char *content)
{
    ASSERT_MSG(content != NULL, "detect_prompt_visible: content is NULL");

    /* UTF-8 sequence for the prompt character (U+276F): 0xe2 0x9d 0xaf */
    static const char prompt_utf8[] = "\xe2\x9d\xaf";

    size_t len = strlen(content);
    if (len == 0)
        return 0;

    /* Search the entire captured content (caller controls the window
     * via the capture line count — typically 30 lines). The prompt
     * character or interrupted prompt text can appear anywhere in
     * the captured window depending on how many blank/control lines
     * Claude has emitted. */

    /* Normal prompt: ❯ character */
    int result = strstr(content, prompt_utf8) != NULL;

    /* Interrupted prompt: Claude shows this after Escape */
    if (!result)
        result = strstr(content, "What should Claude do") != NULL;

    /* Postcondition: return value is boolean */
    ASSERT_MSG(result == 0 || result == 1,
               "detect_prompt_visible postcondition: result %d is not 0 or 1", result);

    return result;
}

int detect_skill_failure(const char *content)
{
    ASSERT_MSG(content != NULL, "detect_skill_failure: content is NULL");

    int result = strstr(content, "Unknown skill") != NULL;

    /* Postcondition: return value is boolean */
    ASSERT_MSG(result == 0 || result == 1,
               "detect_skill_failure postcondition: result %d is not 0 or 1", result);

    return result;
}
