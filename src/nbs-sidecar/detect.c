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
 */
static int detect_plan_mode(const char *content)
{
    return strstr(content, "Would you like to proceed?") != NULL;
}

/*
 * detect_ask_modal — checks for "Type something." AND a numbered option
 * pattern matching ^\s*[?>]?\s*[1-4]\. on any line.
 * Returns 1 if both conditions met, 0 otherwise.
 */
static int detect_ask_modal(const char *content)
{
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

        /* Check for [1-4]\. */
        if (*p >= '1' && *p <= '4' && *(p + 1) == '.')
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
 */
static int detect_permissions(const char *content)
{
    return strstr(content, "Do you want to proceed?") != NULL &&
           strstr(content, "don't ask again") != NULL;
}

/*
 * detect_proceed — checks for "Do you want to proceed?" but NOT
 * "don't ask again".
 * Returns 1 if proceed without permissions, 0 otherwise.
 */
static int detect_proceed(const char *content)
{
    return strstr(content, "Do you want to proceed?") != NULL &&
           strstr(content, "don't ask again") == NULL;
}

/* ---- Public API ---- */

dialogue_type_t detect_blocking_dialogue(const char *content,
                                          dialogue_response_t *response)
{
    ASSERT_MSG(content != NULL, "detect_blocking_dialogue: content is NULL");

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

    if (type != DIALOGUE_NONE && response != NULL) {
        response->option = option;
        response->settle_secs = settle;
    }

    /* Postcondition: if dialogue detected, response fields must be set */
    if (type != DIALOGUE_NONE && response != NULL) {
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

    if (strstr(content, "Compacting conversation") != NULL) return 1;
    if (strstr(content, "Conversation too long") != NULL) return 1;
    if (strstr(content, "Prompt is too long") != NULL) return 1;
    if (strstr(content, "Error compacting conversation") != NULL) return 1;
    return 0;
}

int detect_prompt_visible(const char *content)
{
    ASSERT_MSG(content != NULL, "detect_prompt_visible: content is NULL");

    /* UTF-8 sequence for ❯: 0xe2 0x9d 0xaf */
    static const char prompt_utf8[] = { (char)0xe2, (char)0x9d, (char)0xaf, '\0' };

    size_t len = strlen(content);
    if (len == 0)
        return 0;

    /*
     * Find the start of the last 6 lines.
     * Walk backwards from end, counting newlines.
     * 6 lines covers: prompt line + separator + bypass permissions +
     * blank line + context percentage + possible extra status line.
     */
    const char *search_start = content;
    int newlines_found = 0;
    const char *p = content + len - 1;

    /* Skip trailing newlines (tmux pads pane to full height with empty lines) */
    while (p >= content && *p == '\n')
        p--;

    while (p >= content) {
        if (*p == '\n') {
            newlines_found++;
            if (newlines_found == 6) {
                search_start = p + 1;
                break;
            }
        }
        p--;
    }

    ASSERT_MSG(search_start >= content && search_start <= content + len,
               "detect_prompt_visible: search_start out of bounds");

    return strstr(search_start, prompt_utf8) != NULL;
}

int detect_skill_failure(const char *content)
{
    ASSERT_MSG(content != NULL, "detect_skill_failure: content is NULL");

    return strstr(content, "Unknown skill") != NULL;
}
