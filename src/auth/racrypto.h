#ifndef CORDLESS_AUTH_RACRYPTO_H
#define CORDLESS_AUTH_RACRYPTO_H

#include <stddef.h>

// Crypto for Discord remote-auth (QR login): RSA-2048, OAEP-SHA256, on mbedTLS.
// All base64 buffers are caller-owned.

typedef struct ra_keypair ra_keypair;  // opaque; holds an mbedtls_pk_context

// Call once at startup before generating keys. Returns 0 on success.
int ra_crypto_seed(void);

// Generate a fresh RSA-2048 keypair. Returns NULL on failure.
ra_keypair *ra_keypair_new(void);
void        ra_keypair_free(ra_keypair *kp);

// Export the public key as SPKI/DER, base64 (standard), NUL-terminated. This is
// the encoded_public_key for the init frame. Returns length or -1.
int ra_export_public_b64(const ra_keypair *kp, char *out, size_t out_cap);

// Fingerprint = base64url(SHA256(SPKI-DER)), no padding. Returns length or -1.
int ra_fingerprint(const ra_keypair *kp, char *out, size_t out_cap);

// Decrypt a base64 (standard) ciphertext with the private key, RSA-OAEP-SHA256.
// Returns plaintext length, or -1.
int ra_oaep_decrypt_b64(const ra_keypair *kp, const char *cipher_b64,
                        unsigned char *out, size_t out_cap);

// Test helper: OAEP-SHA256 encrypt with the public key. Returns base64 length or -1.
int ra_oaep_encrypt_b64(const ra_keypair *kp, const unsigned char *plain,
                        size_t plain_len, char *out_b64, size_t out_cap);

// base64url(SHA256(data)), no padding. Builds the nonce_proof. Returns length or -1.
int ra_sha256_b64url(const unsigned char *data, size_t len,
                     char *out, size_t out_cap);

#endif  // CORDLESS_AUTH_RACRYPTO_H
