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
 *   - Collisions are possible but statistically unlikely for
 *     typical pane content (FNV-1a has good distribution, not
 *     collision-free — pigeonhole principle guarantees collisions
 *     exist for any 64-bit hash)
 */
uint64_t fnv1a_hash(const void *data, size_t len);

#endif /* NBS_HASH_H */
