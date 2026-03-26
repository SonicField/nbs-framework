/*
 * nbs_ts_bidi.h — Unicode Bidirectional Algorithm (UAX #9).
 *
 * Reorders a line of Unicode codepoints from logical to visual order.
 * Full UAX #9 implementation: character type resolution, embedding levels,
 * weak type resolution, neutral type resolution, implicit levels, reordering.
 *
 * No external dependencies. Bundled character type table.
 */

#ifndef NBS_TS_BIDI_H
#define NBS_TS_BIDI_H

#include <stdint.h>
#include <stddef.h>

/* UAX #9 Bidi character types */
typedef enum {
    /* Strong types */
    BIDI_L   = 0,   /* Left-to-Right */
    BIDI_R   = 1,   /* Right-to-Left */
    BIDI_AL  = 2,   /* Arabic Letter */

    /* Weak types */
    BIDI_EN  = 3,   /* European Number */
    BIDI_ES  = 4,   /* European Separator */
    BIDI_ET  = 5,   /* European Terminator */
    BIDI_AN  = 6,   /* Arabic Number */
    BIDI_CS  = 7,   /* Common Separator */
    BIDI_NSM = 8,   /* Non-Spacing Mark */
    BIDI_BN  = 9,   /* Boundary Neutral */

    /* Neutral types */
    BIDI_B   = 10,  /* Paragraph Separator */
    BIDI_S   = 11,  /* Segment Separator */
    BIDI_WS  = 12,  /* Whitespace */
    BIDI_ON  = 13,  /* Other Neutral */

    /* Explicit formatting types */
    BIDI_LRE = 14,  /* Left-to-Right Embedding */
    BIDI_RLE = 15,  /* Right-to-Left Embedding */
    BIDI_LRO = 16,  /* Left-to-Right Override */
    BIDI_RLO = 17,  /* Right-to-Left Override */
    BIDI_PDF = 18,  /* Pop Directional Format */
    BIDI_LRI = 19,  /* Left-to-Right Isolate */
    BIDI_RLI = 20,  /* Right-to-Left Isolate */
    BIDI_FSI = 21,  /* First Strong Isolate */
    BIDI_PDI = 22,  /* Pop Directional Isolate */
} bidi_type_t;

/*
 * Look up the bidi character type for a Unicode codepoint.
 */
bidi_type_t nbs_ts_bidi_type(uint32_t cp);

/*
 * Reorder a line of codepoints from logical to visual order.
 *
 * codepoints: array of Unicode codepoints (logical order)
 * count:      number of codepoints
 * visual_map: output array of indices (visual_map[visual_pos] = logical_pos)
 *             Caller allocates, must have room for `count` entries.
 * base_dir:   0 = auto-detect, 1 = force LTR, 2 = force RTL
 *
 * Returns the resolved paragraph direction (0=LTR, 1=RTL).
 */
int nbs_ts_bidi_reorder(const uint32_t *codepoints, int count,
                        int *visual_map, int base_dir);

/*
 * Return the bidi mirrored glyph for a codepoint, or the codepoint
 * itself if no mirror exists. Used for brackets in RTL context (UAX #9 L4).
 */
uint32_t nbs_ts_bidi_mirror(uint32_t cp);

/*
 * Get the resolved embedding level for a position after reorder.
 * Must be called after nbs_ts_bidi_reorder. Returns levels via output array.
 */
int nbs_ts_bidi_reorder_with_levels(const uint32_t *codepoints, int count,
                                     int *visual_map, int *out_levels,
                                     int base_dir);

#endif /* NBS_TS_BIDI_H */
