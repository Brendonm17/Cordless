#ifndef CORDLESS_VOICE_DAVE_H
#define CORDLESS_VOICE_DAVE_H

#include <stddef.h>
#include <stdint.h>

// C-callable bridge to the C++ MLS engine (mlspp + mbedTLS HPKE shim) for DAVE
// voice E2EE. Driven by voice.c; implemented in dave.cpp.

#ifdef __cplusplus
extern "C" {
#endif

// Highest DAVE protocol version this client implements. Advertised to the voice
// gateway in IDENTIFY; 0 would mean "no E2EE".
#define DAVE_PROTOCOL_VERSION 1

// Crypto self-test: ECDSA-P256, DHKEM-P256, HPKE base-mode, and DAVE frame codec.
// Returns 0 on pass, else a non-zero bitmask of failed checks.
int dave_selftest(void);

// Opaque MLS session handle (one per voice connection).
typedef struct dave_session dave_session;

// Create an MLS session. self_user_id = our Discord user id (MLS credential +
// key derivation); group_id = voice channel id (used only if we create the group).
dave_session *dave_session_create(const char *self_user_id, uint64_t group_id);
void          dave_session_free(dave_session *s);

// True once an MLS epoch is established (media keys are derivable).
int  dave_established(dave_session *s);
// Drop all MLS state (protocol downgrade to passthrough / leaving).
void dave_reset(dave_session *s);

// Voice-gateway DAVE opcode handlers. Each returns 0 on success, -1 on error, and
// leaves session state intact on failure. Buffers are raw MLS/TLS wire bytes.
int dave_on_external_sender(dave_session *s, const uint8_t *pkg, size_t len);  // op 25 in
int dave_make_key_package(dave_session *s, uint8_t *out, size_t *len, size_t cap);  // op 26 out
int dave_prepare_epoch(dave_session *s, uint64_t epoch);  // op 24 in
// op 27 in -> MLS Commit (out) + Welcome for any added member (wout), sent as op
// 28. recognized[] (nrecognized) is accepted but not validated. 0 ok, -1 error.
int dave_on_proposals(dave_session *s, const uint8_t *data, size_t len,
                      const char *const *recognized, int nrecognized,
                      uint8_t *out, size_t *out_len, size_t cap,
                      uint8_t *wout, size_t *wout_len, size_t wcap);
int dave_on_commit(dave_session *s, const uint8_t *data, size_t len);  // op 29 in
int dave_on_welcome(dave_session *s, const uint8_t *data, size_t len,
                    const char *const *recognized, int nrecognized);  // op 30 in

// Derive the 16B AES-128 media key for a sender + ratchet generation from the MLS
// epoch (Discord Secure Frames exporter + hash ratchet). 0 ok, -1 if no epoch.
int dave_sender_key(dave_session *s, const char *sender_user_id,
                    uint8_t generation, uint8_t out_key[16]);

#ifdef __cplusplus
}
#endif

#endif  // CORDLESS_VOICE_DAVE_H
