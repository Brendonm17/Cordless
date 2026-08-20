#ifndef CORDLESS_VOICE_VOICE_CRYPTO_H
#define CORDLESS_VOICE_VOICE_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

// Discord voice TRANSPORT crypto + RTP framing (mbedTLS-only, host-verifiable).
// packet = [RTP hdr | ciphertext | tag 16 | counter BE 4]; key = 32B secret_key.

#define VOICE_MODE_AES_GCM  1  // aead_aes256_gcm_rtpsize
#define VOICE_MODE_XCHACHA  2  // aead_xchacha20_poly1305_rtpsize

// Map a Session-Description mode string to VOICE_MODE_*. Unknown -> XCHACHA.
int voice_crypto_mode_id(const char *mode);

// Build the 12-byte send RTP header (byte0=0x80, PT=0x78; seq/ts/ssrc big-endian).
void voice_rtp_build(uint8_t hdr[12], uint16_t seq, uint32_t ts, uint32_t ssrc);

// rtpsize unencrypted-header length for a received packet (0 if malformed).
size_t voice_rtp_header_len(const uint8_t *pkt, size_t n);

// Seal a payload into a full outgoing packet: [hdr | ciphertext | tag16 | ctr_be4].
// hdr (hdrn bytes) is also the AAD. Returns packet length or -1.
int voice_packet_seal(int mode, const uint8_t key[32], const uint8_t *hdr, size_t hdrn,
                      uint32_t counter, const uint8_t *payload, size_t plen,
                      uint8_t *out, size_t out_cap);

// Open an incoming packet -> decrypted payload (any RTP extension body is still
// prefixed; caller skips ext_len*4). Returns plaintext length, or -1 on failure.
int voice_packet_open(int mode, const uint8_t key[32], const uint8_t *pkt, size_t n,
                      uint8_t *out, size_t out_cap);

// Offline self-test: known-answer HChaCha20 vector + full-packet encrypt->decrypt
// round-trips (both modes) + tamper rejection. 0 = all pass, else a bitmask.
int voice_crypto_selftest(void);

#endif  // CORDLESS_VOICE_VOICE_CRYPTO_H
