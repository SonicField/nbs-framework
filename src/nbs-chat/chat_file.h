/*
 * chat_file.h — Chat file protocol for nbs-chat
 *
 * File format:
 *   === nbs-chat ===
 *   last-writer: <handle>
 *   last-write: <ISO 8601 timestamp>
 *   file-length: <byte count>
 *   participants: <handle1>(N1), <handle2>(N2), ...
 *   ---
 *   <base64 encoded message 1>
 *   <base64 encoded message 2>
 *   ...
 *
 * Each message decodes to: "handle: message text"
 */

#ifndef NBS_CHAT_FILE_H
#define NBS_CHAT_FILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ASSERT_MSG — shared across NBS components */
#include "../nbs-common/nbs_assert.h"

/* Maximum sizes */
#define MAX_HANDLE_LEN 64
#define MAX_MESSAGE_LEN (1024 * 1024)  /* 1 MB per message */
#define MAX_MESSAGES 10000
#define MAX_PARTICIPANTS 256
#define MAX_PATH_LEN 4096

/* Decoded message
 *
 * Invariants:
 *   - handle is always NUL-terminated, length < MAX_HANDLE_LEN
 *   - content is heap-allocated (via strdup) and must be freed by caller
 *   - content_len == strlen(content) when content is valid text
 *   - timestamp is 0 for legacy messages without embedded timestamps,
 *     or a positive epoch-seconds value for timestamped messages
 */
typedef struct {
    char handle[MAX_HANDLE_LEN];
    char *content;     /* Dynamically allocated */
    size_t content_len;
    time_t timestamp;  /* Epoch seconds, 0 = no timestamp (legacy message) */
} chat_message_t;

/* Participant info
 *
 * Invariants:
 *   - handle is always NUL-terminated, length < MAX_HANDLE_LEN
 *   - count >= 0 (number of messages sent by this participant)
 *
 * Note: count is signed int rather than unsigned because:
 *   (a) it participates in arithmetic with signed values (e.g. snprintf return)
 *   (b) the invariant count >= 0 is enforced by chat_state_check_invariants()
 *   (c) signed overflow is UB and thus detectable by sanitisers
 */
typedef struct {
    char handle[MAX_HANDLE_LEN];
    int count;
} participant_t;

/* Chat file state
 *
 * Invariants:
 *   - message_count >= 0 && message_count <= MAX_MESSAGES
 *   - participant_count >= 0 && participant_count <= MAX_PARTICIPANTS
 *   - messages != NULL when message_count > 0
 *   - messages is heap-allocated and must be freed via chat_state_free()
 *   - file_length >= 0 when read from a valid chat file
 *
 * Note: message_count and participant_count are signed int rather than
 * unsigned because:
 *   (a) they participate in arithmetic with signed values (loop indices,
 *       comparisons with snprintf return values, keep_count in truncate)
 *   (b) the invariants >= 0 are enforced by chat_state_check_invariants()
 *   (c) signed overflow is UB and thus detectable by sanitisers
 *
 * Use chat_state_check_invariants() to validate these invariants.
 */
typedef struct {
    char last_writer[MAX_HANDLE_LEN];
    char last_write[64];  /* ISO 8601 timestamp */
    int64_t file_length;
    participant_t participants[MAX_PARTICIPANTS];
    int participant_count;
    chat_message_t *messages;
    int message_count;
    int skipped_count;  /* Messages skipped due to decode/alloc failure */
} chat_state_t;

/*
 * chat_state_check_invariants — Validate struct invariants without aborting.
 *
 * Preconditions:
 *   - state != NULL
 *
 * Postconditions:
 *   - Returns 1 if all invariants hold, 0 if any are violated
 *   - Does NOT abort on violation (unlike ASSERT_MSG)
 *   - Logs which invariant failed to stderr
 *
 * Checks:
 *   - message_count in [0, MAX_MESSAGES]
 *   - participant_count in [0, MAX_PARTICIPANTS]
 *   - messages != NULL when message_count > 0
 *   - participant counts >= 0
 */
int chat_state_check_invariants(const chat_state_t *state);

/*
 * chat_create — Create a new empty chat file.
 *
 * Preconditions:
 *   - path != NULL
 *   - path is a writable filesystem location
 *
 * Postconditions:
 *   - On success (returns 0): file exists and file-length header matches actual size
 *   - On -1: file already existed, no modification made
 *   - On -2: I/O error during creation
 *
 * Returns 0 on success, -1 if file already exists, -2 on I/O error.
 */
int chat_create(const char *path);

/*
 * chat_read — Read and parse a chat file.
 *
 * Preconditions:
 *   - path != NULL
 *   - state != NULL
 *
 * Postconditions:
 *   - On success (returns 0): state is fully initialised with header fields
 *     and decoded messages; state->message_count is in [0, MAX_MESSAGES]
 *   - On error (returns -1): state contents are undefined; caller should
 *     not access state fields
 *
 * Caller must call chat_state_free() on the result after successful return.
 * Returns 0 on success, -1 on error.
 */
int chat_read(const char *path, chat_state_t *state);

/*
 * chat_send — Append a message to a chat file.
 *
 * Preconditions:
 *   - path != NULL (must refer to an existing chat file)
 *   - handle != NULL (non-empty sender handle)
 *   - message != NULL
 *
 * Postconditions:
 *   - On success (returns 0): message is appended, headers updated,
 *     file-length header matches actual file size, message_count
 *     increased by exactly 1 relative to pre-call state, and the
 *     sender's read cursor is updated to the new message index
 *   - On -1: I/O or lock error; the original file is unchanged (writes
 *     go to a .tmp file which is only renamed on success)
 *   - On -2: fclose flush error; .tmp file was removed, original unchanged
 *
 * Acquires lock, reads file, appends message, updates headers, writes back.
 * Returns 0 on success, -1 on error, -2 on I/O flush error.
 */
int chat_send(const char *path, const char *handle, const char *message);

/*
 * chat_truncate — Remove messages beyond keep_count, rewrite atomically.
 *
 * Preconditions:
 *   - path != NULL (must refer to an existing chat file)
 *   - keep_count >= 0
 *
 * Postconditions:
 *   - On success (returns 0): file contains at most keep_count messages.
 *     Header fields (last-writer, last-write, file-length, participants)
 *     are recomputed from the remaining messages. File permissions preserved.
 *   - If keep_count >= message_count: no-op, returns 0.
 *   - On -1 (error): the original file is unchanged (writes go to a
 *     .tmp file which is only renamed on success). Falsifiable: read
 *     the file after -1 return and verify message_count is unchanged.
 *
 * Acquires lock, reads file, keeps first keep_count messages, rewrites.
 * Returns 0 on success, -1 on error.
 */
int chat_truncate(const char *path, int keep_count);

/*
 * chat_poll — Wait for a new message not from the given handle.
 *
 * Preconditions:
 *   - path != NULL
 *   - handle != NULL
 *   - timeout_secs >= 0 (zero means return immediately after one check)
 *
 * Postconditions:
 *   - Returns 0: at least one new message from a different handle exists
 *   - Returns 3: timeout_secs elapsed with no new messages from others
 *   - Returns -1: error reading the chat file
 *
 * Returns 0 if a new message arrived, 3 on timeout, -1 on error.
 */
int chat_poll(const char *path, const char *handle, int timeout_secs);

/*
 * chat_state_free — Release memory held by chat_state_t.
 *
 * Preconditions:
 *   - state may be NULL (no-op in that case)
 *
 * Postconditions:
 *   - state->messages is NULL and state->message_count is 0
 *   - All dynamically allocated message content has been freed
 */
void chat_state_free(chat_state_t *state);

/*
 * chat_cursor_read — Get the read cursor for a handle.
 *
 * Preconditions:
 *   - chat_path != NULL
 *   - handle != NULL
 *
 * Postconditions:
 *   - Returns >= 0: the last-read message index for the given handle
 *   - Returns -1: no cursor exists for this handle, or cursor file
 *     does not exist (benign: start reading from the beginning)
 *   - Returns -2: cursor entry exists but value failed to parse
 *     (indicates file corruption; callers should treat as -1 but
 *     may choose to log a warning)
 *
 * The cursor file is <chat_path>.cursors.
 */
int chat_cursor_read(const char *chat_path, const char *handle);

/*
 * chat_cursor_write — Set the read cursor for a handle.
 *
 * Preconditions:
 *   - chat_path != NULL
 *   - handle != NULL
 *   - index >= 0
 *
 * Postconditions:
 *   - On success (returns 0): the cursor file contains the updated index
 *     for the given handle; other handles' cursors are preserved
 *   - On error (returns -1): cursor file may not have been updated
 *
 * Updates (or creates) the cursor entry for the given handle.
 * The cursor file is <chat_path>.cursors.
 * Returns 0 on success, -1 on error.
 */
int chat_cursor_write(const char *chat_path, const char *handle, int index);

#endif /* NBS_CHAT_FILE_H */
