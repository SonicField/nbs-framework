/*
 * base64.c — Base64 encode/decode implementation
 *
 * Invariants:
 *   - decode_table uses 0xFF for invalid entries; only valid base64
 *     characters map to values 0-63, and '=' maps to 64.
 *   - All sextet extractions are masked with & 0x3F.
 *   - Input is validated via is_valid_base64_char() before decode_table lookup.
 *   - An assertion guards the decode_table lookup as a defence-in-depth
 *     measure against the 'A'/0 ambiguity.
 */

#include "base64.h"
#include "chat_file.h"
#include <assert.h>
#include <limits.h>

static const char encode_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * Decode table: maps ASCII byte to 6-bit value.
 *   0xFF = invalid (non-base64 characters)
 *   64   = padding ('=')
 *   0-63 = valid base64 sextet values
 *
 * Fully static const — no runtime initialisation required.
 *
 * Thread safety: this table is const data with static storage duration,
 * initialised at compile time. No runtime init means no data race.
 * This replaces the previous init_decode_table() approach which had a
 * TOCTOU race on the decode_table_initialised flag.
 *
 * The 'A'/0 ambiguity: 'A' maps to 0, and uninitialised entries also
 * default to 0 with C zero-initialisation. We solve this by explicitly
 * setting all non-base64 entries to 0xFF. The is_valid_base64_char()
 * function serves as the primary validity check; the 0xFF sentinel in
 * this table provides defence-in-depth.
 */
/* clang-format off */
static const unsigned char decode_table[256] = {
    /*         0     1     2     3     4     5     6     7  */
    /*         8     9     A     B     C     D     E     F  */
    /* 0x00 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x08 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x10 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x18 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x20 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x28 */ 0xFF, 0xFF, 0xFF, 62,   0xFF, 0xFF, 0xFF, 63,
    /*                          '+'                     '/' */
    /* 0x30 */ 52,   53,   54,   55,   56,   57,   58,   59,
    /*         '0'   '1'   '2'   '3'   '4'   '5'   '6'   '7' */
    /* 0x38 */ 60,   61,   0xFF, 0xFF, 0xFF, 64,   0xFF, 0xFF,
    /*         '8'   '9'                     '='               */
    /* 0x40 */ 0xFF, 0,    1,    2,    3,    4,    5,    6,
    /*               'A'   'B'   'C'   'D'   'E'   'F'   'G'  */
    /* 0x48 */ 7,    8,    9,    10,   11,   12,   13,   14,
    /*         'H'   'I'   'J'   'K'   'L'   'M'   'N'   'O'  */
    /* 0x50 */ 15,   16,   17,   18,   19,   20,   21,   22,
    /*         'P'   'Q'   'R'   'S'   'T'   'U'   'V'   'W'  */
    /* 0x58 */ 23,   24,   25,   0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /*         'X'   'Y'   'Z'                                 */
    /* 0x60 */ 0xFF, 26,   27,   28,   29,   30,   31,   32,
    /*               'a'   'b'   'c'   'd'   'e'   'f'   'g'  */
    /* 0x68 */ 33,   34,   35,   36,   37,   38,   39,   40,
    /*         'h'   'i'   'j'   'k'   'l'   'm'   'n'   'o'  */
    /* 0x70 */ 41,   42,   43,   44,   45,   46,   47,   48,
    /*         'p'   'q'   'r'   's'   't'   'u'   'v'   'w'  */
    /* 0x78 */ 49,   50,   51,   0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /*         'x'   'y'   'z'                                 */
    /* 0x80 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x88 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x90 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x98 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0xA0 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0xA8 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0xB0 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0xB8 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0xC0 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0xC8 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0xD0 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0xD8 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0xE0 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0xE8 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0xF0 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0xF8 */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
/* clang-format on */

/* Validate a character is in the base64 alphabet (including '=' padding) */
static int is_valid_base64_char(unsigned char c) {
    if (c == '=' || c == '+' || c == '/') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return 0;
}

int base64_encode(const unsigned char *input, size_t input_len,
                  char *output, size_t output_size) {
    /* Preconditions */
    ASSERT_MSG(output != NULL,
               "base64_encode: output buffer is NULL; "
               "caller must provide a valid output buffer");
    ASSERT_MSG(input != NULL || input_len == 0,
               "base64_encode: input is NULL with non-zero length %zu; "
               "NULL input is only valid when input_len is 0", input_len);
    /* Independent overflow guard — do not rely solely on the callee
     * (base64_encoded_size) to catch this. */
    ASSERT_MSG(input_len <= (SIZE_MAX - 4) / 4 * 3,
               "base64_encode: input_len %zu would overflow size calculation",
               input_len);

    size_t needed = base64_encoded_size(input_len);
    if (output_size < needed) {
        fprintf(stderr,
                "base64_encode: output buffer too small: need %zu, got %zu\n",
                needed, output_size);
        return -1;
    }

    size_t i = 0, j = 0;

    /* Process 3-byte groups */
    for (; i + 2 < input_len; i += 3) {
        unsigned int triple = ((unsigned int)input[i] << 16) |
                              ((unsigned int)input[i + 1] << 8) |
                              ((unsigned int)input[i + 2]);
        output[j++] = encode_table[(triple >> 18) & 0x3F];
        output[j++] = encode_table[(triple >> 12) & 0x3F];
        output[j++] = encode_table[(triple >> 6) & 0x3F];
        output[j++] = encode_table[triple & 0x3F];
    }

    /* Handle remaining bytes */
    if (i < input_len) {
        unsigned int triple = (unsigned int)input[i] << 16;
        if (i + 1 < input_len) {
            triple |= (unsigned int)input[i + 1] << 8;
        }

        output[j++] = encode_table[(triple >> 18) & 0x3F];
        output[j++] = encode_table[(triple >> 12) & 0x3F];

        if (i + 1 < input_len) {
            output[j++] = encode_table[(triple >> 6) & 0x3F];
        } else {
            output[j++] = '=';
        }
        output[j++] = '=';
    }

    output[j] = '\0';

    /* Postconditions */
    ASSERT_MSG(j == needed - 1,
               "base64_encode: output length %zu != expected %zu; "
               "encoding logic produced wrong number of characters", j, needed - 1);
    ASSERT_MSG(j <= (size_t)INT_MAX,
               "base64_encode: output length %zu exceeds INT_MAX; "
               "input too large for int return type", j);

    return (int)j;
}

int base64_decode(const char *input, size_t input_len,
                  unsigned char *output, size_t output_size) {
    /* Preconditions */
    ASSERT_MSG(input != NULL,
               "base64_decode: input buffer is NULL; "
               "caller must provide valid base64 input");
    ASSERT_MSG(output != NULL,
               "base64_decode: output buffer is NULL; "
               "caller must provide a valid output buffer");

    /* Strip trailing whitespace/newlines */
    while (input_len > 0 && (input[input_len - 1] == '\n' ||
                              input[input_len - 1] == '\r' ||
                              input[input_len - 1] == ' ')) {
        input_len--;
    }

    if (input_len == 0) {
        /* Postcondition: empty input produces zero-length output */
        ASSERT_MSG(output_size >= 1,
                   "base64_decode: output_size %zu too small for empty input "
                   "(need at least 1 for safety)", output_size);
        return 0;
    }

    if (input_len % 4 != 0) {
        fprintf(stderr,
                "base64_decode: input length %zu is not a multiple of 4; "
                "valid base64 input must be padded to a multiple of 4 bytes\n",
                input_len);
        return -1;
    }

    /* Validate all characters and padding position.
     *
     * Padding rules per 4-byte block:
     *   - '=' may only appear in positions 2 and 3 of the final block.
     *   - Once a '=' appears, all subsequent bytes in the block must be '='.
     *   - Valid final-block patterns: XXXX, XXX=, XX==
     *   - Invalid examples: X=XX, X=X=, XX=X, =XXX, ==XX, etc.
     *   - Non-final blocks must not contain '=' at all.
     */
    for (size_t i = 0; i < input_len; i++) {
        if (!is_valid_base64_char((unsigned char)input[i])) {
            fprintf(stderr,
                    "base64_decode: invalid character 0x%02x at position %zu; "
                    "only A-Z, a-z, 0-9, +, /, = are valid base64 characters\n",
                    (unsigned char)input[i], i);
            return -1;
        }
        if (input[i] == '=') {
            /* Padding is only valid in the final 4-byte block */
            size_t block_start = (i / 4) * 4;
            if (block_start + 4 != input_len) {
                fprintf(stderr,
                        "base64_decode: padding '=' at position %zu is invalid; "
                        "padding may only appear in the final 4-byte block\n", i);
                return -1;
            }
            /* Positions 0 and 1 within a block must never be '=' */
            size_t pos_in_block = i - block_start;
            if (pos_in_block < 2) {
                fprintf(stderr,
                        "base64_decode: padding '=' at position %zu is invalid; "
                        "first two characters of a block must not be padding\n", i);
                return -1;
            }
            /* After the first '=' in the block, all remaining must be '=' */
            for (size_t k = i + 1; k < input_len; k++) {
                if (input[k] != '=') {
                    fprintf(stderr,
                            "base64_decode: non-padding character '%c' at "
                            "position %zu after padding '=' at position %zu; "
                            "once padding begins, all subsequent characters "
                            "in the block must be '='\n",
                            input[k], k, i);
                    return -1;
                }
            }
            break; /* Validated all chars from first '=' to end */
        }
    }

    size_t out_len = (input_len / 4) * 3;
    if (input[input_len - 1] == '=') out_len--;
    if (input[input_len - 2] == '=') out_len--;

    if (output_size < out_len) {
        fprintf(stderr,
                "base64_decode: output buffer too small: need %zu, got %zu\n",
                out_len, output_size);
        return -1;
    }

    size_t j = 0;
    for (size_t i = 0; i < input_len; i += 4) {
        /* Defence-in-depth: assert decode_table value is not 0xFF (invalid)
         * BEFORE computing sextets. The validation loop above should have
         * caught invalid characters, but this guards against the table
         * being bypassed or corrupted. Assertions must fire before any
         * masked computation to avoid acting on garbage values. */
        ASSERT_MSG(decode_table[(unsigned char)input[i]] != 0xFF,
                   "base64_decode: decode_table returned invalid sentinel for "
                   "character 0x%02x at position %zu; validation was bypassed",
                   (unsigned char)input[i], i);
        ASSERT_MSG(decode_table[(unsigned char)input[i + 1]] != 0xFF,
                   "base64_decode: decode_table returned invalid sentinel for "
                   "character 0x%02x at position %zu; validation was bypassed",
                   (unsigned char)input[i + 1], i + 1);
        ASSERT_MSG(decode_table[(unsigned char)input[i + 2]] != 0xFF,
                   "base64_decode: decode_table returned invalid sentinel for "
                   "character 0x%02x at position %zu; validation was bypassed",
                   (unsigned char)input[i + 2], i + 2);
        ASSERT_MSG(decode_table[(unsigned char)input[i + 3]] != 0xFF,
                   "base64_decode: decode_table returned invalid sentinel for "
                   "character 0x%02x at position %zu; validation was bypassed",
                   (unsigned char)input[i + 3], i + 3);

        unsigned int sextet_a = decode_table[(unsigned char)input[i]] & 0x3F;
        unsigned int sextet_b = decode_table[(unsigned char)input[i + 1]] & 0x3F;
        unsigned int sextet_c = decode_table[(unsigned char)input[i + 2]] & 0x3F;
        unsigned int sextet_d = decode_table[(unsigned char)input[i + 3]] & 0x3F;

        unsigned int triple = (sextet_a << 18) | (sextet_b << 12) |
                              (sextet_c << 6) | sextet_d;

        if (j < out_len) output[j++] = (triple >> 16) & 0xFF;
        if (j < out_len) output[j++] = (triple >> 8) & 0xFF;
        if (j < out_len) output[j++] = triple & 0xFF;
    }

    /* Postconditions */
    ASSERT_MSG(j == out_len,
               "base64_decode: decoded length %zu != expected %zu; "
               "decode loop produced wrong number of bytes", j, out_len);
    ASSERT_MSG(out_len <= (size_t)INT_MAX,
               "base64_decode: output length %zu exceeds INT_MAX; "
               "input too large for int return type", out_len);

    return (int)out_len;
}
