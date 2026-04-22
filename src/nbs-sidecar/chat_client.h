/*
 * chat_client.h — Chat file operations for the sidecar.
 *
 * Direct file I/O for cursor reading and message counting.
 * Uses nbs-chat send via fork+exec for posting.
 */

#ifndef NBS_CHAT_CLIENT_H
#define NBS_CHAT_CLIENT_H

#include <stddef.h>

/*
 * chat_client_count_messages — Count messages in a chat file.
 *
 * Reads the chat file directly, counts non-empty lines after "---" separator.
 * Returns message count (>= 0), or -1 on error.
 */
int chat_client_count_messages(const char *chat_path);

/*
 * chat_client_read_cursor — Read cursor position for a handle.
 *
 * Reads <chat_path>.cursors file directly (no lock, read-only).
 * Returns cursor value (>= 0), or -1 if no cursor exists.
 */
int chat_client_read_cursor(const char *chat_path, const char *handle);

/*
 * chat_client_check_unread — Count unread messages across registered chats.
 *
 * registry_path: path to control-registry file
 * handle: sidecar handle for cursor lookup
 * unread_count: out — total unread messages
 * summary: out — human-readable summary
 * Returns: 0 = unread found, 1 = caught up, 2 = no chats, -1 = error
 */
int chat_client_check_unread(const char *registry_path, const char *handle,
                              int *unread_count, char *summary, size_t sum_size);

/*
 * chat_client_are_unread_sidecar_only — Check if ALL unread messages
 * are from "sidecar".
 *
 * Decodes base64 message lines and checks handle prefix.
 * Returns: 1 = all unread are sidecar-only, 0 = otherwise or error
 */
int chat_client_are_unread_sidecar_only(const char *registry_path,
                                         const char *handle);

/*
 * chat_client_count_unread_others — Count unread messages in a chat
 * file, excluding messages whose sender handle matches my_handle.
 *
 * Used by the sidecar so it does not notify an agent about its own
 * posts. Decodes base64 message lines and inspects the handle prefix.
 *
 * chat_path: path to the chat file
 * my_handle: handle whose own messages should NOT be counted
 * cursor:    last-read 0-indexed message position; -1 means "no cursor"
 *            (treat all messages as unread). Messages with index > cursor
 *            are considered unread.
 *
 * Returns: count of unread messages from other senders (>= 0),
 *          or -1 if the chat file cannot be opened.
 *
 * Skips empty lines, space-padded repair artefacts, and base64 lines
 * with length not a multiple of 4 (matching chat_send / sidecar_only_cb).
 * Messages whose handle cannot be parsed are conservatively counted as
 * "other" (so a corrupt line doesn't silence a notification).
 */
int chat_client_count_unread_others(const char *chat_path,
                                     const char *my_handle, int cursor);

/*
 * chat_client_send — Send a message via fork+exec of nbs-chat send.
 *
 * Returns: 0 on success, -1 on failure
 */
int chat_client_send(const char *chat_path, const char *handle,
                      const char *message);

/*
 * chat_client_error — Post a [SIDECAR-ERROR] message via fork+exec
 * of nbs-chat error.
 *
 * Returns: 0 on success, -1 on failure
 */
int chat_client_error(const char *chat_path, const char *message);

#endif /* NBS_CHAT_CLIENT_H */
