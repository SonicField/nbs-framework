/*
 * nbs_ts_wcwidth.h — Unicode character width lookup.
 *
 * Returns display width of a Unicode codepoint:
 *   0  combining mark, zero-width character
 *   1  normal width
 *   2  wide (CJK, fullwidth, emoji)
 *  -1  non-printable control character
 */

#ifndef NBS_TS_WCWIDTH_H
#define NBS_TS_WCWIDTH_H

#include <stdint.h>

int nbs_ts_wcwidth(uint32_t cp);

#endif /* NBS_TS_WCWIDTH_H */
