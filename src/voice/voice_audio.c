#include "voice/voice_audio.h"
#include "voice/voice_crypto.h"
#if VOICE_DAVE_ENABLE
#include "voice/dave_frame.h"
#endif
#include "console.h"
#include "net_common.h"

#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#include <opus/opus.h>
#include <psp2/audioout.h>
#include <psp2/audioin.h>
#include <psp2/kernel/threadmgr.h>

// Voice audio pipeline (Vita-only: Opus + sceAudio + threads). Transport crypto
// and RTP framing live in voice_crypto.c.

int voice_audio_selftest(void) { return voice_crypto_selftest(); }

// RX thread: drains the non-blocking UDP socket (net_lock held only per recv),
// decrypts + Opus-decodes per SSRC, mixes into 20ms stereo frames for sceAudioOut.

#define VA_FRAME_SAMPLES 960  // 20ms @ 48kHz, per channel
#define VA_MAX_SPEAKERS  8

static volatile int  s_run;
static int           s_udp = -1;  // borrowed from voice.c
static int           s_aport = -1;  // sceAudioOut port
static int           s_mode_id;
static unsigned char s_key[32];
static unsigned      s_ssrc;  // our own ssrc (ignore our loopback)
static void        (*s_on_speaking)(int);
static SceUID        s_rx_thread = -1;

static struct { unsigned ssrc; OpusDecoder *dec; } s_spk[VA_MAX_SPEAKERS];

#if VOICE_DAVE_ENABLE
// DAVE inner-layer key resolvers (from voice.c) + derived-key caches, so the hooks
// are called only on a generation change.
static const voice_dave_hooks *s_dave_hooks;

void voice_audio_set_dave(const voice_dave_hooks *h) { s_dave_hooks = h; }

// Our TX media key at the current generation.
static uint32_t s_tx_nonce;  // monotonic 32-bit truncated sync nonce
static int      s_tx_key_gen = -1;
static int      s_tx_key_ok;
static uint8_t  s_tx_key[16];

// Per-remote-SSRC RX media-key cache (ssrc==0 => unused slot).
static struct { unsigned ssrc; int gen; uint8_t key[16]; int ok; } s_rxkey[VA_MAX_SPEAKERS];

static int rxkey_get(unsigned ssrc, uint8_t gen, uint8_t out[16])
{
    if (!s_dave_hooks || !s_dave_hooks->sender_key) return 0;
    int slot = -1;
    for (int i = 0; i < VA_MAX_SPEAKERS; i++)
        if (s_rxkey[i].ssrc == ssrc) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < VA_MAX_SPEAKERS; i++)
            if (s_rxkey[i].ssrc == 0) {
                slot = i; s_rxkey[i].ssrc = ssrc; s_rxkey[i].gen = -1; s_rxkey[i].ok = 0; break;
            }
    if (slot < 0) return 0;  // cache full
    if (!s_rxkey[slot].ok || s_rxkey[slot].gen != (int)gen) {  // (re)derive only on change
        s_rxkey[slot].ok  = s_dave_hooks->sender_key(ssrc, gen, s_rxkey[slot].key);
        s_rxkey[slot].gen = (int)gen;
    }
    if (!s_rxkey[slot].ok) return 0;
    memcpy(out, s_rxkey[slot].key, 16);
    return 1;
}
#endif  // VOICE_DAVE_ENABLE

static OpusDecoder *decoder_for(unsigned ssrc)
{
    for (int i = 0; i < VA_MAX_SPEAKERS; i++)
        if (s_spk[i].dec && s_spk[i].ssrc == ssrc) return s_spk[i].dec;
    for (int i = 0; i < VA_MAX_SPEAKERS; i++)
        if (!s_spk[i].dec) {
            int err = 0;
            OpusDecoder *d = opus_decoder_create(48000, 2, &err);
            if (d) { s_spk[i].dec = d; s_spk[i].ssrc = ssrc; }
            return d;
        }
    return NULL;  // table full: ignore further speakers
}

static int rx_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    static uint8_t pkt[1600], plain[1600];
    static int16_t frame[VA_FRAME_SAMPLES * 2], mix[VA_FRAME_SAMPLES * 2];

    while (s_run) {
        memset(mix, 0, sizeof mix);
        for (int guard = 0; guard < 64 && s_run; guard++) {  // drain this tick's packets
            net_lock();
            int n = (int)recv(s_udp, pkt, sizeof pkt, 0);  // O_NONBLOCK: returns fast
            net_unlock();
            if (n <= 0) break;
            if (n < 12) continue;
            int ptype = pkt[1] & 0x7F;
            if (ptype >= 72 && ptype <= 76) continue;  // RTCP, not Opus
            unsigned ssrc = ((unsigned)pkt[8] << 24) | ((unsigned)pkt[9] << 16) |
                            ((unsigned)pkt[10] << 8) | pkt[11];
            if (ssrc == s_ssrc) continue;  // our own loopback
            int plen = voice_packet_open(s_mode_id, s_key, pkt, (size_t)n, plain, sizeof plain);
            if (plen < 0) continue;  // auth fail / malformed
            const uint8_t *opus = plain; int olen = plen;
            if (pkt[0] & 0x10) {  // extension body is encrypted
                size_t hl = voice_rtp_header_len(pkt, (size_t)n);
                if (hl >= 4) {
                    int words = ((int)pkt[hl - 2] << 8) | pkt[hl - 1];
                    int skip = words * 4;
                    if (skip >= 0 && skip <= olen) { opus += skip; olen -= skip; }
                }
            }
            if (olen <= 0) continue;
#if VOICE_DAVE_ENABLE
            // Inner E2EE: if this is a DAVE frame (0xFAFA trailer) with keys,
            // unwrap to plaintext Opus; a non-DAVE payload passes through.
            static uint8_t dave_plain[1600];
            if (s_dave_hooks && s_dave_hooks->sender_key) {
                uint32_t fnonce;
                if (dave_frame_peek_nonce(opus, (size_t)olen, &fnonce) == 0) {
                    uint8_t key[16];
                    if (!rxkey_get(ssrc, (uint8_t)(fnonce >> 24), key))
                        continue;  // no key for sender/gen: drop
                    int dl = dave_frame_decrypt(key, opus, (size_t)olen,
                                                dave_plain, sizeof dave_plain, NULL);
                    if (dl <= 0) continue;  // E2EE auth fail: drop
                    opus = dave_plain; olen = dl;
                }
            }
#endif
            OpusDecoder *d = decoder_for(ssrc);
            if (!d) continue;
            int got = opus_decode(d, opus, olen, frame, VA_FRAME_SAMPLES, 0);
            if (got <= 0) continue;
            int ns = got * 2; if (ns > VA_FRAME_SAMPLES * 2) ns = VA_FRAME_SAMPLES * 2;
            for (int i = 0; i < ns; i++) {  // sum + clip
                int v = (int)mix[i] + (int)frame[i];
                mix[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
        }
        if (s_aport >= 0) sceAudioOutOutput(s_aport, mix);  // blocks ~20ms -> paces us
        else sceKernelDelayThread(20 * 1000);
    }
    return 0;
}

// TX: Vita mic (16kHz mono) upsampled to 48kHz, framed to 20ms, energy-VAD gated,
// Opus-encoded -> sealed -> UDP. Five 0xF8FFFE silence frames end each utterance.

#define VA_MIC_GRAIN   256  // sceAudioIn VOICE grain @ 16kHz (16ms)
#define VA_VAD_MS2     700000  // mean-square energy gate
#define VA_VAD_HANG    25  // quiet frames before we stop (~500ms)

static SceUID       s_tx_thread = -1;
static int          s_mic = -1;
static OpusEncoder *s_enc;

static void tx_send(const uint8_t *payload, int len, uint16_t *seq, uint32_t *ts, uint32_t *ctr)
{
    uint8_t hdr[12], pkt[1400];
#if VOICE_DAVE_ENABLE
    // Inner E2EE: wrap the Opus payload with the DAVE frame codec at our per-
    // generation key before transport seal; passthrough until a key is established.
    uint8_t wrapped[1400];
    if (s_dave_hooks && s_dave_hooks->my_key) {
        uint8_t gen = (uint8_t)(s_tx_nonce >> 24);
        if (!s_tx_key_ok || s_tx_key_gen != (int)gen) {
            s_tx_key_ok  = s_dave_hooks->my_key(gen, s_tx_key);
            s_tx_key_gen = (int)gen;
        }
        if (s_tx_key_ok) {
            int wl = dave_frame_encrypt(s_tx_key, s_tx_nonce, payload, (size_t)len,
                                        wrapped, sizeof wrapped);
            if (wl > 0) { payload = wrapped; len = wl; s_tx_nonce++; }
        }
    }
#endif
    voice_rtp_build(hdr, *seq, *ts, s_ssrc);
    int pn = voice_packet_seal(s_mode_id, s_key, hdr, 12, *ctr, payload, (size_t)len, pkt, sizeof pkt);
    if (pn > 0) { net_lock(); send(s_udp, pkt, (size_t)pn, 0); net_unlock(); }
    *seq = (uint16_t)(*seq + 1); *ts += VA_FRAME_SAMPLES; (*ctr)++;  // ctr (nonce) never reused
}

static void tx_silence(uint16_t *seq, uint32_t *ts, uint32_t *ctr)
{
    static const uint8_t sil[3] = { 0xF8, 0xFF, 0xFE };
    for (int i = 0; i < 5; i++) tx_send(sil, 3, seq, ts, ctr);
}

static int tx_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    int16_t in16[VA_MIC_GRAIN];
    static int16_t acc[4096]; int accn = 0;
    uint8_t opus[1024];
    uint16_t seq = 0; uint32_t ts = 0, ctr = 1;
    int speaking = 0, quiet = 0;

    while (s_run) {
        if (s_mic < 0 || !s_enc) { sceKernelDelayThread(20 * 1000); continue; }
        if (sceAudioInInput(s_mic, in16) < 0) { sceKernelDelayThread(10 * 1000); continue; }
        for (int i = 0; i < VA_MIC_GRAIN && accn + 3 <= (int)(sizeof(acc)/sizeof(acc[0])); i++) {
            int a = in16[i], b = (i + 1 < VA_MIC_GRAIN) ? in16[i + 1] : in16[i];
            acc[accn++] = (int16_t)a;  // 16k -> 48k linear
            acc[accn++] = (int16_t)(a + (b - a) / 3);
            acc[accn++] = (int16_t)(a + 2 * (b - a) / 3);
        }
        while (accn >= VA_FRAME_SAMPLES && s_run) {
            long long e = 0;
            for (int i = 0; i < VA_FRAME_SAMPLES; i++) { int v = acc[i]; e += (long long)v * v; }
            int active = (e / VA_FRAME_SAMPLES) > VA_VAD_MS2;
            if (active || (speaking && quiet < VA_VAD_HANG)) {
                if (!speaking) { speaking = 1; quiet = 0; if (s_on_speaking) s_on_speaking(1); }
                quiet = active ? 0 : quiet + 1;
                int n = opus_encode(s_enc, acc, VA_FRAME_SAMPLES, opus, sizeof opus);
                if (n > 0) tx_send(opus, n, &seq, &ts, &ctr);
            } else if (speaking) {  // trailing quiet -> stop
                tx_silence(&seq, &ts, &ctr);
                speaking = 0; quiet = 0;
                if (s_on_speaking) s_on_speaking(0);
            }
            memmove(acc, acc + VA_FRAME_SAMPLES, (accn - VA_FRAME_SAMPLES) * sizeof(acc[0]));
            accn -= VA_FRAME_SAMPLES;
        }
    }
    if (speaking) { tx_silence(&seq, &ts, &ctr); if (s_on_speaking) s_on_speaking(0); }
    return 0;
}

int voice_audio_start(int udp_fd, unsigned ssrc, const unsigned char secret_key[32],
                      const char *mode, void (*on_speaking)(int on))
{
    if (udp_fd < 0) return -1;
    s_udp = udp_fd;
    s_ssrc = ssrc;
    memcpy(s_key, secret_key, 32);
    s_mode_id = voice_crypto_mode_id(mode);
    s_on_speaking = on_speaking;
    memset(s_spk, 0, sizeof s_spk);
#if VOICE_DAVE_ENABLE
    memset(s_rxkey, 0, sizeof s_rxkey);
    s_tx_nonce = 0; s_tx_key_gen = -1; s_tx_key_ok = 0;
#endif

    // Non-blocking so RX recv never holds net_lock across a wait.
    int fl = fcntl(s_udp, F_GETFL, 0);
    if (fl >= 0) fcntl(s_udp, F_SETFL, fl | O_NONBLOCK);

    s_aport = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, VA_FRAME_SAMPLES,
                                  48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (s_aport < 0) console_logf("[voice] audio out open failed 0x%08x", (unsigned)s_aport);

    s_run = 1;
    s_rx_thread = sceKernelCreateThread("cl_voice_rx", rx_worker, 0x40, 0x10000, 0,
                                        SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s_rx_thread < 0) { voice_audio_stop(); return -1; }
    sceKernelStartThread(s_rx_thread, 0, NULL);

    // TX: Opus encoder (mono 48k) + mic (VOICE port, 16kHz mono).
    int eerr = 0;
    s_enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &eerr);
    if (s_enc) {
        opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(64000));
        opus_encoder_ctl(s_enc, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(s_enc, OPUS_SET_PACKET_LOSS_PERC(15));
        opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(5));
    } else console_logf("[voice] opus encoder create failed %d", eerr);
    s_mic = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, VA_MIC_GRAIN, 16000,
                               SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (s_mic < 0) console_logf("[voice] mic open failed 0x%08x", (unsigned)s_mic);
    s_tx_thread = sceKernelCreateThread("cl_voice_tx", tx_worker, 0x40, 0x10000, 0,
                                        SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s_tx_thread >= 0) sceKernelStartThread(s_tx_thread, 0, NULL);

    console_dbgf("[voice] audio started (mode=%d out=%d mic=%d)", s_mode_id, s_aport, s_mic);
    return 0;
}

void voice_audio_stop(void)
{
    s_run = 0;
    if (s_tx_thread >= 0) {
        sceKernelWaitThreadEnd(s_tx_thread, NULL, NULL);
        sceKernelDeleteThread(s_tx_thread);
        s_tx_thread = -1;
    }
    if (s_rx_thread >= 0) {
        sceKernelWaitThreadEnd(s_rx_thread, NULL, NULL);
        sceKernelDeleteThread(s_rx_thread);
        s_rx_thread = -1;
    }
    if (s_mic >= 0) { sceAudioInReleasePort(s_mic); s_mic = -1; }
    if (s_enc) { opus_encoder_destroy(s_enc); s_enc = NULL; }
    for (int i = 0; i < VA_MAX_SPEAKERS; i++)
        if (s_spk[i].dec) { opus_decoder_destroy(s_spk[i].dec); s_spk[i].dec = NULL; }
    if (s_aport >= 0) { sceAudioOutReleasePort(s_aport); s_aport = -1; }
    s_udp = -1;
}
