#include "gateway/gateway.h"
#include "api/discord.h"
#include "model/store.h"
#include "net_common.h"
#include "voice/voice.h"
#include "console.h"
#include "util/b64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "tls/ws_client.h"
#include <jansson.h>
#define JSMN_STATIC
#define JSMN_PARENT_LINKS
#include "jsmn.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/cpu.h>
#include <psp2/rtc.h>

#define GATEWAY_WSS "wss://gateway.discord.gg/?v=10&encoding=json&compress=zlib-stream"

static volatile bool s_run;
static SceUID        s_thread = -1;
static char          s_token[128];
static char          s_cacert[256];
static const char   *s_status = "Idle";

static char   s_session_id[64];
static char   s_resume_url[256];
static int    s_seq = -1;

// set once a connection reaches READY/RESUMED; resets the reconnect backoff
static volatile bool s_made_progress;
// set on a 4004 close (token no longer valid); the worker stops retrying
static volatile bool s_auth_failed;

// pending op 14 member-list request from the UI thread
static volatile int s_want_members;
static char   s_mem_guild[24];
static char   s_mem_channel[24];

// pending op 3 presence change from the UI thread
static volatile int s_want_status;
static char   s_status_str[16] = "online";
static char   s_custom_status[128];

// pending op 4 voice-state change from the voice thread
static volatile int s_want_voice;
static char   s_voice_guild[24];
static char   s_voice_chan[24];  // empty = disconnect

static int64_t now_ms(void)
{
    SceRtcTick t; sceRtcGetCurrentTick(&t);
    return (int64_t)(t.tick / 1000);
}

// returns the message length (>0), 0 on timeout, -1 on drop/error
static int ws_read_message(ws_client *ws, char **buf, size_t *cap, int timeout_ms)
{
    int64_t start = now_ms();
    for (;;) {
        if (!s_run) return -1;
        net_lock();
        int r = ws_recv(ws, buf, cap);
        net_unlock();
        if (r > 0) return r;
        if (r < 0) return -1;
        if (now_ms() - start > timeout_ms) return 0;
        sceKernelDelayThread(4 * 1000);
    }
}

static int ws_send(ws_client *ws, const char *txt)
{
    net_lock();
    int r = ws_send_text(ws, txt, -1);
    net_unlock();
    return r;
}

static void send_identify(ws_client *ws)
{
    char props[1024];
    discord_superprops_json(props, sizeof(props));
    json_error_t e;
    json_t *pobj = json_loads(props, 0, &e);
    json_t *d = json_pack("{s:s, s:o, s:b, s:i}", "token", s_token,
                          "properties", pobj ? pobj : json_object(),
                          "compress", 0, "capabilities", 30717);
    json_object_set_new(d, "presence",
        json_pack("{s:s,s:n,s:[],s:b}", "status", "online", "since",
                  "activities", "afk", 0));
    json_t *cs = json_pack("{s:{}, s:s, s:i, s:i, s:i, s:s, s:i}",
                  "guild_versions",
                  "highest_last_message_id", "0",
                  "read_state_version", 0,
                  "user_guild_settings_version", -1,
                  "user_settings_version", -1,
                  "private_channels_version", "0",
                  "api_code_version", 0);
    if (cs) json_object_set_new(d, "client_state", cs);
    json_t *p = json_pack("{s:i,s:o}", "op", 2, "d", d);
    char *txt = json_dumps(p, JSON_COMPACT);
    json_decref(p);
    if (txt) { ws_send(ws, txt); free(txt); }
    console_dbgf("[gw] IDENTIFY sent");
}

static void send_resume(ws_client *ws)
{
    json_t *p = json_pack("{s:i, s:{s:s,s:s,s:i}}", "op", 6, "d",
                          "token", s_token, "session_id", s_session_id,
                          "seq", s_seq);
    char *txt = json_dumps(p, JSON_COMPACT);
    json_decref(p);
    if (txt) { ws_send(ws, txt); free(txt); }
    console_dbgf("[gw] RESUME sent (seq=%d)", s_seq);
}

static json_t *find_user(json_t *users, const char *id)
{
    if (!json_is_array(users) || !id) return NULL;
    size_t n = json_array_size(users);
    for (size_t i = 0; i < n; i++) {
        json_t *u = json_array_get(users, i);
        const char *uid = json_string_value(json_object_get(u, "id"));
        if (uid && strcmp(uid, id) == 0) return u;
    }
    return NULL;
}

static const char *user_display(json_t *u)
{
    if (!u) return NULL;
    const char *gn = json_string_value(json_object_get(u, "global_name"));
    const char *un = json_string_value(json_object_get(u, "username"));
    return (gn && gn[0]) ? gn : un;
}

// snowflake ids are decimal numbers: longer string = larger, else strcmp order
static bool snowflake_gt(const char *a, const char *b)
{
    if (!a || !a[0]) return false;
    if (!b || !b[0]) return true;
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return la > lb;
    return strcmp(a, b) > 0;
}

static const char *guild_name(json_t *g)
{
    const char *n = json_string_value(json_object_get(g, "name"));
    if (n) return n;
    json_t *props = json_object_get(g, "properties");
    if (props) n = json_string_value(json_object_get(props, "name"));
    return n;
}

static const char *guild_icon(json_t *g)
{
    const char *ic = json_string_value(json_object_get(g, "icon"));
    if (ic) return ic;
    json_t *props = json_object_get(g, "properties");
    if (props) ic = json_string_value(json_object_get(props, "icon"));
    return ic;
}

static int status_from_str(const char *s);
static void activity_text(json_t *d, char *out, size_t cap);

#define PERM_ADMIN (1ull << 3)
#define PERM_VIEW  (1ull << 10)
static unsigned long long perm_field(json_t *o, const char *key)
{
    const char *s = json_string_value(json_object_get(o, key));
    return s ? strtoull(s, NULL, 10) : 0ull;
}
static int role_is_mine(const char *rid, char myroles[][24], int nmy)
{
    for (int k = 0; k < nmy; k++) if (strcmp(rid, myroles[k]) == 0) return 1;
    return 0;
}
// guild base perms, then @everyone, role, and member overwrites in that order
static int chan_can_view(json_t *cj, const char *guild_id, const char *my_id,
                         char myroles[][24], int nmy,
                         unsigned long long base, int admin)
{
    if (admin) return 1;
    unsigned long long perms = base;
    json_t *ows = json_object_get(cj, "permission_overwrites");
    if (json_is_array(ows)) {
        unsigned long long e_allow=0,e_deny=0, r_allow=0,r_deny=0, m_allow=0,m_deny=0;
        size_t on = json_array_size(ows);
        for (size_t k = 0; k < on; k++) {
            json_t *ow = json_array_get(ows, k);
            const char *oid = json_string_value(json_object_get(ow, "id"));
            int otype = (int)json_integer_value(json_object_get(ow, "type"));
            unsigned long long a = perm_field(ow, "allow"), dny = perm_field(ow, "deny");
            if (!oid) continue;
            if (otype == 0 && strcmp(oid, guild_id) == 0)        { e_allow = a; e_deny = dny; }
            else if (otype == 0 && role_is_mine(oid, myroles, nmy)) { r_allow |= a; r_deny |= dny; }
            else if (otype == 1 && my_id && strcmp(oid, my_id) == 0) { m_allow = a; m_deny = dny; }
        }
        perms &= ~e_deny; perms |= e_allow;
        perms &= ~r_deny; perms |= r_allow;
        perms &= ~m_deny; perms |= m_allow;
    }
    return (perms & PERM_VIEW) != 0;
}

// gate==0 skips VIEW_CHANNEL hiding. Caller holds the store lock.
static void populate_guild(guild *g, json_t *gj, const char *guild_id,
                           char myroles[][24], int nmy, int gate)
{
    const char *my_id = store_me_id();

    json_t *props = json_object_get(gj, "properties");
    const char *bn = json_string_value(json_object_get(gj, "banner"));
    if ((!bn || !bn[0]) && props) bn = json_string_value(json_object_get(props, "banner"));
    if (bn) snprintf(g->banner, sizeof(g->banner), "%s", bn);
    json_t *pt = json_object_get(gj, "premium_tier");
    if (!pt && props) pt = json_object_get(props, "premium_tier");
    g->premium_tier = (int)json_integer_value(pt);
    const char *desc = json_string_value(json_object_get(gj, "description"));
    if ((!desc || !desc[0]) && props) desc = json_string_value(json_object_get(props, "description"));
    if (desc) snprintf(g->description, sizeof(g->description), "%s", desc);
    g->premium_subscription_count =
        (int)json_integer_value(json_object_get(gj, "premium_subscription_count"));
    json_t *mc = json_object_get(gj, "member_count");
    if (json_is_integer(mc)) g->member_count = (int)json_integer_value(mc);

    json_t *roles = json_object_get(gj, "roles");
    size_t roln = json_is_array(roles) ? json_array_size(roles) : 0;
    g->role_count = 0;
    for (size_t ri = 0; ri < roln; ri++) {
        json_t *rj = json_array_get(roles, ri);
        const char *rid = json_string_value(json_object_get(rj, "id"));
        store_add_role(rid, json_string_value(json_object_get(rj, "name")),
                       (unsigned)json_integer_value(json_object_get(rj, "color")));
        // the @everyone role id equals the guild id
        if (rid && rid[0] && !(guild_id && strcmp(rid, guild_id) == 0) &&
            g->role_count < 48)
            snprintf(g->role_ids[g->role_count++], 24, "%s", rid);
    }
    unsigned long long base = 0; int admin = 0;
    if (gate && guild_id) {
        for (size_t ri = 0; ri < roln; ri++) {
            json_t *rj = json_array_get(roles, ri);
            const char *rid = json_string_value(json_object_get(rj, "id"));
            if (!rid) continue;
            if (strcmp(rid, guild_id) == 0 || role_is_mine(rid, myroles, nmy))
                base |= perm_field(rj, "permissions");
        }
        admin = (base & PERM_ADMIN) != 0;
    }
    g->my_perms = admin ? ~0ull : base;
    json_t *chans = json_object_get(gj, "channels");
    size_t cn = json_is_array(chans) ? json_array_size(chans) : 0;
    int nhidden = 0;
    for (size_t j = 0; j < cn; j++) {
        json_t *cj = json_array_get(chans, j);
        const char *chid = json_string_value(json_object_get(cj, "id"));
        store_add_channel(g, chid,
            json_string_value(json_object_get(cj, "name")),
            (int)json_integer_value(json_object_get(cj, "type")),
            (int)json_integer_value(json_object_get(cj, "position")),
            json_string_value(json_object_get(cj, "parent_id")),
            json_string_value(json_object_get(cj, "last_message_id")));
        const char *ctopic = json_string_value(json_object_get(cj, "topic"));
        if (ctopic && chid && g->channel_count > 0 &&
            strcmp(g->channels[g->channel_count - 1].id, chid) == 0)
            snprintf(g->channels[g->channel_count - 1].topic, 256, "%s", ctopic);
        if (gate && chid &&
            !chan_can_view(cj, guild_id, my_id, myroles, nmy, base, admin)) {
            store_set_channel_hidden(chid, true);
            nhidden++;
        }
    }
    if (gate) console_dbgf("[gw] guild %.8s: gated %d/%d channels hidden",
                           guild_id ? guild_id : "?", nhidden, (int)cn);
    json_t *thr = json_object_get(gj, "threads");
    size_t tn = json_is_array(thr) ? json_array_size(thr) : 0;
    for (size_t j = 0; j < tn; j++) {
        json_t *cj = json_array_get(thr, j);
        store_add_channel(g,
            json_string_value(json_object_get(cj, "id")),
            json_string_value(json_object_get(cj, "name")),
            (int)json_integer_value(json_object_get(cj, "type")),
            (int)json_integer_value(json_object_get(cj, "position")),
            json_string_value(json_object_get(cj, "parent_id")),
            json_string_value(json_object_get(cj, "last_message_id")));
    }
}

// returns 1 if our member was found, else 0
static int my_roles_from_members(json_t *members, char myroles[][24], int *nmy)
{
    *nmy = 0;
    const char *my_id = store_me_id();
    if (!json_is_array(members) || !my_id) return 0;
    size_t mn = json_array_size(members);
    for (size_t k = 0; k < mn; k++) {
        json_t *mem = json_array_get(members, k);
        const char *uid = json_string_value(
            json_object_get(json_object_get(mem, "user"), "id"));
        if (!uid) uid = json_string_value(json_object_get(mem, "user_id"));
        if (uid && strcmp(uid, my_id) == 0) {
            json_t *rr = json_object_get(mem, "roles");
            if (json_is_array(rr))
                for (size_t r = 0; r < json_array_size(rr) && *nmy < 64; r++) {
                    const char *rid = json_string_value(json_array_get(rr, r));
                    if (rid) snprintf(myroles[(*nmy)++], 24, "%s", rid);
                }
            return 1;
        }
    }
    return 0;
}

static void parse_guild_create(json_t *d)
{
    const char *id = json_string_value(json_object_get(d, "id"));
    if (!id) return;
    guild *g = store_add_guild(id, guild_name(d), guild_icon(d));
    if (!g) return;
    char myroles[64][24]; int nmy = 0;
    int gate = my_roles_from_members(json_object_get(d, "members"), myroles, &nmy);
    populate_guild(g, d, id, myroles, nmy, gate);
    console_dbgf("[gw] GUILD_CREATE %.8s '%s'", id, guild_name(d) ? guild_name(d) : "?");
}

// One protobuf field from [buf,end); advances *pos. Wire type 2 fills *dp/*dl,
// types 0/1 fill *val. Returns false at the end or on malformed input.
static bool pb_next(const uint8_t *buf, size_t *pos, size_t end,
                    int *field, int *wt, const uint8_t **dp, size_t *dl, uint64_t *val)
{
    uint64_t tag = 0; int shift = 0;
    for (;;) {
        if (*pos >= end || shift > 63) return false;
        uint8_t b = buf[(*pos)++];
        tag |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    *field = (int)(tag >> 3);
    *wt = (int)(tag & 7);
    if (*wt == 0) {  // varint
        uint64_t v = 0; shift = 0;
        for (;;) {
            if (*pos >= end || shift > 63) return false;
            uint8_t b = buf[(*pos)++];
            v |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        *val = v;
    } else if (*wt == 2) {  // length-delimited
        uint64_t len = 0; shift = 0;
        for (;;) {
            if (*pos >= end || shift > 63) return false;
            uint8_t b = buf[(*pos)++];
            len |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        if (len > end - *pos) return false;
        *dp = buf + *pos; *dl = (size_t)len; *pos += (size_t)len;
    } else if (*wt == 1) {  // 64-bit
        if (end - *pos < 8) return false;
        uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)buf[*pos + i] << (8 * i);
        *val = v; *pos += 8;
    } else if (*wt == 5) {  // 32-bit
        if (end - *pos < 4) return false;
        *pos += 4;
    } else {
        return false;
    }
    return true;
}

// A Discord snowflake is a large 64-bit id (>= ~17 digits); colors and counts are smaller.
static bool pb_is_snowflake(uint64_t v) { return v >= 10000000000000000ULL; }

// Collect packed fixed64 snowflakes from [dp,dl) into ids.
static void pb_collect_fixed64(const uint8_t *dp, size_t dl, char ids[][24], int *n, int max)
{
    for (size_t k = 0; k + 8 <= dl && *n < max; k += 8) {
        uint64_t id = 0; for (int i = 0; i < 8; i++) id |= (uint64_t)dp[k + i] << (8 * i);
        if (pb_is_snowflake(id)) snprintf(ids[(*n)++], 24, "%llu", (unsigned long long)id);
    }
}

// Parse a candidate GuildFolders message: folders(1)[].guild_ids(1), else the
// flat guild_positions(2). Returns the count of guild ids collected.
static int pb_guildfolders(const uint8_t *gf, size_t gfl, char ids[][24], int max)
{
    int n = 0;
    size_t pos = 0; int f, wt; const uint8_t *dp; size_t dl; uint64_t v;
    while (pb_next(gf, &pos, gfl, &f, &wt, &dp, &dl, &v)) {
        if (f == 1 && wt == 2) {  // a GuildFolder message
            size_t p2 = 0; int f2, wt2; const uint8_t *dp2; size_t dl2; uint64_t v2;
            while (pb_next(dp, &p2, dl, &f2, &wt2, &dp2, &dl2, &v2)) {
                if (f2 == 1 && wt2 == 2) pb_collect_fixed64(dp2, dl2, ids, &n, max);
                else if (f2 == 1 && wt2 == 1 && n < max && pb_is_snowflake(v2))
                    snprintf(ids[n++], 24, "%llu", (unsigned long long)v2);
            }
        }
    }
    if (n == 0) {  // fallback: flat guild_positions(2)
        pos = 0;
        while (pb_next(gf, &pos, gfl, &f, &wt, &dp, &dl, &v))
            if (f == 2 && wt == 2) pb_collect_fixed64(dp, dl, ids, &n, max);
    }
    return n;
}

// Decode base64 PreloadedUserSettings into guild order. The guild_folders field
// number varies by proto revision, so try every top-level field, keep the best id match.
static int parse_guild_order_proto(const char *b64, char ids[][24], int max)
{
    static uint8_t raw[32768];
    int rawlen = b64_decode(b64, raw, sizeof raw);
    if (rawlen <= 0) { console_dbgf("[order] proto decode -> %d", rawlen); return 0; }

    static char cand[MAX_GUILDS][24];
    int best_n = 0, best_matches = 0, best_field = -1;
    size_t pos = 0; int f, wt; const uint8_t *dp; size_t dl; uint64_t v;
    while (pb_next(raw, &pos, (size_t)rawlen, &f, &wt, &dp, &dl, &v)) {
        if (wt != 2) continue;
        int cn = pb_guildfolders(dp, dl, cand, MAX_GUILDS);
        if (cn == 0) continue;
        int matches = 0;
        for (int i = 0; i < cn; i++) if (store_find_guild(cand[i])) matches++;
        if (matches > best_matches) {
            best_matches = matches; best_field = f;
            best_n = cn < max ? cn : max;
            for (int i = 0; i < best_n; i++) snprintf(ids[i], 24, "%s", cand[i]);
        }
    }
    console_dbgf("[order] proto field=%d ids=%d matched=%d", best_field, best_n, best_matches);
    return best_matches > 0 ? best_n : 0;
}

// jsmn accessors: parse the large READY / READY_SUPPLEMENTAL allocation-free,
// mirroring json_object_get / json_array_* over a shared (buffer, token array).
static const char *s_js;
static jsmntok_t  *s_jt;

static int jt_skip(int i)  // index one past token i's whole subtree
{
    int j = i + 1;
    for (int c = 0; c < s_jt[i].size; c++) j = jt_skip(j);
    return j;
}
static int jt_is_arr(int t) { return t >= 0 && s_jt[t].type == JSMN_ARRAY; }
static int jt_is_obj(int t) { return t >= 0 && s_jt[t].type == JSMN_OBJECT; }
static int jt_is_str(int t) { return t >= 0 && s_jt[t].type == JSMN_STRING; }
static int jt_is_prim(int t){ return t >= 0 && s_jt[t].type == JSMN_PRIMITIVE; }
static int jt_len(int t)    { return jt_is_arr(t) || jt_is_obj(t) ? s_jt[t].size : 0; }
static int jt_keyeq(int t, const char *k)
{
    if (!jt_is_str(t)) return 0;
    int len = s_jt[t].end - s_jt[t].start;
    return (int)strlen(k) == len && memcmp(s_js + s_jt[t].start, k, len) == 0;
}
static int jt_get(int obj, const char *key)  // object member value, or -1
{
    if (!jt_is_obj(obj)) return -1;
    int j = obj + 1;
    for (int c = 0; c < s_jt[obj].size; c++) {
        if (jt_keyeq(j, key)) return j + 1;  // the key's child is its value
        j = jt_skip(j);
    }
    return -1;
}
static int jt_at(int arr, int i)  // array element i, or -1
{
    if (!jt_is_arr(arr) || i < 0 || i >= s_jt[arr].size) return -1;
    int j = arr + 1;
    while (i-- > 0) j = jt_skip(j);
    return j;
}
static long long jt_int(int t)
{
    if (!jt_is_prim(t)) return 0;
    int len = s_jt[t].end - s_jt[t].start; char b[24];
    if (len <= 0 || len >= (int)sizeof(b)) return 0;
    memcpy(b, s_js + s_jt[t].start, len); b[len] = 0;
    return strtoll(b, NULL, 10);
}
static unsigned long long jt_permfield(int obj, const char *key)  // string-or-int
{
    int t = jt_get(obj, key);
    if (t < 0) return 0;
    int len = s_jt[t].end - s_jt[t].start; char b[24];
    if (len <= 0 || len >= (int)sizeof(b)) return 0;
    memcpy(b, s_js + s_jt[t].start, len); b[len] = 0;
    return strtoull(b, NULL, 10);
}
static int jt_hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}
// Copy string token `t` into `out` as UTF-8 with JSON escapes decoded.
static void jt_str(int t, char *out, int cap)
{
    if (cap > 0) out[0] = 0;
    if (!jt_is_str(t) || cap < 2) return;
    const char *p = s_js + s_jt[t].start, *e = s_js + s_jt[t].end;
    int o = 0;
    while (p < e && o < cap - 1) {
        if (*p != '\\') { out[o++] = *p++; continue; }
        if (++p >= e) break;
        switch (*p) {
        case '"': out[o++] = '"';  p++; break;
        case '\\':out[o++] = '\\'; p++; break;
        case '/': out[o++] = '/';  p++; break;
        case 'b': out[o++] = '\b'; p++; break;
        case 'f': out[o++] = '\f'; p++; break;
        case 'n': out[o++] = '\n'; p++; break;
        case 'r': out[o++] = '\r'; p++; break;
        case 't': out[o++] = '\t'; p++; break;
        case 'u': {
            if (p + 5 > e) { p = e; break; }
            int cp = jt_hex4(p + 1); p += 5;
            if (cp < 0) break;
            if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= e && p[0] == '\\' && p[1] == 'u') {
                int lo = jt_hex4(p + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
            }
            if (cp < 0x80)         { if (o < cap-1) out[o++] = cp; }
            else if (cp < 0x800)   { if (o < cap-2) { out[o++]=0xC0|(cp>>6); out[o++]=0x80|(cp&0x3F); } }
            else if (cp < 0x10000) { if (o < cap-3) { out[o++]=0xE0|(cp>>12); out[o++]=0x80|((cp>>6)&0x3F); out[o++]=0x80|(cp&0x3F); } }
            else                   { if (o < cap-4) { out[o++]=0xF0|(cp>>18); out[o++]=0x80|((cp>>12)&0x3F); out[o++]=0x80|((cp>>6)&0x3F); out[o++]=0x80|(cp&0x3F); } }
            break;
        }
        default: out[o++] = *p++; break;
        }
    }
    out[o] = 0;
}
static void jt_gets(int obj, const char *key, char *out, int cap) { jt_str(jt_get(obj, key), out, cap); }

// Walk array `arr`'s elements in one forward pass; `el` is each element token.
// `arr` must be a plain int variable, not an expression.
#define JT_FOREACH(arr, el) \
    for (int el = jt_is_arr(arr) ? (arr) + 1 : -1, _n_##el = jt_len(arr), _i_##el = 0; \
         _i_##el < _n_##el; _i_##el++, el = jt_skip(el))

static int find_user_js(int users, const char *id)
{
    if (!jt_is_arr(users) || !id || !id[0]) return -1;
    JT_FOREACH(users, u) {
        char uid[24]; jt_gets(u, "id", uid, sizeof uid);
        if (uid[0] && strcmp(uid, id) == 0) return u;
    }
    return -1;
}
static void activity_text_js(int d, char *out, int cap)
{
    out[0] = 0;
    int acts = jt_get(d, "activities");
    if (!jt_is_arr(acts)) return;
    int have_game = 0, gametype = -1;
    char game[128] = "";
    JT_FOREACH(acts, a) {
        int type = (int)jt_int(jt_get(a, "type"));
        if (type == 4) {
            char state[128]; jt_gets(a, "state", state, sizeof state);
            int em = jt_get(a, "emoji");
            char ename[64] = ""; if (em >= 0) jt_gets(em, "name", ename, sizeof ename);
            int custom = (em >= 0 && jt_get(em, "id") >= 0);
            if (ename[0] && !custom && state[0]) snprintf(out, cap, "%s %s", ename, state);
            else if (state[0]) snprintf(out, cap, "%s", state);
            if (out[0]) return;
        } else if (!have_game) { jt_gets(a, "name", game, sizeof game); gametype = type; have_game = 1; }
    }
    if (have_game && game[0]) {
        const char *verb = gametype==1?"Streaming ":gametype==2?"Listening to ":
                           gametype==3?"Watching ":gametype==5?"Competing in ":"Playing ";
        snprintf(out, cap, "%s%s", verb, game);
    }
}
static int seed_presences_js(int arr)
{
    if (!jt_is_arr(arr)) return 0;
    int seeded = 0;
    JT_FOREACH(arr, p) {
        char uid[24]; jt_gets(p, "user_id", uid, sizeof uid);
        if (!uid[0]) { int u = jt_get(p, "user"); if (u >= 0) jt_gets(u, "id", uid, sizeof uid); }
        if (!uid[0]) continue;
        char st[16]; jt_gets(p, "status", st, sizeof st);
        store_set_presence(uid, status_from_str(st));
        char act[128]; activity_text_js(p, act, sizeof act);
        if (act[0]) store_set_activity(uid, act);
        seeded++;
    }
    return seeded;
}
static int chan_can_view_js(int cj, const char *guild_id, const char *my_id,
                            char myroles[][24], int nmy, unsigned long long base, int admin)
{
    if (admin) return 1;
    unsigned long long perms = base;
    int ows = jt_get(cj, "permission_overwrites");
    if (jt_is_arr(ows)) {
        unsigned long long e_a=0,e_d=0,r_a=0,r_d=0,m_a=0,m_d=0;
        JT_FOREACH(ows, ow) {
            char oid[24]; jt_gets(ow, "id", oid, sizeof oid);
            int otype = (int)jt_int(jt_get(ow, "type"));
            unsigned long long a = jt_permfield(ow, "allow"), dny = jt_permfield(ow, "deny");
            if (!oid[0]) continue;
            if (otype == 0 && guild_id && strcmp(oid, guild_id) == 0)   { e_a = a; e_d = dny; }
            else if (otype == 0 && role_is_mine(oid, myroles, nmy))     { r_a |= a; r_d |= dny; }
            else if (otype == 1 && my_id && strcmp(oid, my_id) == 0)    { m_a = a; m_d = dny; }
        }
        perms &= ~e_d; perms |= e_a;
        perms &= ~r_d; perms |= r_a;
        perms &= ~m_d; perms |= m_a;
    }
    return (perms & PERM_VIEW) != 0;
}
static void populate_guild_js(guild *g, int gj, const char *guild_id,
                              char myroles[][24], int nmy, int gate)
{
    const char *my_id = store_me_id();
    int props = jt_get(gj, "properties");

    char banner[64]; jt_gets(gj, "banner", banner, sizeof banner);
    if (!banner[0] && props >= 0) jt_gets(props, "banner", banner, sizeof banner);
    if (banner[0]) snprintf(g->banner, sizeof g->banner, "%s", banner);
    int pt = jt_get(gj, "premium_tier");
    if (!jt_is_prim(pt) && props >= 0) pt = jt_get(props, "premium_tier");
    g->premium_tier = (int)jt_int(pt);
    char desc[512]; jt_gets(gj, "description", desc, sizeof desc);
    if (!desc[0] && props >= 0) jt_gets(props, "description", desc, sizeof desc);
    if (desc[0]) snprintf(g->description, sizeof g->description, "%s", desc);
    g->premium_subscription_count = (int)jt_int(jt_get(gj, "premium_subscription_count"));
    int mc = jt_get(gj, "member_count");
    if (jt_is_prim(mc)) g->member_count = (int)jt_int(mc);

    int roles = jt_get(gj, "roles");
    g->role_count = 0;
    JT_FOREACH(roles, rj) {
        char rid[24], rname[80]; jt_gets(rj, "id", rid, sizeof rid); jt_gets(rj, "name", rname, sizeof rname);
        store_add_role(rid, rname, (unsigned)jt_int(jt_get(rj, "color")));
        if (rid[0] && !(guild_id && strcmp(rid, guild_id) == 0) && g->role_count < 48)
            snprintf(g->role_ids[g->role_count++], 24, "%s", rid);
    }
    unsigned long long base = 0; int admin = 0;
    if (gate && guild_id) {
        JT_FOREACH(roles, rj2) {
            char rid[24]; jt_gets(rj2, "id", rid, sizeof rid);
            if (!rid[0]) continue;
            if (strcmp(rid, guild_id) == 0 || role_is_mine(rid, myroles, nmy))
                base |= jt_permfield(rj2, "permissions");
        }
        admin = (base & PERM_ADMIN) != 0;
    }
    g->my_perms = admin ? ~0ull : base;

    int chans = jt_get(gj, "channels");
    JT_FOREACH(chans, cj) {
        char chid[24], cname[100], parent[24], lmid[24], topic[256];
        jt_gets(cj, "id", chid, sizeof chid);          jt_gets(cj, "name", cname, sizeof cname);
        jt_gets(cj, "parent_id", parent, sizeof parent); jt_gets(cj, "last_message_id", lmid, sizeof lmid);
        store_add_channel(g, chid, cname, (int)jt_int(jt_get(cj, "type")),
                          (int)jt_int(jt_get(cj, "position")), parent, lmid);
        // store_add_channel appends, so the new channel is the tail; touch it
        // directly for topic and visibility.
        channel *nc = (chid[0] && g->channel_count > 0 &&
                       strcmp(g->channels[g->channel_count - 1].id, chid) == 0)
                    ? &g->channels[g->channel_count - 1] : NULL;
        jt_gets(cj, "topic", topic, sizeof topic);
        if (nc && topic[0]) snprintf(nc->topic, sizeof nc->topic, "%s", topic);
        if (nc && gate && !chan_can_view_js(cj, guild_id, my_id, myroles, nmy, base, admin))
            nc->hidden = true;
    }
    int thr = jt_get(gj, "threads");
    JT_FOREACH(thr, ct) {
        char chid[24], cname[100], parent[24], lmid[24];
        jt_gets(ct, "id", chid, sizeof chid);          jt_gets(ct, "name", cname, sizeof cname);
        jt_gets(ct, "parent_id", parent, sizeof parent); jt_gets(ct, "last_message_id", lmid, sizeof lmid);
        store_add_channel(g, chid, cname, (int)jt_int(jt_get(ct, "type")),
                          (int)jt_int(jt_get(ct, "position")), parent, lmid);
    }
}
static void parse_ready_js(int d)
{
    store_lock();
    store_clear_guilds();

    int user = jt_get(d, "user");
    if (user >= 0) {
        char un[64], gn[64], uid[24], av[64];
        jt_gets(user, "username", un, sizeof un);   jt_gets(user, "global_name", gn, sizeof gn);
        jt_gets(user, "id", uid, sizeof uid);        jt_gets(user, "avatar", av, sizeof av);
        store_set_me(un, gn, uid, av);
    }

    int guilds = jt_get(d, "guilds");
    int gn = jt_len(guilds);
    int mm = jt_get(d, "merged_members");
    // merged_members is a parallel array to guilds; walk both with cursors.
    int gj  = jt_is_arr(guilds) ? guilds + 1 : -1;
    int mlc = jt_is_arr(mm) ? mm + 1 : -1;
    for (int i = 0; i < gn; i++, gj = jt_skip(gj), mlc = (mlc >= 0 ? jt_skip(mlc) : -1)) {
        int props = jt_get(gj, "properties");
        char gid[24], gname[100], gicon[64];
        jt_gets(gj, "id", gid, sizeof gid);
        jt_gets(gj, "name", gname, sizeof gname);
        if (!gname[0] && props >= 0) jt_gets(props, "name", gname, sizeof gname);
        jt_gets(gj, "icon", gicon, sizeof gicon);
        if (!gicon[0] && props >= 0) jt_gets(props, "icon", gicon, sizeof gicon);
        guild *g = store_add_guild(gid, gname, gicon);
        if (!g) continue;
        const char *my_id = store_me_id();
        char myroles[64][24]; int nmy = 0;
        int gate = jt_is_arr(mm);
        if (gate) {
            int ml = mlc;  // merged_members[i]
            int meMem = -1;
            if (jt_is_arr(ml)) {
                JT_FOREACH(ml, mem) {
                    char muid[24]; jt_gets(mem, "user_id", muid, sizeof muid);
                    if (!muid[0]) { int u = jt_get(mem, "user"); if (u >= 0) jt_gets(u, "id", muid, sizeof muid); }
                    if (muid[0] && my_id && strcmp(muid, my_id) == 0) { meMem = mem; break; }
                }
                if (meMem < 0 && jt_len(ml) > 0) meMem = jt_at(ml, 0);
            }
            if (meMem >= 0) {
                int rr = jt_get(meMem, "roles");
                JT_FOREACH(rr, rtok) {
                    if (nmy >= 64) break;
                    char rid[24]; jt_str(rtok, rid, sizeof rid);
                    if (rid[0]) snprintf(myroles[nmy++], 24, "%s", rid);
                }
            } else gate = 0;
        }
        populate_guild_js(g, gj, gid[0] ? gid : NULL, myroles, nmy, gate);
    }

    int users = jt_get(d, "users");
    store_clear_dms();

    store_clear_notes();
    int notes = jt_get(d, "notes");
    if (jt_is_obj(notes)) {
        int j = notes + 1;
        for (int c = 0; c < s_jt[notes].size; c++) {
            int keytok = j, valtok = j + 1;
            char nk[24], nv[256];
            jt_str(keytok, nk, sizeof nk);
            if (jt_is_str(valtok)) { jt_str(valtok, nv, sizeof nv); if (nk[0]) store_set_note(nk, nv); }
            j = jt_skip(keytok);
        }
    }

    store_clear_blocked();
    store_friends_begin();
    int rels = jt_get(d, "relationships");
    JT_FOREACH(rels, rel) {
        int rtype = (int)jt_int(jt_get(rel, "type"));
        int ru = jt_get(rel, "user");
        char uid[24]; jt_gets(rel, "id", uid, sizeof uid);
        if (!uid[0] && ru >= 0) jt_gets(ru, "id", uid, sizeof uid);
        if (!uid[0]) continue;
        if (rtype == 2) store_set_blocked(uid, true);
        else if (rtype == 1) {
            int u = (ru >= 0) ? ru : find_user_js(users, uid);
            char nm[64] = ""; if (u >= 0) { jt_gets(u, "global_name", nm, sizeof nm); if (!nm[0]) jt_gets(u, "username", nm, sizeof nm); }
            char av[64] = ""; if (u >= 0) jt_gets(u, "avatar", av, sizeof av);
            store_add_friend(nm[0] ? nm : "?", uid, av);
        }
    }

    int dms = jt_get(d, "private_channels");
    int dn = jt_len(dms);
    JT_FOREACH(dms, dm) {
        int dtype = (int)jt_int(jt_get(dm, "type"));
        char name[96] = "Direct Message", gname[96];
        jt_gets(dm, "name", gname, sizeof gname);
        int r0 = -1, extra = 0;
        int recips = jt_get(dm, "recipients");
        int rids = jt_get(dm, "recipient_ids");
        if (jt_is_arr(recips) && jt_len(recips) >= 1) { r0 = jt_at(recips, 0); extra = jt_len(recips) - 1; }
        else if (jt_is_arr(rids) && jt_len(rids) >= 1) {
            char rid0[24]; jt_str(jt_at(rids, 0), rid0, sizeof rid0);
            r0 = find_user_js(users, rid0); extra = jt_len(rids) - 1;
        }
        char base[64] = ""; if (r0 >= 0) { jt_gets(r0, "global_name", base, sizeof base); if (!base[0]) jt_gets(r0, "username", base, sizeof base); }
        if (gname[0]) snprintf(name, sizeof name, "%s", gname);
        else if (base[0] && extra > 0) snprintf(name, sizeof name, "%s +%d", base, extra);
        else if (base[0]) snprintf(name, sizeof name, "%s", base);
        char rav[64] = ""; if (r0 >= 0) jt_gets(r0, "avatar", rav, sizeof rav);
        char rid[24] = ""; if (r0 >= 0) jt_gets(r0, "id", rid, sizeof rid);
        if (dtype == CH_GROUP_DM) jt_gets(dm, "icon", rav, sizeof rav);
        char did[24], lmid[24]; jt_gets(dm, "id", did, sizeof did); jt_gets(dm, "last_message_id", lmid, sizeof lmid);
        store_add_dm(did, name, dtype, rav[0] ? rav : NULL, rid[0] ? rid : NULL, lmid);
    }

    int rs = jt_get(d, "read_state");
    if (jt_is_obj(rs)) rs = jt_get(rs, "entries");
    int rn = jt_len(rs);
    JT_FOREACH(rs, e) {
        char cid[24]; jt_gets(e, "id", cid, sizeof cid);
        channel *c = store_find_channel(cid);
        if (!c) continue;
        char lr[24]; jt_gets(e, "last_message_id", lr, sizeof lr);
        if (lr[0]) snprintf(c->last_read_id, sizeof c->last_read_id, "%s", lr);
        c->mentions = (int)jt_int(jt_get(e, "mention_count"));
        c->unread = snowflake_gt(c->last_message_id, lr[0] ? lr : NULL) || c->mentions > 0;
    }

    int mp = jt_get(d, "merged_presences");
    seed_presences_js(jt_is_obj(mp) ? jt_get(mp, "friends") : mp);
    seed_presences_js(jt_get(d, "presences"));

    store_set_ready(true);
    store_unlock();
    console_dbgf("[gw] READY parsed (jsmn): %d guild(s), %d DM(s), read_state=%d", gn, dn, rn);

    {  // saved sidebar order
        int us = jt_get(d, "user_settings");
        int folders = jt_get(d, "guild_folders");
        if (!jt_is_arr(folders) && jt_is_obj(us)) folders = jt_get(us, "guild_folders");
        int positions = jt_get(d, "guild_positions");
        if (!jt_is_arr(positions) && jt_is_obj(us)) positions = jt_get(us, "guild_positions");
        int proto = jt_get(d, "user_settings_proto");
        static char order[MAX_GUILDS][24];
        int on = 0;
        if (jt_is_arr(folders)) {
            int nf = jt_len(folders);
            for (int i = 0; i < nf && on < MAX_GUILDS; i++) {
                int gids = jt_get(jt_at(folders, i), "guild_ids");
                int ng2 = jt_len(gids);
                for (int j = 0; j < ng2 && on < MAX_GUILDS; j++) {
                    int v = jt_at(gids, j);
                    if (jt_is_str(v)) jt_str(v, order[on++], 24);
                    else if (jt_is_prim(v)) snprintf(order[on++], 24, "%lld", jt_int(v));
                }
            }
        }
        if (on == 0 && jt_is_arr(positions)) {
            int np = jt_len(positions);
            for (int i = 0; i < np && on < MAX_GUILDS; i++) {
                int v = jt_at(positions, i);
                if (jt_is_str(v)) jt_str(v, order[on++], 24);
                else if (jt_is_prim(v)) snprintf(order[on++], 24, "%lld", jt_int(v));
            }
        }
        if (on == 0 && jt_is_str(proto)) {
            int plen = s_jt[proto].end - s_jt[proto].start + 1;
            char *pb = malloc(plen);
            if (pb) { jt_str(proto, pb, plen); on = parse_guild_order_proto(pb, order, MAX_GUILDS); free(pb); }
        }
        console_dbgf("[order] jsmn folders=%d positions=%d proto=%d -> %d ids",
                     jt_is_arr(folders), jt_is_arr(positions), proto >= 0, on);
        if (on > 0) store_set_guild_order((const char (*)[24])order, on);
    }
}
static void parse_ready_supplemental_js(int d)
{
    int mp = jt_get(d, "merged_presences");
    if (!jt_is_obj(mp)) { console_dbgf("[gw] READY_SUPPLEMENTAL(jsmn): no merged_presences"); return; }
    int nf, ng = 0;
    store_lock();
    nf = seed_presences_js(jt_get(mp, "friends"));
    int gp = jt_get(mp, "guilds");
    JT_FOREACH(gp, garr) ng += seed_presences_js(garr);
    store_unlock();
    store_touch();
    console_dbgf("[gw] READY_SUPPLEMENTAL(jsmn): seeded %d friend + %d member presences", nf, ng);
}
// Route a big message (READY / READY_SUPPLEMENTAL) through jsmn. Returns 1 if
// handled, 0 to fall back to jansson.
static int handle_big_message(char *buf, int len)
{
    // Estimate tokens (~1 per 8 bytes of Discord JSON) and grow on NOMEM.
    size_t cap = (size_t)len / 8 + 64;
    jsmntok_t *toks = malloc(cap * sizeof(jsmntok_t));
    if (!toks) return 0;
    jsmn_parser jp;
    int64_t t0 = now_ms();
    int r;
    for (;;) {
        jsmn_init(&jp);
        r = jsmn_parse(&jp, buf, (size_t)len, toks, cap);
        if (r != JSMN_ERROR_NOMEM) break;
        cap += cap / 2;
        jsmntok_t *g = realloc(toks, cap * sizeof(jsmntok_t));
        if (!g) { free(toks); return 0; }
        toks = g;
    }
    if (r < 1 || toks[0].type != JSMN_OBJECT) { free(toks); return 0; }
    s_js = buf; s_jt = toks;

    int handled = 0;
    int op = (int)jt_int(jt_get(0, "op"));
    int stok = jt_get(0, "s");
    if (jt_is_prim(stok)) s_seq = (int)jt_int(stok);
    if (op == 0) {
        char t[40]; jt_gets(0, "t", t, sizeof t);
        int dd = jt_get(0, "d");
        if (strcmp(t, "READY") == 0) {
            char sid[64], rurl2[256];
            jt_gets(dd, "session_id", sid, sizeof sid);
            jt_gets(dd, "resume_gateway_url", rurl2, sizeof rurl2);
            if (sid[0])   snprintf(s_session_id, sizeof s_session_id, "%s", sid);
            if (rurl2[0]) snprintf(s_resume_url,  sizeof s_resume_url,  "%s", rurl2);
            console_dbgf("[gw] READY %d bytes -> jsmn %d tokens in %lld ms (parse core %d)",
                         len, r, (long long)(now_ms() - t0), sceKernelGetCpuId());
            parse_ready_js(dd);
            s_status = "Connected"; s_made_progress = true;
            handled = 1;
        } else if (strcmp(t, "READY_SUPPLEMENTAL") == 0) {
            parse_ready_supplemental_js(jt_get(0, "d"));
            handled = 1;
        }
    }
    s_js = NULL; s_jt = NULL;
    free(toks);
    return handled;
}

static void parse_ready(json_t *d)
{
    store_lock();
    store_clear_guilds();

    json_t *user = json_object_get(d, "user");
    if (user) {
        store_set_me(json_string_value(json_object_get(user, "username")),
                     json_string_value(json_object_get(user, "global_name")),
                     json_string_value(json_object_get(user, "id")),
                     json_string_value(json_object_get(user, "avatar")));
    }

    json_t *guilds = json_object_get(d, "guilds");
    size_t gn = json_is_array(guilds) ? json_array_size(guilds) : 0;
    for (size_t i = 0; i < gn; i++) {
        json_t *gj = json_array_get(guilds, i);
        guild *g = store_add_guild(json_string_value(json_object_get(gj, "id")),
                                   guild_name(gj),
                                   guild_icon(gj));
        if (!g) continue;
        // Our roles for this guild come from READY's merged_members (parallel to
        // guilds). Fail open if absent.
        const char *guild_id = json_string_value(json_object_get(gj, "id"));
        const char *my_id = store_me_id();
        char myroles[64][24]; int nmy = 0;
        json_t *mm = json_object_get(d, "merged_members");
        int gate = json_is_array(mm);
        if (gate) {
            json_t *ml = (i < json_array_size(mm)) ? json_array_get(mm, i) : NULL;
            json_t *meMem = NULL;
            if (json_is_array(ml)) {
                size_t mln = json_array_size(ml);
                for (size_t k = 0; k < mln; k++) {
                    json_t *mem = json_array_get(ml, k);
                    const char *uid = json_string_value(json_object_get(mem, "user_id"));
                    if (!uid) { json_t *u = json_object_get(mem, "user");
                                uid = json_string_value(json_object_get(u, "id")); }
                    if (uid && my_id && strcmp(uid, my_id) == 0) { meMem = mem; break; }
                }
                if (!meMem && mln > 0) meMem = json_array_get(ml, 0);
            }
            if (meMem) {
                json_t *rr = json_object_get(meMem, "roles");
                if (json_is_array(rr))
                    for (size_t k = 0; k < json_array_size(rr) && nmy < 64; k++) {
                        const char *rid = json_string_value(json_array_get(rr, k));
                        if (rid) snprintf(myroles[nmy++], 24, "%s", rid);
                    }
            } else gate = 0;
        }
        populate_guild(g, gj, guild_id, myroles, nmy, gate);
    }
    // Direct messages / group DMs. User-account READY lists `recipient_ids`
    // resolved against the top-level `users` array (older shapes inline `recipients`).
    json_t *users = json_object_get(d, "users");
    store_clear_dms();

    // Personal notes on users: READY `notes` is an object { user_id: "text" }.
    store_clear_notes();
    json_t *notes = json_object_get(d, "notes");
    if (json_is_object(notes)) {
        const char *nk; json_t *nv;
        json_object_foreach(notes, nk, nv)
            if (json_is_string(nv)) store_set_note(nk, json_string_value(nv));
    }

    // Relationships: type 1 = friend (DM-mode friends panel), type 2 = blocked
    // (hide their messages).
    store_clear_blocked();
    store_friends_begin();
    json_t *rels = json_object_get(d, "relationships");
    size_t reln = json_is_array(rels) ? json_array_size(rels) : 0;
    for (size_t i = 0; i < reln; i++) {
        json_t *rel = json_array_get(rels, i);
        int rtype = (int)json_integer_value(json_object_get(rel, "type"));
        json_t *ru = json_object_get(rel, "user");
        const char *uid = json_string_value(json_object_get(rel, "id"));
        if (!uid) uid = json_string_value(json_object_get(ru, "id"));
        if (!uid) continue;
        if (rtype == 2) {
            store_set_blocked(uid, true);
        } else if (rtype == 1) {  // friend
            if (!ru) ru = find_user(users, uid);
            const char *nm = user_display(ru);
            const char *av = ru ? json_string_value(json_object_get(ru, "avatar")) : NULL;
            store_add_friend(nm ? nm : "?", uid, av ? av : "");
        }
    }
    json_t *dms = json_object_get(d, "private_channels");
    size_t dn = json_is_array(dms) ? json_array_size(dms) : 0;
    for (size_t i = 0; i < dn; i++) {
        json_t *dm = json_array_get(dms, i);
        int dtype = (int)json_integer_value(json_object_get(dm, "type"));
        char name[96] = "Direct Message";
        const char *gname = json_string_value(json_object_get(dm, "name"));

        json_t *r0 = NULL;  // first recipient user object
        int extra = 0;
        json_t *recips = json_object_get(dm, "recipients");
        json_t *rids = json_object_get(dm, "recipient_ids");
        if (json_is_array(recips) && json_array_size(recips) >= 1) {
            r0 = json_array_get(recips, 0);
            extra = (int)json_array_size(recips) - 1;
        } else if (json_is_array(rids) && json_array_size(rids) >= 1) {
            r0 = find_user(users, json_string_value(json_array_get(rids, 0)));
            extra = (int)json_array_size(rids) - 1;
        }

        const char *base = user_display(r0);
        if (gname && gname[0])
            snprintf(name, sizeof(name), "%s", gname);  // named group
        else if (base && extra > 0)
            snprintf(name, sizeof(name), "%s +%d", base, extra);
        else if (base)
            snprintf(name, sizeof(name), "%s", base);

        const char *rav = r0 ? json_string_value(json_object_get(r0, "avatar")) : NULL;
        const char *rid = r0 ? json_string_value(json_object_get(r0, "id")) : NULL;
        // A group DM has its own icon (not a recipient avatar); store that hash.
        if (dtype == CH_GROUP_DM)
            rav = json_string_value(json_object_get(dm, "icon"));
        console_dbgf("[gw] DM '%s' rid=%s av=%s", name, rid ? rid : "(none)",
                     rav ? rav : "(none)");
        store_add_dm(json_string_value(json_object_get(dm, "id")), name, dtype,
                     rav, rid,
                     json_string_value(json_object_get(dm, "last_message_id")));
    }

    // Unread state: read_state lists the last message the user has read per
    // channel; a channel is unread if its last_message_id is newer.
    json_t *rs = json_object_get(d, "read_state");
    if (json_is_object(rs)) rs = json_object_get(rs, "entries");
    size_t rn = json_is_array(rs) ? json_array_size(rs) : 0;
    for (size_t i = 0; i < rn; i++) {
        json_t *e = json_array_get(rs, i);
        channel *c = store_find_channel(json_string_value(json_object_get(e, "id")));
        if (!c) continue;
        const char *lr = json_string_value(json_object_get(e, "last_message_id"));
        if (lr) snprintf(c->last_read_id, sizeof(c->last_read_id), "%s", lr);
        c->mentions = (int)json_integer_value(json_object_get(e, "mention_count"));
        c->unread = snowflake_gt(c->last_message_id, lr) || c->mentions > 0;
    }

    // Seed presence from READY. User accounts use `merged_presences`; some
    // payloads also carry a flat `presences` array.
    for (int pass = 0; pass < 2; pass++) {
        json_t *ps = json_object_get(d, pass ? "presences" : "merged_presences");
        if (pass == 0 && json_is_object(ps)) ps = json_object_get(ps, "friends");
        if (!json_is_array(ps)) continue;
        size_t pn = json_array_size(ps);
        for (size_t i = 0; i < pn; i++) {
            json_t *p = json_array_get(ps, i);
            const char *uid = json_string_value(json_object_get(p, "user_id"));
            if (!uid) uid = json_string_value(
                json_object_get(json_object_get(p, "user"), "id"));
            if (!uid) continue;
            store_set_presence(uid,
                status_from_str(json_string_value(json_object_get(p, "status"))));
            char act[128]; activity_text(p, act, sizeof(act));
            if (act[0]) store_set_activity(uid, act);
        }
    }

    store_set_ready(true);
    store_unlock();
    console_dbgf("[gw] READY parsed: %d guild(s), %d DM(s), read_state=%d",
                 (int)gn, (int)dn, (int)rn);

    // Saved sidebar order (guild_folders / guild_positions), applied after the
    // store unlock since store_set_guild_order locks.
    {
        json_t *us       = json_object_get(d, "user_settings");
        json_t *folders  = json_object_get(d, "guild_folders");
        if (!json_is_array(folders) && json_is_object(us))
            folders = json_object_get(us, "guild_folders");
        json_t *positions = json_object_get(d, "guild_positions");
        if (!json_is_array(positions) && json_is_object(us))
            positions = json_object_get(us, "guild_positions");
        json_t *proto = json_object_get(d, "user_settings_proto");

        static char order[MAX_GUILDS][24];
        int on = 0;
        if (json_is_array(folders)) {
            size_t nf = json_array_size(folders);
            for (size_t i = 0; i < nf && on < MAX_GUILDS; i++) {
                json_t *gids = json_object_get(json_array_get(folders, i), "guild_ids");
                if (!json_is_array(gids)) continue;
                size_t ng = json_array_size(gids);
                for (size_t j = 0; j < ng && on < MAX_GUILDS; j++) {
                    json_t *v = json_array_get(gids, j);
                    if (json_is_string(v))
                        snprintf(order[on++], 24, "%s", json_string_value(v));
                    else if (json_is_integer(v))
                        snprintf(order[on++], 24, "%lld", (long long)json_integer_value(v));
                }
            }
        }
        if (on == 0 && json_is_array(positions)) {
            size_t np = json_array_size(positions);
            for (size_t i = 0; i < np && on < MAX_GUILDS; i++) {
                json_t *v = json_array_get(positions, i);
                if (json_is_string(v))
                    snprintf(order[on++], 24, "%s", json_string_value(v));
                else if (json_is_integer(v))
                    snprintf(order[on++], 24, "%lld", (long long)json_integer_value(v));
            }
        }
        console_dbgf("[order] READY user_settings=%d folders=%d positions=%d proto=%d -> %d ids",
                     json_is_object(us), json_is_array(folders),
                     json_is_array(positions), proto != NULL, on);
        // Modern accounts ship the order only in the base64 protobuf blob.
        if (on == 0 && json_is_string(proto))
            on = parse_guild_order_proto(json_string_value(proto), order, MAX_GUILDS);
        if (on > 0) store_set_guild_order((const char (*)[24])order, on);
    }
}

// THREAD_CREATE / THREAD_UPDATE (add=1) or THREAD_DELETE (add=0). A thread is a
// channel with a parent_id; re-add on update by removing the stale copy first.
static void parse_thread(json_t *d, int add)
{
    const char *id = json_string_value(json_object_get(d, "id"));
    if (!id) return;
    store_lock();
    store_remove_channel(id);
    if (add) {
        guild *g = store_find_guild(json_string_value(json_object_get(d, "guild_id")));
        if (g) store_add_channel(g, id,
            json_string_value(json_object_get(d, "name")),
            (int)json_integer_value(json_object_get(d, "type")),
            (int)json_integer_value(json_object_get(d, "position")),
            json_string_value(json_object_get(d, "parent_id")),
            json_string_value(json_object_get(d, "last_message_id")));
    }
    store_unlock();
}

static void parse_message_create(json_t *d)
{
    const char *cid = json_string_value(json_object_get(d, "channel_id"));
    if (!cid) return;
    const char *mid = json_string_value(json_object_get(d, "id"));
    store_lock();
    if (strcmp(cid, store_open_channel()) == 0) {
        message msg;
        discord_json_to_message(d, &msg);
        store_push_message(&msg);
        channel *c = store_find_channel(cid);  // keep it read + current
        if (c) { c->unread = false; c->mentions = 0;
                 if (mid) snprintf(c->last_message_id, sizeof(c->last_message_id), "%s", mid); }
    } else {
        // mark unread; count a mention if it @-mentions us or @everyone
        bool mention = json_is_true(json_object_get(d, "mention_everyone"));
        json_t *ms = json_object_get(d, "mentions");
        const char *me = store_me_id();
        if (!mention && json_is_array(ms) && me && me[0]) {
            size_t n = json_array_size(ms);
            for (size_t i = 0; i < n; i++) {
                const char *uid = json_string_value(
                    json_object_get(json_array_get(ms, i), "id"));
                if (uid && strcmp(uid, me) == 0) { mention = true; break; }
            }
        }
        store_bump_unread(cid, mid, mention);
    }
    store_unlock();
}

static void parse_message_update(json_t *d)
{
    const char *cid = json_string_value(json_object_get(d, "channel_id"));
    const char *content = json_string_value(json_object_get(d, "content"));
    if (!cid || !content) return;  // embed/other updates carry no content
    store_lock();
    if (strcmp(cid, store_open_channel()) == 0)
        store_update_message(json_string_value(json_object_get(d, "id")), content);
    store_unlock();
}

static void parse_message_delete(json_t *d)
{
    const char *cid = json_string_value(json_object_get(d, "channel_id"));
    if (!cid) return;
    store_lock();
    if (strcmp(cid, store_open_channel()) == 0)
        store_delete_message(json_string_value(json_object_get(d, "id")));
    store_unlock();
}

// MESSAGE_REACTION_ADD / _REMOVE. `add` selects which. d carries channel_id,
// message_id, user_id, and emoji{name,id,animated}.
static void parse_reaction(json_t *d, bool add)
{
    const char *cid = json_string_value(json_object_get(d, "channel_id"));
    const char *mid = json_string_value(json_object_get(d, "message_id"));
    if (!cid || !mid) return;
    json_t *em = json_object_get(d, "emoji");
    const char *name = json_string_value(json_object_get(em, "name"));
    const char *eid  = json_string_value(json_object_get(em, "id"));
    bool animated = json_boolean_value(json_object_get(em, "animated"));
    const char *uid = json_string_value(json_object_get(d, "user_id"));
    store_lock();
    bool me = uid && store_me_id() && strcmp(uid, store_me_id()) == 0;
    if (strcmp(cid, store_open_channel()) == 0) {
        if (add) store_reaction_add(mid, name, eid, animated, me);
        else     store_reaction_remove(mid, name, eid, me);
    }
    store_unlock();
}

// MESSAGE_REACTION_REMOVE_ALL: clears every reaction on a message.
static void parse_reaction_clear(json_t *d)
{
    const char *cid = json_string_value(json_object_get(d, "channel_id"));
    const char *mid = json_string_value(json_object_get(d, "message_id"));
    if (!cid || !mid) return;
    store_lock();
    if (strcmp(cid, store_open_channel()) == 0)
        store_reaction_clear(mid);
    store_unlock();
}

static int status_from_str(const char *s)
{
    if (!s) return 0;
    if (strcmp(s, "online") == 0) return 1;
    if (strcmp(s, "idle") == 0)   return 2;
    if (strcmp(s, "dnd") == 0)    return 3;
    return 0;  // offline / invisible
}

// Best single activity line from a presence's `activities`: a custom status
// (type 4, emoji + state) if present, else the first game ("Playing <name>" etc).
static void activity_text(json_t *d, char *out, size_t cap)
{
    out[0] = '\0';
    json_t *acts = json_object_get(d, "activities");
    if (!json_is_array(acts)) return;
    size_t n = json_array_size(acts);
    const char *game = NULL; int gametype = -1;
    for (size_t i = 0; i < n; i++) {
        json_t *a = json_array_get(acts, i);
        int type = (int)json_integer_value(json_object_get(a, "type"));
        if (type == 4) {  // custom status
            const char *state = json_string_value(json_object_get(a, "state"));
            json_t *em = json_object_get(a, "emoji");
            const char *ename = em ? json_string_value(json_object_get(em, "name")) : NULL;
            int custom_emoji = em && json_object_get(em, "id");  // skip custom (needs CDN)
            if (ename && !custom_emoji && state && state[0])
                snprintf(out, cap, "%s %s", ename, state);
            else if (state && state[0])
                snprintf(out, cap, "%s", state);
            if (out[0]) return;  // custom status wins
        } else if (!game) {
            game = json_string_value(json_object_get(a, "name"));
            gametype = type;
        }
    }
    if (game && game[0]) {
        const char *verb = gametype == 1 ? "Streaming "     : gametype == 2 ? "Listening to " :
                           gametype == 3 ? "Watching "      : gametype == 5 ? "Competing in " :
                                           "Playing ";
        snprintf(out, cap, "%s%s", verb, game);
    }
}

static void parse_presence(json_t *d)
{
    json_t *user = json_object_get(d, "user");
    const char *uid = json_string_value(json_object_get(user, "id"));
    if (!uid) return;
    int st = status_from_str(json_string_value(json_object_get(d, "status")));
    char act[128]; activity_text(d, act, sizeof(act));
    store_lock();
    store_set_presence(uid, st);
    store_set_activity(uid, act);
    store_unlock();
    store_touch();  // bump the store rev so the idle render loop repaints the dot
}

// Seed one presence array (READY_SUPPLEMENTAL merged_presences). Caller locks.
static int seed_presences(json_t *arr)
{
    if (!json_is_array(arr)) return 0;
    size_t n = json_array_size(arr); int seeded = 0;
    for (size_t i = 0; i < n; i++) {
        json_t *p = json_array_get(arr, i);
        const char *uid = json_string_value(json_object_get(p, "user_id"));
        if (!uid) uid = json_string_value(json_object_get(json_object_get(p, "user"), "id"));
        if (!uid) continue;
        store_set_presence(uid, status_from_str(json_string_value(json_object_get(p, "status"))));
        char act[128]; activity_text(p, act, sizeof(act));
        if (act[0]) store_set_activity(uid, act);
        seeded++;
    }
    return seeded;
}

// READY_SUPPLEMENTAL: user accounts ship `merged_presences` here as a separate
// dispatch after READY. .friends = friend statuses; .guilds = per-guild members.
static void parse_ready_supplemental(json_t *d)
{
    json_t *mp = json_object_get(d, "merged_presences");
    if (!json_is_object(mp)) { console_dbgf("[gw] READY_SUPPLEMENTAL: no merged_presences"); return; }
    int nf = 0, ng = 0;
    store_lock();
    nf = seed_presences(json_object_get(mp, "friends"));
    json_t *gp = json_object_get(mp, "guilds");
    if (json_is_array(gp)) {
        size_t gn = json_array_size(gp);
        for (size_t i = 0; i < gn; i++) ng += seed_presences(json_array_get(gp, i));
    }
    store_unlock();
    store_touch();
    console_dbgf("[gw] READY_SUPPLEMENTAL: seeded %d friend + %d member presences", nf, ng);
}

// Member-list group counts live in the top-level `groups` array, keyed by id.
static int group_count(json_t *groups, const char *id)
{
    if (!json_is_array(groups) || !id) return 0;
    size_t n = json_array_size(groups);
    for (size_t i = 0; i < n; i++) {
        json_t *g = json_array_get(groups, i);
        const char *gi = json_string_value(json_object_get(g, "id"));
        if (gi && strcmp(gi, id) == 0)
            return (int)json_integer_value(json_object_get(g, "count"));
    }
    return 0;
}

static void parse_member_list_update(json_t *d)
{
    const char *gid = json_string_value(json_object_get(d, "guild_id"));
    if (!gid) return;
    json_t *groups = json_object_get(d, "groups");
    json_t *ops = json_object_get(d, "ops");
    if (!json_is_array(ops)) { console_dbgf("[gw] mlist g=%.12s no-ops", gid); return; }

    // Only a SYNC op carries the full member range. Ignore incremental
    // INSERT/UPDATE/DELETE ops that arrive as presence changes.
    size_t no = json_array_size(ops);
    bool has_sync = false;
    for (size_t o = 0; o < no; o++) {
        const char *opt = json_string_value(json_object_get(json_array_get(ops, o), "op"));
        if (opt && strcmp(opt, "SYNC") == 0) { has_sync = true; break; }
    }
    console_dbgf("[gw] mlist g=%.12s ops=%d sync=%d", gid, (int)no, (int)has_sync);
    if (!has_sync) return;

    store_lock();
    store_members_begin(gid);
    for (size_t o = 0; o < no; o++) {
        json_t *op = json_array_get(ops, o);
        const char *opt = json_string_value(json_object_get(op, "op"));
        if (!opt || strcmp(opt, "SYNC") != 0) continue;
        json_t *items = json_object_get(op, "items");
        size_t ni = json_is_array(items) ? json_array_size(items) : 0;
        for (size_t i = 0; i < ni; i++) {
            json_t *it = json_array_get(items, i);
            json_t *grp = json_object_get(it, "group");
            json_t *mem = json_object_get(it, "member");
            if (json_is_object(grp)) {
                const char *id = json_string_value(json_object_get(grp, "id"));
                int cnt = (int)json_integer_value(json_object_get(grp, "count"));
                if (cnt == 0) cnt = group_count(groups, id);
                const char *label = "MEMBERS";
                if (id && strcmp(id, "online") == 0)  label = "ONLINE";
                else if (id && strcmp(id, "offline") == 0) label = "OFFLINE";
                else { const char *rn = store_role_name(id); if (rn && rn[0]) label = rn; }
                char up[64]; int k = 0;
                for (const char *p = label; *p && k < 63; p++, k++)
                    up[k] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
                up[k] = '\0';
                store_add_member_group(up, cnt);
            } else if (json_is_object(mem)) {
                json_t *u = json_object_get(mem, "user");
                const char *nick = json_string_value(json_object_get(mem, "nick"));
                const char *disp = (nick && nick[0]) ? nick : user_display(u);
                json_t *pres = json_object_get(mem, "presence");
                int st = status_from_str(pres ?
                    json_string_value(json_object_get(pres, "status")) : NULL);
                store_add_member(disp,
                    u ? json_string_value(json_object_get(u, "id")) : NULL,
                    u ? json_string_value(json_object_get(u, "avatar")) : NULL, st);
            }
        }
    }
    store_unlock();
}

static void parse_typing(json_t *d, long long now)
{
    const char *cid = json_string_value(json_object_get(d, "channel_id"));
    if (!cid) return;
    const char *name = "Someone";
    json_t *member = json_object_get(d, "member");
    json_t *user = member ? json_object_get(member, "user") : NULL;
    if (user) {
        const char *gn = json_string_value(json_object_get(user, "global_name"));
        const char *un = json_string_value(json_object_get(user, "username"));
        if (gn && gn[0]) name = gn; else if (un) name = un;
    }
    store_lock();
    if (strcmp(cid, store_open_channel()) == 0)
        store_set_typing(cid, name, now);
    store_unlock();
}

// Returns: 0 = clean stop, 1 = reconnect+resume, 2 = reconnect+reidentify,
// 3 = fatal (do not reconnect -- e.g. 4004 auth failed / dead token).
static int run_connection(bool resuming)
{
    const char *url = (resuming && s_resume_url[0]) ? s_resume_url : GATEWAY_WSS;
    // resume_gateway_url lacks the query string; append it (compression included).
    char rurl[320];
    if (resuming && s_resume_url[0]) {
        snprintf(rurl, sizeof(rurl), "%s/?v=10&encoding=json&compress=zlib-stream", s_resume_url);
        url = rurl;
    }

    s_status = "Connecting";
    console_dbgf("[gw] connecting (%s)", resuming ? "resume" : "identify");
    int result = 2;
    net_lock();
    ws_client *ws = ws_open(url, s_cacert, "https://discord.com", discord_user_agent());
    net_unlock();
    if (!ws) {
        console_logf("[gw] connect failed");
        return resuming ? 1 : 2;
    }

    char *buf = NULL; size_t cap = 0;
    int hb = 0; int64_t last_hb = now_ms(); bool acked = true;

    while (s_run) {
        int n = ws_read_message(ws, &buf, &cap, 500);
        if (n < 0) {
            // 4004 = token no longer valid: stop, don't reconnect. 4010-4014 =
            // non-resumable protocol errors: stop too. Anything else is transient: resume.
            int cc = ws_last_close_code(ws);
            if (cc == 4004) { s_auth_failed = true; result = 3; }
            else if (cc >= 4010 && cc <= 4014) { result = 3; }
            else { result = 1; }
            break;
        }
        if (n > 0) {
            json_error_t e;
            // Big messages (READY / READY_SUPPLEMENTAL) go through jsmn; small live
            // events use jansson.
            if (n > 200000 && handle_big_message(buf, n)) {
            } else {
            json_t *root = json_loads(buf, 0, &e);
            if (root) {
                int op = (int)json_integer_value(json_object_get(root, "op"));
                json_t *sj = json_object_get(root, "s");
                if (json_is_integer(sj)) s_seq = (int)json_integer_value(sj);

                if (op == 10) {
                    hb = (int)json_integer_value(json_object_get(
                        json_object_get(root, "d"), "heartbeat_interval"));
                    console_dbgf("[gw] HELLO hb=%d", hb);
                    if (resuming) send_resume(ws); else send_identify(ws);
                    // Jitter the first heartbeat by hb * rand[0,1).
                    unsigned char jb = 128; sceKernelGetRandomNumber(&jb, 1);
                    last_hb = now_ms() - (int64_t)((long long)hb * (255 - jb) / 255);
                    acked = true;
                } else if (op == 11) {
                    acked = true;
                } else if (op == 1) {  // server asked us to heartbeat now
                    ws_send(ws, "{\"op\":1,\"d\":null}"); last_hb = now_ms();
                } else if (op == 7) {  // reconnect
                    console_dbgf("[gw] op7 reconnect"); result = 1; break;
                } else if (op == 9) {  // invalid session
                    console_logf("[gw] op9 invalid session");
                    bool resumable = json_is_true(json_object_get(root, "d"));
                    result = resumable ? 1 : 2;
                    json_decref(root); break;
                } else if (op == 0) {  // dispatch
                    const char *t = json_string_value(json_object_get(root, "t"));
                    json_t *d = json_object_get(root, "d");
                    if (t && strcmp(t, "READY") == 0) {
                        const char *sid = json_string_value(json_object_get(d, "session_id"));
                        const char *rurl2 = json_string_value(json_object_get(d, "resume_gateway_url"));
                        if (sid) snprintf(s_session_id, sizeof(s_session_id), "%s", sid);
                        if (rurl2) snprintf(s_resume_url, sizeof(s_resume_url), "%s", rurl2);
                        parse_ready(d);
                        s_status = "Connected";
                        s_made_progress = true;
                    } else if (t && strcmp(t, "READY_SUPPLEMENTAL") == 0) {
                        parse_ready_supplemental(d);  // friend/member presences live here
                    } else if (t && strcmp(t, "MESSAGE_CREATE") == 0) {
                        parse_message_create(d);
                    } else if (t && strcmp(t, "MESSAGE_UPDATE") == 0) {
                        parse_message_update(d);
                    } else if (t && strcmp(t, "MESSAGE_DELETE") == 0) {
                        parse_message_delete(d);
                    } else if (t && strcmp(t, "MESSAGE_REACTION_ADD") == 0) {
                        parse_reaction(d, true);
                    } else if (t && strcmp(t, "MESSAGE_REACTION_REMOVE") == 0) {
                        parse_reaction(d, false);
                    } else if (t && strcmp(t, "MESSAGE_REACTION_REMOVE_ALL") == 0) {
                        parse_reaction_clear(d);
                    } else if (t && strcmp(t, "TYPING_START") == 0) {
                        parse_typing(d, now_ms());
                    } else if (t && strcmp(t, "GUILD_MEMBER_LIST_UPDATE") == 0) {
                        parse_member_list_update(d);
                    } else if (t && strcmp(t, "PRESENCE_UPDATE") == 0) {
                        parse_presence(d);
                    } else if (t && (strcmp(t, "THREAD_CREATE") == 0 ||
                                     strcmp(t, "THREAD_UPDATE") == 0)) {
                        parse_thread(d, 1);
                    } else if (t && strcmp(t, "THREAD_DELETE") == 0) {
                        parse_thread(d, 0);
                    } else if (t && strcmp(t, "RELATIONSHIP_ADD") == 0) {
                        if ((int)json_integer_value(json_object_get(d, "type")) == 2) {
                            store_lock();
                            store_set_blocked(json_string_value(json_object_get(d, "id")), true);
                            store_unlock();
                            store_touch();
                        }
                    } else if (t && strcmp(t, "GUILD_CREATE") == 0) {
                        store_lock(); parse_guild_create(d); store_unlock();
                        store_touch();
                    } else if (t && strcmp(t, "GUILD_DELETE") == 0) {
                        const char *gid = json_string_value(json_object_get(d, "id"));
                        if (gid) { store_lock(); store_remove_guild(gid); store_unlock();
                                   store_touch(); }
                    } else if (t && strcmp(t, "RELATIONSHIP_UPDATE") == 0) {
                        // Type can change (e.g. to/from blocked); sync the set.
                        int rtype = (int)json_integer_value(json_object_get(d, "type"));
                        store_lock();
                        store_set_blocked(json_string_value(json_object_get(d, "id")),
                                          rtype == 2);
                        store_unlock();
                        store_touch();
                    } else if (t && strcmp(t, "RELATIONSHIP_REMOVE") == 0) {
                        store_lock();
                        store_set_blocked(json_string_value(json_object_get(d, "id")), false);
                        store_unlock();
                        store_touch();
                    } else if (t && strcmp(t, "RESUMED") == 0) {
                        console_dbgf("[gw] RESUMED"); s_status = "Connected";
                        s_made_progress = true;
                    } else if (t && strcmp(t, "VOICE_SERVER_UPDATE") == 0) {
                        // Voice gateway token + endpoint; the voice session_id is this gateway's.
                        const char *vtok = json_string_value(json_object_get(d, "token"));
                        const char *vep  = json_string_value(json_object_get(d, "endpoint"));
                        const char *vgid = json_string_value(json_object_get(d, "guild_id"));
                        console_dbgf("[gw] VOICE_SERVER_UPDATE endpoint=%s", vep ? vep : "(null)");
                        if (vep)  // null endpoint means "no server yet"; wait for the next
                            voice_on_server(vtok, vep, s_session_id, vgid, store_me_id());
                    }
                }
                json_decref(root);
            }
            }  // close the jansson (non-big-message) branch
        }
        if (hb > 0 && now_ms() - last_hb >= hb) {
            if (!acked) { console_logf("[gw] zombie (no ack) -> resume");
                          result = 1; break; }
            char hbmsg[48];
            snprintf(hbmsg, sizeof(hbmsg), "{\"op\":1,\"d\":%d}", s_seq);
            ws_send(ws, hbmsg);
            last_hb = now_ms(); acked = false;
        }

        if (s_want_members && s_mem_guild[0] && s_mem_channel[0]) {
            s_want_members = 0;
            json_t *arr = json_array();
            json_array_append_new(arr, json_pack("[i,i]", 0, 99));
            json_t *channels = json_object();
            json_object_set_new(channels, s_mem_channel, arr);
            json_t *dd = json_pack("{s:s, s:o}", "guild_id", s_mem_guild,
                                   "channels", channels);
            json_t *p = json_pack("{s:i, s:o}", "op", 14, "d", dd);
            char *txt = json_dumps(p, JSON_COMPACT);
            json_decref(p);
            if (txt) { ws_send(ws, txt); free(txt); }
            console_dbgf("[gw] op14 member request g=%s", s_mem_guild);
        }

        if (s_want_status) {
            s_want_status = 0;
            json_t *acts = json_array();
            if (s_custom_status[0])  // custom status = a type-4 activity
                json_array_append_new(acts, json_pack("{s:s, s:i, s:s}",
                    "name", "Custom Status", "type", 4, "state", s_custom_status));
            json_t *dd = json_pack("{s:i, s:o, s:s, s:b}",
                                   "since", 0, "activities", acts, "status", s_status_str,
                                   "afk", 0);
            json_t *p = json_pack("{s:i, s:o}", "op", 3, "d", dd);
            char *txt = json_dumps(p, JSON_COMPACT);
            json_decref(p);
            if (txt) { ws_send(ws, txt); free(txt); }
            console_dbgf("[gw] presence -> %s custom='%s'", s_status_str, s_custom_status);
        }

        if (s_want_voice) {
            s_want_voice = 0;
            // op4 Voice State Update: move into s_voice_chan, or out (null) if empty.
            json_t *dd = json_pack("{s:s, s:b, s:b}",
                                   "guild_id", s_voice_guild, "self_mute", 0, "self_deaf", 0);
            if (s_voice_chan[0]) json_object_set_new(dd, "channel_id", json_string(s_voice_chan));
            else                 json_object_set_new(dd, "channel_id", json_null());
            json_t *p = json_pack("{s:i, s:o}", "op", 4, "d", dd);
            char *txt = json_dumps(p, JSON_COMPACT);
            json_decref(p);
            if (txt) { ws_send(ws, txt); free(txt); }
            console_dbgf("[gw] op4 voice guild=%s chan=%s", s_voice_guild,
                         s_voice_chan[0] ? s_voice_chan : "(leave)");
        }
    }

    free(buf);
    net_lock();
    ws_close(ws);
    net_unlock();
    if (!s_run) return 0;
    return result;
}

static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    bool resuming = false;
    int backoff_ms = 1000;  // exponential, capped; reset on success
    while (s_run) {
        s_made_progress = false;
        int r = run_connection(resuming);
        if (r == 0 || r == 3) break;  // clean stop, or fatal (dead token)
        resuming = (r == 1);
        if (r == 2) { s_seq = -1; s_session_id[0] = 0; s_resume_url[0] = 0; }
        if (s_made_progress) backoff_ms = 1000;
        // Back off before reconnecting. Sleep in small slices so gateway_stop()
        // is honoured promptly.
        for (int slept = 0; slept < backoff_ms && s_run; slept += 100)
            sceKernelDelayThread(100 * 1000);
        backoff_ms = backoff_ms < 30000 ? backoff_ms * 2 : 60000;  // cap 60s
    }
    s_status = s_auth_failed ? "Signed out" : "Disconnected";
    return 0;
}

int gateway_start(const char *token, const char *cacert_path)
{
    snprintf(s_token, sizeof(s_token), "%s", token);
    snprintf(s_cacert, sizeof(s_cacert), "%s", cacert_path);
    s_seq = -1; s_session_id[0] = 0; s_resume_url[0] = 0;
    s_auth_failed = false; s_made_progress = false;
    s_run = true;
    // Pin the worker to CPU core 2 (main/render is on core 0) so the CPU-bound
    // READY parse runs in parallel with rendering.
    s_thread = sceKernelCreateThread("gw_worker", worker, 0x40, 0x100000, 0,
                                     SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s_thread < 0) { s_run = false; return -1; }
    sceKernelStartThread(s_thread, 0, NULL);
    return 0;
}

void gateway_stop(void)
{
    if (!s_run) return;
    s_run = false;
    if (s_thread >= 0) {
        sceKernelWaitThreadEnd(s_thread, NULL, NULL);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
}

bool gateway_auth_failed(void) { return s_auth_failed; }

void gateway_request_members(const char *guild_id, const char *channel_id)
{
    snprintf(s_mem_guild, sizeof(s_mem_guild), "%s", guild_id ? guild_id : "");
    snprintf(s_mem_channel, sizeof(s_mem_channel), "%s", channel_id ? channel_id : "");
    s_want_members = 1;
}

void gateway_set_status(const char *status)
{
    snprintf(s_status_str, sizeof(s_status_str), "%s", status ? status : "online");
    s_want_status = 1;
}

void gateway_set_custom_status(const char *text)
{
    snprintf(s_custom_status, sizeof(s_custom_status), "%s", text ? text : "");
    s_want_status = 1;
}

void gateway_voice_update(const char *guild_id, const char *channel_id)
{
    snprintf(s_voice_guild, sizeof(s_voice_guild), "%s", guild_id ? guild_id : "");
    snprintf(s_voice_chan, sizeof(s_voice_chan), "%s", channel_id ? channel_id : "");
    s_want_voice = 1;
}
