#ifndef CORDLESS_AUTH_REMOTEAUTH_H
#define CORDLESS_AUTH_REMOTEAUTH_H

#include <stdbool.h>
#include <stdint.h>

#include "qrcodegen.h"

// Discord remote-auth (QR login) client. Runs the WS state machine on a worker
// thread; publishes progress into a session the render thread reads each frame.

typedef enum {
    RA_IDLE = 0,
    RA_CONNECTING,  // opening WS, doing init/nonce handshake
    RA_WAIT_SCAN,  // QR is ready; waiting for the phone to scan
    RA_WAIT_APPROVE,  // phone scanned (user shown); waiting for Approve tap
    RA_EXCHANGING,  // approved; exchanging ticket for the token
    RA_DONE,  // token acquired (see token[])
    RA_CANCELLED,  // user declined on phone
    RA_FAILED  // error (see error[])
} ra_state;

typedef struct {
    volatile ra_state state;
    char    fingerprint[128];
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    bool    have_qr;
    char    user_display[192];  // "username (id)" from pending_ticket
    char    token[128];  // final user token on RA_DONE
    char    error[160];
} ra_session;

// Begin a login attempt: init the session and spawn the worker thread.
// cacert_path is the bundled CA bundle. Returns 0 on success (thread started).
int  remoteauth_start(ra_session *s, const char *cacert_path);

// Poll whether the worker finished (DONE/FAILED/CANCELLED). Safe each frame.
bool remoteauth_is_finished(const ra_session *s);

// Block until the worker exits and free its resources. Call once when finished.
void remoteauth_join(ra_session *s);

#endif  // CORDLESS_AUTH_REMOTEAUTH_H
