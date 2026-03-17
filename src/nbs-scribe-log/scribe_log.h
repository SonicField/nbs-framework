/*
 * scribe_log.h — Decision log management for the NBS Scribe.
 *
 * Provides a deterministic tool for appending decision entries to
 * the scribe log file, with locking, validation, and bus event
 * publishing. Replaces the LLM-constructed heredoc approach.
 */

#ifndef NBS_SCRIBE_LOG_H
#define NBS_SCRIBE_LOG_H

#include "../nbs-common/nbs_assert.h"

#include <stddef.h>

/* Maximum sizes */
#define SCRIBE_MAX_PATH      4096
#define SCRIBE_MAX_SUMMARY   1024
#define SCRIBE_MAX_FIELD     2048
#define SCRIBE_MAX_ENTRY     8192

/* Exit codes */
#define SCRIBE_EXIT_OK        0
#define SCRIBE_EXIT_ERROR     1
#define SCRIBE_EXIT_BAD_ARGS  4

/*
 * scribe_entry_t — Fields for a single decision entry.
 *
 * Invariants:
 *   - summary is NUL-terminated, non-empty
 *   - participants is NUL-terminated, non-empty
 *   - rationale is NUL-terminated, non-empty
 *   - All other fields may be empty (defaults applied)
 *   - SECURITY: All fields must be newline-free. Each field maps to a
 *     single Markdown line; embedded newlines would allow injection of
 *     fake decision entries. Enforced by ASSERT_MSG in scribe_log_append
 *     and by check_no_newline at CLI parse time.
 */
typedef struct {
    char summary[SCRIBE_MAX_SUMMARY];
    char chat_ref[SCRIBE_MAX_FIELD];
    char participants[SCRIBE_MAX_FIELD];
    char artefacts[SCRIBE_MAX_FIELD];
    char risk_tags[SCRIBE_MAX_FIELD];
    char status[64];
    char rationale[SCRIBE_MAX_FIELD];
    char supersedes[64];
    char bus_dir[SCRIBE_MAX_PATH];
} scribe_entry_t;

/*
 * scribe_log_append — Append a decision entry to the log file.
 *
 * Preconditions:
 *   - log_path != NULL, points to a writable location
 *   - entry != NULL, with summary, participants, rationale non-empty
 *
 * Postconditions:
 *   - On success (returns 0): entry appended, bus event published,
 *     decision ID printed to stdout
 *   - On error (returns non-zero): log file may or may not have been
 *     modified; error message printed to stderr
 *
 * Acquires fcntl lock on <log_path>.lock for the duration of the
 * read-validate-append cycle.
 */
int scribe_log_append(const char *log_path, const scribe_entry_t *entry);

/*
 * scribe_log_init — Create a new log file with header if it does not exist.
 *
 * Preconditions:
 *   - log_path != NULL
 *
 * Postconditions:
 *   - If file did not exist: created with header, returns SCRIBE_EXIT_OK (0)
 *   - If file already exists: no-op, returns SCRIBE_EXIT_OK (0)
 *   - On error: returns SCRIBE_EXIT_ERROR (1)
 *
 * Note: When called from scribe_log_append, initialisation is performed
 * under the fcntl lock using an atomic O_CREAT|O_EXCL create to prevent
 * TOCTOU races. Direct callers of scribe_log_init should be aware that
 * concurrent calls are not serialised unless they arrange their own locking.
 */
int scribe_log_init(const char *log_path);

#endif /* NBS_SCRIBE_LOG_H */
