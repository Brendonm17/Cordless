#pragma once

#include <cstddef>
#include <namespace.h>

// A minimal mbedTLS-style RNG callback backed by a shared, lazily-seeded
// CTR_DRBG (entropy from the platform: sceKernelGetRandomNumber on the Vita via
// mbedtls_hardware_poll). This replaces OpenSSL's implicit global RNG that the
// upstream hpke code relied on. Single-threaded use (the voice/DAVE thread).
namespace MLS_NAMESPACE::hpke {

int
mbed_rng(void* ctx, unsigned char* out, size_t len);

} // namespace MLS_NAMESPACE::hpke
