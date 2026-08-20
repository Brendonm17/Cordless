#ifndef CORDLESS_API_DISCORD_H
#define CORDLESS_API_DISCORD_H

#include <stddef.h>

#include <jansson.h>

#include "model/store.h"

#define DISCORD_API_BASE "https://discord.com/api/v10"

const char *discord_user_agent(void);

// Both write NUL-terminated into out and return the length, or -1.
int discord_superprops_json(char *out, size_t cap);
int discord_superprops_b64(char *out, size_t cap);

// GET /experiments: fills the fingerprint and Cloudflare cookies. 0 on ok.
int discord_auth_preflight(const char *cacert_path, char *fingerprint, size_t fp_cap,
                           char *cookies, size_t ck_cap);

typedef struct {
    char id[32];
    char username[64];  // the @handle
    char global_name[64];  // may be empty
    char avatar[40];  // hash, may be empty
} discord_user;

// Returns the HTTP status (<0 = transport error); 200 fills user.
int discord_get_me(const char *token, const char *cacert_path,
                   discord_user *user);

// Loads a channel's newest 50 messages into the store; a non-empty around_id
// centers the window on it instead.
int discord_get_messages(const char *channel_id, const char *around_id,
                         const char *token, const char *cacert_path);
// Loads older history before before_id; prepends. Returns the count prepended.
int discord_get_messages_before(const char *channel_id, const char *before_id,
                                const char *token, const char *cacert_path);

int discord_send_message(const char *channel_id, const char *content,
                         const char *token, const char *cacert_path);

int discord_send_file(const char *channel_id, const char *content,
                      const char *file_path, const char *token,
                      const char *cacert_path);

int discord_send_bytes(const char *channel_id, const char *content,
                       const void *data, int len, const char *filename,
                       const char *token, const char *cacert_path);

int discord_send_reply(const char *channel_id, const char *content,
                       const char *reply_to_id, const char *token,
                       const char *cacert_path);
int discord_edit_message(const char *channel_id, const char *message_id,
                         const char *content, const char *token,
                         const char *cacert_path);
int discord_delete_message_api(const char *channel_id, const char *message_id,
                               const char *token, const char *cacert_path);
int discord_react(const char *channel_id, const char *message_id,
                  const char *emoji, bool add, const char *token,
                  const char *cacert_path);
int discord_ack_message(const char *channel_id, const char *message_id,
                        const char *token, const char *cacert_path);
// Filled by the profile fetch when extra is non-NULL (mutual guilds) and
// guild_id is non-empty (the target's roles in it). IDs only.
typedef struct {
    char guild_ids[16][24]; int n_guilds;
    char role_ids[24][24];  int n_roles;
    char banner[64];
} discord_profile_extra;
int discord_get_user_profile(const char *user_id, const char *guild_id,
                             const char *token, const char *cacert_path,
                             char *bio, size_t bio_cap, unsigned *accent,
                             discord_profile_extra *extra);
int discord_set_bio(const char *bio, const char *token, const char *cacert_path);
int discord_set_global_name(const char *name, const char *token, const char *cacert_path);
int discord_set_avatar(const char *file_path, const char *token, const char *cacert_path);
int discord_poll_vote(const char *channel_id, const char *message_id, int answer_id,
                      const char *token, const char *cacert_path);
int discord_create_forum_post(const char *forum_id, const char *title,
                              const char *content, const char *tag_id,
                              const char *token, const char *cacert_path);
int discord_get_forum_tags(const char *forum_id, char ids[][24], char names[][40],
                           int max, const char *token, const char *cacert_path);
int discord_pin_message(const char *channel_id, const char *message_id, int pin,
                        const char *token, const char *cacert_path);
int discord_get_pins(const char *channel_id, message *out, int max,
                     const char *token, const char *cacert_path);
int discord_get_active_threads(const char *guild_id, const char *parent_id,
                               channel *out, int max, const char *token,
                               const char *cacert_path);
int discord_search_messages(int guild_scope, const char *scope_id, const char *query,
                            message *out, int max, const char *token,
                            const char *cacert_path);

// type: 1 friend, 3 incoming pending, 4 outgoing pending.
typedef struct {
    char id[24];  // the other user's id
    char name[80];
    char avatar[40];
    int  type;
} discord_relationship;
int discord_get_relationships(discord_relationship *out, int max,
                              const char *token, const char *cacert_path);
// remove unfriends, denies an incoming, or cancels an outgoing request.
int discord_friend_add(const char *username, const char *token, const char *cacert_path);
int discord_friend_accept(const char *user_id, const char *token, const char *cacert_path);
int discord_friend_remove(const char *user_id, const char *token, const char *cacert_path);
// Unblock uses discord_friend_remove.
int discord_friend_block(const char *user_id, const char *token, const char *cacert_path);
int discord_set_note(const char *user_id, const char *note,
                     const char *token, const char *cacert_path);
// The joined guild arrives via a GUILD_CREATE gateway event.
#define DISCORD_JOIN_CAPTCHA (-1000)  // invite accept demands a captcha
int discord_join_invite(const char *code, const char *token, const char *cacert_path);
int discord_leave_guild(const char *guild_id, const char *token, const char *cacert_path);
int discord_open_dm(const char *recipient_id, char *out, size_t cap,
                    const char *token, const char *cacert_path);
int discord_set_guild_muted(const char *guild_id, int muted,
                            const char *token, const char *cacert_path);
int discord_create_invite(const char *channel_id, char *out_code, size_t cap,
                          const char *token, const char *cacert_path);
int discord_kick_member(const char *guild_id, const char *user_id,
                        const char *token, const char *cacert_path);
int discord_ban_member(const char *guild_id, const char *user_id,
                       const char *token, const char *cacert_path);
int discord_timeout_member(const char *guild_id, const char *user_id,
                           const char *iso_until, const char *token, const char *cacert_path);
int discord_member_role(const char *guild_id, const char *user_id, const char *role_id,
                        int add, const char *token, const char *cacert_path);
int discord_create_guild(const char *name, const char *token, const char *cacert_path);

typedef struct {
    char code[40];
    char guild_id[24];
    char name[96];
    char icon[40];
    char splash[40];
    int  members;  // approximate
    int  online;  // approximate
} discord_invite;
int discord_get_invite(const char *code, discord_invite *out,
                       const char *token, const char *cacert_path);
int discord_create_thread(const char *channel_id, const char *message_id,
                          const char *name, const char *token, const char *cacert_path);
int discord_crosspost(const char *channel_id, const char *message_id,
                      const char *token, const char *cacert_path);
int discord_forward_message(const char *to_channel_id, const char *from_channel_id,
                            const char *message_id, const char *token,
                            const char *cacert_path);

void discord_json_to_message(json_t *m, message *out);

#endif  // CORDLESS_API_DISCORD_H
