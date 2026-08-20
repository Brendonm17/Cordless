#!/usr/bin/env bash
# Cross-compile mbedTLS 3.6.x for the PS Vita, vendored into third_party/mbedtls/.
# Run from WSL: bash scripts/build_tls.sh
set -euo pipefail

export VITASDK="${VITASDK:-/usr/local/vitasdk}"
export PATH="$VITASDK/bin:$PATH"

MBEDTLS_TAG="v3.6.2"
BD="${TLS_BUILD_DIR:-$HOME/.cache/cordless_tls}"
DST="$(cd "$(dirname "$0")/.." && pwd)/third_party/mbedtls"

mkdir -p "$BD"; cd "$BD"
[ -d mbedtls ] || git clone --depth 1 --branch "$MBEDTLS_TAG" \
  https://github.com/Mbed-TLS/mbedtls.git
cd mbedtls
git submodule update --init

# Apply Vita port tweaks to the mbedTLS config.
CFG=include/mbedtls/mbedtls_config.h
if ! grep -q "CORDLESS_VITA_TLS" "$CFG"; then
  python3 - "$CFG" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
add="""
/* --- CORDLESS_VITA_TLS: Vita port tweaks --- */
#define MBEDTLS_NO_PLATFORM_ENTROPY        /* no /dev/urandom on Vita */
#define MBEDTLS_ENTROPY_HARDWARE_ALT       /* our mbedtls_hardware_poll */
#define MBEDTLS_PLATFORM_MS_TIME_ALT       /* our mbedtls_ms_time */
#undef  MBEDTLS_NET_C                      /* bring our own sceNet transport */
#undef  MBEDTLS_TIMING_C                   /* no gettimeofday/select reliance */
#undef  MBEDTLS_THREADING_C                /* app serializes TLS via net_lock */
"""
i=s.rfind("#endif"); open(p,"w").write(s[:i]+add+"\n"+s[i:])
print("config patched")
PY
fi

cmake -B build_vita -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
  -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build_vita -j4

mkdir -p "$DST/lib" "$DST/include"
cp build_vita/library/libmbedtls.a build_vita/library/libmbedx509.a \
   build_vita/library/libmbedcrypto.a "$DST/lib/"
rm -rf "$DST/include/mbedtls" "$DST/include/psa"
cp -r include/mbedtls include/psa "$DST/include/"
echo ">> vendored mbedTLS $MBEDTLS_TAG into $DST"
