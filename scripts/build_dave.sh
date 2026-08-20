#!/usr/bin/env bash
#
# Cross-compile the vendored mlspp into a static library for the Vita.
# Output: third_party/mlspp/lib/libmlspp_core.a
set -euo pipefail

export VITASDK="${VITASDK:-/usr/local/vitasdk}"
export PATH="$VITASDK/bin:$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR="$ROOT/third_party/mlspp"
MBEDTLS_INC="$ROOT/third_party/mbedtls/include"

# Mirror the mlspp tree to a space-free cache dir and build there.
M="$HOME/.cache/dave_mlspp"
rm -rf "$M"; mkdir -p "$M"
cp -r "$VENDOR/." "$M/"

if [ ! -f "$M/include/namespace.h" ]; then
    sed "s/@MLS_CXX_NAMESPACE@/mlspp/" "$M/cmake/namespace.h.in" > "$M/include/namespace.h"
fi

INCFLAGS=(
    -I"$M/include"
    -I"$M/lib/bytes/include"
    -I"$M/lib/tls_syntax/include"
    -I"$M/lib/hpke/include"
    -I"$M/lib/hpke/src"
    -I"$MBEDTLS_INC"
)

# C++ compile flags.
CXXFLAGS=(
    -std=c++17 -frtti -O2 -DNDEBUG
    -ffunction-sections -fdata-sections -Wno-psabi
    "${INCFLAGS[@]}"
)

CXX=arm-vita-eabi-g++

# MLS core + serialization helper sources.
core_sources=(
    "$M"/src/*.cpp
    "$M"/lib/bytes/src/*.cpp
    "$M"/lib/tls_syntax/src/*.cpp
)

# HPKE sources, minus the excluded files below.
hpke_exclude=(mlkem.cpp hybrid_kem.cpp openssl_common.cpp)
hpke_sources=()
for f in "$M"/lib/hpke/src/*.cpp; do
    base="$(basename "$f")"
    skip=0
    for x in "${hpke_exclude[@]}"; do
        [ "$base" = "$x" ] && skip=1
    done
    [ "$skip" -eq 0 ] && hpke_sources+=("$f")
done

sources=("${core_sources[@]}" "${hpke_sources[@]}")

OBJ="$(mktemp -d)"
trap 'rm -rf "$OBJ"' EXIT

echo ">> compiling ${#sources[@]} sources (MLS core + mbedTLS HPKE shim) for arm-vita-eabi"
for f in "${sources[@]}"; do
    # Name objects by path with slashes turned into underscores.
    rel="${f#$M/}"
    o="$OBJ/$(echo "$rel" | tr '/' '_').o"
    echo "   CXX $rel"
    "$CXX" "${CXXFLAGS[@]}" -c "$f" -o "$o"
done

TMP_OUT="$M/libmlspp_core.a"
arm-vita-eabi-ar rcs "$TMP_OUT" "$OBJ"/*.o
mkdir -p "$VENDOR/lib"
cp "$TMP_OUT" "$VENDOR/lib/libmlspp_core.a"
echo ">> OK: third_party/mlspp/lib/libmlspp_core.a ($(du -h "$TMP_OUT" | cut -f1), $(arm-vita-eabi-ar t "$TMP_OUT" | wc -l) objects)"
