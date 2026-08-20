#include "auth/remoteauth.h"
#include "auth/racrypto.h"
#include "api/discord.h"
#include "ui/qr.h"
#include "net_common.h"
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tls/ws_client.h"
#include "tls/https_get.h"
#include <jansson.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

#define RA_WSS "wss://remote-auth-gateway.discord.gg/?v=2"
#define RA_LOGIN_URL "https://discord.com/api/v10/users/@me/remote-auth/login"
#define CORDLESS_UA "Mozilla/5.0 (PlayStation Vita) Cordless/0.1"

// Worker context: the public session plus private handles.
typedef struct {
    ra_session  *s;
    char         cacert[256];
    ra_keypair  *kp;
    ws_client   *ws;
    SceUID       thread;
    int          heartbeat_ms;
    // Session identity (fingerprint + cookies) from the pre-login /experiments handshake.
    char         fingerprint[64];
    char         cookies[600];
} ra_ctx;

static void set_state(ra_session *s, ra_state st) { s->state = st; }

static void fail(ra_session *s, const char *msg)
{
    snprintf(s->error, sizeof(s->error), "%s", msg);
    console_logf("[ra] FAIL: %s", msg);
    set_state(s, RA_FAILED);
}


static int ws_send_json(ws_client *ws, json_t *obj)
{
    char *txt = json_dumps(obj, JSON_COMPACT);
    if (!txt) return -1;
    int rc = ws_send_text(ws, txt, -1);
    free(txt);
    return rc;
}

// Receive one full text frame into *buf (grown, NUL-terminated).
// Returns length, 0 if none yet, -1 on error/closed.
static int ws_recv_frame(ws_client *ws, char **buf, size_t *cap)
{
    return ws_recv(ws, buf, cap);
}

static int64_t now_ms(void)
{
    SceRtcTick t;
    sceRtcGetCurrentTick(&t);
    return (int64_t)(t.tick / 1000);  // tick is microseconds
}


static int send_init(ra_ctx *c)
{
    char pub_b64[1024];
    if (ra_export_public_b64(c->kp, pub_b64, sizeof(pub_b64)) < 0) return -1;
    json_t *o = json_pack("{s:s, s:s}", "op", "init",
                          "encoded_public_key", pub_b64);
    int rc = ws_send_json(c->ws, o);
    json_decref(o);
    return rc;
}

static int handle_nonce_proof(ra_ctx *c, json_t *root)
{
    const char *enc = json_string_value(json_object_get(root, "encrypted_nonce"));
    if (!enc) return -1;

    unsigned char nonce[256];
    int nlen = ra_oaep_decrypt_b64(c->kp, enc, nonce, sizeof(nonce));
    if (nlen < 0) return -1;

    char proof[128];
    if (ra_sha256_b64url(nonce, nlen, proof, sizeof(proof)) < 0) return -1;

    json_t *o = json_pack("{s:s, s:s}", "op", "nonce_proof", "proof", proof);
    int rc = ws_send_json(c->ws, o);
    json_decref(o);
    console_dbgf("[ra] nonce_proof sent");
    return rc;
}

static int handle_pending_remote_init(ra_ctx *c, json_t *root)
{
    const char *fp = json_string_value(json_object_get(root, "fingerprint"));
    if (!fp) return -1;

    ra_session *s = c->s;
    snprintf(s->fingerprint, sizeof(s->fingerprint), "%s", fp);

    char url[192];
    snprintf(url, sizeof(url), "https://discord.com/ra/%s", fp);
    if (qr_encode(url, s->qr)) {
        s->have_qr = true;
        set_state(s, RA_WAIT_SCAN);
        console_dbgf("[ra] fingerprint=%s", fp);
        console_dbgf("[ra] QR ready - scan with the Discord phone app");
        qr_dump_ascii(s->qr);
    } else {
        return -1;
    }
    return 0;
}

static void handle_pending_ticket(ra_ctx *c, json_t *root)
{
    const char *enc = json_string_value(
        json_object_get(root, "encrypted_user_payload"));
    ra_session *s = c->s;
    if (enc) {
        unsigned char pl[256];
        int n = ra_oaep_decrypt_b64(c->kp, enc, pl, sizeof(pl) - 1);
        if (n > 0) {
            pl[n] = '\0';
            // payload: id:discriminator:avatar:username
            char *save = NULL;
            char *id   = strtok_r((char *)pl, ":", &save);
            strtok_r(NULL, ":", &save);  // discriminator
            strtok_r(NULL, ":", &save);  // avatar
            char *user = strtok_r(NULL, ":", &save);
            snprintf(s->user_display, sizeof(s->user_display), "%s (%s)",
                     user ? user : "?", id ? id : "?");
            console_dbgf("[ra] scanned by %s", s->user_display);
        }
    }
    set_state(s, RA_WAIT_APPROVE);
}

// POST the ticket to exchange it for the (encrypted) token, then decrypt.
static void exchange_token(ra_ctx *c, const char *ticket)
{
    ra_session *s = c->s;
    set_state(s, RA_EXCHANGING);
    console_dbgf("[ra] approved; exchanging ticket for token");

    char *body = NULL;
    json_t *req = json_pack("{s:s}", "ticket", ticket);
    body = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    if (!body) { fail(s, "oom"); return; }

    char sprops[1200], xsp[1400];
    discord_superprops_b64(sprops, sizeof(sprops));
    snprintf(xsp, sizeof(xsp), "X-Super-Properties: %s", sprops);
    char ua[256]; snprintf(ua, sizeof(ua), "User-Agent: %s", discord_user_agent());
    // Carry the pre-login session identity: fingerprint (required until auth
    // completes) and Cloudflare cookies.
    char xfp[96] = "", cookie[700] = "";
    if (c->fingerprint[0]) snprintf(xfp, sizeof(xfp), "X-Fingerprint: %s", c->fingerprint);
    if (c->cookies[0])     snprintf(cookie, sizeof(cookie), "Cookie: %s", c->cookies);
    const char *hdrs[12]; int hi = 0;
    hdrs[hi++] = ua;
    hdrs[hi++] = "Content-Type: application/json";
    hdrs[hi++] = xsp;
    hdrs[hi++] = "Accept: application/json";
    hdrs[hi++] = "X-Discord-Locale: en-US";
    hdrs[hi++] = "X-Discord-Timezone: America/New_York";
    hdrs[hi++] = "X-Debug-Options: bugReporterEnabled";
    if (xfp[0])    hdrs[hi++] = xfp;
    if (cookie[0]) hdrs[hi++] = cookie;
    hdrs[hi] = NULL;

    unsigned char *out = NULL; int outlen = 0; long code = 0;
    net_lock();
    int rc = https_request("POST", RA_LOGIN_URL, c->cacert, hdrs,
                           (const unsigned char *)body, (int)strlen(body),
                           &out, &outlen, &code, 1 << 20);
    net_unlock();
    char *resp_p = (char *)out;
    console_dbgf("[ra] login POST rc=%d http=%ld", rc, code);
    if (code != 200 && resp_p) {
        console_logf("[ra] resp: %.100s", resp_p);
    }

    if (rc == 0 && code == 200 && resp_p) {
        json_error_t err;
        json_t *jr = json_loads(resp_p, 0, &err);
        const char *enc = jr ? json_string_value(
            json_object_get(jr, "encrypted_token")) : NULL;
        if (enc) {
            console_dbgf("[ra] encrypted_token b64 len=%d", (int)strlen(enc));
            unsigned char tok[256];
            int tn = ra_oaep_decrypt_b64(c->kp, enc, tok, sizeof(tok) - 1);
            if (tn > 0) {
                tok[tn] = '\0';
                snprintf(s->token, sizeof(s->token), "%s", (char *)tok);
                set_state(s, RA_DONE);
                console_dbgf("[ra] TOKEN acquired (%d chars). Login complete.", tn);
            } else {
                fail(s, "token decrypt failed");
            }
        } else {
            fail(s, "no encrypted_token in response");
        }
        if (jr) json_decref(jr);
    } else {
        char m[96];
        snprintf(m, sizeof(m), "login POST failed (rc=%d http=%ld)", rc, code);
        fail(s, m);
    }

    free(resp_p);
    free(body);
}


static void process_message(ra_ctx *c, const char *txt)
{
    json_error_t err;
    json_t *root = json_loads(txt, 0, &err);
    if (!root) { console_logf("[ra] bad JSON frame"); return; }

    const char *op = json_string_value(json_object_get(root, "op"));
    if (!op) { json_decref(root); return; }

    if (strcmp(op, "hello") == 0) {
        json_t *hb = json_object_get(root, "heartbeat_interval");
        c->heartbeat_ms = hb ? (int)json_integer_value(hb) : 41250;
        console_dbgf("[ra] hello (heartbeat=%dms); sending init", c->heartbeat_ms);
        if (send_init(c) != 0) fail(c->s, "init send failed");
    } else if (strcmp(op, "nonce_proof") == 0) {
        if (handle_nonce_proof(c, root) != 0) fail(c->s, "nonce_proof failed");
    } else if (strcmp(op, "pending_remote_init") == 0) {
        if (handle_pending_remote_init(c, root) != 0) fail(c->s, "qr/init failed");
    } else if (strcmp(op, "pending_ticket") == 0) {
        handle_pending_ticket(c, root);
    } else if (strcmp(op, "pending_login") == 0) {
        const char *ticket = json_string_value(json_object_get(root, "ticket"));
        if (ticket) exchange_token(c, ticket);
        else fail(c->s, "pending_login without ticket");
    } else if (strcmp(op, "cancel") == 0) {
        console_logf("[ra] user declined on phone");
        set_state(c->s, RA_CANCELLED);
    } else {
        console_logf("[ra] op=%s (ignored)", op);
    }
    json_decref(root);
}

static int worker(SceSize args, void *argp)
{
    ra_ctx *c = *(ra_ctx **)argp;
    ra_session *s = c->s;
    console_dbgf("[ra] worker thread started");

    // Warm up a client session: fetch a fingerprint + Cloudflare cookies.
    // Best-effort; on failure we still attempt the login.
    discord_auth_preflight(c->cacert, c->fingerprint, sizeof(c->fingerprint),
                           c->cookies, sizeof(c->cookies));

    console_dbgf("[ra] connecting remote-auth gateway...");
    c->ws = ws_open(RA_WSS, c->cacert, "https://discord.com", CORDLESS_UA);
    if (!c->ws) { fail(s, "remote-auth WS upgrade failed"); return 0; }
    set_state(s, RA_CONNECTING);

    int64_t last_hb = now_ms();
    char *frame = NULL; size_t fcap = 0;
    while (s->state != RA_DONE && s->state != RA_FAILED &&
           s->state != RA_CANCELLED) {
        int n = ws_recv_frame(c->ws, &frame, &fcap);
        if (n < 0) { fail(s, "ws closed"); break; }
        if (n > 0) process_message(c, frame);

        int64_t t = now_ms();
        if (c->heartbeat_ms > 0 && t - last_hb >= c->heartbeat_ms) {
            json_t *o = json_pack("{s:s}", "op", "heartbeat");
            ws_send_json(c->ws, o);
            json_decref(o);
            last_hb = t;
        }
        sceKernelDelayThread(20 * 1000);  // 20ms poll
    }

    free(frame);
    ws_close(c->ws);
    c->ws = NULL;
    return 0;
}


static ra_ctx *g_ctx;  // single in-flight login at a time

int remoteauth_start(ra_session *s, const char *cacert_path)
{
    memset(s, 0, sizeof(*s));
    set_state(s, RA_CONNECTING);

    if (ra_crypto_seed() != 0) { fail(s, "RNG seed"); return -1; }

    ra_ctx *c = calloc(1, sizeof(*c));
    if (!c) { fail(s, "oom"); return -1; }
    c->s = s;
    snprintf(c->cacert, sizeof(c->cacert), "%s", cacert_path);
    c->kp = ra_keypair_new();
    if (!c->kp) { free(c); fail(s, "keygen"); return -1; }
    c->heartbeat_ms = 41250;
    g_ctx = c;

    // Generous stack: TLS handshakes are stack-hungry.
    SceUID th = sceKernelCreateThread("ra_worker", worker,
                                      0x40, 0x100000, 0, 0, NULL);
    if (th < 0) { ra_keypair_free(c->kp); free(c); g_ctx = NULL;
                  fail(s, "thread create"); return -1; }
    c->thread = th;
    sceKernelStartThread(th, sizeof(ra_ctx *), &c);
    return 0;
}

bool remoteauth_is_finished(const ra_session *s)
{
    return s->state == RA_DONE || s->state == RA_FAILED ||
           s->state == RA_CANCELLED;
}

void remoteauth_join(ra_session *s)
{
    (void)s;
    if (!g_ctx) return;
    sceKernelWaitThreadEnd(g_ctx->thread, NULL, NULL);
    sceKernelDeleteThread(g_ctx->thread);
    if (g_ctx->kp) ra_keypair_free(g_ctx->kp);
    free(g_ctx);
    g_ctx = NULL;
}
