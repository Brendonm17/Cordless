#include "model/store.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <psp2/kernel/threadmgr.h>

// Global presence-map capacity (~156 bytes per entry).
#define PRESENCE_MAX 4096

typedef struct {
    guild   guilds[MAX_GUILDS];
    int     guild_count;
    channel dms[MAX_DMS];
    int     dm_count;
    bool    ready;

    char    open_channel[24];
    message messages[MAX_MESSAGES];
    int     message_count;
    bool    msg_loading;

    char    me_name[96];
    char    me_id[24];
    char    me_avatar[40];

    unsigned  msg_rev;
    char      typing_name[80];
    char      typing_channel[24];
    long long typing_at;

    member_row members[MAX_MEMBERS];
    int        member_count;
    char       member_guild[24];

    // Friends (relationship type 1 from READY): name/id/avatar only; status
    // comes from the presence map below.
    member_row friends[MAX_FRIENDS];
    int        friend_count;

    // Global role map (ids are unique snowflakes).
    struct { char id[24]; char name[40]; unsigned color; } roles[4096];
    int        role_count;

    // Presence by user id, seeded from READY_SUPPLEMENTAL and updated by
    // PRESENCE_UPDATE.
    struct { char id[24]; int status; char activity[128]; } presence[PRESENCE_MAX];
    int        presence_count;

    // Blocked user ids (relationship type 2), from READY + live updates.
    char       blocked[128][24];
    int        blocked_count;

    // Personal notes on users (from READY `notes`, edited via the profile popout).
    struct { char id[24]; char text[256]; } notes[64];
    int        notes_count;

    // Saved sidebar order (guild ids) from the user's settings (guild_folders).
    // Empty until fetched; the rail then uses load order.
    char       guild_order[MAX_GUILDS][24];
    int        guild_order_n;
} store_t;

static store_t *S;
static SceUID   s_mutex;

void store_init(void)
{
    S = calloc(1, sizeof(*S));
    s_mutex = sceKernelCreateMutex("cordless_store", 0, 0, NULL);
}

void store_lock(void)   { sceKernelLockMutex(s_mutex, 1, NULL); }
void store_unlock(void) { sceKernelUnlockMutex(s_mutex, 1); }

guild *store_guilds(int *count_out)
{
    if (count_out) *count_out = S->guild_count;
    return S->guilds;
}

void store_clear_guilds(void)
{
    S->guild_count = 0;
    S->ready = false;
}

guild *store_add_guild(const char *id, const char *name, const char *icon)
{
    // Dedupe by id: reuse the slot and let the caller repopulate its channels.
    if (id && id[0])
        for (int i = 0; i < S->guild_count; i++)
            if (strcmp(S->guilds[i].id, id) == 0) {
                guild *g = &S->guilds[i];
                if (name && name[0]) snprintf(g->name, sizeof(g->name), "%s", name);
                snprintf(g->icon, sizeof(g->icon), "%s", icon ? icon : "");
                g->channel_count = 0;
                return g;
            }
    if (S->guild_count >= MAX_GUILDS) return NULL;
    guild *g = &S->guilds[S->guild_count++];
    memset(g, 0, sizeof(*g));
    snprintf(g->id, sizeof(g->id), "%s", id ? id : "");
    snprintf(g->name, sizeof(g->name), "%s", name ? name : "");
    snprintf(g->icon, sizeof(g->icon), "%s", icon ? icon : "");
    return g;
}

void store_add_channel(guild *g, const char *id, const char *name,
                       int type, int position, const char *parent_id,
                       const char *last_message_id)
{
    if (!g || g->channel_count >= MAX_CHANNELS) return;
    channel *c = &g->channels[g->channel_count++];
    memset(c, 0, sizeof(*c));
    snprintf(c->id, sizeof(c->id), "%s", id ? id : "");
    snprintf(c->name, sizeof(c->name), "%s", name ? name : "");
    c->type = type;
    c->position = position;
    if (parent_id) snprintf(c->parent_id, sizeof(c->parent_id), "%s", parent_id);
    if (last_message_id)
        snprintf(c->last_message_id, sizeof(c->last_message_id), "%s", last_message_id);
}

bool store_ready(void)          { return S->ready; }
void store_set_ready(bool r)    { S->ready = r; }

// Saved sidebar order (guild ids). Written once by the settings-fetch thread.
void store_set_guild_order(const char (*ids)[24], int n)
{
    if (n < 0) n = 0;
    if (n > MAX_GUILDS) n = MAX_GUILDS;
    store_lock();
    for (int i = 0; i < n; i++) snprintf(S->guild_order[i], 24, "%s", ids[i]);
    S->guild_order_n = n;
    store_unlock();
}
int store_guild_order_count(void) { return S->guild_order_n; }
const char *store_guild_order_id(int i)
{
    return (i >= 0 && i < S->guild_order_n) ? S->guild_order[i] : NULL;
}

void store_clear_dms(void) { S->dm_count = 0; }

void store_add_dm(const char *id, const char *name, int type,
                  const char *avatar, const char *recipient_id,
                  const char *last_message_id)
{
    if (S->dm_count >= MAX_DMS) return;
    channel *c = &S->dms[S->dm_count++];
    memset(c, 0, sizeof(*c));
    snprintf(c->id, sizeof(c->id), "%s", id ? id : "");
    snprintf(c->name, sizeof(c->name), "%s", name ? name : "Direct Message");
    c->type = type;
    snprintf(c->avatar, sizeof(c->avatar), "%s", avatar ? avatar : "");
    snprintf(c->recipient_id, sizeof(c->recipient_id), "%s", recipient_id ? recipient_id : "");
    if (last_message_id)
        snprintf(c->last_message_id, sizeof(c->last_message_id), "%s", last_message_id);
}

channel *store_dms(int *count_out)
{
    if (count_out) *count_out = S->dm_count;
    return S->dms;
}

void store_set_open_channel(const char *channel_id)
{
    snprintf(S->open_channel, sizeof(S->open_channel), "%s",
             channel_id ? channel_id : "");
}
const char *store_open_channel(void) { return S->open_channel; }

void store_clear_messages(void) { S->message_count = 0; S->msg_rev++; }

void store_update_message(const char *id, const char *content)
{
    if (!id) return;
    for (int i = 0; i < S->message_count; i++) {
        if (strcmp(S->messages[i].id, id) == 0) {
            if (content)
                snprintf(S->messages[i].content, sizeof(S->messages[i].content),
                         "%s", content);
            S->messages[i].edited = true;
            S->msg_rev++;
            return;
        }
    }
}

void store_delete_message(const char *id)
{
    if (!id) return;
    for (int i = 0; i < S->message_count; i++) {
        if (strcmp(S->messages[i].id, id) == 0) {
            memmove(&S->messages[i], &S->messages[i + 1],
                    sizeof(message) * (S->message_count - i - 1));
            S->message_count--;
            S->msg_rev++;
            return;
        }
    }
}

static bool rx_match(const reaction *r, const char *name, const char *id)
{
    if (id && id[0]) return strcmp(r->emoji_id, id) == 0;
    return r->emoji_id[0] == 0 && strcmp(r->name, name ? name : "") == 0;
}

void store_reaction_add(const char *msg_id, const char *name, const char *id,
                        bool animated, bool me)
{
    if (!msg_id) return;
    for (int i = 0; i < S->message_count; i++) {
        message *m = &S->messages[i];
        if (strcmp(m->id, msg_id) != 0) continue;
        for (int j = 0; j < m->reaction_count; j++) {
            if (rx_match(&m->reactions[j], name, id)) {
                m->reactions[j].count++;
                if (me) m->reactions[j].me = true;
                S->msg_rev++;
                return;
            }
        }
        if (m->reaction_count < MAX_REACTIONS) {
            reaction *r = &m->reactions[m->reaction_count++];
            memset(r, 0, sizeof(*r));
            snprintf(r->name, sizeof(r->name), "%s", name ? name : "");
            snprintf(r->emoji_id, sizeof(r->emoji_id), "%s", id ? id : "");
            r->animated = animated;
            r->count = 1;
            r->me = me;
            S->msg_rev++;
        }
        return;
    }
}

void store_reaction_remove(const char *msg_id, const char *name, const char *id,
                           bool me)
{
    if (!msg_id) return;
    for (int i = 0; i < S->message_count; i++) {
        message *m = &S->messages[i];
        if (strcmp(m->id, msg_id) != 0) continue;
        for (int j = 0; j < m->reaction_count; j++) {
            if (rx_match(&m->reactions[j], name, id)) {
                if (--m->reactions[j].count <= 0) {
                    memmove(&m->reactions[j], &m->reactions[j + 1],
                            sizeof(reaction) * (m->reaction_count - j - 1));
                    m->reaction_count--;
                } else if (me) {
                    m->reactions[j].me = false;
                }
                S->msg_rev++;
                return;
            }
        }
        return;
    }
}

void store_reaction_clear(const char *msg_id)
{
    if (!msg_id) return;
    for (int i = 0; i < S->message_count; i++) {
        if (strcmp(S->messages[i].id, msg_id) == 0) {
            S->messages[i].reaction_count = 0;
            S->msg_rev++;
            return;
        }
    }
}

unsigned store_msg_rev(void) { return S->msg_rev; }
void     store_touch(void)   { S->msg_rev++; }

void store_set_typing(const char *channel_id, const char *name, long long at_ms)
{
    snprintf(S->typing_channel, sizeof(S->typing_channel), "%s", channel_id ? channel_id : "");
    snprintf(S->typing_name, sizeof(S->typing_name), "%s", name ? name : "Someone");
    S->typing_at = at_ms;
}
const char *store_typing_name(void)    { return S->typing_name; }
const char *store_typing_channel(void) { return S->typing_channel; }
long long   store_typing_at(void)      { return S->typing_at; }

void store_push_message(const message *src)
{
    message *m;
    if (S->message_count < MAX_MESSAGES) {
        m = &S->messages[S->message_count++];
    } else {
        // drop oldest: shift down
        memmove(&S->messages[0], &S->messages[1],
                sizeof(message) * (MAX_MESSAGES - 1));
        m = &S->messages[MAX_MESSAGES - 1];
    }
    *m = *src;
    S->msg_rev++;
}

// Insert one older message at the front (scroll-back). No-op when full.
bool store_prepend_message(const message *src)
{
    if (S->message_count >= MAX_MESSAGES) return false;
    memmove(&S->messages[1], &S->messages[0],
            sizeof(message) * S->message_count);
    S->messages[0] = *src;
    S->message_count++;
    S->msg_rev++;
    return true;
}

const char *store_oldest_message_id(void)
{
    return S->message_count > 0 ? S->messages[0].id : "";
}

message *store_messages(int *count_out)
{
    if (count_out) *count_out = S->message_count;
    return S->messages;
}

void store_set_msg_loading(bool loading) { S->msg_loading = loading; }
bool store_msg_loading(void)             { return S->msg_loading; }

void store_set_me(const char *username, const char *global_name, const char *id,
                  const char *avatar)
{
    const char *disp = (global_name && global_name[0]) ? global_name : username;
    snprintf(S->me_name, sizeof(S->me_name), "%s", disp ? disp : "");
    snprintf(S->me_id, sizeof(S->me_id), "%s", id ? id : "");
    if (avatar) snprintf(S->me_avatar, sizeof(S->me_avatar), "%s", avatar);
}
const char *store_me_name(void)   { return S->me_name; }
const char *store_me_id(void)     { return S->me_id; }
const char *store_me_avatar(void) { return S->me_avatar; }

// color is Discord's RGB int (0 = default); stored converted to our ABGR.
void store_add_role(const char *id, const char *name, unsigned rgb)
{
    if (!id) return;
    unsigned abgr = rgb ? (0xFF000000u | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) |
                           ((rgb >> 16) & 0xFF)) : 0;
    for (int i = 0; i < S->role_count; i++)  // dedupe: refresh name/color
        if (strcmp(S->roles[i].id, id) == 0) {
            snprintf(S->roles[i].name, 40, "%s", name ? name : "");
            S->roles[i].color = abgr; return;
        }
    if (S->role_count >= 4096) return;
    snprintf(S->roles[S->role_count].id, 24, "%s", id);
    snprintf(S->roles[S->role_count].name, 40, "%s", name ? name : "");
    S->roles[S->role_count].color = abgr;
    S->role_count++;
}

const char *store_role_name(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < S->role_count; i++)
        if (strcmp(S->roles[i].id, id) == 0) return S->roles[i].name;
    return NULL;
}

// Returns the role's ABGR color, or 0 if unknown / no color.
unsigned store_role_color(const char *id)
{
    if (!id) return 0;
    for (int i = 0; i < S->role_count; i++)
        if (strcmp(S->roles[i].id, id) == 0) return S->roles[i].color;
    return 0;
}

void store_members_begin(const char *guild_id)
{
    S->member_count = 0;
    snprintf(S->member_guild, sizeof(S->member_guild), "%s", guild_id ? guild_id : "");
}

void store_add_member_group(const char *label, int count)
{
    if (S->member_count >= MAX_MEMBERS) return;
    member_row *r = &S->members[S->member_count++];
    memset(r, 0, sizeof(*r));
    r->is_group = true;
    snprintf(r->text, sizeof(r->text), "%s", label ? label : "");
    r->count = count;
}

void store_add_member(const char *name, const char *id, const char *avatar, int status)
{
    if (S->member_count >= MAX_MEMBERS) return;
    member_row *r = &S->members[S->member_count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->text, sizeof(r->text), "%s", name ? name : "?");
    snprintf(r->id, sizeof(r->id), "%s", id ? id : "");
    snprintf(r->avatar, sizeof(r->avatar), "%s", avatar ? avatar : "");
    r->status = status;
}

member_row *store_members(int *count_out)
{
    if (count_out) *count_out = S->member_count;
    return S->members;
}
const char *store_members_guild(void) { return S->member_guild; }

void store_friends_begin(void) { S->friend_count = 0; }

void store_add_friend(const char *name, const char *id, const char *avatar)
{
    if (S->friend_count >= MAX_FRIENDS) return;
    member_row *r = &S->friends[S->friend_count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->text,   sizeof(r->text),   "%s", name   ? name   : "?");
    snprintf(r->id,     sizeof(r->id),     "%s", id     ? id     : "");
    snprintf(r->avatar, sizeof(r->avatar), "%s", avatar ? avatar : "");
}

member_row *store_friends(int *count_out)
{
    if (count_out) *count_out = S->friend_count;
    return S->friends;
}

int store_member_status(const char *user_id)
{
    if (!user_id || !user_id[0]) return -1;
    for (int i = 0; i < S->member_count; i++)
        if (!S->members[i].is_group && strcmp(S->members[i].id, user_id) == 0)
            return S->members[i].status;
    // Fall back to the global presence map (covers DM avatars, etc.).
    for (int i = 0; i < S->presence_count; i++)
        if (strcmp(S->presence[i].id, user_id) == 0)
            return S->presence[i].status;
    return -1;
}

const char *store_member_nick(const char *user_id)
{
    if (!user_id || !user_id[0]) return NULL;
    for (int i = 0; i < S->member_count; i++)
        if (!S->members[i].is_group && strcmp(S->members[i].id, user_id) == 0)
            return S->members[i].text;
    return NULL;
}

bool store_set_presence(const char *user_id, int status)
{
    if (!user_id || !user_id[0]) return false;
    bool changed = false;
    for (int i = 0; i < S->member_count; i++)
        if (!S->members[i].is_group && strcmp(S->members[i].id, user_id) == 0
            && S->members[i].status != status) {
            S->members[i].status = status;
            changed = true;
        }
    // Upsert into the presence map.
    for (int i = 0; i < S->presence_count; i++)
        if (strcmp(S->presence[i].id, user_id) == 0) {
            if (S->presence[i].status != status) { S->presence[i].status = status; changed = true; }
            return changed;
        }
    if (S->presence_count < PRESENCE_MAX) {
        snprintf(S->presence[S->presence_count].id,
                 sizeof(S->presence[0].id), "%s", user_id);
        S->presence[S->presence_count].status = status;
        S->presence_count++;
        changed = true;
    }
    return changed;
}

void store_set_activity(const char *user_id, const char *activity)
{
    if (!user_id || !user_id[0]) return;
    for (int i = 0; i < S->presence_count; i++)
        if (strcmp(S->presence[i].id, user_id) == 0) {
            snprintf(S->presence[i].activity, sizeof(S->presence[0].activity),
                     "%s", activity ? activity : "");
            return;
        }
    if (S->presence_count < PRESENCE_MAX) {
        snprintf(S->presence[S->presence_count].id, sizeof(S->presence[0].id), "%s", user_id);
        S->presence[S->presence_count].status = 0;
        snprintf(S->presence[S->presence_count].activity,
                 sizeof(S->presence[0].activity), "%s", activity ? activity : "");
        S->presence_count++;
    }
}
const char *store_member_activity(const char *user_id)
{
    if (!user_id || !user_id[0]) return NULL;
    for (int i = 0; i < S->presence_count; i++)
        if (strcmp(S->presence[i].id, user_id) == 0)
            return S->presence[i].activity[0] ? S->presence[i].activity : NULL;
    return NULL;
}

void store_clear_notes(void) { S->notes_count = 0; }
void store_set_note(const char *user_id, const char *note)
{
    if (!user_id || !user_id[0]) return;
    for (int i = 0; i < S->notes_count; i++)
        if (strcmp(S->notes[i].id, user_id) == 0) {
            snprintf(S->notes[i].text, sizeof(S->notes[0].text), "%s", note ? note : "");
            return;
        }
    if (S->notes_count < 64) {
        snprintf(S->notes[S->notes_count].id, 24, "%s", user_id);
        snprintf(S->notes[S->notes_count].text, sizeof(S->notes[0].text), "%s", note ? note : "");
        S->notes_count++;
    }
}
const char *store_get_note(const char *user_id)
{
    if (!user_id || !user_id[0]) return NULL;
    for (int i = 0; i < S->notes_count; i++)
        if (strcmp(S->notes[i].id, user_id) == 0)
            return S->notes[i].text[0] ? S->notes[i].text : NULL;
    return NULL;
}

void store_clear_blocked(void) { S->blocked_count = 0; }
void store_set_blocked(const char *user_id, bool blocked)
{
    if (!user_id || !user_id[0]) return;
    for (int i = 0; i < S->blocked_count; i++)
        if (strcmp(S->blocked[i], user_id) == 0) {
            if (!blocked) {  // remove: swap with last
                S->blocked[i][0] = '\0';
                snprintf(S->blocked[i], 24, "%s", S->blocked[S->blocked_count - 1]);
                S->blocked_count--;
            }
            return;
        }
    if (blocked && S->blocked_count < 128)
        snprintf(S->blocked[S->blocked_count++], 24, "%s", user_id);
}
bool store_is_blocked(const char *user_id)
{
    if (!user_id || !user_id[0]) return false;
    for (int i = 0; i < S->blocked_count; i++)
        if (strcmp(S->blocked[i], user_id) == 0) return true;
    return false;
}

channel *store_find_channel(const char *id)
{
    if (!id || !id[0]) return NULL;
    for (int g = 0; g < S->guild_count; g++)
        for (int c = 0; c < S->guilds[g].channel_count; c++)
            if (strcmp(S->guilds[g].channels[c].id, id) == 0)
                return &S->guilds[g].channels[c];
    for (int d = 0; d < S->dm_count; d++)
        if (strcmp(S->dms[d].id, id) == 0) return &S->dms[d];
    return NULL;
}

guild *store_find_guild(const char *id)
{
    if (!id || !id[0]) return NULL;
    for (int g = 0; g < S->guild_count; g++)
        if (strcmp(S->guilds[g].id, id) == 0) return &S->guilds[g];
    return NULL;
}

void store_set_channel_hidden(const char *id, bool hidden)
{
    channel *c = store_find_channel(id);
    if (c) c->hidden = hidden;
}

// Remove a channel/thread by id from whichever guild holds it.
void store_remove_channel(const char *id)
{
    if (!id || !id[0]) return;
    for (int g = 0; g < S->guild_count; g++) {
        guild *gu = &S->guilds[g];
        for (int c = 0; c < gu->channel_count; c++)
            if (strcmp(gu->channels[c].id, id) == 0) {
                memmove(&gu->channels[c], &gu->channels[c + 1],
                        sizeof(channel) * (gu->channel_count - c - 1));
                gu->channel_count--;
                return;
            }
    }
}

void store_remove_guild(const char *id)
{
    if (!id || !id[0]) return;
    for (int g = 0; g < S->guild_count; g++)
        if (strcmp(S->guilds[g].id, id) == 0) {
            memmove(&S->guilds[g], &S->guilds[g + 1],
                    sizeof(guild) * (S->guild_count - g - 1));
            S->guild_count--;
            return;
        }
}

void store_mark_read(const char *id)
{
    channel *c = store_find_channel(id);
    if (c) { c->unread = false; c->mentions = 0;
             // mark read up to the newest message
             snprintf(c->last_read_id, sizeof(c->last_read_id), "%s", c->last_message_id); }
}

void store_bump_unread(const char *id, const char *last_message_id, bool mention)
{
    channel *c = store_find_channel(id);
    if (!c) return;
    c->unread = true;
    if (mention) c->mentions++;
    if (last_message_id)
        snprintf(c->last_message_id, sizeof(c->last_message_id), "%s", last_message_id);
}
