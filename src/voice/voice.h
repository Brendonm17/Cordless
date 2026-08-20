#ifndef CORDLESS_VOICE_VOICE_H
#define CORDLESS_VOICE_VOICE_H

// Discord voice client: join handshake (main-gateway op4 -> VOICE_SERVER_UPDATE
// -> voice-gateway v8), UDP IP discovery, and the Opus + AEAD audio path.

// Build gate for voice join; 0 makes voice_join() refuse.
#ifndef VOICE_JOIN_ENABLE
#define VOICE_JOIN_ENABLE 0
#endif

// Returns 0 if the join request was accepted (the handshake then runs async).
int  voice_join(const char *guild_id, const char *channel_id,
                const char *token, const char *cacert_path);
void voice_leave(void);

// voice_active() is true from join until disconnect; voice_connected() once the
// session description arrives.
int         voice_active(void);
int         voice_connected(void);
const char *voice_channel_id(void);  // channel we're joining/in ("" if none)
const char *voice_status(void);  // short status text for the UI

// Called by the main gateway worker with the voice server assignment
// (VOICE_SERVER_UPDATE token+endpoint, VOICE_STATE_UPDATE session_id).
void voice_on_server(const char *token, const char *endpoint,
                     const char *session_id, const char *guild_id,
                     const char *user_id);

#endif  // CORDLESS_VOICE_VOICE_H
