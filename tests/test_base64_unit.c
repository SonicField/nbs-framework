/*
 * test_base64_unit.c — Unit tests for base64 encode/decode
 *
 * Tests:
 *   1. Round-trip encode/decode for various input sizes (0-256 bytes)
 *   2. Known test vectors from RFC 4648
 *   3. Invalid character rejection
 *   4. Padding position validation
 *   5. Decode table 0xFF sentinel correctness (the 'A'/0 ambiguity fix)
 *   6. Empty input handling
 *   7. Whitespace stripping in decode
 *   8. Output buffer too small (recoverable error, not assertion)
 *   9. INT_MAX overflow guard (via assertion, tested separately)
 *  10. Padding followed by non-padding rejection (AB=C bug)
 *  11. Valid padding acceptance (AB==, QQ==, AAA=)
 *  12. Thread safety of static const decode table
 *  13. Empty input postcondition (return 0 without assertion)
 *  14. Assertion ordering (defence-in-depth checks before computation)
 *
 * Build (from tests/ directory):
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2 \
 *       -I../src/nbs-chat \
 *       -o test_base64_unit test_base64_unit.c \
 *       ../src/nbs-chat/base64.c ../src/nbs-chat/chat_file.c \
 *       ../src/nbs-chat/lock.c \
 *       -lpthread
 *
 * Or with ASan:
 *   clang -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
 *       -fsanitize=address,undefined -fno-omit-frame-pointer \
 *       -I../src/nbs-chat \
 *       -o test_base64_unit test_base64_unit.c \
 *       ../src/nbs-chat/base64.c ../src/nbs-chat/chat_file.c \
 *       ../src/nbs-chat/lock.c \
 *       -fsanitize=address,undefined -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

/* Include the headers from the source directory */
#include "chat_file.h"
#include "base64.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    tests_passed++; \
    printf("  PASS: %s\n", name); \
} while(0)

/* --- RFC 4648 test vectors --- */

static void test_rfc4648_vectors(void) {
    /* Table from RFC 4648 Section 10 */
    struct { const char *plain; const char *encoded; } vectors[] = {
        { "",       "" },
        { "f",      "Zg==" },
        { "fo",     "Zm8=" },
        { "foo",    "Zm9v" },
        { "foob",   "Zm9vYg==" },
        { "fooba",  "Zm9vYmE=" },
        { "foobar", "Zm9vYmFy" },
    };
    size_t n = sizeof(vectors) / sizeof(vectors[0]);

    for (size_t i = 0; i < n; i++) {
        const unsigned char *plain = (const unsigned char *)vectors[i].plain;
        size_t plain_len = strlen(vectors[i].plain);
        const char *expected = vectors[i].encoded;
        size_t expected_len = strlen(expected);

        /* Encode */
        size_t enc_size = base64_encoded_size(plain_len);
        char *enc_buf = malloc(enc_size);
        TEST_ASSERT(enc_buf != NULL, "malloc failed for encode buffer");

        int enc_ret = base64_encode(plain, plain_len, enc_buf, enc_size);
        TEST_ASSERT(enc_ret >= 0,
                    "base64_encode failed for vector %zu (\"%s\")", i, vectors[i].plain);
        TEST_ASSERT((size_t)enc_ret == expected_len,
                    "base64_encode length %d != expected %zu for vector %zu",
                    enc_ret, expected_len, i);
        TEST_ASSERT(strcmp(enc_buf, expected) == 0,
                    "base64_encode output \"%s\" != expected \"%s\" for vector %zu",
                    enc_buf, expected, i);

        /* Decode */
        if (expected_len > 0) {
            size_t dec_size = base64_decoded_size(expected_len);
            unsigned char *dec_buf = malloc(dec_size);
            TEST_ASSERT(dec_buf != NULL, "malloc failed for decode buffer");

            int dec_ret = base64_decode(expected, expected_len, dec_buf, dec_size);
            TEST_ASSERT(dec_ret >= 0,
                        "base64_decode failed for vector %zu (\"%s\")", i, expected);
            TEST_ASSERT((size_t)dec_ret == plain_len,
                        "base64_decode length %d != expected %zu for vector %zu",
                        dec_ret, plain_len, i);
            TEST_ASSERT(memcmp(dec_buf, plain, plain_len) == 0,
                        "base64_decode output mismatch for vector %zu", i);

            free(dec_buf);
        }

        free(enc_buf);
    }

    TEST_PASS("RFC 4648 test vectors");
}

/* --- Round-trip for all byte lengths 0..256 --- */

static void test_roundtrip_all_lengths(void) {
    for (size_t len = 0; len <= 256; len++) {
        unsigned char *input = malloc(len > 0 ? len : 1);
        TEST_ASSERT(input != NULL, "malloc failed for length %zu", len);

        /* Fill with deterministic pattern */
        for (size_t i = 0; i < len; i++) {
            input[i] = (unsigned char)(i & 0xFF);
        }

        /* Encode */
        size_t enc_size = base64_encoded_size(len);
        char *enc_buf = malloc(enc_size);
        TEST_ASSERT(enc_buf != NULL, "malloc failed for encode buffer, length %zu", len);

        int enc_ret = base64_encode(input, len, enc_buf, enc_size);
        TEST_ASSERT(enc_ret >= 0, "base64_encode failed for length %zu", len);

        /* Decode */
        size_t dec_size = base64_decoded_size((size_t)enc_ret);
        unsigned char *dec_buf = malloc(dec_size);
        TEST_ASSERT(dec_buf != NULL, "malloc failed for decode buffer, length %zu", len);

        int dec_ret = base64_decode(enc_buf, (size_t)enc_ret, dec_buf, dec_size);
        TEST_ASSERT(dec_ret >= 0, "base64_decode failed for length %zu", len);
        TEST_ASSERT((size_t)dec_ret == len,
                    "round-trip length mismatch: got %d, expected %zu", dec_ret, len);
        TEST_ASSERT(memcmp(dec_buf, input, len) == 0,
                    "round-trip data mismatch for length %zu", len);

        free(dec_buf);
        free(enc_buf);
        free(input);
    }

    TEST_PASS("round-trip for all lengths 0..256");
}

/* --- Test 'A' decodes correctly (the 0-ambiguity fix) --- */

static void test_decode_A_character(void) {
    /* "AAAA" decodes to three zero bytes */
    const char *input = "AAAA";
    unsigned char output[3];
    int ret = base64_decode(input, 4, output, sizeof(output));
    TEST_ASSERT(ret == 3,
                "decoding 'AAAA': expected length 3, got %d", ret);
    TEST_ASSERT(output[0] == 0 && output[1] == 0 && output[2] == 0,
                "decoding 'AAAA': expected three zero bytes, got 0x%02x 0x%02x 0x%02x",
                output[0], output[1], output[2]);

    /* "QQ==" decodes to single byte 'A' (0x41) */
    const char *input2 = "QQ==";
    unsigned char output2[1];
    int ret2 = base64_decode(input2, 4, output2, sizeof(output2));
    TEST_ASSERT(ret2 == 1,
                "decoding 'QQ==': expected length 1, got %d", ret2);
    TEST_ASSERT(output2[0] == 'A',
                "decoding 'QQ==': expected 0x41 ('A'), got 0x%02x", output2[0]);

    TEST_PASS("'A' character decodes correctly (0-ambiguity fix verified)");
}

/* --- Test all-zeros input round-trips --- */

static void test_all_zeros_roundtrip(void) {
    unsigned char zeros[16];
    memset(zeros, 0, sizeof(zeros));

    size_t enc_size = base64_encoded_size(sizeof(zeros));
    char *enc_buf = malloc(enc_size);
    TEST_ASSERT(enc_buf != NULL, "malloc failed");

    int enc_ret = base64_encode(zeros, sizeof(zeros), enc_buf, enc_size);
    TEST_ASSERT(enc_ret >= 0, "encode failed for all-zeros");

    /* The encoded form of 16 zero bytes should be "AAAAAAAAAAAAAAAAAAAAAA==" */
    TEST_ASSERT(strcmp(enc_buf, "AAAAAAAAAAAAAAAAAAAAAA==") == 0,
                "all-zeros encode: got \"%s\"", enc_buf);

    size_t dec_size = base64_decoded_size((size_t)enc_ret);
    unsigned char *dec_buf = malloc(dec_size);
    TEST_ASSERT(dec_buf != NULL, "malloc failed");

    int dec_ret = base64_decode(enc_buf, (size_t)enc_ret, dec_buf, dec_size);
    TEST_ASSERT(dec_ret == 16,
                "all-zeros decode: expected 16, got %d", dec_ret);
    TEST_ASSERT(memcmp(dec_buf, zeros, 16) == 0,
                "all-zeros round-trip: data mismatch");

    free(dec_buf);
    free(enc_buf);

    TEST_PASS("all-zeros round-trip (verifies 0xFF sentinel works)");
}

/* --- Test invalid character rejection --- */

static void test_invalid_chars_rejected(void) {
    unsigned char output[64];

    /* Characters outside the base64 alphabet */
    const char *bad_inputs[] = {
        "!!!!",      /* exclamation marks */
        "@ABC",      /* @ symbol */
        "AB\x01D",   /* control character */
        "AB{D",      /* brace */
        "AB~D",      /* tilde */
        ("AB\x80" "D"), /* high byte */
    };
    size_t n = sizeof(bad_inputs) / sizeof(bad_inputs[0]);

    for (size_t i = 0; i < n; i++) {
        int ret = base64_decode(bad_inputs[i], 4, output, sizeof(output));
        TEST_ASSERT(ret == -1,
                    "bad input %zu (\"%s\") should return -1, got %d",
                    i, bad_inputs[i], ret);
    }

    TEST_PASS("invalid characters rejected");
}

/* --- Test padding position validation --- */

static void test_padding_position(void) {
    unsigned char output[64];

    /* Padding in wrong positions should be rejected */
    const char *bad_padding[] = {
        "==AA",       /* padding at start */
        "=AAA",       /* padding at start */
        "A=AA",       /* padding in middle */
    };
    size_t n = sizeof(bad_padding) / sizeof(bad_padding[0]);

    for (size_t i = 0; i < n; i++) {
        int ret = base64_decode(bad_padding[i], 4, output, sizeof(output));
        TEST_ASSERT(ret == -1,
                    "bad padding %zu (\"%s\") should return -1, got %d",
                    i, bad_padding[i], ret);
    }

    /* Valid padding patterns */
    int ret;
    ret = base64_decode("AAAA", 4, output, sizeof(output));
    TEST_ASSERT(ret == 3, "no padding: expected 3, got %d", ret);

    ret = base64_decode("AAA=", 4, output, sizeof(output));
    TEST_ASSERT(ret == 2, "single pad: expected 2, got %d", ret);

    ret = base64_decode("AA==", 4, output, sizeof(output));
    TEST_ASSERT(ret == 1, "double pad: expected 1, got %d", ret);

    TEST_PASS("padding position validation");
}

/* --- Test invalid length rejection --- */

static void test_invalid_length(void) {
    unsigned char output[64];

    /* Lengths not multiple of 4 (after whitespace strip) */
    int ret;
    ret = base64_decode("A", 1, output, sizeof(output));
    TEST_ASSERT(ret == -1, "length 1 should return -1, got %d", ret);

    ret = base64_decode("AA", 2, output, sizeof(output));
    TEST_ASSERT(ret == -1, "length 2 should return -1, got %d", ret);

    ret = base64_decode("AAA", 3, output, sizeof(output));
    TEST_ASSERT(ret == -1, "length 3 should return -1, got %d", ret);

    TEST_PASS("invalid length rejection");
}

/* --- Test whitespace stripping --- */

static void test_whitespace_stripping(void) {
    unsigned char output[64];

    /* Trailing whitespace should be stripped */
    int ret;
    ret = base64_decode("Zm9v\n", 5, output, sizeof(output));
    TEST_ASSERT(ret == 3, "trailing newline: expected 3, got %d", ret);
    TEST_ASSERT(memcmp(output, "foo", 3) == 0,
                "trailing newline: data mismatch");

    ret = base64_decode("Zm9v\r\n", 6, output, sizeof(output));
    TEST_ASSERT(ret == 3, "trailing CRLF: expected 3, got %d", ret);

    ret = base64_decode("Zm9v  ", 6, output, sizeof(output));
    TEST_ASSERT(ret == 3, "trailing spaces: expected 3, got %d", ret);

    TEST_PASS("whitespace stripping");
}

/* --- Test empty input --- */

static void test_empty_input(void) {
    unsigned char output[1];

    int ret = base64_decode("", 0, output, sizeof(output));
    TEST_ASSERT(ret == 0, "empty decode: expected 0, got %d", ret);

    char enc_output[8];
    ret = base64_encode((const unsigned char *)"", 0, enc_output, sizeof(enc_output));
    TEST_ASSERT(ret == 0, "empty encode: expected 0, got %d", ret);
    TEST_ASSERT(enc_output[0] == '\0',
                "empty encode: expected null-terminated empty string");

    TEST_PASS("empty input handling");
}

/* --- Test output buffer too small (encode) --- */

static void test_encode_buffer_too_small(void) {
    const unsigned char input[] = "hello";
    char output[4]; /* Too small for base64 of 5 bytes (needs 9) */

    int ret = base64_encode(input, 5, output, sizeof(output));
    TEST_ASSERT(ret == -1,
                "encode with small buffer: expected -1, got %d", ret);

    TEST_PASS("encode rejects undersized output buffer");
}

/* --- Test output buffer too small (decode) --- */

static void test_decode_buffer_too_small(void) {
    const char *encoded = "aGVsbG8="; /* "hello" */
    unsigned char output[2]; /* Too small for 5-byte decode */

    int ret = base64_decode(encoded, strlen(encoded), output, sizeof(output));
    TEST_ASSERT(ret == -1,
                "decode with small buffer: expected -1, got %d", ret);

    TEST_PASS("decode rejects undersized output buffer");
}

/* --- Test binary data round-trip (all 256 byte values) --- */

static void test_binary_roundtrip(void) {
    unsigned char input[256];
    for (int i = 0; i < 256; i++) {
        input[i] = (unsigned char)i;
    }

    size_t enc_size = base64_encoded_size(256);
    char *enc_buf = malloc(enc_size);
    TEST_ASSERT(enc_buf != NULL, "malloc failed");

    int enc_ret = base64_encode(input, 256, enc_buf, enc_size);
    TEST_ASSERT(enc_ret >= 0, "encode failed for all-byte-values");

    size_t dec_size = base64_decoded_size((size_t)enc_ret);
    unsigned char *dec_buf = malloc(dec_size);
    TEST_ASSERT(dec_buf != NULL, "malloc failed");

    int dec_ret = base64_decode(enc_buf, (size_t)enc_ret, dec_buf, dec_size);
    TEST_ASSERT(dec_ret == 256, "all-byte-values: expected 256, got %d", dec_ret);
    TEST_ASSERT(memcmp(dec_buf, input, 256) == 0,
                "all-byte-values: data mismatch");

    free(dec_buf);
    free(enc_buf);

    TEST_PASS("binary round-trip (all 256 byte values)");
}

/* --- Test large input --- */

static void test_large_input(void) {
    /* 64 KB input: non-trivial but not absurd */
    size_t len = 65536;
    unsigned char *input = malloc(len);
    TEST_ASSERT(input != NULL, "malloc failed for large input");

    for (size_t i = 0; i < len; i++) {
        input[i] = (unsigned char)(i * 7 + 13); /* arbitrary pattern */
    }

    size_t enc_size = base64_encoded_size(len);
    char *enc_buf = malloc(enc_size);
    TEST_ASSERT(enc_buf != NULL, "malloc failed for large encode buffer");

    int enc_ret = base64_encode(input, len, enc_buf, enc_size);
    TEST_ASSERT(enc_ret >= 0, "encode failed for large input");

    size_t dec_size = base64_decoded_size((size_t)enc_ret);
    unsigned char *dec_buf = malloc(dec_size);
    TEST_ASSERT(dec_buf != NULL, "malloc failed for large decode buffer");

    int dec_ret = base64_decode(enc_buf, (size_t)enc_ret, dec_buf, dec_size);
    TEST_ASSERT(dec_ret >= 0, "decode failed for large input");
    TEST_ASSERT((size_t)dec_ret == len,
                "large round-trip: length %d != %zu", dec_ret, len);
    TEST_ASSERT(memcmp(dec_buf, input, len) == 0,
                "large round-trip: data mismatch");

    free(dec_buf);
    free(enc_buf);
    free(input);

    TEST_PASS("large input (64 KB) round-trip");
}

/* --- Test encoded output contains only valid characters --- */

static void test_encode_output_charset(void) {
    /* Encode various inputs and verify all output chars are valid base64 */
    for (size_t len = 1; len <= 64; len++) {
        unsigned char *input = malloc(len);
        TEST_ASSERT(input != NULL, "malloc failed");
        for (size_t i = 0; i < len; i++) {
            input[i] = (unsigned char)(i * 31 + 17);
        }

        size_t enc_size = base64_encoded_size(len);
        char *enc_buf = malloc(enc_size);
        TEST_ASSERT(enc_buf != NULL, "malloc failed");

        int enc_ret = base64_encode(input, len, enc_buf, enc_size);
        TEST_ASSERT(enc_ret >= 0, "encode failed for length %zu", len);

        /* Verify every character is in the valid base64 alphabet */
        for (int j = 0; j < enc_ret; j++) {
            unsigned char c = (unsigned char)enc_buf[j];
            int valid = (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '+' || c == '/' || c == '=';
            TEST_ASSERT(valid,
                        "encode output contains invalid char 0x%02x at position %d "
                        "for input length %zu", c, j, len);
        }

        free(enc_buf);
        free(input);
    }

    TEST_PASS("encoded output contains only valid base64 characters");
}

/* --- Test "AB=C" padding bug: padding followed by non-padding must be rejected --- */

static void test_padding_followed_by_nonpadding_rejected(void) {
    unsigned char output[64];

    /* "AB=C": '=' at position 2, 'C' at position 3. This is invalid
     * because once padding begins, all subsequent characters in the
     * 4-byte block must also be '='. The old code allowed this because
     * its check (i < input_len - 2) only rejected '=' before the last
     * two positions, but did not enforce that characters after '=' are
     * also '='. */
    int ret = base64_decode("AB=C", 4, output, sizeof(output));
    TEST_ASSERT(ret == -1,
                "\"AB=C\" should be rejected (padding followed by non-padding), "
                "got %d", ret);

    /* Additional invalid patterns within a single block */
    ret = base64_decode("A=BC", 4, output, sizeof(output));
    TEST_ASSERT(ret == -1,
                "\"A=BC\" should be rejected (padding at position 1), got %d", ret);

    ret = base64_decode("A==C", 4, output, sizeof(output));
    TEST_ASSERT(ret == -1,
                "\"A==C\" should be rejected (padding at position 1), got %d", ret);

    ret = base64_decode("A=B=", 4, output, sizeof(output));
    TEST_ASSERT(ret == -1,
                "\"A=B=\" should be rejected (padding at position 1), got %d", ret);

    /* Multi-block: padding in non-final block */
    ret = base64_decode("AB==AAAA", 8, output, sizeof(output));
    TEST_ASSERT(ret == -1,
                "\"AB==AAAA\" should be rejected (padding in non-final block), "
                "got %d", ret);

    ret = base64_decode("AAA=AAAA", 8, output, sizeof(output));
    TEST_ASSERT(ret == -1,
                "\"AAA=AAAA\" should be rejected (padding in non-final block), "
                "got %d", ret);

    TEST_PASS("padding followed by non-padding rejected (AB=C bug fixed)");
}

/* --- Test "AB==" valid padding is accepted and decodes correctly --- */

static void test_valid_padding_AB_equals_equals(void) {
    unsigned char output[64];

    /* "AB==" is valid: two data characters + two padding = 1 decoded byte.
     * 'A' = 0, 'B' = 1. Sextets: 000000 000001 (padding) (padding)
     * Combined: 00000000 0001xxxx => first byte = 0x00, and only 1 byte output.
     * Actually: sextet_a=0, sextet_b=1, so triple = (0<<18)|(1<<12) = 0x1000.
     * Byte 0 = (0x1000 >> 16) & 0xFF = 0x00. out_len = 1. */
    int ret = base64_decode("AB==", 4, output, sizeof(output));
    TEST_ASSERT(ret == 1,
                "\"AB==\" should decode to 1 byte, got %d", ret);
    TEST_ASSERT(output[0] == 0x00,
                "\"AB==\" should decode to 0x00, got 0x%02x", output[0]);

    /* "QQ==" decodes to 'A' (0x41). Already tested in test_decode_A_character
     * but verify it still works after padding validation changes. */
    ret = base64_decode("QQ==", 4, output, sizeof(output));
    TEST_ASSERT(ret == 1,
                "\"QQ==\" should decode to 1 byte, got %d", ret);
    TEST_ASSERT(output[0] == 0x41,
                "\"QQ==\" should decode to 0x41 ('A'), got 0x%02x", output[0]);

    /* "AAA=" decodes to 2 bytes (both 0x00). */
    ret = base64_decode("AAA=", 4, output, sizeof(output));
    TEST_ASSERT(ret == 2,
                "\"AAA=\" should decode to 2 bytes, got %d", ret);
    TEST_ASSERT(output[0] == 0x00 && output[1] == 0x00,
                "\"AAA=\" should decode to {0x00, 0x00}, got {0x%02x, 0x%02x}",
                output[0], output[1]);

    TEST_PASS("valid padding patterns (AB==, QQ==, AAA=) accepted and decode correctly");
}

/* --- Test thread safety: concurrent decodes must not corrupt output --- */

#define THREAD_COUNT 8
#define THREAD_ITERATIONS 1000

struct thread_result {
    int failures;
    int thread_id;
};

static void *thread_decode_func(void *arg) {
    struct thread_result *result = (struct thread_result *)arg;
    result->failures = 0;

    /* Each thread decodes "Zm9vYmFy" ("foobar") repeatedly */
    const char *input = "Zm9vYmFy";
    const unsigned char expected[] = "foobar";
    size_t input_len = 8;
    unsigned char output[8];

    for (int i = 0; i < THREAD_ITERATIONS; i++) {
        int ret = base64_decode(input, input_len, output, sizeof(output));
        if (ret != 6) {
            result->failures++;
            continue;
        }
        if (memcmp(output, expected, 6) != 0) {
            result->failures++;
        }
    }

    return NULL;
}

static void test_thread_safety_decode_table(void) {
    pthread_t threads[THREAD_COUNT];
    struct thread_result results[THREAD_COUNT];

    for (int i = 0; i < THREAD_COUNT; i++) {
        results[i].thread_id = i;
        results[i].failures = 0;
        int rc = pthread_create(&threads[i], NULL, thread_decode_func,
                                &results[i]);
        TEST_ASSERT(rc == 0, "pthread_create failed for thread %d: %d", i, rc);
    }

    int total_failures = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        int rc = pthread_join(threads[i], NULL);
        TEST_ASSERT(rc == 0, "pthread_join failed for thread %d: %d", i, rc);
        total_failures += results[i].failures;
    }

    TEST_ASSERT(total_failures == 0,
                "thread safety: %d decode failures across %d threads x %d iterations",
                total_failures, THREAD_COUNT, THREAD_ITERATIONS);

    TEST_PASS("thread safety: concurrent decodes produce correct results");
}

/* --- Test empty input postcondition: decode returns 0, no assertion failure --- */

static void test_empty_input_postcondition(void) {
    /* Empty string with length 0 */
    unsigned char output[4];
    int ret = base64_decode("", 0, output, sizeof(output));
    TEST_ASSERT(ret == 0,
                "empty input (len=0) should return 0, got %d", ret);

    /* All-whitespace input (stripped to empty) */
    ret = base64_decode("   \n\r\n  ", 8, output, sizeof(output));
    TEST_ASSERT(ret == 0,
                "all-whitespace input should return 0 after stripping, got %d",
                ret);

    /* Verify encode empty also works with postcondition */
    char enc_output[8];
    ret = base64_encode((const unsigned char *)"", 0, enc_output,
                        sizeof(enc_output));
    TEST_ASSERT(ret == 0,
                "empty encode should return 0, got %d", ret);
    TEST_ASSERT(enc_output[0] == '\0',
                "empty encode should produce null-terminated empty string");

    TEST_PASS("empty input postcondition (returns 0 without assertion failure)");
}

/* --- Test assertion ordering: sextets checked before computation --- */

static void test_assertion_ordering_valid_inputs(void) {
    /* This test verifies that the defence-in-depth assertions do not
     * fire for valid inputs. If assertions were incorrectly ordered
     * (after computation), a valid '=' padding character would have
     * its decode_table value (64) masked to 0 by & 0x3F before the
     * assertion could check for 0xFF. With correct ordering, the
     * assertion sees the raw table value (64 for '=', not 0xFF) and
     * passes.
     *
     * We cannot directly test that assertions fire before computation
     * without triggering abort(), but we CAN verify that all valid
     * base64 inputs (including padding) pass through without assertion
     * failures. If the assertions were incorrectly placed, certain
     * inputs might cause incorrect behaviour detectable in output. */

    unsigned char output[64];

    int ret;
    ret = base64_decode("AAAA", 4, output, sizeof(output));
    TEST_ASSERT(ret == 3, "AAAA: expected 3, got %d", ret);

    ret = base64_decode("AAA=", 4, output, sizeof(output));
    TEST_ASSERT(ret == 2, "AAA=: expected 2, got %d", ret);

    ret = base64_decode("AA==", 4, output, sizeof(output));
    TEST_ASSERT(ret == 1, "AA==: expected 1, got %d", ret);

    ret = base64_decode("Zm9v", 4, output, sizeof(output));
    TEST_ASSERT(ret == 3, "Zm9v: expected 3, got %d", ret);
    TEST_ASSERT(memcmp(output, "foo", 3) == 0,
                "Zm9v should decode to 'foo'");

    ret = base64_decode("Zm8=", 4, output, sizeof(output));
    TEST_ASSERT(ret == 2, "Zm8=: expected 2, got %d", ret);
    TEST_ASSERT(memcmp(output, "fo", 2) == 0,
                "Zm8= should decode to 'fo'");

    ret = base64_decode("Zg==", 4, output, sizeof(output));
    TEST_ASSERT(ret == 1, "Zg==: expected 1, got %d", ret);
    TEST_ASSERT(output[0] == 'f',
                "Zg== should decode to 'f', got 0x%02x", output[0]);

    /* Multi-block with padding in final block */
    ret = base64_decode("AAAAAAAA", 8, output, sizeof(output));
    TEST_ASSERT(ret == 6, "AAAAAAAA: expected 6, got %d", ret);

    ret = base64_decode("AAAAAAA=", 8, output, sizeof(output));
    TEST_ASSERT(ret == 5, "AAAAAAA=: expected 5, got %d", ret);

    ret = base64_decode("AAAAAA==", 8, output, sizeof(output));
    TEST_ASSERT(ret == 4, "AAAAAA==: expected 4, got %d", ret);

    TEST_PASS("assertion ordering: all valid inputs pass without assertion failure");
}

int main(void) {
    printf("=== base64 unit tests ===\n\n");

    test_rfc4648_vectors();
    test_roundtrip_all_lengths();
    test_decode_A_character();
    test_all_zeros_roundtrip();
    test_invalid_chars_rejected();
    test_padding_position();
    test_invalid_length();
    test_whitespace_stripping();
    test_empty_input();
    test_encode_buffer_too_small();
    test_decode_buffer_too_small();
    test_binary_roundtrip();
    test_large_input();
    test_encode_output_charset();
    test_padding_followed_by_nonpadding_rejected();
    test_valid_padding_AB_equals_equals();
    test_thread_safety_decode_table();
    test_empty_input_postcondition();
    test_assertion_ordering_valid_inputs();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
