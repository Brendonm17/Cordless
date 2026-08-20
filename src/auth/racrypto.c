#include "auth/racrypto.h"
#include "util/b64.h"

#include <stdlib.h>
#include <string.h>

#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <psa/crypto.h>

#include "console.h"

// Remote-auth crypto (RSA-2048 keypair, OAEP-SHA256, SHA-256), on mbedTLS.
struct ra_keypair {
    mbedtls_pk_context      pk;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
};

int ra_crypto_seed(void)
{
    // mbedTLS seeds its own CTR_DRBG from the Vita hardware RNG (see
    // vita_mbedtls_port.c). Nothing to do here.
    return 0;
}

ra_keypair *ra_keypair_new(void)
{
    psa_crypto_init();
    ra_keypair *kp = calloc(1, sizeof(*kp));
    if (!kp) return NULL;
    mbedtls_pk_init(&kp->pk);
    mbedtls_entropy_init(&kp->ent);
    mbedtls_ctr_drbg_init(&kp->drbg);

    const char *pers = "cordless-ra";
    if (mbedtls_ctr_drbg_seed(&kp->drbg, mbedtls_entropy_func, &kp->ent,
                              (const unsigned char *)pers, strlen(pers)) != 0) goto fail;
    if (mbedtls_pk_setup(&kp->pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) goto fail;
    if (mbedtls_rsa_gen_key(mbedtls_pk_rsa(kp->pk), mbedtls_ctr_drbg_random,
                            &kp->drbg, 2048, 65537) != 0) goto fail;
    return kp;
fail:
    mbedtls_pk_free(&kp->pk);
    mbedtls_ctr_drbg_free(&kp->drbg);
    mbedtls_entropy_free(&kp->ent);
    free(kp);
    return NULL;
}

void ra_keypair_free(ra_keypair *kp)
{
    if (!kp) return;
    mbedtls_pk_free(&kp->pk);
    mbedtls_ctr_drbg_free(&kp->drbg);
    mbedtls_entropy_free(&kp->ent);
    free(kp);
}

// Serialize the public key to SPKI/DER; mbedtls_pk_write_pubkey_der writes
// backwards from the buffer end. Returns DER start via *start, length or -1.
static int spki_der(const ra_keypair *kp, unsigned char *buf, size_t cap, unsigned char **start)
{
    int len = mbedtls_pk_write_pubkey_der((mbedtls_pk_context *)&kp->pk, buf, cap);
    if (len <= 0) return -1;
    *start = buf + cap - len;
    return len;
}

int ra_export_public_b64(const ra_keypair *kp, char *out, size_t out_cap)
{
    unsigned char der[640]; unsigned char *p = NULL;
    int len = spki_der(kp, der, sizeof(der), &p);
    if (len <= 0) return -1;
    if ((size_t)(4 * ((len + 2) / 3) + 1) > out_cap) return -1;
    return (int)b64_encode(p, len, out);
}

int ra_fingerprint(const ra_keypair *kp, char *out, size_t out_cap)
{
    unsigned char der[640]; unsigned char *p = NULL;
    int len = spki_der(kp, der, sizeof(der), &p);
    if (len <= 0) return -1;
    unsigned char digest[32];
    mbedtls_sha256(p, len, digest, 0);
    if (out_cap < 4 * ((sizeof(digest) + 2) / 3) + 1) return -1;
    return (int)b64url_encode(digest, sizeof(digest), out);
}

int ra_oaep_decrypt_b64(const ra_keypair *kp, const char *cipher_b64,
                        unsigned char *out, size_t out_cap)
{
    unsigned char cipher[512];  // RSA-2048 block = 256 bytes
    int clen = b64_decode(cipher_b64, cipher, sizeof(cipher));
    if (clen <= 0) { console_logf("[crypto] b64 decode fail (in_len=%d)",
                                  (int)strlen(cipher_b64)); return -1; }

    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(kp->pk);
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);

    unsigned char plain[256];
    size_t outlen = 0;
    int rc = mbedtls_rsa_rsaes_oaep_decrypt(rsa, mbedtls_ctr_drbg_random,
                (mbedtls_ctr_drbg_context *)&kp->drbg, NULL, 0,
                &outlen, cipher, plain, sizeof(plain));
    if (rc != 0) { console_logf("[crypto] decrypt fail clen=%d rc=-0x%04x", clen, -rc); return -1; }
    if (outlen > out_cap) { console_logf("[crypto] plaintext %d > cap %d",
                                         (int)outlen, (int)out_cap); return -1; }
    memcpy(out, plain, outlen);
    return (int)outlen;
}

int ra_sha256_b64url(const unsigned char *data, size_t len,
                     char *out, size_t out_cap)
{
    unsigned char digest[32];
    mbedtls_sha256(data, len, digest, 0);
    if (out_cap < 4 * ((sizeof(digest) + 2) / 3) + 1) return -1;
    return (int)b64url_encode(digest, sizeof(digest), out);
}

int ra_oaep_encrypt_b64(const ra_keypair *kp, const unsigned char *plain,
                        size_t plain_len, char *out_b64, size_t out_cap)
{
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(kp->pk);
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);

    unsigned char cipher[256];
    int rc = mbedtls_rsa_rsaes_oaep_encrypt(rsa, mbedtls_ctr_drbg_random,
                (mbedtls_ctr_drbg_context *)&kp->drbg, NULL, 0,
                plain_len, plain, cipher);
    if (rc != 0) return -1;
    size_t clen = mbedtls_rsa_get_len(rsa);
    if (4 * ((clen + 2) / 3) + 1 > out_cap) return -1;
    return (int)b64_encode(cipher, clen, out_b64);
}
