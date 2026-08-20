#ifndef CORDLESS_VOICE_DAVE_FRAME_H
#define CORDLESS_VOICE_DAVE_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// DAVE per-frame media encryption ("OpaqueFrame"): each Opus frame is AES-128-GCM
// sealed with a 16B per-sender/generation key; 8-byte tag, ULEB128 nonce, 0xFAFA.

#define DAVE_FRAME_TAG_SIZE   8
#define DAVE_FRAME_MARKER_0   0xFA
#define DAVE_FRAME_MARKER_1   0xFA
// Worst-case overhead: 8 tag + 5 ULEB128 nonce + 1 size + 2 marker.
#define DAVE_FRAME_MAX_OVERHEAD 16

// Encrypt one Opus frame into the DAVE wire format. nonce is the 32-bit sync nonce
// (MSB = generation); out must be >= len + DAVE_FRAME_MAX_OVERHEAD. -1 on error.
int dave_frame_encrypt(const uint8_t key[16], uint32_t nonce,
                       const uint8_t *in, size_t len,
                       uint8_t *out, size_t out_cap);

// Decrypt a DAVE-wrapped frame (must end with the 0xFAFA marker) to plaintext Opus.
// Returns plaintext length (writes the nonce to nonce_out if non-NULL), -1 on fail.
int dave_frame_decrypt(const uint8_t key[16],
                       const uint8_t *in, size_t len,
                       uint8_t *out, size_t out_cap, uint32_t *nonce_out);

// Recover a wrapped frame's truncated nonce (MSB = ratchet generation) without
// decrypting. Returns 0 and fills *nonce, or -1 if not a DAVE frame (no 0xFAFA).
int dave_frame_peek_nonce(const uint8_t *in, size_t len, uint32_t *nonce);

#ifdef __cplusplus
}
#endif

#endif  // CORDLESS_VOICE_DAVE_FRAME_H
