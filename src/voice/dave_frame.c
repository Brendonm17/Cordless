#include "voice/dave_frame.h"

#include <string.h>

#include <mbedtls/gcm.h>

// ULEB128 (little-endian base-128) codec for a 32-bit value. DAVE encodes the
// truncated sync nonce and the unencrypted-range pairs this way.
static int uleb128_encode(uint32_t value, uint8_t *out, size_t out_cap)
{
    size_t n = 0;
    do {
        if (n >= out_cap) return -1;
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value != 0) byte |= 0x80;
        out[n++] = byte;
    } while (value != 0);
    return (int)n;
}

// Decode a ULEB128 value forward from `p`, stopping before `limit`. Returns the
// number of bytes consumed, or -1.
static int uleb128_decode(const uint8_t *p, const uint8_t *limit, uint32_t *out)
{
    uint32_t value = 0;
    int shift = 0;
    size_t n = 0;
    while (p + n < limit) {
        uint8_t byte = p[n++];
        value |= (uint32_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            *out = value;
            return (int)n;
        }
        shift += 7;
        if (shift >= 35) return -1;
    }
    return -1;
}

// Build the 96-bit GCM nonce: 8 zero bytes then the 32-bit truncated nonce
// big-endian in the 4 least-significant bytes.
static void full_nonce(uint32_t trunc, uint8_t iv[12])
{
    memset(iv, 0, 12);
    iv[8]  = (uint8_t)(trunc >> 24);
    iv[9]  = (uint8_t)(trunc >> 16);
    iv[10] = (uint8_t)(trunc >> 8);
    iv[11] = (uint8_t)(trunc);
}

int dave_frame_encrypt(const uint8_t key[16], uint32_t nonce,
                       const uint8_t *in, size_t len,
                       uint8_t *out, size_t out_cap)
{
    if (out_cap < len + DAVE_FRAME_MAX_OVERHEAD) return -1;

    uint8_t iv[12];
    full_nonce(nonce, iv);

    // Encrypt the whole Opus frame (no unencrypted ranges); 8-byte tag follows the
    // ciphertext. AAD is empty: the frame layer authenticates only the payload.
    uint8_t tag[DAVE_FRAME_TAG_SIZE];
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128) != 0) {
        mbedtls_gcm_free(&gcm);
        return -1;
    }
    int rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len,
                                       iv, sizeof(iv), NULL, 0,
                                       in, out, DAVE_FRAME_TAG_SIZE, tag);
    mbedtls_gcm_free(&gcm);
    if (rc != 0) return -1;

    size_t pos = len;
    memcpy(out + pos, tag, DAVE_FRAME_TAG_SIZE);
    pos += DAVE_FRAME_TAG_SIZE;

    // nonce (ULEB128)
    int nlen = uleb128_encode(nonce, out + pos, out_cap - pos);
    if (nlen < 0) return -1;
    pos += (size_t)nlen;

    // unencrypted range pairs: none for Opus, so nothing here.

    // supplemental size = tag + nonce + ranges + this byte + marker
    size_t supplemental = (size_t)DAVE_FRAME_TAG_SIZE + (size_t)nlen + 1 + 2;
    if (supplemental > 0xFF) return -1;
    out[pos++] = (uint8_t)supplemental;

    out[pos++] = DAVE_FRAME_MARKER_0;
    out[pos++] = DAVE_FRAME_MARKER_1;

    return (int)pos;
}

// Locate the supplemental region and validate the marker. On success sets
// *cipher_len, *tag (into `in`), *nonce; returns 0, or -1 if not a DAVE frame.
static int parse_trailer(const uint8_t *in, size_t len,
                         size_t *cipher_len, const uint8_t **tag,
                         uint32_t *nonce)
{
    if (len < 3 + DAVE_FRAME_TAG_SIZE) return -1;
    if (in[len - 2] != DAVE_FRAME_MARKER_0 || in[len - 1] != DAVE_FRAME_MARKER_1)
        return -1;

    size_t supplemental = in[len - 3];
    if (supplemental < (size_t)DAVE_FRAME_TAG_SIZE + 1 + 2) return -1;
    if (supplemental > len) return -1;

    // Supplemental region begins here; first 8 bytes are the tag, then the
    // ULEB128 nonce, then range pairs (none for Opus), then the size + marker.
    size_t supp_start = len - supplemental;
    const uint8_t *p = in + supp_start;
    const uint8_t *supp_end = in + (len - 3);  // before size byte + marker

    *tag = p;
    p += DAVE_FRAME_TAG_SIZE;

    uint32_t nv = 0;
    int nlen = uleb128_decode(p, supp_end, &nv);
    if (nlen < 0) return -1;
    // Any remaining bytes to supp_end would be unencrypted-range pairs (none for
    // Opus).

    *cipher_len = supp_start;
    *nonce = nv;
    return 0;
}

int dave_frame_peek_nonce(const uint8_t *in, size_t len, uint32_t *nonce)
{
    size_t cipher_len;
    const uint8_t *tag;
    return parse_trailer(in, len, &cipher_len, &tag, nonce);
}

int dave_frame_decrypt(const uint8_t key[16],
                       const uint8_t *in, size_t len,
                       uint8_t *out, size_t out_cap, uint32_t *nonce_out)
{
    size_t cipher_len;
    const uint8_t *tag;
    uint32_t nonce;
    if (parse_trailer(in, len, &cipher_len, &tag, &nonce) != 0) return -1;
    if (cipher_len > out_cap) return -1;
    if (nonce_out) *nonce_out = nonce;

    uint8_t iv[12];
    full_nonce(nonce, iv);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128) != 0) {
        mbedtls_gcm_free(&gcm);
        return -1;
    }
    int rc = mbedtls_gcm_auth_decrypt(&gcm, cipher_len, iv, sizeof(iv),
                                      NULL, 0, tag, DAVE_FRAME_TAG_SIZE,
                                      in, out);
    mbedtls_gcm_free(&gcm);
    if (rc != 0) return -1;

    return (int)cipher_len;
}
