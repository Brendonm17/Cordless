#ifndef CORDLESS_TLS_HTTPS_GET_H
#define CORDLESS_TLS_HTTPS_GET_H

#include <stddef.h>  // size_t

// HTTPS GET over mbedTLS: follows redirects, malloc's the body into *out (caller
// frees) on 200. Returns 0 on success, <0 on error. Serialize with net_lock.
int https_get_mbedtls(const char *url, const char *cacert_path,
                      unsigned char **out, int *out_len, long *http_code,
                      int max_bytes);

// General HTTPS request over mbedTLS (Host/Content-Length/Connection auto-added).
// Returns 0 if a response arrived (any status), <0 on failure; *out malloc'd, caller frees.
int https_request(const char *method, const char *url, const char *cacert_path,
                  const char *const *headers, const unsigned char *body, int body_len,
                  unsigned char **out, int *out_len, long *http_code, int max_bytes);

// Like https_request, but fills `cookie_jar` (a zeroed buffer, cap `cookie_cap`) with
// the Set-Cookie name=value pairs "; "-joined into a ready-to-send Cookie header.
int https_request_cookies(const char *method, const char *url, const char *cacert_path,
                          const char *const *headers, const unsigned char *body, int body_len,
                          unsigned char **out, int *out_len, long *http_code, int max_bytes,
                          char *cookie_jar, size_t cookie_cap);

#endif  // CORDLESS_TLS_HTTPS_GET_H
