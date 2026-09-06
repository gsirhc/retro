#!/usr/bin/env bash
# Fetch the FreeDOS 1.3 boot floppy, 1.2MB (5.25" HD) format -- the "120m"
# variant of the official Floppy Edition, matching this machine's Drive A:
# geometry exactly (80 cyl / 2 head / 15 sec/track / 512B = 1,228,800
# bytes). FreeDOS is public-domain-adjacent/freely redistributable (GPL +
# various permissive licenses per-component); this is the official
# ibiblio.org distribution point. SHA-256 verified, same pattern as
# fetch-basic.sh.
#
#   ./fetch-freedos.sh            # fetch what's missing / wrong
#   ./fetch-freedos.sh --force    # re-fetch

set -euo pipefail
cd "$(dirname "$0")"
FORCE="${1:-}"

URL="https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/FD13-FloppyEdition.zip"
NAME="x86BOOT.img"
WANT_SHA="e898dd2b09e81a1e477f0396c4de0395b9b71b4b90b1c2f79ea0e40b26f3bcf9"

sha() { if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}'; else shasum -a 256 "$1" | awk '{print $1}'; fi; }

if [ -z "$FORCE" ] && [ -f "$NAME" ] && [ "$(sha "$NAME")" = "$WANT_SHA" ]; then
  echo "ok    $NAME"
  exit 0
fi
echo "fetch $NAME (via FD13-FloppyEdition.zip)"
tmp="$(mktemp -d)"
curl -sSfL --retry 3 -o "$tmp/fd.zip" "$URL"
unzip -o -q "$tmp/fd.zip" "120m/x86BOOT.img" -d "$tmp/x"
got="$(sha "$tmp/x/120m/x86BOOT.img")"
if [ "$got" != "$WANT_SHA" ]; then
  echo "  SHA-256 mismatch for $NAME" >&2
  echo "  want $WANT_SHA" >&2
  echo "  got  $got" >&2
  rm -rf "$tmp"
  exit 1
fi
mv "$tmp/x/120m/x86BOOT.img" "$NAME"
chmod 644 "$NAME"
rm -rf "$tmp"
echo "FreeDOS boot floppy ready."
