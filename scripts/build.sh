#!/usr/bin/env bash
# Build Cordless.
# Mirror sources to a space-free workspace, build there, copy the vpk back.
set -euo pipefail

export VITASDK="${VITASDK:-/usr/local/vitasdk}"
export PATH="$VITASDK/bin:$PATH"

if [[ ! -f "$VITASDK/share/vita.toolchain.cmake" ]]; then
  echo "error: VitaSDK not found at $VITASDK. Install it (https://vitasdk.org) or set VITASDK to your install path." >&2
  exit 1
fi

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$HOME/.cache/cordless_build"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo ">> Mirroring sources to $WORK"
mkdir -p "$WORK"
rsync -a --delete \
  --exclude build/ --exclude .git/ \
  "$SRC_DIR"/ "$WORK"/

echo ">> Configuring ${CMAKE_EXTRA:-}"
# Reset feature options each build with -U.
cmake -S "$WORK" -B "$WORK/build" \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -UCORDLESS_AUTOEXIT -UCORDLESS_INSECURE_TLS \
  ${CMAKE_EXTRA:-} \
  >/dev/null

echo ">> Building (-j$JOBS)"
cmake --build "$WORK/build" -j"$JOBS"

VPK="$WORK/build/cordless.vpk"
if [[ -f "$VPK" ]]; then
  cp "$VPK" "$SRC_DIR/cordless.vpk"
  echo ">> OK: $SRC_DIR/cordless.vpk ($(du -h "$VPK" | cut -f1))"
else
  echo ">> BUILD FAILED: no vpk produced" >&2
  exit 1
fi
