// Vita platform hooks for our mbedTLS build (MBEDTLS_ENTROPY_HARDWARE_ALT +
// MBEDTLS_PLATFORM_MS_TIME_ALT): newlib has no /dev/urandom or CLOCK_MONOTONIC.
#include <stddef.h>

#include <psp2/kernel/rng.h>  // sceKernelGetRandomNumber (hardware RNG)
#include <psp2/kernel/processmgr.h>  // sceKernelGetProcessTimeWide (us clock)

#include <mbedtls/entropy.h>
#include <mbedtls/platform_time.h>

// Hardware entropy source. sceKernelGetRandomNumber caps at 64 bytes per call,
// so fill the buffer in chunks.
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > 64) chunk = 64;
        if (sceKernelGetRandomNumber(output + done, chunk) < 0) {
            if (olen) *olen = done;
            return done ? 0 : MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        done += chunk;
    }
    if (olen) *olen = len;
    return 0;
}

// Monotonic-ish millisecond clock for TLS 1.3 timers.
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(sceKernelGetProcessTimeWide() / 1000);
}
