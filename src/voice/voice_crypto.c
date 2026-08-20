#include "voice/voice_crypto.h"
#include "console.h"

#include <string.h>
#include <stdint.h>

#include <mbedtls/gcm.h>
#include <mbedtls/chachapoly.h>

// See voice_crypto.h for the wire format. This file is mbedTLS-only so it builds
// and runs on the host for offline verification.

int voice_crypto_mode_id(const char *mode)
{
    if (mode && strstr(mode, "aes256_gcm"))         return VOICE_MODE_AES_GCM;
    if (mode && strstr(mode, "xchacha20_poly1305")) return VOICE_MODE_XCHACHA;
    return VOICE_MODE_XCHACHA;  // always-offered fallback
}


static void wr32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#define VC_ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define VC_QR(a, b, c, d)                    \
    a += b; d ^= a; d = VC_ROTL32(d, 16);    \
    c += d; b ^= c; b = VC_ROTL32(b, 12);    \
    a += b; d ^= a; d = VC_ROTL32(d, 8);     \
    c += d; b ^= c; b = VC_ROTL32(b, 7)

static void hchacha20(const uint8_t key[32], const uint8_t nonce16[16], uint8_t out[32])
{
    uint32_t x[16];
    x[0] = 0x61707865; x[1] = 0x3320646e;  // "expand 32-byte k"
    x[2] = 0x79622d32; x[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) x[4 + i]  = rd32le(key + 4 * i);
    for (int i = 0; i < 4; i++) x[12 + i] = rd32le(nonce16 + 4 * i);
    for (int i = 0; i < 10; i++) {
        VC_QR(x[0], x[4], x[8],  x[12]); VC_QR(x[1], x[5], x[9],  x[13]);
        VC_QR(x[2], x[6], x[10], x[14]); VC_QR(x[3], x[7], x[11], x[15]);
        VC_QR(x[0], x[5], x[10], x[15]); VC_QR(x[1], x[6], x[11], x[12]);
        VC_QR(x[2], x[7], x[8],  x[13]); VC_QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 4; i++) { wr32le(out + 4 * i, x[i]); wr32le(out + 16 + 4 * i, x[12 + i]); }
}

// XChaCha20-Poly1305-IETF: subkey = HChaCha20(key, nonce24[0:16]); then ChaCha20-
// Poly1305-IETF with a 12-byte nonce = 0x00000000 || nonce24[16:24].
static int xchacha_seal(const uint8_t key[32], const uint8_t nonce24[24],
                        const uint8_t *aad, size_t aadn,
                        const uint8_t *pt, size_t n, uint8_t *ct, uint8_t tag[16])
{
    uint8_t subkey[32], n12[12] = {0};
    hchacha20(key, nonce24, subkey);
    memcpy(n12 + 4, nonce24 + 16, 8);
    mbedtls_chachapoly_context c; mbedtls_chachapoly_init(&c);
    int rc = mbedtls_chachapoly_setkey(&c, subkey);
    if (rc == 0) rc = mbedtls_chachapoly_encrypt_and_tag(&c, n, n12, aad, aadn, pt, ct, tag);
    mbedtls_chachapoly_free(&c);
    return rc;
}
static int xchacha_open(const uint8_t key[32], const uint8_t nonce24[24],
                        const uint8_t *aad, size_t aadn, const uint8_t tag[16],
                        const uint8_t *ct, size_t n, uint8_t *pt)
{
    uint8_t subkey[32], n12[12] = {0};
    hchacha20(key, nonce24, subkey);
    memcpy(n12 + 4, nonce24 + 16, 8);
    mbedtls_chachapoly_context c; mbedtls_chachapoly_init(&c);
    int rc = mbedtls_chachapoly_setkey(&c, subkey);
    if (rc == 0) rc = mbedtls_chachapoly_auth_decrypt(&c, n, n12, aad, aadn, tag, ct, pt);
    mbedtls_chachapoly_free(&c);
    return rc;
}

static int aesgcm_seal(const uint8_t key[32], const uint8_t iv[12],
                       const uint8_t *aad, size_t aadn,
                       const uint8_t *pt, size_t n, uint8_t *ct, uint8_t tag[16])
{
    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0)
        rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, iv, 12,
                                       aad, aadn, pt, ct, 16, tag);
    mbedtls_gcm_free(&g);
    return rc;
}
static int aesgcm_open(const uint8_t key[32], const uint8_t iv[12],
                       const uint8_t *aad, size_t aadn, const uint8_t tag[16],
                       const uint8_t *ct, size_t n, uint8_t *pt)
{
    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0)
        rc = mbedtls_gcm_auth_decrypt(&g, n, iv, 12, aad, aadn, tag, 16, ct, pt);
    mbedtls_gcm_free(&g);
    return rc;
}


void voice_rtp_build(uint8_t hdr[12], uint16_t seq, uint32_t ts, uint32_t ssrc)
{
    hdr[0] = 0x80; hdr[1] = 0x78;
    hdr[2] = (uint8_t)(seq >> 8);  hdr[3] = (uint8_t)seq;
    hdr[4] = (uint8_t)(ts >> 24);  hdr[5] = (uint8_t)(ts >> 16);
    hdr[6] = (uint8_t)(ts >> 8);   hdr[7] = (uint8_t)ts;
    hdr[8] = (uint8_t)(ssrc >> 24);  hdr[9]  = (uint8_t)(ssrc >> 16);
    hdr[10] = (uint8_t)(ssrc >> 8);  hdr[11] = (uint8_t)ssrc;
}

size_t voice_rtp_header_len(const uint8_t *pkt, size_t n)
{
    if (n < 12) return 0;
    size_t hl = 12 + 4 * (size_t)(pkt[0] & 0x0F);
    if (pkt[0] & 0x10) hl += 4;  // extension preamble is in the clear
    return hl <= n ? hl : 0;
}


int voice_packet_seal(int mode, const uint8_t key[32], const uint8_t *hdr, size_t hdrn,
                      uint32_t counter, const uint8_t *payload, size_t plen,
                      uint8_t *out, size_t out_cap)
{
    size_t need = hdrn + plen + 16 + 4;
    if (out_cap < need) return -1;
    memcpy(out, hdr, hdrn);
    uint8_t *ct = out + hdrn, tag[16];
    int rc;
    if (mode == VOICE_MODE_AES_GCM) {
        uint8_t iv[12] = {0}; wr32be(iv, counter);
        rc = aesgcm_seal(key, iv, hdr, hdrn, payload, plen, ct, tag);
    } else {
        uint8_t n24[24] = {0}; wr32be(n24, counter);
        rc = xchacha_seal(key, n24, hdr, hdrn, payload, plen, ct, tag);
    }
    if (rc != 0) return -1;
    memcpy(out + hdrn + plen, tag, 16);
    wr32be(out + hdrn + plen + 16, counter);
    return (int)need;
}

int voice_packet_open(int mode, const uint8_t key[32], const uint8_t *pkt, size_t n,
                      uint8_t *out, size_t out_cap)
{
    size_t hl = voice_rtp_header_len(pkt, n);
    if (hl == 0 || n < hl + 16 + 4) return -1;
    size_t ctn = n - hl - 16 - 4;
    if (ctn > out_cap) return -1;
    const uint8_t *ct  = pkt + hl;
    const uint8_t *tag = pkt + hl + ctn;
    const uint8_t *ctr = pkt + n - 4;
    uint32_t counter = ((uint32_t)ctr[0] << 24) | ((uint32_t)ctr[1] << 16) |
                       ((uint32_t)ctr[2] << 8) | ctr[3];
    int rc;
    if (mode == VOICE_MODE_AES_GCM) {
        uint8_t iv[12] = {0}; wr32be(iv, counter);
        rc = aesgcm_open(key, iv, pkt, hl, tag, ct, ctn, out);
    } else {
        uint8_t n24[24] = {0}; wr32be(n24, counter);
        rc = xchacha_open(key, n24, pkt, hl, tag, ct, ctn, out);
    }
    return rc == 0 ? (int)ctn : -1;
}


int voice_crypto_selftest(void)
{
    int fail = 0;
    const uint8_t key[32] = {
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
        16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31 };
    uint8_t opus[160]; for (int i = 0; i < 160; i++) opus[i] = (uint8_t)(i * 7 + 3);

    for (int m = 0; m < 2; m++) {
        int mode = m ? VOICE_MODE_XCHACHA : VOICE_MODE_AES_GCM;
        uint8_t hdr[12]; voice_rtp_build(hdr, 0x1234, 0x000003C0u, 0xDEADBEEFu);
        uint8_t pkt[256], back[256];
        int pn = voice_packet_seal(mode, key, hdr, 12, 0x01020304u, opus, sizeof opus, pkt, sizeof pkt);
        if (pn != (int)(12 + sizeof(opus) + 16 + 4)) { fail |= (1 << (m * 2)); continue; }
        if (memcmp(pkt, hdr, 12) != 0) fail |= (1 << (m * 2));
        if (!(pkt[pn-4]==0x01 && pkt[pn-3]==0x02 && pkt[pn-2]==0x03 && pkt[pn-1]==0x04))
            fail |= (1 << (m * 2));
        int bn = voice_packet_open(mode, key, pkt, (size_t)pn, back, sizeof back);
        if (bn != (int)sizeof(opus) || memcmp(back, opus, sizeof opus) != 0)
            fail |= (1 << (m * 2 + 1));
        pkt[14] ^= 0x40;  // a flipped ciphertext byte must fail authentication
        if (voice_packet_open(mode, key, pkt, (size_t)pn, back, sizeof back) != -1)
            fail |= (1 << (m * 2 + 1));
    }

    {  // HChaCha20 KAT (vs libsodium crypto_core_hchacha20)
        static const uint8_t hk[32] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
            0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f };
        static const uint8_t hn[16] = {
            0x00,0x00,0x00,0x09,0x00,0x00,0x00,0x4a,0x00,0x00,0x00,0x00,0x31,0x41,0x59,0x27 };
        static const uint8_t want[32] = {
            0x82,0x41,0x3b,0x42,0x27,0xb2,0x7b,0xfe,0xd3,0x0e,0x42,0x50,0x8a,0x87,0x7d,0x73,
            0xa0,0xf9,0xe4,0xd5,0x8a,0x74,0xa8,0x53,0xc1,0x2e,0xc4,0x13,0x26,0xd3,0xec,0xdc };
        uint8_t got[32]; hchacha20(hk, hn, got);
        if (memcmp(got, want, 32) != 0) fail |= (1 << 4);
    }

    console_logf("[voice] crypto selftest %s (0x%02x)", fail ? "FAILED" : "ok", fail);
    return fail;
}
