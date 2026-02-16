/*
 * base64.h — Base64 encode/decode for nbs-chat messages
 *
 * Each chat message is stored as a single base64-encoded line.
 * Standard base64 alphabet (A-Z, a-z, 0-9, +, /) with = padding.
 */

#ifndef NBS_BASE64_H
#define NBS_BASE64_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
/*
 * Architectural note: chat_file.h is included solely for the ASSERT_MSG
 * macro. This couples base64.h to the chat subsystem, which is
 * undesirable. A future refactoring should extract ASSERT_MSG into its
 * own header (e.g., nbs_assert.h) to break this dependency.
 */
#include "chat_file.h"

/*
 * base64_encode — Encode binary data to base64 string.
 *
 * Preconditions:
 *   - input != NULL (or input_len == 0)
 *   - output != NULL
 *   - output_size >= base64_encoded_size(input_len)
 *
 * Postconditions:
 *   - output contains null-terminated base64 string
 *   - return value is length of encoded string (excluding null)
 *   - return -1 on error (output too small)
 */
int base64_encode(const unsigned char *input, size_t input_len,
                  char *output, size_t output_size);

/*
 * base64_decode — Decode base64 string to binary data.
 *
 * Preconditions:
 *   - input != NULL
 *   - output != NULL
 *   - output_size >= base64_decoded_size(input_len)
 *
 * Postconditions:
 *   - output contains decoded binary data
 *   - return value is length of decoded data
 *   - return -1 on error (invalid base64 or output too small)
 */
int base64_decode(const char *input, size_t input_len,
                  unsigned char *output, size_t output_size);

/*
 * Size calculation helpers.
 *
 * Precondition: input_len must not cause arithmetic overflow.
 * The maximum safe input_len for encoding is (SIZE_MAX - 4) / 4 * 3,
 * which ensures (input_len + 2) / 3 * 4 + 1 does not wrap.
 *
 * These are static inline so each translation unit gets its own copy.
 * The ASSERT_MSG macro (from chat_file.h) is NOT gated by NDEBUG — it
 * always fires. This is intentional: these overflow checks are
 * executable specifications, not optional debug aids. The safety of
 * these inline functions depends on this NDEBUG-independence.
 */
static inline size_t base64_encoded_size(size_t input_len) {
    ASSERT_MSG(input_len <= (SIZE_MAX - 4) / 4 * 3,
               "base64_encoded_size: input_len %zu would cause arithmetic "
               "overflow in size calculation", input_len);
    return ((input_len + 2) / 3) * 4 + 1; /* +1 for null terminator */
}

static inline size_t base64_decoded_size(size_t input_len) {
    ASSERT_MSG(input_len <= SIZE_MAX - 3,
               "base64_decoded_size: input_len %zu would cause arithmetic "
               "overflow in size calculation", input_len);
    return (input_len / 4) * 3 + 3; /* conservative upper bound */
}

#endif /* NBS_BASE64_H */
