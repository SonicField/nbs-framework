/*
 * detect.h — Content detection functions for the sidecar.
 *
 * All functions are pure: they take a string, return a result,
 * and have no side effects. Highly testable.
 */

#ifndef NBS_DETECT_H
#define NBS_DETECT_H

/* Dialogue types */
typedef enum {
    DIALOGUE_NONE = 0,
    DIALOGUE_PLAN_MODE,
    DIALOGUE_ASK_MODAL,
    DIALOGUE_PERMISSIONS,
    DIALOGUE_PROCEED
} dialogue_type_t;

/* Dialogue response */
typedef struct {
    int option;       /* Menu option number to send (e.g. 1, 2) */
    int settle_secs;  /* Seconds to wait after responding */
} dialogue_response_t;

/*
 * detect_blocking_dialogue — Check pane content for blocking dialogues.
 *
 * Checked in priority order: plan_mode > ask_modal > permissions > proceed.
 *
 * Preconditions:
 *   - content != NULL
 *   - response != NULL (required parameter)
 *
 * Postconditions:
 *   - Returns DIALOGUE_NONE if no dialogue detected
 *   - If return != DIALOGUE_NONE, *response is populated
 */
dialogue_type_t detect_blocking_dialogue(const char *content,
                                          dialogue_response_t *response);

/*
 * detect_context_stress — Check for context compaction/overflow indicators.
 *
 * Patterns: "Compacting conversation", "Conversation too long",
 *           "Prompt is too long", "Error compacting conversation"
 *
 * Preconditions:
 *   - content != NULL
 *
 * Returns 1 if any stress indicator found, 0 otherwise.
 */
int detect_context_stress(const char *content);

/*
 * detect_prompt_idle — Is Claude idle at its normal prompt?
 *
 * Checks for the ❯ character anywhere in the captured content.
 * Used by notification injection and its verification checks.
 *
 * Returns 1 if idle at prompt, 0 otherwise.
 */
int detect_prompt_idle(const char *content);

/*
 * detect_prompt_ready — Is Claude ready for input after interruption?
 *
 * Checks for ❯ OR the interrupted prompt ("What should Claude do").
 * Used only by the interrupt handler after sending Escape.
 *
 * Returns 1 if ready for input, 0 otherwise.
 */
int detect_prompt_ready(const char *content);

/*
 * detect_prompt_not_trust — Is Claude at its real prompt, not the trust dialog?
 *
 * Checks for ❯ AND absence of "trust this folder".
 * Used only by init-wait to avoid injecting into the trust dialog.
 *
 * Returns 1 if at real prompt, 0 otherwise.
 */
int detect_prompt_not_trust(const char *content);

/*
 * detect_skill_failure — Check if Claude rejected an injection.
 *
 * Looks for "Unknown skill" in content.
 *
 * Preconditions:
 *   - content != NULL
 *
 * Returns 1 if found, 0 otherwise.
 */
int detect_skill_failure(const char *content);

#endif /* NBS_DETECT_H */
