#ifndef CORDLESS_MODEL_STORE_H
#define CORDLESS_MODEL_STORE_H

#include <stdbool.h>

// Shared, mutex-protected model of the user's Discord state. Bracket all
// access with store_lock()/store_unlock().

#define MAX_GUILDS      100
#define MAX_CHANNELS    500  // per guild
#define MAX_DMS         100
#define MAX_MESSAGES    300  // kept for the open channel (scroll-back)
#define MAX_MEMBERS     200  // member-list rows for the open guild
#define MAX_FRIENDS     512  // friends list (relationship type 1)

// Discord channel types we care about.
#define CH_TEXT            0
#define CH_DM              1
#define CH_VOICE           2
#define CH_GROUP_DM        3
#define CH_CATEGORY        4
#define CH_ANNOUNCE        5
#define CH_ANNOUNCE_THREAD 10
#define CH_PUBLIC_THREAD   11
#define CH_PRIVATE_THREAD  12
#define CH_FORUM           15

static inline int ch_is_thread(int type)
{
    return type == CH_ANNOUNCE_THREAD || type == CH_PUBLIC_THREAD ||
           type == CH_PRIVATE_THREAD;
}

typedef struct {
    char id[24];
    char name[80];
    char topic[256];  // empty if none
    int  type;
    int  position;
    char parent_id[24];  // category id, empty if none
    char avatar[40];  // DM recipient avatar hash (DMs only)
    char recipient_id[24];  // DM recipient user id (DMs only)
    char last_message_id[24];  // newest message id in the channel
    char last_read_id[24];  // last message the user has read (from read_state)
    bool unread;  // has messages newer than we've read
    int  mentions;  // unread @mentions
    bool hidden;  // user lacks VIEW_CHANNEL (permission gating)
} channel;

typedef struct {
    char    id[24];
    char    name[96];
    char    icon[40];  // icon hash, empty if none
    char    banner[40];  // banner hash, empty if none
    char    description[256];  // empty if none
    int     premium_tier;  // boost level 0-3
    int     premium_subscription_count;  // number of boosts
    int     member_count;  // total members (approx, if provided)
    unsigned long long my_perms;  // our computed guild base perms (ADMIN => all)
    char    role_ids[48][24];  // this guild's role ids (resolve via store_role_*)
    int     role_count;
    channel channels[MAX_CHANNELS];
    int     channel_count;
} guild;

#define MAX_REACTIONS 16
#define MAX_POLL_ANSWERS 10

typedef struct {
    char name[64];  // unicode emoji chars, or custom-emoji name
    char emoji_id[24];  // custom emoji id; empty for unicode emoji
    bool animated;  // custom animated emoji (gif)
    int  count;
    bool me;  // current user has reacted with this
} reaction;

typedef struct {
    char id[24];
    char channel_id[24];  // the channel this message is in (for search jumps)
    char author[80];  // display name
    char author_id[24];
    bool is_bot;  // author is a bot/app (shows a BOT tag)
    char avatar[40];  // avatar hash, empty if none
    char content[2048];
    char timestamp[40];  // ISO8601 from Discord
    int  type;  // 0 = normal, 7 = member join, 19 = reply, etc.
    bool is_reply;
    char reply_author[80];  // who the reply is to
    char reply_content[160];
    char reply_to_id[24];  // referenced message id (tap the preview to jump)
    bool edited;
    bool mentions_me;  // @-mentions the current user (or @everyone/@here)
    bool has_image;  // first attachment is a PNG/JPEG image
    char image_url[600];  // proxy/base URL (external-proxy URLs are long)
    int  img_w, img_h;
    bool has_file;  // first attachment is a non-image file
    char file_name[96];
    long long file_size;  // bytes
    bool has_video;  // first attachment is a video (mp4/webm/mov)
    char video_url[600];  // direct video URL (poster still is in image_url)
    bool has_invite;  // content has a discord.gg / invite link
    char invite_code[40];  // the invite code (rendered as a rich tile)
    reaction reactions[MAX_REACTIONS];
    int  reaction_count;
    // Rich embed (link previews, bot cards). The embed image reuses has_image/
    // image_url above and renders inside the card.
    bool has_embed;
    unsigned embed_color;  // accent bar color (ARGB from Discord, 0 = default)
    char embed_author[96];
    char embed_title[160];
    char embed_desc[640];
    // Embed fields (name/value pairs, e.g. bot cards). inline_ = render side by
    // side with the previous inline field.
    int  embed_field_count;
    struct { char name[96]; char value[240]; bool inline_; } embed_fields[8];
    bool pending;  // optimistic local echo not yet confirmed
    // Poll (message.poll). Answers carry live vote counts + whether we voted.
    bool has_poll;
    char poll_question[200];
    bool poll_finalized;
    int  poll_total;
    int  poll_answer_count;
    struct { int id; char text[96]; int count; bool me; } poll_answers[MAX_POLL_ANSWERS];
} message;

void store_init(void);

void store_lock(void);
void store_unlock(void);

guild *store_guilds(int *count_out);  // returns array; hold the lock
void   store_clear_guilds(void);
guild *store_add_guild(const char *id, const char *name, const char *icon);
void   store_add_channel(guild *g, const char *id, const char *name,
                         int type, int position, const char *parent_id,
                         const char *last_message_id);
bool   store_ready(void);
void   store_set_ready(bool r);

// Saved sidebar order (guild ids in display order) from the user's settings.
// Empty until fetched; unlisted guilds fall back to load order.
void        store_set_guild_order(const char (*ids)[24], int n);
int         store_guild_order_count(void);
const char *store_guild_order_id(int i);  // NULL if out of range

void     store_clear_dms(void);
void     store_add_dm(const char *id, const char *name, int type,
                      const char *avatar, const char *recipient_id,
                      const char *last_message_id);
channel *store_dms(int *count_out);

void store_set_open_channel(const char *channel_id);
const char *store_open_channel(void);
void store_clear_messages(void);
// Append a fully-built message (copied into the ring buffer).
void store_push_message(const message *src);
// Insert an older message at the front (scroll-back). False if full.
bool store_prepend_message(const message *src);
const char *store_oldest_message_id(void);

// Edit / delete a message in the open channel by id (no-op if not present).
void store_update_message(const char *id, const char *content);
void store_delete_message(const char *id);

// Live reaction updates. `name` is unicode emoji chars or a custom-emoji name;
// `id` is the custom-emoji id (empty for unicode); `me` = our own reaction.
void store_reaction_add(const char *msg_id, const char *name, const char *id,
                        bool animated, bool me);
void store_reaction_remove(const char *msg_id, const char *name, const char *id,
                           bool me);
void store_reaction_clear(const char *msg_id);

// Bumped on any message list change (add/edit/delete/clear).
unsigned store_msg_rev(void);
void     store_touch(void);  // bump the message revision

// Typing indicator: the most recent typer in a channel and when (ms).
void store_set_typing(const char *channel_id, const char *name, long long at_ms);
const char *store_typing_name(void);
const char *store_typing_channel(void);
long long   store_typing_at(void);
message *store_messages(int *count_out);
void store_set_msg_loading(bool loading);
bool store_msg_loading(void);

typedef struct {
    bool is_group;  // group header vs member
    char text[64];  // group label (uppercased) or member display name
    int  count;  // group member count
    char id[24];  // member user id
    char avatar[40];  // member avatar hash
    int  status;  // 0 offline, 1 online, 2 idle, 3 dnd
} member_row;

// Role name lookup (populated from READY guild.roles).
void        store_add_role(const char *id, const char *name, unsigned rgb);
const char *store_role_name(const char *id);  // NULL if unknown
unsigned    store_role_color(const char *id);  // ABGR, 0 if none/unknown

void store_members_begin(const char *guild_id);  // clear + set guild
void store_add_member_group(const char *label, int count);
void store_add_member(const char *name, const char *id, const char *avatar, int status);
member_row *store_members(int *count_out);
const char *store_members_guild(void);

// Friends (relationship type 1 from READY). Rows carry name/id/avatar;
// status comes from store_member_status(id).
void store_friends_begin(void);
void store_add_friend(const char *name, const char *id, const char *avatar);
member_row *store_friends(int *count_out);
// Presence status of a user id: 0 offline, 1 online, 2 idle, 3 dnd, or -1
// if unknown.
int store_member_status(const char *user_id);
// Display name of a user id from the loaded member list, or NULL if not
// loaded. The caller must hold the store lock.
const char *store_member_nick(const char *user_id);
// Update a member's presence (PRESENCE_UPDATE). Returns true if an entry
// changed. status: 0 offline, 1 online, 2 idle, 3 dnd.
bool store_set_presence(const char *user_id, int status);
// Activity line ("Playing X", custom status, etc.) from PRESENCE_UPDATE, or NULL.
void        store_set_activity(const char *user_id, const char *activity);
const char *store_member_activity(const char *user_id);

// current user
void store_set_me(const char *username, const char *global_name, const char *id,
                  const char *avatar);
const char *store_me_name(void);
const char *store_me_id(void);
const char *store_me_avatar(void);

// Find a channel (in any guild, or a DM) by id. Returns NULL if absent.
channel *store_find_channel(const char *id);
guild   *store_find_guild(const char *id);
void     store_remove_channel(const char *id);
void     store_remove_guild(const char *id);
// Set a channel's permission-gating flag (hidden = user can't VIEW_CHANNEL).
void store_set_channel_hidden(const char *id, bool hidden);

// Blocked users (relationship type 2). Used to hide their messages.
void store_clear_blocked(void);
void store_set_blocked(const char *user_id, bool blocked);
bool store_is_blocked(const char *user_id);

// Personal notes on users (from READY `notes`; edited in the profile popout).
void        store_clear_notes(void);
void        store_set_note(const char *user_id, const char *note);
const char *store_get_note(const char *user_id);
// Mark a channel read / unread (updates unread + mentions).
void store_mark_read(const char *id);
void store_bump_unread(const char *id, const char *last_message_id, bool mention);

#endif  // CORDLESS_MODEL_STORE_H
