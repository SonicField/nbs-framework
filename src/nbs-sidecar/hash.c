/*
 * hash.c — FNV-1a 64-bit hash implementation.
 *
 * Used for fast content change detection on pane snapshots.
 * Not cryptographic; sufficient for detecting whether pane
 * content has changed between polling intervals.
 */

#include "hash.h"
#include "../nbs-common/nbs_assert.h"

uint64_t fnv1a_hash(const void *data, size_t len) {
    ASSERT_MSG(len == 0 || data != NULL,
               "fnv1a_hash: NULL data with non-zero len %zu", len);

    const unsigned char *p = data;
    uint64_t h = 14695981039346656037ULL;  /* FNV offset basis */

    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;  /* FNV prime */
    }

    return h;
}
