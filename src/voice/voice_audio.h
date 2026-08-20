#ifndef CORDLESS_VOICE_VOICE_AUDIO_H
#define CORDLESS_VOICE_VOICE_AUDIO_H

#include <stddef.h>
#include <stdint.h>

// DAVE (voice E2EE) build gate shared by voice.c and voice_audio.c. 0 (default)
// advertises no E2EE; -DVOICE_DAVE_ENABLE=1 turns on the inner E2EE layer.
#ifndef VOICE_DAVE_ENABLE
#define VOICE_DAVE_ENABLE 0
#endif

// Voice audio pipeline: RTP + transport AEAD + Opus + mic/speaker, driven by
// voice.c once connected. DAVE inner E2EE wraps the payload when negotiated.

// Offline self-test (KAT vectors + packet round-trips). Returns 0 on pass, else a
// bitmask of failed checks.
int voice_audio_selftest(void);

// Start the RX+TX threads. Borrows voice.c's UDP socket and media params (must
// outlive the pipeline). on_speaking toggles op5 Speaking. Returns 0 on success.
int  voice_audio_start(int udp_fd, unsigned ssrc,
                       const unsigned char secret_key[32], const char *mode,
                       void (*on_speaking)(int on));
void voice_audio_stop(void);

#if VOICE_DAVE_ENABLE
// DAVE inner-layer key resolvers registered by voice.c (NULL restores passthrough).
// Each returns 1 with the 16B media key, or 0 to send/treat the frame as plaintext.
typedef struct {
    int (*my_key)(uint8_t generation, uint8_t key[16]);
    int (*sender_key)(unsigned ssrc, uint8_t generation, uint8_t key[16]);
} voice_dave_hooks;
void voice_audio_set_dave(const voice_dave_hooks *h);
#endif

#endif  // CORDLESS_VOICE_VOICE_AUDIO_H
