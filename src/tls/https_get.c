#include "tls/https_get.h"
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strncasecmp
#include <unistd.h>  // close
#include <sys/socket.h>
#include <netdb.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include "tls/ca_bundle.h"

static int bio_send(void *ctx, const unsigned char *b, size_t n)
{ int r = (int)send(*(int *)ctx, b, n, 0); return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r; }
static int bio_recv(void *ctx, unsigned char *b, size_t n)
{ int r = (int)recv(*(int *)ctx, b, n, 0); return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r; }

// Split "https://host[:port]/path" into host, port, path. Returns 0 on ok.
static int parse_url(const char *url, char *host, size_t hcap, char *port,
                     size_t pcap, char *path, size_t pathcap)
{
    if (strncasecmp(url, "https://", 8) != 0) return -1;
    const char *h = url + 8;
    const char *slash = strchr(h, '/');
    const char *colon = memchr(h, ':', slash ? (size_t)(slash - h) : strlen(h));
    size_t hlen = colon ? (size_t)(colon - h) : (slash ? (size_t)(slash - h) : strlen(h));
    if (hlen == 0 || hlen >= hcap) return -1;
    memcpy(host, h, hlen); host[hlen] = '\0';
    if (colon) { size_t pl = (slash ? (size_t)(slash - (colon + 1)) : strlen(colon + 1));
                 if (pl >= pcap) return -1; memcpy(port, colon + 1, pl); port[pl] = '\0'; }
    else       snprintf(port, pcap, "443");
    snprintf(path, pathcap, "%s", slash ? slash : "/");
    return 0;
}

// Case-insensitive substring search over a bounded buffer.
static int mem_ci_has(const char *hay, int n, const char *needle)
{
    int nl = (int)strlen(needle);
    for (int i = 0; i + nl <= n; i++)
        if (strncasecmp(hay + i, needle, (size_t)nl) == 0) return 1;
    return 0;
}

// Dechunk a Transfer-Encoding: chunked body in place. Returns new length.
static int dechunk(unsigned char *body, int len)
{
    int in = 0, out = 0;
    while (in < len) {
        char *end; long sz = strtol((char *)body + in, &end, 16);
        if (end == (char *)body + in) break;
        while (in < len && body[in] != '\n') in++;
        in++;  // past the \n after the size line
        if (sz <= 0 || in + sz > len) break;
        memmove(body + out, body + in, (size_t)sz);
        out += (int)sz; in += (int)sz;
        if (in + 1 < len && body[in] == '\r') in += 2;  // trailing CRLF
    }
    return out;
}

// Write the whole buffer; mbedtls_ssl_write may return partial. 0 on ok.
static int ssl_write_all(mbedtls_ssl_context *ssl, const unsigned char *buf, int len)
{
    int off = 0;
    while (off < len) {
        int r = mbedtls_ssl_write(ssl, buf + off, (size_t)(len - off));
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return -1;
        off += r;
    }
    return 0;
}

// Append each Set-Cookie name=value pair from the header block to `jar`, a
// "; "-joined Cookie string. Bounded; silently stops when full.
static void collect_cookies(const char *hdr, int hdr_len, char *jar, size_t cap)
{
    if (!jar || cap == 0) return;
    for (int i = 0; i + 12 < hdr_len; i++) {
        if (hdr[i] != '\n') continue;
        if (strncasecmp(hdr + i + 1, "Set-Cookie:", 11) != 0) continue;
        int j = i + 12;
        while (j < hdr_len && hdr[j] == ' ') j++;
        // copy up to ';' or end of line
        char pair[256]; int k = 0;
        while (j < hdr_len && hdr[j] != ';' && hdr[j] != '\r' && hdr[j] != '\n'
               && k < (int)sizeof(pair) - 1)
            pair[k++] = hdr[j++];
        pair[k] = '\0';
        if (k == 0 || !strchr(pair, '=')) continue;
        size_t have = strlen(jar);
        size_t need = strlen(pair) + (have ? 2 : 0);
        if (have + need + 1 >= cap) continue;
        if (have) { strcat(jar, "; "); }
        strcat(jar, pair);
    }
}

// Core HTTPS request. `follow` chases 3xx as GET; `only_200` returns a body only on
// 200 (else any status with a body); `cookie_jar`, if set, collects Set-Cookie pairs.
static int do_request(const char *method, const char *url, const char *cacert_path,
                      const char *const *headers, const unsigned char *body, int body_len,
                      unsigned char **out, int *out_len, long *http_code,
                      int max_bytes, int depth, int follow, int only_200,
                      char *cookie_jar, size_t cookie_cap)
{
    if (depth > 5) return -1;

    // This client speaks TLS only: upgrade http:// to https://.
    char upgraded[1200];
    if (strncasecmp(url, "http://", 7) == 0) {
        snprintf(upgraded, sizeof(upgraded), "https://%s", url + 7);
        url = upgraded;
    }

    char host[128], port[8], path[1024];
    if (parse_url(url, host, sizeof(host), port, sizeof(port), path, sizeof(path)) != 0)
        return -1;

    psa_crypto_init();
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) { close(fd); freeaddrinfo(res); return -1; }
    freeaddrinfo(res);

    mbedtls_ssl_context ssl;   mbedtls_ssl_config conf;
    mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&drbg);

    int rc = -1, ret = -1;
    const char *pers = "cordless-https";
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                              (const unsigned char *)pers, strlen(pers)) != 0) goto done;
    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    // Verify against the shared CA bundle; VERIFY_OPTIONAL tolerates hostname
    // mismatch (Vita3K DNS). NULL bundle -> no verification.
    mbedtls_x509_crt *ca = ca_bundle_get();
    if (ca) {
        mbedtls_ssl_conf_ca_chain(&conf, ca, NULL);
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    } else {
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    if (mbedtls_ssl_setup(&ssl, &conf) != 0) goto done;
    mbedtls_ssl_set_hostname(&ssl, host);
    mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
    do { rc = mbedtls_ssl_handshake(&ssl); }
    while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (rc != 0) { console_logf("[https] handshake %s rc=-0x%04x", host, -rc); goto done; }

    // Request line + headers.
    char req[2048];
    int rl = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: %s\r\n",
                      method, path, host);
    int has_ua = 0, has_accept = 0;
    if (headers) for (int i = 0; headers[i]; i++) {
        if (strncasecmp(headers[i], "User-Agent:", 11) == 0) has_ua = 1;
        if (strncasecmp(headers[i], "Accept:", 7) == 0)      has_accept = 1;
        rl += snprintf(req + rl, sizeof(req) - rl, "%s\r\n", headers[i]);
    }
    // Default UA when the caller set none. Must match the JS navigator.userAgent in
    // src/browser/dom_js.cpp (a header-vs-navigator mismatch is a bot signal).
    if (!has_ua)
        rl += snprintf(req + rl, sizeof(req) - rl,
                       "User-Agent: Mozilla/5.0 (Linux; Android 10; PlayStation Vita) "
                       "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/119.0.0.0 Mobile "
                       "Safari/537.36\r\n");
    if (!has_accept)
        rl += snprintf(req + rl, sizeof(req) - rl,
                       "Accept: text/html,application/json,*/*\r\n");
    if (body && body_len > 0)
        rl += snprintf(req + rl, sizeof(req) - rl, "Content-Length: %d\r\n", body_len);
    rl += snprintf(req + rl, sizeof(req) - rl, "Connection: close\r\n\r\n");
    if (rl <= 0 || rl >= (int)sizeof(req)) goto done;  // headers overflowed
    if (ssl_write_all(&ssl, (const unsigned char *)req, rl) != 0) goto done;
    if (body && body_len > 0 && ssl_write_all(&ssl, body, body_len) != 0) goto done;

    // Read the whole response (we sent Connection: close).
    int cap = 16384, n = 0;
    unsigned char *resp = malloc(cap);
    if (!resp) goto done;
    for (;;) {
        if (n + 4096 > cap) {
            if (cap >= max_bytes + 65536) break;  // body cap + header slack
            cap *= 2; unsigned char *p = realloc(resp, cap);
            if (!p) { free(resp); resp = NULL; goto done; }
            resp = p;
        }
        int r = mbedtls_ssl_read(&ssl, resp + n, cap - n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) break;  // close_notify / EOF / error
        n += r;
    }
    mbedtls_ssl_close_notify(&ssl);

    // Parse status + headers.
    long code = 0; sscanf((char *)resp, "HTTP/%*d.%*d %ld", &code);
    if (http_code) *http_code = code;
    unsigned char *hdr_end = NULL;
    for (int i = 0; i + 3 < n; i++)
        if (resp[i]=='\r'&&resp[i+1]=='\n'&&resp[i+2]=='\r'&&resp[i+3]=='\n') { hdr_end = resp+i+4; break; }
    if (!hdr_end) { free(resp); goto done; }
    int hdr_len = (int)(hdr_end - resp);

    collect_cookies((char *)resp, hdr_len, cookie_jar, cookie_cap);

    // Redirect: follow Location as GET.
    if (follow && code >= 300 && code < 400) {
        char loc[1024] = "";
        for (int i = 0; i < hdr_len; i++)
            if (strncasecmp((char *)resp + i, "\nLocation:", 10) == 0 ||
                strncasecmp((char *)resp + i, "\nlocation:", 10) == 0) {
                int j = i + 10; while (j < hdr_len && (resp[j]==' ')) j++;
                int k = 0; while (j < hdr_len && resp[j] != '\r' && k < 1023) loc[k++] = resp[j++];
                loc[k] = '\0'; break;
            }
        free(resp);
        if (loc[0]) ret = do_request("GET", loc, cacert_path, headers, NULL, 0,
                                     out, out_len, http_code, max_bytes, depth+1, follow, only_200,
                                     cookie_jar, cookie_cap);
        goto done;
    }

    int body_out = n - hdr_len;
    int chunked = mem_ci_has((char *)resp, hdr_len, "transfer-encoding: chunked") ||
                  mem_ci_has((char *)resp, hdr_len, "transfer-encoding:chunked");
    unsigned char *rbody = malloc(body_out > 0 ? body_out + 1 : 1);
    if (!rbody) { free(resp); goto done; }
    memcpy(rbody, hdr_end, (size_t)(body_out > 0 ? body_out : 0));
    free(resp);
    if (chunked) body_out = dechunk(rbody, body_out);
    if (body_out > max_bytes) body_out = max_bytes;
    if (body_out < 0) body_out = 0;
    rbody[body_out] = '\0';  // NUL-terminate for JSON callers

    if (only_200) {
        if (code == 200 && body_out > 0) { *out = rbody; *out_len = body_out; ret = 0; }
        else { free(rbody); ret = -1; }
    } else {
        *out = rbody; *out_len = body_out; ret = 0;  // any status: caller inspects code
    }

done:
    close(fd);
    mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent);
    return ret;
}

int https_get_mbedtls(const char *url, const char *cacert_path, unsigned char **out,
                      int *out_len, long *http_code, int max_bytes)
{
    *out = NULL; *out_len = 0;
    static const char *img_hdrs[] = { "User-Agent: Cordless", "Accept: image/*,*/*", NULL };
    return do_request("GET", url, cacert_path, img_hdrs, NULL, 0,
                      out, out_len, http_code, max_bytes, 0, 1 /*follow*/, 1 /*only_200*/,
                      NULL, 0);
}

int https_request(const char *method, const char *url, const char *cacert_path,
                  const char *const *headers, const unsigned char *body, int body_len,
                  unsigned char **out, int *out_len, long *http_code, int max_bytes)
{
    return https_request_cookies(method, url, cacert_path, headers, body, body_len,
                                 out, out_len, http_code, max_bytes, NULL, 0);
}

int https_request_cookies(const char *method, const char *url, const char *cacert_path,
                          const char *const *headers, const unsigned char *body, int body_len,
                          unsigned char **out, int *out_len, long *http_code, int max_bytes,
                          char *cookie_jar, size_t cookie_cap)
{
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    unsigned char *o = NULL; int ol = 0;
    int r = do_request(method, url, cacert_path, headers, body, body_len,
                       &o, &ol, http_code, max_bytes, 0, 0 /*no follow*/, 0 /*any status*/,
                       cookie_jar, cookie_cap);
    if (out) *out = o; else free(o);
    if (out_len) *out_len = ol;
    return r;
}
