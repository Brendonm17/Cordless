#ifndef CORDLESS_TLS_CA_BUNDLE_H
#define CORDLESS_TLS_CA_BUNDLE_H

#include <mbedtls/x509_crt.h>

// Shared, parse-once CA trust chain: the Mozilla bundle is parsed once at boot and
// the read-only chain shared across all TLS connections (safe across threads).

// Parse the CA bundle once (idempotent). Call at boot, before any TLS connection.
void ca_bundle_init(const char *cacert_path);

// The shared parsed chain for mbedtls_ssl_conf_ca_chain(), or NULL if unavailable
// (callers then fall back to no verification).
mbedtls_x509_crt *ca_bundle_get(void);

#endif  // CORDLESS_TLS_CA_BUNDLE_H
