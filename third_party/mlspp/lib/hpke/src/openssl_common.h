#pragma once

// This build replaces the upstream OpenSSL crypto backend with mbedTLS, so the
// OpenSSL RAII helpers (typed_delete / typed_unique_ptr) and openssl_error()
// that used to live here are gone. The header is retained as an empty shim so
// any lingering include resolves; the mbedTLS shim files manage their own
// contexts with explicit init/free.

#include <namespace.h>
