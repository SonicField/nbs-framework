/*
 * test_auth_unit.c — Unit tests for the auth module (Ed25519 signing)
 *
 * Tests the auth API (auth_init, auth_sign, auth_verify, auth_write_pubkey)
 * using OpenSSL's Ed25519 implementation. We do NOT test OpenSSL's crypto
 * correctness — we test OUR integration: round-trips, rejections, TOFU.
 *
 * Build:
 *   gcc -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L \
 *       -O2 -I ../src/nbs-chat -I ../src/nbs-common \
 *       -o test_auth_unit test_auth_unit.c ../src/nbs-chat/auth.c \
 *       -lcrypto
 *
 * Groups:
 *   A: Initialisation (4 tests)
 *   B: Sign + verify round-trip (5 tests)
 *   C: Verification rejection (5 tests)
 *   D: Trusted keys / TOFU (4 tests)
 *   E: Destroy / cleanup (3 tests)
 *   F: Adversarial (3 tests)
 */

#include "auth.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL %s:%d: " fmt "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define TEST_PASS(name) do { \
    printf("  PASS: %s\n", (name)); \
    tests_passed++; \
} while (0)

static char g_tmpdir[256];

static void make_tmpdir(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/test_auth_XXXXXX");
    char *result = mkdtemp(g_tmpdir);
    if (!result) {
        perror("mkdtemp");
        exit(1);
    }
}

static void rm_tmpdir(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmpdir);
    system(cmd);
}

/* Helper: run a function in a child process and check if it aborts */
static int expect_abort(void (*fn)(void)) __attribute__((unused));
static int expect_abort(void (*fn)(void)) {
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

/* ================================================================
 * Group A: Initialisation
 * ================================================================ */

static void test_init_success(void) {
    auth_state_t as;
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/trusted-keys", g_tmpdir);
    mkdir(tkdir, 0755);

    int rc = auth_init(&as, "test-passphrase", tkdir);
    TEST_ASSERT(rc == 0, "auth_init should return 0, got %d", rc);
    TEST_ASSERT(as.initialised == 1, "initialised should be 1");
    auth_destroy(&as);
    TEST_PASS("A1: auth_init succeeds with valid passphrase");
}

static void test_init_deterministic(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/trusted-keys-det", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as1, as2;
    auth_init(&as1, "same-passphrase", tkdir);
    auth_init(&as2, "same-passphrase", tkdir);

    TEST_ASSERT(memcmp(as1.pubkey, as2.pubkey, AUTH_ED25519_PUBKEY_LEN) == 0,
                "same passphrase should produce same pubkey");
    TEST_ASSERT(memcmp(as1.seed, as2.seed, AUTH_ED25519_SEED_LEN) == 0,
                "same passphrase should produce same seed");
    auth_destroy(&as1);
    auth_destroy(&as2);
    TEST_PASS("A2: same passphrase produces same key pair (deterministic)");
}

static void test_init_different_passphrase(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/trusted-keys-diff", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as1, as2;
    auth_init(&as1, "passphrase-one", tkdir);
    auth_init(&as2, "passphrase-two", tkdir);

    TEST_ASSERT(memcmp(as1.pubkey, as2.pubkey, AUTH_ED25519_PUBKEY_LEN) != 0,
                "different passphrases should produce different pubkeys");
    auth_destroy(&as1);
    auth_destroy(&as2);
    TEST_PASS("A3: different passphrases produce different keys");
}

static void test_init_is_initialised(void) {
    auth_state_t as;
    memset(&as, 0, sizeof(as));
    TEST_ASSERT(auth_is_initialised(&as) == 0,
                "uninitialised state should return 0");

    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/trusted-keys-init", g_tmpdir);
    mkdir(tkdir, 0755);
    auth_init(&as, "test", tkdir);
    TEST_ASSERT(auth_is_initialised(&as) == 1,
                "initialised state should return 1");
    auth_destroy(&as);
    TEST_PASS("A4: auth_is_initialised reflects state correctly");
}

/* ================================================================
 * Group B: Sign + verify round-trip
 * ================================================================ */

static void test_sign_verify_roundtrip(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-roundtrip", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "roundtrip-pass", tkdir);
    auth_write_pubkey(&as, "alex");

    char sig_hex[AUTH_SIG_HEX_LEN];
    int rc = auth_sign(&as, "1742248800", "alex", "Hello world", sig_hex);
    TEST_ASSERT(rc == 0, "auth_sign should return 0, got %d", rc);
    TEST_ASSERT(strlen(sig_hex) == AUTH_ED25519_SIG_LEN * 2,
                "sig_hex should be %d chars, got %zu",
                AUTH_ED25519_SIG_LEN * 2, strlen(sig_hex));

    auth_verify_result_t vr = auth_verify(tkdir, "alex", "1742248800",
                                           "Hello world", sig_hex);
    TEST_ASSERT(vr == AUTH_VERIFY_OK,
                "verification should succeed, got %d", vr);
    auth_destroy(&as);
    TEST_PASS("B1: sign + verify round-trip succeeds");
}

static void test_sign_empty_message(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-empty", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "empty-msg-pass", tkdir);
    auth_write_pubkey(&as, "alex");

    char sig_hex[AUTH_SIG_HEX_LEN];
    int rc = auth_sign(&as, "1742248800", "alex", "", sig_hex);
    TEST_ASSERT(rc == 0, "signing empty message should succeed");

    auth_verify_result_t vr = auth_verify(tkdir, "alex", "1742248800",
                                           "", sig_hex);
    TEST_ASSERT(vr == AUTH_VERIFY_OK, "empty message should verify");
    auth_destroy(&as);
    TEST_PASS("B2: sign + verify empty message");
}

static void test_sign_unicode_message(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-unicode", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "unicode-pass", tkdir);
    auth_write_pubkey(&as, "alex");

    char sig_hex[AUTH_SIG_HEX_LEN];
    int rc = auth_sign(&as, "1742248800", "alex",
                        "Hello 世界 🚀", sig_hex);
    TEST_ASSERT(rc == 0, "signing unicode message should succeed");

    auth_verify_result_t vr = auth_verify(tkdir, "alex", "1742248800",
                                           "Hello 世界 🚀", sig_hex);
    TEST_ASSERT(vr == AUTH_VERIFY_OK, "unicode message should verify");
    auth_destroy(&as);
    TEST_PASS("B3: sign + verify unicode message");
}

static void test_sign_different_messages_different_sigs(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-diffsig", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "diff-pass", tkdir);

    char sig1[AUTH_SIG_HEX_LEN], sig2[AUTH_SIG_HEX_LEN];
    auth_sign(&as, "1742248800", "alex", "Message one", sig1);
    auth_sign(&as, "1742248800", "alex", "Message two", sig2);

    TEST_ASSERT(strcmp(sig1, sig2) != 0,
                "different messages should produce different signatures");
    auth_destroy(&as);
    TEST_PASS("B4: different messages produce different signatures");
}

static void test_sign_deterministic(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-detsig", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "det-pass", tkdir);

    char sig1[AUTH_SIG_HEX_LEN], sig2[AUTH_SIG_HEX_LEN];
    auth_sign(&as, "1742248800", "alex", "Same message", sig1);
    auth_sign(&as, "1742248800", "alex", "Same message", sig2);

    TEST_ASSERT(strcmp(sig1, sig2) == 0,
                "same inputs should produce same signature (Ed25519 is deterministic)");
    auth_destroy(&as);
    TEST_PASS("B5: same inputs produce same signature (deterministic)");
}

/* ================================================================
 * Group C: Verification rejection
 * ================================================================ */

static void test_verify_tampered_message(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-tamper", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "tamper-pass", tkdir);
    auth_write_pubkey(&as, "alex");

    char sig_hex[AUTH_SIG_HEX_LEN];
    auth_sign(&as, "1742248800", "alex", "Original message", sig_hex);

    /* Verify with tampered message */
    auth_verify_result_t vr = auth_verify(tkdir, "alex", "1742248800",
                                           "Tampered message", sig_hex);
    TEST_ASSERT(vr == AUTH_VERIFY_BAD_SIG,
                "tampered message should fail verification, got %d", vr);
    auth_destroy(&as);
    TEST_PASS("C1: tampered message rejected");
}

static void test_verify_tampered_timestamp(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-tampts", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "tampts-pass", tkdir);
    auth_write_pubkey(&as, "alex");

    char sig_hex[AUTH_SIG_HEX_LEN];
    auth_sign(&as, "1742248800", "alex", "Hello", sig_hex);

    /* Verify with different timestamp */
    auth_verify_result_t vr = auth_verify(tkdir, "alex", "9999999999",
                                           "Hello", sig_hex);
    TEST_ASSERT(vr == AUTH_VERIFY_BAD_SIG,
                "tampered timestamp should fail, got %d", vr);
    auth_destroy(&as);
    TEST_PASS("C2: tampered timestamp rejected");
}

static void test_verify_tampered_handle(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-tamhdl", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "tamhdl-pass", tkdir);
    auth_write_pubkey(&as, "alex");
    auth_write_pubkey(&as, "fake"); /* same key for both — tests binding */

    char sig_hex[AUTH_SIG_HEX_LEN];
    auth_sign(&as, "1742248800", "alex", "Hello", sig_hex);

    /* Verify with different handle (same key on file) */
    auth_verify_result_t vr = auth_verify(tkdir, "fake", "1742248800",
                                           "Hello", sig_hex);
    TEST_ASSERT(vr == AUTH_VERIFY_BAD_SIG,
                "wrong handle should fail (sig bound to handle), got %d", vr);
    auth_destroy(&as);
    TEST_PASS("C3: wrong handle rejected (signature bound to handle)");
}

static void test_verify_no_pubkey(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-nopk", g_tmpdir);
    mkdir(tkdir, 0755);

    /* No pubkey written — verification should return NO_KEY */
    auth_verify_result_t vr = auth_verify(tkdir, "unknown", "1742248800",
                                           "Hello", "deadbeef");
    TEST_ASSERT(vr == AUTH_VERIFY_NO_KEY,
                "missing pubkey should return NO_KEY, got %d", vr);
    TEST_PASS("C4: missing public key returns AUTH_VERIFY_NO_KEY");
}

static void test_verify_corrupted_sig(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-corrsig", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "corr-pass", tkdir);
    auth_write_pubkey(&as, "alex");

    char sig_hex[AUTH_SIG_HEX_LEN];
    auth_sign(&as, "1742248800", "alex", "Hello", sig_hex);

    /* Corrupt the signature */
    sig_hex[0] = (sig_hex[0] == 'a') ? 'b' : 'a';

    auth_verify_result_t vr = auth_verify(tkdir, "alex", "1742248800",
                                           "Hello", sig_hex);
    TEST_ASSERT(vr == AUTH_VERIFY_BAD_SIG,
                "corrupted sig should fail, got %d", vr);
    auth_destroy(&as);
    TEST_PASS("C5: corrupted signature rejected");
}

/* ================================================================
 * Group D: Trusted keys / TOFU
 * ================================================================ */

static void test_write_pubkey_creates_file(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-create", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "pk-create", tkdir);
    int rc = auth_write_pubkey(&as, "alex");
    TEST_ASSERT(rc == 0, "auth_write_pubkey should return 0, got %d", rc);

    char pkpath[1024];
    snprintf(pkpath, sizeof(pkpath), "%s/alex.pub", tkdir);
    struct stat st;
    TEST_ASSERT(stat(pkpath, &st) == 0,
                "pubkey file should exist at %s", pkpath);
    /* Pubkey file is hex-encoded (64 hex chars) + newline = 65 bytes */
    long expected_size = AUTH_ED25519_PUBKEY_LEN * 2 + 1;
    TEST_ASSERT(st.st_size == expected_size,
                "pubkey file should be %ld bytes (hex+newline), got %ld",
                expected_size, (long)st.st_size);
    auth_destroy(&as);
    TEST_PASS("D1: auth_write_pubkey creates file");
}

static void test_write_pubkey_tofu(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-tofu", g_tmpdir);
    mkdir(tkdir, 0755);

    /* First write */
    auth_state_t as1;
    auth_init(&as1, "pass-one", tkdir);
    auth_write_pubkey(&as1, "alex");

    /* Second write with different passphrase should NOT overwrite */
    auth_state_t as2;
    auth_init(&as2, "pass-two", tkdir);
    auth_write_pubkey(&as2, "alex");

    /* Verify that the first key is still on file */
    char sig_hex[AUTH_SIG_HEX_LEN];
    auth_sign(&as1, "1742248800", "alex", "test", sig_hex);

    auth_verify_result_t vr = auth_verify(tkdir, "alex", "1742248800",
                                           "test", sig_hex);
    TEST_ASSERT(vr == AUTH_VERIFY_OK,
                "first key should still verify (TOFU), got %d", vr);

    /* Second key should NOT verify (different key, but first is on file) */
    char sig2[AUTH_SIG_HEX_LEN];
    auth_sign(&as2, "1742248800", "alex", "test", sig2);
    auth_verify_result_t vr2 = auth_verify(tkdir, "alex", "1742248800",
                                            "test", sig2);
    TEST_ASSERT(vr2 == AUTH_VERIFY_BAD_SIG,
                "second key should fail (TOFU preserves first), got %d", vr2);

    auth_destroy(&as1);
    auth_destroy(&as2);
    TEST_PASS("D2: TOFU — second passphrase does not overwrite first key");
}

static void test_pubkey_file_permissions(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-perms", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "perms-pass", tkdir);
    auth_write_pubkey(&as, "alex");

    char pkpath[1024];
    snprintf(pkpath, sizeof(pkpath), "%s/alex.pub", tkdir);
    struct stat st;
    stat(pkpath, &st);

    /* Should be readable by all (world-readable for verification) */
    TEST_ASSERT((st.st_mode & 0444) == 0444,
                "pubkey should be readable by all, mode=%o", st.st_mode & 0777);
    auth_destroy(&as);
    TEST_PASS("D3: public key file is world-readable");
}

static void test_multiple_handles(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-multi", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as1, as2;
    auth_init(&as1, "alice-pass", tkdir);
    auth_init(&as2, "bob-pass", tkdir);
    auth_write_pubkey(&as1, "alice");
    auth_write_pubkey(&as2, "bob");

    /* Alice signs, verify as alice */
    char sig1[AUTH_SIG_HEX_LEN];
    auth_sign(&as1, "100", "alice", "msg", sig1);
    TEST_ASSERT(auth_verify(tkdir, "alice", "100", "msg", sig1) == AUTH_VERIFY_OK,
                "alice should verify her own sig");

    /* Bob signs, verify as bob */
    char sig2[AUTH_SIG_HEX_LEN];
    auth_sign(&as2, "100", "bob", "msg", sig2);
    TEST_ASSERT(auth_verify(tkdir, "bob", "100", "msg", sig2) == AUTH_VERIFY_OK,
                "bob should verify his own sig");

    /* Cross-verify should fail */
    TEST_ASSERT(auth_verify(tkdir, "bob", "100", "msg", sig1) != AUTH_VERIFY_OK,
                "alice's sig should not verify as bob");

    auth_destroy(&as1);
    auth_destroy(&as2);
    TEST_PASS("D4: multiple handles with different keys");
}

/* ================================================================
 * Group E: Destroy / cleanup
 * ================================================================ */

static void test_destroy_zeroes_seed(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-zero", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "secret-pass", tkdir);
    TEST_ASSERT(as.initialised == 1, "should be initialised");

    auth_destroy(&as);
    TEST_ASSERT(as.initialised == 0, "should be uninitialised after destroy");

    /* Check seed is zeroed */
    unsigned char zero[AUTH_ED25519_SEED_LEN] = {0};
    TEST_ASSERT(memcmp(as.seed, zero, AUTH_ED25519_SEED_LEN) == 0,
                "seed should be zeroed after destroy");
    TEST_PASS("E1: destroy zeroes seed");
}

static void child_sign_after_destroy(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-signaft", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "destroy-pass", tkdir);
    auth_destroy(&as);

    char sig_hex[AUTH_SIG_HEX_LEN];
    auth_sign(&as, "100", "alex", "msg", sig_hex);
}

static void test_sign_after_destroy_aborts(void) {
    TEST_ASSERT(expect_abort(child_sign_after_destroy),
                "sign after destroy should abort (ASSERT_MSG)");
    TEST_PASS("E2: sign after destroy aborts");
}

static void test_is_initialised_after_destroy(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-isafter", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "init-pass", tkdir);
    TEST_ASSERT(auth_is_initialised(&as) == 1, "should be initialised");
    auth_destroy(&as);
    TEST_ASSERT(auth_is_initialised(&as) == 0, "should not be initialised");
    TEST_PASS("E3: auth_is_initialised returns 0 after destroy");
}

/* ================================================================
 * Group F: Adversarial
 * ================================================================ */

static void test_verify_short_sig_hex(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-shortsig", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "short-pass", tkdir);
    auth_write_pubkey(&as, "alex");

    /* Truncated sig hex — should not crash */
    auth_verify_result_t vr = auth_verify(tkdir, "alex", "100",
                                           "msg", "deadbeef");
    TEST_ASSERT(vr != AUTH_VERIFY_OK,
                "truncated sig should not verify, got %d", vr);
    auth_destroy(&as);
    TEST_PASS("F1: truncated signature hex handled gracefully");
}

static void test_verify_invalid_hex(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-badhex", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "hex-pass", tkdir);
    auth_write_pubkey(&as, "alex");

    /* Invalid hex characters */
    auth_verify_result_t vr = auth_verify(tkdir, "alex", "100",
                                           "msg", "ZZZZZZZZZZZZZZZZ");
    TEST_ASSERT(vr != AUTH_VERIFY_OK,
                "invalid hex should not verify, got %d", vr);
    auth_destroy(&as);
    TEST_PASS("F2: invalid hex signature handled gracefully");
}

static void test_verify_empty_sig(void) {
    char tkdir[512];
    snprintf(tkdir, sizeof(tkdir), "%s/tk-emptysig", g_tmpdir);
    mkdir(tkdir, 0755);

    auth_state_t as;
    auth_init(&as, "empty-pass", tkdir);
    auth_write_pubkey(&as, "alex");

    auth_verify_result_t vr = auth_verify(tkdir, "alex", "100",
                                           "msg", "");
    TEST_ASSERT(vr != AUTH_VERIFY_OK,
                "empty sig should not verify, got %d", vr);
    auth_destroy(&as);
    TEST_PASS("F3: empty signature handled gracefully");
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void) {
    make_tmpdir();
    printf("=== auth unit tests ===\n");
    printf("Test dir: %s\n\n", g_tmpdir);

    /* Group A: Initialisation */
    printf("-- Group A: Initialisation --\n");
    test_init_success();
    test_init_deterministic();
    test_init_different_passphrase();
    test_init_is_initialised();

    /* Group B: Sign + verify round-trip */
    printf("\n-- Group B: Sign + verify round-trip --\n");
    test_sign_verify_roundtrip();
    test_sign_empty_message();
    test_sign_unicode_message();
    test_sign_different_messages_different_sigs();
    test_sign_deterministic();

    /* Group C: Verification rejection */
    printf("\n-- Group C: Verification rejection --\n");
    test_verify_tampered_message();
    test_verify_tampered_timestamp();
    test_verify_tampered_handle();
    test_verify_no_pubkey();
    test_verify_corrupted_sig();

    /* Group D: Trusted keys / TOFU */
    printf("\n-- Group D: Trusted keys / TOFU --\n");
    test_write_pubkey_creates_file();
    test_write_pubkey_tofu();
    test_pubkey_file_permissions();
    test_multiple_handles();

    /* Group E: Destroy / cleanup */
    printf("\n-- Group E: Destroy / cleanup --\n");
    test_destroy_zeroes_seed();
    test_sign_after_destroy_aborts();
    test_is_initialised_after_destroy();

    /* Group F: Adversarial */
    printf("\n-- Group F: Adversarial --\n");
    test_verify_short_sig_hex();
    test_verify_invalid_hex();
    test_verify_empty_sig();

    rm_tmpdir();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
