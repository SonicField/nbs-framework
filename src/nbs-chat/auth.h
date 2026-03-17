/*
 * auth.h — Human message authentication via Ed25519 signatures
 *
 * Provides passphrase-derived Ed25519 key pairs for signing chat messages.
 * Private key lives only in process memory (derived from passphrase via
 * PBKDF2-HMAC-SHA512). Public key written to disk for verification by
 * other processes.
 *
 * Depends on OpenSSL 3.x (libcrypto) for Ed25519 and PBKDF2.
 *
 * Thread safety: auth_state_t is NOT thread-safe. All calls must be
 * from the same thread (the terminal main thread).
 */

#ifndef NBS_AUTH_H
#define NBS_AUTH_H

#include <stddef.h>
#include <stdint.h>

/* Ed25519 key/signature sizes */
#define AUTH_ED25519_SEED_LEN    32
#define AUTH_ED25519_PUBKEY_LEN  32
#define AUTH_ED25519_SIG_LEN     64

/* Hex-encoded signature length (64 bytes * 2 + NUL) */
#define AUTH_SIG_HEX_LEN         (AUTH_ED25519_SIG_LEN * 2 + 1)

/* PBKDF2 parameters */
#define AUTH_PBKDF2_ITERATIONS   100000
#define AUTH_PBKDF2_SALT         "nbs-chat-auth-v1"
#define AUTH_PBKDF2_SALT_LEN     16

/* Verification result */
typedef enum {
    AUTH_VERIFY_OK,          /* Valid signature from known public key */
    AUTH_VERIFY_BAD_SIG,     /* Signature present but invalid */
    AUTH_VERIFY_NO_SIG,      /* No signature field in message */
    AUTH_VERIFY_NO_KEY,      /* No public key on file for this handle */
    AUTH_VERIFY_ERROR        /* Internal error (OpenSSL failure) */
} auth_verify_result_t;

/*
 * Auth state. Holds the derived key pair in memory.
 *
 * Invariants:
 *   - If initialised == 1, seed/pubkey contain valid Ed25519 key material
 *   - seed is zeroed on auth_destroy()
 *   - pubkey_path points to the trusted-keys directory
 */
typedef struct {
    int initialised;
    uint8_t seed[AUTH_ED25519_SEED_LEN];
    uint8_t pubkey[AUTH_ED25519_PUBKEY_LEN];
    char pubkey_dir[4096];  /* .nbs/chat/trusted-keys/ */
} auth_state_t;

/*
 * Derive Ed25519 key pair from passphrase.
 *
 * Preconditions:
 *   state != NULL
 *   passphrase != NULL, non-empty
 *   trusted_keys_dir != NULL, non-empty
 *
 * Postcondition: state->initialised == 1, seed and pubkey populated.
 *
 * Returns 0 on success, -1 on error.
 */
int auth_init(auth_state_t *state,
              const char *passphrase,
              const char *trusted_keys_dir);

/*
 * Sign a message. Produces a hex-encoded Ed25519 signature.
 *
 * The signed data is: timestamp || "|" || handle || "|" || message
 * This binds the signature to the exact wire format fields.
 *
 * Preconditions:
 *   state != NULL, state->initialised == 1
 *   timestamp, handle, message != NULL
 *   sig_hex buffer is at least AUTH_SIG_HEX_LEN bytes
 *
 * Returns 0 on success, -1 on error.
 */
int auth_sign(const auth_state_t *state,
              const char *timestamp,
              const char *handle,
              const char *message,
              char *sig_hex);

/*
 * Verify a message signature against a stored public key.
 *
 * Looks up <trusted_keys_dir>/<handle>.pub for the public key.
 *
 * Preconditions:
 *   trusted_keys_dir != NULL
 *   handle, timestamp, message, sig_hex != NULL
 *
 * Returns AUTH_VERIFY_OK if signature is valid.
 */
auth_verify_result_t auth_verify(const char *trusted_keys_dir,
                                  const char *handle,
                                  const char *timestamp,
                                  const char *message,
                                  const char *sig_hex);

/*
 * Write public key to trusted-keys directory (trust-on-first-use).
 *
 * Creates <trusted_keys_dir>/<handle>.pub if it doesn't exist.
 * If the file already exists, does nothing (preserves existing trust).
 *
 * Returns 0 on success, -1 on error.
 */
int auth_write_pubkey(const auth_state_t *state, const char *handle);

/*
 * Zero the seed and mark state as uninitialised.
 */
void auth_destroy(auth_state_t *state);

/*
 * Check if auth is initialised (passphrase was provided).
 */
int auth_is_initialised(const auth_state_t *state);

#endif /* NBS_AUTH_H */
