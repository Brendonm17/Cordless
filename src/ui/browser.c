#include "ui/browser.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/ime.h"
#include "ui/inlinekbd.h"
#include "util/clipboard.h"
#include "ui/photo.h"
#include "ui/camera.h"
#include "ui/imagecache.h"
#include "auth/tokenstore.h"
#include "media/video_player.h"
#include "model/store.h"
#include "model/settings.h"
#include "api/discord.h"
#include "gateway/gateway.h"
#include "voice/voice.h"
#include "console.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>  // strcasecmp
#include <math.h>

#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <psp2/io/fcntl.h>  // sceIoOpen

static long long now_ms(void)
{
    SceRtcTick t; sceRtcGetCurrentTick(&t);
    return (long long)(t.tick / 1000);
}

#define CL_VERSION "1.00"  // shown in Settings > About

#define RAIL_W     72
#define SIDE_X     RAIL_W
#define SIDE_W     240
#define MSG_X_FULL (SIDE_X + SIDE_W)
#define MEMBER_W   170
#define TOPBAR_H   48

static char s_token[128];
static char s_cacert[256];
static char s_cur_guild_id[24];  // guild whose channel is open (empty = DM)
static int  s_member_w;  // member panel width this frame (0 = hidden)
static bool s_members_collapsed;
static int  s_memtoggle_r[4];
static bool s_channels_collapsed;
static int  s_chtoggle_r[4];
static int  g_msg_x = MSG_X_FULL;  // message pane left edge
static int  s_member_scroll;
static int  s_member_max;

#ifdef GUILD_TEST
static int  s_guild = 0;
#else
static int  s_guild = -1;  // -1 = Home/DMs, else guild index
#endif
static int  s_chan_sel = 0;
static bool s_restored;  // one-shot: restored the last-open view after READY
static int  s_side_scroll = 0;
static int  s_side_max = 0;

// collapsed category ids
static char s_collapsed[32][24];
static int  s_ncollapsed;
static unsigned s_cat_foldgen;  // bumps on any fold change -> invalidates the row cache
static bool cat_collapsed(const char *id)
{
    for (int i = 0; i < s_ncollapsed; i++) if (strcmp(s_collapsed[i], id) == 0) return true;
    return false;
}
static void toggle_cat(const char *id)
{
    if (!id || !id[0]) return;
    s_cat_foldgen++;
    for (int i = 0; i < s_ncollapsed; i++)
        if (strcmp(s_collapsed[i], id) == 0) {
            memmove(s_collapsed[i], s_collapsed[i + 1], 24 * (s_ncollapsed - i - 1));
            s_ncollapsed--; return;
        }
    if (s_ncollapsed < 32) snprintf(s_collapsed[s_ncollapsed++], 24, "%s", id);
}
static int  s_rail_scroll = 0;

#define RAIL_G0   106  // = home hy(40) + 66
#define RAIL_PITCH 60
static int rail_max_scroll(int gcount)
{
    int content = (gcount + 1) * RAIL_PITCH;  // +1 for the "add server" button
    int avail = SCREEN_H - RAIL_G0 - 62;  // reserve the bottom user-area strip
    return content > avail ? content - avail : 0;
}

// rail display order: guild indices in the user's saved sidebar order,
// unlisted guilds appended in load order
static int s_rail_order[MAX_GUILDS];
static int s_rail_order_n;
static void rail_compute_order(guild *gs, int gcount)
{
    if (gcount > MAX_GUILDS) gcount = MAX_GUILDS;
    bool used[MAX_GUILDS];
    for (int i = 0; i < gcount; i++) used[i] = false;
    int n = 0, on = store_guild_order_count();
    for (int k = 0; k < on && n < gcount; k++) {
        const char *id = store_guild_order_id(k);
        if (!id || !id[0]) continue;
        for (int i = 0; i < gcount; i++)
            if (!used[i] && strcmp(gs[i].id, id) == 0) {
                s_rail_order[n++] = i; used[i] = true; break;
            }
    }
    for (int i = 0; i < gcount && n < gcount; i++)
        if (!used[i]) s_rail_order[n++] = i;
    s_rail_order_n = n;
}
static int s_addsrv_y0 = -1, s_addsrv_y1 = -1;
static int  s_msg_scroll = 0;
static bool s_want_bottom = true;
static bool s_pending_open = true;  // open the default item on view entry
static int  s_prev_max = 0;  // last frame's max scroll (live-follow)

static char s_unread_boundary[24];  // last-read message id, captured at open
static bool s_want_unread;  // pending: focus the unread boundary once loaded
static bool s_jump_show;
static int  s_jump_r[3];  // hit circle: cx, cy, r

// snowflake (message-id) ordering: longer string = larger, else lexicographic
static bool snowflake_gt(const char *a, const char *b)
{
    if (!a || !a[0]) return false;
    if (!b || !b[0]) return true;
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return la > lb;
    return strcmp(a, b) > 0;
}

static SceUID s_fetch_thread = -1;
static char   s_fetch_cid[24];
// the view actually open; can differ from the sidebar selection
static channel s_open_ch;
static bool    s_have_open;
static int     s_back_r[4];
static char   s_fetch_around[24];  // load window centered here (last-read), if set
static char   s_ack_newest[24];  // channel's true newest id, to persist read state
static volatile int s_fetch_http;

static volatile bool s_loading_older;
static volatile bool s_history_done;  // reached the start of the channel
static char s_older_cid[24];
static char s_restore_anchor[24];  // keep this msg id at the top after prepend

typedef struct { int y0, y1; char id[24]; bool own; bool has_spoiler;
                 int av_cy;  // avatar center y (-1 if grouped/none)
                 char author_id[24]; char author[80]; char avatar[40]; } msg_hit;

typedef struct { char mid[24]; int aid, x0, y0, x1, y1; } poll_hit;
static poll_hit s_phit[24];
static int      s_nphit;

typedef struct { char mid[24]; char emoji[80]; bool me; int x0, y0, x1, y1; } react_hit;
static react_hit s_rhit[40];
static int       s_nrhit;

typedef struct { char url[600]; int x0, y0, x1, y1; } img_hit;
static img_hit  s_imghit[24];
static int      s_nimghit;
static bool     s_lightbox_open;
static char     s_lightbox_url[600];

// copy src to dst, scaling the width=/height= params so the larger side is d
// while keeping the aspect ratio; URLs without those params copy unchanged
static void make_view_url(const char *src, char *dst, size_t dstsz, int d)
{
    int w = 0, h = 0; const char *q;
    if ((q = strstr(src, "width=")))  { q += 6; while (*q >= '0' && *q <= '9') w = w * 10 + (*q++ - '0'); }
    if ((q = strstr(src, "height="))) { q += 7; while (*q >= '0' && *q <= '9') h = h * 10 + (*q++ - '0'); }
    int nw = d, nh = d;
    if (w > 0 && h > 0) {
        if (w >= h) { nw = d; nh = (int)((long long)h * d / w); }
        else        { nh = d; nw = (int)((long long)w * d / h); }
        if (nw < 1) nw = 1; if (nh < 1) nh = 1;
    }
    size_t o = 0;
    const char *p = src;
    while (*p && o + 1 < dstsz) {
        int kl = !strncmp(p, "width=", 6) ? 6 : (!strncmp(p, "height=", 7) ? 7 : 0);
        if (kl) {
            for (int k = 0; k < kl && o + 1 < dstsz; k++) dst[o++] = *p++;
            while (*p >= '0' && *p <= '9') p++;
            char num[12]; int nn = snprintf(num, sizeof(num), "%d", kl == 6 ? nw : nh);
            for (int k = 0; k < nn && o + 1 < dstsz; k++) dst[o++] = num[k];
        } else {
            dst[o++] = *p++;
        }
    }
    dst[o] = '\0';
}

// message ids whose ||spoilers|| the user tapped to reveal (ring buffer)
static char s_spoil[32][24];
static int  s_spoil_n;
static bool spoiler_shown(const char *id)
{
    for (int i = 0; i < s_spoil_n; i++) if (strcmp(s_spoil[i], id) == 0) return true;
    return false;
}
static void spoiler_reveal(const char *id)
{
    if (spoiler_shown(id)) return;
    if (s_spoil_n < 32) { snprintf(s_spoil[s_spoil_n++], 24, "%s", id); return; }
    memmove(s_spoil[0], s_spoil[1], 24 * 31);
    snprintf(s_spoil[31], 24, "%s", id);
}
static msg_hit s_mhit[MAX_MESSAGES];
static int     s_nmhit;
typedef struct { int y0, y1; char cid[24], mid[24]; } reply_hit;
static reply_hit s_rephit[MAX_MESSAGES];
static int       s_nrephit;
typedef struct { int x0, y0, x1, y1; char code[40], guild_id[24]; int joined; } inv_hit;
static inv_hit s_invhit[16];
static int     s_ninvhit;
typedef struct { int x0, y0, x1, y1; char url[600]; } vid_hit;
static vid_hit s_vidhit[8];
static int     s_nvidhit;

static int  s_menu = 0;  // 0 closed, 1 action menu, 2 quick reactions, 3 emoji grid
static int  s_menu_sel = 0;
static int  s_menu_x, s_menu_y;
static char s_act_msg[24];  // message id the menu acts on
static bool s_act_own;
static char s_reply_to[24];  // pending reply target (empty = none)
static int  s_menu_rx, s_menu_ry, s_menu_rw, s_menu_rh, s_menu_rowh, s_menu_rows;

static bool s_compose_active;
static char s_compose_cid[24];
static char s_compose_reply[24];  // reply target captured at open (empty = none)

static bool     s_prof_open;
static int      s_srvinfo_open;
static char     s_srvinfo_gid[24];
static int      s_srv_confirm_leave;  // Leave button armed for confirm
static int      s_srvbtn_invite[4], s_srvbtn_read[4], s_srvbtn_mute[4], s_srvbtn_leave[4];
static char     s_srv_act_gid[24], s_srv_act_cid[24];
static void open_serverinfo(const char *gid)
{
    snprintf(s_srvinfo_gid, sizeof(s_srvinfo_gid), "%s", gid ? gid : "");
    s_srv_confirm_leave = 0;
    s_srvinfo_open = 1;
}
static char     s_prof_id[24], s_prof_name[80], s_prof_avatar[40];
static char     s_prof_guild[24];
static char     s_prof_bio[256];
static unsigned s_prof_accent;  // 0 = none (use blurple)
static char     s_prof_banner[64];  // banner hash (empty = accent)
static char     s_prof_roles[8][40];
static unsigned s_prof_role_colors[8];  // ABGR, 0 = none
static int      s_prof_nroles;
static char     s_prof_mutuals[8][96];
static int      s_prof_nmut;
static int      s_prof_block_btn[4];
static int      s_prof_note_r[4];
static int      s_prof_kick_btn[4], s_prof_ban_btn[4], s_prof_timeout_btn[4];
static int      s_prof_mod_arm;  // armed mod action: 0 none,1 kick,2 ban,3 timeout
static char     s_mod_gid[24], s_mod_uid[24];
static int      s_mod_action;
static char     s_prof_role_ids[24][24];
static int      s_prof_n_role_ids;
static int      s_prof_rolesedit_r[4];
static int      s_prof_msg_btn[4];
static char     s_msg_uid[24], s_msg_name[80], s_msg_av[40];
static volatile int s_open_dm_pending;  // worker asks the UI thread to open a DM
static char     s_open_dm_id[24], s_open_dm_rid[24], s_open_dm_name[80], s_open_dm_av[40];
static int      s_rolepick_open, s_rolepick_scroll;
static char     s_rp_gid[24], s_rp_uid[24], s_rp_rid[24]; int s_rp_add;
static struct { int y0, y1; char rid[24]; int has; } s_rp_hit[64];
static int      s_n_rp_hit;
static volatile bool s_prof_loading;

static bool     s_settings_open;
static bool     s_logout_req;  // polled by main.c to run the logout
static char     s_me_bio[256];
static unsigned s_me_accent;
static char     s_me_banner[64];
static volatile bool s_me_loading;
static int      s_set_cx, s_set_cy, s_set_cw, s_set_ch;
static int      s_set_lo_y, s_set_eb_y;  // Log Out / Edit Bio button y
static int      s_my_status = 1;  // 1 online 2 idle 3 dnd 0 invisible
static int      s_status_r[4][4];
static char     s_me_custom[128];
static int      s_set_cs_r[4];  // custom-status row hit rect
static int      s_set_av_r[4], s_set_name_r[4];
static int      s_set_sw_r[4];  // Switch Account button
#define SET_BTN_H 34
#define SET_ROW_H  30           // appearance setting row height
#define SET_NAPP    7           // appearance setting rows
static int      s_set_app_r[SET_NAPP][4];  // appearance row hit rects
static int      s_set_scroll;              // settings modal scroll (tall content)
static int      s_set_scroll_max;

#define MAX_FORUM 32
static bool     s_forum_view;
static char     s_forum_id[24];
static char     s_forum_gid[24];
static channel  s_forum[MAX_FORUM];
static int      s_forum_n;
static volatile bool s_forum_loading;
static int      s_forum_scroll;
static int      s_forum_hit_y0[MAX_FORUM], s_forum_hit_y1[MAX_FORUM];
static int      s_forum_new_x0, s_forum_new_x1, s_forum_new_y0, s_forum_new_y1;
static char     s_forum_status[40];

#define MAX_FTAGS 8
static bool     s_np_open;
static char     s_np_title[128], s_np_content[512];
static char     s_np_tag_ids[MAX_FTAGS][24], s_np_tag_names[MAX_FTAGS][40];
static int      s_np_ntags, s_np_tagsel;  // tagsel: -1 = none chosen
static bool     s_np_tag_required;
static int      s_np_title_r[4], s_np_content_r[4], s_np_post_r[4];
static int      s_np_tag_r[MAX_FTAGS][4];

#define MAX_SEARCH 20
static bool     s_search_open;
static char     s_search_q[128], s_search_gid[24];
static message  s_search_res[MAX_SEARCH];
static int      s_search_n;
static volatile bool s_search_loading;
static int      s_search_btn_x0, s_search_btn_x1;
static int      s_search_hit_y0[MAX_SEARCH], s_search_hit_y1[MAX_SEARCH];
static int      s_search_scroll, s_search_max;
#define MAX_SUSERS 5
typedef struct { char id[24], name[80], avatar[40]; } suser;
static suser    s_susers[MAX_SUSERS];
static int      s_suser_n;
static int      s_suser_hit_y0[MAX_SUSERS], s_suser_hit_y1[MAX_SUSERS];
static char     s_jump_to[24];  // if set, next open_channel loads around this msg
static char     s_pin_anchor[24];  // pin this msg near the top each frame (search jump)

#define MAX_FRIENDS 128
static int       s_friends_btn[4];
static bool      s_friends_open;
static int       s_confirm_open;
static char      s_confirm_msg[120], s_confirm_fid[24], s_confirm_yes[24];
static int       s_confirm_yes_r[4], s_confirm_no_r[4];
static discord_relationship s_friends[MAX_FRIENDS];
static int       s_friends_n;
static volatile bool s_friends_loading;
static int       s_friends_scroll, s_friends_max;
static int       s_friends_hit_y0[MAX_FRIENDS], s_friends_hit_y1[MAX_FRIENDS];
static int       s_fbtn_a[MAX_FRIENDS][4], s_fbtn_b[MAX_FRIENDS][4];
static int       s_friends_add_btn[4];

#define MAX_THR 20
static bool     s_thr_open;
static channel  s_thr[MAX_THR];
static int      s_thr_n;
static volatile bool s_thr_loading;
static char     s_thr_cid[24];
static int      s_thr_btn_x0, s_thr_btn_x1;
static int      s_thr_hit_y0[MAX_THR], s_thr_hit_y1[MAX_THR];

#define MAX_PINS 16
static bool     s_pins_open;
static message  s_pins[MAX_PINS];
static int      s_pins_count;
static volatile bool s_pins_loading;
static char     s_pins_cid[24];
static int      s_pin_btn_x0, s_pin_btn_x1;

typedef struct { bool is_cat; const channel *ch; } row_t;
static row_t s_rows[MAX_CHANNELS + 64];
static int   s_row_count;
static int   s_selectable[MAX_CHANNELS + 64];  // row indices that are channels
static int   s_sel_count;

// must match the row heights drawn in draw_sidebar
static int sidebar_rowh(int r, bool home)
{
    return s_rows[r].is_cat ? 24 : (home ? 30 : 26);
}
static int sidebar_row_y(int upto, bool home)
{
    int y = 0;
    for (int i = 0; i < upto && i < s_row_count; i++) y += sidebar_rowh(i, home);
    return y;
}
static int sidebar_total_h(bool home)
{
    int y = 0;
    for (int i = 0; i < s_row_count; i++) y += sidebar_rowh(i, home);
    return y;
}

static void draw_avatar(int cx, int cy, int r, const char *url,
                        const char *fallback_name, unsigned bg);
static void draw_guild_icon(int cx, int cy, int half, const char *url,
                            const char *name, unsigned bg);
static void draw_status_dot(int cx, int cy, int status, unsigned ring);

static bool is_text_channel(int type)
{
    return type == CH_TEXT || type == CH_ANNOUNCE || type == CH_FORUM;
}
static bool is_listed_channel(int type)
{
    return is_text_channel(type) || type == CH_VOICE;
}

// fills out[] with indices into g->channels, ordered by position
static void collect_sorted(const guild *g, const char *parent, int want_cat,
                           int *out, int *n)
{
    *n = 0;
    for (int i = 0; i < g->channel_count; i++) {
        const channel *c = &g->channels[i];
        bool cat = (c->type == CH_CATEGORY);
        if (c->hidden) continue;  // permission gating (VIEW_CHANNEL)
        if (want_cat && !cat) continue;
        if (!want_cat) {
            if (cat || !is_listed_channel(c->type)) continue;
            if (strcmp(c->parent_id, parent ? parent : "") != 0) continue;
        }
        out[(*n)++] = i;
    }
    for (int a = 1; a < *n; a++) {
        int v = out[a], b = a - 1;
        while (b >= 0 && g->channels[out[b]].position > g->channels[v].position) {
            out[b + 1] = out[b]; b--;
        }
        out[b + 1] = v;
    }
}

static void add_channel_with_threads(const guild *g, const channel *ch)
{
    if (s_row_count >= MAX_CHANNELS + 64) return;
    s_selectable[s_sel_count++] = s_row_count;
    s_rows[s_row_count++] = (row_t){ false, ch };
    // forum posts get their own post list, never sidebar rows
    if (ch->type == CH_FORUM) return;
    for (int i = 0; i < g->channel_count; i++) {
        const channel *c = &g->channels[i];
        if (ch_is_thread(c->type) && c->parent_id[0] &&
            strcmp(c->parent_id, ch->id) == 0 && s_row_count < MAX_CHANNELS + 64) {
            s_selectable[s_sel_count++] = s_row_count;
            s_rows[s_row_count++] = (row_t){ false, c };
        }
    }
}

// numeric compare of snowflake ids; an empty id sorts smallest
static int snowflake_cmp(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return la < lb ? -1 : 1;
    return strcmp(a, b);
}

static void build_rows(const guild *g, bool home)
{
    static int      c_guild = -0x7fff;
    static bool     c_home;
    static unsigned c_rev = 0xFFFFFFFFu, c_fold = 0xFFFFFFFFu;
    unsigned rev = store_msg_rev();
    if (s_guild == c_guild && home == c_home && rev == c_rev && s_cat_foldgen == c_fold)
        return;
    c_guild = s_guild; c_home = home; c_rev = rev; c_fold = s_cat_foldgen;

    s_row_count = 0; s_sel_count = 0;
    if (home) {
        int dn = 0;
        channel *dms = store_dms(&dn);
        if (dn > MAX_DMS) dn = MAX_DMS;
        // newest conversation on top
        int order[MAX_DMS];
        for (int i = 0; i < dn; i++) order[i] = i;
        for (int i = 1; i < dn; i++) {
            int v = order[i], j = i - 1;
            while (j >= 0 &&
                   snowflake_cmp(dms[order[j]].last_message_id, dms[v].last_message_id) < 0) {
                order[j + 1] = order[j]; j--;
            }
            order[j + 1] = v;
        }
        for (int i = 0; i < dn; i++) {
            s_selectable[s_sel_count++] = s_row_count;
            s_rows[s_row_count++] = (row_t){ false, &dms[order[i]] };
        }
        return;
    }
    if (!g) return;
    int idx[MAX_CHANNELS], n;

    // uncategorized channels first (parent "" = no category)
    collect_sorted(g, "", 0, idx, &n);
    for (int i = 0; i < n; i++)
        add_channel_with_threads(g, &g->channels[idx[i]]);
    int cats[MAX_CHANNELS], cn;
    collect_sorted(g, NULL, 1, cats, &cn);
    for (int c = 0; c < cn; c++) {
        const channel *cat = &g->channels[cats[c]];
        int ch[MAX_CHANNELS], chn;
        collect_sorted(g, cat->id, 0, ch, &chn);
        if (chn == 0) continue;
        s_rows[s_row_count++] = (row_t){ true, cat };
        if (cat_collapsed(cat->id)) continue;
        for (int i = 0; i < chn; i++)
            add_channel_with_threads(g, &g->channels[ch[i]]);
    }
}


static int fetch_worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    const char *around = s_fetch_around[0] ? s_fetch_around : NULL;
    s_fetch_http = discord_get_messages(s_fetch_cid, around, s_token, s_cacert);
    // ack the channel's newest message (captured at open); DMs fall back to
    // the newest loaded message id
    char ack[24] = "";
    store_lock();
    store_set_msg_loading(false);
    if (s_ack_newest[0]) snprintf(ack, sizeof(ack), "%s", s_ack_newest);
    else { int mc = 0; message *ms = store_messages(&mc);
           if (mc > 0) snprintf(ack, sizeof(ack), "%s", ms[mc-1].id); }
    store_unlock();
    if (ack[0]) discord_ack_message(s_fetch_cid, ack, s_token, s_cacert);
    return sceKernelExitDeleteThread(0);
}

static int older_worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    char before[24];
    store_lock(); snprintf(before, sizeof(before), "%s", store_oldest_message_id());
    store_unlock();
    int added = before[0]
        ? discord_get_messages_before(s_older_cid, before, s_token, s_cacert) : 0;
    if (added > 0) snprintf(s_restore_anchor, sizeof(s_restore_anchor), "%s", before);
    else           s_history_done = true;  // start of channel: stop asking
    console_dbgf("[ui] load older <%s -> +%d", before, added);
    s_loading_older = false;
    return sceKernelExitDeleteThread(0);
}

static void maybe_load_older(void)
{
    if (s_loading_older || s_history_done || s_msg_scroll > 2 || store_msg_loading())
        return;
    if (s_want_unread || s_pin_anchor[0]) return;
    // only when the list genuinely overflows
    if (s_prev_max < 80) return;
    store_lock();
    int mc = 0; store_messages(&mc);
    const char *oc = store_open_channel();
    bool can = (mc > 4 && mc < MAX_MESSAGES && oc[0]);
    char cid[24] = "";
    if (can) snprintf(cid, sizeof(cid), "%s", oc);
    store_unlock();
    if (!can) return;
    s_loading_older = true;
    snprintf(s_older_cid, sizeof(s_older_cid), "%s", cid);
    SceUID th = sceKernelCreateThread("cl_older", older_worker, 0x40,
                                      0x100000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    else s_loading_older = false;
}

static void open_forum(const channel *c);
static struct { char cid[24], mid[24]; } s_markq[120];
static int s_markn;
static int s_markread_btn[4];
static int markread_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    for (int i = 0; i < s_markn; i++)
        discord_ack_message(s_markq[i].cid, s_markq[i].mid, s_token, s_cacert);
    return sceKernelExitDeleteThread(0);
}
static void mark_guild_read(void)
{
    if (s_guild < 0) return;
    s_markn = 0;
    store_lock();
    int gc = 0; guild *gs = store_guilds(&gc);
    if (s_guild < gc) {
        guild *g = &gs[s_guild];
        for (int i = 0; i < g->channel_count && s_markn < 120; i++) {
            channel *c = &g->channels[i];
            if (c->unread && c->last_message_id[0]) {
                snprintf(s_markq[s_markn].cid, 24, "%s", c->id);
                snprintf(s_markq[s_markn].mid, 24, "%s", c->last_message_id);
                s_markn++;
                store_mark_read(c->id);  // optimistic local clear
            }
        }
    }
    store_unlock();
    store_touch();
    if (s_markn > 0) {
        SceUID th = sceKernelCreateThread("cl_markrd", markread_worker, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
        if (th >= 0) sceKernelStartThread(th, 0, NULL);
    }
}

// last-open view; empty guild id = DM/home
#define LASTVIEW_PATH "ux0:data/cordless/lastview.txt"
static void save_last_view(const char *gid, const char *cid)
{
    if (!cid || !cid[0]) return;
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%s,%s", gid ? gid : "", cid);
    SceUID fd = sceIoOpen(LASTVIEW_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    if (n > 0) sceIoWrite(fd, buf, (size_t)n);
    sceIoClose(fd);
}
static bool load_last_view(char *gid, size_t gcap, char *cid, size_t ccap)
{
    SceUID fd = sceIoOpen(LASTVIEW_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    char buf[64];
    int r = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (r <= 0) return false;
    buf[r] = '\0';
    for (int i = r - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' '); i--)
        buf[i] = '\0';
    char *comma = strchr(buf, ',');
    if (!comma) return false;
    *comma = '\0';
    snprintf(gid, gcap, "%s", buf);  // may be empty (DM/home)
    snprintf(cid, ccap, "%s", comma + 1);
    return cid[0] != '\0';
}

static void set_toast(const char *msg);

static void open_channel(const channel *c)
{
    if (!c) return;
    if (c->type == CH_VOICE) {
#if VOICE_JOIN_ENABLE
        if (voice_active() && strcmp(voice_channel_id(), c->id) == 0) voice_leave();
        else voice_join(s_cur_guild_id, c->id, s_token, s_cacert);
#else
        set_toast("Voice needs Discord's E2EE (DAVE) and is disabled in this build.");
#endif
        return;
    }
    s_open_ch = *c; s_have_open = true;
    if (c->type == CH_FORUM) { open_forum(c); return; }
    s_forum_view = false;
    store_lock();
    store_set_open_channel(c->id);
    store_clear_messages();
    store_set_msg_loading(true);
    // capture the last-read id before marking read
    snprintf(s_unread_boundary, sizeof(s_unread_boundary), "%s", c->last_read_id);
    bool unread = c->last_read_id[0] && snowflake_gt(c->last_message_id, c->last_read_id);
    snprintf(s_fetch_around, sizeof(s_fetch_around), "%s", unread ? c->last_read_id : "");
    snprintf(s_ack_newest, sizeof(s_ack_newest), "%s", c->last_message_id);
    store_mark_read(c->id);
    store_unlock();
    save_last_view(s_cur_guild_id, c->id);
    s_msg_scroll = 0;
    s_want_unread = (s_unread_boundary[0] != '\0');
    s_want_bottom = !s_want_unread;
    s_history_done = false; s_restore_anchor[0] = '\0'; s_pin_anchor[0] = '\0';

    if (s_jump_to[0]) {
        snprintf(s_fetch_around, sizeof(s_fetch_around), "%s", s_jump_to);
        snprintf(s_pin_anchor, sizeof(s_pin_anchor), "%s", s_jump_to);
        s_want_unread = false; s_want_bottom = false;
        s_jump_to[0] = '\0';
    }

    snprintf(s_fetch_cid, sizeof(s_fetch_cid), "%s", c->id);
    SceUID th = sceKernelCreateThread("gw_fetch", fetch_worker, 0x40,
                                      0x100000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);

    if (s_cur_guild_id[0])
        gateway_request_members(s_cur_guild_id, c->id);
    console_dbgf("[ui] open #%s", c->name);
}

static const channel *selected_channel(void)
{
    if (s_chan_sel < 0 || s_chan_sel >= s_sel_count) return NULL;
    return s_rows[s_selectable[s_chan_sel]].ch;
}

static void restore_last_view(void)
{
    char gid[24] = "", cid[24] = "";
    if (!load_last_view(gid, sizeof gid, cid, sizeof cid) || !cid[0]) return;

    channel target; bool found = false;
    store_lock();
    int gc = 0; guild *gs = store_guilds(&gc);
    const guild *g = NULL;
    if (gid[0]) {
        int gi = -1;
        for (int i = 0; i < gc; i++) if (strcmp(gs[i].id, gid) == 0) { gi = i; break; }
        if (gi < 0) { store_unlock(); return; }
        s_guild = gi; g = &gs[gi];
    } else {
        s_guild = -1;
    }
    build_rows(g, s_guild < 0);
    for (int i = 0; i < s_sel_count; i++) {
        const channel *ch = s_rows[s_selectable[i]].ch;
        if (ch && strcmp(ch->id, cid) == 0) { s_chan_sel = i; target = *ch; found = true; break; }
    }
    store_unlock();

    if (found) {
        s_side_scroll = 0;
        s_pending_open = false;  // skip the default #general open
        open_channel(&target);
    }
    console_dbgf("[ui] restore last view g=%s c=%s -> %s", gid[0] ? gid : "(dm)", cid,
                 found ? "opened" : "not found");
}

// Reopen the last channel on launch when the Open To pref is set.
void browser_apply_open_pref(void)
{
    if (g_settings.open_last == 1) restore_last_view();  // no-ops if nothing saved
}

// back out of a thread to its parent (forum -> post list, text -> channel);
// no-op if the current view isn't a thread
static void thread_go_back(void)
{
    if (!s_have_open || !ch_is_thread(s_open_ch.type) || !s_open_ch.parent_id[0]) return;
    channel pc; bool found = false;
    store_lock();
    channel *par = store_find_channel(s_open_ch.parent_id);
    if (par) { pc = *par; found = true; }
    store_unlock();
    if (found) open_channel(&pc);  // branches to open_forum() for forums
}


void browser_init(const char *token, const char *cacert_path)
{
    snprintf(s_token, sizeof(s_token), "%s", token);
    snprintf(s_cacert, sizeof(s_cacert), "%s", cacert_path);
    s_restored = false;  // re-run the last-view restore for this login
}

static void compose_with(const char *initial)
{
    // target the channel actually being viewed, not the sidebar selection
    const char *cid = s_have_open ? s_open_ch.id : NULL;
    if (!cid || !cid[0]) return;
    if (s_compose_active) return;  // already typing

    snprintf(s_compose_cid, sizeof(s_compose_cid), "%s", cid);
    snprintf(s_compose_reply, sizeof(s_compose_reply), "%s", s_reply_to);
    if (inlinekbd_open(initial ? initial : "")) s_compose_active = true;
}
static void compose_message(void) { compose_with(""); }

// pump the inline composer once per frame after swap, outside the GXM scene;
// called from main.c; sending is a blocking REST call
void browser_ime_pump(void)
{
    if (!s_compose_active) return;
    int r = inlinekbd_pump();
    if (r == 0) return;  // still typing
    if (r == 1) {  // Enter/Send
        const char *text = inlinekbd_text();
        if (text && text[0]) {
            int code;
            if (s_compose_reply[0])
                code = discord_send_reply(s_compose_cid, text, s_compose_reply,
                                          s_token, s_cacert);
            else
                code = discord_send_message(s_compose_cid, text, s_token, s_cacert);
            console_dbgf("[ui] send -> %d", code);
        }
        s_reply_to[0] = '\0';  // consumed the reply target
    }
    // r == 1 (sent) or r == -1 (cancelled): the keyboard is already closed
    s_compose_active = false;
    s_compose_reply[0] = '\0';
}

static volatile int s_uploading;  // an upload is in flight
static int          s_attach_open;  // attachment source sheet
static int          s_attach_r[2][4];  // [0]=library [1]=camera hit rects
// an upload carries either a file path (photo library) or an in-memory
// bytes/len buffer (camera JPEG)
struct upload { char cid[24]; char path[1024]; unsigned char *bytes; int len; };
static int upload_thread(SceSize sz, void *p)
{
    (void)sz;
    struct upload *u = *(struct upload **)p;
    int code;
    if (u->bytes) {
        code = discord_send_bytes(u->cid, "", u->bytes, u->len, "camera.jpg",
                                  s_token, s_cacert);
        free(u->bytes);
    } else {
        code = discord_send_file(u->cid, "", u->path, s_token, s_cacert);
    }
    console_dbgf("[ui] upload -> %d", code);
    free(u);
    s_uploading = 0;
    return sceKernelExitDeleteThread(0);
}
static void start_upload(struct upload *u)
{
    s_uploading = 1;
    SceUID th = sceKernelCreateThread("cl_upload", upload_thread, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, sizeof(u), &u);
    else { if (u->bytes) free(u->bytes); free(u); s_uploading = 0; }
}
// runs on the render thread: the system dialog drives its own loop, then the
// upload is handed to a worker
static void attach_photo(void)
{
    const char *cid = s_have_open ? s_open_ch.id : NULL;
    if (!cid || !cid[0] || s_uploading) return;
    char path[1024];
    if (!photo_pick(path, sizeof(path))) return;  // cancelled / unavailable
    struct upload *u = calloc(1, sizeof(*u));
    if (!u) return;
    snprintf(u->cid, sizeof(u->cid), "%s", cid);
    snprintf(u->path, sizeof(u->path), "%s", path);
    start_upload(u);
}
static void attach_camera(void)
{
    const char *cid = s_have_open ? s_open_ch.id : NULL;
    if (!cid || !cid[0] || s_uploading) return;
    unsigned char *buf = NULL; int len = 0;
    if (!camera_capture(&buf, &len)) return;  // cancelled / unavailable
    struct upload *u = calloc(1, sizeof(*u));
    if (!u) { free(buf); return; }
    snprintf(u->cid, sizeof(u->cid), "%s", cid);
    u->bytes = buf; u->len = len;
    start_upload(u);
}

struct act { int kind; char cid[24], mid[24], text[1200], emoji[64]; int arg; };
static int act_thread(SceSize sz, void *p)
{
    (void)sz;
    struct act *a = *(struct act **)p;
    switch (a->kind) {
    case 0: discord_react(a->cid, a->mid, a->emoji, a->arg != 0, s_token, s_cacert); break;
    case 1: discord_send_reply(a->cid, a->text, a->mid, s_token, s_cacert); break;
    case 2: discord_edit_message(a->cid, a->mid, a->text, s_token, s_cacert); break;
    case 3: if (discord_delete_message_api(a->cid, a->mid, s_token, s_cacert) < 300) {
                store_lock(); store_delete_message(a->mid); store_unlock(); }
            break;
    case 4: discord_poll_vote(a->cid, a->mid, a->arg, s_token, s_cacert); break;
    case 5: discord_create_thread(a->cid, a->mid, a->text, s_token, s_cacert); break;
    // forward: cid = destination channel, emoji = source channel, mid = message
    case 6: discord_forward_message(a->cid, a->emoji, a->mid, s_token, s_cacert); break;
    case 7: discord_crosspost(a->cid, a->mid, s_token, s_cacert); break;
    case 8: discord_pin_message(a->cid, a->mid, a->arg, s_token, s_cacert); break;
    }
    free(a);
    return sceKernelExitDeleteThread(0);
}
static void spawn_act(struct act *a)
{
    SceUID th = sceKernelCreateThread("cl_act", act_thread, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, sizeof(a), &a); else free(a);
}

// tap-to-vote on a poll: optimistically update the local counts, then PUT the
// vote off-thread
static void poll_vote(const char *mid, int answer_id)
{
    char cid[24] = "";
    int send = answer_id;
    store_lock();
    int mc = 0; message *ms = store_messages(&mc);
    for (int i = 0; i < mc; i++) {
        if (strcmp(ms[i].id, mid) != 0 || !ms[i].has_poll) continue;
        message *m = &ms[i];
        int cur = -1, tap = -1;
        for (int a = 0; a < m->poll_answer_count; a++) {
            if (m->poll_answers[a].me) cur = a;
            if (m->poll_answers[a].id == answer_id) tap = a;
        }
        if (tap < 0) break;
        if (cur == tap) {  // tapping your choice clears the vote
            m->poll_answers[tap].me = false; m->poll_answers[tap].count--; m->poll_total--;
            send = -1;
        } else {
            if (cur >= 0) { m->poll_answers[cur].me = false;
                            m->poll_answers[cur].count--; m->poll_total--; }
            m->poll_answers[tap].me = true; m->poll_answers[tap].count++; m->poll_total++;
        }
        break;
    }
    snprintf(cid, sizeof(cid), "%s", store_open_channel());
    store_unlock();
    if (!cid[0]) return;
    struct act *a = calloc(1, sizeof(*a));
    if (!a) return;
    a->kind = 4; a->arg = send;
    snprintf(a->cid, sizeof(a->cid), "%s", cid);
    snprintf(a->mid, sizeof(a->mid), "%s", mid);
    spawn_act(a);
}

// toggle your reaction on a message: adjust the local count, then PUT/DELETE
// off-thread; emoji is the name (unicode) or "name:id" (custom)
static void toggle_reaction(const char *mid, const char *emoji, bool had_me)
{
    char cid[24] = "";
    store_lock();
    int mc = 0; message *ms = store_messages(&mc);
    for (int i = 0; i < mc && !cid[0]; i++) {
        if (strcmp(ms[i].id, mid) != 0) continue;
        for (int r = 0; r < ms[i].reaction_count; r++) {
            reaction *rc = &ms[i].reactions[r];
            char key[80];
            if (rc->emoji_id[0]) snprintf(key, sizeof(key), "%s:%s", rc->name, rc->emoji_id);
            else                 snprintf(key, sizeof(key), "%s", rc->name);
            if (strcmp(key, emoji) == 0) {
                rc->me = !had_me;
                rc->count += had_me ? -1 : 1;
                if (rc->count <= 0) {  // last reactor left -> drop the pill
                    memmove(rc, rc + 1, sizeof(reaction) * (ms[i].reaction_count - r - 1));
                    ms[i].reaction_count--;
                }
                break;
            }
        }
        snprintf(cid, sizeof(cid), "%s", store_open_channel());
    }
    store_touch();  // reaction change may reflow the block
    store_unlock();
    if (!cid[0]) return;
    struct act *a = calloc(1, sizeof(*a));
    if (!a) return;
    a->kind = 0; a->arg = had_me ? 0 : 1;  // remove if we already reacted
    snprintf(a->cid, sizeof(a->cid), "%s", cid);
    snprintf(a->mid, sizeof(a->mid), "%s", mid);
    snprintf(a->emoji, sizeof(a->emoji), "%s", emoji);
    spawn_act(a);
}

static int prof_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    char id[24]; snprintf(id, sizeof(id), "%s", s_prof_id);
    char gid[24]; snprintf(gid, sizeof(gid), "%s", s_prof_guild);
    char bio[256] = ""; unsigned accent = 0;
    discord_profile_extra extra; extra.n_guilds = 0; extra.n_roles = 0;
    discord_get_user_profile(id, gid, s_token, s_cacert, bio, sizeof(bio), &accent, &extra);
    // resolve role and mutual-guild ids to names via the store
    char roles[8][40]; unsigned rolecols[8]; int nroles = 0;
    char mutuals[8][96]; int nmut = 0;
    store_lock();
    for (int i = 0; i < extra.n_roles && nroles < 8; i++) {
        const char *rn = store_role_name(extra.role_ids[i]);
        if (rn && rn[0] && strcmp(rn, "@everyone") != 0) {
            rolecols[nroles] = store_role_color(extra.role_ids[i]);
            snprintf(roles[nroles++], 40, "%s", rn);
        }
    }
    for (int i = 0; i < extra.n_guilds && nmut < 8; i++) {
        guild *gg = store_find_guild(extra.guild_ids[i]);
        if (gg) snprintf(mutuals[nmut++], 96, "%s", gg->name);
    }
    store_unlock();
    // only apply if the popout still shows the same user
    if (s_prof_open && strcmp(id, s_prof_id) == 0) {
        snprintf(s_prof_bio, sizeof(s_prof_bio), "%s", bio);
        s_prof_accent = accent;
        snprintf(s_prof_banner, sizeof(s_prof_banner), "%s", extra.banner);
        for (int i = 0; i < nroles; i++) { snprintf(s_prof_roles[i], 40, "%s", roles[i]);
                                           s_prof_role_colors[i] = rolecols[i]; }
        s_prof_nroles = nroles;
        s_prof_n_role_ids = 0;
        for (int i = 0; i < extra.n_roles && s_prof_n_role_ids < 24; i++)
            snprintf(s_prof_role_ids[s_prof_n_role_ids++], 24, "%s", extra.role_ids[i]);
        for (int i = 0; i < nmut; i++) snprintf(s_prof_mutuals[i], 96, "%s", mutuals[i]);
        s_prof_nmut = nmut;
    }
    s_prof_loading = false;
    return sceKernelExitDeleteThread(0);
}

// block / unblock the profile's user (optimistic store update + off-thread REST)
struct blk { char id[24]; int block; };
static int block_worker(SceSize sz, void *p)
{
    (void)sz;
    struct blk *b = *(struct blk **)p;
    int code = b->block ? discord_friend_block(b->id, s_token, s_cacert)
                        : discord_friend_remove(b->id, s_token, s_cacert);
    console_dbgf("[ui] block %s -> %d", b->block ? "add" : "remove", code);
    free(b);
    return sceKernelExitDeleteThread(0);
}
// edit the personal note on the profile's user (IME -> PUT /users/@me/notes)
static char s_note_uid[24], s_note_text[256];
static int note_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    console_dbgf("[ui] set note -> %d",
                 discord_set_note(s_note_uid, s_note_text, s_token, s_cacert));
    return sceKernelExitDeleteThread(0);
}
static void profile_edit_note(void)
{
    if (!s_prof_id[0]) return;
    const char *cur = store_get_note(s_prof_id);
    char t[256];
    if (!ime_get_text("Note", cur ? cur : "", t, sizeof(t))) return;
    store_lock(); store_set_note(s_prof_id, t); store_unlock();  // optimistic
    snprintf(s_note_uid, sizeof(s_note_uid), "%s", s_prof_id);
    snprintf(s_note_text, sizeof(s_note_text), "%s", t);
    SceUID th = sceKernelCreateThread("cl_note", note_worker, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}
static void profile_toggle_block(void)
{
    if (!s_prof_id[0]) return;
    store_lock();
    bool blocked = store_is_blocked(s_prof_id);
    store_set_blocked(s_prof_id, !blocked);  // optimistic: hide/show now
    store_unlock();
    store_touch();
    struct blk *b = calloc(1, sizeof(*b));
    if (!b) return;
    snprintf(b->id, sizeof(b->id), "%s", s_prof_id);
    b->block = !blocked;
    SceUID th = sceKernelCreateThread("cl_block", block_worker, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, sizeof(b), &b); else free(b);
}

// add-a-server modal (Create My Own / Join a Server)
static int  s_addserver_open;
static int  s_as_create_r[4], s_as_join_r[4];  // modal button hit rects
static char s_join_code[64], s_new_guild_name[96];

// transient bottom-of-pane toast (e.g. join errors); set from any thread
static char    s_toast[160];
static int64_t s_toast_until;
static void set_toast(const char *msg)
{
    snprintf(s_toast, sizeof(s_toast), "%s", msg ? msg : "");
    s_toast_until = now_ms() + 5000;
}

// true while something on screen still animates (composer, menu, toast, typing
// dots); GIFs and new images are reported separately via imagecache_*
bool browser_wants_redraw(void)
{
    if (s_compose_active) return true;
    if (s_menu)           return true;
    if (s_toast[0] && now_ms() < s_toast_until) return true;
    bool typing = false;
    store_lock();
    if (store_open_channel()[0] &&
        strcmp(store_typing_channel(), store_open_channel()) == 0 &&
        now_ms() - store_typing_at() < 9000) typing = true;
    store_unlock();
    return typing;
}

static int join_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    int code = discord_join_invite(s_join_code, s_token, s_cacert);
    console_dbgf("[ui] join invite '%s' -> %d", s_join_code, code);
    if (code == DISCORD_JOIN_CAPTCHA)
        set_toast("Discord needs a captcha to join this server. Accept the invite from the official Discord app.");
    else if (code >= 200 && code < 300)
        set_toast("Joined!");
    else
        set_toast("Couldn't join this server.");
    return sceKernelExitDeleteThread(0);
}

static int srv_leave_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    int code = discord_leave_guild(s_srv_act_gid, s_token, s_cacert);
    console_dbgf("[ui] leave guild %s -> %d", s_srv_act_gid, code);
    if (code >= 200 && code < 300) {
        store_lock(); store_remove_guild(s_srv_act_gid); store_unlock();
        set_toast("Left the server.");
    } else set_toast("Couldn't leave the server.");
    return sceKernelExitDeleteThread(0);
}
static int s_srv_mute_want;
static int srv_mute_worker(SceSize sz, void *p)
{
    (void)sz; (void)p; int muted = s_srv_mute_want;
    int code = discord_set_guild_muted(s_srv_act_gid, muted, s_token, s_cacert);
    set_toast(code >= 200 && code < 300 ? (muted ? "Server muted." : "Server unmuted.")
                                        : "Couldn't change mute.");
    return sceKernelExitDeleteThread(0);
}
static int srv_invite_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    char code[40] = "";
    int rc = discord_create_invite(s_srv_act_cid, code, sizeof(code), s_token, s_cacert);
    if (rc == 200 && code[0]) { char msg[80]; snprintf(msg, sizeof(msg), "Invite created: discord.gg/%s", code);
        set_toast(msg); }
    else set_toast("Couldn't create an invite (need Create Invite permission).");
    return sceKernelExitDeleteThread(0);
}
static void srv_spawn(int (*fn)(SceSize, void *))
{
    SceUID th = sceKernelCreateThread("cl_srv", fn, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

// ISO8601 UTC timestamp `seconds` in the future (for member timeouts)
static void iso_utc_in(int seconds, char *out, size_t cap)
{
    SceDateTime dt; sceRtcGetCurrentClockUtc(&dt);
    SceRtcTick tick; sceRtcGetTick(&dt, &tick);
    tick.tick += (SceULong64)seconds * 1000000ULL;
    sceRtcSetTick(&dt, &tick);
    snprintf(out, cap, "%04u-%02u-%02uT%02u:%02u:%02u.000Z",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}
// moderation worker (kick/ban/timeout); target and action set before spawning
static int mod_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    int code = -1;
    if (s_mod_action == 1) code = discord_kick_member(s_mod_gid, s_mod_uid, s_token, s_cacert);
    else if (s_mod_action == 2) code = discord_ban_member(s_mod_gid, s_mod_uid, s_token, s_cacert);
    else if (s_mod_action == 3) { char iso[40]; iso_utc_in(300, iso, sizeof(iso));
        code = discord_timeout_member(s_mod_gid, s_mod_uid, iso, s_token, s_cacert); }
    const char *ok = s_mod_action == 1 ? "Member kicked." :
                     s_mod_action == 2 ? "Member banned." : "Member timed out (5 min).";
    set_toast(code >= 200 && code < 300 ? ok : "Action failed (check your permissions).");
    return sceKernelExitDeleteThread(0);
}

static int prof_has_role(const char *rid)
{
    for (int i = 0; i < s_prof_n_role_ids; i++)
        if (strcmp(s_prof_role_ids[i], rid) == 0) return 1;
    return 0;
}
static int rolepick_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    int code = discord_member_role(s_rp_gid, s_rp_uid, s_rp_rid, s_rp_add, s_token, s_cacert);
    set_toast(code >= 200 && code < 300 ? (s_rp_add ? "Role added." : "Role removed.")
                                        : "Couldn't change role (check permissions).");
    return sceKernelExitDeleteThread(0);
}
// open (or create) a DM with s_msg_uid, then ask the UI thread to navigate to it
static int msg_open_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    char cid[24] = "";
    store_lock();
    int dn = 0; channel *dms = store_dms(&dn);
    for (int i = 0; i < dn; i++)
        if (strcmp(dms[i].recipient_id, s_msg_uid) == 0) { snprintf(cid, 24, "%s", dms[i].id); break; }
    store_unlock();
    if (!cid[0]) {
        discord_open_dm(s_msg_uid, cid, sizeof(cid), s_token, s_cacert);
        if (cid[0]) { store_lock(); store_add_dm(cid, s_msg_name, CH_DM, s_msg_av, s_msg_uid, ""); store_unlock(); }
    }
    if (cid[0]) {
        snprintf(s_open_dm_id, 24, "%s", cid);
        snprintf(s_open_dm_rid, 24, "%s", s_msg_uid);
        snprintf(s_open_dm_name, sizeof(s_open_dm_name), "%s", s_msg_name);
        snprintf(s_open_dm_av, 40, "%s", s_msg_av);
        s_open_dm_pending = 1;  // UI thread opens it next frame
    } else set_toast("Couldn't open a DM.");
    return sceKernelExitDeleteThread(0);
}

// Discord permission bits for gating actions
#define PERM_CREATE_INVITE   (1ull << 0)
#define PERM_MANAGE_MESSAGES (1ull << 13)
#define PERM_KICK          (1ull << 1)
#define PERM_BAN           (1ull << 2)
#define PERM_MANAGE_ROLES  (1ull << 28)
#define PERM_MODERATE      (1ull << 40)

// first visible text channel of a guild (for creating an invite)
static const char *first_text_channel(const guild *g)
{
    if (!g) return NULL;
    for (int i = 0; i < g->channel_count; i++)
        if (is_text_channel(g->channels[i].type) && !g->channels[i].hidden)
            return g->channels[i].id;
    return NULL;
}
static int create_guild_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    int code = discord_create_guild(s_new_guild_name, s_token, s_cacert);
    console_dbgf("[ui] create guild '%s' -> %d", s_new_guild_name, code);
    return sceKernelExitDeleteThread(0);
}
// "Join a Server": parse the trailing invite code and POST it
static void add_server_join(void)
{
    char in[128];
    if (!ime_get_text("Invite link or code", "", in, sizeof(in)) || !in[0]) return;
    const char *code = in, *slash = in;
    for (const char *q = in; *q; q++) if (*q == '/') slash = q + 1;
    if (slash != in) code = slash;
    char clean[64]; int k = 0;
    for (const char *q = code; *q && k < 63 && *q != '?' && *q != '#' && *q != ' '; q++)
        clean[k++] = *q;
    clean[k] = '\0';
    if (!clean[0]) return;
    snprintf(s_join_code, sizeof(s_join_code), "%s", clean);
    SceUID th = sceKernelCreateThread("cl_join", join_worker, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}
// "Create My Own": name a new server, POST /guilds
static void add_server_create(void)
{
    char name[96];
    if (!ime_get_text("Server name", "", name, sizeof(name)) || !name[0]) return;
    snprintf(s_new_guild_name, sizeof(s_new_guild_name), "%s", name);
    SceUID th = sceKernelCreateThread("cl_mkguild", create_guild_worker, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

#define MAX_FWD 80
static int  s_forward_open;
static char s_fwd_src_cid[24], s_fwd_src_mid[24];
static int  s_fwd_scroll, s_fwd_max, s_fwd_n;
static char s_fwd_id[MAX_FWD][24], s_fwd_name[MAX_FWD][80];
static int  s_fwd_is_dm[MAX_FWD];
static int  s_fwd_hit_y0[MAX_FWD], s_fwd_hit_y1[MAX_FWD];
static void open_forward(const char *src_cid, const char *src_mid)
{
    snprintf(s_fwd_src_cid, sizeof(s_fwd_src_cid), "%s", src_cid);
    snprintf(s_fwd_src_mid, sizeof(s_fwd_src_mid), "%s", src_mid);
    s_fwd_n = 0; s_fwd_scroll = 0;
    store_lock();
    // DMs first, then the current guild's visible text channels
    int dn = 0; channel *dms = store_dms(&dn);
    for (int i = 0; i < dn && s_fwd_n < MAX_FWD; i++) {
        snprintf(s_fwd_id[s_fwd_n], 24, "%s", dms[i].id);
        snprintf(s_fwd_name[s_fwd_n], 80, "%s", dms[i].name);
        s_fwd_is_dm[s_fwd_n] = 1; s_fwd_n++;
    }
    guild *g = store_find_guild(s_cur_guild_id);
    if (g) for (int i = 0; i < g->channel_count && s_fwd_n < MAX_FWD; i++) {
        channel *c = &g->channels[i];
        if (c->hidden || c->type == CH_CATEGORY || c->type == CH_VOICE) continue;
        if (!is_listed_channel(c->type)) continue;
        snprintf(s_fwd_id[s_fwd_n], 24, "%s", c->id);
        snprintf(s_fwd_name[s_fwd_n], 80, "%s", c->name);
        s_fwd_is_dm[s_fwd_n] = 0; s_fwd_n++;
    }
    store_unlock();
    s_forward_open = 1;
}
static void forward_to(int idx)
{
    if (idx < 0 || idx >= s_fwd_n) return;
    struct act *a = calloc(1, sizeof(*a));
    if (a) {
        a->kind = 6;
        snprintf(a->cid, 24, "%s", s_fwd_id[idx]);  // destination
        snprintf(a->emoji, sizeof(a->emoji), "%s", s_fwd_src_cid);  // source ch
        snprintf(a->mid, 24, "%s", s_fwd_src_mid);
        spawn_act(a);
    }
    s_forward_open = 0;
}

// a discord.gg link renders as a tile (icon, name, splash, counts, Join/Go);
// invite metadata is fetched async and cached by code
#define MAX_INV_CACHE 16
struct inv_entry { char code[40]; discord_invite info; volatile int state; };  // 0 empty 1 fetching 2 done 3 fail
static struct inv_entry s_invites[MAX_INV_CACHE];
struct inv_req { char code[40]; };
static int inv_worker(SceSize sz, void *p)
{
    (void)sz;
    struct inv_req *r = *(struct inv_req **)p;
    discord_invite info;
    int http = discord_get_invite(r->code, &info, s_token, s_cacert);
    for (int i = 0; i < MAX_INV_CACHE; i++)
        if (s_invites[i].state == 1 && strcmp(s_invites[i].code, r->code) == 0) {
            if (http == 200) { s_invites[i].info = info; s_invites[i].state = 2; }
            else s_invites[i].state = 3;
            break;
        }
    free(r);
    return sceKernelExitDeleteThread(0);
}
static struct inv_entry *inv_get(const char *code)
{
    for (int i = 0; i < MAX_INV_CACHE; i++)
        if (s_invites[i].state && strcmp(s_invites[i].code, code) == 0) return &s_invites[i];
    for (int i = 0; i < MAX_INV_CACHE; i++)
        if (s_invites[i].state == 0) {
            snprintf(s_invites[i].code, sizeof(s_invites[i].code), "%s", code);
            s_invites[i].state = 1;
            struct inv_req *r = calloc(1, sizeof(*r));
            if (r) {
                snprintf(r->code, sizeof(r->code), "%s", code);
                SceUID th = sceKernelCreateThread("cl_inv", inv_worker, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
                if (th >= 0) sceKernelStartThread(th, sizeof(r), &r);
                else { free(r); s_invites[i].state = 0; }
            }
            return &s_invites[i];
        }
    return NULL;
}

// the "@" button opens this picker; choosing a member opens the IME prefilled
// with a <@id> mention
#define MAX_MENTION 200
static int  s_mention_open, s_mention_scroll, s_mention_max, s_mention_n;
static char s_mention_id[MAX_MENTION][24], s_mention_name[MAX_MENTION][64];
static char s_mention_av[MAX_MENTION][40];
static int  s_mention_hit_y0[MAX_MENTION], s_mention_hit_y1[MAX_MENTION];
static void open_mention_picker(void)
{
    s_mention_n = 0; s_mention_scroll = 0;
    store_lock();
    int mc = 0; member_row *ms = store_members(&mc);
    for (int i = 0; i < mc && s_mention_n < MAX_MENTION; i++) {
        if (ms[i].is_group || !ms[i].id[0]) continue;
        snprintf(s_mention_id[s_mention_n], 24, "%s", ms[i].id);
        snprintf(s_mention_name[s_mention_n], 64, "%s", ms[i].text);
        snprintf(s_mention_av[s_mention_n], 40, "%s", ms[i].avatar);
        s_mention_n++;
    }
    store_unlock();
    s_mention_open = 1;
}
static void mention_pick(int idx)
{
    if (idx < 0 || idx >= s_mention_n) return;
    s_mention_open = 0;
    char init[40];
    snprintf(init, sizeof(init), "<@%s> ", s_mention_id[idx]);
    compose_with(init);
}

// open the profile popout for a message author (name/avatar shown immediately,
// bio/accent fetched in the background)
static void open_profile(const char *id, const char *name, const char *avatar)
{
    if (!id || !id[0]) return;
    snprintf(s_prof_id, sizeof(s_prof_id), "%s", id);
    snprintf(s_prof_name, sizeof(s_prof_name), "%s", name ? name : "");
    snprintf(s_prof_avatar, sizeof(s_prof_avatar), "%s", avatar ? avatar : "");
    snprintf(s_prof_guild, sizeof(s_prof_guild), "%s", s_cur_guild_id);
    s_prof_bio[0] = '\0'; s_prof_accent = 0;
    s_prof_nroles = 0; s_prof_nmut = 0; s_prof_mod_arm = 0;
    s_prof_open = true; s_prof_loading = true;
    SceUID th = sceKernelCreateThread("cl_prof", prof_worker, 0x40, 0x100000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL); else s_prof_loading = false;
}

static int me_prof_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    char bio[256] = ""; unsigned accent = 0;
    discord_profile_extra ex; ex.n_guilds = 0; ex.n_roles = 0; ex.banner[0] = '\0';
    discord_get_user_profile(store_me_id(), "", s_token, s_cacert,
                             bio, sizeof(bio), &accent, &ex);
    snprintf(s_me_bio, sizeof(s_me_bio), "%s", bio);
    s_me_accent = accent;
    snprintf(s_me_banner, sizeof(s_me_banner), "%s", ex.banner);
    s_me_loading = false;
    return sceKernelExitDeleteThread(0);
}
static void open_settings(void)
{
    s_settings_open = true;
    s_set_scroll = 0;
    s_me_bio[0] = '\0'; s_me_accent = 0; s_me_loading = true;
    SceUID th = sceKernelCreateThread("cl_me", me_prof_worker, 0x40, 0x100000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL); else s_me_loading = false;
}
// account edits: change display name / avatar (PATCH /users/@me)
static char s_new_name[96], s_new_avatar_path[1024];
static int set_name_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    console_dbgf("[ui] set name -> %d", discord_set_global_name(s_new_name, s_token, s_cacert));
    return sceKernelExitDeleteThread(0);
}
static int set_avatar_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    console_dbgf("[ui] set avatar -> %d", discord_set_avatar(s_new_avatar_path, s_token, s_cacert));
    return sceKernelExitDeleteThread(0);
}
static void settings_edit_name(void)
{
    char n[96];
    if (!ime_get_text("Display name", store_me_name(), n, sizeof(n)) || !n[0]) return;
    snprintf(s_new_name, sizeof(s_new_name), "%s", n);
    SceUID th = sceKernelCreateThread("cl_setname", set_name_worker, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}
static void settings_change_avatar(void)
{
    char path[1024];
    if (!photo_pick(path, sizeof(path))) return;  // hardware-only dialog
    snprintf(s_new_avatar_path, sizeof(s_new_avatar_path), "%s", path);
    SceUID th = sceKernelCreateThread("cl_setav", set_avatar_worker, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}
// main.c polls this each frame; returns true once, then clears the request
bool browser_wants_logout(void)
{
    if (!s_logout_req) return false;
    s_logout_req = false;
    return true;
}

static int       s_accounts_open;
static account_t s_accounts[ACCOUNTS_MAX];
static int       s_accounts_n;
static int       s_acc_hit_y0[ACCOUNTS_MAX], s_acc_hit_y1[ACCOUNTS_MAX];
static int       s_acc_add_r[4];
static char      s_switch_tok[128];  // set -> main switches to this token
static int       s_add_account_req;  // set -> main starts a fresh QR login
bool browser_pending_switch(char *out, size_t out_cap)
{
    if (!s_switch_tok[0]) return false;
    snprintf(out, out_cap, "%s", s_switch_tok);
    s_switch_tok[0] = '\0';
    return true;
}
bool browser_wants_add_account(void)
{
    if (!s_add_account_req) return false;
    s_add_account_req = false;
    return true;
}
static void open_accounts(void)
{
    s_accounts_n = accounts_load(s_accounts, ACCOUNTS_MAX);
    s_accounts_open = 1;
}
static void accounts_pick(int idx)
{
    if (idx < 0 || idx >= s_accounts_n) return;
    // ignore a tap on the currently-active account
    if (store_me_id()[0] && strcmp(s_accounts[idx].id, store_me_id()) == 0) {
        s_accounts_open = 0; s_settings_open = 0; return;
    }
    snprintf(s_switch_tok, sizeof(s_switch_tok), "%s", s_accounts[idx].token);
    s_accounts_open = 0; s_settings_open = 0;
}
static int forum_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    channel tmp[MAX_FORUM];
    int n = discord_get_active_threads(s_forum_gid, s_forum_id, tmp, MAX_FORUM,
                                       s_token, s_cacert);
    if (n < 0) n = 0;
    if (s_forum_view) { for (int i = 0; i < n; i++) s_forum[i] = tmp[i];
                        s_forum_n = n; }
    s_forum_loading = false;
    return sceKernelExitDeleteThread(0);
}
static void open_forum(const channel *c)
{
    store_lock(); store_set_open_channel(c->id); store_unlock();
    s_forum_view = true; s_forum_n = 0; s_forum_scroll = 0; s_forum_loading = true;
    s_forum_status[0] = '\0';
    snprintf(s_forum_id, sizeof(s_forum_id), "%s", c->id);
    snprintf(s_forum_gid, sizeof(s_forum_gid), "%s", s_cur_guild_id);
    if (s_forum_loading) {
        SceUID th = sceKernelCreateThread("cl_forum", forum_worker, 0x40, 0x100000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
        if (th >= 0) sceKernelStartThread(th, 0, NULL); else s_forum_loading = false;
    }
}

// open the New Post form (fetches the forum's tags first)
static void new_forum_post(void)
{
    if (!s_forum_id[0]) return;
    s_np_title[0] = '\0'; s_np_content[0] = '\0'; s_np_tagsel = -1;
    s_forum_status[0] = '\0';
    int nt = discord_get_forum_tags(s_forum_id, s_np_tag_ids, s_np_tag_names,
                                    MAX_FTAGS, s_token, s_cacert);
    s_np_ntags = nt > 0 ? nt : 0;
    s_np_tag_required = (s_np_ntags > 0);  // forums with tags usually require one
    s_np_open = true;
}

// submit the New Post form: validate, POST, refresh on success
static void np_submit(void)
{
    if (!s_np_title[0]) { snprintf(s_forum_status, sizeof(s_forum_status), "Enter a title"); return; }
    if (s_np_tag_required && s_np_tagsel < 0) {
        snprintf(s_forum_status, sizeof(s_forum_status), "Pick a tag"); return; }
    const char *tag = (s_np_tagsel >= 0) ? s_np_tag_ids[s_np_tagsel] : NULL;
    int code = discord_create_forum_post(s_forum_id, s_np_title, s_np_content, tag,
                                         s_token, s_cacert);
    console_dbgf("[ui] new forum post -> %d", code);
    if (code >= 200 && code < 300) {
        s_np_open = false;
        snprintf(s_forum_status, sizeof(s_forum_status), "Post created");
        s_forum_n = 0; s_forum_loading = true;  // refresh the post list
        SceUID th = sceKernelCreateThread("cl_forum", forum_worker, 0x40, 0x100000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
        if (th >= 0) sceKernelStartThread(th, 0, NULL); else s_forum_loading = false;
    } else {
        snprintf(s_forum_status, sizeof(s_forum_status), "Failed (%d)", code);
    }
}

// presence rank for online-first ordering (1 online, 2 idle, 3 dnd, else offline)
static int friend_prank(int st) { return st == 1 ? 0 : st == 2 ? 1 : st == 3 ? 2 : 3; }
// group: incoming requests first, then friends, then outgoing
static int friend_group(int type) { return type == 3 ? 0 : type == 1 ? 1 : 2; }
static int friends_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    static discord_relationship tmp[MAX_FRIENDS];
    int n = discord_get_relationships(tmp, MAX_FRIENDS, s_token, s_cacert);
    if (n < 0) n = 0;
    // sort: group (incoming/friends/outgoing), then online-first, then name
    store_lock();
    for (int i = 1; i < n; i++) {
        discord_relationship key = tmp[i];
        int kg = friend_group(key.type), kr = friend_prank(store_member_status(key.id));
        int j = i - 1;
        while (j >= 0) {
            int jg = friend_group(tmp[j].type), jr = friend_prank(store_member_status(tmp[j].id));
            if (jg < kg) break;
            if (jg == kg && (jr < kr || (jr == kr && strcasecmp(tmp[j].name, key.name) <= 0))) break;
            tmp[j + 1] = tmp[j]; j--;
        }
        tmp[j + 1] = key;
    }
    store_unlock();
    for (int i = 0; i < n; i++) s_friends[i] = tmp[i];
    s_friends_n = n;
    s_friends_loading = false;
    return sceKernelExitDeleteThread(0);
}
static void open_friends(void)
{
    s_friends_open = true; s_friends_n = 0; s_friends_loading = true; s_friends_scroll = 0;
    SceUID th = sceKernelCreateThread("cl_friends", friends_worker, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL); else s_friends_loading = false;
}
// friend-request actions run off-thread, then refetch the list
struct friend_act { int kind; char id[24]; char username[80]; };  // 0 accept 1 remove 2 add
static int friend_act_worker(SceSize sz, void *p)
{
    (void)sz;
    struct friend_act *a = *(struct friend_act **)p;
    int code = -1;
    if (a->kind == 0)      code = discord_friend_accept(a->id, s_token, s_cacert);
    else if (a->kind == 1) code = discord_friend_remove(a->id, s_token, s_cacert);
    else if (a->kind == 2) code = discord_friend_add(a->username, s_token, s_cacert);
    console_dbgf("[ui] friend act %d -> %d", a->kind, code);
    free(a);
    // refresh the list
    static discord_relationship tmp[MAX_FRIENDS];
    int n = discord_get_relationships(tmp, MAX_FRIENDS, s_token, s_cacert);
    if (n < 0) n = 0;
    store_lock();
    for (int i = 1; i < n; i++) {
        discord_relationship key = tmp[i];
        int kg = friend_group(key.type), kr = friend_prank(store_member_status(key.id));
        int j = i - 1;
        while (j >= 0) {
            int jg = friend_group(tmp[j].type), jr = friend_prank(store_member_status(tmp[j].id));
            if (jg < kg) break;
            if (jg == kg && (jr < kr || (jr == kr && strcasecmp(tmp[j].name, key.name) <= 0))) break;
            tmp[j + 1] = tmp[j]; j--;
        }
        tmp[j + 1] = key;
    }
    store_unlock();
    for (int i = 0; i < n; i++) s_friends[i] = tmp[i];
    s_friends_n = n;
    s_friends_loading = false;
    return sceKernelExitDeleteThread(0);
}
static void friend_act(int kind, const char *id, const char *username)
{
    struct friend_act *a = calloc(1, sizeof(*a));
    if (!a) return;
    a->kind = kind;
    if (id) snprintf(a->id, sizeof(a->id), "%s", id);
    if (username) snprintf(a->username, sizeof(a->username), "%s", username);
    s_friends_loading = true;
    SceUID th = sceKernelCreateThread("cl_friendact", friend_act_worker, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, sizeof(a), &a); else { free(a); s_friends_loading = false; }
}
static void add_friend_prompt(void)
{
    char uname[80];
    if (!ime_get_text("Add friend by username", "", uname, sizeof(uname)) || !uname[0]) return;
    friend_act(2, NULL, uname);
}
// ask before removing a friend / cancelling / ignoring a request
static void confirm_friend_remove(int idx)
{
    if (idx < 0 || idx >= s_friends_n) return;
    int t = s_friends[idx].type; const char *nm = s_friends[idx].name;
    if (t == 4)      { snprintf(s_confirm_msg, sizeof(s_confirm_msg), "Cancel your friend request to %s?", nm);
                       snprintf(s_confirm_yes, sizeof(s_confirm_yes), "Cancel Request"); }
    else if (t == 3) { snprintf(s_confirm_msg, sizeof(s_confirm_msg), "Ignore the friend request from %s?", nm);
                       snprintf(s_confirm_yes, sizeof(s_confirm_yes), "Ignore"); }
    else             { snprintf(s_confirm_msg, sizeof(s_confirm_msg), "Remove %s from your friends?", nm);
                       snprintf(s_confirm_yes, sizeof(s_confirm_yes), "Remove"); }
    snprintf(s_confirm_fid, sizeof(s_confirm_fid), "%s", s_friends[idx].id);
    s_confirm_open = 1;
}
// tapping a friend opens the existing DM if any, else their profile
static void open_friend(const discord_relationship *f)
{
    channel dm; bool have = false;
    store_lock();
    int dn = 0; channel *dms = store_dms(&dn);
    for (int i = 0; i < dn; i++)
        if (strcmp(dms[i].recipient_id, f->id) == 0) { dm = dms[i]; have = true; break; }
    store_unlock();
    s_friends_open = false;
    if (have) { s_guild = -1; open_channel(&dm); }
    else open_profile(f->id, f->name, f->avatar);
}

static int search_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    static message tmp[MAX_SEARCH];
    int n = discord_search_messages(1, s_search_gid, s_search_q, tmp, MAX_SEARCH,
                                    s_token, s_cacert);
    if (n < 0) n = 0;
    if (s_search_open) { for (int i = 0; i < n; i++) s_search_res[i] = tmp[i];
                         s_search_n = n; }
    s_search_loading = false;
    return sceKernelExitDeleteThread(0);
}
// case-insensitive substring match (ASCII)
static bool istrstr(const char *hay, const char *needle)
{
    if (!needle[0]) return false;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n) {
            char a = *h, b = *n;
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            h++; n++;
        }
        if (!*n) return true;
    }
    return false;
}
// matching users for the query: loaded guild members + DM recipients,
// deduped by user id
static bool suser_have(const char *id)
{
    if (!id || !id[0]) return false;
    for (int i = 0; i < s_suser_n; i++)
        if (strcmp(s_susers[i].id, id) == 0) return true;
    return false;
}
static void collect_search_users(const char *q)
{
    s_suser_n = 0;
    store_lock();
    int mc = 0; member_row *mr = store_members(&mc);
    for (int i = 0; i < mc && s_suser_n < MAX_SUSERS; i++) {
        if (mr[i].is_group || suser_have(mr[i].id)) continue;
        if (istrstr(mr[i].text, q)) {
            suser *u = &s_susers[s_suser_n++];
            snprintf(u->id, 24, "%s", mr[i].id);
            snprintf(u->name, 80, "%s", mr[i].text);
            snprintf(u->avatar, 40, "%s", mr[i].avatar);
        }
    }
    int dn = 0; channel *dms = store_dms(&dn);
    for (int i = 0; i < dn && s_suser_n < MAX_SUSERS; i++) {
        if (suser_have(dms[i].recipient_id)) continue;
        if (istrstr(dms[i].name, q)) {
            suser *u = &s_susers[s_suser_n++];
            snprintf(u->id, 24, "%s", dms[i].recipient_id);
            snprintf(u->name, 80, "%s", dms[i].name);
            snprintf(u->avatar, 40, "%s", dms[i].avatar);
        }
    }
    store_unlock();
}
static void open_search(void)
{
    if (!s_cur_guild_id[0]) return;  // guild-scoped search
    char q[128];
    if (!ime_get_text("Search messages", "", q, sizeof(q)) || !q[0]) return;
    snprintf(s_search_q, sizeof(s_search_q), "%s", q);
    snprintf(s_search_gid, sizeof(s_search_gid), "%s", s_cur_guild_id);
    collect_search_users(q);
    s_search_open = true; s_search_n = 0; s_search_loading = true; s_search_scroll = 0;
    SceUID th = sceKernelCreateThread("cl_search", search_worker, 0x40, 0x100000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL); else s_search_loading = false;
}
static void jump_to_message(const char *cid, const char *mid)
{
    channel pc; bool found = false;
    store_lock();
    channel *c = store_find_channel(cid);
    if (c) { pc = *c; found = true; }
    store_unlock();
    if (!found) return;
    snprintf(s_jump_to, sizeof(s_jump_to), "%s", mid);
    open_channel(&pc);
}

// fetch the open channel's threads (via /threads/search) for the Threads modal
static int thr_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    channel tmp[MAX_THR];
    int n = discord_get_active_threads("", s_thr_cid, tmp, MAX_THR, s_token, s_cacert);
    if (n < 0) n = 0;
    if (s_thr_open) { for (int i = 0; i < n; i++) s_thr[i] = tmp[i]; s_thr_n = n; }
    s_thr_loading = false;
    return sceKernelExitDeleteThread(0);
}
static void open_threads(void)
{
    if (!s_have_open || !s_open_ch.id[0]) return;
    if (s_thr_loading) { s_thr_open = true; return; }
    snprintf(s_thr_cid, sizeof(s_thr_cid), "%s", s_open_ch.id);
    s_thr_open = true; s_thr_n = 0; s_thr_loading = true;
    SceUID th = sceKernelCreateThread("cl_thr", thr_worker, 0x40, 0x100000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL); else s_thr_loading = false;
}

static int pins_worker(SceSize sz, void *p)
{
    (void)sz; (void)p;
    static message tmp[MAX_PINS];
    int n = discord_get_pins(s_pins_cid, tmp, MAX_PINS, s_token, s_cacert);
    if (n < 0) n = 0;
    if (s_pins_open) { for (int i = 0; i < n; i++) s_pins[i] = tmp[i];
                       s_pins_count = n; }
    s_pins_loading = false;
    return sceKernelExitDeleteThread(0);
}
static void open_pins(void)
{
    const channel *c = selected_channel();
    if (!c) return;
    if (s_pins_loading) { s_pins_open = true; return; }  // fetch already running
    snprintf(s_pins_cid, sizeof(s_pins_cid), "%s", c->id);
    s_pins_open = true; s_pins_count = 0; s_pins_loading = true;
    SceUID th = sceKernelCreateThread("cl_pins", pins_worker, 0x40, 0x100000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL); else s_pins_loading = false;
}

// edit your bio via the on-screen keyboard, then PATCH it;
// updates the shown bio on success
static void edit_bio(void)
{
    char buf[256];
    if (ime_get_text("Bio", s_me_bio, buf, sizeof(buf))) {
        int code = discord_set_bio(buf, s_token, s_cacert);
        if (code >= 200 && code < 300) snprintf(s_me_bio, sizeof(s_me_bio), "%s", buf);
        console_dbgf("[ui] set bio -> %d", code);
    }
}

static void store_msg_content(const char *mid, char *out, size_t cap)
{
    out[0] = '\0';
    store_lock();
    int mc = 0; message *ms = store_messages(&mc);
    for (int i = 0; i < mc; i++)
        if (strcmp(ms[i].id, mid) == 0) { snprintf(out, cap, "%s", ms[i].content); break; }
    store_unlock();
}

// quick reactions row (shown first); tap the last cell to expand to the full grid
static const char *k_react[6] = {
    "\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x98\x82",
    "\xF0\x9F\x98\xAE", "\xF0\x9F\x98\xA2", "\xF0\x9F\x99\x8F"
};
static int s_emoji_scroll;  // full-grid scroll offset (px)

// full reaction picker set (common emoji, drawn as a scrollable grid)
static const char *k_emoji[] = {
    "\xF0\x9F\x98\x80", "\xF0\x9F\x98\x83", "\xF0\x9F\x98\x84", "\xF0\x9F\x98\x81", "\xF0\x9F\x98\x86", "\xF0\x9F\x98\x85",
    "\xF0\x9F\x98\x82", "\xF0\x9F\xA4\xA3", "\xF0\x9F\x98\x8A", "\xF0\x9F\x98\x87", "\xF0\x9F\x99\x82", "\xF0\x9F\x99\x83",
    "\xF0\x9F\x98\x89", "\xF0\x9F\x98\x8C", "\xF0\x9F\x98\x8D", "\xF0\x9F\xA5\xB0", "\xF0\x9F\x98\x98", "\xF0\x9F\x98\x8B",
    "\xF0\x9F\x98\x9B", "\xF0\x9F\x98\x9C", "\xF0\x9F\xA4\xAA", "\xF0\x9F\x98\x8E", "\xF0\x9F\xA4\xA9", "\xF0\x9F\xA5\xB3",
    "\xF0\x9F\x98\x8F", "\xF0\x9F\x98\x92", "\xF0\x9F\x98\x9E", "\xF0\x9F\x98\x94", "\xF0\x9F\x98\x9F", "\xF0\x9F\x98\x95",
    "\xF0\x9F\x99\x81", "\xF0\x9F\x98\xA3", "\xF0\x9F\x98\x96", "\xF0\x9F\x98\xAB", "\xF0\x9F\x98\xA9", "\xF0\x9F\xA5\xBA",
    "\xF0\x9F\x98\xA2", "\xF0\x9F\x98\xAD", "\xF0\x9F\x98\xA4", "\xF0\x9F\x98\xA0", "\xF0\x9F\x98\xA1", "\xF0\x9F\xA4\xAC",
    "\xF0\x9F\xA4\xAF", "\xF0\x9F\x98\xB3", "\xF0\x9F\xA5\xB5", "\xF0\x9F\xA5\xB6", "\xF0\x9F\x98\xB1", "\xF0\x9F\x98\xA8",
    "\xF0\x9F\x98\xB0", "\xF0\x9F\x98\xA5", "\xF0\x9F\xA4\x97", "\xF0\x9F\xA4\x94", "\xF0\x9F\xA4\xAD", "\xF0\x9F\xA4\xAB",
    "\xF0\x9F\xA4\xA5", "\xF0\x9F\x98\xB6", "\xF0\x9F\x98\x90", "\xF0\x9F\x98\x91", "\xF0\x9F\x98\xAC", "\xF0\x9F\x99\x84",
    "\xF0\x9F\x98\xAF", "\xF0\x9F\x98\xA6", "\xF0\x9F\x98\xAE", "\xF0\x9F\x98\xB2", "\xF0\x9F\xA5\xB1", "\xF0\x9F\x98\xB4",
    "\xF0\x9F\xA4\xA4", "\xF0\x9F\x98\xAA", "\xF0\x9F\x98\xB5", "\xF0\x9F\xA4\x90", "\xF0\x9F\xA5\xB4", "\xF0\x9F\xA4\xA2",
    "\xF0\x9F\xA4\xAE", "\xF0\x9F\xA4\xA7", "\xF0\x9F\x98\xB7", "\xF0\x9F\xA4\x92", "\xF0\x9F\xA4\x95", "\xF0\x9F\x91\x8D",
    "\xF0\x9F\x91\x8E", "\xF0\x9F\x91\x8C", "\xE2\x9C\x8C\xEF\xB8\x8F", "\xF0\x9F\xA4\x9E", "\xF0\x9F\xA4\x9F", "\xF0\x9F\xA4\x98",
    "\xF0\x9F\xA4\x99", "\xF0\x9F\x91\x88", "\xF0\x9F\x91\x89", "\xF0\x9F\x91\x86", "\xF0\x9F\x91\x87", "\xE2\x9C\x8B",
    "\xF0\x9F\xA4\x9A", "\xF0\x9F\x96\x96", "\xF0\x9F\x91\x8B", "\xF0\x9F\xA4\x9D", "\xF0\x9F\x99\x8F", "\xF0\x9F\x92\xAA",
    "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\xA7\xA1", "\xF0\x9F\x92\x9B", "\xF0\x9F\x92\x9A", "\xF0\x9F\x92\x99", "\xF0\x9F\x92\x9C",
    "\xF0\x9F\x96\xA4", "\xF0\x9F\xA4\x8D", "\xF0\x9F\xA4\x8E", "\xF0\x9F\x92\x94", "\xF0\x9F\x92\x95", "\xF0\x9F\x92\x9E",
    "\xF0\x9F\x92\x93", "\xF0\x9F\x92\x97", "\xF0\x9F\x92\x96", "\xF0\x9F\x92\x98", "\xF0\x9F\x92\x9D", "\xF0\x9F\x94\xA5",
    "\xE2\xAD\x90", "\xF0\x9F\x8C\x9F", "\xE2\x9C\xA8", "\xE2\x9A\xA1", "\xF0\x9F\x92\xAF", "\xE2\x9C\x85",
    "\xE2\x9D\x8C", "\xE2\x9D\x93", "\xE2\x9D\x97", "\xF0\x9F\x8E\x89", "\xF0\x9F\x8E\x8A", "\xF0\x9F\x8E\x81",
    "\xF0\x9F\x91\x80", "\xF0\x9F\x92\x80", "\xF0\x9F\xA4\xA1", "\xF0\x9F\x91\xBB", "\xF0\x9F\x92\xA9",
};
#define K_EMOJI_N ((int)(sizeof(k_emoji) / sizeof(k_emoji[0])))
#define EMOJI_COLS 9
#define EMOJI_CELL 54
#define EMOJI_VISROWS 6

// perform action row `sel` of the open menu
static void open_forward(const char *src_cid, const char *src_mid);
// action-menu items in display order; draw_action_menu builds s_menu_acts[]
// and menu_do dispatches on it
enum { ACT_REACT, ACT_REPLY, ACT_THREAD, ACT_FORWARD, ACT_PIN, ACT_COPY, ACT_COPYLINK,
       ACT_PUBLISH, ACT_EDIT, ACT_DELETE };
static int s_menu_acts[10];
static void menu_do(int sel)
{
    // target the channel being viewed (thread/forum post != sidebar selection)
    const char *cid = s_have_open ? s_open_ch.id : NULL;
    if (!cid || !cid[0] || !s_act_msg[0]) { s_menu = 0; return; }
    if (sel < 0 || sel >= s_menu_rows) { s_menu = 0; return; }
    switch (s_menu_acts[sel]) {
    case ACT_REACT: s_menu = 2; s_menu_sel = 0; return;  // emoji picker
    case ACT_REPLY:
        snprintf(s_reply_to, sizeof(s_reply_to), "%s", s_act_msg);
        s_menu = 0; compose_message(); return;
    case ACT_THREAD: {
        char name[96];
        if (ime_get_text("Thread name", "", name, sizeof(name)) && name[0]) {
            struct act *a = calloc(1, sizeof(*a)); if (a) {
                a->kind = 5; snprintf(a->cid, 24, "%s", cid);
                snprintf(a->mid, 24, "%s", s_act_msg);
                snprintf(a->text, sizeof(a->text), "%s", name); spawn_act(a); }
        }
        s_menu = 0; return; }
    case ACT_FORWARD: {
        char mid[24]; snprintf(mid, sizeof(mid), "%s", s_act_msg);
        s_menu = 0; open_forward(cid, mid); return; }
    case ACT_COPY: {
        char buf[2048]; store_msg_content(s_act_msg, buf, sizeof(buf));
        int r = buf[0] ? clipboard_set_utf8(buf) : -1;
        set_toast(r == 0 ? "Copied to clipboard." : "Nothing to copy.");
        s_menu = 0; return; }
    case ACT_COPYLINK: {
        char url[160];
        snprintf(url, sizeof(url), "https://discord.com/channels/%s/%s/%s",
                 s_cur_guild_id[0] ? s_cur_guild_id : "@me", cid, s_act_msg);
        int r = clipboard_set_utf8(url);
        set_toast(r == 0 ? "Message link copied." : "Couldn't copy link.");
        s_menu = 0; return; }
    case ACT_PUBLISH: {
        struct act *a = calloc(1, sizeof(*a)); if (a) {
            a->kind = 7; snprintf(a->cid, 24, "%s", cid);
            snprintf(a->mid, 24, "%s", s_act_msg); spawn_act(a); }
        s_menu = 0; return; }
    case ACT_PIN: {
        struct act *a = calloc(1, sizeof(*a)); if (a) {
            a->kind = 8; a->arg = 1; snprintf(a->cid, 24, "%s", cid);
            snprintf(a->mid, 24, "%s", s_act_msg); spawn_act(a); }
        set_toast("Message pinned.");
        s_menu = 0; return; }
    case ACT_EDIT: {
        char cur[1024]; store_msg_content(s_act_msg, cur, sizeof(cur));
        char text[1024];
        if (ime_get_text("Edit message", cur, text, sizeof(text))) {
            struct act *a = calloc(1, sizeof(*a)); if (a) {
                a->kind = 2; snprintf(a->cid, 24, "%s", cid);
                snprintf(a->mid, 24, "%s", s_act_msg);
                snprintf(a->text, sizeof(a->text), "%s", text); spawn_act(a); }
        }
        s_menu = 0; return; }
    case ACT_DELETE: {
        struct act *a = calloc(1, sizeof(*a)); if (a) {
            a->kind = 3; snprintf(a->cid, 24, "%s", cid);
            snprintf(a->mid, 24, "%s", s_act_msg); spawn_act(a); }
        s_menu = 0; return; }
    }
    s_menu = 0;
}

static void menu_react(int idx)
{
    const channel *c = selected_channel();
    const char *e = NULL;
    if (s_menu == 2 && idx >= 0 && idx < 6)               e = k_react[idx];
    else if (s_menu == 3 && idx >= 0 && idx < K_EMOJI_N)  e = k_emoji[idx];
    if (c && s_act_msg[0] && e) {
        struct act *a = calloc(1, sizeof(*a)); if (a) {
            a->kind = 0; a->arg = 1; snprintf(a->cid, 24, "%s", c->id);
            snprintf(a->mid, 24, "%s", s_act_msg);
            snprintf(a->emoji, sizeof(a->emoji), "%s", e); spawn_act(a); }
    }
    s_menu = 0;
}

void browser_input(unsigned pressed, unsigned held, int ly)
{
    // compose is modal: ignore app buttons while the inline keyboard is up
    if (s_compose_active) return;

    // first frame after READY: restore the last view per the Open To pref
    if (!s_restored) { s_restored = true; browser_apply_open_pref(); }

    int gcount = 0;
    store_lock();
    guild *gs = store_guilds(&gcount);
    bool home = (s_guild < 0);
    guild *g = (!home && s_guild < gcount) ? &gs[s_guild] : NULL;
    build_rows(g, home);
    bool ready = store_ready();
    snprintf(s_cur_guild_id, sizeof(s_cur_guild_id), "%s", (!home && g) ? g->id : "");
    store_unlock();

    // a "Send Message" tap resolved its DM off-thread; open it here (UI thread)
    if (s_open_dm_pending) {
        s_open_dm_pending = 0;
        channel dm; memset(&dm, 0, sizeof(dm));
        snprintf(dm.id, sizeof(dm.id), "%s", s_open_dm_id);
        dm.type = CH_DM;
        snprintf(dm.recipient_id, sizeof(dm.recipient_id), "%s", s_open_dm_rid);
        snprintf(dm.name, sizeof(dm.name), "%s", s_open_dm_name);
        snprintf(dm.avatar, sizeof(dm.avatar), "%s", s_open_dm_av);
        s_guild = -1; s_prof_open = false; open_channel(&dm);
    }

    // on view entry, open the default item: a guild's #general (or its first
    // channel), or the first DM
    if (s_pending_open && ready && s_sel_count > 0) {
        int def = 0;
        if (!home) {
            for (int i = 0; i < s_sel_count; i++)
                if (strcmp(s_rows[s_selectable[i]].ch->name, "general") == 0) { def = i; break; }
        }
        s_chan_sel = def;
        open_channel(selected_channel());
        s_pending_open = false;
    }

    // action menu captures input while open
    if (s_menu) {
        if (s_menu == 3) {  // full emoji grid: 2-D nav
            int moved = 0;
            if (pressed & SCE_CTRL_LEFT)  { if (s_menu_sel > 0) { s_menu_sel--; moved = 1; } }
            if (pressed & SCE_CTRL_RIGHT) { if (s_menu_sel < s_menu_rows - 1) { s_menu_sel++; moved = 1; } }
            if (pressed & SCE_CTRL_UP)    { if (s_menu_sel >= EMOJI_COLS) { s_menu_sel -= EMOJI_COLS; moved = 1; } }
            if (pressed & SCE_CTRL_DOWN)  { if (s_menu_sel + EMOJI_COLS < s_menu_rows) { s_menu_sel += EMOJI_COLS; moved = 1; } }
            if (moved) {  // follow selection only on D-pad move
                int selrow = s_menu_sel / EMOJI_COLS;
                if (selrow * EMOJI_CELL < s_emoji_scroll) s_emoji_scroll = selrow * EMOJI_CELL;
                if ((selrow + 1) * EMOJI_CELL > s_emoji_scroll + EMOJI_VISROWS * EMOJI_CELL)
                    s_emoji_scroll = (selrow + 1) * EMOJI_CELL - EMOJI_VISROWS * EMOJI_CELL;
            }
        } else {
            if (pressed & (SCE_CTRL_UP | SCE_CTRL_LEFT))   { if (s_menu_sel > 0) s_menu_sel--; }
            if (pressed & (SCE_CTRL_DOWN | SCE_CTRL_RIGHT)){ if (s_menu_sel < s_menu_rows - 1) s_menu_sel++; }
        }
        if (pressed & SCE_CTRL_CROSS) {
            if (s_menu == 1) menu_do(s_menu_sel);
            else if (s_menu == 2 && s_menu_sel == 6) { s_menu = 3; s_emoji_scroll = 0; s_menu_sel = 0; }
            else menu_react(s_menu_sel);
        }
        if (pressed & SCE_CTRL_CIRCLE) s_menu = 0;
        return;
    }

    // L toggles the channel list, R toggles the member list
    if (pressed & SCE_CTRL_LTRIGGER) s_channels_collapsed = !s_channels_collapsed;
    if (pressed & SCE_CTRL_RTRIGGER) s_members_collapsed  = !s_members_collapsed;

    if (pressed & SCE_CTRL_UP)   { if (s_chan_sel > 0) s_chan_sel--; }
    if (pressed & SCE_CTRL_DOWN) { if (s_chan_sel < s_sel_count - 1) s_chan_sel++; }

    if (pressed & SCE_CTRL_CROSS) open_channel(selected_channel());
    if (pressed & SCE_CTRL_TRIANGLE) compose_message();

    // left stick scrolls messages
    if (ly > 40 || ly < -40) { s_msg_scroll += ly / 12; s_want_unread = false; s_pin_anchor[0] = '\0'; }
    if (s_msg_scroll < 0) s_msg_scroll = 0;

    maybe_load_older();

    // scroll the selected channel into view when the selection changes
    static int s_follow_sel = -1;
    if (s_chan_sel != s_follow_sel) {
        s_follow_sel = s_chan_sel;
        bool home = (s_guild < 0);
        int top = TOPBAR_H + 8, view_h = (SCREEN_H - 4) - top;
        int sel_row = (s_chan_sel < s_sel_count) ? s_selectable[s_chan_sel] : 0;
        int sy = sidebar_row_y(sel_row, home);
        int rowh = (sel_row < s_row_count) ? sidebar_rowh(sel_row, home) : 26;
        if (sy < s_side_scroll)                 s_side_scroll = sy;
        if (sy + rowh > s_side_scroll + view_h) s_side_scroll = sy + rowh - view_h;
        if (s_side_scroll < 0)                  s_side_scroll = 0;
    }
}

void browser_touch(int x, int y, int dy, bool tap)
{
    // Modal while composing: the inline keyboard owns input; ignore app taps.
    if (s_compose_active) return;

    bool home = (s_guild < 0);
    g_msg_x = SIDE_X + (s_channels_collapsed ? 0 : SIDE_W);  // channel drawer state

    // Lightbox / profile popout / pins list are modal: any tap dismisses them.
    if (s_lightbox_open) { if (tap) s_lightbox_open = false; return; }
    // Role picker (over the profile popout): tap a row to toggle, drag scrolls.
    if (s_rolepick_open) {
        if (tap) {
            for (int i = 0; i < s_n_rp_hit; i++) if (y >= s_rp_hit[i].y0 && y < s_rp_hit[i].y1) {
                snprintf(s_rp_gid, sizeof(s_rp_gid), "%s", s_prof_guild);
                snprintf(s_rp_uid, sizeof(s_rp_uid), "%s", s_prof_id);
                snprintf(s_rp_rid, sizeof(s_rp_rid), "%s", s_rp_hit[i].rid);
                s_rp_add = !s_rp_hit[i].has;
                if (s_rp_add) { if (s_prof_n_role_ids < 24)
                        snprintf(s_prof_role_ids[s_prof_n_role_ids++], 24, "%s", s_rp_rid); }
                else for (int k = 0; k < s_prof_n_role_ids; k++)
                        if (strcmp(s_prof_role_ids[k], s_rp_rid) == 0) {
                            for (int m = k; m < s_prof_n_role_ids - 1; m++)
                                memcpy(s_prof_role_ids[m], s_prof_role_ids[m + 1], 24);
                            s_prof_n_role_ids--; break;
                        }
                srv_spawn(rolepick_worker);
                return;
            }
            s_rolepick_open = false;  // tap off a row closes
        } else { s_rolepick_scroll -= dy; if (s_rolepick_scroll < 0) s_rolepick_scroll = 0; }
        return;
    }
    if (s_prof_open) {
        if (tap) {
            int *r = s_prof_block_btn;
            if (r[2] && x >= r[0] && x < r[0]+r[2] && y >= r[1] && y < r[1]+r[3]) {
                profile_toggle_block(); return;  // stay open to show the new state
            }
            int *nr = s_prof_note_r;
            if (nr[2] && x >= nr[0] && x < nr[0]+nr[2] && y >= nr[1] && y < nr[1]+nr[3]) {
                profile_edit_note(); return;  // stay open to show the new note
            }
            int *re = s_prof_rolesedit_r;
            if (re[2] && x >= re[0] && x < re[0]+re[2] && y >= re[1] && y < re[1]+re[3]) {
                snprintf(s_rp_gid, sizeof(s_rp_gid), "%s", s_prof_guild);
                s_rolepick_open = 1; s_rolepick_scroll = 0; return;
            }
            int *mb = s_prof_msg_btn;
            if (mb[2] && x >= mb[0] && x < mb[0]+mb[2] && y >= mb[1] && y < mb[1]+mb[3]) {
                snprintf(s_msg_uid, sizeof(s_msg_uid), "%s", s_prof_id);
                snprintf(s_msg_name, sizeof(s_msg_name), "%s", s_prof_name);
                snprintf(s_msg_av, sizeof(s_msg_av), "%s", s_prof_avatar);
                srv_spawn(msg_open_worker); return;  // open/create DM off-thread
            }
            // Moderation buttons: first tap arms (turns red), second tap confirms.
            #define MODHIT(r) ((r)[2] && x >= (r)[0] && x < (r)[0]+(r)[2] && y >= (r)[1] && y < (r)[1]+(r)[3])
            struct { int *r; int act; } modb[3] = {
                { s_prof_kick_btn, 1 }, { s_prof_ban_btn, 2 }, { s_prof_timeout_btn, 3 } };
            for (int i = 0; i < 3; i++) if (MODHIT(modb[i].r)) {
                if (s_prof_mod_arm != modb[i].act) { s_prof_mod_arm = modb[i].act; return; }  // arm
                s_prof_mod_arm = 0;
                snprintf(s_mod_gid, sizeof(s_mod_gid), "%s", s_prof_guild);
                snprintf(s_mod_uid, sizeof(s_mod_uid), "%s", s_prof_id);
                s_mod_action = modb[i].act; srv_spawn(mod_worker);
                s_prof_open = false; return;
            }
            #undef MODHIT
            s_prof_mod_arm = 0;
            s_prof_open = false;
        }
        return;
    }
    if (s_pins_open) { if (tap) s_pins_open = false; return; }
    if (s_srvinfo_open) {
        if (tap) {
            #define SRVHIT(r) ((r)[2] && x >= (r)[0] && x < (r)[0]+(r)[2] && y >= (r)[1] && y < (r)[1]+(r)[3])
            if (SRVHIT(s_srvbtn_invite)) {
                const char *cid = first_text_channel(store_find_guild(s_srvinfo_gid));
                if (cid) { snprintf(s_srv_act_cid, sizeof(s_srv_act_cid), "%s", cid);
                           srv_spawn(srv_invite_worker); }
                s_srv_confirm_leave = 0; return;
            }
            if (SRVHIT(s_srvbtn_read))  { mark_guild_read(); s_srv_confirm_leave = 0; return; }
            if (SRVHIT(s_srvbtn_mute))  {
                snprintf(s_srv_act_gid, sizeof(s_srv_act_gid), "%s", s_srvinfo_gid);
                s_srv_mute_want = 1; srv_spawn(srv_mute_worker); s_srv_confirm_leave = 0; return;
            }
            if (SRVHIT(s_srvbtn_leave)) {
                if (!s_srv_confirm_leave) { s_srv_confirm_leave = 1; return; }  // arm confirm
                snprintf(s_srv_act_gid, sizeof(s_srv_act_gid), "%s", s_srvinfo_gid);
                srv_spawn(srv_leave_worker);
                s_srvinfo_open = 0; s_srv_confirm_leave = 0;
                s_guild = -1; s_pending_open = true;  // drop to Home after leaving
                return;
            }
            #undef SRVHIT
            s_srvinfo_open = false; s_srv_confirm_leave = 0;
        }
        return;
    }

    // Confirmation modal (over anything): Keep dismisses, the red button acts.
    if (s_confirm_open) {
        if (tap) {
            int *y2 = s_confirm_yes_r;
            if (y2[2] && x >= y2[0] && x < y2[0]+y2[2] && y >= y2[1] && y < y2[1]+y2[3])
                friend_act(1, s_confirm_fid, NULL);
            s_confirm_open = 0;  // Keep / act / tap-away all dismiss
        }
        return;
    }

    // Friends modal: action buttons, then row tap -> DM/profile; drag scrolls.
    if (s_friends_open) {
        if (tap) {
            #define IN_R4(r) ((r)[2] && x>=(r)[0] && x<(r)[0]+(r)[2] && y>=(r)[1] && y<(r)[1]+(r)[3])
            if (IN_R4(s_friends_add_btn)) { add_friend_prompt(); return; }
            for (int i = 0; i < s_friends_n; i++) {
                if (IN_R4(s_fbtn_a[i])) {
                    if (s_friends[i].type == 3) { friend_act(0, s_friends[i].id, NULL); return; }  // accept: no confirm
                    confirm_friend_remove(i);  // remove friend / cancel request -> confirm
                    return;
                }
                if (IN_R4(s_fbtn_b[i])) {  // deny (incoming) -> confirm
                    confirm_friend_remove(i);
                    return;
                }
            }
            #undef IN_R4
            for (int i = 0; i < s_friends_n; i++)
                if (y >= s_friends_hit_y0[i] && y < s_friends_hit_y1[i]) {
                    if (s_friends[i].type == 1) open_friend(&s_friends[i]);
                    else { s_friends_open = false;
                           open_profile(s_friends[i].id, s_friends[i].name, s_friends[i].avatar); }
                    return;
                }
            s_friends_open = false;
        } else {
            s_friends_scroll -= dy;
            if (s_friends_scroll > s_friends_max) s_friends_scroll = s_friends_max;
            if (s_friends_scroll < 0) s_friends_scroll = 0;
        }
        return;
    }

    // Search results modal: tap a user -> profile, a message -> jump, else close.
    // Mention picker: tap a member, drag scrolls, else dismiss.
    if (s_mention_open) {
        if (tap) {
            for (int i = 0; i < s_mention_n; i++)
                if (y >= s_mention_hit_y0[i] && y < s_mention_hit_y1[i]) { mention_pick(i); return; }
            s_mention_open = 0;
        } else {
            s_mention_scroll -= dy;
            if (s_mention_scroll > s_mention_max) s_mention_scroll = s_mention_max;
            if (s_mention_scroll < 0) s_mention_scroll = 0;
        }
        return;
    }

    // Forward picker: tap a destination, drag scrolls, else dismiss.
    if (s_forward_open) {
        if (tap) {
            for (int i = 0; i < s_fwd_n; i++)
                if (y >= s_fwd_hit_y0[i] && y < s_fwd_hit_y1[i]) { forward_to(i); return; }
            s_forward_open = 0;
        } else {
            s_fwd_scroll -= dy;
            if (s_fwd_scroll > s_fwd_max) s_fwd_scroll = s_fwd_max;
            if (s_fwd_scroll < 0) s_fwd_scroll = 0;
        }
        return;
    }

    // Add-a-Server modal: Create My Own / Join a Server, else dismiss.
    if (s_addserver_open) {
        if (tap) {
            #define IN_R4(r) ((r)[2] && x>=(r)[0] && x<(r)[0]+(r)[2] && y>=(r)[1] && y<(r)[1]+(r)[3])
            if (IN_R4(s_as_create_r))    { s_addserver_open = 0; add_server_create(); }
            else if (IN_R4(s_as_join_r)) { s_addserver_open = 0; add_server_join(); }
            else                           s_addserver_open = 0;
            #undef IN_R4
        }
        return;
    }

    // Attachment sheet: tap Photo Library / Camera, else dismiss. The picker
    // dialogs block (drive their own render loop), so run them on this thread.
    if (s_attach_open) {
        if (tap) {
            #define IN_AR(r) (x>=(r)[0] && x<(r)[0]+(r)[2] && y>=(r)[1] && y<(r)[1]+(r)[3])
            if (IN_AR(s_attach_r[0]))      { s_attach_open = 0; attach_photo();  }
            else if (IN_AR(s_attach_r[1])) { s_attach_open = 0; attach_camera(); }
            else                             s_attach_open = 0;
            #undef IN_AR
        }
        return;
    }

    if (s_search_open) {
        if (tap) {
            for (int i = 0; i < s_suser_n; i++)
                if (y >= s_suser_hit_y0[i] && y < s_suser_hit_y1[i]) {
                    s_search_open = false;
                    open_profile(s_susers[i].id, s_susers[i].name, s_susers[i].avatar);
                    return;
                }
            for (int i = 0; i < s_search_n; i++)
                if (y >= s_search_hit_y0[i] && y < s_search_hit_y1[i]) {
                    s_search_open = false;
                    jump_to_message(s_search_res[i].channel_id, s_search_res[i].id);
                    return;
                }
            s_search_open = false;
        } else {  // drag scrolls the results
            s_search_scroll -= dy;
            if (s_search_scroll > s_search_max) s_search_scroll = s_search_max;
            if (s_search_scroll < 0) s_search_scroll = 0;
        }
        return;
    }

    // Threads modal: tap a thread to open it, else dismiss.
    if (s_thr_open) {
        if (tap) {
            for (int i = 0; i < s_thr_n; i++)
                if (y >= s_thr_hit_y0[i] && y < s_thr_hit_y1[i]) {
                    s_thr_open = false; open_channel(&s_thr[i]); return;
                }
            s_thr_open = false;
        }
        return;
    }

    // New-post form: tap fields to type, chips to pick a tag, Post to submit.
    if (s_np_open) {
        if (tap) {
            #define IN_R(r) (x>=(r)[0] && x<(r)[0]+(r)[2] && y>=(r)[1] && y<(r)[1]+(r)[3])
            if (IN_R(s_np_title_r)) {
                char t[128]; if (ime_get_text("Title", s_np_title, t, sizeof(t)))
                    snprintf(s_np_title, sizeof(s_np_title), "%s", t);
            } else if (IN_R(s_np_content_r)) {
                char t[512]; if (ime_get_text("Message", s_np_content, t, sizeof(t)))
                    snprintf(s_np_content, sizeof(s_np_content), "%s", t);
            } else if (IN_R(s_np_post_r)) {
                np_submit();
            } else {
                int hit = 0;
                for (int i = 0; i < s_np_ntags; i++)
                    if (s_np_tag_r[i][2] && IN_R(s_np_tag_r[i])) { s_np_tagsel = i; hit = 1; break; }
                if (!hit) {  // tap outside the card = cancel
                    int cw=460, ch=344, cx=(SCREEN_W-cw)/2, cy=(SCREEN_H-ch)/2;
                    if (x<cx || x>=cx+cw || y<cy || y>=cy+ch) s_np_open = false;
                }
            }
            #undef IN_R
        }
        return;
    }

    // Account switcher overlays Settings: pick an account or add one.
    if (s_accounts_open) {
        if (tap) {
            if (x >= s_acc_add_r[0] && x < s_acc_add_r[0] + s_acc_add_r[2] &&
                y >= s_acc_add_r[1] && y < s_acc_add_r[1] + s_acc_add_r[3]) {
                s_add_account_req = 1; s_accounts_open = 0; s_settings_open = 0;
            } else {
                for (int i = 0; i < s_accounts_n; i++)
                    if (y >= s_acc_hit_y0[i] && y < s_acc_hit_y1[i]) { accounts_pick(i); return; }
                s_accounts_open = 0;  // tap elsewhere closes the switcher
            }
        }
        return;
    }

    // Settings is modal: buttons act; a tap outside the card closes.
    if (s_settings_open) {
        if (!tap) {  // drag scrolls the (possibly tall) card
            s_set_scroll -= dy;
            if (s_set_scroll < 0) s_set_scroll = 0;
            if (s_set_scroll > s_set_scroll_max) s_set_scroll = s_set_scroll_max;
            return;
        }
        if (tap) {
            bool inbtn = (x >= s_set_cx + 20 && x < s_set_cx + s_set_cw - 20);
            static const int scode[4] = { 1, 2, 3, 0 };
            static const char *sstr[4] = { "online", "idle", "dnd", "invisible" };
            int hit = -1;
            for (int i = 0; i < 4; i++)
                if (x >= s_status_r[i][0] && x < s_status_r[i][0] + s_status_r[i][2] &&
                    y >= s_status_r[i][1] && y < s_status_r[i][1] + s_status_r[i][3]) hit = i;
            bool in_cs = (x >= s_set_cs_r[0] && x < s_set_cs_r[0] + s_set_cs_r[2] &&
                          y >= s_set_cs_r[1] && y < s_set_cs_r[1] + s_set_cs_r[3]);
            bool in_av = (x >= s_set_av_r[0] && x < s_set_av_r[0] + s_set_av_r[2] &&
                          y >= s_set_av_r[1] && y < s_set_av_r[1] + s_set_av_r[3]);
            bool in_nm = (x >= s_set_name_r[0] && x < s_set_name_r[0] + s_set_name_r[2] &&
                          y >= s_set_name_r[1] && y < s_set_name_r[1] + s_set_name_r[3]);
            bool in_sw = (x >= s_set_sw_r[0] && x < s_set_sw_r[0] + s_set_sw_r[2] &&
                          y >= s_set_sw_r[1] && y < s_set_sw_r[1] + s_set_sw_r[3]);
            int app_hit = -1;
            for (int i = 0; i < SET_NAPP; i++)
                if (x >= s_set_app_r[i][0] && x < s_set_app_r[i][0] + s_set_app_r[i][2] &&
                    y >= s_set_app_r[i][1] && y < s_set_app_r[i][1] + s_set_app_r[i][3]) app_hit = i;
            if (in_av) {
                settings_change_avatar();
            } else if (in_nm) {
                settings_edit_name();
            } else if (in_sw) {
                open_accounts();
            } else if (hit >= 0) {
                s_my_status = scode[hit];
                gateway_set_status(sstr[hit]);
            } else if (in_cs) {
                // Init with current so Cancel keeps it; a confirmed empty clears it.
                char t[128]; snprintf(t, sizeof(t), "%s", s_me_custom);
                ime_get_text("Custom status", s_me_custom, t, sizeof(t));
                snprintf(s_me_custom, sizeof(s_me_custom), "%s", t);
                gateway_set_custom_status(s_me_custom);
            } else if (app_hit >= 0) {
                // Cycle/toggle the setting, live-apply the visual ones, persist.
                switch (app_hit) {
                case 0: g_settings.theme ^= 1; theme_set(g_settings.theme); break;
                case 1: g_settings.accent = (g_settings.accent + 1) % theme_accent_count();
                        theme_set_accent(g_settings.accent); break;
                case 2: g_settings.font_size = (g_settings.font_size + 1) % 3;
                        font_set(g_settings.font_size); break;
                case 3: g_settings.density ^= 1; break;
                case 4: g_settings.clock_24h ^= 1; break;
                case 5: g_settings.open_last ^= 1; break;
                case 6: g_settings.low_battery ^= 1; break;
                }
                settings_save();
            } else if (inbtn && y >= s_set_eb_y && y < s_set_eb_y + SET_BTN_H) {
                edit_bio();
            } else if (inbtn && y >= s_set_lo_y && y < s_set_lo_y + SET_BTN_H) {
                s_logout_req = true; s_settings_open = false;
            } else if (x < s_set_cx || x >= s_set_cx + s_set_cw ||
                       y < s_set_cy || y >= s_set_cy + s_set_ch) {
                s_settings_open = false;
            }
        }
        return;
    }

    // Action menu is modal: a tap either hits an item or dismisses it.
    if (tap && s_menu) {
        bool inside = (x >= s_menu_rx && x < s_menu_rx + s_menu_rw &&
                       y >= s_menu_ry && y < s_menu_ry + s_menu_rh);
        if (!inside) { s_menu = 0; return; }
        if (s_menu == 1) {
            int row = (y - (s_menu_ry + 6)) / s_menu_rowh;
            if (row >= 0 && row < s_menu_rows) menu_do(row);
        } else if (s_menu == 2) {  // quick row + expand cell
            int col = (x - (s_menu_rx + 6)) / s_menu_rowh;
            if (col == 6) { s_menu = 3; s_emoji_scroll = 0; s_menu_sel = 0; }  // expand
            else if (col >= 0 && col < 6) menu_react(col);
        } else {  // full emoji grid
            int col = (x - (s_menu_rx + 6)) / s_menu_rowh;
            int row = (y - (s_menu_ry + 6) + s_emoji_scroll) / s_menu_rowh;
            if (col >= 0 && col < EMOJI_COLS && row >= 0) {
                int idx = row * EMOJI_COLS + col;
                if (idx < K_EMOJI_N) menu_react(idx);
            }
        }
        return;
    }
    if (!tap && s_menu == 3) { s_emoji_scroll -= dy; return; }  // drag scrolls the grid
    if (s_menu) return;  // ignore other drags while the menu is up

    if (tap) {
        int msg_right0 = SCREEN_W - (s_members_collapsed ? 0 : MEMBER_W);
        // Forum view: New Post button, or tap a post card to open that thread.
        if (s_forum_view && x >= g_msg_x && x < msg_right0 && y > TOPBAR_H) {
            if (x >= s_forum_new_x0 && x < s_forum_new_x1 &&
                y >= s_forum_new_y0 && y < s_forum_new_y1) { new_forum_post(); return; }
            for (int i = 0; i < s_forum_n; i++)
                if (y >= s_forum_hit_y0[i] && y < s_forum_hit_y1[i]) {
                    open_channel(&s_forum[i]); return;
                }
            return;
        }
        // Channel-column collapse arrow (far left of the message header).
        if (y < TOPBAR_H && s_chtoggle_r[2] &&
            x >= s_chtoggle_r[0] && x < s_chtoggle_r[0] + s_chtoggle_r[2]) {
            s_channels_collapsed = !s_channels_collapsed; return;
        }
        // Topbar back button (in a thread) / Pins button.
        if (y < TOPBAR_H && s_back_r[2] &&
            x >= s_back_r[0] && x < s_back_r[0] + s_back_r[2]) { thread_go_back(); return; }
        if (y < TOPBAR_H && s_pin_btn_x0 >= 0 &&
            x >= s_pin_btn_x0 && x < s_pin_btn_x1) { open_pins(); return; }
        if (y < TOPBAR_H && s_thr_btn_x0 >= 0 &&
            x >= s_thr_btn_x0 && x < s_thr_btn_x1) { open_threads(); return; }
        if (y < TOPBAR_H && s_search_btn_x0 >= 0 &&
            x >= s_search_btn_x0 && x < s_search_btn_x1) { open_search(); return; }
        if (y < TOPBAR_H && s_memtoggle_r[2] &&
            x >= s_memtoggle_r[0] && x < s_memtoggle_r[0] + s_memtoggle_r[2]) {
            s_members_collapsed = !s_members_collapsed; return;
        }
        // Jump-to-present button (scrolled up).
        if (s_jump_show) {
            int dx = x - s_jump_r[0], dyj = y - s_jump_r[1];
            if (dx*dx + dyj*dyj <= s_jump_r[2]*s_jump_r[2]) {
                s_want_bottom = true; s_want_unread = false; return;
            }
        }
        // Attach "+" at the left of the compose bar -> attachment sheet.
        if (y >= SCREEN_H - 46 && x >= g_msg_x + 14 && x < g_msg_x + 46 &&
            selected_channel() && !s_uploading) { s_attach_open = 1; return; }
        // Mention "@" button -> member picker.
        if (y >= SCREEN_H - 46 && x >= g_msg_x + 46 && x < g_msg_x + 74 &&
            selected_channel()) { open_mention_picker(); return; }
        // Compose bar at the bottom -> open the system keyboard to type/send.
        if (y >= SCREEN_H - 46 && x >= g_msg_x + 12 && x < msg_right0 - 12 &&
            selected_channel()) { compose_message(); return; }
        if (x >= g_msg_x && x < msg_right0 && y > TOPBAR_H && y < SCREEN_H - 52) {
            // Tapping a video card plays it fullscreen (hardware decode).
            for (int i = 0; i < s_nvidhit; i++)
                if (x >= s_vidhit[i].x0 && x < s_vidhit[i].x1 &&
                    y >= s_vidhit[i].y0 && y < s_vidhit[i].y1) {
                    video_play(s_vidhit[i].url, s_cacert); return;
                }
            // Tapping an inline image opens the lightbox at a sharper size.
            for (int i = 0; i < s_nimghit; i++)
                if (x >= s_imghit[i].x0 && x < s_imghit[i].x1 &&
                    y >= s_imghit[i].y0 && y < s_imghit[i].y1) {
                    make_view_url(s_imghit[i].url, s_lightbox_url,
                                  sizeof(s_lightbox_url), 1280);
                    s_lightbox_open = true; return;
                }
            // Tapping a poll answer votes for it.
            for (int i = 0; i < s_nphit; i++)
                if (x >= s_phit[i].x0 && x < s_phit[i].x1 &&
                    y >= s_phit[i].y0 && y < s_phit[i].y1) {
                    poll_vote(s_phit[i].mid, s_phit[i].aid); return;
                }
            // Tapping a reply preview jumps to the referenced message.
            for (int i = 0; i < s_nrephit; i++)
                if (y >= s_rephit[i].y0 && y < s_rephit[i].y1) {
                    jump_to_message(s_rephit[i].cid, s_rephit[i].mid); return;
                }
            // Invite tile Join/Go button.
            for (int i = 0; i < s_ninvhit; i++)
                if (x >= s_invhit[i].x0 && x < s_invhit[i].x1 &&
                    y >= s_invhit[i].y0 && y < s_invhit[i].y1) {
                    if (s_invhit[i].joined) {  // already a member -> switch to it
                        int gc = 0; store_lock(); guild *gs = store_guilds(&gc);
                        int gi = -1;
                        for (int j = 0; j < gc; j++)
                            if (strcmp(gs[j].id, s_invhit[i].guild_id) == 0) { gi = j; break; }
                        store_unlock();
                        if (gi >= 0) { s_guild = gi; s_chan_sel = 0; s_side_scroll = 0;
                                       s_pending_open = true; }
                    } else {  // join it
                        snprintf(s_join_code, sizeof(s_join_code), "%s", s_invhit[i].code);
                        SceUID th = sceKernelCreateThread("cl_join", join_worker, 0x40, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
                        if (th >= 0) sceKernelStartThread(th, 0, NULL);
                    }
                    return;
                }
            // Tapping a reaction pill toggles your reaction.
            for (int i = 0; i < s_nrhit; i++)
                if (x >= s_rhit[i].x0 && x < s_rhit[i].x1 &&
                    y >= s_rhit[i].y0 && y < s_rhit[i].y1) {
                    toggle_reaction(s_rhit[i].mid, s_rhit[i].emoji, s_rhit[i].me); return;
                }
            // Tapping the avatar opens the author's profile; tapping the message
            // body opens the action menu.
            for (int i = 0; i < s_nmhit; i++) {
                msg_hit *hb = &s_mhit[i];
                if (y < hb->y0 || y >= hb->y1) continue;
                if (hb->av_cy >= 0 && x >= g_msg_x + 16 && x < g_msg_x + 52 &&
                    y >= hb->av_cy - 18 && y < hb->av_cy + 18 && hb->author_id[0]) {
                    open_profile(hb->author_id, hb->author, hb->avatar);
                } else if (hb->has_spoiler && !spoiler_shown(hb->id)) {
                    spoiler_reveal(hb->id);  // first tap reveals spoilers
                } else {
                    snprintf(s_act_msg, sizeof(s_act_msg), "%s", hb->id);
                    s_act_own = hb->own;
                    s_menu = 1; s_menu_sel = 0; s_menu_x = x; s_menu_y = y;
                }
                return;
            }
            return;
        }
        if (x < RAIL_W) {  // server rail
            if (y >= SCREEN_H - 52) {  // bottom user area -> Settings
                open_settings();
            } else if (y > 40 - 26 && y < 40 + 26) {  // Home / DMs button
                if (s_guild != -1) { s_guild = -1; s_chan_sel = 0;
                                     s_side_scroll = 0; s_pending_open = true; }
            } else if (s_addsrv_y0 >= 0 && y >= s_addsrv_y0 && y < s_addsrv_y1) {
                s_addserver_open = 1;  // "+" -> Add-a-Server modal
            } else {
                int gc = 0; store_lock(); store_guilds(&gc); store_unlock();
                for (int slot = 0; slot < gc; slot++) {
                    int cy = RAIL_G0 + slot * RAIL_PITCH - s_rail_scroll;
                    if (y > cy - 28 && y < cy + 28) {
                        int gi = (slot < s_rail_order_n) ? s_rail_order[slot] : slot;
                        if (s_guild != gi) { s_guild = gi; s_chan_sel = 0;
                                             s_side_scroll = 0; s_pending_open = true; }
                        break;
                    }
                }
            }
        } else if (x < g_msg_x) {  // channel / DM list
            // Home header "Friends" button.
            if (home && s_friends_btn[2] &&
                x >= s_friends_btn[0] && x < s_friends_btn[0] + s_friends_btn[2] &&
                y >= s_friends_btn[1] && y < s_friends_btn[1] + s_friends_btn[3]) {
                open_friends(); return;
            }
            // Guild header "Mark Read" button.
            if (s_markread_btn[2] &&
                x >= s_markread_btn[0] && x < s_markread_btn[0] + s_markread_btn[2] &&
                y >= s_markread_btn[1] && y < s_markread_btn[1] + s_markread_btn[3]) {
                mark_guild_read(); return;
            }
            // Guild header (name/banner) -> server info popout, like a profile.
            if (!home && y < TOPBAR_H && s_cur_guild_id[0]) {
                open_serverinfo(s_cur_guild_id); return;
            }
            int ry = TOPBAR_H + 8 - s_side_scroll;  // pixel scroll
            for (int r = 0; r < s_row_count; r++) {
                int rowh = sidebar_rowh(r, home);
                if (y >= TOPBAR_H && y >= ry && y < ry + rowh) {
                    if (s_rows[r].is_cat) toggle_cat(s_rows[r].ch->id);
                    else for (int j = 0; j < s_sel_count; j++)
                        if (s_selectable[j] == r) {
                            s_chan_sel = j; open_channel(selected_channel()); break;
                        }
                    break;
                }
                ry += rowh;
            }
        }
        return;
    }

    // drag: scroll the pane the finger is over
    int msg_right = SCREEN_W - s_member_w;
    if (x >= g_msg_x && x < msg_right && s_forum_view) {
        s_forum_scroll -= dy;  // forum post list
        int fmax = s_forum_n * 54 - (SCREEN_H - TOPBAR_H - 60);
        if (s_forum_scroll > fmax) s_forum_scroll = fmax;
        if (s_forum_scroll < 0) s_forum_scroll = 0;
    } else if (x >= g_msg_x && x < msg_right) {
        s_msg_scroll -= dy;  // content follows the finger
        if (s_msg_scroll < 0) s_msg_scroll = 0;
        s_want_bottom = false;
        s_want_unread = false;  // user took over scrolling
        s_pin_anchor[0] = '\0';
    } else if (x >= SIDE_X && x < g_msg_x) {  // channel / DM list
        s_side_scroll -= dy;
        if (s_side_scroll > s_side_max) s_side_scroll = s_side_max;
        if (s_side_scroll < 0) s_side_scroll = 0;
    } else if (x < RAIL_W) {  // scroll the guild rail
        int gc = 0; store_lock(); store_guilds(&gc); store_unlock();
        s_rail_scroll -= dy;
        if (s_rail_scroll > rail_max_scroll(gc)) s_rail_scroll = rail_max_scroll(gc);
        if (s_rail_scroll < 0) s_rail_scroll = 0;
    } else if (s_member_w && x >= SCREEN_W - s_member_w) {  // scroll the member list
        s_member_scroll -= dy;
        if (s_member_scroll > s_member_max) s_member_scroll = s_member_max;
        if (s_member_scroll < 0) s_member_scroll = 0;
    }
}


static void draw_rail(guild *gs, int gcount)
{
    ui_rect(0, 0, RAIL_W, SCREEN_H, COL_GUILDBAR);

    // Home / DMs button at the top (rounded square, like Discord's logo).
    int hy = 40;
    bool home_sel = (s_guild < 0);
    if (home_sel) ui_rect(0, hy - 22, 4, 44, COL_WHITE);
    ui_rounded_rect(RAIL_W / 2 - 22, hy - 22, 44, 44, home_sel ? 15 : 22,
                    home_sel ? COL_BLURPLE : COL_ELEVATED);
    int aw = ui_text_w(FS_HEADER, "@");
    ui_text_bold(RAIL_W / 2 - aw / 2, hy - 12, FS_HEADER, COL_WHITE, "@");
    ui_rect(14, hy + 32, RAIL_W - 28, 2, COL_CHAT);  // divider

    if (s_rail_scroll > rail_max_scroll(gcount)) s_rail_scroll = rail_max_scroll(gcount);
    if (s_rail_scroll < 0) s_rail_scroll = 0;
    rail_compute_order(gs, gcount);
    for (int slot = 0; slot < s_rail_order_n; slot++) {
        int gi = s_rail_order[slot];
        int cy = RAIL_G0 + slot * RAIL_PITCH - s_rail_scroll;
        if (cy < hy + 30 || cy > SCREEN_H + 30) continue;  // off-rail: skip
        unsigned bg = (gi == s_guild) ? COL_BLURPLE : COL_ELEVATED;

        // Roll up unread / mention state from this guild's channels.
        bool gunread = false; int gmentions = 0;
        for (int ch = 0; ch < gs[gi].channel_count; ch++) {
            if (gs[gi].channels[ch].unread)   gunread = true;
            gmentions += gs[gi].channels[ch].mentions;
        }

        // Left indicator pill: tall when selected, a short nub when merely unread
        // (Discord's language).
        if (gi == s_guild)     ui_rect(0, cy - 22, 4, 44, COL_WHITE);
        else if (gunread)      ui_rounded_rect(0, cy - 5, 4, 10, 2, COL_WHITE);

        char url[256] = "";
        if (gs[gi].icon[0])
            snprintf(url, sizeof(url),
                     "https://cdn.discordapp.com/icons/%s/%s.png?size=64",
                     gs[gi].id, gs[gi].icon);
        draw_guild_icon(RAIL_W / 2, cy, 22, url, gs[gi].name, bg);

        // Red mention badge at the bottom-right of the icon.
        if (gmentions > 0) {
            char mb[8]; snprintf(mb, sizeof(mb), "%d", gmentions > 99 ? 99 : gmentions);
            int bw = ui_text_bold_w(FS_TINY, mb) + 10;
            int bx = RAIL_W / 2 + 22 - bw, by = cy + 22 - 15;
            ui_rounded_rect(bx, by, bw, 16, 8, COL_RED);
            ui_text_bold(bx + 5, by - 1, FS_TINY, COL_WHITE, mb);
        }
    }

    // "Add a server" (+) button after the last guild.
    {
        int cy = RAIL_G0 + gcount * RAIL_PITCH - s_rail_scroll;
        if (cy >= hy + 30 && cy <= SCREEN_H - 52) {
            ui_rounded_rect(RAIL_W / 2 - 22, cy - 22, 44, 44, 22, COL_ELEVATED);
            // plus drawn as two centered bars (font metrics don't center cleanly)
            int cxp = RAIL_W / 2, arm = 9;
            ui_rect(cxp - arm, cy - 2, arm * 2, 4, COL_GREEN);
            ui_rect(cxp - 2, cy - arm, 4, arm * 2, COL_GREEN);
            s_addsrv_y0 = cy - 22; s_addsrv_y1 = cy + 22;
        } else { s_addsrv_y0 = s_addsrv_y1 = -1; }
    }

    // Re-fill the top strip and redraw the DM button + divider over the icons, so
    // guild icons scrolled up vanish under the top line.
    ui_rect(0, 0, RAIL_W, hy + 34, COL_GUILDBAR);
    if (home_sel) ui_rect(0, hy - 22, 4, 44, COL_WHITE);
    ui_rounded_rect(RAIL_W / 2 - 22, hy - 22, 44, 44, home_sel ? 15 : 22,
                    home_sel ? COL_BLURPLE : COL_ELEVATED);
    ui_text_bold(RAIL_W / 2 - aw / 2, hy - 12, FS_HEADER, COL_WHITE, "@");
    ui_rect(14, hy + 32, RAIL_W - 28, 2, COL_CHAT);

    // User area at the bottom: your avatar -> Settings (masks scrolled guilds).
    int uy = SCREEN_H - 32;
    ui_rect(0, uy - 30, RAIL_W, 62, COL_GUILDBAR);
    ui_rect(14, uy - 30, RAIL_W - 28, 2, COL_CHAT);
    char murl[256] = "";
    if (store_me_avatar()[0] && store_me_id()[0])
        snprintf(murl, sizeof(murl),
                 "https://cdn.discordapp.com/avatars/%s/%s.png?size=64",
                 store_me_id(), store_me_avatar());
    draw_avatar(RAIL_W / 2, uy, 17, murl, store_me_name(), COL_BLURPLE);
    draw_status_dot(RAIL_W / 2 + 12, uy + 12, s_my_status, COL_GUILDBAR);
}

static void draw_sidebar(guild *g, bool home)
{
    if (s_channels_collapsed) return;  // channel drawer hidden; message pane fills it
    ui_rect(SIDE_X, 0, SIDE_W, SCREEN_H, COL_SIDEBAR);

    int top = TOPBAR_H + 8;
    int bot = SCREEN_H - 4;
    int total = sidebar_total_h(home);
    s_side_max = total > (bot - top) ? total - (bot - top) : 0;
    if (s_side_scroll > s_side_max) s_side_scroll = s_side_max;
    if (s_side_scroll < 0) s_side_scroll = 0;

    int y = top - s_side_scroll;
    for (int r = 0; r < s_row_count; r++) {
        row_t *row = &s_rows[r];
        int rowh = sidebar_rowh(r, home);
        if (y + rowh <= TOPBAR_H || y >= SCREEN_H) { y += rowh; continue; }  // offscreen
        if (row->is_cat) {
            char up[80]; int k = 0;
            for (const char *p = row->ch->name; *p && k < 79; p++, k++)
                up[k] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
            up[k] = '\0';
            // collapse chevron
            bool fold = cat_collapsed(row->ch->id);
            ui_text(SIDE_X + 6, y + 3, FS_TINY, COL_TEXT_MUTED,
                    fold ? "\xE2\x80\xBA" : "\xCB\x85");
            ui_text_bold(SIDE_X + 16, y + 4, FS_TINY, COL_TEXT_MUTED, up);
        } else {
            bool sel = (s_chan_sel < s_sel_count && s_selectable[s_chan_sel] == r);
            bool unread = row->ch->unread;
            int  mentions = row->ch->mentions;
            if (sel) ui_rounded_rect(SIDE_X + 6, y - 2, SIDE_W - 12, rowh - 2, 6, COL_SELECTED);
            // unread pill on the far left
            if (unread && !sel)
                ui_rounded_rect(SIDE_X + 2, y + rowh / 2 - 5, 4, 8, 2, COL_WHITE);
            unsigned col = (sel || unread) ? COL_TEXT_BRIGHT : COL_TEXT_MUTED;

            if (home) {
                char durl[256] = "";
                if (row->ch->avatar[0] && row->ch->type == CH_GROUP_DM)
                    snprintf(durl, sizeof(durl),
                             "https://cdn.discordapp.com/channel-icons/%s/%s.png?size=64",
                             row->ch->id, row->ch->avatar);
                else if (row->ch->avatar[0] && row->ch->recipient_id[0])
                    snprintf(durl, sizeof(durl),
                             "https://cdn.discordapp.com/avatars/%s/%s.png?size=64",
                             row->ch->recipient_id, row->ch->avatar);
                draw_avatar(SIDE_X + 22, y + 11, 12, durl, row->ch->name, COL_BLURPLE);
                if (unread) ui_text_bold_clip(SIDE_X + 42, y, FS_CHANNEL, col, row->ch->name, SIDE_W - 78);
                else        ui_text_clip(SIDE_X + 42, y, FS_CHANNEL, col, row->ch->name, SIDE_W - 78);
            } else if (ch_is_thread(row->ch->type)) {
                // Thread: indented under its parent with a branch connector.
                int tx = SIDE_X + 34;
                ui_rect(SIDE_X + 22, y - 2, 2, rowh / 2, COL_DIVIDER);  // stub
                ui_rect(SIDE_X + 22, y + rowh / 2 - 3, 9, 2, COL_DIVIDER);  // hook
                if (unread) ui_text_bold_clip(tx, y, FS_CHANNEL, col, row->ch->name, SIDE_W - 52);
                else        ui_text_clip(tx, y, FS_CHANNEL, col, row->ch->name, SIDE_W - 52);
            } else {
                int ty = row->ch->type;
                const char *pfx = (ty == CH_FORUM)    ? "\xF0\x9F\x93\x8B"  // forum
                                : (ty == CH_VOICE)    ? "\xF0\x9F\x94\x8A"  // speaker
                                : (ty == CH_ANNOUNCE) ? "\xF0\x9F\x93\xA2"  // megaphone
                                : "#";
                // Voice channels render muted (joining is gated off; see voice.h).
                unsigned ccol = (ty == CH_VOICE) ? COL_TEXT_MUTED : col;
                if (unread && ty != CH_VOICE) {
                    ui_text_bold(SIDE_X + 16, y, FS_CHANNEL, ccol, pfx);
                    ui_text_bold_clip(SIDE_X + 30, y, FS_CHANNEL, ccol, row->ch->name, SIDE_W - 66);
                } else {
                    ui_text(SIDE_X + 16, y, FS_CHANNEL, ccol, pfx);
                    ui_text_clip(SIDE_X + 30, y, FS_CHANNEL, ccol, row->ch->name, SIDE_W - 66);
                }
            }
            // red mention badge on the right
            if (mentions > 0) {
                char mb[8]; snprintf(mb, sizeof(mb), "%d", mentions > 99 ? 99 : mentions);
                int bw = ui_text_bold_w(FS_TINY, mb) + 12;
                ui_rounded_rect(SIDE_X + SIDE_W - bw - 10, y + 2, bw, 16, 8, COL_RED);
                ui_text_bold(SIDE_X + SIDE_W - bw - 10 + 6, y + 1, FS_TINY, COL_WHITE, mb);
            }
        }
        y += rowh;
    }

    ui_rect(SIDE_X, 0, SIDE_W, TOPBAR_H, COL_SIDEBAR);  // mask rows scrolled up

    // Server banner behind the header (cropped to the header height so it never
    // spills over the rows), with a dark scrim under the name for legibility.
    if (!home && g && g->banner[0] && g->id[0]) {
        char burl[256];
        const char *ext = (g->banner[0] == 'a' && g->banner[1] == '_') ? "gif" : "png";
        snprintf(burl, sizeof(burl),
                 "https://cdn.discordapp.com/banners/%s/%s.%s?size=480", g->id, g->banner, ext);
        vita2d_texture *t = imagecache_get_anim(burl, now_ms());
        if (t) {
            float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
            if (tw < 1) tw = 1; if (th < 1) th = 1;
            float s = (float)SIDE_W / tw;
            float srch = TOPBAR_H / s; if (srch > th) srch = th;  // crop to header
            vita2d_draw_texture_part_scale(t, SIDE_X, 0, 0, 0, tw, srch, s, s);
            ui_rect(SIDE_X, TOPBAR_H - 22, SIDE_W, 22, 0x99000000u);  // scrim
        }
    }

    ui_text_bold_clip(SIDE_X + 14, 14, FS_HEADER, COL_TEXT_BRIGHT,
                      home ? "Direct Messages" : (g ? g->name : "No servers"),
                      home ? SIDE_W - 92 : SIDE_W - 100);
    // Boost-tier badge in the header's bottom-right (pink pill, "Lv N").
    if (!home && g && g->premium_tier > 0) {
        char lv[12]; snprintf(lv, sizeof(lv), "Lv %d", g->premium_tier);
        int pw = 30, px = SIDE_X + SIDE_W - pw - 8, py = TOPBAR_H - 16;
        ui_rounded_rect(px, py, pw, 14, 4, 0xFFFA73FFu);  // Nitro boost pink
        ui_text(px + 5, py + 1, FS_TINY, COL_WHITE, lv);
    }
    ui_rect(SIDE_X, TOPBAR_H, SIDE_W, 1, COL_GUILDBAR);
    if (home) {  // Friends button in the header
        int fbw = 64, fbx = SIDE_X + SIDE_W - fbw - 8, fby = 11, fbh = 22;
        ui_rounded_rect(fbx, fby, fbw, fbh, 6, COL_ELEVATED);
        ui_text(fbx + 8, fby + 4, FS_TINY, COL_TEXT_BRIGHT, "Friends");
        s_friends_btn[0] = fbx; s_friends_btn[1] = fby;
        s_friends_btn[2] = fbw; s_friends_btn[3] = fbh;
    } else {
        s_friends_btn[2] = 0;  // not on Home: no hit rect
    }
    // Guild view: "Mark Read" button when the server has unread channels.
    s_markread_btn[2] = 0;
    if (!home && g) {
        int unread = 0;
        for (int i = 0; i < g->channel_count; i++) if (g->channels[i].unread) { unread = 1; break; }
        if (unread) {
            int bw = 84, bx = SIDE_X + SIDE_W - bw - 8, by = 11, bh = 22;
            ui_rounded_rect(bx, by, bw, bh, 6, COL_ELEVATED);
            ui_text(bx + 8, by + 4, FS_TINY, COL_TEXT_BRIGHT, "\xE2\x9C\x93 Mark Read");
            s_markread_btn[0] = bx; s_markread_btn[1] = by; s_markread_btn[2] = bw; s_markread_btn[3] = bh;
        }
    }

    if (home && s_sel_count == 0)
        ui_text(SIDE_X + 14, TOPBAR_H + 14, FS_SMALL, COL_TEXT_MUTED,
                "No conversations yet.");
}

// Cache of shaped (circle/squircle) RGBA icon textures: mask each source into an
// anti-aliased shape once, keyed by url+shape; rebuilt if the source re-decodes.
#define ICON_CACHE_MAX 192
typedef struct {
    char            url[256];
    int             shape;  // 0 = circle, 1 = squircle
    vita2d_texture *src;  // source it was built from (detects re-decode)
    vita2d_texture *tex;  // the masked result (we own it)
    unsigned        last_use;  // LRU clock
} icon_entry;
static icon_entry     s_iconcache[ICON_CACHE_MAX];
static unsigned       s_icon_clock;
// Deferred frees: a texture drawn this frame may still be sampled by the GPU,
// so retire evicted textures and free them a couple of frames later.
static vita2d_texture *s_icon_grave[ICON_CACHE_MAX];
static unsigned        s_icon_grave_fr[ICON_CACHE_MAX];
static int             s_icon_ngrave;
static unsigned        s_icon_frame = 2;

static void icon_retire(vita2d_texture *t)
{
    if (!t) return;
    if (s_icon_ngrave < ICON_CACHE_MAX) {
        s_icon_grave[s_icon_ngrave]    = t;
        s_icon_grave_fr[s_icon_ngrave] = s_icon_frame;
        s_icon_ngrave++;
    } else {
        vita2d_free_texture(t);  // graveyard full: last resort
    }
}

// Free retired textures whose draws have surely been presented. Call once/frame.
static void icon_gc(void)
{
    s_icon_frame++;
    int w = 0;
    for (int i = 0; i < s_icon_ngrave; i++) {
        if (s_icon_frame - s_icon_grave_fr[i] >= 2) {
            vita2d_free_texture(s_icon_grave[i]);
        } else {
            s_icon_grave[w]    = s_icon_grave[i];
            s_icon_grave_fr[w] = s_icon_grave_fr[i];
            w++;
        }
    }
    s_icon_ngrave = w;
}

// Coverage (0..1) of pixel (u,v in [0,1]) inside the inscribed circle.
static float icon_cov_circle(float u, float v)
{
    float du = (u - 0.5f) * 2.0f, dv = (v - 0.5f) * 2.0f;
    float d  = sqrtf(du * du + dv * dv);  // 0 center .. 1 at the edge midpoints
    const float e = 0.05f;  // AA ramp width (of the radius)
    if (d <= 1.0f - e) return 1.0f;
    if (d >= 1.0f)     return 0.0f;
    return (1.0f - d) / e;
}

// Coverage of (u,v) inside a rounded square with normalized corner radius cr.
static float icon_cov_squircle(float u, float v, float cr)
{
    float dx = fabsf(u - 0.5f) - (0.5f - cr);
    float dy = fabsf(v - 0.5f) - (0.5f - cr);
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    float d = sqrtf(dx * dx + dy * dy);  // 0 inside .. cr at the corner edge
    const float e = 0.03f;
    if (d <= cr - e) return 1.0f;
    if (d >= cr)     return 0.0f;
    return (cr - d) / e;
}

// Build a masked copy of `src` (RGB copied straight, alpha scaled by coverage).
static vita2d_texture *icon_build(int shape, vita2d_texture *src)
{
    int sw = (int)vita2d_texture_get_width(src);
    int sh = (int)vita2d_texture_get_height(src);
    if (sw < 2 || sh < 2) return NULL;
    vita2d_texture *d = vita2d_create_empty_texture(sw, sh);
    if (!d) return NULL;
    const unsigned char *sp = vita2d_texture_get_datap(src);
    unsigned char       *dp = vita2d_texture_get_datap(d);
    unsigned ss = vita2d_texture_get_stride(src), ds = vita2d_texture_get_stride(d);
    float cr = (shape == 1) ? 0.30f : 0.0f;  // = (half*3/5)/(2*half), the placeholder radius
    for (int y = 0; y < sh; y++) {
        const unsigned char *srow = sp + (size_t)y * ss;
        unsigned char       *drow = dp + (size_t)y * ds;
        float v = (y + 0.5f) / sh;
        for (int x = 0; x < sw; x++) {
            float u = (x + 0.5f) / sw;
            float c = shape ? icon_cov_squircle(u, v, cr) : icon_cov_circle(u, v);
            const unsigned char *s = srow + x * 4;
            unsigned char       *o = drow + x * 4;
            o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
            o[3] = (unsigned char)((int)(s[3] * c + 0.5f));
        }
    }
    return d;
}

// Shaped texture for `url` (cached; built on first use), or NULL if the source
// image isn't decoded yet. shape: 0 circle, 1 squircle.
static vita2d_texture *icon_shaped(const char *url, int shape)
{
    vita2d_texture *src = imagecache_get(url);  // keeps the source LRU-warm
    s_icon_clock++;
    for (int i = 0; i < ICON_CACHE_MAX; i++) {
        icon_entry *e = &s_iconcache[i];
        if (e->tex && e->shape == shape && strcmp(e->url, url) == 0) {
            if (src && e->src != src) {  // source re-decoded: rebuild
                vita2d_texture *nt = icon_build(shape, src);
                if (nt) { icon_retire(e->tex); e->tex = nt; e->src = src; }
            }
            e->last_use = s_icon_clock;
            return e->tex;  // keep our copy even if src NULL
        }
    }
    if (!src) return NULL;  // nothing cached, not loaded
    // Choose an empty slot, else evict the LRU.
    int slot = -1;
    for (int i = 0; i < ICON_CACHE_MAX; i++)
        if (!s_iconcache[i].tex) { slot = i; break; }
    if (slot < 0) {
        unsigned oldest = 0xffffffffu;
        for (int i = 0; i < ICON_CACHE_MAX; i++)
            if (s_iconcache[i].last_use < oldest) { oldest = s_iconcache[i].last_use; slot = i; }
        icon_retire(s_iconcache[slot].tex);
        s_iconcache[slot].tex = NULL;
    }
    vita2d_texture *nt = icon_build(shape, src);
    if (!nt) return NULL;
    icon_entry *e = &s_iconcache[slot];
    e->tex = nt; e->src = src; e->shape = shape; e->last_use = s_icon_clock;
    snprintf(e->url, sizeof(e->url), "%s", url);
    return e->tex;
}

// Draw a circular avatar from `url` (via the shaped-icon cache); until it loads,
// draw the lettered placeholder. Centered at (cx,cy), radius r.
static void draw_avatar(int cx, int cy, int r, const char *url,
                        const char *fallback_name, unsigned bg)
{
    vita2d_texture *t = (url && url[0]) ? icon_shaped(url, 0) : NULL;
    if (!t) { ui_avatar_placeholder(cx, cy, r, fallback_name, bg); return; }
    float sw = vita2d_texture_get_width(t), sh = vita2d_texture_get_height(t);
    if (sw < 1) sw = 1; if (sh < 1) sh = 1;
    vita2d_draw_texture_scale(t, cx - r, cy - r, (2.0f * r) / sw, (2.0f * r) / sh);
}

// Guild icons are squircles (rounded squares), not circles. `half` is half the
// icon's side length.
static void draw_guild_icon(int cx, int cy, int half, const char *url,
                            const char *name, unsigned bg)
{
    int cr = (half * 3) / 5;  // placeholder corner radius
    vita2d_texture *t = (url && url[0]) ? icon_shaped(url, 1) : NULL;
    if (!t) {
        ui_rounded_rect(cx - half, cy - half, 2 * half, 2 * half, cr, bg);
        char in[2] = { (name && name[0]) ? name[0] : '?', 0 };
        if (in[0] >= 'a' && in[0] <= 'z') in[0] -= 32;
        int lw = ui_text_bold_w(FS_HEADER, in);
        ui_text_bold(cx - lw / 2, cy - 12, FS_HEADER, COL_WHITE, in);
        return;
    }
    float sw = vita2d_texture_get_width(t), sh = vita2d_texture_get_height(t);
    if (sw < 1) sw = 1; if (sh < 1) sh = 1;
    vita2d_draw_texture_scale(t, cx - half, cy - half,
                              (2.0f * half) / sw, (2.0f * half) / sh);
}

// Fit an image within a sensible box while preserving aspect.
static void image_dims(const message *m, int tw, int *dw, int *dh)
{
    int w = m->img_w, h = m->img_h;
    if (w <= 0 || h <= 0) { *dw = 220; *dh = 140; return; }
    int maxw = tw < 350 ? tw : 350, maxh = 240;
    if (w > maxw) { h = (int)((long long)h * maxw / w); w = maxw; }
    if (h > maxh) { w = (int)((long long)w * maxh / h); h = maxh; }
    *dw = w < 60 ? 60 : w; *dh = h < 40 ? 40 : h;
}

// Draw a message's image attachment scaled into the layout box, or a
// placeholder card while it loads or if it's too large to decode.
static void draw_message_image(int x, int y, const message *m, int tw)
{
    int dw, dh; image_dims(m, tw, &dw, &dh);
    if (m->image_url[0] && s_nimghit < 24) {  // tap-to-zoom hit-box
        img_hit *ih = &s_imghit[s_nimghit++];
        snprintf(ih->url, sizeof(ih->url), "%s", m->image_url);
        ih->x0 = x; ih->y0 = y; ih->x1 = x + dw; ih->y1 = y + dh;
    }
    vita2d_texture *t = m->image_url[0]
                        ? imagecache_get_anim(m->image_url, now_ms()) : NULL;
    if (t) {
        float ntw = vita2d_texture_get_width(t), nth = vita2d_texture_get_height(t);
        if (ntw < 1) ntw = 1; if (nth < 1) nth = 1;
        vita2d_draw_texture_scale(t, x, y, (float)dw / ntw, (float)dh / nth);
    } else {
        ui_rounded_rect(x, y, dw, dh, 8, COL_SIDEBAR);
        const char *label = "[ Image ]";
        int lw = ui_text_w(FS_SMALL, label);
        ui_text(x + dw / 2 - lw / 2, y + dh / 2 - 8, FS_SMALL, COL_TEXT_MUTED, label);
    }
}

// Video attachment: the poster still (if the proxy gave one) with a play-button
// overlay; tapping plays it fullscreen.
static void draw_message_video(int x, int y, const message *m, int tw)
{
    int dw, dh; image_dims(m, tw, &dw, &dh);
    if (m->image_url[0]) {
        draw_message_image(x, y, m, tw);  // poster (records the zoom hit)
    } else {
        ui_rounded_rect(x, y, dw, dh, 8, RGBA8(0x18, 0x19, 0x1c, 0xFF));
    }
    if (m->video_url[0] && s_nvidhit < 8) {  // tap-to-play hit-box
        vid_hit *vh = &s_vidhit[s_nvidhit++];
        vh->x0 = x; vh->y0 = y; vh->x1 = x + dw; vh->y1 = y + dh;
        snprintf(vh->url, sizeof(vh->url), "%s", m->video_url);
    }
    int pcx = x + dw / 2, pcy = y + dh / 2;
    vita2d_draw_fill_circle(pcx, pcy, 24, RGBA8(0, 0, 0, 0xB0));
    ui_text_bold(pcx - 9, pcy - 18, FS_TITLE, COL_WHITE, "\xE2\x96\xB6");  // play
    ui_text(x + 8, y + dh - 22, FS_TINY, COL_WHITE, "Video");
}

#define INV_TILE_W 380
#define INV_TILE_H 96
// Discord-style rich invite tile: splash background, server icon, name, online/
// member counts, and a Join button (Go if already a member).
static void draw_invite_tile(int x, int y, const message *m, int tw)
{
    int w = tw < INV_TILE_W ? tw : INV_TILE_W, h = INV_TILE_H;
    ui_rounded_rect(x, y, w, h, 8, COL_SIDEBAR);
    struct inv_entry *e = inv_get(m->invite_code);
    ui_text_bold(x + 12, y + 8, FS_TINY, COL_TEXT_MUTED, "YOU'VE BEEN INVITED TO JOIN A SERVER");

    if (!e || e->state == 1) {
        ui_text(x + 12, y + 40, FS_SMALL, COL_TEXT_MUTED, "Loading invite...");
        return;
    }
    if (e->state == 3) {
        ui_text(x + 12, y + 40, FS_SMALL, COL_TEXT_MUTED, "Invite invalid or expired.");
        return;
    }
    discord_invite *iv = &e->info;
    // Splash background (dimmed) if the server has one.
    if (iv->splash[0] && iv->guild_id[0]) {
        char surl[256];
        snprintf(surl, sizeof(surl),
                 "https://cdn.discordapp.com/splashes/%s/%s.jpg?size=480", iv->guild_id, iv->splash);
        vita2d_texture *t = imagecache_get(surl);
        if (t) {
            float ntw = vita2d_texture_get_width(t), nth = vita2d_texture_get_height(t);
            if (ntw < 1) ntw = 1; if (nth < 1) nth = 1;
            float s = (float)w / ntw; int dhh = (int)(nth * s);
            vita2d_draw_texture_scale(t, x, y, s, s);
            if (dhh > h) ui_rect(x, y + h, w, dhh - h, COL_CHAT);  // mask spill
            ui_rect(x, y, w, h, RGBA8(0x0e, 0x0f, 0x11, 0xC0));  // dim overlay
            ui_rounded_rect(x, y, w, 18, 8, COL_SIDEBAR);  // re-round top
            ui_text_bold(x + 12, y + 8, FS_TINY, COL_TEXT_MUTED, "YOU'VE BEEN INVITED TO JOIN A SERVER");
        }
    }
    // Server icon.
    char iurl[256] = "";
    if (iv->icon[0] && iv->guild_id[0])
        snprintf(iurl, sizeof(iurl),
                 "https://cdn.discordapp.com/icons/%s/%s.png?size=64", iv->guild_id, iv->icon);
    draw_guild_icon(x + 34, y + 54, 22, iurl, iv->name, COL_BLURPLE);
    // Name + counts.
    ui_text_bold_clip(x + 66, y + 34, FS_BODY, COL_TEXT_BRIGHT, iv->name, w - 66 - 96);
    char counts[64];
    snprintf(counts, sizeof(counts), "\xE2\x97\x8F %d Online   \xE2\x97\x8B %d Members",
             iv->online, iv->members);
    ui_text(x + 66, y + 58, FS_TINY, COL_TEXT_MUTED, counts);
    // Join / Go button.
    int joined = store_find_guild(iv->guild_id) != NULL;
    int bw = 74, bx = x + w - bw - 12, by = y + h / 2 - 14, bh = 28;
    ui_rounded_rect(bx, by, bw, bh, 6, joined ? COL_ELEVATED : COL_GREEN);
    const char *bl = joined ? "Joined" : "Join";
    ui_text_bold(bx + bw / 2 - ui_text_bold_w(FS_SMALL, bl) / 2, by + 6, FS_SMALL, COL_WHITE, bl);
    if (s_ninvhit < 16) {
        inv_hit *ih = &s_invhit[s_ninvhit++];
        ih->x0 = bx; ih->y0 = by; ih->x1 = bx + bw; ih->y1 = by + bh;
        snprintf(ih->code, sizeof(ih->code), "%s", m->invite_code);
        snprintf(ih->guild_id, sizeof(ih->guild_id), "%s", iv->guild_id);
        ih->joined = joined;
    }
}

static void human_size(long long b, char *out, size_t cap)
{
    if (b >= (1 << 20))      snprintf(out, cap, "%.1f MB", b / 1048576.0);
    else if (b >= (1 << 10)) snprintf(out, cap, "%.1f KB", b / 1024.0);
    else                     snprintf(out, cap, "%lld B", b);
}

// A non-image attachment shown as a Discord-style file card.
static void draw_message_file(int x, int y, const message *m, int tw)
{
    int cw = tw < 340 ? tw : 340, ch = 52;
    ui_rounded_rect(x, y, cw, ch, 8, COL_SIDEBAR);
    ui_rounded_rect(x + 10, y + 10, 32, 32, 6, COL_BLURPLE);  // file icon
    ui_text_bold(x + 20, y + 16, FS_SMALL, COL_WHITE, "\xE2\xAC\x87");  // down arrow
    char sz[24]; human_size(m->file_size, sz, sizeof(sz));
    ui_text_clip(x + 52, y + 8, FS_SMALL, COL_TEXT_LINK, m->file_name, cw - 64);
    ui_text(x + 52, y + 28, FS_TINY, COL_TEXT_MUTED, sz);
}

#define EMBED_MAXW 400

// Lay out (and optionally draw) an embed's inner content at (x,y) within
// `innerw`. Returns the content height.
static int embed_content(int x, int y, const message *m, int innerw, bool draw)
{
    int cy = y;
    if (m->embed_author[0]) {
        if (draw) ui_text_clip(x, cy, FS_SMALL, COL_TEXT_MUTED, m->embed_author, innerw);
        cy += 18;
    }
    if (m->embed_title[0]) {
        if (draw) ui_richtext_draw(x, cy, FS_BODY, COL_TEXT_LINK, m->embed_title, innerw, 20);
        cy += ui_richtext_height(FS_BODY, m->embed_title, innerw, 20) + 3;
    }
    if (m->embed_desc[0]) {
        if (draw) ui_richtext_draw(x, cy, FS_SMALL, COL_TEXT, m->embed_desc, innerw, 18);
        cy += ui_richtext_height(FS_SMALL, m->embed_desc, innerw, 18) + 4;
    }
    // Fields (name/value). Consecutive inline fields pack two per row; a non-
    // inline field takes the full width. Measure and draw share this walk.
    for (int i = 0; i < m->embed_field_count; ) {
        int two = m->embed_fields[i].inline_ && i + 1 < m->embed_field_count &&
                  m->embed_fields[i + 1].inline_;
        int colw = two ? (innerw - 10) / 2 : innerw;
        int rowh = 0;
        for (int c = 0; c <= (two ? 1 : 0); c++) {
            const char *nm = m->embed_fields[i + c].name;
            const char *vl = m->embed_fields[i + c].value;
            int fx = x + (c ? colw + 10 : 0), fy = cy, fh = 0;
            if (nm[0]) {
                if (draw) ui_text_bold_clip(fx, fy, FS_TINY, COL_TEXT_BRIGHT, nm, colw);
                fy += 15; fh += 15;
            }
            if (vl[0]) {
                if (draw) ui_richtext_draw(fx, fy, FS_TINY, COL_TEXT, vl, colw, 15);
                int vh = ui_richtext_height(FS_TINY, vl, colw, 15);
                fy += vh; fh += vh;
            }
            if (fh > rowh) rowh = fh;
        }
        cy += rowh + 6;
        i += two ? 2 : 1;
    }
    if (m->has_image) {
        if (draw) draw_message_image(x, cy, m, innerw);
        int dw, dh; image_dims(m, innerw, &dw, &dh);
        cy += dh + 4;
    }
    return cy - y;
}

static int embed_height(const message *m, int tw)
{
    if (!m->has_embed) return 0;
    int cw = tw < EMBED_MAXW ? tw : EMBED_MAXW;
    return 10 + embed_content(0, 0, m, cw - 26, false) + 10;
}

// Discord-style embed card: accent bar + panel behind author/title/desc/image.
static void draw_embed(int x, int y, const message *m, int tw)
{
    if (!m->has_embed) return;
    int cw = tw < EMBED_MAXW ? tw : EMBED_MAXW;
    int h  = embed_height(m, tw);
    unsigned bar = m->embed_color
        ? RGBA8((m->embed_color >> 16) & 0xFF, (m->embed_color >> 8) & 0xFF,
                m->embed_color & 0xFF, 0xFF)
        : COL_TEXT_MUTED;  // neutral accent bar, adapts to theme
    ui_rounded_rect(x, y, cw, h, 6, COL_SIDEBAR);
    ui_rect(x, y + 2, 4, h - 4, bar);  // accent bar
    embed_content(x + 14, y + 10, m, cw - 26, true);
}

#define POLL_MAXW  380
#define POLL_ROW_H 30
static int poll_height(const message *m, int tw)
{
    if (!m->has_poll) return 0;
    int cw = tw < POLL_MAXW ? tw : POLL_MAXW;
    int h = 10 + ui_richtext_height(FS_BODY, m->poll_question, cw - 24, 20) + 8;
    h += m->poll_answer_count * (POLL_ROW_H + 6);
    return h + 18;  // footer + padding
}

// Discord-style poll: question + answer bars (share of the vote) + totals.
static void draw_poll(int x, int y, const message *m, int tw)
{
    if (!m->has_poll) return;
    int cw = tw < POLL_MAXW ? tw : POLL_MAXW;
    int h  = poll_height(m, tw);
    ui_rounded_rect(x, y, cw, h, 8, COL_SIDEBAR);
    int ix = x + 12, iy = y + 10, iw = cw - 24;
    ui_richtext_draw(ix, iy, FS_BODY, COL_TEXT_BRIGHT, m->poll_question, iw, 20);
    iy += ui_richtext_height(FS_BODY, m->poll_question, iw, 20) + 8;
    int total = m->poll_total > 0 ? m->poll_total : 1;
    for (int i = 0; i < m->poll_answer_count; i++) {
        int cnt = m->poll_answers[i].count;
        int barw = iw * cnt / total;
        int pct  = m->poll_total > 0 ? cnt * 100 / total : 0;
        ui_rounded_rect(ix, iy, iw, POLL_ROW_H, 6, COL_ELEVATED);
        if (barw > 6)
            ui_rounded_rect(ix, iy, barw, POLL_ROW_H, 6,
                            m->poll_answers[i].me ? COL_BLURPLE : COL_SELECTED);
        char pc[8]; snprintf(pc, sizeof(pc), "%d%%", pct);
        ui_text(ix + iw - ui_text_w(FS_SMALL, pc) - 10, iy + 7, FS_SMALL, COL_TEXT_BRIGHT, pc);
        ui_text_clip(ix + 10, iy + 7, FS_SMALL, COL_TEXT,
                     m->poll_answers[i].text, iw - 60);
        if (!m->poll_finalized && s_nphit < 24) {  // tap-to-vote hit-box
            poll_hit *ph = &s_phit[s_nphit++];
            snprintf(ph->mid, sizeof(ph->mid), "%s", m->id);
            ph->aid = m->poll_answers[i].id;
            ph->x0 = ix; ph->y0 = iy; ph->x1 = ix + iw; ph->y1 = iy + POLL_ROW_H;
        }
        iy += POLL_ROW_H + 6;
    }
    char ft[48];
    snprintf(ft, sizeof(ft), "%d vote%s%s", m->poll_total, m->poll_total == 1 ? "" : "s",
             m->poll_finalized ? " \xE2\x80\xA2 final" : "");
    ui_text(ix, iy, FS_TINY, COL_TEXT_MUTED, ft);
}

#define RX_H     24
#define RX_GAP    6
#define RX_EMOJI  16

static int reaction_pill_w(const reaction *r)
{
    char cnt[16]; snprintf(cnt, sizeof(cnt), "%d", r->count);
    return 8 + RX_EMOJI + 5 + ui_text_w(FS_SMALL, cnt) + 8;
}

// Lay out (and optionally draw) the reaction pills for `m` within width `tw`,
// wrapping to new rows like Discord. Returns the total height consumed.
static int draw_reactions(int x, int y, const message *m, int tw, bool draw)
{
    if (m->reaction_count <= 0) return 0;
    int cx = x, cy = y, rows = 1;
    for (int i = 0; i < m->reaction_count; i++) {
        const reaction *r = &m->reactions[i];
        int pw = reaction_pill_w(r);
        if (cx + pw > x + tw && cx > x) { cx = x; cy += RX_H + RX_GAP; rows++; }
        if (draw) {
            unsigned bg = r->me ? COL_SELECTED : COL_ELEVATED;  // themed so light reads
            ui_rounded_rect(cx, cy, pw, RX_H, 8, bg);
            if (r->me) {  // blurple outline when you reacted
                ui_rect(cx, cy, pw, 1, COL_BLURPLE);
                ui_rect(cx, cy + RX_H - 1, pw, 1, COL_BLURPLE);
                ui_rect(cx, cy, 1, RX_H, COL_BLURPLE);
                ui_rect(cx + pw - 1, cy, 1, RX_H, COL_BLURPLE);
            }
            int ex = cx + 8, ey = cy + (RX_H - RX_EMOJI) / 2;
            if (r->emoji_id[0]) {
                // custom emoji: Discord CDN sprite
                char url[128];
                snprintf(url, sizeof(url),
                    "https://cdn.discordapp.com/emojis/%s.png?size=32", r->emoji_id);
                vita2d_texture *t = imagecache_get(url);
                if (t) {
                    float twd = vita2d_texture_get_width(t);
                    float thd = vita2d_texture_get_height(t);
                    if (twd < 1) twd = 1; if (thd < 1) thd = 1;
                    vita2d_draw_texture_part_scale(t, ex, ey, 0, 0, twd, thd,
                                                   (float)RX_EMOJI / twd,
                                                   (float)RX_EMOJI / thd);
                }
            } else {  // unicode: bundled atlas
                ui_draw_emoji(r->name, ex, ey, RX_EMOJI);
            }
            char cnt[16]; snprintf(cnt, sizeof(cnt), "%d", r->count);
            ui_text(ex + RX_EMOJI + 5, cy + 4, FS_SMALL,
                    r->me ? COL_TEXT_BRIGHT : COL_TEXT_MUTED, cnt);
            if (s_nrhit < 40) {  // tap-to-toggle hit-box
                react_hit *rh = &s_rhit[s_nrhit++];
                snprintf(rh->mid, sizeof(rh->mid), "%s", m->id);
                if (r->emoji_id[0]) snprintf(rh->emoji, sizeof(rh->emoji), "%s:%s", r->name, r->emoji_id);
                else                snprintf(rh->emoji, sizeof(rh->emoji), "%s", r->name);
                rh->me = r->me;
                rh->x0 = cx; rh->y0 = cy; rh->x1 = cx + pw; rh->y1 = cy + RX_H;
            }
        }
        cx += pw + RX_GAP;
    }
    return rows * RX_H + (rows - 1) * RX_GAP;
}

// Presence status dot with a background ring. status: 0 off,1 on,2 idle,3 dnd;
// <0 draws nothing (unknown).
static void draw_status_dot(int cx, int cy, int status, unsigned ring)
{
    if (status < 0) return;
    unsigned dot = status == 0 ? RGBA8(0x80,0x84,0x8e,0xFF)
                 : status == 2 ? RGBA8(0xf0,0xb2,0x32,0xFF)
                 : status == 3 ? COL_RED : COL_GREEN;
    vita2d_draw_fill_circle(cx, cy, 5, ring);
    vita2d_draw_fill_circle(cx, cy, 3, dot);
}

// ISO8601 UTC time -> "HH:MM" (24h) or "h:MM AM/PM" (12h). out needs >= 12 bytes.
static void hhmm(const char *ts, char *out)
{
    out[0] = '\0';
    if (!ts || strlen(ts) < 16 || ts[10] != 'T') return;
    int h = (ts[11]-'0')*10 + (ts[12]-'0');
    int m = (ts[14]-'0')*10 + (ts[15]-'0');
    if (h < 0 || h > 23 || m < 0 || m > 59) return;
    if (g_settings.clock_24h) {
        snprintf(out, 12, "%02d:%02d", h, m);
    } else {
        int h12 = h % 12; if (h12 == 0) h12 = 12;
        snprintf(out, 12, "%d:%02d %s", h12, m, h < 12 ? "AM" : "PM");
    }
}

// True if two ISO8601 timestamps fall on the same calendar day (compares the
// "YYYY-MM-DD" prefix). A missing/short timestamp never forces a divider.
static bool same_day(const char *a, const char *b)
{
    if (!a || !b || strlen(a) < 10 || strlen(b) < 10) return true;
    return strncmp(a, b, 10) == 0;
}

// "2026-08-09T..." -> "August 9, 2026" (the date-divider label, Discord-style).
// Days since a fixed epoch for a civil date (Howard Hinnant's algorithm).
static long civil_days(int y, int mo, int d)
{
    y -= mo <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static void date_label(const char *ts, char *out, size_t n)
{
    static const char *mon[12] = { "January","February","March","April","May",
        "June","July","August","September","October","November","December" };
    out[0] = '\0';
    if (!ts || strlen(ts) < 10 || ts[4] != '-' || ts[7] != '-') return;
    int yr = (ts[0]-'0')*1000 + (ts[1]-'0')*100 + (ts[2]-'0')*10 + (ts[3]-'0');
    int mo = (ts[5]-'0')*10 + (ts[6]-'0');
    int da = (ts[8]-'0')*10 + (ts[9]-'0');
    if (mo < 1 || mo > 12) return;
    // "Today" / "Yesterday" relative to the console's UTC date.
    SceDateTime now; sceRtcGetCurrentClock(&now, 0);
    long diff = civil_days(now.year, now.month, now.day) - civil_days(yr, mo, da);
    if (diff == 0)      snprintf(out, n, "Today");
    else if (diff == 1) snprintf(out, n, "Yesterday");
    else                snprintf(out, n, "%s %d, %d", mon[mo-1], da, yr);
}

#define DIVIDER_H 26
#define UNREAD_H  20

// A date separator: a hairline across the pane broken by the centered label.
static void draw_date_divider(int x, int y, int w, const char *label)
{
    int midy = y + DIVIDER_H/2;
    ui_rect(x + 12, midy, w - 24, 1, COL_DIVIDER);
    int lw = ui_text_w(FS_TINY, label);
    int lx = x + (w - lw)/2;
    ui_rect(lx - 8, y + 2, lw + 16, DIVIDER_H - 4, COL_CHAT);  // break the line
    ui_text(lx, midy - 8, FS_TINY, COL_TEXT_MUTED, label);
}

// The unread marker: a red hairline across the pane with a "NEW" tag on the
// right, drawn just above the first message the user hasn't read yet.
static void draw_unread_divider(int x, int y, int w)
{
    unsigned red = COL_RED;
    int midy = y + UNREAD_H/2;
    ui_rect(x + 12, midy, w - 24, 1, red);
    const char *nw = "NEW";
    int lw = ui_text_w(FS_TINY, nw);
    int bx = x + w - lw - 20;
    ui_rect(bx - 5, y + 1, lw + 10, UNREAD_H - 2, red);  // tag chip
    ui_text(bx, midy - 8, FS_TINY, COL_WHITE, nw);
}

// DM/Home right panel: friends with live presence (from the global presence
// map), online first then offline. Reuses s_member_scroll/s_member_max.
static void draw_friends_panel(void)
{
    int x0 = SCREEN_W - MEMBER_W;
    ui_rect(x0, 0, MEMBER_W, SCREEN_H, COL_SIDEBAR);

    int fc = 0;
    store_lock();
    member_row *fr = store_friends(&fc);
    int online = 0;
    for (int i = 0; i < fc; i++) if (store_member_status(fr[i].id) > 0) online++;

    int top = TOPBAR_H + 8, bot = SCREEN_H - 4;
    int total = fc * 28;
    s_member_max = total > (bot - top) ? total - (bot - top) : 0;
    if (s_member_scroll > s_member_max) s_member_scroll = s_member_max;
    if (s_member_scroll < 0) s_member_scroll = 0;
    int y = top - s_member_scroll;
    for (int pass = 0; pass < 2; pass++) {  // 0 = online, 1 = offline
        for (int i = 0; i < fc; i++) {
            int st = store_member_status(fr[i].id);
            bool offline = (st <= 0);
            if ((pass == 0) == offline) continue;  // pass 0 skips offline; pass 1 skips online
            member_row *r = &fr[i];
            if (y + 28 > TOPBAR_H && y < SCREEN_H) {
                char url[256] = "";
                if (r->avatar[0] && r->id[0])
                    snprintf(url, sizeof(url),
                             "https://cdn.discordapp.com/avatars/%s/%s.png?size=64",
                             r->id, r->avatar);
                draw_avatar(x0 + 20, y + 12, 12, url, r->text, COL_BLURPLE);
                draw_status_dot(x0 + 29, y + 21, offline ? 0 : st, COL_SIDEBAR);
                ui_text_clip(x0 + 40, y + 4, FS_SMALL,
                             offline ? COL_TEXT_MUTED : COL_TEXT, r->text, MEMBER_W - 50);
            }
            y += 28;
        }
    }
    store_unlock();

    if (fc == 0)
        ui_text(x0 + 14, TOPBAR_H + 14, FS_SMALL, COL_TEXT_MUTED, "No friends");

    // Header on top (masks rows scrolled up into it).
    ui_rect(x0, 0, MEMBER_W, TOPBAR_H, COL_SIDEBAR);
    char hdr[40]; snprintf(hdr, sizeof(hdr), "FRIENDS \xE2\x80\x94 %d online", online);
    ui_text_bold(x0 + 14, 14, FS_TINY, COL_TEXT_MUTED, fc ? hdr : "FRIENDS");
    ui_rect(x0, TOPBAR_H, MEMBER_W, 1, COL_GUILDBAR);
}

// Right-side member list (guild view only).
static void draw_members(void)
{
    int x0 = SCREEN_W - MEMBER_W;
    ui_rect(x0, 0, MEMBER_W, SCREEN_H, COL_SIDEBAR);

    int mc = 0;
    store_lock();
    member_row *rows = store_members(&mc);
    // If the loaded list is for a different guild than the one being viewed (a
    // lagged member request), treat it as still loading, not the old server's.
    bool stale = strcmp(store_members_guild(), s_cur_guild_id) != 0;
    if (stale) mc = 0;
    // Large/restricted servers never return a member list (Discord ignores op14).
    // Time the wait to show a message instead of a forever "loading".
    static char s_ml_g[24]; static long long s_ml_since;
    if (strcmp(s_ml_g, s_cur_guild_id) != 0) {
        snprintf(s_ml_g, sizeof(s_ml_g), "%s", s_cur_guild_id);
        s_ml_since = now_ms();
    }
    bool ml_timeout = stale && (now_ms() - s_ml_since > 7000);
    int members = 0;
    for (int i = 0; i < mc; i++) if (!rows[i].is_group) members++;

    // Scrollable: clamp, then draw rows offset; the header masks the top.
    int top = TOPBAR_H + 8, bot = SCREEN_H - 4;
    int total = mc * 28;
    s_member_max = total > (bot - top) ? total - (bot - top) : 0;
    if (s_member_scroll > s_member_max) s_member_scroll = s_member_max;
    if (s_member_scroll < 0) s_member_scroll = 0;
    int y = top - s_member_scroll;
    for (int i = 0; i < mc; i++) {
        member_row *r = &rows[i];
        if (y + 28 > TOPBAR_H && y < SCREEN_H) {
            if (r->is_group) {
                char gh[80];
                snprintf(gh, sizeof(gh), "%s \xE2\x80\x94 %d", r->text, r->count);
                ui_text_bold(x0 + 14, y + 8, FS_TINY, COL_TEXT_MUTED, gh);
            } else {
                bool offline = (r->status == 0);
                char url[256] = "";
                if (r->avatar[0] && r->id[0])
                    snprintf(url, sizeof(url),
                             "https://cdn.discordapp.com/avatars/%s/%s.png?size=64",
                             r->id, r->avatar);
                draw_avatar(x0 + 20, y + 12, 12, url, r->text, COL_BLURPLE);
                draw_status_dot(x0 + 29, y + 21, r->status, COL_SIDEBAR);
                ui_text_clip(x0 + 40, y + 4, FS_SMALL,
                             offline ? COL_TEXT_MUTED : COL_TEXT, r->text, MEMBER_W - 50);
            }
        }
        y += 28;
    }
    store_unlock();

    if (ml_timeout) {  // two lines -- the panel is too narrow for one
        ui_text(x0 + 14, TOPBAR_H + 14, FS_SMALL, COL_TEXT_MUTED, "Couldn't load");
        ui_text(x0 + 14, TOPBAR_H + 30, FS_SMALL, COL_TEXT_MUTED, "member list.");
    } else if (stale || mc == 0) {
        ui_text(x0 + 14, TOPBAR_H + 14, FS_SMALL, COL_TEXT_MUTED, "Loading members\xE2\x80\xA6");
    }

    // Header on top (masks rows scrolled up into it).
    ui_rect(x0, 0, MEMBER_W, TOPBAR_H, COL_SIDEBAR);
    char hdr[32]; snprintf(hdr, sizeof(hdr), "MEMBERS \xE2\x80\x94 %d", members);
    ui_text_bold(x0 + 14, 14, FS_TINY, COL_TEXT_MUTED, mc ? hdr : "MEMBERS");
    ui_rect(x0, TOPBAR_H, MEMBER_W, 1, COL_GUILDBAR);
}

// The channel top bar, drawn after the messages so it sits on top of anything
// scrolled up into it (GXM region-clipping crashes Vita3K; mask + overdraw).
static void draw_topbar(const channel *cc, int msg_w)
{
    ui_rect(g_msg_x, 0, msg_w, TOPBAR_H, COL_TOPBAR);
    ui_rect(g_msg_x, TOPBAR_H, msg_w, 1, COL_GUILDBAR);
    const channel *c = s_have_open ? &s_open_ch : cc;  // the actual open view
    s_back_r[2] = 0;
    if (c && c->name[0]) {
        // Right-side header buttons, laid out right-to-left.
        int rx = g_msg_x + msg_w;

        // Right-panel toggle arrow (far right): shows/hides the member list
        // (server) or friends list (DM/home). Mirrors the R button.
        {
            int ax = rx - 26;
            const char *gl = s_members_collapsed ? "\xE2\x80\xB9" : "\xE2\x80\xBA";  // chevron
            ui_text_bold(ax + 7, 11, FS_HEADER, COL_TEXT_MUTED, gl);
            s_memtoggle_r[0] = ax; s_memtoggle_r[1] = 0;
            s_memtoggle_r[2] = 26; s_memtoggle_r[3] = TOPBAR_H;
            rx = ax;
        }
        // Pins
        int pw = ui_text_bold_w(FS_SMALL, "Pins") + 20;
        int px = rx - pw - 8;
        ui_rounded_rect(px, 9, pw, 24, 8, COL_ELEVATED);
        ui_text_bold(px + 10, 14, FS_SMALL, COL_TEXT_BRIGHT, "Pins");
        s_pin_btn_x0 = px; s_pin_btn_x1 = px + pw;
        rx = px;
        // Search (guild view).
        s_search_btn_x0 = s_search_btn_x1 = -1;
        if (s_guild >= 0) {
            int sw = ui_text_bold_w(FS_SMALL, "Search") + 20;
            int sx = rx - sw - 8;
            ui_rounded_rect(sx, 9, sw, 24, 8, COL_ELEVATED);
            ui_text_bold(sx + 10, 14, FS_SMALL, COL_TEXT_BRIGHT, "Search");
            s_search_btn_x0 = sx; s_search_btn_x1 = sx + sw;
            rx = sx;
        }
        // Threads (text/announce).
        s_thr_btn_x0 = s_thr_btn_x1 = -1;
        if (c->type == CH_TEXT || c->type == CH_ANNOUNCE) {
            int tw2 = ui_text_bold_w(FS_SMALL, "Threads") + 20;
            int tx2 = rx - tw2 - 8;
            ui_rounded_rect(tx2, 9, tw2, 24, 8, COL_ELEVATED);
            ui_text_bold(tx2 + 10, 14, FS_SMALL, COL_TEXT_BRIGHT, "Threads");
            s_thr_btn_x0 = tx2; s_thr_btn_x1 = tx2 + tw2;
            rx = tx2;
        }

        // Channel-column collapse arrow (far left): toggles the channel drawer,
        // mirroring the member arrow on the right.
        const char *cg = s_channels_collapsed ? "\xE2\x80\xBA" : "\xE2\x80\xB9";  // chevron
        ui_text_bold(g_msg_x + 8, 11, FS_HEADER, COL_TEXT_MUTED, cg);
        s_chtoggle_r[0] = g_msg_x; s_chtoggle_r[1] = 0; s_chtoggle_r[2] = 26; s_chtoggle_r[3] = TOPBAR_H;

        int base = g_msg_x + 26;  // content after the collapse arrow
        int namex = base + 22;
        if (ch_is_thread(c->type)) {  // back arrow into the thread's parent
            ui_text_bold(base + 4, 11, FS_HEADER, COL_TEXT_BRIGHT, "\xE2\x80\xB9");
            s_back_r[0] = base; s_back_r[1] = 0; s_back_r[2] = 34; s_back_r[3] = TOPBAR_H;
            namex = base + 32;
        } else {
            bool dm = (c->type == CH_DM || c->type == CH_GROUP_DM);
            if (dm) namex = base + 6;  // DMs: just the name, no "@" prefix
            else    ui_text(base + 6, 14, FS_HEADER, COL_TEXT_MUTED, "#");
        }
        ui_text_bold_clip(namex, 14, FS_HEADER, COL_TEXT_BRIGHT, c->name, rx - namex - 8);
        // Channel topic after the name (single line, clipped), like Discord.
        if (c->topic[0]) {
            int namew = ui_text_bold_w(FS_HEADER, c->name);
            int avail = rx - namex - 8; if (namew > avail) namew = avail;
            int tx = namex + namew + 14, troom = rx - tx - 8;
            if (troom > 40) {
                char tp[256]; int k = 0;
                for (const char *p = c->topic; *p && k < 255; p++)
                    tp[k++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
                tp[k] = '\0';
                ui_rect(tx - 8, 16, 1, TOPBAR_H - 32, COL_DIVIDER);
                ui_text_clip(tx, 15, FS_SMALL, COL_TEXT_MUTED, tp, troom);
            }
        }
    } else {
        ui_text(g_msg_x + 16, 14, FS_HEADER, COL_TEXT_MUTED, "Select a channel");
        s_pin_btn_x0 = s_pin_btn_x1 = -1;
    }
}

// A forum channel's post list (each post is a thread).
static void draw_forum(int msg_w)
{
    int pane_top = TOPBAR_H + 12, pane_bot = SCREEN_H - 12;
    ui_text_bold(g_msg_x + 16, pane_top, FS_TINY, COL_TEXT_MUTED, "POSTS");
    if (s_forum_status[0])
        ui_text(g_msg_x + 70, pane_top, FS_TINY, COL_TEXT_MUTED, s_forum_status);
    // "+ New Post" button (top-right).
    {
        int bw = ui_text_bold_w(FS_SMALL, "+ New Post") + 20;
        int bx = g_msg_x + msg_w - bw - 16, by = pane_top - 6;
        ui_rounded_rect(bx, by, bw, 26, 8, COL_BLURPLE);
        ui_text_bold(bx + 10, by + 5, FS_SMALL, COL_WHITE, "+ New Post");
        s_forum_new_x0 = bx; s_forum_new_x1 = bx + bw;
        s_forum_new_y0 = by; s_forum_new_y1 = by + 26;
    }
    if (s_forum_loading && s_forum_n == 0) {
        ui_text(g_msg_x + 16, pane_top + 22, FS_SMALL, COL_TEXT_MUTED, "Loading posts...");
        return;
    }
    if (s_forum_n == 0) {
        ui_text(g_msg_x + 16, pane_top + 22, FS_SMALL, COL_TEXT_MUTED, "No active posts.");
        return;
    }
    int cardh = 54;
    int y = pane_top + 22 - s_forum_scroll;
    for (int i = 0; i < s_forum_n; i++) {
        s_forum_hit_y0[i] = y; s_forum_hit_y1[i] = y + cardh - 8;
        if (y + cardh > pane_top && y < pane_bot) {
            ui_rounded_rect(g_msg_x + 12, y, msg_w - 24, cardh - 8, 8, COL_SIDEBAR);
            ui_text_bold_clip(g_msg_x + 24, y + 8, FS_BODY, COL_TEXT_BRIGHT,
                              s_forum[i].name, msg_w - 48);
            ui_text(g_msg_x + 24, y + 28, FS_TINY, COL_TEXT_MUTED,
                    "Post \xE2\x80\xA2 tap to open");
        }
        y += cardh;
    }
}

static void draw_messages(void)
{
    int msg_right = SCREEN_W - s_member_w;  // right edge (member panel takes the rest)
    int msg_w = msg_right - g_msg_x;
    ui_rect(g_msg_x, 0, msg_w, SCREEN_H, COL_CHAT);
    s_jump_show = false;  // off unless the normal path re-enables it

    const channel *c = selected_channel();

    // Forum channels show a scrollable list of post threads, not a message stream.
    if (s_forum_view) {
        draw_forum(msg_w);
        ui_rect(g_msg_x, 0, msg_w, TOPBAR_H + 8, COL_CHAT);  // mask above topbar
        draw_topbar(c, msg_w);
        return;
    }

    int pane_top = TOPBAR_H + 8;
    int pane_bot = SCREEN_H - 52;  // leave room for the compose bar
    int pane_h = pane_bot - pane_top;
    // Density: compact hides the avatar (author drawn inline) and tightens spacing.
    int compact = (g_settings.density == 1);
    int line_h  = compact ? 18 : 22;   // message body line spacing
    int hdr_h   = compact ? 18 : 22;   // author line -> first content line
    int blk_pad = compact ? 3  : 8;    // trailing pad under a header block
    int grp_pad = compact ? 2  : 4;    // trailing pad under a grouped message
    int txt_x = g_msg_x + (compact ? 16 : 64);  // text column (after avatar)
    int tw = msg_right - txt_x - 16;

    store_lock();
    if (store_msg_loading()) {
        ui_text(g_msg_x + 16, pane_top + 8, FS_BODY, COL_TEXT_MUTED, "Loading messages...");
        store_unlock();
        draw_topbar(c, msg_w);
        return;
    }
    int mc = 0;
    message *msgs = store_messages(&mc);

    // A compact "system" line is a member-join (type 7) or a text-less message
    // (embed/attachment-only). Consecutive same-author normals share one header.
    #define MSG_IS_SYSTEM(m) ((m).type == 7 || \
        ((m).content[0] == '\0' && !(m).has_image && !(m).has_file && \
         !(m).has_embed && !(m).has_poll && !(m).has_video && !(m).has_invite))

    // Block heights cached; recomputed only when the message set changes.
    static int      s_bh[MAX_MESSAGES];
    static int      s_texth[MAX_MESSAGES];  // cached wrapped-text height
    static bool     s_div[MAX_MESSAGES];  // a date divider precedes message i
    static bool     s_udiv[MAX_MESSAGES];  // the NEW/unread divider precedes message i
    static unsigned s_bh_rev = 0xFFFFFFFFu;
    static int      s_bh_tw = -1;
    static int      s_bh_fs = -1;   // font size baked into the cached heights
    unsigned rev = store_msg_rev();
    bool changed = (rev != s_bh_rev) || (tw != s_bh_tw) || (FS_BODY != s_bh_fs);
    if (changed) {
        s_bh_tw = tw; s_bh_fs = FS_BODY;
        const char *pa = "";
        for (int i = 0; i < mc; i++) {
            bool blk = store_is_blocked(msgs[i].author_id);
            bool sys = MSG_IS_SYSTEM(msgs[i]) || blk;  // blocked -> compact line
            bool grp = (!sys && i > 0 && strcmp(msgs[i].author, pa) == 0
                        && !msgs[i].is_reply);
            // Date divider before the first message of each new calendar day.
            // A new day always starts a fresh header (never grouped).
            s_div[i] = (i == 0) || !same_day(msgs[i].timestamp, msgs[i-1].timestamp);
            if (s_div[i]) grp = false;
            // NEW divider before the first message newer than the last-read id.
            s_udiv[i] = s_unread_boundary[0] &&
                        snowflake_gt(msgs[i].id, s_unread_boundary) &&
                        (i == 0 || !snowflake_gt(msgs[i-1].id, s_unread_boundary));
            if (s_udiv[i]) grp = false;
            int wrap = msgs[i].content[0]
                       ? ui_richtext_height(FS_BODY, msgs[i].content, tw, line_h) : 0;
            s_texth[i] = wrap;  // reused by the draw pass
            int imgh = 0;
            if (msgs[i].has_embed) imgh += embed_height(&msgs[i], tw) + 8;
            else if (msgs[i].has_image || msgs[i].has_video) {
                int dw, dh; image_dims(&msgs[i], tw, &dw, &dh); imgh = dh + 8; }
            if (msgs[i].has_file) imgh += 60;
            if (msgs[i].has_invite) imgh += INV_TILE_H + 8;
            if (msgs[i].has_poll) imgh += poll_height(&msgs[i], tw) + 8;
            if (msgs[i].reaction_count)
                imgh += draw_reactions(0, 0, &msgs[i], tw, false) + 8;
            if (sys) s_bh[i] = compact ? 22 : 28;
            else if (grp) s_bh[i] = wrap + imgh + grp_pad;
            else s_bh[i] = (msgs[i].is_reply ? 20 : 0) + hdr_h + wrap + imgh + blk_pad;
            if (s_div[i]) s_bh[i] += DIVIDER_H;
            if (s_udiv[i]) s_bh[i] += UNREAD_H;
            pa = sys ? "" : msgs[i].author;
        }
        s_bh_rev = rev;
    }

    int total = 0;
    for (int i = 0; i < mc; i++) total += s_bh[i];

    // After older history prepends, keep the previous top message where it was
    // (offset the scroll by the height of the newly-inserted messages).
    bool restored = false;
    if (s_restore_anchor[0]) {
        int above = 0;
        for (int i = 0; i < mc; i++) {
            if (strcmp(msgs[i].id, s_restore_anchor) == 0) {
                s_msg_scroll = above; restored = true; break;
            }
            above += s_bh[i];
        }
        s_restore_anchor[0] = '\0';
    }

    int max_scroll = total > pane_h ? total - pane_h : 0;
    bool was_bottom = !restored && (s_msg_scroll >= s_prev_max - 2);
    // Focus the unread boundary once the messages that contain it have loaded.
    if (s_want_unread) {
        int above = 0, found = 0;
        for (int i = 0; i < mc; i++) {
            if (s_udiv[i]) { s_msg_scroll = above - 40; found = 1; break; }
            above += s_bh[i];
        }
        // Pin the boundary every frame (not one-shot); async height changes and
        // prepends would otherwise drift it out of view. Cleared when the user scrolls.
        if (found) { if (s_msg_scroll < 0) s_msg_scroll = 0; restored = true; }
        else if (mc > 0) { s_want_unread = false; s_want_bottom = true; }  // fully read
    }
    // Search-jump: pin the target message near the top each frame until scrolled.
    if (s_pin_anchor[0]) {
        int above = 0, found = 0;
        for (int i = 0; i < mc; i++) {
            if (strcmp(msgs[i].id, s_pin_anchor) == 0) {
                s_msg_scroll = above - 40; found = 1; break;
            }
            above += s_bh[i];
        }
        if (found) { if (s_msg_scroll < 0) s_msg_scroll = 0; restored = true; }
    }
    if (s_want_bottom) { s_msg_scroll = max_scroll; s_want_bottom = false; }
    else if (was_bottom && !restored) s_msg_scroll = max_scroll;  // follow live messages
    if (s_msg_scroll > max_scroll) s_msg_scroll = max_scroll;
    if (s_msg_scroll < 0) s_msg_scroll = 0;
    s_prev_max = max_scroll;

    int y = pane_top - s_msg_scroll;
    const char *pa = "";
    const char *me = store_me_id();
    s_nmhit = 0; s_nphit = 0; s_nrhit = 0; s_nimghit = 0; s_nrephit = 0; s_ninvhit = 0; s_nvidhit = 0;
    for (int i = 0; i < mc; i++) {
        bool blk = store_is_blocked(msgs[i].author_id);
        bool sys = MSG_IS_SYSTEM(msgs[i]) || blk;
        bool grp = (!sys && i > 0 && strcmp(msgs[i].author, pa) == 0
                    && !msgs[i].is_reply);
        if (s_div[i] || s_udiv[i]) grp = false;
        int block_h = s_bh[i];
        if (s_div[i]) {  // date separator band
            if (y + DIVIDER_H > pane_top && y < pane_bot) {
                char dl[40]; date_label(msgs[i].timestamp, dl, sizeof(dl));
                if (dl[0]) draw_date_divider(g_msg_x, y, msg_w, dl);
            }
            y += DIVIDER_H;
            block_h -= DIVIDER_H;
        }
        if (s_udiv[i]) {  // NEW / unread band
            if (y + UNREAD_H > pane_top && y < pane_bot)
                draw_unread_divider(g_msg_x, y, msg_w);
            y += UNREAD_H;
            block_h -= UNREAD_H;
        }
        if (y + block_h > pane_top && y < pane_bot) {  // visible
            if (!sys && s_nmhit < MAX_MESSAGES) {  // record hit-box
                msg_hit *hb = &s_mhit[s_nmhit++];
                hb->y0 = y; hb->y1 = y + block_h;
                snprintf(hb->id, sizeof(hb->id), "%s", msgs[i].id);
                hb->own = me && msgs[i].author_id[0] &&
                          strcmp(msgs[i].author_id, me) == 0;
                // Avatar band for profile taps: only non-grouped rows draw one.
                hb->av_cy = grp ? -1 : (y + (msgs[i].is_reply ? 20 : 0) + 18);
                hb->has_spoiler = strstr(msgs[i].content, "||") != NULL;
                snprintf(hb->author_id, sizeof(hb->author_id), "%s", msgs[i].author_id);
                snprintf(hb->author, sizeof(hb->author), "%s", msgs[i].author);
                snprintf(hb->avatar, sizeof(hb->avatar), "%s", msgs[i].avatar);
            }
            // Gold tint + left border when the message @-mentions you.
            if (msgs[i].mentions_me) {
                ui_rect(g_msg_x, y - 2, msg_w, block_h, RGBA8(0x42,0x3a,0x1e,0xFF));
                ui_rect(g_msg_x, y - 2, 3, block_h, RGBA8(0xf0,0xb2,0x32,0xFF));
            }
            if (blk) {
                ui_text(txt_x, y + 2, FS_SMALL, COL_TEXT_MUTED, "Blocked message");
            } else if (sys) {
                char line[160];
                if (msgs[i].type == 7)
                    snprintf(line, sizeof(line), "%s joined the server", msgs[i].author);
                else
                    snprintf(line, sizeof(line), "%s sent an attachment", msgs[i].author);
                ui_text(txt_x, y + 2, FS_SMALL, COL_TEXT_MUTED, line);
            } else if (grp) {
                int cy = y;
                if (msgs[i].content[0]) {
                    ui_set_spoiler_reveal(spoiler_shown(msgs[i].id));
                    ui_richtext_draw(txt_x, cy, FS_BODY, COL_TEXT, msgs[i].content, tw, line_h);
                    ui_set_spoiler_reveal(true);
                    cy += s_texth[i];  // cached; don't re-measure
                }
                if (msgs[i].has_embed) {
                    draw_embed(txt_x, cy + 4, &msgs[i], tw);
                    cy += embed_height(&msgs[i], tw) + 8;
                } else if (msgs[i].has_image) {
                    draw_message_image(txt_x, cy + 4, &msgs[i], tw);
                    int dw, dh; image_dims(&msgs[i], tw, &dw, &dh); cy += dh + 8;
                } else if (msgs[i].has_video) {
                    draw_message_video(txt_x, cy + 4, &msgs[i], tw);
                    int dw, dh; image_dims(&msgs[i], tw, &dw, &dh); cy += dh + 8;
                }
                if (msgs[i].has_file) { draw_message_file(txt_x, cy + 4, &msgs[i], tw);
                                        cy += 60; }
                if (msgs[i].has_invite) { draw_invite_tile(txt_x, cy + 4, &msgs[i], tw);
                                          cy += INV_TILE_H + 8; }
                if (msgs[i].has_poll) { draw_poll(txt_x, cy + 4, &msgs[i], tw);
                                        cy += poll_height(&msgs[i], tw) + 8; }
                if (msgs[i].reaction_count)
                    draw_reactions(txt_x, cy + 4, &msgs[i], tw, true);
            } else {
                int my = y;
                if (msgs[i].is_reply) {
                    // reply preview line + a small connector hook to the avatar
                    // compact: no avatar, preview at the text column
                    int rx;
                    if (compact) {
                        rx = txt_x;
                    } else {
                        int cxp = g_msg_x + 44;
                        ui_rect(cxp, y + 8, 2, 14, COL_DIVIDER);
                        ui_rect(cxp, y + 8, 18, 2, COL_DIVIDER);
                        rx = cxp + 24;
                    }
                    ui_text_bold(rx, y, FS_SMALL, COL_TEXT, msgs[i].reply_author);
                    int raw = ui_text_bold_w(FS_SMALL, msgs[i].reply_author);
                    ui_text_clip(rx + raw + 8, y, FS_SMALL, COL_TEXT_MUTED,
                                 msgs[i].reply_content,
                                 (txt_x + tw) - (rx + raw + 8));
                    if (msgs[i].reply_to_id[0] && s_nrephit < MAX_MESSAGES) {
                        reply_hit *rh = &s_rephit[s_nrephit++];
                        rh->y0 = y - 2; rh->y1 = y + 20;
                        snprintf(rh->cid, sizeof(rh->cid), "%s", msgs[i].channel_id);
                        snprintf(rh->mid, sizeof(rh->mid), "%s", msgs[i].reply_to_id);
                    }
                    my = y + 20;
                }
                char aurl[256] = "";
                if (msgs[i].avatar[0] && msgs[i].author_id[0])
                    snprintf(aurl, sizeof(aurl),
                             "https://cdn.discordapp.com/avatars/%s/%s.png?size=64",
                             msgs[i].author_id, msgs[i].avatar);
                // Per-server nickname from the loaded member list (covers REST
                // history, which omits member.nick); else the stored name.
                const char *dispname = store_member_nick(msgs[i].author_id);
                if (!dispname || !dispname[0]) dispname = msgs[i].author;
                if (!compact) {  // cozy: circular avatar + presence dot in the left gutter
                    draw_avatar(g_msg_x + 16 + 18, my + 18, 18, aurl, dispname, COL_BLURPLE);
                    draw_status_dot(g_msg_x + 16 + 18 + 12, my + 18 + 12,
                                    store_member_status(msgs[i].author_id), COL_CHAT);
                }
                discord_profile_extra extra; extra.n_guilds = 0; extra.n_roles = 0;
                discord_get_user_profile(msgs[i].author_id, msgs[i].guild_id, s_token, s_cacert, NULL, 0, NULL, &extra);
                ui_text_bold(txt_x, my, FS_CHANNEL, store_role_color(extra.role_ids[0]), dispname);
                int aw = ui_text_bold_w(FS_CHANNEL, dispname);
                int hx = txt_x + aw + 10;
                if (msgs[i].is_bot) {  // blurple BOT tag
                    int bw = ui_text_bold_w(FS_TINY, "BOT") + 8;
                    ui_rounded_rect(hx, my + 1, bw, 14, 4, COL_BLURPLE);
                    ui_text_bold(hx + 4, my + 1, FS_TINY, COL_WHITE, "BOT");
                    hx += bw + 8;
                }
                char tm[12]; hhmm(msgs[i].timestamp, tm);
                if (tm[0]) { ui_text(hx, my + 4, FS_TINY, COL_TEXT_MUTED, tm);
                             hx += ui_text_w(FS_TINY, tm) + 8; }
                if (msgs[i].edited)
                    ui_text(hx, my + 4, FS_TINY, COL_TEXT_MUTED, "(edited)");
                int cy = my + hdr_h;
                if (msgs[i].content[0]) {
                    ui_set_spoiler_reveal(spoiler_shown(msgs[i].id));
                    ui_richtext_draw(txt_x, cy, FS_BODY, COL_TEXT, msgs[i].content, tw, line_h);
                    ui_set_spoiler_reveal(true);
                    cy += s_texth[i];  // cached; don't re-measure
                }
                if (msgs[i].has_embed) {
                    draw_embed(txt_x, cy + 4, &msgs[i], tw);
                    cy += embed_height(&msgs[i], tw) + 8;
                } else if (msgs[i].has_image) {
                    draw_message_image(txt_x, cy + 4, &msgs[i], tw);
                    int dw, dh; image_dims(&msgs[i], tw, &dw, &dh); cy += dh + 8;
                } else if (msgs[i].has_video) {
                    draw_message_video(txt_x, cy + 4, &msgs[i], tw);
                    int dw, dh; image_dims(&msgs[i], tw, &dw, &dh); cy += dh + 8;
                }
                if (msgs[i].has_file) { draw_message_file(txt_x, cy + 4, &msgs[i], tw);
                                        cy += 60; }
                if (msgs[i].has_invite) { draw_invite_tile(txt_x, cy + 4, &msgs[i], tw);
                                          cy += INV_TILE_H + 8; }
                if (msgs[i].has_poll) { draw_poll(txt_x, cy + 4, &msgs[i], tw);
                                        cy += poll_height(&msgs[i], tw) + 8; }
                if (msgs[i].reaction_count)
                    draw_reactions(txt_x, cy + 4, &msgs[i], tw, true);
            }
        }
        y += block_h;
        pa = sys ? "" : msgs[i].author;
    }
    if (mc == 0 && c) {
        // Distinguish an empty channel from one we can't read: a 401/403 fetch
        // means no permission (user accounts still see channels they can't view).
        const char *empty = (s_fetch_http == 403 || s_fetch_http == 401)
            ? "You don't have access to this channel."
            : "No messages yet.";
        ui_text(g_msg_x + 16, pane_top + 8, FS_BODY, COL_TEXT_MUTED, empty);
    }

    // Cover message content that scrolled out of the pane, then paint the header
    // on top - messages flow *under* the header/compose bar (no GXM clipping).
    ui_rect(g_msg_x, 0, msg_w, pane_top, COL_CHAT);  // top overflow
    ui_rect(g_msg_x, pane_bot, msg_w, (SCREEN_H - 44) - pane_bot, COL_CHAT);  // bottom
    draw_topbar(c, msg_w);

    // capture typing state while locked
    bool typing = false; char typ_name[80] = "";
    if (store_open_channel()[0] &&
        strcmp(store_typing_channel(), store_open_channel()) == 0) {
        long long dt = now_ms() - store_typing_at();
        if (dt >= 0 && dt < 9000) {
            typing = true;
            snprintf(typ_name, sizeof(typ_name), "%s", store_typing_name());
        }
    }
    store_unlock();

    if (typing) {
        ui_rect(g_msg_x, SCREEN_H - 62, msg_w, 18, COL_CHAT);  // mask overlap
        char t[128];
        snprintf(t, sizeof(t), "%s is typing...", typ_name);
        ui_text(g_msg_x + 16, SCREEN_H - 60, FS_SMALL, COL_TEXT_MUTED, t);
    }

    // Scrollbar (only when the content overflows).
    if (max_scroll > 0) {
        int track_h = pane_h;
        int thumb_h = track_h * pane_h / total;
        if (thumb_h < 24) thumb_h = 24;
        int thumb_y = pane_top + (track_h - thumb_h) * s_msg_scroll / max_scroll;
        ui_rounded_rect(msg_right - 7, thumb_y, 4, thumb_h, 2, COL_DIVIDER);
    }

    // Jump-to-present button (only when scrolled up off the bottom).
    s_jump_show = (max_scroll > 0 && s_msg_scroll < max_scroll - 60);
    if (s_jump_show) {
        int jx = msg_right - 34, jy = pane_bot - 20;
        vita2d_draw_fill_circle(jx, jy, 18, COL_ELEVATED);
        vita2d_draw_fill_circle(jx, jy, 17, COL_SIDEBAR);
        ui_text_bold(jx - 6, jy - 11, FS_BODY, COL_TEXT_BRIGHT, "\xE2\x86\x93");  // down arrow
        s_jump_r[0] = jx; s_jump_r[1] = jy; s_jump_r[2] = 18;
    }

    // Compose bar (Discord-style). Pressing Triangle opens the keyboard to send. A
    // "+" button on the left opens the photo library to attach an image.
    int cb_y = SCREEN_H - 44, cb_h = 34;
    ui_rounded_rect(g_msg_x + 12, cb_y, msg_w - 24, cb_h, 8, COL_ELEVATED);
    // attach "+" and mention "@" buttons
    int ab_cx = g_msg_x + 30, ab_cy = cb_y + cb_h / 2;
    vita2d_draw_fill_circle(ab_cx, ab_cy, 11, COL_SELECTED);
    ui_text_bold(ab_cx - 6, ab_cy - 12, FS_BODY, COL_TEXT_BRIGHT, "+");
    int at_cx = g_msg_x + 58;
    vita2d_draw_fill_circle(at_cx, ab_cy, 11, COL_SELECTED);
    ui_text_bold(at_cx - 5, ab_cy - 12, FS_BODY, COL_TEXT_BRIGHT, "@");
    if (c) {
        bool dm = (c->type == CH_DM || c->type == CH_GROUP_DM);
        char ph[128];
        snprintf(ph, sizeof(ph), "Message %s%s", dm ? "" : "#", c->name);
        ui_text(g_msg_x + 76, cb_y + 8, FS_BODY, COL_TEXT_MUTED, ph);
    }
    if (s_uploading)
        ui_text(msg_right - 150, cb_y + 10, FS_SMALL, COL_TEXT_LINK, "Uploading image...");
    else {
        // "tap or [triangle] to type" -- real PS button glyph from psexchar.pvf,
        // drawn inline (falls back to [/\] text if the system font is missing).
        int hy = cb_y + 10, hx = msg_right - 150;
        hx += ui_text(hx, hy, FS_SMALL, COL_TEXT_MUTED, "tap or ");
        hx += ui_pad_glyph(hx, hy, FS_SMALL, COL_TEXT_MUTED, UI_PAD_TRIANGLE, "[/\\]");
        ui_text(hx, hy, FS_SMALL, COL_TEXT_MUTED, " to type");
    }
}

// The message action menu / emoji picker popup. Also records its rect so touch
// can hit-test it.
static void draw_action_menu(void)
{
    if (!s_menu) return;
    if (s_menu == 1) {
        const char *items[10]; int n = 0;
        #define ADD_ACT(lbl, code) do { items[n] = (lbl); s_menu_acts[n] = (code); n++; } while (0)
        ADD_ACT("Add Reaction", ACT_REACT);
        ADD_ACT("Reply", ACT_REPLY);
        ADD_ACT("Create Thread", ACT_THREAD);
        ADD_ACT("Forward", ACT_FORWARD);
        // Pin: in DMs anyone can; in guilds needs Manage Messages.
        {
            guild *pgg = s_cur_guild_id[0] ? store_find_guild(s_cur_guild_id) : NULL;
            int can_pin = (s_cur_guild_id[0] == 0) || (pgg && (pgg->my_perms & PERM_MANAGE_MESSAGES));
            if (can_pin) ADD_ACT("Pin Message", ACT_PIN);
        }
        ADD_ACT("Copy Text", ACT_COPY);
        ADD_ACT("Copy Link", ACT_COPYLINK);
        if (s_have_open && s_open_ch.type == CH_ANNOUNCE && s_act_own)
            ADD_ACT("Publish", ACT_PUBLISH);
        if (s_act_own) { ADD_ACT("Edit", ACT_EDIT); ADD_ACT("Delete", ACT_DELETE); }
        #undef ADD_ACT
        int w = 180, rowh = 34, h = n * rowh + 12;
        int x = s_menu_x, yy = s_menu_y;
        if (x + w > SCREEN_W - 8) x = SCREEN_W - 8 - w;
        if (yy + h > SCREEN_H - 8) yy = SCREEN_H - 8 - h;
        if (yy < TOPBAR_H + 4) yy = TOPBAR_H + 4;
        s_menu_rx = x; s_menu_ry = yy; s_menu_rw = w; s_menu_rh = h;
        s_menu_rowh = rowh; s_menu_rows = n;
        ui_rect(x + 3, yy + 4, w, h, RGBA8(0,0,0,0x60));  // shadow
        ui_rounded_rect(x, yy, w, h, 8, COL_ELEVATED);
        if (s_menu_sel >= n) s_menu_sel = n - 1;
        for (int i = 0; i < n; i++) {
            int ry = yy + 6 + i * rowh;
            if (i == s_menu_sel) ui_rounded_rect(x + 4, ry, w - 8, rowh - 2, 6, COL_SELECTED);
            unsigned col = (strcmp(items[i], "Delete") == 0) ? COL_RED : COL_TEXT;
            ui_text(x + 14, ry + 7, FS_CHANNEL, col, items[i]);
        }
    } else if (s_menu == 2) {
        // Quick reactions row (6) + an expand cell that opens the full grid.
        int cell = 46, ncells = 7, w = ncells * cell + 12, h = cell + 12;
        int x = s_menu_x, yy = s_menu_y;
        if (x + w > SCREEN_W - 8) x = SCREEN_W - 8 - w;
        if (yy + h > SCREEN_H - 8) yy = SCREEN_H - 8 - h;
        if (yy < TOPBAR_H + 4) yy = TOPBAR_H + 4;
        s_menu_rx = x; s_menu_ry = yy; s_menu_rw = w; s_menu_rh = h;
        s_menu_rowh = cell; s_menu_rows = 7;
        ui_rect(x + 3, yy + 4, w, h, RGBA8(0,0,0,0x60));
        ui_rounded_rect(x, yy, w, h, 8, COL_ELEVATED);
        if (s_menu_sel >= 7) s_menu_sel = 6;
        for (int i = 0; i < 6; i++) {
            int ex = x + 6 + i * cell;
            if (i == s_menu_sel) ui_rounded_rect(ex, yy + 6, cell, cell, 6, COL_SELECTED);
            ui_draw_emoji(k_react[i], ex + (cell - 30) / 2, yy + 6 + (cell - 30) / 2, 30);
        }
        int ex = x + 6 + 6 * cell;  // expand cell -> full grid
        ui_rounded_rect(ex, yy + 6, cell, cell, 6, s_menu_sel == 6 ? COL_SELECTED : COL_BLURPLE);
        ui_text_bold(ex + cell / 2 - 5, yy + 6 + cell / 2 - 13, FS_HEADER, COL_WHITE, "\xE2\x80\xBA");  // chevron
    } else if (s_menu == 3) {
        // Full emoji grid: fixed size, scrollable, clipped so cells never spill.
        // Scroll is driven by drag / D-pad; the draw only clamps it to range.
        int cols = EMOJI_COLS, cell = EMOJI_CELL, visrows = EMOJI_VISROWS;
        int w = cols * cell + 12, h = visrows * cell + 12;
        int x = (SCREEN_W - w) / 2, yy = (SCREEN_H - h) / 2;
        s_menu_rx = x; s_menu_ry = yy; s_menu_rw = w; s_menu_rh = h;
        s_menu_rowh = cell; s_menu_rows = K_EMOJI_N;
        int totrows = (K_EMOJI_N + cols - 1) / cols;
        int maxscroll = totrows * cell - visrows * cell; if (maxscroll < 0) maxscroll = 0;
        if (s_emoji_scroll > maxscroll) s_emoji_scroll = maxscroll;
        if (s_emoji_scroll < 0) s_emoji_scroll = 0;
        ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0x80));  // dim backdrop
        ui_rect(x + 3, yy + 4, w, h, RGBA8(0, 0, 0, 0x60));
        ui_rounded_rect(x, yy, w, h, 8, COL_ELEVATED);
        int ix = x + 6, iy = yy + 6, iw = cols * cell, ih = visrows * cell;
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(ix, iy, ix + iw, iy + ih);
        for (int i = 0; i < K_EMOJI_N; i++) {
            int r = i / cols, cc = i % cols;
            int ey = iy + r * cell - s_emoji_scroll;
            if (ey + cell < iy || ey > iy + ih) continue;
            int ecx = ix + cc * cell;
            if (i == s_menu_sel) ui_rounded_rect(ecx, ey, cell, cell, 6, COL_SELECTED);
            ui_draw_emoji(k_emoji[i], ecx + (cell - 40) / 2, ey + (cell - 40) / 2, 40);
        }
        vita2d_disable_clipping();
        if (maxscroll > 0)
            ui_text(x + 8, yy + h - 2, FS_TINY, COL_TEXT_MUTED, "drag to scroll");
    }
}

// Banner: the user's banner image if set (cover-fit to width, overflow masked),
// else the accent colour. Rounded to match the card's top.
static void draw_profile_banner(int cx, int cy, int cw, int bh, const char *id,
                                const char *banner, unsigned accent)
{
    ui_rounded_rect(cx, cy, cw, bh, 12, accent);
    ui_rect(cx, cy + bh - 12, cw, 12, accent);  // square off the bottom
    if (banner && banner[0] && id && id[0]) {
        char burl[256];
        const char *ext = (banner[0] == 'a' && banner[1] == '_') ? "gif" : "png";
        snprintf(burl, sizeof(burl),
                 "https://cdn.discordapp.com/banners/%s/%s.%s?size=480", id, banner, ext);
        vita2d_texture *t = imagecache_get_anim(burl, now_ms());
        if (t) {
            float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
            if (tw < 1) tw = 1; if (th < 1) th = 1;
            float s = (float)cw / tw;  // fill width exactly
            int dh = (int)(th * s);
            vita2d_draw_texture_scale(t, cx, cy, s, s);
            if (dh > bh) ui_rect(cx, cy + bh, cw, dh - bh, COL_SIDEBAR);  // mask spill
        }
    }
}

// Shared profile-card header (banner + avatar + name + optional activity +
// divider). Returns the y where content begins; used by the popout and Settings.
static int draw_profile_card_header(int cx, int cy, int cw, const char *id,
    const char *name, const char *avatar, const char *banner, unsigned accent,
    const char *activity)
{
    draw_profile_banner(cx, cy, cw, 60, id, banner, accent);
    int avx = cx + 46, avy = cy + 70;
    vita2d_draw_fill_circle(avx, avy, 42, COL_SIDEBAR);
    char url[256] = "";
    if (avatar && avatar[0] && id && id[0])
        snprintf(url, sizeof(url),
                 "https://cdn.discordapp.com/avatars/%s/%s.png?size=128", id, avatar);
    draw_avatar(avx, avy, 36, url, name, accent);
    draw_status_dot(avx + 26, avy + 26, store_member_status(id), COL_SIDEBAR);
    int ny = avy + 46;  // name below the avatar
    ui_text_bold(cx + 20, ny, FS_HEADER, COL_TEXT_BRIGHT, name);
    int y = ny + 26;
    if (activity && activity[0]) {
        ui_text_clip(cx + 20, y, FS_TINY, COL_TEXT_MUTED, activity, cw - 40);
        y += 16;
    }
    ui_rect(cx + 20, y + 2, cw - 40, 1, COL_DIVIDER);
    return y + 12;
}

// Lay out (and optionally draw) the role chips inside width `w`; returns height.
static int prof_role_chips(int x0, int y0, int w, bool draw)
{
    int rx = x0, y = y0, rowh = 20;
    for (int i = 0; i < s_prof_nroles; i++) {
        int cwd = ui_text_w(FS_TINY, s_prof_roles[i]) + 22;
        if (cwd > w) cwd = w;
        if (rx + cwd > x0 + w) { rx = x0; y += rowh + 4; }
        if (draw) {
            unsigned dot = s_prof_role_colors[i] ? s_prof_role_colors[i] : COL_TEXT_MUTED;
            ui_rounded_rect(rx, y, cwd, rowh, 9, COL_ELEVATED);
            vita2d_draw_fill_circle(rx + 9, y + rowh / 2, 3, dot);
            ui_text_clip(rx + 16, y + 3, FS_TINY, COL_TEXT, s_prof_roles[i], cwd - 20);
        }
        rx += cwd + 6;
    }
    return (y - y0) + rowh;
}
// Discord-style user profile popout: dim backdrop, accent banner, big avatar,
// name, async-fetched bio, plus role chips and mutual servers.
static void draw_profile_popout(void)
{
    if (!s_prof_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));  // dim

    int cw = 360;
    // Dynamic height: header (144) + bio + roles + mutuals + footer.
    // Long bio is truncated with an ellipsis (no clipped rich-text primitive).
    static char biobuf[260];
    const int BIO_MAXH = 90;
    int rawlen = (int)strlen(s_prof_bio), shown = rawlen;
    for (;;) {
        memcpy(biobuf, s_prof_bio, shown);
        biobuf[shown] = '\0';
        if (shown < rawlen) { biobuf[shown] = '\0'; strcat(biobuf, "\xE2\x80\xA6"); }
        if (shown <= 8 || ui_richtext_height(FS_SMALL, biobuf, cw - 40, 18) <= BIO_MAXH) break;
        shown -= 8;
    }
    int bioH = s_prof_bio[0] ? ui_richtext_height(FS_SMALL, biobuf, cw - 40, 18) : 16;
    if (bioH > BIO_MAXH) bioH = BIO_MAXH;
    int rolesH = s_prof_nroles ? 16 + prof_role_chips(0, 0, cw - 40, false) + 6 : 0;
    int mutH   = s_prof_nmut ? 16 + s_prof_nmut * 18 + 4 : 0;
    const char *act = store_member_activity(s_prof_id);
    int headerH = 154 + (act && act[0] ? 16 : 0);  // matches the shared header
    int is_self = store_me_id() && s_prof_id[0] && strcmp(s_prof_id, store_me_id()) == 0;
    int noteH = is_self ? 0 : 46;  // personal note row
    guild *ppg = s_prof_guild[0] ? store_find_guild(s_prof_guild) : NULL;
    int canmanage = ppg && (ppg->my_perms & PERM_MANAGE_ROLES) && !is_self && s_prof_id[0];
    int ch = headerH + bioH + 8 + rolesH + mutH + noteH + (canmanage ? 26 : 0)
             + (is_self ? 0 : 32) + 34;
    if (ch > 500) ch = 500;
    int cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - ch) / 2;
    unsigned accent = s_prof_accent ? s_prof_accent : COL_BLURPLE;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);
    int y = draw_profile_card_header(cx, cy, cw, s_prof_id, s_prof_name,
                                     s_prof_avatar, s_prof_banner, accent, act);
    // Send Message (open/create a DM) - prominent, not on your own profile.
    s_prof_msg_btn[2] = 0;
    if (!is_self && s_prof_id[0]) {
        int bw = cw - 40, bx = cx + 20, by = y, bh = 26;
        ui_rounded_rect(bx, by, bw, bh, 6, COL_BLURPLE);
        ui_text_bold(bx + bw / 2 - ui_text_bold_w(FS_SMALL, "Send Message") / 2, by + 6,
                     FS_SMALL, COL_WHITE, "Send Message");
        s_prof_msg_btn[0] = bx; s_prof_msg_btn[1] = by; s_prof_msg_btn[2] = bw; s_prof_msg_btn[3] = bh;
        y += bh + 8;
    }
    if (s_prof_bio[0]) {
        ui_richtext_draw(cx + 20, y, FS_SMALL, COL_TEXT, biobuf, cw - 40, 18);
        y += bioH + 8;
    } else if (s_prof_loading) {
        ui_text(cx + 20, y, FS_SMALL, COL_TEXT_MUTED, "Loading profile..."); y += 24;
    } else {
        ui_text(cx + 20, y, FS_SMALL, COL_TEXT_MUTED, "No bio."); y += 24;
    }
    if (s_prof_nroles) {
        ui_text_bold(cx + 20, y, FS_TINY, COL_TEXT_MUTED, "ROLES"); y += 16;
        y += prof_role_chips(cx + 20, y, cw - 40, true) + 6;
    }
    // Manage Roles button - only with the Manage Roles permission.
    s_prof_rolesedit_r[2] = 0;
    if (canmanage) {
        int bw = 108, bx = cx + 20, by = y, bh = 20;
        ui_rounded_rect(bx, by, bw, bh, 6, COL_ELEVATED);
        ui_text(bx + 8, by + 3, FS_TINY, COL_TEXT_BRIGHT, "\xE2\x9A\x99 Manage Roles");
        s_prof_rolesedit_r[0] = bx; s_prof_rolesedit_r[1] = by;
        s_prof_rolesedit_r[2] = bw; s_prof_rolesedit_r[3] = bh;
        y += bh + 6;
    }
    if (s_prof_nmut) {
        char mh[40];
        snprintf(mh, sizeof(mh), "%d MUTUAL SERVER%s", s_prof_nmut, s_prof_nmut > 1 ? "S" : "");
        ui_text_bold(cx + 20, y, FS_TINY, COL_TEXT_MUTED, mh); y += 16;
        for (int i = 0; i < s_prof_nmut && y < cy + ch - 24; i++) {
            ui_text_clip(cx + 20, y, FS_SMALL, COL_TEXT, s_prof_mutuals[i], cw - 40);
            y += 18;
        }
    }
    // Personal note (tap to edit), not on your own profile.
    s_prof_note_r[2] = 0;
    if (!is_self && s_prof_id[0]) {
        const char *nt = store_get_note(s_prof_id);
        ui_text_bold(cx + 20, y, FS_TINY, COL_TEXT_MUTED, "NOTE"); y += 15;
        ui_rounded_rect(cx + 20, y, cw - 40, 22, 6, COL_ELEVATED);
        ui_text_clip(cx + 28, y + 4, FS_TINY, nt ? COL_TEXT : COL_TEXT_MUTED,
                     nt ? nt : "Tap to add a note", cw - 56);
        s_prof_note_r[0]=cx+20; s_prof_note_r[1]=y; s_prof_note_r[2]=cw-40; s_prof_note_r[3]=22;
    }
    // Block / Unblock button (bottom-left), unless this is us.
    s_prof_block_btn[2] = 0;
    if (s_prof_id[0] && !(store_me_id() && strcmp(s_prof_id, store_me_id()) == 0)) {
        bool blocked = store_is_blocked(s_prof_id);
        int bx = cx + 20, by = cy + ch - 30, bw = 92, bh = 22;
        ui_rounded_rect(bx, by, bw, bh, 6, blocked ? COL_ELEVATED : COL_RED);
        ui_text_bold(bx + 12, by + 4, FS_TINY, COL_WHITE, blocked ? "Unblock" : "Block");
        s_prof_block_btn[0]=bx; s_prof_block_btn[1]=by; s_prof_block_btn[2]=bw; s_prof_block_btn[3]=bh;
    }
    // Moderation buttons: each shown only if our role has the matching permission
    // and never on ourselves. Armed (red) = tap again to confirm.
    s_prof_kick_btn[2] = s_prof_ban_btn[2] = s_prof_timeout_btn[2] = 0;
    if (s_prof_id[0] && s_prof_guild[0] &&
        !(store_me_id() && strcmp(s_prof_id, store_me_id()) == 0)) {
        guild *pg = store_find_guild(s_prof_guild);
        unsigned long long pm = pg ? pg->my_perms : 0;
        struct { int *r; const char *l; unsigned long long need; int act; } mb[3] = {
            { s_prof_kick_btn,    s_prof_mod_arm == 1 ? "Kick?" : "Kick",    PERM_KICK,     1 },
            { s_prof_ban_btn,     s_prof_mod_arm == 2 ? "Ban?"  : "Ban",     PERM_BAN,      2 },
            { s_prof_timeout_btn, s_prof_mod_arm == 3 ? "5m?"   : "Timeout", PERM_MODERATE, 3 },
        };
        int mx = cx + 120, my = cy + ch - 30, mh = 22;
        for (int i = 0; i < 3; i++) {
            if (!(pm & mb[i].need)) continue;
            int mw = 52;
            ui_rounded_rect(mx, my, mw, mh, 6, s_prof_mod_arm == mb[i].act ? COL_RED : COL_ELEVATED);
            int lx = mx + (mw - (int)strlen(mb[i].l) * 6) / 2; if (lx < mx + 4) lx = mx + 4;
            ui_text(lx, my + 4, FS_TINY, COL_WHITE, mb[i].l);
            mb[i].r[0]=mx; mb[i].r[1]=my; mb[i].r[2]=mw; mb[i].r[3]=mh;
            mx += mw + 6;
        }
    }
    ui_text(cx + cw - 70, cy + ch - 22, FS_TINY, COL_TEXT_MUTED, "tap to close");
}

// Settings / My Account: your avatar, name, bio, and a Log Out button.
static void draw_settings(void)
{
    if (!s_settings_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));

    int cw = 380;
    unsigned accent = s_me_accent ? s_me_accent : COL_BLURPLE;

    // Measure the bio (wrapped, capped at 3 lines) up front to size the modal.
    const int inner = cw - 40, lh = 18, bioMax = 3 * lh;
    static char sbio[264];
    int bioH;
    if (s_me_bio[0]) {
        int full = (int)strlen(s_me_bio), shown = full < 255 ? full : 255;
        for (;;) {
            memcpy(sbio, s_me_bio, shown); sbio[shown] = '\0';
            if (shown < full) strcat(sbio, "\xE2\x80\xA6");
            if (shown <= 8 || ui_richtext_height(FS_SMALL, sbio, inner, lh) <= bioMax) break;
            shown -= 8;
        }
        bioH = ui_richtext_height(FS_SMALL, sbio, inner, lh);
        if (bioH > bioMax) bioH = bioMax;
    } else {
        sbio[0] = '\0';
        bioH = lh;
    }

    // Section heights -> total card height. Tall content scrolls behind the dim.
    const int headerH = 154;
    int aboutH    = 6 + 18 + bioH;
    int statusH   = 12 + 18 + 30 + 8 + 30;
    int appearH   = 12 + 18 + SET_NAPP * (SET_ROW_H + 6);  // label + 7 rows
    int aboutAppH = 12 + 18 + 18 + 16;                     // label + 2 text lines
    int buttonsH  = 12 + SET_BTN_H + 10 + SET_BTN_H + 14;
    int ch = headerH + aboutH + statusH + appearH + aboutAppH + buttonsH;
    int cx = (SCREEN_W - cw) / 2, cy;
    const int SET_MARGIN = 8, avail = SCREEN_H - 2 * SET_MARGIN;
    if (ch <= avail) { cy = (SCREEN_H - ch) / 2; s_set_scroll_max = 0; }
    else {
        s_set_scroll_max = ch - avail;
        if (s_set_scroll < 0) s_set_scroll = 0;
        if (s_set_scroll > s_set_scroll_max) s_set_scroll = s_set_scroll_max;
        cy = SET_MARGIN - s_set_scroll;
    }
    s_set_cx = cx; s_set_cy = cy; s_set_cw = cw; s_set_ch = ch;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);

    // Header: banner + avatar + name (same look as the profile popout).
    int y = draw_profile_card_header(cx, cy, cw, store_me_id(), store_me_name(),
                                     store_me_avatar(), s_me_banner, accent, NULL);
    // Tap the avatar to change it, tap the name to edit it.
    s_set_av_r[0] = cx + 46 - 42; s_set_av_r[1] = cy + 70 - 42; s_set_av_r[2] = 84; s_set_av_r[3] = 84;
    vita2d_draw_fill_circle(cx + 46 + 26, cy + 70 + 26, 10, COL_ELEVATED);  // camera badge
    ui_text(cx + 46 + 20, cy + 70 + 18, FS_TINY, COL_TEXT_BRIGHT, "\xF0\x9F\x93\xB7");
    int nmw = ui_text_bold_w(FS_HEADER, store_me_name());
    s_set_name_r[0] = cx + 20; s_set_name_r[1] = cy + 112; s_set_name_r[2] = nmw + 20; s_set_name_r[3] = 28;
    ui_text(cx + 20 + nmw + 6, cy + 118, FS_TINY, COL_TEXT_MUTED, "\xE2\x9C\x8E");  // pencil

    y += 6;
    ui_text(cx + 20, y, FS_TINY, COL_TEXT_MUTED, "ABOUT ME");
    y += 18;
    if (sbio[0])
        ui_richtext_draw(cx + 20, y, FS_SMALL, COL_TEXT, sbio, inner, lh);
    else
        ui_text(cx + 20, y, FS_SMALL, COL_TEXT_MUTED,
                s_me_loading ? "Loading profile\xE2\x80\xA6" : "No bio set.");
    y += bioH;

    y += 12;
    ui_text(cx + 20, y, FS_TINY, COL_TEXT_MUTED, "STATUS");
    y += 18;
    static const char *slab[4] = { "Online", "Idle", "DND", "Invis" };
    static const int   scode[4] = { 1, 2, 3, 0 };
    int chw = (cw - 40 - 18) / 4;
    for (int i = 0; i < 4; i++) {
        int chx = cx + 20 + i * (chw + 6);
        bool cur = (s_my_status == scode[i]);
        ui_rounded_rect(chx, y, chw, 30, 6, cur ? COL_BLURPLE : COL_ELEVATED);
        draw_status_dot(chx + 12, y + 15, scode[i], cur ? COL_BLURPLE : COL_ELEVATED);
        ui_text(chx + 22, y + 8, FS_TINY, cur ? COL_WHITE : COL_TEXT, slab[i]);
        s_status_r[i][0] = chx; s_status_r[i][1] = y; s_status_r[i][2] = chw; s_status_r[i][3] = 30;
    }
    y += 30 + 8;

    // Custom status row (tap to edit).
    ui_rounded_rect(cx + 20, y, cw - 40, 30, 6, COL_ELEVATED);
    ui_text_clip(cx + 30, y + 8, FS_SMALL, s_me_custom[0] ? COL_TEXT : COL_TEXT_MUTED,
                 s_me_custom[0] ? s_me_custom : "Set a custom status\xE2\x80\xA6", cw - 60);
    s_set_cs_r[0] = cx + 20; s_set_cs_r[1] = y; s_set_cs_r[2] = cw - 40; s_set_cs_r[3] = 30;
    y += 30;

    // APPEARANCE: app-local look + behavior. Tapping a row cycles its setting.
    y += 12;
    ui_text(cx + 20, y, FS_TINY, COL_TEXT_MUTED, "APPEARANCE");
    y += 18;
    static const char *fs_lab[3] = { "Small", "Medium", "Large" };
    char av[SET_NAPP][20];
    snprintf(av[0], 20, "%s", g_settings.theme ? "Light" : "Dark");
    av[1][0] = '\0';  // accent draws a swatch, not text
    snprintf(av[2], 20, "%s", fs_lab[g_settings.font_size % 3]);
    snprintf(av[3], 20, "%s", g_settings.density ? "Compact" : "Cozy");
    snprintf(av[4], 20, "%s", g_settings.clock_24h ? "24-hour" : "12-hour");
    snprintf(av[5], 20, "%s", g_settings.open_last ? "Last channel" : "Home");
    snprintf(av[6], 20, "%s", g_settings.low_battery ? "On" : "Off");
    static const char *app_lab[SET_NAPP] = {
        "Theme", "Accent", "Font Size", "Density", "Clock", "Open To", "Low Battery" };
    for (int i = 0; i < SET_NAPP; i++) {
        ui_rounded_rect(cx + 20, y, cw - 40, SET_ROW_H, 6, COL_ELEVATED);
        ui_text(cx + 30, y + 8, FS_SMALL, COL_TEXT, app_lab[i]);
        if (i == 1) {  // accent swatch (COL_BLURPLE tracks the active accent)
            int sw = 46, sh = SET_ROW_H - 12;
            ui_rounded_rect(cx + 20 + (cw - 40) - sw - 10, y + 6, sw, sh, 4, COL_BLURPLE);
        } else {
            int vw = ui_text_w(FS_SMALL, av[i]);
            ui_text(cx + 20 + (cw - 40) - vw - 10, y + 8, FS_SMALL, COL_TEXT_BRIGHT, av[i]);
        }
        s_set_app_r[i][0] = cx + 20; s_set_app_r[i][1] = y;
        s_set_app_r[i][2] = cw - 40; s_set_app_r[i][3] = SET_ROW_H;
        y += SET_ROW_H + 6;
    }

    // ABOUT: static app info.
    y += 12;
    ui_text(cx + 20, y, FS_TINY, COL_TEXT_MUTED, "ABOUT");
    y += 18;
    ui_text_bold(cx + 20, y, FS_SMALL, COL_TEXT_BRIGHT, "Cordless " CL_VERSION);
    y += 18;
    ui_text(cx + 20, y, FS_TINY, COL_TEXT_MUTED, "Log: ux0:data/cordless/log.txt");
    y += 16;

    y += 12;
    s_set_eb_y = y;
    int halfw = (cw - 40 - 10) / 2;
    ui_rounded_rect(cx + 20, s_set_eb_y, halfw, SET_BTN_H, 8, COL_BLURPLE);
    int ew = ui_text_bold_w(FS_BODY, "Edit Bio");
    ui_text_bold(cx + 20 + halfw / 2 - ew / 2, s_set_eb_y + 8, FS_BODY, COL_WHITE, "Edit Bio");
    int swx = cx + 20 + halfw + 10;
    ui_rounded_rect(swx, s_set_eb_y, halfw, SET_BTN_H, 8, COL_ELEVATED);
    int sww = ui_text_bold_w(FS_SMALL, "Switch Account");
    ui_text_bold(swx + halfw / 2 - sww / 2, s_set_eb_y + 9, FS_SMALL, COL_TEXT_BRIGHT, "Switch Account");
    s_set_sw_r[0] = swx; s_set_sw_r[1] = s_set_eb_y; s_set_sw_r[2] = halfw; s_set_sw_r[3] = SET_BTN_H;

    s_set_lo_y = s_set_eb_y + SET_BTN_H + 10;
    ui_rounded_rect(cx + 20, s_set_lo_y, cw - 40, SET_BTN_H, 8, RGBA8(0xda,0x37,0x3c,0xFF));
    int lw = ui_text_bold_w(FS_BODY, "Log Out");
    ui_text_bold(cx + cw / 2 - lw / 2, s_set_lo_y + 8, FS_BODY, COL_WHITE, "Log Out");
}

// Account switcher: saved accounts + "Add Account".
static void draw_accounts(void)
{
    if (!s_accounts_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));
    int cw = 360, ch = 300, cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - ch) / 2;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);
    ui_rect(cx, cy, cw, 40, COL_TOPBAR);
    ui_text_bold(cx + 16, cy + 12, FS_BODY, COL_TEXT_BRIGHT, "Accounts");
    ui_text(cx + cw - 74, cy + 14, FS_TINY, COL_TEXT_MUTED, "tap to close");
    int y = cy + 50;
    for (int i = 0; i < s_accounts_n; i++) {
        bool active = store_me_id()[0] && strcmp(s_accounts[i].id, store_me_id()) == 0;
        if (active) ui_rounded_rect(cx + 12, y - 2, cw - 24, 38, 8, COL_SELECTED);
        char url[256] = "";
        if (s_accounts[i].avatar[0] && s_accounts[i].id[0])
            snprintf(url, sizeof(url), "https://cdn.discordapp.com/avatars/%s/%s.png?size=64",
                     s_accounts[i].id, s_accounts[i].avatar);
        draw_avatar(cx + 32, y + 16, 14, url, s_accounts[i].name, COL_BLURPLE);
        ui_text_bold(cx + 56, y + 8, FS_SMALL, COL_TEXT_BRIGHT, s_accounts[i].name);
        if (active) ui_text(cx + cw - 60, y + 10, FS_TINY, COL_GREEN, "active");
        s_acc_hit_y0[i] = y; s_acc_hit_y1[i] = y + 36;
        y += 40;
    }
    int bx = cx + 16, bw = cw - 32, bh = 34;
    ui_rounded_rect(bx, y, bw, bh, 8, COL_BLURPLE);
    int aw = ui_text_bold_w(FS_BODY, "+ Add Account");
    ui_text_bold(cx + cw / 2 - aw / 2, y + 8, FS_BODY, COL_WHITE, "+ Add Account");
    s_acc_add_r[0] = bx; s_acc_add_r[1] = y; s_acc_add_r[2] = bw; s_acc_add_r[3] = bh;
}

// Pinned-messages list popout.
static void draw_pins(void)
{
    if (!s_pins_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));
    int cw = 460, ch = 420;
    int cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - ch) / 2;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);
    ui_rect(cx, cy, cw, 40, COL_TOPBAR);
    ui_text_bold(cx + 16, cy + 12, FS_BODY, COL_TEXT_BRIGHT, "Pinned Messages");
    ui_text(cx + cw - 74, cy + 14, FS_TINY, COL_TEXT_MUTED, "tap to close");

    int y = cy + 50;
    if (s_pins_loading && s_pins_count == 0) {
        ui_text(cx + 16, y, FS_SMALL, COL_TEXT_MUTED, "Loading pins...");
    } else if (s_pins_count == 0) {
        ui_text(cx + 16, y, FS_SMALL, COL_TEXT_MUTED, "No pinned messages.");
    } else {
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(cx, y, cx + cw, y + ch);
        for (int i = 0; i < s_pins_count && y < cy + ch - 40; i++) {
            message *m = &s_pins[i];
            char tm[12]; hhmm(m->timestamp, tm);
            ui_text_bold(cx + 16, y, FS_SMALL, COL_TEXT_BRIGHT, m->author);
            int aw = ui_text_bold_w(FS_SMALL, m->author);
            if (tm[0]) ui_text(cx + 16 + aw + 8, y + 2, FS_TINY, COL_TEXT_MUTED, tm);
            y += 18;
            const char *body = m->content[0] ? m->content
                             : (m->has_image ? "[image]"
                             : (m->has_file ? "[attachment]" : "[embed]"));
            int hh = ui_richtext_height(FS_SMALL, body, cw - 32, 16);
            if (hh > 40) hh = 40;  // cap each preview
            ui_richtext_draw(cx + 16, y, FS_SMALL, COL_TEXT, body, cw - 32, 16);
            y += hh + 6;
            ui_rect(cx + 16, y, cw - 32, 1, COL_DIVIDER);
            y += 8;
        }
        vita2d_disable_clipping();
    }
}

// Full-screen image viewer (tap an inline image).
static void draw_lightbox(void)
{ // to maybe do: replace current lightbox with the photo review dialog?
    if (!s_lightbox_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xE6));
    vita2d_texture *t = s_lightbox_url[0] ? imagecache_get_anim(s_lightbox_url, now_ms()) : NULL;
    if (t) {
        float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
        if (tw < 1) tw = 1; if (th < 1) th = 1;
        float maxw = SCREEN_W - 40, maxh = SCREEN_H - 60;
        float s = maxw / tw; if (th * s > maxh) s = maxh / th;  // fit within the screen
        if (s > 3.0f) s = 3.0f;
        int dw = (int)(tw * s), dh = (int)(th * s);
        vita2d_draw_texture_scale(t, (SCREEN_W - dw) / 2, (SCREEN_H - dh) / 2, s, s);
    } else {
        ui_text(SCREEN_W / 2 - 40, SCREEN_H / 2, FS_BODY, COL_TEXT_MUTED, "Loading...");
    }
    ui_text(SCREEN_W / 2 - 40, SCREEN_H - 24, FS_TINY, COL_TEXT_MUTED, "tap to close");
}

// Message-search results modal. Tap a result to jump to it.
static void draw_search(void)
{
    if (!s_search_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));
    int cw = 540, ch = 440, cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - ch) / 2;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);
    ui_rect(cx, cy, cw, 40, COL_TOPBAR);
    char hdr[100]; snprintf(hdr, sizeof(hdr), "Search: %.48s", s_search_q);
    ui_text_bold(cx + 16, cy + 12, FS_BODY, COL_TEXT_BRIGHT, hdr);
    ui_text(cx + cw - 74, cy + 14, FS_TINY, COL_TEXT_MUTED, "tap to close");
    // Scrollable content drawn offset by -s_search_scroll, then the header bar is
    // redrawn on top to mask rows scrolled under it. Offscreen rows are skipped.
    const int vis_top = cy + 44, vis_bot = cy + ch - 8;
    int y = cy + 50 - s_search_scroll;
    // USERS section (matched from loaded members + DMs).
    if (s_suser_n > 0) {
        if (y + 14 > vis_top && y < vis_bot)
            ui_text_bold(cx + 16, y, FS_TINY, COL_TEXT_MUTED, "USERS");
        y += 18;
        for (int i = 0; i < s_suser_n; i++) {
            int vis = (y + 30 > vis_top && y < vis_bot);
            s_suser_hit_y0[i] = vis ? y : -9999;
            s_suser_hit_y1[i] = vis ? y + 30 : -9999;
            if (vis) {
                char url[256] = "";
                if (s_susers[i].avatar[0] && s_susers[i].id[0])
                    snprintf(url, sizeof(url),
                        "https://cdn.discordapp.com/avatars/%s/%s.png?size=64",
                        s_susers[i].id, s_susers[i].avatar);
                draw_avatar(cx + 28, y + 12, 12, url, s_susers[i].name, COL_BLURPLE);
                ui_text_bold(cx + 48, y + 6, FS_SMALL, COL_TEXT_BRIGHT, s_susers[i].name);
            }
            y += 32;
        }
        if (y + 2 > vis_top && y < vis_bot)
            ui_rect(cx + 16, y + 2, cw - 32, 1, COL_DIVIDER);
        y += 12;
    }
    // MESSAGES section.
    if (s_search_loading && s_search_n == 0) {
        if (y < vis_bot) ui_text(cx + 16, y, FS_SMALL, COL_TEXT_MUTED, "Searching messages...");
    } else if (s_search_n == 0) {
        if (s_suser_n == 0 && y < vis_bot)
            ui_text(cx + 16, y, FS_SMALL, COL_TEXT_MUTED, "No results.");
    } else {
      if (y + 14 > vis_top && y < vis_bot)
          ui_text_bold(cx + 16, y, FS_TINY, COL_TEXT_MUTED, "MESSAGES");
      y += 18;
      for (int i = 0; i < s_search_n; i++) {
        message *m = &s_search_res[i];
        int vis = (y + 44 > vis_top && y < vis_bot);
        s_search_hit_y0[i] = vis ? y : -9999;
        s_search_hit_y1[i] = vis ? y + 44 : -9999;
        if (vis) {
            char chn[44];
            store_lock();
            channel *c2 = store_find_channel(m->channel_id);
            snprintf(chn, sizeof(chn), "#%s", c2 ? c2->name : "channel");
            store_unlock();
            ui_text_bold(cx + 16, y, FS_SMALL, COL_TEXT_BRIGHT, m->author);
            int aw = ui_text_bold_w(FS_SMALL, m->author);
            ui_text(cx + 16 + aw + 8, y + 2, FS_TINY, COL_TEXT_MUTED, chn);
            // Snippet: strip the mention sentinels (plain drawer can't render them).
            char snip[140]; int sj = 0;
            const char *src = m->content[0] ? m->content
                            : (m->has_image ? "[image]" : m->has_poll ? "[poll]" : "[attachment]");
            for (const char *p = src; *p && sj < (int)sizeof(snip) - 1; p++)
                if (*p != '\x01' && *p != '\x02' && *p != '\n') snip[sj++] = *p;
            snip[sj] = '\0';
            ui_text_clip(cx + 16, y + 18, FS_SMALL, COL_TEXT, snip, cw - 32);
            ui_rect(cx + 16, y + 40, cw - 32, 1, COL_DIVIDER);
        }
        y += 48;
      }
    }
    // Total content height -> max scroll. y ended at content bottom (in scrolled
    // coords); undo the offset to get absolute content height.
    int content_bot = y + s_search_scroll;
    int total = content_bot - (cy + 50);
    s_search_max = total - (vis_bot - (cy + 50));
    if (s_search_max < 0) s_search_max = 0;
    // Redraw the header bar so scrolled-up rows are masked behind it.
    ui_rect(cx, cy, cw, 40, COL_TOPBAR);
    ui_text_bold(cx + 16, cy + 12, FS_BODY, COL_TEXT_BRIGHT, hdr);
    ui_text(cx + cw - 74, cy + 14, FS_TINY, COL_TEXT_MUTED, "tap to close");
}

// Threads list for a text channel (topbar "Threads" button).
static void draw_threads_modal(void)
{
    if (!s_thr_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));
    int cw = 440, ch = 380, cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - ch) / 2;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);
    ui_rect(cx, cy, cw, 40, COL_TOPBAR);
    ui_text_bold(cx + 16, cy + 12, FS_BODY, COL_TEXT_BRIGHT, "Threads");
    ui_text(cx + cw - 74, cy + 14, FS_TINY, COL_TEXT_MUTED, "tap to close");
    int y = cy + 50;
    if (s_thr_loading && s_thr_n == 0)
        ui_text(cx + 16, y, FS_SMALL, COL_TEXT_MUTED, "Loading threads...");
    else if (s_thr_n == 0)
        ui_text(cx + 16, y, FS_SMALL, COL_TEXT_MUTED, "No active threads.");
    else for (int i = 0; i < s_thr_n && y < cy + ch - 30; i++) {
        s_thr_hit_y0[i] = y; s_thr_hit_y1[i] = y + 30;
        ui_rounded_rect(cx + 12, y, cw - 24, 28, 6, COL_ELEVATED);
        ui_text_bold_clip(cx + 22, y + 6, FS_SMALL, COL_TEXT_BRIGHT, s_thr[i].name, cw - 40);
        y += 34;
    }
}

// New-post form: title + tag chips + message + Post button. Records hit rects.
static void draw_newpost(void)
{
    if (!s_np_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));
    int cw = 460, ch = 344, cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - ch) / 2;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);
    ui_rounded_rect(cx, cy, cw, 38, 12, COL_TOPBAR);
    ui_rect(cx, cy + 26, cw, 12, COL_TOPBAR);
    ui_text_bold(cx + 16, cy + 11, FS_BODY, COL_TEXT_BRIGHT, "New Post");
    ui_text(cx + cw - 78, cy + 14, FS_TINY, COL_TEXT_MUTED, "tap out = cancel");

    int y = cy + 48;
    ui_text(cx + 16, y, FS_TINY, COL_TEXT_MUTED, "TITLE"); y += 15;
    ui_rounded_rect(cx + 16, y, cw - 32, 28, 6, COL_ELEVATED);
    ui_text_clip(cx + 24, y + 6, FS_SMALL, s_np_title[0] ? COL_TEXT : COL_TEXT_MUTED,
                 s_np_title[0] ? s_np_title : "Tap to enter title", cw - 48);
    s_np_title_r[0]=cx+16; s_np_title_r[1]=y; s_np_title_r[2]=cw-32; s_np_title_r[3]=28;
    y += 38;

    for (int i = 0; i < MAX_FTAGS; i++) s_np_tag_r[i][2] = 0;
    if (s_np_ntags > 0) {
        ui_text(cx + 16, y, FS_TINY, COL_TEXT_MUTED,
                s_np_tag_required ? "TAG (required)" : "TAG"); y += 15;
        int tx = cx + 16, ty = y;
        for (int i = 0; i < s_np_ntags; i++) {
            int w = ui_text_w(FS_SMALL, s_np_tag_names[i]) + 16;
            if (tx + w > cx + cw - 16) { tx = cx + 16; ty += 26; }
            unsigned bg = (i == s_np_tagsel) ? COL_BLURPLE : COL_ELEVATED;
            ui_rounded_rect(tx, ty, w, 22, 8, bg);
            ui_text(tx + 8, ty + 3, FS_SMALL, (i == s_np_tagsel) ? COL_WHITE : COL_TEXT,
                    s_np_tag_names[i]);
            s_np_tag_r[i][0]=tx; s_np_tag_r[i][1]=ty; s_np_tag_r[i][2]=w; s_np_tag_r[i][3]=22;
            tx += w + 6;
        }
        y = ty + 32;
    }

    ui_text(cx + 16, y, FS_TINY, COL_TEXT_MUTED, "MESSAGE"); y += 15;
    ui_rounded_rect(cx + 16, y, cw - 32, 44, 6, COL_ELEVATED);
    ui_text_clip(cx + 24, y + 6, FS_SMALL, s_np_content[0] ? COL_TEXT : COL_TEXT_MUTED,
                 s_np_content[0] ? s_np_content : "Tap to enter message (optional)", cw - 48);
    s_np_content_r[0]=cx+16; s_np_content_r[1]=y; s_np_content_r[2]=cw-32; s_np_content_r[3]=44;

    if (s_forum_status[0])
        ui_text(cx + 16, cy + ch - 34, FS_SMALL, COL_RED, s_forum_status);
    int pw = ui_text_bold_w(FS_BODY, "Post") + 40, px = cx + cw - pw - 16, py = cy + ch - 40;
    ui_rounded_rect(px, py, pw, 30, 8, COL_BLURPLE);
    ui_text_bold(px + (pw - ui_text_bold_w(FS_BODY, "Post")) / 2, py + 7, FS_BODY, COL_WHITE, "Post");
    s_np_post_r[0]=px; s_np_post_r[1]=py; s_np_post_r[2]=pw; s_np_post_r[3]=30;
}

// "Add a Server" modal: Create My Own / Join a Server (Discord's flow).
static void draw_addserver(void)
{
    if (!s_addserver_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));
    int cw = 320, ch = 176, cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - ch) / 2;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);
    ui_text_bold(cx + 16, cy + 12, FS_BODY, COL_TEXT_BRIGHT, "Add a Server");
    ui_text(cx + 16, cy + 34, FS_TINY, COL_TEXT_MUTED, "Create your own or join with an invite.");
    struct { const char *l; int *r; unsigned col; } opt[2] = {
        { "\xE2\x9C\xA6  Create My Own", s_as_create_r, COL_BLURPLE },
        { "\xF0\x9F\x94\x97  Join a Server", s_as_join_r, COL_ELEVATED },
    };
    for (int i = 0; i < 2; i++) {
        int bx = cx + 16, by = cy + 56 + i * 52, bw = cw - 32, bh = 44;
        ui_rounded_rect(bx, by, bw, bh, 8, opt[i].col);
        ui_text_bold(bx + 14, by + 13, FS_BODY, COL_TEXT_BRIGHT, opt[i].l);
        opt[i].r[0]=bx; opt[i].r[1]=by; opt[i].r[2]=bw; opt[i].r[3]=bh;
    }
}

// Mention picker: scrollable list of the loaded guild members.
static void draw_mention(void)
{
    if (!s_mention_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));
    int cw = 420, ch = 430, cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - ch) / 2;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);
    const int vis_top = cy + 44, vis_bot = cy + ch - 8;
    int y = cy + 50 - s_mention_scroll;
    if (s_mention_n == 0)
        ui_text(cx + 16, cy + 50, FS_SMALL, COL_TEXT_MUTED, "No members loaded (open a server).");
    for (int i = 0; i < s_mention_n; i++) {
        int vis = (y + 32 > vis_top && y < vis_bot);
        s_mention_hit_y0[i] = vis ? y : -9999;
        s_mention_hit_y1[i] = vis ? y + 32 : -9999;
        if (vis) {
            char url[256] = "";
            if (s_mention_av[i][0] && s_mention_id[i][0])
                snprintf(url, sizeof(url),
                    "https://cdn.discordapp.com/avatars/%s/%s.png?size=64",
                    s_mention_id[i], s_mention_av[i]);
            draw_avatar(cx + 28, y + 13, 12, url, s_mention_name[i], COL_BLURPLE);
            ui_text_bold(cx + 48, y + 6, FS_SMALL, COL_TEXT_BRIGHT, s_mention_name[i]);
        }
        y += 34;
    }
    int total = (y + s_mention_scroll) - (cy + 50);
    s_mention_max = total - (vis_bot - (cy + 50));
    if (s_mention_max < 0) s_mention_max = 0;
    ui_rect(cx, cy, cw, 40, COL_TOPBAR);
    ui_text_bold(cx + 16, cy + 12, FS_BODY, COL_TEXT_BRIGHT, "Mention someone");
    ui_text(cx + cw - 74, cy + 14, FS_TINY, COL_TEXT_MUTED, "tap to close");
}

// Forward picker: scrollable list of DMs + current-guild channels.
static void draw_forward(void)
{
    if (!s_forward_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));
    int cw = 440, ch = 430, cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - ch) / 2;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);
    const int vis_top = cy + 44, vis_bot = cy + ch - 8;
    int y = cy + 50 - s_fwd_scroll;
    if (s_fwd_n == 0)
        ui_text(cx + 16, cy + 50, FS_SMALL, COL_TEXT_MUTED, "No channels to forward to.");
    for (int i = 0; i < s_fwd_n; i++) {
        int vis = (y + 28 > vis_top && y < vis_bot);
        s_fwd_hit_y0[i] = vis ? y : -9999;
        s_fwd_hit_y1[i] = vis ? y + 28 : -9999;
        if (vis) {
            char label[96];
            snprintf(label, sizeof(label), "%s%s", s_fwd_is_dm[i] ? "@" : "#", s_fwd_name[i]);
            ui_text_clip(cx + 16, y + 4, FS_SMALL, COL_TEXT_BRIGHT, label, cw - 32);
        }
        y += 30;
    }
    int total = (y + s_fwd_scroll) - (cy + 50);
    s_fwd_max = total - (vis_bot - (cy + 50));
    if (s_fwd_max < 0) s_fwd_max = 0;
    // header mask
    ui_rect(cx, cy, cw, 40, COL_TOPBAR);
    ui_text_bold(cx + 16, cy + 12, FS_BODY, COL_TEXT_BRIGHT, "Forward to...");
    ui_text(cx + cw - 74, cy + 14, FS_TINY, COL_TEXT_MUTED, "tap to close");
}

static void draw_attach_sheet(void)
{
    if (!s_attach_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));
    int cw = 300, ch = 150, cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - ch) / 2;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);
    ui_text_bold(cx + 16, cy + 12, FS_BODY, COL_TEXT_BRIGHT, "Add attachment");
    const char *labels[2] = { "\xF0\x9F\x96\xBC  Photo Library", "\xF0\x9F\x93\xB7  Camera" };
    for (int i = 0; i < 2; i++) {
        int bx = cx + 16, by = cy + 44 + i * 48, bw = cw - 32, bh = 40;
        ui_rounded_rect(bx, by, bw, bh, 8, COL_ELEVATED);
        ui_text(bx + 14, by + 11, FS_BODY, COL_TEXT_BRIGHT, labels[i]);
        s_attach_r[i][0] = bx; s_attach_r[i][1] = by; s_attach_r[i][2] = bw; s_attach_r[i][3] = bh;
    }
}

static const char *status_label(int st)
{
    return st == 1 ? "Online" : st == 2 ? "Idle" : st == 3 ? "Do Not Disturb" : "Offline";
}
// Friends list modal (Home header "Friends"): incoming requests show Accept/Deny,
// outgoing show Cancel, friends show Remove and open the DM/profile on a row tap.
static void draw_friends(void)
{
    if (!s_friends_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));
    int cw = 480, ch = 452, cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - ch) / 2;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);

    const int vis_top = cy + 44, vis_bot = cy + ch - 8;
    int y = cy + 50 - s_friends_scroll;
    if (s_friends_loading && s_friends_n == 0) {
        ui_text(cx + 16, cy + 50, FS_SMALL, COL_TEXT_MUTED, "Loading friends...");
    } else if (s_friends_n == 0) {
        ui_text(cx + 16, cy + 50, FS_SMALL, COL_TEXT_MUTED, "No friends yet. Tap + Add to send a request.");
    } else {
        for (int i = 0; i < s_friends_n; i++) {
            int vis = (y + 40 > vis_top && y < vis_bot);
            s_friends_hit_y0[i] = vis ? y : -9999;
            s_friends_hit_y1[i] = vis ? y + 40 : -9999;
            s_fbtn_a[i][2] = s_fbtn_b[i][2] = 0;  // reset hit rects each frame
            if (vis) {
                int st = store_member_status(s_friends[i].id);
                char url[256] = "";
                if (s_friends[i].avatar[0] && s_friends[i].id[0])
                    snprintf(url, sizeof(url),
                        "https://cdn.discordapp.com/avatars/%s/%s.png?size=64",
                        s_friends[i].id, s_friends[i].avatar);
                draw_avatar(cx + 30, y + 17, 14, url, s_friends[i].name, COL_BLURPLE);
                draw_status_dot(cx + 30 + 10, y + 17 + 10, st, COL_SIDEBAR);
                ui_text_bold(cx + 54, y + 6, FS_SMALL, COL_TEXT_BRIGHT, s_friends[i].name);
                const char *sub = s_friends[i].type == 3 ? "Incoming friend request"
                                : s_friends[i].type == 4 ? "Outgoing friend request"
                                : status_label(st);
                ui_text(cx + 54, y + 23, FS_TINY, COL_TEXT_MUTED, sub);
                // Action buttons on the right.
                int bx = cx + cw - 42, by = y + 8, bs = 26;
                if (s_friends[i].type == 3) {  // accept + deny
                    ui_rounded_rect(bx, by, bs, bs, 6, COL_GREEN);
                    ui_text_bold(bx + 7, by + 5, FS_SMALL, COL_WHITE, "\xE2\x9C\x93");  // check
                    s_fbtn_a[i][0]=bx; s_fbtn_a[i][1]=by; s_fbtn_a[i][2]=bs; s_fbtn_a[i][3]=bs;
                    int dx = bx - bs - 8;
                    ui_rounded_rect(dx, by, bs, bs, 6, COL_ELEVATED);
                    ui_text_bold(dx + 8, by + 4, FS_SMALL, COL_TEXT_BRIGHT, "\xC3\x97");  // x
                    s_fbtn_b[i][0]=dx; s_fbtn_b[i][1]=by; s_fbtn_b[i][2]=bs; s_fbtn_b[i][3]=bs;
                } else {  // remove / cancel
                    ui_rounded_rect(bx, by, bs, bs, 6, COL_ELEVATED);
                    ui_text_bold(bx + 8, by + 4, FS_SMALL, COL_TEXT_BRIGHT, "\xC3\x97");
                    s_fbtn_a[i][0]=bx; s_fbtn_a[i][1]=by; s_fbtn_a[i][2]=bs; s_fbtn_a[i][3]=bs;
                }
            }
            y += 42;
        }
    }
    int total = (y + s_friends_scroll) - (cy + 50);
    s_friends_max = total - (vis_bot - (cy + 50));
    if (s_friends_max < 0) s_friends_max = 0;

    // Header (drawn last to mask scrolled rows).
    ui_rect(cx, cy, cw, 40, COL_TOPBAR);
    char hdr[40]; snprintf(hdr, sizeof(hdr), "Friends \xE2\x80\x94 %d", s_friends_n);
    ui_text_bold(cx + 16, cy + 12, FS_BODY, COL_TEXT_BRIGHT, hdr);
    int abw = 60, abx = cx + cw - abw - 84, aby = cy + 9;
    ui_rounded_rect(abx, aby, abw, 22, 6, COL_GREEN);
    ui_text_bold(abx + 9, aby + 4, FS_TINY, COL_WHITE, "+ Add");
    s_friends_add_btn[0]=abx; s_friends_add_btn[1]=aby; s_friends_add_btn[2]=abw; s_friends_add_btn[3]=22;
    ui_text(cx + cw - 74, cy + 14, FS_TINY, COL_TEXT_MUTED, "tap to close");
}

// Transient toast centered near the bottom of the message pane.
static void draw_toast(void)
{
    if (!s_toast[0] || now_ms() >= s_toast_until) return;
    int pad = 12, maxw = 460;
    int tw = ui_wrap_height(FS_SMALL, s_toast, maxw, 18);  // height
    int bw = maxw + pad * 2, bh = tw + pad * 2;
    int bx = g_msg_x + (SCREEN_W - g_msg_x - bw) / 2, by = SCREEN_H - 60 - bh;
    ui_rounded_rect(bx, by, bw, bh, 8, COL_SIDEBAR);
    ui_rect(bx, by, 3, bh, COL_BLURPLE);
    ui_text_wrapped(bx + pad, by + pad, FS_SMALL, COL_TEXT_BRIGHT, s_toast, maxw, 18);
}

// Lay out (and optionally draw) a guild's role chips (colored dot + name) inside
// width `w`; returns the height consumed. Shows up to 18 roles.
static int srv_role_chips(int x0, int y0, int w, const guild *g, bool draw)
{
    if (!g || g->role_count == 0) return 0;
    int rx = x0, y = y0, rowh = 20, shown = 0;
    for (int i = 0; i < g->role_count && shown < 18; i++) {
        const char *rn = store_role_name(g->role_ids[i]);
        if (!rn || !rn[0]) continue;
        int cwd = ui_text_w(FS_TINY, rn) + 22; if (cwd > w) cwd = w;
        if (rx + cwd > x0 + w) { rx = x0; y += rowh + 4; }
        if (draw) {
            unsigned dot = store_role_color(g->role_ids[i]); if (!dot) dot = COL_TEXT_MUTED;
            ui_rounded_rect(rx, y, cwd, rowh, 9, COL_ELEVATED);
            vita2d_draw_fill_circle(rx + 9, y + rowh / 2, 3, dot);
            ui_text_clip(rx + 16, y + 3, FS_TINY, COL_TEXT, rn, cwd - 20);
        }
        rx += cwd + 6; shown++;
    }
    return shown ? (y - y0) + rowh : 0;
}

static void draw_serverinfo(void)
{
    if (!s_srvinfo_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));  // dim backdrop
    guild *g = store_find_guild(s_srvinfo_gid);
    int cw = 420, cx = (SCREEN_W - cw) / 2, bh = 116, descw = cw - 32;
    int dh = (g && g->description[0]) ? ui_wrap_height(FS_SMALL, g->description, descw, 18) : 0;
    int rh = g ? srv_role_chips(0, 0, descw, g, false) : 0;
    int ch = bh + 84 + (dh > 0 ? dh + 12 : 0) + (rh > 0 ? rh + 20 : 0) + 52;
    int cy = (SCREEN_H - ch) / 2;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_ELEVATED);
    if (!g) { ui_text(cx + 16, cy + 16, FS_SMALL, COL_TEXT_MUTED, "Server unavailable"); return; }

    draw_profile_banner(cx, cy, cw, bh, g->id, g->banner, COL_BLURPLE);
    char iurl[256] = "";
    if (g->icon[0])
        snprintf(iurl, sizeof(iurl),
                 "https://cdn.discordapp.com/icons/%s/%s.png?size=128", g->id, g->icon);
    draw_guild_icon(cx + 46, cy + bh, 28, iurl, g->name, COL_BLURPLE);

    int ty = cy + bh + 14;
    ui_text_bold_clip(cx + 88, ty, FS_HEADER, COL_TEXT_BRIGHT, g->name, cw - 88 - 16);
    ty += 24;
    if (g->member_count > 0) {
        char meta[64]; snprintf(meta, sizeof(meta), "\xE2\x97\x8B %d Members", g->member_count);
        ui_text(cx + 88, ty, FS_TINY, COL_TEXT_MUTED, meta);
    }
    if (g->premium_tier > 0) {
        char bp[40]; snprintf(bp, sizeof(bp), "Boost Lv %d  \xC2\xB7  %d boosts",
                              g->premium_tier, g->premium_subscription_count);
        ui_rounded_rect(cx + 88, ty + 18, 150, 16, 4, 0xFFFA73FFu);
        ui_text(cx + 94, ty + 19, FS_TINY, COL_WHITE, bp);
        ty += 22;
    }
    ty += 24;
    if (dh > 0) { ui_text_wrapped(cx + 16, ty, FS_SMALL, COL_TEXT_MUTED, g->description, descw, 18);
                  ty += dh + 10; }
    if (rh > 0) {
        ui_text_bold(cx + 16, ty, FS_TINY, COL_TEXT_MUTED, "ROLES"); ty += 16;
        srv_role_chips(cx + 16, ty, descw, g, true);
    }
    ui_text(cx + cw - 84, cy + 12, FS_TINY, COL_WHITE, "tap to close");

    // Action buttons. Invite is gated on the Create Invite permission (only shown
    // if our role allows it); Mark Read / Mute / Leave are personal actions.
    s_srvbtn_invite[2] = s_srvbtn_read[2] = s_srvbtn_mute[2] = s_srvbtn_leave[2] = 0;
    struct { int *r; const char *l; unsigned col; int show; } btn[4] = {
        { s_srvbtn_invite, "Invite",    COL_BLURPLE,  (g->my_perms & PERM_CREATE_INVITE) != 0 },
        { s_srvbtn_read,   "Mark Read", COL_ELEVATED, 1 },
        { s_srvbtn_mute,   "Mute",      COL_ELEVATED, 1 },
        { s_srvbtn_leave,  s_srv_confirm_leave ? "Confirm?" : "Leave", COL_RED, 1 },
    };
    int nvis = 0; for (int i = 0; i < 4; i++) if (btn[i].show) nvis++;
    int brow = cy + ch - 40, gap = 8;
    int bw = (cw - 32 - gap * (nvis - 1)) / (nvis > 0 ? nvis : 1), bh2 = 28, k = 0;
    for (int i = 0; i < 4; i++) {
        if (!btn[i].show) continue;
        int bx = cx + 16 + k++ * (bw + gap);
        ui_rounded_rect(bx, brow, bw, bh2, 6, btn[i].col);
        int lx = bx + (bw - (int)strlen(btn[i].l) * 6) / 2; if (lx < bx + 4) lx = bx + 4;
        ui_text(lx, brow + 7, FS_TINY, COL_WHITE, btn[i].l);
        btn[i].r[0] = bx; btn[i].r[1] = brow; btn[i].r[2] = bw; btn[i].r[3] = bh2;
    }
}

// Role picker: assign/remove the profile member's roles (Manage Roles).
static void draw_rolepick(void)
{
    if (!s_rolepick_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB0));
    int cw = 360, ch = 380, cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - ch) / 2;
    ui_rounded_rect(cx, cy, cw, ch, 12, COL_SIDEBAR);
    guild *g = store_find_guild(s_rp_gid);
    int vis_top = cy + 44, vis_bot = cy + ch - 8;
    int y = cy + 48 - s_rolepick_scroll;
    s_n_rp_hit = 0;
    if (g) for (int i = 0; i < g->role_count; i++) {
        const char *rn = store_role_name(g->role_ids[i]);
        if (!rn || !rn[0]) continue;
        if (y + 26 > vis_top && y < vis_bot) {
            int has = prof_has_role(g->role_ids[i]);
            unsigned dot = store_role_color(g->role_ids[i]); if (!dot) dot = COL_TEXT_MUTED;
            vita2d_draw_fill_circle(cx + 24, y + 13, 4, dot);
            ui_text_clip(cx + 40, y + 6, FS_SMALL, COL_TEXT, rn, cw - 40 - 46);
            ui_rounded_rect(cx + cw - 38, y + 3, 20, 20, 5, has ? COL_GREEN : COL_ELEVATED);
            if (has) ui_text(cx + cw - 34, y + 3, FS_TINY, COL_WHITE, "\xE2\x9C\x93");
            if (s_n_rp_hit < 64) { s_rp_hit[s_n_rp_hit].y0 = y; s_rp_hit[s_n_rp_hit].y1 = y + 26;
                snprintf(s_rp_hit[s_n_rp_hit].rid, 24, "%s", g->role_ids[i]);
                s_rp_hit[s_n_rp_hit].has = has; s_n_rp_hit++; }
        }
        y += 28;
    }
    ui_rect(cx, cy, cw, 40, COL_TOPBAR);  // header on top masks scroll
    ui_text_bold(cx + 16, cy + 12, FS_BODY, COL_TEXT_BRIGHT, "Manage Roles");
    ui_text(cx + cw - 74, cy + 14, FS_TINY, COL_TEXT_MUTED, "tap to close");
}

// Generic confirmation modal (Keep / destructive action).
static void draw_confirm(void)
{
    if (!s_confirm_open) return;
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xC0));
    int cw = 360, chh = 150, cx = (SCREEN_W - cw) / 2, cy = (SCREEN_H - chh) / 2;
    ui_rounded_rect(cx, cy, cw, chh, 12, COL_ELEVATED);
    ui_text_wrapped(cx + 20, cy + 22, FS_SMALL, COL_TEXT_BRIGHT, s_confirm_msg, cw - 40, 20);
    int bw = 150, bh = 34, by = cy + chh - 48;
    int bxl = cx + 20, bxr = cx + cw - 20 - bw;
    ui_rounded_rect(bxl, by, bw, bh, 6, COL_SIDEBAR);
    ui_text(bxl + bw / 2 - 18, by + 9, FS_SMALL, COL_TEXT_BRIGHT, "Keep");
    ui_rounded_rect(bxr, by, bw, bh, 6, COL_RED);
    ui_text(bxr + bw / 2 - ui_text_w(FS_SMALL, s_confirm_yes) / 2, by + 9, FS_SMALL, COL_WHITE, s_confirm_yes);
    s_confirm_no_r[0]=bxl;  s_confirm_no_r[1]=by;  s_confirm_no_r[2]=bw;  s_confirm_no_r[3]=bh;
    s_confirm_yes_r[0]=bxr; s_confirm_yes_r[1]=by; s_confirm_yes_r[2]=bw; s_confirm_yes_r[3]=bh;
}

// Live message box shown while the inline (libime) keyboard is up. The box and
// wrapping live in inlinekbd_draw; here we only decide the header.
static void draw_inline_composer(void)
{
    if (!s_compose_active) return;

    char hdr[160];
    if (s_compose_reply[0]) {
        snprintf(hdr, sizeof(hdr), "Replying");
    } else if (s_have_open) {
        bool dm = (s_open_ch.type == CH_DM || s_open_ch.type == CH_GROUP_DM);
        snprintf(hdr, sizeof(hdr), "Message %s%s", dm ? "" : "#", s_open_ch.name);
    } else {
        snprintf(hdr, sizeof(hdr), "Message");
    }
    inlinekbd_draw(hdr);
}

void browser_draw(void)
{
    imagecache_frame_begin();  // reset the "a GIF drew this frame" dirty signal
    ui_prime_glyphs();  // one-time; inside the draw frame (safe on Vita3K)
    icon_gc();  // free shaped-icon textures retired a couple frames ago
    g_msg_x = SIDE_X + (s_channels_collapsed ? 0 : SIDE_W);  // channel drawer state

    int gcount = 0;
    store_lock();
    guild *gs = store_guilds(&gcount);
    bool home = (s_guild < 0);
    guild *g = (!home && s_guild < gcount) ? &gs[s_guild] : NULL;
    build_rows(g, home);
    store_unlock();

    // right panel: the member list in a guild, or the friends list in DM/home --
    // unless collapsed via the header arrow / R button.
    s_member_w = s_members_collapsed ? 0 : MEMBER_W;

    draw_rail(gs, gcount);
    draw_sidebar(g, home);
    draw_messages();
    if (s_member_w) { if (home) draw_friends_panel(); else draw_members(); }
    draw_action_menu();
    draw_profile_popout();
    draw_settings();
    draw_pins();
    draw_threads_modal();
    draw_search();
    draw_newpost();
    draw_attach_sheet();
    draw_addserver();
    draw_forward();
    draw_mention();
    draw_accounts();
    draw_friends();
    draw_serverinfo();
    draw_rolepick();
    draw_confirm();
    draw_lightbox();
    draw_toast();
    draw_inline_composer();  // on top: the live message box under the keyboard
}
