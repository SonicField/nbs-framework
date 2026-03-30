/*
 * handle_styles.h — Bracket-handle to style mapping table.
 *
 * Maps reserved bracket handles (e.g. [MEDIC-WARNING], [SIDECAR-ERROR])
 * to their terminal rendering styles. Adding a new bracket handle type
 * requires only one row here and one style constant in nbs_term_attr.
 *
 * Used by: render.c, terminal.c, main.c (export), editor.c
 */

#ifndef HANDLE_STYLES_H
#define HANDLE_STYLES_H

#include "../nbs-common/nbs_term_attr.h"
#include <string.h>

typedef struct {
    const char *handle;
    const nbs_style_t *style;
} handle_style_entry_t;

/*
 * Bracket-handle to style mapping.
 * To add a new bracket handle type: add one row here.
 */
static const handle_style_entry_t HANDLE_STYLE_TABLE[] = {
    { "[MEDIC-WARNING]",  &NBS_STYLE_MEDIC_WARNING },
    { "[SIDECAR-ERROR]",  &NBS_STYLE_SIDECAR_ERROR },
};

#define HANDLE_STYLE_TABLE_SIZE \
    ((int)(sizeof(HANDLE_STYLE_TABLE) / sizeof(HANDLE_STYLE_TABLE[0])))

/*
 * handle_style_lookup — Look up the style for a bracket handle.
 *
 * Returns the style pointer if the handle matches a table entry,
 * or NULL if the handle is not a registered bracket handle.
 */
static inline const nbs_style_t *handle_style_lookup(const char *handle) {
    for (int i = 0; i < HANDLE_STYLE_TABLE_SIZE; i++) {
        if (strcmp(handle, HANDLE_STYLE_TABLE[i].handle) == 0)
            return HANDLE_STYLE_TABLE[i].style;
    }
    return NULL;
}

#endif /* HANDLE_STYLES_H */
