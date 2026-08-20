#include "api/discord.h"
#include "util/b64.h"
#include "model/store.h"
#include "net_common.h"
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strcasecmp

#include "tls/https_get.h"
#include <jansson.h>

#include <psp2/io/fcntl.h>  // sceIoOpen/Read for multipart uploads
#include <psp2/kernel/threadmgr.h>  // sceKernelDelayThread for 429 backoff

// A recent Discord desktop (Electron) UA.
#define UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " \
           "(KHTML, like Gecko) discord/1.0.9040 Chrome/128.0.6613.186 " \
           "Electron/32.2.5 Safari/537.36"

// Client super-properties (gateway + REST send the same object).
// Override client_build_number per release with -DCLIENT_BUILD_NUMBER=<n>.
#ifndef CLIENT_BUILD_NUMBER
#define CLIENT_BUILD_NUMBER 396858
#endif
#define CL_STR2(x) #x
#define CL_STR(x)  CL_STR2(x)
#define SUPERPROPS_JSON \
    "{\"os\":\"Windows\",\"browser\":\"Discord Client\"," \
    "\"release_channel\":\"stable\",\"client_version\":\"1.0.9040\"," \
    "\"os_version\":\"10.0.22631\",\"os_arch\":\"x64\"," \
    "\"app_arch\":\"x64\",\"system_locale\":\"en-US\"," \
    "\"browser_user_agent\":\"" UA "\"," \
    "\"browser_version\":\"32.2.5\",\"client_build_number\":" CL_STR(CLIENT_BUILD_NUMBER) "," \
    "\"native_build_number\":63451,\"client_event_source\":null}"

const char *discord_user_agent(void) { return UA; }

int discord_superprops_json(char *out, size_t cap)
{
    size_t n = strlen(SUPERPROPS_JSON);
    if (n + 1 > cap) return -1;
    memcpy(out, SUPERPROPS_JSON, n + 1);
    return (int)n;
}

int discord_superprops_b64(char *out, size_t cap)
{
    const char *j = SUPERPROPS_JSON;
    size_t n = strlen(j);
    if (4 * ((n + 2) / 3) + 1 > cap) return -1;
    return (int)b64_encode((const unsigned char *)j, n, out);
}

// Unauthenticated preflight: GET /experiments for the fingerprint and Cloudflare
// cookies, carried through auth. Fills fingerprint/cookies; returns 0 on success.
int discord_auth_preflight(const char *cacert_path, char *fingerprint, size_t fp_cap,
                           char *cookies, size_t ck_cap)
{
    if (fingerprint && fp_cap) fingerprint[0] = '\0';
    if (cookies && ck_cap) cookies[0] = '\0';

    char sprops[1200], xsp[1400];
    discord_superprops_b64(sprops, sizeof(sprops));
    snprintf(xsp, sizeof(xsp), "X-Super-Properties: %s", sprops);
    const char *hdrs[] = {
        "User-Agent: " UA, xsp, "Accept: */*", "Accept-Language: en-US",
        "X-Discord-Locale: en-US", "X-Discord-Timezone: America/New_York", NULL
    };

    unsigned char *out = NULL; int outlen = 0; long code = -1;
    net_lock();
    int r = https_request_cookies("GET", DISCORD_API_BASE "/experiments", cacert_path,
                                  hdrs, NULL, 0, &out, &outlen, &code, 256 * 1024,
                                  cookies, ck_cap);
    net_unlock();
    if (r != 0 || code != 200 || !out) {
        free(out);
        console_logf("[auth] preflight failed (rc=%d http=%ld)", r, code);
        return -1;
    }

    int ok = -1;
    json_error_t err;
    json_t *root = json_loads((char *)out, 0, &err);
    if (root) {
        const char *fp = json_string_value(json_object_get(root, "fingerprint"));
        if (fp && fp[0] && fingerprint && fp_cap) {
            snprintf(fingerprint, fp_cap, "%s", fp);
            ok = 0;
        }
        json_decref(root);
    }
    free(out);
    console_dbgf("[auth] preflight ok fp=%s cookies=%dB", ok == 0 ? "yes" : "no",
                 cookies ? (int)strlen(cookies) : 0);
    return ok;
}

// Parse retry_after (seconds) from a 429 body; -1 if absent.
static double parse_retry_after(const char *body, int len)
{
    if (!body || len <= 0) return -1.0;
    json_t *j = json_loads(body, 0, NULL);
    if (!j) return -1.0;
    json_t *ra = json_object_get(j, "retry_after");
    double v = json_is_number(ra) ? json_number_value(ra) : -1.0;
    json_decref(j);
    return v;
}

// Authenticated REST call. Returns HTTP status (-1 on transport failure); on
// success *resp gets a malloc'd body (caller frees). Waits out 429 retry_after.
static long rest_call(const char *method, const char *url, const char *body,
                      const char *token, const char *cacert_path, char **resp)
{
    if (resp) *resp = NULL;
    char sprops[1200];
    discord_superprops_b64(sprops, sizeof(sprops));
    char auth[300], xsp[1400];
    snprintf(auth, sizeof(auth), "Authorization: %s", token);
    snprintf(xsp, sizeof(xsp), "X-Super-Properties: %s", sprops);
    const char *hdrs[11]; int hi = 0;
    hdrs[hi++] = "User-Agent: " UA;
    hdrs[hi++] = auth;
    hdrs[hi++] = xsp;
    hdrs[hi++] = "Accept: application/json";
    hdrs[hi++] = "X-Discord-Locale: en-US";
    hdrs[hi++] = "X-Discord-Timezone: America/New_York";
    hdrs[hi++] = "X-Debug-Options: bugReporterEnabled";
    hdrs[hi++] = "Accept-Language: en-US";
    if (body) hdrs[hi++] = "Content-Type: application/json";
    hdrs[hi] = NULL;

    for (int attempt = 0; ; attempt++) {
        unsigned char *out = NULL; int outlen = 0; long code = -1;
        net_lock();
        int r = https_request(method, url, cacert_path, hdrs,
                              (const unsigned char *)body, body ? (int)strlen(body) : 0,
                              &out, &outlen, &code, 6 * 1024 * 1024);
        net_unlock();
        if (r != 0) { free(out); console_logf("[api] %s transport err", method); return -1; }
        if (code == 429 && attempt < 3) {
            double wait = parse_retry_after((const char *)out, outlen);
            free(out);
            if (wait < 0.0)  wait = 1.0;  // no/garbled body: gentle default
            if (wait > 30.0) wait = 30.0;  // safety cap against a bogus value
            console_logf("[api] 429 rate-limited; waiting %.2fs then retry (%d/3)",
                         wait, attempt + 1);
            sceKernelDelayThread((SceUInt)(wait * 1000000.0) + 50000);
            continue;
        }
        if (resp) *resp = (char *)out; else free(out);
        return code;
    }
}

// Percent-encode `s` (RFC 3986 unreserved preserved) into `out`; for URL path
// segments such as a reaction emoji.
static void url_encode(const char *s, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = '\0';
}

int discord_get_me(const char *token, const char *cacert_path,
                   discord_user *user)
{
    char *resp = NULL;
    long code = rest_call("GET", DISCORD_API_BASE "/users/@me", NULL, token, cacert_path, &resp);
    if (code == 200 && resp && user) {
        json_error_t err;
        json_t *root = json_loads(resp, 0, &err);
        if (root) {
            memset(user, 0, sizeof(*user));
            const char *id = json_string_value(json_object_get(root, "id"));
            const char *un = json_string_value(json_object_get(root, "username"));
            const char *gn = json_string_value(json_object_get(root, "global_name"));
            if (id) snprintf(user->id, sizeof(user->id), "%s", id);
            if (un) snprintf(user->username, sizeof(user->username), "%s", un);
            if (gn) snprintf(user->global_name, sizeof(user->global_name), "%s", gn);
            const char *av = json_string_value(json_object_get(root, "avatar"));
            if (av) snprintf(user->avatar, sizeof(user->avatar), "%s", av);
            json_decref(root);
        }
    }
    free(resp);
    return (int)code;
}

static const char *author_display(json_t *author)
{
    if (!author) return NULL;
    const char *gn = json_string_value(json_object_get(author, "global_name"));
    const char *un = json_string_value(json_object_get(author, "username"));
    return (gn && gn[0]) ? gn : un;
}

// display name of a mentioned user id from the message's `mentions` array
static const char *mention_name(json_t *mentions, const char *id)
{
    if (!json_is_array(mentions) || !id) return NULL;
    size_t n = json_array_size(mentions);
    for (size_t i = 0; i < n; i++) {
        json_t *u = json_array_get(mentions, i);
        const char *uid = json_string_value(json_object_get(u, "id"));
        if (uid && strcmp(uid, id) == 0) return author_display(u);
    }
    return NULL;
}

// Rewrite mention tokens to text: <@id>/<@!id> -> @name, <@&id> -> @role,
// <#id> -> #channel, <t:..> -> HH:MM. mark wraps mentions in \x01..\x02 pills.
static void resolve_mentions(const char *raw, json_t *mentions, char *out, size_t cap,
                             int mark)
{
    if (!raw) { out[0] = '\0'; return; }
    size_t o = 0;
    for (const char *p = raw; *p && o < cap - 1; ) {
        if (*p == '<') {
            const char *end = strchr(p, '>');
            if (end && (size_t)(end - p) < 48) {
                char rep[100] = "";
                char idbuf[24];
                int is_mention = 0;
                if (p[1] == '#') {
                    int n = (int)(end - (p + 2));
                    if (n > 0 && n < 24) { memcpy(idbuf, p + 2, n); idbuf[n] = '\0';
                        channel *c = store_find_channel(idbuf);
                        snprintf(rep, sizeof(rep), "#%s", c ? c->name : "channel");
                        is_mention = 1; }
                } else if (p[1] == '@' && p[2] == '&') {
                    int n = (int)(end - (p + 3));
                    if (n > 0 && n < 24) { memcpy(idbuf, p + 3, n); idbuf[n] = '\0';
                        const char *rn = store_role_name(idbuf);
                        snprintf(rep, sizeof(rep), "@%s", (rn && rn[0]) ? rn : "role");
                        is_mention = 1; }
                } else if (p[1] == '@') {
                    const char *ids = (p[2] == '!') ? p + 3 : p + 2;
                    int n = (int)(end - ids);
                    if (n > 0 && n < 24) { memcpy(idbuf, ids, n); idbuf[n] = '\0';
                        const char *nm = mention_name(mentions, idbuf);
                        snprintf(rep, sizeof(rep), "@%s", nm ? nm : "user");
                        is_mention = 1; }
                } else if (p[1] == 't' && p[2] == ':') {
                    // <t:unix:style> timestamp -> HH:MM UTC.
                    long long ts = 0; const char *q = p + 3;
                    while (q < end && *q >= '0' && *q <= '9') ts = ts * 10 + (*q++ - '0');
                    if (ts > 0) { int s = (int)(ts % 86400);
                        snprintf(rep, sizeof(rep), "%02d:%02d", s / 3600, (s % 3600) / 60); }
                }
                if (rep[0]) {
                    int wrap = mark && is_mention;  // pill sentinels \x01..\x02
                    size_t rl = strlen(rep);
                    if (o + rl + (wrap ? 2 : 0) < cap - 1) {
                        if (wrap) out[o++] = '\x01';
                        memcpy(out + o, rep, rl); o += rl;
                        if (wrap) out[o++] = '\x02';
                    }
                    p = end + 1;
                    continue;
                }
            }
        }
        // Pill literal @everyone / @here like real mentions.
        if (mark && *p == '@') {
            const char *kw = NULL;
            if (strncmp(p, "@everyone", 9) == 0) kw = "@everyone";
            else if (strncmp(p, "@here", 5) == 0) kw = "@here";
            if (kw) {
                char w[24];
                int wl = snprintf(w, sizeof(w), "\x01%s\x02", kw);
                if (o + (size_t)wl < cap - 1) { memcpy(out + o, w, wl); o += wl; }
                p += strlen(kw);
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

void discord_json_to_message(json_t *m, message *out)
{
    memset(out, 0, sizeof(*out));
    json_t *a = json_object_get(m, "author");
    snprintf(out->id, sizeof(out->id), "%s",
             json_string_value(json_object_get(m, "id")) ?: "");
    snprintf(out->channel_id, sizeof(out->channel_id), "%s",
             json_string_value(json_object_get(m, "channel_id")) ?: "");
    // Gateway MESSAGE_CREATE/UPDATE embed a member object whose nick overrides
    // the global name (REST history omits it).
    json_t *mmem = json_object_get(m, "member");
    const char *mnick = mmem ? json_string_value(json_object_get(mmem, "nick")) : NULL;
    if (mnick && mnick[0])
        snprintf(out->author, sizeof(out->author), "%s", mnick);
    else
        snprintf(out->author, sizeof(out->author), "%s", author_display(a) ?: "?");
    if (a) {
        snprintf(out->author_id, sizeof(out->author_id), "%s",
                 json_string_value(json_object_get(a, "id")) ?: "");
        snprintf(out->avatar, sizeof(out->avatar), "%s",
                 json_string_value(json_object_get(a, "avatar")) ?: "");
        out->is_bot = json_is_true(json_object_get(a, "bot"));
    }
    resolve_mentions(json_string_value(json_object_get(m, "content")),
                     json_object_get(m, "mentions"), out->content, sizeof(out->content), 1);
    snprintf(out->timestamp, sizeof(out->timestamp), "%s",
             json_string_value(json_object_get(m, "timestamp")) ?: "");
    out->type = (int)json_integer_value(json_object_get(m, "type"));

    // Does this message mention us (or @everyone/@here)?
    if (json_is_true(json_object_get(m, "mention_everyone"))) out->mentions_me = true;
    else {
        const char *me = store_me_id();
        json_t *ms = json_object_get(m, "mentions");
        if (me && me[0] && json_is_array(ms)) {
            size_t mn = json_array_size(ms);
            for (size_t i = 0; i < mn; i++) {
                const char *uid = json_string_value(
                    json_object_get(json_array_get(ms, i), "id"));
                if (uid && strcmp(uid, me) == 0) { out->mentions_me = true; break; }
            }
        }
    }

    // First image attachment (png/jpeg/gif/webp).
    json_t *atts = json_object_get(m, "attachments");
    if (json_is_array(atts) && json_array_size(atts) >= 1) {
        json_t *a0 = json_array_get(atts, 0);
        const char *ct = json_string_value(json_object_get(a0, "content_type"));
        const char *fn = json_string_value(json_object_get(a0, "filename"));
        bool img = false, is_gif = false;
        if (ct && strncmp(ct, "image/", 6) == 0) {
            img = true; if (!strcmp(ct, "image/gif")) is_gif = true;
        } else if (fn) {
            const char *e = strrchr(fn, '.');
            if (e && (!strcasecmp(e, ".png") || !strcasecmp(e, ".jpg") ||
                      !strcasecmp(e, ".jpeg") || !strcasecmp(e, ".gif") ||
                      !strcasecmp(e, ".webp"))) img = true;
            if (e && !strcasecmp(e, ".gif")) is_gif = true;
        }
        const char *purl = json_string_value(json_object_get(a0, "proxy_url"));
        const char *url  = json_string_value(json_object_get(a0, "url"));
        if (img && (purl || url)) {
            out->has_image = true;
            out->img_w = (int)json_integer_value(json_object_get(a0, "width"));
            out->img_h = (int)json_integer_value(json_object_get(a0, "height"));
            if (out->img_w <= 0) out->img_w = 200;
            if (out->img_h <= 0) out->img_h = 200;
            // Request a resized copy from the media proxy: GIFs stay GIF, others
            // become PNG.
            int rw = out->img_w, rh = out->img_h, MAXD = 400;
            if (rw > MAXD) { rh = (int)((long long)rh * MAXD / rw); rw = MAXD; }
            if (rh > MAXD) { rw = (int)((long long)rw * MAXD / rh); rh = MAXD; }
            if (rw < 1) rw = 1; if (rh < 1) rh = 1;
            if (purl && purl[0])
                snprintf(out->image_url, sizeof(out->image_url),
                         "%s%cwidth=%d&height=%d%s", purl,
                         strchr(purl, '?') ? '&' : '?', rw, rh,
                         is_gif ? "" : "&format=png");
            else
                snprintf(out->image_url, sizeof(out->image_url), "%s", url);
        } else {
            // Video: content_type video/* or a video extension.
            bool vid = (ct && strncmp(ct, "video/", 6) == 0);
            if (!vid && fn) {
                const char *e = strrchr(fn, '.');
                vid = e && (!strcasecmp(e, ".mp4") || !strcasecmp(e, ".webm") ||
                            !strcasecmp(e, ".mov") || !strcasecmp(e, ".m4v"));
            }
            if (vid && (url || purl)) {
                out->has_video = true;
                snprintf(out->video_url, sizeof(out->video_url), "%s", url ? url : purl);
                out->img_w = (int)json_integer_value(json_object_get(a0, "width"));
                out->img_h = (int)json_integer_value(json_object_get(a0, "height"));
                if (out->img_w <= 0) out->img_w = 320;
                if (out->img_h <= 0) out->img_h = 180;
                int rw = out->img_w, rh = out->img_h, MAXD = 400;
                if (rw > MAXD) { rh = (int)((long long)rh * MAXD / rw); rw = MAXD; }
                if (rh > MAXD) { rw = (int)((long long)rw * MAXD / rh); rh = MAXD; }
                if (rw < 1) rw = 1; if (rh < 1) rh = 1;
                // Poster still from the media proxy (first frame PNG).
                if (purl && purl[0])
                    snprintf(out->image_url, sizeof(out->image_url),
                             "%s%cwidth=%d&height=%d&format=png", purl,
                             strchr(purl, '?') ? '&' : '?', rw, rh);
            } else if (fn) {  // non-image -> a file attachment
                out->has_file = true;
                snprintf(out->file_name, sizeof(out->file_name), "%s", fn);
                out->file_size = (long long)json_integer_value(json_object_get(a0, "size"));
            }
        }
    }

    // Stickers arrive as sticker_items (id + format_type). PNG(1)/APNG(2) -> .png,
    // GIF(4) -> .gif; Lottie(3) is vector we can't decode (skipped).
    if (!out->has_image && !out->has_file) {
        json_t *stk = json_object_get(m, "sticker_items");
        if (json_is_array(stk) && json_array_size(stk) >= 1) {
            json_t *s0 = json_array_get(stk, 0);
            const char *sid = json_string_value(json_object_get(s0, "id"));
            int fmt = (int)json_integer_value(json_object_get(s0, "format_type"));
            if (sid && sid[0] && fmt != 3) {
                out->has_image = true;
                out->img_w = out->img_h = 160;
                if (fmt == 4)
                    snprintf(out->image_url, sizeof(out->image_url),
                             "https://media.discordapp.net/stickers/%s.gif?size=160", sid);
                else
                    snprintf(out->image_url, sizeof(out->image_url),
                             "https://cdn.discordapp.com/stickers/%s.png?size=160", sid);
            }
        }
    }

    // Embeds: link previews, bot cards, and Tenor/Giphy/klipy GIFs. Render as a
    // card (accent bar, author, title, description) plus image/thumbnail.
    json_t *embeds = json_object_get(m, "embeds");
    if (json_is_array(embeds) && json_array_size(embeds) >= 1) {
        json_t *e0 = json_array_get(embeds, 0);
        // gifv/image/video embeds are media-only: just the media, no card chrome.
        const char *etype = json_string_value(json_object_get(e0, "type"));
        int media_only = etype && (strcmp(etype, "gifv") == 0 ||
                                   strcmp(etype, "image") == 0 ||
                                   strcmp(etype, "video") == 0);
        const char *etitle = json_string_value(json_object_get(e0, "title"));
        const char *edesc  = json_string_value(json_object_get(e0, "description"));
        json_t *eauth = json_object_get(e0, "author");
        const char *ean = json_string_value(json_object_get(eauth, "name"));
        json_t *ecol = json_object_get(e0, "color");
        if (!media_only && (etitle || edesc || ean)) {
            out->has_embed = true;
            if (ean) snprintf(out->embed_author, sizeof(out->embed_author), "%s", ean);
            if (etitle) snprintf(out->embed_title, sizeof(out->embed_title), "%s", etitle);
            if (edesc) snprintf(out->embed_desc, sizeof(out->embed_desc), "%s", edesc);
            if (json_is_integer(ecol))  // raw 0xRRGGBB; 0 = default
                out->embed_color = (unsigned)json_integer_value(ecol);
            json_t *efields = json_object_get(e0, "fields");
            if (json_is_array(efields)) {
                size_t fn = json_array_size(efields), fi;
                int cap = (int)(sizeof(out->embed_fields) / sizeof(out->embed_fields[0]));
                for (fi = 0; fi < fn && out->embed_field_count < cap; fi++) {
                    json_t *f = json_array_get(efields, fi);
                    const char *fname = json_string_value(json_object_get(f, "name"));
                    const char *fval  = json_string_value(json_object_get(f, "value"));
                    if (!fname && !fval) continue;
                    int k = out->embed_field_count++;
                    if (fname) snprintf(out->embed_fields[k].name, sizeof(out->embed_fields[k].name), "%s", fname);
                    if (fval)  snprintf(out->embed_fields[k].value, sizeof(out->embed_fields[k].value), "%s", fval);
                    out->embed_fields[k].inline_ = json_is_true(json_object_get(f, "inline"));
                }
            }
        }
        // Prefer .image, fall back to .thumbnail (gifv/video use thumbnail).
        json_t *im = json_object_get(e0, "image");
        if (!json_is_object(im)) im = json_object_get(e0, "thumbnail");
        if (!out->has_image && json_is_object(im)) {
            // Fetch via proxy_url with format=png: transcodes any source
            // (webp/gif) to a resized, decodable PNG.
            const char *purl = json_string_value(json_object_get(im, "proxy_url"));
            const char *url  = json_string_value(json_object_get(im, "url"));
            out->img_w = (int)json_integer_value(json_object_get(im, "width"));
            out->img_h = (int)json_integer_value(json_object_get(im, "height"));
            if (out->img_w <= 0) out->img_w = 240;
            if (out->img_h <= 0) out->img_h = 180;
            int rw = out->img_w, rh = out->img_h, MAXD = 400;
            if (rw > MAXD) { rh = (int)((long long)rh * MAXD / rw); rw = MAXD; }
            if (rh > MAXD) { rw = (int)((long long)rw * MAXD / rh); rh = MAXD; }
            if (rw < 1) rw = 1; if (rh < 1) rh = 1;
            // images-ext-* proxies reject our TLS fingerprint; for those fetch
            // the embed's original url and decode webp ourselves.
            bool ext_proxy = purl && strstr(purl, "images-ext");
            if (ext_proxy && url && url[0]) {
                out->has_image = true;
                snprintf(out->image_url, sizeof(out->image_url), "%s", url);
            } else if (purl && purl[0]) {
                out->has_image = true;
                snprintf(out->image_url, sizeof(out->image_url),
                         "%s?width=%d&height=%d&format=png", purl, rw, rh);
            } else if (url && url[0]) {
                out->has_image = true;
                snprintf(out->image_url, sizeof(out->image_url), "%s", url);
            }
        }
        // If the content is just the media URL, hide it so only the image shows.
        if (media_only && out->has_image && out->content[0] &&
            strncmp(out->content, "http", 4) == 0) {
            int has_space = 0;
            for (const char *p = out->content; *p; p++)
                if (*p == ' ' || *p == '\n') { has_space = 1; break; }
            if (!has_space) out->content[0] = '\0';
        }
    }

    // Rich invite tile: detect a discord.gg / .../invite/ link in the content.
    {
        static const char *marks[] = { "discord.gg/", "discord.com/invite/",
                                       "discordapp.com/invite/", NULL };
        for (int mi = 0; marks[mi] && !out->has_invite; mi++) {
            const char *p = strstr(out->content, marks[mi]);
            if (!p) continue;
            p += strlen(marks[mi]);
            int k = 0;
            while (*p && k < (int)sizeof(out->invite_code) - 1 &&
                   ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
                out->invite_code[k++] = *p++;
            out->invite_code[k] = '\0';
            if (k > 0) out->has_invite = true;
        }
        // If the message is just the invite URL, hide the raw text (show the tile).
        if (out->has_invite && strncmp(out->content, "http", 4) == 0) {
            int has_space = 0;
            for (const char *q = out->content; *q; q++)
                if (*q == ' ' || *q == '\n') { has_space = 1; break; }
            if (!has_space) out->content[0] = '\0';
        }
    }

    // Reactions: array of { emoji:{name,id,animated}, count, me }.
    json_t *rxs = json_object_get(m, "reactions");
    if (json_is_array(rxs)) {
        size_t n = json_array_size(rxs);
        for (size_t i = 0; i < n && out->reaction_count < MAX_REACTIONS; i++) {
            json_t *r  = json_array_get(rxs, i);
            json_t *em = json_object_get(r, "emoji");
            reaction *rc = &out->reactions[out->reaction_count];
            snprintf(rc->name, sizeof(rc->name), "%s",
                     json_string_value(json_object_get(em, "name")) ?: "");
            snprintf(rc->emoji_id, sizeof(rc->emoji_id), "%s",
                     json_string_value(json_object_get(em, "id")) ?: "");
            rc->animated = json_boolean_value(json_object_get(em, "animated"));
            rc->count    = (int)json_integer_value(json_object_get(r, "count"));
            rc->me       = json_boolean_value(json_object_get(r, "me"));
            if (rc->name[0] || rc->emoji_id[0]) out->reaction_count++;
        }
    }

    json_t *ref = json_object_get(m, "referenced_message");
    if (json_is_object(ref)) {
        out->is_reply = true;
        snprintf(out->reply_to_id, sizeof(out->reply_to_id), "%s",
                 json_string_value(json_object_get(ref, "id")) ?: "");
        json_t *ra = json_object_get(ref, "author");
        snprintf(out->reply_author, sizeof(out->reply_author), "%s",
                 author_display(ra) ?: "?");
        const char *rc = json_string_value(json_object_get(ref, "content"));
        if (rc && rc[0])
            resolve_mentions(rc, json_object_get(ref, "mentions"),
                             out->reply_content, sizeof(out->reply_content), 0);
        else
            snprintf(out->reply_content, sizeof(out->reply_content), "%s",
                     "Click to see attachment");
    } else if (out->type == 19) {
        out->is_reply = true;
        snprintf(out->reply_content, sizeof(out->reply_content), "%s",
                 "Original message was deleted");
    }

    // Poll: question + answers, with live vote counts from `results`.
    json_t *poll = json_object_get(m, "poll");
    if (json_is_object(poll)) {
        out->has_poll = true;
        json_t *q = json_object_get(poll, "question");
        snprintf(out->poll_question, sizeof(out->poll_question), "%s",
                 (q ? json_string_value(json_object_get(q, "text")) : NULL) ?: "Poll");
        json_t *results = json_object_get(poll, "results");
        out->poll_finalized = results && json_is_true(json_object_get(results, "is_finalized"));
        json_t *counts = results ? json_object_get(results, "answer_counts") : NULL;
        json_t *ans = json_object_get(poll, "answers");
        size_t an = json_is_array(ans) ? json_array_size(ans) : 0;
        for (size_t i = 0; i < an && out->poll_answer_count < MAX_POLL_ANSWERS; i++) {
            json_t *a = json_array_get(ans, i);
            int aid = (int)json_integer_value(json_object_get(a, "answer_id"));
            json_t *pm = json_object_get(a, "poll_media");
            const char *txt = pm ? json_string_value(json_object_get(pm, "text")) : NULL;
            int idx = out->poll_answer_count++;
            out->poll_answers[idx].id = aid;
            snprintf(out->poll_answers[idx].text, sizeof(out->poll_answers[idx].text),
                     "%s", txt ?: "?");
            // Match this answer's count/me from results by answer id.
            if (json_is_array(counts)) {
                size_t cn = json_array_size(counts);
                for (size_t k = 0; k < cn; k++) {
                    json_t *c = json_array_get(counts, k);
                    if ((int)json_integer_value(json_object_get(c, "id")) == aid) {
                        out->poll_answers[idx].count = (int)json_integer_value(json_object_get(c, "count"));
                        out->poll_answers[idx].me = json_is_true(json_object_get(c, "me_voted"));
                        out->poll_total += out->poll_answers[idx].count;
                        break;
                    }
                }
            }
        }
    }
}

int discord_get_messages(const char *channel_id, const char *around_id,
                         const char *token, const char *cacert_path)
{
    char url[200];
    if (around_id && around_id[0])
        snprintf(url, sizeof(url),
                 DISCORD_API_BASE "/channels/%s/messages?limit=50&around=%s",
                 channel_id, around_id);
    else
        snprintf(url, sizeof(url),
                 DISCORD_API_BASE "/channels/%s/messages?limit=50", channel_id);

    char *resp = NULL;
    long code = rest_call("GET", url, NULL, token, cacert_path, &resp);
    if (code == 200 && resp) {
        json_error_t err;
        json_t *arr = json_loads(resp, 0, &err);
        if (json_is_array(arr)) {
            store_lock();
            store_clear_messages();
            // Discord returns newest-first; add in reverse for oldest-first.
            for (int i = (int)json_array_size(arr) - 1; i >= 0; i--) {
                message msg;
                discord_json_to_message(json_array_get(arr, i), &msg);
                store_push_message(&msg);
            }
            store_unlock();
        }
        if (arr) json_decref(arr);
    } else {
        console_logf("[api] get_messages http=%ld", code);
    }
    free(resp);
    return (int)code;
}

// Authenticated request with optional JSON body, response discarded.
// Returns the HTTP status (-1 on transport failure).
static int api_perform(const char *method, const char *url, const char *body,
                       const char *token, const char *cacert_path)
{
    return (int)rest_call(method, url, body, token, cacert_path, NULL);
}

int discord_send_reply(const char *channel_id, const char *content,
                       const char *reply_to_id, const char *token,
                       const char *cacert_path)
{
    char url[128];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/messages", channel_id);
    json_t *body = json_pack("{s:s, s:{s:s}}", "content", content,
                             "message_reference", "message_id", reply_to_id);
    char *bs = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    int r = api_perform("POST", url, bs, token, cacert_path);
    free(bs);
    return r;
}

int discord_edit_message(const char *channel_id, const char *message_id,
                         const char *content, const char *token,
                         const char *cacert_path)
{
    char url[160];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/messages/%s",
             channel_id, message_id);
    json_t *body = json_pack("{s:s}", "content", content);
    char *bs = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    int r = api_perform("PATCH", url, bs, token, cacert_path);
    free(bs);
    return r;
}

int discord_delete_message_api(const char *channel_id, const char *message_id,
                               const char *token, const char *cacert_path)
{
    char url[160];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/messages/%s",
             channel_id, message_id);
    return api_perform("DELETE", url, NULL, token, cacert_path);
}

// Fetch a user's profile (bio + accent color). Empty bio / 0 accent when
// absent. Returns the HTTP status.
int discord_get_user_profile(const char *user_id, const char *guild_id,
                             const char *token, const char *cacert_path,
                             char *bio, size_t bio_cap, unsigned *accent,
                             discord_profile_extra *extra)
{
    if (bio && bio_cap) bio[0] = '\0';
    if (accent) *accent = 0;
    if (extra) { extra->n_guilds = 0; extra->n_roles = 0; extra->banner[0] = '\0'; }
    bool want_extra = extra != NULL;  // mutuals always; roles need a guild
    bool want_roles = want_extra && guild_id && guild_id[0];
    char url[256];
    if (want_roles)
        snprintf(url, sizeof(url),
                 DISCORD_API_BASE "/users/%s/profile?with_mutual_guilds=true&guild_id=%s",
                 user_id, guild_id);
    else if (want_extra)
        snprintf(url, sizeof(url),
                 DISCORD_API_BASE "/users/%s/profile?with_mutual_guilds=true", user_id);
    else
        snprintf(url, sizeof(url),
                 DISCORD_API_BASE "/users/%s/profile?with_mutual_guilds=false", user_id);
    char *resp = NULL;
    long code = rest_call("GET", url, NULL, token, cacert_path, &resp);
    if (code == 200 && resp) {
        json_t *root = json_loads(resp, 0, NULL);
        if (root) {
            // bio lives under user_profile.bio (fall back to user.bio).
            json_t *up = json_object_get(root, "user_profile");
            const char *b = up ? json_string_value(json_object_get(up, "bio")) : NULL;
            if (!b || !b[0]) { json_t *u = json_object_get(root, "user");
                               b = u ? json_string_value(json_object_get(u, "bio")) : NULL; }
            // Clean Discord mention tokens out of the bio.
            if (b && bio && bio_cap) resolve_mentions(b, NULL, bio, bio_cap, 0);
            json_t *ac = up ? json_object_get(up, "accent_color") : NULL;
            if (json_is_integer(ac) && accent) {
                unsigned rgb = (unsigned)json_integer_value(ac);
                *accent = 0xFF000000u | ((rgb & 0xFF) << 16) |
                          (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);  // -> ABGR
            }
            if (want_extra) {
                // Banner hash lives on user (or user_profile).
                json_t *pu = json_object_get(root, "user");
                const char *bn = pu ? json_string_value(json_object_get(pu, "banner")) : NULL;
                if (!bn) { json_t *upr = json_object_get(root, "user_profile");
                           bn = upr ? json_string_value(json_object_get(upr, "banner")) : NULL; }
                if (bn) snprintf(extra->banner, sizeof(extra->banner), "%s", bn);
                json_t *mg = json_object_get(root, "mutual_guilds");
                if (json_is_array(mg)) {
                    size_t mn = json_array_size(mg);
                    for (size_t k = 0; k < mn && extra->n_guilds < 16; k++) {
                        const char *gid = json_string_value(
                            json_object_get(json_array_get(mg, k), "id"));
                        if (gid) snprintf(extra->guild_ids[extra->n_guilds++], 24, "%s", gid);
                    }
                }
                // Target's roles in this guild (skip @everyone == guild id).
                json_t *gm = json_object_get(root, "guild_member");
                json_t *rr = gm ? json_object_get(gm, "roles") : NULL;
                if (json_is_array(rr)) {
                    size_t rn = json_array_size(rr);
                    for (size_t k = 0; k < rn && extra->n_roles < 24; k++) {
                        const char *rid = json_string_value(json_array_get(rr, k));
                        if (rid && strcmp(rid, guild_id) != 0)
                            snprintf(extra->role_ids[extra->n_roles++], 24, "%s", rid);
                    }
                }
            }
            json_decref(root);
        }
    }
    free(resp);
    return (int)code;
}

// Search messages guild-wide (guild_scope) or in one channel. Fills out with
// each group's hit message. Returns the count, or <0 on error.
int discord_search_messages(int guild_scope, const char *scope_id, const char *query,
                            message *out, int max, const char *token,
                            const char *cacert_path)
{
    char enc[256]; url_encode(query ? query : "", enc, sizeof(enc));
    char url[320];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/%s/%s/messages/search?content=%s",
             guild_scope ? "guilds" : "channels", scope_id, enc);
    char *resp = NULL;
    long code = rest_call("GET", url, NULL, token, cacert_path, &resp);
    if (code != 200) console_logf("[search] %s http=%ld", guild_scope ? "guild" : "chan", code);
    int n = 0;
    if (code == 200 && resp) {
        json_t *root = json_loads(resp, 0, NULL);
        json_t *groups = root ? json_object_get(root, "messages") : NULL;
        if (json_is_array(groups)) {
            int gn = (int)json_array_size(groups);
            for (int g = 0; g < gn && n < max; g++) {
                json_t *grp = json_array_get(groups, g);
                if (!json_is_array(grp)) continue;
                // The matched message carries "hit":true; fall back to the first.
                json_t *pick = json_array_get(grp, 0);
                int sz = (int)json_array_size(grp);
                for (int k = 0; k < sz; k++) {
                    json_t *mm = json_array_get(grp, k);
                    if (json_is_true(json_object_get(mm, "hit"))) { pick = mm; break; }
                }
                if (pick) discord_json_to_message(pick, &out[n++]);
            }
        }
        if (root) json_decref(root);
    }
    free(resp);
    return code == 200 ? n : -1;
}

// Fetch a guild's active threads whose parent is `parent_id` (a forum's posts,
// or a text channel's threads) into `out`. Returns the count, or <0 on error.
int discord_get_active_threads(const char *guild_id, const char *parent_id,
                               channel *out, int max, const char *token,
                               const char *cacert_path)
{
    (void)guild_id;
    // Use the channel thread-search endpoint (guild /threads/active 403s for
    // user accounts). Newest activity first.
    char url[220];
    snprintf(url, sizeof(url),
             DISCORD_API_BASE "/channels/%s/threads/search"
             "?sort_by=last_message_time&sort_order=desc&limit=25", parent_id);
    char *resp = NULL;
    long code = rest_call("GET", url, NULL, token, cacert_path, &resp);
    int n = 0;
    if (code == 200 && resp) {
        json_t *root = json_loads(resp, 0, NULL);
        json_t *arr = root ? json_object_get(root, "threads") : NULL;
        if (json_is_array(arr)) {
            int sz = (int)json_array_size(arr);
            for (int i = 0; i < sz && n < max; i++) {
                json_t *cj = json_array_get(arr, i);
                const char *pid = json_string_value(json_object_get(cj, "parent_id"));
                if (parent_id && parent_id[0] && (!pid || strcmp(pid, parent_id) != 0))
                    continue;
                channel *c = &out[n++];
                memset(c, 0, sizeof(*c));
                snprintf(c->id, sizeof(c->id), "%s",
                         json_string_value(json_object_get(cj, "id")) ?: "");
                snprintf(c->name, sizeof(c->name), "%s",
                         json_string_value(json_object_get(cj, "name")) ?: "");
                c->type = (int)json_integer_value(json_object_get(cj, "type"));
                if (pid) snprintf(c->parent_id, sizeof(c->parent_id), "%s", pid);
                snprintf(c->last_message_id, sizeof(c->last_message_id), "%s",
                         json_string_value(json_object_get(cj, "last_message_id")) ?: "");
            }
        }
        if (root) json_decref(root);
    }
    free(resp);
    return code == 200 ? n : -1;
}

// Pin (or unpin) a message: PUT/DELETE /channels/{c}/pins/{m}. Needs
// MANAGE_MESSAGES in a guild; always allowed in DMs.
int discord_pin_message(const char *channel_id, const char *message_id, int pin,
                        const char *token, const char *cacert_path)
{
    char url[200];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/pins/%s", channel_id, message_id);
    return api_perform(pin ? "PUT" : "DELETE", url, pin ? "" : NULL, token, cacert_path);
}

// Fetch a channel's pinned messages into `out` (up to `max`, newest-first as
// Discord returns them). Returns the number parsed, or <0 on error.
int discord_get_pins(const char *channel_id, message *out, int max,
                     const char *token, const char *cacert_path)
{
    char url[160];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/pins", channel_id);
    char *resp = NULL;
    long code = rest_call("GET", url, NULL, token, cacert_path, &resp);
    int n = 0;
    if (code == 200 && resp) {
        json_t *arr = json_loads(resp, 0, NULL);
        if (json_is_array(arr)) {
            int sz = (int)json_array_size(arr);
            for (int i = 0; i < sz && n < max; i++)
                discord_json_to_message(json_array_get(arr, i), &out[n++]);
        }
        if (arr) json_decref(arr);
    }
    free(resp);
    return code == 200 ? n : -1;
}

// Read a forum's available tags into parallel id/name arrays (GET /channels/:id).
// Returns the number of tags, or <0 on error.
int discord_get_forum_tags(const char *forum_id, char ids[][24], char names[][40],
                           int max, const char *token, const char *cacert_path)
{
    char url[128];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s", forum_id);
    char *resp = NULL;
    long code = rest_call("GET", url, NULL, token, cacert_path, &resp);
    int n = 0;
    if (code == 200 && resp) {
        json_t *root = json_loads(resp, 0, NULL);
        json_t *arr = root ? json_object_get(root, "available_tags") : NULL;
        if (json_is_array(arr)) {
            int sz = (int)json_array_size(arr);
            for (int i = 0; i < sz && n < max; i++) {
                json_t *t = json_array_get(arr, i);
                snprintf(ids[n], 24, "%s", json_string_value(json_object_get(t, "id")) ?: "");
                snprintf(names[n], 40, "%s", json_string_value(json_object_get(t, "name")) ?: "?");
                n++;
            }
        }
        if (root) json_decref(root);
    }
    free(resp);
    return code == 200 ? n : -1;
}

int discord_get_relationships(discord_relationship *out, int max,
                              const char *token, const char *cacert_path)
{
    char *resp = NULL;
    long code = rest_call("GET", DISCORD_API_BASE "/users/@me/relationships",
                          NULL, token, cacert_path, &resp);
    int n = 0;
    if (code == 200 && resp) {
        json_t *arr = json_loads(resp, 0, NULL);
        if (json_is_array(arr)) {
            int sz = (int)json_array_size(arr);
            for (int i = 0; i < sz && n < max; i++) {
                json_t *r = json_array_get(arr, i);
                json_t *u = json_object_get(r, "user");
                if (!u) continue;
                out[n].type = (int)json_integer_value(json_object_get(r, "type"));
                snprintf(out[n].id, sizeof(out[n].id), "%s",
                         json_string_value(json_object_get(u, "id")) ?: "");
                snprintf(out[n].name, sizeof(out[n].name), "%s", author_display(u) ?: "?");
                snprintf(out[n].avatar, sizeof(out[n].avatar), "%s",
                         json_string_value(json_object_get(u, "avatar")) ?: "");
                n++;
            }
        }
        if (arr) json_decref(arr);
    } else {
        console_logf("[api] relationships http=%ld", code);
    }
    free(resp);
    return code == 200 ? n : -1;
}

int discord_friend_add(const char *username, const char *token, const char *cacert_path)
{
    json_t *body = json_pack("{s:s}", "username", username);
    char *bs = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    int r = api_perform("POST", DISCORD_API_BASE "/users/@me/relationships",
                        bs, token, cacert_path);
    free(bs);
    return r;
}
int discord_friend_accept(const char *user_id, const char *token, const char *cacert_path)
{
    char url[128];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/users/@me/relationships/%s", user_id);
    return api_perform("PUT", url, "{}", token, cacert_path);
}
int discord_friend_remove(const char *user_id, const char *token, const char *cacert_path)
{
    char url[128];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/users/@me/relationships/%s", user_id);
    return api_perform("DELETE", url, NULL, token, cacert_path);
}
int discord_friend_block(const char *user_id, const char *token, const char *cacert_path)
{
    char url[128];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/users/@me/relationships/%s", user_id);
    return api_perform("PUT", url, "{\"type\":2}", token, cacert_path);
}
int discord_set_note(const char *user_id, const char *note,
                     const char *token, const char *cacert_path)
{
    char url[128];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/users/@me/notes/%s", user_id);
    json_t *body = json_pack("{s:s}", "note", note ? note : "");
    char *bs = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    int r = api_perform("PUT", url, bs, token, cacert_path);
    free(bs);
    return r;
}
// Returns 200 on success, DISCORD_JOIN_CAPTCHA when a captcha is required,
// else the HTTP status.
int discord_join_invite(const char *code, const char *token, const char *cacert_path)
{
    char url[160];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/invites/%s", code);
    char *resp = NULL;
    long c = rest_call("POST", url, "{}", token, cacert_path, &resp);
    int captcha = (c == 400 && resp && strstr(resp, "captcha_key") != NULL);
    if (c < 200 || c >= 300) console_logf("[api] join %ld: %.200s", c, resp ? resp : "(no body)");
    free(resp);
    return captcha ? DISCORD_JOIN_CAPTCHA : (int)c;
}
int discord_create_guild(const char *name, const char *token, const char *cacert_path)
{
    json_t *body = json_pack("{s:s}", "name", (name && name[0]) ? name : "My Server");
    char *bs = json_dumps(body, JSON_COMPACT); json_decref(body);
    int r = api_perform("POST", DISCORD_API_BASE "/guilds", bs, token, cacert_path);
    free(bs);
    return r;
}

// Open (or create) a DM with a user. Copies the DM channel id into out.
// Returns the HTTP status.
int discord_open_dm(const char *recipient_id, char *out, size_t cap,
                    const char *token, const char *cacert_path)
{
    if (out && cap) out[0] = '\0';
    json_t *body = json_pack("{s:[s]}", "recipients", recipient_id);
    char *bs = json_dumps(body, JSON_COMPACT); json_decref(body);
    char *resp = NULL;
    long c = rest_call("POST", DISCORD_API_BASE "/users/@me/channels", bs, token, cacert_path, &resp);
    free(bs);
    if (c == 200 && resp && out) {
        json_t *root = json_loads(resp, 0, NULL);
        const char *id = root ? json_string_value(json_object_get(root, "id")) : NULL;
        if (id) snprintf(out, cap, "%s", id);
        if (root) json_decref(root);
    }
    free(resp);
    return (int)c;
}

int discord_leave_guild(const char *guild_id, const char *token, const char *cacert_path)
{
    char url[128];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/users/@me/guilds/%s", guild_id);
    return api_perform("DELETE", url, NULL, token, cacert_path);
}

int discord_set_guild_muted(const char *guild_id, int muted,
                            const char *token, const char *cacert_path)
{
    char url[160];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/users/@me/guilds/%s/settings", guild_id);
    char body[32]; snprintf(body, sizeof(body), "{\"muted\":%s}", muted ? "true" : "false");
    return api_perform("PATCH", url, body, token, cacert_path);
}


int discord_kick_member(const char *guild_id, const char *user_id,
                        const char *token, const char *cacert_path)
{
    char url[160];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/guilds/%s/members/%s", guild_id, user_id);
    return api_perform("DELETE", url, NULL, token, cacert_path);
}

int discord_ban_member(const char *guild_id, const char *user_id,
                       const char *token, const char *cacert_path)
{
    char url[160];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/guilds/%s/bans/%s", guild_id, user_id);
    return api_perform("PUT", url, "{}", token, cacert_path);
}

// Time a member out until iso_until (ISO8601 UTC); NULL/empty clears it.
int discord_timeout_member(const char *guild_id, const char *user_id,
                           const char *iso_until, const char *token, const char *cacert_path)
{
    char url[160];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/guilds/%s/members/%s", guild_id, user_id);
    char body[96];
    if (iso_until && iso_until[0])
        snprintf(body, sizeof(body), "{\"communication_disabled_until\":\"%s\"}", iso_until);
    else
        snprintf(body, sizeof(body), "{\"communication_disabled_until\":null}");
    return api_perform("PATCH", url, body, token, cacert_path);
}

// Add or remove a role on a member (needs MANAGE_ROLES).
int discord_member_role(const char *guild_id, const char *user_id, const char *role_id,
                        int add, const char *token, const char *cacert_path)
{
    char url[200];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/guilds/%s/members/%s/roles/%s",
             guild_id, user_id, role_id);
    return api_perform(add ? "PUT" : "DELETE", url, add ? "" : NULL, token, cacert_path);
}

// Create an invite to channel_id. Copies the invite code into out_code.
// Returns the HTTP status.
int discord_create_invite(const char *channel_id, char *out_code, size_t cap,
                          const char *token, const char *cacert_path)
{
    if (out_code && cap) out_code[0] = '\0';
    char url[128];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/invites", channel_id);
    char *resp = NULL;
    long c = rest_call("POST", url, "{}", token, cacert_path, &resp);
    if (c == 200 && resp && out_code) {
        json_t *root = json_loads(resp, 0, NULL);
        const char *code = root ? json_string_value(json_object_get(root, "code")) : NULL;
        if (code) snprintf(out_code, cap, "%s", code);
        if (root) json_decref(root);
    }
    free(resp);
    return (int)c;
}
int discord_get_invite(const char *code, discord_invite *out,
                       const char *token, const char *cacert_path)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->code, sizeof(out->code), "%s", code);
    char url[160];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/invites/%s?with_counts=true", code);
    char *resp = NULL;
    long code_http = rest_call("GET", url, NULL, token, cacert_path, &resp);
    if (code_http == 200 && resp) {
        json_t *root = json_loads(resp, 0, NULL);
        if (root) {
            json_t *g = json_object_get(root, "guild");
            snprintf(out->guild_id, sizeof(out->guild_id), "%s",
                     json_string_value(json_object_get(g, "id")) ?: "");
            snprintf(out->name, sizeof(out->name), "%s",
                     json_string_value(json_object_get(g, "name")) ?: "Server");
            snprintf(out->icon, sizeof(out->icon), "%s",
                     json_string_value(json_object_get(g, "icon")) ?: "");
            snprintf(out->splash, sizeof(out->splash), "%s",
                     json_string_value(json_object_get(g, "splash")) ?: "");
            out->members = (int)json_integer_value(json_object_get(root, "approximate_member_count"));
            out->online  = (int)json_integer_value(json_object_get(root, "approximate_presence_count"));
            json_decref(root);
        }
    }
    free(resp);
    return (int)code_http;
}

int discord_create_thread(const char *channel_id, const char *message_id,
                          const char *name, const char *token, const char *cacert_path)
{
    char url[192];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/messages/%s/threads",
             channel_id, message_id);
    json_t *body = json_pack("{s:s, s:i}", "name", (name && name[0]) ? name : "New Thread",
                             "auto_archive_duration", 1440);
    char *bs = json_dumps(body, JSON_COMPACT); json_decref(body);
    int r = api_perform("POST", url, bs, token, cacert_path);
    free(bs);
    return r;
}
int discord_crosspost(const char *channel_id, const char *message_id,
                      const char *token, const char *cacert_path)
{
    char url[192];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/messages/%s/crosspost",
             channel_id, message_id);
    return api_perform("POST", url, "{}", token, cacert_path);
}
int discord_forward_message(const char *to_channel_id, const char *from_channel_id,
                            const char *message_id, const char *token,
                            const char *cacert_path)
{
    char url[128];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/messages", to_channel_id);
    // A forward is a message with a message_reference of type 1 (FORWARD) that
    // points at the source message + channel, and empty content.
    json_t *body = json_pack("{s:s, s:{s:i, s:s, s:s}}",
                             "content", "",
                             "message_reference",
                             "type", 1,
                             "message_id", message_id,
                             "channel_id", from_channel_id);
    char *bs = json_dumps(body, JSON_COMPACT); json_decref(body);
    int r = api_perform("POST", url, bs, token, cacert_path);
    free(bs);
    return r;
}

int discord_create_forum_post(const char *forum_id, const char *title,
                              const char *content, const char *tag_id,
                              const char *token, const char *cacert_path)
{
    // A forum post's first message must be non-empty; fall back to the title.
    const char *msg = (content && content[0]) ? content
                    : (title && title[0]) ? title : "\xE2\x80\x8B";  // zero-width
    char url[128];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/threads", forum_id);
    json_t *body = json_pack("{s:s, s:i, s:{s:s}}",
                             "name", (title && title[0]) ? title : "New Post",
                             "auto_archive_duration", 1440,
                             "message", "content", msg);
    if (tag_id && tag_id[0]) {
        json_t *tags = json_array();
        json_array_append_new(tags, json_string(tag_id));
        json_object_set_new(body, "applied_tags", tags);
    }
    char *bs = json_dumps(body, JSON_COMPACT);
    json_decref(body);

    char *resp = NULL;
    long code = rest_call("POST", url, bs, token, cacert_path, &resp);
    console_dbgf("[forum] create http=%ld resp=%.120s", code, resp ? resp : "(none)");
    free(resp); free(bs);
    return (int)code;
}

// Vote for one poll answer; answer_id < 0 clears the vote.
// Returns the HTTP status.
int discord_poll_vote(const char *channel_id, const char *message_id, int answer_id,
                      const char *token, const char *cacert_path)
{
    char url[160];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/polls/%s/answers/@me",
             channel_id, message_id);
    json_t *ids = json_array();
    if (answer_id >= 0) json_array_append_new(ids, json_integer(answer_id));
    json_t *body = json_pack("{s:o}", "answer_ids", ids);
    char *bs = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    int r = api_perform("PUT", url, bs, token, cacert_path);
    free(bs);
    return r;
}

int discord_set_bio(const char *bio, const char *token, const char *cacert_path)
{
    json_t *body = json_pack("{s:s}", "bio", bio ? bio : "");
    char *bs = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    int r = api_perform("PATCH", DISCORD_API_BASE "/users/@me/profile",
                        bs, token, cacert_path);
    free(bs);
    return r;
}

int discord_set_global_name(const char *name, const char *token, const char *cacert_path)
{
    json_t *body = json_pack("{s:s}", "global_name", name ? name : "");
    char *bs = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    int r = api_perform("PATCH", DISCORD_API_BASE "/users/@me", bs, token, cacert_path);
    free(bs);
    return r;
}

int discord_set_avatar(const char *file_path, const char *token, const char *cacert_path)
{
    SceUID fd = sceIoOpen(file_path, SCE_O_RDONLY, 0);
    if (fd < 0) { console_logf("[api] set_avatar open %s -> 0x%08X", file_path, (unsigned)fd); return -1; }
    SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz <= 0 || sz > 8LL * 1024 * 1024) { sceIoClose(fd); return -1; }
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { sceIoClose(fd); return -1; }
    int rd = sceIoRead(fd, buf, (SceSize)sz);
    sceIoClose(fd);
    if (rd != (int)sz) { free(buf); return -1; }

    const char *mime = (buf[0] == 0xFF && buf[1] == 0xD8) ? "image/jpeg"
                     : (buf[0] == 0x89 && buf[1] == 'P')  ? "image/png"
                     : "image/jpeg";
    char *b64 = malloc(4 * (((size_t)sz + 2) / 3) + 1);
    if (!b64) { free(buf); return -1; }
    b64_encode(buf, (size_t)sz, b64);
    free(buf);
    size_t urilen = strlen(b64) + 64;
    char *uri = malloc(urilen);
    if (!uri) { free(b64); return -1; }
    snprintf(uri, urilen, "data:%s;base64,%s", mime, b64);
    free(b64);
    json_t *body = json_pack("{s:s}", "avatar", uri);
    char *bs = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    free(uri);
    int r = api_perform("PATCH", DISCORD_API_BASE "/users/@me", bs, token, cacert_path);
    free(bs);
    return r;
}

// Mark the channel read up to message_id on Discord's servers.
int discord_ack_message(const char *channel_id, const char *message_id,
                        const char *token, const char *cacert_path)
{
    if (!channel_id || !channel_id[0] || !message_id || !message_id[0]) return -1;
    char url[200];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/messages/%s/ack",
             channel_id, message_id);
    return api_perform("POST", url, "{}", token, cacert_path);
}

// Add or remove our reaction. emoji is raw unicode, or "name:id" for a
// custom emoji (URL-encoded here).
int discord_react(const char *channel_id, const char *message_id,
                  const char *emoji, bool add, const char *token,
                  const char *cacert_path)
{
    char enc[256]; url_encode(emoji, enc, sizeof(enc));
    char url[256];
    snprintf(url, sizeof(url),
             DISCORD_API_BASE "/channels/%s/messages/%s/reactions/%s/@me",
             channel_id, message_id, enc);
    int r = api_perform(add ? "PUT" : "DELETE", url,
                        add ? "" : NULL, token, cacert_path);
    return r;
}

// Fetch up to 50 messages before before_id and prepend them (older history).
int discord_get_messages_before(const char *channel_id, const char *before_id,
                                const char *token, const char *cacert_path)
{
    char url[160];
    snprintf(url, sizeof(url),
             DISCORD_API_BASE "/channels/%s/messages?limit=50&before=%s",
             channel_id, before_id);
    char *resp = NULL;
    long code = rest_call("GET", url, NULL, token, cacert_path, &resp);

    int added = 0;
    if (code == 200 && resp) {
        json_t *arr = json_loads(resp, 0, NULL);
        if (json_is_array(arr)) {
            store_lock();
            // Discord returns newest-first; prepend in that order for oldest-first.
            size_t n = json_array_size(arr);
            for (size_t i = 0; i < n; i++) {
                message msg;
                discord_json_to_message(json_array_get(arr, i), &msg);
                if (!store_prepend_message(&msg)) break;
                added++;
            }
            store_unlock();
        }
        if (arr) json_decref(arr);
    }
    free(resp);
    return added;
}

int discord_send_message(const char *channel_id, const char *content,
                         const char *token, const char *cacert_path)
{
    char url[128];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/messages", channel_id);

    json_t *body = json_pack("{s:s}", "content", content);
    char *bodystr = json_dumps(body, JSON_COMPACT);
    json_decref(body);

    int code = (int)rest_call("POST", url, bodystr, token, cacert_path, NULL);
    free(bodystr);
    return code;
}

// Multipart upload: POST data/len as attachment filename with optional text
// content.
static int post_multipart(const char *channel_id, const char *content,
                          const void *data, size_t len, const char *filename,
                          const char *token, const char *cacert_path)
{
    char url[128];
    snprintf(url, sizeof(url), DISCORD_API_BASE "/channels/%s/messages", channel_id);

    static const char BND[] = "----CordlessBoundaryZ29vZEJvdW5kYXJ5";
    json_t *pl = json_pack("{s:s}", "content", content ? content : "");
    char *pls = json_dumps(pl, JSON_COMPACT);
    json_decref(pl);
    if (!pls) return -1;
    if (!filename || !filename[0]) filename = "file.bin";

    // Assemble the multipart body: payload_json part, file part header, raw
    // bytes, closing boundary.
    size_t prelen_cap = strlen(pls) + strlen(filename) + sizeof(BND) * 2 + 256;
    char *pre = malloc(prelen_cap);
    if (!pre) { free(pls); return -1; }
    int prelen = snprintf(pre, prelen_cap,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"payload_json\"\r\n"
        "Content-Type: application/json\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"files[0]\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        BND, pls, BND, filename);
    free(pls);
    char post[64];
    int postlen = snprintf(post, sizeof(post), "\r\n--%s--\r\n", BND);
    if (prelen <= 0 || postlen <= 0) { free(pre); return -1; }

    int total = prelen + (int)len + postlen;
    unsigned char *bodybuf = malloc((size_t)total);
    if (!bodybuf) { free(pre); return -1; }
    memcpy(bodybuf, pre, (size_t)prelen);
    memcpy(bodybuf + prelen, data, len);
    memcpy(bodybuf + prelen + len, post, (size_t)postlen);
    free(pre);

    char sprops[1200]; discord_superprops_b64(sprops, sizeof(sprops));
    char auth[300], xsp[1400], ct[128];
    snprintf(auth, sizeof(auth), "Authorization: %s", token);
    snprintf(xsp, sizeof(xsp), "X-Super-Properties: %s", sprops);
    snprintf(ct, sizeof(ct), "Content-Type: multipart/form-data; boundary=%s", BND);
    const char *hdrs[] = { "User-Agent: " UA, auth, xsp, "Accept: application/json",
                           "X-Discord-Locale: en-US", "X-Discord-Timezone: America/New_York",
                           "X-Debug-Options: bugReporterEnabled", "Accept-Language: en-US",
                           ct, NULL };

    long code = -1;
    net_lock();
    int r = https_request("POST", url, cacert_path, hdrs, bodybuf, total,
                          NULL, NULL, &code, 1 << 20);
    net_unlock();
    free(bodybuf);
    if (r != 0) console_logf("[api] upload transport err");
    return (int)code;
}

int discord_send_file(const char *channel_id, const char *content,
                      const char *file_path, const char *token,
                      const char *cacert_path)
{
    // Read the file into memory via sceIo. Cap at 24 MB (under Discord's 25 MB
    // non-nitro limit).
    SceUID fd = sceIoOpen(file_path, SCE_O_RDONLY, 0);
    if (fd < 0) { console_logf("[api] send_file: open failed %s (0x%08X)", file_path, (unsigned)fd); return -1; }
    SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz <= 0 || sz > 24LL * 1024 * 1024) { sceIoClose(fd); console_logf("[api] send_file: bad size %lld", (long long)sz); return -1; }
    char *fbuf = malloc((size_t)sz);
    if (!fbuf) { sceIoClose(fd); return -1; }
    int rd = sceIoRead(fd, fbuf, (SceSize)sz);
    sceIoClose(fd);
    if (rd != (int)sz) { free(fbuf); console_logf("[api] send_file: short read %d/%lld", rd, (long long)sz); return -1; }

    // Basename for the multipart filename.
    const char *base = file_path;
    for (const char *p = file_path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;

    int code = post_multipart(channel_id, content, fbuf, (size_t)sz, base, token, cacert_path);
    free(fbuf);
    return code;
}

int discord_send_bytes(const char *channel_id, const char *content,
                       const void *data, int len, const char *filename,
                       const char *token, const char *cacert_path)
{
    if (!data || len <= 0) return -1;
    return post_multipart(channel_id, content, data, (size_t)len,
                          filename && filename[0] ? filename : "upload.bin",
                          token, cacert_path);
}
