#include <hpke/random.h>
#include <namespace.h>

#include "mbed_rng.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include <stdexcept>

namespace MLS_NAMESPACE::hpke {

// Shared, lazily-seeded CTR_DRBG. The upstream code leaned on OpenSSL's global
// RAND_bytes; we provide the equivalent on mbedTLS. Not reentrant, which is
// fine: all DAVE crypto runs on the single voice thread.
static mbedtls_ctr_drbg_context&
drbg()
{
  static mbedtls_entropy_context entropy;
  static mbedtls_ctr_drbg_context ctr_drbg;
  static bool seeded = false;

  if (!seeded) {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    static const unsigned char pers[] = "cordless-dave-hpke";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg,
                              mbedtls_entropy_func,
                              &entropy,
                              pers,
                              sizeof(pers) - 1) != 0) {
      throw std::runtime_error("CTR_DRBG seed failed");
    }
    seeded = true;
  }

  return ctr_drbg;
}

int
mbed_rng(void* /* ctx */, unsigned char* out, size_t len)
{
  return mbedtls_ctr_drbg_random(&drbg(), out, len);
}

bytes
random_bytes(size_t size)
{
  auto rand = bytes(size);
  if (size > 0 && mbedtls_ctr_drbg_random(&drbg(), rand.data(), size) != 0) {
    throw std::runtime_error("random_bytes failed");
  }
  return rand;
}

} // namespace MLS_NAMESPACE::hpke
