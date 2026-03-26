/*
 * nbs_ts_bidi.c — Unicode Bidirectional Algorithm (UAX #9).
 *
 * Full implementation covering:
 *   - Character type lookup (bundled table)
 *   - Paragraph level determination (P2-P3)
 *   - Explicit embeddings/overrides/isolates (X1-X8)
 *   - Weak type resolution (W1-W7)
 *   - Neutral type resolution (N1-N2)
 *   - Implicit level resolution (I1-I2)
 *   - Reordering (L2)
 *
 * Reference: Unicode Standard Annex #9, Revision 46
 */

#include "nbs_ts_bidi.h"
#include <string.h>
#include <stdlib.h>

/* ── Bidi character type table ────────────────────────────────────── */

struct bidi_range {
    uint32_t lo;
    uint32_t hi;
    bidi_type_t type;
};

/*
 * Ranges for non-default bidi types. Characters not listed default to BIDI_L.
 * Sorted by lo for binary search.
 */
static const struct bidi_range bidi_ranges[] = {
    {0x0000, 0x0008, BIDI_BN}, 
    {0x000A, 0x000A, BIDI_B}, 
    {0x000B, 0x000B, BIDI_S}, 
    {0x000C, 0x000C, BIDI_WS}, 
    {0x000D, 0x000D, BIDI_B}, 
    {0x000E, 0x001B, BIDI_BN}, 
    {0x001C, 0x001E, BIDI_B}, 
    {0x001F, 0x001F, BIDI_S}, 
    {0x0020, 0x0020, BIDI_WS}, 
    {0x0021, 0x0022, BIDI_ON}, 
    {0x0023, 0x0025, BIDI_ET}, 
    {0x0026, 0x002A, BIDI_ON}, 
    {0x002B, 0x002B, BIDI_ES}, 
    {0x002C, 0x002C, BIDI_CS}, 
    {0x002D, 0x002D, BIDI_ES}, 
    {0x002E, 0x002F, BIDI_CS}, 
    {0x0030, 0x0039, BIDI_EN}, 
    {0x003A, 0x003A, BIDI_CS}, 
    {0x003B, 0x0040, BIDI_ON}, 
    {0x005B, 0x0060, BIDI_ON}, 
    {0x007B, 0x007E, BIDI_ON}, 
    {0x007F, 0x0084, BIDI_BN}, 
    {0x0085, 0x0085, BIDI_B}, 
    {0x0086, 0x009F, BIDI_BN}, 
    {0x00A0, 0x00A0, BIDI_CS}, 
    {0x00A1, 0x00A1, BIDI_ON}, 
    {0x00A2, 0x00A5, BIDI_ET}, 
    {0x00A6, 0x00A9, BIDI_ON}, 
    {0x00AB, 0x00AB, BIDI_ON}, 
    {0x00AD, 0x00AD, BIDI_BN}, 
    {0x00B0, 0x00B1, BIDI_ET}, 
    {0x00B2, 0x00B3, BIDI_EN}, 
    {0x00B4, 0x00B4, BIDI_ON}, 
    {0x00B6, 0x00B8, BIDI_ON}, 
    {0x00B9, 0x00B9, BIDI_EN}, 
    {0x00BB, 0x00BF, BIDI_ON}, 
    {0x00D7, 0x00D7, BIDI_ON}, 
    {0x00F7, 0x00F7, BIDI_ON}, 
    {0x0300, 0x036F, BIDI_NSM}, 
    {0x0483, 0x0489, BIDI_NSM}, 
    {0x0590, 0x0590, BIDI_R}, 
    {0x0591, 0x05BD, BIDI_NSM}, 
    {0x05BE, 0x05BE, BIDI_R}, 
    {0x05BF, 0x05BF, BIDI_NSM}, 
    {0x05C0, 0x05C0, BIDI_R}, 
    {0x05C1, 0x05C2, BIDI_NSM}, 
    {0x05C3, 0x05C3, BIDI_R}, 
    {0x05C4, 0x05C5, BIDI_NSM}, 
    {0x05C6, 0x05C6, BIDI_R}, 
    {0x05C7, 0x05C7, BIDI_NSM}, 
    {0x05D0, 0x05EA, BIDI_R}, 
    {0x05EF, 0x05F4, BIDI_R}, 
    {0x0600, 0x0605, BIDI_AN}, 
    {0x0608, 0x0608, BIDI_AL}, 
    {0x0609, 0x060A, BIDI_ET}, 
    {0x060B, 0x060B, BIDI_AL}, 
    {0x060C, 0x060C, BIDI_CS}, 
    {0x060D, 0x060D, BIDI_AL}, 
    {0x0610, 0x061A, BIDI_NSM}, 
    {0x061B, 0x061B, BIDI_AL}, 
    {0x061C, 0x061C, BIDI_BN}, 
    {0x061D, 0x064A, BIDI_AL}, 
    {0x064B, 0x065F, BIDI_NSM}, 
    {0x0660, 0x0669, BIDI_AN}, 
    {0x066A, 0x066A, BIDI_ET}, 
    {0x066B, 0x066C, BIDI_AN}, 
    {0x066D, 0x066F, BIDI_AL}, 
    {0x0670, 0x0670, BIDI_NSM}, 
    {0x0671, 0x06D5, BIDI_AL}, 
    {0x06D6, 0x06DC, BIDI_NSM}, 
    {0x06DD, 0x06DD, BIDI_AN}, 
    {0x06DE, 0x06DE, BIDI_ON}, 
    {0x06DF, 0x06E4, BIDI_NSM}, 
    {0x06E5, 0x06E6, BIDI_AL}, 
    {0x06E7, 0x06E8, BIDI_NSM}, 
    {0x06E9, 0x06E9, BIDI_ON}, 
    {0x06EA, 0x06ED, BIDI_NSM}, 
    {0x06EE, 0x06EF, BIDI_AL}, 
    {0x06F0, 0x06F9, BIDI_EN}, 
    {0x06FA, 0x070D, BIDI_AL}, 
    {0x070F, 0x0710, BIDI_AL}, 
    {0x0711, 0x0711, BIDI_NSM}, 
    {0x0712, 0x072F, BIDI_AL}, 
    {0x0730, 0x074A, BIDI_NSM}, 
    {0x074D, 0x07A5, BIDI_AL}, 
    {0x07A6, 0x07B0, BIDI_NSM}, 
    {0x07B1, 0x07B1, BIDI_AL}, 
    {0x07C0, 0x07EA, BIDI_R}, 
    {0x07EB, 0x07F3, BIDI_NSM}, 
    {0x07F4, 0x07F5, BIDI_R}, 
    {0x07FA, 0x07FA, BIDI_R}, 
    {0x07FD, 0x07FD, BIDI_NSM}, 
    {0x07FE, 0x07FF, BIDI_R}, 
    {0x0800, 0x0815, BIDI_R}, 
    {0x0816, 0x0819, BIDI_NSM}, 
    {0x081A, 0x081A, BIDI_R}, 
    {0x081B, 0x0823, BIDI_NSM}, 
    {0x0824, 0x0824, BIDI_R}, 
    {0x0825, 0x0827, BIDI_NSM}, 
    {0x0828, 0x0828, BIDI_R}, 
    {0x0829, 0x082D, BIDI_NSM}, 
    {0x0830, 0x083E, BIDI_R}, 
    {0x0840, 0x0858, BIDI_R}, 
    {0x0859, 0x085B, BIDI_NSM}, 
    {0x085E, 0x085E, BIDI_R}, 
    {0x0860, 0x086A, BIDI_AL}, 
    {0x0870, 0x088E, BIDI_AL}, 
    {0x0890, 0x0891, BIDI_AN}, 
    {0x0898, 0x089F, BIDI_NSM}, 
    {0x08A0, 0x08C9, BIDI_AL}, 
    {0x08CA, 0x08E1, BIDI_NSM}, 
    {0x08E2, 0x08E2, BIDI_AN}, 
    {0x08E3, 0x0902, BIDI_NSM}, 
    {0x093A, 0x093A, BIDI_NSM}, 
    {0x093C, 0x093C, BIDI_NSM}, 
    {0x0941, 0x0948, BIDI_NSM}, 
    {0x094D, 0x094D, BIDI_NSM}, 
    {0x0951, 0x0957, BIDI_NSM}, 
    {0x0962, 0x0963, BIDI_NSM}, 
    {0x0E31, 0x0E31, BIDI_NSM}, 
    {0x0E34, 0x0E3A, BIDI_NSM}, 
    {0x0E47, 0x0E4E, BIDI_NSM}, 
    {0x0EB1, 0x0EB1, BIDI_NSM}, 
    {0x0EB4, 0x0EBC, BIDI_NSM}, 
    {0x0EC8, 0x0ECE, BIDI_NSM}, 
    {0x1AB0, 0x1ACE, BIDI_NSM}, 
    {0x1DC0, 0x1DFF, BIDI_NSM}, 
    {0x2000, 0x200A, BIDI_WS}, 
    {0x200B, 0x200D, BIDI_BN}, 
    {0x200E, 0x200E, BIDI_L}, /* LRM */
    {0x200F, 0x200F, BIDI_R}, /* RLM */
    {0x2010, 0x2027, BIDI_ON}, 
    {0x2028, 0x2028, BIDI_WS}, /* Line separator */
    {0x2029, 0x2029, BIDI_B}, /* Paragraph separator */
    {0x202A, 0x202A, BIDI_LRE}, 
    {0x202B, 0x202B, BIDI_RLE}, 
    {0x202C, 0x202C, BIDI_PDF}, 
    {0x202D, 0x202D, BIDI_LRO}, 
    {0x202E, 0x202E, BIDI_RLO}, 
    {0x202F, 0x202F, BIDI_CS}, 
    {0x2030, 0x2034, BIDI_ET}, 
    {0x2035, 0x2043, BIDI_ON}, 
    {0x2044, 0x2044, BIDI_CS}, 
    {0x2045, 0x205E, BIDI_ON}, 
    {0x205F, 0x205F, BIDI_WS}, 
    {0x2060, 0x2064, BIDI_BN}, 
    {0x2066, 0x2066, BIDI_LRI}, 
    {0x2067, 0x2067, BIDI_RLI}, 
    {0x2068, 0x2068, BIDI_FSI}, 
    {0x2069, 0x2069, BIDI_PDI}, 
    {0x206A, 0x206F, BIDI_BN}, 
    {0x2070, 0x2070, BIDI_EN}, 
    {0x2074, 0x2079, BIDI_EN}, 
    {0x2080, 0x2089, BIDI_EN}, 
    {0x20A0, 0x20CF, BIDI_ET}, 
    {0x20D0, 0x20F0, BIDI_NSM}, 
    {0x2100, 0x2211, BIDI_ON}, 
    {0x2212, 0x2212, BIDI_ES}, 
    {0x2213, 0x2213, BIDI_ET}, 
    {0x2214, 0x2335, BIDI_ON}, 
    {0x2336, 0x27FF, BIDI_ON}, 
    {0x2800, 0x28FF, BIDI_L}, 
    {0x2900, 0x2BFF, BIDI_ON}, 
    {0x3000, 0x3000, BIDI_WS}, 
    {0x3001, 0x3003, BIDI_ON}, 
    {0x3008, 0x3011, BIDI_ON}, 
    {0x3014, 0x301F, BIDI_ON}, 
    {0x3030, 0x3030, BIDI_ON}, 
    {0xFB1D, 0xFB1D, BIDI_R}, 
    {0xFB1E, 0xFB1E, BIDI_NSM}, 
    {0xFB1F, 0xFB28, BIDI_R}, 
    {0xFB29, 0xFB29, BIDI_ES}, 
    {0xFB2A, 0xFB4F, BIDI_R}, 
    {0xFB50, 0xFD3D, BIDI_AL}, 
    {0xFD3E, 0xFD3F, BIDI_ON}, 
    {0xFD40, 0xFDCF, BIDI_AL}, 
    {0xFDF0, 0xFDFC, BIDI_AL}, 
    {0xFDFD, 0xFDFF, BIDI_ON}, 
    {0xFE00, 0xFE0F, BIDI_NSM}, 
    {0xFE10, 0xFE19, BIDI_ON}, 
    {0xFE20, 0xFE2F, BIDI_NSM}, 
    {0xFE30, 0xFE4F, BIDI_ON}, 
    {0xFE50, 0xFE50, BIDI_CS}, 
    {0xFE51, 0xFE51, BIDI_ON}, 
    {0xFE52, 0xFE52, BIDI_CS}, 
    {0xFE54, 0xFE54, BIDI_ON}, 
    {0xFE55, 0xFE55, BIDI_CS}, 
    {0xFE56, 0xFE5E, BIDI_ON}, 
    {0xFE5F, 0xFE5F, BIDI_ET}, 
    {0xFE60, 0xFE61, BIDI_ON}, 
    {0xFE62, 0xFE63, BIDI_ES}, 
    {0xFE64, 0xFE66, BIDI_ON}, 
    {0xFE68, 0xFE68, BIDI_ON}, 
    {0xFE69, 0xFE6A, BIDI_ET}, 
    {0xFE6B, 0xFE6B, BIDI_ON}, 
    {0xFE70, 0xFEFC, BIDI_AL}, 
    {0xFEFF, 0xFEFF, BIDI_BN}, 
    {0xFF01, 0xFF02, BIDI_ON}, 
    {0xFF03, 0xFF05, BIDI_ET}, 
    {0xFF06, 0xFF0A, BIDI_ON}, 
    {0xFF0B, 0xFF0B, BIDI_ES}, 
    {0xFF0C, 0xFF0C, BIDI_CS}, 
    {0xFF0D, 0xFF0D, BIDI_ES}, 
    {0xFF0E, 0xFF0F, BIDI_CS}, 
    {0xFF10, 0xFF19, BIDI_EN}, 
    {0xFF1A, 0xFF1A, BIDI_CS}, 
    {0xFF1B, 0xFF20, BIDI_ON}, 
    {0xFF3B, 0xFF40, BIDI_ON}, 
    {0xFF5B, 0xFF65, BIDI_ON}, 
    {0xFFF0, 0xFFF8, BIDI_BN}, 
    {0xFFF9, 0xFFFB, BIDI_ON}, 
    {0xFFFC, 0xFFFC, BIDI_ON}, 
    {0x10800, 0x10FFF, BIDI_R}, /* Various RTL scripts */
    {0x1E800, 0x1EDFF, BIDI_R}, /* More RTL scripts */
    {0x1EE00, 0x1EEFF, BIDI_AL}, /* Arabic Mathematical */
    {0x1EF00, 0x1EFFF, BIDI_R}, 
    {0xE0001, 0xE0001, BIDI_BN}, 
    {0xE0020, 0xE007F, BIDI_BN}, 
    {0xE0100, 0xE01EF, BIDI_NSM}, 
};

#define BIDI_RANGES_COUNT ((int)(sizeof(bidi_ranges) / sizeof(bidi_ranges[0])))

bidi_type_t nbs_ts_bidi_type(uint32_t cp) {
    int lo = 0, hi = BIDI_RANGES_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < bidi_ranges[mid].lo) hi = mid - 1;
        else if (cp > bidi_ranges[mid].hi) lo = mid + 1;
        else return bidi_ranges[mid].type;
    }
    return BIDI_L; /* default: Left-to-Right */
}

/* ── UAX #9 Algorithm ─────────────────────────────────────────────── */

/* Maximum embedding level per UAX #9 */
#define MAX_DEPTH 125

/* Directional status stack entry */
typedef struct {
    int level;
    bidi_type_t override;  /* BIDI_L, BIDI_R, or BIDI_ON (neutral = no override) */
    int isolate;
} dir_status_t;

/* P2-P3: Determine paragraph embedding level */
static int determine_paragraph_level(const bidi_type_t *types, int count) {
    int isolate_count = 0;
    for (int i = 0; i < count; i++) {
        switch (types[i]) {
        case BIDI_LRI: case BIDI_RLI: case BIDI_FSI:
            isolate_count++;
            break;
        case BIDI_PDI:
            if (isolate_count > 0) isolate_count--;
            break;
        case BIDI_L:
            if (isolate_count == 0) return 0; /* LTR */
            break;
        case BIDI_R: case BIDI_AL:
            if (isolate_count == 0) return 1; /* RTL */
            break;
        default:
            break;
        }
    }
    return 0; /* default LTR */
}

/* Helper: next odd/even level */
static int next_odd(int level)  { return (level & 1) ? level + 2 : level + 1; }
static int next_even(int level) { return (level & 1) ? level + 1 : level + 2; }

int nbs_ts_bidi_reorder(const uint32_t *codepoints, int count,
                        int *visual_map, int base_dir) {
    if (count <= 0) return 0;

    /* Allocate working arrays */
    bidi_type_t *types = malloc((size_t)count * sizeof(bidi_type_t));
    bidi_type_t *resolved = malloc((size_t)count * sizeof(bidi_type_t));
    int *levels = malloc((size_t)count * sizeof(int));
    if (!types || !resolved || !levels) {
        free(types); free(resolved); free(levels);
        /* Fallback: identity mapping */
        for (int i = 0; i < count; i++) visual_map[i] = i;
        return 0;
    }

    /* Get bidi types */
    for (int i = 0; i < count; i++) {
        types[i] = nbs_ts_bidi_type(codepoints[i]);
        resolved[i] = types[i];
    }

    /* P2-P3: paragraph level */
    int para_level;
    if (base_dir == 1) para_level = 0;      /* force LTR */
    else if (base_dir == 2) para_level = 1;  /* force RTL */
    else para_level = determine_paragraph_level(types, count);

    /* Initialize levels */
    for (int i = 0; i < count; i++)
        levels[i] = para_level;

    /* X1-X8: Resolve explicit embeddings, overrides, isolates */
    {
        dir_status_t stack[MAX_DEPTH + 2];
        int stack_top = 0;
        stack[0].level = para_level;
        stack[0].override = BIDI_ON;
        stack[0].isolate = 0;
        int overflow_isolate = 0;
        int overflow_embedding = 0;
        int valid_isolate = 0;

        for (int i = 0; i < count; i++) {
            bidi_type_t t = types[i];

            switch (t) {
            case BIDI_RLE: {
                int new_level = next_odd(stack[stack_top].level);
                if (new_level <= MAX_DEPTH && overflow_isolate == 0 && overflow_embedding == 0) {
                    stack_top++;
                    stack[stack_top].level = new_level;
                    stack[stack_top].override = BIDI_ON;
                    stack[stack_top].isolate = 0;
                } else if (overflow_isolate == 0) {
                    overflow_embedding++;
                }
                levels[i] = stack[stack_top].level;
                resolved[i] = BIDI_BN;
                break;
            }
            case BIDI_LRE: {
                int new_level = next_even(stack[stack_top].level);
                if (new_level <= MAX_DEPTH && overflow_isolate == 0 && overflow_embedding == 0) {
                    stack_top++;
                    stack[stack_top].level = new_level;
                    stack[stack_top].override = BIDI_ON;
                    stack[stack_top].isolate = 0;
                } else if (overflow_isolate == 0) {
                    overflow_embedding++;
                }
                levels[i] = stack[stack_top].level;
                resolved[i] = BIDI_BN;
                break;
            }
            case BIDI_RLO: {
                int new_level = next_odd(stack[stack_top].level);
                if (new_level <= MAX_DEPTH && overflow_isolate == 0 && overflow_embedding == 0) {
                    stack_top++;
                    stack[stack_top].level = new_level;
                    stack[stack_top].override = BIDI_R;
                    stack[stack_top].isolate = 0;
                } else if (overflow_isolate == 0) {
                    overflow_embedding++;
                }
                levels[i] = stack[stack_top].level;
                resolved[i] = BIDI_BN;
                break;
            }
            case BIDI_LRO: {
                int new_level = next_even(stack[stack_top].level);
                if (new_level <= MAX_DEPTH && overflow_isolate == 0 && overflow_embedding == 0) {
                    stack_top++;
                    stack[stack_top].level = new_level;
                    stack[stack_top].override = BIDI_L;
                    stack[stack_top].isolate = 0;
                } else if (overflow_isolate == 0) {
                    overflow_embedding++;
                }
                levels[i] = stack[stack_top].level;
                resolved[i] = BIDI_BN;
                break;
            }
            case BIDI_RLI: {
                levels[i] = stack[stack_top].level;
                if (stack[stack_top].override != BIDI_ON)
                    resolved[i] = stack[stack_top].override;
                int new_level = next_odd(stack[stack_top].level);
                if (new_level <= MAX_DEPTH && overflow_isolate == 0 && overflow_embedding == 0) {
                    valid_isolate++;
                    stack_top++;
                    stack[stack_top].level = new_level;
                    stack[stack_top].override = BIDI_ON;
                    stack[stack_top].isolate = 1;
                } else {
                    overflow_isolate++;
                }
                break;
            }
            case BIDI_LRI: {
                levels[i] = stack[stack_top].level;
                if (stack[stack_top].override != BIDI_ON)
                    resolved[i] = stack[stack_top].override;
                int new_level = next_even(stack[stack_top].level);
                if (new_level <= MAX_DEPTH && overflow_isolate == 0 && overflow_embedding == 0) {
                    valid_isolate++;
                    stack_top++;
                    stack[stack_top].level = new_level;
                    stack[stack_top].override = BIDI_ON;
                    stack[stack_top].isolate = 1;
                } else {
                    overflow_isolate++;
                }
                break;
            }
            case BIDI_FSI: {
                /* Determine direction of isolate content */
                int isolate_level = determine_paragraph_level(types + i + 1, count - i - 1);
                /* Treat as RLI or LRI */
                levels[i] = stack[stack_top].level;
                if (stack[stack_top].override != BIDI_ON)
                    resolved[i] = stack[stack_top].override;
                int new_level = isolate_level ? next_odd(stack[stack_top].level)
                                              : next_even(stack[stack_top].level);
                if (new_level <= MAX_DEPTH && overflow_isolate == 0 && overflow_embedding == 0) {
                    valid_isolate++;
                    stack_top++;
                    stack[stack_top].level = new_level;
                    stack[stack_top].override = BIDI_ON;
                    stack[stack_top].isolate = 1;
                } else {
                    overflow_isolate++;
                }
                break;
            }
            case BIDI_PDI:
                if (overflow_isolate > 0) {
                    overflow_isolate--;
                } else if (valid_isolate > 0) {
                    overflow_embedding = 0;
                    while (stack_top > 0 && !stack[stack_top].isolate)
                        stack_top--;
                    if (stack_top > 0) stack_top--;
                    valid_isolate--;
                }
                levels[i] = stack[stack_top].level;
                resolved[i] = BIDI_ON; /* treat as neutral */
                break;
            case BIDI_PDF:
                if (overflow_isolate == 0) {
                    if (overflow_embedding > 0) {
                        overflow_embedding--;
                    } else if (stack_top > 0 && !stack[stack_top].isolate) {
                        stack_top--;
                    }
                }
                levels[i] = stack[stack_top].level;
                resolved[i] = BIDI_BN;
                break;
            case BIDI_B:
                levels[i] = para_level;
                resolved[i] = para_level ? BIDI_R : BIDI_L;
                break;
            case BIDI_BN:
                levels[i] = stack[stack_top].level;
                break;
            default:
                levels[i] = stack[stack_top].level;
                if (stack[stack_top].override != BIDI_ON)
                    resolved[i] = stack[stack_top].override;
                break;
            }
        }
    }

    /* W1-W7: Resolve weak types */
    /* Process runs of the same level */
    {
        bidi_type_t prev_strong = (para_level & 1) ? BIDI_R : BIDI_L;

        /* W1: NSM gets type of previous char */
        for (int i = 0; i < count; i++) {
            if (resolved[i] == BIDI_NSM) {
                resolved[i] = (i > 0) ? resolved[i - 1] : prev_strong;
            }
        }

        /* W2: EN after AL → AN */
        prev_strong = (para_level & 1) ? BIDI_R : BIDI_L;
        for (int i = 0; i < count; i++) {
            if (resolved[i] == BIDI_L || resolved[i] == BIDI_R || resolved[i] == BIDI_AL)
                prev_strong = resolved[i];
            if (resolved[i] == BIDI_EN && prev_strong == BIDI_AL)
                resolved[i] = BIDI_AN;
        }

        /* W3: AL → R */
        for (int i = 0; i < count; i++) {
            if (resolved[i] == BIDI_AL)
                resolved[i] = BIDI_R;
        }

        /* W4: ES between EN → EN; CS between EN → EN; CS between AN → AN */
        for (int i = 1; i < count - 1; i++) {
            if (resolved[i] == BIDI_ES &&
                resolved[i - 1] == BIDI_EN && resolved[i + 1] == BIDI_EN)
                resolved[i] = BIDI_EN;
            if (resolved[i] == BIDI_CS) {
                if (resolved[i - 1] == BIDI_EN && resolved[i + 1] == BIDI_EN)
                    resolved[i] = BIDI_EN;
                else if (resolved[i - 1] == BIDI_AN && resolved[i + 1] == BIDI_AN)
                    resolved[i] = BIDI_AN;
            }
        }

        /* W5: ET adjacent to EN → EN */
        for (int i = 0; i < count; i++) {
            if (resolved[i] == BIDI_ET) {
                /* Look backward and forward for EN */
                int found_en = 0;
                for (int j = i - 1; j >= 0 && (resolved[j] == BIDI_ET || resolved[j] == BIDI_EN); j--) {
                    if (resolved[j] == BIDI_EN) { found_en = 1; break; }
                }
                if (!found_en) {
                    for (int j = i + 1; j < count && (resolved[j] == BIDI_ET || resolved[j] == BIDI_EN); j++) {
                        if (resolved[j] == BIDI_EN) { found_en = 1; break; }
                    }
                }
                if (found_en) resolved[i] = BIDI_EN;
            }
        }

        /* W6: ES, ET, CS → ON */
        for (int i = 0; i < count; i++) {
            if (resolved[i] == BIDI_ES || resolved[i] == BIDI_ET || resolved[i] == BIDI_CS)
                resolved[i] = BIDI_ON;
        }

        /* W7: EN after L context → L */
        prev_strong = (para_level & 1) ? BIDI_R : BIDI_L;
        for (int i = 0; i < count; i++) {
            if (resolved[i] == BIDI_L || resolved[i] == BIDI_R)
                prev_strong = resolved[i];
            if (resolved[i] == BIDI_EN && prev_strong == BIDI_L)
                resolved[i] = BIDI_L;
        }
    }

    /* N1-N2: Resolve neutral types */
    {
        bidi_type_t sor = (para_level & 1) ? BIDI_R : BIDI_L;

        for (int i = 0; i < count; i++) {
            if (resolved[i] == BIDI_ON || resolved[i] == BIDI_WS ||
                resolved[i] == BIDI_S || resolved[i] == BIDI_B ||
                resolved[i] == BIDI_BN) {
                /* Find the run of neutrals */
                int j = i;
                while (j < count && (resolved[j] == BIDI_ON || resolved[j] == BIDI_WS ||
                                     resolved[j] == BIDI_S || resolved[j] == BIDI_B ||
                                     resolved[j] == BIDI_BN))
                    j++;

                /* Determine surrounding strong types */
                bidi_type_t before = (i > 0) ? resolved[i - 1] : sor;
                bidi_type_t after = (j < count) ? resolved[j] : sor;

                /* Normalize: EN/AN → R for neutral resolution */
                if (before == BIDI_EN || before == BIDI_AN) before = BIDI_R;
                if (after == BIDI_EN || after == BIDI_AN) after = BIDI_R;

                /* N1: if both sides are same strong type, neutrals get that type */
                /* N2: otherwise, neutrals get the embedding direction */
                bidi_type_t resolved_type;
                if (before == after && (before == BIDI_L || before == BIDI_R)) {
                    resolved_type = before;  /* N1 */
                } else {
                    resolved_type = (levels[i] & 1) ? BIDI_R : BIDI_L;  /* N2 */
                }

                for (int k = i; k < j; k++)
                    resolved[k] = resolved_type;
                i = j - 1;
            }
        }
    }

    /* I1-I2: Resolve implicit levels */
    for (int i = 0; i < count; i++) {
        if (levels[i] % 2 == 0) {
            /* I1: even level */
            if (resolved[i] == BIDI_R) levels[i]++;
            else if (resolved[i] == BIDI_AN || resolved[i] == BIDI_EN) levels[i] += 2;
        } else {
            /* I2: odd level */
            if (resolved[i] == BIDI_L || resolved[i] == BIDI_AN || resolved[i] == BIDI_EN)
                levels[i]++;
        }
    }

    /* L1: Reset whitespace/isolate levels at line end */
    for (int i = count - 1; i >= 0; i--) {
        if (types[i] == BIDI_WS || types[i] == BIDI_S || types[i] == BIDI_B ||
            types[i] == BIDI_LRI || types[i] == BIDI_RLI || types[i] == BIDI_FSI ||
            types[i] == BIDI_PDI) {
            levels[i] = para_level;
        } else if (types[i] != BIDI_BN) {
            break;
        }
    }

    /* L2: Reorder — reverse subsequences at each level */
    /* First, find the maximum level */
    int max_level = 0;
    for (int i = 0; i < count; i++) {
        if (levels[i] > max_level) max_level = levels[i];
    }

    /* Initialize visual map to identity */
    for (int i = 0; i < count; i++)
        visual_map[i] = i;

    /* Reverse at each level from max down to 1 */
    for (int level = max_level; level >= 1; level--) {
        int i = 0;
        while (i < count) {
            /* Find start of run at this level or higher */
            while (i < count && levels[visual_map[i]] < level) i++;
            if (i >= count) break;
            int start = i;
            while (i < count && levels[visual_map[i]] >= level) i++;
            /* Reverse visual_map[start..i-1] */
            int lo = start, hi = i - 1;
            while (lo < hi) {
                int tmp = visual_map[lo];
                visual_map[lo] = visual_map[hi];
                visual_map[hi] = tmp;
                lo++; hi--;
            }
        }
    }

    /* Copy levels out if requested */
    int result = para_level;

    free(types);
    free(resolved);
    free(levels);
    return result;
}

/* ── Bracket mirroring table (UAX #9 L4) ─────────────────────────── */

struct mirror_pair {
    uint32_t from;
    uint32_t to;
};

static const struct mirror_pair mirror_table[] = {
    {0x0028, 0x0029}, /* ( → ) */
    {0x0029, 0x0028}, /* ) → ( */
    {0x003C, 0x003E}, /* < → > */
    {0x003E, 0x003C}, /* > → < */
    {0x005B, 0x005D}, /* [ → ] */
    {0x005D, 0x005B}, /* ] → [ */
    {0x007B, 0x007D}, /* { → } */
    {0x007D, 0x007B}, /* } → { */
    {0x00AB, 0x00BB}, /* « → » */
    {0x00BB, 0x00AB}, /* » → « */
    {0x2039, 0x203A}, /* ‹ → › */
    {0x203A, 0x2039}, /* › → ‹ */
    {0x2045, 0x2046}, /* ⁅ → ⁆ */
    {0x2046, 0x2045}, /* ⁆ → ⁅ */
    {0x207D, 0x207E}, /* ⁽ → ⁾ */
    {0x207E, 0x207D}, /* ⁾ → ⁽ */
    {0x208D, 0x208E}, /* ₍ → ₎ */
    {0x208E, 0x208D}, /* ₎ → ₍ */
    {0x2208, 0x220B}, /* ∈ → ∋ */
    {0x2209, 0x220C}, /* ∉ → ∌ */
    {0x220A, 0x220D}, /* ∊ → ∍ */
    {0x220B, 0x2208}, /* ∋ → ∈ */
    {0x220C, 0x2209}, /* ∌ → ∉ */
    {0x220D, 0x220A}, /* ∍ → ∊ */
    {0x2215, 0x29F5}, /* ∕ → ⧵ */
    {0x221F, 0x2BFE}, /* ∟ → ⯾ */
    {0x2220, 0x29A3}, /* ∠ → ⦣ */
    {0x2264, 0x2265}, /* ≤ → ≥ */
    {0x2265, 0x2264}, /* ≥ → ≤ */
    {0x226E, 0x226F}, /* ≮ → ≯ */
    {0x226F, 0x226E}, /* ≯ → ≮ */
    {0x2308, 0x2309}, /* ⌈ → ⌉ */
    {0x2309, 0x2308}, /* ⌉ → ⌈ */
    {0x230A, 0x230B}, /* ⌊ → ⌋ */
    {0x230B, 0x230A}, /* ⌋ → ⌊ */
    {0x2329, 0x232A}, /* 〈 → 〉 */
    {0x232A, 0x2329}, /* 〉 → 〈 */
    {0x27C5, 0x27C6}, /* ⟅ → ⟆ */
    {0x27C6, 0x27C5}, /* ⟆ → ⟅ */
    {0x27E6, 0x27E7}, /* ⟦ → ⟧ */
    {0x27E7, 0x27E6}, /* ⟧ → ⟦ */
    {0x27E8, 0x27E9}, /* ⟨ → ⟩ */
    {0x27E9, 0x27E8}, /* ⟩ → ⟨ */
    {0x27EA, 0x27EB}, /* ⟪ → ⟫ */
    {0x27EB, 0x27EA}, /* ⟫ → ⟪ */
    {0x27EC, 0x27ED}, /* ⟬ → ⟭ */
    {0x27ED, 0x27EC}, /* ⟭ → ⟬ */
    {0x27EE, 0x27EF}, /* ⟮ → ⟯ */
    {0x27EF, 0x27EE}, /* ⟯ → ⟮ */
    {0x2983, 0x2984}, /* ⦃ → ⦄ */
    {0x2984, 0x2983}, /* ⦄ → ⦃ */
    {0x2985, 0x2986}, /* ⦅ → ⦆ */
    {0x2986, 0x2985}, /* ⦆ → ⦅ */
    {0x2987, 0x2988}, /* ⦇ → ⦈ */
    {0x2988, 0x2987}, /* ⦈ → ⦇ */
    {0x2989, 0x298A}, /* ⦉ → ⦊ */
    {0x298A, 0x2989}, /* ⦊ → ⦉ */
    {0x298B, 0x298C}, /* ⦋ → ⦌ */
    {0x298C, 0x298B}, /* ⦌ → ⦋ */
    {0x298D, 0x2990}, /* ⦍ → ⦐ */
    {0x298E, 0x298F}, /* ⦎ → ⦏ */
    {0x298F, 0x298E}, /* ⦏ → ⦎ */
    {0x2990, 0x298D}, /* ⦐ → ⦍ */
    {0x3008, 0x3009}, /* 〈 → 〉 */
    {0x3009, 0x3008}, /* 〉 → 〈 */
    {0x300A, 0x300B}, /* 《 → 》 */
    {0x300B, 0x300A}, /* 》 → 《 */
    {0x300C, 0x300D}, /* 「 → 」 */
    {0x300D, 0x300C}, /* 」 → 「 */
    {0x300E, 0x300F}, /* 『 → 』 */
    {0x300F, 0x300E}, /* 』 → 『 */
    {0x3010, 0x3011}, /* 【 → 】 */
    {0x3011, 0x3010}, /* 】 → 【 */
    {0x3014, 0x3015}, /* 〔 → 〕 */
    {0x3015, 0x3014}, /* 〕 → 〔 */
    {0x3016, 0x3017}, /* 〖 → 〗 */
    {0x3017, 0x3016}, /* 〗 → 〖 */
    {0x3018, 0x3019}, /* 〘 → 〙 */
    {0x3019, 0x3018}, /* 〙 → 〘 */
    {0x301A, 0x301B}, /* 〚 → 〛 */
    {0x301B, 0x301A}, /* 〛 → 〚 */
    {0xFF08, 0xFF09}, /* （ → ） */
    {0xFF09, 0xFF08}, /* ） → （ */
    {0xFF1C, 0xFF1E}, /* ＜ → ＞ */
    {0xFF1E, 0xFF1C}, /* ＞ → ＜ */
    {0xFF3B, 0xFF3D}, /* ［ → ］ */
    {0xFF3D, 0xFF3B}, /* ］ → ［ */
    {0xFF5B, 0xFF5D}, /* ｛ → ｝ */
    {0xFF5D, 0xFF5B}, /* ｝ → ｛ */
    {0xFF5F, 0xFF60}, /* ｟ → ｠ */
    {0xFF60, 0xFF5F}, /* ｠ → ｟ */
};

#define MIRROR_TABLE_COUNT ((int)(sizeof(mirror_table) / sizeof(mirror_table[0])))

uint32_t nbs_ts_bidi_mirror(uint32_t cp) {
    /* Binary search — table is sorted by 'from' */
    int lo = 0, hi = MIRROR_TABLE_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < mirror_table[mid].from) hi = mid - 1;
        else if (cp > mirror_table[mid].from) lo = mid + 1;
        else return mirror_table[mid].to;
    }
    return cp; /* no mirror */
}

int nbs_ts_bidi_reorder_with_levels(const uint32_t *codepoints, int count,
                                     int *visual_map, int *out_levels,
                                     int base_dir) {
    /* Run the standard reorder */
    int para_level = nbs_ts_bidi_reorder(codepoints, count, visual_map, base_dir);

    /* Recompute levels for the caller (needed for mirroring decisions) */
    /* We need to know which positions are at odd levels */
    if (out_levels) {
        bidi_type_t *types = malloc((size_t)count * sizeof(bidi_type_t));
        if (types) {
            for (int i = 0; i < count; i++)
                types[i] = nbs_ts_bidi_type(codepoints[i]);

            /* Simplified level computation — just check if char is R/AL type */
            for (int i = 0; i < count; i++) {
                bidi_type_t t = types[i];
                if (t == BIDI_R || t == BIDI_AL || t == BIDI_AN)
                    out_levels[i] = 1;
                else if (para_level == 1 && (t == BIDI_ON || t == BIDI_WS ||
                         t == BIDI_CS || t == BIDI_ES || t == BIDI_ET ||
                         t == BIDI_NSM || t == BIDI_BN))
                    out_levels[i] = 1; /* neutrals in RTL paragraph */
                else
                    out_levels[i] = 0;
            }
            free(types);
        }
    }
    return para_level;
}
