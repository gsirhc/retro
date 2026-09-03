#!/usr/bin/env bash
# Fetch the Microsoft Altair BASIC images the emulator loads (4K and 8K, the
# flat load-and-go tapes plus the 8K EPROM build). Verified by SHA-256. Run
# from anywhere; files land next to this script. Used by CI and locally.
#
#   ./fetch-basic.sh            # fetch what's missing / wrong
#   ./fetch-basic.sh --force    # re-fetch everything

set -euo pipefail
cd "$(dirname "$0")"

FORCE="${1:-}"

# name  |  url  |  sha256
FILES=(
  "8kbas.bin|https://raw.githubusercontent.com/nippur72/8-bit-projects/844a09655c100b75e994a12af0b00de14d2e2ad4/altair-basic/8kbas.bin|dfe4b1576c6ac9fe1a47e9ba0fe697f098209ef8eab61cd54cffc626a84152d3"
  "4kbas.bin|https://raw.githubusercontent.com/nippur72/8-bit-projects/844a09655c100b75e994a12af0b00de14d2e2ad4/altair-basic/4kbas40.bin|3aaa9907f8a2c32452b9b4580c9b4cced2146d7877129cdcd0b9a50c798fb616"
  "8kBas_e0.bin|https://deramp.com/downloads/altair/software/roms/custom_roms/8K%20ROM%20BASIC/8kBas_e0.bin|efa591baea4fbf94c0efc9f0ca9bd429351d53b5765dec373cdc4919ef2c99a8"
  "8kBas_e8.bin|https://deramp.com/downloads/altair/software/roms/custom_roms/8K%20ROM%20BASIC/8kBas_e8.bin|1fd187321ba1fa0bfaa66e54b3c408151e7e320f4badfd8759d91f3f50f2d096"
  "8kBas_f0.bin|https://deramp.com/downloads/altair/software/roms/custom_roms/8K%20ROM%20BASIC/8kBas_f0.bin|3e37740c7155fb5b98a8009f46eea53f2109947f44d34c79ef0707246558a39e"
  "8kBas_f8.bin|https://deramp.com/downloads/altair/software/roms/custom_roms/8K%20ROM%20BASIC/8kBas_f8.bin|d663432a584fc9efaf762e76e4daa24f28b4be32df18dcb999cbc0f4007408ad"
)

sha() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}';
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

for row in "${FILES[@]}"; do
  IFS='|' read -r name url want <<<"$row"
  if [ -z "$FORCE" ] && [ -f "$name" ] && [ "$(sha "$name")" = "$want" ]; then
    echo "ok    $name"
    continue
  fi
  echo "fetch $name"
  tmp="$(mktemp)"
  curl -sSfL --retry 3 -o "$tmp" "$url"
  got="$(sha "$tmp")"
  if [ "$got" != "$want" ]; then
    echo "  SHA-256 mismatch for $name" >&2
    echo "  want $want" >&2
    echo "  got  $got" >&2
    rm -f "$tmp"
    exit 1
  fi
  mv "$tmp" "$name"
  chmod 644 "$name"
done

echo "Altair BASIC ready."
