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
 * chat_client_send — Send a message via fork+exec of nbs-chat send.
 *
 * Returns: 0 on success, -1 on failure
 */
int chat_client_send(const char *chat_path, const char *handle,
                      const char *message);

#endif /* NBS_CHAT_CLIENT_H */
