#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>  // sceClibMspace*
#include <psp2/kernel/sysmem.h>  // sceKernelAllocMemBlock
#include <jansson.h>

#include "console.h"
#include "net_common.h"
#include "tls/https_get.h"
#include "tls/ca_bundle.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/qr.h"
#include "ui/ime.h"
#include "ui/imagecache.h"
#include "auth/remoteauth.h"
#include <psp2/gxm.h>
#include "auth/tokenstore.h"
#include "api/discord.h"
#include "gateway/gateway.h"
#include "model/store.h"
#include "ui/browser.h"
#include "qrcodegen.h"

// Heap for TLS + image/video buffers.
unsigned int _newlib_heap_size_user = 192 * 1024 * 1024;

typedef enum {
    ST_BOOT, ST_NONET, ST_QR, ST_VALIDATING, ST_HOME
} app_state;

static vita2d_pgf *g_font;
static char         g_cacert[256];
static char         g_token[128];
static discord_user g_me;
static ra_session   g_login;
static bool         g_login_active;
static app_state    g_state;
static char         g_msg[160];  // transient status line
static bool         g_app_started;  // gateway + browser initialised
static int          s_qrtoken_btn[4];  // "Log in with token" button hit rect
static account_t    s_qr_accts[ACCOUNTS_MAX];  // saved accounts on the login screen
static int          s_qr_nacct;
static int          s_qracct_hit[ACCOUNTS_MAX][4];  // per-row hit rects

static void begin_qr_login(void);  // fwd

static bool load_token_txt(char *out, size_t cap)
{
    SceUID fd = sceIoOpen("ux0:data/cordless/token.txt", SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    char buf[256];
    int r = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (r <= 0) return false;
    buf[r] = '\0';
    // trim trailing whitespace/newlines
    for (int i = r - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r' ||
                                   buf[i] == ' ' || buf[i] == '\t'); i--)
        buf[i] = '\0';
    if (buf[0] == '\0' || (size_t)strlen(buf) >= cap) return false;
    snprintf(out, cap, "%s", buf);
    return true;
}

// 200 keeps the token, 401 clears it, anything else retries.
static int g_validate_tries;  // consecutive attempts on the current token

// Mount photo0: and load the shutter-sound PRX for the import dialogs.
static void media_import_init(void)
{
    sceSysmoduleLoadModule(SCE_SYSMODULE_PHOTO_EXPORT);  // 0x0016
    sceSysmoduleLoadModule(SCE_SYSMODULE_SHUTTER_SOUND);  // 0x0027 - camera dialog dep
    SceAppUtilInitParam initp; SceAppUtilBootParam bootp;
    memset(&initp, 0, sizeof initp);
    memset(&bootp, 0, sizeof bootp);
    int ar = sceAppUtilInit(&initp, &bootp);
    int pr = sceAppUtilPhotoMount();
    console_dbgf("[media] appUtilInit=0x%08X photoMount=0x%08X", (unsigned)ar, (unsigned)pr);
}

static void validate_token(void)
{
    g_validate_tries++;
    int code = discord_get_me(g_token, g_cacert, &g_me);
    console_dbgf("[app] /users/@me -> HTTP %d (try %d)", code, g_validate_tries);

    if (code == 200) {
        g_validate_tries = 0;
        tokenstore_save(g_token);
        accounts_add(g_token, g_me.global_name[0] ? g_me.global_name : g_me.username,
                     g_me.id, g_me.avatar);
        snprintf(g_msg, sizeof(g_msg), "");
        if (!g_app_started) {
            store_set_me(g_me.username, g_me.global_name, g_me.id, g_me.avatar);
            gateway_start(g_token, g_cacert);
            browser_init(g_token, g_cacert);
            g_app_started = true;
        }
        g_state = ST_HOME;
    } else if (code == 401) {
        g_validate_tries = 0;
        console_logf("[app] token rejected (401) -- clearing saved login");
        tokenstore_clear();
        // drop the rejected token.txt too
        sceIoRemove("ux0:data/cordless/token.txt");
        g_token[0] = '\0';
        snprintf(g_msg, sizeof(g_msg), "Saved login expired - add a fresh token.");
        begin_qr_login();
    } else if (g_validate_tries < 8) {
        // transient (no network yet / 429 / 403 / 5xx): keep the token and retry
        snprintf(g_msg, sizeof(g_msg), "Reaching Discord\xE2\x80\xA6 (%d)", g_validate_tries);
        sceKernelDelayThread(1500 * 1000);
        g_state = ST_VALIDATING;
    } else {
        // give up auto-login; keep the token and fall back to QR
        g_validate_tries = 0;
        console_logf("[app] auto-login gave up (%d); token kept on card", code);
        snprintf(g_msg, sizeof(g_msg), "Can't reach Discord (%d). Saved login kept.", code);
        begin_qr_login();
    }
}

static void begin_qr_login(void)
{
    console_dbgf("[app] starting QR login");
    g_state = ST_QR;
    memset(&g_login, 0, sizeof(g_login));
    if (remoteauth_start(&g_login, g_cacert) == 0) {
        g_login_active = true;
        g_msg[0] = '\0';
    } else {
        snprintf(g_msg, sizeof(g_msg), "Connection problem - retrying...");
    }
}

static void try_manual_token(void)
{
    // Prompt for a token via the keyboard, pre-filled from token.txt if present.
    char tok[128], prefill[128];
    if (!load_token_txt(prefill, sizeof(prefill))) prefill[0] = '\0';
    if (ime_get_text("Paste your Discord token", prefill, tok, sizeof(tok))) {
        snprintf(g_token, sizeof(g_token), "%s", tok);
        g_state = ST_VALIDATING;
    }
}

// Log in / add an account via token (Triangle or the button). On cancel, the QR resumes.
static void do_manual_token(void)
{
    if (g_login_active) { remoteauth_join(&g_login); g_login_active = false; }
    try_manual_token();
    if (g_state == ST_QR) begin_qr_login();  // cancelled: bring the QR back
}

// Log straight into a saved account (accounts.bin) by its stored token.
static void pick_saved_account(int i)
{
    if (i < 0 || i >= s_qr_nacct) return;
    if (g_login_active) { remoteauth_join(&g_login); g_login_active = false; }
    snprintf(g_token, sizeof(g_token), "%s", s_qr_accts[i].token);
    g_state = ST_VALIDATING;
}

// Pick the first screen once the network is up: validate a saved/token.txt token, else QR.
static void boot_after_network(void)
{
    if (tokenstore_load(g_token, sizeof(g_token))) {
        console_dbgf("[app] stored token found; validating");
        g_state = ST_VALIDATING;
    } else if (load_token_txt(g_token, sizeof(g_token))) {
        // plaintext token; saved to session.bin once validated
        console_dbgf("[app] token.txt found; validating");
        g_state = ST_VALIDATING;
    } else {
        begin_qr_login();
    }
}


static const char *ra_status(ra_state st)
{
    switch (st) {
        case RA_CONNECTING:   return "Connecting to Discord...";
        case RA_WAIT_SCAN:    return "Scan this QR with the Discord phone app";
        case RA_WAIT_APPROVE: return "Scanned - tap Approve on your phone";
        case RA_EXCHANGING:   return "Finishing login...";
        case RA_DONE:         return "Success!";
        case RA_CANCELLED:    return "Declined on phone";
        case RA_FAILED:       return "Login failed";
        default:              return "";
    }
}

// Loading screen: wordmark, animated dot ring, status line.
static void draw_loading(const char *msg, int frame)
{
    int cx = SCREEN_W / 2, cy = SCREEN_H / 2 - 10;
    int tw = ui_text_bold_w(FS_TITLE, "Cordless");
    ui_text_bold(cx - tw / 2, cy - 66, FS_TITLE, COL_TEXT_BRIGHT, "Cordless");
    // spinner: 8 dots on a circle, brightness trailing a rotating head
    static const int ox[8] = { 18, 13, 0, -13, -18, -13, 0, 13 };
    static const int oy[8] = { 0, 13, 18, 13, 0, -13, -18, -13 };
    int head = (frame / 5) % 8;
    for (int i = 0; i < 8; i++) {
        int dist = (head - i + 8) % 8;
        int b = 255 - dist * 26; if (b < 45) b = 45;
        vita2d_draw_fill_circle(cx + ox[i], cy + oy[i], 4, RGBA8(0x58, 0x65, 0xf2, b));
    }
    if (msg && msg[0]) {
        int mw = ui_text_w(FS_BODY, msg);
        ui_text(cx - mw / 2, cy + 52, FS_BODY, COL_TEXT_MUTED, msg);
    }
}


static void draw_qr(void)
{
    // White card behind the QR (left), instructions (right).
    if (g_login.have_qr) {
        qr_draw(g_login.qr, 230, 285, 6);
    } else {
        ui_text(150, 275, FS_BODY, COL_TEXT_MUTED,
                g_msg[0] ? g_msg : "Preparing QR code...");
    }

    ui_text_bold(470, 96, FS_TITLE, COL_TEXT_BRIGHT, "Log in to Cordless");

    // Saved accounts (accounts.bin): tap one to log in; shown instead of the scan steps.
    s_qr_nacct = accounts_load(s_qr_accts, ACCOUNTS_MAX);
    int by;
    if (s_qr_nacct > 0) {
        ui_text(470, 138, FS_SMALL, COL_TEXT_MUTED, "Tap an account:");
        for (int i = 0; i < s_qr_nacct; i++) {
            int ry = 160 + i * 32;
            ui_rounded_rect(470, ry, 300, 28, 6, COL_ELEVATED);
            const char *nm = s_qr_accts[i].name[0] ? s_qr_accts[i].name : s_qr_accts[i].id;
            ui_text(484, ry + 6, FS_BODY, COL_TEXT_BRIGHT, nm);
            s_qracct_hit[i][0] = 470; s_qracct_hit[i][1] = ry;
            s_qracct_hit[i][2] = 300; s_qracct_hit[i][3] = 28;
        }
        by = 160 + s_qr_nacct * 32 + 16;
        if (by > 450) by = 450;
    } else {
        ui_text(470, 150, FS_BODY, COL_TEXT, "1.  Open Discord on your phone");
        ui_text(470, 180, FS_BODY, COL_TEXT, "2.  Settings -> Scan QR Code");
        ui_text(470, 210, FS_BODY, COL_TEXT, "3.  Point it at this code");
        by = 262;
    }

    // "Log in with token" / "Add another account" button.
    {
        int bw = 300, bh = 34;
        ui_rounded_rect(470, by, bw, bh, 8, COL_BLURPLE);
        const char *bl = s_qr_nacct > 0 ? "Add another account (token)" : "Log in with token";
        int lw = ui_text_bold_w(FS_BODY, bl);
        ui_text_bold(470 + bw / 2 - lw / 2, by + 9, FS_BODY, COL_TEXT_BRIGHT, bl);
        s_qrtoken_btn[0] = 470; s_qrtoken_btn[1] = by;
        s_qrtoken_btn[2] = bw; s_qrtoken_btn[3] = bh;
    }

    ui_text(470, by + 46, FS_SMALL, COL_BLURPLE, ra_status(g_login.state));
    if (g_login.user_display[0])
        ui_text(470, by + 66, FS_SMALL, COL_TEXT_MUTED, g_login.user_display);
}


static void pump_qr_login(void)
{
    if (!g_login_active) return;

    if (remoteauth_is_finished(&g_login)) {
        ra_state st = g_login.state;
        char tok[128];
        if (st == RA_DONE) snprintf(tok, sizeof(tok), "%s", g_login.token);
        remoteauth_join(&g_login);
        g_login_active = false;
        if (st == RA_DONE) {
            snprintf(g_token, sizeof(g_token), "%s", tok);
            g_state = ST_VALIDATING;
        } else {
            // QR expired / declined / dropped / cancelled: show a fresh QR.
            console_dbgf("[app] login attempt ended (%d); refreshing QR", st);
            begin_qr_login();
        }
    }
}

int main(void)
{
    // Pin the main/render thread to CPU core 0; workers run on other cores.
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
                                         SCE_KERNEL_CPU_MASK_USER_0);

    // 16 MiB GPU vertex pool, 4x MSAA.
    vita2d_init_advanced_with_msaa(16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);
    vita2d_set_clear_color(COL_CHAT);
    g_font = vita2d_load_default_pgf();
    console_init(g_font);
    store_init();

    net_app_path(g_cacert, sizeof(g_cacert), "data/cacert.pem");
    // Parse the CA bundle once; shared by every TLS connection.
    ca_bundle_init(g_cacert);

    char freg[256], fsemi[256], fatlas[256];
    net_app_path(freg,   sizeof(freg),   "data/fonts/Inter-Regular.ttf");
    net_app_path(fsemi,  sizeof(fsemi),  "data/fonts/Inter-SemiBold.ttf");
    net_app_path(fatlas, sizeof(fatlas), "data/emoji_atlas.png");
    if (!ui_init(freg, fsemi, fatlas)) console_logf("ui_init: font load FAILED");

    console_dbgf("Cordless booting...");
    // Bring up sceNet + net_lock before imagecache_init spawns its TLS worker.
    // ST_BOOT polls net_is_connected() while Wi-Fi associates.
    int net_mod_ok = (net_init() == 0);
    imagecache_init(g_cacert);
    media_import_init();  // mount photo0: + load shutter PRX for the attach dialogs
    g_state = net_mod_ok ? ST_BOOT : ST_NONET;
    if (!net_mod_ok) console_logf("[app] net_init FAILED");

#ifdef CORDLESS_AUTOEXIT
    // Headless gateway probe for Vita3K using the persisted token.
    net_wait_for_connection(15000);
    tokenstore_load(g_token, sizeof(g_token));
    console_logf("=== headless gateway probe ===");
    if (g_token[0]) {
        gateway_start(g_token, g_cacert);
        for (int i = 0; i < 100; i++) {  // wait up to ~20s for READY
            store_lock(); bool rdy = store_ready(); store_unlock();
            if (rdy) break;
            sceKernelDelayThread(200 * 1000);
        }
        store_lock();
        int gc = 0; guild *gs = store_guilds(&gc);
        console_logf("[test] READY=%d guilds=%d", store_ready(), gc);
        for (int i = 0; i < gc && i < 5; i++)
            console_logf("[test]   %s (%d channels)", gs[i].name, gs[i].channel_count);
        store_unlock();
        gateway_stop();
    } else {
        console_logf("[test] no stored token; log in first");
    }
    console_logf("=== headless probe done ===");
    for (int f = 0; f < 3; f++) {
        vita2d_start_drawing(); vita2d_clear_screen(); console_draw();
        vita2d_end_drawing(); vita2d_swap_buffers();
    }
    vita2d_fini(); net_finish(); sceKernelExitProcess(0);
#endif

    SceCtrlData pad, prev;
    memset(&prev, 0, sizeof(prev));
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    // Front-panel touch gesture state (tap vs drag).
    bool tw_down = false; int tw_dx = 0, tw_dy = 0, tw_ly = 0;
    bool tw_moved = false;

    // No in-app exit button: the render loop runs until the OS ends the process.
    int frame = 0;
    for (;;) {
        frame++;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned pressed = pad.buttons & ~prev.buttons;
        prev = pad;

        switch (g_state) {
            case ST_QR: {
                // Add an account / log in via token: Triangle or tap the button.
                if (pressed & SCE_CTRL_TRIANGLE) { do_manual_token(); break; }
                SceTouchData td;
                sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
                bool qdown = td.reportNum > 0;
                int qx = qdown ? td.report[0].x / 2 : 0;
                int qy = qdown ? td.report[0].y / 2 : 0;
                if (qdown && !tw_down) {
                    // Tap a saved-account row: log straight in from its token.
                    for (int i = 0; i < s_qr_nacct; i++) {
                        if (qx >= s_qracct_hit[i][0] && qx < s_qracct_hit[i][0] + s_qracct_hit[i][2] &&
                            qy >= s_qracct_hit[i][1] && qy < s_qracct_hit[i][1] + s_qracct_hit[i][3]) {
                            pick_saved_account(i);
                            break;
                        }
                    }
                    // Tap the "log in with token" button.
                    if (g_state == ST_QR && s_qrtoken_btn[2] &&
                        qx >= s_qrtoken_btn[0] && qx < s_qrtoken_btn[0] + s_qrtoken_btn[2] &&
                        qy >= s_qrtoken_btn[1] && qy < s_qrtoken_btn[1] + s_qrtoken_btn[3]) {
                        do_manual_token();
                    }
                }
                tw_down = qdown;
                break;
            }
            case ST_HOME: if (store_ready()) {
                // Skip UI input until READY is parsed.
                browser_input(pressed, pad.buttons, (int)pad.ly - 128);

                // Front touch: tap (select/open) + drag (scroll). The panel
                // reports 1920x1088 for a 960x544 screen, so halve.
                SceTouchData td;
                sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
                bool down = td.reportNum > 0;
                int tx = down ? td.report[0].x / 2 : 0;
                int ty = down ? td.report[0].y / 2 : 0;
                if (down) {
                    if (!tw_down) { tw_dx = tx; tw_dy = ty; tw_ly = ty; tw_moved = false; }
                    else {
                        if (abs(tx - tw_dx) > 10 || abs(ty - tw_dy) > 10) tw_moved = true;
                        // Scroll only on moves of 3px+; the anchor (tw_ly) advances only when we scroll.
                        if (tw_moved) {
                            int ddy = ty - tw_ly;
                            if (ddy <= -3 || ddy >= 3) {
                                browser_touch(tx, ty, ddy, false);
                                tw_ly = ty;
                            }
                        }
                    }
                } else if (tw_down && !tw_moved) {
                    browser_touch(tw_dx, tw_dy, 0, true);  // tap
                }
                tw_down = down;
            }
                // Gateway 4004 (token invalidated): stop gateway, clear saved login, back to QR.
                if (gateway_auth_failed()) {
                    console_logf("[app] gateway auth failed; clearing saved login");
                    gateway_stop();
                    g_app_started = false;
                    store_lock(); store_clear_guilds(); store_unlock();
                    if (g_me.id[0]) accounts_remove(g_me.id);
                    tokenstore_clear();
                    g_token[0] = '\0';
                    g_me.username[0] = '\0';
                    begin_qr_login();
                    snprintf(g_msg, sizeof(g_msg), "Discord signed you out \xE2\x80\x94 scan again.");
                    break;
                }
                // Logout via the Settings -> Log Out button.
                if (browser_wants_logout()) {
                    gateway_stop();
                    g_app_started = false;
                    store_lock(); store_clear_guilds(); store_unlock();
                    if (g_me.id[0]) accounts_remove(g_me.id);  // forget this account
                    tokenstore_clear();
                    g_token[0] = '\0';
                    g_me.username[0] = '\0';
                    begin_qr_login();
                }
                // Switch to another saved account: re-validate with its token.
                {
                    char swtok[128];
                    if (browser_pending_switch(swtok, sizeof(swtok))) {
                        gateway_stop();
                        g_app_started = false;
                        store_lock(); store_clear_guilds(); store_unlock();
                        snprintf(g_token, sizeof(g_token), "%s", swtok);
                        g_state = ST_VALIDATING;
                    } else if (browser_wants_add_account()) {
                        gateway_stop();
                        g_app_started = false;
                        store_lock(); store_clear_guilds(); store_unlock();
                        g_token[0] = '\0';
                        begin_qr_login();  // keep saved roster; add a new one
                    }
                }
                break;
            default: break;
        }

        pump_qr_login();
        imagecache_pump();  // upload any freshly-fetched avatars (main thread)

        // Poll for Wi-Fi and animate the spinner; boot_after_network() picks the next screen.
        if (g_state == ST_BOOT) {
            if (net_is_connected()) { boot_after_network(); }
            if (g_state == ST_BOOT) {
                static int waitf = 0; waitf++;
                vita2d_start_drawing();
                vita2d_clear_screen();
                draw_loading(waitf > 900 ? "Waiting for Wi-Fi\xE2\x80\xA6" : "Connecting\xE2\x80\xA6", frame);
                ui_prime_glyphs();  // start warming the glyph atlas
                vita2d_end_drawing();
                vita2d_swap_buffers();
                continue;
            }
        }

        // Blocking one-shot validation; draw a frame first for feedback.
        if (g_state == ST_VALIDATING) {
            vita2d_start_drawing();
            vita2d_clear_screen();
            draw_loading("Signing in\xE2\x80\xA6", frame);
            ui_prime_glyphs();
            vita2d_end_drawing();
            vita2d_swap_buffers();
            validate_token();
            continue;
        }

        // Idle-redraw skip: redraw home only on input, change, or animation (~150ms heartbeat).
        if (g_state == ST_HOME && store_ready()) {
            static unsigned s_last_rev = 0;
            static int      s_last_draw_f = 0;
            bool had_input = pressed || pad.buttons ||
                             pad.lx < 96 || pad.lx > 160 || pad.ly < 96 || pad.ly > 160 ||
                             tw_down;
            unsigned rev = store_msg_rev();
            bool need = had_input || browser_wants_redraw() ||
                        imagecache_animating() || imagecache_uploaded() ||
                        rev != s_last_rev || (frame - s_last_draw_f) >= 9;
            if (!need) { sceKernelDelayThread(16000); browser_ime_pump(); continue; }
            s_last_rev = rev; s_last_draw_f = frame;
        }

        vita2d_start_drawing();
        vita2d_clear_screen();
        switch (g_state) {
            case ST_NONET: ui_text(60, 120, FS_HEADER, COL_RED, "No network. Check Wi-Fi and relaunch."); break;
            case ST_QR:    draw_qr();      break;
            case ST_HOME: {
                // Show the loading screen until READY has populated the store.
                if (store_ready()) browser_draw();
                else {
                    draw_loading("Connecting to Discord\xE2\x80\xA6", frame);
                    ui_prime_glyphs();  // warm the glyph atlas during the idle load
                }
                break;
            }
            default:       draw_loading("Loading\xE2\x80\xA6", frame); break;
        }
        vita2d_end_drawing();
        vita2d_swap_buffers();

        // Pump the inline (libime) message keyboard outside the GXM scene,
        // after the swap. No-op unless a message is being composed.
        browser_ime_pump();
    }
}
