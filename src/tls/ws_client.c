#include "tls/ws_client.h"
#include "util/b64.h"
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>

#include <psp2/kernel/rng.h>
#include <zlib.h>

#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include "tls/ca_bundle.h"

// Max payload of a single WebSocket frame.
#define WS_MAX_FRAME (16u * 1024u * 1024u)

struct ws_client {
    int fd;
    mbedtls_ssl_context ssl;   mbedtls_ssl_config conf;
    mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg;
    unsigned char *rx; size_t rx_len, rx_cap;  // raw bytes pulled from TLS
    unsigned char *msg; size_t msg_len, msg_cap;  // app message being assembled
    int msg_started;  // mid text/binary reassembly
    int msg_opcode;  // opcode being assembled (0x1 text / 0x2 binary)
    int last_close_code;  // status code from a server close frame

    // Transport compression (?compress=zlib-stream): one zlib stream spans the
    // connection; zbuf accumulates until the 00 00 FF FF flush, inflated into infl.
    int compressed;
    z_stream zs;
    unsigned char *zbuf; size_t zbuf_len, zbuf_cap;  // compressed accumulator
    unsigned char *infl; size_t infl_cap;  // inflated (JSON) output
};

// Map recv/send errors: would-block/interrupt becomes WANT_*, real errors
// surface as send/recv failures.
static int would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    int r = (int)send(*(int *)ctx, b, n, 0);
    if (r < 0) return would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return r;
}
static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    int r = (int)recv(*(int *)ctx, b, n, 0);
    if (r < 0) return would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    if (r == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    return r;
}

static int parse_url(const char *url, char *host, size_t hcap, char *port,
                     size_t pcap, char *path, size_t pathcap)
{
    const char *h = url;
    if (strncasecmp(url, "wss://", 6) == 0) { h = url + 6; }
    else if (strncasecmp(url, "https://", 8) == 0) { h = url + 8; }
    const char *slash = strchr(h, '/');
    const char *colon = memchr(h, ':', slash ? (size_t)(slash - h) : strlen(h));
    size_t hlen = colon ? (size_t)(colon - h) : (slash ? (size_t)(slash - h) : strlen(h));
    if (hlen == 0 || hlen >= hcap) return -1;
    memcpy(host, h, hlen); host[hlen] = '\0';
    if (colon) {
        size_t pl = (slash ? (size_t)(slash - (colon + 1)) : strlen(colon + 1));
        if (pl >= pcap) return -1;
        memcpy(port, colon + 1, pl); port[pl] = '\0';
    } else {
        snprintf(port, pcap, "443");
    }
    snprintf(path, pathcap, "%s", slash ? slash : "/");
    return 0;
}

// Block (spinning on WANT_*) until `len` bytes are written or an error occurs.
static int ssl_write_blocking(mbedtls_ssl_context *ssl, const unsigned char *buf, int len)
{
    int off = 0;
    for (int spins = 0; off < len && spins < 100000; ) {
        int r = mbedtls_ssl_write(ssl, buf + off, (size_t)(len - off));
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) { spins++; continue; }
        if (r <= 0) return -1;
        off += r; spins = 0;
    }
    return off == len ? 0 : -1;
}

ws_client *ws_open(const char *url, const char *cacert_path,
                   const char *origin, const char *user_agent)
{
    char host[128], port[8], path[512];
    if (parse_url(url, host, sizeof(host), port, sizeof(port), path, sizeof(path)) != 0)
        return NULL;

    ws_client *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->fd = -1;

    psa_crypto_init();
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) { free(w); return NULL; }
    w->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (w->fd < 0) { freeaddrinfo(res); free(w); return NULL; }
    if (connect(w->fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res); close(w->fd); free(w); return NULL;
    }
    freeaddrinfo(res);

    mbedtls_ssl_init(&w->ssl); mbedtls_ssl_config_init(&w->conf);
    mbedtls_entropy_init(&w->ent); mbedtls_ctr_drbg_init(&w->drbg);

    const char *pers = "cordless-ws";
    if (mbedtls_ctr_drbg_seed(&w->drbg, mbedtls_entropy_func, &w->ent,
                              (const unsigned char *)pers, strlen(pers)) != 0) goto fail;
    mbedtls_ssl_config_defaults(&w->conf, MBEDTLS_SSL_IS_CLIENT,
                                MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_x509_crt *ca = ca_bundle_get();
    if (ca) {
        mbedtls_ssl_conf_ca_chain(&w->conf, ca, NULL);
        mbedtls_ssl_conf_authmode(&w->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    } else {
        mbedtls_ssl_conf_authmode(&w->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&w->conf, mbedtls_ctr_drbg_random, &w->drbg);
    if (mbedtls_ssl_setup(&w->ssl, &w->conf) != 0) goto fail;
    mbedtls_ssl_set_hostname(&w->ssl, host);
    mbedtls_ssl_set_bio(&w->ssl, &w->fd, bio_send, bio_recv, NULL);

    // Handshake on a blocking socket; switched to non-blocking once established.
    int rc;
    do { rc = mbedtls_ssl_handshake(&w->ssl); }
    while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (rc != 0) { console_logf("[ws] handshake %s rc=-0x%04x", host, -rc); goto fail; }

    // HTTP Upgrade.
    unsigned char keyraw[16];
    for (int i = 0; i < 16; i += 8) sceKernelGetRandomNumber(keyraw + i, 8);
    char key_b64[32]; b64_encode(keyraw, 16, key_b64);
    char req[1024];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n",
        path, host, key_b64);
    // rl is snprintf's would-have-written length; clamp after each append.
    if (rl < 0 || rl >= (int)sizeof(req)) goto fail;
    if (origin) {
        rl += snprintf(req + rl, sizeof(req) - rl, "Origin: %s\r\n", origin);
        if (rl >= (int)sizeof(req)) goto fail;
    }
    if (user_agent) {
        rl += snprintf(req + rl, sizeof(req) - rl, "User-Agent: %s\r\n", user_agent);
        if (rl >= (int)sizeof(req)) goto fail;
    }
    rl += snprintf(req + rl, sizeof(req) - rl, "\r\n");
    if (rl <= 0 || rl >= (int)sizeof(req)) goto fail;
    if (ssl_write_blocking(&w->ssl, (const unsigned char *)req, rl) != 0) goto fail;

    // Read the handshake response headers (up to the blank line).
    char resp[2048]; int rn = 0;
    for (int spins = 0; rn < (int)sizeof(resp) - 1 && spins < 200000; ) {
        int r = mbedtls_ssl_read(&w->ssl, (unsigned char *)resp + rn, 1);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) { spins++; continue; }
        if (r <= 0) goto fail;
        rn += r; spins = 0;
        if (rn >= 4 && resp[rn-4]=='\r' && resp[rn-3]=='\n' && resp[rn-2]=='\r' && resp[rn-1]=='\n') break;
    }
    resp[rn] = '\0';
    if (strstr(resp, " 101") == NULL) { console_logf("[ws] upgrade not 101"); goto fail; }

    // Switch the socket to non-blocking for the poll-driven recv loop.
    int fl = fcntl(w->fd, F_GETFL, 0);
    fcntl(w->fd, F_SETFL, fl | O_NONBLOCK);

    // Enable zlib-stream decompression if the URL asks for it.
    if (strstr(url, "zlib-stream")) {
        w->zs.zalloc = Z_NULL; w->zs.zfree = Z_NULL; w->zs.opaque = Z_NULL;
        if (inflateInit(&w->zs) == Z_OK) { w->compressed = 1; console_dbgf("[ws] zlib-stream on"); }
        else console_logf("[ws] inflateInit failed -- falling back to raw");
    }
    return w;

fail:
    ws_close(w);
    return NULL;
}

// XOR-mask and frame `payload` as one client frame (opcode) into a temp buffer,
// then write it. Returns 0 on success.
static int send_frame(ws_client *w, int opcode, const unsigned char *payload, int len)
{
    unsigned char hdr[14]; int h = 0;
    hdr[h++] = (unsigned char)(0x80 | (opcode & 0x0F));  // FIN + opcode
    if (len < 126) hdr[h++] = (unsigned char)(0x80 | len);
    else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (unsigned char)(len >> 8); hdr[h++] = (unsigned char)(len & 0xFF);
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[h++] = (unsigned char)((unsigned long long)len >> (i * 8) & 0xFF);
    }
    unsigned char mask[4];
    sceKernelGetRandomNumber(mask, 4);
    memcpy(hdr + h, mask, 4); h += 4;

    if (ssl_write_blocking(&w->ssl, hdr, h) != 0) return -1;
    if (len > 0) {
        unsigned char stackbuf[4096];
        unsigned char *m = (len <= (int)sizeof(stackbuf)) ? stackbuf : malloc(len);
        if (!m) return -1;
        for (int i = 0; i < len; i++) m[i] = payload[i] ^ mask[i & 3];
        int r = ssl_write_blocking(&w->ssl, m, len);
        if (m != stackbuf) free(m);
        if (r != 0) return -1;
    }
    return 0;
}

int ws_send_text(ws_client *w, const char *txt, int len)
{
    if (!w) return -1;
    if (len < 0) len = (int)strlen(txt);
    return send_frame(w, 0x1, (const unsigned char *)txt, len);
}

int ws_send_binary(ws_client *w, const unsigned char *data, int len)
{
    if (!w || len < 0) return -1;
    return send_frame(w, 0x2, data, len);
}

static int rx_reserve(ws_client *w, size_t extra)
{
    if (w->rx_len + extra <= w->rx_cap) return 0;
    size_t nc = w->rx_cap ? w->rx_cap : 8192;
    while (nc < w->rx_len + extra) nc *= 2;
    unsigned char *g = realloc(w->rx, nc);
    if (!g) return -1;
    w->rx = g; w->rx_cap = nc;
    return 0;
}

static int msg_append(ws_client *w, const unsigned char *p, size_t n)
{
    if (w->msg_len + n + 1 > w->msg_cap) {
        size_t nc = w->msg_cap ? w->msg_cap : 8192;
        while (nc < w->msg_len + n + 1) nc *= 2;
        unsigned char *g = realloc(w->msg, nc);
        if (!g) return -1;
        w->msg = g; w->msg_cap = nc;
    }
    memcpy(w->msg + w->msg_len, p, n);
    w->msg_len += n;
    return 0;
}

// Consume one frame from the head of rx. Returns 1 if parsed (rx compacted), 0 if
// more bytes needed, -1 on close/error; sets *done=1 on a completed app message.
static int consume_frame(ws_client *w, int *done)
{
    if (w->rx_len < 2) return 0;
    unsigned char *b = w->rx;
    int fin = b[0] & 0x80, opcode = b[0] & 0x0F;
    int masked = b[1] & 0x80;  // server frames are not masked
    unsigned long long paylen = b[1] & 0x7F;
    size_t off = 2;
    if (paylen == 126) {
        if (w->rx_len < 4) return 0;
        paylen = ((unsigned)b[2] << 8) | b[3]; off = 4;
    } else if (paylen == 127) {
        if (w->rx_len < 10) return 0;
        paylen = 0; for (int i = 0; i < 8; i++) paylen = (paylen << 8) | b[2 + i];
        off = 10;
    }
    if (paylen > WS_MAX_FRAME) {  // DoS backstop
        console_logf("[ws] frame too large (%llu) -- closing", paylen);
        w->last_close_code = 1009;
        return -1;
    }
    // control frames (close/ping/pong) are capped at 125 bytes by RFC 6455
    if ((opcode == 0x8 || opcode == 0x9 || opcode == 0xA) && paylen > 125) return -1;
    size_t mask_off = off;
    if (masked) off += 4;
    if (w->rx_len < off + paylen) return 0;  // frame incomplete

    unsigned char *pl = b + off;
    if (masked) for (unsigned long long i = 0; i < paylen; i++) pl[i] ^= b[mask_off + (i & 3)];

    int ret = 1;
    if (opcode == 0x8) {  // close
        int code = paylen >= 2 ? (pl[0] << 8) | pl[1] : 0;
        char reason[128]; int rn = paylen > 2 ? (int)(paylen - 2) : 0;
        if (rn > 127) rn = 127;
        if (rn > 0) memcpy(reason, pl + 2, rn);
        reason[rn > 0 ? rn : 0] = '\0';
        console_logf("[ws] close code=%d %s", code, rn > 0 ? reason : "");
        w->last_close_code = code;
        ret = -1;
    } else if (opcode == 0x9) {  // ping -> pong
        send_frame(w, 0xA, pl, (int)paylen);
    } else if (opcode == 0xA) {  // pong: ignore
    } else if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
        if (opcode != 0x0) { w->msg_started = 1; w->msg_opcode = opcode; }  // start of text/binary
        if (msg_append(w, pl, paylen) != 0) return -1;  // OOM: fail, don't truncate
        if (fin) { *done = 1; w->msg_started = 0; }
    }

    // drop the consumed frame
    size_t consumed = off + paylen;
    memmove(w->rx, w->rx + consumed, w->rx_len - consumed);
    w->rx_len -= consumed;
    return ret;
}

// Append compressed bytes to the zlib-stream accumulator.
static int zbuf_append(ws_client *w, const unsigned char *p, size_t n)
{
    if (w->zbuf_len + n > w->zbuf_cap) {
        size_t nc = w->zbuf_cap ? w->zbuf_cap : 16384;
        while (nc < w->zbuf_len + n) nc *= 2;
        unsigned char *g = realloc(w->zbuf, nc);
        if (!g) return -1;
        w->zbuf = g; w->zbuf_cap = nc;
    }
    memcpy(w->zbuf + w->zbuf_len, p, n);
    w->zbuf_len += n;
    return 0;
}

// Inflate the accumulated zbuf (one flush-terminated run) through the persistent
// context into w->infl. Returns the inflated length, or -1 on a zlib error.
static int ws_inflate(ws_client *w)
{
    w->zs.next_in  = w->zbuf;
    w->zs.avail_in = (uInt)w->zbuf_len;
    size_t have = 0;
    for (;;) {
        if (have + 65536 > w->infl_cap) {
            size_t nc = w->infl_cap ? w->infl_cap * 2 : 262144;
            while (nc < have + 65536) nc *= 2;
            unsigned char *g = realloc(w->infl, nc);
            if (!g) return -1;
            w->infl = g; w->infl_cap = nc;
        }
        w->zs.next_out  = w->infl + have;
        w->zs.avail_out = (uInt)(w->infl_cap - have);
        int zr = inflate(&w->zs, Z_SYNC_FLUSH);
        if (zr == Z_DATA_ERROR || zr == Z_MEM_ERROR ||
            zr == Z_STREAM_ERROR || zr == Z_NEED_DICT) {
            console_logf("[ws] inflate err %d", zr);
            return -1;
        }
        have = w->infl_cap - w->zs.avail_out;
        if (zr == Z_BUF_ERROR) break;  // no more progress -> flushed
        if (w->zs.avail_out != 0) break;  // output has room -> all flushed
    }
    return (int)have;
}

int ws_recv_ex(ws_client *w, char **buf, size_t *cap, int *is_binary)
{
    if (!w) return -1;
    if (is_binary) *is_binary = 0;
    for (;;) {
        // 1. Assemble from whatever is already buffered.
        int done = 0, cr;
        while ((cr = consume_frame(w, &done)) == 1) {
            if (!done) continue;
            done = 0;

            if (w->compressed) {
                // Feed this message's compressed bytes into the stream; inflate only
                // once the 00 00 FF FF flush terminates a gateway event.
                if (zbuf_append(w, w->msg, w->msg_len) != 0) return -1;
                w->msg_len = 0;
                if (w->zbuf_len < 4 ||
                    memcmp(w->zbuf + w->zbuf_len - 4, "\x00\x00\xff\xff", 4) != 0)
                    continue;  // event incomplete; read more
                int jl = ws_inflate(w);
                w->zbuf_len = 0;
                if (jl < 0) return -1;
                if ((size_t)jl + 1 > *cap) { char *g = realloc(*buf, jl + 1); if (!g) return -1; *buf = g; *cap = jl + 1; }
                memcpy(*buf, w->infl, jl);
                (*buf)[jl] = '\0';
                return jl;
            }

            if (is_binary) *is_binary = (w->msg_opcode == 0x2);
            size_t need = w->msg_len + 1;
            if (need > *cap) { char *g = realloc(*buf, need); if (!g) return -1; *buf = g; *cap = need; }
            memcpy(*buf, w->msg, w->msg_len);
            (*buf)[w->msg_len] = '\0';
            int out = (int)w->msg_len;
            w->msg_len = 0;
            return out;
        }
        if (cr < 0) return -1;  // close/error

        // 2. Pull more TLS bytes (non-blocking).
        if (rx_reserve(w, 16384) != 0) return -1;
        int r = mbedtls_ssl_read(&w->ssl, w->rx + w->rx_len, 16384);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        if (r <= 0) return -1;  // close_notify / error
        w->rx_len += r;
    }
}

int ws_recv(ws_client *w, char **buf, size_t *cap)
{
    return ws_recv_ex(w, buf, cap, NULL);
}

int ws_last_close_code(ws_client *w) { return w ? w->last_close_code : 0; }

void ws_close(ws_client *w)
{
    if (!w) return;
    if (w->fd >= 0) {
        send_frame(w, 0x8, NULL, 0);  // best-effort close
        mbedtls_ssl_close_notify(&w->ssl);
    }
    mbedtls_ssl_free(&w->ssl); mbedtls_ssl_config_free(&w->conf);
    mbedtls_ctr_drbg_free(&w->drbg); mbedtls_entropy_free(&w->ent);
    if (w->fd >= 0) close(w->fd);
    if (w->compressed) inflateEnd(&w->zs);
    free(w->rx); free(w->msg); free(w->zbuf); free(w->infl);
    free(w);
}
