/*
 * auth.c — Human message authentication via Ed25519 signatures
 *
 * Uses OpenSSL 3.x EVP API for Ed25519 signing/verification
 * and PKCS5_PBKDF2_HMAC for passphrase-to-seed derivation.
 */

#include "auth.h"
#include "../nbs-common/nbs_assert.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <openssl/evp.h>
#include <openssl/err.h>

/* --- Hex encoding/decoding --- */

static void hex_encode(const uint8_t *data, size_t len, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[data[i] >> 4];
        out[i * 2 + 1] = hex[data[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static int hex_decode(const char *hex_str, uint8_t *out, size_t out_len) {
    size_t hex_len = strlen(hex_str);
    if (hex_len != out_len * 2) return -1;

    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte;
        char buf[3] = { hex_str[i*2], hex_str[i*2+1], '\0' };
        if (sscanf(buf, "%02x", &byte) != 1) return -1;
        out[i] = (uint8_t)byte;
    }
    return 0;
}

/* --- Key derivation --- */

int auth_init(auth_state_t *state,
              const char *passphrase,
              const char *trusted_keys_dir) {
    ASSERT_MSG(state != NULL, "auth_init: state is NULL");
    ASSERT_MSG(passphrase != NULL && passphrase[0] != '\0',
               "auth_init: passphrase is NULL or empty");
    ASSERT_MSG(trusted_keys_dir != NULL && trusted_keys_dir[0] != '\0',
               "auth_init: trusted_keys_dir is NULL or empty");

    memset(state, 0, sizeof(*state));

    /* Derive 32-byte seed from passphrase via PBKDF2-HMAC-SHA512 */
    int rc = PKCS5_PBKDF2_HMAC(passphrase, (int)strlen(passphrase),
                                (const unsigned char *)AUTH_PBKDF2_SALT,
                                AUTH_PBKDF2_SALT_LEN,
                                AUTH_PBKDF2_ITERATIONS,
                                EVP_sha512(),
                                AUTH_ED25519_SEED_LEN,
                                state->seed);
    if (rc != 1) {
        fprintf(stderr, "auth_init: PBKDF2 failed\n");
        return -1;
    }

    /* Generate Ed25519 key pair from seed */
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL,
                                                   state->seed,
                                                   AUTH_ED25519_SEED_LEN);
    if (!pkey) {
        fprintf(stderr, "auth_init: Ed25519 key generation failed\n");
        memset(state->seed, 0, sizeof(state->seed));
        return -1;
    }

    /* Extract public key */
    size_t pubkey_len = AUTH_ED25519_PUBKEY_LEN;
    if (EVP_PKEY_get_raw_public_key(pkey, state->pubkey, &pubkey_len) != 1 ||
        pubkey_len != AUTH_ED25519_PUBKEY_LEN) {
        fprintf(stderr, "auth_init: failed to extract public key\n");
        EVP_PKEY_free(pkey);
        memset(state->seed, 0, sizeof(state->seed));
        return -1;
    }

    EVP_PKEY_free(pkey);

    int sn = snprintf(state->pubkey_dir, sizeof(state->pubkey_dir),
                      "%s", trusted_keys_dir);
    ASSERT_MSG(sn > 0 && (size_t)sn < sizeof(state->pubkey_dir),
               "auth_init: trusted_keys_dir truncated");

    state->initialised = 1;

    /* Postconditions */
    ASSERT_MSG(state->initialised == 1,
               "auth_init postcondition: initialised must be 1");
    return 0;
}

/* --- Signing --- */

int auth_sign(const auth_state_t *state,
              const char *timestamp,
              const char *handle,
              const char *message,
              char *sig_hex) {
    ASSERT_MSG(state != NULL && state->initialised == 1,
               "auth_sign: state is NULL or not initialised");
    ASSERT_MSG(timestamp != NULL, "auth_sign: timestamp is NULL");
    ASSERT_MSG(handle != NULL, "auth_sign: handle is NULL");
    ASSERT_MSG(message != NULL, "auth_sign: message is NULL");
    ASSERT_MSG(sig_hex != NULL, "auth_sign: sig_hex is NULL");

    /* Build signed data: timestamp|handle|message */
    size_t data_len = strlen(timestamp) + 1 + strlen(handle) + 1 + strlen(message);
    char *data = malloc(data_len + 1);
    if (!data) return -1;
    snprintf(data, data_len + 1, "%s|%s|%s", timestamp, handle, message);

    /* Create key from seed */
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL,
                                                   state->seed,
                                                   AUTH_ED25519_SEED_LEN);
    if (!pkey) {
        free(data);
        return -1;
    }

    /* Sign */
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        free(data);
        return -1;
    }

    uint8_t sig[AUTH_ED25519_SIG_LEN];
    size_t sig_len = sizeof(sig);

    int ok = (EVP_DigestSignInit(md_ctx, NULL, NULL, NULL, pkey) == 1 &&
              EVP_DigestSign(md_ctx, sig, &sig_len,
                             (const unsigned char *)data, data_len) == 1 &&
              sig_len == AUTH_ED25519_SIG_LEN);

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    free(data);

    if (!ok) return -1;

    hex_encode(sig, AUTH_ED25519_SIG_LEN, sig_hex);
    return 0;
}

/* --- Verification --- */

auth_verify_result_t auth_verify(const char *trusted_keys_dir,
                                  const char *handle,
                                  const char *timestamp,
                                  const char *message,
                                  const char *sig_hex) {
    ASSERT_MSG(trusted_keys_dir != NULL, "auth_verify: trusted_keys_dir is NULL");
    ASSERT_MSG(handle != NULL, "auth_verify: handle is NULL");

    if (!sig_hex || sig_hex[0] == '\0') return AUTH_VERIFY_NO_SIG;
    if (!timestamp || !message) return AUTH_VERIFY_ERROR;

    /* Load public key from file */
    char pubkey_path[4096 + 256];
    int sn = snprintf(pubkey_path, sizeof(pubkey_path),
                      "%s/%s.pub", trusted_keys_dir, handle);
    if (sn <= 0 || (size_t)sn >= sizeof(pubkey_path)) return AUTH_VERIFY_ERROR;

    FILE *fp = fopen(pubkey_path, "r");
    if (!fp) return AUTH_VERIFY_NO_KEY;

    char hex_buf[AUTH_ED25519_PUBKEY_LEN * 2 + 2];
    if (!fgets(hex_buf, sizeof(hex_buf), fp)) {
        fclose(fp);
        return AUTH_VERIFY_NO_KEY;
    }
    fclose(fp);

    /* Strip trailing newline */
    size_t hlen = strlen(hex_buf);
    if (hlen > 0 && hex_buf[hlen - 1] == '\n') hex_buf[--hlen] = '\0';

    uint8_t pubkey[AUTH_ED25519_PUBKEY_LEN];
    if (hex_decode(hex_buf, pubkey, AUTH_ED25519_PUBKEY_LEN) != 0)
        return AUTH_VERIFY_NO_KEY;

    /* Decode signature */
    uint8_t sig[AUTH_ED25519_SIG_LEN];
    if (hex_decode(sig_hex, sig, AUTH_ED25519_SIG_LEN) != 0)
        return AUTH_VERIFY_BAD_SIG;

    /* Build signed data: timestamp|handle|message */
    size_t data_len = strlen(timestamp) + 1 + strlen(handle) + 1 + strlen(message);
    char *data = malloc(data_len + 1);
    if (!data) return AUTH_VERIFY_ERROR;
    snprintf(data, data_len + 1, "%s|%s|%s", timestamp, handle, message);

    /* Create public key object */
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                                  pubkey, AUTH_ED25519_PUBKEY_LEN);
    if (!pkey) {
        free(data);
        return AUTH_VERIFY_ERROR;
    }

    /* Verify */
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        free(data);
        return AUTH_VERIFY_ERROR;
    }

    auth_verify_result_t result;
    if (EVP_DigestVerifyInit(md_ctx, NULL, NULL, NULL, pkey) != 1) {
        result = AUTH_VERIFY_ERROR;
    } else if (EVP_DigestVerify(md_ctx, sig, AUTH_ED25519_SIG_LEN,
                                 (const unsigned char *)data, data_len) == 1) {
        result = AUTH_VERIFY_OK;
    } else {
        result = AUTH_VERIFY_BAD_SIG;
    }

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    free(data);
    return result;
}

/* --- Public key I/O --- */

int auth_write_pubkey(const auth_state_t *state, const char *handle) {
    ASSERT_MSG(state != NULL && state->initialised == 1,
               "auth_write_pubkey: state not initialised");
    ASSERT_MSG(handle != NULL && handle[0] != '\0',
               "auth_write_pubkey: handle is NULL or empty");

    /* Create directory if needed */
    mkdir(state->pubkey_dir, 0755);

    char path[4096 + 256];
    int sn = snprintf(path, sizeof(path), "%s/%s.pub",
                      state->pubkey_dir, handle);
    if (sn <= 0 || (size_t)sn >= sizeof(path)) return -1;

    /* Trust-on-first-use: don't overwrite existing key */
    FILE *check = fopen(path, "r");
    if (check) {
        fclose(check);
        return 0;  /* Key already exists */
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "auth_write_pubkey: failed to write %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    char hex[AUTH_ED25519_PUBKEY_LEN * 2 + 1];
    hex_encode(state->pubkey, AUTH_ED25519_PUBKEY_LEN, hex);
    fprintf(fp, "%s\n", hex);
    fclose(fp);
    return 0;
}

/* --- Cleanup --- */

void auth_destroy(auth_state_t *state) {
    if (!state) return;
    memset(state->seed, 0, sizeof(state->seed));
    state->initialised = 0;
}

int auth_is_initialised(const auth_state_t *state) {
    return state && state->initialised == 1;
}
