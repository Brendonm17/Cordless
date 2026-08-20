#ifndef CORDLESS_NET_COMMON_H
#define CORDLESS_NET_COMMON_H

// Brings up the Vita network stack (SceNet + pool + sceNetCtl). Returns 0 on success.
int  net_init(void);

// Polls until connected or timeout. Returns 1 if connected, 0 on timeout.
int  net_wait_for_connection(int timeout_ms);

// Non-blocking: 1 if connected right now, else 0.
int  net_is_connected(void);

void net_finish(void);

// Writes the absolute path (ux0:app/<TITLEID>/rel) of a vpk-shipped file into `out`.
void net_app_path(char *out, unsigned out_len, const char *rel);

// Hold net_lock() around every blocking network op (TLS request / ws_recv / ws_send).
void net_lock(void);
void net_unlock(void);

#endif  // CORDLESS_NET_COMMON_H
