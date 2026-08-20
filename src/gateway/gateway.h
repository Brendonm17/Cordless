#ifndef CORDLESS_GATEWAY_GATEWAY_H
#define CORDLESS_GATEWAY_GATEWAY_H

#include <stdbool.h>

// Gateway client on a worker thread; dispatches events into the shared store.

int  gateway_start(const char *token, const char *cacert_path);
void gateway_stop(void);

// True after a 4004 close (the saved token is no longer valid).
bool gateway_auth_failed(void);

// Op 14 member-list request for a guild+channel; the worker thread sends it.
void gateway_request_members(const char *guild_id, const char *channel_id);

// Op 3 presence: "online" | "idle" | "dnd" | "invisible".
void gateway_set_status(const char *status);
// Empty text clears the custom status.
void gateway_set_custom_status(const char *text);

// Op 4 Voice State Update; an empty channel_id leaves the voice channel.
void gateway_voice_update(const char *guild_id, const char *channel_id);

#endif  // CORDLESS_GATEWAY_GATEWAY_H
