/*
 * transport.h — Abstraction over tmux and pty-session transports.
 *
 * The sidecar monitors Claude Code via one of two transports:
 *   - tmux: capture-pane, send-keys
 *   - pty-session: read, send
 *
 * This vtable allows the main loop to be transport-agnostic.
 *
 * Invariants:
 *   - All function pointers are non-NULL after initialisation
 *     (enforced by postcondition assertions in transport_*_init)
 *   - capture returns heap-allocated NUL-terminated string (caller frees) or NULL on error
 *     (enforced by postcondition assertion in capture implementations)
 *   - send_text sends text via the transport's exec mechanism
 *   - send_key sends a named key ("Enter", "Escape")
 *   - is_alive returns 1 if alive, 0 if gone, -1 on exec error
 *     (three-valued contract enforced by implementation)
 */

#ifndef NBS_TRANSPORT_H
#define NBS_TRANSPORT_H

#include <stddef.h>

typedef struct transport {
    /*
     * capture — Capture recent pane/session content.
     *
     * scrollback: number of lines from bottom to capture
     * Returns: heap-allocated NUL-terminated string, or NULL on error.
     * Caller must free() the returned string.
     */
    char *(*capture)(const struct transport *self, int scrollback);

    /*
     * send_text — Send literal text to the pane/session.
     *
     * text: NUL-terminated string to send (no trailing Enter)
     * Returns: 0 on success, -1 on error
     */
    int (*send_text)(const struct transport *self, const char *text);

    /*
     * send_key — Send a named key to the pane/session.
     *
     * key: key name ("Enter" or "Escape")
     * Returns: 0 on success, -1 on error
     */
    int (*send_key)(const struct transport *self, const char *key);

    /*
     * is_alive — Check if the monitored pane/session still exists.
     *
     * Returns: 1 if alive, 0 if gone, -1 on error
     */
    int (*is_alive)(const struct transport *self);

    /* Transport-specific context (opaque, allocated by init) */
    void *ctx;
} transport_t;

/*
 * transport_tmux_init — Initialise a tmux transport.
 *
 * Preconditions:
 *   - tp != NULL
 *   - pane_id != NULL, non-empty
 *
 * Postconditions:
 *   - On success (returns 0): all function pointers set, ctx allocated
 *   - On error (returns -1): tp is zeroed
 */
int transport_tmux_init(transport_t *tp, const char *pane_id);

/*
 * transport_pty_init — Initialise a pty-session transport.
 *
 * Preconditions:
 *   - tp != NULL
 *   - pty_path != NULL (path to pty-session binary)
 *   - session_name != NULL, non-empty
 *
 * Postconditions:
 *   - On success (returns 0): all function pointers set, ctx allocated
 *   - On error (returns -1): tp is zeroed
 */
int transport_pty_init(transport_t *tp, const char *pty_path,
                       const char *session_name);

/*
 * transport_ts_init — Initialise an nbs-ts transport.
 *
 * Preconditions:
 *   - tp != NULL
 *   - handle != NULL, non-empty (8-char hex session handle)
 *
 * Postconditions:
 *   - On success (returns 0): all function pointers set, ctx allocated
 *   - On error (returns -1): tp is zeroed
 */
int transport_ts_init(transport_t *tp, const char *handle);

/*
 * transport_free — Release transport resources.
 *
 * Frees ctx and zeroes function pointers. Safe to call on zeroed transport.
 */
void transport_free(transport_t *tp);

#endif /* NBS_TRANSPORT_H */
