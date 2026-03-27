/*
 * nbs_helper_check.h — Check if nbs-ts-helper is running.
 *
 * Shared module used by nbs-chat-terminal, nbs-ts, and nbs-chat-edit
 * to provide consistent helper-missing warnings.
 */

#ifndef NBS_HELPER_CHECK_H
#define NBS_HELPER_CHECK_H

#include <stdio.h>

/*
 * helper_is_running — Check if nbs-ts-helper socket exists.
 *
 * Returns 1 if ~/.nbs-ts/helper.sock exists (stat succeeds), 0 otherwise.
 * Returns 0 if HOME is not set.
 */
int helper_is_running(void);

/*
 * helper_warn_if_not_running — Print warning if helper is not running.
 *
 * If helper is not running, writes a warning to `out`.
 *   verbose=1: multi-line warning with explanation (for interactive terminals)
 *   verbose=0: single-line warning (for background/library use)
 *
 * Both modes include nbs-ts-sysctl install and manual nbs-ts-helper instructions.
 *
 * Returns 0 if helper is running, 1 if not (warning was printed).
 */
int helper_warn_if_not_running(FILE *out, int verbose);

#endif /* NBS_HELPER_CHECK_H */
