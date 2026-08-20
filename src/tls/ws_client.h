#ifndef CORDLESS_TLS_WS_CLIENT_H
#define CORDLESS_TLS_WS_CLIENT_H

#include <stddef.h>

// Minimal RFC 6455 WebSocket client over mbedTLS. Non-blocking: the caller polls
// under net_lock. Control frames handled internally; ws_recv returns app messages.

typedef struct ws_client ws_client;

// Open a wss:// connection and perform the HTTP Upgrade handshake. `origin` and
// `user_agent` may be NULL. Returns a handle, or NULL on failure.
ws_client *ws_open(const char *url, const char *cacert_path,
                   const char *origin, const char *user_agent);

// Send one text frame (masked). Returns 0 on success, <0 on error.
int ws_send_text(ws_client *w, const char *txt, int len);

// Send one binary frame (masked). Returns 0 on success, <0 on error.
int ws_send_binary(ws_client *w, const unsigned char *data, int len);

// Non-blocking. Assembles one message into *buf (grown as needed, NUL-terminated),
// returning its length; 0 if none ready yet, <0 on error / close.
int ws_recv(ws_client *w, char **buf, size_t *cap);

// Like ws_recv, but reports a binary frame (opcode 0x2) via *is_binary (may be NULL).
// For binary, use the returned length: the payload may contain NULs.
int ws_recv_ex(ws_client *w, char **buf, size_t *cap, int *is_binary);

// Status code from the most recent server close frame (e.g. 4004 = auth failed), or 0
// if none. Valid after ws_recv() returns <0; read it before ws_close() frees the handle.
int ws_last_close_code(ws_client *w);

// Send a close frame (best-effort) and tear everything down.
void ws_close(ws_client *w);

#endif  // CORDLESS_TLS_WS_CLIENT_H
