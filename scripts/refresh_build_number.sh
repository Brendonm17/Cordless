#!/usr/bin/env bash
# Print Discord's current web-client build number.
#     BN=$(bash scripts/refresh_build_number.sh) && \
#       CMAKE_EXTRA="-DCLIENT_BUILD_NUMBER=$BN" bash scripts/build.sh
set -euo pipefail

UA='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36'
BASE='https://discord.com'

html=$(curl -fsSL -A "$UA" "$BASE/login") || { echo "fetch /login failed" >&2; exit 1; }

# Collect asset .js paths in document order.
mapfile -t assets < <(printf '%s' "$html" | grep -oE '/assets/[A-Za-z0-9._-]+\.js' | awk '!seen[$0]++')
if [ "${#assets[@]}" -eq 0 ]; then echo "no asset chunks found" >&2; exit 1; fi

found=""
for (( i=${#assets[@]}-1; i>=0 && i>${#assets[@]}-16; i-- )); do
    js=$(curl -fsSL -A "$UA" "$BASE${assets[$i]}") || continue
    # Match the build number in its several forms.
    n=$(printf '%s' "$js" \
        | grep -oiE '(build_?number)["'"'"' :=]{1,4}[0-9]{5,7}' \
        | grep -oE '[0-9]{5,7}' | head -n1 || true)
    if [ -n "$n" ]; then found="$n"; break; fi
done

if [ -z "$found" ]; then
    echo "could not locate a build number (Discord asset layout may have changed)" >&2
    exit 1
fi
echo "$found"
