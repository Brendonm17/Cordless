#include "voice/voice.h"
#include "voice/voice_audio.h"
#include "voice/dave.h"
#include "gateway/gateway.h"
#include "tls/ws_client.h"
#include "net_common.h"
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include <jansson.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

// DAVE E2EE is gated by VOICE_DAVE_ENABLE (defined in voice_audio.h). With the
// gate off the client advertises max_dave_protocol_version 0 and creates no MLS session.

enum {
    DAVE_OP_PREPARE_TRANSITION     = 21,  // GW->C JSON  {protocol_version,transition_id}
    DAVE_OP_EXECUTE_TRANSITION     = 22,  // GW->C JSON  {transition_id}
    DAVE_OP_READY_FOR_TRANSITION   = 23,  // C->GW JSON  {transition_id}
    DAVE_OP_PREPARE_EPOCH          = 24,  // GW->C JSON  {protocol_version,epoch}
    DAVE_OP_EXTERNAL_SENDER        = 25,  // GW->C BIN   TLS ExternalSender
    DAVE_OP_KEY_PACKAGE            = 26,  // C->GW BIN   TLS KeyPackage
    DAVE_OP_PROPOSALS              = 27,  // GW->C BIN   TLS proposals -> we commit
    DAVE_OP_COMMIT_WELCOME         = 28,  // C->GW BIN   MLS Commit(+Welcome)
    DAVE_OP_ANNOUNCE_COMMIT        = 29,  // GW->C BIN   winning MLS Commit
    DAVE_OP_WELCOME                = 30,  // GW->C BIN   targeted MLS Welcome
    DAVE_OP_INVALID_COMMIT_WELCOME = 31,  // C->GW JSON  {transition_id}
};

static volatile int s_active;  // join requested, not yet left
static volatile int s_connected;  // session description received
static volatile int s_run;
static const char  *s_status = "Idle";

static char s_guild[24], s_chan[24], s_token[128], s_cacert[256];

// Server assignment from the main gateway (VOICE_SERVER_UPDATE + STATE_UPDATE).
static volatile int s_have_server;
static char s_vtoken[256], s_endpoint[128], s_session[64], s_user[24];

static unsigned      s_ssrc;
static unsigned char s_secret[32];
static int           s_have_secret;
static char          s_mode[48];
static int           s_udp = -1;
static char          s_local_ip[64];
static int           s_local_port;

static SceUID s_thread = -1;
static ws_client *s_vws;  // voice-gateway ws (op5 Speaking from the TX thread)

#if VOICE_DAVE_ENABLE
static dave_session *s_dave;
static int           s_dave_ver;  // negotiated DAVE protocol version (0 = off)
static int           s_dave_pending_tid;  // transition id awaiting op22 execute (-1 = none)

// Serializes MLS state access between the worker and audio threads.
static SceUID s_dave_mtx = -1;
static void dave_lock(void)   { if (s_dave_mtx >= 0) sceKernelLockMutex(s_dave_mtx, 1, NULL); }
static void dave_unlock(void) { if (s_dave_mtx >= 0) sceKernelUnlockMutex(s_dave_mtx, 1); }

static uint64_t str_to_u64(const char *s)
{
    return s ? (uint64_t)strtoull(s, NULL, 10) : 0;
}

// ssrc <-> user_id map, learned from incoming op5 Speaking events.
#define VOICE_SSRC_MAP_N 16
static struct { unsigned ssrc; char uid[24]; } s_ssrc_map[VOICE_SSRC_MAP_N];

static void dave_note_speaker(json_t *d)
{
    if (!d) return;
    unsigned ssrc = (unsigned)json_integer_value(json_object_get(d, "ssrc"));
    const char *uid = json_string_value(json_object_get(d, "user_id"));
    if (!ssrc || !uid) return;
    for (int i = 0; i < VOICE_SSRC_MAP_N; i++)
        if (s_ssrc_map[i].ssrc == ssrc) { snprintf(s_ssrc_map[i].uid, 24, "%s", uid); return; }
    for (int i = 0; i < VOICE_SSRC_MAP_N; i++)
        if (!s_ssrc_map[i].ssrc) { s_ssrc_map[i].ssrc = ssrc; snprintf(s_ssrc_map[i].uid, 24, "%s", uid); return; }
}

static const char *dave_uid_for_ssrc(unsigned ssrc)
{
    for (int i = 0; i < VOICE_SSRC_MAP_N; i++)
        if (s_ssrc_map[i].ssrc == ssrc) return s_ssrc_map[i].uid;
    return NULL;
}
#endif  // VOICE_DAVE_ENABLE

static int64_t now_ms(void)
{
    SceRtcTick t; sceRtcGetCurrentTick(&t);
    return (int64_t)(t.tick / 1000);
}

int         voice_active(void)     { return s_active; }
int         voice_connected(void)  { return s_connected; }
const char *voice_channel_id(void) { return s_active ? s_chan : ""; }
const char *voice_status(void)     { return s_status; }

static int vws_send(ws_client *ws, json_t *obj)
{
    char *txt = json_dumps(obj, JSON_COMPACT);
    if (!txt) return -1;
    net_lock();
    int r = ws_send_text(ws, txt, -1);
    net_unlock();
    free(txt);
    return r;
}

static int vws_read(ws_client *ws, char **buf, size_t *cap, int timeout_ms, int *is_binary)
{
    int64_t start = now_ms();
    for (;;) {
        if (!s_run) return -1;
        net_lock();
        int r = ws_recv_ex(ws, buf, cap, is_binary);
        net_unlock();
        if (r > 0) return r;
        if (r < 0) return -1;
        if (now_ms() - start > timeout_ms) return 0;
        sceKernelDelayThread(15 * 1000);
    }
}

// Called from the audio TX thread.
static void voice_send_speaking(int on)
{
    if (!s_vws) return;
    json_t *d = json_pack("{s:i, s:i, s:i}", "speaking", on ? 1 : 0,
                          "delay", 0, "ssrc", (json_int_t)s_ssrc);
    json_t *p = json_pack("{s:i, s:o}", "op", 5, "d", d);
    vws_send(s_vws, p);
    json_decref(p);
}

// Binary framing: GW->C [seq:u16 BE][opcode:u8][payload]; C->GW [opcode:u8][payload].
#if VOICE_DAVE_ENABLE

static void dave_send_binary(ws_client *ws, int op, const uint8_t *payload, size_t len)
{
    uint8_t *frame = malloc(len + 1);
    if (!frame) return;
    frame[0] = (uint8_t)op;
    if (len) memcpy(frame + 1, payload, len);
    net_lock();
    int r = ws_send_binary(ws, frame, (int)(len + 1));
    net_unlock();
    free(frame);
    console_logf("[dave] -> op%d (%u bytes)%s", op, (unsigned)len, r ? " SEND-FAIL" : "");
}

// Returns 1 if consumed, 0 to pass the message to the normal handlers.
static int dave_handle_json(ws_client *ws, int op, json_t *d)
{
    switch (op) {
    case DAVE_OP_PREPARE_TRANSITION: {
        int pv  = (int)json_integer_value(json_object_get(d, "protocol_version"));
        int tid = (int)json_integer_value(json_object_get(d, "transition_id"));
        console_logf("[dave] prepare_transition v=%d tid=%d", pv, tid);
        if (tid != 0) {
            // announce ready; the epoch/version switches on op22
            json_t *rd = json_pack("{s:i}", "transition_id", tid);
            json_t *p  = json_pack("{s:i, s:o}", "op", DAVE_OP_READY_FOR_TRANSITION, "d", rd);
            vws_send(ws, p); json_decref(p);
            s_dave_pending_tid = tid;
        } else {
            s_dave_ver = pv;  // tid 0: immediate reinit, no execute step
            if (pv == 0 && s_dave) dave_reset(s_dave);  // downgrade to passthrough
        }
        return 1;
    }
    case DAVE_OP_EXECUTE_TRANSITION: {
        int tid = (int)json_integer_value(json_object_get(d, "transition_id"));
        console_logf("[dave] execute_transition tid=%d", tid);
        s_dave_pending_tid = -1;  // the new epoch is applied via the MLS commit
        return 1;
    }
    case DAVE_OP_PREPARE_EPOCH: {
        int       pv    = (int)json_integer_value(json_object_get(d, "protocol_version"));
        json_int_t epoch = json_integer_value(json_object_get(d, "epoch"));
        console_logf("[dave] prepare_epoch v=%d epoch=%lld", pv, (long long)epoch);
        s_dave_ver = pv;
        if (s_dave && pv >= 1) {
            // epoch 1: we are the first member, create the group locally
            dave_prepare_epoch(s_dave, (uint64_t)epoch);
            uint8_t kp[8192]; size_t kl = 0;
            if (dave_make_key_package(s_dave, kp, &kl, sizeof(kp)) == 0)
                dave_send_binary(ws, DAVE_OP_KEY_PACKAGE, kp, kl);
            else
                console_logf("[dave] key_package build failed");
        }
        return 1;
    }
    default:
        return 0;
    }
}

static void dave_handle_binary(ws_client *ws, const uint8_t *msg, size_t n)
{
    if (n < 3) return;
    int          op = msg[2];
    const uint8_t *pl = msg + 3;
    size_t        pn = n - 3;
    console_logf("[dave] <- op%d (%u bytes)", op, (unsigned)pn);
    if (!s_dave) return;
    switch (op) {
    case DAVE_OP_EXTERNAL_SENDER:
        if (dave_on_external_sender(s_dave, pl, pn) != 0)
            console_logf("[dave] external_sender install failed");
        break;
    case DAVE_OP_PROPOSALS: {
        // op28 payload is [Commit][Welcome] concatenated
        uint8_t *cbuf = malloc(32768), *wbuf = malloc(32768);
        size_t cl = 0, wl = 0;
        if (cbuf && wbuf &&
            dave_on_proposals(s_dave, pl, pn, NULL, 0, cbuf, &cl, 32768, wbuf, &wl, 32768) == 0) {
            uint8_t *both = malloc(cl + wl);
            if (both) {
                memcpy(both, cbuf, cl);
                memcpy(both + cl, wbuf, wl);
                dave_send_binary(ws, DAVE_OP_COMMIT_WELCOME, both, cl + wl);
                free(both);
            }
        } else {
            console_logf("[dave] proposals->commit failed");
        }
        free(cbuf); free(wbuf);
        break;
    }
    case DAVE_OP_ANNOUNCE_COMMIT:
        if (dave_on_commit(s_dave, pl, pn) != 0)
            console_logf("[dave] announce_commit apply failed");
        break;
    case DAVE_OP_WELCOME:
        if (dave_on_welcome(s_dave, pl, pn, NULL, 0) != 0)
            console_logf("[dave] welcome join failed");
        break;
    default:
        console_logf("[dave] unhandled binary op%d", op);
        break;
    }
}

// Audio-thread key resolvers; return 0 until an epoch is established.
static int dave_my_key(uint8_t gen, uint8_t key[16])
{
    int r = 0;
    dave_lock();
    if (s_dave && dave_established(s_dave))
        r = (dave_sender_key(s_dave, s_user, gen, key) == 0);
    dave_unlock();
    return r;
}

static int dave_peer_key(unsigned ssrc, uint8_t gen, uint8_t key[16])
{
    int r = 0;
    dave_lock();
    if (s_dave && dave_established(s_dave)) {
        const char *uid = dave_uid_for_ssrc(ssrc);
        if (uid) r = (dave_sender_key(s_dave, uid, gen, key) == 0);
    }
    dave_unlock();
    return r;
}

static const voice_dave_hooks s_dave_audio_hooks = { dave_my_key, dave_peer_key };
#endif

static int udp_discover(const char *ip, int port, unsigned ssrc)
{
    s_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_udp < 0) return -1;
    struct sockaddr_in dst; memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET; dst.sin_port = htons((unsigned short)port);
    dst.sin_addr.s_addr = inet_addr(ip);
    if (connect(s_udp, (struct sockaddr *)&dst, sizeof(dst)) < 0) return -1;

    struct timeval tv = { 4, 0 };
    setsockopt(s_udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned char pkt[74]; memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x00; pkt[1] = 0x01;  // type: request
    pkt[2] = 0x00; pkt[3] = 70;  // length
    pkt[4] = (ssrc >> 24) & 0xFF; pkt[5] = (ssrc >> 16) & 0xFF;
    pkt[6] = (ssrc >> 8) & 0xFF;  pkt[7] = ssrc & 0xFF;

    net_lock();
    int sent = (int)send(s_udp, pkt, sizeof(pkt), 0);
    net_unlock();
    if (sent != (int)sizeof(pkt)) return -1;

    unsigned char resp[74];
    net_lock();
    int got = (int)recv(s_udp, resp, sizeof(resp), 0);
    net_unlock();
    if (got < 74) { console_logf("[voice] udp discovery no reply (%d)", got); return -1; }

    // address is a NUL-terminated string at offset 8; port is BE16 at offset 72
    resp[71] = '\0';
    snprintf(s_local_ip, sizeof(s_local_ip), "%.63s", (const char *)resp + 8);
    s_local_port = (resp[72] << 8) | resp[73];
    console_dbgf("[voice] discovered %s:%d", s_local_ip, s_local_port);
    return 0;
}

static const char *choose_mode(json_t *modes)
{
    const char *prefer[] = { "aead_xchacha20_poly1305_rtpsize",
                             "aead_aes256_gcm_rtpsize", NULL };
    for (int p = 0; prefer[p]; p++) {
        size_t n = json_array_size(modes);
        for (size_t i = 0; i < n; i++) {
            const char *m = json_string_value(json_array_get(modes, i));
            if (m && strcmp(m, prefer[p]) == 0) return prefer[p];
        }
    }
    const char *first = json_string_value(json_array_get(modes, 0));
    return first ? first : "aead_xchacha20_poly1305_rtpsize";
}


static void send_identify(ws_client *ws)
{
    // advertise our E2EE capability (0 when the gate is off)
    int dave_ver = VOICE_DAVE_ENABLE ? DAVE_PROTOCOL_VERSION : 0;
    json_t *d = json_pack("{s:s, s:s, s:s, s:s, s:i}",
                          "server_id", s_guild, "user_id", s_user,
                          "session_id", s_session, "token", s_vtoken,
                          "max_dave_protocol_version", dave_ver);
    json_t *p = json_pack("{s:i, s:o}", "op", 0, "d", d);
    vws_send(ws, p);
    json_decref(p);
    console_dbgf("[voice] IDENTIFY sent (guild=%s dave=%d)", s_guild, dave_ver);
}

static void send_select_protocol(ws_client *ws, const char *mode)
{
    snprintf(s_mode, sizeof(s_mode), "%s", mode);
    json_t *data = json_pack("{s:s, s:i, s:s}",
                             "address", s_local_ip, "port", s_local_port, "mode", mode);
    json_t *d = json_pack("{s:s, s:o}", "protocol", "udp", "data", data);
    json_t *p = json_pack("{s:i, s:o}", "op", 1, "d", d);
    vws_send(ws, p);
    json_decref(p);
    console_dbgf("[voice] SELECT PROTOCOL mode=%s", mode);
}

static int worker(SceSize argsz, void *argp)
{
    (void)argsz; (void)argp;

    // wait for the main gateway to deliver the voice server assignment
    int64_t start = now_ms();
    while (s_run && !s_have_server) {
        if (now_ms() - start > 10000) { console_logf("[voice] no server update"); goto done; }
        sceKernelDelayThread(30 * 1000);
    }
    if (!s_run) goto done;

    // build a wss URL from the endpoint (may arrive as bare host[:port])
    char url[192];
    snprintf(url, sizeof(url), "wss://%s/?v=8", s_endpoint);
    s_status = "Connecting";
    console_dbgf("[voice] connecting %s", url);

    net_lock();
    ws_client *ws = ws_open(url, s_cacert, "https://discord.com", NULL);
    net_unlock();
    if (!ws) { s_status = "Voice failed"; console_logf("[voice] ws connect failed"); goto done; }
    s_vws = ws;

#if VOICE_DAVE_ENABLE
    // create the MLS session; group id = the numeric voice channel id
    s_dave = NULL; s_dave_ver = 0; s_dave_pending_tid = -1;
    memset(s_ssrc_map, 0, sizeof s_ssrc_map);
    s_dave_mtx = sceKernelCreateMutex("cl_dave", 0, 0, NULL);
    s_dave = dave_session_create(s_user, str_to_u64(s_chan));
    if (!s_dave) console_logf("[dave] session create failed -- E2EE disabled this call");
#endif

    send_identify(ws);

    char *buf = NULL; size_t cap = 0;
    int hb = 0; int64_t last_hb = now_ms(); unsigned nonce = 1;
    int audio_started = 0; unsigned char audio_key[32] = {0};

    while (s_run) {
        int is_bin = 0;
        int n = vws_read(ws, &buf, &cap, 500, &is_bin);
        if (n < 0) { if (s_run) console_logf("[voice] ws closed by server (pre-connect=%d)", !s_connected); break; }
        if (n > 0 && is_bin) {  // binary frame (DAVE only)
#if VOICE_DAVE_ENABLE
            dave_lock();
            dave_handle_binary(ws, (const uint8_t *)buf, (size_t)n);
            dave_unlock();
#endif
            // DAVE off: ignore binary frames
        } else if (n > 0) {
            json_error_t e; json_t *root = json_loads(buf, 0, &e);
            if (root) {
                int op = (int)json_integer_value(json_object_get(root, "op"));
                json_t *d = json_object_get(root, "d");
                int dave_consumed = 0;
#if VOICE_DAVE_ENABLE
                if (op >= DAVE_OP_PREPARE_TRANSITION) {
                    dave_lock();
                    dave_consumed = dave_handle_json(ws, op, d);
                    dave_unlock();
                }
#endif
                if (dave_consumed) {
                    // consumed by the DAVE state machine
                } else if (op == 8) {  // Hello
                    hb = (int)json_number_value(json_object_get(d, "heartbeat_interval"));
                    if (hb < 1000) hb = 13750;
                    last_hb = now_ms();
                    console_dbgf("[voice] HELLO hb=%d", hb);
                } else if (op == 2) {  // Ready
                    s_ssrc = (unsigned)json_integer_value(json_object_get(d, "ssrc"));
                    const char *ip = json_string_value(json_object_get(d, "ip"));
                    int port = (int)json_integer_value(json_object_get(d, "port"));
                    json_t *modes = json_object_get(d, "modes");
                    console_dbgf("[voice] READY ssrc=%u %s:%d", s_ssrc, ip ? ip : "?", port);
                    if (ip && udp_discover(ip, port, s_ssrc) == 0)
                        send_select_protocol(ws, choose_mode(modes));
                    else { console_logf("[voice] discovery failed"); json_decref(root); break; }
                } else if (op == 4) {  // Session Description
                    const char *mode = json_string_value(json_object_get(d, "mode"));
                    json_t *key = json_object_get(d, "secret_key");
                    if (json_is_array(key)) {
                        size_t kn = json_array_size(key);
                        for (size_t i = 0; i < kn && i < sizeof(s_secret); i++)
                            s_secret[i] = (unsigned char)json_integer_value(json_array_get(key, i));
                        s_have_secret = 1;
                    }
                    if (mode) snprintf(s_mode, sizeof(s_mode), "%s", mode);
#if VOICE_DAVE_ENABLE
                    json_t *dpv = json_object_get(d, "dave_protocol_version");
                    if (dpv) { s_dave_ver = (int)json_integer_value(dpv);
                               console_logf("[voice] negotiated dave_protocol_version=%d", s_dave_ver); }
#endif
                    s_connected = 1; s_status = "Connected";
                    console_dbgf("[voice] SESSION DESCRIPTION mode=%s key=%d bytes -> CONNECTED",
                                 s_mode, s_have_secret ? 32 : 0);
                    // start audio only when the secret_key changes
                    if (s_have_secret && s_udp >= 0 &&
                        (!audio_started || memcmp(audio_key, s_secret, 32) != 0)) {
                        if (audio_started) voice_audio_stop();
#if VOICE_DAVE_ENABLE
                        // register the E2EE key resolvers before the audio threads start
                        voice_audio_set_dave(&s_dave_audio_hooks);
#endif
                        voice_audio_start(s_udp, s_ssrc, s_secret, s_mode, voice_send_speaking);
                        memcpy(audio_key, s_secret, 32);
                        audio_started = 1;
                    }
                } else if (op == 5) {  // incoming Speaking: ssrc<->user_id
#if VOICE_DAVE_ENABLE
                    dave_note_speaker(d);
#endif
                } else if (op == 6) {  // Heartbeat ACK: ignore
                }
                json_decref(root);
            }
        }
        if (hb > 0 && now_ms() - last_hb >= hb) {
            json_t *p = json_pack("{s:i, s:i}", "op", 3, "d", (json_int_t)nonce++);
            vws_send(ws, p);
            json_decref(p);
            last_hb = now_ms();
        }
    }

    voice_audio_stop();
    s_vws = NULL;
#if VOICE_DAVE_ENABLE
    voice_audio_set_dave(NULL);
    dave_session_free(s_dave); s_dave = NULL;
#endif
    free(buf);
    net_lock();
    ws_close(ws);
    net_unlock();

done:
    voice_audio_stop();
    s_vws = NULL;
#if VOICE_DAVE_ENABLE
    voice_audio_set_dave(NULL);
    if (s_dave_mtx >= 0) { sceKernelDeleteMutex(s_dave_mtx); s_dave_mtx = -1; }
    dave_session_free(s_dave); s_dave = NULL;
#endif
    if (s_udp >= 0) { close(s_udp); s_udp = -1; }
    s_connected = 0;
    if (s_active) s_status = "Disconnected";
    return 0;
}


void voice_on_server(const char *token, const char *endpoint,
                     const char *session_id, const char *guild_id,
                     const char *user_id)
{
    if (!s_active) return;
    if (guild_id && s_guild[0] && strcmp(guild_id, s_guild) != 0) return;
    snprintf(s_vtoken, sizeof(s_vtoken), "%s", token ? token : "");
    snprintf(s_endpoint, sizeof(s_endpoint), "%s", endpoint ? endpoint : "");
    snprintf(s_session, sizeof(s_session), "%s", session_id ? session_id : "");
    snprintf(s_user, sizeof(s_user), "%s", user_id ? user_id : "");
    s_have_server = 1;
    console_dbgf("[voice] server assigned: %s", s_endpoint);
}

int voice_join(const char *guild_id, const char *channel_id,
               const char *token, const char *cacert_path)
{
    if (!guild_id || !channel_id || !channel_id[0]) return -1;
    if (!VOICE_JOIN_ENABLE) {
        console_logf("[voice] join refused: voice disabled (DAVE E2EE required)");
        return -1;
    }
    if (s_active) voice_leave();

    // run the crypto self-tests once, on first join
    static int s_crypto_checked;
    if (!s_crypto_checked) {
        s_crypto_checked = 1;
        voice_audio_selftest();
#if VOICE_DAVE_ENABLE
        dave_selftest();
#endif
    }

    snprintf(s_guild, sizeof(s_guild), "%s", guild_id);
    snprintf(s_chan, sizeof(s_chan), "%s", channel_id);
    snprintf(s_token, sizeof(s_token), "%s", token ? token : "");
    snprintf(s_cacert, sizeof(s_cacert), "%s", cacert_path ? cacert_path : "");
    s_have_server = 0; s_have_secret = 0; s_connected = 0;
    s_active = 1; s_run = 1; s_status = "Requesting";

    // ask the main gateway to move us into the voice channel (op4)
    gateway_voice_update(guild_id, channel_id);

    s_thread = sceKernelCreateThread("cl_voice", worker, 0x40, 0x40000, 0, 0, NULL);
    if (s_thread < 0) { s_active = 0; s_run = 0; s_status = "Idle"; return -1; }
    sceKernelStartThread(s_thread, 0, NULL);
    console_dbgf("[voice] join #%s", channel_id);
    return 0;
}

void voice_leave(void)
{
    if (!s_active) return;
    console_dbgf("[voice] leaving");
    // tell the gateway to leave the voice channel (op4, empty channel)
    gateway_voice_update(s_guild, "");
    s_run = 0;
    if (s_thread >= 0) {
        sceKernelWaitThreadEnd(s_thread, NULL, NULL);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    s_active = 0; s_connected = 0; s_have_server = 0;
    s_chan[0] = '\0';
    s_status = "Idle";
}
