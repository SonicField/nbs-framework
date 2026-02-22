/*
 * hash.h — Fast hash for content change detection.
 */

#ifndef NBS_HASH_H
#define NBS_HASH_H

#include <stddef.h>
#include <stdint.h>

/*
 * fnv1a_hash — FNV-1a hash of a byte string.
 *
 * Preconditions:
 *   - data != NULL when len > 0
 *
 * Postconditions:
 *   - Returns a 64-bit hash value
 *   - Same (data, len) always produces the same hash
 *   - Differing data produces different hashes (not cryptographic, but
 *     sufficient for change detection on pane content)
 */
uint64_t fnv1a_hash(const void *data, size_t len);

#endif /* NBS_HASH_H */
